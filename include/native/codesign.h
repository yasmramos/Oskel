#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// PHASE 10.3d.3: the ad-hoc code signature a macOS image needs to run.
//
// The Mach-O linker used to close an executable with no `LC_CODE_SIGNATURE` and
// leave the signature to the host's `codesign` (the macOS test re-signed the
// image before running it, which hid the gap). An image that carries no
// signature is not a finished container: arm64 macOS refuses to start one at
// all, and on both architectures the signature is what lets the kernel map a
// page executable.
//
// An *ad-hoc* signature names no signing identity: the CodeDirectory records a
// SHA-256 per page and the kernel trusts it because it is consistent with the
// file, not because a certificate vouched for it. It is what `codesign --sign -`
// writes, and what `ld64.lld` puts in an image it links on a non-macOS host.
//
// Layout: a `CS_SuperBlob` (`0xfade0cc0`) holding one CodeDirectory
// (`0xfade0c02`, version `0x20400`, SHA-256). The `execSeg*` fields that arm64
// reads stay zero, like `ld64.lld` writes them for x86-64; filling them belongs
// to the ARM64 slice, not to this one.

// SHA-256 of `size` bytes at `data`. Public because the signature is only as
// good as its hash: the test pins this against the published vectors, and a
// wrong implementation would make every emitted signature invalid in a way no
// structural check can see.
std::vector<std::uint8_t> sha256(const std::uint8_t* data, std::size_t size);

namespace codesign {

// The page size the CodeDirectory hashes in, and its log2 as stored.
constexpr std::uint32_t kPageSize = 4096;
constexpr std::uint8_t kPageSizeShift = 12;

// The CodeDirectory fixed header (version 0x20400), identifier excluded.
constexpr std::size_t kCodeDirectoryHeaderSize = 88;

// `CS_SuperBlob` header (magic, length, count) plus one `(type, offset)` index
// entry per blob, i.e. where the CodeDirectory begins inside the signature.
constexpr std::size_t kSignaturePreamble = 12 + 2 * 8;

// Number of `kPageSize` pages the signature hashes for a `codeLimit`-byte
// image, i.e. the CodeDirectory's `nCodeSlots`.
std::uint32_t codeSlots(std::uint64_t codeLimit);

// The size of the signature `adHocSignature` returns for that identifier and
// slot count. The caller needs it *before* it can hash anything: the signature
// begins at its own `codeLimit`, so the slot count follows from the image
// length, and the `LC_CODE_SIGNATURE` fields the hash covers follow from this.
std::size_t signatureSize(const std::string& identifier, std::uint32_t slots);

// `CS_EXECSEG_MAIN_BINARY`: this image is the main executable, not a dylib or
// a bundle. It is the only `execSegFlags` value the x86-64 linker sets.
constexpr std::uint64_t kExecSegMainBinary = 1;

// The signature over `code`, the image bytes that precede it. `code` must
// already contain the final `LC_CODE_SIGNATURE` `dataoff`/`datasize` values:
// both are derivable from the image length (`code.size()` and the slot count
// above), so nothing needs to be zeroed before hashing and the region the
// kernel checks is byte for byte the region this hashed.
//
// `execSecBase`/`execSegLimit` describe the main executable relative to the
// start of `__TEXT`; the kernel takes the entry point from them on arm64. The
// x86-64 image passes the whole `__TEXT` and `kExecSegMainBinary`.
std::vector<std::uint8_t> adHocSignature(const std::vector<std::uint8_t>& code,
                                         const std::string& identifier, std::uint64_t execSegBase,
                                         std::uint64_t execSegLimit, std::uint64_t execSegFlags);

// Rounds an image length up to the 16-byte boundary `LC_CODE_SIGNATURE.dataoff`
// is required to keep. A CodeDirectory page hash assumes its page is whole, so
// the padding is part of the hashed region.
std::uint64_t alignSignatureOffset(std::uint64_t size);

}  // namespace codesign
