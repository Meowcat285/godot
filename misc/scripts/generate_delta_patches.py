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
#   Zip archives inside packages (like `macos.zip` or the Android APKs in the export templates) get one operation
#   per entry, as do archives inside them (like the AARs in `android_source.zip`).
#
# Changed files are diffed with HDiffPatch (`hdiffz`), which handles the shifted addresses of recompiled executables
# much better than generic compressors, and compresses its patches with zstd. The launcher applies them with
# SharpHDiffPatch. Older releases have bsdiff patches (`MGBSDF01`, the `patch` op), which the launcher still reads.
#
# Requires the `gh` CLI (authenticated), `hdiffz` (https://github.com/sisong/HDiffPatch) on the PATH, and the
# `zstandard` Python package.

import argparse
import hashlib
import json
import multiprocessing
import os
import re
import shutil
import stat
import subprocess
import sys
import time
import zipfile
from concurrent.futures import FIRST_COMPLETED, Future, ProcessPoolExecutor, wait
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import zstandard  # type: ignore

MANIFEST_NAME = "meow-launcher-manifest.json"
MANIFEST_FORMAT = 1

# Must match `PackageKinds.cs` in the launcher.
PACKAGES = {
    "editor-linux-x86_64": "_linux_x86_64.zip",
    "editor-windows-x86_64": "_win64.zip",
    "editor-macos-universal": "_macos.universal.zip",
    "export-templates": "_export_templates.tpz",
}

# Release and asset objects of the GitHub REST API.
Json = dict[str, Any]

# Pre-releases of branches other than the default one are named `<release name>-<branch>.<run number>`, where
# `<branch>` is the branch name with anything but letters, digits and dashes replaced by dashes (see `release-name`
# in `.github/workflows/runner.yml`). Must match `ReleaseInfo.cs` in the launcher.
PRERELEASE_TAG = re.compile(r"^\d+\.\d+\.\d+-(?P<branch>[0-9A-Za-z-]+)\.\d+$")


def branch_slug(branch: str) -> str:
    # Byte by byte, like `tr` in runner.yml.
    return re.sub(rb"[^0-9A-Za-z-]", b"-", branch.encode()).decode()


def prerelease_branch(release: Json) -> str | None:
    """The (slugged) branch of a pre-release, None for releases."""
    match = PRERELEASE_TAG.match(release["tag_name"]) if release["prerelease"] else None
    return match["branch"] if match else None


# Archives inside packages whose entries are diffed individually, see `DeltaEncoder.plan_archive()`.
# APKs and AARs are zips too: the Android templates aren't signed, and the editor rebuilds, aligns and signs the APK
# when exporting, so their exact layout doesn't matter.
NESTED_ARCHIVE_SUFFIXES = (".zip", ".apk", ".aar")

