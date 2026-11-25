#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "native/link.h"
#include "native/object.h"
#include "native/target.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
  if (condition) {
    std::cout << "ok: " << message << "\n";
  } else {
    std::cerr << "not ok: " << message << "\n";
    ++failures;
  }
}

std::uint32_t read32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  if (offset + 4 > bytes.size()) return 0;
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
  return value;
}

std::uint64_t read64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  if (offset + 8 > bytes.size()) return 0;
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
  return value;
}

bool contains(const std::vector<std::uint8_t>& bytes, std::size_t offset,
              std::size_t size, const char* text) {
  const std::size_t length = std::strlen(text);
  if (offset > bytes.size() || size > bytes.size() - offset) return false;
  for (std::size_t i = 0; i + length <= size; ++i) {
    if (std::memcmp(bytes.data() + offset + i, text, length) == 0) return true;
  }
  return false;
}

struct MachSection {
  bool found = false;
  std::uint64_t addr = 0;
  std::uint64_t size = 0;
  std::uint32_t offset = 0;
  std::uint32_t reserved1 = 0;
};

std::size_t findCommand(const std::vector<std::uint8_t>& pe, std::uint32_t want) {
  const std::uint32_t count = read32(pe, 16);
  std::size_t command = 0x20;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (command + 8 > pe.size()) return 0;
    if (read32(pe, command) == want) return command;
    command += read32(pe, command + 4);
  }
  return 0;
}

MachSection findSection(const std::vector<std::uint8_t>& pe, const char* want) {
  const std::uint32_t count = read32(pe, 16);
  std::size_t command = 0x20;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (command + 8 > pe.size()) break;
    const std::uint32_t cmd = read32(pe, command);
    const std::uint32_t size = read32(pe, command + 4);
    if (cmd == 0x19) {
      const std::uint32_t sections = read32(pe, command + 64);
      std::size_t section = command + 72;
      for (std::uint32_t s = 0; s < sections; ++s) {
        if (std::strncmp(reinterpret_cast<const char*>(pe.data() + section), want,
                         std::strlen(want)) == 0) {
          MachSection result;
          result.found = true;
          result.addr = read64(pe, section + 32);
          result.size = read64(pe, section + 40);
          result.offset = read32(pe, section + 48);
          result.reserved1 = read32(pe, section + 68);
          return result;
        }
        section += 80;
      }
    }
    command += size;
  }
  return MachSection{};
}

