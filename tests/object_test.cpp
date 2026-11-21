// Structural test for the relocatable object emitter (PHASE 7f.4): builds an
// ELF64 `ET_REL` for a hand-written code buffer with one external call and one
// exported symbol, and verifies the header, the section table, the relocation
// entry and the symbol table against the ELF/x86-64 specification.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "native/codegen.h"
#include "native/object.h"
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

static std::uint16_t read16(const std::vector<std::uint8_t>& b, std::size_t off) {
  return static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8));
}

static std::uint32_t read32(const std::vector<std::uint8_t>& b, std::size_t off) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[off + i]) << (8 * i);
  return v;
}

static std::uint64_t read64(const std::vector<std::uint8_t>& b, std::size_t off) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b[off + i]) << (8 * i);
  return v;
}

// Locates a section by its name through `.shstrtab`. Returns its index, or 0
// when the name is not found.
static std::uint16_t findSection(const std::vector<std::uint8_t>& obj, const std::string& name) {
  const std::uint64_t shoff = read64(obj, 40);
  const std::uint16_t shnum = read16(obj, 60);
  const std::uint16_t shstrndx = read16(obj, 62);
  const std::uint64_t shstrOff = read64(obj, shoff + static_cast<std::uint64_t>(shstrndx) * 64 + 24);
  for (std::uint16_t i = 0; i < shnum; ++i) {
    const std::uint64_t entry = shoff + static_cast<std::uint64_t>(i) * 64;
    const std::uint32_t nameOff = read32(obj, entry);
    std::string sectionName;
    for (std::size_t p = shstrOff + nameOff; p < obj.size() && obj[p] != 0; ++p) {
      sectionName.push_back(static_cast<char>(obj[p]));
    }
    if (sectionName == name) return i;
  }
  return 0;
}

static bool fixedNameEquals(const std::vector<std::uint8_t>& b, std::size_t off,
                            std::size_t width, const std::string& expected) {
  for (std::size_t i = 0; i < width; ++i) {
    const char actual = i < b.size() ? static_cast<char>(b[off + i]) : '\0';
    const char want = i < expected.size() ? expected[i] : '\0';
    if (actual != want) return false;
  }
  return true;
}

// A Mach-O `MH_OBJECT` for the same code buffer: the header, the one segment
// with its `__text` section, the `LC_SYMTAB` and the pcrel branch relocation.
static void testMachO() {
  const Target target = Target::macX64();
  CodeGen cg(target);
  cg.callExternal("write");
  cg.exportSymbol("main", 0);
  cg.finalize();
  const std::vector<std::uint8_t> obj = buildRelocatableObject(
      target, cg.code(), {}, cg.externalRefs(), cg.exportedSymbols());

  check(obj.size() > 240, "Mach-O object has a header and section data");
  check(read32(obj, 0) == 0xFEEDFACF, "Mach-O magic is 64-bit");
  check(read32(obj, 4) == 0x01000007, "Mach-O cputype is x86-64");
  check(read32(obj, 12) == 1, "Mach-O filetype is MH_OBJECT");
  check(read32(obj, 16) == 3, "Mach-O has three load commands");
  check(read32(obj, 32) == 0x19, "first load command is LC_SEGMENT_64");
  check(read32(obj, 96) == 1, "the segment has one section");
  check(fixedNameEquals(obj, 104, 16, "__text"), "section is __text");
  check(fixedNameEquals(obj, 120, 16, "__TEXT"), "section segment is __TEXT");
  check(read32(obj, 152) == 240, "section data starts at the 16-byte aligned offset");
  check(read32(obj, 164) == 1, "one relocation in __text");
  check(read32(obj, 208) == 0x2, "third load command is LC_SYMTAB");

  const std::uint32_t symoff = read32(obj, 216);
  const std::uint32_t nsyms = read32(obj, 220);
  const std::uint32_t stroff = read32(obj, 224);
  check(nsyms == 3, "Mach-O symbol table has null, main and write");
  check(read32(obj, symoff + 16) == 1, "main's name is at string offset 1");
  check(obj[symoff + 16 + 4] == 0x0F, "main is a defined external section symbol");
  check(obj[symoff + 16 + 5] == 1, "main is in section 1");
  check(obj[symoff + 32 + 4] == 0x01, "write is an undefined external symbol");
  check(obj[symoff + 32 + 5] == 0, "write has no section");
  check(fixedNameEquals(obj, stroff + 1, 5, "_main"), "Mach-O name carries the underscore");
  check(fixedNameEquals(obj, stroff + 7, 6, "_write"), "Mach-O external name carries the underscore");

  const std::uint32_t reloff = read32(obj, 160);
  check(read32(obj, reloff) == 1, "Mach-O relocation is at the call displacement");
  const std::uint32_t word = read32(obj, reloff + 4);
  check((word & 0x00FFFFFFu) == 2, "Mach-O relocation names symbol 2 (write)");
  check(((word >> 28) & 0xF) == 2, "Mach-O relocation type is X86_64_RELOC_BRANCH");
  check(((word >> 24) & 1) == 1, "Mach-O relocation is PC-relative");
  check(((word >> 27) & 1) == 1, "Mach-O relocation is external");
  check(((word >> 25) & 3) == 2, "Mach-O relocation field is four bytes");
}

