/**************************************************************************/
/*  cas_layout.cpp                                                        */
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

#include "cas_layout.h"

#include "core/object/class_db.h"

void CASLayout::add_cut(uint64_t p_offset) {
	cuts.push_back(p_offset);
}

void CASLayout::add_paged_range(uint64_t p_offset, uint64_t p_length, uint64_t p_page_size) {
	ERR_FAIL_COND_MSG(p_page_size == 0, "Page size must be greater than zero.");
	PagedRange r;
	r.offset = p_offset;
	r.length = p_length;
	r.page_size = p_page_size;
	paged_ranges.push_back(r);
}

void CASLayout::add_group(uint64_t p_offset, uint64_t p_length) {
	Range r;
	r.offset = p_offset;
	r.length = p_length;
	groups.push_back(r);
}

void CASLayout::set_range_codec(uint64_t p_offset, uint64_t p_length, Codec p_codec, int p_level) {
	CodecRange r;
	r.offset = p_offset;
	r.length = p_length;
	r.codec = p_codec;
	r.level = p_level;
	codec_ranges.push_back(r);
}

void CASLayout::set_range_priority(uint64_t p_offset, uint64_t p_length, int p_priority) {
	PriorityRange r;
	r.offset = p_offset;
	r.length = p_length;
	r.priority = p_priority;
	priority_ranges.push_back(r);
}

void CASLayout::clear() {
	cuts.clear();
	paged_ranges.clear();
	groups.clear();
	codec_ranges.clear();
	priority_ranges.clear();
	priority = 0;
	hint_data.clear();
}

void CASLayout::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_cut", "offset"), &CASLayout::add_cut);
	ClassDB::bind_method(D_METHOD("add_paged_range", "offset", "length", "page_size"), &CASLayout::add_paged_range);
	ClassDB::bind_method(D_METHOD("add_group", "offset", "length"), &CASLayout::add_group);
	ClassDB::bind_method(D_METHOD("set_range_codec", "offset", "length", "codec", "level"), &CASLayout::set_range_codec, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_range_priority", "offset", "length", "priority"), &CASLayout::set_range_priority);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &CASLayout::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &CASLayout::get_priority);
	ClassDB::bind_method(D_METHOD("set_hint_data", "data"), &CASLayout::set_hint_data);
	ClassDB::bind_method(D_METHOD("get_hint_data"), &CASLayout::get_hint_data);
	ClassDB::bind_method(D_METHOD("clear"), &CASLayout::clear);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "priority"), "set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "hint_data"), "set_hint_data", "get_hint_data");

	BIND_ENUM_CONSTANT(CODEC_DEFAULT);
	BIND_ENUM_CONSTANT(CODEC_RAW);
	BIND_ENUM_CONSTANT(CODEC_ZSTD);
}

//////////////////////////////////////////////////////////////////

String CASLayoutProvider::get_provider_name() const {
	String name;
	GDVIRTUAL_CALL(_get_provider_name, name);
	return name;
}

bool CASLayoutProvider::handles_file(const String &p_path) const {
	bool handles = false;
	GDVIRTUAL_CALL(_handles_file, p_path, handles);
	return handles;
}

Ref<CASLayout> CASLayoutProvider::get_layout(const String &p_path, const Vector<uint8_t> &p_data) const {
	Ref<CASLayout> layout;
	GDVIRTUAL_CALL(_get_layout, p_path, p_data, layout);
	return layout;
}

void CASLayoutProvider::_bind_methods() {
	GDVIRTUAL_BIND(_get_provider_name);
	GDVIRTUAL_BIND(_handles_file, "path");
	GDVIRTUAL_BIND(_get_layout, "path", "data");
}
