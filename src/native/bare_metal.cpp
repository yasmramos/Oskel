#include "native/bare_metal.h"

#include <cstring>
#include <stdexcept>

namespace {

// 16550 UART registers (COM1). The prelude drives the chip directly: a kernel
// has no `write` syscall, so the serial port is the whole output channel.
constexpr std::int64_t kSerialData = 0x3F8;  // transmit/receive holding
constexpr std::int64_t kSerialIer = 0x3F9;   // interrupt enable
constexpr std::int64_t kSerialFcr = 0x3FA;   // FIFO control
constexpr std::int64_t kSerialLcr = 0x3FB;   // line control (8N1 = 0x03)
constexpr std::int64_t kSerialLsr = 0x3FD;   // line status (bit 5 = THR empty)
constexpr std::uint8_t kSerialThrEmpty = 0x20;

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
constexpr std::uint64_t kTrampolineLength = 112;    // cli .. ljmp, inclusive
constexpr std::uint64_t kTrampolineGdtRel = 0x70;   // GDT, right after the ljmp
constexpr std::uint64_t kTrampolineGdtPtrRel = 0x88;  // GDT descriptor record
constexpr std::uint64_t kTrampolineLongModeRel = 0x8E;
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
//   mov eax, cr4; or eax, 0x20; mov cr4, eax   ; PAE
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
    0x0F, 0x20, 0xE0, 0x83, 0xC8, 0x20, 0x0F, 0x22, 0xE0,
    0xB9, 0x80, 0x00, 0x00, 0xC0,
    0x0F, 0x32, 0x0D, 0x00, 0x01, 0x00, 0x00, 0x0F, 0x30,
    0x0F, 0x20, 0xC0, 0x0D, 0x01, 0x00, 0x00, 0x80, 0x0F, 0x22, 0xC0,
    0x0F, 0x01, 0x15, 0x00, 0x00, 0x00, 0x00,  // lgdt [gdt_ptr] (patched)
    0xEA, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,  // ljmp 0x08:long_mode (patched)
};

// Byte offsets of the two patched dwords inside `kTrampoline`.
constexpr std::size_t kTrampolineLgdtDisp = 0x65;   // lgdt's absolute operand
constexpr std::size_t kTrampolineLjmpTarget = 0x6A;  // ljmp's 32-bit offset

void write32At(std::vector<std::uint8_t>& out, std::size_t off, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
  }
}

}  // namespace

std::uint64_t emitBareMetalPreamble(CodeGen& code, const RuntimeHandles& handles,
                                    CodeGen::Label programEntry) {
  if (code.offset() != 0) {
    throw std::runtime_error("native/bare_metal: the multiboot header must be first");
  }

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
  code.callLabel(programEntry);
  code.movReg(X64Reg::Rdi, X64Reg::Rax);  // exit code = main's result
  code.callLabel(handles.exit);
  code.ud2();  // oskel_exit never returns

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

  write32At(out, 16, 2);                                     // e_type = ET_EXEC
  write32At(out, 18, 3);                                     // e_machine = EM_386
  write32At(out, 20, 1);                                     // e_version
  write32At(out, 24, static_cast<std::uint32_t>(kBMCodeBase + entryOffset));  // e_entry
  write32At(out, 28, kEhdrSize);                             // e_phoff
  write32At(out, 32, 0);                                     // e_shoff (no section table)
  write32At(out, 36, 0);                                     // e_flags
  write32At(out, 40, kEhdrSize);                             // e_ehsize
  write32At(out, 42, kPhdrSize);                             // e_phentsize
  write32At(out, 44, kPhnum);                                // e_phnum
  write32At(out, 46, 40);                                    // e_shentsize
  write32At(out, 48, 0);                                     // e_shnum
  write32At(out, 50, 0);                                     // e_shstrndx

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
