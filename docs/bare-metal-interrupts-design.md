# Bare-metal interrupt path — design

Status: **design only, no code**. This document proposes the first slice of the
bare-metal operating-system path: taking hardware interrupts, returning from
them, and receiving a periodic timer tick.

Scope: the bare-metal x86-64 (multiboot) target only
(`--target x86_64-freestanding-sysv`). It touches `include/native/codegen.h`,
`src/native/codegen.cpp`, `include/native/bare_metal.h`,
`src/native/bare_metal.cpp` and `tests/bare_metal_test.cpp`. It changes no
token, node, parser rule, opcode, VM path or compiler path.

Non-goals: `asm!`, `#[naked]`/`#[interrupt]`, `--no-runtime`, a scheduler,
memory management, drivers beyond the timer, userland and an ELF loader. Those
are later slices; this one only makes the interrupt *mechanism* work, because
every one of them depends on it.

---

## 1. The gap

The bare-metal prelude installs an IDT (`emitIdt`,
`src/native/bare_metal.cpp:185`) whose 256 gates all point at a handler that
prints `oskel: cpu exception 0xVV rip=0x...` and halts
(`src/native/bare_metal.cpp:200-228`). Three facts make that an *exception*
reporter and not an *interrupt* service:

1. **No return path.** The handler ends in a `cli; hlt` loop
   (`src/native/bare_metal.cpp:221-228`); there is no `iretq`, so the frame the
   CPU pushed is never unwound and nothing resumes.
2. **Interrupts are never enabled.** The 32-bit trampoline opens with `cli`
   (`src/native/bare_metal.cpp:90`) and nothing ever executes `sti`: the
   `CodeGen` has no `sti` primitive, and no byte `0xFB` is emitted as an
   instruction anywhere in `src/native/`.
3. **The PIC is not remapped.** The 8259 PIC powers up mapping IRQ0-IRQ7 onto
   vectors 0x08-0x0F, which collide with the CPU's own exceptions (#DF at 8,
   #TS at 10, ...). Without a remap, enabling IRQ0 delivers a spurious-looking
   fault, not a usable interrupt.

So the machine can *report* a fault and *halt*; it cannot *service* a device.
The timer, the keyboard and any scheduler need the second thing.

## 2. What this slice delivers

A periodic timer tick that the kernel takes, acknowledges and **returns from**.
Concretely, after this slice the prelude:

1. Remaps the 8259 PIC so IRQ0-IRQ15 land on vectors 0x20-0x2F.
2. Programs the 8254 PIT (channel 0, mode 3) for a fixed frequency.
3. Unmasks only IRQ0 and masks the rest.
4. Adds a returning handler: the tick stub ends in `iretq`, not a halt loop.
5. Enables interrupts (`sti`) immediately before it runs the program.

The observable proof is a program that spins forever while the handler fires
three times, prints a line on each tick and then exits — the first two ticks
only happen if the handler returned.

## 3. Mandatory checklist (AGENTS.md)

| Question | Answer |
|----------|--------|
| New lexer token? | **No.** No source syntax changes. |
| New AST node? | **No.** |
| New parser rule? | **No.** |
| New bytecode opcode? | **No.** The 39-opcode set is untouched. |
| Opcode 1:1 or 1:2 ASM? | **N/A** — no opcode is added. |
| New VM logic? | **No.** The VM never runs the bare-metal prelude. |
| New compiler logic? | **No.** `src/compiler.cpp` is untouched. |

This is a backend-only change: two new `CodeGen` primitives (each one
instruction) and a rewrite of the bare-metal prelude's interrupt setup.

## 4. Decisions

### D1 — Two handlers, not one

The current single handler is fatal on purpose: an exception is a bug, and a
kernel that continues after a `#PF` in the middle of the runtime is a kernel
that corrupts its own state. That property must stay. So the slice keeps the
fault handler exactly as it is (report + halt) and adds a *second*, separate
handler for IRQ0 that acknowledges the PIC and `iretq`s.

The dispatch is by vector, in the stub:

- Vectors other than 0x20 jump to `faultHandler` (unchanged).
- Vector 0x20 jumps to `irqHandler` (new).

`emitIdt` already creates one stub per vector
(`src/native/bare_metal.cpp:187-234`); the change is one conditional in that
loop. This keeps a device interrupt from ever being confused with a fault: a
`#PF` still halts with a trace.

### D2 — The returning handler preserves the interrupted context

An interrupt arrives at an arbitrary instruction, so the handler must leave
every register it touches the way it found it. The handler therefore pushes
the caller-saved set it uses, does its work, pops them back, discards the two
dwords the stub pushed (vector + dummy error code) and `iretq`s:

