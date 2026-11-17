#pragma once

#include <cstdint>
#include <vector>

#include "native/codegen.h"
#include "native/entry.h"
#include "native/target.h"

// Bare-metal x86-64 (multiboot) backend (PHASE 7g).
//
// The third entry prelude, after the Linux syscall one and the Windows
// kernel32 one. It shares the whole x86-64 lowering and the embedded runtime
// with the other two; only the way the image is loaded and the way it reaches
// the outside world differ. There is no operating system: the generated image
// is a multiboot kernel that a boot loader (QEMU `-kernel`, GRUB, Limine)
// loads at a fixed physical address and enters in 32-bit protected mode.
//
// The image is therefore not a Linux-style ELF64 but an ELF32/i386 multiboot
// kernel. Because multiboot enters the CPU in 32-bit mode with paging off and
// leaves a 32-bit code descriptor in the GDT, the prelude carries a small
// trampoline that installs its own 64-bit GDT, builds an identity page table,
// turns on long mode and only then reaches the shared 64-bit runtime. After
// that the program is byte-for-byte the same machine code the Linux target
// emits; only `oskel_write` (serial port), `oskel_exit` (halt) and
// `oskel_init_arena` (a fixed `.bss` region instead of `mmap`) are different.
//
// The arena lives inside the image: the ELF's second PT_LOAD segment is a
// `.bss` region (`p_memsz > p_filesz`) the loader zeroes, and the prelude
// points the allocator at it. That is the freestanding answer to `mmap`/`brk`
// and it needs no memory map from the boot loader.

// Physical load addresses of the image. Multiboot loads the code at 1 MiB
// (the classic kernel load address, above the VGA hole and the BIOS data) and
// the data/`.bss` region at 2 MiB. Both are identity-mapped, so the same value
// serves as a virtual address and a physical one -- which is what lets the
// 32-bit trampoline use absolute addresses before paging is even on.
constexpr std::uint64_t kBMCodeBase = 0x100000;  // .text load address
constexpr std::uint64_t kBMDataBase = 0x200000;  // .bss load address

// The multiboot 1 header (magic, flags, checksum). Flags = 0 asks for a plain
// ELF load with no extra information; the boot loader jumps to `e_entry` in
// 32-bit protected mode.
constexpr std::uint32_t kMultibootMagic = 0x1BADB002;
constexpr std::uint32_t kMultibootFlags = 0x00000000;

// Offset of the 32-bit trampoline inside the code buffer: the 12-byte
// multiboot header comes first (it must sit in the first 8 KiB of the file),
// then the entry point.
constexpr std::uint64_t kBMPreambleHeaderBytes = 12;

// Size of the `.bss` region the image reserves: the page tables, a page of
// kernel data, the boot stack and the arena.
constexpr std::uint64_t kBMBssBytes =
    (0x3000 /* page tables */ + 0x1000 /* kernel data */ +
     0x4000 /* 16 KiB stack */) +
    (1 << 20); /* arena, matching kAotArenaSize */

// Appends the multiboot header, the 32-bit long-mode trampoline, its GDT and
// the 64-bit entry point that runs the program, in that order, so the header
// lands at the very start of the code buffer. `handles.exit` is the runtime's
// exit thunk the 64-bit entry calls after the program returns. Returns the
// offset of the entry point (`e_entry`).
std::uint64_t emitBareMetalPreamble(CodeGen& code, const RuntimeHandles& handles,
                                    CodeGen::Label programEntry);

// Appends the bare-metal runtime thunks the shared runtime references,
// binding `handles.write` (the 16550 serial port), `handles.exit` (halt) and
// `handles.initArena` (point the allocator at the `.bss` arena). Same contract
// as `emitFreestandingRuntime`, without a system call in sight.
void emitBareMetalRuntime(CodeGen& code, const RuntimeHandles& handles);

// Wraps the lowered code buffer in an ELF32/i386 multiboot kernel: an ELF
// header, two PT_LOAD program headers (the code at `kBMCodeBase`, read+execute;
// the `.bss` at `kBMDataBase`, read+write, `p_filesz = 0`) and the code bytes.
// `entryOffset` is `e_entry` relative to the code buffer.
std::vector<std::uint8_t> buildBareMetalElf(const Target& target,
                                            const std::vector<std::uint8_t>& code,
                                            std::uint64_t entryOffset);
