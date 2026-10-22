/**************************************************************************/
/*  gcas_writer.cpp                                                       */
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

#include "gcas_writer.h"

#include "core/io/dir_access.h"
#include "core/io/file_access_gcas.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/sort_array.h"
#include "core/version.h"

#include <zstd.h>

namespace {

struct IndexBuilder {
	Vector<uint8_t> data;

	void u8(uint8_t p_value) { data.push_back(p_value); }

	void u32(uint32_t p_value) {
		for (int i = 0; i < 4; i++) {
			data.push_back((p_value >> (i * 8)) & 0xFF);
		}
	}

	void u64(uint64_t p_value) {
		for (int i = 0; i < 8; i++) {
			data.push_back((p_value >> (i * 8)) & 0xFF);
		}
	}

	void bytes(const uint8_t *p_data, uint64_t p_size) {
		int64_t ofs = data.size();
		data.resize(ofs + p_size);
		memcpy(data.ptrw() + ofs, p_data, p_size);
	}

	void section(uint32_t p_tag, const IndexBuilder &p_payload, uint32_t p_flags = GCAS_SECTION_FLAG_REQUIRED) {
		u32(p_tag);
		u32(p_flags);
		u64(p_payload.data.size());
		bytes(p_payload.data.ptr(), p_payload.data.size());
	}
};

struct StringPool {
	IndexBuilder builder;
	HashMap<String, Pair<uint32_t, uint32_t>> strings;

	Pair<uint32_t, uint32_t> add(const String &p_string) {
		if (const Pair<uint32_t, uint32_t> *existing = strings.getptr(p_string)) {
			return *existing;
		}
		CharString cs = p_string.utf8();
		Pair<uint32_t, uint32_t> location(builder.data.size(), cs.length());
		builder.bytes((const uint8_t *)cs.get_data(), cs.length());
		strings[p_string] = location;
		return location;
	}
};

} // namespace

GCASWriter::Settings::Settings() {
	const char *extensions[] = { "png", "jpg", "jpeg", "webp", "ogg", "ogv", "mp3", "oggvorbisstr", "mp3str", "zip", "gz", "zst", "br", "ktx2", "basis" };
	for (const char *ext : extensions) {
		uncompressed_extensions.insert(ext);
	}
}

String GCASWriter::_normalize_path(const String &p_path) {
	return p_path.simplify_path().trim_prefix("res://").trim_prefix("/");
}

Error GCASWriter::start(const String &p_path, bool p_embed) {
	ERR_FAIL_COND_V_MSG(started, ERR_ALREADY_IN_USE, "GCAS writer already started.");
	path = p_path;
	embed = p_embed;
	staging_path = p_path + ".chunks.tmp";

	Error err;
	staging = FileAccess::open(staging_path, FileAccess::WRITE_READ, &err);
	ERR_FAIL_COND_V_MSG(staging.is_null(), err, vformat("Can't create temporary file \"%s\".", staging_path));
	started = true;
	return OK;
}

Error GCASWriter::add_base_pack(const String &p_path) {
	PackedSourceGCAS::PackInfo info;
	Error err = PackedSourceGCAS::read_pack_info(p_path, info);
	ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Can't read GCAS base pack \"%s\".", p_path));
	for (const CASHash &h : info.chunk_hashes) {
		base_chunks.insert(h);
	}
	for (const CASHash &h : info.external_chunk_hashes) {
		base_chunks.insert(h);
	}
	Vector<uint8_t> id;
	id.resize(16);
	memcpy(id.ptrw(), info.header.pack_id, 16);
	base_pack_ids.push_back(id);
	return OK;
}

void GCASWriter::add_layout_provider(const Ref<CASLayoutProvider> &p_provider) {
	ERR_FAIL_COND(p_provider.is_null());
	layout_providers.push_back(p_provider);
}

