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
#include "sljitLir.c"

#ifndef SLJIT_ARGS5
#define SLJIT_ARGS5(ret, arg1, arg2, arg3, arg4, arg5) \
	(SLJIT_ARGS4(ret, arg1, arg2, arg3, arg4) | SLJIT_ARG_VALUE(SLJIT_ARG_TO_TYPE(arg5), 5))
#endif

#if defined(SLJIT_CONFIG_UNSUPPORTED) && SLJIT_CONFIG_UNSUPPORTED

void *GDScriptJIT::compile(GDScriptFunction *p_function) {
	return nullptr;
}

void GDScriptJIT::free_code(void *p_code) {
}

#else

// Helper to copy Variant. Called from JIT.
static void gdscript_jit_copy_variant(Variant *r_dest, const Variant *p_src) {
	*r_dest = *p_src;
}

void *GDScriptJIT::compile(GDScriptFunction *p_function) {
	if (!p_function || !p_function->_code_ptr) {
		return nullptr;
	}

	struct sljit_compiler *compiler = sljit_create_compiler(nullptr);
	if (!compiler) {
		return nullptr;
	}

	// Signature: void jit_func(GDScriptInstance *p_instance, Variant *p_stack, int p_argcount, Callable::CallError &r_err, Variant *r_ret)
	// Arguments:
	// 1 (S0): p_instance
	// 2 (S1): p_stack (Pointer)
	// 3 (S2): p_argcount
	// 4 (S3): r_err
	// 5 (S4): r_ret (Pointer)

	sljit_emit_enter(compiler, 0, SLJIT_ARGS5(void, P, P, W, P, P), 5, 5, 0);

	const int *code = p_function->_code_ptr;
	int code_size = p_function->_code_size;
	int ip = 0;

	bool compiled = false;

	// Simple linear scan for a single basic block.
	while (ip < code_size) {
		int opcode = code[ip];

		if (opcode == GDScriptFunction::OPCODE_RETURN) {
			// OPCODE_RETURN has 1 argument: address of return value.
			// Format: OPCODE_RETURN, address
			if (ip + 1 >= code_size) {
				goto fail;
			}
			int address = code[ip + 1];
			int address_type = (address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS;
			int address_index = address & GDScriptFunction::ADDR_MASK;

			if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
				// Return a constant.
				if (address_index < 0 || address_index >= p_function->_constant_count) {
					goto fail;
				}
				const Variant *constant = &p_function->constants[address_index];

				// Call helper: gdscript_jit_copy_variant(r_ret, constant)
				// r_ret is the 5th argument (SLJIT_S4).
				// constant is a pointer (immediate).

				// Move arguments for the call.
				// Arg 1: r_ret (S4) -> R0
				sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S4, 0);
				// Arg 2: constant (immediate) -> R1
				sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)constant);

				sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS2(void, P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(gdscript_jit_copy_variant));

				sljit_emit_return_void(compiler);
				compiled = true;
				ip += 2; // OPCODE_RETURN + address
				break; // Done compiling this path.
			} else {
				// Only support returning constants for now.
				goto fail;
			}
		} else if (opcode == GDScriptFunction::OPCODE_END) {
			// Just return void/null.
			sljit_emit_return_void(compiler);
			compiled = true;
			ip += 1; // OPCODE_END
			break;
		} else {
			// Unsupported opcode.
			goto fail;
		}
	}

	if (compiled) {
		void *code_ptr = sljit_generate_code(compiler);
		sljit_free_compiler(compiler);
		return code_ptr;
	}

fail:
	sljit_free_compiler(compiler);
	return nullptr;
}

void GDScriptJIT::free_code(void *p_code) {
	if (p_code) {
		sljit_free_code(p_code, nullptr);
	}
}

#endif // SLJIT_CONFIG_UNSUPPORTED