// A COFF `IMAGE_FILE_MACHINE_AMD64` object: the file header, `.text`, the
// `IMAGE_REL_AMD64_REL32` entry and the 18-byte symbol table.
static void testCoff() {
  const Target target = Target::winX64();
  CodeGen cg(target);
  cg.callExternal("write");
  cg.exportSymbol("main", 0);
  cg.finalize();
  const std::vector<std::uint8_t> obj = buildRelocatableObject(
      target, cg.code(), {}, cg.externalRefs(), cg.exportedSymbols());

  check(obj.size() > 128, "COFF object has a header and section data");
  check(read16(obj, 0) == 0x8664, "COFF machine is IMAGE_FILE_MACHINE_AMD64");
  check(read16(obj, 2) == 1, "COFF has one section");
  check(read32(obj, 8) == 92, "COFF symbol table starts after the relocations");
  check(read32(obj, 12) == 2, "COFF symbol table has main and write");
  check(fixedNameEquals(obj, 20, 8, ".text"), "COFF section is .text");
  check(read32(obj, 36) == 5, "COFF .text size is the code length");
  check(read32(obj, 40) == 64, "COFF .text data starts at the 16-byte aligned offset");
  check(read32(obj, 44) == 80, "COFF relocation table follows the text");
  check(read16(obj, 52) == 1, "COFF has one relocation");
  check(read32(obj, 56) == 0x60000020, "COFF .text is code, execute and read");

  check(read32(obj, 80) == 1, "COFF relocation is at the call displacement");
  check(read32(obj, 84) == 1, "COFF relocation names symbol 1 (write)");
  check(read16(obj, 88) == 4, "COFF relocation type is IMAGE_REL_AMD64_REL32");

  check(fixedNameEquals(obj, 92, 8, "main"), "COFF exports main");
  check(read16(obj, 92 + 12) == 1, "COFF main is in section 1");
  check(read16(obj, 92 + 14) == 0x20, "COFF main is a function");
  check(obj[92 + 16] == 2, "COFF main is an external symbol");
  check(fixedNameEquals(obj, 110, 8, "write"), "COFF imports write");
  check(read16(obj, 110 + 12) == 0, "COFF write is undefined");
  check(obj[110 + 16] == 2, "COFF write is an external symbol");
  check(read32(obj, 128) == 4, "COFF empty string table is just its size field");
}

int main() {
  const Target target = Target::linuxX64();

  // A code buffer with a single `call rel32` whose displacement slot is an
  // external reference, and one exported symbol. `mov eax, 0; ret` stands in
  // for the program body.
  CodeGen cg(target);
  cg.callExternal("write");
  cg.exportSymbol("main", 0);
  cg.finalize();

  const std::vector<std::uint8_t> obj = buildRelocatableObject(
      target, cg.code(), {}, cg.externalRefs(), cg.exportedSymbols());

  // --- ELF header ---
  check(obj.size() >= 64, "object has at least 64 bytes");
  check(obj[0] == 0x7F && obj[1] == 'E' && obj[2] == 'L' && obj[3] == 'F',
        "ELF magic is 7F 45 4C 46");
  check(obj[4] == 2, "ELF class is 64-bit");
  check(obj[5] == 1, "ELF data is little-endian");
  check(read16(obj, 16) == 1, "e_type is ET_REL");
  check(read16(obj, 18) == 0x3E, "e_machine is EM_X86_64");
  check(read64(obj, 24) == 0, "e_entry is 0 (the linker picks it)");
  check(read64(obj, 32) == 0, "e_phoff is 0 (an object has no program headers)");
  check(read16(obj, 56) == 0, "e_phnum is 0");
  check(read16(obj, 60) == 7, "e_shnum is 7");

  // --- Section table ---
  check(findSection(obj, ".text") != 0, ".text section exists");
  check(findSection(obj, ".rela.text") != 0, ".rela.text section exists");
  check(findSection(obj, ".symtab") != 0, ".symtab section exists");
  check(findSection(obj, ".strtab") != 0, ".strtab section exists");

  const std::uint16_t relaIndex = findSection(obj, ".rela.text");
  const std::uint64_t relaOff =
      read64(obj, read64(obj, 40) + static_cast<std::uint64_t>(relaIndex) * 64 + 24);
  const std::uint64_t relaSize =
      read64(obj, read64(obj, 40) + static_cast<std::uint64_t>(relaIndex) * 64 + 32);
  check(relaSize == 24, ".rela.text holds exactly one 24-byte entry");

  // --- Relocation entry ---
  check(read64(obj, relaOff) == 1, "relocation offset is 1 (after the call opcode)");
  const std::uint64_t info = read64(obj, relaOff + 8);
  check(static_cast<std::uint32_t>(info) == 4, "relocation type is R_X86_64_PLT32");
  check(read64(obj, relaOff + 16) == static_cast<std::uint64_t>(-4),
        "relocation addend is -4 (PC-relative)");

  // --- Symbol table: null, the .text section symbol, main, write ---
  const std::uint16_t symIndex = findSection(obj, ".symtab");
  const std::uint64_t symOff =
      read64(obj, read64(obj, 40) + static_cast<std::uint64_t>(symIndex) * 64 + 24);
  const std::uint64_t symSize =
      read64(obj, read64(obj, 40) + static_cast<std::uint64_t>(symIndex) * 64 + 32);
  check(symSize == 4 * 24, ".symtab holds four entries (null, .text, main, write)");
  // Entry 2 is `main`: global function, defined in section 1, value 0.
  check(read16(obj, symOff + 2 * 24 + 6) == 1, "main is defined in section 1 (.text)");
  check(read64(obj, symOff + 2 * 24 + 8) == 0, "main has value 0");
  check((obj[symOff + 2 * 24 + 4] >> 4) == 1, "main is a global symbol");
  // Entry 3 is `write`: global function, undefined.
  check(read16(obj, symOff + 3 * 24 + 6) == 0, "write is undefined (SHN_UNDEF)");
  check((obj[symOff + 3 * 24 + 4] >> 4) == 1, "write is a global symbol");

  testMachO();
  testCoff();

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all object emitter checks passed\n");
  return 0;
}
