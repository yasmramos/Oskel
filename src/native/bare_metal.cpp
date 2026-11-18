#include "native/bare_metal.h"

#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

// 16550 UART registers (COM1). The prelude drives the chip directly: a kernel
// has no `write` syscall, so the serial port is the whole output channel.
constexpr std::int64_t kSerialData = 0x3F8;  // transmit/receive holding
constexpr std::int64_t kSerialIer = 0x3F9;   // interrupt enable
constexpr std::int64_t kSerialFcr = 0x3FA;   // FIFO control
constexpr std::int64_t kSerialLcr = 0x3FB;   // line control (8N1 = 0x03)
constexpr std::int64_t kSerialLsr = 0x3FD;   // line status (bit 5 = THR empty)
constexpr std::uint8_t kSerialThrEmpty = 0x20;

// The interrupt descriptor table the prelude installs. It has one 16-byte
// gate per vector, so a CPU exception (#UD, #GP, #PF, ...) reaches our handler
// instead of escalating to a triple fault: with no IDT, a `#UD` is a double
// fault and then a reset -- the machine hangs with no trace. The gates point
// at one tiny stub per vector; each stub pushes the vector number and jumps to
// a handler that reports the vector and the faulting RIP over the serial port.
//
// There are two handlers behind those stubs. The *fault* handler (every vector
// but the timer) is fatal: it prints and halts, because a CPU exception means
// the runtime is in an unknown state and there is nothing safe to return to.
// The *timer* handler (IRQ0, remapped to vector 0x20) is not fatal: it
// acknowledges the 8259 PIC and returns to the interrupted code with `iretq`.
// Keeping them apart is deliberate -- a device interrupt must never be
// confused with a fault, so a `#PF` still halts with a trace.
constexpr std::uint64_t kIdtGateCount = 256;
constexpr std::uint64_t kIdtGateBytes = 16;
constexpr std::uint8_t kIdtGateType = 0x8E;  // present, DPL 0, 64-bit interrupt gate
constexpr std::uint16_t kIdtLimit =
    static_cast<std::uint16_t>(kIdtGateCount * kIdtGateBytes - 1);

// The 8259 PIC and the 8254 PIT: the two chips the prelude programs to get a
// periodic timer interrupt. Neither is remapped or programmed by the firmware
// in a way a kernel can rely on, so the prelude does it itself before enabling
// interrupts.
//
// The PIC powers up mapping IRQ0-IRQ7 onto vectors 0x08-0x0F, which collide
// with the CPU's own exceptions (#DF at 8, #TS at 10, ...), so it is remapped
// to 0x20-0x2F with the standard ICW1-ICW4 sequence. The PIT then drives IRQ0
// at a fixed frequency, and only IRQ0 is unmasked: every other device stays
// quiet until it has a driver.
constexpr std::int64_t kPicMasterCmd = 0x20;   // master: command (ICW1, OCW2)
constexpr std::int64_t kPicMasterData = 0x21;  // master: data (ICW2-4, OCW1)
constexpr std::int64_t kPicSlaveCmd = 0xA0;
constexpr std::int64_t kPicSlaveData = 0xA1;
constexpr std::uint8_t kPicIcw1Init = 0x11;             // init + ICW4 needed
constexpr std::uint8_t kPicVectorBaseMaster = 0x20;     // IRQ0 -> vector 0x20
constexpr std::uint8_t kPicVectorBaseSlave = 0x28;      // IRQ8 -> vector 0x28
constexpr std::uint8_t kPicIcw3Master = 0x04;           // slave cascaded on IRQ2
constexpr std::uint8_t kPicIcw3Slave = 0x02;            // cascade identity 2
constexpr std::uint8_t kPicIcw4Mode8086 = 0x01;         // 8086/88 mode
constexpr std::uint8_t kPicMaskIrq0Irq1 = 0xFC;         // all masked but IRQ0+IRQ1
constexpr std::uint8_t kPicMaskAll = 0xFF;
constexpr std::uint8_t kPicEoi = 0x20;                  // non-specific EOI

constexpr std::int64_t kPitChannel0 = 0x40;             // channel 0 data port
constexpr std::int64_t kPitCommand = 0x43;              // mode/command register
constexpr std::uint8_t kPitMode3Channel0 = 0x36;        // ch0, lobyte/hibyte, mode 3
constexpr std::uint16_t kPitDivisor = 11931;            // 1193182 / 100 Hz

// The PS/2 keyboard: the second device with a driver, after the timer. The
// i8042 controller raises IRQ1 when a scancode is ready at the data port, and
// the handler translates scancode-set-1 make codes to ASCII and appends them
// to a ring buffer in the kernel-data page. A program reads them through the
// `input` builtin, which the prelude's `oskel_read` thunk serves.
//
// The vector is IRQ1 after the PIC remap, and the two i8042 ports are the
// status/command register (0x64) and the data register (0x60).
constexpr std::int32_t kKeyboardVector = 0x21;  // IRQ1 after the PIC remap
constexpr std::int64_t kKeyboardData = 0x60;    // i8042 data port (scancode)
constexpr std::uint8_t kScanLeftShift = 0x2A;   // set-1 make code
constexpr std::uint8_t kScanRightShift = 0x36;
constexpr std::uint8_t kScanLeftShiftUp = 0xAA;  // set-1 break code
constexpr std::uint8_t kScanRightShiftUp = 0xB6;
constexpr std::uint8_t kScanBreakBit = 0x80;     // set on every break code

// Scancode-set-1 make codes to ASCII, unshifted and shifted. Index 0 is unused
// (no scancode produces it), an entry of 0 means "no character" (a modifier, a
// function key or a key the driver ignores), and 0x0D is Enter. The two tables
// are emitted as bytes in the text section and indexed by the raw scancode the
// controller delivers; a key with no entry stays unmapped until a later slice
// needs it.
constexpr std::uint8_t kKbNoChar = 0x00;
constexpr std::uint8_t kKbEnter = '\n';  // the line terminator `input` consumes
constexpr std::uint8_t kKbBackspace = 0x08;
constexpr std::uint8_t kKbTab = 0x09;

constexpr std::uint8_t kKbUnshifted[128] = {
    kKbNoChar, kKbNoChar, '1', '2', '3', '4', '5', '6',
    '7', '8', '9', '0', '-', '=', kKbBackspace, kKbTab,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    'o', 'p', '[', ']', kKbEnter, kKbNoChar,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k',
    'l', ';', '\'', '`', kKbNoChar, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',',
    '.', '/', kKbNoChar, kKbNoChar, kKbNoChar, ' ',
    // 0x3A..0x7F: caps lock, function keys, keypad, arrows -- unmapped.
};

constexpr std::uint8_t kKbShifted[128] = {
    kKbNoChar, kKbNoChar, '!', '@', '#', '$', '%', '^',
    '&', '*', '(', ')', '_', '+', kKbBackspace, kKbTab,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{', '}', kKbEnter, kKbNoChar,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K',
    'L', ':', '"', '~', kKbNoChar, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<',
    '>', '?', kKbNoChar, kKbNoChar, kKbNoChar, ' ',
    // 0x3A..0x7F: caps lock, function keys, keypad, arrows -- unmapped.
};

// The vector IRQ0 lands on after the remap, and the tick ceiling that stops a
// program which never returns. The bare-metal prelude has no scheduler to hand
// control to, so a `while (true)` would spin forever with nothing to preempt it
// and no way for a test harness to observe it; after `kTickLimit` ticks the
// kernel prints one line and exits.
//
// The handler is *silent* on every tick before the ceiling. Printing per tick
// was the first idea, and it is wrong: a normal program already spans at least
// one tick (the PIT is wall-clock driven and the prelude enables interrupts
// just before the program runs), so a per-tick line would interleave with the
// program's own output and break every boot test that checks stdout byte for
// byte. The tick count is therefore kernel state, and the *ceiling* is the
// observable.
//
// The ceiling is a watchdog, not a limit on normal programs: it is set at five
// seconds of guest time (500 ticks at 100 Hz), far above every program in the
// test suite (the boot tests finish in well under a second), so a program that
// returns on its own never reaches it and keeps byte-identical stdout. Reaching
// the ceiling also proves the return path: the count only advances if every
// earlier tick's handler `iretq`d back to the program.
constexpr std::int32_t kTimerVector = 0x20;
constexpr std::int32_t kTickLimit = 500;
// A distinctive exit code, so the tick watchdog's stop cannot be confused with
// a program returning that value on its own.
constexpr std::int32_t kTickExitCode = 0x77;
constexpr char kTickTemplate[] = "tick limit\n";
constexpr std::int64_t kTickBytes = sizeof(kTickTemplate) - 1;

