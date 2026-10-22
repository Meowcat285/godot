/**************************************************************************/
/*  file_access_gcas.cpp                                                  */
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

#include "file_access_gcas.h"

#include "core/config/project_settings.h"
#include "core/io/compression.h"
#include "core/io/file_access_patched.h"
#include "core/os/os.h"
#include "core/version.h"

namespace {

void encode_u16(uint8_t *p_dst, uint16_t p_value) {
	p_dst[0] = p_value & 0xFF;
	p_dst[1] = (p_value >> 8) & 0xFF;
}

void encode_u32(uint8_t *p_dst, uint32_t p_value) {
	for (int i = 0; i < 4; i++) {
		p_dst[i] = (p_value >> (i * 8)) & 0xFF;
	}
}

void encode_u64(uint8_t *p_dst, uint64_t p_value) {
	for (int i = 0; i < 8; i++) {
		p_dst[i] = (p_value >> (i * 8)) & 0xFF;
	}
}

uint16_t decode_u16(const uint8_t *p_src) {
	return uint16_t(p_src[0]) | (uint16_t(p_src[1]) << 8);
}

uint32_t decode_u32(const uint8_t *p_src) {
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		v |= uint32_t(p_src[i]) << (i * 8);
	}
	return v;
}

uint64_t decode_u64(const uint8_t *p_src) {
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) {
		v |= uint64_t(p_src[i]) << (i * 8);
	}
	return v;
}

// Bounds-checked reader over index data.
struct IndexReader {
	const uint8_t *ptr = nullptr;
	uint64_t size = 0;
	uint64_t pos = 0;
	bool error = false;

	bool has(uint64_t p_bytes) {
		if (error || p_bytes > size - pos) {
			error = true;
			return false;
		}
		return true;
	}

	uint32_t u32() {
		if (!has(4)) {
			return 0;
		}
		uint32_t v = decode_u32(ptr + pos);
		pos += 4;
		return v;
	}

	uint64_t u64() {
		if (!has(8)) {
			return 0;
		}
		uint64_t v = decode_u64(ptr + pos);
		pos += 8;
		return v;
	}

	const uint8_t *bytes(uint64_t p_bytes) {
		if (!has(p_bytes)) {
			return nullptr;
		}
		const uint8_t *p = ptr + pos;
		pos += p_bytes;
		return p;
	}

	// Reads a u32 element count and checks that the elements fit in the remaining data.
	uint32_t count(uint32_t p_element_size) {
		uint32_t c = u32();
		if (!error && uint64_t(c) * p_element_size > size - pos) {
			error = true;
			return 0;
		}
		return c;
	}

	IndexReader() {}
	IndexReader(const uint8_t *p_ptr, uint64_t p_size) :
			ptr(p_ptr), size(p_size) {}
};

constexpr uint64_t RAW_DIRECT_READ_MIN = 16384;

} // namespace

//////////////////////////////////////////////////////////////////

void GCASHeader::encode(uint8_t r_buffer[GCAS_HEADER_SIZE]) const {
	memset(r_buffer, 0, GCAS_HEADER_SIZE);
	encode_u32(r_buffer + 0, magic);
	encode_u16(r_buffer + 4, format_version);
	encode_u16(r_buffer + 6, header_size);
	encode_u32(r_buffer + 8, ver_major);
	encode_u32(r_buffer + 12, ver_minor);
	encode_u32(r_buffer + 16, ver_patch);
	encode_u32(r_buffer + 20, index_codec);
	encode_u64(r_buffer + 24, compat_flags);
	encode_u64(r_buffer + 32, incompat_flags);
	memcpy(r_buffer + 40, pack_id, 16);
	encode_u64(r_buffer + 56, index_offset);
	encode_u64(r_buffer + 64, index_compressed_size);
	encode_u64(r_buffer + 72, index_size);
	memcpy(r_buffer + 80, index_checksum, 16);
	// 96-127: reserved.
}

bool GCASHeader::decode(const uint8_t *p_buffer, uint32_t p_size) {
	if (p_size < GCAS_HEADER_SIZE) {
		return false;
	}
	magic = decode_u32(p_buffer + 0);
	format_version = decode_u16(p_buffer + 4);
	header_size = decode_u16(p_buffer + 6);
	ver_major = decode_u32(p_buffer + 8);
	ver_minor = decode_u32(p_buffer + 12);
	ver_patch = decode_u32(p_buffer + 16);
	index_codec = decode_u32(p_buffer + 20);
	compat_flags = decode_u64(p_buffer + 24);
	incompat_flags = decode_u64(p_buffer + 32);
	memcpy(pack_id, p_buffer + 40, 16);
	index_offset = decode_u64(p_buffer + 56);
	index_compressed_size = decode_u64(p_buffer + 64);
	index_size = decode_u64(p_buffer + 72);
	memcpy(index_checksum, p_buffer + 80, 16);
	return magic == GCAS_HEADER_MAGIC && header_size >= GCAS_HEADER_SIZE;
}

