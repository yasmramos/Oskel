#pragma once

#include <cstdint>
#include <vector>

#include "native/codegen.h"
#include "native/target.h"

// Generates a relocatable object for the target's hosted link (PHASE 7f.4):
// ELF64 `ET_REL` for Linux, Mach-O `MH_OBJECT` for macOS and COFF
// `IMAGE_FILE_MACHINE_AMD64` for Windows.
//
// This is the container the *hosted* link mode needs. A freestanding image is
// closed by the emitter itself -- every `call` resolves through a label and the
// entry point is a fixed address -- so it needs no linker. A hosted image must
// resolve symbols the program does not define (libc's `write`, `exit`, `malloc`
// and, from 7f.5, any `extern "C"` the source names), and only a linker can do
// that. The emitter therefore wraps the code buffer in an object with the
// format's relocation and symbol tables:
//
//   - ELF: `.rela.text` (`R_X86_64_PLT32` for a call, `R_X86_64_PC32` for an
//     address) plus `.symtab`/`.strtab`.
//   - Mach-O: pcrel `X86_64_RELOC_BRANCH`/`X86_64_RELOC_SIGNED` entries plus
//     `LC_SYMTAB`; every symbol name carries the Mach-O leading underscore.
//   - COFF: `IMAGE_REL_AMD64_REL32` entries plus the 18-byte symbol table and
//     its string table.
//
// The result is fed to the host C compiler, which links it against the system C
// library. The object carries no program header and no entry point: those are
// the linker's job, exactly as for an object a C compiler emits.
std::vector<std::uint8_t> buildRelocatableObject(
    const Target& target, const std::vector<std::uint8_t>& text,
    const std::vector<std::uint8_t>& rodata,
    const std::vector<ExternalRef>& externals,
    const std::vector<ExportedSymbol>& exports);
