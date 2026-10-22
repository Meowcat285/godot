#!/usr/bin/env python3

if __name__ != "__main__":
    raise SystemExit(f'Utility script "{__file__}" should not be used as a module!')

# Packages the CI build artifacts into release assets:
# - An export templates archive (.tpz) in the layout expected by the editor's export template manager.
# - One archive per editor build.
#
# Expects the artifacts to be downloaded into one subfolder per artifact name
# (the default behavior of `actions/download-artifact` when downloading all artifacts).

import argparse
import os
import re
import shutil
import stat
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

import version  # noqa: E402


def get_version_config(module_config: str) -> str:
    # Must match GODOT_VERSION_FULL_CONFIG in `core/version.h`.
    number = f"{version.major}.{version.minor}"
    if version.patch:
        number += f".{version.patch}"
    status = os.getenv("GODOT_VERSION_STATUS", version.status)
    return f"{number}.{status}{version.module_config}{module_config}"


def add_file(zf: zipfile.ZipFile, src: Path, arcname: str, executable: bool = False) -> None:
    info = zipfile.ZipInfo.from_file(src, arcname)
    # Artifacts lose their permissions when uploaded, so set them explicitly.
    info.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    with open(src, "rb") as fin, zf.open(info, "w") as fout:
        shutil.copyfileobj(fin, fout)


def add_tree(zf: zipfile.ZipFile, src: Path, arcname: str, executables: set[Path] | None = None) -> None:
    for path in sorted(src.rglob("*")):
        if path.is_file():
            rel = path.relative_to(src).as_posix()
            add_file(zf, path, f"{arcname}/{rel}" if arcname else rel, executables is not None and path in executables)


def warn(message: str) -> None:
    print(f"::warning::{message}")


