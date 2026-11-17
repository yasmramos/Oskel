# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is `0`, the language surface and the bytecode contract
may change between minor versions.

## [Unreleased]

### Changed

- **JIT (slice 3): compare-branch fusion.** When a comparison's boolean result
  only feeds the conditional branch right after it, the JIT no longer builds the
  bool (`cmp; setcc; movzx; mov; test; jcc`) and jumps straight off the flags
  (`cmp; jcc`). A backward liveness pass proves the result dead past the branch
  and that the branch is reached by fall-through alone (never a jump target), so
  the re-test the VM would perform is never lost; a result that is reused
  afterwards stays unfused and correct. The integer loop drops from 0.062s to
  0.051s (about 320x the interpreter, within ~1.4x of a hand-written `-O2` C++
  loop). Still 1:1 with the bytecode — the fusion rewrites how one opcode and its
  branch are emitted, and no opcode, superinstruction or runtime specialization
  is added. The differential suite (including the fuzz corpus) still proves the
  compiled body identical to the interpreter.

- **JIT (slice 2): constant folding, strength reduction and register-direct
  codegen.** The development engine's JIT now folds an expression whose operands
  are compile-time constants (`2 * 3` becomes `6`), applies a constant operand as
  a machine immediate (`n + 7` is one `add`, `i * 2` is one `shl`), and computes
  in the destination register instead of copying operands through scratch, so a
  tight integer loop is another ~2x faster than slice 1 (about 260x the
  interpreter, within ~1.7x of a hand-written `-O2` C++ loop). Everything stays
  1:1 with the bytecode — a constant propagation over the proto feeds the same
  emitter, and no opcode, superinstruction or runtime specialization is added.
  The differential suite (including the fuzz corpus) still proves the compiled
  body identical to the interpreter.

### Added

- **Bare-metal CPU-exception handler** (PHASE 7g). The entry point installs
  an interrupt descriptor table (256 gates, one stub per vector) before the
  program runs, so a CPU fault reaches a handler that prints `oskel: cpu
  exception 0xVV rip=0xRRRRRRRRRRRRRRRR` on COM1 and halts. Each stub pushes
  the vector and, for the ten vectors the CPU does not already tag with one, a
  dummy error code, so the frame the handler reads is uniform: vector, error
  code, faulting RIP. Without an IDT a `#UD` is a double fault and then a
  reset -- a silent hang, which is exactly how the missing `CR4.OSFXSR` hid.
  `tests/bare_metal_test.cpp` boots a `#UD` (vector 6, no error code) and a
  `#PF` (vector 14, with error code) under QEMU and checks both are reported
  with the right vector and RIP. No new opcode: the prelude drives the CPU with
  `lidt`, `rep movsb` and `push imm8`, primitives the emitter gained (each one
  instruction, so the 1:1 budget holds).
- **Two AOT link modes** (PHASE 7f.3). `oskel build --mode
  freestanding|hosted` (default `freestanding`) selects how the image is closed.
  `freestanding` is the self-contained binary every target already emits;
  `hosted` links against the host C library so an `extern "C"` symbol resolves
  to the real C function — the mode that makes FFI work. The mode is a
  deployment choice, never language syntax, and the (target, mode) matrix is
  validated (freestanding: all targets; hosted: Linux x86-64 only, for now) so
  an unsupported pair is reported instead of emitted. The freestanding binary is
  unchanged, byte for byte. The hosted image itself is PHASE 7f.4; until it
  lands, `--mode hosted` fails cleanly.
- **Bare-metal AOT target** (PHASE 7g). `oskel build --target
  x86_64-freestanding-sysv` emits a multiboot 1 kernel that a boot loader
  (QEMU `-kernel`, GRUB, Limine) loads at 1 MiB and enters directly, with no
  operating system underneath. The image is an ELF32/i386 whose 32-bit
  trampoline builds an identity page table, turns on long mode and installs its
  own 64-bit GDT before jumping into the shared 64-bit runtime, so the program
  itself is the same machine code the Linux target emits. The runtime talks to
  a 16550 serial port at COM1 (`oskel_write`), ends the program with a `cli`/
  `hlt` loop (`oskel_exit`) and takes its bump-allocator arena from a `.bss`
  region the loader zeroes (`oskel_init_arena`). Verified end to end under
  `qemu-system-x86_64 -kernel` in `tests/bare_metal_test.cpp`.
