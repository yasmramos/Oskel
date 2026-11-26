# Oskel examples

Every `.oskl` file here is a complete, runnable program. They fall into two
groups: **programs** that do something useful, and **feature demos** that show
one part of the language at a time.

Run any of them with the development engine (the VM):

```sh
oskel examples/fizzbuzz.oskl
```

Programs that stay inside the native subset also compile to a self-contained
static binary with the AOT backend:

```sh
oskel build examples/calc.oskl -o calc
./calc
```

`oskel build` emits Linux x86-64 freestanding/hosted, Windows x86-64, bare
metal and WebAssembly images; hosted macOS x86-64 is supported too. The examples
below note which ones build.

## Full projects

### `ledger/` — a double-entry accounting CLI

A complete project scaffolded with `oskelp new`, not a single file: a chart of
accounts, balanced journal entries, a trial balance, an income statement, a
balance sheet, JSON persistence, CSV import/export and a `std.cli` command
line. Amounts are exact `std.decimal` values, and the VM and native binaries
print the same reports.

```sh
oskelp run --manifest-path examples/ledger/oskel.toml
oskelp test --manifest-path examples/ledger/oskel.toml
oskelp build --manifest-path examples/ledger/oskel.toml && ./examples/ledger/target/ledger
```

See [`ledger/README.md`](ledger/README.md) for the commands, the data formats
and the modules it exercises. The `oskelp.example_ledger` CTest runs the demo,
the project's tests and the file-backed commands, and compares the native
trial balance with the VM's.

## Programs

### `calc.oskl` — an arithmetic expression evaluator

A lexer, a recursive-descent parser and a tree-walking evaluator: a calculator
that turns a string such as `2 * (3 + 4) ** 2 - 8 / 4` into a value. It is the
flagship example — a real program, not a tour of features.

```sh
oskel examples/calc.oskl
oskel build examples/calc.oskl -o calc && ./calc
```