void testHello() {
  const std::vector<std::uint8_t> text = {0xE8, 0, 0, 0, 0, 0xC3};
  const std::vector<std::uint8_t> data = {0x41, 0x42, 0x43};
  const std::vector<std::uint8_t> object = buildRelocatableObject(
      Target::macX64(), text, data, {{1, "write", true}}, {{"main", 0}});
  LinkOptions options;
  options.target = Target::macX64();
  const std::vector<std::uint8_t> pe = linkHostedExecutable(
      std::vector<LinkInput>{{"hello.obj", object}}, options);

  check(read32(pe, 0) == 0xFEEDFACF, "MH_MAGIC_64");
  check(read32(pe, 4) == 0x01000007, "x86-64");
  check(read32(pe, 12) == 2, "MH_EXECUTE");
  check(read32(pe, 16) == 12, "twelve load commands");
  check(contains(pe, 0, pe.size(), "libSystem.B.dylib"), "imports libSystem");
  check(contains(pe, 0, pe.size(), "/usr/lib/dyld"), "uses dyld");
  check(contains(pe, 0, pe.size(), "__mh_execute_header"), "exports __mh_execute_header");
  check(contains(pe, 0, pe.size(), "_main"), "exports _main");
  check(contains(pe, 0, pe.size(), "_write"), "imports _write");

  const MachSection textSection = findSection(pe, "__text");
  const MachSection constSection = findSection(pe, "__const");
  const MachSection stubs = findSection(pe, "__stubs");
  const MachSection got = findSection(pe, "__got");
  check(textSection.found && textSection.size == text.size(), "__text section");
  check(constSection.found && constSection.size == data.size(), "__const section");
  check(stubs.found && stubs.size == 6, "__stubs section");
  check(got.found && got.size == 8, "__got section");
  if (!textSection.found || !stubs.found || !got.found) return;

  const std::uint8_t* code = pe.data() + textSection.offset;
  check(code[0] == 0xE8, "the entry calls the stub");
  const std::int32_t callDisplacement = static_cast<std::int32_t>(read32(pe, textSection.offset + 1));
  const std::uint64_t callTarget = textSection.addr + 1 + 4 + callDisplacement;
  check(callTarget == stubs.addr, "the call lands on __stubs");

  const std::uint8_t* stub = pe.data() + stubs.offset;
  check(stub[0] == 0xFF && stub[1] == 0x25, "the stub jumps through the GOT");
  const std::int32_t stubDisplacement = static_cast<std::int32_t>(read32(pe, stubs.offset + 2));
  const std::uint64_t stubTarget = stubs.addr + 6 + stubDisplacement;
  check(stubTarget == got.addr, "the stub targets __got");

  const std::size_t mainCommand = findCommand(pe, 0x80000028);
  check(mainCommand != 0, "LC_MAIN");
  if (mainCommand != 0) {
    check(read64(pe, mainCommand + 8) == textSection.offset, "LC_MAIN enters the text");
  }
  const std::size_t dyldInfo = findCommand(pe, 0x80000022);
  check(dyldInfo != 0, "LC_DYLD_INFO_ONLY");
  if (dyldInfo != 0) {
    const std::uint32_t bindOffset = read32(pe, dyldInfo + 16);
    const std::uint32_t bindSize = read32(pe, dyldInfo + 20);
    check(contains(pe, bindOffset, bindSize, "_write"), "the bind names _write");

    // Walk the bind stream the way dyld does. A wrong opcode (0x32 is
    // SET_DYLIB_SPECIAL_IMM, not SET_SEGMENT_AND_OFFSET_ULEB) still leaves the
    // symbol name in the bytes, so `contains` alone cannot catch it -- the
    // stream has to parse to the end and bind every import into __DATA.
    std::size_t cursor = bindOffset;
    const std::size_t end = bindOffset + bindSize;
    int bound = 0;
    bool wellFormed = true;
    while (cursor < end) {
      const std::uint8_t opcode = pe[cursor++];
      const std::uint8_t immediate = opcode & 0x0F;
      switch (opcode & 0xF0) {
        case 0x00:  // DONE
          wellFormed = cursor == end;
          cursor = end;
          break;
        case 0x10:  // SET_DYLIB_ORDINAL_IMM
        case 0x30:  // SET_DYLIB_SPECIAL_IMM
        case 0x50:  // SET_TYPE_IMM
          break;
        case 0x40:  // SET_SYMBOL_TRAILING_FLAGS_IMM
          while (cursor < end && pe[cursor] != 0) ++cursor;
          ++cursor;
          break;
        case 0x70:                                    // SET_SEGMENT_AND_OFFSET_ULEB
          wellFormed = wellFormed && immediate == 2;  // __DATA
          while (cursor < end && (pe[cursor] & 0x80) != 0) ++cursor;
          ++cursor;
          break;
        case 0x90:  // DO_BIND
          ++bound;
          break;
        default:
          wellFormed = false;
          cursor = end;
          break;
      }
    }
    check(wellFormed, "the bind stream is well formed to the end");
    // One DO_BIND per import; LC_DYSYMTAB counts each twice (a stub and a GOT
    // slot), so the import count is half its indirect count.
    const std::size_t dysymtabForImports = findCommand(pe, 0xB);
    const int imports =
        dysymtabForImports == 0 ? 1 : static_cast<int>(read32(pe, dysymtabForImports + 60)) / 2;
    check(bound == imports, "every import is bound into __DATA");
  }
  const std::size_t dysymtab = findCommand(pe, 0xB);
  check(dysymtab != 0 && read32(pe, dysymtab + 60) == 2, "two indirect symbols");
}