// Vectors for which the CPU pushes an error code on the interrupt frame
// (Intel SDM, "Exception and Interrupt Reference"): #DF, #TS, #NP, #SS, #GP,
// #PF, #AC, #CP, #VC, #SX. Every other vector does not, so its stub pushes a
// dummy zero first and the frame layout is the same for both -- [rsp] = vector,
// [rsp+8] = error code, [rsp+16] = faulting RIP.
bool vectorPushesErrorCode(int vector) {
  switch (vector) {
    case 8: case 10: case 11: case 12: case 13: case 14:
    case 17: case 21: case 29: case 30:
      return true;
    default:
      return false;
  }
}

// The message the common fault handler prints and the stack buffer it builds
// it in. The template carries placeholder hex digits the handler overwrites:
// two for the vector and sixteen for the faulting RIP.
constexpr char kFaultTemplate[] =
    "oskel: cpu exception 0x00 rip=0x0000000000000000\n";
constexpr std::int64_t kFaultTemplateLen = sizeof(kFaultTemplate) - 1;
constexpr std::int64_t kFaultVectorHexOffset = 23;  // right after "exception 0x"
constexpr std::int64_t kFaultRipHexOffset = 32;     // right after " rip=0x"
constexpr std::int32_t kFaultMessageBytes = 64;
// The fault handler publishes this on QEMU's isa-debug-exit port (the same
// port `oskel_exit` uses) so a test harness sees a deterministic, fast exit
// instead of spinning in `hlt` forever. On real hardware nothing decodes the
// port and the `hlt` loop is the end.
constexpr std::uint8_t kFaultExitCode = 0xFD;

// .bss layout, all relative to kBMDataBase: the page tables first, then a
// page of kernel data (the tick counter the timer handler owns), the boot stack
// (which grows down toward them) and the arena at the top.
constexpr std::uint64_t kBMPml4Offset = 0x0000;
constexpr std::uint64_t kBMPdptOffset = 0x1000;
constexpr std::uint64_t kBMPdOffset = 0x2000;
constexpr std::uint64_t kBMKernelDataOffset = 0x3000;  // 4 KiB of kernel state
constexpr std::uint64_t kBMStackTopOffset = 0x8000;    // 16 KiB stack, top address
constexpr std::uint64_t kBMArenaOffset = 0x8000;       // arena sits right above

// The tick counter, the first word of the kernel-data page: the timer handler
// increments it and exits once it reaches `kTickLimit`. A `.bss` word is the
// only writable state the image has (there is no data segment), and the loader
// zeroes it.
constexpr std::uint64_t kBMKernelDataTicks = kBMKernelDataOffset;

// The keyboard ring buffer, later in the same kernel-data page. The IRQ1
// handler is the producer (`head`), the `oskel_read` thunk the consumer
// (`tail`), so a keystroke typed while the program is busy is queued instead of
// lost. The buffer is a byte array of `kKbBufferSize` slots; `head`/`tail`
// wrap modulo that size, and the buffer is empty when the two are equal.
// Keeping all of this in the zeroed `.bss` page means no data segment and no
// relocation.
constexpr std::uint64_t kKbHeadOffset = kBMKernelDataOffset + 0x100;
constexpr std::uint64_t kKbTailOffset = kBMKernelDataOffset + 0x108;
constexpr std::uint64_t kKbBufferOffset = kBMKernelDataOffset + 0x110;
constexpr std::uint64_t kKbBufferSize = 256;
// The shift state the IRQ1 handler maintains: 0 with no shift key held, 1
// while either shift is down. A single byte, so a scancode table lookup can
// select between the unshifted and shifted maps.
constexpr std::uint64_t kKbShiftStateOffset =
    kBMKernelDataOffset + 0x110 + kKbBufferSize;

// Fixed offsets, relative to the trampoline's first byte, of the pieces the
// trampoline addresses by absolute address. They are a property of the
// instruction sequence below (its length), not of where the preamble lands, so
// they hold for any `entryOffset`.
constexpr std::uint64_t kTrampolineLength = 120;    // cli .. ljmp + padding
constexpr std::uint64_t kTrampolineGdtRel = 0x78;   // GDT, right after the ljmp
constexpr std::uint64_t kTrampolineGdtPtrRel = 0x90;  // GDT descriptor record
constexpr std::uint64_t kTrampolineLongModeRel = 0x96;
constexpr std::uint64_t kTrampolineGdtBytes = 24;   // null + code64 + data
constexpr std::uint64_t kTrampolineGdtPtrBytes = 6;  // limit(2) + base(4)

// The 32-bit trampoline, from `cli` to the `ljmp` into long mode, with the two
// absolute address fields (the `lgdt` operand and the `ljmp` target) left zero
// here and patched by `emitBareMetalPreamble`, which knows the addresses.
//
// It is hand-assembled on purpose: this is the one part of the image that runs
// in 32-bit protected mode, before the shared 64-bit runtime exists, so it
// cannot use the `CodeGen` helpers (they emit REX.W 64-bit forms). The bytes
// are the exact encoding of the sequence in the comment below; the two zero
// dwords are the patch points.
//
//   cli                                  ; no interrupts during setup
//   mov eax, PDPT|3; mov [PML4], eax      ; PML4[0] -> PDPT, present+rw
//   mov eax, PD|3;   mov [PDPT], eax      ; PDPT[0] -> PD, present+rw
//   mov edi, PD; mov edx, 0x83; mov ecx, 512
//   loop: mov [edi], edx                  ; 2 MiB page, present+rw+ps
//         mov dword [edi+4], 0
//         add edx, 0x200000; add edi, 8; loop loop
//   mov eax, PML4; mov cr3, eax           ; point the TLB at the PML4
//   mov eax, cr4; or eax, 0x620; mov cr4, eax  ; PAE | OSFXSR | OSXMMEXCPT
//   mov ecx, 0xC0000080; rdmsr; or eax, 0x100; wrmsr  ; EFER.LME
//   mov eax, cr0; or eax, 0x80000001; mov cr0, eax    ; PG | PE
//   lgdt [gdt_ptr]                        ; our own GDT (see below)
//   ljmp 0x08:long_mode                   ; the 64-bit code selector
constexpr std::uint8_t kTrampoline[] = {
    0xFA,
    0xB8, 0x03, 0x10, 0x20, 0x00, 0xA3, 0x00, 0x00, 0x20, 0x00,
    0xB8, 0x03, 0x20, 0x20, 0x00, 0xA3, 0x00, 0x10, 0x20, 0x00,
    0xBF, 0x00, 0x20, 0x20, 0x00, 0xBA, 0x83, 0x00, 0x00, 0x00,
    0xB9, 0x00, 0x02, 0x00, 0x00,
    0x89, 0x17,
    0xC7, 0x47, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x81, 0xC2, 0x00, 0x00, 0x20, 0x00,
    0x83, 0xC7, 0x08,
    0xE2, 0xEC,
    0xB8, 0x00, 0x00, 0x20, 0x00,
    0x0F, 0x22, 0xD8,
    0x0F, 0x20, 0xE0, 0x0D, 0x20, 0x06, 0x00, 0x00, 0x0F, 0x22, 0xE0,
    0xB9, 0x80, 0x00, 0x00, 0xC0,
    0x0F, 0x32, 0x0D, 0x00, 0x01, 0x00, 0x00, 0x0F, 0x30,
    0x0F, 0x20, 0xC0, 0x0D, 0x01, 0x00, 0x00, 0x80, 0x0F, 0x22, 0xC0,
    0x0F, 0x01, 0x15, 0x00, 0x00, 0x00, 0x00,  // lgdt [gdt_ptr] (patched)
    0xEA, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,  // ljmp 0x08:long_mode (patched)
    // Padding so the trampoline ends on an 8-byte boundary and the GDT lands
    // at `kTrampolineGdtRel` with no gap. Never executed: the `ljmp` above
    // leaves 32-bit mode.
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Byte offsets of the two patched dwords inside `kTrampoline`.
constexpr std::size_t kTrampolineLgdtDisp = 0x67;   // lgdt's absolute operand
constexpr std::size_t kTrampolineLjmpTarget = 0x6C;  // ljmp's 32-bit offset

void write32At(std::vector<std::uint8_t>& out, std::size_t off, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
  }
}

