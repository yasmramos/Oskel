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

`oskel build` targets `x86_64-linux-sysv` only. The examples below note which
ones build.

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

**AOT-clean.** It avoids string/array methods and the `int()`/`float()`
conversions (PHASE 7c work) so that `oskel build` produces a native binary that
agrees with the VM byte for byte. This is checked by the
`oskel.build_binary_calc` test.

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
and saves with `:w`/`:wq`. It is the showcase for the features the native
backend cannot compile yet.

```sh
oskel examples/tui_editor.oskl notes.txt   # interactive
echo "" | oskel examples/tui_editor.oskl   # self-test (no tty)
```

Keys: arrows to move, `Enter` and `Backspace` to edit, `Ctrl-S` to save,
`Ctrl-C` to quit, `:` to enter a command (`:w`, `:q`, `:wq`, `:h`).

**Not AOT-clean (yet).** It uses `extern "C"` FFI (`system`, `isatty`, `read`),
pointers and buffers, file I/O, and `args()` — none of which the freestanding
backend supports today (PHASE 7c). It also exercises `struct`, closures for
undo/redo, `defer` to restore the terminal, `switch`, optional types and unions,
`@MustUse` and variadic parameters.

When stdin is not a terminal — a pipe, or CTest — it runs a deterministic
self-test instead of the interactive loop, so `examples.tui_editor` verifies it
without needing a tty. The test is registered on UNIX only, because the editor
drives the terminal through POSIX C functions.

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
| `wordcount.oskl` | A map-based frequency count. |
| `full.oskl` | A single program that touches most features; prints `full_example_output.txt`. |

## How the examples are tested

`CMakeLists.txt` globs `examples/*.oskl` and runs each one as a CTest script
test (`examples.<name>`), so every example must exit cleanly. The programs that
build with the AOT backend are additionally compiled and run by
`oskel.build_binary_calc` and `oskel.build_binary_mandelbrot`, which compare the
native output against the VM.
