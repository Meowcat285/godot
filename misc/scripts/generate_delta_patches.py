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
#   Zip archives inside packages (like `macos.zip` in the export templates) get one operation per entry.
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
import sys
import time
import zipfile
from concurrent.futures import FIRST_COMPLETED, Future, ProcessPoolExecutor, wait
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

# Archives inside packages whose entries are diffed individually, see `DeltaEncoder.plan_archive()`.
NESTED_ARCHIVE_SUFFIXES = (".zip",)

ZSTD_LEVEL = 22
# Stay within the default window size limit of zstd decoders (2^27 bytes).
ZSTD_WINDOW_LOG = 27
# Patches this many times smaller than the file compressed at `QUICK_LEVEL` are kept without trying the full file.
# On Godot binaries, `ZSTD_LEVEL` only gets 20-30% smaller than `QUICK_LEVEL`.
QUICK_LEVEL = 3
QUICK_MARGIN = 4

# Files taking long enough to encode that their start is logged, and how often to log the files left to encode.
SLOW_FILE_SIZE = 16 * 1024 * 1024
HEARTBEAT_SECONDS = 60


@dataclass
class FileInfo:
    path: str
    size: int
    sha256: str
    mode: int

    def to_json(self) -> dict[str, object]:
        return {"path": self.path, "size": self.size, "sha256": self.sha256, "mode": self.mode}


START_TIME = time.monotonic()


def log(message: str) -> None:
    """Prints a line prefixed with the time since the start, so progress shows in CI logs as it happens.
    GitHub Actions commands (like `::warning::`) aren't prefixed, they must start the line."""
    if not message.startswith("::"):
        elapsed = int(time.monotonic() - START_TIME)
        message = f"[{elapsed // 60:3d}:{elapsed % 60:02d}] {message}"
    # A single write, so lines from the worker processes don't get mixed up.
    sys.stdout.write(message + "\n")
    sys.stdout.flush()


def mib(size: int) -> str:
    return f"{size / 1024 / 1024:.1f} MiB"


def human_size(size: int) -> str:
    return mib(size) if size >= 1024 * 1024 else f"{size / 1024:.1f} KiB"


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
        log(f"  Downloading {release['tag_name']}/{asset['name']} ({mib(asset['size'])}).")
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


def extract(archive: Path, dest: Path) -> tuple[dict[str, FileInfo], dict[str, zipfile.ZipInfo]]:
    """Extracts a zip, returning its files and the zip entries of all files and folders, by path."""
    start = time.monotonic()
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    files: dict[str, FileInfo] = {}
    entries: dict[str, zipfile.ZipInfo] = {}
    with zipfile.ZipFile(archive) as zf:
        for info in zf.infolist():
            name = info.filename.replace("\\", "/")
            entries[name] = info
            if info.is_dir():
                continue
            path = dest / name
            # Refuse paths escaping the destination.
            if not path.resolve().is_relative_to(dest.resolve()):
                raise SystemExit(f'Unsafe path "{name}" in {archive}.')
            path.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(info) as fin, open(path, "wb") as fout:
                shutil.copyfileobj(fin, fout)
            mode = stat.S_IMODE(info.external_attr >> 16) or 0o644
            files[name] = FileInfo(name, path.stat().st_size, sha256_file(path), mode)
    total = sum(f.size for f in files.values())
    log(f"  Extracted {archive.name}: {len(files)} files, {mib(total)}, in {time.monotonic() - start:.0f}s.")
    return files, entries


def has_symlinks(entries: dict[str, zipfile.ZipInfo]) -> bool:
    """Symlinks are extracted as regular files holding the link target, and the launcher only writes regular
    files, so the operations can't reproduce them."""
    return any(stat.S_ISLNK(info.external_attr >> 16) for info in entries.values())


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


def quick_full_size(new_path: Path) -> int:
    """Size of the file compressed at a fast level, which `ZSTD_LEVEL` can't make several times smaller."""
    with open(new_path, "rb") as fin, open(os.devnull, "wb") as null:
        return int(zstandard.ZstdCompressor(level=QUICK_LEVEL).copy_stream(fin, null)[1])