// The ELF32 header's 16-bit fields (`e_ehsize`..`e_shstrndx`) must be written
// as two bytes: `write32At` would spill into the next field, and past the end
// of the header for the last one.
void write16At(std::vector<std::uint8_t>& out, std::size_t off, std::uint16_t v) {
  out[off] = static_cast<std::uint8_t>(v & 0xFF);
  out[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

}  // namespace

// Emits a branch-free-in-spirit sequence that writes `digits` hexadecimal
// digits of `value` (most significant first) into `[base + offset]`. The fault
// handler uses it to render the vector number and the faulting RIP. `value`
// and `base` survive; rax is clobbered.
static void emitHexDigits(CodeGen& code, X64Reg value, X64Reg base, std::int64_t offset,
                   int digits) {
  for (int i = 0; i < digits; ++i) {
    const int shift = (digits - 1 - i) * 4;
    CodeGen::Label letter = code.newLabel();
    CodeGen::Label store = code.newLabel();
    code.movReg(X64Reg::Rax, value);
    if (shift != 0) code.shrImm8(X64Reg::Rax, static_cast<std::uint8_t>(shift));
    code.andImm32(X64Reg::Rax, 0xF);
    code.cmpImm32(X64Reg::Rax, 9);
    code.jcc(Jcc::A, letter);            // unsigned above 9 -> a-f
    code.addImm32(X64Reg::Rax, '0');
    code.jmp(store);
    code.bind(letter);
    code.addImm32(X64Reg::Rax, 'a' - 10);
    code.bind(store);
    code.movByteToMem(base, offset + i, X64Reg::Rax);
  }
}

// Emits the interrupt descriptor table, one stub per vector, the fatal fault
// handler and the returning timer handler, and binds `descriptorLabel` at the
// 10-byte pseudo-descriptor the entry point loads with `lidt`.
//
// A CPU exception is not maskable by `cli`, so the only defence against a
// silent triple fault is an IDT: without one, the first `#UD` (say an SSE
// instruction before CR4.OSFXSR is set) escalates to a double fault and then a
// reset with nothing printed. Every gate points at a stub that pushes the
// vector number -- preceded by a dummy error code when the CPU did not push
// one -- so the frame the handler sees is always `[rsp]` = vector, `[rsp+8]` =
// error code, `[rsp+16]` = faulting RIP.
//
// The stubs dispatch by vector. The timer (IRQ0, remapped to `kTimerVector`)
// reaches the *returning* handler: it counts the tick and acknowledges the PIC
// and `iretq`s -- silently, because a per-tick print would interleave with the
// program's own output (see `kTickLimit`) -- until the tick ceiling, where a
// program that never returned is stopped with one line and a dedicated exit
// code, since a bare-metal program has no scheduler to preempt it. Every other
// vector reaches the *fatal* handler, which prints `oskel: cpu exception 0xVV
// rip=0x...` and halts: a fault means the runtime is in an unknown state and
// there is nothing safe to return to.
static void emitIdt(CodeGen& code, const RuntimeHandles& handles,
             CodeGen::Label descriptorLabel) {
  std::vector<CodeGen::Label> stubs(kIdtGateCount);
  for (std::uint64_t v = 0; v < kIdtGateCount; ++v) stubs[v] = code.newLabel();
  CodeGen::Label faultHandler = code.newLabel();
  CodeGen::Label tickHandler = code.newLabel();
  CodeGen::Label keyboardHandler = code.newLabel();
  CodeGen::Label tickDone = code.newLabel();
  CodeGen::Label faultTemplateBytes = code.newLabel();
  CodeGen::Label tickBytes = code.newLabel();
  CodeGen::Label scancodeUnshifted = code.newLabel();
  CodeGen::Label scancodeShifted = code.newLabel();

  const std::int64_t stackTop =
      static_cast<std::int64_t>(kBMDataBase + kBMStackTopOffset);

  // --- the common fault handler ---
  // Read the vector and the faulting RIP off the interrupt frame, then switch
  // to the top of the boot stack: the fault may have happened on a stack that
  // is itself broken (a stack overflow is a `#PF`/`#DF`), and the handler
  // needs room to build its message.
  code.bind(faultHandler);
  code.movFromMem(X64Reg::R8, X64Reg::Rsp, 0);   // vector
  code.movFromMem(X64Reg::R9, X64Reg::Rsp, 16);  // faulting RIP
  code.movImm(X64Reg::Rsp, stackTop);
  code.subRegImm32(X64Reg::Rsp, kFaultMessageBytes);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);  // message buffer base

  code.leaRipLabel(X64Reg::Rsi, faultTemplateBytes);
  code.movReg(X64Reg::Rdi, X64Reg::Rbp);
  code.movImm(X64Reg::Rcx, kFaultTemplateLen);
  code.cld();
  code.repMovsb();

  emitHexDigits(code, X64Reg::R9, X64Reg::Rbp, kFaultRipHexOffset, 16);
  emitHexDigits(code, X64Reg::R8, X64Reg::Rbp, kFaultVectorHexOffset, 2);

  code.movImm(X64Reg::Rdi, 1);  // fd, ignored: a kernel prints to COM1
  code.movReg(X64Reg::Rsi, X64Reg::Rbp);
  code.movImm(X64Reg::Rdx, kFaultTemplateLen);
  code.callLabel(handles.write);

  CodeGen::Label spin = code.newLabel();
  code.cli();
  code.movImm(X64Reg::Rax, kFaultExitCode);
  code.movImm(X64Reg::Rdx, 0x501);  // QEMU isa-debug-exit; ignored on real hardware
  code.outDxAl();
  code.bind(spin);
  code.hlt();
  code.jmp(spin);

  // --- the timer handler (IRQ0): count, (silently) acknowledge, return ---
  // Save every register the handler touches. rbp is included because the
  // interrupted code may be using it as a frame pointer (every lowered frame
  // does); it doubles as the saved-rsp slot below. An interrupt gate has
  // already cleared IF, so the handler cannot be re-entered.
  code.bind(tickHandler);
  code.push(X64Reg::Rax);
  code.push(X64Reg::Rcx);
  code.push(X64Reg::Rdx);
  code.push(X64Reg::Rsi);
  code.push(X64Reg::Rdi);
  code.push(X64Reg::Rbp);
  code.push(X64Reg::R8);
  code.push(X64Reg::R9);
  code.push(X64Reg::R10);
  code.push(X64Reg::R11);
  // The interrupted rsp is not known to be 16-byte aligned -- an interrupt can
  // land anywhere, not only at a `call` boundary -- so save it and force the
  // alignment the System V ABI wants at the `call` into `oskel_write`.
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.andImm32(X64Reg::Rsp, -16);

  // ticks++ (r10 = &ticks, r11 = the new value)
  code.movImm(X64Reg::R10, static_cast<std::int64_t>(kBMDataBase + kBMKernelDataTicks));
  code.movFromMem(X64Reg::R11, X64Reg::R10, 0);
  code.addImm32(X64Reg::R11, 1);
  code.movToMem(X64Reg::R10, 0, X64Reg::R11);

  // After kTickLimit ticks there is nothing to return to, so stop. The demo
  // ends here; a real kernel would reschedule instead.
  code.cmpImm32(X64Reg::R11, kTickLimit);
  code.jcc(Jcc::Ge, tickDone);

  // End of interrupt: acknowledge the PIC, restore, `iretq`. No print here:
  // see `kTickLimit` for why a per-tick line would corrupt a program's stdout.
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.movImm(X64Reg::Rax, kPicEoi);
  code.movImm(X64Reg::Rdx, kPicMasterCmd);
  code.outDxAl();
  code.pop(X64Reg::R11);
  code.pop(X64Reg::R10);
  code.pop(X64Reg::R9);
  code.pop(X64Reg::R8);
  code.pop(X64Reg::Rbp);
  code.pop(X64Reg::Rdi);
  code.pop(X64Reg::Rsi);
  code.pop(X64Reg::Rdx);
  code.pop(X64Reg::Rcx);
  code.pop(X64Reg::Rax);
  code.addImm32(X64Reg::Rsp, 16);  // drop the stub's vector + error code
  code.iretq();

  // The ceiling: a program that never returned has run for kTickLimit ticks.
  // Print the one line that proves the timer fired and returned this far, then
  // stop with a dedicated exit code.
  code.bind(tickDone);
  code.movImm(X64Reg::Rdi, 1);  // fd, ignored
  code.leaRipLabel(X64Reg::Rsi, tickBytes);
  code.movImm(X64Reg::Rdx, kTickBytes);
  code.callLabel(handles.write);
  code.movImm(X64Reg::Rdi, kTickExitCode);
  code.callLabel(handles.exit);
  code.ud2();  // oskel_exit never returns

  // --- the keyboard handler (IRQ1): translate, buffer, acknowledge, return ---
  // The i8042 raises IRQ1 when a scancode is ready at port 0x60. The handler
  // reads it, tracks the shift keys, translates the make code to ASCII through
  // one of the two tables and appends the character to the ring buffer in the
  // kernel-data page; the `oskel_read` thunk drains that buffer for `input`.
  // Like the timer handler it saves every register it touches, acknowledges the
  // PIC and `iretq`s. A break code (bit 7 set) only updates the shift state.
  code.bind(keyboardHandler);
  code.push(X64Reg::Rax);
  code.push(X64Reg::Rcx);
  code.push(X64Reg::Rdx);
  code.push(X64Reg::Rsi);
  code.push(X64Reg::Rdi);
  code.push(X64Reg::Rbp);
  code.push(X64Reg::R8);
  code.push(X64Reg::R9);
  code.push(X64Reg::R10);
  code.push(X64Reg::R11);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);

  // Read the scancode into r8 (zero-extended).
  code.movImm(X64Reg::Rdx, kKeyboardData);
  code.inDxAl();
  code.movzx(X64Reg::R8, X64Reg::Rax);

  // Break code (bit 7 set): a key release. Only the shifts matter, and only to
  // clear the shift state.
  CodeGen::Label kbEoi = code.newLabel();
  CodeGen::Label kbRelease = code.newLabel();
  CodeGen::Label kbShiftUp = code.newLabel();
  code.testAlImm8(kScanBreakBit);
  code.jcc(Jcc::Ne, kbRelease);

  // Make code: both shift keys set the shift state.
  CodeGen::Label kbShiftsOn = code.newLabel();
  CodeGen::Label kbNeedShifted = code.newLabel();
  CodeGen::Label kbHaveBase = code.newLabel();
  code.cmpImm32(X64Reg::R8, kScanLeftShift);
  code.jcc(Jcc::Eq, kbShiftsOn);
  code.cmpImm32(X64Reg::R8, kScanRightShift);
  code.jcc(Jcc::Eq, kbShiftsOn);

  // Translate: pick the table by shift state, then index it by the scancode.
  // CodeGen has no indexed memory operand, so the table base is added to the
  // scancode first and the byte is loaded from [base + 0].
  code.movImm(X64Reg::R10, static_cast<std::int64_t>(kBMDataBase + kKbShiftStateOffset));
  code.movzxFromMem(X64Reg::R9, X64Reg::R10, 0);  // shift state
  code.testReg(X64Reg::R9);
  code.jcc(Jcc::Ne, kbNeedShifted);
  code.leaRipLabel(X64Reg::R10, scancodeUnshifted);
  code.jmp(kbHaveBase);
  code.bind(kbNeedShifted);
  code.leaRipLabel(X64Reg::R10, scancodeShifted);
  code.bind(kbHaveBase);
  code.addReg(X64Reg::R10, X64Reg::R8);            // &table[scancode]
  code.movzxFromMem(X64Reg::Rax, X64Reg::R10, 0);  // ASCII, 0 if unmapped
  code.testReg(X64Reg::Rax);
  code.jcc(Jcc::Eq, kbEoi);                        // modifier/ignored key

  // Append: buffer[head] = ascii; head = (head + 1) mod kKbBufferSize.
  code.movImm(X64Reg::R10, static_cast<std::int64_t>(kBMDataBase + kKbHeadOffset));
  code.movFromMem(X64Reg::R9, X64Reg::R10, 0);    // head
  code.movImm(X64Reg::R11, static_cast<std::int64_t>(kBMDataBase + kKbBufferOffset));
  code.addReg(X64Reg::R11, X64Reg::R9);           // &buffer[head]
  code.movByteToMem(X64Reg::R11, 0, X64Reg::Rax);
  code.addImm32(X64Reg::R9, 1);
  code.andImm32(X64Reg::R9, static_cast<std::int32_t>(kKbBufferSize - 1));
  code.movToMem(X64Reg::R10, 0, X64Reg::R9);
  code.jmp(kbEoi);

  code.bind(kbShiftsOn);
  code.movImm(X64Reg::R10, static_cast<std::int64_t>(kBMDataBase + kKbShiftStateOffset));
  code.movImmByteToMem(X64Reg::R10, 0, 1);
  code.jmp(kbEoi);

  code.bind(kbRelease);
  code.cmpImm32(X64Reg::R8, kScanLeftShiftUp);
  code.jcc(Jcc::Eq, kbShiftUp);
  code.cmpImm32(X64Reg::R8, kScanRightShiftUp);
  code.jcc(Jcc::Ne, kbEoi);
  code.bind(kbShiftUp);
  code.movImm(X64Reg::R10, static_cast<std::int64_t>(kBMDataBase + kKbShiftStateOffset));
  code.movImmByteToMem(X64Reg::R10, 0, 0);

  // End of interrupt: acknowledge the PIC, restore, `iretq` -- the same tail as
  // the timer handler.
  code.bind(kbEoi);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.movImm(X64Reg::Rax, kPicEoi);
  code.movImm(X64Reg::Rdx, kPicMasterCmd);
  code.outDxAl();
  code.pop(X64Reg::R11);
  code.pop(X64Reg::R10);
  code.pop(X64Reg::R9);
  code.pop(X64Reg::R8);
  code.pop(X64Reg::Rbp);
  code.pop(X64Reg::Rdi);
  code.pop(X64Reg::Rsi);
  code.pop(X64Reg::Rdx);
  code.pop(X64Reg::Rcx);
  code.pop(X64Reg::Rax);
  code.addImm32(X64Reg::Rsp, 16);  // drop the stub's vector + error code
  code.iretq();

  // --- one stub per vector ---
  // Dispatch by vector: the timer (IRQ0) and the keyboard (IRQ1) reach their
  // returning handlers, every other vector the fatal fault handler.
  for (std::uint64_t v = 0; v < kIdtGateCount; ++v) {
    code.bind(stubs[v]);
    if (!vectorPushesErrorCode(static_cast<int>(v))) code.pushImm8(0);
    code.pushImm8(static_cast<std::int32_t>(v));
    if (v == static_cast<std::uint64_t>(kTimerVector)) {
      code.jmp(tickHandler);
    } else if (v == static_cast<std::uint64_t>(kKeyboardVector)) {
      code.jmp(keyboardHandler);
    } else {
      code.jmp(faultHandler);
    }
  }

  // --- the message templates ---
  code.bind(faultTemplateBytes);
  code.emitBytes(reinterpret_cast<const std::uint8_t*>(kFaultTemplate),
                 static_cast<std::size_t>(kFaultTemplateLen));
  code.bind(tickBytes);
  code.emitBytes(reinterpret_cast<const std::uint8_t*>(kTickTemplate),
                 static_cast<std::size_t>(kTickBytes));

  // --- the scancode translation tables ---
  code.bind(scancodeUnshifted);
  code.emitBytes(kKbUnshifted, sizeof kKbUnshifted);
  code.bind(scancodeShifted);
  code.emitBytes(kKbShifted, sizeof kKbShifted);

  // --- the gate table ---
  const std::uint8_t zero = 0x00;
  while (code.offset() % 16 != 0) code.emitBytes(&zero, 1);
  const std::uint64_t idtOffset = code.offset();
  for (std::uint64_t v = 0; v < kIdtGateCount; ++v) {
    const std::uint64_t stub = kBMCodeBase + code.labelOffset(stubs[v]);
    std::uint8_t gate[kIdtGateBytes] = {0};
    gate[0] = static_cast<std::uint8_t>(stub & 0xFF);
    gate[1] = static_cast<std::uint8_t>((stub >> 8) & 0xFF);
    gate[2] = 0x08;  // code selector (the 64-bit code segment)
    gate[3] = 0x00;
    gate[4] = 0x00;  // no interrupt stack table
    gate[5] = kIdtGateType;
    gate[6] = static_cast<std::uint8_t>((stub >> 16) & 0xFF);
    gate[7] = static_cast<std::uint8_t>((stub >> 24) & 0xFF);
    gate[8] = static_cast<std::uint8_t>((stub >> 32) & 0xFF);
    gate[9] = static_cast<std::uint8_t>((stub >> 40) & 0xFF);
    gate[10] = static_cast<std::uint8_t>((stub >> 48) & 0xFF);
    gate[11] = static_cast<std::uint8_t>((stub >> 56) & 0xFF);
    code.emitBytes(gate, kIdtGateBytes);
  }

  // --- the pseudo-descriptor (limit + base) ---
  code.bind(descriptorLabel);
  const std::uint64_t idtBase = kBMCodeBase + idtOffset;
  std::uint8_t descriptor[10];
  descriptor[0] = static_cast<std::uint8_t>(kIdtLimit & 0xFF);
  descriptor[1] = static_cast<std::uint8_t>((kIdtLimit >> 8) & 0xFF);
  for (int i = 0; i < 8; ++i) {
    descriptor[2 + i] = static_cast<std::uint8_t>((idtBase >> (8 * i)) & 0xFF);
  }
  code.emitBytes(descriptor, sizeof descriptor);
}

