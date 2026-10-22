/**************************************************************************/
/*  gcas_packer.h                                                         */
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

#include "gcas_writer.h"

#include "core/object/ref_counted.h"

// Script API to create GCAS packs (e.g. for DLC or mods).
class GCASPacker : public RefCounted {
	GDCLASS(GCASPacker, RefCounted);

	GCASWriter *writer = nullptr;

	static inline LocalVector<Ref<CASLayoutProvider>> export_layout_providers;

protected:
	static void _bind_methods();

public:
	Error start(const String &p_path, int p_compression_level = 19);
	Error add_base_pack(const String &p_base_path);
	void add_layout_provider(const Ref<CASLayoutProvider> &p_provider);

	Error add_file(const String &p_target_path, const String &p_source_path, int64_t p_uid = ResourceUID::INVALID_ID);
	Error add_file_from_buffer(const String &p_target_path, const PackedByteArray &p_data, int64_t p_uid = ResourceUID::INVALID_ID);
	Error add_file_removal(const String &p_target_path);

	Error flush();

	// Layout providers used when exporting projects to GCAS packs.
	static void add_export_layout_provider(const Ref<CASLayoutProvider> &p_provider);
	static void remove_export_layout_provider(const Ref<CASLayoutProvider> &p_provider);
	static const LocalVector<Ref<CASLayoutProvider>> &get_export_layout_providers() { return export_layout_providers; }
	static void clear_export_layout_providers() { export_layout_providers.clear(); }

	~GCASPacker();
};
