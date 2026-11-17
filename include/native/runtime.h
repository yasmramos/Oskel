#pragma once

#include <cstdint>

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
  // `$isError` (try/catch) reports whether a value is an error; the AOT runtime
  // has no error values yet, so it is always false and the success path of
  // `try`/`x catch y` runs (a real failure still exits 70).
  CodeGen::Label isErrorBuiltin;   // AotValue oskel_builtin_is_error(AotValue* args, long argc, AotClosure* self)
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
};

inline AotRuntimeHandles newAotRuntimeHandles(CodeGen& code) {
  return AotRuntimeHandles{
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel()
  };
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

// Appends every runtime helper to `code` and binds `handles`. `entry` comes
// from `entry.h`: the runtime prints through `oskel_write` and reports a fatal
// error through `oskel_exit`, so the caller must emit the entry prelude into
// the same buffer (see `emitFreestandingRuntime`).
void emitAotRuntime(CodeGen& code, const AotRuntimeHandles& handles,
                    const RuntimeHandles& entry);
