#pragma once

#include <cstdint>
#include <vector>

#include "native/link.h"

// PHASE 10.3a: the own linker's Windows x86-64 backend.
//
// `linkWindowsExecutable` parses one or more COFF `IMAGE_FILE_MACHINE_AMD64`
// objects, merges their `.text`/`.rdata`/`.data`/`.bss` sections, resolves the
// internal symbols, turns the undefined runtime symbols into imports from
// `kernel32.dll` and `ucrtbase.dll`, and emits a non-PIE PE32+ image. The
// program's `main` is reached through a generated `_start` that calls
// `main(0, NULL)` and then `ExitProcess`.
//
// Scope (10.3a): COFF objects, not MSVC `.lib` archives; `REL32`/`ADDR64`/
// `ADDR32`/`ADDR32NB` relocations; `.pdata`/`.xdata`/`.drectve`/`.debug$*` are
// ignored. `args()` is empty until the `GetCommandLineA` parser lands (10.3b).
std::vector<std::uint8_t> linkWindowsExecutable(
    const std::vector<LinkInput>& inputs, const LinkOptions& options);

// PHASE 10.3b: combines COFF objects into a new COFF object. Defined symbols
// are placed at their merged section offsets, local symbols are renamed to stay
// unique, section-symbol relocations are retargeted to the output section
// symbol with the addend adjusted, and undefined symbols stay undefined.
std::vector<std::uint8_t> linkRelocatableCoff(
    const std::vector<LinkInput>& inputs, const LinkOptions& options);
