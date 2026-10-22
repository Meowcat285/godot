/**************************************************************************/
/*  cas_hash.cpp                                                          */
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

#include "cas_hash.h"

#include "core/io/file_access.h"
#include "core/os/memory.h"

#include <blake3.h>

bool CASHash::is_zero() const {
	for (int i = 0; i < SIZE; i++) {
		if (data[i] != 0) {
			return false;
		}
	}
	return true;
}

String CASHash::to_string() const {
	return String::hex_encode_buffer(data, SIZE);
}

CASHash CASHash::from_string(const String &p_hex) {
	CASHash h;
	if (p_hex.length() != SIZE * 2) {
		return h;
	}
	for (int i = 0; i < SIZE; i++) {
		int v = 0;
		for (int j = 0; j < 2; j++) {
			char32_t c = p_hex[i * 2 + j];
			int n;
			if (c >= '0' && c <= '9') {
				n = c - '0';
			} else if (c >= 'a' && c <= 'f') {
				n = c - 'a' + 10;
			} else if (c >= 'A' && c <= 'F') {
				n = c - 'A' + 10;
			} else {
				return CASHash();
			}
			v = (v << 4) | n;
		}
		h.data[i] = v;
	}
	return h;
}

CASHash CASHash::compute(const uint8_t *p_data, uint64_t p_size) {
	Blake3Context ctx;
	ctx.start();
	ctx.update(p_data, p_size);
	return ctx.finish();
}

Error CASHash::compute_file(const String &p_path, CASHash &r_hash) {
	Error err;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ, &err);
	if (f.is_null()) {
		return err;
	}

	Blake3Context ctx;
	ctx.start();

	constexpr uint64_t step_size = 65536;
	uint8_t *step = (uint8_t *)memalloc(step_size);
	while (true) {
		uint64_t br = f->get_buffer(step, step_size);
		if (br > 0) {
			ctx.update(step, br);
		}
		if (br < step_size) {
			break;
		}
	}
	memfree(step);

	r_hash = ctx.finish();
	return OK;
}

void Blake3Context::start() {
	blake3_hasher_init((blake3_hasher *)ctx);
}

void Blake3Context::start_keyed(const uint8_t p_key[32]) {
	blake3_hasher_init_keyed((blake3_hasher *)ctx, p_key);
}

void Blake3Context::update(const uint8_t *p_src, size_t p_len) {
	blake3_hasher_update((blake3_hasher *)ctx, p_src, p_len);
}

void Blake3Context::update_u32(uint32_t p_value) {
	uint8_t buf[4];
	for (int i = 0; i < 4; i++) {
		buf[i] = (p_value >> (i * 8)) & 0xFF;
	}
	update(buf, 4);
}

void Blake3Context::update_u64(uint64_t p_value) {
	uint8_t buf[8];
	for (int i = 0; i < 8; i++) {
		buf[i] = (p_value >> (i * 8)) & 0xFF;
	}
	update(buf, 8);
}

void Blake3Context::update_string(const String &p_string) {
	CharString cs = p_string.utf8();
	update_u32(cs.length());
	update((const uint8_t *)cs.get_data(), cs.length());
}

CASHash Blake3Context::finish() {
	CASHash h;
	blake3_hasher_finalize((blake3_hasher *)ctx, h.data, CASHash::SIZE);
	return h;
}

void Blake3Context::finish(uint8_t *r_out, size_t p_out_len) {
	blake3_hasher_finalize((blake3_hasher *)ctx, r_out, p_out_len);
}

Blake3Context::Blake3Context() {
	ctx = memalloc(sizeof(blake3_hasher));
	blake3_hasher_init((blake3_hasher *)ctx);
}

Blake3Context::~Blake3Context() {
	memfree(ctx);
}
