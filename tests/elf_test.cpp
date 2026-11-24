// Minimal test for the ELF emitter: generates an ELF with a `ret` and
// verifies the structure (magic, type, machine, entry point).

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "native/elf.h"
#include "native/target.h"

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

static std::uint16_t read16(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return static_cast<std::uint16_t>(bytes.at(at) | (bytes.at(at + 1) << 8));
}

static std::uint32_t read32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(bytes.at(at + static_cast<std::size_t>(i)))
             << (8 * i);
  }
  return value;
}

static std::uint64_t read64(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes.at(at + static_cast<std::size_t>(i)))
             << (8 * i);
  }
  return value;
}

static bool throwsWith(const std::string& needle,
                       const std::vector<std::uint8_t>& text,
                       const std::vector<std::uint8_t>& rodata,
                       std::uint64_t entryOffset) {
  try {
    buildStaticElf(Target::linuxX64(), text, rodata, entryOffset);
  } catch (const std::runtime_error& error) {
    return std::string(error.what()).find(needle) != std::string::npos;
  }
  return false;
}

int main() {
  Target target = Target::linuxX64();

  // Machine code: `ret` (0xC3)
  std::vector<std::uint8_t> text = {0xC3};
  std::vector<std::uint8_t> rodata;
  std::uint64_t entryOffset = 0;

  std::vector<std::uint8_t> elf = buildStaticElf(target, text, rodata, entryOffset);

  // Verify magic
  check(elf.size() >= 64, "ELF has at least 64 bytes");
  check(elf[0] == 0x7F && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F',
        "ELF magic is 7F 45 4C 46");
  check(elf[4] == 2, "ELF class is 64-bit");
  check(elf[5] == 1, "ELF data is little-endian");

  // Verify e_type (offset 16) = ET_EXEC (2)
  check(elf[16] == 2 && elf[17] == 0, "e_type is ET_EXEC");

  // Verify e_machine (offset 18) = EM_X86_64 (0x3E)
  check(elf[18] == 0x3E && elf[19] == 0, "e_machine is EM_X86_64");

  // Verify e_entry (offset 24) = kTextAddress + 0 = 0x401000
  check(read64(elf, 24) == 0x401000, "e_entry is 0x401000");

  // Verify e_phnum (offset 56) = 1 when there is no rodata
  check(read16(elf, 56) == 1, "e_phnum is 1 without rodata");

  // Verify there are bytes after the header (code)
  check(elf.size() > 0x1000, "ELF contains .text section");

  // Verify the 0xC3 byte is at offset 0x1000
  check(elf[0x1000] == 0xC3, ".text starts at file offset 0x1000");

  // A non-zero entry offset is folded into e_entry.
  std::vector<std::uint8_t> longer = {0x90, 0x90, 0xC3};
  std::vector<std::uint8_t> entered = buildStaticElf(target, longer, {}, 2);
  check(read64(entered, 24) == 0x401002, "e_entry honors entryOffset");

  // A non-empty .rodata gets its own read-only PT_LOAD on the next page.
  std::vector<std::uint8_t> data = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
  std::vector<std::uint8_t> withData = buildStaticElf(target, text, data, 0);
  check(read16(withData, 56) == 2, "e_phnum is 2 with rodata");
  const std::size_t phoff = static_cast<std::size_t>(read64(withData, 32));
  check(read32(withData, phoff) == 1, "text p_type is PT_LOAD");
  check(read32(withData, phoff + 4) == 5, "text flags are R|X");
  check(read64(withData, phoff + 8) == 0x1000, "text p_offset is page-aligned");
  check(read64(withData, phoff + 16) == 0x401000, "text p_vaddr is 0x401000");
  check(read64(withData, phoff + 32) == text.size(), "text p_filesz");
  check(read64(withData, phoff + 40) == text.size(), "text p_memsz");
  const std::size_t rodataHeader = phoff + 56;
  check(read32(withData, rodataHeader) == 1, "rodata p_type is PT_LOAD");
  check(read32(withData, rodataHeader + 4) == 4, "rodata flags are R");
  check(read64(withData, rodataHeader + 16) == 0x600000, "rodata p_vaddr is 0x600000");
  check(read64(withData, rodataHeader + 32) == data.size(), "rodata p_filesz");
  check(read64(withData, rodataHeader + 40) == data.size(), "rodata p_memsz");
  const std::size_t rodataOffset =
      static_cast<std::size_t>(read64(withData, rodataHeader + 8));
  check(rodataOffset == 0x2000, "rodata starts on the page after .text");
  check(withData.size() >= rodataOffset + data.size(), "rodata bytes are in the file");
  for (std::size_t i = 0; i < data.size(); ++i) {
    check(withData[rodataOffset + i] == data[i], "rodata byte is preserved");
  }

  // The error paths are part of the contract, not an internal assertion.
  check(throwsWith("text section is empty", {}, {}, 0), "empty text throws");
  check(throwsWith("entry offset out of range", text, {}, 1),
        "out-of-range entry throws");

  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all elf tests passed\n");
  return 0;
}