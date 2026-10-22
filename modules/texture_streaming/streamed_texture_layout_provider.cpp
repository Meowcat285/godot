/**************************************************************************/
/*  streamed_texture_layout_provider.cpp                                  */
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

#include "streamed_texture_layout_provider.h"

#ifdef MODULE_CAS_ENABLED

#include "core/io/marshalls.h"

bool StreamedTextureLayoutProvider::handles_file(const String &p_path) const {
	return p_path.get_extension().to_lower() == "stex";
}

Ref<CASLayout> StreamedTextureLayoutProvider::get_layout(const String &p_path, const Vector<uint8_t> &p_data) const {
	// See StreamedTexture2D::_parse_header().
	const uint8_t *data = p_data.ptr();
	const uint64_t size = p_data.size();
	constexpr uint64_t fixed_header_size = 28;
	if (size < fixed_header_size || data[0] != 'G' || data[1] != 'S' || data[2] != 'T' || data[3] != 'F') {
		return Ref<CASLayout>();
	}
	uint32_t num_mipmaps = decode_uint32(data + 24);
	if (num_mipmaps > 16) {
		return Ref<CASLayout>();
	}
	const uint64_t header_size = fixed_header_size + (num_mipmaps + 1) * 8 + 16;
	if (size < header_size) {
		return Ref<CASLayout>();
	}

	Ref<CASLayout> layout;
	layout.instantiate();

	// The header is read every time a mip is streamed, keep it small and first.
	layout->add_cut(header_size);
	layout->set_range_priority(0, header_size, -1);

	PackedByteArray hint;
	hint.resize(4 + (num_mipmaps + 1) * 16);
	encode_uint32(num_mipmaps + 1, hint.ptrw());

	uint64_t tail_start = 0;
	for (uint32_t i = 0; i <= num_mipmaps; i++) {
		const uint8_t *e = data + fixed_header_size + i * 8;
		uint64_t mip_offset = header_size + decode_uint32(e);
		uint64_t mip_size = decode_uint32(e + 4);
		if (mip_offset + mip_size > size) {
			return Ref<CASLayout>();
		}
		encode_uint64(mip_offset, hint.ptrw() + 4 + i * 16);
		encode_uint64(mip_size, hint.ptrw() + 4 + i * 16 + 8);

		if (tail_start == 0 && mip_size < MIP_TAIL_SIZE) {
			tail_start = mip_offset;
		}
		if (tail_start == 0) {
			// Large mips are streamed individually, and each one is a whole number of chunks.
			layout->add_cut(mip_offset);
			layout->add_cut(mip_offset + mip_size);
		}
	}

	if (tail_start != 0) {
		layout->add_group(tail_start, size - tail_start);
		layout->set_range_priority(tail_start, size - tail_start, -1);
	}

	// Mip ranges (u32 count, then u64 offset and u64 size per mip) for streaming code reading GCAS layout hints.
	layout->set_hint_data(hint);
	return layout;
}

#endif // MODULE_CAS_ENABLED