def encode_file(old_path: Path | None, new_path: Path, out_dir: Path, index: int, label: str) -> tuple[str, Path]:
    """Returns the cheapest of a patch against `old_path` and the full file, as (op, data path)."""
    if new_path.stat().st_size >= SLOW_FILE_SIZE:
        log(f"    Encoding {label} ({mib(new_path.stat().st_size)}).")
    full = out_dir / f"{index}.full"
    if old_path is None:
        write_full(new_path, full)
        return "add", full
    patch = out_dir / f"{index}.patch"
    patch_size = write_patch(old_path, new_path, patch)
    # Compressing a large file at `ZSTD_LEVEL` takes longer than diffing it, so skip it when it can't win.
    if patch_size * QUICK_MARGIN <= quick_full_size(new_path):
        return "patch", patch
    if patch_size < write_full(new_path, full):
        full.unlink()
        return "patch", patch
    patch.unlink()
    return "add", full


class DeltaEncoder:
    """Works out the operations turning a folder of old files into the new ones, encoding the data they need
    in the background."""

    def __init__(self, pool: ProcessPoolExecutor, data_dir: Path):
        self.pool = pool
        self.data_dir = data_dir
        self.next_index = 0
        self.pending: list[tuple[dict[str, object], FileInfo | None, Future[tuple[str, Path]]]] = []
        # Path shown in the log, size, and submission time of each file to encode.
        self.labels: dict[Future[tuple[str, Path]], tuple[str, int, float]] = {}

    def plan(
        self,
        old_dir: Path,
        old_files: dict[str, FileInfo],
        new_dir: Path,
        new_files: dict[str, FileInfo],
        nested: bool,
        prefix: str = "",
    ) -> list[dict[str, object]]:
        old_by_hash = {f.sha256: f for f in old_files.values()}
        operations: list[dict[str, object]] = []
        # Encode the largest files first so they don't end up running last on their own.
        for new in sorted(new_files.values(), key=lambda f: f.size, reverse=True):
            op: dict[str, object] = new.to_json()
            operations.append(op)
            old = old_files.get(new.path)
            if nested and old is not None and new.path.endswith(NESTED_ARCHIVE_SUFFIXES):
                if self.plan_archive(op, old_dir / old.path, new_dir / new.path, old.path):
                    continue
            if old is not None and old.sha256 == new.sha256:
                op.update(op="copy", source=old.path, source_sha256=old.sha256)
            elif new.sha256 in old_by_hash:
                renamed = old_by_hash[new.sha256]
                op.update(op="copy", source=renamed.path, source_sha256=renamed.sha256)
            else:
                index = self.next_index
                self.next_index += 1
                old_path = old_dir / old.path if old is not None else None
                label = prefix + new.path
                future = self.pool.submit(encode_file, old_path, new_dir / new.path, self.data_dir, index, label)
                op["entry"] = f"data/{index}"
                self.pending.append((op, old, future))
                self.labels[future] = (label, new.size, time.monotonic())
        return operations

    def plan_archive(self, op: dict[str, object], old_zip: Path, new_zip: Path, source: str) -> bool:
        """Archives (like `macos.zip` in the export templates) are compressed, so patching them as a whole
        barely helps. Instead, the operation describes their entries, and the launcher rebuilds the archive.
        It's then not byte-identical to the original, so its entries are checked instead of the whole file.
        Returns false for archives holding symlinks, which are then handled as regular files."""
        work = self.data_dir.parent / f"nested-{self.next_index}"
        self.next_index += 1
        old_files, old_entries = extract(old_zip, work / "old")
        new_files, new_entries = extract(new_zip, work / "new")
        if has_symlinks(old_entries) or has_symlinks(new_entries):
            log(f"  {source} holds symlinks, diffing it as a whole.")
            shutil.rmtree(work)
            return False
        entries = self.plan(work / "old", old_files, work / "new", new_files, nested=False, prefix=f"{source}/")
        for entry in entries:
            entry["compressed"] = new_entries[str(entry["path"])].compress_type != zipfile.ZIP_STORED
        for name, info in new_entries.items():
            if info.is_dir():
                entries.append({"op": "dir", "path": name, "mode": stat.S_IMODE(info.external_attr >> 16) or 0o755})
        entries.sort(key=lambda e: str(e["path"]))
        op.update(op="archive", source=source, entries=entries)
        return True

    def finish(self) -> list[tuple[str, Path]]:
        """Waits for the encoding to finish, and returns the data to store, as (entry name, file)."""
        total = len(self.pending)
        total_size = sum(size for _, size, _ in self.labels.values())
        log(f"  Waiting for {total} changed files ({mib(total_size)}) to be encoded.")
        done = done_size = 0
        remaining = set(self.labels)
        while remaining:
            finished, remaining = wait(remaining, timeout=HEARTBEAT_SECONDS, return_when=FIRST_COMPLETED)
            if not finished:
                left = sum(self.labels[f][1] for f in remaining)
                log(f"  Still encoding, {len(remaining)}/{total} files ({mib(left)}) left.")
            for future in finished:
                kind, path = future.result()
                label, size, submitted = self.labels[future]
                done += 1
                done_size += size
                log(
                    f"  [{done}/{total}, {done_size * 100 // max(total_size, 1)}%] {kind} {label}: "
                    + f"{human_size(size)} -> {human_size(path.stat().st_size)}"
                    + f" ({time.monotonic() - submitted:.0f}s since queued)."
                )
        data = []
        for op, old, future in self.pending:
            kind, path = future.result()
            op["op"] = kind
            if kind == "patch":
                assert old is not None
                op.update(source=old.path, source_sha256=old.sha256)
            data.append((str(op["entry"]), path))
        return data


