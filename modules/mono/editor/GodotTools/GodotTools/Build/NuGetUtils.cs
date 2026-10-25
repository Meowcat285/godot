using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Xml;
using System.Xml.Linq;
using Godot;
using GodotTools.Internals;
using Environment = System.Environment;
using OS = GodotTools.Utils.OS;

namespace GodotTools.Build
{
    /// <summary>
    /// Fork: makes the NuGet packages bundled with the editor (GodotSharp, Godot.NET.Sdk, etc.) available to NuGet,
    /// as this fork's package versions (e.g. "2026.10.14") are not on nuget.org.
    /// The packages are copied to a folder shared by all editor versions, which is added as a package source to the
    /// user's NuGet config, so projects build without any setup, inside or outside the editor.
    /// </summary>
    public static class NuGetUtils
    {
        private const string PackageSourceName = "Godot (bundled packages)";

        // Exact IDs for `packageSourceMapping`: prefix patterns would take over community packages (e.g. "GodotSharp.*").
        private static readonly string[] PackageIds = { "GodotSharp", "GodotSharpEditor", "Godot.NET.Sdk", "Godot.SourceGenerators" };

        public static string BundledPackagesDir => Path.Combine(GodotSharpDirs.DataEditorToolsDir, "nupkgs");

        // Next to NuGet's global packages folder rather than in the editor data dir, as the latter is per-editor in
        // self-contained mode: the source must be the same path for every editor and must outlive any of them.
        public static string SharedPackagesDir => Path.GetFullPath(Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".nuget", "godot-packages"));

        public static void SetupBundledPackagesSource()
        {
            try
            {
                if (!Directory.Exists(BundledPackagesDir))
                    return;

                CopyBundledPackages(BundledPackagesDir, SharedPackagesDir);
                AddPackageSourceToUserNuGetConfig(GetUserNuGetConfigPath(), PackageSourceName, SharedPackagesDir);
            }
            catch (Exception e)
            {
                GD.PushWarning($"Failed to set up the bundled NuGet packages: {e.Message}");
            }
        }

        private static void CopyBundledPackages(string sourceDir, string destDir)
        {
            Directory.CreateDirectory(destDir);

            foreach (string package in Directory.EnumerateFiles(sourceDir, "*.nupkg"))
            {
                string dest = Path.Combine(destDir, Path.GetFileName(package));
                // Unique, so editors starting at the same time don't write the same temporary file.
                string temp = $"{dest}.{Guid.NewGuid():N}.tmp";
                try
                {
                    if (File.Exists(dest) && FilesHaveSameContent(dest, package))
                        continue;

                    // Copy to a temporary file first, so a NuGet restore never sees a partially written package.
                    File.Copy(package, temp);
                    File.Move(temp, dest, overwrite: true);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    // e.g. another editor or a restore has the package open. It's retried on the next start.
                    if (Godot.OS.IsStdOutVerbose())
                        GD.PrintErr($"Failed to copy NuGet package '{package}': {e.Message}");
                }
                finally
                {
                    if (File.Exists(temp))
                        File.Delete(temp);
                }
            }
        }

        private static bool FilesHaveSameContent(string a, string b)
        {
            if (new FileInfo(a).Length != new FileInfo(b).Length)
                return false;

            using var streamA = File.OpenRead(a);
            using var streamB = File.OpenRead(b);
            return SHA256.HashData(streamA).AsSpan().SequenceEqual(SHA256.HashData(streamB));
        }