- AOT: the public builtins `range`, `assertEq` and `exit` compile into the
  freestanding binary (PHASE 7c.5b.5), so a program that uses them builds with
  `oskel build` instead of failing on an unassigned global. `range` builds the
  same `Range` value as the hidden `$range` helper (one bound means `0..end`);
  `assertEq` compares with the shared structural `==` and, on a mismatch,
  renders the hosted `"<msg>: expected A, got B"` text through the embedded
  `repr` and string builder before exiting 70; `exit` leaves through the entry
  prelude's `oskel_exit`, so it stops the program with the given code. All three
  match the development VM byte for byte and are covered by
  `tests/native_build_builtins.oskl` and the `freestanding_test` suite.
- A `switch` case label may be a `const` name (a top-level `const`, or a local
  `const`/`let` in the same frame), not only a literal or a range, so a shared
  constant reads as a label instead of a repeated magic number. The name is
  checked to be an immutable binding; a `var`, an unknown name or a captured
  binding is a compile error, so a label stays a fixed value. No new opcode:
  the label lowers to the same `EQ` (or `IN` for a range) against the subject.
- AOT: `oskel build` no longer fails on an unused host declaration (PHASE 7f.1).
  A `native fn`/`extern "C"` declaration binds a name to a host function; the
  lowerer now counts reads against declarations, so a program that imports a
  module declaring host functions (`std.math` declares ten) builds even when it
  calls none of them — the original gap, where `import std.math` broke the build
  for a program that used only the pure Oskel `abs`/`min`/`max`/`clamp`. A *used*
  declaration without a stub fails with a dedicated message
  (`native fn 'X' cannot be resolved by the AOT backend (PHASE 7f)`) instead of
  the generic unassigned-global error a typo produces. `sqrt` is seeded from the
  exact `sqrtsd` stub, so `native fn sqrt` resolves without a host; the libm
  primitives (`exp`, `log`, `pow`, `sin`, `cos`, `tan`) resolve from the embedded
  musl ports (PHASE 7c.5b.4).
- A qualified call to a module's native export is now checked at compile time
  and rejected by `oskel build` (PHASE 7f.2). `std.math.pow(...)` reads the
  module table with `GETTABLE`, not with `GETGLOBAL`, so the AOT lowerer never
  saw the use: a program that called a host binding through its namespace
  compiled to a binary that faulted at run time (`cannot call a non-function
  value`) instead of failing the build. The compiler propagates each public
  native export's signature on the imported module, records the qualified access
  when it emits the member read, and resolves it once the chunk is compiled
  (deferred, because a hoisted function is compiled before the `import` that
  loads the module). The build now rejects the use with the same message as the
  direct form, and a qualified call is checked against the export's signature
  (arity and argument types) exactly like `pow(2.0)`, so `std.math.pow(2.0)`
  fails with `'std.math.pow' expects 2 arguments, got 1`.
- AOT: the transcendental builtins `exp`, `log` and `pow` (PHASE 7c.5b.4a) and
  the trigonometric builtins `sin`, `cos` and `tan` (PHASE 7c.5b.4b) are now
  available in a freestanding binary. The development engine resolves them
  through the host's libm, which a freestanding AOT binary cannot link, so the
  musl routines (MIT, Arm Limited) are emitted directly as x86-64 through
  `CodeGen` (the approach already used for `dtoa` and `parse_float`) and the
  runtime stays libc-free. `exp`/`log`/`pow` port the musl kernels and the
  `pow` special cases; the trigonometric family ports `__rem_pio2` (with its
  large-argument helper), the `__sin`/`__cos`/`__tan` kernels and the public
  entry points. The wrappers do the arity/type checks and the `log`
  non-positive check, and are seeded into their global slots like the other
  builtins, so neither the compiler nor the bytecode changes. glibc is not
  correctly rounded for these functions, so the contract is "agrees to 1 ulp".
- `struct` literals support field shorthand: `Point { x, y }` is sugar for
  `Point { x: x, y: y }`, taking each value from the variable of the same name
  in scope. The shorthand and the `name: value` form mix freely in one literal
  (`Point { x, y: 5 }`). It is a parser-level desugaring — no new token, node,
  opcode or runtime change — so the lowered bytecode is identical to the
  explicit form and the VM and the AOT backend are unaffected.
