#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ffi_types.h"
#include "native/codegen.h"
#include "native/entry.h"
#include "native/value.h"

// Embedded runtime for the freestanding AOT backend (PHASE 7c.1).
//
// A generated binary has no libc, so every operation the bytecode needs that
// is not a single machine instruction is *emitted as machine code* into the
// same buffer as the program -- exactly the way `entry.cpp` appends `_start`
// and the syscall wrappers. There is no linker, no relocations and no shared
// library: a `call` from a lowered opcode targets a label this module binds.
//
// The helpers implement the value semantics the hosted engine implements in
// `ops.h`. They must agree with it -- what `+` does, what is truthy, how two
// values compare, what a type mismatch reports -- without sharing any code,
// because `ops.h` is built on the reference-counted `Value` and a throwing
// `Runtime`, neither of which exists in a `-nostdlib` binary.

// Labels for the runtime helpers. The caller creates them up front so a
// program lowered *before* the runtime can already reference them, then calls
// `emitAotRuntime` to bind them.
//
// Every helper follows the System V AMD64 ABI: an `AotValue` is passed in two
// registers (tag then payload, in rdi:rsi for the first and rdx:rcx for the
// second) and returned in rax:rdx.
struct AotRuntimeHandles {
  CodeGen::Label printValue;     // void  oskel_print_value(AotValue v)
  // The same printer without the trailing newline, for the `print`/`write`/
  // `eprint` builtins, which join their arguments with single spaces and add
  // one newline themselves. `printValueRaw` writes to stdout, `printValueErrRaw`
  // to stderr (PHASE 7c.2d).
  CodeGen::Label printValueRaw;     // void oskel_print_value_raw(AotValue v)
  CodeGen::Label printValueErrRaw;  // void oskel_print_value_err_raw(AotValue v)
  // Numeric binary arithmetic with the int/float/double promotion of the
  // hosted engine. The selector (AotArithOp) is the third argument.
  CodeGen::Label arithValues;    // AotValue oskel_arith(AotValue a, AotValue b, long op)
  // `**`: integer power for int/int with a non-negative exponent (the hosted
  // `intPow`); every other case needs libm and is rejected for now.
  CodeGen::Label powValues;      // AotValue oskel_pow(AotValue a, AotValue b)
  // Integer `<<`/`>>`, selected by AotShiftOp; the shift count is a second
  // `AotValue` and is range-checked against 0..63.
  CodeGen::Label shiftValues;    // AotValue oskel_shift(AotValue a, AotValue b, long op)
  CodeGen::Label bitwiseValues;  // AotValue oskel_bitwise(AotValue a, AotValue b, long op)
  CodeGen::Label negateValue;    // AotValue oskel_negate(AotValue a)
  CodeGen::Label notValue;       // AotValue oskel_not(AotValue a)
  CodeGen::Label bitNotValue;    // AotValue oskel_bit_not(AotValue a)
  CodeGen::Label compareValues;  // AotValue oskel_compare(AotValue a, AotValue b, long op)
  CodeGen::Label truthy;         // long      oskel_truthy(AotValue a)
  // Bump allocation from the arena seeded by `oskel_init_arena`: returns an
  // 8-byte-aligned pointer, or null when the region is exhausted. The size is
  // in rdi.
  CodeGen::Label alloc;          // void* oskel_alloc(long size)
  // String concatenation: allocates `lenA + lenB + 8` bytes, writes the
  // combined length and bytes, and returns an `AotValue` (tag Str). Used by
  // `oskel_arith` for `Add` when both operands are strings.
  CodeGen::Label concat;         // AotValue oskel_concat(AotValue a, AotValue b)
  // The global table (PHASE 7c.2a). It is a contiguous array of `AotValue`
  // slots in the arena; the chunk allocates it once at entry and keeps its
  // base in r15. `initGlobals` takes the slot count in rdi. `globalSet` takes
  // the slot index in rdi and the value in rsi:rdx; `globalGet` takes the slot
  // index in rdi and returns the value in rax:rdx. Global names never reach
  // the binary: the compiler assigns each one an index, exactly like a local.
  CodeGen::Label initGlobals;    // void oskel_init_globals(long count)
  CodeGen::Label globalSet;      // void oskel_global_set(long slot, AotValue v)
  CodeGen::Label globalGet;      // AotValue oskel_global_get(long slot)
  // Closures and calls (PHASE 7c.2c.1). `closure` allocates an `AotClosure` in
  // the arena for a proto's code address (rdi), arity (rsi) and captured-cell
  // count (rdx), and returns it as a `Func` value in rax:rdx. The lowering
  // writes the captured cell pointers into the header itself (each is a plain
  // 8-byte store), so `closure` only zero-fills the slot area. `call`
  // dispatches a callee (rdi:rsi) with `argc` arguments (rdx) whose window
  // starts at `args` (rcx): it checks the callee is a function, then calls its
  // code with the lowering's own function convention (`AotValue entry(AotValue*
  // args, long argc, AotClosure* self)`), returning the result in rax:rdx. A
  // non-function callee is a fatal error.
  CodeGen::Label closure;        // AotValue oskel_closure(void* code, long arity, long upvalueCount)
  CodeGen::Label call;           // AotValue oskel_call(AotValue callee, long argc, AotValue* args)
  // `AotValue* oskel_cell_new(AotValue v)`: allocate a 16-byte cell in the arena
  // holding `v` (rdi:rsi) and return its address (rax). A captured local lives
  // in a cell shared with the closures that capture it, so a write through the
  // closure and a read in the frame see the same value (PHASE 7c.2c.2).
  CodeGen::Label cellNew;        // AotValue* oskel_cell_new(AotValue v)
  // Fatal "wrong number of arguments" report. A function's prologue calls it
  // when the argument count does not match its declared arity, the runtime
  // counterpart of the hosted engine's arity check in `pushOskelFrame`.
  CodeGen::Label arityError;     // void oskel_arity_error(void)
  // The `print`/`write`/`eprint` builtins, as machine-code functions with the
  // same convention as a bytecode proto (`AotValue entry(AotValue* args, long
  // argc, AotClosure* self)`). The chunk seeds each into its global slot as an
  // `AotClosure`, so a `CALL` reaches them through the ordinary `oskel_call`
  // path. `print` and `eprint` add a trailing newline, `write` does not;
  // `eprint` writes to stderr. Arguments are joined with single spaces
  // (PHASE 7c.2d).
  CodeGen::Label printBuiltin;   // AotValue oskel_builtin_print(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label writeBuiltin;   // AotValue oskel_builtin_write(...)
  CodeGen::Label eprintBuiltin;  // AotValue oskel_builtin_eprint(...)
  // Collections (PHASE 7c.3). `tableNew` allocates an empty array (kind 0) or
  // map (kind 1); `tableSet`/`tableGet` are the dynamic-index forms
  // (`R[b][R[c]]`), `memberSet`/`memberGet` the constant-name forms
  // (`R[b].name`); `contains` implements `in`; `iterNext` advances a `for-in`
  // cursor, taking a pointer to the cursor slot (rdi) and the iterable
  // (rdx:rcx) and returning the item (rax:rdx) with the done flag in r8;
  // `strEq` compares two `AotString`s by content (map keys are compared by
  // value, not pointer).
  CodeGen::Label tableNew;   // AotValue oskel_table_new(long kind)
  CodeGen::Label tableGet;   // AotValue oskel_table_get(AotValue obj, AotValue index)
  CodeGen::Label memberGet;  // AotValue oskel_member_get(AotValue obj, AotValue name)
  CodeGen::Label tableSet;   // void oskel_table_set(AotValue obj, AotValue index, AotValue value)
  CodeGen::Label memberSet;  // void oskel_member_set(AotValue obj, AotValue name, AotValue value)
  CodeGen::Label contains;   // AotValue oskel_contains(AotValue needle, AotValue haystack)
  CodeGen::Label iterNext;   // AotValue oskel_iter_next(AotValue* cursor, AotValue iterable)
  CodeGen::Label strEq;      // long oskel_str_eq(AotString* a, AotString* b)
  // `AotValue oskel_pack_rest(AotValue* first, long count)`: the variadic
  // parameter pack (PHASE 7c.6). A variadic function accepts `numParams - 1` or
  // more arguments; its prologue binds the fixed parameters and calls this
  // helper to build an array from the extras and bind it to the last slot. The
  // argument window grows *down*, so `first` points at the first extra (the
  // highest address) and item `i` sits at `first - 16 * i`. The result is an
  // ordinary `Array` value, exactly like the VM's packed `rest` binding.
  CodeGen::Label packRest;   // AotValue oskel_pack_rest(AotValue* first, long count)
  // The hidden helpers the compiler emits for PHASE 6.5 features. They use the
  // proto convention and are seeded into their global slots like `print`, so
  // each one is an ordinary call.
  // `$isError` (try/catch) reports whether a value is an error. It reads the
  // error tag, so a value built by `error(...)` (PHASE 7c.5c) is recognized and
  // `try`/`x catch y` propagates/recovers exactly like the VM.
  CodeGen::Label isErrorBuiltin;   // AotValue oskel_builtin_is_error(AotValue* args, long argc, AotClosure* self)
  // `error(message, code?)` (PHASE 7c.5c) builds an error value: a distinct
  // runtime tag (`type(err) == "error"`) carrying a message and an integer code
  // (0 by default). It mirrors the hosted `error` builtin, so `error("x")` is a
  // value the AOT program can store, compare, render and catch.
  CodeGen::Label errorBuiltin;     // AotValue oskel_builtin_error(AotValue* args, long argc, AotClosure* self)
  // `$nonNull` (`a!!`): returns the operand, or fails when it is nil.
  CodeGen::Label nonNullBuiltin;   // AotValue oskel_builtin_non_null(AotValue* args, long argc, AotClosure* self)
  // `$str` (string interpolation and computed keys): renders the operand to a
  // string, byte-identical to the hosted `Value::toString`.
  CodeGen::Label strBuiltin;       // AotValue oskel_builtin_str(AotValue* args, long argc, AotClosure* self)
  // `$range` (`a..b` / `a..=b`): allocates a `Range` value from two integer
  // bounds and an inclusive flag.
  CodeGen::Label rangeBuiltin;     // AotValue oskel_builtin_range(AotValue* args, long argc, AotClosure* self)
  // `$slice` (`obj[start:end:step]`): slices a string by UTF-8 character or an
  // array by element, following Python's bounds rule.
  CodeGen::Label sliceBuiltin;     // AotValue oskel_builtin_slice(AotValue* args, long argc, AotClosure* self)
  // The public standard-library builtins (PHASE 7c.5a). They use the proto
  // convention too and are seeded into their global slots like `print`, so a
  // call reaches them through the ordinary `oskel_call` path. Each is a pure
  // value transformation -- no libm, no libc -- so they compile into the
  // freestanding binary. `str` and `isError` reuse the existing `strBuiltin`
  // and `isErrorBuiltin` (the public names are just two more seeds of the same
  // code), so only the remaining eight need their own entry point.
  CodeGen::Label lenBuiltin;       // AotValue oskel_builtin_len(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label typeBuiltin;      // AotValue oskel_builtin_type(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label reprBuiltin;      // AotValue oskel_builtin_repr(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label boolBuiltin;      // AotValue oskel_builtin_bool(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label ordBuiltin;       // AotValue oskel_builtin_ord(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label chrBuiltin;       // AotValue oskel_builtin_chr(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label assertBuiltin;    // AotValue oskel_builtin_assert(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label panicBuiltin;     // AotValue oskel_builtin_panic(AotValue* args, long argc, AotClosure* self)
  // The numeric conversions (PHASE 7c.5b.1). Each is the public `int`/`float`/
  // `double` for a numeric or boolean operand, matching the hosted conversion
  // (SSE only -- no libm). `int` also parses a string operand since 7c.5b.2a;
  // `float`/`double` still reject a string until 7c.5b.2b, with the same
  // "cannot convert" shape.
  CodeGen::Label intBuiltin;       // AotValue oskel_builtin_int(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label floatBuiltin;     // AotValue oskel_builtin_float(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label doubleBuiltin;    // AotValue oskel_builtin_double(AotValue* args, long argc, AotClosure* self)
  // The exact math builtins (PHASE 7c.5b.3): `floor`, `ceil`, `round` and
  // `sqrt`. They are correctly rounded -- SSE `roundsd`-free rounding plus
  // `sqrtsd` -- so they reproduce the hosted engine bit for bit without an
  // embedded libm. `sqrt` of a negative operand fails like the hosted one.
  CodeGen::Label floorBuiltin;     // AotValue oskel_builtin_floor(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label ceilBuiltin;      // AotValue oskel_builtin_ceil(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label roundBuiltin;     // AotValue oskel_builtin_round(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label sqrtBuiltin;      // AotValue oskel_builtin_sqrt(AotValue* args, long argc, AotClosure* self)
  // The integer text parser behind `int(string)` (PHASE 7c.5b.2a). It is not a
  // public builtin: `oskel_builtin_int` calls it with the trimmed string bytes.
  CodeGen::Label intTextParser;    // int64_t oskel_parse_int_text(char* data, long length)
  // The string builder primitives, exposed so a test can drive them directly
  // (a capacity of zero and a capacity that overflows the doubling must both
  // terminate and produce the exact bytes).
  CodeGen::Label sbInit;    // void oskel_sb_init(AotStringBuilder* sb, char* data, long capacity)
  CodeGen::Label sbAppend;  // void oskel_sb_append(AotStringBuilder* sb, const char* ptr, long len)
  CodeGen::Label sbNew;     // AotStringBuilder* oskel_sb_new(void)
  // The transcendental builtins (PHASE 7c.5b.4a): `exp`, `log` and `pow`. They
  // need libm in the hosted engine; a freestanding binary embeds the ported
  // musl routines (`native/mathlib.h`) and these wrappers do the argument
  // checks, the int-to-double conversion and the error messages.
  CodeGen::Label expBuiltin;  // AotValue oskel_builtin_exp(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label logBuiltin;  // AotValue oskel_builtin_log(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label powBuiltin;  // AotValue oskel_builtin_pow(AotValue* args, long argc, AotClosure* self)
  // The trigonometric builtins (PHASE 7c.5b.4b): `sin`, `cos` and `tan`. Same
  // shape as the exp/log/pow wrappers; the embedded routines reduce the
  // argument with `__rem_pio2` and match the hosted engine to 1 ulp.
  CodeGen::Label sinBuiltin;  // AotValue oskel_builtin_sin(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label cosBuiltin;  // AotValue oskel_builtin_cos(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label tanBuiltin;  // AotValue oskel_builtin_tan(AotValue* args, long argc, AotClosure* self)
  // Array concatenation (`a + b` on two arrays), the array twin of `concat`:
  // allocates a fresh array of `lenA + lenB` items and copies both runs in,
  // leaving the operands unchanged. It is the hosted `ops::binary` `Tok::Plus`
  // case for two arrays, used by `oskel_arith` for `Add`.
  CodeGen::Label arrayConcat;  // AotValue oskel_array_concat(AotValue a, AotValue b)
  // The remaining public builtins (PHASE 7c.5b.5): `range`, `assertEq` and
  // `exit`. `range` builds the same `Range` value as the hidden `$range` helper
  // (one bound defaults the start to 0); `assertEq` compares two values with the
  // shared `oskel_compare` and, on a mismatch, renders the same
  // "<msg>: expected A, got B" text the hosted builtin produces; `exit` leaves
  // through the entry prelude's `oskel_exit`, so it never returns.
  CodeGen::Label rangeFnBuiltin;   // AotValue oskel_builtin_range_fn(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label assertEqBuiltin;  // AotValue oskel_builtin_assert_eq(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label exitBuiltin;      // AotValue oskel_builtin_exit(AotValue* args, long argc, AotClosure* self)
  // `time()` (wall clock): takes no argument and returns the whole seconds since
  // the Unix epoch as an `int`. It is the only public builtin that reaches the
  // outside world for a *value* rather than output, so it reads the `entry.now`
  // handle the target's prelude binds (syscall, libc, kernel32 or the CMOS RTC).
  CodeGen::Label timeBuiltin;      // AotValue oskel_builtin_time(AotValue* args, long argc, AotClosure* self)
  // `input([prompt])`: reads one line from the keyboard and returns it as a
  // string (nil on end of input). It reads through the `entry.read` handle,
  // which only the bare-metal prelude binds -- the keyboard slice gives the
  // kernel an input channel -- so it is seeded as a global for that target
  // only (see `lowerToX64`). The other targets keep the AOT `input` unseeded.
  CodeGen::Label inputBuiltin;     // AotValue oskel_builtin_input(AotValue* args, long argc, AotClosure* self)
  // `memSize()` (usable RAM, bare metal only): takes no argument and returns the
  // bytes of available physical memory as an `int`, read from the `entry.memSize`
  // handle the bare-metal prelude binds (the multiboot map). The hosted targets
  // have no such handle, so `lower.cpp` seeds this builtin only for
  // `Os::Freestanding` and rejects `memSize()` elsewhere by name.
  CodeGen::Label memSizeBuiltin;   // AotValue oskel_builtin_mem_size(AotValue* args, long argc, AotClosure* self)
  // `random()`/`seed()` (PHASE 7c.5b.6): the `mt19937_64` generator the hosted
  // engine uses, emitted in machine code so a native binary draws the same
  // sequence. The state (312 words plus a cursor) lives in the hidden
  // `$randomState` global, because the image has no writable data segment. The
  // reduction formulas are the hosted engine's, so `random()` returns the same
  // double and `random(n)` the same integer on both engines -- see
  // `src/runtime/builtins.cpp` for why a library distribution cannot be used.
  CodeGen::Label randomStateInit;  // void oskel_random_state_init(AotValue* slot)
  CodeGen::Label randomBuiltin;    // AotValue oskel_builtin_random(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label seedBuiltin;      // AotValue oskel_builtin_seed(AotValue* args, long argc, AotClosure* self)
  // `pcall(fn, args...)` (BACKLOG §6 item 4). It is the only builtin that has to
  // recover from a *raised* error, which the AOT runtime reports by exiting, so
  // it installs a handler record on its own stack that `oskel_fail_dynamic`
  // jumps back to instead of printing (a non-local return in the call ABI). The
  // record's address lives in the hidden `$pcallHandler` global, which the
  // lowerer reserves only when the program calls `pcall`; a program that does
  // not keeps a runtime and an image byte for byte what they were.
  CodeGen::Label pcallBuiltin;     // AotValue oskel_builtin_pcall(AotValue* args, long argc, AotClosure* self)
  // Bound array methods (PHASE 7c.3c/7c.3e). `a.<method>` on an array yields a
  // closure that captured its receiver in one upvalue cell, exactly like the
  // hosted `bindArray`. `bindArrayMethod` builds that closure from the receiver
  // (rdi:rsi) and a selector (rdx); the method bodies are ordinary builtins
  // with the proto convention, so a `CALL` reaches them through `oskel_call`.
  CodeGen::Label bindArrayMethod;  // AotValue oskel_bind_array_method(AotValue arr, long which)
  CodeGen::Label arrayPushMethod;  // AotValue oskel_builtin_array_push(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label arrayPopMethod;   // AotValue oskel_builtin_array_pop(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label arrayShiftMethod;     // AotValue oskel_builtin_array_shift(...)
  CodeGen::Label arrayUnshiftMethod;   // AotValue oskel_builtin_array_unshift(...)
  CodeGen::Label arrayInsertMethod;    // AotValue oskel_builtin_array_insert(...)
  CodeGen::Label arrayRemoveAtMethod;  // AotValue oskel_builtin_array_remove_at(...)
  CodeGen::Label arrayContainsMethod;  // AotValue oskel_builtin_array_contains(...)
  CodeGen::Label arrayIndexOfMethod;   // AotValue oskel_builtin_array_index_of(...)
  CodeGen::Label arrayClearMethod;     // AotValue oskel_builtin_array_clear(...)
  CodeGen::Label arraySliceMethod;     // AotValue oskel_builtin_array_slice(...)
  CodeGen::Label arrayConcatMethod;    // AotValue oskel_builtin_array_concat(...)
  CodeGen::Label arrayReversedMethod;  // AotValue oskel_builtin_array_reversed(...)
  CodeGen::Label arrayCloneMethod;     // AotValue oskel_builtin_array_clone(...)
  // The callback-taking array methods (PHASE 7c.3h). Unlike the bodies above,
  // these invoke a closure they receive as an argument, through `oskel_call`:
  // the first helpers in this runtime to call back into the program. A callback
  // runs arbitrary Oskel code and allocates, so r13/r14 -- the arena cursor and
  // end -- must be left advanced by whatever it allocated, and r15 (the global
  // table) is never written: only rbx and r12, the program's own locals in the
  // calling frame, are saved and restored.
  CodeGen::Label arrayMapMethod;       // AotValue oskel_builtin_array_map(...)
  CodeGen::Label arrayFilterMethod;    // AotValue oskel_builtin_array_filter(...)
  CodeGen::Label arrayEachMethod;      // AotValue oskel_builtin_array_each(...)
  CodeGen::Label arrayFindMethod;      // AotValue oskel_builtin_array_find(...)
  CodeGen::Label arrayAnyMethod;       // AotValue oskel_builtin_array_any(...)
  CodeGen::Label arrayAllMethod;       // AotValue oskel_builtin_array_all(...)
  CodeGen::Label arrayReduceMethod;    // AotValue oskel_builtin_array_reduce(...)
  CodeGen::Label arraySumMethod;       // AotValue oskel_builtin_array_sum(...)
  CodeGen::Label arrayJoinMethod;      // AotValue oskel_builtin_array_join(...)
  // Bound map methods (PHASE 7c.3g). `m.<method>` on a map yields a closure
  // that captured its receiver in one upvalue cell, exactly like the array
  // methods above and the hosted `bindMap`. `bindMapMethod` builds that closure
  // from the receiver (rdi:rsi) and a selector (rdx); the method bodies are
  // ordinary builtins with the proto convention, so a `CALL` reaches them
  // through `oskel_call`. `oskel_member_get` dispatches the ten names through a
  // selector-indexed table.
  CodeGen::Label bindMapMethod;    // AotValue oskel_bind_map_method(AotValue map, long which)
  CodeGen::Label mapKeysMethod;    // AotValue oskel_builtin_map_keys(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label mapValuesMethod;  // AotValue oskel_builtin_map_values(...)
  CodeGen::Label mapEntriesMethod; // AotValue oskel_builtin_map_entries(...)
  CodeGen::Label mapHasMethod;     // AotValue oskel_builtin_map_has(...)
  CodeGen::Label mapGetMethod;     // AotValue oskel_builtin_map_get(...)
  CodeGen::Label mapSetMethod;     // AotValue oskel_builtin_map_set(...)
  CodeGen::Label mapDeleteMethod;  // AotValue oskel_builtin_map_delete(...)
  CodeGen::Label mapClearMethod;   // AotValue oskel_builtin_map_clear(...)
  CodeGen::Label mapMergeMethod;   // AotValue oskel_builtin_map_merge(...)
  CodeGen::Label mapCloneMethod;   // AotValue oskel_builtin_map_clone(...)
  // The pointer builtins (BACKLOG §6 item 25). A `ptr`/`cstring` is an opaque
  // handle (PHASE 6.6): `cstring` builds one from a string, `buffer` allocates
  // zeroed bytes, `cstr` reads the bytes back as a string, `byteLen` reports how
  // many there are, `loadByte`/`storeByte` read and write one, `intToPtr`/
  // `ptrToInt` wrap and unwrap the opaque token, and `free` releases the handle.
  // The layout is `AotPtr` in `native/value.h`, so the two engines agree on what
  // a handle is; nothing here reaches a host, which is why every target emits
  // them.
  CodeGen::Label cstringBuiltin;   // AotValue oskel_builtin_cstring(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label cstrBuiltin;      // AotValue oskel_builtin_cstr(...)
  CodeGen::Label bufferBuiltin;    // AotValue oskel_builtin_buffer(...)
  CodeGen::Label byteLenBuiltin;   // AotValue oskel_builtin_byte_len(...)
  CodeGen::Label loadByteBuiltin;  // AotValue oskel_builtin_load_byte(...)
  CodeGen::Label storeByteBuiltin; // AotValue oskel_builtin_store_byte(...)
  CodeGen::Label intToPtrBuiltin;  // AotValue oskel_builtin_int_to_ptr(...)
  CodeGen::Label ptrToIntBuiltin;  // AotValue oskel_builtin_ptr_to_int(...)
  CodeGen::Label freeBuiltin;      // AotValue oskel_builtin_free(...)
  // The file builtins (BACKLOG §6 item 25): `readFile`, `writeFile` and
  // `fileExists`, written against the `entry.open`/`readFd`/`writeFd`/`close`
  // handles so the syscall/API difference stays in the prelude. Only the targets
  // whose prelude binds those handles emit them (see `AotRuntimeConfig::hasFiles`).
  CodeGen::Label readFileBuiltin;  // AotValue oskel_builtin_read_file(...)
  CodeGen::Label writeFileBuiltin; // AotValue oskel_builtin_write_file(...)
  CodeGen::Label fileExistsBuiltin;// AotValue oskel_builtin_file_exists(...)
  // `clock()`: the seconds since the process started, as a `double`, read from
  // the `entry.monotonic` handle. The origin is captured once, at entry, into
  // the hidden `$clockOrigin` global, so the first call is not the reference.
  CodeGen::Label clockBuiltin;     // AotValue oskel_builtin_clock(AotValue* args, long argc, AotClosure* self)
  // `args()`: the script arguments as an array of strings, read from the hidden
  // `$args` global the chunk fills at entry from the process command line.
  CodeGen::Label argsBuiltin;      // AotValue oskel_builtin_args(AotValue* args, long argc, AotClosure* self)
  // `void oskel_build_args(long argc, char** argv, long slot)` builds the
  // `$args` array at entry from the vector the prelude handed `main`.
  CodeGen::Label buildArgs;        // void oskel_build_args(long argc, char** argv, long slot)
};