//////////////////////////////////////////////////////////////////

static GCASChunkCache *chunk_cache_singleton = nullptr;

GCASChunkCache *GCASChunkCache::get_singleton() {
	return chunk_cache_singleton;
}

void GCASChunkCache::_evict() {
	while (used > budget && !lru.is_empty()) {
		List<CASHash>::Element *E = lru.back();
		HashMap<CASHash, Entry>::Iterator I = entries.find(E->get());
		if (I) {
			used -= I->value.data.size();
			entries.remove(I);
		}
		lru.erase(E);
	}
}

bool GCASChunkCache::get(const CASHash &p_hash, Vector<uint8_t> &r_data) {
	MutexLock lock(mutex);
	Entry *e = entries.getptr(p_hash);
	if (!e) {
		misses++;
		return false;
	}
	hits++;
	lru.move_to_front(e->lru);
	r_data = e->data;
	return true;
}

void GCASChunkCache::insert(const CASHash &p_hash, const Vector<uint8_t> &p_data) {
	MutexLock lock(mutex);
	if (unlikely(!budget_from_settings)) {
		ProjectSettings *ps = ProjectSettings::get_singleton();
		if (ps && ps->is_project_loaded() && ps->has_setting("file_access/gcas/chunk_cache_size_mb")) {
			budget = uint64_t(MAX(0, int(ps->get_setting("file_access/gcas/chunk_cache_size_mb")))) * 1024 * 1024;
			budget_from_settings = true;
		}
	}
	if (entries.has(p_hash) || uint64_t(p_data.size()) > budget) {
		return;
	}
	Entry e;
	e.data = p_data;
	e.lru = lru.push_front(p_hash);
	entries.insert(p_hash, e);
	used += p_data.size();
	_evict();
}

void GCASChunkCache::clear() {
	MutexLock lock(mutex);
	entries.clear();
	lru.clear();
	used = 0;
}

void GCASChunkCache::set_budget(uint64_t p_bytes) {
	MutexLock lock(mutex);
	budget = p_bytes;
	budget_from_settings = true;
	_evict();
}

Dictionary GCASChunkCache::get_stats() {
	MutexLock lock(mutex);
	Dictionary stats;
	stats["hits"] = hits;
	stats["misses"] = misses;
	stats["entries"] = entries.size();
	stats["used_bytes"] = used;
	stats["budget_bytes"] = budget;
	return stats;
}

//////////////////////////////////////////////////////////////////

bool PackedSourceGCAS::_locate_header(Ref<FileAccess> p_file, uint64_t p_offset, uint64_t &r_pack_start) {
	p_file->seek(p_offset);
	if (p_file->get_32() == GCAS_HEADER_MAGIC) {
		r_pack_start = p_offset;
		return true;
	}

	// Self-contained executables don't support loading with an offset.
	if (p_offset != 0) {
		return false;
	}

	// Search for the header in the executable "pck" section.
	int64_t section_ofs = OS::get_singleton()->get_embedded_pck_offset();
	if (section_ofs != 0) {
		// Search for the header, in case the pack start and section have different alignment.
		for (int i = 0; i < 8; i++) {
			p_file->seek(section_ofs + i);
			if (p_file->get_32() == GCAS_HEADER_MAGIC) {
				r_pack_start = section_ofs + i;
				return true;
			}
		}
	}

	// Search for the trailer at the end of the file.
	uint64_t length = p_file->get_length();
	if (length < GCAS_HEADER_SIZE + GCAS_TRAILER_SIZE) {
		return false;
	}
	p_file->seek(length - 4);
	if (p_file->get_32() != GCAS_HEADER_MAGIC) {
		return false;
	}
	p_file->seek(length - GCAS_TRAILER_SIZE);
	uint64_t pack_size = p_file->get_64();
	if (pack_size > length - GCAS_TRAILER_SIZE) {
		return false;
	}
	uint64_t start = length - GCAS_TRAILER_SIZE - pack_size;
	p_file->seek(start);
	if (p_file->get_32() != GCAS_HEADER_MAGIC) {
		return false;
	}
	r_pack_start = start;
	return true;
}