void GCASWriter::_compute_chunk_specs(const String &p_path, const Vector<uint8_t> &p_data, const Ref<CASLayout> &p_layout, LocalVector<ChunkSpec> &r_specs) const {
	uint64_t size = p_data.size();
	if (size == 0) {
		return;
	}

	LocalVector<uint64_t> bounds;
	bounds.push_back(0);
	bounds.push_back(size);
	auto add_bound = [&](uint64_t p_bound) {
		if (p_bound > 0 && p_bound < size) {
			bounds.push_back(p_bound);
		}
	};

	if (p_layout.is_valid()) {
		for (uint64_t cut : p_layout->get_cuts()) {
			add_bound(cut);
		}
		for (const CASLayout::PagedRange &r : p_layout->get_paged_ranges()) {
			uint64_t end = MIN(size, r.offset + r.length);
			for (uint64_t o = r.offset; o < end; o += r.page_size) {
				add_bound(o);
			}
			add_bound(end);
		}
		for (const CASLayout::Range &r : p_layout->get_groups()) {
			add_bound(r.offset);
			add_bound(r.offset + r.length);
		}
		for (const CASLayout::CodecRange &r : p_layout->get_codec_ranges()) {
			add_bound(r.offset);
			add_bound(r.offset + r.length);
		}
		for (const CASLayout::PriorityRange &r : p_layout->get_priority_ranges()) {
			add_bound(r.offset);
			add_bound(r.offset + r.length);
		}
	}

	bounds.sort();

	CASLayout::Codec default_codec = settings.uncompressed_extensions.has(p_path.get_extension().to_lower()) ? CASLayout::CODEC_RAW : CASLayout::CODEC_ZSTD;
	LocalVector<uint64_t> ends;

	for (uint32_t i = 0; i + 1 < bounds.size(); i++) {
		uint64_t a = bounds[i];
		uint64_t b = bounds[i + 1];
		if (a == b) {
			continue;
		}

		ChunkSpec spec;
		spec.codec = default_codec;
		spec.level = settings.zstd_level;
		bool atomic = false;
		if (p_layout.is_valid()) {
			spec.priority = p_layout->get_priority();
			for (const CASLayout::Range &r : p_layout->get_groups()) {
				atomic = atomic || (r.offset <= a && b <= r.offset + r.length);
			}
			for (const CASLayout::PagedRange &r : p_layout->get_paged_ranges()) {
				atomic = atomic || (r.offset <= a && b <= r.offset + r.length);
			}
			for (const CASLayout::CodecRange &r : p_layout->get_codec_ranges()) {
				if (r.offset <= a && b <= r.offset + r.length && r.codec != CASLayout::CODEC_DEFAULT) {
					spec.codec = r.codec;
					if (r.level != 0) {
						spec.level = r.level;
					}
				}
			}
			for (const CASLayout::PriorityRange &r : p_layout->get_priority_ranges()) {
				if (r.offset <= a && b <= r.offset + r.length) {
					spec.priority = r.priority;
				}
			}
		}

		ends.clear();
		if (atomic) {
			for (uint64_t o = a + MAX_ATOMIC_CHUNK_SIZE; o < b; o += MAX_ATOMIC_CHUNK_SIZE) {
				ends.push_back(o - a);
			}
			ends.push_back(b - a);
		} else {
			FastCDC::split(p_data.ptr() + a, b - a, settings.cdc, ends);
		}

		uint64_t start = a;
		for (uint64_t end : ends) {
			spec.start = start;
			spec.end = a + end;
			r_specs.push_back(spec);
			start = spec.end;
		}
	}
}

