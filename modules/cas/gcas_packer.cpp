/**************************************************************************/
/*  gcas_packer.cpp                                                       */
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

#include "gcas_packer.h"

#include "core/io/file_access_gcas.h"
#include "core/object/class_db.h"

Error GCASPacker::start(const String &p_path, int p_compression_level) {
	if (writer) {
		writer->abort();
		memdelete(writer);
	}
	writer = memnew(GCASWriter);
	writer->get_settings().zstd_level = p_compression_level;
	Error err = writer->start(p_path);
	if (err != OK) {
		memdelete(writer);
		writer = nullptr;
	}
	return err;
}

Error GCASPacker::add_base_pack(const String &p_base_path) {
	ERR_FAIL_NULL_V_MSG(writer, ERR_UNCONFIGURED, "Call start() first.");
	return writer->add_base_pack(p_base_path);
}

void GCASPacker::add_layout_provider(const Ref<CASLayoutProvider> &p_provider) {
	ERR_FAIL_NULL_MSG(writer, "Call start() first.");
	writer->add_layout_provider(p_provider);
}

Error GCASPacker::add_file(const String &p_target_path, const String &p_source_path, int64_t p_uid) {
	ERR_FAIL_NULL_V_MSG(writer, ERR_UNCONFIGURED, "Call start() first.");
	Error err;
	Vector<uint8_t> data = FileAccess::get_file_as_bytes(p_source_path, &err);
	ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Can't read \"%s\".", p_source_path));
	return writer->add_file(p_target_path, data, p_uid);
}

Error GCASPacker::add_file_from_buffer(const String &p_target_path, const PackedByteArray &p_data, int64_t p_uid) {
	ERR_FAIL_NULL_V_MSG(writer, ERR_UNCONFIGURED, "Call start() first.");
	return writer->add_file(p_target_path, p_data, p_uid);
}

Error GCASPacker::add_file_removal(const String &p_target_path) {
	ERR_FAIL_NULL_V_MSG(writer, ERR_UNCONFIGURED, "Call start() first.");
	writer->add_removal(p_target_path);
	return OK;
}

Error GCASPacker::flush() {
	ERR_FAIL_NULL_V_MSG(writer, ERR_UNCONFIGURED, "Call start() first.");
	Error err = writer->finish();
	memdelete(writer);
	writer = nullptr;
	return err;
}

void GCASPacker::add_export_layout_provider(const Ref<CASLayoutProvider> &p_provider) {
	ERR_FAIL_COND(p_provider.is_null());
	if (!export_layout_providers.has(p_provider)) {
		export_layout_providers.push_back(p_provider);
	}
}

void GCASPacker::remove_export_layout_provider(const Ref<CASLayoutProvider> &p_provider) {
	export_layout_providers.erase(p_provider);
}

void GCASPacker::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "path", "compression_level"), &GCASPacker::start, DEFVAL(19));
	ClassDB::bind_method(D_METHOD("add_base_pack", "base_path"), &GCASPacker::add_base_pack);
	ClassDB::bind_method(D_METHOD("add_layout_provider", "provider"), &GCASPacker::add_layout_provider);
	ClassDB::bind_method(D_METHOD("add_file", "target_path", "source_path", "uid"), &GCASPacker::add_file, DEFVAL(ResourceUID::INVALID_ID));
	ClassDB::bind_method(D_METHOD("add_file_from_buffer", "target_path", "data", "uid"), &GCASPacker::add_file_from_buffer, DEFVAL(ResourceUID::INVALID_ID));
	ClassDB::bind_method(D_METHOD("add_file_removal", "target_path"), &GCASPacker::add_file_removal);
	ClassDB::bind_method(D_METHOD("flush"), &GCASPacker::flush);

	ClassDB::bind_static_method("GCASPacker", D_METHOD("add_export_layout_provider", "provider"), &GCASPacker::add_export_layout_provider);
	ClassDB::bind_static_method("GCASPacker", D_METHOD("remove_export_layout_provider", "provider"), &GCASPacker::remove_export_layout_provider);
}

GCASPacker::~GCASPacker() {
	if (writer) {
		writer->abort();
		memdelete(writer);
	}
}
