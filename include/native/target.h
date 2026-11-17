#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

// Target descriptor for the PHASE 7 native backend.
//
// The backend is written against a Target from the start, even though only
// Linux x86-64 (System V) has an implementation today. Adding Windows, macOS
// or WebAssembly must be a new backend behind this descriptor, not a rewrite
// of every call site that currently assumes "the target".
//
// `CodeGen` rejects any target it cannot emit for; the enumerators below name
// the targets the backend is expected to grow into, not a promise that they
// exist.

enum class Arch : std::uint8_t {
  X86_64,
  Aarch64,
  Wasm32,
};

enum class Os : std::uint8_t {
  Linux,
  Windows,
  MacOs,
  Freestanding,
};

enum class Abi : std::uint8_t {
  SysV,
  Win64,
  Aapcs64,
  Wasm,
};

struct Target {
  Arch arch = Arch::X86_64;
  Os os = Os::Linux;
  Abi abi = Abi::SysV;

  // The targets with a backend today: Linux x86-64 (direct syscalls), Windows
  // x86-64 (kernel32 through the Microsoft x64 ABI) and bare metal x86-64
  // (multiboot, no operating system). All three share the x86-64 lowering and
  // the embedded runtime; only the entry prelude differs, because each reaches
  // the outside world differently.
  static constexpr Target linuxX64() { return Target{}; }
  static constexpr Target winX64() {
    return Target{Arch::X86_64, Os::Windows, Abi::Win64};
  }
  // Freestanding bare metal: the generated image is a multiboot kernel the
  // boot loader (QEMU `-kernel`, GRUB, Limine) loads and enters directly. It
  // shares the System V x86-64 lowering with Linux; the prelude speaks to the
  // serial port instead of the kernel.
  static constexpr Target bareMetalX64() {
    return Target{Arch::X86_64, Os::Freestanding, Abi::SysV};
  }
};

inline const char* archName(Arch arch) {
  switch (arch) {
    case Arch::X86_64:
      return "x86_64";
    case Arch::Aarch64:
      return "aarch64";
    case Arch::Wasm32:
      return "wasm32";
  }
  return "unknown";
}

inline const char* osName(Os os) {
  switch (os) {
    case Os::Linux:
      return "linux";
    case Os::Windows:
      return "windows";
    case Os::MacOs:
      return "macos";
    case Os::Freestanding:
      return "freestanding";
  }
  return "unknown";
}

inline const char* abiName(Abi abi) {
  switch (abi) {
    case Abi::SysV:
      return "sysv";
    case Abi::Win64:
      return "win64";
    case Abi::Aapcs64:
      return "aapcs64";
    case Abi::Wasm:
      return "wasm";
  }
  return "unknown";
}

// Canonical backend name, e.g. "x86_64-linux-sysv". This is deliberately not
// a GNU triple: the backend cares about the three components it can emit for.
inline std::string targetName(const Target& target) {
  return std::string(archName(target.arch)) + "-" + osName(target.os) + "-" +
         abiName(target.abi);
}

inline bool isSupportedTarget(const Target& target) {
  if (target.arch != Arch::X86_64) return false;
  if (target.os == Os::Linux && target.abi == Abi::SysV) return true;
  if (target.os == Os::Windows && target.abi == Abi::Win64) return true;
  if (target.os == Os::Freestanding && target.abi == Abi::SysV) return true;
  return false;
}

// Fails loudly instead of emitting bytes for an ABI the encoder does not know.
// Callers construct their backend with a Target; a missing target is a
// configuration error, not a reason to silently fall back to Linux.
inline void requireSupportedTarget(const Target& target) {
  if (!isSupportedTarget(target)) {
    throw std::invalid_argument("native: no backend for target " + targetName(target) +
                                " (supported: " + targetName(Target::linuxX64()) + ", " +
                                targetName(Target::winX64()) + ", " +
                                targetName(Target::bareMetalX64()) + ")");
  }
}

// How the AOT backend terminates an image (PHASE 7f.3).
//
// The lowering and the embedded runtime are shared; the mode only decides how
// the generated code reaches the outside world and how the image is closed:
//
//   - Freestanding: the default today. The image carries its own runtime and
//     speaks to the kernel (Linux syscalls), kernel32 (Windows) or the serial
//     port (bare metal) directly. No libc, no linker, no external symbol: an
//     `extern "C"`/`native fn` binding is resolved from a curated stub or the
//     build fails. `ldd` reports "not a dynamic executable".
//   - Hosted: an object file linked against the host C library, so an
//     `extern "C"` symbol resolves to the real C function at link time. This is
//     the mode that makes FFI work, and the one `oskelc` is documented to be.
//     The image depends on libc, so it is *not* freestanding.
//
// The mode is a deployment choice, never language syntax: the same source
// compiles in either mode, and the choice lives on the command line.
enum class LinkMode : std::uint8_t {
  Freestanding,
  Hosted,
};

inline const char* linkModeName(LinkMode mode) {
  switch (mode) {
    case LinkMode::Freestanding:
      return "freestanding";
    case LinkMode::Hosted:
      return "hosted";
  }
  return "unknown";
}

// Whether the (target, mode) pair has a backend today. Freestanding is
// implemented for all three targets; hosted needs a C toolchain to link against
// and is Linux x86-64 only for now (Windows and macOS hosted link differently
// and are later work), so the CLI can report the unsupported pair instead of
// emitting bytes it cannot close.
inline bool isSupportedLinkMode(const Target& target, LinkMode mode) {
  if (!isSupportedTarget(target)) return false;
  switch (mode) {
    case LinkMode::Freestanding:
      return true;
    case LinkMode::Hosted:
      return target.os == Os::Linux && target.arch == Arch::X86_64;
  }
  return false;
}

// Fails loudly instead of emitting bytes for a (target, mode) pair the backend
// cannot close. A caller that reaches here with an unsupported pair is a
// configuration error, not a reason to silently pick another mode.
inline void requireSupportedLinkMode(const Target& target, LinkMode mode) {
  if (!isSupportedLinkMode(target, mode)) {
    throw std::invalid_argument(
        std::string("native: no ") + linkModeName(mode) + " backend for target " +
        targetName(target));
  }
}
