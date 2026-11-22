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
| `core/value.cpp` | `Value`, tagged union | shared vocabulary | ✅ |
| `core/gc.cpp` | tracked `GcNode`s | cycle reclamation | ✅ |
| `core/platform_posix.cpp` | `Platform` ops (dev engine) | libc-backed I/O | ✅ |
| `frontend/lexer.cpp` | source text | `Token[]` | ✅ |
| `frontend/parser.cpp` | `Token[]` | `Node*` (AST) | ✅ |
| `frontend/bytecode.cpp` | `Bytecode` | basic operations | ✅ |
| `frontend/compiler.cpp` | `Node*` | `Bytecode*` | ✅ |
| `frontend/module_resolver.cpp` | dotted module path | source file path | ✅ |
| `frontend/diagnostic.cpp` | `CompileError`, `Warning`, `RuntimeError` | stderr output | ✅ |
| `frontend/cli.cpp` | shared CLI plumbing | module resolution, `std/` lookup, AOT pipeline | ✅ |
| `frontend/native/codegen.cpp` | x86-64 instructions | machine-code bytes | ✅ PHASE 7a/7b |
| `frontend/native/lower.cpp` | bytecode | x86-64 (boxed `Value`) | ✅ PHASE 7c.3 (all opcodes) |
| `frontend/native/runtime.cpp` | boxed values | runtime helpers as machine code | ✅ PHASE 7c.3 |
| `frontend/native/dtoa.cpp` | float/double | shortest round-trip text | ✅ PHASE 7c.1b |
| `frontend/native/elf.cpp` | text + rodata | static ELF64 bytes | ✅ |
| `frontend/native/pe.cpp` | text + rdata | static PE32+ bytes (imports) | ✅ PHASE 7e |
| `frontend/native/arena.cpp` | block + size | aligned allocations | ✅ PHASE 7c.0 |
| `frontend/native/platform_freestanding.cpp` | `Platform` ops | direct Linux syscalls | ✅ PHASE 7c.0 |
| `frontend/native/entry.cpp` | program entry label | `_start`/entry + prelude (Linux syscalls, Windows kernel32) | ✅ PHASE 7c.0/7e |
| `frontend/native/bare_metal.cpp` | code + entry | multiboot ELF32/i386 kernel (bare-metal prelude + trampoline) | ✅ PHASE 7g |
| `runtime/vm.cpp` | `Bytecode*` | execution | ✅ |
| `runtime/builtins.cpp` / `runtime/ops.cpp` | native builtins, shared operators | runtime library | ✅ |
| `runtime/ffi.cpp` | `extern "C"` declarations | marshalling natives | ✅ PHASE 6.6 |
| `runtime/jit/jit.cpp` | integer leaf proto | native x86-64 (dev engine only) | 🔶 slices 1-4 |
| `main.cpp` | CLI | full pipeline | ✅ (VM as dev engine, PHASE 4) |
| `oskelc.cpp` | CLI | full pipeline, AOT only | ✅ (host compiler, no VM) |

### Toolchain binaries and the compiler boundary

Two binaries share one front end. The engine's sources live in three directories
and **the directory a file lives in is its classification** — there is no
hand-maintained list to forget:

- **`src/core/`** (shared, no engine state): `Value`, the cycle collector, the
  platform layer, `util.h` and `errors.h`. Both binaries need it.
- **`src/frontend/`** (`oskel_frontend`, on top of core): lexer, parser,
  bytecode compiler, module resolver, diagnostics, the shared CLI and the whole
  AOT backend (`src/frontend/native/*`). A compiler needs exactly this.
- **`src/runtime/`** (`oskel_lib`, on top of the front end): `vm.cpp`, `ops.cpp`,
  `builtins.cpp`, `ffi.cpp`, `unicode_case.cpp` and `jit/jit.cpp`.

The binaries:

- `oskel` development engine (run, REPL, -c, `build`). Links the runtime on top
  of the front end, so it can interpret. Includes the JIT (the JIT uses the
  project's own `native/codegen.h` emitter, not AsmJIT).
- `oskelc` host compiler (`oskelc file.oskl -o app`). Links core + front end
  alone, so the shipped compiler carries no VM, no JIT and no builtins. Reads
  sources and writes the ELF with the project's own `native/codegen.cpp` and
  `native/elf.cpp` (no AsmJIT, no ELFIO).
- `oskel build` is a thin alias: both commands call `runBuildCommand` in
  `src/frontend/cli.cpp`, so their flags, defaults, diagnostics and exit codes
  cannot drift. `oskelc` is the same AOT pipeline with a shorter command line.

`oskelc` has no bytecode serializer yet, so `-c` (`.oskc`) and `-pkg`
(`.oskpkg`) are refused by name with exit 64 instead of being read as a typo.
Those are the documented next steps for the compiler, not implemented features.

A third, standalone binary ships alongside them: `bindgen`, the C binding
generator (`docs/bindgen.md`). It reads a C header and writes an Oskel
`extern "C"` module. It is a pure text transformation, so it does not link
`oskel_lib` (or the runtime, the compiler, the VM or libffi): its sources live
under `tools/`, outside the three `src/` globs that build the engine, so it can
be built and shipped on its own.

A fourth standalone binary is `oskelp`, the project and package manager
(`docs/oskelp.md`). Like `bindgen` it lives under `tools/` and links no engine
code; it drives `oskel`/`oskelc` as child processes. It owns the manifest
(`oskel.toml`), the lockfile, path-dependency resolution and the
`new`/`build`/`run`/`check`/`test` workflow. The only compiler-side hook is
`OSKEL_MODULE_ROOT`: `resolverFor` honours it so a package's `src/` is the
module root, and both engines resolve imports identically.

Boundary rule: no source in `src/frontend/` may include `vm.h`, `jit/jit.h`,
`builtins.h`, `ops.h`, `ffi_marshal.h` or libffi, and `src/frontend/compiler.*`
plus `src/frontend/native/*` must also stay clear of `interpreter.h`. Enforced
three times, because each check sees something the others cannot:

1. `tests/check_compiler_boundary.cmake` walks the include graph of every source
   `oskelc` compiles (CMake passes the exact `OSKEL_CORE_SOURCES` +
   `OSKEL_FRONTEND_SOURCES` list, so a new front-end file is checked
   automatically).
2. `tests/check_oskelc_no_vm.cmake` checks the **archive**, not just the binary:
   `nm` must not show `VM::`, `jitCompile`, `Jit::`, `registerBuiltins`,
   `ops::binary` or `Runtime::fail` in either `oskelc` or
   `liboskel_frontend.a`. The binary alone is not enough — a static library only
   pulls in the objects a reference needs, so an unreferenced runtime object
   sits in the archive without ever reaching the executable, and `nm oskelc`
   stays clean while the compiler's input is dirty.
3. The configure step refuses a `.cpp` dropped straight into `src/`: every
   source must be in one of the three directories, so the classification is
   never skipped by default.

All three exist because the previous single list was only checked in one
direction: it failed when a listed runtime source did not exist, but a new
runtime source that nobody listed silently defaulted to the front end.

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
  `include/bytecode.h`, implemented in `src/frontend/bytecode.cpp`)

### PHASE 2 — `src/frontend/compiler.cpp` (AST → bytecode) ✅
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

### PHASE 3 — `src/runtime/vm.cpp` (run bytecode) ✅
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

