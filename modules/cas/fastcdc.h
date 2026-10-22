/**************************************************************************/
/*  fastcdc.h                                                             */
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

#include "core/templates/local_vector.h"

#include <cstdint>

// Content-defined chunking (FastCDC with normalized chunking).
// Boundaries depend only on nearby content, so inserting or removing data only changes the chunks around the edit,
// which keeps most chunks of similar files (and of new versions of a file) identical.
class FastCDC {
public:
	struct Params {
		uint32_t min_size = 16 * 1024;
		uint32_t avg_size = 64 * 1024;
		uint32_t max_size = 256 * 1024;
	};

private:
	static uint64_t gear[256];
	static bool gear_initialized;
	static void _init_gear();

	static uint64_t _find_cut(const uint8_t *p_data, uint64_t p_size, const Params &p_params, uint64_t p_mask_small, uint64_t p_mask_large);

public:
	// Appends the end offsets (exclusive) of the chunks of p_data to r_ends.
	static void split(const uint8_t *p_data, uint64_t p_size, const Params &p_params, LocalVector<uint64_t> &r_ends);
};