std::uint64_t emitBareMetalPreamble(CodeGen& code, const RuntimeHandles& handles,
                                    CodeGen::Label programEntry) {
  if (code.offset() != 0) {
    throw std::runtime_error("native/bare_metal: the multiboot header must be first");
  }

  // The IDT pseudo-descriptor is a forward reference from the entry point's
  // `lidt`, so the label is created before anything is emitted.
  const CodeGen::Label idtDescriptor = code.newLabel();

  // --- Multiboot 1 header (12 bytes, 4-byte aligned, first 8 KiB of the file) ---
  const std::uint32_t magic = kMultibootMagic;
  const std::uint32_t flags = kMultibootFlags;
  const std::uint32_t checksum = 0u - (magic + flags);
  const std::uint8_t header[12] = {
      static_cast<std::uint8_t>(magic),    static_cast<std::uint8_t>(magic >> 8),
      static_cast<std::uint8_t>(magic >> 16), static_cast<std::uint8_t>(magic >> 24),
      static_cast<std::uint8_t>(flags),    static_cast<std::uint8_t>(flags >> 8),
      static_cast<std::uint8_t>(flags >> 16), static_cast<std::uint8_t>(flags >> 24),
      static_cast<std::uint8_t>(checksum), static_cast<std::uint8_t>(checksum >> 8),
      static_cast<std::uint8_t>(checksum >> 16), static_cast<std::uint8_t>(checksum >> 24),
  };
  code.emitBytes(header, sizeof header);

  // The 64-bit `ljmp` target and the GDT must be 8-byte aligned; the header is
  // 12 bytes, so pad to the next multiple of 16 to leave room for both.
  const std::uint8_t zero = 0x00;
  while (code.offset() % 16 != 0) code.emitBytes(&zero, 1);

  const std::uint64_t entryOffset = code.offset();  // _start

  // --- The 32-bit trampoline, with its two absolute fields patched ---
  std::uint8_t trampoline[kTrampolineLength];
  std::memcpy(trampoline, kTrampoline, kTrampolineLength);
  // The `lgdt` operand is the address of the GDT *descriptor* record
  // (`gdt_ptr`, limit + base), not of the descriptors themselves.
  const std::uint32_t gdtPtrAddress =
      static_cast<std::uint32_t>(kBMCodeBase + entryOffset + kTrampolineGdtPtrRel);
  const std::uint32_t longModeAddress =
      static_cast<std::uint32_t>(kBMCodeBase + entryOffset + kTrampolineLongModeRel);
  for (int i = 0; i < 4; ++i) {
    trampoline[kTrampolineLgdtDisp + i] =
        static_cast<std::uint8_t>((gdtPtrAddress >> (8 * i)) & 0xFF);
    trampoline[kTrampolineLjmpTarget + i] =
        static_cast<std::uint8_t>((longModeAddress >> (8 * i)) & 0xFF);
  }
  code.emitBytes(trampoline, kTrampolineLength);

  // --- The GDT: a null descriptor, a 64-bit code segment and a data segment ---
  // The trampoline's `lgdt` operand points at `gdt_ptr` (limit + base), and the
  // `ljmp` selector 0x08 is the code descriptor. Multiboot leaves its own GDT
  // in place, but its code descriptor is 32-bit, so a 64-bit far jump needs a
  // GDT of our own.
  const std::uint32_t gdtAddress =
      static_cast<std::uint32_t>(kBMCodeBase + entryOffset + kTrampolineGdtRel);
  while (code.offset() % 8 != 0) code.emitBytes(&zero, 1);
  const std::uint8_t gdt[kTrampolineGdtBytes] = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // null
      0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xAF, 0x00,  // code64, ring 0
      0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0xAF, 0x00,  // data, ring 0
  };
  code.emitBytes(gdt, sizeof gdt);
  const std::uint8_t gdtPtr[kTrampolineGdtPtrBytes] = {
      0x17, 0x00,  // limit = 24 - 1
      static_cast<std::uint8_t>(gdtAddress),       static_cast<std::uint8_t>(gdtAddress >> 8),
      static_cast<std::uint8_t>(gdtAddress >> 16), static_cast<std::uint8_t>(gdtAddress >> 24),
  };
  code.emitBytes(gdtPtr, sizeof gdtPtr);

  // --- The 64-bit entry point ---
  // Long mode is on: point rsp at the top of the `.bss` stack, run the program
  // through the shared chunk entry, and hand its result to the runtime's exit
  // thunk (a `cli; hlt` loop -- there is no operating system to return to).
  // `mov esp, imm32` zero-extends into rsp, so the low stack address is enough.
  {
    const std::uint32_t stackTop =
        static_cast<std::uint32_t>(kBMDataBase + kBMStackTopOffset);
    const std::uint8_t movEsp[5] = {0xBC, static_cast<std::uint8_t>(stackTop),
                                    static_cast<std::uint8_t>(stackTop >> 8),
                                    static_cast<std::uint8_t>(stackTop >> 16),
                                    static_cast<std::uint8_t>(stackTop >> 24)};
    code.emitBytes(movEsp, sizeof movEsp);
  }

  // Bring COM1 up before anything prints. QEMU's emulated 16550 works without
  // this, but a real chip powers up with interrupts masked, the divisor latch
  // in an unknown state and the FIFO off, so `oskel_write` would poll a
  // line-status register that never reports the transmitter ready. The
  // sequence is the standard 16550 setup: mask interrupts, raise DLAB to load
  // a divisor of 1 (115200 baud), drop DLAB with an 8N1 frame, then enable and
  // clear the FIFO. Interrupts stay masked -- the runtime polls, it does not
  // take interrupts.
  auto outSerial = [&code](std::int64_t port, std::uint8_t value) {
    code.movImm(X64Reg::Rdx, port);
    code.movImm(X64Reg::Rax, value);
    code.outDxAl();
  };
  outSerial(kSerialIer, 0x00);   // interrupt enable: none
  outSerial(kSerialLcr, 0x80);   // line control: DLAB on
  outSerial(kSerialData, 0x01);  // divisor latch low = 1 (115200 baud)
  outSerial(kSerialIer, 0x00);   // divisor latch high = 0
  outSerial(kSerialLcr, 0x03);   // line control: 8N1, DLAB off
  outSerial(kSerialFcr, 0xC7);   // FIFO: enable, clear, 14-byte threshold

  // Remap the 8259 PIC off the CPU's exception vectors and onto 0x20-0x2F, so
  // IRQ0 becomes a usable interrupt instead of colliding with #DF/#TS/... The
  // sequence is the standard ICW1-ICW4 initialisation of both chips: start the
  // init, set the vector bases, tell the master the slave hangs off IRQ2 and
  // the slave its cascade identity, then select 8086/88 mode. Interrupts are
  // still masked here (`cli` from the trampoline, and the PIC masks below).
  auto outPort = [&code](std::int64_t port, std::uint8_t value) {
    code.movImm(X64Reg::Rdx, port);
    code.movImm(X64Reg::Rax, value);
    code.outDxAl();
  };
  outPort(kPicMasterCmd, kPicIcw1Init);
  outPort(kPicSlaveCmd, kPicIcw1Init);
  outPort(kPicMasterData, kPicVectorBaseMaster);
  outPort(kPicSlaveData, kPicVectorBaseSlave);
  outPort(kPicMasterData, kPicIcw3Master);
  outPort(kPicSlaveData, kPicIcw3Slave);
  outPort(kPicMasterData, kPicIcw4Mode8086);
  outPort(kPicSlaveData, kPicIcw4Mode8086);
  outPort(kPicMasterData, kPicMaskIrq0Irq1);  // unmask the timer and the keyboard
  outPort(kPicSlaveData, kPicMaskAll);

  // Program the 8254 PIT channel 0 in mode 3 (square wave) at a fixed
  // frequency: the 16-bit divisor is loaded low byte first, and the chip
  // divides its 1.193182 MHz input by it to drive IRQ0.
  outPort(kPitCommand, kPitMode3Channel0);
  outPort(kPitChannel0, static_cast<std::uint8_t>(kPitDivisor & 0xFF));
  outPort(kPitChannel0, static_cast<std::uint8_t>((kPitDivisor >> 8) & 0xFF));

  // Install the IDT before the program can fault. `lidt` loads a forward
  // reference the emitter patches at `finalize()`, since the table is emitted
  // after this code.
  code.lidtRipLabel(idtDescriptor);

  // Unmask interrupts as the very last step: the IDT and the PIC are now both
  // ready, so the first timer tick arrives with somewhere to go. Nothing before
  // this point can be interrupted, which is what keeps the half-configured PIC
  // from delivering a spurious vector.
  code.sti();

  code.callLabel(programEntry);
  code.movReg(X64Reg::Rdi, X64Reg::Rax);  // exit code = main's result
  code.callLabel(handles.exit);
  code.ud2();  // oskel_exit never returns

  // The IDT, its per-vector stubs and the common fault handler. Emitted after
  // the entry point and never fallen into: `oskel_exit` above is a `cli; hlt`
  // loop, so the `ud2` never even runs.
  emitIdt(code, handles, idtDescriptor);

  return entryOffset;
}