```
irqHandler:
  push rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11   ; the handler's scratch set
  mov  rbp, rsp                                    ; save the interrupted rsp
  and  rsp, -16                                    ; 16-byte alignment for the call
  ; ... tick work (increment counter, maybe write to COM1) ...
  mov  rsp, rbp                                    ; back to the interrupt frame
  mov  al, 0x20 ; mov dx, 0x20 ; out dx, al        ; EOI to the master PIC
  pop  r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax
  add  rsp, 16                                     ; drop vector + error code
  iretq
```

(The alignment detail is why `rbp` appears: the interrupted `rsp` is not known
to be 16-byte aligned, so the handler must save and restore it around the
`call` into the runtime's `oskel_write`. See D7.)

The interrupt gate clears IF on entry (gate type 0x8E is an *interrupt* gate,
`src/native/bare_metal.cpp:27`), so the handler cannot be preempted by another
interrupt and needs no re-entrancy guard.

No TSS and no IST are configured (`gate[4] = 0x00`,
`src/native/bare_metal.cpp:254`), so the handler runs on the interrupted
stack. That is correct for a single-stack kernel and keeps the slice small; a
per-interrupt stack is later work, not this slice.

### D3 — The EOI order matters

The end-of-interrupt command must be written to the master PIC (port 0x20,
value 0x20) *after* the device work and *before* `iretq`. The EOI clears the
PIC's in-service bit for IRQ0; until it is written, the PIC will not raise
IRQ0 again. Writing it at the end (rather than at the top of the handler) keeps
the "one tick in flight at a time" guarantee explicit and leaves room for a
future handler that re-enables interrupts in the middle (nested interrupts),
where an early EOI would let a second tick re-enter before the first finished.
The design fixes the order: work, EOI, restore, `iretq`.

### D4 — Remap the PIC, then program the PIT, then `sti`

The prelude sequence, in this order (each step depends on the previous):

1. **PIC remap.** The standard ICW1-ICW4 dance to both 8259s: master to vector
   base 0x20, slave to 0x28, cascade identity, 8086/88 mode. The exact port
   values are listed in §5.
2. **PIT setup.** Channel 0, mode 3 (square wave), 16-bit divisor loaded
   low-byte-first, so IRQ0 fires at a fixed, known frequency.
3. **Unmask IRQ0.** Write the PIC interrupt mask register (port 0x21) with
   every bit set except bit 0, and the slave mask (port 0xA1) with every bit
   set. Only the timer is enabled; the rest of the devices stay quiet until a
   driver exists.
4. **Install the IDT** (already done today, `code.lidtRipLabel`,
   `src/native/bare_metal.cpp:384`).
5. **`sti`.** Enabling interrupts is the *last* step before the program runs,
   so no interrupt can arrive while the IDT or the PIC is half-configured.

`sti` sits immediately before `call programEntry`
(`src/native/bare_metal.cpp:386`).

### D5 — The tick counter lives in `.bss`, and `.bss` grows for it

The handler needs a place to count ticks, and the image has no writable data
segment today: `.bss` is page tables (`0x0000-0x2FFF`), a 16 KiB stack
(`0x3000-0x6FFF`) and the arena from `0x7000` up
(`src/native/bare_metal.cpp:63-67`). The arena starts exactly at the stack
top, so there is no free word.

Decision: reserve one 4 KiB **kernel-data page** at offset `0x3000`, move the
stack to `0x4000-0x7FFF` and the arena to `0x8000`. The tick counter is a
64-bit word at the start of that page. `kBMBssBytes`
(`include/native/bare_metal.h:54`) grows by `0x1000` and
`kBMStackTopOffset`/`kBMArenaOffset` (`src/native/bare_metal.cpp:66-67`) shift
with it. The structural test's `.bss` size assertion
(`tests/bare_metal_test.cpp:129`) is updated in the same commit, so the ELF
segment and the emitter never disagree.

### D6 — Two new `CodeGen` primitives, each 1:1

`outDxAl`/`inDxAl` already reach any PIC or PIT port (`out dx, al` /
`in al, dx`, `src/native/codegen.cpp:868-870`), so port I/O needs nothing new.
Two instructions are missing:

| Primitive | Encoding | Notes |
|-----------|----------|-------|
| `sti()` | `FB` | set interrupt flag |
| `iretq()` | `48 CF` | `REX.W` + `iret`; 64-bit interrupt return |

Both are a single instruction, so they keep the 1:1 budget the privileged
set already follows (`include/native/codegen.h:300-321`). Each gets a
byte-exact case in `tests/codegen_test.cpp` next to the existing `cli`/`hlt`
checks (`tests/codegen_test.cpp:91-97`).

### D7 — The handler's work is a counter, then a serial write

The tick handler increments the `.bss` counter. To make the tick observable
without a driver, the handler writes a fixed line to COM1 through the existing
`handles.write` thunk (`oskel_write`, `src/native/bare_metal.cpp:406`). The
write is the runtime's own polling loop, which is safe in interrupt context
because it touches only rax/rdx/r8/r9/r10 — all of which the handler saves
(D2).

`handles.write` is a plain `call`, so the handler must respect the System V
stack alignment at that call. The interrupted `rsp` is *not* known to be
16-byte aligned: an interrupt can land anywhere in a function, not only at a
`call` boundary. The handler therefore cannot fix alignment by counting bytes.
It saves the incoming `rsp`, forces alignment with `and rsp, ~0xF`, runs the
work, and restores the saved `rsp` before the `add rsp, 16` and the `iretq`.
That is the standard technique, and the design spells it out so the write path
never runs on a misaligned stack. (The write path uses no SSE, so a
misalignment would not fault today, but relying on that is the kind of latent
bug this design exists to avoid.)

## 5. Exact port values (reference)

PIC (8259), the standard remap:

| Port | Value | Meaning |
|------|-------|---------|
| `0x20` | `0x11` | master ICW1: init + ICW4 needed |
| `0xA0` | `0x11` | slave ICW1 |
| `0x21` | `0x20` | master ICW2: vector base 32 |
| `0xA1` | `0x28` | slave ICW2: vector base 40 |
| `0x21` | `0x04` | master ICW3: slave on IRQ2 |
| `0xA1` | `0x02` | slave ICW3: cascade identity 2 |
| `0x21` | `0x01` | master ICW4: 8086/88 mode |
| `0xA1` | `0x01` | slave ICW4 |
| `0x21` | `0xFE` | master OCW1: mask all but IRQ0 |
| `0xA1` | `0xFF` | slave OCW1: mask everything |

PIT (8254), channel 0:

| Port | Value | Meaning |
|------|-------|---------|
| `0x43` | `0x36` | channel 0, lobyte/hibyte, mode 3, binary |
| `0x40` | divisor low | `divisor = 1193182 / frequency` |
| `0x40` | divisor high | (low byte first) |

At 100 Hz the divisor is 11931 (`0x2E9B`), so the low/high bytes are `0x9B`
and `0x2E`.

EOI: `0x20` -> `0x20` (master, non-specific EOI).

## 6. Testing

Two levels, matching the existing split in `tests/bare_metal_test.cpp`.

**Structural (always runs, portable).** Extend `test_idt_is_present` or add a
sibling that scans the image for the new bytes:

- the `sti` byte (`FB`) and the `iretq` bytes (`48 CF`) are present;
- the PIC ICW1 bytes appear as `out 0x20` / `out 0xA0` sequences with `0x11`;
- the PIT command byte `0x36` is written to port `0x43`.

These pin the setup so a future edit cannot silently drop the remap or the
`sti`.

**End-to-end (QEMU, gated by `OSKEL_REQUIRE_QEMU`).** A new case boots a
program that spins forever:

```oskl
while (true) { }
```

The prelude's handler prints one line per tick and exits after three. The
test asserts the serial output is exactly `tick\ntick\ntick\n` and the exit
code is 0. The first two lines only appear if the handler returned twice, so
the case proves the `iretq` path, not just that the timer fired. It is
deterministic because the boot ends at a fixed tick count, not at a wall-clock
timeout.

The existing `test_qemu_boot`, `test_qemu_float_arithmetic` and
`test_qemu_fault_reports` must keep passing unchanged: a program that returns
before three ticks still exits through `oskel_exit`, and a fault still halts
with a trace. The fault tests also confirm D1 — vector 6 and vector 14 still
reach the fatal handler.

## 7. What this unlocks, and what it does not

Unlocks: a real interrupt *service* loop. The keyboard, a disk driver and a
round-robin scheduler all become possible once a handler can return.

Does not do: scheduling, context switching, per-interrupt stacks, the APIC
(the PIC is fine for a single-CPU educational kernel), nested interrupts,
`asm!` and `#[naked]`. Those are separate slices, and the ergonomics of
writing handlers in Oskel (rather than in the prelude) is a language-surface
question that stays out of this backend-only change.

## 8. Risks

- **The `sti` window.** Enabling interrupts before the program runs means any
  latent fault now arrives as an interrupt-time exception. Mitigation: the
  IDT is installed *before* `sti` (D4), so a fault is still reported, not a
  triple fault.
- **A wrong EOI.** A missing or misplaced EOI wedges IRQ0 after the first
  tick. Mitigation: the three-tick test fails loudly if the second tick never
  arrives, which is exactly the symptom.
- **`.bss` layout drift.** Moving the stack and the arena can break the
  structural test or the loader if the two are not updated together.
  Mitigation: the offset constants live in one place
  (`src/native/bare_metal.cpp:63-67`) and the test reads the same
  `kBMBssBytes` the emitter uses.