bool PackedSourceGCAS::_load_index(const String &p_path, uint64_t p_offset, GCASHeader &r_header, uint64_t &r_pack_start, Vector<uint8_t> &r_index) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null()) {
		return false;
	}

	uint64_t pack_start = 0;
	if (!_locate_header(f, p_offset, pack_start)) {
		return false;
	}

	f->seek(pack_start);
	uint8_t header_data[GCAS_HEADER_SIZE];
	ERR_FAIL_COND_V_MSG(f->get_buffer(header_data, GCAS_HEADER_SIZE) != GCAS_HEADER_SIZE, false, vformat("Truncated GCAS header in \"%s\".", p_path));
	GCASHeader &header = r_header;
	ERR_FAIL_COND_V_MSG(!header.decode(header_data, GCAS_HEADER_SIZE), false, vformat("Invalid GCAS header in \"%s\".", p_path));
	ERR_FAIL_COND_V_MSG(header.format_version != GCAS_FORMAT_VERSION, false, vformat("GCAS format version unsupported: %d.", header.format_version));
	ERR_FAIL_COND_V_MSG(header.ver_major > GODOT_VERSION_MAJOR || (header.ver_major == GODOT_VERSION_MAJOR && header.ver_minor > GODOT_VERSION_MINOR), false, vformat("GCAS pack created with a newer version of the engine: %d.%d.%d.", header.ver_major, header.ver_minor, header.ver_patch));
	ERR_FAIL_COND_V_MSG(header.incompat_flags & ~GCAS_INCOMPAT_SUPPORTED, false, vformat("GCAS pack \"%s\" uses unsupported features (flags 0x%x).", p_path, header.incompat_flags & ~GCAS_INCOMPAT_SUPPORTED));

	// Read and verify the index.
	ERR_FAIL_COND_V_MSG(header.index_compressed_size > f->get_length() || header.index_offset > f->get_length() - pack_start - header.index_compressed_size, false, vformat("Invalid GCAS index location in \"%s\".", p_path));
	Vector<uint8_t> stored_index;
	stored_index.resize_uninitialized(header.index_compressed_size);
	f->seek(pack_start + header.index_offset);
	ERR_FAIL_COND_V_MSG(f->get_buffer(stored_index.ptrw(), stored_index.size()) != uint64_t(stored_index.size()), false, vformat("Truncated GCAS index in \"%s\".", p_path));

	uint8_t checksum[16];
	Blake3Context ctx;
	ctx.start();
	ctx.update(stored_index);
	ctx.finish(checksum, 16);
	ERR_FAIL_COND_V_MSG(memcmp(checksum, header.index_checksum, 16) != 0, false, vformat("Corrupted GCAS index in \"%s\".", p_path));

	if (header.index_codec == GCAS_INDEX_CODEC_RAW) {
		r_index = stored_index;
	} else if (header.index_codec == GCAS_INDEX_CODEC_ZSTD) {
		r_index.resize_uninitialized(header.index_size);
		int64_t ret = Compression::decompress(r_index.ptrw(), r_index.size(), stored_index.ptr(), stored_index.size(), Compression::MODE_ZSTD);
		ERR_FAIL_COND_V_MSG(ret != int64_t(header.index_size), false, vformat("Can't decompress GCAS index in \"%s\".", p_path));
	} else {
		ERR_FAIL_V_MSG(false, vformat("Unsupported GCAS index codec in \"%s\".", p_path));
	}

	r_pack_start = pack_start;
	return true;
}

bool PackedSourceGCAS::try_open_pack(const String &p_path, bool p_replace_files, uint64_t p_offset, const Vector<uint8_t> &p_decryption_key) {
	GCASHeader header;
	uint64_t pack_start = 0;
	Vector<uint8_t> index;
	if (!_load_index(p_path, p_offset, header, pack_start, index)) {
		return false;
	}
	return _parse_index(p_path, index, pack_start, header, p_replace_files);
}

static bool _gcas_split_sections(const String &p_path, const Vector<uint8_t> &p_index, HashMap<uint32_t, IndexReader> &r_sections) {
	IndexReader reader(p_index.ptr(), p_index.size());
	while (reader.pos < reader.size) {
		uint32_t tag = reader.u32();
		uint32_t flags = reader.u32();
		uint64_t section_size = reader.u64();
		const uint8_t *payload = reader.bytes(section_size);
		ERR_FAIL_COND_V_MSG(reader.error, false, vformat("Corrupted GCAS index in \"%s\".", p_path));
		switch (tag) {
			case GCAS_SECTION_BASES:
			case GCAS_SECTION_CHUNKS:
			case GCAS_SECTION_EXTERNAL_CHUNKS:
			case GCAS_SECTION_CHUNK_REFS:
			case GCAS_SECTION_BLOBS:
			case GCAS_SECTION_STRINGS:
			case GCAS_SECTION_FILES:
			case GCAS_SECTION_UIDS:
			case GCAS_SECTION_HINTS:
				r_sections[tag] = IndexReader(payload, section_size);
				break;
			default:
				ERR_FAIL_COND_V_MSG(flags & GCAS_SECTION_FLAG_REQUIRED, false, vformat("GCAS pack \"%s\" requires an unsupported index section.", p_path));
				break;
		}
	}
	return true;
}