It exercises functions and recursion, closures and lambdas (including mutual
recursion between the parser's precedence levels), arrays and maps, `for-in`,
`switch`, ranges, string interpolation and slicing, `in`, `type`, `len`,
`assert`, ternaries and the full operator set. It also carries its own
`assert`-based checks, so it doubles as a test.

**AOT-clean.** It stays on the portable primitive surface, so `oskel build`
produces a native binary that agrees with the VM byte for byte. This is checked
by the `oskel.build_binary_calc` test.

### `mandelbrot.oskl` — the Mandelbrot set in ASCII

Renders the Mandelbrot set on a grid of complex points and maps each escape
count to a character, so the image is plain text on stdout. It is the
floating-point counterpart to `calc.oskl`.

```sh
oskel examples/mandelbrot.oskl
oskel build examples/mandelbrot.oskl -o mandelbrot && ./mandelbrot
```

It exercises floating-point arithmetic and comparisons, nested `while` loops,
arrays, string building and indexing, the modulo operator and `assert`.

**AOT-clean.** The `oskel.build_binary_mandelbrot` test checks that the VM and
the native binary print the same image.

### `tui_editor.oskl` — a minimal terminal text editor

A real interactive application: it opens a file, edits it with the arrow keys,
and saves with `:w`/`:wq`. It showcases FFI, pointer buffers, file I/O and
`args()` together in one program.

```sh
oskel examples/tui_editor.oskl notes.txt   # interactive
echo "" | oskel examples/tui_editor.oskl   # self-test (no tty)
```

Keys: arrows to move, `Enter` and `Backspace` to edit, `Ctrl-S` to save,
`Ctrl-C` to quit, `:` to enter a command (`:w`, `:q`, `:wq`, `:h`).

**Hosted AOT, not freestanding.** It uses POSIX `extern "C"` FFI (`system`,
`isatty`, `read`), pointer buffers, file I/O and `args()`. The AOT backend
supports all of these in `--mode hosted`, but a freestanding image has no host
to resolve the symbols. The suite runs it on the VM because it needs a tty; it
also exercises `struct`, closures for undo/redo, `defer` to restore the
terminal, `switch`, optional types and unions, `@MustUse` and variadic
parameters.

When stdin is not a terminal — a pipe, or CTest — it runs a deterministic
self-test instead of the interactive loop, so `examples.tui_editor` verifies it
without needing a tty. The test is registered on UNIX only, because the editor
drives the terminal through POSIX C functions.

## Desktop UI (`examples/ui/`, `examples/sdl/`)

These demos open a real window, so they need SDL3 and the Skia shim. The
release SDK bundles both; a source build fetches the same pair with
`python3 tools/prepare_ui_deps.py` (or points at its own copies through
`OSKEL_UI_LIBDIR`). The widget layer itself is pure Oskel and stays testable
without a display; only the presentation path needs the libraries.

### `ui/todo.oskl` — a task list with buttons

The flagship desktop demo: `std.ui` builds the widget tree and
`std.ui.window` runs the event loop. Each row has a `Toggle` and a `Remove`
button, and the toolbar adds a sample task or clears the finished ones. The
tree is rebuilt in place, so the window reflects every click.

```sh
# after `python3 tools/prepare_ui_deps.py` and a CMake build
OSKEL_UI_LIBDIR=build-cmake/ui ./build-cmake/oskel examples/ui/todo.oskl
```

A numeric argument bounds the run and drives one pointer click headlessly, so
the same file can be exercised without a device:

```sh
OSKEL_UI_LIBDIR=build-cmake/ui ./build-cmake/oskel examples/ui/todo.oskl 2
```

The native form is a hosted build; the compiler selects it from the imported
UI modules and links the bundle:

```sh
OSKEL_UI_LIBDIR=build-cmake/ui ./build-cmake/oskel build examples/ui/todo.oskl -o oskel-todo
./oskel-todo
```

### `ui/window.oskl` — the minimal window

A counter with one button, the smallest complete `std.ui` + `std.ui.window`
program. Start here when reading the widget API.

### `sdl/window.oskl` — raw SDL3

A moving rectangle drawn with the low-level `std.internal.sdl` bindings, the
layer the UI backend sits on. User code normally imports `std.ui` and
`std.ui.window` instead.

## Feature demos

Smaller programs, each focused on one area.

| File | Shows |
| --- | --- |
| `hello.oskl` | Greeting and string interpolation. |
| `fizzbuzz.oskl` | Loops, conditionals and modulo. |
| `fibonacci.oskl` | Recursion and an iterative version. |
| `closures.oskl` | Closures and functions that remember state. |
| `collections.oskl` | Arrays, maps, filtering, reduction and sorting. |
| `structs.oskl` | `struct`: a named, checked map shape. |
| `builtins.oskl` | The public standard-library builtins. |
| `imports.oskl` | The four Java-style `import` forms, with `examples/modules/geometry.oskl`. |
| `math_imports.oskl` | The `std.math` standard library through every import form. |
| `json.oskl` | The `std.json` standard library: parsing, serializing and the value mapping. |
| `datetime.oskl` | The `std.datetime` standard library: dates, times, the Unix epoch, ISO-8601 and the clock. |
| `path.oskl` | The `std.path` standard library: joining, splitting and normalising filesystem paths. |
| `collections_stdlib.oskl` | The `std.collections` standard library: mapping, filtering, folding, grouping and sorting, over a small data set. |
| `wordcount.oskl` | A map-based frequency count. |
| `full.oskl` | A single program that touches most features; prints `full_example_output.txt`. |

## How the examples are tested

`CMakeLists.txt` globs `examples/*.oskl` and runs each one as a CTest script
test (`examples.<name>`), so every top-level example must exit cleanly. The
programs that build with the AOT backend are additionally compiled and run by
`oskel.build_binary_calc` and `oskel.build_binary_mandelbrot`, which compare the
native output against the VM.

The demos under `examples/ui/` and `examples/sdl/` are not part of that glob:
they need SDL3 and the Skia shim, which a plain checkout does not have. The
widget half is covered by `tests/native_build_ui.oskl`, and the window half by
`tests/run_skia_case.cmake` when those libraries are present.