class Packager:
    def __init__(self, artifacts: Path, output: Path, work: Path, version_config: str):
        self.artifacts = artifacts
        self.output = output
        self.work = work
        self.version_config = version_config
        self.base_name = f"Godot_v{version_config}"

    def artifact_file(self, artifact: str, filename: str) -> Path | None:
        path = self.artifacts / artifact / filename
        if not path.is_file():
            warn(f'"{filename}" not found in artifact "{artifact}", skipping.')
            return None
        return path

    # Export templates.

    def package_templates(self) -> Path:
        # Maps file names in the `templates/` folder of the .tpz to their source path and executable flag.
        templates: dict[str, tuple[Path, bool]] = {}

        def add(name: str, src: Path | None, executable: bool = False) -> None:
            if src is not None:
                templates[name] = (src, executable)

        for target in ["release", "debug"]:
            suffix = "" if target == "release" else "-debug"

            add(
                f"linux_{target}.x86_64",
                self.artifact_file(f"linux-template-mono{suffix}", f"godot.linuxbsd.template_{target}.x86_64.mono"),
                True,
            )
            add(
                f"windows_{target}_x86_64.exe",
                self.artifact_file(
                    f"windows-template-mono{suffix}", f"godot.windows.template_{target}.x86_64.mono.exe"
                ),
            )
            add(
                f"windows_{target}_x86_64_console.exe",
                self.artifact_file(
                    f"windows-template-mono{suffix}", f"godot.windows.template_{target}.x86_64.mono.console.exe"
                ),
            )
            # Mono Android builds are named `android_mono<Target>.apk` by Gradle, but the editor expects
            # the same names as non-Mono builds in the export templates.
            android_artifact = "android-template-mono-arm64" + ("-release" if target == "release" else "")
            add(
                f"android_{target}.apk",
                self.artifact_file(android_artifact, f"android_mono{target.capitalize()}.apk"),
            )

        add("macos.zip", self.package_macos_templates())
        add("ios.zip", self.package_ios_templates())
        add("android_source.zip", self.package_android_source())
        add("icudt_godot.dat", ROOT / "thirdparty" / "icu4c" / "icudt_godot.dat")

        version_txt = self.work / "version.txt"
        version_txt.write_text(self.version_config + "\n", encoding="utf-8")
        add("version.txt", version_txt)

        tpz = self.output / f"{self.base_name}_export_templates.tpz"
        with zipfile.ZipFile(tpz, "w", zipfile.ZIP_DEFLATED) as zf:
            for name, (src, executable) in sorted(templates.items()):
                add_file(zf, src, f"templates/{name}", executable)
                print(f"  templates/{name}")
        return tpz

    def package_macos_templates(self) -> Path | None:
        binaries = {}
        for target in ["release", "debug"]:
            suffix = "" if target == "release" else "-debug"
            src = self.artifact_file(f"macos-template-mono{suffix}", f"godot.macos.template_{target}.universal.mono")
            if src is not None:
                binaries[f"godot_macos_{target}.universal"] = src
        if not binaries:
            return None

        # Same layout as `platform/macos/platform_macos_builders.py`.
        out = self.work / "macos.zip"
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
            add_tree(zf, ROOT / "misc" / "dist" / "macos_template.app", "macos_template.app")
            for name, src in binaries.items():
                add_file(zf, src, f"macos_template.app/Contents/MacOS/{name}", True)
        return out

    def package_ios_templates(self) -> Path | None:
        # Same layout as `generate_bundle_apple_embedded()` in `platform_methods.py`.
        libs: dict[str, Path] = {}
        lib_regex = re.compile(r"^libgodot(_\w+)?\.ios\.template_(release|debug)\.arm64\.a$")
        for artifact in ["ios-template-mono", "ios-template-mono-debug"]:
            artifact_dir = self.artifacts / artifact
            if not artifact_dir.is_dir():
                warn(f'Artifact "{artifact}" not found, skipping.')
                continue
            for path in artifact_dir.iterdir():
                match = lib_regex.match(path.name)
                if match:
                    module = match.group(1) or ""
                    target = match.group(2)
                    libs[f"libgodot{module}.ios.{target}.xcframework/ios-arm64/libgodot{module}.a"] = path
        if not libs:
            return None

        xcode_dir = self.work / "ios_xcode"
        shutil.copytree(ROOT / "misc" / "dist" / "apple_embedded_xcode", xcode_dir)
        for entry in xcode_dir.iterdir():
            # Remove other platforms' xcframeworks.
            if entry.name.startswith("libgodot") and entry.name.endswith(".xcframework"):
                parts = entry.name.split(".")
                if len(parts) >= 3 and parts[1] != "ios":
                    shutil.rmtree(entry)
        for dest, src in libs.items():
            shutil.copy(src, xcode_dir / dest)

        # The libraries are statically linked against AccessKit when it's enabled, so the Xcode project
        # needs its framework too. The editor detects it in the template and adds it to the exported project.
        for artifact in ["ios-template-mono", "ios-template-mono-debug"]:
            accesskit = self.artifacts / artifact / "build_deps" / "accesskit" / "lib" / "ios" / "AccessKit.xcframework"
            if accesskit.is_dir():
                shutil.copytree(accesskit, xcode_dir / "AccessKit.xcframework")
                break
        else:
            warn('"AccessKit.xcframework" not found in iOS artifacts, skipping.')

        out = self.work / "ios.zip"
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
            add_tree(zf, xcode_dir, "")
        return out

    def package_android_source(self) -> Path | None:
        # Each Android template build generates a Gradle source template containing only its own
        # library (AAR), so merge them into a single source template containing both.
        sources = [
            self.artifact_file("android-template-mono-arm64", "android_source.zip"),
            self.artifact_file("android-template-mono-arm64-release", "android_source.zip"),
        ]
        merged_dir = self.work / "android_source"
        executables = set()
        found = False
        for source in sources:
            if source is None:
                continue
            found = True
            with zipfile.ZipFile(source) as zf:
                for info in zf.infolist():
                    if info.is_dir():
                        continue
                    dest = merged_dir / info.filename
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    with zf.open(info) as fin, open(dest, "wb") as fout:
                        shutil.copyfileobj(fin, fout)
                    if (info.external_attr >> 16) & 0o111:
                        executables.add(dest)
        if not found:
            return None

        out = self.work / "android_source.zip"
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
            add_tree(zf, merged_dir, "", executables)
        return out

    # Editors.

    def package_editor(self, artifact: str, platform: str, executables: list[str]) -> Path | None:
        artifact_dir = self.artifacts / artifact
        if not artifact_dir.is_dir():
            warn(f'Artifact "{artifact}" not found, skipping.')
            return None

        for exe in executables:
            if not (artifact_dir / exe).is_file():
                warn(f'"{exe}" not found in artifact "{artifact}".')

        name = f"{self.base_name}_{platform}"
        out = self.output / f"{name}.zip"
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
            add_tree(zf, artifact_dir, name, {artifact_dir / exe for exe in executables})
        return out

    def package_macos_editor(self) -> Path | None:
        binary = self.artifact_file("macos-editor-mono", "godot.macos.editor.universal.mono")
        if binary is None:
            return None

        # Same layout as `platform/macos/platform_macos_builders.py`.
        app_name = "Godot_mono.app"
        app_dir = self.work / app_name
        shutil.copytree(
            ROOT / "misc" / "dist" / "macos_tools.app",
            app_dir,
            ignore=shutil.ignore_patterns("Info.plist"),
        )
        (app_dir / "Contents" / "MacOS").mkdir(parents=True, exist_ok=True)
        shutil.copy(binary, app_dir / "Contents" / "MacOS" / "Godot")
        godot_sharp = self.artifacts / "macos-editor-mono" / "GodotSharp"
        if godot_sharp.is_dir():
            shutil.copytree(godot_sharp, app_dir / "Contents" / "Resources" / "GodotSharp")

        status = os.getenv("GODOT_VERSION_STATUS", version.status)
        build = os.getenv("BUILD_NAME", "custom_build")
        plist = (ROOT / "misc" / "dist" / "macos" / "editor_info_plist.template").read_text(encoding="utf-8")
        plist = plist.replace("$version", f"{version.major}.{version.minor}.{version.patch}.{status}.{build}")
        plist = plist.replace("$short_version", f"{version.major}.{version.minor}.{version.patch}")
        if build not in ["official", "steam"]:
            plist = plist.replace("org.godotengine.godot", f"org.godotengine.godot.{build}")
        (app_dir / "Contents" / "Info.plist").write_text(plist, encoding="utf-8", newline="\n")

        out = self.output / f"{self.base_name}_macos.universal.zip"
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
            add_tree(zf, app_dir, app_name, {app_dir / "Contents" / "MacOS" / "Godot"})
        return out


parser = argparse.ArgumentParser(description="Package CI build artifacts into release assets.")
parser.add_argument("--artifacts", type=Path, required=True, help="Folder containing the downloaded artifacts.")
parser.add_argument("--output", type=Path, required=True, help="Folder to write the release assets to.")
parser.add_argument("--module-config", default=".mono", help='Module configuration suffix (e.g. ".mono").')
args = parser.parse_args()

args.output.mkdir(parents=True, exist_ok=True)
version_config = get_version_config(args.module_config)
print(f"Packaging release assets for Godot {version_config}.")

with tempfile.TemporaryDirectory() as tmp:
    packager = Packager(args.artifacts, args.output, Path(tmp), version_config)
    assets = [
        packager.package_templates(),
        packager.package_editor("linux-editor-mono", "linux_x86_64", ["godot.linuxbsd.editor.x86_64.mono"]),
        packager.package_editor(
            "windows-editor-mono",
            "win64",
            ["godot.windows.editor.x86_64.mono.exe", "godot.windows.editor.x86_64.mono.console.exe"],
        ),
        packager.package_macos_editor(),
    ]

for asset in assets:
    if asset is not None:
        print(f"Created {asset} ({asset.stat().st_size / 1024 / 1024:.1f} MiB).")
