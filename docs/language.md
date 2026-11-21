# Oskel Language Reference

Language version: **0.1.0**

This guide describes the syntax and semantics of Oskel as implemented by the compiler and the register-based VM included in this repository.

## Implementation status

The language surface this reference documents is implemented on the development engine (`oskel`) in full. The native (AOT) backend and the JIT lag behind it, and the gaps are listed in §16:

| Area | Development engine (`oskel`) | AOT backend (`oskelc` / `oskel build`) |
| --- | --- | --- |
| Language core: functions, closures, control flow, collections | ✅ | ✅ (subset the lowerer covers) |
| Types, nullability, `struct`, `class`, variadics, `import`, `switch`, `defer` (PHASE 6.5/6.8) | ✅ | ✅ (all are compile-time; `struct`/`class` are erased, `switch`/`defer` lower to existing opcodes) |
| `native fn` (PHASE 6.5) | ✅ | 🔶 only for the curated stubs the backend seeds; anything else fails the build (PHASE 7f) |
| FFI with C (PHASE 6.6) | ✅ | 🔶 scalar `extern "C"` in `--mode hosted` only; FFI is hosted-only by design (PHASE 7f, §4.5) |
| Error values, `pcall`, `try`/`catch`, unions, `@MustUse` (PHASE 6.7) | ✅ | ✅ `error` values, `try`/`catch` and `pcall` (PHASE 7c.5c/7c.5d); a raised error with no active `pcall` exits 70 |
| JIT (PHASE 8) | 🔶 integer leaf protos (slices 1-4) | — (never) |

## Table of contents

