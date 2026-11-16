# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is `0`, the language surface and the bytecode contract
may change between minor versions.

## [Unreleased]

### Added

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
  exact `sqrtsd` stub, so `native fn sqrt` resolves without a host. The
  transcendentals (`pow`, `sin`, `cos`, `tan`, `log`, `exp`) stay rejected: a
  hand-written implementation would not reproduce libm's last bit.
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