        // The same location as NuGet's user-wide config: https://learn.microsoft.com/nuget/consume-packages/configuring-nuget-behavior
        private static string GetUserNuGetConfigPath()
        {
            string baseDir = OS.IsWindows
                ? Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData)
                : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".nuget");
            return Path.Combine(baseDir, "NuGet", "NuGet.Config");
        }

        private static void AddPackageSourceToUserNuGetConfig(string configPath, string name, string sourcePath)
        {
            XDocument doc;
            bool isNewConfig = !File.Exists(configPath);
            if (!isNewConfig)
            {
                // Keep the user's formatting.
                doc = XDocument.Load(configPath, LoadOptions.PreserveWhitespace);
            }
            else
            {
                // NuGet only adds nuget.org itself when it creates the config, so keep it when creating it here.
                doc = new XDocument(new XDeclaration("1.0", "utf-8", null), new XElement("configuration",
                    new XElement("packageSources",
                        new XElement("add",
                            new XAttribute("key", "nuget.org"),
                            new XAttribute("value", "https://api.nuget.org/v3/index.json"),
                            new XAttribute("protocolVersion", "3")))));
            }

            var root = doc.Root ?? throw new InvalidDataException($"Invalid NuGet config: {configPath}");
            bool changed = false;

            var packageSources = GetOrAddElement(root, "packageSources", ref changed);
            var source = packageSources.Elements("add").FirstOrDefault(e => (string?)e.Attribute("key") == name);
            if (source == null)
            {
                AddElement(packageSources, new XElement("add", new XAttribute("key", name), new XAttribute("value", sourcePath)));
                changed = true;
            }
            else if ((string?)source.Attribute("value") != sourcePath)
            {
                source.SetAttributeValue("value", sourcePath);
                changed = true;
            }

            // With package source mapping, a source without patterns can't provide any package. An exact ID mapped
            // to one source excludes the others, so it's also mapped to nuget.org for projects on upstream versions.
            var mapping = root.Element("packageSourceMapping");
            if (mapping != null)
            {
                foreach (string key in new[] { name, "nuget.org" })
                {
                    var mappedSource = mapping.Elements("packageSource").FirstOrDefault(e => (string?)e.Attribute("key") == key);
                    if (mappedSource == null)
                    {
                        if (key != name)
                            continue;
                        mappedSource = new XElement("packageSource", new XAttribute("key", key));
                        // Added before its children, so that they get its indentation.
                        AddElement(mapping, mappedSource);
                        changed = true;
                    }

                    foreach (string id in PackageIds)
                    {
                        if (mappedSource.Elements("package").Any(e => (string?)e.Attribute("pattern") == id))
                            continue;
                        AddElement(mappedSource, new XElement("package", new XAttribute("pattern", id)));
                        changed = true;
                    }
                }
            }

            if (!changed)
                return;

            // Write through symlinks (e.g. from a dotfiles manager), so the link is kept.
            if (!isNewConfig)
                configPath = new FileInfo(configPath).ResolveLinkTarget(returnFinalTarget: true)?.FullName ?? configPath;
            Directory.CreateDirectory(Path.GetDirectoryName(configPath)!);

            // Write to a temporary file first, so a concurrent restore never reads a partially written config.
            string temp = $"{configPath}.{Guid.NewGuid():N}.tmp";
            try
            {
                var settings = new XmlWriterSettings
                {
                    Encoding = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
                    OmitXmlDeclaration = doc.Declaration == null,
                    Indent = isNewConfig,
                };
                using (var writer = XmlWriter.Create(temp, settings))
                    doc.Save(writer);
                if (!OperatingSystem.IsWindows() && !isNewConfig)
                    File.SetUnixFileMode(temp, File.GetUnixFileMode(configPath));
                File.Move(temp, configPath, overwrite: true);
            }
            finally
            {
                if (File.Exists(temp))
                    File.Delete(temp);
            }
        }

        private static XElement GetOrAddElement(XElement parent, string name, ref bool changed)
        {
            var element = parent.Element(name);
            if (element == null)
            {
                element = new XElement(name);
                AddElement(parent, element);
                changed = true;
            }
            return element;
        }

        // Adds an element after the last child element, with the same indentation, as the file keeps its whitespace.
        private static void AddElement(XElement parent, XElement element)
        {
            var last = parent.Elements().LastOrDefault();
            if (last != null)
            {
                if (last.PreviousNode is XText text)
                    last.AddAfterSelf(new XText(text.Value), element);
                else
                    last.AddAfterSelf(element);
            }
            else
            {
                if (parent.PreviousNode is XText text)
                {
                    // Indent by the same step as between the parent and its own parent.
                    string indent = text.Value;
                    string? parentIndent = (parent.Parent?.PreviousNode as XText)?.Value;
                    string step = parentIndent != null && indent.Length > parentIndent.Length && indent.StartsWith(parentIndent, StringComparison.Ordinal)
                        ? indent[parentIndent.Length..]
                        : "  ";
                    parent.Add(new XText(indent + step), element, new XText(indent));
                }
                else
                {
                    parent.Add(element);
                }
            }
        }
    }
}
