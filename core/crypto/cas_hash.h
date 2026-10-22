/**************************************************************************/
/*  cas_hash.h                                                            */
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

#include "core/string/ustring.h"
#include "core/templates/vector.h"

// 256-bit BLAKE3 digest, used as a content address (GCAS packs, editor import cache).
struct CASHash {
	static constexpr int SIZE = 32;

	uint8_t data[SIZE] = {};

	_FORCE_INLINE_ bool operator==(const CASHash &p_other) const { return memcmp(data, p_other.data, SIZE) == 0; }
	_FORCE_INLINE_ bool operator!=(const CASHash &p_other) const { return memcmp(data, p_other.data, SIZE) != 0; }
	_FORCE_INLINE_ bool operator<(const CASHash &p_other) const { return memcmp(data, p_other.data, SIZE) < 0; }

	// The digest is already uniformly distributed, so any 32 bits of it make a good hash table key.
	_FORCE_INLINE_ uint32_t hash() const {
		uint32_t h;
		memcpy(&h, data, sizeof(h));
		return h;
	}

	bool is_zero() const;

	String to_string() const;
	static CASHash from_string(const String &p_hex);

	static CASHash compute(const uint8_t *p_data, uint64_t p_size);
	static CASHash compute(const Vector<uint8_t> &p_data) { return compute(p_data.ptr(), p_data.size()); }
	static Error compute_file(const String &p_path, CASHash &r_hash);
};

class Blake3Context {
	void *ctx = nullptr;

public:
	void start();
	void start_keyed(const uint8_t p_key[32]);
	void update(const uint8_t *p_src, size_t p_len);
	void update(const Vector<uint8_t> &p_data) { update(p_data.ptr(), p_data.size()); }
	void update_u32(uint32_t p_value);
	void update_u64(uint64_t p_value);
	// Length-prefixed, so that consecutive strings can't produce the same input.
	void update_string(const String &p_string);
	CASHash finish();
	void finish(uint8_t *r_out, size_t p_out_len);

	Blake3Context();
	~Blake3Context();
};
