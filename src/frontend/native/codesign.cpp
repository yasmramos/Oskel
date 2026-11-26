#include "native/codesign.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace {

constexpr std::uint32_t kSuperBlobMagic = 0xFADE0CC0;
constexpr std::uint32_t kCodeDirectoryMagic = 0xFADE0C02;
constexpr std::uint32_t kRequirementsMagic = 0xFADE0C01;
constexpr std::uint32_t kCodeDirectoryVersion = 0x20400;
constexpr std::uint32_t kAdHocFlag = 0x2;  // CS_ADHOC: what `codesign -d` reports
constexpr std::uint8_t kHashTypeSha256 = 2;
constexpr std::uint32_t kHashSize = 32;
// An empty `CSMAGIC_REQUIREMENTS` set: magic, length and a zero count, the
// blob `codesign --sign -` writes for an identity that names no requirement.
constexpr std::uint32_t kRequirementsSize = 12;

void put32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void put64(std::vector<std::uint8_t>& out, std::size_t offset, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// The CodeDirectory and the superblob are big-endian throughout -- not only
// their magic, which only looks that way because every blob starts with one.
// An image whose version reads 0x00020400 has its bytes in the wrong order.
void putBig32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}

void putBig64(std::vector<std::uint8_t>& out, std::size_t offset, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (56 - 8 * i));
}

// `magic`, `length` and `type` are the only big-endian fields: they are the
// ones every blob shares, and Apple's own tooling reads them before it knows
// which structure follows.
void putBlobHeader(std::vector<std::uint8_t>& out, std::size_t offset,
                   std::uint32_t magic, std::uint32_t size) {
  for (int i = 0; i < 4; ++i) {
    out[offset + i] = static_cast<std::uint8_t>(magic >> (24 - 8 * i));
    out[offset + 4 + i] = static_cast<std::uint8_t>(size >> (24 - 8 * i));
  }
}

std::uint32_t directorySize(const std::string& identifier, std::uint32_t slots) {
  return static_cast<std::uint32_t>(codesign::kCodeDirectoryHeaderSize + identifier.size() + 1 +
                                    static_cast<std::size_t>(slots) * kHashSize);
}

}  // namespace

std::vector<std::uint8_t> sha256(const std::uint8_t* data, std::size_t size) {
  static constexpr std::uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};

  std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

  // 0x80, zeros, then the message length in bits as a big-endian 64-bit count.
  const std::size_t padded = (size + 9 + 63) / 64 * 64;
  std::vector<std::uint8_t> block(padded, 0);
  if (size != 0) std::memcpy(block.data(), data, size);
  block[size] = 0x80;
  const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
  for (int i = 0; i < 8; ++i) {
    block[padded - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
  }

  for (std::size_t chunk = 0; chunk < padded; chunk += 64) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block[chunk + i * 4]) << 24) |
             (static_cast<std::uint32_t>(block[chunk + i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(block[chunk + i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(block[chunk + i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t t1 = hh + s1 + ch + k[i] + w[i];
      const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }

  std::vector<std::uint8_t> digest(32);
  for (int i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
    digest[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
    digest[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
    digest[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
  }
  return digest;
}

namespace codesign {

std::uint32_t codeSlots(std::uint64_t codeLimit) {
  if (codeLimit == 0) return 0;
  return static_cast<std::uint32_t>((codeLimit + kPageSize - 1) / kPageSize);
}

std::uint64_t alignSignatureOffset(std::uint64_t size) { return (size + 15) / 16 * 16; }

std::size_t signatureSize(const std::string& identifier, std::uint32_t slots) {
  return kSignaturePreamble + directorySize(identifier, slots) + kRequirementsSize;
}

std::vector<std::uint8_t> adHocSignature(const std::vector<std::uint8_t>& code,
                                         const std::string& identifier,
                                         std::uint64_t execSegBase,
                                         std::uint64_t execSegLimit,
                                         std::uint64_t execSegFlags) {
  const std::uint64_t codeLimit = code.size();
  const std::uint32_t slots = codeSlots(codeLimit);
  const std::uint32_t directory = directorySize(identifier, slots);
  const std::uint32_t total = static_cast<std::uint32_t>(signatureSize(identifier, slots));
  if (total != kSignaturePreamble + directory + kRequirementsSize) {
    throw std::logic_error("codesign: the signature size disagrees with its parts");
  }

  std::vector<std::uint8_t> out(total, 0);
  putBlobHeader(out, 0, kSuperBlobMagic, total);
  putBig32(out, 8, 2);  // count: the CodeDirectory and the requirements blob
  // Index entries are (type, offset) pairs, big-endian.
  putBig32(out, 12, 0);  // CS_SLOT_CODE_DIRECTORY
  putBig32(out, 16, static_cast<std::uint32_t>(kSignaturePreamble));
  putBig32(out, 20, 2);  // CS_SLOT_REQUIREMENTS
  putBig32(out, 24, static_cast<std::uint32_t>(kSignaturePreamble + directory));

  const std::size_t base = kSignaturePreamble;
  putBlobHeader(out, base, kCodeDirectoryMagic, directory);
  putBig32(out, base + 8, kCodeDirectoryVersion);
  putBig32(out, base + 12, kAdHocFlag);
  // The layout is the one Apple defines: header, then nSpecialSlots hashes,
  // then nCodeSlots hashes, then the NUL-terminated identifier. So hashOffset
  // is the header size (no special slots) and identOffset is past every hash.
  // Writing the identifier first would still be readable through the offsets,
  // but `codesign` walks the structure the documented way.
  const std::uint32_t hashOffset = static_cast<std::uint32_t>(kCodeDirectoryHeaderSize);
  const std::uint32_t identOffset =
      static_cast<std::uint32_t>(kCodeDirectoryHeaderSize + static_cast<std::size_t>(slots) * kHashSize);
  putBig32(out, base + 16, hashOffset);
  putBig32(out, base + 20, identOffset);
  putBig32(out, base + 24, 0);  // nSpecialSlots
  putBig32(out, base + 28, slots);
  putBig32(out, base + 32, static_cast<std::uint32_t>(codeLimit));
  out[base + 36] = static_cast<std::uint8_t>(kHashSize);
  out[base + 37] = kHashTypeSha256;
  out[base + 38] = 0;  // platform
  out[base + 39] = kPageSizeShift;
  putBig32(out, base + 40, 0);  // spare2
  putBig32(out, base + 44, 0);  // scatterOffset
  putBig32(out, base + 48, 0);  // teamOffset
  putBig32(out, base + 52, 0);  // spare3
  putBig64(out, base + 56, 0);  // codeLimit64
  putBig64(out, base + 64, execSegBase);
  putBig64(out, base + 72, execSegLimit);
  putBig64(out, base + 80, execSegFlags);

  std::memcpy(out.data() + base + identOffset, identifier.data(), identifier.size());

  const std::size_t hashes = base + hashOffset;
  for (std::uint32_t i = 0; i < slots; ++i) {
    const std::size_t start = static_cast<std::size_t>(i) * kPageSize;
    const std::size_t length = std::min<std::size_t>(kPageSize, code.size() - start);
    const std::vector<std::uint8_t> digest = sha256(code.data() + start, length);
    std::memcpy(out.data() + hashes + static_cast<std::size_t>(i) * kHashSize, digest.data(),
                kHashSize);
  }

  const std::size_t requirements = base + directory;
  putBlobHeader(out, requirements, kRequirementsMagic, kRequirementsSize);
  putBig32(out, requirements + 8, 0);  // count: no requirement in the set
  return out;
}

}  // namespace codesign
