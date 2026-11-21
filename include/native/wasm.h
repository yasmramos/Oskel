#pragma once

#include <cstdint>
#include <vector>

#include "bytecode.h"
#include "native/target.h"

// WebAssembly backend (PHASE 9).
//
// The AOT backend has two front ends: the x86-64 lowering (`native/lower.h`)
// and this one. Both consume the same `Bytecode` and share the *semantics* of
// the runtime (what `+` does, what is truthy, how a value renders), but not one
// line of emission code: WASM is a stack machine with its own instruction set,
// so reusing `CodeGen` (an x86-64 encoder) would mean emitting x86 bytes into a
// `.wasm` container. What is reused is the value model and the contract, not the
// encoder.
//
// The emitted module is self-contained except for two imports:
//
//   (import "oskel" "write" (func (param i32 i32 i32) (result i32)))
//   (import "oskel" "exit"  (func (param i32)))
//
// A browser, an edge runtime or a test harness supplies them; that is the only
// host surface. `memory` and `_start` are exported.
//
// The value model mirrors `native/value.h`: a value is a (tag, payload) pair of
// i64, exactly the `AotValue` the x86-64 runtime uses, so the two backends agree
// on tags and on the on-image layouts of strings, closures, ranges and errors.
// A value is passed and returned as two WASM values (multi-value), not boxed in
// linear memory, so arithmetic does not allocate.
//
// PHASE 9.1 supports the scalar core: nil/bool/int/float/double/string, ranges,
// every operator, control flow, globals, locals, functions, closures and the
// portable builtins (see `wasm.cpp`). Arrays and maps are rejected at build time
// with a dedicated message, and a builtin the runtime does not bind is a clean
// run-time error rather than a mis-compiled program. The gap is tracked in
// `docs/BACKLOG.md` §6.
std::vector<std::uint8_t> lowerToWasm(const Target& target, const Bytecode& proto);