Error PackedSourceGCAS::read_pack_info(const String &p_path, PackInfo &r_info, uint64_t p_offset) {
	Vector<uint8_t> index;
	if (!_load_index(p_path, p_offset, r_info.header, r_info.pack_start, index)) {
		return ERR_FILE_UNRECOGNIZED;
	}
	HashMap<uint32_t, IndexReader> sections;
	if (!_gcas_split_sections(p_path, index, sections)) {
		return ERR_FILE_CORRUPT;
	}

	if (IndexReader *r = sections.getptr(GCAS_SECTION_BASES)) {
		uint32_t count = r->count(16);
		for (uint32_t i = 0; i < count; i++) {
			r_info.base_pack_ids.push_back(String::hex_encode_buffer(r->bytes(16), 16));
		}
	}
	if (IndexReader *r = sections.getptr(GCAS_SECTION_CHUNKS)) {
		uint32_t count = r->count(GCAS_CHUNK_ENTRY_SIZE);
		r_info.chunk_hashes.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			memcpy(r_info.chunk_hashes[i].data, r->bytes(GCAS_CHUNK_ENTRY_SIZE), CASHash::SIZE);
		}
	}
	if (IndexReader *r = sections.getptr(GCAS_SECTION_EXTERNAL_CHUNKS)) {
		uint32_t count = r->count(CASHash::SIZE);
		r_info.external_chunk_hashes.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			memcpy(r_info.external_chunk_hashes[i].data, r->bytes(CASHash::SIZE), CASHash::SIZE);
		}
	}

	IndexReader *strings = sections.getptr(GCAS_SECTION_STRINGS);
	IndexReader *blob_section = sections.getptr(GCAS_SECTION_BLOBS);
	IndexReader *file_section = sections.getptr(GCAS_SECTION_FILES);
	ERR_FAIL_COND_V_MSG(!strings || !blob_section || !file_section, ERR_FILE_CORRUPT, vformat("GCAS pack \"%s\" is missing index sections.", p_path));

	uint32_t blob_count = blob_section->count(GCAS_BLOB_ENTRY_SIZE);
	const uint8_t *blobs = blob_section->bytes(uint64_t(blob_count) * GCAS_BLOB_ENTRY_SIZE);
	uint32_t file_count = file_section->count(GCAS_FILE_ENTRY_SIZE);
	r_info.files.resize(file_count);
	for (uint32_t i = 0; i < file_count; i++) {
		const uint8_t *e = file_section->bytes(GCAS_FILE_ENTRY_SIZE);
		uint32_t offset = decode_u32(e + 0);
		uint32_t length = decode_u32(e + 4);
		ERR_FAIL_COND_V(uint64_t(offset) + length > strings->size, ERR_FILE_CORRUPT);
		PackInfo::File &file = r_info.files[i];
		file.path = "res://" + String::utf8((const char *)strings->ptr + offset, length);
		uint32_t blob = decode_u32(e + 8);
		if (blob == GCAS_FILE_BLOB_REMOVED) {
			file.removed = true;
		} else {
			ERR_FAIL_COND_V(blob >= blob_count, ERR_FILE_CORRUPT);
			memcpy(file.blob_hash.data, blobs + blob * GCAS_BLOB_ENTRY_SIZE, CASHash::SIZE);
			file.size = decode_u64(blobs + blob * GCAS_BLOB_ENTRY_SIZE + 32);
		}
	}

	if (IndexReader *r = sections.getptr(GCAS_SECTION_UIDS)) {
		uint32_t count = r->count(GCAS_UID_ENTRY_SIZE);
		for (uint32_t i = 0; i < count; i++) {
			ResourceUID::ID id = ResourceUID::ID(r->u64());
			uint32_t offset = r->u32();
			uint32_t length = r->u32();
			ERR_FAIL_COND_V(r->error || uint64_t(offset) + length > strings->size, ERR_FILE_CORRUPT);
			r_info.uids.push_back(Pair<ResourceUID::ID, String>(id, "res://" + String::utf8((const char *)strings->ptr + offset, length)));
		}
	}

	return OK;
}

