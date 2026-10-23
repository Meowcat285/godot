#!/usr/bin/env python3

if __name__ != "__main__":
    raise SystemExit(f'Utility script "{__file__}" should not be used as a module!')

# Generates delta patches between a GitHub release and the releases published before it,
# for use by Meow Godot Launcher (https://github.com/Meowcat285/meow-godot-launcher).
#
# Outputs, to be uploaded as assets of the target release:
# - `meow-launcher-manifest.json`: the files of each package (release asset) and the deltas available for it.
# - `<package>.from-<tag>.mgdelta`: one delta per package and older release. A delta is an uncompressed zip
#   holding `delta.json` (one operation per file of the new package) and the data the operations need.
#
# Changed files are diffed with bsdiff, which handles the shifted addresses of recompiled executables
# much better than generic compressors, and the result is compressed with zstd. The patch data format
# (`MGBSDF01`) is read by `BsPatch.cs` in the launcher, keep both in sync.
#
# Requires the `gh` CLI (authenticated), and the `bsdiff4` and `zstandard` Python packages.

import argparse
import hashlib
import json
import multiprocessing
import os
import shutil
import stat
import struct
import subprocess
import time
import zipfile
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import bsdiff4.core  # type: ignore
import zstandard  # type: ignore

MANIFEST_NAME = "meow-launcher-manifest.json"
MANIFEST_FORMAT = 1
PATCH_MAGIC = b"MGBSDF01"

# Must match `PackageKinds.cs` in the launcher.
PACKAGES = {
    "editor-linux-x86_64": "_linux_x86_64.zip",
    "editor-windows-x86_64": "_win64.zip",
    "editor-macos-universal": "_macos.universal.zip",
    "export-templates": "_export_templates.tpz",
}

# Release and asset objects of the GitHub REST API.
Json = dict[str, Any]

ZSTD_LEVEL = 22
# Stay within the default window size limit of zstd decoders (2^27 bytes).
ZSTD_WINDOW_LOG = 27


@dataclass
class FileInfo:
    path: str
    size: int
    sha256: str
    mode: int

    def to_json(self) -> dict[str, object]:
        return {"path": self.path, "size": self.size, "sha256": self.sha256, "mode": self.mode}


def log(message: str) -> None:
    print(message, flush=True)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest()


def compressor() -> zstandard.ZstdCompressor:
    params = zstandard.ZstdCompressionParameters.from_level(ZSTD_LEVEL, window_log=ZSTD_WINDOW_LOG)
    return zstandard.ZstdCompressor(compression_params=params)


def gh(*args: str) -> str:
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True).stdout


# Releases.


class Releases:
    def __init__(self, repo: str, cache: Path):
        self.repo = repo
        self.cache = cache
        self.releases: list[Json] = json.loads(gh("api", "--paginate", "--slurp", f"repos/{repo}/releases"))
        # `--slurp` returns one list per page.
        self.releases = [r for page in self.releases for r in page if not r["draft"]]
        self.releases.sort(key=lambda r: r["published_at"], reverse=True)

    def get(self, tag: str) -> Json:
        for release in self.releases:
            if release["tag_name"] == tag:
                return release
        raise SystemExit(f'Release "{tag}" not found in {self.repo}.')

    def previous(self, tag: str, count: int) -> list[Json]:
        target = self.get(tag)
        older = [r for r in self.releases if r["published_at"] < target["published_at"]]
        return older[:count]

    @staticmethod
    def find_asset(release: Json, suffix: str) -> Json | None:
        assets: list[Json] = release["assets"]
        for asset in assets:
            if asset["name"].endswith(suffix):
                return asset
        return None

    def download(self, release: Json, asset: Json) -> Path:
        dest: Path = self.cache / release["tag_name"] / asset["name"]
        if dest.is_file() and dest.stat().st_size == asset["size"]:
            return dest
        dest.parent.mkdir(parents=True, exist_ok=True)
        log(f"Downloading {release['tag_name']}/{asset['name']} ({asset['size'] / 1024 / 1024:.1f} MiB).")
        for attempt in range(3):
            try:
                gh(
                    "release",
                    "download",
                    release["tag_name"],
                    "--repo",
                    self.repo,
                    "--pattern",
                    asset["name"],
                    "--dir",
                    str(dest.parent),
                    "--clobber",
                )
                break
            except subprocess.CalledProcessError as e:
                if attempt == 2:
                    raise
                log(f"Download failed ({e.stderr.strip()}), retrying.")
                time.sleep(10)
        return dest


