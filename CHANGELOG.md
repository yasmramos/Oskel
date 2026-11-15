# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is `0`, the language surface and the bytecode contract
may change between minor versions.

## [Unreleased]

### Added

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

### Changed

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