bool PackedSourceGCAS::_parse_index(const String &p_path, const Vector<uint8_t> &p_index, uint64_t p_pack_start, const GCASHeader &p_header, bool p_replace_files) {
	HashMap<uint32_t, IndexReader> sections;
	if (!_gcas_split_sections(p_path, p_index, sections)) {
		return false;
	}

	const char *corrupt_message = "Corrupted GCAS index.";

	// Base packs must be mounted first.
	if (IndexReader *r = sections.getptr(GCAS_SECTION_BASES)) {
		uint32_t count = r->count(16);
		for (uint32_t i = 0; i < count; i++) {
			const uint8_t *id = r->bytes(16);
			ERR_FAIL_COND_V_MSG(!is_pack_mounted(id), false, vformat("GCAS patch \"%s\" requires base pack %s, which isn't loaded.", p_path, String::hex_encode_buffer(id, 16)));
		}
		ERR_FAIL_COND_V_MSG(r->error, false, corrupt_message);
	}

	// Resolve external chunks before registering anything.
	LocalVector<uint32_t> external;
	if (IndexReader *r = sections.getptr(GCAS_SECTION_EXTERNAL_CHUNKS)) {
		uint32_t count = r->count(CASHash::SIZE);
		external.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			CASHash h;
			memcpy(h.data, r->bytes(CASHash::SIZE), CASHash::SIZE);
			const uint32_t *gid = chunk_index.getptr(h);
			ERR_FAIL_NULL_V_MSG(gid, false, vformat("GCAS patch \"%s\" references chunk %s, which isn't in any loaded pack.", p_path, h.to_string()));
			external[i] = *gid;
		}
	}

	IndexReader *strings = sections.getptr(GCAS_SECTION_STRINGS);
	IndexReader *chunk_section = sections.getptr(GCAS_SECTION_CHUNKS);
	IndexReader *ref_section = sections.getptr(GCAS_SECTION_CHUNK_REFS);
	IndexReader *blob_section = sections.getptr(GCAS_SECTION_BLOBS);
	IndexReader *file_section = sections.getptr(GCAS_SECTION_FILES);
	ERR_FAIL_COND_V_MSG(!strings || !chunk_section || !ref_section || !blob_section || !file_section, false, vformat("GCAS pack \"%s\" is missing index sections.", p_path));

	// Chunks.
	uint32_t pack_file = pack_files.size();
	pack_files.push_back(p_path);
	uint32_t chunk_count = chunk_section->count(GCAS_CHUNK_ENTRY_SIZE);
	LocalVector<uint32_t> local_chunks;
	local_chunks.resize(chunk_count);
	for (uint32_t i = 0; i < chunk_count; i++) {
		const uint8_t *e = chunk_section->bytes(GCAS_CHUNK_ENTRY_SIZE);
		ChunkLocation loc;
		memcpy(loc.hash.data, e, CASHash::SIZE);
		loc.offset = p_pack_start + decode_u64(e + 32);
		loc.compressed_size = decode_u32(e + 40);
		loc.size = decode_u32(e + 44);
		loc.codec = e[48];
		loc.pack_file = pack_file;
		ERR_FAIL_COND_V_MSG(loc.codec != GCAS_CHUNK_CODEC_RAW && loc.codec != GCAS_CHUNK_CODEC_ZSTD, false, vformat("Unsupported GCAS chunk codec in \"%s\".", p_path));

		const uint32_t *existing = chunk_index.getptr(loc.hash);
		if (existing) {
			local_chunks[i] = *existing;
		} else {
			local_chunks[i] = chunks.size();
			chunk_index[loc.hash] = chunks.size();
			chunks.push_back(loc);
		}
	}
	ERR_FAIL_COND_V_MSG(chunk_section->error, false, corrupt_message);

	Pack *pack = memnew(Pack);
	pack->path = p_path;
	memcpy(pack->pack_id, p_header.pack_id, 16);
	packs.push_back(pack);

	// Chunk references.
	uint32_t ref_count = ref_section->count(4);
	pack->chunk_refs.resize(ref_count);
	for (uint32_t i = 0; i < ref_count; i++) {
		uint32_t ref = ref_section->u32();
		if (ref & GCAS_CHUNK_REF_EXTERNAL) {
			ref &= ~GCAS_CHUNK_REF_EXTERNAL;
			ERR_FAIL_COND_V_MSG(ref >= external.size(), false, corrupt_message);
			pack->chunk_refs[i] = external[ref];
		} else {
			ERR_FAIL_COND_V_MSG(ref >= local_chunks.size(), false, corrupt_message);
			pack->chunk_refs[i] = local_chunks[ref];
		}
	}

	// Blobs.
	uint32_t blob_count = blob_section->count(GCAS_BLOB_ENTRY_SIZE);
	pack->blobs.resize(blob_count);
	for (uint32_t i = 0; i < blob_count; i++) {
		const uint8_t *e = blob_section->bytes(GCAS_BLOB_ENTRY_SIZE);
		Blob &b = pack->blobs[i];
		memcpy(b.hash.data, e, CASHash::SIZE);
		b.size = decode_u64(e + 32);
		b.first_ref = decode_u32(e + 40);
		b.ref_count = decode_u32(e + 44);
		ERR_FAIL_COND_V_MSG(uint64_t(b.first_ref) + b.ref_count > pack->chunk_refs.size(), false, corrupt_message);
		uint64_t total = 0;
		for (uint32_t j = 0; j < b.ref_count; j++) {
			total += chunks[pack->chunk_refs[b.first_ref + j]].size;
		}
		ERR_FAIL_COND_V_MSG(total != b.size, false, corrupt_message);
	}

	// Files.
	auto get_string = [&](uint32_t p_offset, uint32_t p_length, String &r_string) -> bool {
		if (uint64_t(p_offset) + p_length > strings->size) {
			return false;
		}
		r_string = String::utf8((const char *)strings->ptr + p_offset, p_length);
		return true;
	};

	uint32_t file_count = file_section->count(GCAS_FILE_ENTRY_SIZE);
	LocalVector<String> file_paths;
	file_paths.resize(file_count);
	for (uint32_t i = 0; i < file_count; i++) {
		const uint8_t *e = file_section->bytes(GCAS_FILE_ENTRY_SIZE);
		String path;
		ERR_FAIL_COND_V_MSG(!get_string(decode_u32(e + 0), decode_u32(e + 4), path), false, corrupt_message);
		path = "res://" + path;
		file_paths[i] = path;

		uint32_t blob = decode_u32(e + 8);
		if (blob == GCAS_FILE_BLOB_REMOVED) {
			PackedData::get_singleton()->remove_path(path);
			layout_hints.erase(path);
			continue;
		}
		ERR_FAIL_COND_V_MSG(blob >= pack->blobs.size(), false, corrupt_message);
		if (p_replace_files || !PackedData::get_singleton()->has_path(path)) {
			layout_hints.erase(path);
		}
		// The pack directory uses MD5 sized hashes, the start of the BLAKE3 hash is as good.
		PackedData::get_singleton()->add_path(p_path, path, uint64_t(blob) + 1, pack->blobs[blob].size, pack->blobs[blob].hash.data, this, p_replace_files);
	}

	// UIDs.
	if (IndexReader *r = sections.getptr(GCAS_SECTION_UIDS)) {
		uint32_t count = r->count(GCAS_UID_ENTRY_SIZE);
		for (uint32_t i = 0; i < count; i++) {
			ResourceUID::ID id = ResourceUID::ID(r->u64());
			uint32_t offset = r->u32();
			uint32_t length = r->u32();
			String path;
			ERR_FAIL_COND_V_MSG(r->error || !get_string(offset, length, path), false, corrupt_message);
			uids.push_back(Pair<ResourceUID::ID, String>(id, "res://" + path));
		}
	}

	// Layout hints.
	if (IndexReader *r = sections.getptr(GCAS_SECTION_HINTS)) {
		uint32_t count = r->count(GCAS_HINT_ENTRY_SIZE);
		const uint8_t *entries = r->bytes(uint64_t(count) * GCAS_HINT_ENTRY_SIZE);
		uint32_t data_size = r->count(1);
		const uint8_t *data = r->bytes(data_size);
		ERR_FAIL_COND_V_MSG(r->error, false, corrupt_message);
		for (uint32_t i = 0; i < count; i++) {
			const uint8_t *e = entries + i * GCAS_HINT_ENTRY_SIZE;
			uint32_t file = decode_u32(e + 0);
			uint32_t data_offset = decode_u32(e + 12);
			uint32_t data_length = decode_u32(e + 16);
			LayoutHint hint;
			ERR_FAIL_COND_V_MSG(file >= file_paths.size() || !get_string(decode_u32(e + 4), decode_u32(e + 8), hint.provider), false, corrupt_message);
			ERR_FAIL_COND_V_MSG(uint64_t(data_offset) + data_length > data_size, false, corrupt_message);
			hint.data.resize_uninitialized(data_length);
			memcpy(hint.data.ptrw(), data + data_offset, data_length);
			layout_hints[file_paths[file]].push_back(hint);
		}
	}

	ERR_FAIL_COND_V_MSG(ref_section->error || blob_section->error || file_section->error, false, corrupt_message);

	packs_by_path[p_path] = pack;
	return true;
}

