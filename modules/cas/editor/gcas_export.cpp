/**************************************************************************/
/*  gcas_export.cpp                                                       */
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

#include "gcas_export.h"

#include "../gcas_packer.h"
#include "../gcas_writer.h"

#include "core/config/project_settings.h"
#include "core/io/file_access_gcas.h"
#include "core/io/file_access_pack.h"
#include "core/io/marshalls.h"
#include "editor/editor_node.h"
#include "editor/export/editor_export.h"

namespace {

struct GCASExportData {
	GCASWriter *writer = nullptr;
	EditorProgress *ep = nullptr;
	Vector<SharedObject> *so_files = nullptr;
	HashMap<String, ResourceUID::ID> uids;
	bool patch = false;
	int saved_files = 0;
};

// Exported UIDs are stored in the pack's UID table instead of a cache file.
void parse_uid_cache(const Vector<uint8_t> &p_data, HashMap<String, ResourceUID::ID> &r_uids) {
	const uint8_t *ptr = p_data.ptr();
	uint64_t size = p_data.size();
	uint64_t pos = 0;
	auto read = [&](uint64_t p_bytes) -> const uint8_t * {
		if (p_bytes > size - pos) {
			return nullptr;
		}
		const uint8_t *r = ptr + pos;
		pos += p_bytes;
		return r;
	};
	const uint8_t *count_ptr = read(4);
	ERR_FAIL_NULL(count_ptr);
	uint32_t count = decode_uint32(count_ptr);
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *id_ptr = read(8);
		const uint8_t *len_ptr = read(4);
		ERR_FAIL_COND(!id_ptr || !len_ptr);
		uint32_t len = decode_uint32(len_ptr);
		const uint8_t *str = read(len);
		ERR_FAIL_NULL(str);
		r_uids[String::utf8((const char *)str, len)] = ResourceUID::ID(decode_uint64(id_ptr));
	}
}

Error save_file(const Ref<EditorExportPreset> &p_preset, void *p_userdata, const EditorExportPlatform::SaveFileInfo &p_info, const Vector<uint8_t> &p_data) {
	GCASExportData *data = (GCASExportData *)p_userdata;
	const String path = EditorExportPlatform::simplify_path(p_info.path);

	if (data->ep->step(vformat(TTR("Storing File: %s"), p_info.path), 2 + p_info.file_index * 100 / MAX(1, p_info.file_count), false)) {
		return ERR_SKIP;
	}

	if (path == ResourceUID::get_cache_file()) {
		parse_uid_cache(p_data, data->uids);
		return OK;
	}

	if (data->patch) {
		Ref<FileAccess> old_file = PackedData::get_singleton()->try_open_path(path, p_preset->resolve_script_encryption_key());
		if (old_file.is_valid() && old_file->get_length() == uint64_t(p_data.size())) {
			if (old_file->get_buffer(old_file->get_length()) == p_data) {
				return OK; // Unchanged.
			}
		}
	}

	data->saved_files++;
	return data->writer->add_file(path, p_data);
}

Error remove_file(const Ref<EditorExportPreset> &p_preset, void *p_userdata, const String &p_path) {
	GCASExportData *data = (GCASExportData *)p_userdata;
	if (EditorExportPlatform::simplify_path(p_path) == ResourceUID::get_cache_file()) {
		return OK;
	}
	data->writer->add_removal(p_path);
	data->saved_files++;
	return OK;
}

Error add_shared_object(const Ref<EditorExportPreset> &p_preset, void *p_userdata, const SharedObject &p_so) {
	GCASExportData *data = (GCASExportData *)p_userdata;
	if (data->so_files) {
		data->so_files->push_back(p_so);
	}
	return OK;
}

} // namespace

Error GCASExport::save_gcas(EditorExportPlatform *p_platform, const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, Vector<SharedObject> *p_so_files, bool p_patch, bool p_embed, int64_t *r_embedded_start, int64_t *r_embedded_size) {
	EditorProgress ep("savegcas", TTR("Packing"), 102, true);

	GCASWriter writer;
	writer.get_settings().zstd_level = GLOBAL_GET("editor/export/gcas/compression_level");
	for (const Ref<CASLayoutProvider> &provider : GCASPacker::get_export_layout_providers()) {
		writer.add_layout_provider(provider);
	}

	if (p_patch) {
		// Chunks of the base packs (mounted by EditorExportPlatform::_load_patches()) aren't stored again.
		if (PackedSourceGCAS::get_singleton()) {
			for (const String &base : PackedSourceGCAS::get_singleton()->get_mounted_pack_paths()) {
				Error err = writer.add_base_pack(base);
				if (err != OK) {
					p_platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Save GCAS"), vformat(TTR("Can't read base pack \"%s\"."), base));
					return err;
				}
			}
		}
	}

	Error err = writer.start(p_path, p_embed);
	if (err != OK) {
		p_platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Save GCAS"), vformat(TTR("Can't open file for writing at path \"%s\"."), p_path));
		return ERR_CANT_CREATE;
	}

	GCASExportData data;
	data.writer = &writer;
	data.ep = &ep;
	data.so_files = p_so_files;
	data.patch = p_patch;

	err = p_platform->export_project_files(p_preset, p_debug, save_file, p_patch ? remove_file : nullptr, &data, add_shared_object);
	if (err != OK) {
		writer.abort();
		if (err != ERR_SKIP) {
			p_platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Save GCAS"), TTR("Failed to export project files."));
		}
		return err;
	}

	if (data.saved_files == 0) {
		writer.abort();
		p_platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Save GCAS"), TTR("No files or changes to export."));
		return FAILED;
	}

	for (const KeyValue<String, ResourceUID::ID> &E : data.uids) {
		writer.add_uid(E.key, E.value);
	}

	ep.step(TTR("Writing pack..."), 101, true);
	uint64_t start = 0;
	uint64_t size = 0;
	err = writer.finish(&start, &size);
	if (err != OK) {
		p_platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Save GCAS"), vformat(TTR("Can't write GCAS pack \"%s\"."), p_path));
		return err;
	}

	print_verbose(vformat("GCAS: exported %s of data in %d chunks to \"%s\" (%s, %s deduplicated).", String::humanize_size(writer.get_input_size()), writer.get_chunk_count(), p_path, String::humanize_size(size), String::humanize_size(writer.get_deduplicated_size())));

	if (r_embedded_start) {
		*r_embedded_start = start;
	}
	if (r_embedded_size) {
		*r_embedded_size = size;
	}
	return OK;
}