Error GCASWriter::add_file(const String &p_path, const Vector<uint8_t> &p_data, ResourceUID::ID p_uid) {
	ERR_FAIL_COND_V_MSG(!started, ERR_UNCONFIGURED, "GCAS writer not started.");
	String file_path = _normalize_path(p_path);
	ERR_FAIL_COND_V(file_path.is_empty(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(uint64_t(p_data.size()) > UINT32_MAX * MAX_ATOMIC_CHUNK_SIZE, ERR_OUT_OF_MEMORY, "File too large.");

	FileEntry entry;
	if (p_uid != ResourceUID::INVALID_ID) {
		uids[file_path] = p_uid;
	}

	String res_path = "res://" + file_path;
	Ref<CASLayout> layout;
	for (const Ref<CASLayoutProvider> &provider : layout_providers) {
		if (provider->handles_file(res_path)) {
			layout = provider->get_layout(res_path, p_data);
			if (layout.is_valid() && !layout->get_hint_data().is_empty()) {
				entry.hints.push_back(Pair<String, Vector<uint8_t>>(provider->get_provider_name(), layout->get_hint_data()));
			}
			break;
		}
	}

	stats_input_bytes += p_data.size();
	CASHash blob_hash = CASHash::compute(p_data);
	if (const uint32_t *existing = blob_map.getptr(blob_hash)) {
		entry.blob = *existing;
		stats_dedup_bytes += p_data.size();
	} else {
		Blob blob;
		blob.hash = blob_hash;
		blob.size = p_data.size();

		LocalVector<ChunkSpec> specs;
		_compute_chunk_specs(file_path, p_data, layout, specs);
		for (const ChunkSpec &spec : specs) {
			const uint8_t *chunk_data = p_data.ptr() + spec.start;
			uint32_t chunk_size = spec.end - spec.start;
			CASHash h = CASHash::compute(chunk_data, chunk_size);

			ChunkRef ref;
			if (const uint32_t *local = chunk_map.getptr(h)) {
				ref.index = *local;
				chunks[*local].priority = MIN(chunks[*local].priority, spec.priority);
				stats_dedup_bytes += chunk_size;
			} else if (const uint32_t *ext = external_map.getptr(h)) {
				ref.index = *ext;
				ref.external = true;
				stats_dedup_bytes += chunk_size;
			} else if (base_chunks.has(h)) {
				ref.index = external_chunks.size();
				ref.external = true;
				external_map[h] = external_chunks.size();
				external_chunks.push_back(h);
				stats_dedup_bytes += chunk_size;
			} else {
				ref.index = chunks.size();
				Chunk c;
				c.hash = h;
				c.size = chunk_size;
				c.priority = spec.priority;
				chunk_map[h] = chunks.size();
				chunks.push_back(c);

				PendingChunk p;
				p.chunk = ref.index;
				p.data.resize_uninitialized(chunk_size);
				memcpy(p.data.ptrw(), chunk_data, chunk_size);
				p.level = spec.level;
				p.compress = spec.codec != CASLayout::CODEC_RAW;
				pending.push_back(p);
				pending_bytes += chunk_size;
			}
			blob.refs.push_back(ref);
		}

		entry.blob = blobs.size();
		blob_map[blob_hash] = blobs.size();
		blobs.push_back(blob);
	}

	files[file_path] = entry;

	if (pending_bytes >= settings.compression_batch_size) {
		return _flush_pending();
	}
	return OK;
}

void GCASWriter::add_removal(const String &p_path) {
	ERR_FAIL_COND_MSG(!started, "GCAS writer not started.");
	String file_path = _normalize_path(p_path);
	ERR_FAIL_COND(file_path.is_empty());
	files[file_path] = FileEntry();
}

void GCASWriter::add_uid(const String &p_path, ResourceUID::ID p_uid) {
	String uid_path = _normalize_path(p_path);
	ERR_FAIL_COND(uid_path.is_empty() || p_uid == ResourceUID::INVALID_ID);
	uids[uid_path] = p_uid;
}

bool GCASWriter::has_file(const String &p_path) const {
	return files.has(_normalize_path(p_path));
}

void GCASWriter::_compress_pending(uint32_t p_index, void *p_userdata) {
	PendingChunk &p = pending[p_index];
	if (!p.compress) {
		return;
	}
	size_t bound = ZSTD_compressBound(p.data.size());
	p.compressed.resize_uninitialized(bound);
	size_t ret = ZSTD_compress(p.compressed.ptrw(), bound, p.data.ptr(), p.data.size(), CLAMP(p.level, 1, ZSTD_maxCLevel()));
	if (ZSTD_isError(ret)) {
		p.compressed.clear();
		return;
	}
	p.compressed.resize(ret);
}

Error GCASWriter::_flush_pending() {
	if (pending.is_empty()) {
		return OK;
	}

	WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &GCASWriter::_compress_pending, (void *)nullptr, pending.size(), -1, false, "GCASWriter compression");
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);

	for (PendingChunk &p : pending) {
		Chunk &c = chunks[p.chunk];
		c.staged_offset = staging->get_position();
		bool use_compressed = !p.compressed.is_empty() && double(p.compressed.size()) <= double(p.data.size()) * (1.0 - settings.min_compression_saving);
		if (use_compressed) {
			c.codec = GCAS_CHUNK_CODEC_ZSTD;
			c.compressed_size = p.compressed.size();
			staging->store_buffer(p.compressed.ptr(), p.compressed.size());
		} else {
			c.codec = GCAS_CHUNK_CODEC_RAW;
			c.compressed_size = p.data.size();
			staging->store_buffer(p.data.ptr(), p.data.size());
		}
	}

	pending.clear();
	pending_bytes = 0;
	ERR_FAIL_COND_V_MSG(staging->get_error() != OK, ERR_FILE_CANT_WRITE, vformat("Can't write temporary file \"%s\".", staging_path));
	return OK;
}