Ref<FileAccess> PackedSourceGCAS::get_file(const String &p_path, PackedData::PackedFile *p_file, const Vector<uint8_t> &p_decryption_key) {
	Pack **pack_ptr = packs_by_path.getptr(p_file->pack);
	ERR_FAIL_NULL_V_MSG(pack_ptr, Ref<FileAccess>(), vformat("GCAS pack \"%s\" isn't loaded.", p_file->pack));
	Pack *pack = *pack_ptr;
	uint64_t blob_index = p_file->offset - 1;
	ERR_FAIL_COND_V(blob_index >= pack->blobs.size(), Ref<FileAccess>());
	const Blob &blob = pack->blobs[blob_index];

	LocalVector<FileAccessGCAS::Chunk> file_chunks;
	file_chunks.resize(blob.ref_count);
	LocalVector<String> file_packs;
	HashMap<uint32_t, uint32_t> pack_remap;
	uint64_t start = 0;
	for (uint32_t i = 0; i < blob.ref_count; i++) {
		FileAccessGCAS::Chunk &c = file_chunks[i];
		c.location = chunks[pack->chunk_refs[blob.first_ref + i]];
		c.start = start;
		start += c.location.size;

		uint32_t *local = pack_remap.getptr(c.location.pack_file);
		if (local) {
			c.location.pack_file = *local;
		} else {
			uint32_t idx = file_packs.size();
			file_packs.push_back(pack_files[c.location.pack_file]);
			pack_remap[c.location.pack_file] = idx;
			c.location.pack_file = idx;
		}
	}

	Ref<FileAccess> file = memnew(FileAccessGCAS(p_path, blob.size, file_chunks, file_packs));

	// Delta patches from PCK patches can be applied on top of files from GCAS packs.
	if (PackedData::get_singleton()->has_delta_patches(p_path)) {
		Ref<FileAccessPatched> file_patched;
		file_patched.instantiate();
		Error err = file_patched->open_custom(file);
		ERR_FAIL_COND_V(err != OK, Ref<FileAccess>());
		file = file_patched;
	}

	return file;
}

