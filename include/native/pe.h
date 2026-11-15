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

std::vector<std::uint8_t> buildStaticPe(const Target& target,
                                        const std::vector<std::uint8_t>& text,
                                        const std::vector<std::uint8_t>& rdata,
                                        std::uint64_t entryOffset);