def count_ops(operations: list[dict[str, object]], counts: dict[str, int]) -> dict[str, int]:
    for op in operations:
        counts[str(op["op"])] = counts.get(str(op["op"]), 0) + 1
        if op["op"] == "archive":
            count_ops(op["entries"], counts)  # type: ignore
    return counts


@dataclass
class PackageState:
    """A package whose deltas are being built."""

    asset: Json
    new_dir: Path
    # The package's entry in the manifest, its deltas are added as they're done.
    record: dict[str, object]
    pending: int = 0
    planned: bool = False


@dataclass
class PendingDelta:
    """A delta whose changed files are being encoded by the worker pool."""

    state: PackageState
    package: str
    old_tag: str
    old_dir: Path
    work: Path
    encoder: DeltaEncoder
    operations: list[dict[str, object]]
    start: float


def start_delta(
    pool: ProcessPoolExecutor,
    state: PackageState,
    package: str,
    old_tag: str,
    old_dir: Path,
    old_files: dict[str, FileInfo],
    new_files: dict[str, FileInfo],
    work: Path,
) -> PendingDelta:
    log(f"  Building {package} delta from {old_tag}.")
    delta_work = work / f"delta-{package}-{old_tag}"
    if delta_work.exists():
        shutil.rmtree(delta_work)
    data_dir = delta_work / "data"
    data_dir.mkdir(parents=True)
    encoder = DeltaEncoder(pool, data_dir)
    operations = encoder.plan(old_dir, old_files, state.new_dir, new_files, nested=True)
    state.pending += 1
    return PendingDelta(state, package, old_tag, old_dir, delta_work, encoder, operations, time.monotonic())


def finish_delta(delta: PendingDelta, new_tag: str, output: Path) -> Path:
    log(f"  Finishing {delta.package} delta from {delta.old_tag}.")
    data = delta.encoder.finish()
    operations = delta.operations
    operations.sort(key=lambda o: str(o["path"]))
    descriptor = {
        "format": MANIFEST_FORMAT,
        "package": delta.package,
        "from": delta.old_tag,
        "to": new_tag,
        "operations": operations,
    }

    out = output / f"{delta.package}.from-{delta.old_tag}.mgdelta"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED, allowZip64=True) as zf:
        zf.writestr("delta.json", json.dumps(descriptor, indent=1))
        for entry, path in sorted(data):
            zf.write(path, entry)
    shutil.rmtree(delta.work)
    shutil.rmtree(delta.old_dir)

    counts = count_ops(operations, {})
    summary = ", ".join(f"{n} {kind}" for kind, n in sorted(counts.items()))
    log(f"  {out.name}: {mib(out.stat().st_size)} ({summary}), in {time.monotonic() - delta.start:.0f}s.")
    return out


