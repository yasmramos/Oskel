# Tutorial: a native CLI tool in Oskel

This tutorial builds a small command-line tool with Oskel and ships it as a
**self-contained native binary**: no interpreter, no runtime to install, no
shared libraries. It is the use case Oskel is designed for — a fast, small,
predictable CLI — and it exercises the whole pipeline (lexer → parser →
compiler → bytecode → AOT backend).

The numbers below were measured on Linux x86-64 (Debian, g++ 14, `-O2`) from
the tree at the time of writing. Re-measure on your machine before quoting
them.

---

## 1. Why AOT, and what it buys you

`oskel <file.oskl>` compiles to bytecode and runs it on the register VM. That is
the **development engine**: fast iteration, a REPL, and the JIT for hot loops.
`oskel build <file.oskl>` compiles the same source all the way to **machine code**
and writes a static executable. That is the **product**.

| Property | `oskel` (dev engine) | `oskel build` (AOT) |
| --- | --- | --- |
| Needs Oskel installed to run | yes | **no** |
| Runtime in the binary | VM + JIT | embedded runtime only |
| Startup | ~1.7 ms (VM boot) | **~0.5 ms** |
| Hello-world binary | — | **24 KB** |
| Distribution | ship the `.oskl` + Oskel | ship **one file** |

The AOT binary links **no libc and no libstdc++**: it talks to the kernel
through direct syscalls and carries its own bump allocator and value runtime.
`ldd` reports *"not a dynamic executable"*.

For comparison, a hello-world compiled with `gcc -static` against glibc is
about **740 KB**; the Oskel binary is roughly **30× smaller** because it drags in
no C runtime.

---

## 2. A real tool: word frequency

Here is a complete program — `wordcount.oskl` (it is also in `examples/`). It
counts words, computes percentages and prints a report. It uses functions,
arrays, maps, `for-in`, the key/value form of `for-in`, `in`, indexing and
string interpolation:

```oskel
fn tally(words) {
  var counts = {};
  for w in words {
    if (w in counts) {
      counts[w] = counts[w] + 1;
    } else {
      counts[w] = 1;
    }
  }
  return counts;
}

fn report(counts, total) {
  print("word frequency report");
  print("total words:", total);
  for k, v in counts {
    var pct = (v * 100) / total;
    print(" ", k, v, pct);
  }
}

var words = ["red", "blue", "red", "green", "blue", "red"];
report(tally(words), words.length);
```

Run it with the development engine:

```sh
$ oskel wordcount.oskl
word frequency report
total words: 6
  red 3 50
  blue 2 33
  green 1 16
```

Now build it:

```sh
$ oskel build wordcount.oskl
$ ./wordcount
word frequency report
total words: 6
  red 3 50
  blue 2 33
  green 1 16
```

Same output, byte for byte. The difference is that `./wordcount` is a native
binary you can copy to a machine with no Oskel, no Python and no shared
libraries:

```sh
$ ls -l wordcount
-rwxr-xr-x 1 you you 26K wordcount
$ ldd wordcount
        not a dynamic executable
```

---

## 3. What the AOT backend supports today

`oskel build` covers the **core of the language** and the portable standard
library. Every language construct the VM implements lowers to native code:

- literals: `nil`, `true`/`false`, integers, floats/doubles, strings;
- arithmetic (`+ - * / % **`), bitwise (`& | ^ << >>`), unary (`- ! ~`) and
  every comparison;
- `if`/`else`, `while`, classic `for`, `for-in` (including `for k, v in map`),
  `break`, `continue`, ternary, `switch`, `defer`;
- functions, recursion, mutual recursion, closures and lambdas that capture
  their environment, variadic `rest...`, `struct`, `class`, interfaces and the
  eager `async fn`/`await` surface (no scheduler);
- arrays and maps as **data**: `[...]`, `{...}`, indexing (`xs[i]`, `xs[-1]`),
  members (`m.name`), `in` for arrays, maps and strings, and all bound
  array/map/string/range methods;
- every builtin the VM registers, gated only by what the target can back
  (`print`, `write`, `eprint`, `len`, `type`, `range`, `args`, `exit`, the
  conversions and math builtins, random, pointers, and the file families);
- string interpolation.

The remaining limits are properties of the **target**, not missing language
features. `extern "C"` and `native fn` need a host, so they work in
`--mode hosted`; the default freestanding image reports the unresolved symbol
and exits 65 instead of emitting a broken binary:

```sh
$ printf 'extern "C" { fn getpid(): int; }\nprint(getpid());\n' > f.oskl
$ oskel build f.oskl
lowering: extern "C" symbol 'getpid' cannot be resolved by the AOT backend (PHASE 7f)
$ oskel build --mode hosted f.oskl -o f   # links libc
```

