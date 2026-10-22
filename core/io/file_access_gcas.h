/**************************************************************************/
/*  file_access_gcas.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/crypto/cas_hash.h"
#include "core/io/file_access_pack.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/list.h"
#include "core/templates/local_vector.h"

// GCAS ("Godot content-addressed store") packs.
//
// Layout (little-endian):
//   - 128 byte header (see GCASHeader).
//   - Chunk data: content-defined chunks, each independently compressed so any byte range can be read without
//     decompressing the rest of the file.
//   - Index: a sequence of tagged sections (one zstd frame), loaded in memory when the pack is mounted.
//   - Trailer: u64 pack size (excluding the trailer) and the magic, to find packs appended to executables.
//
// Files are blobs made of chunks. Chunks are identified by the BLAKE3 hash of their uncompressed contents, so
// identical data is only stored once. Patch packs reference chunks of the packs they are applied on by hash.

#define GCAS_HEADER_MAGIC 0x53414347 // "GCAS" in ASCII.
#define GCAS_FORMAT_VERSION 1
#define GCAS_HEADER_SIZE 128
#define GCAS_TRAILER_SIZE 12

// Readers ignore unknown compatible flags.
enum GCASCompatFlags : uint64_t {
	GCAS_COMPAT_LAYOUT_HINTS = 1 << 0, // Pack contains a layout hints section.
};

// Readers refuse packs with unknown incompatible flags.
enum GCASIncompatFlags : uint64_t {
	GCAS_INCOMPAT_PATCH = 1 << 0, // Pack references chunks of base packs, which must be mounted first.
	GCAS_INCOMPAT_ENCRYPTED = 1 << 1, // Reserved, not supported yet.
};

constexpr uint64_t GCAS_INCOMPAT_SUPPORTED = GCAS_INCOMPAT_PATCH;

enum GCASIndexCodec : uint32_t {
	GCAS_INDEX_CODEC_RAW = 0,
	GCAS_INDEX_CODEC_ZSTD = 1,
};

enum GCASChunkCodec : uint8_t {
	GCAS_CHUNK_CODEC_RAW = 0,
	GCAS_CHUNK_CODEC_ZSTD = 1,
};

constexpr uint32_t gcas_fourcc(const char p_tag[5]) {
	return uint32_t(uint8_t(p_tag[0])) | (uint32_t(uint8_t(p_tag[1])) << 8) | (uint32_t(uint8_t(p_tag[2])) << 16) | (uint32_t(uint8_t(p_tag[3])) << 24);
}

// Index sections. Each one is stored as: u32 tag, u32 flags, u64 size, payload.
// Unknown sections are skipped, unless they have GCAS_SECTION_FLAG_REQUIRED.
enum GCASSection : uint32_t {
	GCAS_SECTION_BASES = gcas_fourcc("BASE"), // u32 count, count * u8[16] base pack ID.
	GCAS_SECTION_CHUNKS = gcas_fourcc("CHNK"), // u32 count, count * GCAS_CHUNK_ENTRY_SIZE.
	GCAS_SECTION_EXTERNAL_CHUNKS = gcas_fourcc("EXTC"), // u32 count, count * u8[32] chunk hash.
	GCAS_SECTION_CHUNK_REFS = gcas_fourcc("REFS"), // u32 count, count * u32 chunk index (GCAS_CHUNK_REF_EXTERNAL for EXTC).
	GCAS_SECTION_BLOBS = gcas_fourcc("BLOB"), // u32 count, count * GCAS_BLOB_ENTRY_SIZE.
	GCAS_SECTION_STRINGS = gcas_fourcc("STRS"), // UTF-8 string pool.
	GCAS_SECTION_FILES = gcas_fourcc("FILE"), // u32 count, count * GCAS_FILE_ENTRY_SIZE.
	GCAS_SECTION_UIDS = gcas_fourcc("UIDS"), // u32 count, count * GCAS_UID_ENTRY_SIZE.
	GCAS_SECTION_HINTS = gcas_fourcc("HINT"), // u32 count, count * GCAS_HINT_ENTRY_SIZE, u32 size, hint data.
};

constexpr uint32_t GCAS_SECTION_FLAG_REQUIRED = 1 << 0;

// u8[32] hash, u64 offset (from pack start), u32 compressed size, u32 size, u8 codec, u8[3] reserved.
constexpr uint32_t GCAS_CHUNK_ENTRY_SIZE = 52;
// u8[32] hash, u64 size, u32 first chunk ref, u32 chunk ref count.
constexpr uint32_t GCAS_BLOB_ENTRY_SIZE = 48;
// u32 path offset, u32 path length (in the string pool, without "res://"), u32 blob index, u32 flags.
constexpr uint32_t GCAS_FILE_ENTRY_SIZE = 16;
// u64 uid, u32 path offset, u32 path length (in the string pool, without "res://").
// Paths don't have to be files of the pack (e.g. imported resources only store their `.import` file and artifacts).
constexpr uint32_t GCAS_UID_ENTRY_SIZE = 16;
// u32 file index, u32 provider offset, u32 provider length (in the string pool), u32 data offset, u32 data length.
constexpr uint32_t GCAS_HINT_ENTRY_SIZE = 20;

constexpr uint32_t GCAS_CHUNK_REF_EXTERNAL = 0x80000000;
constexpr uint32_t GCAS_FILE_BLOB_REMOVED = 0xFFFFFFFF;

struct GCASHeader {
	uint32_t magic = GCAS_HEADER_MAGIC;
	uint16_t format_version = GCAS_FORMAT_VERSION;
	uint16_t header_size = GCAS_HEADER_SIZE;
	uint32_t ver_major = 0;
	uint32_t ver_minor = 0;
	uint32_t ver_patch = 0;
	uint32_t index_codec = GCAS_INDEX_CODEC_ZSTD;
	uint64_t compat_flags = 0;
	uint64_t incompat_flags = 0;
	uint8_t pack_id[16] = {};
	uint64_t index_offset = 0; // From pack start.
	uint64_t index_compressed_size = 0;
	uint64_t index_size = 0;
	uint8_t index_checksum[16] = {}; // BLAKE3 (truncated) of the stored index.

	void encode(uint8_t r_buffer[GCAS_HEADER_SIZE]) const;
	bool decode(const uint8_t *p_buffer, uint32_t p_size);
};

// Decompressed chunks, shared by all open GCAS files.
class GCASChunkCache {
	struct Entry {
		Vector<uint8_t> data;
		List<CASHash>::Element *lru = nullptr;
	};

	Mutex mutex;
	HashMap<CASHash, Entry> entries;
	List<CASHash> lru; // Most recently used first.
	uint64_t used = 0;
	uint64_t budget = 16 * 1024 * 1024;
	bool budget_from_settings = false;
	uint64_t hits = 0;
	uint64_t misses = 0;

	void _evict();

public:
	static GCASChunkCache *get_singleton();

	bool get(const CASHash &p_hash, Vector<uint8_t> &r_data);
	void insert(const CASHash &p_hash, const Vector<uint8_t> &p_data);
	void clear();

	void set_budget(uint64_t p_bytes);
	uint64_t get_budget() const { return budget; }
	Dictionary get_stats();
};

class PackedSourceGCAS : public PackSource {
public:
	struct ChunkLocation {
		CASHash hash;
		uint64_t offset = 0; // Absolute offset in the pack file.
		uint32_t compressed_size = 0;
		uint32_t size = 0;
		uint32_t pack_file = 0; // Index in pack_files.
		uint8_t codec = GCAS_CHUNK_CODEC_RAW;
	};

	struct LayoutHint {
		String provider;
		Vector<uint8_t> data;
	};

	// Contents of a pack's index, without mounting it.
	struct PackInfo {
		struct File {
			String path;
			CASHash blob_hash;
			uint64_t size = 0;
			bool removed = false;
		};

		GCASHeader header;
		uint64_t pack_start = 0;
		LocalVector<String> base_pack_ids; // Hexadecimal.
		LocalVector<CASHash> chunk_hashes;
		LocalVector<CASHash> external_chunk_hashes;
		LocalVector<File> files;
		LocalVector<Pair<ResourceUID::ID, String>> uids;
	};

private:
	struct Blob {
		CASHash hash;
		uint64_t size = 0;
		uint32_t first_ref = 0;
		uint32_t ref_count = 0;
	};

	struct Pack {
		String path;
		uint8_t pack_id[16] = {};
		LocalVector<uint32_t> chunk_refs; // Global chunk indices.
		LocalVector<Blob> blobs;
	};

	static inline PackedSourceGCAS *singleton = nullptr;

	LocalVector<String> pack_files;
	LocalVector<ChunkLocation> chunks;
	HashMap<CASHash, uint32_t> chunk_index;
	LocalVector<Pack *> packs;
	HashMap<String, Pack *> packs_by_path;
	List<Pair<ResourceUID::ID, String>> uids;
	HashMap<String, Vector<LayoutHint>> layout_hints;

	static bool _locate_header(Ref<FileAccess> p_file, uint64_t p_offset, uint64_t &r_pack_start);
	static bool _load_index(const String &p_path, uint64_t p_offset, GCASHeader &r_header, uint64_t &r_pack_start, Vector<uint8_t> &r_index);
	bool _parse_index(const String &p_path, const Vector<uint8_t> &p_index, uint64_t p_pack_start, const GCASHeader &p_header, bool p_replace_files);

public:
	static PackedSourceGCAS *get_singleton() { return singleton; }

	static Error read_pack_info(const String &p_path, PackInfo &r_info, uint64_t p_offset = 0);

	virtual bool try_open_pack(const String &p_path, bool p_replace_files, uint64_t p_offset, const Vector<uint8_t> &p_decryption_key = Vector<uint8_t>()) override;
	virtual Ref<FileAccess> get_file(const String &p_path, PackedData::PackedFile *p_file, const Vector<uint8_t> &p_decryption_key = Vector<uint8_t>()) override;
	virtual void get_uids(List<Pair<ResourceUID::ID, String>> *r_uids) const override;
	virtual void clear_packs() override;

	bool is_pack_mounted(const uint8_t p_pack_id[16]) const;
	Vector<String> get_mounted_pack_paths() const;
	// Layout hints stored by the pack writer's layout providers for a file (e.g. mip ranges of streamed textures).
	Vector<uint8_t> get_layout_hints(const String &p_path, const String &p_provider) const;

	PackedSourceGCAS();
	~PackedSourceGCAS();
};

class FileAccessGCAS : public FileAccess {
	GDSOFTCLASS(FileAccessGCAS, FileAccess);

public:
	struct Chunk {
		PackedSourceGCAS::ChunkLocation location;
		uint64_t start = 0; // Position of the chunk in the file.
	};

private:
	String path;
	LocalVector<Chunk> chunks;
	LocalVector<String> pack_paths;
	mutable LocalVector<Ref<FileAccess>> pack_handles;
	uint64_t size = 0;
	bool open = false;

	mutable uint64_t pos = 0;
	mutable bool eof = false;
	mutable int64_t current_chunk = -1;
	mutable Vector<uint8_t> current_data;
	mutable Vector<uint8_t> compressed_buffer;

	int64_t _find_chunk(uint64_t p_position) const;
	Ref<FileAccess> _get_pack_handle(uint32_t p_pack_file) const;
	bool _read_chunk_data(const Chunk &p_chunk, uint8_t *r_dst) const;
	bool _get_chunk(int64_t p_chunk, Vector<uint8_t> &r_data, bool p_store_in_cache) const;

	virtual Error open_internal(const String &p_path, int p_mode_flags) override { return ERR_UNAVAILABLE; }
	virtual uint64_t _get_modified_time(const String &p_file) override { return 0; }
	virtual uint64_t _get_access_time(const String &p_file) override { return 0; }
	virtual int64_t _get_size(const String &p_file) override { return -1; }
	virtual BitField<FileAccess::UnixPermissionFlags> _get_unix_permissions(const String &p_file) override { return 0; }
	virtual Error _set_unix_permissions(const String &p_file, BitField<FileAccess::UnixPermissionFlags> p_permissions) override { return FAILED; }

	virtual bool _get_hidden_attribute(const String &p_file) override { return false; }
	virtual Error _set_hidden_attribute(const String &p_file, bool p_hidden) override { return ERR_UNAVAILABLE; }
	virtual bool _get_read_only_attribute(const String &p_file) override { return false; }
	virtual Error _set_read_only_attribute(const String &p_file, bool p_ro) override { return ERR_UNAVAILABLE; }

public:
	virtual bool is_open() const override { return open; }

	virtual String get_path() const override { return path; }
	virtual String get_path_absolute() const override { return path; }

	virtual void seek(uint64_t p_position) override;
	virtual void seek_end(int64_t p_position = 0) override;
	virtual uint64_t get_position() const override { return pos; }
	virtual uint64_t get_length() const override { return size; }

	virtual bool eof_reached() const override { return eof; }

	virtual uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const override;
	virtual void prefetch(uint64_t p_offset, uint64_t p_length) const override;

	virtual Error get_error() const override { return eof ? ERR_FILE_EOF : OK; }

	virtual Error resize(int64_t p_length) override { return ERR_UNAVAILABLE; }
	virtual void flush() override {}
	virtual bool store_buffer(const uint8_t *p_src, uint64_t p_length) override { return false; }

	virtual bool file_exists(const String &p_name) override { return false; }

	virtual void close() override;

	uint32_t get_chunk_count() const { return chunks.size(); }
	const Chunk &get_chunk(uint32_t p_index) const { return chunks[p_index]; }

	FileAccessGCAS(const String &p_path, uint64_t p_size, const LocalVector<Chunk> &p_chunks, const LocalVector<String> &p_pack_paths);
};
