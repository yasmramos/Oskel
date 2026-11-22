#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "native/target.h"

// PHASE 10.1: the project's own linker for the hosted x86-64 Linux target.
//
// `native/object.*` already wraps the lowered code in an ELF64 `ET_REL` object
// with a symbol table and `.rela.text` relocations. Until now the only thing
// that could turn that object into an executable was the host C compiler
// (`oskel build --mode hosted` shells out to `$CC`). This module closes the
// object itself: it resolves the program's `main`, builds the PLT/GOT, writes
// the `.dynamic` table and the four loadable segments, and provides the `_start`
// that hands control to libc's `__libc_start_main`.
//
// The result is a non-PIE `ET_EXEC` that depends on `libc.so.6` (and `libm.so.6`
// when the program names a symbol only libm defines), exactly like the image
// `cc` produced before it. The linker never runs the program, so it can link a
// Linux object from any host, provided the target's shared libraries are
// reachable through `-L`/`LIBRARY_PATH` for the symbol check.
//
// Scope: one object, produced by `oskel build --emit-object`. Multiple objects,
// `ar` archives and the macOS/Windows containers are later phases; an object
// this module does not understand is reported, never mis-linked.
struct LinkError : std::exception {
  explicit LinkError(std::string message) : message(std::move(message)) {}
  std::string message;
  const char* what() const noexcept override { return message.c_str(); }
};

struct LinkOptions {
  Target target = Target::linuxX64();
  // `--link-lib` names (`SDL3` resolves `libSDL3.so`). Every library named here
  // becomes a `DT_NEEDED` entry; the symbol check uses its `.dynsym`.
  std::vector<std::string> libraries;
  // `-L` directories, searched before `LIBRARY_PATH`, `LD_LIBRARY_PATH` and the
  // system defaults. Only the *link-time* symbol check reads them; the loader
  // finds the `DT_NEEDED` sonames at run time.
  std::vector<std::string> libraryPaths;
};

// The only (arch, os, abi) the own linker implements today.
bool isOwnLinkerTarget(const Target& target);

// Links one relocatable object into a dynamically linked ELF64 executable.
// Throws `LinkError` for a malformed object, a missing `main`, an unresolved
// symbol or a relocation this phase does not lower.
std::vector<std::uint8_t> linkHostedExecutable(
    const std::vector<std::uint8_t>& object, const LinkOptions& options);