void emitBareMetalRuntime(CodeGen& code, const RuntimeHandles& handles) {
  // --- long oskel_write(long fd, const char* buf, long len) ---
  // The fd is ignored: everything a kernel prints goes to COM1. The 16550
  // transmit holding register must be empty before each byte (line status bit
  // 5), which is the poll the loop spins on. r8/r9/r10 are scratch -- the
  // runtime holds nothing live in them across a call -- so the cursor, the
  // remaining count and the total stay off the stack.
  code.bind(handles.write);
  {
    CodeGen::Label loop = code.newLabel();
    CodeGen::Label wait = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.movReg(X64Reg::R10, X64Reg::Rdx);  // total to return
    code.movReg(X64Reg::R8, X64Reg::Rsi);   // cursor
    code.movReg(X64Reg::R9, X64Reg::Rdx);   // remaining
    code.bind(loop);
    code.testReg(X64Reg::R9);
    code.jcc(Jcc::Le, done);
    code.bind(wait);
    code.movImm(X64Reg::Rdx, kSerialLsr);
    code.inDxAl();
    code.testAlImm8(kSerialThrEmpty);
    code.jcc(Jcc::Eq, wait);
    code.movImm(X64Reg::Rdx, kSerialData);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R8, 0);
    code.outDxAl();
    code.addImm32(X64Reg::R8, 1);
    code.subRegImm32(X64Reg::R9, 1);
    code.jmp(loop);
    code.bind(done);
    code.movReg(X64Reg::Rax, X64Reg::R10);
    code.retInsn();
  }

  // --- void oskel_exit(long code) ---
  // No `exit_group` and no `ExitProcess`: there is no operating system to
  // return to. The exit code is published on the QEMU `isa-debug-exit` port
  // (0x501) so a test harness gets a deterministic, fast exit; on real hardware
  // nothing decodes that port, the write is ignored, and the `hlt` loop below
  // is the real end of the program. Interrupts are masked first, so the halted
  // CPU never wakes.
  code.bind(handles.exit);
  {
    CodeGen::Label spin = code.newLabel();
    code.cli();
    code.movReg(X64Reg::Rax, X64Reg::Rdi);  // al = low byte of the exit code
    code.movImm(X64Reg::Rdx, 0x501);
    code.outDxAl();
    code.bind(spin);
    code.hlt();
    code.jmp(spin);
  }

  // --- void oskel_init_arena(void) ---
  // The arena is the `.bss` region the loader zeroed, not an `mmap`. The
  // cursor starts `kAotArenaFirstOffset` in so no live pointer is ever zero,
  // exactly like the Linux and Windows preludes.
  code.bind(handles.initArena);
  code.movImm(kAotArenaCursor,
              static_cast<std::int64_t>(kBMDataBase + kBMArenaOffset +
                                        kAotArenaFirstOffset));
  code.movImm(kAotArenaEnd,
              static_cast<std::int64_t>(kBMDataBase + kBMArenaOffset + kAotArenaSize));
  code.retInsn();

  // --- long oskel_read(char* buf, long len) ---
  // The input counterpart to `oskel_write`: it fills `buf` with one line and
  // returns the byte count, consuming the terminating newline. The bytes come
  // from the IRQ1 keyboard ring buffer, which the handler fills; when the
  // buffer is empty the thunk `hlt`s (interrupts are enabled during the
  // program) and wakes on the next keystroke. A CR is dropped so a host that
  // injects `\r\n` sees the same line a bare `\n` produces. Only r8-r11 and the
  // scratch rax/rcx/rdx are touched; the callee-saved registers the runtime
  // carries are preserved.
  code.bind(handles.read);
  {
    CodeGen::Label loop = code.newLabel();
    CodeGen::Label haveByte = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.xorReg(X64Reg::R8, X64Reg::R8);  // count = 0
    code.bind(loop);
    code.cmpReg(X64Reg::R8, X64Reg::Rsi);
    code.jcc(Jcc::Ge, done);  // buffer full: an over-long line is truncated
    code.movImm(X64Reg::R11, static_cast<std::int64_t>(kBMDataBase + kKbHeadOffset));
    code.movFromMem(X64Reg::R9, X64Reg::R11, 0);  // head
    code.movImm(X64Reg::R11, static_cast<std::int64_t>(kBMDataBase + kKbTailOffset));
    code.movFromMem(X64Reg::R10, X64Reg::R11, 0);  // tail
    code.cmpReg(X64Reg::R9, X64Reg::R10);
    code.jcc(Jcc::Ne, haveByte);
    code.hlt();  // empty: wait for the next interrupt (a keystroke or a tick)
    code.jmp(loop);
    code.bind(haveByte);
    code.movImm(X64Reg::R11, static_cast<std::int64_t>(kBMDataBase + kKbBufferOffset));
    code.addReg(X64Reg::R11, X64Reg::R10);  // &buffer[tail]
    code.movzxFromMem(X64Reg::Rax, X64Reg::R11, 0);
    code.addImm32(X64Reg::R10, 1);
    code.andImm32(X64Reg::R10, static_cast<std::int32_t>(kKbBufferSize - 1));
    code.movImm(X64Reg::R11, static_cast<std::int64_t>(kBMDataBase + kKbTailOffset));
    code.movToMem(X64Reg::R11, 0, X64Reg::R10);  // publish the new tail
    code.cmpImm32(X64Reg::Rax, kKbEnter);
    code.jcc(Jcc::Eq, done);
    code.cmpImm32(X64Reg::Rax, '\r');
    code.jcc(Jcc::Eq, loop);  // drop a CR the host may send
    code.movReg(X64Reg::R11, X64Reg::Rdi);
    code.addReg(X64Reg::R11, X64Reg::R8);  // &buf[count]
    code.movByteToMem(X64Reg::R11, 0, X64Reg::Rax);
    code.addImm32(X64Reg::R8, 1);
    code.jmp(loop);
    code.bind(done);
    code.movReg(X64Reg::Rax, X64Reg::R8);
    code.retInsn();
  }

  // --- long oskel_time(void) ---
  // A kernel has no `time` syscall and no libc, so the wall clock comes from the
  // CMOS real-time clock (the mc146818 at ports 0x70/0x71 every PC has). The RTC
  // stores the fields in BCD or binary and in 12- or 24-hour form, selected by
  // status register B, so both are handled. The fields are converted to a Unix
  // timestamp with the "days from civil" algorithm (Howard Hinnant), which is
  // the same value Linux and Windows produce.
  //
  // Only rax/rcx/rdx/rsi are used (all caller-saved scratch); the callee-saved
  // rbx/r12-r15 the runtime carries are untouched. The stack frame holds the
  // fields and the intermediates so no register has to survive a `div`.
  code.bind(handles.now);
  {
    // Field and intermediate slots, rbp-relative.
    constexpr std::int64_t kSec = -8;
    constexpr std::int64_t kMin = -16;
    constexpr std::int64_t kHour = -24;
    constexpr std::int64_t kDay = -32;
    constexpr std::int64_t kMonth = -40;
    constexpr std::int64_t kYear = -48;
    constexpr std::int64_t kStatusB = -56;
    constexpr std::int64_t kEra = -64;
    constexpr std::int64_t kYoe = -72;
    constexpr std::int64_t kDoy = -80;
    constexpr std::int64_t kTemp = -88;
    constexpr std::int64_t kDays = -104;
    constexpr std::int64_t kPmFlag = -112;

    constexpr std::int64_t kCmosIndex = 0x70;
    constexpr std::int64_t kCmosData = 0x71;
    constexpr std::uint8_t kCmosUip = 0x80;    // status A: update in progress
    constexpr std::uint8_t kCmosBinary = 0x04; // status B bit 2: binary (vs BCD)
    constexpr std::uint8_t kCmosHour24 = 0x02; // status B bit 1: 24-hour (vs 12)

    // Reads CMOS register `index` and leaves its raw byte in al/rax.
    auto readRtc = [&](int index) {
      code.movImm(X64Reg::Rax, index);
      code.movImm(X64Reg::Rdx, kCmosIndex);
      code.outDxAl();
      code.movImm(X64Reg::Rdx, kCmosData);
      code.inDxAl();
      code.movzx(X64Reg::Rax, X64Reg::Rax);
    };
    auto store = [&](std::int64_t slot) { code.movToMem(X64Reg::Rbp, slot, X64Reg::Rax); };

    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.subRegImm32(X64Reg::Rsp, 128);

    // Wait out an in-progress update, so the fields are read from one instant.
    CodeGen::Label wait = code.newLabel();
    code.bind(wait);
    code.movImm(X64Reg::Rax, 0x0A);
    code.movImm(X64Reg::Rdx, kCmosIndex);
    code.outDxAl();
    code.movImm(X64Reg::Rdx, kCmosData);
    code.inDxAl();
    code.testAlImm8(kCmosUip);
    code.jcc(Jcc::Ne, wait);

    readRtc(0x0B);
    store(kStatusB);

    readRtc(0x00);
    store(kSec);
    readRtc(0x02);
    store(kMin);
    readRtc(0x04);
    // The hour carries the PM flag in bit 7 in 12-hour mode, which the BCD
    // conversion below would read as a tens digit. Split it off first: bit 7
    // becomes `kPmFlag`, and the field keeps its seven low bits (harmless in
    // 24-hour and binary modes, where bit 7 is already clear).
    code.movReg(X64Reg::Rcx, X64Reg::Rax);
    code.andImm32(X64Reg::Rax, 0x80);
    store(kPmFlag);
    code.movReg(X64Reg::Rax, X64Reg::Rcx);
    code.andImm32(X64Reg::Rax, 0x7F);
    store(kHour);
    readRtc(0x07);
    store(kDay);
    readRtc(0x08);
    store(kMonth);
    readRtc(0x09);
    store(kYear);

    // BCD -> binary, unless status B says the fields are already binary. Each
    // field is `(bcd & 0x0F) + (bcd >> 4) * 10`.
    CodeGen::Label bcdDone = code.newLabel();
    CodeGen::Label bcdLoop = code.newLabel();
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kStatusB);
    code.andImm32(X64Reg::Rax, kCmosBinary);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Ne, bcdDone);
    code.leaMem(X64Reg::Rsi, X64Reg::Rbp, kYear);  // slots are contiguous
    code.movImm(X64Reg::Rcx, 6);
    code.bind(bcdLoop);
    code.movFromMem(X64Reg::Rax, X64Reg::Rsi, 0);
    code.movReg(X64Reg::Rdx, X64Reg::Rax);
    code.andImm32(X64Reg::Rdx, 0x0F);
    code.shrImm8(X64Reg::Rax, 4);
    code.imulImm32(X64Reg::Rax, X64Reg::Rax, 10);
    code.addReg(X64Reg::Rax, X64Reg::Rdx);
    code.movToMem(X64Reg::Rsi, 0, X64Reg::Rax);
    code.addImm32(X64Reg::Rsi, 8);
    code.subRegImm32(X64Reg::Rcx, 1);
    code.testReg(X64Reg::Rcx);
    code.jcc(Jcc::Ne, bcdLoop);
    code.bind(bcdDone);

    // 12-hour form: `kPmFlag` (bit 7 of the raw hour, split off above) is the
    // PM flag. Normalize to 0-23 (`12 AM` is 0, `12 PM` is 12).
    CodeGen::Label hourDone = code.newLabel();
    CodeGen::Label notNoon = code.newLabel();
    CodeGen::Label hourStore = code.newLabel();
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kStatusB);
    code.andImm32(X64Reg::Rax, kCmosHour24);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Ne, hourDone);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kHour);
    code.cmpImm32(X64Reg::Rax, 12);
    code.jcc(Jcc::Ne, notNoon);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);  // 12 -> 0
    code.bind(notNoon);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, kPmFlag);
    code.testReg(X64Reg::Rcx);
    code.jcc(Jcc::Eq, hourStore);
    code.addImm32(X64Reg::Rax, 12);
    code.bind(hourStore);
    code.movToMem(X64Reg::Rbp, kHour, X64Reg::Rax);
    code.bind(hourDone);

    // The year register holds the last two digits; the kernel targets the 21st
    // century, so the full year is 2000 + yy.
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kYear);
    code.addImm32(X64Reg::Rax, 2000);
    code.movToMem(X64Reg::Rbp, kYear, X64Reg::Rax);

    // days_from_civil(year, month, day): the days since 1970-01-01, with the
    // standard 400-year era decomposition. Every year here is positive, so the
    // truncating `div` is the floor the algorithm assumes.
    CodeGen::Label noDec = code.newLabel();
    CodeGen::Label mGt2 = code.newLabel();
    CodeGen::Label mDone = code.newLabel();
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kMonth);
    code.cmpImm32(X64Reg::Rax, 2);
    code.jcc(Jcc::Gt, noDec);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kYear);
    code.subRegImm32(X64Reg::Rax, 1);  // January/February belong to the prior year
    code.movToMem(X64Reg::Rbp, kYear, X64Reg::Rax);
    code.bind(noDec);

    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kYear);
    code.movImm(X64Reg::Rcx, 400);
    code.xorReg(X64Reg::Rdx, X64Reg::Rdx);
    code.divUnsigned(X64Reg::Rcx);
    code.movToMem(X64Reg::Rbp, kEra, X64Reg::Rax);
    code.movToMem(X64Reg::Rbp, kYoe, X64Reg::Rdx);

    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kMonth);
    code.cmpImm32(X64Reg::Rax, 2);
    code.jcc(Jcc::Gt, mGt2);
    code.addImm32(X64Reg::Rax, 9);
    code.jmp(mDone);
    code.bind(mGt2);
    code.subRegImm32(X64Reg::Rax, 3);
    code.bind(mDone);
    code.imulImm32(X64Reg::Rax, X64Reg::Rax, 153);
    code.addImm32(X64Reg::Rax, 2);
    code.movImm(X64Reg::Rcx, 5);
    code.xorReg(X64Reg::Rdx, X64Reg::Rdx);
    code.divUnsigned(X64Reg::Rcx);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, kDay);
    code.addReg(X64Reg::Rax, X64Reg::Rcx);
    code.subRegImm32(X64Reg::Rax, 1);
    code.movToMem(X64Reg::Rbp, kDoy, X64Reg::Rax);

    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kYoe);
    code.imulImm32(X64Reg::Rax, X64Reg::Rax, 365);
    code.movToMem(X64Reg::Rbp, kTemp, X64Reg::Rax);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kYoe);
    code.shrImm8(X64Reg::Rax, 2);  // yoe / 4
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, kTemp);
    code.addReg(X64Reg::Rcx, X64Reg::Rax);
    code.movToMem(X64Reg::Rbp, kTemp, X64Reg::Rcx);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kYoe);
    code.movImm(X64Reg::Rcx, 100);
    code.xorReg(X64Reg::Rdx, X64Reg::Rdx);
    code.divUnsigned(X64Reg::Rcx);  // yoe / 100
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, kTemp);
    code.subReg(X64Reg::Rcx, X64Reg::Rax);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kDoy);
    code.addReg(X64Reg::Rcx, X64Reg::Rax);
    code.movToMem(X64Reg::Rbp, kTemp, X64Reg::Rcx);  // doe

    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kEra);
    code.imulImm32(X64Reg::Rax, X64Reg::Rax, 146097);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, kTemp);
    code.addReg(X64Reg::Rax, X64Reg::Rcx);
    code.subRegImm32(X64Reg::Rax, 719468);
    code.movToMem(X64Reg::Rbp, kDays, X64Reg::Rax);

    // seconds = days * 86400 + hour * 3600 + minute * 60 + second
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kDays);
    code.imulImm32(X64Reg::Rax, X64Reg::Rax, 86400);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, kHour);
    code.imulImm32(X64Reg::Rcx, X64Reg::Rcx, 3600);
    code.addReg(X64Reg::Rax, X64Reg::Rcx);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, kMin);
    code.imulImm32(X64Reg::Rcx, X64Reg::Rcx, 60);
    code.addReg(X64Reg::Rax, X64Reg::Rcx);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, kSec);
    code.addReg(X64Reg::Rax, X64Reg::Rcx);

    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }
}

