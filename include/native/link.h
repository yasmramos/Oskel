#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "native/target.h"

// PHASE 10: the project's own linker. It closes x86-64 Linux ELF objects and,
// from PHASE 10.3a, x86-64 Windows COFF objects into PE32+ images.
//
// `native/object.*` already wraps the lowered code in an ELF64 `ET_REL` object
// with a symbol table and `.rela.text` relocations. This module closes one or
// more such objects (and System V/GNU `ar` archives) into a dynamically linked
// ELF64 executable, or combines them into a new `ET_REL` object. It resolves
// the program's `main`, builds the PLT/GOT, writes the `.dynamic` table and the
// loadable segments, and provides the `_start` that hands control to libc's
// `__libc_start_main`.
//
// The executable is a non-PIE `ET_EXEC` that depends on `libc.so.6` (and
// `libm.so.6` when the program names a symbol only libm defines), exactly like
// the image `cc` produced before it. The linker never runs the program, so it
// can link a Linux object from any host, provided the target's shared libraries
// are reachable through `-L`/`LIBRARY_PATH` for the symbol check.
//
// Scope: the object shape `oskel build --emit-object` writes. On Linux,
// `.text`/`.rodata`/`.data`/`.bss` are loaded (PHASE 10.2b) and archives are
// System V/GNU `ar` (not thin archives). On Windows, COFF `.text`/`.rdata`/
// `.data`/`.bss` are loaded and the undefined runtime symbols become imports
// (PHASE 10.3a); relocatable COFF links are PHASE 10.3b.1; MSVC `.lib`
// archives are handled by expandCoffArchives (PHASE 10.3b.2); short import
// objects, `.pdata`/`.xdata` and `.tls` sections are rejected (PHASE 10.3b.4).
// The macOS Mach-O container is PHASE 10.3d: the first slice links a hosted
// `MH_EXECUTE` from one `MH_OBJECT`; the `-r` Mach-O link, the ad-hoc
// signature and ARM64 remain. An input this module does not understand is
// reported, never mis-linked.
struct LinkError : std::exception {
  explicit LinkError(std::string message) : message(std::move(message)) {}
  std::string message;
  const char* what() const noexcept override { return message.c_str(); }
};

// One linker input. `name` is used only in diagnostics (a path, or
// `archive(member)`); `bytes` is the file contents, either an ELF64 `ET_REL`
// object or a System V/GNU `ar` archive.
struct LinkInput {
  std::string name;
  std::vector<std::uint8_t> bytes;
};

struct LinkOptions {
  Target target = Target::linuxX64();
  // `--link-lib` names (`SDL3` resolves `libSDL3.so`). Every library named here
  // becomes a `DT_NEEDED` entry; the symbol check uses its `.dynsym`. Ignored
  // by `linkRelocatable`, which leaves external symbols undefined.
  std::vector<std::string> libraries;
  // `-L` directories, searched before `LIBRARY_PATH`, `LD_LIBRARY_PATH` and the
  // system defaults. Only the *link-time* symbol check reads them; the loader
  // finds the `DT_NEEDED` sonames at run time.
  std::vector<std::string> libraryPaths;
};

// The (arch, os, abi) triples the own linker implements today: Linux SysV and
// Windows Win64.
bool isOwnLinkerTarget(const Target& target);

// Links one or more objects (and `ar` archives) into a dynamically linked
// ELF64 executable. An archive is searched for the members that define a
// still-undefined symbol; a member nobody references is not pulled in.
// Throws `LinkError` for a malformed input, a missing `main`, an unresolved
// symbol or a relocation this phase does not lower.
std::vector<std::uint8_t> linkHostedExecutable(
    const std::vector<LinkInput>& inputs, const LinkOptions& options);

// Single-object convenience overload.
std::vector<std::uint8_t> linkHostedExecutable(
    const std::vector<std::uint8_t>& object, const LinkOptions& options);

// Combines one or more objects (and `ar` archives) into a new ELF64 `ET_REL`
// object. Undefined symbols stay undefined and the relocations are preserved,
// so the result can be linked again by either function. `--link-lib`/`-L` do
// not apply: a relocatable link does not resolve external symbols.
std::vector<std::uint8_t> linkRelocatable(
    const std::vector<LinkInput>& inputs, const LinkOptions& options);
