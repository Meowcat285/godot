/**************************************************************************/
/*  fastcdc.cpp                                                           */
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

#include "fastcdc.h"

#include "core/error/error_macros.h"
#include "core/typedefs.h"

uint64_t FastCDC::gear[256];
bool FastCDC::gear_initialized = false;

void FastCDC::_init_gear() {
	// The gear table must never change, otherwise chunk boundaries (and deduplication with existing packs and caches)
	// change too. Generate it from a fixed seed with SplitMix64.
	uint64_t state = 0x6a09e667f3bcc909ULL;
	for (int i = 0; i < 256; i++) {
		state += 0x9e3779b97f4a7c15ULL;
		uint64_t z = state;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
		gear[i] = z ^ (z >> 31);
	}
	gear_initialized = true;
}

uint64_t FastCDC::_find_cut(const uint8_t *p_data, uint64_t p_size, const Params &p_params, uint64_t p_mask_small, uint64_t p_mask_large) {
	if (p_size <= p_params.min_size) {
		return p_size;
	}
	uint64_t end = MIN(p_size, uint64_t(p_params.max_size));
	uint64_t normal = MIN(end, uint64_t(p_params.avg_size));
	uint64_t fp = 0;
	uint64_t i = p_params.min_size;
	// Harder to match before the average size and easier after it, which narrows the chunk size distribution.
	for (; i < normal; i++) {
		fp = (fp << 1) + gear[p_data[i]];
		if (!(fp & p_mask_small)) {
			return i + 1;
		}
	}
	for (; i < end; i++) {
		fp = (fp << 1) + gear[p_data[i]];
		if (!(fp & p_mask_large)) {
			return i + 1;
		}
	}
	return end;
}

void FastCDC::split(const uint8_t *p_data, uint64_t p_size, const Params &p_params, LocalVector<uint64_t> &r_ends) {
	ERR_FAIL_COND(p_params.min_size == 0 || p_params.min_size > p_params.avg_size || p_params.avg_size > p_params.max_size);
	if (unlikely(!gear_initialized)) {
		_init_gear();
	}

	// Each shift moves older bytes to higher bits, so the high bits of the fingerprint depend on a ~64 byte window.
	uint32_t bits = 0;
	while ((1U << (bits + 1)) <= p_params.avg_size && bits < 31) {
		bits++;
	}
	uint64_t mask_small = ~0ULL << (64 - MIN(63U, bits + 1));
	uint64_t mask_large = ~0ULL << (64 - MAX(1U, bits - 1));

	uint64_t offset = 0;
	while (offset < p_size) {
		uint64_t cut = _find_cut(p_data + offset, p_size - offset, p_params, mask_small, mask_large);
		offset += cut;
		r_ends.push_back(offset);
	}
}