template <std::size_t... I>
inline AotRuntimeHandles makeHandles(CodeGen& code, std::index_sequence<I...>) {
  return AotRuntimeHandles{(static_cast<void>(I), code.newLabel())...};
}

inline AotRuntimeHandles newAotRuntimeHandles(CodeGen& code) {
  // Every field is a `CodeGen::Label`, so the aggregate initialiser takes one
  // label per field. The count comes from the struct, not from a literal: a
  // positional initialiser written by hand fell one short twice, and a short
  // one value-initialises the tail to the default (invalid) label, which only
  // surfaced as "binding an unknown label" at the end of codegen. Producing
  // exactly `kHandles` elements also makes a field of any other type a
  // compile error (too many initialisers) instead of a silent shift.
  static_assert(sizeof(AotRuntimeHandles) % sizeof(CodeGen::Label) == 0);
  constexpr std::size_t kHandles = sizeof(AotRuntimeHandles) / sizeof(CodeGen::Label);
  return makeHandles(code, std::make_index_sequence<kHandles>{});
}

// The selector `oskel_compare` accepts: the low nibble of the x86-64 condition
// code for each bytecode comparison, so the runtime can branch with a plain
// `cmp` + `je` chain. The values match `OpCode::EQ`, `NEQ`, `LT`, `LTE`, `GT`
// and `GTE` one-to-one.
enum class AotCompareOp : std::int64_t {
  Eq = 4,
  Ne = 5,
  Lt = 12,
  Gt = 15,
  Le = 14,
  Ge = 13,
};