# Level 22 takes about 1.7x as long as 19 on patches, for patches only about 1% smaller.
ZSTD_LEVEL = 19
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
        """The releases to generate deltas from: the `count` previous releases, and the last pre-release of the
        branch merged by the release's commit. For a pre-release, the previous pre-release of its branch, or for the
        first one, the release before it."""
        target = self.get(tag)
        older = [r for r in self.releases if r["published_at"] < target["published_at"]]
        releases = [r for r in older if not r["prerelease"]]
        if target["prerelease"]:
            branch = prerelease_branch(target)
            same_branch = [r for r in older if branch is not None and prerelease_branch(r) == branch]
            return (same_branch or releases)[:1]

        previous = releases[:count]
        merged = self.merged_branch(target["target_commitish"])
        last_build = next(
            (r for r in older if merged is not None and prerelease_branch(r) == branch_slug(merged)), None
        )
        if last_build is not None:
            log(f"Commit {target['target_commitish']} merged branch {merged}, last built as {last_build['tag_name']}.")
            previous.append(last_build)
        return previous

    def merged_branch(self, commit: str) -> str | None:
        """The branch of this repository merged by a pull request into `commit`, if any."""
        try:
            pulls: list[Json] = json.loads(gh("api", f"repos/{self.repo}/commits/{commit}/pulls"))
        except subprocess.CalledProcessError as e:
            log(f"::warning::Couldn't find the pull request merged by {commit}: {e.stderr.strip()}")
            return None
        for pull in pulls:
            head_repo: Json | None = pull["head"]["repo"]
            if pull["merge_commit_sha"] == commit and head_repo is not None and head_repo["full_name"] == self.repo:
                return str(pull["head"]["ref"])
        return None

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
    """Writes an HDiffPatch patch (`HDIFF13`), compressed with zstd. `-block-0` skips the fast block match, which
    makes patches of Godot binaries about 6% smaller in the same time. `hdiffz` also checks the patch it wrote."""
    subprocess.run(
        ["hdiffz", "-f", "-m", "-block-0", f"-c-zstd-{ZSTD_LEVEL}", str(old_path), str(new_path), str(out_path)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
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
        return "hpatch", patch
    if patch_size < write_full(new_path, full):
        full.unlink()
        return "hpatch", patch
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
        # Data entry and encoding of each (old hash, new hash) pair, since the same file can appear in several
        # places, like the native library of the Android templates, in both the APK and the AAR.
        self.encoded: dict[tuple[str | None, str], tuple[str, Future[tuple[str, Path]]]] = {}

    def plan(
        self,
        old_dir: Path,
        old_files: dict[str, FileInfo],
        new_dir: Path,
        new_files: dict[str, FileInfo],
        prefix: str = "",
    ) -> list[dict[str, object]]:
        old_by_hash = {f.sha256: f for f in old_files.values()}
        operations: list[dict[str, object]] = []
        # Encode the largest files first so they don't end up running last on their own.
        for new in sorted(new_files.values(), key=lambda f: f.size, reverse=True):
            op: dict[str, object] = new.to_json()
            operations.append(op)
            old = old_files.get(new.path)
            if old is not None and new.path.endswith(NESTED_ARCHIVE_SUFFIXES):
                if self.plan_archive(op, old_dir / old.path, new_dir / new.path, old.path, prefix):
                    continue
            if old is not None and old.sha256 == new.sha256:
                op.update(op="copy", source=old.path, source_sha256=old.sha256)
            elif new.sha256 in old_by_hash:
                renamed = old_by_hash[new.sha256]
                op.update(op="copy", source=renamed.path, source_sha256=renamed.sha256)
            else:
                key = (old.sha256 if old is not None else None, new.sha256)
                if key not in self.encoded:
                    index = self.next_index
                    self.next_index += 1
                    old_path = old_dir / old.path if old is not None else None
                    label = prefix + new.path
                    future = self.pool.submit(encode_file, old_path, new_dir / new.path, self.data_dir, index, label)
                    self.encoded[key] = (f"data/{index}", future)
                    self.labels[future] = (label, new.size, time.monotonic())
                entry, future = self.encoded[key]
                op["entry"] = entry
                self.pending.append((op, old, future))
        return operations

    def plan_archive(self, op: dict[str, object], old_zip: Path, new_zip: Path, source: str, prefix: str) -> bool:
        """Archives (like `macos.zip` in the export templates) are compressed, so patching them as a whole
        barely helps. Instead, the operation describes their entries, and the launcher rebuilds the archive.
        It's then not byte-identical to the original, so its entries are checked instead of the whole file.
        Returns false for archives holding symlinks, which are then handled as regular files."""
        work = self.data_dir.parent / f"nested-{self.next_index}"
        self.next_index += 1
        old_files, old_entries = extract(old_zip, work / "old")
        new_files, new_entries = extract(new_zip, work / "new")
        if has_symlinks(old_entries) or has_symlinks(new_entries):
            log(f"  {prefix}{source} holds symlinks, diffing it as a whole.")
            shutil.rmtree(work)
            return False
        entries = self.plan(work / "old", old_files, work / "new", new_files, prefix=f"{prefix}{source}/")
        for entry in entries:
            entry["compressed"] = new_entries[str(entry["path"])].compress_type != zipfile.ZIP_STORED
        for name, info in new_entries.items():
            if info.is_dir():
                entries.append({"op": "dir", "path": name, "mode": stat.S_IMODE(info.external_attr >> 16) or 0o755})
        entries.sort(key=lambda e: str(e["path"]))
        op.update(op="archive", source=source, entries=entries)
        return True

    def finish(self) -> list[tuple[str, Path]]:
        """Returns the data to store once all the files are encoded, as (entry name, file)."""
        data = {}
        for op, old, future in self.pending:
            kind, path = future.result()
            op["op"] = kind
            if kind == "hpatch":
                assert old is not None
                op.update(source=old.path, source_sha256=old.sha256)
            data[str(op["entry"])] = path
        return list(data.items())


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
    left: int


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
    operations = encoder.plan(old_dir, old_files, state.new_dir, new_files)
    state.pending += 1
    pending = len(encoder.labels)
    return PendingDelta(state, package, old_tag, old_dir, delta_work, encoder, operations, time.monotonic(), pending)


def finish_delta(delta: PendingDelta, new_tag: str, output: Path) -> Path:
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


class Progress:
    """Logs the files of all deltas as they're encoded, and closes each delta once all its files are."""

    def __init__(self, new_tag: str, output: Path):
        self.new_tag = new_tag
        self.output = output
        self.deltas: dict[Future[tuple[str, Path]], PendingDelta] = {}
        self.total = self.total_size = self.done = self.done_size = 0

    def add(self, delta: PendingDelta) -> None:
        for future in delta.encoder.labels:
            self.deltas[future] = delta
            self.total += 1
            self.total_size += delta.encoder.labels[future][1]
        if delta.left == 0:
            close_delta(delta, self.new_tag, self.output)

    def poll(self, timeout: float) -> bool:
        """Handles the files encoded within `timeout` seconds, returns whether there were any."""
        if not self.deltas:
            return False
        finished, _ = wait(self.deltas, timeout=timeout, return_when=FIRST_COMPLETED)
        for future in finished:
            delta = self.deltas.pop(future)
            kind, path = future.result()
            label, size, submitted = delta.encoder.labels[future]
            self.done += 1
            self.done_size += size
            log(
                f"  [{self.done}/{self.total}, {self.done_size * 100 // max(self.total_size, 1)}%] "
                + f"{kind} {delta.package}:{label}: {human_size(size)} -> {human_size(path.stat().st_size)}"
                + f" ({time.monotonic() - submitted:.0f}s since queued)."
            )
            delta.left -= 1
            if delta.left == 0:
                close_delta(delta, self.new_tag, self.output)
        return bool(finished)

    def run(self) -> None:
        """Waits for all the files to be encoded."""
        while self.deltas:
            if not self.poll(HEARTBEAT_SECONDS):
                left = sum(d.encoder.labels[f][1] for f, d in self.deltas.items())
                log(f"  Still encoding, {len(self.deltas)}/{self.total} files ({mib(left)}) left.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate delta patches for Meow Godot Launcher.")
    parser.add_argument("--repo", default=os.getenv("GITHUB_REPOSITORY"), help="Repository, as `owner/name`.")
    parser.add_argument("--tag", required=True, help="Tag of the release to generate deltas to.")
    parser.add_argument("--history", type=int, default=2, help="Number of older releases to generate deltas from.")
    parser.add_argument(
        "--jobs",
        type=int,
        default=3,
        help="Files to diff in parallel (each needs ~6x the file size in RAM).",
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

    found: list[tuple[str, str, Json]] = []
    for package, suffix in PACKAGES.items():
        asset = releases.find_asset(target, suffix)
        if asset is None:
            log(f"::warning::No asset matching *{suffix} in {args.tag}, skipping {package}.")
        else:
            found.append((package, suffix, asset))
    # Start with the largest packages, so their files, the slowest to encode, are queued first.
    found.sort(key=lambda p: p[2]["size"], reverse=True)

    # All the deltas of all the packages share one pool: the files of every delta are queued as soon as it's
    # planned, so the workers stay busy until the end. `--jobs` still caps the files encoded at once, and with it
    # the memory used. Fork explicitly, since other start methods re-import this script, which isn't allowed.
    records: dict[str, dict[str, object]] = {}
    progress = Progress(args.tag, args.output)
    with ProcessPoolExecutor(max_workers=args.jobs, mp_context=multiprocessing.get_context("fork")) as pool:
        for package_index, (package, suffix, asset) in enumerate(found, 1):
            log(f"Package {package_index}/{len(found)}, {package}: {asset['name']} ({mib(asset['size'])}).")
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
            records[package] = state.record
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
                progress.add(
                    start_delta(pool, state, package, old_release["tag_name"], old_dir, old_files, new_files, args.work)
                )
                progress.poll(0)

            state.planned = True
            close_package(state)

        log(f"All deltas planned, {progress.total} changed files ({mib(progress.total_size)}) to encode in total.")
        progress.run()

    # Deltas are listed from the newest release, in the order of `PACKAGES`, whatever order they're done in.
    order = {r["tag_name"]: i for i, r in enumerate(previous)}
    for package in PACKAGES:
        if package in records:
            deltas: list[dict[str, object]] = records[package]["deltas"]  # type: ignore
            deltas.sort(key=lambda d: order[str(d["from"])])
            packages.append(records[package])

    (args.output / MANIFEST_NAME).write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    log(f"Wrote {args.output / MANIFEST_NAME}, done.")


main()
