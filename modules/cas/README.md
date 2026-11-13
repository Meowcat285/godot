# Content-addressed storage (CAS)

This module stores exported game data by content, in GCAS packs. Identical data is stored once, and patches only
contain the data that changed.

Content is addressed by [BLAKE3](https://github.com/BLAKE3-team/BLAKE3) hashes (`core/crypto/cas_hash.h`).

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