// The selector `oskel_bitwise` accepts.
enum class AotBitwiseOp : std::int64_t {
  And = 1,
  Or = 2,
  Xor = 3,
};

// The selector `oskel_arith` accepts: `+`, `-`, `*`, `/` and `%`. `Add` and
// `Sub` also concatenate strings once strings land (7c.1c); for now a
// non-numeric operand is the "unsupported operand types" error.
enum class AotArithOp : std::int64_t {
  Add = 1,
  Sub = 2,
  Mul = 3,
  Div = 4,
  Mod = 5,
};

// The selector `oskel_shift` accepts. `>>` is arithmetic in Oskel (it preserves
// the sign), so the two directions are distinct helpers.
enum class AotShiftOp : std::int64_t {
  Shl = 1,
  Shr = 2,
};

// A scalar `extern "C"` symbol the hosted backend resolves through the system
// linker (PHASE 7f.5, extended to `cstring`/`ptr` in 7f.6).
//
// The compiler's `ExternSymbol` carries the whole ABI signature in the FFI
// vocabulary; the lowerer narrows it to this register-level description -- one
// `ParamClass` per parameter and one `ReturnClass` -- so the runtime emits a
// marshalling thunk without reaching into the compiler's type tables. A scalar,
// `cstring` or `ptr` *parameter* is narrowed (a pointer travels in a GPR); a
// struct or a callback parameter is left out and keeps the build rejection.
// A `cstring` return is narrowed (the thunk scans the `const char *` into an
// `AotString`); an opaque `ptr` return is left out, because the AOT runtime has
// no representation for a handle it must never dereference.
// One flattened parameter, in System V argument order. A scalar is one entry in
// the register class the ABI assigns it; a struct contributes one entry per
// eightbyte, an INTEGER eightbyte travelling as a plain integer and an SSE one
// as a double. `argIndex` is the declared argument the entry came from (several
// entries share it when they are the eightbytes of one struct) and `eightbyte`
// is the entry's position inside that struct (-1 for a scalar). `structIndex`
// names the `structs` entry (-1 for a scalar).
struct AotThunkParam {
  ffi::ParamClass cls = ffi::ParamClass::Int;
  int argIndex = 0;
  int eightbyte = -1;
  int structIndex = -1;
};

