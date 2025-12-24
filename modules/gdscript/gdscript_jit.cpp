/**************************************************************************/
/*  gdscript_jit.cpp                                                      */
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

#include "gdscript_jit.h"

#include "core/config/project_settings.h"
#include "core/os/os.h"

GDScriptJIT *GDScriptJIT::singleton = nullptr;

GDScriptJIT::GDScriptJIT() {
	singleton = this;
}

GDScriptJIT::~GDScriptJIT() {
	singleton = nullptr;
}

void GDScriptJIT::_check_platform_support() {
	// Currently, JIT compilation is a placeholder/experimental feature.
	// Actual native code generation would require integration with a JIT library
	// such as LLVM, LuaJIT, or a custom code generator.
	//
	// Platform support would need to be checked based on:
	// - CPU architecture (x86_64, ARM64, etc.)
	// - Operating system (Windows, Linux, macOS, etc.)
	// - Memory protection capabilities (W^X, etc.)

	String arch = Engine::get_singleton()->get_architecture_name();

	// For now, mark JIT as available on common desktop platforms
	// This is placeholder logic - actual implementation would verify
	// that the JIT backend can generate code for this platform.
#if defined(LINUXBSD_ENABLED) || defined(WINDOWS_ENABLED) || defined(MACOS_ENABLED)
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
	jit_available = true;
#else
	jit_available = false;
#endif
#else
	jit_available = false;
#endif
}

void GDScriptJIT::initialize() {
	_check_platform_support();

	// Read JIT settings from project settings
	int mode = GLOBAL_GET("debug/gdscript/jit/mode");
	jit_mode = static_cast<JITMode>(CLAMP(mode, 0, 2));

	hot_function_threshold = GLOBAL_GET("debug/gdscript/jit/hot_function_threshold");

	if (jit_mode != JIT_MODE_DISABLED && !jit_available) {
		WARN_PRINT("GDScript JIT compilation is not available on this platform. Falling back to interpreter.");
		jit_mode = JIT_MODE_DISABLED;
	}

	if (jit_mode != JIT_MODE_DISABLED) {
		print_verbose("GDScript JIT: Initialized in mode " + itos(jit_mode) + " with threshold " + itos(hot_function_threshold));
	}
}

void GDScriptJIT::finalize() {
	// Clean up any JIT-compiled code
	for (KeyValue<GDScriptFunction *, JITFunctionInfo> &kv : compiled_functions) {
		if (kv.value.native_code != nullptr) {
			// In a real implementation, this would free the native code memory
			kv.value.native_code = nullptr;
		}
	}
	compiled_functions.clear();
}

void GDScriptJIT::set_jit_mode(JITMode p_mode) {
	if (p_mode != JIT_MODE_DISABLED && !jit_available) {
		WARN_PRINT("GDScript JIT compilation is not available on this platform.");
		return;
	}
	jit_mode = p_mode;
}

void GDScriptJIT::set_hot_function_threshold(uint32_t p_threshold) {
	hot_function_threshold = MAX(1u, p_threshold);
}

void GDScriptJIT::register_function(GDScriptFunction *p_function) {
	if (!is_jit_enabled()) {
		return;
	}

	if (compiled_functions.has(p_function)) {
		return;
	}

	JITFunctionInfo info;
	info.function_name = p_function->get_name();
	info.bytecode_function = p_function;
	info.status = STATUS_NOT_COMPILED;
	info.call_count = 0;
	info.compile_time_us = 0;
	info.native_code = nullptr;

	compiled_functions[p_function] = info;

	// In aggressive mode, compile immediately
	if (jit_mode == JIT_MODE_AGGRESSIVE) {
		request_compile(p_function);
	}
}

void GDScriptJIT::unregister_function(GDScriptFunction *p_function) {
	if (!compiled_functions.has(p_function)) {
		return;
	}

	JITFunctionInfo &info = compiled_functions[p_function];
	if (info.native_code != nullptr) {
		// In a real implementation, this would free the native code memory
		info.native_code = nullptr;
	}

	compiled_functions.erase(p_function);
}

void GDScriptJIT::record_function_call(GDScriptFunction *p_function) {
	if (!is_jit_enabled() || jit_mode == JIT_MODE_AGGRESSIVE) {
		return;
	}

	HashMap<GDScriptFunction *, JITFunctionInfo>::Iterator it = compiled_functions.find(p_function);
	if (it == compiled_functions.end()) {
		return;
	}

	JITFunctionInfo &info = it->value;
	info.call_count++;

	// Check if function has become "hot" and should be compiled
	if (info.status == STATUS_NOT_COMPILED && info.call_count >= hot_function_threshold) {
		request_compile(p_function);
	}
}

bool GDScriptJIT::request_compile(GDScriptFunction *p_function) {
	if (!is_jit_enabled()) {
		return false;
	}

	HashMap<GDScriptFunction *, JITFunctionInfo>::Iterator it = compiled_functions.find(p_function);
	if (it == compiled_functions.end()) {
		return false;
	}

	JITFunctionInfo &info = it->value;

	if (info.status == STATUS_COMPILED || info.status == STATUS_PENDING) {
		return info.status == STATUS_COMPILED;
	}

	// Mark as pending
	info.status = STATUS_PENDING;

	// TODO: Actual JIT compilation would happen here.
	// This would involve:
	// 1. Analyzing the bytecode
	// 2. Generating native machine code (using LLVM, custom backend, etc.)
	// 3. Storing the native code pointer for later execution
	//
	// For now, this is a placeholder that marks compilation as "failed"
	// since no actual JIT backend is implemented yet.

	uint64_t start_time = OS::get_singleton()->get_ticks_usec();

	// Placeholder: In a real implementation, this would generate native code
	// For now, we just simulate the compilation process
	info.status = STATUS_FAILED; // No actual JIT backend yet
	info.native_code = nullptr;

	uint64_t end_time = OS::get_singleton()->get_ticks_usec();
	info.compile_time_us = end_time - start_time;

	if (info.status == STATUS_COMPILED) {
		print_verbose("GDScript JIT: Compiled function '" + String(info.function_name) + "' in " + itos(info.compile_time_us) + " us");
	}

	return info.status == STATUS_COMPILED;
}

GDScriptJIT::JITStatus GDScriptJIT::get_function_status(GDScriptFunction *p_function) const {
	HashMap<GDScriptFunction *, JITFunctionInfo>::ConstIterator it = compiled_functions.find(p_function);
	if (it == compiled_functions.end()) {
		return STATUS_NOT_COMPILED;
	}
	return it->value.status;
}

uint32_t GDScriptJIT::get_compiled_function_count() const {
	uint32_t count = 0;
	for (const KeyValue<GDScriptFunction *, JITFunctionInfo> &kv : compiled_functions) {
		if (kv.value.status == STATUS_COMPILED) {
			count++;
		}
	}
	return count;
}

uint64_t GDScriptJIT::get_total_compile_time_us() const {
	uint64_t total = 0;
	for (const KeyValue<GDScriptFunction *, JITFunctionInfo> &kv : compiled_functions) {
		total += kv.value.compile_time_us;
	}
	return total;
}
