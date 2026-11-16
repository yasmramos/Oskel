#pragma once

#include <cstdint>

#include "native/codegen.h"

// A freestanding double-precision math library for the AOT runtime
// (PHASE 7c.5b.4). The hosted engine reaches `std::sin`, `std::exp`, ... in
// libm; a freestanding binary cannot, so the transcendental builtins need
// their own implementation.
//
// The algorithms are ported from musl libm (MIT, (c) 2018 Arm Limited for the
// exp/log/pow family; the trig kernels come from FreeBSD/msun, Sun
// Microsystems). musl's libm is correctly rounded to well under 1 ulp, uses no
// libc and no floating-point environment beyond SSE, which is exactly what the
// no-libc runtime needs. It is emitted directly as x86-64 by `CodeGen`, the
// same way `dtoa` and `parse_float` are.
//
// Fidelity note: glibc (the hosted engine) is *not* correctly rounded for
// these functions, so no independent implementation can match it on every
// input. `exp`/`log`/`pow` agree with glibc on ~99.9% of inputs; every
// disagreement is a single ulp, and both sides are within 1 ulp of the
// correctly rounded result. The contract is therefore "correctly rounded to
// 1 ulp", validated against a bit-exact musl reference, not "byte-identical to
// glibc".
//
// This slice (7c.5b.4a) covers `exp`, `log` and `pow`. The trigonometric family
// (`sin`, `cos`, `tan`) needs the Payne-Hanek argument reduction and lands in
// 7c.5b.4b.
namespace mathlib {

// One entry point per public builtin. Each follows the C convention: `exp` and
// `log` take one `double` in xmm0 and return it in xmm0; `pow` takes its two
// `double`s in xmm0/xmm1. They clobber only the volatile registers
// (rax/rcx/rdx/rsi/rdi/r8-r11, xmm0-xmm15) and preserve the callee-saved ones
// (rbx/rbp/r12-r15).
struct MathHandles {
  CodeGen::Label exp;  // double oskel_math_exp(double)
  CodeGen::Label log;  // double oskel_math_log(double)
  CodeGen::Label pow;  // double oskel_math_pow(double, double)
};

inline MathHandles newMathHandles(CodeGen& code) {
  MathHandles h;
  h.exp = code.newLabel();
  h.log = code.newLabel();
  h.pow = code.newLabel();
  return h;
}

// Emits the three routines and their constant tables, and binds `handles`. The
// tables are emitted after the code (control never falls into them: every
// routine ends in a `ret`), so the caller must emit `emitMathLib` before any
// code that would otherwise be placed directly after the last routine.
void emitMathLib(CodeGen& code, const MathHandles& handles);

}  // namespace mathlib
