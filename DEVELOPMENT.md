# DEVELOPMENT.md — Oskel Project

Development guide for **Oskel**, a compiled programming language with a
Lua-style internal architecture: the language specification, the
architecture rules and the development phases.

---

## 1. Project identity

| Field | Value |
|-------|-------|
| **Name** | Oskel |
| **Extension** | `.oskl` |
| **Philosophy** | Simple like Python, structured like Java, fast like Lua, safe like Kotlin |
| **Implementation** | C++17 (standard library only; optional libffi, development engine only) |
| **Initial platform** | Linux x86-64 (musl static), then Windows/macOS |
| **Architecture** | Register-based bytecode + VM (dev engine) + AOT backend (production) |
| **Niche** | Desktop apps, CLI, UI (not systems, not backend, not data science) |

**Oskel is a general-purpose language** with a clear focus: desktop apps, CLI,
and UI. It **can** do backend, data science, games, or mobile (with FFI), but
those are not priorities.

---

## 2. Mandatory architecture

```text
.oskl → LEXER → PARSER → AST → COMPILER → BYTECODE ─┬─→ VM (dev engine)
                                                     └─→ AOT BACKEND → native binary
```

The VM is a development tool (REPL, fast iteration, differential testing).
oskelc produces a self-contained native binary (musl static by default,
no runtime to install); end users never install Oskel or any runtime.

The bytecode is designed from day one to be compiled to native. It is
low-level (40 opcodes, 1:1 or 1:2 lowering to x86-64) so the AOT backend can
generate efficient native code. Do NOT repeat Java's mistake (high-level
bytecode that later turns out to be impossible to compile efficiently).

### Modules and responsibilities

| Module | Input | Output | Status |
|--------|-------|--------|--------|
| `lexer.cpp` | source text | `Token[]` | ✅ |
| `parser.cpp` | `Token[]` | `Node*` (AST) | ✅ |
| `bytecode.cpp` | `Bytecode` | basic operations | ✅ |
| `compiler.cpp` | `Node*` | `Bytecode*` | ✅ |
| `module_resolver.cpp` | dotted module path | source file path | ✅ |
| `vm.cpp` | `Bytecode*` | execution | ✅ |
| `main.cpp` | CLI | full pipeline | ✅ (VM as dev engine, PHASE 4) |
| `jit/jit.cpp` | integer leaf proto | native x86-64 (dev engine only) | 🔶 slices 1-4 |
| `value.cpp` | `Value`, tagged union | shared runtime | ✅ |
| `builtins.cpp` / `ops.cpp` | native builtins, shared operators | runtime library | ✅ |
| `ffi.cpp` | `extern "C"` declarations | marshalling natives | ✅ PHASE 6.6 |
| `diagnostic.cpp` | `CompileError`, `Warning`, `RuntimeError` | stderr output | ✅ |
| `gc.cpp` | tracked `GcNode`s | cycle reclamation | ✅ |
| `platform_posix.cpp` | `Platform` ops (dev engine) | libc-backed I/O | ✅ |
| `native/codegen.cpp` | x86-64 instructions | machine-code bytes | ✅ PHASE 7a/7b |
| `native/lower.cpp` | bytecode | x86-64 (boxed `Value`) | ✅ PHASE 7c.3 (all opcodes) |
| `native/runtime.cpp` | boxed values | runtime helpers as machine code | ✅ PHASE 7c.3 |
| `native/dtoa.cpp` | float/double | shortest round-trip text | ✅ PHASE 7c.1b |
| `native/elf.cpp` | text + rodata | static ELF64 bytes | ✅ |
| `native/pe.cpp` | text + rdata | static PE32+ bytes (imports) | ✅ PHASE 7e |
| `native/arena.cpp` | block + size | aligned allocations | ✅ PHASE 7c.0 |
| `native/platform_freestanding.cpp` | `Platform` ops | direct Linux syscalls | ✅ PHASE 7c.0 |
| `native/entry.cpp` | program entry label | `_start`/entry + prelude (Linux syscalls, Windows kernel32) | ✅ PHASE 7c.0/7e |
| `native/bare_metal.cpp` | code + entry | multiboot ELF32/i386 kernel (bare-metal prelude + trampoline) | ✅ PHASE 7g |

### Toolchain binaries and the compiler boundary

`oskel` ships as a single binary today, and `oskel build` is the AOT entry
point inside it. PHASE 7d wired that subcommand into the CLI; the split into a
separate host compiler is still pending, so the two binaries below are the
target of that split, not the current state:

- `oskel` development engine (run, REPL, -c). Includes the VM and the JIT (the
  JIT uses the project's own `native/codegen.h` emitter, not AsmJIT).
- `oskelc` host compiler (oskelc file.oskl -o app). Reads sources and
  writes the ELF with the project's own `native/codegen.cpp` and
  `native/elf.cpp` (no AsmJIT, no ELFIO). Links against musl static. Planned;
  not part of 0.1.0.
- `oskel build` thin alias over `oskelc` (the goal; today it runs the AOT
  backend in-process)

A third, standalone binary ships alongside them: `bindgen`, the C binding
generator (`docs/bindgen.md`). It reads a C header and writes an Oskel
`extern "C"` module. It is a pure text transformation, so it does not link
`oskel_lib` (or the runtime, the compiler, the VM or libffi): its sources live
under `tools/` and are deliberately outside the `src/*.cpp` glob that builds the
engine, so it can be built and shipped on its own.

Boundary rule: src/compiler.* and src/native/* must not include
interpreter.h, vm.h or jit/jit.h. Enforced by tests/check_compiler_boundary.cmake.

---

## 3. Bytecode rules (CRITICAL)

### 3.1 Representation

1. **Register-based**, NOT stack-based. Registers are an array of dynamic
   values. All registers, globals, frames and temporaries use the same `Value`
   type. `Bytecode::constants` is a single tagged constant pool shared by the
   compiler and the VM. Arithmetic operations include fast paths for `int` and
   `float`. `Value` is a 16-byte tagged union with non-atomic reference
   counting (single-threaded). Strings are immutable and shared.

### 3.2 Lowering tiers

2. **Every opcode has exactly one direct lowering**, but not every lowering is
   a single machine instruction. Two tiers:

   | Tier | Lowering | Opcodes |
   |------|----------|---------|
   | **Inline** | 1–3 instructions, no call | `LOADK`, `MOVE`, `ADD`–`POW` (int/float fast paths), `UNOP`, `BAND`–`SHR`, `EQ`–`GTE`, `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`, `GETLOCAL`, `SETLOCAL` |
   | **Runtime primitive** | 1 call to a named helper | `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL`, `NEWTABLE`, `GETTABLE`, `SETTABLE`, `IN`, `ITER`, `PRINT`, `DECLLOCAL`, `GETGLOBAL`, `SETGLOBAL` |

   **Rule 1:1 / 1:2**: every opcode lowers to **1 instruction** (inline) or
   **2 instructions** (inline + fallback) or **1 call** (runtime primitive).
   **Never 3+ instructions.**

   A runtime primitive is **one small typed operation** (e.g.
   `oskel_table_get`), never the compiler, the AST or `eval`.

### 3.3 Opcode budget

3. **At most 40 opcodes** in total. **40 is a hard ceiling, not a target.**
   Enforced at compile time: `static_assert(kOpcodeCount <= 40)`.

   **Retirement ladder** (if a new opcode is needed):
   | Rung | Consolidation | Saves |
   |------|---------------|-------|
   | 1 | `EQ` `NEQ` `LT` `LTE` `GT` `GTE` → `CMP` | -5 |
   | 2 | `JMP_IF_FALSE` + `JMP_IF_TRUE` → `JMP_IF` | -1 |
   | 3 | `BAND` `BOR` `BXOR` `SHL` `SHR` → `BITOP` | -4 |
   | 4 | `ADD` `SUB` `MUL` `DIV` `MOD` `POW` → `ARITH` | -5 |

   **`PRINT` deletion (6f) frees one slot (39/40).** It does not postpone
   rung 1.

### 3.4 Forbidden instructions

4. **Forbidden**: `OP_EVAL_EXPR`, `OP_EXECUTE_STATEMENT`,
   `OP_CALL_METHOD_DYNAMIC`, `OP_IF`, `OP_WHILE`.

### 3.5 Allowed instructions (the complete 40)

5. **Allowed**:
   - Load: `LOADK`, `MOVE`
   - Arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `POW`, `UNOP`
   - Bits: `BAND`, `BOR`, `BXOR`, `SHL`, `SHR`
   - Comparison: `EQ`, `NEQ`, `LT`, `GT`, `LTE`, `GTE`
   - Variables: `GETGLOBAL`, `SETGLOBAL`, `DECLLOCAL`, `GETLOCAL`, `SETLOCAL`
   - Control: `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`
   - Functions: `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL`
   - Collections: `NEWTABLE`, `GETTABLE`, `SETTABLE`, `IN`
   - Iteration: `ITER`
   - I/O: `PRINT` (**temporary**, see §3.6)
   - End: `HALT`

### 3.6 `PRINT` is retired

`PRINT` was the only opcode that touched the outside world. It was retired in
PHASE 6f: every `print` arity (including the single-argument form) compiles to
`CALL` to the `print` builtin. The opcode is gone from the enum, the VM and the
compiler.

Removing `PRINT` freed one slot (39/40). The next opcode request (if any) starts
at rung 1 of the §3.3 ladder.

### Explicit anti-goal

> **Do NOT repeat Java's mistake**: high-level bytecode that later turns out
> to be impossible to compile to native code efficiently. Oskel's bytecode
> must be born with future native compilation in mind.

### Design rules for the AOT backend

1. **No VM-only features.** Every language feature must be compilable by the
   AOT backend. Runtime `eval`/`load` of bytecode is forbidden unless the
   produced binary embeds the compiler itself (same pitfall as GraalVM: a
   dynamic escape hatch that defeats static compilation).
2. **The bytecode must carry enough information for AOT.** Instructions stay
   annotatable with provable types so the backend can emit typed fast paths
   instead of falling back to full dynamic dispatch.
3. **The embeddable runtime cannot call libc.** The runtime that ships inside
   AOT binaries may only reach the outside world through the `Platform`
   interface (see PHASE 7): write, read, alloc, exit and file I/O go through
   it, never through libc or libstdc++ symbols.

### Expected bytecode → native lowering

This table is normative for PHASE 7. **Sequence** means inline instructions;
**call** means one call into the embedded runtime. The instruction count is a
budget, not a promise that every opcode is one instruction.

| Bytecode | Lowering |
|----------|----------|
| `LOADK R0, K1` | sequence: write tag + payload (2–3 insns) |
| `MOVE R0, R1` | sequence: tagged move (2 insns) |
| `ADD R2, R0, R1` | sequence: int/float fast path; generic path calls `oskel_add` |
| `SUB`–`POW` | sequence: same fast-path shape as `ADD`; generic path calls `oskel_arith` |
| `UNOP R0, R1, op` | sequence: 1–2 insns (negate/not/bit-not) |
| `BAND`–`SHR` | sequence: 1–2 insns (integer only) |
| `EQ`–`GTE` | sequence: `cmp` + `setcc`; generic path calls `oskel_compare` |
| `JMP +n` | sequence: `jmp rel32` |
| `JMP_IF_FALSE R2, +2` | sequence: `test`; `jz .label` |
| `JMP_IF_TRUE R2, +2` | sequence: `test`; `jnz .label` |
| `GETGLOBAL R0, K1` | call: `oskel_global_get` until globals are fixed slots |
| `SETGLOBAL R2, K3` | call: `oskel_global_set` until globals are fixed slots |
| `DECLLOCAL R0, 3` | call: `oskel_cell_new` (arena alloc) |
| `GETLOCAL R1, 10` | sequence: cell load + tagged load |
| `SETLOCAL R0, 10` | sequence: tagged store into the cell |
| `CALL R0, 2` | call: `oskel_call` (frame setup + dispatch) |
| `RETURN R0` | call: `oskel_return` (frame teardown) |
| `CLOSURE R0, 1` | call: `oskel_closure` (capture cells) |
| `GETUPVAL R0, 0` | call: `oskel_upval_get` (shared cell deref) |
| `SETUPVAL R0, 0` | call: `oskel_upval_set` (write barrier + cell store) |
| `NEWTABLE R0, 8` | call: `oskel_table_new` (arena alloc) |
| `GETTABLE R0, R1, R2` | call: `oskel_table_get` (bounds/hash/type checks) |
| `SETTABLE R1, R2, R0` | call: `oskel_table_set` (grow + write barrier) |
| `IN R0, R1, R2` | call: `oskel_contains` |
| `ITER R0, R1` | call: `oskel_iter_next` (writes the map value and the done flag) |
| `PRINT R3` | call: `oskel_print` → `Platform::write` (temporary, §3.6) |
| `HALT` | sequence: frame epilogue + `ret` |


---

## 4. Development phases (strict order)

You cannot move on to the next phase without completing and testing the
previous one.

### PHASE 1 — `include/bytecode.h` ✅
- Define `enum class OpCode` with the 40 opcodes listed in §3.5
- Define `struct Instruction { OpCode op; int a, b, c; int jump; int line, col; }`
- Define `struct Bytecode` with `code`, a tagged `constants` pool and `numRegisters`
- `constants` is a `std::vector<Value>` so `int`, `float`, strings, booleans and
  nil keep their type
- `LOADK` loads any constant (`nil`, booleans, numbers and strings)
- Functions: `bytecodeInit`, `bytecodeEmit`, `bytecodeFree` (declared in
  `include/bytecode.h`, implemented in `src/bytecode.cpp`)

### PHASE 2 — `src/compiler.cpp` (AST → bytecode) ✅
- Literals: every literal → `LOADK` with the value in the tagged pool
- `VariableExpr` → `GETGLOBAL`
- `BinaryExpr` with `+`, `-`, `*`, `/`, `%`, `**` and comparisons (`==`, `!=`,
  `<`, `<=`, `>`, `>=`) → compile left, compile right, emit the opcode
- `UnaryExpr`: `-` → `UNOP` negate, `!` → `UNOP` not, `+` emits no
  instruction, `~` → `UNOP` bit-not
- `&&`, `||`, `in` and bitwise operators → PHASE 5
- `AssignExpr` on a global variable → compile the expression, emit `SETGLOBAL`
- Compound assignment (`+=`, `-=`, `*=`, `/=`, `%=`, `**=`) → `GETGLOBAL`,
  operation, `SETGLOBAL`
- `LetStmt` → compile the initializer; if there is none, load an explicit nil
  constant; then `SETGLOBAL`
- Compile-time immutability tracking (`isImmutable` on locals and globals):
  assigning to a `let` binding throws a
  `CompileError` with the same message as the retired AST oracle
- `print(expr)` → compile the expression, emit `PRINT` (temporary; see §3.6;
  every arity becomes a `CALL` to the builtin at 6c/6e); in the current AST
  `print` is a call, so it is detected as a `CallExpr`
- Register allocation with a counter that goes up and down
- Update `numRegisters` when reserving registers
- Reuse of constants in the tagged pool; `int` and `float` are distinct
  constants
- Nodes not yet supported throw `CompileError` with line/column

### PHASE 3 — `src/vm.cpp` (run bytecode) ✅
- Loop `while (pc < count)` + `switch` on the opcode
- `std::vector<Value> registers_` sized by `numRegisters`
- Global variable table `std::unordered_map<std::string, Value>`
- Numeric fast paths in `ADD`, `SUB`, `MUL`, `DIV` without walking variants
- `ADD` also concatenates strings; `DIV` and `MOD` detect division by zero
- `POW` replicates `intPow` for `int ** int` with a non-negative exponent and
  uses `std::pow` in every other case
- `UNOP` respects the retired oracle's semantics (`0` is truthy) for negate, not
  and bit-not
- Comparisons with `EQ`/`NEQ`/`LT`/`LTE`/`GT`/`GTE` and the same ordering
  relation the oracle used (`lessThan`)
- `PRINT` uses `Value::toString()` (temporary; removed at the 6f gate, see
  §3.6)
- Runtime errors through `RuntimeError`
- `run()` resets the registers and keeps the globals between executions
- Each opcode: 2-3 lines of C++

### PHASE 4 — `src/main.cpp` (VM as the dev engine) ✅
- Lua/LuaJIT model: `oskel`, `oskel -e` and the REPL **always** compile to
  bytecode and run on the VM as the **development mode**. The AST is never
  executed in production.
- REPL with **a single persistent VM**: each input is compiled to a chunk and
  run on the same globals.
- The temporary `--ast` differential flag was removed from the binary once
  VM/AST parity was reached; it is gone.
- `oskel -c file.oskl` compiles without running.
- `oskel build file.oskl` → PHASE 7 AOT backend: freestanding static native
  binary (no libc), no Oskel installation needed by end users.
  `--target x86_64-windows-win64` emits a Windows PE32+ (PHASE 7e) and
  `--target x86_64-freestanding-sysv` a bare-metal multiboot kernel (PHASE 7g);
  the default target follows the host.

### PHASE 5 — Control flow and locals ✅
- Opcodes: `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`
- Jumps with backpatching (absolute target inside `code`)
- `if`/`else` (including `else if`), `while`, classic `for`, `break` and
  `continue` (compile-time error outside a loop)
- Block scopes and local variables with slots (`DECLLOCAL`, `GETLOCAL`,
  `SETLOCAL`); `Bytecode::numLocals`
- Locals live in reference-counted `Cell`s so the PHASE 6 closures capture the
  variable, not just its value. A closure owns a reference to every cell it
  captures (`Function::upvalues`), so a cell lives exactly as long as its frame
  or a closure that captured it: the VM keeps no registry of captured cells
- Top-level `var`/`let` remain globals (they persist in the REPL); `const`
  bindings are folded to literals by the parser and never reach the bytecode
- `&&`, `||` with real short-circuiting (jumps), ternary with jumps
- Bitwise operators: `BAND`, `BOR`, `BXOR`, `SHL`, `SHR`, `BNOT`
- `+x` emits no instruction; `~x` requires an integer
- Do **NOT** add `OP_IF` or `OP_WHILE` (high level)
- `in` and `for-in` are left for PHASE 6 (they depend on collections)

### PHASE 6 — Functions, collections and library 🔶 in progress ← NEXT
- `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL`
- Call stack in the VM and registers per frame
- `Bytecode` evolves into **per-function protos** with child protos (Lua style)
- Closures with upvalues over `Cell` (locals are already cells)
- Shared `Runtime` interface so that the builtins (`map`, `filter`, `sort`) can
  invoke VM functions without re-entering a separate engine
- Arrays, maps, indexing, members, ranges, interpolation and `for-in`
- `in` for arrays, maps, strings and ranges
- Retire the temporary `PRINT` opcode: from 6c/6e every `print` arity compiles
  to `CALL`, and the opcode is deleted at the 6f gate (§3.6)
- Already in the tree: the shared `Value` (`value.cpp`) plus the cycle collector in `gc.cpp`. Slice 6a is done: top-level `fn` declarations, parameters, `return`, recursion and general calls compile and run on the VM. Slice 6b is done: `GETUPVAL`/`SETUPVAL`, upvalue descriptors, lambdas and closures that escape their defining scope (verified by `examples/closures.oskl` and `tests/09_lambdas.oskl`). The compiler still rejects collections and `for-in` with a `(Phase 6)` message; those land in 6c.
- Memory in the VM: closures that capture each other's variables, and tables that contain themselves (directly or through a closure stored in them), form cycles that reference counting cannot free. A `Cell` registers with the cycle collector when a closure captures it, and `Function` must report its `upvalues` (the captured cells) as well as `capturedGcNodes` from `visit`; a missing edge makes every cycle through a cell immortal. The VM runs `gcCollectIfNeeded()` at `CLOSURE` and `NEWTABLE`, the instructions that create the objects cycles are made of, so a program that never creates a closure is still collected. Regression tests: `testCapturedCellsAreNotRetainedByTheVM`, `testClosureCyclesAreCollected`, `testTableCyclesAreCollectedWithoutClosures` and `testClosureStoredInTheTableItCapturesIsCollected` in `tests/vm_test.cpp`; build it with `-DOSKEL_GC_STRESS` to collect at every safe point.
- **6f done**: `PRINT` retired. Every `print` arity compiles to `CALL`. The
  opcode count is now 39/40.

### PHASE 6.5 — Types and nullability (1.0)

- Optional types: `let x: int = 5` — **done**: annotations on declarations,
  parameters and return types, checked at compile time and then **erased**
  (identical bytecode with or without the annotation, so the VM and the AOT
  backend never see a type). Accepted names are the `type()` primitives plus
  `any`; `int` widens to `float`. See `docs/language.md` §4.6 and
  `tests/16_types.oskl`.
- Local type inference: `let x = 5` (x: int) — **done**: the type is inferred
  from literal initializers and from other typed bindings, and later
  assignments are checked. A `nil` initializer stays untyped; an expression
  whose type is not evident is left unchecked.
- Nullability: `string?` — **done** as an annotation (a `?` type accepts
  `nil`). The expression forms are **done** too: `a ?? b` (null coalescing,
  short-circuiting), `a?.b` (safe access, read-only) and `a!!` (non-null
  assertion). Only `nil` triggers them (not truthiness), and none adds an
  opcode: `??`/`?.` lower to `EQ`/`JMP_IF_*`/`GETTABLE`/`MOVE`, `!!` to a call
  to the hidden `$nonNull` helper. See `docs/language.md` §4.6 and §5.8.1 and
  `tests/17_nullability.oskl`.
- `defer { ... }` — **done**: the deferred block is inlined at every exit from
  its scope (fall-through, `return`, `break`/`continue`), LIFO order, no new
  opcode and no VM support. `defer` is rejected outside a function, and a
  `return`/`break`/`continue` that would leave the deferred block itself is
  rejected. See `docs/language.md` §6.11 and `tests/15_defer.oskl`.
- **Modules: Java-style `import`** — **done** (`import std.math;`,
  `import std.math.*;`, `import std.math as m;`,
  `import { sqrt } from std.math;`) with compile-time
  conflict detection across every import form (not only two `.*` imports: an
  alias or a namespace root that collides with another import is reported too)
  and a reserved `std.` prefix. A module is loaded
  once, eagerly; a cyclic import (`a -> b -> a`, including a self-import) is a
  compile error rather than a stack overflow. A module's own imports are
  private to it: a namespace, alias or wildcard name it creates never reaches
  the importing program's globals, and never collides with the importer's own
  wildcard bindings. Resolution tries the project directory first, then the
  packages root (`$OSKEL_PACKAGES`, else `~/.oskel/packages`), so a project module
  shadows an installed package of the same path. `require` (Lua-style) is
  **dropped**: `import` is the only module form. See `docs/language.md` §18,
  `tests/13_modules.oskl` and `tests/module_test.cpp`.
- **`struct`** (data only, no methods) — **done**: a named, module-level map
  shape with optional field types. The declaration emits no bytecode and adds
  no opcode; a `Point { x: 1, y: 2 }` literal is checked against the
  declaration (unknown, missing, duplicate or mistyped field is a compile
  error) and then lowered to the same `NEWTABLE`/`SETTABLE` sequence as a map
  literal, so a struct value is a plain map at runtime. A field may use the
  shorthand `Point { x, y }` (sugar for `Point { x: x, y: y }`, from the
  variables in scope), a parser-only desugaring. A struct name is a valid
  annotation and is visible to the whole file (like a hoisted `fn`). See
  `docs/language.md` §4.7 and `tests/18_structs.oskl`.
- `switch` simple (literals and ranges) — **done**: no fallthrough, labels are
  literals, integer ranges or `const` names (a top-level `const`, or a local
  `const`/`let` in the same frame; the compiler rejects any other name, so a
  label stays a fixed value), optional single `default`; compiles to the
  existing `EQ`/`IN`/`JMP` opcodes (no new opcode). See `docs/language.md` §6.10.
- **`native fn`** for internal FFI (stdlib in Oskel) — **done**: a bodyless,
  module-level declaration (`native fn sqrt(x: float): float;`) that binds a
  name to a function the host provides. It emits no bytecode and adds no
  opcode (the binding is a `GETGLOBAL` of the host function); the signature is
  checked at call sites (arity and argument types) and the declared return type
  feeds inference. A parameter with no annotation (or `any`) turns the check
  off for that argument. See `docs/language.md` §7.6 and
  `tests/19_native_fn.oskl`.
- **`void` return type** — **done**: `fn f(): void` and `native fn free(p):
  void;` declare a function that produces no value, mapping directly to C's
  `void` in the FFI (PHASE 6.6). It is a return-only annotation (rejected on
  variables, parameters and fields), compile-time metadata only: a `void`
  function still returns `nil` at run time, so no opcode and no runtime type is
  added. `return expr;` is rejected in a `void` function (use a bare
  `return;`), and a `void` expression cannot be used as a value. See
  `docs/language.md` §4.6 and `tests/27_void.oskl`.
- **Variadic parameters** — **done**: a trailing `rest...` parameter
  (`fn sum(first, rest...)`) binds an array holding every extra argument; the
  `...` follows the optional element type (`rest...` or `parts: int...`) and
  must be the last parameter. A variadic function accepts `n - 1` or more
  arguments; the VM packs the extras into an array, so no opcode is added and
  the bytecode count stays 39/40. The same rule applies to `native fn`
  signatures. See `docs/language.md` §7.1 and `tests/20_variadic.oskl`.
- **Standard library: `std.math`** — **done**: the primitives that need C++
  (`sqrt`, `pow`, `sin`, `cos`, `tan`, `log`, `exp`, `floor`, `ceil`, `round`)
  are declared `native fn` in `std/math.oskl` and bind to the host builtins of
  the same name; `abs`, `min`, `max` (variadic) and `clamp` are plain Oskel, and
  `PI`/`E` are constants. Moving the four C++ builtins out keeps the host
  registry small while the standard library stays mostly Oskel. See
  `docs/language.md` §12.3/§18 and `tests/21_std_math.oskl`.
- **Standard library: `std.json`** — **done**: a pure-Oskel module for parsing
  and serializing JSON. It maps JSON onto Oskel's own values (null → `nil`,
  number → `int`|`double`, object → `map`, array → `array`), so no new runtime
  type, opcode or builtin is added, and exposes `parse`, `isValid`, `tryParse`
  and `stringify` (compact or indented). The module is written over the portable
  primitive surface (indexing, slicing, `len`, `ord`/`chr`,
  `int`/`double`/`str`/`type`, `panic`, `for ... in`, closures), avoiding the
  collection/string methods and `pcall`/`error` the AOT backend does not
  implement yet, so it behaves identically on the VM and under `oskel build`.
  See `docs/language.md` §12.9/§18, `tests/55_std_json.oskl` and the AOT
  fixture `tests/native_build_json.oskl`.
- **Standard library: `std.datetime`** — **done**: a pure-Oskel module for civil
  dates and wall-clock times. A `Date`/`DateTime` is an ordinary map built by
  the exported constructor functions (`date`, `datetime`), so no new runtime
  type, opcode or builtin is added beyond `time()` (below). It offers the
  `days_from_civil` algorithm, epoch conversion, calendar arithmetic,
  differences, components, a `strftime`-like `format` and ISO-8601
  `parse`/`iso`/`isValid`/`tryParse`. The module is written over the portable
  primitive surface, avoiding the collection/string methods and `pcall`/`error`
  the AOT backend does not implement yet, so it behaves identically on the VM and
  under `oskel build`. See `docs/language.md` §12.10/§18,
  `tests/56_std_datetime.oskl` and the AOT fixture
  `tests/native_build_datetime.oskl`.
- **Builtin `time()` and the AOT wall clock** — **done**: `time()` returns the
  seconds since the Unix epoch as an `int` (`clock()` stays monotonic and
  untouched). The AOT runtime binds it as the `now` handle on every path — a
  syscall on Linux, `time(NULL)` hosted, `GetSystemTimeAsFileTime` on Windows
  (a new kernel32 import), and the CMOS real-time clock in a bare-metal kernel
  (the RTC's BCD/binary fields converted to a Unix timestamp in assembly) — so
  `std.datetime.now()`/`today()` work everywhere. See `docs/language.md` §12.4
  and `tests/bare_metal_test.cpp` (`test_qemu_wall_clock`).
- **Standard library: `std.path`** — **done**: a pure-Oskel module for
  filesystem paths. A path is an ordinary string, not a new type, so a result
  feeds the file builtins directly and no new runtime type, opcode or builtin is
  added. It offers `join`, `basename`, `dirname`, `extension`, `stem`,
  `withExtension`, `withBasename`, `split`, `isAbsolute`/`isRelative`,
  `normalize` and `relative`. The separator is `/` on every platform and a `\`
  is folded to `/`, so the results are deterministic across Linux, macOS and
  Windows; `normalize`/`relative` match `posixpath.normpath`/`relpath` exactly,
  with three documented deviations elsewhere (`basename`, `dirname`, `join`).
  The module is written over the portable primitive surface, avoiding the
  collection/string methods and `pcall`/`error` the AOT backend does not
  implement yet, so it behaves identically on the VM and under `oskel build`.
  See `docs/language.md` §12.11/§18, `tests/57_std_path.oskl` and the AOT
  fixture `tests/native_build_path.oskl`.

### PHASE 6.6 — FFI with C (1.0)

- `extern "C" { ... }` blocks, and the single-line `extern fn ...;` form
  (implicit `"C"` ABI); both are sugar for the same representation.
- Compile-time stubs (like Zig/Rust).
- **No `libffi` in the AOT backend.** The development engine (`oskel`) may use
  libffi for callbacks; the freestanding AOT runtime never does (callbacks are
  compile-time stubs there).
- Support for ints, floats, strings, structs, callbacks.
- **FFI type vocabulary (done)** — the type names needed to *spell* a C
  signature: `ptr` (`void *`), `cstring` (`const char *`), the fixed-width
  integers (`int8`/`16`/`32`/`64`, `uint8`/`16`/`32`/`64`), `uint` and
  `size_t`. The integer family is a compile-time annotation erased before the
  bytecode: the runtime value stays an `int` and `type()` keeps reporting the
  primitive name. `checkAssignable` knows `nil` is NULL for a pointer (even
  without a `?`), `cstring` upcasts to `ptr`, and the integer family is
  mutually assignable and widens to `float`/`double`. See
  `docs/language.md` §4.6 and `tests/28_ffi_types.oskl`.
- **Pointer runtime representation (done)** — `ptr`/`cstring` is the one FFI
  type with a runtime value of its own: `VType::Ptr` backed by a
  reference-counted `PtrObj` (`PtrKind::Void`/`CString`/`Buffer`). It is an
  *opaque handle*, never a machine address, so a program can pass it to a
  native and read it back but can never dereference arbitrary memory (Oskel is
  memory-safe) and the allocation stays visible to the collector. `PtrObj`
  holds only raw bytes, so — like `StringObj`/`ErrorObj` — it is **not** a
  `GcNode` and can never sit in a cycle. `typeName()` reports `cstring` or
  `ptr` from the handle's kind, a `cstring` upcasts to `ptr` keeping its kind,
  and pointers compare by identity. The development engine exposes the
  storage through the builtins `cstring`, `cstr`, `buffer`, `byteLen`,
  `loadByte`, `storeByte`, `intToPtr`/`ptrToInt` and `free` (a released handle
  is a clean runtime error, not a use-after-free). No new opcode: these are
  ordinary host natives. The AOT backend (PHASE 7) lowers the same handles to
  real C pointers. See `docs/language.md` §4.6, `tests/30_ffi_ptr.oskl` and
  `tests/value_test.cpp`.
- **`extern` declarations and marshalling (done)** — a C function is declared
  with an `extern "C" { fn ...; }` block or the single-line `extern fn ...;`
  form. The single-line form is sugar for a one-entry block with an implicit
  `"C"` ABI, so both spellings parse into the same `ExternBlockStmt` (no new AST
  node and no new opcode). The block's ABI is a plain string literal and only
  `"C"` is accepted. Each entry is a bodyless `fn ...;` stored as a `FnDeclStmt`
  with `isNative`, so it follows exactly the `native fn` path: the name binds to
  the host function of the same name and the signature is checked at the call
  site. A declaration emits no bytecode (it compiles to the bytecode of the
  equivalent `native fn`), the declarations are hoisted, and they are
  module-level metadata (nesting one is a compile error). The engine resolves
  each declared name through `Platform::lookupSymbol` and binds it to a
  marshalling native that passes the arguments and reads the result across the
  System V AMD64 ABI **without `libffi`** (the scalar/struct call path uses
  compile-time thunks; only callbacks use libffi, and only in the development
  engine): integers, bools and pointers in general-purpose registers,
  `float`/`double` in SSE registers, and the result read back at the exact
  declared width. A parameter may be spelled `string` as well as `cstring`: a
  Oskel string is NUL-terminated and alive for the whole call, so it is a valid
  `const char *`; a `string` is not a valid *return* type (C returns a
  `cstring`). See `docs/language.md` §4.6 and `tests/29_extern_c.oskl` /
  `tests/33_extern_single.oskl`.
- **Structs by value (done)** — a `struct` (see `docs/language.md` §4.7) crosses
  the C boundary as a parameter and as a return value. The compiler resolves the struct's fields
  from its declaration into a `ffi::TypeSpec` on the proto's `ExternSymbol`, and
  `ffi_types.h` computes the C layout (natural alignment, field offsets) and the
  System V classification into up to two eightbytes, each INTEGER (a GPR) or SSE
  (an xmm register). A struct argument is flattened to those eightbytes and
  passed in the registers the ABI assigns them (an INTEGER eightbyte as an
  `int64_t`, an SSE eightbyte as a `double`, both bit-for-bit), so the callee
  reads back the exact bytes; a struct return is captured through a fixed POD
  whose members land in `rax`/`rdx`/`xmm0`/`xmm1` and rebuilt as a map. Only
  structs of **16 bytes or fewer** are supported: a larger one is class MEMORY
  (passed on the stack), which the register thunks cannot express, so the
  declaration is rejected at compile time, as is a nested (non-scalar) field.
  No new opcode and no compiler-side ABI logic: the layout is the single source
  of truth in `ffi_types.h`, shared by the compiler and the runtime across the
  §2 boundary. Struct-by-value is **System V AMD64 only**: the register
  classification is that ABI's, and the Microsoft x64 ABI (Windows) passes a
  struct larger than 8 bytes through a hidden pointer instead, a shape the
  register thunks cannot express. Off System V the compiler rejects the
  declaration (`kStructByValueSupported` in `ffi_types.h`; the CMake capability
  `OSKEL_FFI_STRUCT_BY_VALUE` skips the script test), so a Windows build fails
  cleanly at compile time instead of crashing at the call. Scalars stay portable
  because the call is a typed function pointer. See `docs/language.md` §4.6 and
  `tests/31_ffi_struct.oskl`.
- **Callbacks (done)** — a parameter typed `fn(P...): R` is a C function
  pointer. `fn` is a keyword, so `parseTypeAnnotation` reads the signature
  before the ordinary type-name path; the parameters are scalar FFI names and
  the return is a scalar or a struct by value (a nested callback, a non-scalar
  callback parameter and a struct *parameter* inside the callback are out of
  scope). A callback is
  only meaningful as a parameter: a `fn` type as the *return type of the
  `extern "C"` function* is rejected at compile
  time, as is a non-scalar callback parameter. In the development engine the
  callback is a `libffi` closure (`ffi_closure_alloc`/`ffi_prep_closure_loc`):
  the trampoline re-enters the VM through `Runtime::call`, boxing the C
  arguments as Oskel values and writing the result back in the declared C type
  (a struct return is serialised field by field into the return storage using
  its layout, and needs the by-value struct marshalling, plus the `libffi` path
  for a class-MEMORY struct).
  The closure lives in a reference-counted `CallbackObj` held for the whole
  extern call, so the trampoline address stays valid while C runs (a callback C
  stores for a later call is out of scope for this phase). An error raised
  inside a callback cannot unwind through the C frame, so the extern call
  captures it and re-raises it once C returns; `pcall` catches it like any other
  runtime error. `nil` is the NULL function pointer. `libffi` is a
  **development-engine-only** dependency (§5.1/§6): the AOT backend lowers
  callbacks to compile-time stubs and never links it. The boundary check
  (`tests/check_compiler_boundary.cmake`) forbids the compiler and the native
  backend from including `ffi_marshal.h` or `libffi`, so the split cannot
  regress. See `docs/language.md` §4.6, `tests/32_ffi_callback.oskl` and
  `tests/vm_test.cpp`.
- **C variadics (done)** — a trailing bare `...` declares a C variadic
  (`extern "C" { fn printf(fmt: cstring, ...): int; }`): the named parameters
  are the fixed prefix and every extra argument is passed through the default
  argument promotions (`float` widens to `double`, `bool` to `int`, a string to
  `const char *`). The spelling is a bare `...`, told apart from the Oskel
  `rest...` parameter pack (which collects into an array and cannot cross the
  boundary, so it is rejected in an `extern` declaration). A variadic
  declaration is recorded on the `ExternSymbol` (`variadic`), and the compiler
  checks only that the fixed prefix is present — the tail is not type-checked,
  because C promotes and consumes it at run time. The call always goes through
  `libffi` (`ffi_prep_cif_var`, prepared per call because the argument count
  changes), so like a class-MEMORY struct it needs `kCVariadicSupported`
  (libffi + System V AMD64); off that the declaration is rejected at compile
  time (`OSKEL_FFI_VARIADIC` skips the script test). See `docs/language.md`
  §4.6 and `tests/53_ffi_variadic.oskl`.

### PHASE 6.7 — Error handling 🔶 in progress

Value-based error handling, no catchable exceptions: the AOT runtime has no
unwinding, so a feature the freestanding backend cannot compile would be
forbidden by the "no VM-only features" rule (§3). The design combines Rust
(propagation, must-use) and Zig (local recovery) with a Oskel syntax that does
not collide with the existing ternary `?` or bitwise `|`.

Slices, each independently shippable:

- **6.7.1 `pcall` (done)** — `pcall(fn, args...)` calls `fn` and returns a map
  `{ok, value, error}` instead of aborting. It is a plain builtin: no new
  token, node, parser rule or opcode, and no new VM logic (it reuses
  `Runtime::call` and the unwinding `catch(...)` already in `VM::call`). This
  is the value-based core; the rest is sugar over it.
- **6.7.2 dedicated `error` value type (done)** — `VType::Error` is a real tag
  (10 types), so `isError` is exact instead of the `ok == false` heuristic: a
  user map with `{ok: false}` is never mistaken for an error. An `ErrorObj`
  carries `message` and `code`; it holds no references, so like a string it
  cannot sit in a cycle and is not a `GcNode`. Touches `Value` (the union,
  the three private `switch`es), `typeName`/`equals`/`writeValue`, the GC
  (no new edge, so no change), and `ops.cpp` (`err.message`/`err.code`).
  Builtins `error(message, code?)` and `isError(value)`. `pcall` now returns
  an error value in `result.error` instead of a string. There is no `.oskc`
  serializer in the tree, so serialization is out of scope. Tested by
  `tests/24_error_type.oskl` and the C++ `testErrorValues` in `vm_test.cpp`.
- **6.7.3 `try expr catch default` (done)** — prefix `try` (propagation) and postfix
  `catch` (local recovery), Zig-style. Prefix `try` is chosen over postfix `?`
  because `?` already starts the ternary `a ? b : c`. Compiles to
  `CALL isError` + `JMP_IF_FALSE` + `RETURN`/`MOVE`: no new opcode. `try` obeys
  the `return` rules (a function is required, `defer` is rejected), runs the
  deferred blocks on the propagation path, and its conditional `RETURN` is
  tracked so a trailing `return try f();` still emits the normal return. The
  hidden `$isError` helper keeps the lowering independent of a shadowed public
  `isError`. Tested by `tests/25_try_catch.oskl`, the `try_*` error cases and
  `testTryCatchBytecode`/`testTryCatchErrors` in `compiler_test.cpp`.
- **6.7.4 unions `T | E` (done)** — `|` is accepted only in type position; in
  expression position it stays `BOR`, so no token changes. `TypeRef` gains an
  `alternatives` list; `nil` is **not** stored as a member, it folds into the
  existing `nullable` flag, so `int | nil` is exactly `int?` and the two
  spellings stay interchangeable. Assignment applies the single-type rule per
  alternative: a value fits the union when it fits **at least one** member
  (`int` still widens to `float`), and a union source fits the target only when
  **every** member fits. `catch` now infers the union of its two sides, which
  is what makes a fallback of a different type check without a cast. A union is
  compile-time only and has no C representation, so it is rejected in `extern
  "C"`/`native fn` signatures and in struct fields (`void` cannot be a member
  either). Tested by `tests/34_unions.oskl`, the `union_*` error cases and
  `testUnionTypes` in `compiler_test.cpp`.
- **6.7.5 `@MustUse` (done)** — a warning (never a hard error) when a
  `@MustUse` return is discarded, matching Rust `#[must_use]` and Kotlin
  `@CheckReturnValue`. A hard error would violate the `docs/language.md` §4.6
  principle that inference never rejects a program that would have run. `_ = expr`
  acknowledges a discard. `@` is a new lexer token (`Tok::At`), the only
  grammar change: `@MustUse` precedes a `fn` or `native fn` and sets a
  `mustUse` flag on the node, so it emits no bytecode. The compiler collects
  warnings (`errors.h` `Warning`, `printCompileWarning` in `diagnostic.*`,
  `Compiler::warnings()`), and the CLI prints them to stderr while the program
  still runs. Tested by `tests/35_must_use.oskl`, the `unknown_attribute` and
  `attribute_before_let` error cases and `testMustUse` in `compiler_test.cpp`.

### PHASE 7 — Bytecode → native backend (AOT, freestanding) 🔶 in progress
- New module `src/native/` + `include/native/`: lowering of every opcode to
  x86-64 following the bytecode → native lowering table of §3.
- **Own machine-code and static ELF executable emitter** (~500-1000 lines:
  ELF header, program headers, `.text`, `.rodata`, entry point), keeping the
  zero-dependency rule of §5.1/§6. LIEF, AsmJit and LLVM were evaluated and
  rejected: LIEF is built to parse/modify binaries (not generate them), and
  AsmJit/LLVM break the no-external-dependencies rule.
- **Freestanding**: its own `_start` entry point (argc/argv read from the
  stack, `exit` via syscall), direct syscalls (`write`, `read`, `openat`,
  `close`, `mmap`, `brk`, `exit_group`), a custom arena/bump allocator on top
  of `mmap`, and no libc or libstdc++ linked into the generated binary —
  `ldd` must report "not a dynamic executable". Custom dtoa/formatting for
  printing floats in the `print` builtin.
- **Platform layer**: extract a minimal `Platform` interface from
  `vm.cpp`/runtime (write, read, alloc, exit, file I/O) with two
  implementations: `PlatformPosix` (dev engine; may use libc) and
  `PlatformFreestanding` (AOT; direct syscalls only). The `Runtime` of slice
  6a.0 (PHASE 6) must be built on this interface from the start.
- **Target descriptor from day one**: `include/native/target.h` defines
  `Target { arch, os, abi }` and every backend entry point takes one.
  `CodeGen` validates it and accepts `x86_64-linux-sysv`,
  `x86_64-windows-win64` and `x86_64-freestanding-sysv`, so adding macOS or
  WebAssembly is a new backend behind the same descriptor instead of a driver
  rewrite.
- Initial targets: Linux x86-64 (direct syscalls), Windows x86-64
  (kernel32 through the Microsoft x64 ABI) and bare metal x86-64 (multiboot,
  no operating system). All three share the x86-64 lowering and the embedded
  runtime byte for byte; only the entry prelude differs, because each reaches
  the outside world differently.
- Subslices:
  - **7a** — "hello world" spike: hand-written `_start` + `write` syscall →
    static freestanding ELF → `./hello` runs, no libc.
  - **7b** — lowering of the opcodes that need no runtime, integer-only:
    `LOADK` (int/bool), `MOVE`, `ADD`/`SUB`/`MUL`, `BAND`/`BOR`/`BXOR`,
    `UNOP`, `EQ`–`GTE`, `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE` and `HALT`.
    R0–R11 map to x86-64 registers (r15 is the scratch); comparisons and
    `!x` take three instructions (`cmp`/`test` + `setcc` + `movzx`) because
    x86-64 cannot materialize a full-width 0/1 in one instruction — the
    documented exception to the 1:1/1:2 budget. Floats need value tags and
    move to 7c; `SHL`/`SHR` need `cl` (R1) and would clobber the window;
    `DIV`/`MOD`/`POW` need the error path and libm.
  - **7c** — freestanding embedded runtime calls (strings, the `print`
    builtin, collections on top of the custom allocator) and the boxed
    `Value` representation that unlocks floats and the rest of the opcodes.
    - **7c.0** (done) — the freestanding substrate: the static ELF64 emitter
      (`include/native/elf.h`, `src/native/elf.cpp`) and the PE32+ emitter
      (`include/native/pe.h`, `src/native/pe.cpp`); the bump allocator
      (`include/native/arena.h`, `src/native/arena.cpp`); `PlatformFreestanding`
      (`include/native/platform_freestanding.h`,
      `src/native/platform_freestanding.cpp`), the second `Platform`
      implementation, reaching the kernel through direct Linux x86-64 syscalls
      (`write`, `read`, `openat`, `close`, `lseek`, `mmap`, `exit_group`) with
      no libc; and the entry point (`include/native/entry.h`,
      `src/native/entry.cpp`), which appends the `_start` and the runtime
      prelude (`oskel_write`, `oskel_exit`) to the same code buffer as the
      program so every `call` resolves through the emitter's labels, with no
      relocations and no linker. Verified end to end in
      `tests/freestanding_test.cpp`: the test emits a program, wraps it in an
      ELF, writes it to disk, runs it and checks its stdout, exit code and
      that `ldd` reports no dynamic dependencies.
    - **7c.1a** (done) — the boxed value model. A bytecode register is no
      longer a physical CPU register: it is a 16-byte slot in a register file
      on the stack (`[rbp - 16*(i+1)]`), holding an `AotValue { tag; payload }`
      (`include/native/value.h`). Sixteen bytes is the System V ABI's
      two-register size (rdi:rsi in, rax:rdx out), so a helper takes and
      returns a value without touching memory, and the old 12-register ceiling
      is gone. The embedded runtime (`include/native/runtime.h`,
      `src/native/runtime.cpp`) emits, as machine code in the same buffer as
      the program, the helpers the opcodes call: integer arithmetic
      (`+`/`-`/`*`), bitwise (`&`/`|`/`^`), unary (`-`/`!`/`~`), comparison
      (equality for any pair, ordering for int/int and bool/bool, all
      type-aware) and truthiness. `oskel_print_value` renders int/bool/nil with
      an integer-to-text routine of our own (no libc); a type mismatch reports
      "unsupported operand types" on stderr and exits 70, matching the
      development engine. `lowerToX64` now returns a `LoweredChunk` (the code
      image plus the chunk and `_start` offsets) and emits the entry prelude
      and the runtime alongside the program. Verified in `tests/lower_test.cpp`
      (execution: loads, arithmetic, bitwise, comparisons, truthiness, jumps,
      a backward loop, 40 registers) and end to end in
      `tests/freestanding_test.cpp` (a real static ELF that prints
      `42`, `-7`, `true`, `false`, `nil`, and a type error that exits 70).
    - **7c.1b** (done) — floats and integers division/shift over the boxed
      model. The `AotValue` gains the `Double` tag next to `Float`
      (`include/native/value.h`); a float/double payload is the IEEE-754
      bit pattern of a binary64 value, so a `Float` stores a binary32 value
      widened exactly the way the hosted `Value` does. `LOADK` of a
      float/double constant now lowers to a `movabs` of the bit pattern.
      `oskel_arith` implements `+`/`-`/`*`/`/`/`%` with the int/float/double
      promotion of `ops::binary`: int/int uses 64-bit wrapping arithmetic
      (with the `INT64_MIN / -1` overflow special-cased), anything with a
      float promotes to double and a float/float result rounds back to
      binary32, so `0.1f + 0.2f == 0.3f` holds. Division by zero (int and
      float) reports an error and exits 70. `oskel_shift` lowers `SHL`/`SHR`
      (arithmetic `>>`, count range-checked to 0..63), `oskel_pow` lowers
      integer `**` with a non-negative exponent (`intPow`), and `oskel_negate`
      negates a float by flipping the sign bit. Numeric equality and ordering
      are numeric across int/float/double (`1 == 1.0` is true), matching
      `Value::equals`/`lessThan`. Float `%`, float `**` (libm `fmod`/`pow`)
      and float-to-text for `print` stay deferred to 7c.1c.
    - **7c.1c** (done) — strings over the arena. The `Str` tag
      (`include/native/value.h`) carries the address of an `AotString`, an
      8-byte length followed by the bytes (no NUL terminator, so an embedded
      NUL survives, like `std::string`). `LOADK` of a string constant no
      longer throws: `lowerToX64` collects the bytes as data islands at the
      end of the image and lowers the load to a `lea` of the header.
      `oskel_print_value` writes a string's bytes verbatim; equality is content
      equality (`oskel_str_equals`) and ordering is lexicographic over unsigned
      bytes with a proper prefix smaller (`oskel_str_order`), both matching
      `ops::binary`/`lessThan`. `_start`/the chunk map a 1 MiB anonymous
      private region (`oskel_init_arena`) and keep the bump cursor/end in
      `r13`/`r14`; `oskel_alloc` bumps it and `oskel_concat` joins two strings
      with `rep movsb`. `oskel_arith` concatenates when both operands are
      strings and the op is `+`. Float `%` lands in 7c.1d.1 (x87 `fprem`);
      float `**` and float-to-text for `print` (own dtoa) follow in 7c.1d.2/3.
    - **7c.1d.1** (done) — float `%`. `oskel_arith` implements `Mod` for
      float/double operands with the x87 unit: SSE has no remainder
      instruction, so the operands cross through the stack (`movsdFromXmm` /
      `fldM64`) and `fprem` reduces them in a loop until its C2 (incomplete)
      bit clears, which is exactly `std::fmod` (the truncated remainder, sign
      following the dividend) — bit-for-bit, unlike x87 `pow`. A zero divisor
      reports the same "division by zero" error as `/`. The x87 stack is
      drained on every path, so the rest of the backend stays SSE. `codegen`
      gains `fldM64`/`fstpM64`/`fstpSt0`/`fprem`/`fnstswAx`/`testAhImm8`,
      covered byte-for-byte in `tests/codegen_test.cpp`; `tests/lower_test.cpp`
      runs the remainder and `tests/freestanding_test.cpp` checks it inside a
      real static ELF.
    - **7c.1d.3** (done) — float-to-text for `print`/`str` (own dtoa). The
      hosted engine prints a `double` with the shortest text that round-trips
      through binary64 (`std::to_chars`, e.g. `0.1 + 0.2` → `"0.3"`) and a
      `float` with the shortest binary32 round-trip (`0.1f` → `"0.1"`,
      `1.0f/3.0f` → `"0.33333334"`). `src/native/dtoa.cpp` implements a
      freestanding Dragon4 (Steele & White free-format) over a small fixed
      bignum (32-bit limbs in 64-bit slots), emitted as x86-64 through the
      shared `CodeGen` — no libc, no `printf`, no table of powers of ten. It
      handles both widths, including subnormals (a widened binary32 subnormal
      is a *normal* double, so the float path rebuilds the original 23-bit
      fraction from the double significand) and reproduces the
      fixed-versus-scientific choice of the hosted formatter. The emitter gains
      32-bit load/store, immediate `and`/`imul`, immediate shifts, `bsr` and a
      raw `div`. `oskel_print_value` routes `Float`/`Double` through
      `oskel_format_double` into a stack `Scratch` block plus text buffer and
      writes the returned bytes. `tests/dtoa_test.cpp` maps the emitted code and
      compares it with `std::to_chars` over millions of random values;
      `tests/codegen_test.cpp` covers the new encodings byte for byte, and
      `tests/freestanding_test.cpp` prints doubles and floats from a real static
      ELF.
    - **7c.1d.2** (done) — `**` on floats. `std::pow` is *correctly rounded*
      (glibc, ~0.5 ulp) and the hosted engine calls it. `oskel_pow` now takes
      the same int/float/double promotion as `ops::binary`: int/int with a
      non-negative exponent keeps the wrapping integer loop (`intPow`), and
      every other numeric pair (a float/double operand, or a negative exponent)
      converts to binary64, calls the embedded musl `pow` already used by the
      `pow` builtin, and re-tags the result with `makeNumeric`'s rank rule —
      binary32 only when at least one operand is a float and none is a double,
      binary64 otherwise. The result type is decided *before* the call, since
      `mathlib::pow` clobbers the volatile registers holding the operand tags.
      `mathlib::pow` is correctly rounded to under 1 ulp, so the printed values
      match glibc on the fixtures. `tests/lower_test.cpp` runs the four
      promotion cases and `tests/native_build_pow.oskl` checks a real static
      ELF against the VM, `type` included.
    - **7c.2a** (done) — globals. `GETGLOBAL`/`SETGLOBAL` lower to a flat
      `AotValue` table the chunk allocates from the arena once at entry (its
      base lives in r15, a callee-saved register no helper touches, so no
      writable data segment is needed). The compiler names a global by a string
      constant; the lowerer derives the name-to-slot map from the code in
      first-assignment order, so the `Bytecode` structure and the `.oskc` format
      are unchanged and the binary carries no global names. `oskel_init_globals`
      clears every slot to nil (the hosted engine's "unset" state),
      `oskel_global_set`/`oskel_global_get` are one shift and a store/load. A
      `GETGLOBAL` of a name the chunk never assigns is a `LoweringError` naming
      the variable -- the compile-time counterpart of the hosted engine's
      run-time "undefined variable". `tests/lower_test.cpp` runs globals
      (assign, reassign, distinct slots, a string global) and drives the real
      compiler into the lowerer; `tests/freestanding_test.cpp` exercises the
      table inside a real static ELF. Locals and calls stay for 7c.2b/7c.2c.
    - **7c.2b** (done) — locals. `DECLLOCAL`/`GETLOCAL`/`SETLOCAL` lower to a
      second region of the chunk frame, right below the register file: a local
      is a plain 16-byte `AotValue` slot at `[rbp - 16*(numRegisters + b + 1)]`,
      so `DECLLOCAL`/`SETLOCAL` are a tagged store and `GETLOCAL` a tagged load,
      with no `Cell`, no arena and no helper. This is sound while closures are
      absent: a local is only captured through `CLOSURE`, which is still a
      `LoweringError` (7c.2c), so a captured local cannot reach the backend. The
      hosted engine's "undefined local variable" check needs a null cell, which
      a stack slot cannot express; a well-formed chunk (the compiler always
      emits `DECLLOCAL` first) is assumed, and that is the one semantic
      difference. The frame grows to `16*(numRegisters + numLocals)` bytes, still
      a multiple of 16. Locals appear even without a call: a nested block or a
      classic `for` header at the top level is a local scope, so `lower_test.cpp`
      runs them directly and drives the real compiler into the lowerer (a nested
      block, a `for` loop), and `freestanding_test.cpp` runs a locals chunk
      through the real `_start`. Calls and closures stay for 7c.2c.
    - **7c.2c.1** (done) — calls to bytecode functions. Every proto in the tree
      is emitted as its own machine function with the lowering's own convention
      `AotValue entry(AotValue* args, long argc, AotClosure* self)`, which the
      `CALL` opcode dispatches through `oskel_call`. `CALL` passes the callee
      (rdi:rsi), the argument count (rdx) and the address of its argument window
      (rcx, a `lea` of `R[a+1]`); `oskel_call` checks the callee is a `Func` and
      indirect-calls its code with (args, argc, self). The callee copies its
      arguments into its own register file -- consecutive registers sit 16 bytes
      *below* each other, so `args[i]` is at `args - 16*i` -- and checks `argc`
      against its declared arity at run time (the compiler does not), reporting a
      mismatch through `oskel_arity_error`. A non-function callee is
      `oskel: cannot call a non-function value` (exit 70). `CLOSURE` allocates an
      `AotClosure` (`code`, `arity`, `upvalueCount = 0`) with `oskel_closure`;
      `RETURN` publishes `R[a]` and drops the frame. Globals are keyed by *name*,
      not constant index, because each proto has its own constant pool: a
      recursion (`fact` reading `fact`) resolves to one slot across protos. The
      root frame saves rbx/r12-r15 and maps the arena; a function frame saves only
      rbp, because the arena cursor/end (r13/r14) and the global base (r15) are
      the program's state, not the call's -- an allocation inside a function must
      advance the shared cursor. Covered by `tests/lower_test.cpp` (add, `fact`
      recursion, mutual recursion, a capture-free lambda) and
      `tests/freestanding_test.cpp` (a call through the real `_start` and the
      non-function error).
    - **7c.2c.2** (done) — closures with upvalues. A local a child proto
      captures lives in a shared *cell* (a 16-byte `AotValue` in the arena)
      instead of an inline frame slot, so the closure and the defining frame
      mutate the same variable, exactly like the hosted engine's `Cell`. A slot
      no child captures stays inline, so an ordinary local still costs no
      allocation. `oskel_cell_new` allocates the cell; `DECLLOCAL` allocates a
      fresh cell and stores the value, `SETLOCAL`/`GETLOCAL` store/load through
      it, and a plain slot keeps the direct 16-byte store/load. `oskel_closure`
      now takes `(code, arity, upvalueCount)` and reserves the capture slots;
      the lowering writes each captured cell pointer with a plain store, reading
      it from a local slot of this frame or, for a re-exported capture, from this
      frame's own `self`. The frame reserves one extra slot below the locals for
      the `AotClosure*` (`self`, passed in rdx) that `GETUPVAL`/`SETUPVAL` and a
      nested `CLOSURE` resolve through. A frame no longer saves the arena state:
      the root owns rbx/r12-r15, a function frame saves only rbp. Covered by
      `tests/lower_test.cpp` (escaping closure, a shared-cell counter, two
      closures over one variable, nested capture through two frames, a captured
      parameter, creation-time independence) and `tests/freestanding_test.cpp`
      (a captured local read through the real `_start`).
    - **7c.2d** (done) — the `print`/`write`/`eprint` builtins. The backend had
      no builtins, so a call to `print` was a lowering error ("global 'print'
      is read but never assigned") and a compiled binary could not print. The
      lowering now seeds each builtin the program reads as an `AotClosure` in
      its global slot, so a `CALL` reaches it through the ordinary `oskel_call`
      path. A builtin is a machine-code function with the proto convention
      `AotValue entry(AotValue* args, long argc, AotClosure* self)`; it joins
      its arguments with single spaces, `print`/`eprint` add a trailing newline
      (`write` does not) and `eprint` writes to stderr. The printer is factored
      into one core parameterised by fd and newline. Covered by
      `tests/lower_test.cpp` (a `print` call lowers and returns nil) and
      `tests/freestanding_test.cpp` (a compiled program prints through `print`
      and `write` under the real `_start`).
    - **7c.3** (done) — collections and `for-in`. `NEWTABLE` allocates an array
      (`c == 0`) or a map (`c == 1`) header in the arena; `GETTABLE`/`SETTABLE`
      cover both the dynamic-index form (`R[b][R[c]]`) and the constant-member
      form (`R[b].name`, the name encoded as `-c - 1`) through `oskel_table_get`/
      `oskel_member_get` and their setters, mirroring `ops::indexGet`/
      `ops::memberGet` (negative array indices from the end, appending at the
      length, maps keyed by string with content comparison via `oskel_str_eq`).
      `IN` lowers to `oskel_contains` (array value equality through
      `oskel_compare`, map keys, string substrings) and `ITER` to
      `oskel_iter_next`, which advances a `for-in` cursor over an array, a map's
      keys or a string's UTF-8 code points; for a map it also writes the paired
      value straight into the loop variable's slot (the `valueOut` argument), so
      the key/value form (`for k, v in map`) needs no helper call. Growth goes
      through a shared
      `oskel_block_ensure` (next power-of-two capacity, arena copy). Printing a
      container uses a recursive `oskel_print_repr` (`[1, 2]`, `{"k": 1}`,
      quoted strings, `<fn>`), depth-capped at 8 so a self-referential
      structure prints `...` instead of recursing forever; nested strings reuse
      the interpreter's escapes through `oskel_print_str_escaped`. Covered by
      `tests/freestanding_test.cpp` (arrays, maps, indexing, `.length`, `in`,
      `for-in` and container printing under the real `_start`) and
      `tests/lower_test.cpp` (the collection opcodes now lower instead of
      raising).
    - **7c.3b** (done) — array concatenation (`a + b`). The hosted `binary`
      `Plus` on two arrays builds a fresh array with A's items followed by B's
      items and leaves both operands unchanged; `oskel_arith` rejected the pair
      as "unsupported operand types", so a compiled binary disagreed with the
      VM (the VM concatenated, the AOT binary exited 70). `oskel_arith` now
      dispatches `+` on two arrays to the new helper `oskel_array_concat`,
      which allocates an `AotArray` of `lenA + lenB` items, grows its block
      once through `oskel_block_ensure` and copies both runs in with two
      element loops (the operands are read only). No new opcode: it is the
      `ADD` path the string concatenation already uses, extended to the array
      tag. Covered by `tests/lower_test.cpp` (empty and non-empty operands,
      operand immutability, a growing accumulator) and the `oskel build`
      fixture.
    - **7c.3c** (done) — bound array methods `arr.push`/`arr.pop`. The hosted
      engine binds both to a native closure that captures its receiver:
      `push` appends its arguments in place and returns the array, `pop`
      removes and returns the last item (or nil when empty). The AOT
      `oskel_member_get` only knew `.length`, so `a.push` failed at run time
      with "value has no such member". `oskel_member_get` now builds a bound
      method exactly like the hosted `bindArray`: the receiver goes into a
      16-byte cell (`oskel_cell_new`, the same shared cell a captured local
      uses) and the method is an ordinary `AotClosure` whose single upvalue is
      that cell, so a `CALL` reaches the body through the existing
      `oskel_call` path. No new opcode and no compiler change: the member form
      of `GETTABLE` plus `CALL` already carries it, so the `.oskc` is
      untouched. Covered by `tests/lower_test.cpp` (push/pop as calls and as
      detached first-class values, an empty-array `pop`, a growing/draining
      accumulator) and the `oskel build` fixture.
    - **7c.3d** (done) — negative string indexing. `ops::indexGet` normalizes a
      string index like an array index (`normalizeIndex`): a negative index
      counts UTF-8 code points from the end, so `"abc"[-1]` is `"c"`. The
      string branch of `oskel_table_get` passed the raw index straight to
      `utf8CharAt`, which only walks forward, so a compiled binary reported
      "index out of range" and exited 70 on a program the VM accepts. The
      branch now rebases a negative index against the code-point length
      (`oskel_utf8_len`) before the lookup; a positive index skips the rebase
      and an out-of-range index still reports the error, so the VM and the AOT
      backend agree. No new opcode: it is the `GETTABLE` path the array branch
      already normalizes. Covered by `tests/freestanding_test.cpp`
      (`test_negative_string_index`, both the values and the error).
    - **7c.3e** (done) — the remaining in-place and query array methods:
      `shift`, `unshift`, `insert`, `removeAt`, `contains`, `indexOf` and
      `clear`. They bind exactly like `push`/`pop` (PHASE 7c.3c): the receiver
      goes into a shared cell and the method is an `AotClosure` over that one
      upvalue, so a `CALL` reaches the body through `oskel_call`. None of them
      allocates a new array, so they need no new runtime helper beyond the
      arena the array already lives in. `shift`/`removeAt` return the removed
      item (or nil when empty), `unshift`/`insert`/`clear` return the receiver,
      and `contains`/`indexOf` scan with structural equality
      (`oskel_value_equal`), so an array or map needle compares by content, as
      in the VM. A negative index counts from the end
      (`ops::normalizeIndex`). `oskel_member_get` dispatches the nine bound
      names through a selector-indexed table instead of one branch per name,
      and the failure paths report the same messages as the hosted binder
      (bad arity, `insert()/removeAt() index out of range`) and exit 70. No
      new opcode and no compiler change: the member form of `GETTABLE` plus
      `CALL` already carries it, so the `.oskc` is untouched. Covered by
      `tests/lower_test.cpp` (`testExecArrayMethodsInPlace`), the freestanding
      `test_array_methods_inplace` (values and the error paths) and the
      `oskel build` fixture.
    - **7c.4a** (done) — the hidden `$nonNull` and `$isError` helpers. The
      compiler emits `$nonNull` for the non-null assertion `a!!` and `$isError`
      for `try` / `x catch y` (PHASE 6.5/6.7); the AOT backend had no binding
      for them, so a program using either failed to build with
      `lowering: global '$nonNull'/'$isError' is read but never assigned`. They
      are now seeded builtins like `print`:
      `oskel_builtin_non_null` returns its operand or calls `fail` with the
      hosted helper's message, and `oskel_builtin_is_error` returns `false`
      (the AOT runtime has no error values yet, so the success path of `try`/
      `catch` runs; a real failure still exits 70). Covered by
      `tests/freestanding_test.cpp` (both helpers under the real `_start`, plus
      the failing `!!` exiting 70) and the `oskel build` fixture.
    - **7c.4b** (done) — the hidden `$str` helper for string interpolation and
      computed keys. It renders the operand to an `AotString` in the arena,
      byte-identical to the hosted `Value::toString` for nil, booleans,
      integers (with the sign), floats/doubles (through the embedded dtoa) and
      strings (returned unchanged, so a string interpolates without quotes);
      a collection or function operand fails with a clear message instead of
      producing different text (deferred to 7c.4b.2). Covered by
      `tests/freestanding_test.cpp` and the `oskel build` fixture.
    - **7c.4b.2** (done) — `$str` for collections and functions (recursive
      repr). The printer's traversal is factored into `oskel_repr_into`, which
      appends the representation of a value to a string builder; `print` and
      `$str` both go through it, so `"${[1, 2]}"` and `print([1, 2])` agree
      byte for byte (nested strings quoted, maps as `{k: v}`, functions as
      `<fn>`, depth capped at 8). `oskel_print_repr` renders into a stack
      builder and writes once; `oskel_repr_to_string` renders into an arena
      `AotString`. The string builder (`oskel_sb_*`, `AotStringBuilder`) grows
      by doubling in the arena. A function's *name* is not part of the AOT
      closure yet, so `$str`/`print` render `<fn>` where the VM renders
      `<fn name>`; that is pre-existing and tracked separately. Covered by
      `tests/freestanding_test.cpp` and the `oskel build` fixture.
    - **7c.4c** (done) — the hidden `$range` helper for the range literals
      `a..b` / `a..=b`. A new `AotTag::Range` names a three-word arena header
      (start, end, inclusive). `oskel_builtin_range` builds it from two integer
      bounds (a non-integer bound fails with the hosted message). The value is
      wired through the runtime: `for-in` (`oskel_iter_next` walks the range up
      or down, inclusive or exclusive, stepping with unsigned arithmetic so
      INT64_MIN/MAX stay defined), `in` (`oskel_contains`, including the
      "descending exclusive matches neither bound" rule), `.length`
      (`oskel_member_get`, saturated at INT64_MAX) and printing/interpolation
      (`oskel_print_repr`/`oskel_builtin_str` render `<start>..<end>` or
      `<start>..=<end>`). Covered by `tests/freestanding_test.cpp` (print,
      iterate both directions, `.length`, membership, interpolation and the
      failing bound) and the `oskel build` fixture.
    - **7c.4d** (done) — the hidden `$slice` helper for
      `obj[start:end:step]`. `oskel_builtin_slice` slices an array by element
      and a string by UTF-8 code point, following the hosted rule: an omitted
      bound is a step-aware default (`0`/`length` for a positive step,
      `length-1`/`-1` for a negative one), a negative bound counts from the
      end, and both clamp to `[-1, length-1]` so a reverse slice never wraps.
      It builds a fresh array/string over the arena (two passes: measure then
      copy), and fails on a non-integer start or end, a zero step and a
      non-sliceable operand. Covered by `tests/freestanding_test.cpp` (array
      and string slices, both step signs, negative and out-of-range bounds,
      UTF-8, the error cases) and the `oskel build` fixture.
    - **7c.5a** (done) — the public standard-library builtins. A compiled
      binary could print and use collections, but a call to `len`, `type`,
      `str`, `repr`, `bool`, `ord`, `chr`, `assert`, `panic` or `isError` was
      still a lowering error ("global 'len' is read but never assigned"), so a
      program using them could not build. Each is now a machine-code function
      with the proto convention, seeded into its global slot as an `AotClosure`
      like `print`, so a `CALL` reaches it through the ordinary `oskel_call`
      path and the compiler is unchanged. `len` counts UTF-8 code points,
      array/map entries or a range's span (saturated at INT64_MAX); `type`
      returns the hosted type-name string (`nil`, `bool`, `int`, `float`,
      `double`, `string`, `array`, `map`, `function`, `range`); `str` reuses
      the `$str` renderer and `repr` the recursive `oskel_repr_to_string`
      (`Value::repr()` semantics, so a top-level string is quoted); `bool` is
      `oskel_truthy`;
      `ord`/`chr` decode and encode a UTF-8 code point; `assert`/`panic` fail
      through a new dynamic-failure helper (`oskel_fail_dynamic`) that prints
      "oskel: " + the caller's message and exits 70, matching the hosted engine;
      `isError` reuses `$isError`. Every builtin is pure -- no libm, no libc --
      so it links into the freestanding binary. Covered by
      `tests/freestanding_test.cpp` (each builtin and every failure path under
      the real `_start`) and the `oskel build` fixture.
    - **7c.5b** (in progress) — the remaining builtins. **7c.5b.1** (done) adds
      the numeric conversions `int`/`float`/`double` for a numeric or boolean
      operand: `int` truncates toward zero (returning an int unchanged, a bool
      as 0/1, and a float/double after a finiteness and a `-2^63 <= x < 2^63`
      range check that matches the hosted bound), `float` rounds to binary32
      and stores the widened value (`cvtsd2ss` + `cvtss2sd`, as the hosted
      `Float` holds), and `double` is the binary64 form. All three are pure SSE
      -- no libm, no libc -- so they link into the freestanding binary.
      **7c.5b.2a** (done) adds the `int(string)` parse, matching the hosted
      `int` byte for byte: it trims ASCII whitespace, accepts a `+`/`-` sign
      and a `0x`/`0X`, `0b`/`0B` or `0o`/`0O` base prefix (a plain leading zero
      keeps base 10), and reproduces `strtoll`'s second `0x`/`0b` prefix
      detection inside the body (`"0x0x10"` is 16, `"0b0b101"` is 5) and its
      overflow rules (`-2^63` is accepted, `2^63` is not). The parser is pure
      integer code -- no libm, no libc -- so it links into the freestanding
      binary. **7c.5b.2b** (done) adds the `float(string)`/`double(string)`
      parse: a freestanding decimal/hex text to binary64 parser that is
      correctly rounded, matching the hosted `strtod` bit for bit. It keeps the
      exact value in a fixed bignum (32-bit limbs, the `dtoa` representation)
      and rounds once, so subnormals, ties to even and the `1e400` overflow
      edge all agree with the host; a silent overflow is a clean error, and
      `inf`/`infinity`/`nan` (case-insensitive, optional sign) are accepted.
      `float` parses to binary64 and narrows, exactly as the hosted `float`
      does. It reuses the `bn_*` primitives `dtoa.cpp` already emits and needs
      no libm or libc, so it links into the freestanding binary. The algorithm
      was prototyped and validated against `strtod` over hundreds of thousands
      of random inputs (binary64, subnormals, hex floats) before being emitted.
      Covered by `tests/parse_float_test.cpp` (the emitted code called through
      a function pointer against `strtod`, 500k random cases plus the edges)
      and `tests/freestanding_test.cpp` (the builtin paths under the real
      `_start`) and the `oskel build` fixture. **7c.5b.3** (done) adds the exact
      math builtins `floor`, `ceil`, `round` and `sqrt`. They need no libm:
      `floor`/`ceil`/`round` are `cvttsd2si` (truncate) plus a correction
      (`floor` subtracts one when `trunc(x) > x`, `ceil` adds one when
      `trunc(x) < x`, and `round` moves one away from zero when the exact
      remainder `|x - trunc(x)| >= 0.5`, which avoids the double rounding that
      `trunc(x + copysign(0.5, x))` suffers for `0.49999999999999994`), and
      `sqrt` is the SSE `sqrtsd`, correctly rounded for every input. Every
      result matches the hosted engine (glibc) bit for bit, including the sign
      of a zero result (`floor(-0.3)` is `-0`) and the `|x| >= 2^63`, inf and
      NaN passthrough; a negative `sqrt` fails like the hosted one. Covered by
      `tests/codegen_test.cpp` (the `sqrtsd` encoding and its execution) and
      `tests/freestanding_test.cpp` (the builtins and the failure paths under
      the real `_start`), plus a 11k-case differential run against the hosted
      engine.
      **7c.5b.4a** (done) adds the transcendental builtins `exp`, `log` and
      `pow`. Unlike the exact builtins above, these need libm in the hosted
      engine and glibc is not correctly rounded there, so no independent
      implementation can match it on every input. The freestanding runtime
      embeds its own copy of the musl routines (`native/mathlib.*`), emitted as
      x86-64 by `CodeGen` the same way `dtoa` and `parse_float` are: `exp` and
      `log` are the musl kernels with their 256-entry tables, and `pow` is
      `exp(y * log(x))` built from the same `log_inline` plus the `exp_inline`
      core (special cases included: `pow(1, y)`, `pow(x, 0)`, the negative-base
      integer-exponent path, `0^0`, inf/NaN). The result agrees with glibc on
      ~99.9% of inputs and every disagreement is a single ulp, so the contract
      is "correctly rounded to 1 ulp" -- validated against a bit-exact musl
      reference, not "byte-identical to glibc". The wrappers do the arity and
      type checks (an int operand is converted to binary64; `log` rejects a
      non-positive operand) and are seeded into their global slots like the
      other builtins. Covered by `tests/mathlib_test.cpp` (the emitted routines
      called through function pointers against libm over 175k/100k/200k random
      inputs) and the `oskel build` fixture.
      **7c.5b.4b** (done) adds the trigonometric builtins `sin`, `cos` and
      `tan`, which need the Payne-Hanek argument reduction (`__rem_pio2` and its
      large-argument helper `__rem_pio2_large` over the two `ipio2`/`pio2`
      tables) plus the `__sin`/`__cos`/`__tan` kernels, all ported from musl and
      emitted by `CodeGen`. The large-argument branch is exercised by the tests
      with inputs above `2^20 * pi/2` (a small random input never reaches it),
      and the diff against a bit-exact musl reference is zero. Like the other
      transcendentals the contract is "agrees to 1 ulp"; `sin`/`cos`/`tan`
      resolve from these ports in a freestanding binary, so `std.math`'s libm
      surface is now complete.
      **7c.5b.5** (done) adds the last three public builtins: `range`, `assertEq`
      and `exit`. `range` builds the same exclusive `Range` value as the hidden
      `$range` helper, with one bound meaning `0..end` and both bounds checked to
      be integers (the failure names `start` or `end`, matching the hosted
      message). `assertEq` compares with the shared `oskel_compare` (the same
      structural equality `==` lowers to) and returns nil on a match; on a
      mismatch it renders the hosted `"<msg>: expected A, got B"` text -- the
      optional third argument, `assertion failed` by default, then the two
      operands through the recursive `repr` -- into a string builder and fails
      through `oskel_fail_dynamic`, so the message and the exit code (70) match
      the VM. `exit` leaves through the entry prelude's `oskel_exit` with the
      integer code (`0` when omitted), so it never returns to the caller. All
      three are seeded into their global slots like the other builtins and are
      covered by `tests/freestanding_test.cpp` (the values and every failure
      path) and `tests/native_build_builtins.oskl` (the `oskel build` fixture).
    - **7c.5c** (done) — error values. `error()`/`isError` were previously
      unbound in AOT (`isError` returned a constant `false`), so `try`/`catch`
      never caught and an error was not a value. The AOT runtime now has a real
      `error` tag: an `AotError` header (`AotString* message` + `int64 code`)
      over the arena. `error(message, code?)` mirrors the hosted builtin (the
      message is a string taken verbatim or any other value through `$str`; the
      optional code is an integer, `0` by default; one or two arguments, else the
      wrong-arity error). The tag flows through every shared helper: `type`
      reports `"error"`, `isError`/`$isError` see it (so `try` propagates and
      `catch` recovers), `==` compares by message *and* code, `print`/`toString`
      render the message alone and the recursive `repr` renders the
      `error("message")` / `error("message", code)` debug form (the code only
      when non-zero), `$str` interpolation renders the message, and
      `err.message`/`err.code` read the fields (`oskel_member_get` gained the
      two member-name islands; any other name is the "no such member" error).
      No new opcode and no bytecode change -- it is a runtime value like the
      others. Covered by `tests/native_build_error.oskl` (built and run against
      the VM by `tests/build_binary.cmake`), which is what makes value-based
      `try`/`catch` work in a native binary. `pcall` (backlog §6.4) and
      catchable *runtime* errors (the AOT runtime has no unwinding) remain out
      of scope.
    - **7c.6** (done) — variadic parameter packs. A function with a trailing
      `rest...` parameter was a lowering error ("variadic function",
      `PHASE 7c.3`); it now compiles. The child proto's prologue checks the
      argument count against `numParams - 1` (a variadic function accepts that
      many or more, unlike the exact match a fixed function needs), copies the
      fixed arguments into R0..R(n-2) and calls the new runtime helper
      `oskel_pack_rest(first, count)` to build an array from the extras and
      bind it to the last slot. The caller's argument window grows *down*, so
      `first` is `args - 16 * (numParams - 1)` and item `i` sits at
      `first - 16 * i`; the helper allocates an array header, grows its block
      once and copies the extras in order, yielding an ordinary `Array` value
      (the VM's packed `rest` binding). No new opcode and no bytecode change:
      `Bytecode::variadic`/`numParams` already carried the information. Covered
      by `tests/freestanding_test.cpp` (zero, one and many extras, `for-in`
      over the pack, `len`, the arity error) and the `oskel build` fixture.
    - **7c.1+** — boxed `Value` over the arena, `print`/`toString` (own dtoa),
      collections and functions. The AOT runtime has its own boxed value and
      its own arena-backed objects, sharing *semantics* with `ops.h` but not
      code, because the hosted `Value` is reference counted and cycle
      collected (`gc.cpp`), which cannot live in a no-libc binary.
  - **7d** (done) — `oskel build` wired into the CLI. `oskel build <file.oskl>
    [-o <out>]` (default output: the script's base name without extension)
    runs the whole AOT pipeline and writes the static executable. See the
    `oskel build` paragraph above for the details.
  - **7e** (done) — Windows x86-64 as a second AOT target. The `Target`
    descriptor gains `Target::winX64()` and `isSupportedTarget` accepts
    `x86_64-windows-win64`; `oskel build --target x86_64-windows-win64` (the
    default follows the host, and a Windows output name gets `.exe`) selects
    it. The lowering and the embedded runtime are shared with Linux unchanged:
    the Microsoft x64 ABI lives only in the entry prelude
    (`emitWindowsRuntime` in `src/native/entry.cpp`), which reaches kernel32
    instead of issuing syscalls — `oskel_write` marshals `GetStdHandle` +
    `WriteFile` (retrying for a full write, with the mandatory NULL
    `lpOverlapped` and shadow space), `oskel_exit` calls `ExitProcess` and
    `oskel_init_arena` calls `VirtualAlloc`. `src/native/pe.cpp` emits a static
    PE32+ with a real import table built from a fixed list
    (`WriteFile`, `GetStdHandle`, `ExitProcess`, `VirtualAlloc`), a `.idata`
    section pinned at RVA 0x1000 so the IAT slots are constant and the prelude
    calls through them RIP-relative, and `IMAGE_FILE_RELOCS_STRIPPED` so a
    Windows with "Force ASLR" maps the non-PIE image instead of aborting it.
    The image reserves 8 MiB of stack (`kPeStackReserve`), matching the Linux
    default: the AOT runtime has no call-depth guard, so the recursion a native
    binary reaches is bounded only by the OS stack, and the 1 MiB Windows
    default killed a program Linux ran. Covered by `tests/pe_test.cpp`
    (structure and, on Windows, execution) and the same `oskel.build_binary*`
    CTest cases Linux runs (`tests/build_binary.cmake` builds the host's target,
    so the whole fixture list exercises the PE on Windows too); the stack
    reserve is guarded by `tests/native_build_deep_recursion.oskl`
    (`oskel.build_binary_deep_recursion`).
  - **7f** (open) — FFI in the AOT backend. An `extern "C"` symbol (and a
    `native fn`) is a host binding the AOT backend cannot resolve: the compiler
    records the declaration in `bc.externs` as pure link metadata, but the
    lowerer never reads it, so a read fails. The read now names the real
    problem (`extern "C" symbol 'X' cannot be resolved by the AOT backend
    (PHASE 7f)`) instead of the generic unassigned-global message
    (`tests/build_error.cmake` pins it). Lowering the symbol is a design
    decision still open, because the generated binary is freestanding today
    (`ldd` reports "not a dynamic executable"): curated syscall stubs keep that
    invariant; an object + external link or a dynamic ELF break it. Slices 7f.1
    through 7f.5 landed the hosted object + link and scalar `extern "C"`
    marshalling; the pointer, struct, callback and variadic shapes remain.
    - **7f.1** (done) — tolerance of an unused host declaration plus the first
      curated stub. A `native fn`/`extern "C"` declaration binds a name to the
      host function the host registered; the compiler emits one read of the name
      for the declaration itself, so the lowerer counts reads against
      declarations and treats only a *used* binding as a failure. This fixes the
      original gap: `import std.math` declares ten `native fn` (`pow`, `sin`,
      ...), and the build no longer fails when the program calls none of them
      (only the pure Oskel `abs`/`min`/`max`/`clamp`). A used declaration
      without a stub is a real limitation, so it fails with a dedicated message
      (`native fn 'X' cannot be resolved by the AOT backend (PHASE 7f)`), while
      `var x = x;` keeps the generic unassigned-global error. `sqrt` joins the
      exact math builtins PHASE 7c.5b.3 already provides (`floor`, `ceil`,
      `round`): `sqrtsd` is correctly rounded, so the hardware result and the
      hosted `std::sqrt` agree bit for bit. This slice tightens that stub to the
      hosted semantics (a NaN operand is not "negative", and the arity message
      matches `sqrt() expects 1 argument, got N`). The libm primitives
      (`exp`, `log`, `pow`, `sin`, `cos`, `tan`) resolve from the embedded musl
      ports PHASE 7c.5b.4 adds, so a program that calls them now builds; a
      `native fn` still without a stub (for example `clock`) is rejected instead
      of compiling to a binary that disagrees with the VM.
      `tests/native_build_math.oskl` and `tests/native_build_math_pure.oskl`
      build and agree with the VM; `tests/native_build_native_error.oskl`
      pins the rejection.
    - **7f.2** (done) — a qualified access to a module's native export is a use
      too, and a qualified *call* is checked at compile time. `std.math.pow(...)`
      reads the module table with `GETTABLE`, not `GETGLOBAL`, so the lowerer
      never saw the use: a program that called a host binding through its
      namespace compiled to a binary that faulted at run time (`cannot call a
      non-function value`) instead of failing the build. The compiler now
      propagates each public native export's *signature* on the `ImportedModule`
      (`nativeSigs`), records the qualified access when it emits the member read,
      and resolves it once the chunk is compiled — deferred, because a hoisted
      function is compiled before the `import` statement that loads the module.
      Resolution both records the use for the AOT backend (so the rejection
      matches the direct form) and checks a call site against the export's
      signature, so `std.math.pow(2.0)` fails at compile time with `'std.math.pow'
      expects 2 arguments, got 1`, exactly like `pow(2.0)`. Propagating the
      signature (not just the name) is what makes the qualified and direct forms
      agree; before, `tests/modules/hostmath.oskl` declared `sqrt` as `float` and
      was called with a `double`, a mismatch the qualified form silently
      accepted. The argument types are captured at the call site, not at flush
      time: a local that shadows a global is gone by then, and inferring late
      resolved it to the global. Covered by
      `tests/native_build_qualified_error.oskl` (the build rejection; the
      `std.math` libm surface resolves from the embedded musl ports, so the
      rejection is exercised with `modules.hostclock.clock`, which has no stub),
      `tests/errors/native_qualified_arity.oskl` and
      `tests/errors/native_qualified_arg_type.oskl` (the compile-time checks).
    - **7f.3** (done) — the two link modes as a CLI concept. The AOT backend
      gains `LinkMode` (`include/native/target.h`): `Freestanding`, the default,
      is the self-contained image every target already emits; `Hosted` is the
      mode that will link against the host C library so an `extern "C"` symbol
      resolves to the real C function. The mode is a deployment choice, never
      language syntax: the same source compiles in either mode and the choice
      lives on the command line (`oskel build --mode freestanding|hosted`,
      default `freestanding`). `isSupportedLinkMode`/`requireSupportedLinkMode`
      pin the (target, mode) matrix — freestanding for all three targets, hosted
      for Linux x86-64 only for now — so an unsupported pair is reported instead
      of emitting bytes. This slice changes no code path for freestanding: the
      emitted binary is byte for byte identical to before. Covered by
      `tests/build_mode.cmake`.
    - **7f.4** (done) — the hosted image: a relocatable object plus a link
      against libc. The emitter gains external references (`ExternalRef` in
      `include/native/codegen.h`): `callExternal` emits `call rel32` to an
      undefined symbol and records the displacement slot, and `leaRipExternal`
      does the same for an address; `exportSymbol` records a defined symbol
      (the program's `main`). `src/native/object.cpp` wraps the code buffer in
      an ELF64 `ET_REL` object — `.text`, `.rodata`, `.rela.text` (one
      relocation per reference: `R_X86_64_PLT32` for a call, `R_X86_64_PC32` for
      an address, both with addend `-4` because the CPU measures the
      displacement from the slot's end), `.symtab`/`.strtab` and `.shstrtab`,
      with the local symbols before the global ones as the ABI requires.
      `emitHostedRuntime` (`src/native/entry.cpp`) binds the same three entry
      handles the runtime already references but reaches the system through libc
      (`write`/`exit`/`malloc`) instead of a raw syscall, and exports the
      program entry as the C `main` the C runtime calls — the lowering's `main`
      already has the `int main(void)` shape, so no thunk is needed. `oskel
      build --mode hosted` writes the object beside the output and links it with
      the host C compiler (`$CC`, default `cc`, `-no-pie -lm`), removing the
      temporary object either way. The freestanding path is untouched and its
      binary is byte for byte identical; a hosted binary agrees with the
      development VM byte for byte. Covered by `tests/object_test.cpp` (the
      object's structure) and `tests/build_mode.cmake` (the hosted build runs and
      is dynamically linked). General `extern "C"` marshalling is 7f.5.
    - **7f.5** (done) — scalar `extern "C"` resolution in the hosted backend.
      With the object emitter and the hosted link in place, an `extern "C"`
      symbol can now resolve through the system linker instead of failing the
      build. The lowerer narrows each declaration to an `AotExternThunk`
      (`include/native/runtime.h`): one `ffi::ParamClass` per parameter and one
      `ffi::ReturnClass`, i.e. the register-level shape only, so the runtime
      never reaches into the compiler's FFI type tables. Only a *scalar*
      signature is narrowed (an integer/bool/float/double parameter or return);
      a `ptr`/`cstring`, a struct, a callback or a C variadic is left out and
      keeps the build rejection, because the thunk's register marshalling cannot
      express it. `collectExternThunks` builds the list in the hosted mode and
      seeds each thunk into the symbol's global slot as an `AotClosure`, exactly
      like a builtin, so a `CALL` reaches it through the ordinary `oskel_call`
      path. The thunk itself (`emitExternThunk`, `src/native/runtime.cpp`) has
      the lowering's own function shape and reproduces the hosted
      `ffi_marshal.h` conversions: a bool is 0/1, an integer is the payload, a
      number is truncated toward zero for an integer parameter and widened for a
      `double`; the result is boxed, an integer normalized to its declared width
      (a C `int` leaves the upper half of `rax` undefined, so a `shl`/`sar`
      pair — the new `sarImm8` — re-extends it). The C symbol is called through
      a `callExternal` relocation the linker resolves. The freestanding path is
      untouched: it passes no thunks, so an `extern "C"` use still fails the
      build and its image is byte for byte identical. Fixing this slice also
      surfaced two latent bugs: `cvttsd2si` swapped the REX `R`/`B` bits (only
      reachable from an r8-r15 destination or an xmm8-15 source, which no prior
      caller used), and the hosted float/double printer restored `rsp` before
      the write, leaving the text buffer in the dead zone where libc's `write`
      clobbered it (the first `print` of a double came out as NUL bytes). The
      printer now keeps its frame allocated across the write in the hosted mode
      only, so the freestanding image stays byte-identical. Covered by
      `tests/build_hosted_extern.cmake` and `tests/native_build_extern_hosted.oskl`
      (a program calling `abs`/`labs`/`getpid`/`fabs`/`fmod`, built hosted and
      checked against the VM), and `tests/codegen_test.cpp` pins the extended
      `cvttsd2si` encodings. A `ptr`/`cstring` parameter or return, a struct, a
      callback and a C variadic remain 7f work.
  - **7g** (done) — bare-metal x86-64 (multiboot). A third target,
    `x86_64-freestanding-sysv` (`oskel build --target
    x86_64-freestanding-sysv`), emits a multiboot 1 kernel that a boot loader
    (QEMU `-kernel`, GRUB, Limine) loads at 1 MiB and enters in 32-bit
    protected mode. Because the CPU starts in 32-bit mode with paging off and
    multiboot leaves a *32-bit* code descriptor in the GDT, the image begins
    with a small hand-assembled 32-bit trampoline (`src/native/bare_metal.cpp`,
    `include/native/bare_metal.h`): it builds an identity page table with 2 MiB
    huge pages, turns on PAE and long mode (and, in CR4, `OSFXSR` and
    `OSXMMEXCPT`, without which the first SSE instruction the float path emits
    is a `#UD` that, with no IDT installed yet, escalated to a triple fault),
    installs its own
    GDT with a 64-bit code segment and far-jumps into the shared 64-bit
    runtime. From there the program is byte-for-byte the same machine code the
    Linux target emits; only the prelude differs, because a kernel has no
    system calls. `oskel_write` drives the 16550 UART at COM1 directly (the
    entry point first brings the chip up -- mask interrupts, load divisor 1,
    8N1, enable the FIFO -- so a real UART transmits, not just QEMU's; then
    polling the line-status register, `out dx, al`); `oskel_exit` masks
    interrupts, publishes the exit code on
    QEMU's `isa-debug-exit` port and spins in `hlt` (on real hardware the port
    write is ignored and the `hlt` loop is the end of the program); and
    `oskel_init_arena` points the bump allocator at a `.bss` region the loader
    zeroes instead of `mmap`. The entry point also installs an interrupt
    descriptor table (256 gates, one stub per vector) before running the
    program, so a CPU exception reaches a handler that prints `oskel: cpu
    exception 0xVV rip=0x...` over the serial port and halts. Without an IDT a
    `#UD` is a double fault and then a reset: the machine hangs with no trace,
    which is exactly how the missing `CR4.OSFXSR` (above) hid. The IDT backs
    two handlers: the *fault* handler above (every vector but the timer) and a
    *timer* handler for IRQ0 that acknowledges the 8259 PIC and returns with
    `iretq` -- a device interrupt must never be confused with a fault, so a
    `#PF` still halts with a trace. Before enabling interrupts the prelude
    remaps the 8259 PIC off the CPU's exception vectors onto 0x20-0x2F (the
    standard ICW1-ICW4 sequence), programs the 8254 PIT channel 0 in mode 3
    (divisor 11931, 100 Hz), unmasks only IRQ0 and masks the rest, and only
    then executes `sti` -- the last step before the program runs, so no
    interrupt arrives with the IDT or the PIC half-configured. The timer
    handler counts ticks in a `.bss` kernel-data page (added by this slice,
    which shifts the stack and the arena up by one page) and is silent on every
    tick: it acknowledges the PIC and `iretq`s. A per-tick print would
    interleave with the program's own output -- a normal program already spans
    at least one tick -- so the tick count is kernel state and the ceiling is
    the observable: after 500 ticks (five seconds) a program that never returned
    gets one `tick limit` line and the dedicated exit code 0x77. The handler
    saves and restores every register it touches (including `rbp`, since the
    interrupted code may use it as a frame pointer) and forces 16-byte stack
    alignment around the `call` into `oskel_write`, because an interrupt lands
    at an arbitrary instruction, not only at a call boundary. The ELF emitter
    produces an ELF32/i386 image with
    two `PT_LOAD` segments: the code at 1 MiB (R+X) and the `.bss` at 2 MiB
    (R+W, `p_filesz = 0`, `p_memsz` = page tables + kernel data + stack +
    arena). The
    lowering and the embedded runtime are shared with Linux unchanged. Verified
    in `tests/bare_metal_test.cpp`: the structural checks always run, and the
    end-to-end boot (real serial stdout and exit code under
    `qemu-system-x86_64 -kernel`) runs whenever QEMU is on `PATH`. The image is
    x86-64, but QEMU emulates it on any host, so the boot tests run on Linux,
    Windows and macOS alike -- the arm64 macOS image included; the harness picks
    its temp directory and exit-code decoding per platform. CI installs QEMU on
    every runner and sets `OSKEL_REQUIRE_QEMU`, which turns a missing binary
    into a test failure rather than a silent skip, so the boot path cannot
    disappear from the suite unnoticed; on a developer box without the variable
    the boot tests skip, and the structural checks still run. The
    `CodeGen` gained six single-opcode primitives (`cli`, `sti`, `hlt`,
    `iretq`, `out dx, al`, `in al, dx`) plus `test al, imm8`, each byte-checked
    in `tests/codegen_test.cpp`.
- Deferred: per-instruction type annotations for fast paths without dynamic
  dispatch (bridge toward the speculative work in
  `docs/ARCHITECTURE_ANALYSIS.md`); other targets (macOS via libc or stubs;
  ARM64 bare metal).
- Afterwards, LuaJIT's JIT as a reference (see PHASE 8 below).
- **In the tree now**: the `Target` descriptor (`include/native/target.h`) and
  the x86-64 instruction emitter (`include/native/codegen.h`,
  `src/native/codegen.cpp`): labels with backpatching, `mov`/`lea`, integer
  ALU, shifts, compares, `setcc`/`movzx`, `jmp`/`jcc`, SSE2 float ops,
  `push`/`pop`, `call rel32`, `syscall` and `ret`, covered by the byte-exact
  tests in `tests/codegen_test.cpp` (the mmap/execution checks run on Linux
  x86-64 only). The 7b lowering (`include/native/lower.h`,
  `src/native/lower.cpp`) turned the runtime-free, integer-only opcodes into
  machine code, and 7c.1a replaced it with the boxed value model; both are
  tested by execution in `tests/lower_test.cpp`.
  7c.0 adds the freestanding substrate: the static ELF64 and PE32+ emitters,
  the arena, `PlatformFreestanding` (direct syscalls, no libc) and the
  `_start`/prelude, exercised end to end (a real generated binary is run) in
  `tests/freestanding_test.cpp`. 7c.1a adds the boxed value model
  (`include/native/value.h`) and the embedded runtime
  (`include/native/runtime.cpp`): a register is a 16-byte `AotValue` slot in a
  stack register file, and the integer/float arithmetic, bitwise/shift, unary,
  comparison and truthiness helpers plus the int/bool/nil printer are emitted
  as machine code into the program's own buffer. 7c.1b adds floats and doubles
  (`Double` tag, `oskel_arith`/`oskel_shift`/`oskel_pow`), with the int/float/double
  promotion of the hosted engine. 7c.2c.1 lowers calls: each proto becomes a
  machine function, `CALL` dispatches it through `oskel_call` and `CLOSURE`
  allocates an `AotClosure` with `oskel_closure`. 7c.2c.2 adds captured cells:
  a captured local lives in a shared arena cell, `oskel_cell_new` allocates it,
  `GETUPVAL`/`SETUPVAL` and nested `CLOSURE`s resolve through the frame's saved
  `self`, and `oskel_closure` reserves the capture slots the lowering fills.
  7c.2d seeds the `print`/`write`/`eprint` builtins as globals, so a compiled
  program can print. 7c.3 lowers collections: arrays and maps over arena
  headers, indexing/membership/`for-in` through the collection helpers and a
  recursive container printer. 7d wires the backend into the CLI: `oskel build
  <file.oskl> [-o <out>]` runs the whole pipeline (lex, parse, compile,
  `lowerToX64`, `buildStaticElf`) and writes a static executable, defaulting
  the output to the script's base name. The chunk's `processEntry` (`_start`)
  is handed to the emitter, so the binary runs and exits without libc. The
  command reports an unsupported target rather than emitting bytes for an ABI
  the backend cannot produce, and the emitted binary embeds only the runtime,
  never the compiler (DEVELOPMENT.md §3, the GraalVM pitfall).

### PHASE 8 — JIT for the development VM 🔶 in progress

A JIT for `oskel` only, never for `oskelc` (§6: the product is AOT; the JIT is a
development tool). It follows the Lua/LuaJIT model: the same bytecode the VM
interprets is compiled to native x86-64 for the hot path, and the interpreter
stays the fallback for everything else, so a program's result never depends on
whether the JIT ran. There is **no** inline caching, **no** superinstruction and
**no** runtime specialization (§6): the JIT lowers each opcode 1:1 with the
`include/native/codegen.h` emitter (the project's own, so §5.1 keeps its
zero-external-dependency rule; AsmJIT is not required).

Slice 1 (done) is the smallest useful tier: a **leaf proto over the integer
subset**. A proto is compiled when its body only touches `nil`, `bool` and
`int` values, has no calls, no upvalues, no globals, no collections and no
child protos; `Jit::get` verifies that by a small type propagation over the
bytecode (`LOADK`, `MOVE`, `ADD`/`SUB`/`MUL`, the bitwise ops, `UNOP`,
`EQ`–`GTE`, `JMP`, `JMP_IF_FALSE`/`JMP_IF_TRUE`, `DECLLOCAL`, `GETLOCAL`,
`SETLOCAL`, `RETURN`, `HALT`) and rejects anything else, leaving it to the VM.
The generated code keeps the same `Value` layout as the interpreter (a tag
byte plus a 64-bit payload per slot), so the two engines share the value
representation and the differential test is exact. The VM runs it through
`VM::jitFor` at `CALL` and at `Runtime::call`, only when the JIT is enabled
(the CLI turns it on; `--no-jit` turns it off; `oskelc` never sees it) and only
when every argument is an integer and the arity matches, because the compiled
body reads integer slots only. The compiled body also keeps every scalar slot
in a physical CPU register instead of its 16-byte stack slot whenever the
dataflow proves the slot only ever holds `nil`/`bool`/`int`: the register holds
the raw payload and the tag is materialized only at the frame boundary, so a
tight integer loop does almost no memory traffic.

Slice 2 (done) adds a **compile-time optimizer** in front of the same emitter:
a constant propagation over the proto folds any subtree whose operands are
constants (`2 * 3` becomes `6`), and the emitter then applies a constant operand
as a machine immediate (`n + 7` is one `add`, `i * 2` is one `shl` via the
strength reduction, `x & -1` and `x * 1` disappear as identities), computes into
the destination register rather than copying operands through scratch, and turns
a branch on a constant condition into a taken or not-taken edge. A slot no
reachable read materializes loses its `LOADK`, so the constants a folded
expression leaves behind emit nothing. This is still 1:1 with the bytecode — the
optimizer rewrites *how* an opcode is emitted, never *how many* opcodes run, and
no new opcode, superinstruction or runtime specialization appears. A tight
integer loop is about 260x faster than the interpreter, within about 1.7x of a
hand-written `-O2` C++ loop.

Slice 3 (done) adds **compare-branch fusion**. A comparison whose boolean result
only feeds the conditional branch right after it no longer materializes the bool
(`cmp; setcc; movzx; mov; test; jcc`) and jumps straight off the flags
(`cmp; jcc`). A backward liveness pass proves the result dead past the branch and
that the branch is reached by fall-through alone, so the re-test the VM would
perform is never lost; a comparison whose result is reused afterwards stays
unfused and correct. The same loop drops to about 320x the interpreter, within
about 1.4x of the hand-written loop. The fusion rewrites *how* one opcode and its
branch are emitted; the bytecode still executes the same two opcodes, so the
contract below holds.

**What "1:1" means here.** The invariant is about the *bytecode*, not about the
native instruction count: the JIT never adds, removes or merges a bytecode
opcode, and a program's result never depends on which opcodes were compiled.
The optimizer is free to change how an opcode (or an adjacent opcode plus its
branch) is *emitted* — constant folding, strength reduction, compare-branch
fusion are all peephole rewrites of the emission, and none of them introduces a
superinstruction or a runtime specialization. This is why slice 3's fusion is
still within the rule: it emits one `cmp; jcc` where the interpreter would
execute an `EQ` then a `JMP_IF_FALSE`, but the two opcodes still run, and an
unfused comparison (one whose result is reused) stays correct.

Slice 4 (done) replaces the fixed per-slot register map with **web-based register
allocation and copy coalescing**. The emitter used to tie a slot to a register by
index (`R0` to the first pool register, `R1` to the second, ...), so a temporary
reused for two values forced a `mov` at each reuse and every `GETLOCAL`/
`SETLOCAL` copied between the temporary's register and the local's. The emitter
now resolves every read and write through a per-instruction allocation table
(`readReg[i][slot]`/`writeReg[i][slot]`), and `allocateRegisters` fills it: it
builds a *web* per slot (the definitions a single live use can observe, via
reaching definitions unioned over the live points), interferes webs that are live
at the same point, coalesces the copies that do not interfere (a `MOVE`, a local
copy, or an in-place `s = s + x`) after dropping the spurious interference edge
between a pure-copy web and its source, and colors the merged graph greedily over
the seven-register pool. A web the pool cannot cover keeps its 16-byte stack
slot, exactly like the old overflow path, so the code stays correct when the
register pressure exceeds the pool. A liveness pass that mirrors the emitter's
fold decisions also drops a definition no read observes (`deadDef`), which
removes the constant a folded expression left behind. The integer loop falls from
17 instructions to 9 (a hand-written `-O2` C++ loop is 7) and from 0.051s to
0.039s, within about 1.08x of the hand-written loop and about 420x the
interpreter. The allocation rewrites *where* a value lives, never *how many*
opcodes run, so it is still 1:1 with the bytecode.

Later slices widen the subset (floats, then strings, then the boxed value
runtime, then calls and guards) and add the tiering policy (compile after a
call threshold instead of on the first call), following LuaJIT as the
reference. Everything stays 1:1 with the bytecode in the sense above; the
moment a design needs inline caching or specialization, it is out of scope
(§6).

### Post-1.0 — UI library

- **Skia** as rendering backend.
- **SDL2** for windows and events.
- **Declarative UI** (SwiftUI/Flutter style): `Window`, `Column`, `Row`,
  `Text`, `Button`, `Input`, `Checkbox`, `Slider`, etc.

### Post-1.0 — Other features

- **Cosmopolitan (APE)** for CLI multi-platform (optional).
- **Interfaces/traits** (for generic code without full generics).
- **Dynamic FFI** (`dlopen`, `dlsym`, runtime stubs with the project's own JIT).
- **Hot reload** (1.1+).
- **Extended stdlib**: JSON, dates, bytes, regex, networking.
- **Package manager** (`oskel install foo`) and registry.
- **Windows and macOS** (1.2+).
- **Embedded VM** (`--embed-vm`): AOT binary + VM for plugins, hot reload.

### Future — Static with `@Dynamic` (2.0)

- Static by default, `@Dynamic` for dynamic parts.
- Type inference, monomorphization.
- Requires: types (6.5), FFI (6.6), AOT (7).

### Current status

Phase 1: ✅ completed and reviewed
Phase 2: ✅ completed
Phase 3: ✅ completed
Phase 4: ✅ completed (VM as the dev engine: `oskel`, `oskel -e` and the REPL compile to bytecode and run on the VM; the AST interpreter and its `--ast` flag are gone)
Phase 5: ✅ completed and tested
Phase 6: ✅ completed (6a, 6b, 6c, 6d, 6f done)
Phase 6.5: ✅ completed (Java-style `import`, `switch`, `defer`, type annotations/inference/nullable `T?` and nullability expressions `?.`/`??`/`!!`, `struct`, `native fn`, variadic `rest...` parameters and the `std.math` module). Post-6.5 additions: slicing syntax `obj[start:end:step]` for strings and arrays with an optional step (still one `$slice` helper call, no new opcode), the short-circuiting compound assignments `&&=`, `||=` and `??=` (existing `EQ`/`NEQ`/`JMP_IF_*`/`MOVE` opcodes, no new opcode), the post-test loop `do { ... } while (cond);` (existing `JMP`/`JMP_IF_TRUE` opcodes, no new opcode), optional parentheses in `for ... in` (`for (x in xs)`, told apart from the classic header by a `( name in` lookahead), the key/value form `for k, v in map` (a second loop variable after a comma binds the value of each pair, which `ITER` now yields directly, no new opcode and no helper call) and brace-less single-statement bodies for `if`/`else`/`while`/`for`/`for-in` (a single statement wrapped in a block, so the bytecode is unchanged)
Phase 6.6: ✅ completed (type vocabulary, `extern "C"` declarations + marshalling, pointers, structs by value and callbacks)
Phase 6.7: ✅ completed (6.7.1 `pcall`, 6.7.2 `error` type, 6.7.3 `try`/`catch`, 6.7.4 unions `T | E` and 6.7.5 `@MustUse` done)
Phase 7: 🔶 in progress (7a, 7b, 7c.0, 7c.1a-d, 7c.2a-d, 7c.3, 7c.3b, 7c.3c, 7c.3d, 7c.3e, 7c.4a, 7c.4b, 7c.4c, 7c.4d, 7d, 7e, 7g done; 7f — FFI in AOT — 7f.1-7f.5 done, scalar `extern "C"` resolves in the hosted backend, pointer/struct/callback/variadic still open)
Phase 8: 🔶 in progress (JIT for the development VM, slices 1-4 done: integer leaf protos with constant folding, strength reduction, compare-branch fusion and web-based register allocation with copy coalescing)
Post-1.0: ⬜ pending (UI, Cosmopolitan, package manager, VM embedded)
2.0: ⬜ future (static with `@Dynamic`)

Bugs are tracked in the git history and the regression tests; there is no
separate bug log file.

Distribution model: musl static by default (self-contained, no runtime).
Optional: newlib (bare-metal), freestanding (no libc).

VM = dev engine only. The AST interpreter was retired at the 6f parity gate
and is no longer in the tree.


---

## 5. Code rules

1. **Pure C++17**, standard library only. The single optional dependency is
   **libffi** (for FFI callbacks, **development engine only**). The machine
   code, the static ELF/PE emitters and the JIT are the project's own
   (`native/codegen.*`, `native/elf.*`, `native/pe.*`): **no AsmJIT and no
   ELFIO** (LIEF, AsmJIT and LLVM were evaluated and rejected, see PHASE 7).
   libffi is never linked into an AOT product: the freestanding backend lowers
   callbacks to compile-time stubs, so the end-user binary stays dependency
   free.
2. **Names**: `camelCase` for functions, variables and members (private data
   members end with `_`), `PascalCase` for types, `UPPER_CASE` for opcodes
   and constants.
3. **Structures**: `struct` for data, `enum class` for opcodes.
4. **Memory**: RAII and standard containers (`std::vector`, `std::string`);
   no `malloc`/`free`.
5. **Errors**: diagnostic structs with line/column; the CLI prints them to
   **`stderr`** (via `Platform::writeErr`).
6. **Language**: all code, comments, documentation and commit messages are
   written in **English**.
7. **Every new `.cpp`** has its `.h` in `include/`.
8. **No mutable global variables** except the VM's globals table.
9. **Build with**: `g++ -std=c++17 -Wall -Wextra -O2`.
10. **Reference cycles**: values are reference counted and `gc.cpp` reclaims
    the cycles counting cannot. Anything that can own a `Value` and be
    reachable from itself is a `GcNode`, and its `visit` operation must report
    every strong reference it holds to another tracked object. **Do not add
    side owners** such as registries or caches of objects that can sit in a
    cycle: the collector sees them as references from outside, which makes the
    cycle immortal. Cells are tracked lazily (`GcNode::track`).

---

## 6. Explicit prohibitions

| Prohibited | Reason |
|-----------|--------|
| Stack-based VM | Oskel is register-based (Lua style) |
| High-level bytecode (`OP_EVAL_EXPR`) | Breaks future native compilation |
| **JIT in the AOT product** (the JIT is for `oskel` only, never for `oskelc`) | The product is AOT; the JIT is a dev tool |
| Migrating the core to pure C | The core stays in C++17 |
| Adding features without testing the previous one | Breaks incremental development |
| Skipping the 7-point checklist | Creates architectural debt |
| **External libraries beyond libffi** | libffi (development engine only) is the only allowed dependency; the emitter and the JIT are the project's own |
| Superinstructions | Violate the 40-opcode ceiling |
| Inline caching | Changes the bytecode contract |
| Runtime specialization | Changes the bytecode contract |
| `async`/`await`, generics, full pattern matching, generators, transactions | Out of scope |

**Note**: libffi is the only **optional** dependency, and it is
**development-engine only** (FFI callbacks). The machine-code emitter, the
static ELF/PE writers and the JIT are the project's own C++ (no AsmJIT, no
ELFIO): the AOT product (`oskelc`) emits the binary itself and the development
VM (`oskel`) may use libffi for callbacks. **The end-user binary (`hello.exe`)
contains none of them.**

---

## 7. Historical context

- There was a **previous AST interpreter** (MiniLang, renamed Oskel) used as a
  **differential test oracle** while parity with the VM was being reached. It
  has been **retired**: the tree no longer contains `interpreter.cpp`/`.h`.
- The product model is Lua/LuaJIT's: source → lexer → parser → AST → compiler
  → bytecode → VM. The AST is **not executed** in production.
- The "dual" approach (AST interpreter as an alternative engine) was **NOT**
  preserved. Once the VM covered the language (PHASE 4), the AST interpreter
  left the binary and was retired.
- The phases ran in order: 5 ✅, 6, then 4 (VM as the dev engine) and 7 (AOT
  native backend).

---

## 8. File extensions and packaging

| Extension | What it is | Java analog |
|-----------|------------|-------------|
| `.oskl` | Source code | `.java` |
| `.oskc` | Bytecode | `.class` |
| `.oskpkg` | Package (ZIP) | `.jar` |
| `.osklib` | Native library | `.dll`/`.so`/`.dylib` |
| `.oskdoc` | Documentation | `.javadoc` |
| `myapp` | Native binary (Linux/macOS) | (GraalVM) `.exe` |
| `myapp.exe` | Native binary (Windows) | (GraalVM) `.exe` |

**`.oskpkg` format** (ZIP) — **planned; not implemented in 0.1.0**:
myapp.oskpkg
├── META-INF/MANIFEST.MF
├── bin/main.oskc
├── lib/mylib.osklib
├── resources/icon.png
└── README.md


**`.oskc` format** (binary) — **planned; not implemented in 0.1.0**. The compiler
holds a `Bytecode` in memory and the AOT backend lowers it directly; nothing
serializes it to disk yet, and no command writes or reads a `.oskc` file. The
layout below is the target format, not the current behaviour:
+--------+--------+--------+--------+
| 'J' | 'O' | 'L' | 'C' | magic
+--------+--------+--------+--------+
| version (u32) |
+--------+--------+--------+--------+
| flags (u32) |
+--------+--------+--------+--------+
| name_len (u32) + name |
+--------+--------+--------+--------+
| numParams, numRegisters, numLocals|
+--------+--------+--------+--------+
| code_size + code |
+--------+--------+--------+--------+
| constants_size + constants |
+--------+--------+--------+--------+
| children_size + children |
+--------+--------+--------+--------+
| upvalues_size + upvalues |
+--------+--------+--------+--------+
| checksum (u32) |
+--------+--------+--------+--------+


**Bytecode verification** (on `.oskc` load) — **planned; not implemented in
0.1.0** (there is no `.oskc` loader yet):
- **Level 1 (structural)**: mandatory. Magic, version, ranges, valid opcodes,
  operands in range, checksum.
- **Level 2 (semantic)**: optional. Jumps to valid instructions, registers
  initialized.
- **Digital signature**: post-1.0, if the threat model requires it.