- **Windows AOT target** (PHASE 7e). `oskel build --target
  x86_64-windows-win64` emits a static Windows PE32+ executable; the default
  target follows the host, and a Windows output name gets the `.exe` suffix.
  The lowering and the embedded runtime are shared with the Linux target
  unchanged — only the entry prelude differs, reaching kernel32
  (`GetStdHandle`/`WriteFile`, `ExitProcess`, `VirtualAlloc`) through the
  Microsoft x64 ABI. The PE emitter now writes a real import table with a
  `.idata` section pinned at a fixed RVA and declares
  `IMAGE_FILE_RELOCS_STRIPPED`, so the non-PIE image is mapped by a Windows
  with "Force ASLR" instead of aborting.
- Variadic functions (`rest...`) now compile to a static binary (PHASE 7c.6).
  The AOT prologue accepts `numParams - 1` or more arguments and builds the
  packed array with the new runtime helper `oskel_pack_rest`, so a program
  using variadic parameters behaves the same on the VM and in an AOT binary.
- FFI: a struct larger than 16 bytes (System V class MEMORY) now crosses the C
  boundary by value. Such a struct travels on the stack and comes back through
  a hidden `sret` pointer, a shape the register thunks cannot express, so the
  call goes through `libffi` where the development engine links it on the
  System V AMD64 ABI; elsewhere the declaration is rejected at compile time as
  before.
- FFI: a callback (`fn(P...): R`) may now return a struct by value. The
  trampoline serialises the map the Oskel function returns into the C return
  storage using the struct layout, so C reads the fields back through the
  registers the ABI assigns.
- AOT: `oskel build` reports an `extern "C"` symbol with a dedicated message
  (`extern "C" symbol 'X' cannot be resolved by the AOT backend (PHASE 7f)`)
  instead of the generic unassigned-global error a typo produces. The symbol
  still cannot be lowered (PHASE 7f work), but the failure now names the real
  problem. Pinned by `tests/build_error.cmake`.
- FFI: `extern "C"` declarations support a C variadic. A trailing bare `...`
  (`fn printf(fmt: cstring, ...): int`) declares one; the named parameters are
  the fixed prefix and the extra arguments pass through the default argument
  promotions. Like a class-MEMORY struct, the call goes through `libffi` and the
  declaration needs the System V AMD64 ABI, so off that it is rejected at
  compile time instead of mis-called.

### Changed

- The development-engine JIT keeps each scalar slot in a physical CPU register
  instead of its 16-byte stack slot. The bytecode contract is untouched (no new
  opcode, no superinstruction, no runtime specialization): the JIT still lowers
  each opcode 1:1, but a slot the dataflow proves only ever holds
  `nil`/`bool`/`int` now lives in `rax`/`rcx`/`rdx`/`rsi`/`rdi`/`r8`/`r9` with
  the raw payload, and the tag is materialized only at the frame boundary. A
  slot that changes scalar kind (a `bool` comparison feeding an `int` slot and
  back) stays in its register, because the kind at each use is read from the
  dataflow state rather than fixed per slot. A tight integer loop is now about
  130x faster than the interpreter (about 5x faster than the previous
  stack-slot JIT), and the differential suite still proves the compiled body
  identical to the interpreter for every accepted proto.
- `ITER` now yields a map's paired value directly. The opcode took two new
  operands: `Instruction::d` is the register that receives the value (`-1` when
  the loop has no value variable) and `Instruction::e` is the done flag, which
  used to overload `Instruction::jump` (backlog §1.2). `jump` is now a jump
  target only. `for k, v in map` no longer calls the hidden `$mapValue` helper
  (retired from the hosted engine and the AOT runtime), so the value form is one
  `ITER` per turn instead of an `ITER` plus a call.