def close_delta(delta: PendingDelta, new_tag: str, output: Path) -> None:
    """Finishes a delta and adds it to its package, cleaning up the package once all its deltas are done."""
    state = delta.state
    out = finish_delta(delta, new_tag, output)
    # Not worth it when it saves less than a tenth of the download.
    if out.stat().st_size > state.asset["size"] * 0.9:
        log(f"  Dropping {out.name}, it isn't much smaller than the full download.")
        out.unlink()
    else:
        deltas: list[dict[str, object]] = state.record["deltas"]  # type: ignore
        deltas.append({
            "from": delta.old_tag,
            "asset": out.name,
            "size": out.stat().st_size,
            "sha256": sha256_file(out),
        })
    state.pending -= 1
    close_package(state)


def close_package(state: PackageState) -> None:
    if state.planned and state.pending == 0:
        shutil.rmtree(state.new_dir)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate delta patches for Meow Godot Launcher.")
    parser.add_argument("--repo", default=os.getenv("GITHUB_REPOSITORY"), help="Repository, as `owner/name`.")
    parser.add_argument("--tag", required=True, help="Tag of the release to generate deltas to.")
    parser.add_argument("--history", type=int, default=2, help="Number of older releases to generate deltas from.")
    parser.add_argument(
        "--jobs",
        type=int,
        default=3,
        help="Files to diff in parallel (each needs ~18x the file size in RAM, 3 fit in 16 GB with Godot binaries).",
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

    # One pool for the whole run, so the files of the next delta are encoded while the last (and largest) files
    # of the previous one are, instead of leaving workers idle. One delta is planned ahead of the one finishing.
    # Fork explicitly, since other start methods re-import this script, which isn't allowed.
    in_flight: list[PendingDelta] = []
    with ProcessPoolExecutor(max_workers=args.jobs, mp_context=multiprocessing.get_context("fork")) as pool:
        for package_index, (package, suffix) in enumerate(PACKAGES.items(), 1):
            asset = releases.find_asset(target, suffix)
            if asset is None:
                log(f"::warning::No asset matching *{suffix} in {args.tag}, skipping {package}.")
                continue
            log(f"Package {package_index}/{len(PACKAGES)}, {package}: {asset['name']} ({mib(asset['size'])}).")
            new_archive = releases.download(target, asset)
            new_dir = args.work / f"new-{package}"
            new_files, new_entries = extract(new_archive, new_dir)
            state = PackageState(
                asset,
                new_dir,
                {
                    "id": package,
                    "asset": asset["name"],
                    "asset_size": asset["size"],
                    "asset_sha256": sha256_file(new_archive),
                    "files": [f.to_json() for f in sorted(new_files.values(), key=lambda f: f.path)],
                    "deltas": [],
                },
            )
            packages.append(state.record)
            if has_symlinks(new_entries):
                log(f"::warning::{asset['name']} holds symlinks, which deltas can't reproduce, skipping its deltas.")
                previous_for_package = []
            else:
                previous_for_package = previous

            for old_release in previous_for_package:
                old_asset = releases.find_asset(old_release, suffix)
                if old_asset is None:
                    continue
                old_dir = args.work / f"old-{package}-{old_release['tag_name']}"
                old_files, old_entries = extract(releases.download(old_release, old_asset), old_dir)
                if has_symlinks(old_entries):
                    log(f"  {old_asset['name']} holds symlinks, which deltas can't rely on, skipping it.")
                    shutil.rmtree(old_dir)
                    continue
                in_flight.append(
                    start_delta(pool, state, package, old_release["tag_name"], old_dir, old_files, new_files, args.work)
                )
                while len(in_flight) > 1:
                    close_delta(in_flight.pop(0), args.tag, args.output)

            state.planned = True
            close_package(state)

        while in_flight:
            close_delta(in_flight.pop(0), args.tag, args.output)

    (args.output / MANIFEST_NAME).write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    log(f"Wrote {args.output / MANIFEST_NAME}, done.")


main()
