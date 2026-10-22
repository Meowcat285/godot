/**************************************************************************/
/*  cas_layout.h                                                          */
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

#include "core/object/gdvirtual.gen.h"
#include "core/object/ref_counted.h"
#include "core/variant/typed_array.h"

// Describes how a file should be split into chunks when written to a GCAS pack, so that data read together at
// runtime (a texture mip, a mesh LOD, a virtual texture page...) maps to whole, independently compressed chunks.
class CASLayout : public RefCounted {
	GDCLASS(CASLayout, RefCounted);

public:
	enum Codec {
		CODEC_DEFAULT,
		CODEC_RAW,
		CODEC_ZSTD,
	};

	struct Range {
		uint64_t offset = 0;
		uint64_t length = 0;
	};

	struct PagedRange {
		uint64_t offset = 0;
		uint64_t length = 0;
		uint64_t page_size = 0;
	};

	struct CodecRange {
		uint64_t offset = 0;
		uint64_t length = 0;
		Codec codec = CODEC_DEFAULT;
		int level = 0;
	};

	struct PriorityRange {
		uint64_t offset = 0;
		uint64_t length = 0;
		int priority = 0;
	};

private:
	LocalVector<uint64_t> cuts;
	LocalVector<PagedRange> paged_ranges;
	LocalVector<Range> groups;
	LocalVector<CodecRange> codec_ranges;
	LocalVector<PriorityRange> priority_ranges;
	int priority = 0;
	PackedByteArray hint_data;

protected:
	static void _bind_methods();

public:
	void add_cut(uint64_t p_offset);
	void add_paged_range(uint64_t p_offset, uint64_t p_length, uint64_t p_page_size);
	void add_group(uint64_t p_offset, uint64_t p_length);
	void set_range_codec(uint64_t p_offset, uint64_t p_length, Codec p_codec, int p_level = 0);
	void set_range_priority(uint64_t p_offset, uint64_t p_length, int p_priority);

	void set_priority(int p_priority) { priority = p_priority; }
	int get_priority() const { return priority; }

	void set_hint_data(const PackedByteArray &p_data) { hint_data = p_data; }
	PackedByteArray get_hint_data() const { return hint_data; }

	const LocalVector<uint64_t> &get_cuts() const { return cuts; }
	const LocalVector<PagedRange> &get_paged_ranges() const { return paged_ranges; }
	const LocalVector<Range> &get_groups() const { return groups; }
	const LocalVector<CodecRange> &get_codec_ranges() const { return codec_ranges; }
	const LocalVector<PriorityRange> &get_priority_ranges() const { return priority_ranges; }

	void clear();
};

VARIANT_ENUM_CAST(CASLayout::Codec);

// Computes the layout of files when they are written to a GCAS pack.
// Implement it in an engine module, a GDExtension or a script; register it with GCASPacker or an export plugin.
class CASLayoutProvider : public RefCounted {
	GDCLASS(CASLayoutProvider, RefCounted);

protected:
	static void _bind_methods();

	GDVIRTUAL0RC_REQUIRED(String, _get_provider_name)
	GDVIRTUAL1RC(bool, _handles_file, String)
	GDVIRTUAL2RC(Ref<CASLayout>, _get_layout, String, PackedByteArray)

public:
	// Identifies the provider's layout hints in packs, so it must be stable.
	virtual String get_provider_name() const;
	virtual bool handles_file(const String &p_path) const;
	virtual Ref<CASLayout> get_layout(const String &p_path, const Vector<uint8_t> &p_data) const;
};
