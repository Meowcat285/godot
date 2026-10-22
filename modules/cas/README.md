# Content-addressed storage (CAS)

This module stores imported assets (in the editor) and exported game data (at runtime) by content, Unity style.
Identical data is stored once, reverting a change or switching branches doesn't require reimporting, and patches
only contain the data that changed.

Content is addressed by [BLAKE3](https://github.com/BLAKE3-team/BLAKE3) hashes (`core/crypto/cas_hash.h`).

## Editor import cache

`CASImportStore` (`editor/cas_import_store.*`) is registered as an `EditorImportArtifactStore`
(`editor/file_system/editor_import_artifact_store.h`) and as the `ResourceFormatImporter` artifact provider.

```
res://.godot/cas/
├── objects/ab/<hash>.<ext>        Imported files, named by the hash of their contents.
├── manifests/ab/<key>.manifest    Import results: outputs, metadata, variants, generated files.
└── sources.log                    Import key and content hash currently used by each source file.
```

- The **import key** is a hash of everything an import depends on: the source file contents, the importer
  (name, format version, `get_import_settings_string()`), its options, generator parameters, build dependencies and
  the engine version. The source path and UID are only included when the importer isn't
  `is_import_path_independent()`, so identical files in different places share their imported files.
- `.import` files only contain settings (importer, UID, options...). `path=`, `dest_files=`, `metadata=`... are
  provided from the manifest when the file is read (`ResourceFormatImporter::open_import_file()`).
- Importers are unchanged: they still write to their import base path in `res://.godot/imported`, which is only used
  as a staging area. After an import, everything written there is moved to `objects/`, and logical paths
  (`res://.godot/imported/<file>-<md5>.<suffix>`) are resolved to objects when loading
  (`ResourceFormatImporter::resolve_artifact_path()`).
- Before importing, the store is checked for a result with the same key. Scene importers (which can write extracted
  resources to the project) and script import plugins always run, unless `can_share_import_cache()` is true.
- Unused objects and manifests are removed when the cache exceeds `editor/import/cas/max_cache_size_mb`, or with
  **Project > Tools > Clean Import Cache**.

### Shared import cache

`CASBackend` (`editor/cas_backend.*`) is the interface to a store shared between machines. Backends are queried, in
order, when a result isn't available locally, and given the results of imports made locally by importers that
`can_share_import_cache()`. `CASLocalDirectoryBackend` stores everything in a directory (e.g. a network drive), and
is configured with the `editor/import/cas/shared_cache_directory` and `editor/import/cas/shared_cache_read_only` editor
settings. Other backends can be implemented in scripts or GDExtensions and registered with
`CASBackend.add_import_cache_backend()`. Backends are only used by the editor.

Everything received from a backend is verified against its hash, and manifests can't contain objects.

A cache server could implement this HTTP protocol (not implemented yet):

| Request | Description |
|---|---|
| `GET /manifests/<key>` | Manifest of an import key (404 if missing). |
| `PUT /manifests/<key>` | Store a manifest, after all of its objects. |
| `GET /objects/<hash>` | Chunk list of an object (one chunk hash per line). |
| `PUT /objects/<hash>` | Store the chunk list of an object, after its chunks. |
| `POST /chunks/query` | Body: chunk hashes, one per line. Response: one `0`/`1` per hash. |
| `GET /chunks/<hash>` / `PUT /chunks/<hash>` | Chunk data (zstd-compressed in transit). |

Objects are split with content-defined chunking (`fastcdc.*`), so near-identical files share most chunks and only
upload the chunks that differ. Servers should verify hashes of everything they receive.

## GCAS packs

`.gcas` packs are an alternative to `.pck` packs (which keep working, also mixed with GCAS packs). The reader is in
core (`core/io/file_access_gcas.*`), as packs are loaded before modules are initialized. The writer is
`GCASWriter`, also available to scripts as `GCASPacker`.

- Files are split into chunks with content-defined chunking, deduplicated, and each chunk is compressed independently
  with zstd (or stored uncompressed when it doesn't compress), so any byte range can be read by only decompressing the
  chunks it overlaps. Decompressed chunks are kept in a cache shared by all open files
  (`file_access/gcas/chunk_cache_size_mb`).
- Layout providers (`CASLayoutProvider`) control how files are split: forced cuts, fixed-size pages (e.g. virtual
  texture tiles), groups always stored in one chunk, per-range compression and placement priority. They can also store
  hint data that runtime streaming code can read back with `PackedSourceGCAS::get_layout_hints()`. Streamed textures
  (`.stex`) are split at mip boundaries (see `modules/texture_streaming`), so streaming a mip only decompresses that mip.
- Packs contain their own UID table, registered with `ResourceUID` when they are mounted (no `uid_cache.bin`).
- Patch packs reference chunks of their base packs by hash, and only contain new chunks.

Layout (little-endian):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `GCAS` |
| 4 | 2 | Format version (1) |
| 6 | 2 | Header size (128) |
| 8 | 12 | Engine major, minor, patch |
| 20 | 4 | Index codec (0 raw, 1 zstd) |
| 24 | 8 | Compatible flags (unknown flags are ignored) |
| 32 | 8 | Incompatible flags (packs with unknown flags are refused) |
| 40 | 16 | Pack ID (derived from the index) |
| 56 | 8 | Index offset |
| 64 | 8 | Index stored size |
| 72 | 8 | Index size |
| 80 | 16 | BLAKE3 of the stored index (truncated) |
| 96 | 32 | Reserved |

The header is followed by the chunks, then the index (tagged sections, see `GCASSection`), then a 12-byte trailer
(`u64` pack size, magic) used to find packs appended to executables.
