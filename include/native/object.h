#pragma once

#include <cstdint>
#include <vector>

#include "native/codegen.h"
#include "native/target.h"

// Generates a relocatable ELF64 object (`ET_REL`) for Linux x86-64 (PHASE 7f.4).
//
// This is the container the *hosted* link mode needs. A freestanding image is
// closed by the emitter itself -- every `call` resolves through a label and the
// entry point is a fixed address -- so it needs no linker. A hosted image must
// resolve symbols the program does not define (libc's `write`, `exit`, `malloc`
// and, from 7f.5, any `extern "C"` the source names), and only a linker can do
// that. The emitter therefore wraps the code buffer in an object with the two
// tables a linker reads:
//
//   - `.rela.text`: one relocation per external reference, so the linker fills
//     each PC-relative displacement slot with the resolved symbol's address
//     (`R_X86_64_PLT32` for a call, `R_X86_64_PC32` for an address).
//   - `.symtab`/`.strtab`: the undefined symbols the relocations name and the
//     defined symbols the object exports (the program's `main`).
//
// The result is fed to the system C compiler, which links it against libc. The
// object carries no program header and no entry point: those are the linker's
// job, exactly as for a `.o` a C compiler emits.
std::vector<std::uint8_t> buildRelocatableObject(
    const Target& target, const std::vector<std::uint8_t>& text,
    const std::vector<std::uint8_t>& rodata,
    const std::vector<ExternalRef>& externals,
    const std::vector<ExportedSymbol>& exports);
