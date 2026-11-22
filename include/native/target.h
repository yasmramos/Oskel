#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

// Target descriptor for the PHASE 7 native backend.
//
// The backend is written against a Target from the start, even though only
// x86-64 has an implementation today (Linux System V, Windows Win64, macOS
// System V hosted, and bare metal). Adding WebAssembly or another architecture
// must be a new backend behind this descriptor, not a rewrite of every call
// site that currently assumes "the target".
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
  // x86-64 (kernel32 through the Microsoft x64 ABI), macOS x86-64 (System V,
  // hosted only) and bare metal x86-64 (multiboot, no operating system). All
  // four share the x86-64 lowering and the embedded runtime; only the entry
  // prelude and the container differ, because each reaches the outside world
  // differently.
  static constexpr Target linuxX64() { return Target{}; }
  static constexpr Target winX64() {
    return Target{Arch::X86_64, Os::Windows, Abi::Win64};
  }
  // macOS x86-64 uses the System V ABI, like Linux, so it shares the lowering
  // and the extern-thunk marshalling. It has no freestanding backend: the
  // object format is Mach-O and the image is closed by the host C toolchain,
  // which is exactly the hosted link mode.
  static constexpr Target macX64() {
    return Target{Arch::X86_64, Os::MacOs, Abi::SysV};
  }
  // Freestanding bare metal: the generated image is a multiboot kernel the
  // boot loader (QEMU `-kernel`, GRUB, Limine) loads and enters directly. It
  // shares the System V x86-64 lowering with Linux; the prelude speaks to the
  // serial port instead of the kernel.
  static constexpr Target bareMetalX64() {
    return Target{Arch::X86_64, Os::Freestanding, Abi::SysV};
  }
  // WebAssembly (PHASE 9). The image is a `.wasm` module, not an executable:
  // it is emitted by `native/wasm.cpp`, which shares the value model with the
  // x86-64 backend but none of its emission code. The OS component is
  // `Freestanding` because the module reaches the outside world only through
  // the two imports it declares, never through an operating system.
  static constexpr Target wasm32() {
    return Target{Arch::Wasm32, Os::Freestanding, Abi::Wasm};
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
  if (target.os == Os::MacOs && target.abi == Abi::SysV) return true;
  if (target.os == Os::Freestanding && target.abi == Abi::SysV) return true;
  return false;
}

// The WebAssembly target has its own lowering (`native/wasm.h`) and is kept
// out of `isSupportedTarget` on purpose: `requireSupportedTarget` guards the
// x86-64 emitter, and accepting a wasm target there would emit x86 bytes into
// a `.wasm` file. The CLI dispatches on this predicate before choosing the
// backend.
inline bool isWasmTarget(const Target& target) {
  return target.arch == Arch::Wasm32 && target.os == Os::Freestanding &&
         target.abi == Abi::Wasm;
}

// Fails loudly instead of emitting bytes for an ABI the encoder does not know.
// Callers construct their backend with a Target; a missing target is a
// configuration error, not a reason to silently fall back to Linux.
inline void requireSupportedTarget(const Target& target) {
  if (!isSupportedTarget(target)) {
    throw std::invalid_argument("native: no backend for target " + targetName(target) +
                                " (supported: " + targetName(Target::linuxX64()) + ", " +
                                targetName(Target::winX64()) + ", " +
                                targetName(Target::macX64()) + ", " +
                                targetName(Target::bareMetalX64()) + ")");
  }
}

// How the AOT backend terminates an image (PHASE 7f.3).
//
// The lowering and the embedded runtime are shared; the mode only decides how
// the generated code reaches the outside world and how the image is closed:
//
//   - Freestanding: the default on Linux and Windows. The image carries its own
//     runtime and speaks to the kernel (Linux syscalls), kernel32 (Windows) or
//     the serial port (bare metal) directly. No libc, no linker, no external
//     symbol: an `extern "C"`/`native fn` binding is resolved from a curated
//     stub or the build fails. `ldd` reports "not a dynamic executable".
//   - Hosted: an object file linked against the host C library, so an
//     `extern "C"` symbol resolves to the real C function at link time. This is
//     the mode that makes FFI work, and the one `oskelc` is documented to be.
//     The image depends on libc, so it is *not* freestanding. It is the only
//     mode macOS has: the object is Mach-O and the image is closed by the host
//     toolchain.
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
// implemented for Linux, Windows and bare metal; macOS has no freestanding
// image (its container would be Mach-O and the runtime reaches the system
// through libSystem anyway). Hosted needs a C toolchain to link against and is
// implemented for Linux, Windows and macOS x86-64. The CLI reports an
// unsupported pair instead of emitting bytes it cannot close.
inline bool isSupportedLinkMode(const Target& target, LinkMode mode) {
  if (isWasmTarget(target)) return mode == LinkMode::Freestanding;
  if (!isSupportedTarget(target)) return false;
  switch (mode) {
    case LinkMode::Freestanding:
      return target.os != Os::MacOs;
    case LinkMode::Hosted:
      return target.arch == Arch::X86_64 &&
             (target.os == Os::Linux || target.os == Os::Windows ||
              target.os == Os::MacOs);
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