- The `oskel build` CTest cases now run the same fixture list on Windows as on
  Linux. Windows registered only two cases (`build_binary` and
  `build_binary_deep_recursion`) against Linux's ten, so the Windows PE path was
  exercised by two programs while the shared AOT backend was covered by ten —
  the gap the 1 MiB stack reserve slipped through. The runners
  (`tests/build_binary.cmake`, `tests/build_error.cmake`,
  `tests/aot_runtime_error.cmake`) are now portable: the Windows output keeps
  its `.exe` suffix and the static-link `ldd` check is Linux-only. The
  Windows-only `tests/build_binary_pe.cmake` is gone, and one list registers
  every case on both hosts.
- The AOT backend compares two arrays or maps by content, like the hosted
  engine, instead of by pointer identity. `[1, 2] == [1, 2]` is now true in a
  compiled binary, and `in` over an array of arrays matches the VM. The
  equality helper tracks the pairs already under comparison on the stack, the
  same `visited` rule as `Value::equals`, so a cyclic structure compares equal
  to an isomorphic one and terminates instead of recursing forever.
- `examples/tui_editor.oskl` now exercises every feature its header claims:
  the non-null assertion `!!`, safe access `?.` with `??`, a union return type
  (`configValue`), `byteLen`/`buffer`/`cstring` and a range, all checked by the
  self-test. The header's FFI list is corrected (`system`, not `stty`), it
  documents both ways to run the example, and the untestable parts
  (`render`/`readKey`/`enableRawMode`/`restoreTerminal`) are called out. The
  self-test cleans up its temporary file, and the editor reports a clear status
  for `Ctrl-S`/`:wq` with no file name and for a failed `:wq`.
- The README documents the VM/AOT split explicitly (a "What runs where" section):
  the AOT backend covers the language core, so collection methods (`arr.push`,
  `map.keys`, `str.upper`), `pcall`, the remaining standard-library builtins and
  catchable `try`/`catch` are VM-only in 0.1.0, while structural equality and
  `map.get` behave differently in an AOT binary. The FFI callback dependency on
  `libffi` and the JIT's narrow scope are now stated in the README too.
- `DEVELOPMENT.md` marks PHASE 4 completed and notes that `env.cpp` is unused
  dead code (kept only by `tests/gc_test.cpp`).

### Fixed

- The `oskel build` end-to-end tests (`tests/build_binary.cmake`,
  `tests/build_error.cmake`, `tests/aot_runtime_error.cmake`) wrote their
  generated binary to a fixed name under `CMAKE_CURRENT_BINARY_DIR`. That
  variable is the same in every `cmake -P` run, so under `ctest -j` the
  registered tests (each a separate process) clobbered one another's output:
  one test removed the file another had just built, or ran a binary produced
  from a different fixture, and the suite failed intermittently with "oskel
  build did not create ..." or "the generated binary disagrees with the VM".
  Each test now passes its unique CTest name (`-DOSKEL_TEST_NAME`) and writes
  to `${OSKEL_TEST_NAME}${exe_suffix}`, so the output paths never collide. A
  parallel run is now deterministic.
- AOT bare metal: the trampoline now sets `CR4.OSFXSR` and `CR4.OSXMMEXCPT`
  (bits 9 and 10) in addition to PAE, so the SSE instructions the float path
  emits execute instead of raising `#UD`. With no IDT installed that fault was
  a triple fault, so any program doing float *arithmetic* (`1.0 / 3.0`, a
  `float` comparison) hung the machine silently, while printing a float
  *literal* worked because it only exercises the integer `dtoa`. The 64-bit
  entry point also brings COM1 up (mask interrupts, divisor 1 for 115200 baud,
  8N1, enable the FIFO) before the program runs, so a real 16550 transmits
  rather than only QEMU's emulated one. `tests/bare_metal_test.cpp` pins both:
  a structural check for the CR4 sequence and an end-to-end float-arithmetic
  boot under QEMU.