### PHASE 6 — Functions, collections and library ✅
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
  and a reserved `std.` prefix. A qualified import also binds the last segment
  (`math` in `import std.math;`) when that name is free, so the module is
  reachable without repeating the path; an explicit alias suppresses it, a
  top-level declaration wins over it, and two imports that would claim the same
  short name are a compile error. A module is loaded
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
  collection/string methods so it also runs on WebAssembly; it behaves
  identically on the VM and under `oskel build`.
  See `docs/language.md` §12.9/§18, `tests/55_std_json.oskl` and the AOT
  fixture `tests/native_build_json.oskl`.
- **Standard library: `std.datetime`** — **done**: a pure-Oskel module for civil
  dates and wall-clock times. A `Date`/`DateTime` is an ordinary map built by
  the exported constructor functions (`date`, `datetime`), so no new runtime
  type, opcode or builtin is added beyond `time()` (below). It offers the
  `days_from_civil` algorithm, epoch conversion, calendar arithmetic,
  differences, components, a `strftime`-like `format` and ISO-8601
  `parse`/`iso`/`isValid`/`tryParse`. The module is written over the portable
  primitive surface, avoiding the collection/string methods so it also runs on
  WebAssembly; it behaves identically on the VM and under `oskel build`. See
  `docs/language.md` §12.10/§18, `tests/56_std_datetime.oskl` and the AOT
  fixture `tests/native_build_datetime.oskl`.
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
  collection/string methods so it also runs on WebAssembly; it behaves
  identically on the VM and under `oskel build`.
  See `docs/language.md` §12.11/§18, `tests/57_std_path.oskl` and the AOT
  fixture `tests/native_build_path.oskl`.
- **Standard library: `std.collections`** — **done**: a pure-Oskel module of
  functional helpers over arrays. It offers `map`, `filter`, `reject`, `each`,
  `fold`, `sum`, `sumBy`, `minimum`/`maximum`, `minBy`/`maxBy`, `count`,
  `find`/`findIndex`, `any`/`all`/`none`, `take`/`drop`, `partition`, `chunk`,
  `zip`, `flatten`, `reverse`, `unique`/`uniqueBy`, `groupBy`, `countBy` and
  `sort`/`sortBy`/`sortByKey`. Every function returns a new array or map and
  never mutates its input; `sort` is a stable merge sort. No new runtime type,
  opcode or builtin. The module stays on the portable primitive surface so it
  also runs on WebAssembly, where the array methods are not bound; the VM and
  the AOT backend bind them, so either form works there.
  See `docs/language.md` §12.12/§18, `tests/58_std_collections.oskl`
  and the AOT fixture `tests/native_build_collections.oskl`.
- **Standard library: `std.strings`** — **done**: a pure-Oskel module of text
  helpers that the string methods do not express: `words`, `isBlank`, `countOf`,
  `indexOfAll`, `containsAll`, `containsAny`, `padBoth` and `truncate`. Every
  length and index is a code-point count; occurrences are non-overlapping. The
  module stays on the portable primitive surface (indexing, slicing, `len`,
  `for ... in`, closures, `panic`), so it behaves identically on the development
  VM, under `oskel build` and on WebAssembly. Case mapping stays with the
  Unicode `upper`/`lower` methods. No new runtime type, opcode or builtin. See
  `docs/language.md` §12.13/§18, `tests/59_std_strings.oskl` and the AOT/WASM
  fixture `tests/native_build_strings.oskl`.
- **Standard library: `std.io`** — **done**: in-memory text streams built on
  classes. `StringReader` walks a string by code point (`readLine`, `read`,
  `readChar`, `readToken`, `readWhile`, `lines`, ...); `StringWriter`
  accumulates chunks (`write`, `writeln`, `writeAll`, `writeLines`, `toString`).
  `splitLines`/`joinLines` are the free-function line helpers. File access
  deliberately stays with the `readFile`/`writeFile`/`fileExists` builtins: the
  freestanding AOT backend does not seed them, so a module that mentions one
  fails `oskel build` even when the function is unused. No new runtime type,
  opcode or builtin; classes are maps with closures, so the VM, the AOT backend
  and WebAssembly agree. See `docs/language.md` §12.14/§18,
  `tests/62_std_io.oskl` and the AOT/WASM fixture `tests/native_build_io.oskl`.

### PHASE 6.6 — FFI with C (1.0)

- `extern "C" { ... }` blocks, and the single-line `extern fn ...;` form
  (implicit `"C"` ABI); both are sugar for the same representation.
- Compile-time stubs (like Zig/Rust).
- **No `libffi` in the AOT backend.** The development engine (`oskel`) may use
  libffi for callbacks; the freestanding AOT runtime never does (callbacks are
  compile-time stubs there). This is also why the compiler validates a
  declaration against the *destination*: an engine built without libffi still
  accepts, for `oskel build`, every shape the backend marshals itself.
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
  whose members land in `rax`/`rdx`/`xmm0`/`xmm1` and rebuilt as a map. A
  nested (non-scalar) field is rejected. No new opcode and no compiler-side ABI
  logic: the layout is the single source of truth in `ffi_types.h`, shared by
  the compiler and the runtime across the §2 boundary. Struct-by-value is
  **System V AMD64 only**: the register classification is that ABI's, and the
  Microsoft x64 ABI (Windows) passes a struct larger than 8 bytes through a
  hidden pointer instead. Off System V the compiler rejects the declaration
  (`kStructByValueSupported` in `ffi_types.h`; the CMake capability
  `OSKEL_FFI_STRUCT_BY_VALUE` skips the script test), so a Windows build fails
  cleanly at compile time instead of crashing at the call. Scalars stay portable
  because the call is a typed function pointer. See `docs/language.md` §4.6 and
  `tests/31_ffi_struct.oskl`.
- **Class-MEMORY structs by value (done, PHASE 7f.7)** — a struct larger than
  two eightbytes (more than 16 bytes) is class MEMORY: it travels in the
  outgoing stack-argument area and is returned through a hidden `sret` buffer the
  caller supplies in `rdi`, which shifts every integer argument one register
  along. The development engine expresses both directions through `libffi`
  (`kMemoryStructsSupported`, so it needs libffi + System V AMD64; elsewhere the
  declaration is rejected at compile time). The validation is per *destination*,
  not per build: the compiler asks `validateSignature` with the ABI the target
  marshals (`Compiler::setAbiCapabilities`), so `oskel build` accepts a MEMORY
  struct with `ffi::kAotAbiCapabilities` even on an engine built without libffi,
  while the interpreter keeps `AbiCapabilities{}`, this build's. The hosted AOT
  backend expresses them in its register thunks: a MEMORY argument is written
  field by field straight into the outgoing area, the `sret` pointer is staged with the other
  integer arguments, and the returned bytes are read back out of the buffer and
  boxed into a map, exactly like a register-class return. The thunk frame keeps
  the outgoing area, the `sret` buffer, the register-class staging buffers and
  the integer staging slots in disjoint regions so nothing overlaps. Each field
  is boxed from the `AotMap` and back: an integer field is normalized to its
  declared width, a `float` field is narrowed to binary32 on the way out and
  widened on the way back, a `cstring` field is NUL-terminated in the arena on
  the way out and copied into an `AotString` on the way back (NULL is nil, and
  `type()` reports `"string"` for a non-NULL one -- the same documented VM/AOT
  divergence as a `cstring` scalar return, BACKLOG §6 item 23), and a `ptr`
  field boxes a NULL as nil and any other address as the integer it is. See
  `tests/native_build_extern_struct.oskl` (registers) and
  `tests/native_build_extern_memory_struct.oskl` (MEMORY, with
  `tests/hosted/memory_struct_helper.c`, which also covers the narrow,
  mixed-class, `ptr` and `cstring` field cases).