// One by-value struct in a thunk's signature: its C layout plus one data-island
// label per field, holding the field's name as a raw `AotString`. The thunk uses
// the names to read a field out of the `AotMap` the program holds and to write a
// returned field back into a fresh one. `ffi::StructLayout` is the layout the
// compiler and the hosted engine share, so no ABI logic lives here.
//
// `bufSlot` is the thunk frame's 16-byte staging buffer for a register-class
// struct (-1 for a class-MEMORY one, which lives in the outgoing stack-argument
// area instead).
struct AotStructLayout {
  ffi::StructLayout layout;
  std::vector<CodeGen::Label> fieldNameLabels;  // parallel to `layout.fields`
  int bufSlot = -1;
};

// A class-MEMORY struct argument: it travels in the outgoing stack-argument
// area at `offset` bytes past the call's `rsp`, in declaration order.
struct AotStackArg {
  int structIndex = -1;
  int argIndex = 0;
  std::int64_t offset = 0;
};

struct AotExternThunk {
  // Bound by `emitAotRuntime` to the thunk's entry, so the caller can seed it
  // into the symbol's global slot before the body is emitted (a forward
  // reference, exactly like a builtin's code label).
  CodeGen::Label label;
  std::string symbol;                  // the C symbol, e.g. "abs"
  // The declared parameters, one per argument (a struct is one entry here, not
  // one per eightbyte): the thunk's arity check counts these.
  std::size_t arity = 0;
  std::vector<AotThunkParam> params;   // flattened to eightbytes, in ABI order
  ffi::ReturnClass returns = ffi::ReturnClass::Int;
  // The declared spellings, read only to truncate the returned integer to its
  // width (a C `int` leaves the upper half of `rax` undefined).
  std::string returnType;
  // The by-value structs the signature needs: one entry per struct parameter or
  // return. Empty for a scalar-only signature.
  std::vector<AotStructLayout> structs;
  int returnStructIndex = -1;  // `structs` entry a struct return rebuilds
  // The class-MEMORY arguments, in declaration order, and the size of the
  // outgoing stack-argument area they occupy (0 when there are none).
  std::vector<AotStackArg> stackArgs;
  std::int64_t stackArgBytes = 0;
  // The size of a class-MEMORY return (0 for none). Such a return does not come
  // back in registers: the caller passes a hidden buffer pointer in `rdi`
  // (`sret`), which also shifts every integer argument one register along.
  std::int64_t returnMemoryBytes = 0;
};

