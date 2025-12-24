/**************************************************************************/
/*  gdscript_jit.h                                                        */
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

#include "gdscript_function.h"

#include "core/object/ref_counted.h"
#include "core/string/string_name.h"
#include "core/templates/hash_map.h"
#include "core/variant/variant.h"

class GDScript;

/**
 * GDScriptJIT provides infrastructure for Just-In-Time compilation of GDScript.
 *
 * This is an experimental feature that can optionally compile frequently-executed
 * GDScript functions to native code for improved performance.
 *
 * JIT compilation modes:
 * - DISABLED: No JIT compilation (default, interpreter only)
 * - ENABLED: JIT compile hot functions automatically
 * - AGGRESSIVE: JIT compile all functions immediately
 */
class GDScriptJIT {
public:
	enum JITMode {
		JIT_MODE_DISABLED = 0,
		JIT_MODE_ENABLED = 1,
		JIT_MODE_AGGRESSIVE = 2,
	};

	enum JITStatus {
		STATUS_NOT_COMPILED = 0,
		STATUS_PENDING = 1,
		STATUS_COMPILED = 2,
		STATUS_FAILED = 3,
	};

	struct JITFunctionInfo {
		StringName function_name;
		GDScriptFunction *bytecode_function = nullptr;
		JITStatus status = STATUS_NOT_COMPILED;
		uint64_t call_count = 0;
		uint64_t compile_time_us = 0;
		void *native_code = nullptr;
	};

private:
	static GDScriptJIT *singleton;

	JITMode jit_mode = JIT_MODE_DISABLED;
	uint32_t hot_function_threshold = 1000; // Number of calls before considering a function "hot"
	bool jit_available = false;

	HashMap<GDScriptFunction *, JITFunctionInfo> compiled_functions;

	void _check_platform_support();

public:
	static GDScriptJIT *get_singleton() { return singleton; }

	void initialize();
	void finalize();

	// Configuration
	void set_jit_mode(JITMode p_mode);
	JITMode get_jit_mode() const { return jit_mode; }
	void set_hot_function_threshold(uint32_t p_threshold);
	uint32_t get_hot_function_threshold() const { return hot_function_threshold; }

	// JIT availability
	bool is_jit_available() const { return jit_available; }
	bool is_jit_enabled() const { return jit_mode != JIT_MODE_DISABLED && jit_available; }

	// Function management
	void register_function(GDScriptFunction *p_function);
	void unregister_function(GDScriptFunction *p_function);
	void record_function_call(GDScriptFunction *p_function);

	// Compilation
	bool request_compile(GDScriptFunction *p_function);
	JITStatus get_function_status(GDScriptFunction *p_function) const;

	// Statistics
	uint32_t get_compiled_function_count() const;
	uint64_t get_total_compile_time_us() const;

	GDScriptJIT();
	~GDScriptJIT();
};
