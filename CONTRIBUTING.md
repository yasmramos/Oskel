# Contributing to Oskel

Oskel is a compiled programming language with a register-based VM (the
development engine) and an AOT backend (the shipped product). Two binaries
share one front end, and the whole project is plain C++17 with no dependency
beyond the standard library.

This guide covers the mechanical part of a contribution: build, test, and the
rules that keep the two engines and the three platforms in step. The language
specification, the architecture and the development phases live in
[`DEVELOPMENT.md`](DEVELOPMENT.md); the git workflow and commit format live in
[`CONVENTIONS.md`](CONVENTIONS.md). Read both before a non-trivial change.

## Language and scope

- Every repository artifact is written in **English**: code, comments, docs,
  test names, commit messages, branch names, PR titles and review comments.
- The design is deliberately small: dynamic types, no inheritance, no
  exceptions, no generics. Do not add them in a drive-by change.
- Every new feature lands in the **VM and the AOT backend in the same change**.
  A feature that works only in the VM is unfinished; a feature that works only
  under `oskel build` is a bug. When the backend genuinely cannot lower a
  construct yet, it must fail cleanly with a dedicated message and exit 65 —
  never emit a mis-compiled binary and never silently diverge.
- Platform parity is part of "done": Linux, Windows and macOS, on the VM, the
  AOT backend and WebAssembly where they apply, plus the VS Code grammar and
  snippets and every packaging format.

## Build

### The script (fastest)

```sh
sh build.sh
./build/oskel examples/hello.oskl
```

`build.sh` builds all six binaries into `build/`:

| Binary | Purpose |
| --- | --- |
| `oskel` | development engine: VM, REPL, JIT and `oskel build` |
| `oskelc` | host compiler: front end + AOT backend, no interpreter |
| `oskelld` | the project's own linker (PHASE 10) |
| `bindgen` | C binding generator (standalone) |
| `oskelp` | project and package manager (standalone) |
| `oskelint` | linter (standalone; links the runtime for the builtin names) |

Choose the compiler and flags with environment variables:

```sh
CXX=clang++ CXXFLAGS="-O0 -g" sh build.sh
```

On Windows use `build.bat`.

### CMake

Ninja is the recommended generator; `CMakePresets.json` pins it:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --preset ninja
```

Without presets, pass `-G Ninja` yourself:

```sh
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j4
```

### Directory classification

There is **no hand-maintained source list**. The directory a file lives in is
its classification, and both `build.sh` and `CMakeLists.txt` glob the same
three directories:

| Directory | Contents | Linked into |
| --- | --- | --- |
| `src/core/` | `Value`, the cycle collector, the platform layer, `util.h`, `errors.h` | `oskel` and `oskelc` |
| `src/frontend/` | lexer, parser, bytecode compiler, module resolver, diagnostics, shared CLI and the AOT backend (`src/frontend/native/`) | `oskel` and `oskelc` |
| `src/runtime/` | VM, operators, builtins, FFI marshalling, Unicode case tables and the JIT (`src/runtime/jit/`) | `oskel` only |

A `.cpp` dropped straight into `src/` is refused at configure time, and the
boundary checks fail if a front-end file includes a runtime header. A new file
belongs in one of the three directories; it needs no edit to the build files.

## Tests

The suite is CTest. The glob is evaluated at **configure time**, so a new test
or example needs a reconfigure before CTest sees it (a build alone will not):

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j4
ctest --test-dir build-cmake --output-on-failure
```

The same scripts are available without CMake:

```sh
./tests/run.sh            # all .oskl scripts, errors, check tests and examples
./tests/run_compiler.sh   # compiler_test
./tests/run_vm.sh         # vm_test
./tests/run_gc.sh         # gc_test
```

On Windows use the matching `tests\run*.ps1` files. The bare-metal boot tests
need `qemu-system-x86_64`; set `OSKEL_REQUIRE_QEMU=1` to make a missing binary a
failure instead of a skip, exactly as CI does.

## Adding a builtin

A builtin has to exist in **both engines**. The parity check
(`tests/check_builtin_parity.cmake`) reads the two registries from the source
and fails when one side has a name the other does not.