// Appends every runtime helper to `code` and binds `handles`. `entry` comes
// from `entry.h`: the runtime prints through `oskel_write` and reports a fatal
// error through `oskel_exit`, so the caller must emit the entry prelude into
// the same buffer (see `emitFreestandingRuntime`).
//
// `externThunks`, when non-null, is the `extern "C"` surface of the
// hosted mode: one marshalling thunk is emitted per symbol and each thunk's
// `label` is bound. The freestanding path passes null, so its image is
// unchanged and carries no external reference.
//
// `hosted` selects the hosted write path, where the float/double printer must
// keep its formatting frame allocated across the call into libc's `write` (see
// `emitPrintValueCore`). It is false for every freestanding target, whose image
// then stays byte for byte what it was.
//
// `hasInput` is true only for the bare-metal target, the one whose prelude
// binds `entry.read` (the keyboard). It makes `emitAotRuntime` emit the `input`
// builtin body; every other target leaves it out, so its image never references
// the unbound `entry.read` label. `lowerToX64` seeds the `input` global under
// the same condition, so the body and its only caller appear together.
//
// `bareMetal` emits the `memSize()` builtin, which reads the `entry.memSize`
// handle only the bare-metal prelude binds. It is false for every other target,
// so their runtime is unchanged.
//
// `randomStateSlot` is the hidden `$randomState` global's index, or -1 when the
// program never touches `random`/`seed`; the generator and both builtins are
// emitted only when it is set, so a program that does not draw keeps a runtime
// with no generator in it.
//
// `pcallHandlerSlot` is the hidden `$pcallHandler` global's index, or -1 when
// the program never calls `pcall`. When it is set, the error sink checks that
// slot before printing and jumps back into the innermost `pcall` instead (the
// one non-local return in the AOT runtime), and the `pcall` body is emitted.
// A program that never calls `pcall` keeps a sink with no handler check and no
// `pcall` code, so its image is unchanged.
// What the pointer/file/clock/argument builtins (BACKLOG §6 item 25) need from
// the caller, bundled so the argument list stays readable.
//
// The pointer builtins need nothing: a handle lives in the arena, so every
// target emits them. The rest depend on the prelude:
//
//   * `clock()` reads `entry.monotonic` and subtracts the origin the chunk
//     captured into the hidden `$clockOrigin` global (`clockOriginSlot`), so the
//     value is elapsed time and not an absolute counter;
//   * `args()` reads the hidden `$args` global (`argsSlot`) the chunk fills at
//     entry from the `(argc, argv)` its caller handed `main`. Only a prelude that
//     actually passes a vector can back it -- Linux freestanding and the hosted
//     mode do, Windows and bare metal do not -- so `hasArgv` is false there,
//     `argsSlot` stays at -1 and `args` is rejected by name (see `lower.cpp`);
//   * `readFile`/`writeFile`/`fileExists` are written against the
//     `entry.open`/`readFd`/`writeFd`/`close` handles, so `hasFiles` is true only
//     for a prelude that binds them (bare metal has no file system).
//
// A field left at its default keeps the image byte for byte what it was: no
// body is emitted and no label is referenced.
struct AotRuntimeExtras {
  int clockOriginSlot = -1;  // the hidden `$clockOrigin` global, or -1
  int argsSlot = -1;         // the hidden `$args` global, or -1
  bool hasArgv = false;      // the prelude hands the program argc/argv
  bool hasFiles = false;     // the prelude binds the file handles
};

void emitAotRuntime(CodeGen& code, const AotRuntimeHandles& handles,
                    const RuntimeHandles& entry,
                    const std::vector<AotExternThunk>* externThunks = nullptr,
                    bool hosted = false, bool hasInput = false, bool bareMetal = false,
                    int randomStateSlot = -1, int pcallHandlerSlot = -1,
                    const AotRuntimeExtras& extras = AotRuntimeExtras{});