// A data import: a pcrel `X86_64_RELOC_SIGNED` reference (`call = false`) must
// reach the import's non-lazy pointer in `__got`, not a stub. The instruction
// shape is `lea rax, [rip + disp]` (0x48 0x8D 0x05 disp32), whose field-relative
// walk is the same one the object emitter leaves zero.
void testDataImport() {
  // `lea rax, [rip+0]` then `ret`.
  const std::vector<std::uint8_t> text = {0x48, 0x8D, 0x05, 0, 0, 0, 0xC3};
  const std::vector<std::uint8_t> object =
      buildRelocatableObject(Target::macX64(), text, {}, {{3, "stdout", false}}, {{"main", 0}});
  LinkOptions options;
  options.target = Target::macX64();
  const std::vector<std::uint8_t> pe =
      linkHostedExecutable(std::vector<LinkInput>{{"data.obj", object}}, options);

  const MachSection textSection = findSection(pe, "__text");
  const MachSection stubs = findSection(pe, "__stubs");
  const MachSection got = findSection(pe, "__got");
  check(got.found && got.size == 8, "a data import still gets a __got slot");
  check(stubs.found && stubs.size == 6, "a data import still gets a __stubs entry");
  if (!textSection.found || !got.found) return;

  const std::int32_t displacement = static_cast<std::int32_t>(read32(pe, textSection.offset + 3));
  const std::uint64_t field = textSection.addr + 3;
  const std::uint64_t target = field + 4 + displacement;
  check(target == got.addr, "the data reference lands on __got, not __stubs");

  // `__stubs` is the first block of the indirect symbol table; the GOT pointers
  // are the second, so `__got`'s `reserved1` is the import count, not 0. A 0
  // there makes dyld resolve the GOT slot through the stub block, which names
  // the same symbol but a different section.
  check(stubs.reserved1 == 0, "__stubs starts the indirect table at index 0");
  check(got.reserved1 == 1, "__got starts after the stub block");

  // The bind still reaches __DATA with the right symbol.
  const std::size_t dyldInfo = findCommand(pe, 0x80000022);
  check(dyldInfo != 0, "LC_DYLD_INFO_ONLY is present");
  if (dyldInfo != 0) {
    const std::uint32_t bindOffset = read32(pe, dyldInfo + 16);
    const std::uint32_t bindSize = read32(pe, dyldInfo + 20);
    check(contains(pe, bindOffset, bindSize, "_stdout"), "the bind names _stdout");
  }
}

void testRelocatable() {
  // `main` calls `helper`, which a second object defines, and `write`, which
  // stays undefined. `-r` merges the two objects and keeps both relocations;
  // the executable link resolves `helper` in the merged text and `write`
  // through `__stubs`.
  const std::vector<std::uint8_t> mainText = {0xE8, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0xC3};
  const std::vector<std::uint8_t> helperText = {0xC3};
  const std::vector<std::uint8_t> mainObject = buildRelocatableObject(
      Target::macX64(), mainText, {}, {{1, "helper", true}, {6, "write", true}},
      {{"main", 0}});
  const std::vector<std::uint8_t> helperObject = buildRelocatableObject(
      Target::macX64(), helperText, {}, {}, {{"helper", 0}});

  LinkOptions options;
  options.target = Target::macX64();
  const std::vector<std::uint8_t> merged = linkRelocatable(
      std::vector<LinkInput>{{"main.o", mainObject}, {"helper.o", helperObject}},
      options);
  check(read32(merged, 0) == 0xFEEDFACF, "the relocatable output is Mach-O");
  check(read32(merged, 12) == 1, "the relocatable output is MH_OBJECT");
  check(contains(merged, 0, merged.size(), "_main"), "the relocatable output keeps main");
  check(contains(merged, 0, merged.size(), "_helper"), "the relocatable output keeps helper");
  check(contains(merged, 0, merged.size(), "_write"), "the relocatable output keeps write");

  const std::vector<std::uint8_t> image = linkHostedExecutable(
      std::vector<LinkInput>{{"merged.o", merged}}, options);
  const MachSection text = findSection(image, "__text");
  const MachSection stubs = findSection(image, "__stubs");
  check(text.found && text.size == 17, "the executable carries both text blocks");
  check(stubs.found && stubs.size == 6, "write still reaches a stub");
  if (!text.found || !stubs.found) return;
  const std::int32_t helperCall =
      static_cast<std::int32_t>(read32(image, text.offset + 1));
  const std::uint64_t helperTarget = text.addr + 1 + 4 + helperCall;
  const std::uint64_t helperAddress = text.addr + 16;  // second object, 16-aligned
  check(helperTarget == helperAddress, "the cross-object call lands on helper");
  const std::int32_t writeCall =
      static_cast<std::int32_t>(read32(image, text.offset + 6));
  const std::uint64_t writeTarget = text.addr + 6 + 4 + writeCall;
  check(writeTarget == stubs.addr, "the import call still lands on __stubs");
}

