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
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO, THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "gdscript_jit.h"

#define SLJIT_CONFIG_AUTO 1
#define SLJIT_CONFIG_STATIC 1
#include "thirdparty/pcre2/deps/sljit/sljit_src/sljitLir.c"

#if defined(SLJIT_CONFIG_UNSUPPORTED) && SLJIT_CONFIG_UNSUPPORTED

void *GDScriptJIT::compile(GDScriptFunction *p_function) {
	return nullptr;
}

void GDScriptJIT::free_code(void *p_code) {
}

#else

void *GDScriptJIT::compile(GDScriptFunction *p_function) {
	// For now, we return nullptr to force fallback to the interpreter.
	// This ensures that we don't break existing functionality while the JIT is being implemented.
	// Once the JIT is capable of correctly executing GDScript bytecode, we can return the generated code.
	return nullptr;

	/*
	// Infrastructure for future implementation:
	struct sljit_compiler *compiler = sljit_create_compiler(nullptr);
	if (!compiler) {
		return nullptr;
	}

	// Signature: void jit_func(GDScriptInstance *p_instance, const Variant **p_args, int p_argcount, Callable::CallError &r_err, Variant *r_ret)
	sljit_emit_enter(compiler, 0, SLJIT_ARGS5(void, P, P, W, P, P), 0, 0, 0);

	// ... Generate code ...

	sljit_emit_return_void(compiler);

	void *code = sljit_generate_code(compiler);
	sljit_free_compiler(compiler);

	return code;
	*/
}

void GDScriptJIT::free_code(void *p_code) {
	if (p_code) {
		sljit_free_code(p_code, nullptr);
	}
}

#endif // SLJIT_CONFIG_UNSUPPORTED
