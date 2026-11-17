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
// a single common handler that reports the vector and the faulting RIP over
// the serial port before halting.
constexpr std::uint64_t kIdtGateCount = 256;
constexpr std::uint64_t kIdtGateBytes = 16;
constexpr std::uint8_t kIdtGateType = 0x8E;  // present, DPL 0, 64-bit interrupt gate
constexpr std::uint16_t kIdtLimit =
    static_cast<std::uint16_t>(kIdtGateCount * kIdtGateBytes - 1);

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

// .bss layout, all relative to kBMDataBase: the page tables first, then the
// boot stack (which grows down toward them) and the arena at the top.
constexpr std::uint64_t kBMPml4Offset = 0x0000;
constexpr std::uint64_t kBMPdptOffset = 0x1000;
constexpr std::uint64_t kBMPdOffset = 0x2000;
constexpr std::uint64_t kBMStackTopOffset = 0x7000;  // 16 KiB stack, top address
constexpr std::uint64_t kBMArenaOffset = 0x7000;     // arena sits right above

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

// Emits the interrupt descriptor table, one stub per vector and the single
// common fault handler, and binds `descriptorLabel` at the 10-byte
// pseudo-descriptor the entry point loads with `lidt`.
//
// A CPU exception is not maskable by `cli`, so the only defence against a
// silent triple fault is an IDT: without one, the first `#UD` (say an SSE
// instruction before CR4.OSFXSR is set) escalates to a double fault and then a
// reset with nothing printed. Every gate points at a stub that pushes the
// vector number -- preceded by a dummy error code when the CPU did not push
// one -- so the frame the handler sees is always `[rsp]` = vector, `[rsp+8]` =
// error code, `[rsp+16]` = faulting RIP. The handler prints `oskel: cpu
// exception 0xVV rip=0xRRRRRRRRRRRRRRRR` over the serial port and halts: a
// fault is fatal here, there is no handler to return to.
static void emitIdt(CodeGen& code, const RuntimeHandles& handles,
             CodeGen::Label descriptorLabel) {
  std::vector<CodeGen::Label> stubs(kIdtGateCount);
  for (std::uint64_t v = 0; v < kIdtGateCount; ++v) stubs[v] = code.newLabel();
  CodeGen::Label handler = code.newLabel();
  CodeGen::Label templateBytes = code.newLabel();

  const std::int64_t stackTop =
      static_cast<std::int64_t>(kBMDataBase + kBMStackTopOffset);

  // --- the common fault handler ---
  // Read the vector and the faulting RIP off the interrupt frame, then switch
  // to the top of the boot stack: the fault may have happened on a stack that
  // is itself broken (a stack overflow is a `#PF`/`#DF`), and the handler
  // needs room to build its message.
  code.bind(handler);
  code.movFromMem(X64Reg::R8, X64Reg::Rsp, 0);   // vector
  code.movFromMem(X64Reg::R9, X64Reg::Rsp, 16);  // faulting RIP
  code.movImm(X64Reg::Rsp, stackTop);
  code.subRegImm32(X64Reg::Rsp, kFaultMessageBytes);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);  // message buffer base

  code.leaRipLabel(X64Reg::Rsi, templateBytes);
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

  // --- one stub per vector ---
  for (std::uint64_t v = 0; v < kIdtGateCount; ++v) {
    code.bind(stubs[v]);
    if (!vectorPushesErrorCode(static_cast<int>(v))) code.pushImm8(0);
    code.pushImm8(static_cast<std::int32_t>(v));
    code.jmp(handler);
  }

  // --- the message template ---
  code.bind(templateBytes);
  code.emitBytes(reinterpret_cast<const std::uint8_t*>(kFaultTemplate),
                 static_cast<std::size_t>(kFaultTemplateLen));

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

  // Install the IDT before the program can fault. `lidt` loads a forward
  // reference the emitter patches at `finalize()`, since the table is emitted
  // after this code.
  code.lidtRipLabel(idtDescriptor);

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