std::vector<std::uint8_t> buildBareMetalElf(const Target& target,
                                            const std::vector<std::uint8_t>& code,
                                            std::uint64_t entryOffset) {
  if (target.arch != Arch::X86_64 || target.os != Os::Freestanding ||
      target.abi != Abi::SysV) {
    throw std::runtime_error(
        "native/bare_metal: only x86_64-freestanding-sysv is supported");
  }
  if (code.empty()) {
    throw std::runtime_error("native/bare_metal: code is empty");
  }
  if (entryOffset >= code.size()) {
    throw std::runtime_error("native/bare_metal: entry offset out of range");
  }

  constexpr std::uint32_t kEhdrSize = 52;    // ELF32 header
  constexpr std::uint32_t kPhdrSize = 32;    // ELF32 program header
  constexpr std::uint32_t kPhnum = 2;
  constexpr std::uint32_t kCodeFileOffset = 0x1000;

  const std::uint32_t codeSize = static_cast<std::uint32_t>(code.size());

  std::vector<std::uint8_t> out;
  out.reserve(kCodeFileOffset + code.size());

  // --- ELF header (52 bytes) ---
  out.resize(kEhdrSize, 0);
  out[0] = 0x7F;
  out[1] = 'E';
  out[2] = 'L';
  out[3] = 'F';
  out[4] = 1;  // ELFCLASS32: multiboot enters in 32-bit protected mode
  out[5] = 1;  // ELFDATA2LSB
  out[6] = 1;  // EV_CURRENT
  out[7] = 0;  // ELFOSABI_SYSV

  write16At(out, 16, 2);                                     // e_type = ET_EXEC
  write16At(out, 18, 3);                                     // e_machine = EM_386
  write32At(out, 20, 1);                                     // e_version
  write32At(out, 24, static_cast<std::uint32_t>(kBMCodeBase + entryOffset));  // e_entry
  write32At(out, 28, kEhdrSize);                             // e_phoff
  write32At(out, 32, 0);                                     // e_shoff (no section table)
  write32At(out, 36, 0);                                     // e_flags
  write16At(out, 40, kEhdrSize);                             // e_ehsize
  write16At(out, 42, kPhdrSize);                             // e_phentsize
  write16At(out, 44, kPhnum);                                // e_phnum
  write16At(out, 46, 40);                                    // e_shentsize
  write16At(out, 48, 0);                                     // e_shnum
  write16At(out, 50, 0);                                     // e_shstrndx

  // --- Program headers (2 * 32 bytes) ---
  out.resize(kEhdrSize + kPhnum * kPhdrSize, 0);
  const std::size_t ph = kEhdrSize;

  // Segment 1: the code, at 1 MiB, read + execute.
  write32At(out, ph + 0, 1);  // PT_LOAD
  write32At(out, ph + 4, kCodeFileOffset);
  write32At(out, ph + 8, static_cast<std::uint32_t>(kBMCodeBase));
  write32At(out, ph + 12, static_cast<std::uint32_t>(kBMCodeBase));
  write32At(out, ph + 16, codeSize);  // p_filesz
  write32At(out, ph + 20, codeSize);  // p_memsz
  write32At(out, ph + 24, 5);         // PF_R | PF_X
  write32At(out, ph + 28, 0x1000);

  // Segment 2: the `.bss`, at 2 MiB, read + write, with no file bytes
  // (`p_filesz = 0`): the loader zeroes `p_memsz` bytes. That region holds the
  // page tables, the boot stack and the arena.
  write32At(out, ph + kPhdrSize + 0, 1);  // PT_LOAD
  write32At(out, ph + kPhdrSize + 4, 0);  // p_offset
  write32At(out, ph + kPhdrSize + 8, static_cast<std::uint32_t>(kBMDataBase));
  write32At(out, ph + kPhdrSize + 12, static_cast<std::uint32_t>(kBMDataBase));
  write32At(out, ph + kPhdrSize + 16, 0);  // p_filesz
  write32At(out, ph + kPhdrSize + 20, static_cast<std::uint32_t>(kBMBssBytes));
  write32At(out, ph + kPhdrSize + 24, 6);  // PF_R | PF_W
  write32At(out, ph + kPhdrSize + 28, 0x1000);

  // --- The code, at file offset 0x1000 ---
  out.resize(kCodeFileOffset, 0);
  out.insert(out.end(), code.begin(), code.end());

  return out;
}