# Packages.


def extract(archive: Path, dest: Path) -> dict[str, FileInfo]:
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    files: dict[str, FileInfo] = {}
    with zipfile.ZipFile(archive) as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            name = info.filename.replace("\\", "/")
            path = dest / name
            # Refuse paths escaping the destination.
            if not path.resolve().is_relative_to(dest.resolve()):
                raise SystemExit(f'Unsafe path "{name}" in {archive}.')
            path.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(info) as fin, open(path, "wb") as fout:
                shutil.copyfileobj(fin, fout)
            mode = stat.S_IMODE(info.external_attr >> 16) or 0o644
            files[name] = FileInfo(name, path.stat().st_size, sha256_file(path), mode)
    return files


def write_patch(old_path: Path, new_path: Path, out_path: Path) -> int:
    """Writes a `MGBSDF01` patch: the magic, then a zstd stream of bsdiff records
    (int64 diff length, int64 extra length, int64 seek, diff bytes, extra bytes)."""
    old = old_path.read_bytes()
    new = new_path.read_bytes()
    control, diff, extra = bsdiff4.core.diff(old, new)
    del old, new
    with open(out_path, "wb") as f:
        f.write(PATCH_MAGIC)
        with compressor().stream_writer(f, closefd=False) as w:
            diff_pos = extra_pos = 0
            for diff_len, extra_len, seek in control:
                w.write(struct.pack("<qqq", diff_len, extra_len, seek))
                w.write(diff[diff_pos : diff_pos + diff_len])
                w.write(extra[extra_pos : extra_pos + extra_len])
                diff_pos += diff_len
                extra_pos += extra_len
    return out_path.stat().st_size


def write_full(new_path: Path, out_path: Path) -> int:
    with open(new_path, "rb") as fin, open(out_path, "wb") as fout:
        compressor().copy_stream(fin, fout, size=new_path.stat().st_size)
    return out_path.stat().st_size


def encode_file(old_path: Path | None, new_path: Path, out_dir: Path, index: int) -> tuple[str, Path]:
    """Returns the cheapest of a patch against `old_path` and the full file, as (op, data path)."""
    full = out_dir / f"{index}.full"
    full_size = write_full(new_path, full)
    if old_path is None:
        return "add", full
    patch = out_dir / f"{index}.patch"
    if write_patch(old_path, new_path, patch) < full_size:
        full.unlink()
        return "patch", patch
    patch.unlink()
    return "add", full