- **Callbacks (done in both engines)** — a parameter typed `fn(P...): R` is a
  C function pointer. The `libffi` closure below is the *development engine*
  implementation; the AOT backend lowers the same declaration to a generated
  stub (PHASE 7f.8). `fn` is a keyword, so `parseTypeAnnotation` reads the signature before
  the ordinary type-name path; the parameters are scalar FFI names and
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
  extern call, so the trampoline address stays valid while C runs. A callback C
  stores for a later call is out of scope in the development engine (the
  closure is released when the extern call returns); the AOT backend's
  stub/context lives in the arena and supports it (BACKLOG §6). An error raised
  inside a callback cannot unwind through the C frame, so the extern call
  captures it and re-raises it once C returns; `pcall` catches it like any other
  runtime error. `nil` is the NULL function pointer. `libffi` is a
  **development-engine-only** dependency (§5.1/§6): the AOT backend lowers
  callbacks to generated stubs and never links it. The boundary check
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
  time (`OSKEL_FFI_VARIADIC` skips the script test). `oskel build` does not
  consult that flag: the AOT backend lowers the variadic itself on System V and
  Win64 (7f.10). See `docs/language.md` §4.6 and
  `tests/53_ffi_variadic.oskl`.

### PHASE 6.7 — Error handling ✅

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

### PHASE 6.8 — `class` (sugar over the map-plus-closures idiom) ✅

A `class` is a named shape **with methods**, and it is **pure parser sugar**:
the parser rewrites the declaration into an ordinary top-level constructor
function, so a class adds no AST node, no opcode, no compiler path and no VM
logic. This is what keeps it inside the "no VM-only features" rule (§3): the
desugared program is plain Oskel that the AOT backend already lowers, so the
feature is identical on the VM and in a native binary with zero backend work.

The desugaring of

```oskel
class Point {
  x: int;
  y: int;
  fn init(x: int, y: int) { self.x = x; self.y = y; }
  fn lenSquared(): int { return self.x * self.x + self.y * self.y; }
}
```

is the equivalent of

```oskel
fn Point(x: int, y: int) {
  let self = { x: nil, y: nil };
  self.init = (x: int, y: int) => { self.x = x; self.y = y; };
  self.lenSquared = () => { return self.x * self.x + self.y * self.y; };
  self.init(x, y);
  return self;
}
```

A method's declared return type rides on the desugared lambda (a lambda the
user writes has no return-annotation position), so the body's `return` is still
checked.

Design decisions, all enforced by the parser:

- **No `new`.** The class name *is* the constructor (`Point(3, 4)`), and the
  class name is a value of type `function`.
- **Explicit `self`.** A method reaches its receiver through `self`; the name is
  reserved inside a class body (a field, a method or a parameter may not use it).
- **No inheritance.** There is no `extends`, no `super` and no base class;
  composition is how one class reuses another.
- **`init` is optional** and defines the constructor's parameters. A variadic
  `init` is rejected (the constructor forwards its arguments one by one). Without
  `init` the constructor takes no arguments and the fields keep their initial
  `nil`.
- **A class name is a valid type annotation.** `classNames_` is pre-scanned in
  `parseProgram` (like `structNames_`) so an annotation written before the
  declaration is accepted, and the compiler records the name of every desugared
  constructor (`FnDeclStmt::isClassConstructor` → `Compiler::classNames_`) so
  the REPL seeds it across entries (`Parser::seedClassNames`).
- **Rejected at parse time:** a nested class (not top level), a duplicate class
  name, a class/`struct` name collision, a duplicate field or method, a field
  and a method sharing a name, `self` as a member name, and a variadic `init`.
- **A field may carry a default** (`n: int = 5;`, PHASE 6.8.1). The desugaring
  seeds the instance map with the default expression instead of `nil`, so it is
  evaluated per instance and `init` may still override it.
- **An `init` must set every non-nullable annotated field that has no default**
  (PHASE 6.8.1). `Parser::collectSelfWrites` walks `init`'s body collecting
  `self.<field> = ...` targets and reports a field it never reaches. The walk
  covers the nested statement forms (`if`, loops, `switch`, `defer`) and
  deliberately does not descend into a nested function or lambda, whose body runs
  later, if at all. The check is syntactic: it catches a forgotten field, not
  every path through `init`.
- **Only `=` and `??=` count as setting a field** (PHASE 6.8.1). `??=` falls back
  to its right operand when the field is still `nil`, so it initializes. Every
  other compound *reads* the field first: `self.n += 1` on a field that is still
  `nil` faults at run time, so counting it would pass the check and then crash
  the constructor. `SelfWrites::readFirst` records those, and the diagnostic says
  the field is only updated in place instead of claiming it was never assigned.
  A compound on a field that *has* a default is fine.
- **A method may be replaced, but only by a function of the declared arity**
  (PHASE 6.8.1). A method is a map key holding a closure, so a non-function
  value would make it silently uncallable and another arity would be a latent
  crash (the body reads its parameters positionally). A lambda written at the
  assignment site is checked directly, including its parameter types; any other
  value only has to be known to be a function. A receiver of unknown type stays
  unchecked, as everywhere else.
- **A field name within two edits of a declared one is a warning** (PHASE 6.8.1).
  An instance is a map, so writing a new key is allowed and must keep working;
  but a near miss silently adds a field while every read of the real one keeps
  returning its old value, so `Compiler::checkFieldAssign` reports it through
  `Compiler::warn` and the program still runs.

Consequences of the desugaring, documented so they are not surprises:

- An instance **is a map at runtime** (`type(p)` is `"map"`), so the map surface
  applies (`in`, `len`, `for ... in`, indexing, nesting, collections).
- Two instances compare **unequal** even with identical fields, because their
  method entries are distinct closures and closures compare by identity (§3.2).
- A class name **is** a module's public name, because a class desugars to a
  top-level constructor `fn`: `geom.Point(3, 4)` reaches it through the qualified
  form and the value is a `function`. (A `struct` name, by contrast, is not a
  value and stays local to its file.) A module that wants the class private
  exports a plain constructor function instead.
- A **field** annotation is enforced on assignment, like a `struct` literal's
  field: `self.field = value` inside a method and `instance.field = value`
  through a receiver whose type is known statically are checked against it
  (`Compiler::checkFieldAssign`). The declared type is carried to the compiler on
  the constructor declaration (`FnDeclStmt::classFields`) and on each method
  lambda (`LambdaExpr::ownerClass`), which is what types `self`; a lambda written
  *inside* a method inherits that owner, because it captures the same `self`. A
  receiver is statically known when it is an annotated binding, a constructor
  call, a call to a function with a declared return type, or a field of another
  statically known instance (`inferExprType`, PHASE 6.8.1). Three cases stay
  unchecked: an unannotated field, a receiver whose type is not known statically
  (an array or map element, an unannotated parameter or function), and a
  compound assignment (`self.x += 1`), whose result type is not tracked
  statically.
- `class` is a **reserved word** from 0.1.0 on.

Tested by `tests/58_classes.oskl` (VM), the `tests/errors/class_*.oskl` cases
(parse-time rejections), `tests/60_class_hardening.oskl` and
`tests/61_class_field_typo.oskl` (the PHASE 6.8.1 surface, registered as
`oskel.build_binary_class_hardening` for the AOT parity check) and
`tests/native_build_classes.oskl` (the AOT parity
fixture, registered as `oskel.build_binary_classes`, which asserts the native
binary prints byte for byte what the VM prints).

