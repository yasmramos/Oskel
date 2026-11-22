#pragma once

#include <cstdint>
#include <vector>

#include "native/target.h"

// Generates a static PE32+ executable (Windows x86-64) with no external
// dependencies. Non-PIE: fixed image base (0x140000000).
//
// The generated binary contains:
//   - DOS header (64 bytes)
//   - PE signature + COFF file header
//   - Optional header (PE32+)
//   - Section headers (.idata, .text)
//   - .idata (import directory for the kernel32 functions the prelude calls)
//   - .text (machine code)
//
// This is a minimal PE generator: it has no relocations, resources or TLS. It
// is *not* position independent, so the COFF header carries
// IMAGE_FILE_RELOCS_STRIPPED and the loader maps the image at its preferred
// base. The import table is the only dynamic dependency: the prelude reaches
// the Windows API (WriteFile, GetStdHandle, ExitProcess, VirtualAlloc) through
// kernel32.dll.
//
// The set of imported functions is fixed and known to the prelude, so the
// import table is built from a static list here and the prelude's call sites
// only need each slot's RVA, which `importIatRva` returns.

// The kernel32 functions the Windows prelude imports, in a fixed order. The
// order defines the IAT layout, so `importIatRva` and the prelude agree on
// which slot holds which function.
enum class PeImport : int {
  WriteFile = 0,
  GetStdHandle,
  ExitProcess,
  VirtualAlloc,
  GetSystemTimeAsFileTime,
  GetTickCount64,
  CreateFileA,
  ReadFile,
  CloseHandle,
  SetFilePointerEx,
  Count,
};

// The IAT slot RVA for `import`, valid for any image `buildStaticPe` produces
// (the import section sits at a fixed RVA, so the slot addresses do not depend
// on the size of the program). A prelude emits
// `call qword ptr [rip + (slotRva - (textRva + callSiteOffset + 6))]`.
std::uint32_t importIatRva(PeImport import);

// RVA of the `.text` section (where the machine code is mapped). The prelude
// needs it to compute the RIP-relative displacement of an IAT call.
constexpr std::uint32_t kPeTextRva = 0x2000;

// Fixed image base of the generated (non-PIE) executable.
constexpr std::uint64_t kPeImageBase = 0x140000000;

// The stack the loader reserves for the generated process. Windows' own
// default is 1 MiB, but the AOT runtime has no call-depth guard -- the VM's
// 2000-frame limit is the interpreter's, not the native code's -- so the
// recursion a program reaches is bounded only by the OS stack. Linux hands a
// process 8 MiB by default (`ulimit -s`), so reserving the same here keeps a
// program that runs as a Linux AOT binary from being killed on Windows: a few
// thousand frames with a non-trivial local set overflow 1 MiB, and Windows
// tears the process down with no message. Only the reserve is set; the loader
// commits `kPeStackCommit` up front and grows the stack on demand up to it.
constexpr std::uint64_t kPeStackReserve = 8ull * 1024 * 1024;
constexpr std::uint64_t kPeStackCommit = 0x1000;

std::vector<std::uint8_t> buildStaticPe(const Target& target,
                                        const std::vector<std::uint8_t>& text,
                                        const std::vector<std::uint8_t>& rdata,
                                        std::uint64_t entryOffset);
