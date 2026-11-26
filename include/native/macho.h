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
// a `__stubs` entry per symbol, enters at `main` through `LC_MAIN`, and ends
// with an ad-hoc code signature (`LC_CODE_SIGNATURE`) over every page, because
// the kernel refuses to start an image that carries none.
//
// A call reaches an import through `__stubs` (`X86_64_RELOC_BRANCH`) and a data
// reference reaches the import's non-lazy pointer in `__got`
// (`X86_64_RELOC_SIGNED`). A pcrel reference to a symbol defined in the same
// object is resolved directly; an absolute relocation is reported, never
// mis-linked.
std::vector<std::uint8_t> linkMachOExecutable(
    const std::vector<LinkInput>& inputs, const LinkOptions& options);

// Combines one or more `MH_OBJECT` inputs into a new `MH_OBJECT` (`-r`). The
// output concatenates `__text` and `__const`, keeps every defined symbol at its
// merged offset, leaves an undefined symbol undefined, and preserves the
// external pcrel relocations so `linkMachOExecutable` can close the result.
// Internal (section-relative) relocations and absolute relocations are
// reported, never mis-linked.
std::vector<std::uint8_t> linkRelocatableMachO(
    const std::vector<LinkInput>& inputs, const LinkOptions& options);
