# AOT diagnostics: line numbers and stack traces

Status: increment 1 shipped (PHASE 7g.3); increments 2-3 designed.

A compiled Oskel binary (`oskel build`) reports a runtime error as one bare
line:

    oskel: type 'nil' has no member 'foo'

while the VM prints the source position, the snippet, and the call stack:

    file.oskl:2:10: runtime error: type 'nil' has no member 'foo'
      |
    2 |   return nil.foo;
      |          ^
    stack trace:
      at inner (file.oskl:5:15)

This document records why the compiled image cannot print that today, what a
faithful fix needs, and the increments it is delivered in.

---

## 1. Why the image has no position

Three independent reasons, each of which alone is enough to lose the position:

1. **The lowering never reads it.** `Instruction` carries `line` and `col`
   (`include/bytecode.h`), the compiler fills them on every emission, and the
   comment on the field says "Native codegen in PHASE 7 ignores these fields".
   `src/frontend/native/lower.cpp` contains zero references to `ins.line`.
   The position is dropped at the first step of the backend.

2. **There is no table to look a position up in.** The image is one flat code
   buffer with every `call` resolved through emitter labels; there is no
   table, no section and no relocation the runtime could index by an address.
   The failure path (`emitFailDynamic`, `src/frontend/native/runtime.cpp`) is
   handed constant blobs only, so it can only print text the *compiler* wrote
   into the image.

3. **The call stack is not walked.** Each lowered function does establish a
   frame (`push rbp; mov rbp, rsp` in `emitFunctionPrologue`) and each runtime
   helper does too (`prologue()`), so an `rbp` chain exists through *helper*
   frames -- but a lowered function does not publish where it began, so the
   chain through *program* frames is not walkable. The sink never tries.

## 2. What a faithful fix needs

- **A PC → line table per function**, emitted into the image and reachable from
  the failure path. The lowering knows the offset of every instruction label
  (`cg.labelOffset()`), so the table costs one row per instruction and no
  extra pass over the code.
- **A function table**: entry address, end address, name, and the address of
  its line table. The lowering already materialises each proto's name as a
  static `AotString` for the closure `display` field (PHASE 7f), so the name
  bytes exist; the failure path needs them keyed by *address range* instead.
- **A chain through program frames.** A lowered function must record where its
  caller's frame is. Pushing one extra word per frame is the price; it is what
  turns `rbp` from "a saved value" into a walkable chain.
- **The sink stops being text-only.** `emitFailDynamic` must be able to append
  `:line:col` to the position it is given and to walk the chain after it.

## 3. What increment 1 does

The design in §2 is the destination; increment 1 takes the shortest route that
survives the constraints rather than the one drawn above.

- **A hidden global carries the line, not a PC → line table.** The line is
  written to a global slot the runtime already addresses (`r15` + `16 * slot`)
  before the instruction that needs it, and the one sink reads it. That costs
  one store per *line change* (deduplicated at lowering time) and no table, no
  lookup, and no image section -- none of which the freestanding image has.
- **The lowering publishes only when the line changes, and always at a jump
  target.** Deduplication is per function. Unconditional publication at a jump
  target is not an optimisation detail: the fall-through path may skip the
  instruction that published, so without it an `if` inside a loop can report
  the wrong line. The cost is one store per loop header and merge point.
- **The sink is the only place that changed.** Every runtime error already
  funnels through `emitFailDynamic`; the position is printed there, so none of
  the ~40 `emitFail*` call sites were touched.
- **Opt-in at lowering time, on by default on the command line.**
  `DebugSource{path}` decides whether the slot is reserved and the stores are
  emitted. A caller that passes no path -- every C++ test, the bare-metal and
  freestanding harnesses -- gets the image it always got, byte for byte.
  `oskel build` passes the path, so debuggability is the default; the
  `--no-source-positions` flag passes an empty one and reproduces the frozen
  image exactly. The two paths are the same code, so the frozen guarantee
  cannot rot: `tests/aot_runtime_error.cmake` builds the fixture both ways and
  requires the sizes to differ and the frozen report to carry no position.
- **The path travels in the chunk, not on the command line.** An `.oskc` image
  stores the source path it was compiled from, so building a binary from the
  image names the `.oskl` and emits the *same bytes* as building from the
  source. This is what keeps the `oskc_parity` test honest: without it the
  image build would name the `.oskc` and the two binaries would differ.

The column (§3 increment 2) is deliberately not in this step: `col` is in the
bytecode, but printing it needs the same store, a second hidden slot, and the
VM's caret format has no meaning without the snippet. It is a small follow-up
on the mechanism this step establishes.

## 4. Increments

Each increment is independently shippable, keeps the emitted binary of the
program itself unchanged, and adds its own fixture under `tests/native_build_*`
so the VM/AOT parity contract covers it.

| # | Increment | Ends when |
|---|-----------|-----------|
| 1 | Position on the sink: `oskel: file.oskl:LINE: <message>` | **shipped**: `tests/aot_runtime_error.cmake` compares the binary's line with the VM's; `tests/lower_test.cpp` pins the byte-for-byte opt-in |
| 2 | Column, from the same table | the caret column matches the VM's |
| 3 | Function frame chain: `stack trace:` with `at fn (line)` per frame | a nested-call fixture prints the same three frames the VM prints |
| 4 | Snippet (`|` gutter and caret) | optional; needs the source text in the image |

## 5. Non-goals

- **DWARF.** A debugger needs line *sections* a linker understands, which the
  freestanding path deliberately does not have (no linker, no sections). The
  table in §2 is for the program's own error report, not for `gdb`. A DWARF
  emitter belongs to the hosted mode, where an object with sections already
  exists (`native/object.h`), and is a separate piece of work.
- **Recovering a position the image never had.** If a runtime helper faults
  without the sink (`ud2`, a CPU exception, the bare-metal fault handler), the
  report stays what it is today. Only an error that reaches the sink carries a
  position.
- **Changing the program's own machine code to carry the table.** The table
  lives beside the code and is addressed through the runtime handles; a
  program's lowering must stay byte for byte what it is today, exactly like
  the freestanding contract in AGENTS.md.