### PHASE 7 — Bytecode → native backend (AOT, freestanding) 🔶 in progress
- New module `src/frontend/native/` + `include/native/`: lowering of every opcode to
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
      (`include/native/elf.h`, `src/frontend/native/elf.cpp`) and the PE32+ emitter
      (`include/native/pe.h`, `src/frontend/native/pe.cpp`); the bump allocator
      (`include/native/arena.h`, `src/frontend/native/arena.cpp`); `PlatformFreestanding`
      (`include/native/platform_freestanding.h`,
      `src/frontend/native/platform_freestanding.cpp`), the second `Platform`
      implementation, reaching the kernel through direct Linux x86-64 syscalls
      (`write`, `read`, `openat`, `close`, `lseek`, `mmap`, `exit_group`) with
      no libc; and the entry point (`include/native/entry.h`,
      `src/frontend/native/entry.cpp`), which appends the `_start` and the runtime
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
      `src/frontend/native/runtime.cpp`) emits, as machine code in the same buffer as
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
      `1.0f/3.0f` → `"0.33333334"`). `src/frontend/native/dtoa.cpp` implements a
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
      quoted strings, `<fn name>`), depth-capped at 8 so a self-referential
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
    - **7c.3f** (done) — the allocating array methods: `slice`, `concat`,
      `reversed` and `clone`. Each returns a *new* array and leaves the receiver
      untouched, so unlike 7c.3c/7c.3e they need to allocate a fresh
      `AotArray` header plus its items block from the arena. The binding is the
      same as the other methods (the receiver captured in a shared cell, the
      body reached through `oskel_call`), with four new selectors (9..12) in
      `bindArrayMethod`'s table. A shared `emitArrayAlloc` helper allocates the
      header and the block once, sized to the exact item count, so no method
      grows a block incrementally. `slice(start?, end?)` follows the hosted
      `bindArray`: both bounds are optional (defaults `0` and the length), a
      negative bound counts from the end, every bound is clamped to
      `[0, length]` and `end < start` is raised to `start`. `concat(other)`
      copies the receiver's items then `other`'s and fails with
      "concat() expects an array" for a non-array. `reversed()` copies the
      items in reverse; `clone()` copies them in order (a shallow copy, so the
      copy shares any nested arrays, as in the VM). The methods read the
      receiver through the same cell as the in-place ones, so a detached binder
      (`var s = a.slice;`) still works. The emitters keep every live value in a
      stack slot and never touch r13/r14 (the allocator's cursor and end), as
      the other arena helpers do. No new opcode and no compiler change: the
      member form of `GETTABLE` plus `CALL` already carries it, so the `.oskc`
      is untouched. Covered by `tests/lower_test.cpp`
      (`testExecArrayMethodsAlloc`), the freestanding `test_array_methods_alloc`
      (values and the error paths) and the `oskel build` fixture.
    - **7c.3g** (done) — the bound map methods: `keys`, `values`, `entries`,
      `has`, `get`, `set`, `delete`, `clear`, `merge` and `clone`. They are the
      map twin of the array methods and bind the same way (the receiver captured
      in a shared cell, the body reached through `oskel_call`), with ten
      selectors (0..9) in `bindMapMethod`'s table and a shared
      `emitLoadMapReceiver`. `keys`/`values`/`entries` materialise a fresh array
      through `emitMapListMethod` + `emitArrayAlloc`: `keys` yields the key
      strings, `values` the value slots and `entries` a two-element `[key,
      value]` array per entry. `has`/`get`/`delete` share a one-string-key
      preamble (`emitMapKeyQueryMethod`) and a linear `mapFind`; `set` upserts
      and `merge` folds another map in (last writer wins), both returning the
      receiver, and `delete`/`clear` mutate it in place. The list loop keeps its
      state in stack slots, never in r13/r14 (the allocator's cursor and end),
      because the entry filler allocates and an allocation clobbers every
      caller-saved register. An unknown map member is `nil`, matching the hosted
      `ops::memberGet`, which returns `Value()` for a map rather than failing
      (arrays still fail with "value has no such member"). No new opcode and no
      compiler change: the member form of `GETTABLE` plus `CALL` already carries
      it, so the `.oskc` is untouched. Covered by the freestanding
      `test_map_methods` (values, mutation and the error paths) and the
      `oskel build` fixture `tests/native_build_map_methods.oskl`.
    - **7c.3h** (done) — the callback-taking and folding array methods:
      `map`, `filter`, `each`, `find`, `any`, `all`, `reduce`, `sum` and `join`.
      The first six are the first emitted helpers that call *back* into the
      program: each invokes the closure it is handed through `oskel_call`, once
      per item, so a callback runs real Oskel code (allocating, recursing,
      capturing locals) inside the helper. The binding is the one 7c.3c/7c.3e
      already use (the receiver captured in a shared cell, the body reached
      through `oskel_call`), with nine new selectors (13..21) in
      `bindArrayMethod`'s table. `map` stores the callback's result and
      `filter` the item it was given; `each` returns the receiver, `find` the
      matching item or `nil`, and `any`/`all` short-circuit to a boolean.
      `reduce` is the first helper to hand its callback *two* values: a callee
      reads `args[i]` at `args - 16*i` (the register file grows down), so the
      argument window is built with the accumulator at its highest address --
      the ordering a stack-slot mix-up silently breaks. `sum` mirrors the
      hosted all-int fast path (a wrapping integer add until the first
      float/double, then a binary64 total seeded from the integer one); `join`
      renders every item, and the separator, through `$str`, since the hosted
      `join` calls `toString` on whatever it is given. Only rbx and r12 are
      saved around a callback: r13/r14 are the arena cursor and end and must be
      left advanced by whatever the callback allocated, and r15 (the global
      table) is never written. No new opcode and no compiler change: the member
      form of `GETTABLE` plus `CALL` already carries it, so the `.oskc` is
      untouched. Covered by the `oskel build` fixtures
      `tests/native_build_array_callbacks.oskl` and
      `tests/native_build_array_folds.oskl`. `sort`/`sorted` remain open (they
      need a stable sort that calls a comparator), as do the string methods.
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
      by doubling in the arena. A `Func` renders the text its closure's
      `display` field points at (`<fn name>`, `<native fn name>`), so
      `$str`/`print` agree with the VM; a closure built with no name falls
      back to the literal `<fn>` (BACKLOG §6.22). Covered by
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
    - **7c.5b** (done, except the VM-only builtins listed in BACKLOG §6)
      — the remaining builtins. **7c.5b.1** (done) adds
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
      `_start`) and the `oskel build` fixture. **7c.5b.2c** (done) makes the
      three conversions report a failed conversion with the hosted engine's
      exact text. The parsers now return *why* they failed -- 1 malformed, 2
      out of range, 3 empty after trimming -- and the builtin assembles
      `cannot convert '<operand>' to <target>` plus an optional cause
      (` (out of range)`, ` (use inf, -inf or nan)`) in the arena, so a string
      operand quotes its own bytes and a non-string one quotes its type name.
      The whole-string cases keep their own text (`cannot convert an empty
      string to int`, `cannot convert a non-finite float to int`, `float is out
      of the integer range`). The builder and the operand live in stack slots,
      never in r13-r15: those three registers are the arena cursor, the arena
      end and the global table, and clobbering them silently breaks every later
      allocation. The parser checks "empty" on the trimmed text *before* it cuts
      at the first NUL, so `float("\u0000abc")` is malformed rather than empty,
      matching the hosted `strtod` path. **7c.5b.3** (done) adds the exact
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
      `try`/`catch` work in a native binary.
    - **7c.5d** (done) — `pcall` in AOT, the one non-local return in the call
      ABI. Value-based recovery (`try`/`catch` over a returned error) needed no
      unwinding, but `pcall` calls a function the program chose at run time and
      must catch a `panic` raised *inside* it, and the AOT runtime reports a
      failure by writing to stderr and exiting. The builtin now installs a
      handler record in its own frame and publishes the record's address in the
      hidden `$pcallHandler` global; `oskel_fail_dynamic` checks that slot before
      printing and, when it is set, stores the message in the record, restores
      the `pcall` frame's `rsp`/`rbp` and jumps to the resume address the record
      saved. The record carries the previous handler, so a nested `pcall` and a
      normal return both restore the chain. The slot, the builtin body, the three
      result keys and the sink's handler check are emitted only when the program
      calls `pcall`, so every other image is byte for byte what it was. Covered
      by `tests/native_build_pcall.oskl` (built and run against the VM by
      `tests/build_binary.cmake`): a success, a caught failure, a nested catch
      and a closure. What stays out of scope is the *other* direction: a raised
      runtime error with no active `pcall` still exits 70.
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
    (`emitWindowsRuntime` in `src/frontend/native/entry.cpp`), which reaches kernel32
    instead of issuing syscalls — `oskel_write` marshals `GetStdHandle` +
    `WriteFile` (retrying for a full write, with the mandatory NULL
    `lpOverlapped` and shadow space), `oskel_exit` calls `ExitProcess` and
    `oskel_init_arena` calls `VirtualAlloc`. `src/frontend/native/pe.cpp` emits a static
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
    through 7f.6 landed the hosted object + link and scalar/pointer `extern "C"`
    marshalling; the struct, callback and variadic shapes remain.
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
      pins the rejection. The fixture declares `getpid` rather than `clock`:
      `clock` became an AOT builtin (BACKLOG §6 item 25), so it no longer
      exercises a stub-less binding.
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
      `std.math` libm surface resolves from the embedded musl ports and `clock`
      is an AOT builtin since BACKLOG §6 item 25, so the rejection is exercised
      with `modules.hostclock.getpid`, a libc call with no stub),
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
      pin the (target, mode) matrix — freestanding for Linux, Windows and bare
      metal, hosted for Linux, Windows and macOS x86-64 (macOS has no
      freestanding image) — so an unsupported pair is reported instead of
      emitting bytes. This slice changes no code path for freestanding: the
      emitted binary is byte for byte identical to before. Covered by
      `tests/build_mode.cmake`.
    - **7f.4** (done) — the hosted image: a relocatable object plus a link
      against libc. The emitter gains external references (`ExternalRef` in
      `include/native/codegen.h`): `callExternal` emits `call rel32` to an
      undefined symbol and records the displacement slot, and `leaRipExternal`
      does the same for an address; `exportSymbol` records a defined symbol
      (the program's `main`). `src/frontend/native/object.cpp` wraps the code
      buffer in the target's container: ELF64 `ET_REL` on Linux (`.text`,
      `.rodata`, `.rela.text` with `R_X86_64_PLT32` for a call and
      `R_X86_64_PC32` for an address, both addend `-4`), Mach-O `MH_OBJECT` on
      macOS (`__text`/`__const`, `X86_64_RELOC_BRANCH`/`X86_64_RELOC_SIGNED`,
      `LC_SYMTAB`, underscore-prefixed names) and COFF
      `IMAGE_FILE_MACHINE_AMD64` on Windows (`.text`/`.rdata`,
      `IMAGE_REL_AMD64_REL32`, an 18-byte symbol table and its string table).
      `emitHostedRuntime` (`src/frontend/native/entry.cpp`) binds the same entry
      handles the runtime already references but reaches the system through the
      host C library (`write`/`exit`/`malloc` on Linux and macOS,
      `_write`/`exit`/`malloc` plus `_open`/`_read`/`_close` and
      `GetTickCount64` on Windows) instead of a raw syscall, and exports the
      program entry as the C `main` the C runtime calls — the lowering's `main`
      already has the `int main(void)` shape, so no thunk is needed. `oskel
      build --mode hosted` writes the object beside the output and links it with
      the host C compiler (`$CC`, default `cc`, `-no-pie -lm` on Linux;
      `-arch x86_64 -lm` on macOS; `cl /Fe:...` on Windows), removing the
      temporary object either way. The freestanding path is untouched and its
      binary is byte for byte identical; a hosted binary agrees with the
      development VM byte for byte. Covered by `tests/object_test.cpp` (all
      three containers) and `tests/build_mode.cmake` (the hosted build runs and
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
      path. The thunk itself (`emitExternThunk`, `src/frontend/native/runtime.cpp`) has
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
    - **7f.6** (done) — `cstring`/`ptr` parameters and a `cstring` return in the
      hosted backend. The scalar thunk of 7f.5 is extended to the pointer
      register class, so a program can pass text to a C function and read text
      back. A `cstring`/`ptr` *parameter* travels in a general-purpose register
      as a machine address: `nil` marshals to C's NULL (0) and a Oskel string is
      copied into a fresh, NUL-terminated arena block whose address is passed,
      because an `AotString` is a length word followed by the bytes and is
      *not* NUL-terminated — exactly the `const char *` a C callee dereferences.
      Marshalling a pointer argument calls `alloc` and `rep movsb`, which
      clobber the integer argument registers, so every integer argument is now
      staged in a small stack frame and loaded into its register only just
      before the call (the SSE arguments are written directly, since nothing
      clobbers an xmm register). A `cstring` *return* comes back in `rax`: NULL
      becomes `nil`, otherwise the bytes are scanned to the NUL (with the hosted
      engine's 16 MiB cap, `src/runtime/ffi.cpp`) and copied into an arena block
      that also holds an `AotPtr` header and the terminator, which the thunk
      returns as a `Ptr` of kind `CString` — the same handle `cstring(text)`
      builds. That is what the VM returns for a `const char *`, so `type()` says
      `"cstring"`, equality is handle identity (never text) and `+` rejects the
      value, all matching the development engine (BACKLOG §6 item 23, closed).
      The parameter side accepts a `Ptr` as well as a `Str`: a handle already
      owns NUL-terminated bytes, so it crosses as its own address with no copy,
      which is how a `cstring` returned by one C function is fed to another. A
      `cstring` field of a returned struct is boxed the same way. No thunk is
      emitted in freestanding mode, so its image stays byte for byte identical.
      An opaque
      `ptr` *return* is still rejected: the AOT runtime has no representation
      for a handle it must never dereference. A struct, a callback and a C
      variadic remain 7f work. Covered by
      `tests/native_build_extern_hosted.oskl`, extended with
      `strlen`/`strcmp`/`strncmp` (a pointer then a `size_t`, exercising the
      staging order) and `strchr` (a `cstring` return, including the NULL
      no-match case), built hosted and checked against the VM.
    - **7f.8** (done) — C callbacks in the hosted backend. A parameter typed
      `fn(P...): R` is no longer left out: `oskel_callback_create` allocates a
      32-byte context (the closure, the Oskel global base and a pointer to a
      shared arena-state block) and a private `mmap` page holding
      `movabs r10, context; movabs r11, body; jmp r11`, so the C callee receives
      a real function pointer. The body saves the C arguments, boxes a
      `ptr`/`cstring` into a handle, calls the closure through `oskel_call`, and
      converts the result to the callback's C return class
      (int/bool/float/double/cstring/ptr). A C caller expects r13/r14/r15
      preserved, but they are the Oskel arena cursor/end and global base: the
      body saves the C values, loads the Oskel ones from the shared state, runs
      the closure, parks the advanced cursor/end back, and restores the C
      values. Every extern thunk in a program with a callback publishes the
      current cursor/end before the call and reloads it afterwards, and the root
      epilogue publishes the final state, so a callback C stores for a later
      call (`atexit`, a setter/invoker pair) resumes from the program's current
      arena. The page is mapped read+write while the stub is copied in, then
      `mprotect`ed to read+execute (W^X); it is never W+X. It is
      process-lifetime: C may store the pointer, so freeing it when the extern
      call returns would break a stored callback (BACKLOG §6 item 28). A
      callback returning a struct by value and a callback with a struct
      parameter are rejected with a dedicated build error; a C variadic is
      lowered separately (7f.10). Covered by
      `tests/native_build_extern_callback.oskl` (`qsort`,
      VM parity), `tests/native_build_extern_callback_stored.oskl`,
      `..._exit.oskl` (an `atexit` handler after the chunk returns),
      `..._wx.oskl` (the stub page is not W+X), `..._many_args.oskl` and
      `..._returns.oskl` (AOT-only helpers) and
      `tests/errors/callback_struct_{return,param}.oskl` (rejections).
    - **7f.9** (done) — hosted mode on Windows and macOS. The hosted object and
      link were Linux-only because both halves named the platform: the emitter
      always wrote ELF64 and the linker command always drove a POSIX `cc`. The
      container now follows the target: Mach-O `MH_OBJECT` for
      `x86_64-macos-sysv` (one `LC_SEGMENT_64` with `__text`/`__const`,
      `LC_BUILD_VERSION` for macOS 10.15, `LC_SYMTAB`, pcrel
      `X86_64_RELOC_BRANCH`/`X86_64_RELOC_SIGNED` and the leading-underscore
      names the Mach-O ABI expects) and COFF `IMAGE_FILE_MACHINE_AMD64` for
      `x86_64-windows-win64` (`.text`/`.rdata`, `IMAGE_REL_AMD64_REL32`, an
      18-byte symbol table with a string table). `oskel build --mode hosted`
      picks the link line per target: `cc -no-pie -lm` on Linux,
      `cc -arch x86_64 -lm` on macOS (so an arm64 host can cross-link the
      x86-64 object) and `cl /MT /Fe:... /link /SUBSYSTEM:CONSOLE` on Windows,
      with `$CC` overriding the compiler on all three. The Windows object is
      not produced by `cl`, so it carries no `.drectve` asking for the CRT; the
      link names `libcmt.lib`, `libucrt.lib`, `libvcruntime.lib` and
      `kernel32.lib` explicitly, or every runtime symbol stays unresolved.
      When `$CC` is a wrapper (the Windows test drives `cl` through a
      `vcvarsall.bat` batch file), the file name says nothing about the command
      line, so `$OSKEL_CC_KIND=msvc` (or `posix`) names the dialect explicitly.
      macOS has no freestanding image, so `runBuildCommand` defaults its mode
      to `hosted` when the target is macOS and the user did not pass `--mode`;
      `oskelp build` therefore works there without an extra flag. The Windows
      entry prelude is separate (`emitWindowsHostedRuntime`) because the
      Microsoft x64 ABI uses rcx/rdx/r8/r9 plus 32 bytes of shadow space and
      the UCRT spells the file functions `_open`/`_read`/`_write`/`_close`;
      `time(NULL)` is the wall clock and `GetTickCount64` the monotonic
      counter. `main` moves the CRT's `(argc, argv)` from rcx/rdx to rdi/rsi
      before the chunk, so `args()` works there too. The extern thunk takes the
      ABI as a parameter: four integer and four SSE registers, shadow space,
      stack arguments from offset 32, and a fifth SSE value spilled to the
      outgoing stack area. The scalar, `cstring` and `ptr` classes are supported
      on Windows; a struct and a callback are rejected with a dedicated message
      (the System V fixtures keep covering those), and a C variadic is lowered
      positionally (7f.10). macOS uses System V, so it shares the existing thunk
      unchanged. Two Windows details
      are easy to miss: the UCRT opens stdout and stderr in text mode, so
      `oskel_init_arena` calls `_setmode(1/2, _O_BINARY)` before the first
      write -- otherwise every `\n` becomes `\r\n` and the binary disagrees
      with the VM -- and CTest does not inherit the environment `vcvarsall.bat`
      sets, so the test runner reaches `cl` through a generated batch wrapper
      that calls it first (without `LIB`, the link cannot find `libcmt.lib`
      even when `cl` is on `PATH`). The wrapper is derived from
      `CMAKE_C_COMPILER`, falling back to `CMAKE_CXX_COMPILER` when C is not
      enabled; the Visual Studio generator in the artifacts workflow leaves
      the former empty.
      Covered by `tests/object_test.cpp` (all three containers),
      `tests/build_mode.cmake` on Linux, Windows and macOS (the macOS arm64
      runner builds the x86-64 image but skips running it, and checks
      `otool -L` for libSystem), and
      `tests/native_build_extern_hosted_windows.oskl` on Windows.
    - **7f.10** (done) — C variadics on the hosted backend, Windows included.
      System V continues the independent GPR/SSE counters after the fixed
      prefix. Win64 assigns registers by **argument position**, not by class:
      a fixed `double` at position one is xmm1 (not xmm0), and the same rule
      picks the tail registers from `arity` onward. A floating value in a
      register position is duplicated in the integer register of the same
      position (the Microsoft varargs rule), and every position from four
      onward travels on the stack. The fixed frame of a Win64 variadic thunk
      carries no outgoing area; the thunk reserves it at run time as
      `32 + 8*fixedStackCount + 8*stackSlots + 8*tailCount` bytes (aligned to
      16), so the shadow space, the fixed stack arguments and the variable tail
      are one contiguous area and the staging slots sit above it. `%al` counts
      the vector registers actually used. A non-variadic Win64 thunk keeps the
      shadow space plus its fixed stack arguments in the frame, and its
      `gprUsed` mask loads only the integer registers an argument actually
      occupies. The mixed `ldexp`/`modf`/`remquo` calls in
      `tests/native_build_extern_hosted_windows.oskl` pin the positional fixed
      rule. `tests/native_build_extern_variadic.oskl` runs on Windows (AOT-only,
      because the development VM has no libffi variadic path there) and on
      macOS (built and linked on every host, run only on x86-64).
      `tests/native_build_extern_variadic_mixed.oskl` with
      `tests/hosted/variadic_mixed_helper.c` pins a mixed fixed prefix on Linux
      and Windows. The development engine's own variadic path still needs
      libffi plus System V; the AOT destination accepts the declaration through
      `kAotAbiCapabilities`, independent of how the engine was built.
  - **7g** (done) — bare-metal x86-64 (multiboot). A third target,
    `x86_64-freestanding-sysv` (`oskel build --target
    x86_64-freestanding-sysv`), emits a multiboot 1 kernel that a boot loader
    (QEMU `-kernel`, GRUB, Limine) loads at 1 MiB and enters in 32-bit
    protected mode. Because the CPU starts in 32-bit mode with paging off and
    multiboot leaves a *32-bit* code descriptor in the GDT, the image begins
    with a small hand-assembled 32-bit trampoline (`src/frontend/native/bare_metal.cpp`,
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
  - **7g.1** (done) — the `memSize()` builtin. It returns the machine's total
    physical memory in bytes as an `int`, the RAM analogue of `time()`. On the
    development VM it comes from the platform (`Platform::physicalMemory`:
    `sysconf(_SC_PHYS_PAGES)` on POSIX, `GlobalMemoryStatusEx` on Windows, and
    `0` for a platform with no view of the machine, so the value is never
    invented). The AOT backend implements it for the bare-metal target only:
    the multiboot header now sets flag bit 1 (`kMultibootFlagMemory`), the
    prelude captures the multiboot info pointer that the boot loader passes in
    `EBX`, and `oskel_mem_size` walks the memory map (`mmap_addr`/`mmap_length`)
    summing the entries of type 1 (available), with the older
    `mem_lower`/`mem_upper` fields as a fallback. On any hosted target the
    lowerer rejects a call with `memSize() is only available on the bare-metal
    target (x86_64-freestanding-sysv)` instead of emitting a binary that reads
    a nil handle -- but only when the name is the builtin, so a user binding
    (`var memSize = 5`) still shadows it exactly as it shadows `print`. Covered
    by `tests/bare_metal_test.cpp` (`test_multiboot_requests_memory`,
    `test_qemu_mem_size`, which boots `-m 256M` and asserts `255 MiB`),
    `tests/lower_test.cpp` (`testMemSizeTargetGate`) and `tests/vm_test.cpp`
    (`testMemSizeReadsThePlatform`).
- Deferred: per-instruction type annotations for fast paths without dynamic
  dispatch (bridge toward the speculative work in
  `docs/ARCHITECTURE_ANALYSIS.md`); other targets (macOS via libc or stubs;
  ARM64 bare metal).
- Afterwards, LuaJIT's JIT as a reference (see PHASE 8 below).
- **In the tree now**: the `Target` descriptor (`include/native/target.h`) and
  the x86-64 instruction emitter (`include/native/codegen.h`,
  `src/frontend/native/codegen.cpp`): labels with backpatching, `mov`/`lea`, integer
  ALU, shifts, compares, `setcc`/`movzx`, `jmp`/`jcc`, SSE2 float ops,
  `push`/`pop`, `call rel32`, `syscall` and `ret`, covered by the byte-exact
  tests in `tests/codegen_test.cpp` (the mmap/execution checks run on Linux
  x86-64 only). The 7b lowering (`include/native/lower.h`,
  `src/frontend/native/lower.cpp`) turned the runtime-free, integer-only opcodes into
  machine code, and 7c.1a replaced it with the boxed value model; both are
  tested by execution in `tests/lower_test.cpp`.
  7c.0 adds the freestanding substrate: the static ELF64 and PE32+ emitters,
  the arena, `PlatformFreestanding` (direct syscalls, no libc) and the
  `_start`/prelude, exercised end to end (a real generated binary is run) in
  `tests/freestanding_test.cpp`. 7c.1a adds the boxed value model
  (`include/native/value.h`) and the embedded runtime
  (`src/frontend/native/runtime.cpp`): a register is a 16-byte `AotValue` slot in a
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

### PHASE 9 — WebAssembly backend 🔶 in progress

The same bytecode that the VM runs and the x86-64 backend compiles can also be
emitted as a `.wasm` module. This is a **new backend behind the existing
`Target` descriptor**, not a rewrite: `Target::wasm32()` is
`{Arch::Wasm32, Os::Freestanding, Abi::Wasm}`, `targetName()` already prints
`wasm32-freestanding-wasm`, and `oskel build --target wasm32-freestanding-wasm`
selects it. `isWasmTarget()` gates the dispatch in `src/frontend/cli.cpp`;
`isSupportedTarget()` stays x86-64-only on purpose, because
`requireSupportedTarget` guards the x86-64 emitter and would otherwise emit x86
bytes into a `.wasm` file.

The backend lives in `include/native/wasm.h` and
`src/frontend/native/wasm.cpp`. It shares the **value model** with the AOT
backend but none of its emission code:

- A small module builder writes the LEB128-encoded sections (type, import,
  function, table, memory, global, export, element, data, code).
- A `Runtime` emits a WASM helper per operation the x86-64 runtime implements
  in machine code: allocation, string formatting, `toStr`/`repr`, truthiness,
  iteration, type names, every binary/unary/comparison operator, `in`, member
  access, and the builtins. The helpers are ordinary WASM functions, so the
  engine validates and compiles them; there is no hand-written binary blob.
- Each Oskel register is a pair of WASM locals (an `i64` tag and an `i64`
  payload). The module memory holds the globals table (16 bytes per slot, at
  offset 0), the static string pool and the bump arena; a string is an 8-byte
  length followed by UTF-8 bytes, and a closure is a 40-byte block
  (`table slot`, arity, upvalue count, name address, flags) followed by one
  cell pointer per upvalue.
- Control flow is a `loop` + `br_table` dispatcher over basic blocks: the
  lowering splits each proto into blocks at jump targets and after terminators,
  keeps the program counter in a local, and branches back to the dispatcher.
  Calls go through one `call_indirect` type
  `(i32 args, i32 argc, i32 self) -> (i64, i64)`; the callee's table slot is
  read from its closure, so builtins and program functions share one table.
- The module imports `oskel.write(fd, ptr, len) -> i32`, `oskel.exit(code)`,
  `oskel.time() -> i64` and `oskel.clock() -> f64`, and exports `_start`.
  `tests/run_wasm.cjs` is the Node host that supplies them.

Supported today (PHASE 9.1): `nil`/`bool`/`int`/`float`/`double`/`string`,
ranges, every operator (`+ - * / % ** & | ^ << >>`, unary `- ! ~`, the six
comparisons, `in`), `for`/`while`/`do`/`switch`/`break`/`continue`, globals,
locals, closures and upvalues (captured locals are boxed in cells), error
values with `message`/`code`, character indexing, arrays and maps (literals,
indexing with negative indices, assignment/append, `length`, `for ... in`
including the key/value form, `in`, structural `==`, array concatenation,
`str`/`repr`), slicing through `$slice` (arrays and UTF-8 strings, positive and
negative steps, Python clamping), variadic functions (`rest...` is packed into
an array), and the builtins `print`, `write`, `eprint`, `len`, `type`, `str`,
`$str`, `repr`, `int`, `float`, `double`, `bool`, `chr`, `ord`, `floor`,
`ceil`, `round`, `sqrt`, `assert`, `assertEq`, `panic`, `error`, `isError`,
`$isError`, `$nonNull`, `range`, `$range`, `$slice`, `time`, `clock` and
`exit`. The array, map and string methods are bound too (the same list as the
x86-64 backend), so `std/json`, `std/datetime`, `std/collections`, `std/path`
and the bound half of `std.math` build and agree with the VM byte for byte; the
host clock is what lets `std/datetime` read `time()`/`clock()`.

Still open, and each reported rather than mis-compiled:

- **Transcendental builtins** (`sin`, `cos`, `tan`, `log`, `exp`, ...) are not
  bound yet; `std.math` builds and its `abs`/`min`/`max`/`clamp`/`sqrt`/`PI`/`E`
  subset agrees with the VM, but a call to an unbound name reports the gap.
- A builtin the backend does not bind is **seeded as a closure that raises a
  clear runtime error when called**, not left unseeded: a module that merely
  *declares* `native fn sin` (through `import std.math`) still builds, while a
  call to `sin` reports the gap. The list of unbound names is
  `supportedBuiltins()` in `src/frontend/native/wasm.cpp`.
- A used `extern "C"` or `native fn` symbol with no builtin stub is a build
  error, the WASM counterpart of the AOT backend's message.
- `double` printing is the shortest round-trip form the VM emits. The WASM
  backend ports the x86-64 Dragon4 generator (`dtoa.cpp`): a static scratch
  block holds the `r`/`s`/`mp`/`mm`/`t` bignums, the digit buffer and the input
  fields; emitted helpers (`bnSetU64`, `bnSetU32`, `bnCopy`, `bnCmp`,
  `bnMulSmall`, `bnShl`, `bnSub`, `bnAdd`, `bnMulPow10`, `bnDivMod`) implement
  the arithmetic and `generate` fills `digits`, `nd` and `k`. `fmtDouble`
  formats from `nd`/`k` with the same fixed/scientific choice as the x86-64
  backend, including `-0`, subnormals and `e±NN`. `parseDoubleStr` combines the
  fraction into one `mantissa / scale` division and applies the exponent with
  one table multiply/divide. `tests/native_build_double_format.oskl` and
  `tests/native_build_math.oskl` pin it.

Testing: `tests/native_build_wasm.oskl` is compiled to a module and run under
Node by `tests/wasm_case.cmake` (CTest `oskel.build_wasm`), which compares its
output with the VM's byte for byte. The fixture covers the scalar core, arrays,
maps, slicing and variadics; `oskel.build_wasm_datetime`,
`oskel.build_wasm_collections`, `oskel.build_wasm_math` and
`oskel.build_wasm_double_format` add `std/datetime`, `std/collections`,
`std/math` and the full-range double formatter. The tests need Node and are
skipped where it is absent.

### Post-1.0 — UI library

- **SDL3** for windows, renderer and events: the low-level bindings landed as
  `std/sdl.oskl` (slice 1, `docs/sdl.md`), and a native binary links them with
  `oskel build --mode hosted --link-lib SDL3`.
- **C++ bridge** (`bindgen --cxx`, `docs/cxxbind.md`): a descriptor plus a
  generated C++ shim turns a C++ class into an Oskel class with an opaque
  `handle`, and a free function into a wrapper. A class with a C++ ctor is
  constructed with `Class(...)`; an adopt-only class takes
  `Class(handle, owned)`. Factory objects (`object Surfaces { ... }`) generate a
  map of functions, class returns carry `owned`/`borrow`/`sp`/`value`, and a
  class chooses `release "delete"`, `"unref"` or `"none"`. `Class&` parameters
  are dereferenced by the shim. The C++ ABI stays in the shim; the engine still
  speaks C. `bindings/skia/skia.oskelbind` is the first real descriptor and it
  compiles against a real Skia build; `tests/cxxbind/skia_test.oskl` checks
  drawing and pixel readback on the VM and the hosted AOT binary.
- **Skia** as the rendering backend for the widgets, on top of SDL3's window.
  `bindings/skia/skia_ui.oskl` renders the `std/ui` display list; the first
  slice targets a raster surface, and presenting that surface into an SDL3
  window is the next slice.
- **Declarative UI** (SwiftUI/Flutter style), first slice: `std/ui.oskl` has
  `window`, `column`, `row`, `padding`, `spacer`, `text` and `button`, a
  deterministic layout, a `rect`/`text` display list and button hit testing.
  `tests/native_build_ui.oskl` proves it on the VM and under `oskel build`.
  Input widgets (`Input`, `Checkbox`, `Slider`), an event loop and real font
  metrics are still pending.

### Post-1.0 — Other features

- **Cosmopolitan (APE)** for CLI multi-platform (optional).
- **Interfaces/traits** (for generic code without full generics).
- **Dynamic FFI** (`dlopen`, `dlsym`, runtime stubs with the project's own JIT).
- **Hot reload** (1.1+).
- **Extended stdlib**: JSON, dates, bytes, regex, networking.
- **Package manager** (`oskelp`, the Cargo-style project layer): the MVP landed
  with `oskel.toml`, `oskel.lock`, path dependencies and
  `new`/`init`/`build`/`run`/`check`/`test`/`clean`/`tree`; a local registry,
  `add`/`remove`/`update`/`publish` and a network registry are still pending.
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
Phase 6.5: ✅ completed (Java-style `import`, `switch`, `defer`, type annotations/inference/nullable `T?` and nullability expressions `?.`/`??`/`!!`, `struct`, `native fn`, variadic `rest...` parameters and the `std.math` module). Post-6.5 additions: slicing syntax `obj[start:end:step]` for strings and arrays with an optional step (still one `$slice` helper call, no new opcode), the short-circuiting compound assignments `&&=`, `||=` and `??=` (existing `EQ`/`NEQ`/`JMP_IF_*`/`MOVE` opcodes, no new opcode), the post-test loop `do { ... } while (cond);` (existing `JMP`/`JMP_IF_TRUE` opcodes, no new opcode), optional parentheses in `for ... in` (`for (x in xs)`, told apart from the classic header by a `( name in` lookahead), the key/value form `for k, v in map` (a second loop variable after a comma binds the value of each pair, which `ITER` now yields directly, no new opcode and no helper call) and brace-less single-statement bodies for `if`/`else`/`while`/`for`/`for-in` (a single statement wrapped in a block, so the bytecode is unchanged). The compound bitwise assignments `&=`, `|=`, `^=`, `<<=` and `>>=` close the last gap in the assignment operators: five new tokens in the lexer, one case each in `isAssignOp` and `compoundOpCode`, and the existing `BAND`/`BOR`/`BXOR`/`SHL`/`SHR` opcodes behind the same `GET; op; SET` shape as `+=` (no new opcode, VM and AOT in the same change).
Phase 6.6: ✅ completed (type vocabulary, `extern "C"` declarations + marshalling, pointers, structs by value and callbacks)
Phase 6.7: ✅ completed (6.7.1 `pcall`, 6.7.2 `error` type, 6.7.3 `try`/`catch`, 6.7.4 unions `T | E` and 6.7.5 `@MustUse` done)
Phase 6.8: ✅ completed (`class`: named shapes with methods, desugared by the parser into a top-level constructor function — a map plus closures capturing an explicit `self`; no `new`, no inheritance, no new opcode, and identical on the VM and the AOT backend)
Phase 7: 🔶 in progress (7a, 7b, 7c.0, 7c.1a-d, 7c.2a-d, 7c.3, 7c.3b, 7c.3c, 7c.3d, 7c.3e, 7c.3f, 7c.3g, 7c.3h, 7c.3i, 7c.3j, 7c.3k, 7c.3l, 7c.4a-d, 7c.5a, 7c.5b, 7c.5c, 7c.5d, 7c.6, 7d, 7e, 7g done; 7f — FFI in AOT — 7f.1-7f.10 done, scalar, `cstring`/`ptr`, struct-by-value `extern "C"` (register and class-MEMORY) and callbacks resolve in the hosted backend on Linux and macOS, and the scalar/`cstring`/`ptr` surface plus a C variadic on Windows. 7c.5b covers every builtin the AOT runtime can implement without the host, and the fourteen that needed one (files, `clock`, `args`, raw memory) landed as BACKLOG §6 item 25, so no builtin is VM-only any more -- a target that cannot back one rejects it by name; `random`/`seed` joined the AOT side in 7c.5b.6 and `pcall` in 7c.5d)
Phase 8: 🔶 in progress (JIT for the development VM, slices 1-4 done: integer leaf protos with constant folding, strength reduction, compare-branch fusion and web-based register allocation with copy coalescing)
Post-1.0: 🔶 package manager MVP (`oskelp`) landed; UI, Cosmopolitan, registry, VM embedded pending
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
3. **Structures**: `struct` for data, `class` for data plus methods (sugar over a map plus closures), `enum class` for opcodes.
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