The streaming and directory file builtins need a filesystem, so bare metal
rejects them by name; `args()` is available where the entry point receives an
argument vector; `memSize()` is bare-metal only; `std.ui.window` needs the
bundled SDL3 and Skia bridge and a hosted build. A `native fn` with no backend
stub fails the build with a dedicated message. This is the "no VM-only
features" rule (§3 of `DEVELOPMENT.md`): a construct is compiled or rejected
cleanly, never mis-compiled.

### 3.1 How the binary grows

The 24 KB of a hello-world is almost all fixed cost — the embedded runtime
(arena, value runtime, `print`/`format`, collections) is linked in whole whether
the program uses it or not. What the *program itself* adds on top is small and
roughly linear in the code, not in the source size:

| Program | Source | Binary |
| --- | --- | --- |
| `hello.oskl` (`print("hello")`) | 1 line | **24 KB** |
| `wordcount.oskl` (functions, maps, `for-in`, interpolation) | 23 lines | **26 KB** |
| 60 generated functions, each called once | 122 lines | **46 KB** |

The runtime floor is the bulk of it; the 60 functions add about 22 KB, i.e.
**~370 bytes per small function** of real machine code and metadata. There is no
interpreter loop or bytecode table carried along, so a larger program does not
pay a per-feature startup cost — it just emits more instructions. That is the
opposite of shipping an interpreter, where every feature is present in the
binary whether you use it or not.

---

## 4. Development speed vs. product speed

The development engine runs the same bytecode on the VM, with the JIT
specialising integer leaf functions. A tight integer loop shows the three
speeds on the same source (`n = 30_000_000`):

| Engine | Time |
| --- | --- |
| `oskel --no-jit` (pure VM) | ~4.6 s |
| `oskel` (VM + JIT) | **~0.20 s** |
| `oskel build` then `./prog` (AOT) | ~0.45 s |

### Why the JIT wins here, and why that is expected

The JIT *specialises* this loop and the AOT backend *generalises* it, and that
difference is the whole story.

- The JIT watches the loop run, sees that `s` and `i` are always integers, and
  emits a machine loop that keeps them in CPU registers as **unboxed** 64-bit
  ints. An `add` is one instruction; the tag is not carried at all.
- The AOT backend has to be correct for *every* value that could ever reach the
  loop — an `int`, a `float`, a string (where `+` concatenates), a table — so it
  emits the **general** lowering: each value is a boxed 16-byte `AotValue` in a
  stack register file, and every `+` reads two tags, branches to the int fast
  path, and writes a tag back. It never guesses, so it never needs a bailout.

That is not a bug in the AOT backend; it is the price of a static compiler with
no runtime type feedback. Oskel forbids exactly the escape hatch that would close
the gap — profile-guided or on-stack recompilation inside the AOT binary is a
VM-only feature (§3 of `DEVELOPMENT.md`: no VM-only features), and per-instruction
type annotations for the AOT fast paths are explicitly deferred (PHASE 7,
"Deferred"). The JIT may specialise because it runs in the development engine,
where a wrong guess can bail out to the VM; a shipped binary cannot.

So the fair comparison is the one that matters for a CLI: the AOT binary is
still about **10× faster than the plain VM** on this loop (it removes the
dispatch and the per-opcode interpretation), starts in half a millisecond, and
is the only one of the three that runs with no Oskel installed.

Use the JIT while you develop; ship the AOT binary.

---

## 5. Startup matters for a CLI

A CLI tool is launched over and over — from a shell, a script, a Makefile, a
CI job. Startup latency is a feature. Measured over 100 launches:

| Tool | Per-launch |
| --- | --- |
| `./wordcount` (Oskel AOT) | **~0.5 ms** |
| `python3 -c "print('hi')"` | ~12.5 ms |

A Oskel binary starts in well under a millisecond because there is nothing to
initialise: no interpreter, no GC heap, no shared-library resolution. It maps
its few kilobytes, runs, and exits.

---

## 6. Distribution

Because the binary is static and self-contained, "installing" your tool is
copying one file:

```sh
$ oskel build wordcount.oskl -o /usr/local/bin/wordcount
$ scp wordcount server:/usr/local/bin/
```

No `oskel` on the target, no matching runtime version, no `LD_LIBRARY_PATH`, no
container needed just to run a program. That is the model of `DEVELOPMENT.md` §2:
`oskelc` produces a self-contained native binary; end users never install Oskel
or any runtime.

---

## 7. Where to go next

- `docs/language.md` — the full language reference.
- `DEVELOPMENT.md` §4 (PHASE 7) — the AOT backend design and its sub-slices.
- `DEVELOPMENT.md` §4 (PHASE 8) — the JIT, development engine only.
- `examples/` — runnable programs (`collections.oskl`, `closures.oskl`, …).