def build_delta(
    package: str,
    old_tag: str,
    old_dir: Path,
    old_files: dict[str, FileInfo],
    new_tag: str,
    new_dir: Path,
    new_files: dict[str, FileInfo],
    work: Path,
    output: Path,
    jobs: int,
) -> Path:
    data_dir = work / "delta-data"
    if data_dir.exists():
        shutil.rmtree(data_dir)
    data_dir.mkdir(parents=True)

    old_by_hash = {f.sha256: f for f in old_files.values()}
    operations: list[dict[str, object]] = []
    pending = {}

    # Fork explicitly, since other start methods re-import this script, which isn't allowed.
    with ProcessPoolExecutor(max_workers=jobs, mp_context=multiprocessing.get_context("fork")) as pool:
        # Encode the largest files first so they don't end up running last on their own.
        for index, new in enumerate(sorted(new_files.values(), key=lambda f: f.size, reverse=True)):
            op: dict[str, object] = new.to_json()
            operations.append(op)
            old = old_files.get(new.path)
            if old is not None and old.sha256 == new.sha256:
                op.update(op="copy", source=old.path, source_sha256=old.sha256)
            elif new.sha256 in old_by_hash:
                renamed = old_by_hash[new.sha256]
                op.update(op="copy", source=renamed.path, source_sha256=renamed.sha256)
            else:
                old_path = old_dir / old.path if old is not None else None
                pending[index] = (op, old, pool.submit(encode_file, old_path, new_dir / new.path, data_dir, index))

        for index, (op, old, future) in pending.items():
            kind, data = future.result()
            op.update(op=kind, entry=f"data/{index}")
            if kind == "patch":
                assert old is not None
                op.update(source=old.path, source_sha256=old.sha256)

    operations.sort(key=lambda o: str(o["path"]))
    descriptor = {
        "format": MANIFEST_FORMAT,
        "package": package,
        "from": old_tag,
        "to": new_tag,
        "operations": operations,
    }

    out = output / f"{package}.from-{old_tag}.mgdelta"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED, allowZip64=True) as zf:
        zf.writestr("delta.json", json.dumps(descriptor, indent=1))
        for op in operations:
            if "entry" in op:
                data_index = str(op["entry"]).removeprefix("data/")
                kind = "patch" if op["op"] == "patch" else "full"
                zf.write(data_dir / f"{data_index}.{kind}", str(op["entry"]))
    shutil.rmtree(data_dir)

    counts = {kind: sum(1 for o in operations if o["op"] == kind) for kind in ["copy", "patch", "add"]}
    log(
        f"  {out.name}: {out.stat().st_size / 1024 / 1024:.1f} MiB ({counts['copy']} unchanged, {counts['patch']} patched, {counts['add']} added)."
    )
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate delta patches for Meow Godot Launcher.")
    parser.add_argument("--repo", default=os.getenv("GITHUB_REPOSITORY"), help="Repository, as `owner/name`.")
    parser.add_argument("--tag", required=True, help="Tag of the release to generate deltas to.")
    parser.add_argument("--history", type=int, default=2, help="Number of older releases to generate deltas from.")
    parser.add_argument(
        "--jobs", type=int, default=2, help="Files to diff in parallel (each needs ~17x the file size in RAM)."
    )
    parser.add_argument("--work", type=Path, required=True, help="Scratch folder (downloads are cached in it).")
    parser.add_argument("--output", type=Path, required=True, help="Folder to write the release assets to.")
    args = parser.parse_args()

    if not args.repo:
        raise SystemExit("--repo is required outside of GitHub Actions.")

    args.output.mkdir(parents=True, exist_ok=True)
    releases = Releases(args.repo, args.work / "downloads")
    target = releases.get(args.tag)
    previous = releases.previous(args.tag, args.history)
    log(f"Generating deltas to {args.tag} from: {', '.join(r['tag_name'] for r in previous) or '(none)'}.")

    manifest: dict[str, object] = {
        "format": MANIFEST_FORMAT,
        "tag": args.tag,
        "commit": target["target_commitish"],
        "packages": [],
    }
    packages: list[dict[str, object]] = manifest["packages"]  # type: ignore

    for package, suffix in PACKAGES.items():
        asset = releases.find_asset(target, suffix)
        if asset is None:
            log(f"::warning::No asset matching *{suffix} in {args.tag}, skipping {package}.")
            continue
        log(f"{package}: {asset['name']}")
        new_archive = releases.download(target, asset)
        new_dir = args.work / "new"
        new_files = extract(new_archive, new_dir)
        deltas: list[dict[str, object]] = []

        for old_release in previous:
            old_asset = releases.find_asset(old_release, suffix)
            if old_asset is None:
                continue
            old_dir = args.work / "old"
            old_files = extract(releases.download(old_release, old_asset), old_dir)
            delta = build_delta(
                package,
                old_release["tag_name"],
                old_dir,
                old_files,
                args.tag,
                new_dir,
                new_files,
                args.work,
                args.output,
                args.jobs,
            )
            shutil.rmtree(old_dir)
            # Not worth it when it saves less than a tenth of the download.
            if delta.stat().st_size > asset["size"] * 0.9:
                log(f"  Dropping {delta.name}, it isn't much smaller than the full download.")
                delta.unlink()
                continue
            deltas.append({
                "from": old_release["tag_name"],
                "asset": delta.name,
                "size": delta.stat().st_size,
                "sha256": sha256_file(delta),
            })

        shutil.rmtree(new_dir)
        packages.append({
            "id": package,
            "asset": asset["name"],
            "asset_size": asset["size"],
            "asset_sha256": sha256_file(new_archive),
            "files": [f.to_json() for f in sorted(new_files.values(), key=lambda f: f.path)],
            "deltas": deltas,
        })

    (args.output / MANIFEST_NAME).write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    log(f"Wrote {args.output / MANIFEST_NAME}.")


main()
