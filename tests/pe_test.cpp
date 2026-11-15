// Test for the PE emitter: verifies the structure (magic, machine, sections,
// entry point, import table) and, on Windows, executes the generated binary.
//
// The layout the emitter guarantees:
//   .idata at RVA 0x1000 (fixed, so IAT slot addresses are constants),
//   .text  at RVA 0x2000,
//   the COFF header declares the relocations stripped (a modern Windows with
//   "Force ASLR" refuses an image without relocations otherwise),
//   and kernel32.dll is the only import.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "native/pe.h"
#include "native/target.h"

#if defined(_WIN32)
#include <cstdlib>
#include <fstream>
#endif

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

// Reads a little-endian integer from `data` at `offset`.
static std::uint32_t read32(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i)
    v |= static_cast<std::uint32_t>(data[offset + i]) << (8 * i);
  return v;
}

static std::uint16_t read16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint16_t v = 0;
  for (int i = 0; i < 2; ++i)
    v |= static_cast<std::uint16_t>(data[offset + i]) << (8 * i);
  return v;
}

static std::uint64_t read64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<std::uint64_t>(data[offset + i]) << (8 * i);
  return v;
}

static constexpr std::size_t kOh = 0x44 + 20;   // optional header offset
static constexpr std::size_t kSh = kOh + 240;   // first section header offset

// Machine code that calls ExitProcess(0) through its IAT slot:
//   sub rsp, 0x28            ; shadow space (32) + alignment
//   xor ecx, ecx             ; ExitProcess(0)
//   call qword [rip + delta] ; the IAT slot for ExitProcess
// The delta is measured from the end of the 6-byte call, and the .idata section
// sits at a fixed RVA, so it can be computed here exactly as the prelude does.
static std::vector<std::uint8_t> exitProgram() {
  // The call sits after the two prologue instructions (sub rsp,0x28 = 4 bytes,
  // xor ecx,ecx = 2 bytes), so its offset is 6. The displacement is measured
  // from the end of the 6-byte call, exactly as the prelude computes it.
  const std::int64_t callSiteRva = static_cast<std::int64_t>(kPeTextRva) + 6;
  const std::int64_t next = callSiteRva + 6;
  const std::int64_t delta =
      static_cast<std::int64_t>(importIatRva(PeImport::ExitProcess)) - next;

  std::vector<std::uint8_t> text = {
      0x48, 0x83, 0xEC, 0x28,  // sub rsp, 0x28
      0x31, 0xC9,              // xor ecx, ecx
      0xFF, 0x15};             // call qword [rip + disp32]
  for (int i = 0; i < 4; ++i)
    text.push_back(static_cast<std::uint8_t>((delta >> (8 * i)) & 0xFF));
  return text;
}