1. **VM side** — register the native in `src/runtime/builtins.cpp` with
   `in.addNative("name", [](Runtime& in, Args args) -> Value { ... });`.
   Check the argument count with `need(in, "name", args, N)` **before** reading
   `args[0]`, and word every error message so the AOT backend can reproduce it
   (the VM's text is the contract; see the message rules in `DEVELOPMENT.md`).
2. **AOT side** — seed the same name in the `builtins` map in
   `src/frontend/native/lower.cpp` and implement the machine-code body in
   `src/frontend/native/runtime.cpp`. If a target cannot back the builtin,
   reject it by name with a dedicated message and exit 65; never fall back to
   "global 'X' is read but never assigned", which is what a typo produces.
3. **Tests** — add the happy path to the closest `tests/native_build_*.oskl`
   fixture so the VM and the compiled binary are compared byte for byte. A
   runtime error that quotes a value is asserted through `pcall` in
   `tests/native_build_runtime_error_messages.oskl`.
4. **Docs and editor support** — if the name is public, add it to the reference
   docs. The VS Code grammar derives its vocabulary from the compiler, so a new
   keyword or type also means updating `editors/vscode/syntaxes/oskel.tmLanguage.json`.

## Adding a `.oskl` test

- A positive test lives in `tests/` and exits 0. Use `assert` and `assertEq`.
- An error test lives in `tests/errors/` and declares its expected exit code on
  the first line: `// expect-exit: 70`.
- A compile-only test lives in `tests/check/` and runs under `-c`; it must exit
  with the declared code and write nothing to stdout.
- A capability the test needs is declared on the first lines:
  `// requires: x86-64` or `// requires: x86-64-system-v`. The runners skip it
  when the engine lacks the capability, mirroring CMake.
- Name a numbered behavioural test `tests/NN_topic.oskl` (the number orders the
  suite, it is not a contract). The suite globs `tests/*.oskl` at configure
  time, so re-run CMake after adding one.

### The VM/AOT parity fixture

A `tests/native_build_<topic>.oskl` fixture is built with `oskel build` and run
next to the VM; the harness (`tests/build_binary.cmake`) asserts that the native
binary prints **exactly** the VM's output. Register it in `CMakeLists.txt` with
the same script on both runs, and pass the same `-DOSKEL_ARGS` vector to both so
the fixture can assert `args()` content rather than only its shape.

The fixture must stay on the portable primitive surface the backend supports
(indexing, slicing, `len`, `ord`/`chr`, conversions, `panic`, `for ... in`,
closures, variadic `rest...` and the operators). When a construct is not
lowered yet, the fixture belongs in `tests/errors/` as a clean failure, not in
the parity set.

## The 40-opcode budget

The bytecode is the contract between the two engines. It is register-based and
capped at **40 opcodes**, enforced by

```cpp
static_assert(kOpcodeCount <= 40, "...");
```

in `include/bytecode.h`; the VM executes that table in `src/runtime/vm.cpp`.

Before adding an opcode, check the 7-point checklist in `DEVELOPMENT.md` and ask
whether the construct maps to **1:1 or 1:2** machine instructions. If it needs
three or more, redesign it in terms of the existing opcodes (a helper call, a
compound sequence, parser sugar) instead of growing the table. Superinstructions
and inline caching are explicitly prohibited because they change the bytecode
contract.

## Commits and pull requests

- Branch from `develop`: `feat/phase-X.Y.Z-<slug>`, `fix/<slug>` or
  `docs/<slug>`. Only a release targets `main`.
- Conventional Commits in English, imperative summary, one logical change per
  commit. No AI attribution and no `Co-Authored-By:` bot trailers.
- Before opening a PR: `sh build.sh` and the full CTest suite must be green.
- The PR base is `develop`. CI builds and tests Linux, Windows and macOS, so a
  local green run is necessary but not sufficient; read every failing job.
- A PR is not finished until it is mergeable (`mergeable_state` is `clean`),
  the base is `develop`, and every CI job is green.
