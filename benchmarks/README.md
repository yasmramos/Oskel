# Benchmarks

Four small programs, written once per language, that do the same work and print
the same checksum. The point is not to win a shoot-out but to have a number to
regress against, and to catch the kind of bug a micro-test misses: the arena
limit that killed `oskel build` on arrays (`docs/BACKLOG.md` §6 item 33) and the
quadratic `join` this suite found (item 36) both showed up here first.

```
python3 benchmarks/run.py                                  # everything installed
python3 benchmarks/run.py --only fib,arrays --reps 7
python3 benchmarks/run.py --output docs/benchmarks.md      # the checked-in table
```

`$OSKEL_EXE` overrides the engine (default `build/oskel`); `$CC` the C driver
(default `cc`). A runtime that is not installed is reported as `n/a` and named
in the footnotes, so a missing column is never silent. `--no-wasm` drops the
WebAssembly column.

## What is measured

| benchmark | work | checksum |
| --- | --- | --- |
| `startup` | print one line | `startup 1` |
| `fib` | naive `fib(30)`, 2 692 537 calls, no memoisation | `fib 832040` |
| `arrays` | append 200 000 integers, then sum them | `arrays 19999900000 200000` |
| `strings` | build 5 000 short strings, join with `,`, split, sum lengths | `strings 48889 5000 43890` |

Oskel appears four times: `oskel-vm` (the development VM, JIT on),
`oskel-nojit` (`--no-jit`), `oskel-aot` (`oskel build`, the freestanding image)
and `oskel-wasm` (`--target wasm32-freestanding-wasm` under `tests/run_wasm.cjs`).
The other columns are reference points: C (`-O2`), CPython, Node, Lua 5.4 and
LuaJIT.

The algorithm is the same everywhere and so are the sizes; where a language has
no primitive for a step, the step is written by hand and the README says so
(C has no `join`/`split`, Lua has no `split`). Nothing is memoised, nothing is
special-cased.

## Method

- The runner takes the **VM's output as the reference** and refuses to time an
  implementation whose output differs. A benchmark that computes something else
  measures nothing, and a divergence between engines is a bug, not a result.
- One warm-up run, then `--reps` (5 by default) timed runs; the **median** is
  reported.
- The measurement is the **whole process**: wall-clock of `fork`/`exec` to exit.
  That is what a user waits for, and it keeps the languages comparable without
  each one needing its own in-language timer. The `startup` row is the baseline
  to subtract before comparing two runtimes on a short workload -- an empty Node
  process costs ~28 ms against ~0.2 ms for a static Oskel image, which dominates
  every other row that runs in single-digit milliseconds.
- No CPU pinning, no `taskset`: the numbers move a few percent between runs.
  Read them as orders of magnitude and as a trend, not as a measurement of the
  hardware. Re-run with `--reps 9` before quoting a small difference.
- Nothing here runs in CI. It needs `cc`, `node`, `lua` and `luajit`, and a
  timing test on a shared runner is a flaky test. The suite is a developer tool:
  `python3 benchmarks/run.py` after a change that touches allocation, the call
  path, the method dispatch or the arena.

## Reading the table

`docs/benchmarks.md` is the last recorded run, with the machine in its header.
Two things in it are expected and worth knowing before they look like bugs:

- **`strings` is slow on `oskel-aot` and `oskel-wasm`.** Both build a `join` by
  concatenating one item at a time, so the work is O(n²) in allocation while the
  VM's `join` is linear (`std::string`). `n` is capped at 5 000 for that reason;
  at 20 000 the WASM column dies in the host's `write` (`docs/BACKLOG.md` §6
  items 36 and 37) and AOT allocates ~2 GB. The row is here because the suite
  should show the regression, not hide it.
- **`arrays` on `oskel-aot` pays for a closure per `push`.** `xs.push(i)` reads
  the method before every call and the arena never reclaims the closure
  (`docs/BACKLOG.md` §6 item 35). Caching it (`var p = xs.push;`) makes the
  binary faster; the suite measures the code a user writes instead.

## Adding a benchmark

Add `<name>.oskl`, `.c`, `.py`, `.js` and `.lua` beside the others, print one
line whose values are computed from the same algorithm in every language, and
list the name in `BENCHMARKS` in `run.py`. Keep the sizes small enough that the
whole suite finishes in seconds: the runner multiplies every row by
`--reps + --warmup`, and `oskel-vm` is 300x slower than C on `fib`.

A new language column is one `Implementation` in `run.py` -- a prepare step
that compiles, the argv that runs the program, and the command that names the
runtime version -- plus the `<name>.<ext>` source. Rust, Go and Java fit the
same shape; they are not here because the suite is meant to run on a working
copy of the engine without a toolchain hunt.
