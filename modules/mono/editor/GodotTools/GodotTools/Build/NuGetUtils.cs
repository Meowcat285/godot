using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using Godot;
using GodotTools.Internals;
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

        public static string BundledPackagesDir => Path.Combine(GodotSharpDirs.DataEditorToolsDir, "nupkgs");

        public static string SharedPackagesDir => Path.Combine(GodotSharpDirs.MonoUserDir, "GodotNuGetPackages");

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
                if (File.Exists(dest) && new FileInfo(dest).Length == new FileInfo(package).Length)
                    continue;

                // Copy to a temporary file first, so a NuGet restore never sees a partially written package.
                string temp = dest + ".tmp";
                File.Copy(package, temp, overwrite: true);
                File.Move(temp, dest, overwrite: true);
            }
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
            if (File.Exists(configPath))
            {
                doc = XDocument.Load(configPath);
            }
            else
            {
                // NuGet only adds nuget.org itself when it creates the config, so keep it when creating it here.
                doc = new XDocument(new XElement("configuration",
                    new XElement("packageSources",
                        new XElement("add",
                            new XAttribute("key", "nuget.org"),
                            new XAttribute("value", "https://api.nuget.org/v3/index.json"),
                            new XAttribute("protocolVersion", "3")))));
            }

            var root = doc.Root ?? throw new InvalidDataException($"Invalid NuGet config: {configPath}");
            var packageSources = root.Element("packageSources");
            if (packageSources == null)
            {
                packageSources = new XElement("packageSources");
                root.Add(packageSources);
            }

            var source = packageSources.Elements("add").FirstOrDefault(e => (string?)e.Attribute("key") == name);
            if (source != null)
            {
                if ((string?)source.Attribute("value") == sourcePath)
                    return;
                source.SetAttributeValue("value", sourcePath);
            }
            else
            {
                packageSources.Add(new XElement("add", new XAttribute("key", name), new XAttribute("value", sourcePath)));
            }

            Directory.CreateDirectory(Path.GetDirectoryName(configPath)!);
            doc.Save(configPath);
        }
    }
}
