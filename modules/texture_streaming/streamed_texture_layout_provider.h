/**************************************************************************/
/*  streamed_texture_layout_provider.h                                    */
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

#include "modules/modules_enabled.gen.h" // For cas.

#ifdef MODULE_CAS_ENABLED

#include "modules/cas/cas_layout.h"

// Splits streamed textures (.stex) at mip boundaries when exporting to GCAS packs, so that streaming a mip only
// decompresses that mip. The small mips, which are always loaded, are kept together.
class StreamedTextureLayoutProvider : public CASLayoutProvider {
	GDSOFTCLASS(StreamedTextureLayoutProvider, CASLayoutProvider);

public:
	// Mips smaller than this are grouped into a single chunk.
	static constexpr uint32_t MIP_TAIL_SIZE = 64 * 1024;

	virtual String get_provider_name() const override { return "streamed_texture"; }
	virtual bool handles_file(const String &p_path) const override;
	virtual Ref<CASLayout> get_layout(const String &p_path, const Vector<uint8_t> &p_data) const override;
};

#endif // MODULE_CAS_ENABLED