int main() {
  const Target target = Target::winX64();

  std::vector<std::uint8_t> text = exitProgram();
  std::vector<std::uint8_t> pe = buildStaticPe(target, text, {}, 0);

  // --- DOS header and PE signature ---
  check(pe.size() >= 0x200, "PE has at least 512 bytes");
  check(pe[0] == 'M' && pe[1] == 'Z', "DOS magic is MZ");
  check(read32(pe, 0x3C) == 0x40, "e_lfanew is 0x40");
  check(std::memcmp(pe.data() + 0x40, "PE\0\0", 4) == 0, "PE signature is PE\\0\\0");

  // --- COFF file header ---
  check(read16(pe, 0x44) == 0x8664, "machine is AMD64");
  check(read16(pe, 0x46) == 2, "number of sections is 2");
  check(read16(pe, 0x44 + 16) == 240, "SizeOfOptionalHeader is 240");
  const std::uint16_t characteristics = read16(pe, 0x44 + 18);
  check((characteristics & 0x0002) != 0, "characteristics mark an executable");
  check((characteristics & 0x0001) != 0,
        "characteristics mark the relocations stripped");
  check((characteristics & 0x0020) != 0, "characteristics mark large-address-aware");

  // --- Optional header ---
  check(read16(pe, kOh) == 0x020B, "optional magic is PE32+");
  check(read32(pe, kOh + 16) == kPeTextRva, "AddressOfEntryPoint is .text + 0");
  check(read64(pe, kOh + 24) == kPeImageBase, "ImageBase is 0x140000000");
  check(read32(pe, kOh + 32) == 0x1000, "SectionAlignment is 0x1000");
  check(read32(pe, kOh + 36) == 0x200, "FileAlignment is 0x200");
  check(read16(pe, kOh + 68) == 3, "Subsystem is console");

  // DataDirectory[1] is the import table, inside .idata.
  check(read32(pe, kOh + 112 + 8) == 0x1000, "import directory RVA is 0x1000");
  check(read32(pe, kOh + 112 + 12) == 5 * 20, "import directory size is 5 descriptors");

  // --- Section headers ---
  // Section 1 is .idata (first, so its RVA is fixed regardless of the code size).
  check(std::memcmp(pe.data() + kSh, ".idata", 6) == 0, "section 1 is .idata");
  const std::uint32_t idataVa = read32(pe, kSh + 12);
  const std::uint32_t idataPtr = read32(pe, kSh + 20);
  check(idataVa == 0x1000, ".idata VirtualAddress is 0x1000");
  check((read32(pe, kSh + 36) & 0x80000000u) != 0, ".idata is writable");

  // Section 2 is .text.
  check(std::memcmp(pe.data() + kSh + 40, ".text", 5) == 0, "section 2 is .text");
  const std::uint32_t textVa = read32(pe, kSh + 40 + 12);
  check(textVa == kPeTextRva, ".text VirtualAddress is 0x2000");
  check((read32(pe, kSh + 40 + 36) & 0x20000000u) != 0, ".text is executable");

  // Every section's raw data must fit in the file: Windows rejects an image
  // whose SizeOfRawData runs past EOF with ERROR_BAD_EXE_FORMAT, even though
  // Wine maps the short file. The last section is the one that can be short.
  const std::uint32_t textPtr = read32(pe, kSh + 40 + 20);
  const std::uint32_t textRaw = read32(pe, kSh + 40 + 16);
  check(textRaw % 0x200 == 0, ".text SizeOfRawData is FileAlignment-aligned");
  check(static_cast<std::size_t>(textPtr) + textRaw <= pe.size(),
        "the file carries the whole .text raw data (no truncation)");
  const std::uint32_t idataRaw = read32(pe, kSh + 16);
  check(static_cast<std::size_t>(idataPtr) + idataRaw <= pe.size(),
        "the file carries the whole .idata raw data (no truncation)");

  // --- Import table ---
  // The first import descriptor names kernel32.dll.
  const std::uint32_t nameRva = read32(pe, idataPtr + 12);
  const std::size_t dllNameOffset = idataPtr + (nameRva - idataVa);
  check(std::memcmp(pe.data() + dllNameOffset, "kernel32.dll", 12) == 0,
        "DLL name is kernel32.dll");

  // The descriptor's OriginalFirstThunk and FirstThunk point into .idata.
  const std::uint32_t iltRva = read32(pe, idataPtr + 0);
  const std::uint32_t iatRva = read32(pe, idataPtr + 16);
  check(iltRva >= idataVa, "import lookup table lives in .idata");
  check(iatRva >= idataVa, "import address table lives in .idata");

  // Each import has a hint/name entry whose name matches the enum, and the IAT
  // slot `importIatRva` publishes holds that same entry RVA before binding.
  const char* expected[] = {"WriteFile", "GetStdHandle", "ExitProcess", "VirtualAlloc"};
  for (int i = 0; i < 4; ++i) {
    const std::uint32_t entryRva = read32(pe, idataPtr + (iltRva - idataVa) + i * 8);
    const std::size_t entryOffset = idataPtr + (entryRva - idataVa);
    check(std::memcmp(pe.data() + entryOffset + 2, expected[i],
                      std::strlen(expected[i])) == 0,
          expected[i]);
    const std::uint32_t slotRva = importIatRva(static_cast<PeImport>(i));
    const std::uint32_t slotValue = read32(pe, idataPtr + (slotRva - idataVa));
    check(slotValue == entryRva, "IAT slot points at its hint/name entry");
  }

  // The import list is terminated by a zero descriptor.
  check(read32(pe, idataPtr + 4 * 20) == 0 &&
            read32(pe, idataPtr + 4 * 20 + 4) == 0,
        "import descriptors are zero-terminated");

#if defined(_WIN32)
  const char* path = "oskel_pe_test.exe";
  {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
      check(false, "cannot open oskel_pe_test.exe for writing");
      return g_failures != 0 ? 1 : 0;
    }
    f.write(reinterpret_cast<const char*>(pe.data()),
            static_cast<std::streamsize>(pe.size()));
  }
  const int rc = std::system(path);
  check(rc == 0, "generated PE runs and exits 0");
  std::remove(path);
#else
  std::printf("skipped: execution test requires Windows\n");
#endif

  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all pe tests passed\n");
  return 0;
}