// The image must be internally consistent even on a host that cannot run it, so
// every field the loader reads is checked against the file it describes: each
// load command fits in `sizeofcmds`, the case that a wrong `ncmds` hides; every
// segment's `fileoff`/`filesize` lies in the file; the three mapped segments do
// not overlap in virtual address space; `LC_MAIN` enters `__text`; and the
// symbol/string/indirect/bind tables of `LC_SYMTAB`, `LC_DYSYMTAB` and
// `LC_DYLD_INFO_ONLY` lie in `__LINKEDIT` and inside the file.
void testContainerStructure() {
  const std::vector<std::uint8_t> text = {0xE8, 0, 0, 0, 0, 0xC3};
  const std::vector<std::uint8_t> data = {0x41, 0x42};
  const std::vector<std::uint8_t> object = buildRelocatableObject(
      Target::macX64(), text, data, {{1, "write", true}, {1, "exit", true}}, {{"main", 0}});
  LinkOptions options;
  options.target = Target::macX64();
  const std::vector<std::uint8_t> image =
      linkHostedExecutable(std::vector<LinkInput>{{"structure.obj", object}}, options);
  if (image.size() < 0x20) {
    check(false, "the image has a header");
    return;
  }

  const std::uint32_t commandCount = read32(image, 16);
  const std::uint32_t commandsSize = read32(image, 20);
  check(commandsSize != 0 && 0x20u + commandsSize <= image.size(), "sizeofcmds fits in the file");

  // Walk the load commands by their own `cmdsize` and require the walk to land
  // exactly on `0x20 + sizeofcmds`; a command whose size is wrong, or an
  // `ncmds` that does not match, breaks the sum.
  struct Segment {
    std::uint64_t vmaddr = 0;
    std::uint64_t vmsize = 0;
    std::uint64_t fileoff = 0;
    std::uint64_t filesize = 0;
    std::uint32_t initprot = 0;
    std::string name;
  };
  std::vector<Segment> segments;
  bool walkOk = true;
  bool allInFile = true;
  std::size_t cursor = 0x20;
  for (std::uint32_t i = 0; i < commandCount; ++i) {
    if (cursor + 8 > image.size()) {
      walkOk = false;
      break;
    }
    const std::uint32_t command = read32(image, cursor);
    const std::uint32_t size = read32(image, cursor + 4);
    if (size < 8 || cursor + size > 0x20u + commandsSize) {
      walkOk = false;
      break;
    }
    if (command == 0x19) {  // LC_SEGMENT_64
      Segment segment;
      char name[17] = {0};
      std::memcpy(name, image.data() + cursor + 8, 16);
      segment.name = name;
      segment.vmaddr = read64(image, cursor + 24);
      segment.vmsize = read64(image, cursor + 32);
      segment.fileoff = read64(image, cursor + 40);
      segment.filesize = read64(image, cursor + 48);
      segment.initprot = read32(image, cursor + 60);
      segments.push_back(segment);
      if (segment.filesize != 0 &&
          (segment.fileoff > image.size() || segment.filesize > image.size() - segment.fileoff)) {
        allInFile = false;
      }
    }
    cursor += size;
  }
  check(walkOk, "every load command fits in sizeofcmds");
  check(cursor == 0x20u + commandsSize, "the load commands sum to exactly sizeofcmds");
  check(allInFile, "every segment's file range lies in the file");

  const bool hasText = segments.size() > 1 && segments[1].name == "__TEXT";
  const bool hasData = segments.size() > 2 && segments[2].name == "__DATA";
  const bool hasLinkedit = segments.size() > 3 && segments[3].name == "__LINKEDIT";
  check(hasText && hasData && hasLinkedit,
        "the segments are __PAGEZERO, __TEXT, __DATA, __LINKEDIT");
  if (hasText && hasData && hasLinkedit) {
    // __text is mapped read+execute, __DATA read+write and __LINKEDIT read
    // only; a segment with the wrong protection is rejected by the kernel
    // (or, worse, silently lets the program write its own code).
    check(segments[1].initprot == 5, "__TEXT is read+execute");
    check(segments[2].initprot == 3, "__DATA is read+write");
    check(segments[3].initprot == 1, "__LINKEDIT is read only");
    check(segments[1].vmsize % 0x1000 == 0, "__TEXT is page aligned");
    check(segments[2].vmaddr >= segments[1].vmaddr + segments[1].vmsize,
          "__DATA does not overlap __TEXT");
    check(segments[3].vmaddr >= segments[2].vmaddr + segments[2].vmsize,
          "__LINKEDIT does not overlap __DATA");
    check(segments[3].filesize != 0 && segments[3].fileoff + segments[3].filesize <= image.size(),
          "__LINKEDIT carries the tables and lies in the file");
  }

  const MachSection textSection = findSection(image, "__text");
  const std::size_t mainCommand = findCommand(image, 0x80000028);
  check(mainCommand != 0, "LC_MAIN is present");
  if (mainCommand != 0 && textSection.found) {
    const std::uint64_t entry = read64(image, mainCommand + 8);
    check(entry >= textSection.offset && entry < textSection.offset + textSection.size,
          "LC_MAIN enters __text");
  }

  const std::size_t symtab = findCommand(image, 0x2);
  check(symtab != 0, "LC_SYMTAB is present");
  if (symtab != 0) {
    const std::uint32_t symoff = read32(image, symtab + 8);
    const std::uint32_t nsyms = read32(image, symtab + 12);
    const std::uint32_t stroff = read32(image, symtab + 16);
    const std::uint32_t strsize = read32(image, symtab + 20);
    check(nsyms != 0 && symoff + static_cast<std::uint64_t>(nsyms) * 16 <= image.size(),
          "the symbol table lies in the file");
    check(strsize != 0 && stroff + static_cast<std::uint64_t>(strsize) <= image.size(),
          "the string table lies in the file");
    // Every symbol's name offset must land inside the string table, or a
    // debugger reads past it.
    bool namesInRange = true;
    for (std::uint32_t i = 0; i < nsyms; ++i) {
      const std::uint32_t nameOffset = read32(image, symoff + i * 16);
      if (nameOffset >= strsize) namesInRange = false;
    }
    check(namesInRange, "every symbol name offset lies in the string table");
  }

  const std::size_t dysymtab = findCommand(image, 0xB);
  check(dysymtab != 0, "LC_DYSYMTAB is present");
  if (dysymtab != 0) {
    const std::uint32_t indirectOff = read32(image, dysymtab + 56);
    const std::uint32_t indirectCount = read32(image, dysymtab + 60);
    check(indirectCount == 4, "one indirect entry per import, twice");
    check(indirectOff + static_cast<std::uint64_t>(indirectCount) * 4 <= image.size(),
          "the indirect symbol table lies in the file");
    // Each indirect entry indexes the symbol table's undefined block, so an
    // entry must name one of the two imports.
    const std::size_t symtabCommand = findCommand(image, 0x2);
    const std::uint32_t nsyms = symtabCommand == 0 ? 0 : read32(image, symtabCommand + 12);
    bool inRange = true;
    for (std::uint32_t i = 0; i < indirectCount; ++i) {
      if (read32(image, indirectOff + i * 4) >= nsyms) inRange = false;
    }
    check(inRange, "every indirect entry indexes the symbol table");
  }

  const std::size_t dyldInfo = findCommand(image, 0x80000022);
  check(dyldInfo != 0, "LC_DYLD_INFO_ONLY is present");
  if (dyldInfo != 0) {
    const std::uint32_t bindOff = read32(image, dyldInfo + 16);
    const std::uint32_t bindSize = read32(image, dyldInfo + 20);
    check(bindSize != 0 && bindOff + static_cast<std::uint64_t>(bindSize) <= image.size(),
          "the bind stream lies in the file");
  }
}

}  // namespace

int main() {
  testHello();
  testDataImport();
  testRelocatable();
  testContainerStructure();
  return failures == 0 ? 0 : 1;
}
