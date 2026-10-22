/**************************************************************************/
/*  gcas_writer.h                                                         */
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

#include "cas_layout.h"
#include "fastcdc.h"

#include "core/crypto/cas_hash.h"
#include "core/io/file_access.h"
#include "core/io/resource_uid.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

// Writes GCAS packs (see core/io/file_access_gcas.h for the format).
//
// Files are split into chunks according to their layout (from layout providers) and content-defined chunking,
// deduplicated by hash (within the pack and against base packs when writing a patch), compressed in parallel and
// staged in a temporary file, then written in placement order when the pack is finished.
class GCASWriter {
public:
	struct Settings {
		int zstd_level = 19;
		FastCDC::Params cdc;
		// Chunks are stored uncompressed when compression saves less than this ratio.
		float min_compression_saving = 0.03;
		// Amount of uncompressed data queued before compressing it in parallel.
		uint64_t compression_batch_size = 64 * 1024 * 1024;
		// Files with these extensions are stored uncompressed (already compressed formats).
		HashSet<String> uncompressed_extensions;

		Settings();
	};

	// Largest chunk the writer creates for groups and pages.
	static constexpr uint64_t MAX_ATOMIC_CHUNK_SIZE = 64 * 1024 * 1024;

private:
	struct Chunk {
		CASHash hash;
		uint64_t staged_offset = 0;
		uint64_t final_offset = 0;
		uint32_t compressed_size = 0;
		uint32_t size = 0;
		uint8_t codec = 0;
		int priority = 0;
		bool written = false;
	};

	struct ChunkRef {
		uint32_t index = 0;
		bool external = false;
	};

	struct Blob {
		CASHash hash;
		uint64_t size = 0;
		LocalVector<ChunkRef> refs;
	};

	struct FileEntry {
		uint32_t blob = UINT32_MAX; // UINT32_MAX for removals.
		LocalVector<Pair<String, Vector<uint8_t>>> hints;
	};

	struct PendingChunk {
		uint32_t chunk = 0;
		Vector<uint8_t> data;
		int level = 0;
		bool compress = true;
		Vector<uint8_t> compressed;
	};

	struct ChunkSpec {
		uint64_t start = 0;
		uint64_t end = 0;
		CASLayout::Codec codec = CASLayout::CODEC_DEFAULT;
		int level = 0;
		int priority = 0;
	};

	Settings settings;
	String path;
	bool embed = false;
	bool started = false;

	String staging_path;
	Ref<FileAccess> staging;

	LocalVector<Chunk> chunks;
	HashMap<CASHash, uint32_t> chunk_map;
	LocalVector<CASHash> external_chunks;
	HashMap<CASHash, uint32_t> external_map;
	HashSet<CASHash> base_chunks;
	LocalVector<Vector<uint8_t>> base_pack_ids;

	LocalVector<Blob> blobs;
	HashMap<CASHash, uint32_t> blob_map;
	HashMap<String, FileEntry> files;
	HashMap<String, ResourceUID::ID> uids;

	LocalVector<Ref<CASLayoutProvider>> layout_providers;

	LocalVector<PendingChunk> pending;
	uint64_t pending_bytes = 0;

	uint64_t stats_input_bytes = 0;
	uint64_t stats_dedup_bytes = 0;

	static String _normalize_path(const String &p_path);
	void _compute_chunk_specs(const String &p_path, const Vector<uint8_t> &p_data, const Ref<CASLayout> &p_layout, LocalVector<ChunkSpec> &r_specs) const;
	void _compress_pending(uint32_t p_index, void *p_userdata);
	Error _flush_pending();

public:
	Settings &get_settings() { return settings; }

	Error start(const String &p_path, bool p_embed = false);
	// Makes this pack a patch for the given base pack: its chunks aren't stored again.
	Error add_base_pack(const String &p_path);
	void add_layout_provider(const Ref<CASLayoutProvider> &p_provider);

	Error add_file(const String &p_path, const Vector<uint8_t> &p_data, ResourceUID::ID p_uid = ResourceUID::INVALID_ID);
	void add_removal(const String &p_path);
	// UIDs can be given to any path, including ones that aren't files of the pack (e.g. imported resources).
	void add_uid(const String &p_path, ResourceUID::ID p_uid);
	bool has_file(const String &p_path) const;

	// Writes the pack. r_start/r_size are the location of the pack in the output file (for embedding).
	Error finish(uint64_t *r_start = nullptr, uint64_t *r_size = nullptr);
	void abort();

	uint64_t get_input_size() const { return stats_input_bytes; }
	uint64_t get_deduplicated_size() const { return stats_dedup_bytes; }
	uint32_t get_chunk_count() const { return chunks.size(); }

	~GCASWriter();
};
