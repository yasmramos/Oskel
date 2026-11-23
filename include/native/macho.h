#pragma once

#include <cstdint>
#include <vector>

#include "native/link.h"

// PHASE 10.3d: the macOS Mach-O/dyld container.
//
// The input is the `MH_OBJECT` that `native/object.*` emits for macOS x86-64
// (one `__TEXT` segment with `__text` and, when present, `__const`, plus the
// pcrel relocations of every `extern "C"` reference). The output is a
// non-PIE `MH_EXECUTE` that imports libSystem through a non-lazy `__got` and
// a `__stubs` entry per symbol, and enters at `main` through `LC_MAIN`.
//
// The object is the only container the first slice understands: the
// `__stubs`/`__got` path covers a call (`X86_64_RELOC_BRANCH`); a data
// reference (`X86_64_RELOC_SIGNED` to an import) is reported, not mis-linked.
std::vector<std::uint8_t> linkMachOExecutable(
    const std::vector<LinkInput>& inputs, const LinkOptions& options);