- AOT: `+` on two arrays now concatenates in a compiled binary. The hosted
  engine builds a fresh array with the left items followed by the right items
  and leaves both operands unchanged (`docs/language.md`: "concatenation of
  strings or arrays"; `tests/06_operators.oskl` asserts `[1] + [2] == [1, 2]`),
  but the AOT `oskel_arith` rejected the pair as "unsupported operand types", so
  a program that ran on the VM exited 70 as an AOT binary — a mis-compiled
  image, which the backend must never emit. `oskel_arith` now dispatches `+` on
  two arrays to the new `oskel_array_concat` helper (one `ADD` path, no new
  opcode), and a `tests/build_binary` fixture pins the binary against the VM on
  Linux and Windows.
- A compiled binary printed `0 1 2` for `for c in "abc"` instead of `a b c`.
  The `str` branch of `oskel_iter_next` built the character but fell through
  into the `range` block, which overwrote `rax:rdx` with the cursor index; a
  missing `jmp` after the string was built. The VM was correct, so this was an
  AOT-only divergence, now pinned by `tests/freestanding_test.cpp`.
- A `struct` name and an import binding the same name no longer coexist
  silently. `struct Point { ... }` plus `import std.math as Point;` used to
  leave both alive: `Point { ... }` stayed a struct literal while
  `Point.sqrt(9.0)` resolved to the alias, so the winner was undefined. The
  collision is now a compile error in both source orders and for every import
  form (alias, wildcard, selective), reported by `recordImportBinding` within a
  chunk and by `compileStructDecl` across REPL entries. A struct is
  compile-time metadata, not a value binding, so it is not covered by the
  "a local `let`/`var`/`fn` wins over an import" rule.
- A Windows AOT binary was killed with no message when a program recursed a few
  thousand frames with a non-trivial frame. The PE emitter reserved only 1 MiB
  of stack (the Windows default) while the AOT runtime has no call-depth guard,
  so the recursion a native binary reaches is bounded only by the OS stack;
  Linux hands a process 8 MiB (`ulimit -s`), so the same program ran there and
  was torn down silently on Windows (exit code 1, no output). The emitter now
  reserves 8 MiB, matching Linux, and `pe_test` pins the value. Covered by
  `tests/native_build_deep_recursion.oskl` (`oskel.build_binary_deep_recursion`
  on Linux and Windows).
- `chr(code)` now rejects a lone surrogate (U+D800..U+DFFF) instead of
  encoding it as CESU-8 (three bytes, invalid UTF-8), matching the `\uXXXX`
  escape. The check lives in the builtin, so the VM and the AOT backend reject
  the same value with the same message (`chr() code point is a surrogate`) and
  the same exit code. The scalar values on either side of the range
  (U+D7FF, U+E000) still encode.
- A `\uXXXX` string escape whose value is a lone surrogate (U+D800..U+DFFF) is
  now a compile error instead of producing a string with invalid UTF-8. The
  lexer previously encoded the code point as CESU-8 (three bytes), so
  `"\uD800"` yielded bytes the rest of the language (`len`, indexing, `==`)
  treated as one character but that are not valid UTF-8. The check lives in the
  lexer, so the VM and the AOT backend reject the same source identically.
- `extern "C"` declarations on Windows could not name the Win32 API. The
  development engine's `lookupSymbol` searched only the process image and the
  C runtime, so a declaration such as `GetTickCount` or `Sleep` — which live in
  `kernel32.dll` and have no CRT equivalent — failed at run time with
  `extern "C" symbol '...' was not found in the host`. The search now also
  covers `kernel32.dll`, matching the set of modules `dlsym(RTLD_DEFAULT)`
  reaches on POSIX; the CRT names still resolve.
- AOT: `double(f)` on a `float` operand returned `0` instead of the widened
  value. A boxed `Float` already holds its value widened to binary64, but the
  `double` builtin widened the payload a second time, reading the binary32 bits
  as binary64. The `sqrt` stub had the same shape, so both are fixed together.
  `tests/native_build_conversions.oskl` pins the conversions against the VM
  (the script tests that cover them only run on the hosted engine).
- The PE emitter padded the image only to the end of `.text`'s bytes, not to
  the `SizeOfRawData` the section header declares, so the file could end up to
  `FileAlignment` bytes short of the image it described. Windows rejected such
  an `.exe` with `ERROR_BAD_EXE_FORMAT` (Wine tolerated it, which hid the bug);
  the same short image made `pe_test` hang on Windows when it launched the
  rejected binary and the loader's error dialog blocked. The emitter now pads
  to the declared size and publishes the IAT data directory
  (`DataDirectory[12]`), which was left zero. `pe_test` also measured its
  `call [ExitProcess]` displacement from two bytes past the call site, so the
  stub faulted once the image loaded; the offset is now exact. `pe_test`
  asserts the image size and the IAT directory on every platform.
- The PE import directory table held one descriptor per imported function
  (four identical kernel32.dll descriptors) instead of one per DLL, which the
  PE/COFF specification requires ("one entry for each DLL"). Windows tolerated
  the duplicates, so it never surfaced at run time, but the table was
  non-conformant and `objdump` interpreted it as four kernel32.dll imports.
  The emitter now writes a single descriptor for kernel32.dll followed by the
  null terminator, and `pe_test` pins the descriptor count.
- FFI: two `extern "C"` declarations of the same C symbol with different
  signatures are now a compile error instead of a silent last-wins. The check
  spans the whole program, so a conflict between two imported modules is caught
  as well.
- Removed leftover debug `printf` output from the compiler, VM and PE test
  suites.

## [0.1.0] - 2026-11-14

The first public release. Oskel is a dynamic, general-purpose language
implemented from scratch in C++17: lexer, parser, AST, bytecode compiler,
register-based VM, AOT backend, JIT and standard library, with no external
dependencies beyond the C++ standard library.

### Added

- **Language core**: `nil`, `bool`, `int` (64-bit), `float` (binary32),
  `double` (binary64), `string`, `error`, `array`, `map`, `function` and
  `range` values; `var`, `let` and compile-time `const` bindings with block
  scope; first-class functions, recursion, `=>` lambdas and variadic
  `rest...` parameters; real closures over shared cells.
- **Operators**: arithmetic, logical, bitwise, comparison, ternary and
  compound assignment, including the short-circuiting `&&=`, `||=` and `??=`.
- **Control flow**: `if`/`else`, `while`, classic `for`, `for ... in`
  (arrays, maps, strings and ranges, with the `for k, v in map` key/value
  form), `switch` on literals and integer ranges, `break`/`continue`, the
  post-test `do { ... } while (cond);` loop, `defer { ... }` and brace-less
  single-statement bodies.
- **Types and nullability**: optional annotations (`let x: int = 5`), local
  inference, nullable `T?`, unions (`T | E`), `@MustUse`, and the null-safe
  expressions `?.`, `??` and `!!`.
- **Strings and collections**: `"${expression}"` interpolation with UTF-8
  support, ordered arrays and maps with insertion-order iteration, ranges
  (`1..5`, `0..=10`, `5..0`) and slicing (`s[1:3]`, `a[::2]`, `a[::-1]`).
- **Error handling as values**: `error(message, code?)`, `isError`, `pcall`,
  `try expr`, `expr catch default` and the dedicated `error` value type.
- **Modules**: Java-style `import` (qualified, wildcard `.*`, alias `as` and
  selective `{ ... } from`) with compile-time conflict detection and a
  reserved `std.` prefix.
- **C interop**: `extern "C" { ... }` blocks and `native fn` declarations,
  marshalling scalars, strings, pointers and structs by value without
  `libffi`, plus callbacks from C into Oskel through `libffi` in the
  development engine.
- **Standard library**: `std.math` and the builtins for strings, arrays,
  maps, math, files, console and utilities.
- **Development engine**: a register-based bytecode VM, an interactive REPL
  with session state (`:help`, `:load`, `:vars`, `:clear`), and a JIT that
  compiles integer leaf functions to native x86-64.
- **AOT backend**: `oskel build file.oskl` compiles the bytecode to a
  self-contained, statically linked executable with no libc and no runtime to
  install (`ldd` reports "not a dynamic executable").
- **Tooling**: `oskel -c` for a full compile check, diagnostics with file,
  line, column, code snippet and stack trace, and a VS Code
  syntax-highlighting extension.
- **Packaging**: `.deb`/`.rpm` (Linux), an Inno Setup installer (Windows),
  `.pkg`/`.dmg` (macOS) and the VS Code extension (`.vsix`), published as
  GitHub Release assets with a `SHA256SUMS` file on a `v*` tag.

### Notes

- The development engine ships a JIT; the AOT binary never contains it, and
  the JIT never changes the result of a program.
- The AOT backend targets `x86_64-linux-sysv`; on any other target `oskel
  build` reports the unsupported target instead of emitting bytes.

[Unreleased]: https://github.com/yasmramos/Oskel/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/yasmramos/Oskel/releases/tag/v0.1.0
