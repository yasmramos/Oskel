# Oskel Programming Language 

**Oskel** is a dynamic, general-purpose programming language implemented from scratch in **C++17**. The project includes the lexer, the parser, the AST, the bytecode compiler, the register-based virtual machine and the standard library, with no external dependencies beyond the C++ standard library.

Its goal is to be small, readable and easy to study: a single executable, a Lua-style bytecode architecture and a familiar syntax with braces and semicolons.

```oskel
fn greet(name) {
  return "Hello, ${name}!";
}

let languages = ["C++", "Oskel"];
for language in languages {
  print(greet(language));
}
```

## Features

- Dynamic types: `nil`, `bool`, `int` (64-bit), `float` (binary32), `double` (binary64), `string`, `error`, `array`, `map`, `function` and `range`.
- Variables with `var` (mutable), `let` (immutable, runtime) and `const` (immutable, folded at compile time), block scope and real closures.
- First-class functions, recursion, `=>` lambdas (`x => x * 2`) and variadic `rest...` parameters.
- Optional types and nullability: annotations (`let x: int = 5`), local inference, nullable `T?`, unions (`T | E`) and the null-safe expressions `?.`, `??` and `!!`.
- `struct` declarations (data only, no methods) checked against their literal at compile time.
- `class` declarations (fields plus methods): instantiating is calling the class name (`Point(3, 4)`, no `new`), `self` is explicit and there is no inheritance. A class is parser sugar over a map plus closures, so it runs identically on the VM and under `oskel build`. Field and method annotations are checked at compile time.
- String interpolation with `"${expression}"` and UTF-8 support.
- Ordered arrays and maps that preserve insertion order, with `for k, v in map` key/value iteration.
- Ascending and descending ranges: `1..5`, `0..=10`, `5..0`.
- Arithmetic, logical, bitwise, comparison, ternary and compound assignment operators.
- Control flow: `if`/`else`, `while`, classic `for`, `for ... in`, `switch`, `break`/`continue` and `defer`.
- Error handling as values: `error(message, code?)`, `isError`, `pcall`, `try`/`catch` and `@MustUse`.
- Modules with Java-style `import`, and C interop via `extern "C"` / `native fn`.
- Built-in standard library: strings, arrays, maps, math, files, console and utilities.
- Interactive REPL with session state, commands and expression evaluation.
- **Ahead-of-time native compilation**: `oskel build file.oskl` emits a
  self-contained static executable (no libc, no runtime to install; `ldd`
  reports *"not a dynamic executable"*). The AOT backend covers the language
  core, not the whole runtime; see
  [What runs where](#what-runs-where-vm-vs-aot) for the exact split and the
  [CLI tutorial](docs/tutorial-cli.md).
- **Bare-metal target (experimental)**: `oskel build --target
  x86_64-freestanding-sysv` emits a multiboot kernel that boots with no
  operating system underneath, printing to a serial port. It runs under
  `qemu-system-x86_64 -kernel`, GRUB or Limine; see
  [Bare metal](#bare-metal-experimental).
- **WebAssembly target (experimental)**: `oskel build --target
  wasm32-freestanding-wasm` emits a `.wasm` module that runs in a browser, an
  edge runtime or Node. The module imports only `oskel.write` and
  `oskel.exit`; see [WebAssembly](#webassembly-experimental).
- Diagnostics with file, line, column, code snippet and stack trace.

## Status

Version **0.3.0**. The language is functional and covered by a test suite, but it is not yet considered stable or complete. See the [limitations](#limitations-and-roadmap) section and the [changelog](CHANGELOG.md) for what each release contains.

## Requirements

- A C++17 compiler: `g++`, `clang++` or MSVC.
- `cmake` 3.15 or newer (optional, only if you prefer building with CMake).
- On Windows it has been tested with **llvm-mingw** (clang 21 with the `g++` interface).

## Building

### Windows (script)

```bat
build.bat
build\oskel.exe examples\hello.oskl
```

### Linux, macOS or MSYS2 (script)

```sh
chmod +x build.sh tests/run.sh
./build.sh
./build/oskel examples/hello.oskl
```

You can choose the compiler and its options:

```sh
CXX=clang++ CXXFLAGS="-O0 -g" ./build.sh
```

### CMake

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
./build-cmake/oskel examples/hello.oskl
```

### Manual build

The sources split in three, and the directory a file lives in is its
classification: **core** (`Value`, the cycle collector, the platform layer),
**front end** (lexer, parser, bytecode compiler, AOT backend) and **runtime**
(VM, JIT, builtins, FFI marshalling). `oskel` needs all three; `oskelc` needs
core plus the front end.

```sh
# the development engine (VM + REPL + AOT)
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build/oskel \
    src/main.cpp src/core/*.cpp src/frontend/*.cpp src/frontend/native/*.cpp \
    src/runtime/*.cpp src/runtime/jit/*.cpp

# the host compiler (AOT only, no interpreter)
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build/oskelc \
    src/oskelc.cpp src/core/*.cpp src/frontend/*.cpp src/frontend/native/*.cpp

# the project's own linker (front end only, no interpreter)
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build/oskelld \
    src/oskelld.cpp src/core/*.cpp src/frontend/*.cpp src/frontend/native/*.cpp

# the project and package manager (standalone)
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build/oskelp \
    tools/oskelp.cpp tools/oskelp_main.cpp
```

`./build.sh` and `build.bat` run these commands for you, and `CMakeLists.txt`
builds the same five targets (`oskel`, `oskelc`, `oskelld`, `bindgen`, `oskelp`).

### oskelc (the host compiler)

`oskelc` is the same compiler as `oskel build`, with a shorter command line. It
links the front end only, so the shipped compiler carries no VM, no JIT and no
builtins:

```sh
./build/oskelc examples/hello.oskl -o hello
./hello
```

Both entry points call the same pipeline (`src/frontend/cli.cpp`), so the flags, the
defaults, the diagnostics and the exit codes are identical and the emitted image
is byte for byte the same. `-c` (a `.oskc` bytecode image) and `-pkg` (a
`.oskpkg`) are not implemented yet and are refused by name with exit 64.

### oskelld (the project's own linker)

`oskelld` closes the hosted relocatable object(s) without a C toolchain. It is
the standalone entry point over the same core `oskel build --linker own` and
`oskelc --linker own` call:

```sh
./build/oskel build examples/hello.oskl --mode hosted --emit-object -o hello.o
./build/oskelld hello.o -o hello
./hello

# several objects and a System V/GNU ar archive
./build/oskelld hello.o libhelpers.a -o hello

# combine objects into a new relocatable object
./build/oskelld -r hello.o other.o -o combined.o
```

The image is a dynamically linked x86-64 Linux ELF (`libc.so.6`, plus
`libm.so.6` when a symbol needs it). It loads `.text`, `.rodata`, `.data` and
`.bss` from GCC/Clang objects (including the `.text.*`/`.rodata.*`/`.data.*`/
`.bss.*` variants), resolves section symbols and the common x86-64 relocations,
and ignores `.eh_frame`, TLS and the C++ runtime sections. An archive member is
pulled in only when it defines a still-undefined symbol. `-L` and `--link-lib`
select a third-party library (`--link-lib SDL3`); the binary records the
library's SONAME. On Windows the same `--linker own` path parses COFF objects
and emits a non-PIE PE32+ image, with the runtime's UCRT calls imported from
`ucrtbase.dll` and the Windows API from `kernel32.dll`; a generated `_start`
calls `main` and `ExitProcess`.
`oskel build --linker own --link-object helper.o` is the same capability
through the build command. The linker is opt-in (PHASE 10.1/10.2) and only
targets `x86_64-linux-sysv`; `--linker host` keeps the `$CC` path. See
[docs/language.md](docs/language.md) for the flags and `DEVELOPMENT.md`
PHASE 10 for the design.

### bindgen (the C binding generator)

Every build above also produces `bindgen` (`bindgen.exe` on Windows) next to the
engine. It reads a C header and writes an Oskel `extern "C"` module, so a C
library can be called from Oskel without writing the signatures by hand:

```sh
./build/bindgen mylib.h -o mylib.oskl
```

With CMake it is built as `build-cmake/bindgen`, and with the manual build it is
a second command:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build/bindgen \
    tools/bindgen.cpp tools/cxxbind.cpp tools/bindgen_main.cpp
```

See [docs/bindgen.md](docs/bindgen.md) for the C → Oskel type mapping and what is
skipped.

### oskelp (the project and package manager)

`oskelp` is the Cargo-style project layer: a manifest (`oskel.toml`), a
lockfile, dependency resolution (path and local-registry) and the
`new`/`init`/`add`/`remove`/`publish`/`update`/`build`/`run`/`check`/`test`
workflow. It is a standalone tool that drives `oskel` and `oskelc` as child
processes, so the engine carries no project or dependency policy:

```sh
./build/oskelp new hello
cd hello
../build/oskelp add utils --path ../utils  # ../utils is another package
../build/oskelp add text --version "^1.0.0"  # from $OSKEL_REGISTRY
../build/oskelp run          # the VM, for the fast development loop
../build/oskelp build        # target/hello, the native binary
../build/oskelp test         # every tests/**/*.oskl
../build/oskelp update       # move registry pins to newer versions
```

`oskelp publish --registry <dir>` copies the current package into a local
registry, so another project can depend on its version. See
[docs/oskelp.md](docs/oskelp.md) for the project layout, the manifest, the
dependency model, the registry layout and every command.

## Usage

```text
oskel                      starts the REPL
oskel <file.oskl>          runs a script
oskel -e "<code>"          runs code from the command line
oskel -c <file.oskl>       compiles the script without running it
oskel build <file.oskl>    compiles the script to a native binary (AOT)
oskelc <file.oskl>         the same compiler without the interpreter
oskelp new <dir>           creates a project (oskel.toml, src/<name>/...)
oskelp add <name> --path <dir>  adds a path dependency to oskel.toml
oskelp add <name> --version <req>  adds a registry dependency (OSKEL_REGISTRY)
oskelp remove <name>       removes a dependency from oskel.toml
oskelp publish             publishes the package into a local registry
oskelp update [<name>]     moves registry pins to newer versions
oskelp build               builds the project to target/<name>
oskelp run                 runs the project (--native for the AOT binary)
oskelp test                runs every tests/**/*.oskl
```

Options:

| Option | Description |
| --- | --- |
| `-h`, `--help` | Shows the help. |
| `-v`, `--version` | Shows the version. |
| `-e`, `--eval <code>` | Runs the given code. |
| `-c`, `--check` | Compiles without running; reports syntax, type and import errors. |
| `-o`, `--output <file>` | Output path for `build`/`oskelc` (default: the script's base name). |
| `--target <name>` | Build target for `build`/`oskelc` (default: the host). |
| `--mode <name>` | Link mode for `build`/`oskelc` (default: `freestanding`). |
| `--no-jit` | Disables the JIT (development engine only). |
| `--` | Marks the end of the options; the next argument is the file. |

Arguments that follow the file (or the `-e` code) are passed to the script and can be read with `args()`:

```sh
oskel greet.oskl Ada Grace
```

```oskel
for name in args() {
  print("Hello, ${name}");
}
```

### Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Successful execution. |
| `64` | Command-line usage error. |
| `65` | Compile error (lexer or parser). |
| `66` | The file could not be read. |
| `70` | Runtime error. |
| other | Value given to `exit(code)`. |

### REPL

Running `oskel` with no arguments opens the REPL:

```text
Oskel 0.3.0 - a small scripting language written in C++
Type :help for help, :quit to exit.
oskel> let x = 21
oskel> x * 2
42
oskel> let double = n => n * 2
oskel> double(x)
42
```

- Expressions print their result in `repr` format.
- `_` holds the last evaluated value.
- The `...>` prompt means the input continues on the next line.
- A blank line cancels an incomplete input.
- The trailing `;` of each REPL entry may be omitted.
- Definitions stay in the session until `:clear` is used.

Available commands:

| Command | Description |
| --- | --- |
| `:help`, `:?` | Shows the REPL help. |
| `:quit`, `:q`, `:exit` | Leaves the REPL. |
| `:clear` | Forgets all user definitions. |
| `:load <file>` | Runs a script inside the current session. |
| `:vars` | Lists the user-defined global variables. |

## Examples

In the `examples/` directory (see `examples/README.md` for details):

| File | Contents |
| --- | --- |
| `calc.oskl` | An arithmetic expression evaluator: lexer, parser and evaluator. Builds with `oskel build`. |
| `mandelbrot.oskl` | The Mandelbrot set rendered as ASCII art. Builds with `oskel build`. |
| `tui_editor.oskl` | A minimal terminal text editor (interactive; VM only). |
| `hello.oskl` | Greeting and string interpolation. |
| `fizzbuzz.oskl` | Loops, conditionals and modulo. |
| `fibonacci.oskl` | Recursion and an iterative version. |
| `closures.oskl` | Closures and functions that remember state. |
| `collections.oskl` | Arrays, maps, filtering, reduction and sorting. |

```sh
oskel examples/fizzbuzz.oskl
oskel build examples/calc.oskl -o calc && ./calc
```

## The language in a minute

```oskel
// Variables: var is mutable, let is immutable, const is a compile-time constant
let name = "world";
const VERSION = "0.1.0";

// Functions and closures
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

// Conditionals and loops
if (counter() > 2) {
  print("going fast");
} else {
  print("going slow");
}

for i in 1..=3 {
  print("i = ${i}");
}

// Collections
let numbers = [1, 2, 3, 4, 5];
let evenSquares = numbers
  .filter(n => n % 2 == 0)
  .map(n => n * n);
print(evenSquares); // [4, 16]

let person = {name: "Ada", age: 36};
person.age += 1;
print("${person.name} is ${person.age} years old");
```

The full reference is in **[docs/language.md](docs/language.md)**.

## What runs where (VM vs AOT)

Oskel has two execution paths: the register-based **VM** (used by `oskel`,
`oskel -e` and the REPL) and the **AOT backend** (used by `oskel build`, which
emits a static native binary). The AOT backend covers the language core, not
the whole runtime yet, so a program can run on the VM and fail to build — or
behave differently — under AOT. This is the exact split in 0.3.0.

**Available in both:**

- Arithmetic, comparisons, logical and bitwise operators, control flow
  (`if`/`while`/`for`/`for-in`/`switch`), functions, closures, `defer`,
  `struct`, `class`, ranges, slicing, string interpolation.
- Arrays and maps (literals, indexing, `in`, `for-in`, printing, and `+` to
  concatenate two arrays).
- The array methods `push`, `pop`, `shift`, `unshift`, `insert`, `removeAt`,
  `contains`, `indexOf` and `clear`. Each binds to a closure that captures its
  receiver, so a call mutates or scans the same array; `contains`/`indexOf`
  compare structurally (an array or map needle compares by content).
- These builtins: `print`, `write`, `eprint`, `len`, `type`, `str`, `repr`,
  `bool`, `ord`, `chr`, `assert`, `panic`, `isError`, `error`, `pcall`, `exit`,
  `clock`, `args`, `readFile`, `writeFile` and `fileExists`.
- The numeric conversions `int`, `float`, `double`, and the math builtins
  `floor`, `ceil`, `round`, `sqrt`, `pow`, `exp`, `log`, `sin`, `cos`, `tan`.

**VM only — not yet available in `oskel build` (a build error names the global):**

- The string methods such as `str.upper`, `str.split` and `str.trim`, and
  `sort`/`sorted` on an array. They compile but fail at runtime with
  `value has no such member`.

**Available in both, but with different semantics:**

- **`map.get` is O(n) in AOT** (linear scan) instead of O(1) (hash lookup), so
  a loop that looks up every key of a large map is O(n²) in a compiled binary.

The divergences are tracked internally in `docs/BACKLOG.md`; closing them (and
a VM/AOT parity test) is the goal of the remaining AOT work.

## Bare metal (experimental)

`oskel build --target x86_64-freestanding-sysv file.oskl -o kernel.elf` emits a
**multiboot 1 kernel**: a 32-bit ELF image that a boot loader loads at 1 MiB
and enters directly, with no operating system underneath. The image opens with
a small 32-bit trampoline that builds an identity page table, turns on long
mode and installs its own 64-bit GDT, then jumps into the same 64-bit runtime
the Linux target uses — so the compiled program is byte-for-byte identical; only
the prelude differs, because a kernel has no system calls.

The bare-metal prelude reaches the outside world through the hardware:

- `oskel_write` drives the 16550 UART at COM1 directly (polling the
  line-status register), so `print` shows up on the serial line.
- `oskel_exit` masks interrupts and halts the CPU in a loop (`cli`; `hlt`).
  Under QEMU it also publishes the exit code on the `isa-debug-exit` port so a
  test can observe it; on real hardware that port write is ignored.
- `oskel_init_arena` points the bump allocator at a `.bss` region the loader
  zeroes, instead of calling `mmap`.

Run it under QEMU:

```sh
oskel build --target x86_64-freestanding-sysv hello.oskl -o hello.elf
qemu-system-x86_64 -kernel hello.elf -serial stdio -display none
```

The same image boots from GRUB or Limine with a multiboot entry. The whole
language core that builds for Linux builds here too — the two share the
lowering and the embedded runtime; the divergences in
[What runs where](#what-runs-where-vm-vs-aot) apply unchanged. The end-to-end
boot is exercised by `tests/bare_metal_test.cpp` when `qemu-system-x86_64` is on
`PATH`.

The prelude reports CPU *exceptions* (it installs an IDT and prints a trace
before halting) and now *services* the timer: it remaps the 8259 PIC onto
vectors 0x20-0x2F, programs the 8254 PIT at 100 Hz, unmasks only IRQ0 and
enables interrupts (`sti`) once the IDT and the PIC are ready. The timer
handler acknowledges the PIC and returns with `iretq` — a real interrupt
service loop, kept separate from the fatal fault handler. The tick count is
kernel state (the handler is silent per tick); after 500 ticks a program that
never returns gets one `tick limit` line and a dedicated exit code. The design
lives in
[docs/bare-metal-interrupts-design.md](docs/bare-metal-interrupts-design.md).

## WebAssembly (experimental)

`oskel build --target wasm32-freestanding-wasm file.oskl -o app.wasm` emits a
**WebAssembly module** from the same bytecode the VM and the x86-64 backend
consume. It is a new backend behind the `Target` descriptor
(`Target::wasm32()`), not a different compiler: the language, the type checker
and the lowering pipeline are shared, and only the emitter differs.

The module is self-contained except for four imports and one export:

```wat
(import "oskel" "write" (func (param i32 i32 i32) (result i32)))  ;; fd, ptr, len
(import "oskel" "exit"  (func (param i32)))                       ;; exit code
(import "oskel" "time"  (func (result i64)))                      ;; epoch second
(import "oskel" "clock" (func (result f64)))                      ;; monotonic second
(export "_start" (func))
```

A host supplies them. `tests/run_wasm.cjs` is the reference Node host:

```sh
oskel build --target wasm32-freestanding-wasm hello.oskl -o hello.wasm
node tests/run_wasm.cjs hello.wasm
```

In a browser, `WebAssembly.instantiate` with an `oskel` import object whose
`write` copies `memory.buffer[ptr .. ptr + len]` into a `Uint8Array` (or a
`TextDecoder`) is enough; `_start` runs the program.

The value model is the AOT one — a `(tag, payload)` pair of `i64` per register —
so tags, string layout, closure layout and the semantics of every operator
match the x86-64 backend. Strings, ranges, closures/upvalues, error values,
arrays and maps (indexing, iteration, structural equality, `str`/`repr`),
slicing, variadic functions, control flow, the portable builtins and the
array/map/string *methods* (`.push`, `.map`, `.split`, ...) work;
`std/json`, `std/datetime`, `std/collections`, `std/path` and the bound half of
`std.math` build and agree with the VM byte for byte, and `double` printing is
the same shortest round-trip form. A builtin with no WASM body is seeded as a
closure that raises a clear runtime error when called. The remaining gaps are
listed in [DEVELOPMENT.md](DEVELOPMENT.md) PHASE 9 and `docs/BACKLOG.md` §6.

## Tests

The simplest way to run everything (all `.oskl` scripts, the error cases, the
examples and both C++ suites) is CTest:

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

With a multi-config generator (for example Visual Studio) add
`-C Release` to the `ctest` invocation. The same tests are available through
the helper scripts below.

Windows:

```bat
powershell -ExecutionPolicy Bypass -File tests\run.ps1
powershell -ExecutionPolicy Bypass -File tests\run_compiler.ps1
powershell -ExecutionPolicy Bypass -File tests\run_vm.ps1
powershell -ExecutionPolicy Bypass -File tests\run_gc.ps1
```

Linux, macOS or MSYS2:

```sh
./tests/run.sh
./tests/run_compiler.sh
./tests/run_vm.sh
./tests/run_gc.sh
```

The positive tests use `assert` and `assertEq`; a script exits with a non-zero code if any assertion fails. The tests in `tests/errors/` check compile-time and runtime errors: they declare the expected exit code on their first line with `// expect-exit: N`. The bytecode compiler tests (`compiler_test.cpp`) verify the instruction sequence, the constant pools, the `nil`/boolean literals, the unary and compound operators, the comparisons, the `if`/`while`/`for` jumps with backpatching, the local slots and the rejection of assignments to immutable `let` bindings. The VM tests (`vm_test.cpp`) compile snippets to bytecode and check the execution of integer and float arithmetic, `%`, `**`, comparisons, unary operators, globals, strings, compound assignment, `if`/`else`, loops, `break`/`continue`, block shadowing, bitwise operators, `&&`/`||` short-circuiting, the ternary operator and runtime errors.

## Limitations and roadmap

Current limitations:

- Dynamic typing. The product model is Lua's: source code is always compiled to bytecode and run on the VM. The CLI and the REPL both compile to bytecode and run on the register-based VM; the AST is never executed.
- The development engine (`oskel`) also ships an **experimental JIT** for a narrow integer subset: a *leaf* function whose body only manipulates `nil`/`bool`/`int` is compiled to native x86-64 the first time it is called. A compile-time optimizer folds constants and applies them as machine immediates (`i * 2` becomes one `shl`), a comparison that only feeds the branch after it becomes `cmp; jcc`, and a web-based register allocator with copy coalescing gives each value its own register (a copy whose ends do not interfere disappears, and a slot only keeps its stack slot when the seven-register pool runs out), so a tight integer loop runs about 420x faster than the interpreter and within ~1.08x of a hand-written `-O2` C++ loop. Everything else keeps running on the interpreter, so results never change. It is a development-engine feature only: `oskel build` and the generated binary never contain it (`oskel --no-jit` disables it).
- There are no interfaces or inheritance; maps and closures replace them. A `struct` (data only, no methods) is a named map shape checked at compile time. A `class` (fields plus methods, PHASE 6.8) is the method-carrying form: it is parser sugar over a map plus closures capturing an explicit `self`, instantiated by calling the class name (no `new`), with no inheritance.
- There is no `try`/`catch` *statement* or catchable exceptions. Errors are values: `error(message, code?)` builds a first-class error value and `isError(value)` detects it. `pcall(fn, ...)` turns a raised runtime error into a `{ok, value, error}` map instead of aborting. Two operators work on error values: `try expr` propagates one to the caller (Rust's `?`) and `expr catch default` recovers locally (Zig's `catch`). `panic()` is for unrecoverable errors.
- Modules use Java-style `import` (`import std.math;`, `import std.math.*;`, `import std.math as m;`, `import { sqrt } from std.math;`) with compile-time conflict detection for `.*` and a reserved `std.` prefix. There is no `require`; modules are loaded eagerly and a cyclic import is a compile error.
- There is no `++`/`--` or default parameters. `switch` on literals and integer ranges is supported, with no fallthrough, and so is the post-test loop `do { ... } while (cond);`.
- `defer { ... }` runs a block when its scope exits, in LIFO order, on fall-through, `return` and `break`/`continue` alike; it compiles to no new opcode.
- Optional types and nullability are compile-time only and are erased before the bytecode; a union (`T | E`) has no C representation, so it is rejected in `extern "C"`/`native fn` signatures and struct fields.
- FFI **callbacks** use a small x86-64 closure stub in the development engine, available on the System V ABI (Linux and x86-64 macOS). On Windows and arm64 a callback declaration is rejected with a clear compile-time message; the AOT backend emits its own callback stub on System V.
- Map keys are always strings. Map literals accept identifier, string, scalar (`int`, `true`, `false`, `nil`) and computed `[expr]` keys; non-string keys are canonicalized to their string form.
- The body of `if`, `while`, `for` and `for-in` may be a block or a single statement without braces; a brace-less body is wrapped in a block, so its bytecode and scoping match the brace form. `switch` arms always require braces.
- Slicing syntax works for strings and arrays (`s[1:3]`, `a[2:]`, `a[:-1]`), with negative bounds counted from the end and an optional step (`a[::2]`, `a[::-1]` reverses); `substr`/`slice` remain available.
- `upper` and `lower` only transform ASCII letters.
- The maximum call depth is 2000.
- Memory is managed with reference counting plus a cycle collector (`src/core/gc.h`) that reclaims unreachable cycles, such as a function stored in the scope it closes over. A cycle that runs through state captured by a native method (`a.f = a.push`) is never collected.

Ideas for future versions:

- Completing the AOT backend (PHASE 7): the core of the language already
  compiles to native code; the standard library builtins and file I/O are next.
- User-defined methods on structs.
- Better Unicode support (upper/lower case and graphemes).
- More tooling: formatter and debugger.

Editor support: a VS Code syntax-highlighting extension lives in
[`editors/vscode`](editors/vscode).

## Releases

**0.3.0 is the current release.** Prebuilt packages are published on the
[releases page](https://github.com/yasmramos/Oskel/releases):

| Platform | Asset |
| --- | --- |
| Linux | `oskel-linux-x64.tar.gz`, `.deb`, `.rpm` |
| Windows | `oskel-windows-x64.zip`, `oskel-windows-x64-setup.exe` |
| macOS | `oskel-macos-arm64.tar.gz`, `.pkg`, `.dmg` |
| Editor | `oskel-vscode.vsix` |

Every release also ships a `SHA256SUMS` file; verify a download with
`sha256sum -c SHA256SUMS`. The changelog for each version is in
[`CHANGELOG.md`](CHANGELOG.md), and the maintainer checklist is in
[`docs/RELEASING.md`](docs/RELEASING.md).

`oskel build` is the AOT entry point. A separate host compiler, `oskelc`,
ships beside it: it links the front end and the AOT backend without the VM or
the JIT, so a compiled program carries no interpreter. Both call the same
`buildBinary` pipeline, so they emit the same image.

## License

Distributed under the **MIT** license. See [LICENSE](LICENSE).

---

Made with C++17, with no external dependencies.