void PackedSourceGCAS::get_uids(List<Pair<ResourceUID::ID, String>> *r_uids) const {
	for (const Pair<ResourceUID::ID, String> &E : uids) {
		r_uids->push_back(E);
	}
}

void PackedSourceGCAS::clear_packs() {
	for (Pack *pack : packs) {
		memdelete(pack);
	}
	packs.clear();
	packs_by_path.clear();
	pack_files.clear();
	chunks.clear();
	chunk_index.clear();
	uids.clear();
	layout_hints.clear();
	if (GCASChunkCache::get_singleton()) {
		GCASChunkCache::get_singleton()->clear();
	}
}

bool PackedSourceGCAS::is_pack_mounted(const uint8_t p_pack_id[16]) const {
	for (const Pack *pack : packs) {
		if (memcmp(pack->pack_id, p_pack_id, 16) == 0) {
			return true;
		}
	}
	return false;
}

Vector<String> PackedSourceGCAS::get_mounted_pack_paths() const {
	Vector<String> paths;
	for (const Pack *pack : packs) {
		if (!paths.has(pack->path)) {
			paths.push_back(pack->path);
		}
	}
	return paths;
}

Vector<uint8_t> PackedSourceGCAS::get_layout_hints(const String &p_path, const String &p_provider) const {
	String path = ResourceUID::ensure_path(p_path).simplify_path();
	if (!path.begins_with("res://")) {
		path = "res://" + path;
	}
	const Vector<LayoutHint> *hints = layout_hints.getptr(path);
	if (hints) {
		for (const LayoutHint &hint : *hints) {
			if (hint.provider == p_provider) {
				return hint.data;
			}
		}
	}
	return Vector<uint8_t>();
}

PackedSourceGCAS::PackedSourceGCAS() {
	singleton = this;
	if (!chunk_cache_singleton) {
		chunk_cache_singleton = memnew(GCASChunkCache);
	}
}

PackedSourceGCAS::~PackedSourceGCAS() {
	clear_packs();
	if (singleton == this) {
		singleton = nullptr;
		if (chunk_cache_singleton) {
			memdelete(chunk_cache_singleton);
			chunk_cache_singleton = nullptr;
		}
	}
}

//////////////////////////////////////////////////////////////////