1. [Execution model](#1-execution-model)
2. [Lexical structure](#2-lexical-structure)
3. [Data types](#3-data-types)
4. [Variables and scope](#4-variables-and-scope)
   - [4.6 Type annotations](#46-type-annotations)
   - [4.7 `struct`](#47-struct)
   - [4.8 `class`](#48-class)
5. [Operators](#5-operators)
6. [Statements](#6-statements)
7. [Functions and closures](#7-functions-and-closures)
   - [7.6 `native fn`](#76-native-fn)
   - [7.7 `@MustUse`](#77-mustuse)
8. [Arrays](#8-arrays)
9. [Strings](#9-strings)
10. [Maps](#10-maps)
11. [Ranges](#11-ranges)
12. [Standard library](#12-standard-library)
13. [Errors and diagnostics](#13-errors-and-diagnostics)
    - [13.3 Propagating and recovering: `try` and `catch`](#133-propagating-and-recovering-try-and-catch)
14. [REPL](#14-repl)
15. [Command line](#15-command-line)
16. [Known limitations](#16-known-limitations)
17. [Complete example](#17-complete-example)
18. [Modules and `import`](#18-modules-and-import)
19. [File extensions](#19-file-extensions)

---

A Oskel program is a UTF-8 text file (extension `.oskl`). The pipeline is:

1. **Lexer**: turns the text into tokens.
2. **Parser**: builds an AST with a recursive-descent parser and operator precedence.
3. **Compiler**: translates the AST to bytecode (39 opcodes today, register-based, under a hard budget of 40).
4. **VM**: executes the bytecode.

The AST is never executed: the `oskel` executable compiles source to bytecode and runs it on the register-based VM. The compiler and the VM implement the whole language surface this reference documents — functions, closures and control flow, collections, the type system with nullability, `struct` and `class` (PHASE 6.5/6.8), FFI with C (PHASE 6.6) and error values with `pcall`, `try`/`catch`, unions and `@MustUse` (PHASE 6.7). The AOT backend (PHASE 7) compiles a subset of the same programs to a native binary through `oskelc` (also reachable as `oskel build`); §16 lists the constructs it does not yet lower.

The program runs from top to bottom in the global scope. Simple statements end with `;`. Statements with a block (`if`, `while`, `for`, `fn`) close with `}` and optionally accept a `;` after it.

```oskel
let a = 1;
if (a == 1) {
  print("one");
}; // the ';' is optional after the block
```

A file may start with a `#!...` line (shebang); the lexer ignores it.

---

## 2. Lexical structure

### 2.1 Comments

```oskel
// Line comment

/* Block comment.
   /* Block comments can be nested. */
*/
```

`//` always starts a comment; there is no integer division operator.

### 2.2 Identifiers and reserved words

Identifiers start with an ASCII letter or `_`, and continue with ASCII letters, digits or `_`. They are case-sensitive.

Reserved words:

```text
break  case   catch  class  const  continue  default  defer  do     else  false  fn  for  if
import in     let    native nil       return   struct switch true  try  var
while
```

They cannot be used as names of variables, parameters or unquoted map keys.

The convention is `camelCase` for variables, functions and methods, and the standard library follows it (`startsWith`, `readFile`, `assertEq`).

### 2.3 Numbers

| Form | Example | Value |
| --- | --- | --- |
| Decimal | `123` | `123` |
| Separators | `1_000_000` | `1000000` |
| Hexadecimal | `0x2A` | `42` |
| Binary | `0b101010` | `42` |
| Octal | `0o52` | `42` |
| Double | `1.5`, `0.25` | `1.5`, `0.25` (binary64) |
| Float | `1.5f`, `0.25F` | `1.5`, `0.25` (binary32) |
| Scientific notation | `1.5e3`, `2E-2` | `1500.0`, `0.02` |

- Integers are 64-bit signed. An out-of-range literal is a compile error.
- The `-` sign is a unary operator, not part of the literal. `-9223372036854775808`
  is therefore the one literal whose magnitude (2^63) does not fit a positive
  `int`: the lexer reads the digits as unsigned, and the parser accepts the value
  only directly under a unary minus. Written bare, in any base
  (`9223372036854775808`, `0x8000000000000000`), it is the usual out-of-range
  error.
- A dot is only part of a float when followed by a digit: `1.` is not a valid float; write `1.0`.
- Underscores are ignored inside digits: `1_0` is equivalent to `10`.
- A fractional literal with no suffix is a `double` (binary64); a trailing
  `f`/`F` makes it a `float` (binary32), like Java. The suffix also turns an
  integer-looking literal into a float: `1f` is a `float`.
- A floating-point literal is a compile error only when it is outside the range
  of its type: `1e400` (overflows to infinity) and `1e-400` (underflows to
  zero) are rejected, as is `1e40f`, which is a valid `double` but overflows
  binary32. A representable subnormal is a valid literal, so `4.9e-324` (the
  smallest `double`) and `1e-45f` (the smallest `float`) are accepted.

### 2.4 Strings

Strings are written between double quotes. There are no single quotes or raw strings. They may span several lines:

```oskel
let greeting = "Hello";
let text = "first line
second line";
```

Escape sequences:

| Escape | Meaning |
| --- | --- |
| `\n` | Newline |
| `\t` | Tab |
| `\r` | Carriage return |
| `\0` | Null byte |
| `\\` | Backslash |
| `\"` | Double quote |
| `\$` | Literal dollar sign |
| `\xNN` | Hexadecimal byte (two digits) |
| `\uXXXX` | Unicode code point (four digits, not a lone surrogate) |

A `\uXXXX` escape must name a Unicode scalar value, so the surrogate range
`U+D800`..`U+DFFF` is a compile error. Surrogate pairs are not supported; write
the four-digit escape of the code point itself (`\uXXXX` for `U+0000`..`U+FFFF`).

### 2.5 Interpolation

Inside a string, `${expression}` evaluates an expression and converts it to text:

```oskel
let name = "Ada";
let age = 36;
print("${name} is ${age} years old");
print("2 + 3 = ${2 + 3}");
print("result: ${ {a: 1}.a }");
```

Interpolated expressions may contain strings and nested braces:

```oskel
print("nested: ${"inner ${1 + 1}"}");
```

To write a literal `${`, escape the dollar sign:

```oskel
print("\${not evaluated}"); // ${not evaluated}
```

Strings are UTF-8 and **immutable**. Length and index operations work on code points, not bytes.

---

## 3. Data types

| Type | Values | Notes |
| --- | --- | --- |
| `nil` | `nil` | Absence of a value. |
| `bool` | `true`, `false` | Logical values. |
| `int` | 64-bit signed integers. | Arithmetic wraps around on overflow. |
| `float` | IEEE 754 single-precision floating-point numbers (binary32). | Literal with an `f`/`F` suffix. Can be `inf`, `-inf` or `nan`. |
| `double` | IEEE 754 double-precision floating-point numbers (binary64). | Default for a fractional literal. Can be `inf`, `-inf` or `nan`. |
| `string` | UTF-8 text. | Immutable. |
| `error` | Error values built by `error(message, code?)`. | Immutable. Carries a `message` and an integer `code` (see §13.2). |
| `array` | Ordered sequence of values. | Mutable and by reference. |
| `map` | Collection of string keys. | Preserves insertion order. Mutable and by reference. |
| `function` | Native or user-defined functions. | First-class citizens. |
| `range` | Interval of integers. | Immutable. |
| `void` | (no values) | Return type only: a function that produces no value (see §4.6). Maps to C's `void` in the FFI. |

The `type(value)` function returns the name of the type as a string.

### 3.1 Truthiness

Only two values are falsy:

```oskel
false
nil
```

Everything else is truthy, including `0`, `0.0`, `""`, `[]` and `{}`.

```oskel
if (0) {
  print("0 is truthy in Oskel");
}
```

### 3.2 Equality

- `int`, `float` and `double` are compared numerically: `1 == 1.0` and `1.5f == 1.5` are `true`.
- Strings are compared by content.
- Errors are compared by content (message and code).
- Arrays and maps are compared **structurally**, in depth.
- Ranges compare start, end and inclusiveness.
- Functions are compared by identity (the very same function).
- Values of different types (except the numeric types) are not equal: `0 == false` is `false`.

Structural equality terminates even when the values are cyclic. A container
compared against itself is equal, and two self-referential containers are equal
when their contents match once the cycle closes, so a value that contains
itself can be compared safely:

```oskel
let a = [1];
a.push(a);
let b = [1];
b.push(b);
assertEq(a == b, true);
assertEq(a == a, true);
```

### 3.3 Mutability and references

Assigning an array or a map to another variable **does not copy it**: both variables point to the same value.

```oskel
let a = [1];
let b = a;
b.push(2);
print(a); // [1, 2]
```

For a shallow copy use `.clone()`. Strings are immutable, so they behave like values.


---

## 4. Variables and scope

### 4.1 Declaration

```oskel
var counter;           // mutable, initialized to nil
var name = "Oskel";     // mutable binding, initializer optional
let LIMIT = 10;        // immutable binding evaluated at runtime
const MAX = 10 * 2;    // immutable binding folded at compile time
```

| Keyword | Reassignable | Initializer required | Evaluated |
| --- | --- | --- | --- |
| `var` | yes | no (defaults to `nil`) | runtime |
| `let` | no | yes | runtime |
| `const` | no | yes | compile time |

An optional **type annotation** may follow the name: `let x: int = 5`. See
[Type annotations](#46-type-annotations) below.

`var` declares a mutable variable. `let` declares an immutable binding: assigning another value to it later is rejected at compile time by the bytecode compiler. Use `let` by default for values that never change, and `var` only when reassignment is needed.

Immutability is also enforced by the engine, not only by the compiler: `SETGLOBAL` carries the write mode (a fresh declaration versus a later assignment), so the VM rejects an assignment to a `let`/`const` binding at run time. This closes the gap the compiler cannot see — a hand-built `.jolc` chunk, or an assignment that comes from a *different* compilation unit (a module body or a `:load`ed file) that mutates a `let` declared by the main program. Such an assignment is a runtime error (`cannot assign to immutable variable 'x'`) instead of silently rebinding the value.

`const` is also immutable, but its value must be a compile-time constant expression: literals, arithmetic (`+ - * / %`), unary minus, comparisons and boolean operators over other constants, and references to other top-level `const` names (including forward references, which the parser resolves to a fixed point). Top-level `const` declarations are hoisted to the front of the program (in source order), so any statement — including one written before the declaration — may reference the constant. The parser folds each `const` initializer to a literal before the program runs, so uses of the name cost nothing at runtime. If an initializer cannot be folded (for example it calls a function or reads a `var`), the parser reports an error instead of silently falling back to runtime evaluation. Nested blocks fold their `const`s eagerly as well.

An integer `const` initializer whose folded value does not fit an `int` (`+ - *` or `**`) still wraps around — the same result the expression would produce at run time — but the parser prints a **warning** naming the operator, so a value that is almost always a mistake does not pass unnoticed. It is a warning and not an error because the program is valid and would run. A shift that drops high bits (`1 << 63`) is not an arithmetic overflow and does not warn.

### 4.2 Block scope

Every block `{ ... }` creates a new scope. Variables are resolved from the current scope up through the parents.

```oskel
let x = 1;
{
  let x = 2;   // shadows the outer x
  print(x);    // 2
}
print(x);      // 1
```

Redeclaring a name in the same block replaces the previous definition.

### 4.3 Assignment

Assignment looks the variable up in the current scope and, if it does not exist, in the parent scopes. It does not create new variables:

```oskel
let x = 1;
{
  x = 2; // modifies the outer x
}
print(x); // 2

y = 3; // error: undefined variable 'y'
```

### 4.4 Function declarations

`fn name(...) { ... }` declarations are hoisted within their block, so they can be called before the line where they appear. This allows recursion and mutual recursion.

`var` and `let` declarations are **not** hoisted: using a variable before its declaration produces an error.

---

### 4.6 Type annotations

Oskel is dynamically typed, but a declaration, a parameter or a return value may
carry an optional type annotation. Annotations are **compile-time metadata**:
the compiler checks them and then erases them, so the bytecode is identical
with or without them and the VM never sees a type.

```oskel
let count: int = 5;
let ratio: float = 0.5f;
let precise: double = 0.5;
let title: string = "Oskel";
let ok: bool = true;
let items: array = [1, 2, 3];
let table: map = {a: 1};
let span: range = 0..10;
let failure: error = error("not found", 404);

fn add(a: int, b: int): int {
  return a + b;
}
```

The accepted type names are the primitives reported by `type()`: `nil`, `bool`,
`int`, `float`, `double`, `string`, `error`, `array`, `map`, `function` and
`range`, plus `any`, plus the name of a `struct` (§4.7) or a `class` (§4.8)
declared in the file. A name that is not one of these is a compile error.
`void` is accepted **only as a return type** (see below).

**`void` return type.** A function may be declared `: void` to say it produces
no value. This maps directly to C's `void` in the FFI (PHASE 6.6), so a native
reads like its C declaration: `native fn free(p): void;`. `void` is
compile-time metadata: the function still returns `nil` at run time, so no
opcode and no runtime type is added.

```oskel
fn logLine(msg: string): void {
  print(msg);
}

fn maybeLog(flag: bool): void {
  if (!flag) {
    return;        // bare return: ok
  }
  print("logged");
}
```

`void` is a return type only: it cannot annotate a variable, a parameter or a
struct field. A `void` function cannot `return` a value (use a bare `return;`),
and a `void` expression cannot be used where a value is expected. A binding
initialized from a `void` call stays untyped, like `var x = nil`. Because an
expression body (`fn f(): void => g();`) is an implicit `return`, a `void`
function uses a block body.

**FFI scalar types.** To write a C signature the way C writes it, the FFI
vocabulary (PHASE 6.6) adds pointers and fixed-width/unsigned integers:

| Oskel | C | Notes |
| --- | --- | --- |
| `ptr` | `void *` | Opaque handle. `nil` is NULL. |
| `cstring` | `const char *` | NUL-terminated byte string. `nil` is NULL. Upcasts to `ptr`. |
| `int8` `int16` `int32` `int64` | `int8_t` … `int64_t` | Signed, fixed width. |
| `uint8` `uint16` `uint32` `uint64` | `uint8_t` … `uint64_t` | Unsigned, fixed width. |
| `uint` | `unsigned int` | Unsigned. |
| `size_t` | `size_t` | Unsigned size. |
| `fn(P...): R` | `R (*)(P...)` | C function pointer (callback). Only scalar types in `P`; `R` is a scalar or a struct by value. |

These are **compile-time annotations**: the integer family is erased before the
bytecode (the runtime value is still an `int`) and `type()` keeps reporting the
primitive name. Pointers are the exception: `ptr` and `cstring` are a distinct
runtime value (see **Pointer values** below), because a pointer has to be
recognisable at run time to be marshalled safely. The width and signedness of
the integers are recorded for the FFI marshalling to honour, not for the VM.

```oskel
native fn strlen(s: cstring): size_t;
native fn memcmp(a: ptr, b: ptr, n: size_t): int32;
native fn malloc(n: size_t): ptr;
native fn free(p: ptr): void;

let empty: ptr = nil;      // nil is NULL
```

`nil` is assignable to any pointer (NULL), even without a `?`. A `cstring` is
also a `ptr` (an upcast to `void*`); the reverse needs an explicit cast, so it
is not implicit. The integer family (`int`, the fixed-width types, `uint`,
`size_t`) is mutually assignable at compile time and widens to `float`/`double`,
following the same numeric rule as `int`.

A C function's `const char *` **parameter** may also be spelled `string`: a Oskel
string is NUL-terminated and stays alive for the whole call, so its bytes are a
valid `const char *` (`extern fn strlen(s: string): int;`). A `string` is not a
valid *return* type — a C function that returns text returns a `cstring`.

**Pointer values.** A `ptr`/`cstring` is an **opaque handle**, never a machine
address: the program can pass it to a native and read it back, but it can never
dereference an arbitrary location. This keeps Oskel memory-safe and keeps the
allocation visible to the cycle collector. `type()` reports `ptr` for a `void *`
and `cstring` for a C string, and a `cstring` upcasts to `ptr` without losing
its kind. Pointers compare by identity (like functions), not by their contents.

The development engine owns the storage through the handle and exposes it with
these builtins:

| Builtin | Meaning |
| --- | --- |
| `cstring(s)` | Builds a `cstring` from a Oskel string (bytes plus a NUL). |
| `cstr(p)` | Reads a `cstring`/buffer back as a Oskel string. A `cstring` is NUL-terminated, so the read stops at the first `\0` (an embedded NUL truncates, like C); a buffer is read whole. |
| `buffer(n)` | Allocates `n` zero-initialised bytes and returns a `ptr`. |
| `byteLen(p)` | Bytes the handle owns (`cstring` excludes the NUL; `void *` is 0). |
| `loadByte(p, i)` | Byte `0..255` at index `i` (bounds-checked). |
| `storeByte(p, i, b)` | Writes a byte into a buffer (`cstring` is read-only). |
| `intToPtr(n)` / `ptrToInt(p)` | Wraps and reads the opaque integer token a native returns. |
| `free(p)` | Releases the handle's storage; a later use is a runtime error. |

```oskel
let greeting = cstring("hi");
print(cstr(greeting));      // hi
print(byteLen(greeting));   // 2

let buf = buffer(4);
storeByte(buf, 0, 65);      // 'A'
print(loadByte(buf, 0));    // 65
free(buf);
```

The AOT backend (PHASE 7) is to lower the same handles to real C pointers, but
that is not implemented yet (PHASE 7f): a program that passes a pointer to a
native does not compile with `oskel build` today.

**FFI is hosted-only under `oskel build`.** The decision (PHASE 7f) is that a
program using `extern "C"` compiles with `oskel build --mode hosted`, which
links it against the host C library so the symbol resolves to the real C
function; the freestanding mode, which is the default, has no host to resolve a
symbol against and reports it (exit 65) instead of emitting a broken binary.
This is deliberate, not a gap to paper over: the freestanding image is
self-contained by construction, and a general `extern "C"` call is exactly the
thing that breaks that property. So the rule is simple — **if a program uses
FFI, build it hosted.** The development engine (`oskel`) is unaffected: it
resolves the symbol through `Platform::lookupSymbol` and runs the same program
either way. When the symbol lives in a third-party library, name it with
`--link-lib` (`--link-lib SDL3` becomes `-lSDL3`); libc and libm are already
linked.

**`extern "C"` blocks.** A C function is declared with `extern` (PHASE 6.6).
Three spellings are accepted and compile to the same thing:

```oskel
extern "C" { fn sqrt(x: float): float; }   // block: explicit ABI
extern fn sqrt(x: float): float;           // single: implicit "C" ABI
extern "C" fn sqrt(x: float): float;       // single: explicit ABI
```

The single-line forms are sugar for a one-entry block. The block's ABI is a
plain string literal and only `"C"` is supported.
Each entry is a bodyless `fn ...;`; the compiler validates the signature against
the FFI type vocabulary (see §4.6) and records it as compile-time metadata on
the proto, so a declaration emits no bytecode of its own. Declarations are
hoisted, so a function may call one declared later. Like `import`, `struct` and
`native fn`, they are module-level metadata: nesting one is a compile error.

Before a proto runs, the engine resolves each declared name through
`Platform::lookupSymbol` and binds it to a marshalling native. On POSIX the
search is `dlsym(RTLD_DEFAULT)`, which reaches libc, libm and the host program;
on Windows it walks the process image, `kernel32.dll` (the Win32 base API:
`GetTickCount`, `Sleep`, ...) and the C runtime, so a declaration can name a CRT
function or a Win32 one. The call passes its arguments and reads its result
across the System V AMD64 ABI without
`libffi`: integers, bools and pointers travel in general-purpose registers and
`float`/`double` in SSE registers, and the result is read back at the exact
width the declaration named (so a C `int` is not read as a 64-bit value). A
symbol the host cannot resolve is a clear runtime error, never a wild call.

A C symbol is bound to a single marshalling thunk, so two declarations of the
same name must agree on their signature. A second declaration with a different
signature is a compile error rather than a silent last-wins; the check spans the
whole program, so a conflict between two imported modules is caught too.

```oskel
extern "C" { fn abs(x: int): int; }
extern "C" { fn abs(x: double): double; }  // error: two different signatures
```

```oskel
extern "C" {
  fn strlen(s: cstring): size_t;
  fn sqrt(x: double): double;
  fn free(p: ptr): void;
}

print(sqrt(16.0));         // 4
let n: size_t = strlen("hi");
```

A Oskel `string` is a valid `cstring` argument (`strlen("hi")`), and a parameter
may be spelled `string` directly: a Oskel string is NUL-terminated and stays alive
for the whole call, so its bytes are a valid `const char *`. A `string` is not a
valid *return* type; a C function returns a `cstring`. A `ptr` argument accepts
`nil` (NULL) and an owned `cstring`/buffer, whose bytes are the address the C
callee receives.

**Structs by value.** A `struct` (see §4.7) can also cross the C boundary, both
as a parameter and as a return value. A struct is a map at runtime, so the
compiler records its field layout from the declaration and the marshalling
moves the bytes across the ABI in registers:

```oskel
struct DivResult {
  quot: int32;
  rem: int32;
}
extern "C" {
  fn div(numerator: int, denominator: int): DivResult;
}

let d = div(17, 5);
print(d.quot);   // 3
print(d.rem);    // 2
```

The ABI classifies a small struct as up to two **eightbytes**, each INTEGER (a
general-purpose register) or SSE (an xmm register); a `{int; int}` struct comes
back wholly in `rax`, a `{double; double}` in `xmm0`/`xmm1`, and a mixed
`{int; double}` in `rax` + `xmm0`. A struct argument is flattened to the same
eightbytes and passed in the registers the ABI assigns it, so the callee reads
back the exact bytes of the struct.

A struct of **more than 16 bytes** is class MEMORY: it is passed in the outgoing
stack-argument area and returned through a hidden `sret` buffer the caller
supplies in `rdi` (which shifts every integer argument one register along). The
development engine expresses both directions through `libffi`, so a class-MEMORY
struct needs libffi and the System V AMD64 ABI; elsewhere the declaration is
rejected at compile time with a clear message instead of a wrong call. A struct
field must be an FFI scalar type (an integer, `float`/`double`, `ptr`/`cstring`
or `bool`); a nested struct field is rejected.

Structs by value are **System V AMD64 only**. The register classification above
is the System V ABI's; the Microsoft x64 ABI (Windows) passes a struct larger
than 8 bytes through a hidden pointer instead, a shape the register thunks
cannot express. On such a target the declaration is rejected at compile time
with `a struct cannot cross the C boundary by value on this target`. Scalar
arguments and results stay portable: the call goes through a typed function
pointer, so the host compiler applies whichever ABI it targets.

**Callbacks.** A parameter typed `fn(paramTypes): returnType` is a C function
pointer. A Oskel function (or a closure) passed for it is callable from C, and a
callback signature only carries scalar FFI types:

```oskel
extern "C" {
  fn qsort(base: ptr, nmemb: size_t, size: size_t, compar: fn(ptr, ptr): int): void;
  fn memcmp(a: ptr, b: ptr, n: size_t): int;
}

fn ascending(a: ptr, b: ptr): int {
  return memcmp(a, b, 1);
}

let data = buffer(4);
storeByte(data, 0, 50);
storeByte(data, 1, 10);
storeByte(data, 2, 90);
storeByte(data, 3, 30);
qsort(data, 4, 1, ascending);
```

In the development engine the callback is a `libffi` closure: the trampoline
re-enters the VM with the C arguments boxed as Oskel values and writes the Oskel
result back in the declared C type. The callback runs while the C function is on
the stack, so the trampoline is alive for the whole call; storing the function
pointer for a later call is out of scope in the development engine. The AOT
backend's stub and context live in the arena, so a callback C stores for a later
call -- including one invoked from an `atexit` handler after the chunk returns
-- works there (BACKLOG §6). The executable stub page is W^X (read+execute,
never writable and executable) and lives until the process exits. `nil` is the
NULL function pointer.

A callback can only be a **parameter**; a C function that returns a function
pointer is not supported. A callback may return a **struct by value** (its
fields are scalars, like a struct parameter): the trampoline serialises the map
the Oskel function returns into the C return storage using the struct's layout,
so C reads the fields back through the registers the ABI assigns. A struct
larger than 16 bytes (class MEMORY) is returned through the hidden `sret`
pointer, which needs the same `libffi`-backed path as a large struct argument.
An error raised inside a callback cannot unwind
through the C frame, so the extern call captures it and re-raises it once C
returns: `pcall` catches it like any other runtime error. A struct the callback
returns that does not match the declared shape (not a map, a missing field, a
field of the wrong type) fails the same way. The AOT backend (PHASE 7f.8)
lowers callbacks to generated stubs and never links `libffi`; it supports every
scalar parameter and return class, but rejects a callback that returns a struct
by value and a callback with a struct parameter with a dedicated build error.

**C variadics.** A trailing bare `...` declares a C variadic function: the named
parameters are the fixed prefix and every extra argument is passed through the
default argument promotions.

```oskel
extern "C" {
  fn snprintf(buf: cstring, size: size_t, fmt: cstring, ...): int;
}

let buf = cstring("          ");
snprintf(buf, 10, "%s-%d", "id", 7);   // "id-7"
```

The `...` must be the last parameter. The spelling is a *bare* `...`, told apart
from the Oskel parameter pack `rest...` (`fn sum(first, rest...)`), which
collects the extra arguments into an array and cannot cross the C boundary — a
`rest...` in an `extern` declaration is a compile error. A variadic call applies
the C default promotions: a `float` argument is passed as a `double`, a `bool`
as an `int`, and a string or pointer as a `const char *`. The compiler checks
only that the fixed prefix is present and that the earlier arguments match; the
trailing arguments are **not** type-checked, because C promotes and consumes
them at run time, so a wrong format is the program's responsibility.

A variadic call always goes through `libffi` (the argument count changes per
call, so the call interface is prepared each time with `ffi_prep_cif_var`). Like
a class-MEMORY struct, a variadic declaration therefore needs `libffi` and the
System V AMD64 ABI; on any other target it is rejected at compile time with a
clear message. The return type must be a scalar. A declaration that is variadic
and one that is not are different C types, so declaring one symbol both ways is
a compile error (see above).

**Scope of the boundary.** A C type crosses the boundary when Oskel has a value
that spells it: integers (including a C enum, which *is* an `int`), `float`/
`double`, `bool`, pointers (`ptr`/`cstring`/`string`), a `struct` of scalar
fields, and a `fn(...)` callback. The C types with no Oskel value — `long
double`, `__int128`, an array by value (it decays to a pointer at the ABI, so
`ptr` spells it), a union, a bitfield, and a callback that itself takes a
callback — are out of scope: a declaration that uses one is rejected at compile
time with a clear message instead of a wrong call. See `docs/BACKLOG.md` §7 for
the reason for each.

**Local inference.** With no annotation, the compiler infers the type from the
initializer when it is a literal or another typed binding, and then checks
later assignments against it:

```oskel
var n = 0;
n = 1;        // ok
n = "text";   // compile error: cannot assign 'string' to 'int' in 'n'
```

A `nil` initializer does **not** pin a type (`var x = nil;` stays untyped), and
an expression whose type is not evident is left unchecked, so inference never
rejects a program that would have run.

**Widening.** Numeric widening follows Java: `int` → `float` → `double`. The
reverse is not implicit; use an explicit conversion.

```oskel
let a: float = 3;      // ok: int widens to float
let b: double = 3;     // ok: int widens to double
let c: double = 1.5f;  // ok: float widens to double
let d: float = 1.5;    // compile error: double does not narrow to float
let e: int = 1.5;      // compile error
```

**Nullability.** A trailing `?` marks a type that also accepts `nil`. Without
it, `nil` is rejected.

```oskel
let maybe: string? = nil;   // ok
let name: string = nil;     // compile error
```

**Null-safe expressions.** Three operators work on nullable values:

| Operator | Form | Result |
| --- | --- | --- |
| Null coalescing | `a ?? b` | `a` unless it is `nil`, in which case `b` |
| Safe access | `a?.b` | `nil` when `a` is `nil`, otherwise `a.b` |
| Non-null assertion | `a!!` | `a`, or a runtime error when `a` is `nil` |

```oskel
let a: string? = nil;
print(a ?? "default");   // "default"
let b: string? = "hi";
print(b ?? "default");   // "hi"

let m: map? = nil;
print(m?.length);        // nil
let m2: map? = {k: 1};
print(m2?.k);            // 1

let s: string? = "ok";
print(s!!);              // "ok"
let t: string? = nil;
print(t!!);              // runtime error
```

Only `nil` triggers the `??` fallback and the `?.` guard: `0`, `false` and `""`
are kept, so this is not truthiness. `??` short-circuits, so its right-hand side
is evaluated only when the left is `nil`. `a?.b` is read-only and cannot be an
assignment target.

These operators add no opcode: `??` and `?.` lower to `EQ`/`JMP_IF_*`/`GETTABLE`
and `!!` to a call to a hidden helper, so the bytecode budget is unchanged
(DEVELOPMENT.md §3).

**Union types.** A type may be a union of alternatives, written with `|`. A
value of the union's type may be any one of the alternatives, and the
annotation is checked like every other type and then erased, so it adds no
opcode.

```oskel
let value: int | string = 1;
value = "text";                 // the other alternative is accepted too
let result: int | error = try parse(input);
```

`nil` is not written as a union member: `int | nil` is the same type as `int?`,
so `nil` folds into the nullability of the whole annotation. That keeps the two
spellings interchangeable, and a `?` alone means exactly the same thing.

```oskel
let a: int | nil = nil;     // same type as `int?`
let b: int | string | nil = nil;
```

Assignment follows the same rule as a single type, applied to each alternative:

- A value fits the union when it fits **at least one** alternative. `int`
  widens to `float`, so `let x: float | string = 1;` is accepted.
- A union source fits the target only when **every** member fits, so a union is
  assignable to a wider union that contains all of its members.
- `nil` needs the union to be nullable (written `| nil` or with a `?`).

```oskel
let x: int | string = 1;                 // ok
let y: float | string = 1;               // ok: int widens to float
let wide: int | string | bool = x;       // ok: every member fits
let bad: int | string = true;            // compile error: bool fits neither
let narrow: int = x;                     // compile error: a union is not one type
```

A union is a compile-time-only construct with **no C representation**, so it is
rejected wherever a type must cross the FFI boundary: `extern "C"` signatures,
`native fn` signatures and struct fields (a struct can be passed by value).
`void` is a return-only type, so it cannot be a union member either.

`catch` produces a union: `e catch d` is either `e` or `d`, so its inferred
type is the union of the two. That is what lets a fallback of a different type
type-check without a cast.

```oskel
fn parse(s: string): int | error { ... }
let recovered = parse(input) catch 0;    // int
```

---

### 4.7 `struct`

A `struct` is a named, data-only shape: a fixed set of fields with optional
type annotations. It has no methods and no inheritance. A `class` (§4.8) is the
method-carrying form: it is sugar over the same map shape plus closures, so a
`struct` remains the lighter choice when no behaviour is attached. Like
`import`, a `struct` is
**module-level metadata**: it may only be declared at the top level of a file,
and the declaration itself emits no bytecode.

```oskel
struct Point {
  x: int;
  y: int;
}

let p = Point { x: 1, y: 2 };
print(p.x, p.y);   // 1 2
```

A struct value **is a map at runtime**, so everything that works on a map works
on a struct: member access, `in`, `len`, `for ... in`, indexing, and structural
equality. `Point { x: 1, y: 2 }` lowers to the same `NEWTABLE`/`SETTABLE`
sequence as `{x: 1, y: 2}`, so `struct` adds no opcode and the register VM and
the future AOT backend never see a struct type. `type(p)` reports `"map"`.

**Field order does not matter** in a literal, but every declared field must be
present exactly once. The compiler rejects an unknown field (a typo), a missing
field (a key the declaration promises), a duplicate field and a duplicate
struct name. A field annotation is optional; when present, the value is checked
with the same rules as every other annotation (including `int` → `float` →
`double` widening).

A field may be written in **shorthand**: `Point { x, y }` is sugar for
`Point { x: x, y: y }`, taking each value from the variable of the same name in
scope (like Go, JavaScript and Rust). The shorthand and the `name: value` form
mix freely in one literal, and a shorthand value is an ordinary expression, so
it is type-checked exactly like the explicit form.

```oskel
let x = 1;
let y = 2;
let p = Point { x, y };        // Point { x: 1, y: 2 }
let q = Point { x, y: 5 };     // Point { x: 1, y: 5 }
```

```oskel
struct Pair { first; second; }          // untyped fields
let a = Pair { first: 1, second: "x" }; // ok: any value fits

struct Vec { x: float; y: float; }
let v = Vec { x: 1, y: 2.5 };           // ok: int widens to float

let bad1 = Vec { x: 1 };                // error: missing field 'y'
let bad2 = Vec { x: 1, z: 2 };          // error: unknown field 'z'
let bad3 = Vec { x: 1, y: "no" };       // error: cannot assign 'string' to 'float'
```

A struct name is a valid type annotation, and — like a hoisted `fn` — a struct
declared later in the file is visible earlier:

```oskel
fn origin(): Point {
  return Point { x: 0, y: 0 };
}

struct Point { x: int; y: int; }   // visible to `origin`, declared above
```

Because `struct` is a reserved word, the sequence `struct Name` is always a
declaration, so `Name { ... }` is unambiguously a struct literal whenever
`Name` is a struct declared in the file (the literal form takes precedence over
reading `Name` as a variable).

A struct is local to its file: its name is **not** exported by `import`. A
module that builds a struct value exports the map, and the importing file sees
a plain map (it cannot write `Point { ... }` unless it declares its own
`struct Point`). A module that wants to hand out its struct values to importers
therefore exports **constructor functions** rather than the type name:
`std.datetime` (§12.10) declares `struct Date`/`struct DateTime` and exports
`date`/`datetime`, which return the maps.

### 4.8 `class`

A `class` is a named shape **with methods**. It is deliberately the smallest
step beyond `struct`, and it is **pure sugar**: the parser rewrites the
declaration into an ordinary top-level constructor function, so a class adds no
AST node, no opcode and no engine code, and the VM and the AOT backend run it
identically (see [§16](#16-known-limitations) for the parity guarantee).

```oskel
class Point {
  x: int;
  y: int;

  fn init(x: int, y: int) {
    self.x = x;
    self.y = y;
  }

  fn lenSquared(): int {
    return self.x * self.x + self.y * self.y;
  }
}

let p = Point(3, 4);        // no `new`
print(p.x, p.y);            // 3 4
print(p.lenSquared());      // 25
```

The design has three hard rules:

- **Instantiation is calling the class name.** There is no `new`, no `Point::new`
  and no separate constructor syntax: `Point(3, 4)` builds an instance. The
  class name is a value of type `function` (it *is* the constructor), so it can
  be passed around and stored like any other function.
- **`self` is explicit.** A method reaches its receiver through `self`, exactly
  as a plain function reaches a parameter; there is no implicit `this`. The name
  `self` is reserved inside a class body (it cannot be a field, a method or a
  parameter).
- **No inheritance.** There is no `extends`, no `super` and no base class. A
  class is a flat shape; composition (a field that holds another class's
  instance) is how one class reuses another.

**The desugaring.** The class above compiles to the equivalent of:

```oskel
fn Point(x: int, y: int) {
  let self = { x: nil, y: nil };
  self.init = (x: int, y: int) => {
    self.x = x;
    self.y = y;
  };
  self.lenSquared = () => {
    return self.x * self.x + self.y * self.y;
  };
  self.init(x, y);   // forward the constructor arguments
  return self;
}
```

Every field becomes a key of the instance map (each starting `nil`), every
method becomes a closure assigned to that key, and `self` is the local that
those closures capture. A method's declared return type rides on the desugared
lambda, so the body's `return` is still checked even though a lambda the user
writes has no return-annotation position. An instance **is a map at runtime**, so
the whole map surface applies: member access, `in`, `len`, `for ... in`,
indexing, `type(p)` reports `"map"`, and instances nest and live in collections.

`init` is optional and defines the constructor's parameters: the arguments the
caller writes after the class name are forwarded to `init`, so the arity of
`Point(...)` is the arity `init` declares. Without `init` the fields keep their
initial `nil` and the constructor takes no arguments:

```oskel
class Counter {
  n: int;
  fn set(v: int) { self.n = v; }
  fn get(): int { return self.n; }
}
let c = Counter();   // no init: the fields start nil
c.set(41);
print(c.get());      // 41
```

A method returns with `return` like any function; `return self;` makes the call
chainable (`p.scaled(2).scaled(2)`). A method may call another through `self`
(`self.bump()`) and may recurse through `self`.

**Field defaults.** A field may carry a default (`n: int = 5;`). The desugaring
seeds the instance map with it instead of `nil`, so a constant field needs no
line in `init`:

```oskel
class Config {
  port: int = 8080;
  host: string = "localhost";
  fn init() {}
}
print(Config().port);   // 8080
```

A default is evaluated per instance, so mutating one instance never leaks into
the next. `init` still runs after the defaults and may override them.

**Every non-nullable annotated field must end up set.** When a class declares
`init`, the parser checks that every field whose annotation is present and not
nullable is either given a default or assigned as `self.<field> = ...` somewhere
in `init`'s body. A forgotten field would otherwise leave `nil` in a slot the
annotation says is never nil, and the first read would fault far from the
declaration:

```oskel
class Config {
  host: string;
  timeout: int;          // error: 'init' never assigns it
  fn init(host: string) { self.host = host; }
}
```

A plain `=` and `??=` count as setting the field, because `??=` falls back to its
right operand when the field is still `nil`. Every other compound does **not**:
`self.n += 1` reads the field before writing it, so on an unset field it faults
on `nil` — the check reports it rather than letting the constructor crash:

```oskel
class Counter {
  n: int;
  fn init() { self.n += 1; }   // error: reads the field before writing it
}
```

Give such a field a default (`n: int = 0;`) and the compound is fine.

The check is syntactic: it catches a forgotten field, not every path through
`init`. An assignment inside an `if`, a loop or a `switch` counts, and so does a
nullable field left `nil` on purpose. A lambda body inside `init` does **not**
count, because it runs later, if at all — write the field's default instead.

A method is a key of the instance map, so it can be **replaced**, but only by a
function of the declared arity: a method body reads its parameters positionally,
so another arity would be a latent crash. A non-function value is rejected for
the same reason (the method would silently stop being callable):

```oskel
class Counter {
  n: int;
  fn init() { self.n = 0; }
  fn get(): int { return self.n; }
}
let c = Counter();
c.get = () => 42;      // ok: a function of the declared arity
c.get = 5;             // error: a method is a function
c.get = (a) => a;      // error: must take 0 parameters, got 1
```

A method's parameter and return annotations are checked like any function's, and
so is a **field** annotation. An assignment to `self.field` inside a method, or
to `instance.field` through a binding whose type is known (an annotated binding
like `let p: Point = ...`, a plain `let p = Point(...)` whose type the
constructor call gives, a call to a function with a declared return type, or a
field of another instance), is checked against the field's declared type with the
same rule as every other annotation:

```oskel
class Point {
  x: int;
  fn init() { self.x = "oops"; }   // error: cannot assign 'string' to 'int'
}
let p: Point = Point();
p.x = "oops";                      // error: same rule through the binding
```

The check is compile-time metadata only, so it never changes the emitted code
and the AOT backend inherits it. A lambda written inside a method captures that
method's `self`, so its field writes are checked against the same class. Three
cases stay unchecked, by design:

- a field declared without an annotation (`x;`) has no type to enforce;
- a receiver whose type is not known statically — an element of an array or a
  map, or a parameter with no annotation — stays untyped, as everywhere else;
- a compound assignment (`self.x += 1`) has a result type the compiler does not
  track statically.

Writing a key the class never declared (`p.added = ...`) is allowed, because an
instance is a map and the map surface stays open. A name within two edits of a
declared field is far more likely a typo than a deliberate new key — the typo
would add a field while every read of the real one keeps returning its old value
— so it is reported as a **warning** and the program still runs:

```oskl
class Counter {
  count: int;
  fn init() { self.count = 0; }
}
let c = Counter();
c.cont = 5;    // warning: 'cont' is not a field of class 'Counter'
               //          (did you mean 'count'?); the assignment adds a new key
```

The declared type name itself is validated as a known type (or a declared
struct/class) at parse time.

**Compile-time checks.** The parser rejects, before any bytecode is produced:

- a class declared anywhere but the top level of a module (like `struct`);
- a duplicate class name, and a class whose name collides with a `struct`
  (a class name is also a valid type annotation, so the two would be ambiguous);
- a duplicate field or a duplicate method, and a field and a method that share a
  name (both are keys of the instance map, so one would overwrite the other);
- the reserved name `self` used as a field, a method or a parameter;
- an `init` with a variadic parameter (the constructor forwards its arguments
  one by one, so it cannot pass the collected array);
- an `init` that never assigns a non-nullable annotated field which has no
  default (see above);
- an assignment that replaces a method with a non-function, or with a function
  of the wrong arity.

A class name is a valid type annotation, and — like a hoisted `fn` — a class
declared later in the file is visible earlier:

```oskel
fn area(r: Rect): int { return r.w * r.h; }

class Rect {
  w: int;
  h: int;
  fn init(w: int, h: int) { self.w = w; self.h = h; }
}

let r: Rect = Rect(3, 5);
print(area(r));   // 15
```

Two instances are distinct map values, and because their method entries are
distinct closures (closures compare by identity, §3.2), two instances compare
unequal even when every field matches. Compare fields, not instances.

Because a class desugars to a top-level constructor `fn`, its name **is** a
module's public name: a class declared in a module is reachable through the
qualified form, and `geom.Point(3, 4)` calls the constructor (the value is a
`function`). This differs from a `struct`, whose name is not a value and stays
local to its file. A module can still export a plain constructor function
instead when it wants to keep the class itself private.

`class` is a **reserved word** from 0.1.0 on: a variable or function named
`class` no longer parses.

---

## 5. Operators

### 5.1 Precedence

From highest to lowest precedence:

| Precedence | Operators | Associativity |
| --- | --- | --- |
| 1 | `()` `[]` `.` `?.` `!!` | left |
| 2 | `**` | right |
| 3 | `!` `-` `+` `~` (unary) `try` (prefix) | right |
| 4 | `*` `/` `%` | left |
| 5 | `+` `-` | left |
| 6 | `..` `..=` | left |
| 7 | `<<` `>>` | left |
| 8 | `<` `<=` `>` `>=` `in` | left |
| 9 | `==` `!=` | left |
| 10 | `&` | left |
| 11 | `^` | left |
| 12 | `\|` | left |
| 13 | `&&` | left |
| 14 | `\|\|` | left |
| 15 | `??` | left |
| 16 | `catch` | left |
| 17 | `?:` | right |
| 18 | `=` `+=` `-=` `*=` `/=` `%=` `**=` `&=` `\|=` `^=` `<<=` `>>=` `&&=` `\|\|=` `??=` | right |

Notes:

- `**` is right-associative: `2 ** 3 ** 2` is `2 ** (3 ** 2) = 512`.
- Unary minus has lower precedence than `**`: `-2 ** 2` is `-(2 ** 2) = -4`.
- Ranges have higher precedence than shifts and comparisons: `2 in 1..5` is equivalent to `2 in (1..5)`.
- Comparisons do not chain: `1 < 2 < 3` tries to compare a boolean with an integer and produces an error.
- `??` binds looser than `||`: `a || b ?? c` groups as `(a || b) ?? c`.
- `catch` binds looser than `??` and is left-associative: `a() catch b() catch c` recovers at the first failure (see §13.3).
- `!!` is postfix and binds tightest, next to member access: `a?.b!!` asserts on the result of the access.
- Because `!!` is a single token, prefix double negation is not available; write `!(!x)` where a double negation is needed.

### 5.2 Arithmetic

| Operator | Description |
| --- | --- |
| `+` | Addition of numbers, concatenation of strings or arrays. |
| `-` | Subtraction. |
| `*` | Multiplication. |
| `/` | Division. |
| `%` | Remainder. |
| `**` | Power. |

Rules:

- If both operands are `int`, `+ - * / %` produce `int`.
- Integer division truncates toward zero: `7 / 2 == 3`, `-7 / 2 == -3`.
- The integer remainder takes the sign of the dividend: `-7 % 3 == -1`.
- If an operand is `float`, the result is `float`; if an operand is `double`, the result is `double`. A `float` with a `double` promotes to `double`.
- Dividing or taking the remainder with a zero divisor is a runtime error, for floating-point types too.
- `**` returns `int` if base and exponent are integers and the exponent is not negative; otherwise it uses `std::pow` and returns `double`.
- `int` overflow wraps around (two's complement).
- `+` concatenates two strings and also two arrays (creating a new array).

```oskel
assertEq(7 / 2, 3);
assertEq(7 / 2.0, 3.5);
assertEq(type(1.5f + 1.5f), "float");
assertEq(type(1.5f + 1.5), "double");
assertEq(2 ** 10, 1024);
assertEq("a" + "b", "ab");
assertEq([1] + [2], [1, 2]);
```

### 5.3 Comparison

`<`, `<=`, `>`, `>=` work with:

- two numbers (`int`, `float` or `double`, mixed);
- two strings (lexicographic order by UTF-8 bytes);
- two booleans (`false < true`).

Comparing incompatible types is an error.

### 5.4 Equality

`==` and `!=` use the structural equality described in section 3.2.

### 5.5 Logical

| Operator | Description |
| --- | --- |
| `!` | Logical negation; always returns `bool`. |
| `&&` | Logical AND with short-circuiting. |
| `\|\|` | Logical OR with short-circuiting. |

`&&` and `||` **return one of their operands**, not a boolean:

```oskel
assertEq(nil || "default", "default");
assertEq("value" || "default", "value");
assertEq(false && 123, false);
assertEq(true && 123, 123);
assertEq(nil && 123, nil);
```

The right operand is only evaluated if needed.

### 5.6 Bitwise

| Operator | Description |
| --- | --- |
| `&` | Bitwise AND. |
| `\|` | Bitwise OR. |
| `^` | Bitwise XOR. |
| `~` | One's complement (unary). |
| `<<` | Left shift. |
| `>>` | Arithmetic (signed) right shift. |

They only accept `int`. Shifting requires a count between 0 and 63.

```oskel
assertEq(6 & 3, 2);
assertEq(6 | 3, 7);
assertEq(1 << 4, 16);
assertEq(256 >> 3, 32);
assertEq(~0, -1);
```

Each bitwise operator also has a compound form (`&=`, `|=`, `^=`, `<<=`,
`>>=`); see section 5.9.

### 5.7 Membership

The `in` operator tests membership:

| Right side | Behavior |
| --- | --- |
| array | Structural equality with some element. |
| map | The left side must be a string; checks whether the key exists. |
| string | The left side must be a string; checks whether it is a substring. |
| range | The left side must be an integer; checks whether it lies within the range. |

```oskel
assertEq(2 in [1, 2, 3], true);
assertEq("a" in {a: 1}, true);
assertEq("ell" in "hello", true);
assertEq(5 in 1..10, true);
```

### 5.8 Ternary

```oskel
let label = age >= 18 ? "adult" : "minor";
```

Only the chosen branch is evaluated. It can be nested:

```oskel
let size = n > 100 ? "large" : n > 10 ? "medium" : "small";
```

### 5.8.1 Null-safe operators

| Operator | Form | Result |
| --- | --- | --- |
| Null coalescing | `a ?? b` | `a` unless it is `nil`, in which case `b` |
| Safe access | `a?.b` | `nil` when `a` is `nil`, otherwise `a.b` |
| Non-null assertion | `a!!` | `a`, or a runtime error when `a` is `nil` |

```oskel
let name: string? = nil;
print(name ?? "anonymous");   // "anonymous"

let m: map? = {k: 1};
print(m?.k);                  // 1
let empty: map? = nil;
print(empty?.k);              // nil

let s: string? = "ok";
print(s!!);                   // "ok"
```

Only `nil` triggers the fallback and the guard, so `0`, `false` and `""` are
kept: these operators are not truthiness. `??` short-circuits (its right-hand
side is evaluated only when the left is `nil`), and `a?.b` is read-only. See
[4.6 Type annotations](#46-type-annotations) for the full rules.

### 5.9 Assignment

Assignment is an expression and returns the assigned value:

```oskel
var x = 0;
let y = (x = 5);
assertEq(x, 5);
assertEq(y, 5);

var a = 0;
var b = 0;
let c = (a = b = 3); // right-associative
```

Compound operators:

```oskel
x += 1;
x -= 1;
x *= 2;
x /= 2;
x %= 3;
x **= 2;
x &= 0xF0;
x |= 0x0F;
x ^= 0xFF;
x <<= 2;
x >>= 1;
```

The bitwise compounds are the `&`, `|`, `^`, `<<` and `>>` operators in
assignment form: `x &= y` is exactly `x = x & y`, so they accept only `int`
operands and a shift count between 0 and 63, and they lower to the existing
`BAND`/`BOR`/`BXOR`/`SHL`/`SHR` opcodes (no new opcode).

They also work on indexes and members:

```oskel
let data = [1, 2];
data[0] += 10;
data[0] *= 2;
data[0] <<= 1;

let person = {age: 36};
person.age += 1;
person["age"] &= 0x7F;
```

The logical operators have a compound form too. Each one short-circuits exactly
like its binary counterpart and writes the target back only when it does not,
so the right-hand side is evaluated at most once:

```oskel
var name = nil;
name ??= "anon";  // assigns only when `name` is nil

var count = 0;    // 0 is truthy in Oskel
count ||= 10;     // not assigned: `count` is already truthy

var flag = true;
flag &&= check(); // calls `check()` and assigns its result
```

- `a ??= b` is `a = a ?? b`: assign `b` only when `a` is nil (`false` and `0`
  are kept).
- `a ||= b` is `a = a || b`: assign `b` only when `a` is falsy (`false` or
  `nil`).
- `a &&= b` is `a = a && b`: assign `b` only when `a` is truthy.

Like the other compound operators, they work on indexes and members
(`data[0] ??= 1`, `obj.name ||= "x"`) and yield the target's final value. They
read the target once and lower to the existing opcodes (`EQ`/`NEQ`,
`JMP_IF_FALSE`/`JMP_IF_TRUE`, `MOVE`), so no opcode is added.

There are no `++` or `--`; use `+= 1` and `-= 1`.

---

## 6. Statements

### 6.1 Expression statements

An expression followed by `;` is evaluated and its result is discarded:

```oskel
1 + 2;
print("hello");
```

### 6.2 Blocks

```oskel
{
  let temporary = 1;
  print(temporary);
}
```

Blocks create a new scope. An empty `;` statement is also valid.

### 6.3 `if` / `else`

The condition goes in parentheses. The body is usually a block:

```oskel
if (age >= 18) {
  print("adult");
} else if (age >= 13) {
  print("teenager");
} else {
  print("minor");
}
```

A body may also be a single statement without braces. It is wrapped in a
block, so its bytecode is the same as the brace form, it is a scope of its own
(a `let` in it does not leak) and a `defer` in it runs when the body exits:

```oskel
if (x > 0) print("positive"); else print("non-positive");

if (n >= 90) return "A";
else if (n >= 70) return "B";
else return "C";
```

The dangling `else` binds to the nearest `if`, like C. The same brace-less form
works for `while`, `for` and `for-in` (§6.4, §6.5, §6.6).

### 6.4 `while` and `do`/`while`

`while` tests its condition before each iteration:

```oskel
var i = 0;
while (i < 5) {
  print(i);
  i += 1;
}
```

`do { ... } while (cond);` is the post-test form: the body runs once before the
condition is checked, so it always runs at least once. The trailing `;` after
the condition is optional.

```oskel
var n = 0;
do {
  n += 1;
} while (n < 3);   // runs three times

do { n += 1; } while (false);   // runs once
```

In both loops `break` leaves the loop, `continue` re-tests the condition (in a
`do`/`while` it jumps to the condition, not to the top of the body), and a
`defer` in the body runs on every exit path.

### 6.5 Classic `for`

```oskel
for (var i = 0; i < 5; i += 1) {
  print(i);
}
```

Any of the three clauses may be omitted:

```oskel
for (;;) {
  break;
}
```

The variable declared in the initializer lives in a scope of its own for the loop. The body additionally creates a new scope on every iteration.

### 6.6 `for ... in`

Syntax, with or without parentheses:

```oskel
for value in [10, 20, 30] {
  print(value);
}

for (value in [10, 20, 30]) {
  print(value);
}
```

The two spellings are equivalent. The parenthesized form is told apart from a
classic `for` header by the lookahead `( name in` (or `( name , name in` for
the key/value form), so `for (var i = 0; ...)` is unaffected.

You can iterate over:

- **arrays**: in order, by value;
- **strings**: by code point, each one as a one-character string;
- **maps**: by key, in insertion order;
- **ranges**: ascending or descending depending on the bounds.

```oskel
for c in "abc" {
  print(c.upper());
}

for key in {a: 1, b: 2} {
  print(key);
}

for n in 1..=3 {
  print(n);
}
```

A **map** also accepts a second loop variable after a comma: `for k, v in map`
binds the key to the first variable and its value to the second, so a map is
walked as entries without going through `entries()`. The parenthesized form
`for (k, v in map)` is the same. Key/value iteration is only valid for maps;
any other iterable raises a runtime error. `ITER` yields the paired value
directly, so the value form costs no extra lookup and does not change the
iteration order or the live-mutation rules below.

```oskel
for key, value in {a: 1, b: 2} {
  print("${key} = ${value}");
}
```

The iterable is evaluated only once. Arrays and maps are iterated **live**: an array is walked by index and a map over its keys, both as they exist at each step, so mutating the collection inside the body changes what the loop sees. Appending to an array extends the loop; removing an element that the cursor has not reached skips it. Iterate over a copy (for example `for x in a.slice()` or `for k in m.keys()`) when the body mutates the collection. Ranges are iterated lazily: no intermediate array is created, so `for i in 0..1000000000 { ... }` runs in constant memory. The iteration variable gets a fresh binding on every turn, so closures created inside the body capture different values.

### 6.7 `break` and `continue`

```oskel
for n in 0..10 {
  if (n % 2 == 0) {
    continue;
  }
  if (n > 7) {
    break;
  }
  print(n);
}
```

They only affect the innermost loop. They cannot cross a function boundary: a `break` inside a function that is not in a loop is an error, even if the function is called from inside a loop.

### 6.8 `return`

```oskel
fn maximum(a, b) {
  if (a > b) {
    return a;
  }
  return b;
}
```

`return;` returns `nil`. Using `return` outside a function is an error.

### 6.9 Function declarations

```oskel
fn name(parameter1, parameter2) {
  // body
  return value;
}
```

They can be declared in the global scope or inside a block. They are hoisted within their block. See section 7.

### 6.10 `switch`

```oskel
switch (value) {
  case 1: {
    print("one");
  }
  case 2: case 3: {
    print("two or three");
  }
  case 10..=20: {
    print("in the teens");
  }
  default: {
    print("something else");
  }
}
```

A `switch` evaluates its subject **exactly once** and compares it against the case labels in order. A label is a **literal** (`nil`, `true`, `false`, a number or a string, optionally signed as in `-1`), an **integer range** (`10..20`, `10..=20`), or a **`const` name** (a top-level `const`, or a local `const`/`let` in the same frame) whose value is used as the label. A literal matches by equality; a range matches by membership, exactly like `value in 10..=20`; a `const` name matches by equality against its value. A name that is not an immutable binding (a `var`, an unknown name, or a captured binding) is a compile error, so a label is always a fixed value.

The first matching case runs and then control leaves the switch. **There is no fallthrough**: unlike C or Java, the body of the next case does not run after a match, so a forgotten `break` cannot cause a bug. Several labels may share one body (`case 2: case 3:`), and a `default` clause (at most one) runs only when nothing matched; it is optional, so a switch with no match simply does nothing.

The subject may be any expression. Each case body is a block with its own scope, so a `let` declared there stays local. `break` and `continue` inside a switch bind to the enclosing loop (the switch is not a loop), and `return` works as usual inside a case. Because the bytecode is built from the existing `EQ`, `IN` and `JMP` instructions, `switch` adds no opcode.

```oskel
fn classify(n) {
  switch (n) {
    case 0: { return "zero"; }
    case 1..=9: { return "small"; }
    default: { return "big"; }
  }
}
```

### 6.11 `defer`

```oskel
fn readConfig(path) {
  let file = open(path);
  defer {
    file.close();
  }
  // ... use `file` ...
  return parse(file.read());
}
```

`defer` registers a block to run when the **enclosing scope exits**. The block does not run where it is written: the compiler inlines it at every exit from that scope, so it runs whether the scope is left normally (falling off the end), through `return`, or through the `break`/`continue` that leave it. This makes `defer` the idiomatic way to release a resource, mirroring Go's `defer`.

Several deferred blocks in the same scope run in **LIFO order**: the last one declared runs first. A `defer` inside a loop body runs once per iteration, and once more on the iteration that leaves the loop early. A standalone block is a scope too, so its deferred blocks run when the block ends.

The body must be a `{ ... }` block, and `defer` may only appear inside a function body (at the program top level there is no scope exit to attach it to). A deferred block cannot contain a `return`, `break` or `continue` that would leave the block itself: that would re-enter the teardown that is running it. A loop or a nested function declared *inside* the deferred block is a fresh boundary, so its own `break`/`return` is allowed.

Because the deferred blocks are inlined at the scope exits, `defer` adds no opcode and no VM support.

```oskel
fn lifo() {
  var log = [];
  defer { log.push("first"); }
  defer { log.push("second"); }
  log.push("body");
  return log; // ["body", "second", "first"]
}
```

---

## 7. Functions and closures

### 7.1 Declarations and calls

```oskel
fn add(a, b) {
  return a + b;
}

print(add(2, 3)); // 5
```

A function whose body is a single expression can use the short form with `=>`. It needs a terminating `;`:

```oskel
fn square(x) => x * x;

print(square(4)); // 16
```

The number of arguments must match the number of parameters exactly, unless the
last parameter is **variadic**. A parameter written `rest...` collects every
extra argument into an array; the `...` follows the optional type annotation
(`rest...` or `parts: int...`), which describes the elements, not the array.
A variadic parameter must be the last one, and a function may have at most one.
A trailing comma is allowed after it (`fn f(a, rest...,)`), like after any other
last parameter.

```oskel
fn sum(first, rest...) {
  var total = first;
  for x in rest {
    total += x;
  }
  return total;
}

print(sum(1));          // 1
print(sum(1, 2, 3));    // 6
print(sum(1, 2, 3, 4)); // 10
```

`rest` is an ordinary array inside the body, so `len(rest)`, indexing and
`for ... in rest` all work. There are no default parameters.

Parameters are local. Arrays and maps are passed by reference; reassigning the parameter inside the function does not change the caller's variable.

```oskel
fn append(arr) {
  arr.push(1); // visible outside
}

fn replace(arr) {
  arr = [9];   // only changes the local parameter
}
```

### 7.2 Lambdas

`fn` always declares a named function. An anonymous function is written as a **lambda** with `=>`, and a function is a value:
```oskel
let double = x => {
  return x * 2;
};

let triple = x => x * 3;

let anonymous = () => {
  print("hello");
};
```

A single parameter needs no parentheses (`x => ...`); zero or several do (`() => ...`, `(a, b) => ...`). The body is either an expression, which becomes the return value, or a `{ ... }` block. Because a `{` after `=>` starts a block, a lambda that returns a map literal wraps it in parentheses: `x => ({value: x})`.

Lambdas can be passed as arguments:

```oskel
let squares = [1, 2, 3].map(x => x * x);
```

### 7.3 Recursive lambdas

A lambda bound to a variable can call itself through that variable, because the name is resolved when the lambda is called:

```oskel
let power = (base, exp) => exp == 0 ? 1 : base * power(base, exp - 1);

print(power(2, 10)); // 1024
```

For anything longer than a one-liner, prefer a declared function (`fn power(base, exp) { ... }`), which is also hoisted.

### 7.4 Closures

Functions capture the environment where they are created and keep it alive:

```oskel
fn makeCounter() {
  var n = 0;
  return () => {
    n += 1;
    return n;
  };
}

let counter = makeCounter();
print(counter()); // 1
print(counter()); // 2
```

Capture is by reference: if the captured variable changes after the function is created, the function sees the new value.

```oskel
fn example() {
  var state = 1;
  let read = () => state;
  state = 2;
  return read;
}

print(example()()); // 2
```

### 7.5 Recursion and depth

Recursion works both in declared functions and in named lambdas. The runtime limits the call depth to **2000**; exceeding it produces a runtime error.

```oskel
fn factorial(n) {
  if (n <= 1) {
    return 1;
  }
  return n * factorial(n - 1);
}
```

### 7.6 `native fn`

A `native fn` declares a function whose body is **not** written in Oskel: the
name binds to a function the host (the VM or the future AOT runtime) already
provides, such as a standard-library builtin. The declaration is bodyless and
carries only a signature, so it emits no bytecode and adds no opcode — the name
is bound with a plain `GETGLOBAL` of the host function.

```oskel
native fn sqrt(x: double): double;

print(sqrt(9.0));   // 3
print(sqrt(16));    // 4  (int widens to double)
```

The signature is checked at compile time: a call must pass the exact number of
arguments, and each argument must be assignable to its parameter type. A
trailing `rest...` parameter makes the native variadic: the call must pass the
fixed parameters plus zero or more extras, and each extra is checked against the
variadic parameter's element type. The return type is used for inference, so
`let y: double = sqrt(4.0)` is checked like any other annotated binding. An
unannotated parameter (or `any`) turns the check off for that argument, which is
how a builtin with a loose signature is declared:

```oskel
native fn str(v: any): string;
native fn min(first: double, rest: double...): double;   // variadic
native fn len(v);                 // fully untyped parameter
native fn free(p: any): void;     // no return value (maps to C's void)

assertEq(str(42), "42");
assertEq(len([1, 2, 3]), 3);
```

The same check applies to a **qualified** call through a module namespace. A
`native fn` a module exports keeps its signature on the imported module, so
`std.math.sqrt(16.0)` is checked exactly like `sqrt(16.0)` — the arity and the
argument types are enforced, and the message names the qualified form
(`'std.math.pow' expects 2 arguments, got 1`). The check is deferred to the end
of the chunk, because a function declared before the `import` that loads the
module is compiled first, but it runs before the program executes, so a
mismatch is always a compile error rather than a run-time failure.

Like `struct`, a `native fn` is **module-level metadata**: it may only be
declared at the top level of a file. Its parameter and return types must be
type names the compiler knows (or `any`). A `void` return type is allowed and
maps directly to C's `void` in the FFI (PHASE 6.6).

A `native fn` binds the name at run time to whatever the host registered under
that name. If the host provides nothing, using the name reports
`undefined variable '<name>'`, so a typo or a missing host function is a clear
runtime error rather than a silent failure. Builtins such as `print`, `len` and
`str` are host functions, so a `native fn` declaration is the typed way to
expose one to the checker.

---

### 7.7 `@MustUse`

A function may be declared `@MustUse`: the caller must do something with the
return value. When a call to it is used as a statement, so its result is thrown
away, the compiler reports a **warning** (never an error), naming the function:

```oskel
@MustUse
fn parse(input: string): int | error { ... }

parse(text);          // warning: the result of '@MustUse' function 'parse' is discarded
let n = parse(text);  // ok: the result is used
```

The annotation is compile-time metadata: it emits no bytecode, and the bytecode
of a program is identical with and without it. It is accepted on `fn` and on
`native fn` declarations, at the top level and inside a function body.

Discarding the result is sometimes intentional. Assigning it to `_` is the
explicit acknowledgement, and it never warns:

```oskel
_ = parse(text);      // ok: the discard is deliberate
```

A warning is a diagnostic, not a rejection. The program still compiles and
runs, and the exit code is unchanged, so a `@MustUse` annotation can never turn
a program that would have run into a failure (docs/language.md §4.6). This mirrors Rust's
`#[must_use]` and Kotlin's `@CheckReturnValue`, and follows Go's
`errcheck` convention that a deliberate discard should be visible.

The check is precise about **which** function is called. A local or upvalue that
shadows a `@MustUse` name is a different function, so it carries its own flag and
does not inherit the annotation:

```oskel
@MustUse
fn parse(input: string): int | error { ... }

fn use() {
  fn parse(input) => 0;   // a different function
  parse("x");             // no warning: this `parse` is not `@MustUse`
}
```

Only `@MustUse` is a known attribute. An unknown attribute is a compile error
rather than being silently ignored, so a typo cannot drop the check; and an
attribute must precede a `fn` or `native fn` declaration. Attribute names are
`PascalCase` (`@MustUse`), like the future `@Dynamic`, so they are never
confused with the `camelCase` of ordinary identifiers.

---

## 8. Arrays

### 8.1 Literals and indexes

```oskel
let empty = [];
let numbers = [1, 2, 3,];
```

A trailing comma is allowed. Indexes are integers; negative ones count from the end:

```oskel
assertEq(numbers[0], 1);
assertEq(numbers[-1], 3);
```

An out-of-range index is a runtime error. Arrays are mutable:

```oskel
numbers[0] = 10;
numbers.push(4);
```

A slice `a[start:end:step]` returns a new array with the half-open range
`[start, end)`, taking every `step`-th element. Every part may be omitted:
`a[:]`, `a[2:]`, `a[:3]`, `a[::2]` and `a[::-1]` all parse. Negative bounds
count from the end and are clamped to the size; a negative `step` walks
backwards, and a `step` of 1 (the default) is the plain range. The bounds
follow Python's rule: with a negative step the defaults are the end and
before the start, so `a[::-1]` reverses. A slice is a copy, so mutating it
never touches the source, and it is not an assignment target.

```oskel
let a = [10, 20, 30, 40, 50];
assertEq(a[1:3], [20, 30]);
assertEq(a[2:], [30, 40, 50]);
assertEq(a[:2], [10, 20]);
assertEq(a[-2:], [40, 50]);
assertEq(a[:], a);
assertEq(a[::2], [10, 30, 50]);
assertEq(a[::-1], [50, 40, 30, 20, 10]);
assertEq(a[3:0:-1], [40, 30, 20]);
```

The same syntax works on strings, slicing by UTF-8 character
(`"hello"[::-1]` is `"olleh"`). A zero step is a runtime error. Slicing a
value that is neither an array nor a string (for example `5[1:2]`) is a
runtime error that names the receiver's type; that check runs before the
bounds, so `5["a":"b"]` reports the receiver, not the bound.

### 8.2 Members

`array.length` returns the number of elements. `len(array)` does the same.

### 8.3 Array methods

| Method | Description |
| --- | --- |
| `push(value, ...)` | Appends one or more values at the end and returns the array. |
| `pop()` | Removes and returns the last element; `nil` if empty. |
| `shift()` | Removes and returns the first element; `nil` if empty. |
| `unshift(value)` | Inserts a value at the start and returns the array. |
| `insert(index, value)` | Inserts at `index` (negatives allowed) and returns the array. |
| `removeAt(index)` | Removes and returns the element at `index`. |
| `slice(start?, end?)` | Returns a new array with the range `[start, end)`. |
| `concat(other)` | Returns a new array with both arrays. |
| `join(separator?)` | Joins the elements into a string. |
| `sort(comparator?)` | Sorts the array in place and returns it. If the comparator throws, the array is left unchanged. |
| `sorted(comparator?)` | Returns a sorted copy. |
| `reversed()` | Returns a reversed copy. |
| `contains(value)` | Checks structural equality. |
| `indexOf(value)` | Returns the first index or `-1`. |
| `map(f)` | Returns an array with the results of `f(element)`. |
| `filter(f)` | Returns the elements for which `f` is truthy. |
| `reduce(f, initial?)` | Accumulates with `f(accumulator, element)`. |
| `each(f)` | Calls `f(element)` for each element and returns the array. |
| `find(f)` | Returns the first element that satisfies `f`, or `nil`. |
| `any(f)` | `true` if any element satisfies `f`. |
| `all(f)` | `true` if all elements satisfy `f`. |
| `sum()` | Sum of an array of numbers. |
| `clear()` | Empties the array and returns it. |
| `clone()` | Shallow copy. |

Details:

- `slice` clamps the indexes to the size of the array and accepts negatives. If `end < start`, it returns an empty array.
- `sort` and `sorted` use ascending order by default. The comparator receives `(a, b)` and must return `true` if `a` should come before `b`; the sort is stable.
- `reduce` without an initial value uses the first element as the accumulator and fails if the array is empty.
- `map`, `filter`, `reduce`, `each`, `find`, `any` and `all` call their callback through the VM, so the callback may itself call a builtin (a nested `map`), read a captured outer binding, or raise. An error raised inside a callback unwinds out of the builtin and can be caught with `pcall`, like any other runtime error.
- `sum` requires all elements to be numbers; it returns `int` if all are integers and `double` otherwise.
- `clone` is shallow: nested arrays and maps are shared.
- The in-place mutators (`push`, `unshift`, `insert`, `sort`, `clear`, and the
  map's `merge`) return the receiver, not `nil`, so they can be chained
  (`a.push(1).push(2)`) or assigned back (`a = a.push(1)`). `pop`, `shift` and
  `removeAt` instead return the removed element, since that is the value the
  caller wants.

```oskel
let nums = [3, 1, 2];
assertEq(nums.sorted(), [1, 2, 3]);
assertEq(nums.sorted((a, b) => a > b), [3, 2, 1]);
assertEq(nums.map(n => n * 2), [6, 2, 4]);
assertEq(nums.reduce((a, n) => a + n, 0), 6);

let chain = [1].push(2).push(3);   // mutators return the receiver
assertEq(chain, [1, 2, 3]);
```

---

## 9. Strings

Strings are immutable and indexed by code point:

```oskel
let s = "áé";
assertEq(s.length, 2);
assertEq(s[0], "á");
assertEq(s[-1], "é");
```

An out-of-range index is an error. You cannot assign to a string index.

Strings also support slicing by code point, with the same rules as arrays
(bounds and step omitted, negatives counted from the end, clamped, negative
step reverses):

```oskel
let s = "hello";
assertEq(s[1:3], "el");
assertEq(s[2:], "llo");
assertEq(s[:-1], "hell");
assertEq(s[::-1], "olleh");
```

### 9.1 String methods

| Method | Description |
| --- | --- |
| `upper()` | Applies the Unicode simple uppercase mapping. |
| `lower()` | Applies the Unicode simple lowercase mapping. |
| `trim()` | Removes ASCII whitespace at the start and end. |
| `trimStart()` | Removes ASCII whitespace at the start. |
| `trimEnd()` | Removes ASCII whitespace at the end. |
| `split(separator?)` | With no argument splits on whitespace; with `""` splits into characters. |
| `lines()` | Splits on `\n` line breaks. |
| `replace(search, replacement)` | Replaces all non-overlapping occurrences. |
| `replaceFirst(search, replacement)` | Replaces the first occurrence. |
| `startsWith(prefix)` | Checks the prefix. |
| `endsWith(suffix)` | Checks the suffix. |
| `contains(text)` | Checks whether it contains the substring. |
| `indexOf(text)` | Code-point index of the first occurrence, or `-1`. |
| `substr(start, length?)` | Substring from `start` (negatives allowed). |
| `chars()` | Array of one-character strings. |
| `repeat(n)` | Repeats the string `n` times. |
| `reversed()` | Reverses the code points. |
| `padStart(width, fill?)` | Pads on the left up to `width`. |
| `padEnd(width, fill?)` | Pads on the right up to `width`. |

Details:

- `upper` and `lower` apply the Unicode simple case mapping (one code point to
  one code point), so `"á".upper()` is `"Á"`. A code point whose full mapping
  expands to several code points (for example `ß` -> `SS`, `İ` -> `i` plus a
  combining dot, the `ﬁ` ligature -> `FI`) has no simple mapping and is left
  unchanged. Greek final sigma is mapped to the ordinary small sigma.
- `trim*` considers ASCII whitespace (`std::isspace`), not every Unicode space.
- `split` with no argument breaks on runs of that same ASCII whitespace and keeps no leading or trailing empty field (`"  a  b  ".split()` is `["a", "b"]`, `"   ".split()` is `[]`); with a separator it keeps empty fields (`"a,,b".split(",")` is `["a", "", "b"]`).
- `lines` does not add an empty trailing line if the string ends with `\n`.
- `indexOf` searches bytes but returns the index in code points.
- `substr` counts code points; a negative `length` produces an empty string.
- `padStart` and `padEnd` pad by code points and trim the fill to the exact width. The result has the same 64 MB limit as `repeat`.
- `repeat` has a 64 MB limit on the result. An empty text repeats to an empty text for any non-negative count (including a very large one), and a negative count is an error.

```oskel
assertEq("  hello  ".trim(), "hello");
assertEq("a,b,c".split(","), ["a", "b", "c"]);
assertEq("abc".substr(1, 2), "bc");
assertEq("5".padStart(3, "0"), "005");
```

---

## 10. Maps

### 10.1 Literals

Keys in a literal can be identifiers, string literals, scalar literals (`int`, `float`, `double`, `true`, `false`, `nil`) or computed expressions in brackets:

```oskel
let person = {
  name: "Ada",
  age: 36,
  "key with spaces": true,
};

// Scalar literals are canonicalized to their string form.
let coded = {1: "one", 1.5: "one and a half", true: "yes", nil: "nothing"};
assertEq(coded["1"], "one");
assertEq(coded["1.5"], "one and a half");

// Computed keys use the [expr] syntax and are evaluated at runtime.
let n = 2;
let dyn = {[n * 10]: "twenty", ["a" + str(n)]: "a2"};
assertEq(dyn["20"], "twenty");
```

All map keys are strings internally: scalar and computed keys are normalized with the value's canonical string form (for example `1` becomes `"1"` and `1.5` becomes `"1.5"`), so lookups always use strings. If a key is repeated, the last value wins, but the position of its first appearance is kept.

### 10.2 Access and assignment

```oskel
assertEq(person.name, "Ada");
assertEq(person["age"], 36);

person.age = 37;
person["city"] = "London";
```

Accessing a missing key returns `nil` (both with `.` and with `[]`). Keys must be strings; using another type as an index is an error.

Maps preserve insertion order. Reassigning an existing key does not move it.

### 10.3 Members and collisions with methods

`map.length` returns the number of keys, unless a `"length"` key exists, in which case its value is returned. Likewise, a key named like a method (for example `"keys"`) takes priority over the method.

### 10.4 Map methods

| Method | Description |
| --- | --- |
| `keys()` | Array of keys in insertion order. |
| `values()` | Array of values in insertion order. |
| `entries()` | Array of `[key, value]` pairs. |
| `has(key)` | `true` if the key exists. |
| `get(key, default?)` | Value of the key, or `default`/`nil`. |
| `set(key, value)` | Assigns and returns the map. |
| `delete(key)` | Removes the key; returns `true` if it existed. |
| `clear()` | Empties the map and returns it. |
| `merge(other)` | Copies the keys of `other` into this map and returns it. |
| `clone()` | Shallow copy. |

```oskel
let m = {a: 1};
m.set("b", 2);
assertEq(m.keys(), ["a", "b"]);
assertEq(m.entries(), [["a", 1], ["b", 2]]);
assertEq(m.get("z", 0), 0);
assertEq(m.delete("a"), true);
assertEq(m, {b: 2});
```

Map equality does not depend on order: two maps with the same keys and values are equal.

Iterating a map produces its keys:

```oskel
for key in {a: 1, b: 2} {
  print(key);
}
```

---

## 11. Ranges

### 11.1 Syntax

| Expression | Meaning |
| --- | --- |
| `a..b` | From `a` up to `b`, excluding `b`. |
| `a..=b` | From `a` up to `b`, including `b`. |

The bounds must be integers. A range can be ascending or descending:

```oskel
assertEq((1..4).toArray(), [1, 2, 3]);
assertEq((1..=3).toArray(), [1, 2, 3]);
assertEq((3..0).toArray(), [3, 2, 1]);
assertEq((3..=0).toArray(), [3, 2, 1, 0]);
```

`5..5` is empty; `5..=5` contains one element.

### 11.2 Operations

- `.length` / `len(range)`: number of elements.
- `.toArray()`: converts to an array (limit of 10,000,000 elements).
- `.contains(value)`: same as `value in range`.
- Equality compares start, end and inclusiveness: `1..3 != 1..=3`.

The `range(end)` function produces `0..end`, and `range(start, end)` produces `start..end` (always exclusive). Like the literal form, `range(start, end)` descends when `start > end`, and equal bounds give an empty range.

Range values are lightweight: they only store the bounds and the inclusiveness flag. Iterating a range with `for ... in` is lazy (constant memory); operations that need all elements at once, such as `toArray()` or passing the range to `map`/`filter`-style builtins, materialize them and are capped at 10,000,000 elements. A range wider than `INT64_MAX` elements reports a `.length` of `9223372036854775807` (the count saturates rather than overflowing).

```oskel
assertEq(2 in 1..5, true);
assertEq(5 in 1..5, false);
assertEq(5 in 1..=5, true);
assertEq(len(5..0), 5);
```

---

## 12. Standard library

### 12.1 Console and input

| Function | Description |
| --- | --- |
| `print(value, ...)` | Prints the values separated by spaces and appends `\n`. |
| `write(value, ...)` | Like `print`, but without the trailing newline. |
| `eprint(value, ...)` | Like `print`, but to the error output. |
| `input(prompt?)` | Reads a line; returns `nil` at the end of the input. |

### 12.2 Types and conversion

| Function | Description |
| --- | --- |
| `type(value)` | Name of the type as a string. |
| `str(value)` | Text representation. |
| `repr(value)` | Representation with strings in quotes. |
| `int(value)` | Converts to integer (truncates floats and doubles). |
| `float(value)` | Converts to `float` (binary32). |
| `double(value)` | Converts to `double` (binary64). |
| `bool(value)` | Applies Oskel's truthiness. |

`int(string)` accepts the same bases the language spells for literals (see §2.3): an explicit `0x`/`0X`, `0b`/`0B` or `0o`/`0O` prefix, otherwise decimal. A bare leading zero stays decimal (`int("010")` is `10`, matching the literal `010`), and an optional `+`/`-` sign is allowed before the prefix (`int("-0x10")` is `-16`). Surrounding whitespace is trimmed. `float(string)` and `double(string)` parse the text with `strtod` (`float` then rounds to binary32). The non-finite values are accepted only when the text spells them out (`inf`, `infinity` or `nan`, case-insensitive, with an optional sign); a silent overflow such as `1e400` is an error, and a value outside the `float` range (`1e40`) is an error for `float`. Converting an empty or badly formatted string is an error. `int(float)` and `int(double)` truncate toward zero and are an error when the value is outside the `int` range (a non-finite value is an error too).

A floating-point value prints with the shortest decimal text that round-trips through its type: parsing the printed form back yields exactly the same value. A `double` uses binary64 precision (`str(1.0 / 3.0)` is `"0.3333333333333333"`) and a `float` uses binary32 (`str(0.1f)` is `"0.1"`). Values that fit in fewer digits keep the shortest form (`str(1.5)` is `"1.5"`).

`str` of an array or map uses the representation of its elements, with inner strings in quotes:

```oskel
assertEq(str(42), "42");
assertEq(str([1, "a"]), "[1, \"a\"]");
assertEq(repr("a"), "\"a\"");
```

### 12.3 Math

Some math functions are host builtins (always in scope); `abs`, `min`, `max`
and `clamp` live in the `std.math` module and are imported. See §18.

Host builtins:

| Function | Description |
| --- | --- |
| `floor(n)` | Rounds down. |
| `ceil(n)` | Rounds up. |
| `round(n)` | Rounds to the nearest integer (halves away from zero). |
| `sqrt(n)` | Square root; error if `n < 0`. |
| `pow(base, exp)` | Power as `double`. |
| `sin(n)`, `cos(n)`, `tan(n)` | Trigonometric functions (radians). |
| `log(n)` | Natural logarithm; error if `n <= 0`. |
| `exp(n)` | `e ** n`. |

`std.math` (import it first):

| Function | Description |
| --- | --- |
| `abs(n)` | Absolute value. |
| `min(a, ...)` | Minimum of at least one argument. |
| `max(a, ...)` | Maximum of at least one argument. |
| `clamp(n, minimum, maximum)` | Limits `n` to the interval. |
| `PI`, `E` | Constants. |

`floor`, `ceil` and `round` return the same `int` if they receive an `int`, and a `double` for a `float` or `double` argument (like Java's `Math.floor`, which takes and returns `double`).

Under `oskel build` (the AOT backend), `sqrt`, `floor`, `ceil` and `round` have
an exact stub (correctly-rounded SSE, so they match the VM bit for bit). The
transcendentals (`pow`, `sin`, `cos`, `tan`, `log`, `exp`) and the other
`std.math` host bindings have no stub yet — a program that *calls* one is
rejected at build time with a message naming the symbol (an unused declaration
is harmless), since a hand-written implementation would not reproduce libm's
last bit. See `DEVELOPMENT.md` PHASE 7f.

```oskel
import std.math.*;

assertEq(abs(-7), 7);
assertEq(min(3, 1, 2), 1);
assertEq(clamp(15, 0, 10), 10);
assertEq(sqrt(81), 9.0);
```

### 12.4 Randomness and time

| Function | Description |
| --- | --- |
| `random()` | `double` in `[0, 1)`. |
| `random(high)` | `int` in `[0, high)`. |
| `random(low, high)` | `int` in `[low, high)`. |
| `seed(n)` | Reseeds the generator. |
| `clock()` | Seconds elapsed since the process started (monotonic, for timing — not a date). |
| `time()` | Seconds since the Unix epoch, UTC, as an `int` (the wall clock; see `std.datetime` in §12.10). |
| `memSize()` | Total physical memory of the machine, in bytes, as an `int` (`0` when it cannot be determined). |

`random()` draws from `mt19937_64`, whose bit stream the C++ standard fully
specifies. Both engines therefore apply the same stated reductions — the top 53
bits over 2^53 for `random()`, `next() % span` for the integer forms — so a
native binary reproduces the development VM's sequence exactly (the
`tests/native_build_random.oskl` fixture compares the two). The generator is
emitted in machine code by the AOT backend, with its 312-word state in a hidden
global backed by the arena.

Two runs agree only after the same `seed(n)` call: the VM seeds from the
platform's entropy source and a native binary from the standard's default seed
(5489), because an image has no entropy source and a fixture must be
reproducible. A program that needs a fixed stream calls `seed(n)` first.

`time()` reads the system wall clock. It is available on the development VM and
under `oskel build` on every target: the AOT runtime binds it to a syscall on
Linux, `time(NULL)` in hosted mode, `GetSystemTimeAsFileTime` on Windows, and
the CMOS real-time clock in a bare-metal kernel. `clock()` is monotonic (it
measures elapsed time, not the date) and has no AOT binding, so a native build
that calls it fails cleanly.

`memSize()` reads the machine's RAM, the way `time()` reads its clock. On the
development VM it comes from the platform (`sysconf` on POSIX, `GlobalMemoryStatusEx`
on Windows) and is `0` on a platform with no view of the machine. Under
`oskel build` it is **bare-metal only**: the `x86_64-freestanding-sysv` kernel
reads the boot loader's multiboot memory map (the available entries summed), so
a hosted target rejects a call with a message naming the target instead of
emitting a binary that returns a wrong number.

### 12.5 Utilities

| Function | Description |
| --- | --- |
| `len(value)` | Length of a string, array, map or range. |
| `chr(code)` | Unicode character from a code point (`0..0x10FFFF`, not a lone surrogate). |
| `ord(character)` | Code point of a one-character string. |
| `range(end)` | Range `0..end`. |
| `range(start, end)` | Range `start..end`. |
| `args()` | Script arguments as an array of strings (see §15.1). |

### 12.6 Files

| Function | Description |
| --- | --- |
| `readFile(path)` | Returns the contents of a file as a string. |
| `writeFile(path, contents)` | Writes the contents (exact binary). |
| `fileExists(path)` | Checks whether the file can be opened. |

A path is handed to the OS as a NUL-terminated C string, so a path containing an
embedded NUL (`"a\0b"`) is rejected with a runtime error instead of silently
operating on the truncated path.

### 12.7 Assertions and control

| Function | Description |
| --- | --- |
| `assert(condition, message?)` | Fails if the condition is falsy. |
| `assertEq(expected, actual, message?)` | Fails if they are not equal. |
| `panic(message?)` | Raises a runtime error. |
| `pcall(fn, args...)` | Calls `fn` and turns a runtime error into a value (see §13.1). |
| `error(message, code?)` | Builds an error value (see §13.2). |
| `isError(value)` | `true` only for an error value (see §13.2). |
| `exit(code?)` | Ends the process with the given code (0 by default). |

`assertEq` follows the order `expected, actual` and adds both values to the error message.

### 12.8 FFI pointers

Opaque `ptr`/`cstring` handles (PHASE 6.6, see §4.6). They own host storage and
are never a machine address the program can dereference.

| Function | Description |
| --- | --- |
| `cstring(s)` | Builds a `cstring` from a string (bytes plus a NUL). |
| `cstr(p)` | Reads a `cstring`/buffer back as a string (a `cstring` stops at the first NUL). |
| `buffer(n)` | Allocates `n` zero-initialised bytes and returns a `ptr`. |
| `byteLen(p)` | Bytes the handle owns. |
| `loadByte(p, i)` | Byte `0..255` at index `i` (bounds-checked). |
| `storeByte(p, i, b)` | Writes a byte into a buffer. |
| `intToPtr(n)` | Wraps an opaque integer token in a `ptr`. |
| `ptrToInt(p)` | Reads the opaque integer token back. |
| `free(p)` | Releases the handle's storage. |

### 12.9 JSON

`std.json` (import it first, see §18) parses and serializes JSON. It maps JSON
onto Oskel's own values, so no new runtime type is introduced:

| JSON | Oskel |
| --- | --- |
| `null` | `nil` |
| `true` / `false` | `bool` |
| number | `int` when it is an integer literal in the 64-bit range, `double` otherwise |
| string | `string` |
| array | `array` |
| object | `map` |

| Function | Description |
| --- | --- |
| `parse(text)` | Parses `text`; a malformed document is a runtime error naming the position. |
| `isValid(text)` | `true` when `text` is a complete, well-formed JSON document. |
| `tryParse(text)` | Parses without aborting: `{ok, value, error}` (see below). |
| `stringify(value)` | Serializes `value` to compact JSON. |
| `stringify(value, indent)` | Pretty-prints with `indent` spaces per level (`0` is compact). |

Details:

- A JSON number becomes an `int` when the literal is an integer that fits the
  signed 64-bit range, and a `double` otherwise (a fraction, an exponent, or a
  value outside the range). `1e3` and `1.0` are `double`s; `42` and
  `-9223372036854775808` are `int`s.
- Strings are UTF-8. The parser decodes the `\uXXXX` escapes, combining a
  surrogate pair into one supplementary code point; a lone surrogate is a parse
  error (matching `chr`, which refuses to encode one as invalid UTF-8).
  `stringify` escapes the control characters and `"`/`\` and passes every other
  code point through unescaped.
- A duplicate object key keeps the last value, matching an Oskel map literal.
- `stringify` refuses the values JSON cannot spell: a non-finite number
  (`inf`/`nan`) and a value that is not one of the six above (a function, a
  range, an error) are a runtime error.
- The nesting depth is bounded at 256 levels; a deeper document is a parse
  error rather than a stack overflow.
- `tryParse` mirrors `pcall`'s shape: `{ok, value, error}` where `ok` is a
  `bool`, `value` is the parsed value on success, and `error` is a message on
  failure (or `nil`). `parse`, `isValid`, `tryParse` and `stringify` are
  `@MustUse` (see §7.7).

```oskel
import std.json.*;

let doc = {"name": "Ada", "age": 36, "tags": ["a", "b"]};
let text = stringify(doc);
print(text);                       // {"name":"Ada","age":36,"tags":["a","b"]}
print(parse(text).age);            // 36
print(stringify([1, 2], 2));       // [\n  1,\n  2\n]
print(isValid("{bad}"));           // false
```

`std.json` is written in plain Oskel over the portable primitive surface, so it
behaves identically on the development VM and under `oskel build` (the AOT
backend).

### 12.10 Dates and time

`std.datetime` (import it first, see §18) works with civil dates and wall-clock
times. Importing it makes the constructor functions `date` and `datetime`
visible, but a `Date` or `DateTime` is an ordinary map: its fields are read
with `.`, it prints as a map, and it compares by value. The module is pure Oskel
over the portable primitive surface, so it behaves identically on the
development VM and under `oskel build`.

| Function | Description |
| --- | --- |
| `date(year, month, day)` | A `Date`. The fields are validated. |
| `datetime(year, month, day, hour, minute, second, offset)` | A `DateTime`; the trailing fields default to `0`, and `offset` is minutes from UTC. |
| `now(offset)` | The current instant as a `DateTime` at `offset` minutes from UTC (0 by default). |
| `today(offset)` | The current civil day as a `Date`. |
| `isLeapYear(year)` | True when February has 29 days. |
| `daysInMonth(year, month)` | 28, 29, 30 or 31. |
| `toDays(d)` / `fromDays(n)` | Days between 1970-01-01 and the date, and its inverse. |
| `toSeconds(dt)` | Seconds since the epoch, *without* applying the offset. |
| `toEpoch(dt)` | Seconds since the epoch of the instant `dt` names (offset applied). |
| `fromEpoch(seconds, offset)` | A `DateTime` at `offset` minutes from UTC for `seconds`. |
| `toOffset(dt, offset)` / `toUtc(dt)` | The same instant at another offset. |
| `addDays(d, n)` / `addMonths(d, n)` / `addSeconds(dt, n)` | Calendar arithmetic (`n` may be negative). |
| `daysBetween(a, b)` / `secondsBetween(a, b)` | `b - a`. |
| `weekday(d)` / `weekdayName(d)` | 1 (Monday) to 7 (Sunday), and its name. |
| `monthName(d)` | The month's full name. |
| `dayOfYear(d)` | 1 through 366. |
| `isoWeek(d)` / `isoWeekYear(d)` | The ISO-8601 week number and week-numbering year. |
| `iso(value)` / `isoLocal(dt)` | ISO-8601 text; `isoLocal` omits the offset. |
| `format(value, pattern)` | A `strftime`-like template. |
| `parse(text)` | ISO-8601 text to a `Date` or `DateTime`; a malformed string is a runtime error. |
| `isValid(text)` / `tryParse(text)` | Test a string without aborting. |

```oskl
import std.datetime.*;

let d = date(2024, 2, 29);
print(iso(d));                     // 2024-02-29
print(daysInMonth(2024, 2));       // 29
print(toDays(d));                  // 19782
print(iso(addDays(d, 1)));         // 2024-03-01
print(weekdayName(d));             // Thursday

let dt = datetime(2024, 1, 2, 3, 4, 5, -300);
print(iso(dt));                    // 2024-01-02T03:04:05-05:00
print(toEpoch(dt));                // 1704182645
print(iso(fromEpoch(0)));          // 1970-01-01T00:00:00Z
print(format(dt, "%Y-%m-%d %A"));  // 2024-01-02 Tuesday
print(iso(parse("2024-06-15T12:30:45Z")));   // 2024-06-15T12:30:45Z

let todayDate = today();
print(iso(todayDate));             // the current date
print(iso(now(-300)));             // the current instant at UTC-5
```

A `DateTime` carries its UTC offset as **data**, not as a rule: the module never
consults a timezone database, so `toOffset`/`toUtc` are exact and a value is the
same on every machine. The clock comes from the `time()` builtin (§12.4), which
the AOT backend binds on every target.

### 12.11 Paths

`import std.path;` adds filesystem-path manipulation. A path is an ordinary
string, not a new type, so a result feeds the file builtins (§12.6) directly and
nests, compares and interpolates like any other string. The module is pure
Oskel, so it runs identically on the VM and under `oskel build`.

| Function | Description |
| --- | --- |
| `join(parts...)` | Joins the parts with `/`, dropping empty ones and collapsing the separators between them. |
| `basename(p)` | The last component, with trailing separators ignored. |
| `dirname(p)` | Everything up to the last separator (`.` for a bare name). |
| `extension(p)` | The last component's extension, including the dot (`.gz` for `a.tar.gz`). |
| `stem(p)` | The last component without its extension. |
| `withExtension(p, ext)` | Replaces the extension; a leading dot in `ext` is optional. |
| `withBasename(p, name)` | Replaces the last component. |
| `split(p)` | The path as an array of components (the root is its own element). |
| `isAbsolute(p)` / `isRelative(p)` | Whether the path starts with `/`. |
| `normalize(p)` | Resolves `.` and `..` and collapses separators, lexically. |
| `relative(from, to)` | The path that reaches `to` from `from`. |

```oskl
import std.path.*;

print(join("a", "b", "c"));            // a/b/c
print(join("/", "usr", "local", "bin")); // /usr/local/bin
print(basename("/a/b/c.txt"));         // c.txt
print(dirname("/a/b/c.txt"));          // /a/b
print(extension("a.tar.gz"));          // .gz
print(stem("/a/b/c.txt"));             // c
print(split("/a/b"));                  // ["/", "a", "b"]
print(normalize("/a/./b/../c"));       // /a/c
print(relative("/a", "/a/b/c"));       // b/c
```

The separator is `/` on every platform, and a `\` is folded to `/` on the way
in, so the module is deterministic and its results are the same on Linux, macOS
and Windows. Three cases deviate from POSIX `posixpath`, all deliberately:
`basename("a/b/")` is `b` (a trailing separator is ignored, as Rust's
`Path::file_name` does, so `basename` and `dirname` stay inverse),
`dirname("a")` is `.` (the parent of a bare name is the current directory), and
`join("a", "/b")` is `a/b` (a separator between parts collapses rather than
discarding the parts before it). `normalize` and `relative` match
`posixpath.normpath`/`relpath` exactly.

### 12.12 Collections

`import std.collections;` adds functional helpers over arrays: mapping,
filtering, folding, searching, reshaping, grouping and sorting. Every function
takes a **new** array or map and never mutates its input, so a caller can keep
the original and chain freely.

The array methods (`xs.map(f)`, `xs.filter(f)`, `xs.reduce(f, init)`,
`xs.sum()`, `xs.join(sep)`, `xs.push(x)`, ...) exist too and are the shorter
form; both engines bind them. The exceptions are `xs.sort()`/`xs.sorted()` and
the string methods (`str.upper`, `str.split`, ...), which `oskel build` compiles
and the binary then fails at run time with `value has no such member` (BACKLOG
§6 item 1). These functions are the same operations over the portable primitive
surface, so they work identically on the VM and in a native binary. **Prefer
them when a program must compile with `oskel build`.**

| Function | Description |
| --- | --- |
| `map(xs, f)` | A new array with `f(element)`. |
| `filter(xs, f)` / `reject(xs, f)` | The elements for which `f` is truthy / falsy. |
| `each(xs, f)` | Calls `f(element)` for side effects; returns `xs`. |
| `fold(xs, f, init)` | Left fold with `f(accumulator, element)`. |
| `sum(xs)` / `sumBy(xs, f)` | The sum of the elements / of `f(element)`. |
| `minimum(xs)` / `maximum(xs)` | The smallest / largest element, or `nil` when empty. |
| `minBy(xs, key)` / `maxBy(xs, key)` | The element whose key is smallest / largest, or `nil`. |
| `count(xs, f)` | How many elements satisfy `f`. |
| `find(xs, f)` / `findIndex(xs, f)` | The first element / index satisfying `f`, else `nil` / `-1`. |
| `any(xs, f)` / `all(xs, f)` / `none(xs, f)` | Whether some / every / no element satisfies `f`. |
| `take(xs, n)` / `drop(xs, n)` | The first `n` / the elements after the first `n` (clamped). |
| `partition(xs, f)` | `[yes, no]`: the elements that satisfy `f` and those that do not. |
| `chunk(xs, size)` | Consecutive groups of at most `size` elements. |
| `zip(a, b)` | Pairs of elements, stopping at the shorter array. |
| `flatten(xs)` | One level of nesting removed. |
| `reverse(xs)` | The elements in reverse order. |
| `unique(xs)` / `uniqueBy(xs, key)` | The elements with duplicates removed, keeping the first. |
| `groupBy(xs, key)` | A map from `str(key(element))` to the elements that produced it. |
| `countBy(xs, key)` | A map from `str(key(element))` to how many produced it. |
| `sort(xs)` / `sortBy(xs, less)` / `sortByKey(xs, key)` | A new sorted array. `sort` is stable. |

```oskl
import std.collections.*;

let nums = [5, 3, 8, 1, 9, 2, 8, 1];

print(filter(nums, x => x > 4));          // [5, 8, 9, 8]
print(fold(nums, (acc, x) => acc + x, 0)); // 37
print(sort(nums));                        // [1, 1, 2, 3, 5, 8, 8, 9]
print(unique(nums));                      // [5, 3, 8, 1, 9, 2]
print(groupBy(nums, x => x % 3));         // {"2": [5, 8, 2, 8], "0": [3, 9], "1": [1, 1]}
print(take(sortBy(nums, (a, b) => a > b), 3)); // [9, 8, 8]
```

A `groupBy`/`countBy`/`uniqueBy`/`*By` key is compared through `str`, because a
map key must be a string (§10); `2` and `"2"` therefore land in the same bucket,
which is the useful default for a grouping key. `sort` is a stable merge sort, so
two elements the comparator considers equal keep their input order. `sort`,
`minimum`/`maximum` and the `By` variants need the elements (or their keys) to be
mutually comparable: numbers with numbers, strings with strings.

---

## 13. Errors and diagnostics

Oskel distinguishes two kinds of errors:

- **Compile error**: detected by the lexer or the parser (for example, a missing `;` or a malformed literal).
- **Runtime error**: detected while the program runs (for example, an undefined variable, a division by zero or an out-of-range index).

Both are shown with the location and a code snippet:

```text
script.oskl:3:11: runtime error: division by zero
  |
3 | let x = 1 / 0;
  |           ^
```

Runtime errors also include a stack trace with the active functions. A deep
call chain (for example an accidental infinite recursion) builds thousands of
frames, so the trace prints the outermost ten and the innermost ten and elides
the middle with a `... N more ...` line; a short trace is printed in full.

A read of an undefined name suggests the full path when the name exists as a
member of a visible global table. `import std.math;` binds the namespace
`std.math`, not a bare `math` (see §18), so the common mistake of dropping the
root is caught with the fix spelled out:

```text
script.oskl:2:7: runtime error: undefined variable 'math' (did you mean 'std.math'?)
  |
2 | print(math.sqrt(4));
  |       ^
```

A third kind of diagnostic is non-fatal: a **warning**. A warning is reported
with the same snippet form but does not stop the program, which still compiles
and runs and keeps its exit code. The only warning today is the discarded result
of a `@MustUse` function (see §7.7).

```text
script.oskl:4:6: warning: the result of '@MustUse' function 'parse' is discarded; assign it to '_' to acknowledge
  |
4 | parse(text);
  |      ^
```

Process exit codes:

| Code | Meaning |
| --- | --- |
| `0` | Success. |
| `64` | Incorrect command-line usage. |
| `65` | Compile error. |
| `66` | The file could not be read. |
| `70` | Runtime error. |
| other | Code given to `exit(n)`. |

`oskel -c file.oskl` compiles the script without running it: the lexer, the parser, the compiler and import resolution all run, so syntax errors, type errors and a missing module are reported, but the VM never starts and no output is produced. Use it to validate a file in CI.

### 13.1 Recovering from an error: `pcall`

`pcall(fn, args...)` calls `fn` with the remaining arguments and turns a
runtime error into a value instead of aborting the program. It returns a map
`{ok, value, error}`:

- on success, `ok` is `true`, `value` is the return value and `error` is `nil`;
- on failure, `ok` is `false`, `value` is `nil` and `error` is an error value
  (see §13.2).

```oskel
let result = pcall(() => 1 / 0);
if (!result.ok) {
  print("failed: ${result.error}");            // failed: division by zero
  print(result.error.code);                    // 0
}

// Arguments are forwarded to the callee.
let sum = pcall((a, b) => a + b, 2, 3);
print(sum.value);                              // 5
```

Every runtime error is catchable, including `panic(...)`, an out-of-range
index, an undefined variable and an error raised deep in the call stack. A
host failure that is not a runtime error — an allocation the engine cannot
satisfy, such as `buffer(huge)` or an oversized `repeat` — is also turned into
an error value instead of aborting the process. After `pcall` returns, the
program keeps running and the engine stays consistent.

`pcall` is a function, not a keyword, so it compiles to an ordinary call: no
new syntax and no new bytecode. Passing a non-function as the first argument is
a caller bug, not a catchable error, and fails loudly.

`pcall` works the same way in a native binary (`oskel build`): the AOT runtime
reports a raised error by jumping back into the innermost active `pcall`, so the
program keeps control exactly as it does on the VM. The one difference is the
message: it is the bare text (`division by zero`), not the CLI's
`file:line:col: runtime error: ` decoration, which belongs to the process sink
that a caught error never reaches.

### 13.2 Errors as values

An error is a first-class value with its own runtime type, not a map that
happens to have the right shape. `error(message, code?)` builds one and
`isError(value)` detects it:

```oskel
let failure = error("not found", 404);
print(type(failure));        // error
print(failure.message);      // not found
print(failure.code);         // 404
print("${failure}");         // not found
print(isError(failure));     // true
```

The message is the display form, so `print(err)` and `"${err}"` render the
text; `repr` shows the constructor form `error("not found", 404)`. Two errors
are equal when their message and code match.

`isError` is exact: a user map such as `{ok: false, error: "x"}` is never
mistaken for an error. This is why the type exists instead of reusing a map —
a helper that returns an error must be distinguishable from one that returns
data with the same field names.

`error(...)` builds a value; it does **not** raise. To abort the program use
`panic(...)`. To catch a raised error use `pcall` (§13.1). Because errors are
ordinary values, they can be stored, returned, compared and carried through
containers like any other value.

Error values behave identically on the development VM and under `oskel build`
(the AOT backend): `error`, `isError`, `type`, `.message`/`.code`, equality,
`print`/`repr` and interpolation all agree, which is what makes value-based
`try`/`catch` (§13.3) work in a native binary. A raised runtime error, in
contrast, has no representation in the freestanding AOT runtime (no unwinding),
so only `error` values are recoverable there.

### 13.3 Propagating and recovering: `try` and `catch`

`pcall` turns a raised error into a value, but it is awkward for the common
case where a function wants to hand a failure back to its caller. Two
operators cover that, and both work on error **values** (the result of
`error(...)` or of a function that returns one):

- **`try expr`** — if `expr` is an error value, the enclosing function returns
  it immediately; otherwise `expr` is yielded unchanged. It is Rust's `?`.
- **`expr catch default`** — if `expr` is an error value, the whole expression
  is `default`; otherwise it is `expr`. It is Zig's `catch`.

```oskel
fn parse(s) {
  if (len(s) == 0) { return error("empty input"); }
  return 42;
}

// `catch` recovers locally, with a default.
print(parse("") catch 0);     // 0
print(parse("x") catch 0);    // 42

// `try` propagates to the caller.
fn double(s) {
  let n = try parse(s);       // returns the error from `double` if parse fails
  return n * 2;
}
let r = double("");           // the error value, not a crash
print(isError(r));            // true
print(r.message);             // empty input
print(double("x"));           // 84
```

Precedence and associativity:

- `try` is a prefix operator, binding like `-`, `!` and `~`, so `try f().x`
  propagates the result of the member access.
- `catch` binds looser than `??` and is left-associative, so
  `a() catch b() catch c` recovers at the first failure, and `a + b catch 0`
  is `(a + b) catch 0`.

Both are pure sugar over `isError` plus a jump, so they add **no opcode**:
`try` lowers to `CALL $isError` + `JMP_IF_FALSE` + `RETURN`, and `catch` to
`CALL $isError` + `JMP_IF_FALSE` + `MOVE`.

Two rules mirror `return`:

- `try` needs a function to return from: it is a compile error outside a
  function.
- `try` is rejected inside a `defer` block, because the block is inlined at the
  scope exit and returning from it would re-enter the teardown.

A propagating `try` leaves the function like a `return`, so every `defer` in
the function runs (LIFO) before the caller sees the error.

`catch` only recovers error **values**. A raised runtime error (for example
`1 / 0` or `panic(...)`) is not a value and is not caught by `catch`; use
`pcall` (§13.1) to turn those into values.

---

## 14. REPL

Running `oskel` with no arguments starts the REPL. Features:

- Expressions are evaluated and shown with `repr`; `nil` is not printed.
- `_` holds the last evaluated value.
- The `...>` prompt means continuation; a blank line cancels the incomplete input.
- The trailing `;` of each entry may be omitted.
- Definitions persist until `:clear`.

The session is semantic, not just a bag of values. An entry sees the
definitions of the entries before it exactly as if they were one accumulating
program: a `let` stays immutable, a `var` keeps its type annotation, a
`struct` stays usable as a literal or an annotation, and an `import` is bound
once. Redefining a name in a later entry is allowed and replaces the old
binding. An entry that fails to compile leaves no trace: it commits nothing,
so an import it started can be imported again cleanly. `:clear` drops the
whole session state with the definitions. A file run directly (`oskel
file.oskl`) always starts from an empty session; `:load` instead runs the file
in the current session, so it sees the session's definitions and merges its
own into it.

Commands:

| Command | Description |
| --- | --- |
| `:help`, `:?` | Help. |
| `:quit`, `:q`, `:exit` | Quit. |
| `:clear` | Delete the user definitions. |
| `:load <file>` | Run a script in the current session. |
| `:vars` | List the user's global variables. |

Example:

```text
oskel> let x = 10
oskel> let double = n => n * 2
oskel> double(x)
20
oskel> _
20
oskel> :vars
  _: int
  double: function
  x: int
```

---

## 15. Command line

```text
oskel                      starts the REPL
oskel <file.oskl>          runs a script
oskel -e "<code>"          runs code from the command line
oskel -c <file.oskl>       compiles the script without running it
oskel build <file.oskl>    compiles the script to a native binary (AOT)
oskelc <file.oskl>         the same compiler without the interpreter
```

| Option | Description |
| --- | --- |
| `-h`, `--help` | Help. |
| `-v`, `--version` | Version. |
| `-e`, `--eval <code>` | Code to run. The remaining arguments go to the script. |
| `-c`, `--check` | Compiles without running; reports syntax, type and import errors. |
| `-o`, `--output <file>` | Output path for `build`/`oskelc` (default: the script's base name). |
| `--target <name>` | Build target for `build`/`oskelc` (default: the host). |
| `--mode <name>` | Link mode for `build`/`oskelc` (default: `freestanding`). |
| `--link-lib <name>` | Library for the hosted link, passed as `-l<name>` (repeatable). |
| `--no-jit` | Disables the JIT (development engine only). |
| `--` | End of options; the next argument is the file. |

Options must come before the file. Everything after the file is considered an argument of the script and is obtained with `args()`.

```sh
oskel program.oskl one two
```

```oskel
for argument in args() {
  print(argument);
}
```

### 15.1 Script arguments

`args()` returns the arguments that follow the script name, as an array of
strings, in order. It never includes the script name itself and never includes
the interpreter's own options: it is Lua's `arg` table, not C's `argv`.

```sh
oskel program.oskl one two three
```

```oskel
assertEq(args(), ["one", "two", "three"]);
```

With no arguments the result is an empty array, not `nil`. With `-e`, the
arguments after the code are the script arguments; `--` ends the options, so
`oskel -- -weird.oskl a` runs a file whose name starts with `-`.

`oskel build` runs the full AOT pipeline: lex, parse, compile to bytecode and lower
that bytecode to machine code, then wrap it in a static executable. The emitted
binary is freestanding — it carries its own runtime and calls the Linux kernel
directly, so it has no dynamic dependencies (`ldd` reports "not a dynamic
executable") and runs on any Linux x86-64 without Oskel installed. It embeds only
the runtime, never the compiler, so a compiled program cannot `eval`.

```sh
oskel build app.oskl -o app   # writes ./app
./app
```

`oskel build` has two **link modes**, chosen on the command line with `--mode`
(the mode is a deployment choice, never syntax in the source — the same program
compiles either way):

- `--mode freestanding` (the default) is the self-contained image described
  above: it carries its own runtime, speaks to the kernel directly and has no
  dynamic dependencies.
- `--mode hosted` links the compiled program against the host C library, so an
  `extern "C"` symbol resolves to the real C function at link time. This is the
  mode that makes FFI work, and the mode `oskelc` emits. It needs a C toolchain at build time and produces a binary
  that *does* depend on libc, so it is not freestanding. The emitter wraps the
  program in a relocatable object and the host C compiler (`$CC`, default `cc`)
  links it against libc; the entry prelude reaches the system through libc
  (`write`/`exit`/`malloc`) and the program's entry is exported as the C `main`.

```sh
oskel build app.oskl                       # freestanding (default)
oskel build app.oskl --mode freestanding   # the same, stated explicitly
oskel build app.oskl --mode hosted         # links libc (FFI; PHASE 7f.4)
oskel build app.oskl --mode hosted --link-lib SDL3 -o app   # links SDL3 too
```

`--link-lib <name>` adds a library to the hosted link as `-l<name>`, which is
how an `extern "C"` symbol that lives in a third-party library resolves. It is
repeatable, it is only meaningful with `--mode hosted` (the freestanding image
has no linker), and the name is restricted to letters, digits, `_`, `+`, `-`
and `.` so it cannot smuggle a shell metacharacter into the `$CC` command. The
development engine does not use it: it links the library itself (CMake finds
SDL3, for example) and resolves the symbol through `Platform::lookupSymbol`.

The backend targets `x86_64-linux-sysv`, `x86_64-windows-win64`,
`x86_64-macos-sysv` and the bare-metal `x86_64-freestanding-sysv`; on any other
target `oskel build` reports the unsupported target instead of emitting bytes
for an ABI it cannot produce. The hosted mode is available on Linux, Windows
and macOS x86-64; macOS has no freestanding image, so there the default
`freestanding` mode is reported as unsupported and `--mode hosted` is required.
An unsupported (target, mode) pair is reported, never emitted.

A compiled binary carries the standard-library builtins that need no libm:
`print`/`write`/`eprint`, `len`, `type`, `str`, `repr`, `bool`, `ord`, `chr`,
`assert`, `panic`, `isError`, the numeric conversions `int`/`float`/`double`
(including the string forms) and the exact math builtins `floor`, `ceil`,
`round` and `sqrt` (correctly rounded from SSE alone, so they match the hosted
engine bit for bit). The remaining math builtins (`pow`, `sin`, `cos`, `tan`,
`log`, `exp`) embed the musl routines ported in `native/mathlib.*`, emitted as
x86-64 through the same `CodeGen` path as `dtoa` and `parse_float`, so the
freestanding binary stays libc-free. glibc is not correctly rounded for these,
so the contract is "agrees to 1 ulp".

A compiled binary supports variadic functions (§7.1): a trailing `rest...`
parameter binds an ordinary array of the extras, built by the runtime's
`oskel_pack_rest`, so a program using them behaves the same on the VM and in the
static binary.

### `oskelc`: the host compiler

`oskelc` is the same compiler behind a shorter command line. It links the lexer,
the parser, the bytecode compiler and the AOT backend, and never the VM, the JIT
or the builtins, so the compiler you ship carries no interpreter
(`DEVELOPMENT.md` §2, toolchain boundary). Both entry points call the same
pipeline, so the flags, the defaults, the diagnostics and the exit codes are the
same; the emitted image is byte for byte identical.

```sh
oskelc app.oskl -o app                     # freestanding (default)
oskelc app.oskl --mode hosted -o app       # links libc (FFI)
oskelc app.oskl --target x86_64-windows-win64 -o app.exe
```

`oskel` keeps `oskel build` as a thin alias, so an existing command line keeps
working and the two cannot drift:

```sh
oskel build app.oskl -o app                # the same binary as `oskelc`
```

**Planned (a later release)**:

```sh
oskelc <file.oskl> -c          compiles to bytecode (.oskc)
oskelc -pkg ... -o app.oskpkg  packages a .oskpkg
```

There is no `.oskc` serializer in the tree yet, so `-c` and `-pkg` are refused
by name with exit 64 rather than read as a typo. They are future work, not
features of this release.

---

## 16. Known limitations

- Dynamic typing. The `oskel` executable compiles source to bytecode and runs it on the register-based VM; the AST is never executed. The compiler and the VM implement the whole language surface this reference documents (PHASE 6 and its 6.5/6.6/6.7 additions).
- The JIT covers only a narrow integer subset, and only in the development engine. `oskel` (a script run, `-e` and the REPL) compiles a *leaf* function whose body only manipulates `nil`/`bool`/`int` to native x86-64 the first time it is called (PHASE 8, slices 1-4: constant folding, strength reduction, compare-branch fusion and web-based register allocation with copy coalescing). Everything else keeps running on the interpreter, so results never change, and `oskel build` and the generated binary never contain the JIT. `oskel --no-jit` disables it.
- Classes are supported (see §4.8): a `class` declares a named shape with methods, desugared by the parser into a top-level constructor function (a map plus closures capturing an explicit `self`), so it needs no new opcode. There is **no inheritance** and no interfaces; composition replaces them. A `struct` (data only, no methods) is the method-less form (see §4.7). Both are named map shapes checked at compile time.
- No `try`/`catch` *statement* and no catchable exceptions. Error handling is value-based: `nil` (not found), `pcall(fn, ...)` to turn a raised runtime error into a `{ok, value, error}` map (see §13.1), and the error-value operators `try expr` (propagate, Rust's `?`) and `expr catch default` (recover locally, Zig's `catch`, see §13.3). `panic()` is for unrecoverable errors. A raised runtime error is not a value and is not caught by `catch`; use `pcall` for that.
- Modules use Java-style `import` (see §18). `import` is only allowed at the top level of a module: nesting an import inside a function or block is a compile error. `require` (Lua-style) is dropped — `import` is the only module form. A module is loaded once, eagerly, when the program is compiled, and a cyclic import is a compile error rather than a crash.
- No `++`, `--` or default parameters. A trailing `rest...` parameter makes a function variadic (see §7.1). `switch` on literals and ranges is supported (see §6.10), and so is the post-test loop `do { ... } while (cond);` (see §6.4).
- Optional types and nullability are implemented for declarations and expressions (PHASE 6.5). Type annotations (`let x: int = 5`), local inference, the nullable `T?` annotation, and the null-safe expressions `?.`, `??` and `!!` are checked at compile time. `struct` is implemented (see §4.7) and so is `native fn` (see §7.6), the typed declaration of a host-provided function.
- FFI with C is implemented for the development engine (PHASE 6.6). C functions are declared with `extern "C" { ... }` blocks or the single-line `extern fn ...;` form (implicit `"C"` ABI); both desugar to the same representation. The FFI type vocabulary is documented in §4.6: the compiler validates each signature, records it on the proto, and the engine resolves the symbol through `Platform::lookupSymbol` and binds it to a marshalling native. The call marshals its arguments and result across the System V AMD64 ABI without `libffi` for scalars and structs by value. A `fn(P...): R` parameter is a callback: the development engine builds a `libffi` closure that re-enters the VM (its `R` may be a scalar or a struct by value). A trailing bare `...` declares a C variadic (`fn printf(fmt: cstring, ...): int`), which applies the C default argument promotions and goes through `libffi`; like a class-MEMORY struct it needs the System V AMD64 ABI plus `libffi`, and is rejected at compile time elsewhere.
- FFI in the AOT backend is partial (PHASE 7f, slices 7f.1-7f.9 done). A scalar `extern "C"` call resolves in the **hosted** link mode (`oskel build --mode hosted`, Linux, Windows and macOS x86-64), where the program is linked against the host C library; the freestanding mode, which is the default on Linux and Windows, still reports the unresolved symbol and exits 65. This is the intended design, not a defect: FFI is **hosted-only** under `oskel build` (see §4.5), because a freestanding image has no host to resolve a symbol against. Pointers, `cstring` returns and structs by value — both register-class and class-MEMORY — are lowered on the System V targets (Linux and macOS, 7f.6, 7f.7); the Windows x86-64 backend supports the scalar, `cstring` and `ptr` classes and rejects a struct, a callback or a C variadic with a dedicated message. A C variadic is lowered on the System V targets (Linux and macOS), and `native fn` declarations the backend has no stub for fail the build with a dedicated message. Callbacks are lowered on System V (7f.8): a scalar parameter or return crosses through a generated stub; a struct return or a struct parameter is rejected with a dedicated message.
- A function's text representation is the same on both engines: `<fn name>` for a named function, `<native fn name>` for a host binding (a builtin, a `native fn`, an `extern "C"` symbol or a bound collection method such as `<native fn array.map>`), and `<fn <anonymous>>` for a lambda. The AOT closure carries a `display` pointer to a static `AotString` with that exact text, so `print`, `repr`, `str` and string interpolation all agree byte for byte (BACKLOG §6.22, `tests/native_build_function_repr.oskl`).
- No UI library yet. Skia + SDL2 + declarative UI is planned for post-1.0.
- No async/await, generics, full pattern matching, generators, or transactions. These are out of scope (see `DEVELOPMENT.md`).
- Map keys are always strings. Literals accept identifier, string, scalar (`int`, `float`, `double`, `true`, `false`, `nil`) and computed `[expr]` keys; non-string keys are canonicalized to their string form.
- A body of `if`, `while`, `for` or `for-in` may be a block or a single statement without braces; a brace-less body is wrapped in a block, so its bytecode and scoping match the brace form. `switch` arms always require braces.
- `for ... in` accepts the loop variable and the iterable with or without parentheses (`for x in xs` and `for (x in xs)`).
- Slicing syntax `s[start:end:step]` works for strings and arrays, with an optional step (`s[::-1]` reverses); the methods `substr`/`slice` remain available.
- `upper` and `lower` apply the Unicode simple case mapping (see §9).
- Maximum call depth: 2000.
- Source nesting is bounded: deeply nested expressions and statements (nested parentheses, arrays, blocks, string interpolation, or a very long operator chain) are rejected at compile time with `expression nesting is too deep` instead of overflowing the stack. The limit is far above any nesting a real program reaches.
- Memory is managed with reference counting plus a cycle collector. Unreachable cycles (a function stored in the scope it closes over, an array that contains itself, **a bound method stored in its own receiver such as `a.f = a.push`**) are reclaimed. Destruction is deferred to an explicit work list, so a deeply nested structure — a container chain (`a = [a]` in a loop), a closure chain (each closure capturing the previous one) or any mix — is freed with a constant C++ stack depth instead of overflowing the stack.
- Sorting with custom comparators relies on the comparator defining a strict order.

---

## 17. Complete example

```oskel
// Counts words and shows the most frequent ones.

fn countWords(text) {
  let counts = {};
  for word in text.lower().split() {
    counts[word] = counts.get(word, 0) + 1;
  }
  return counts;
}

let text = "one two two three three three four four four four";
let counts = countWords(text);

let pairs = counts.entries();
pairs.sort((a, b) => a[1] > b[1]);

for pair in pairs {
  print("${pair[0]}: ${pair[1]}");
}
```

Output:

```text
four: 4
three: 3
two: 2
one: 1
```

## 18. Modules and `import`

Oskel imports are Java-style: the module is named by a **dotted path without quotes**, and `std.` is reserved for the standard library. Four forms exist.

```oskel
import std.math;                  // qualified: access is std.math.sqrt
import std.math.*;                // wildcard: access is sqrt
import std.math as m;             // alias: access is m.sqrt
import { sqrt, pow } from std.math; // selective: access is sqrt and pow
```

`import` is a declaration, not a function call, and it is only allowed at the **top level of a module**. Nesting an import inside a function or a block is a compile error.

### 18.1 The four forms

| Form | Meaning |
| --- | --- |
| `import a.b;` | Load `a/b.oskl` and bind it to the namespace `a.b`. Access is `a.b.name`. |
| `import a.b.*;` | Load `a/b.oskl` and add all its public names to the current scope. Access is `name`. |
| `import a.b as c;` | Load `a/b.oskl` and bind it to `c`. Access is `c.name`. |
| `import { x, y } from a.b;` | Load `a/b.oskl` and bind only `x` and `y`. Access is `x` and `y`. |

### 18.2 What a module exports

A module is a `.oskl` file. Its top-level declarations become the module's public names. A binding whose name starts with `_` is **private** and is not exported.

```oskel
// std/math.oskl
const PI = 3.141592653589793;

fn sqrt(x) {
  return x ** 0.5;
}

fn _helper() {   // private: not exported
  return 1;
}
```

A module's own top-level bindings are **local to the module frame**: importing a module never injects its internals into the importing program's globals. Only the names you import become visible.

The same holds for a module's own imports: the namespaces, aliases and wildcard names a module creates with `import` are private to that module. A module that imports `a.b` makes `a.b` visible to its own functions, but the importing program does not see `a.b` unless it imports it too. Likewise, a wildcard import inside a module never collides with a wildcard import in the importing program.

A module body runs **exactly once**, no matter how many times it is imported, and its exported table is shared by every import.

### 18.3 Conflict detection

Wildcard imports are safe because conflicts are caught at compile time. If two imports bind the same name, the compiler reports an error instead of silently letting one shadow the other:

```text
'sqrt' is ambiguous between std.math and std.stats; use 'std.math.sqrt' or 'std.stats.sqrt' to disambiguate
```

Conflict detection covers every pair of forms, not only two `.*` imports: a wildcard name that collides with an alias or with a namespace root is reported too, and so is an alias that collides with a namespace root. When both sides are unqualified names the error suggests the qualified form; otherwise it asks you to rename one of the imports.

A `struct` name is compile-time metadata, not a value binding, so it is not covered by the "a local binding wins" rule below: an import that binds the same name is a conflict and is reported. Otherwise `Name { ... }` would stay a struct literal (that form always wins) and hide the import, so `Name.member` would fail only at run time.

Two cases are *not* conflicts:

- Importing the same module twice (the same binding, whatever the form).
- Two dotted imports with the same root (`import a.b;` then `import a.c;`): they are sibling modules of one package and share the namespace table.

A name the importing module declares itself (`let`, `var` or `fn`) wins over an imported one, whatever the order of the declarations: an import never silently shadows a local binding. The two cases are distinct — a local binding is deliberate and wins; two imports colliding is a mistake and is reported.

### 18.4 Module resolution

A dotted path `a.b.c` maps to the file `a/b/c.oskl`, looked up in this order:

| Path | Resolves to |
| --- | --- |
| `std.*` | the standard library directory (`std/math.oskl`). `std.` is reserved: a user module under that prefix is a compile error. |
| anything else | the project directory (the directory of the file being compiled), then the installed packages (`$OSKEL_PACKAGES`, or `$HOME/.oskel/packages`). |

The standard library directory is found by looking in the working directory, then walking up from the running executable's directory (so `std/` is found whether `oskel` sits in the source tree, in `build/`, or in an install prefix), then `../share/oskel/std` next to the binary. `$OSKEL_STDLIB` overrides the search. `cmake --install` ships `std/` to `share/oskel/std`, so an installed `oskel` resolves `import std.math` from any working directory.

A path that resolves nowhere is a compile error: `module 'X' not found`.

Nested imports (a module that imports another module) resolve from the same project root. A module that imports itself, directly or through a chain (`a -> b -> a`), is a compile error: `cyclic import detected: a -> b -> a`.

### 18.5 Example

```oskel
import std.math;            // std.math.sqrt, std.math.pow, ...
import { sqrt } from std.math;

print(std.math.sqrt(16));   // 4
print(sqrt(25));            // 5
```

The standard library ships `std.math` (§12.3), `std.json` (§12.9),
`std.datetime` (§12.10), `std.path` (§12.11) and `std.collections` (§12.12). All
are ordinary modules, so the same four import forms apply:

```oskel
import std.json;             // std.json.parse, std.json.stringify, ...
import std.json.*;           // parse, stringify, ...
import std.json as json;     // json.parse, json.stringify, ...
import { parse, stringify } from std.json;
```

---

## 19. File extensions

Only `.oskl` (source) and the native binary exist in 0.1.0. The rest of the
table is the target naming, not something the compiler writes today: there is no
bytecode serializer, no package reader and no native-library loader yet.

| Extension | What it is | Java analog | Status |
|-----------|------------|-------------|--------|
| `.oskl` | Source code | `.java` | ✅ |
| `.oskc` | Bytecode | `.class` | planned |
| `.oskpkg` | Package (ZIP) | `.jar` | planned |
| `.osklib` | Native library | `.dll` / `.so` / `.dylib` | planned |
| `.oskdoc` | Documentation | `.javadoc` | planned |
| `myapp` | Native binary (Linux/macOS) | (GraalVM) `.exe` | ✅ |
| `myapp.exe` | Native binary (Windows) | (GraalVM) `.exe` | ✅ |

**`.oskpkg` format** (ZIP) — planned, not implemented in 0.1.0:

```text
myapp.oskpkg
├── META-INF/MANIFEST.MF
├── bin/main.oskc
├── lib/mylib.osklib
├── resources/icon.png
└── README.md
```

The `.oskc` bytecode layout, its checksum and the Level 1/Level 2 verification
steps are specified in `DEVELOPMENT.md` §8; all of it is planned, since nothing
serializes a `Bytecode` to disk yet — the compiler keeps it in memory and the
AOT backend lowers it directly.