Error GCASWriter::finish(uint64_t *r_start, uint64_t *r_size) {
	ERR_FAIL_COND_V_MSG(!started, ERR_UNCONFIGURED, "GCAS writer not started.");

	Error err = _flush_pending();
	if (err != OK) {
		abort();
		return err;
	}

	Ref<FileAccess> f;
	if (embed) {
		f = FileAccess::open(path, FileAccess::READ_WRITE, &err);
		if (f.is_valid()) {
			f->seek_end();
			while (f->get_position() % 8 != 0) {
				f->store_8(0);
			}
		}
	} else {
		f = FileAccess::open(path, FileAccess::WRITE, &err);
	}
	if (f.is_null()) {
		abort();
		ERR_FAIL_V_MSG(err, vformat("Can't open GCAS pack \"%s\" for writing.", path));
	}

	uint64_t pack_start = f->get_position();
	uint8_t header_data[GCAS_HEADER_SIZE] = {};
	f->store_buffer(header_data, GCAS_HEADER_SIZE);

	// Place chunks by priority, then in the order they were added.
	struct ChunkOrder {
		const LocalVector<Chunk> *chunks = nullptr;
		bool operator()(uint32_t p_a, uint32_t p_b) const {
			int pa = (*chunks)[p_a].priority;
			int pb = (*chunks)[p_b].priority;
			return pa != pb ? pa < pb : p_a < p_b;
		}
	};
	LocalVector<uint32_t> order;
	order.resize(chunks.size());
	for (uint32_t i = 0; i < chunks.size(); i++) {
		order[i] = i;
	}
	SortArray<uint32_t, ChunkOrder> sorter;
	sorter.compare.chunks = &chunks;
	sorter.sort(order.ptr(), order.size());

	Vector<uint8_t> buffer;
	for (uint32_t idx : order) {
		Chunk &c = chunks[idx];
		c.final_offset = f->get_position() - pack_start;
		buffer.resize_uninitialized(c.compressed_size);
		staging->seek(c.staged_offset);
		if (staging->get_buffer(buffer.ptrw(), c.compressed_size) != c.compressed_size) {
			abort();
			ERR_FAIL_V_MSG(ERR_FILE_CANT_READ, vformat("Can't read temporary file \"%s\".", staging_path));
		}
		f->store_buffer(buffer.ptr(), c.compressed_size);
	}

	// Build the index.
	IndexBuilder index;
	StringPool strings;

	if (!base_pack_ids.is_empty()) {
		IndexBuilder s;
		s.u32(base_pack_ids.size());
		for (const Vector<uint8_t> &id : base_pack_ids) {
			s.bytes(id.ptr(), 16);
		}
		index.section(GCAS_SECTION_BASES, s);
	}

	{
		IndexBuilder s;
		s.u32(chunks.size());
		for (const Chunk &c : chunks) {
			s.bytes(c.hash.data, CASHash::SIZE);
			s.u64(c.final_offset);
			s.u32(c.compressed_size);
			s.u32(c.size);
			s.u8(c.codec);
			s.u8(0);
			s.u8(0);
			s.u8(0);
		}
		index.section(GCAS_SECTION_CHUNKS, s);
	}

	if (!external_chunks.is_empty()) {
		IndexBuilder s;
		s.u32(external_chunks.size());
		for (const CASHash &h : external_chunks) {
			s.bytes(h.data, CASHash::SIZE);
		}
		index.section(GCAS_SECTION_EXTERNAL_CHUNKS, s);
	}

	{
		IndexBuilder refs;
		IndexBuilder blob_section;
		uint32_t ref_count = 0;
		for (const Blob &b : blobs) {
			ref_count += b.refs.size();
		}
		refs.u32(ref_count);
		blob_section.u32(blobs.size());
		uint32_t first_ref = 0;
		for (const Blob &b : blobs) {
			for (const ChunkRef &r : b.refs) {
				refs.u32(r.external ? (r.index | GCAS_CHUNK_REF_EXTERNAL) : r.index);
			}
			blob_section.bytes(b.hash.data, CASHash::SIZE);
			blob_section.u64(b.size);
			blob_section.u32(first_ref);
			blob_section.u32(b.refs.size());
			first_ref += b.refs.size();
		}
		index.section(GCAS_SECTION_CHUNK_REFS, refs);
		index.section(GCAS_SECTION_BLOBS, blob_section);
	}

	// Files, sorted for reproducible packs.
	LocalVector<String> paths;
	for (const KeyValue<String, FileEntry> &E : files) {
		paths.push_back(E.key);
	}
	paths.sort();

	IndexBuilder file_section;
	IndexBuilder hint_entries;
	IndexBuilder hint_data;
	uint32_t hint_count = 0;
	file_section.u32(paths.size());
	for (uint32_t i = 0; i < paths.size(); i++) {
		const FileEntry &entry = files[paths[i]];
		Pair<uint32_t, uint32_t> s = strings.add(paths[i]);
		file_section.u32(s.first);
		file_section.u32(s.second);
		file_section.u32(entry.blob == UINT32_MAX ? GCAS_FILE_BLOB_REMOVED : entry.blob);
		file_section.u32(0);

		for (const Pair<String, Vector<uint8_t>> &hint : entry.hints) {
			Pair<uint32_t, uint32_t> provider = strings.add(hint.first);
			hint_entries.u32(i);
			hint_entries.u32(provider.first);
			hint_entries.u32(provider.second);
			hint_entries.u32(hint_data.data.size());
			hint_entries.u32(hint.second.size());
			hint_data.bytes(hint.second.ptr(), hint.second.size());
			hint_count++;
		}
	}

	index.section(GCAS_SECTION_FILES, file_section);
	{
		LocalVector<String> uid_paths;
		for (const KeyValue<String, ResourceUID::ID> &E : uids) {
			uid_paths.push_back(E.key);
		}
		uid_paths.sort();
		IndexBuilder s;
		s.u32(uid_paths.size());
		for (const String &uid_path : uid_paths) {
			Pair<uint32_t, uint32_t> location = strings.add(uid_path);
			s.u64(uint64_t(uids[uid_path]));
			s.u32(location.first);
			s.u32(location.second);
		}
		index.section(GCAS_SECTION_UIDS, s);
	}
	if (hint_count > 0) {
		IndexBuilder s;
		s.u32(hint_count);
		s.bytes(hint_entries.data.ptr(), hint_entries.data.size());
		s.u32(hint_data.data.size());
		s.bytes(hint_data.data.ptr(), hint_data.data.size());
		// Layout hints are optional for readers.
		index.section(GCAS_SECTION_HINTS, s, 0);
	}
	// Last, as the other sections add strings.
	index.section(GCAS_SECTION_STRINGS, strings.builder);

	Vector<uint8_t> stored_index;
	size_t bound = ZSTD_compressBound(index.data.size());
	stored_index.resize_uninitialized(bound);
	size_t compressed_size = ZSTD_compress(stored_index.ptrw(), bound, index.data.ptr(), index.data.size(), 19);
	if (ZSTD_isError(compressed_size)) {
		abort();
		ERR_FAIL_V_MSG(ERR_BUG, "Can't compress GCAS index.");
	}
	stored_index.resize(compressed_size);

	GCASHeader header;
	header.ver_major = GODOT_VERSION_MAJOR;
	header.ver_minor = GODOT_VERSION_MINOR;
	header.ver_patch = GODOT_VERSION_PATCH;
	header.index_codec = GCAS_INDEX_CODEC_ZSTD;
	if (!base_pack_ids.is_empty() || !external_chunks.is_empty()) {
		header.incompat_flags |= GCAS_INCOMPAT_PATCH;
	}
	if (hint_count > 0) {
		header.compat_flags |= GCAS_COMPAT_LAYOUT_HINTS;
	}
	// Derived from the contents, so identical packs get identical IDs (reproducible builds).
	Blake3Context id_ctx;
	id_ctx.start();
	id_ctx.update(index.data);
	id_ctx.finish(header.pack_id, 16);
	header.index_offset = f->get_position() - pack_start;
	header.index_compressed_size = stored_index.size();
	header.index_size = index.data.size();
	Blake3Context checksum_ctx;
	checksum_ctx.start();
	checksum_ctx.update(stored_index);
	checksum_ctx.finish(header.index_checksum, 16);

	f->store_buffer(stored_index.ptr(), stored_index.size());

	uint64_t pack_size = f->get_position() - pack_start;
	f->store_64(pack_size);
	f->store_32(GCAS_HEADER_MAGIC);
	uint64_t end = f->get_position();

	header.encode(header_data);
	f->seek(pack_start);
	f->store_buffer(header_data, GCAS_HEADER_SIZE);

	bool write_error = f->get_error() != OK;
	f.unref();
	abort(); // Release the staging file.

	ERR_FAIL_COND_V_MSG(write_error, ERR_FILE_CANT_WRITE, vformat("Can't write GCAS pack \"%s\".", path));

	if (r_start) {
		*r_start = pack_start;
	}
	if (r_size) {
		*r_size = end - pack_start;
	}
	return OK;
}

void GCASWriter::abort() {
	if (staging.is_valid()) {
		staging.unref();
		DirAccess::remove_absolute(staging_path);
	}
	pending.clear();
	pending_bytes = 0;
	started = false;
}

GCASWriter::~GCASWriter() {
	if (started) {
		abort();
	}
}