int64_t FileAccessGCAS::_find_chunk(uint64_t p_position) const {
	if (p_position >= size) {
		return -1;
	}
	// Sequential reads are the common case.
	if (current_chunk >= 0) {
		const Chunk &c = chunks[current_chunk];
		if (p_position >= c.start && p_position < c.start + c.location.size) {
			return current_chunk;
		}
		if (uint32_t(current_chunk + 1) < chunks.size()) {
			const Chunk &n = chunks[current_chunk + 1];
			if (p_position >= n.start && p_position < n.start + n.location.size) {
				return current_chunk + 1;
			}
		}
	}
	int64_t lo = 0;
	int64_t hi = int64_t(chunks.size()) - 1;
	while (lo < hi) {
		int64_t mid = (lo + hi + 1) / 2;
		if (chunks[mid].start <= p_position) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	return lo;
}

Ref<FileAccess> FileAccessGCAS::_get_pack_handle(uint32_t p_pack_file) const {
	Ref<FileAccess> &f = pack_handles[p_pack_file];
	if (f.is_null()) {
		f = FileAccess::open(pack_paths[p_pack_file], FileAccess::READ);
		ERR_FAIL_COND_V_MSG(f.is_null(), Ref<FileAccess>(), vformat("Can't open GCAS pack \"%s\".", pack_paths[p_pack_file]));
	}
	return f;
}

bool FileAccessGCAS::_read_chunk_data(const Chunk &p_chunk, uint8_t *r_dst) const {
	const PackedSourceGCAS::ChunkLocation &loc = p_chunk.location;
	Ref<FileAccess> f = _get_pack_handle(loc.pack_file);
	if (f.is_null()) {
		return false;
	}
	f->seek(loc.offset);
	if (loc.codec == GCAS_CHUNK_CODEC_RAW) {
		return f->get_buffer(r_dst, loc.size) == loc.size;
	}

	if (uint32_t(compressed_buffer.size()) < loc.compressed_size) {
		compressed_buffer.resize_uninitialized(loc.compressed_size);
	}
	ERR_FAIL_COND_V_MSG(f->get_buffer(compressed_buffer.ptrw(), loc.compressed_size) != loc.compressed_size, false, vformat("Truncated GCAS chunk in \"%s\".", path));
	int64_t ret = Compression::decompress(r_dst, loc.size, compressed_buffer.ptr(), loc.compressed_size, Compression::MODE_ZSTD);
	ERR_FAIL_COND_V_MSG(ret != int64_t(loc.size), false, vformat("Corrupted GCAS chunk in \"%s\".", path));
	return true;
}

bool FileAccessGCAS::_get_chunk(int64_t p_chunk, Vector<uint8_t> &r_data, bool p_store_in_cache) const {
	const Chunk &c = chunks[p_chunk];
	GCASChunkCache *cache = GCASChunkCache::get_singleton();
	bool cacheable = cache && c.location.codec != GCAS_CHUNK_CODEC_RAW;
	if (cacheable && cache->get(c.location.hash, r_data)) {
		return true;
	}
	r_data.resize_uninitialized(c.location.size);
	if (!_read_chunk_data(c, r_data.ptrw())) {
		r_data.clear();
		return false;
	}
	if (cacheable && p_store_in_cache) {
		cache->insert(c.location.hash, r_data);
	}
	return true;
}

void FileAccessGCAS::seek(uint64_t p_position) {
	ERR_FAIL_COND_MSG(!open, "File must be opened before use.");
	eof = p_position > size;
	pos = p_position;
}

void FileAccessGCAS::seek_end(int64_t p_position) {
	seek(size + p_position);
}

uint64_t FileAccessGCAS::get_buffer(uint8_t *p_dst, uint64_t p_length) const {
	ERR_FAIL_COND_V_MSG(!open, -1, "File must be opened before use.");
	ERR_FAIL_COND_V(!p_dst && p_length > 0, -1);

	if (eof) {
		return 0;
	}

	uint64_t to_read = p_length;
	if (pos + to_read > size) {
		eof = true;
		to_read = pos < size ? size - pos : 0;
	}

	uint64_t done = 0;
	while (done < to_read) {
		int64_t ci = _find_chunk(pos);
		if (ci < 0) {
			break;
		}
		const Chunk &c = chunks[ci];
		uint64_t in_chunk = pos - c.start;
		uint64_t n = MIN(to_read - done, uint64_t(c.location.size) - in_chunk);

		if (ci == current_chunk) {
			memcpy(p_dst + done, current_data.ptr() + in_chunk, n);
		} else if (n == c.location.size) {
			// Whole chunk, decompress straight into the destination (e.g. a full mip of a streamed texture).
			Vector<uint8_t> cached;
			GCASChunkCache *cache = GCASChunkCache::get_singleton();
			if (c.location.codec != GCAS_CHUNK_CODEC_RAW && cache && cache->get(c.location.hash, cached)) {
				memcpy(p_dst + done, cached.ptr(), n);
			} else if (!_read_chunk_data(c, p_dst + done)) {
				eof = true;
				break;
			}
		} else if (c.location.codec == GCAS_CHUNK_CODEC_RAW && n >= RAW_DIRECT_READ_MIN) {
			Ref<FileAccess> f = _get_pack_handle(c.location.pack_file);
			if (f.is_null()) {
				eof = true;
				break;
			}
			f->seek(c.location.offset + in_chunk);
			if (f->get_buffer(p_dst + done, n) != n) {
				eof = true;
				break;
			}
		} else {
			if (!_get_chunk(ci, current_data, true)) {
				current_chunk = -1;
				eof = true;
				break;
			}
			current_chunk = ci;
			memcpy(p_dst + done, current_data.ptr() + in_chunk, n);
		}

		done += n;
		pos += n;
	}

	return done;
}

void FileAccessGCAS::prefetch(uint64_t p_offset, uint64_t p_length) const {
	if (!open || p_offset >= size || !GCASChunkCache::get_singleton()) {
		return;
	}
	uint64_t end = MIN(size, p_offset + p_length);
	for (int64_t ci = _find_chunk(p_offset); ci >= 0 && uint32_t(ci) < chunks.size() && chunks[ci].start < end; ci++) {
		if (chunks[ci].location.codec == GCAS_CHUNK_CODEC_RAW) {
			continue;
		}
		Vector<uint8_t> data;
		_get_chunk(ci, data, true);
	}
}

void FileAccessGCAS::close() {
	open = false;
	chunks.clear();
	pack_handles.clear();
	current_data.clear();
	compressed_buffer.clear();
	current_chunk = -1;
}

FileAccessGCAS::FileAccessGCAS(const String &p_path, uint64_t p_size, const LocalVector<Chunk> &p_chunks, const LocalVector<String> &p_pack_paths) {
	path = p_path;
	size = p_size;
	chunks = p_chunks;
	pack_paths = p_pack_paths;
	pack_handles.resize(pack_paths.size());
	open = true;
}
