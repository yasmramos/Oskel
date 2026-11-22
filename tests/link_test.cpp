// Structural test for the project's own linker (PHASE 10.1): builds an ELF64
// `ET_REL` object with the existing emitter, links it into a hosted executable
// and verifies the header, the segments, the dynamic table, the PLT/GOT and the
// `_start` prelude against the ELF/x86-64 specification.
//
// The image is not executed here; `tests/build_hosted_own.cmake` runs the real
// fixtures on the VM and as a linked binary. This test only needs a host where
// the target libc is visible for the symbol check, so the image cases are
// Linux-only; the target predicates run everywhere.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "native/codegen.h"
#include "native/link.h"
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

static std::string readString(const std::vector<std::uint8_t>& b, std::size_t off) {
  std::string text;
  while (off < b.size() && b[off] != 0) text.push_back(static_cast<char>(b[off++]));
  return text;
}

struct SectionView {
  std::string name;
  std::uint32_t type = 0;
  std::uint64_t flags = 0;
  std::uint64_t addr = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint32_t link = 0;
  std::uint32_t info = 0;
  std::uint64_t entsize = 0;
};

static std::vector<SectionView> readSections(const std::vector<std::uint8_t>& image) {
  std::vector<SectionView> sections;
  const std::uint64_t shoff = read64(image, 40);
  const std::uint16_t shnum = read16(image, 60);
  const std::uint16_t shstrndx = read16(image, 62);
  const std::uint64_t shstr = read64(image, shoff + static_cast<std::uint64_t>(shstrndx) * 64 + 24);
  for (std::uint16_t i = 0; i < shnum; ++i) {
    const std::uint64_t entry = shoff + static_cast<std::uint64_t>(i) * 64;
    SectionView view;
    view.name = readString(image, shstr + read32(image, entry));
    view.type = read32(image, entry + 4);
    view.flags = read64(image, entry + 8);
    view.addr = read64(image, entry + 16);
    view.offset = read64(image, entry + 24);
    view.size = read64(image, entry + 32);
    view.link = read32(image, entry + 40);
    view.info = read32(image, entry + 44);
    view.entsize = read64(image, entry + 56);
    sections.push_back(std::move(view));
  }
  return sections;
}

static const SectionView* findSection(const std::vector<SectionView>& sections,
                                      const std::string& name) {
  for (const SectionView& section : sections) {
    if (section.name == name) return &section;
  }
  return nullptr;
}

struct PhdrView {
  std::uint32_t type = 0;
  std::uint32_t flags = 0;
  std::uint64_t offset = 0;
  std::uint64_t vaddr = 0;
  std::uint64_t filesz = 0;
  std::uint64_t memsz = 0;
};

static std::vector<PhdrView> readPhdrs(const std::vector<std::uint8_t>& image) {
  std::vector<PhdrView> headers;
  const std::uint64_t phoff = read64(image, 32);
  const std::uint16_t phentsize = read16(image, 54);
  const std::uint16_t phnum = read16(image, 56);
  for (std::uint16_t i = 0; i < phnum; ++i) {
    const std::uint64_t entry = phoff + static_cast<std::uint64_t>(i) * phentsize;
    PhdrView view;
    view.type = read32(image, entry);
    view.flags = read32(image, entry + 4);
    view.offset = read64(image, entry + 8);
    view.vaddr = read64(image, entry + 16);
    view.filesz = read64(image, entry + 32);
    view.memsz = read64(image, entry + 40);
    headers.push_back(view);
  }
  return headers;
}

static std::vector<std::pair<std::int64_t, std::uint64_t>> readDynamic(
    const std::vector<std::uint8_t>& image, const SectionView& dynamic) {
  std::vector<std::pair<std::int64_t, std::uint64_t>> tags;
  const std::uint64_t count = dynamic.size / 16;
  for (std::uint64_t i = 0; i < count; ++i) {
    const std::uint64_t entry = dynamic.offset + i * 16;
    const std::int64_t tag = static_cast<std::int64_t>(read64(image, entry));
    if (tag == 0) break;
    tags.emplace_back(tag, read64(image, entry + 8));
  }
  return tags;
}

// Dynamic symbol names, excluding the null entry.
static std::vector<std::string> readDynSymbols(
    const std::vector<std::uint8_t>& image,
    const std::vector<SectionView>& sections) {
  std::vector<std::string> names;
  const SectionView* dynsym = findSection(sections, ".dynsym");
  const SectionView* dynstr = findSection(sections, ".dynstr");
  if (dynsym == nullptr || dynstr == nullptr) return names;
  const std::uint64_t count = dynsym->size / 24;
  for (std::uint64_t i = 1; i < count; ++i) {
    const std::uint64_t entry = dynsym->offset + i * 24;
    names.push_back(readString(image, dynstr->offset + read32(image, entry)));
  }
  return names;
}

static std::vector<std::uint8_t> makeObject(
    const std::vector<std::uint8_t>& text,
    const std::vector<ExternalRef>& externals,
    const std::vector<ExportedSymbol>& exports) {
  return buildRelocatableObject(Target::linuxX64(), text, {}, externals, exports);
}

static void testTargets() {
  check(isOwnLinkerTarget(Target::linuxX64()), "the own linker accepts x86-64 Linux");
  check(isOwnLinkerTarget(Target::winX64()), "the own linker accepts Windows");
  check(!isOwnLinkerTarget(Target::macX64()), "the own linker rejects macOS");
  check(!isOwnLinkerTarget(Target::bareMetalX64()), "the own linker rejects bare metal");

  bool rejected = false;
  try {
    LinkOptions options;
    options.target = Target::macX64();
    linkHostedExecutable(std::vector<std::uint8_t>{}, options);
  } catch (const LinkError& error) {
    rejected = std::string(error.what()).find("x86_64-linux-sysv") != std::string::npos;
  }
  check(rejected, "a non-Linux target is reported before the object is read");
}

#ifdef __linux__

static void testMinimalImage() {
  const std::vector<std::uint8_t> text = {0xB8, 42, 0, 0, 0, 0xC3};  // mov eax,42; ret
  const std::vector<std::uint8_t> object =
      makeObject(text, {}, {{"main", 0}});
  const std::vector<std::uint8_t> image =
      linkHostedExecutable(object, LinkOptions{});

  check(image.size() > 64, "the linker produced an image");
  check(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F',
        "the image starts with the ELF magic");
  check(read16(image, 16) == 2, "e_type is ET_EXEC");
  check(read16(image, 18) == 62, "e_machine is EM_X86_64");

  const std::uint64_t entry = read64(image, 24);
  check(entry != 0, "the entry point is set");
  const std::vector<PhdrView> phdrs = readPhdrs(image);
  check(phdrs.size() == 7, "seven program headers (no .rodata)");

  bool hasInterp = false;
  bool entryMapped = false;
  for (const PhdrView& phdr : phdrs) {
    if (phdr.type == 3) {  // PT_INTERP
      hasInterp = true;
      check(readString(image, phdr.offset) == "/lib64/ld-linux-x86-64.so.2",
            "PT_INTERP names the x86-64 dynamic loader");
    }
    if (phdr.type == 1 && (phdr.flags & 1) != 0 && entry >= phdr.vaddr &&
        entry < phdr.vaddr + phdr.memsz) {
      entryMapped = true;
    }
  }
  check(hasInterp, "the image carries PT_INTERP");
  check(entryMapped, "the entry point lies in an executable PT_LOAD");

  const std::vector<SectionView> sections = readSections(image);
  const SectionView* dynamic = findSection(sections, ".dynamic");
  const SectionView* dynstr = findSection(sections, ".dynstr");
  check(dynamic != nullptr && dynstr != nullptr, "the image has .dynamic and .dynstr");
  if (dynamic == nullptr || dynstr == nullptr) return;

  const auto tags = readDynamic(image, *dynamic);
  const auto hasTag = [&](std::int64_t wanted) {
    for (const auto& tag : tags) {
      if (tag.first == wanted) return true;
    }
    return false;
  };
  check(hasTag(1), "DT_NEEDED is present");
  check(hasTag(4), "DT_HASH is present");
  check(hasTag(5), "DT_STRTAB is present");
  check(hasTag(6), "DT_SYMTAB is present");
  check(hasTag(3), "DT_PLTGOT is present");
  check(hasTag(23), "DT_JMPREL is present");
  check(hasTag(24), "DT_BIND_NOW is present");

  for (const auto& tag : tags) {
    if (tag.first == 1) {
      check(readString(image, dynstr->offset + tag.second) == "libc.so.6",
            "DT_NEEDED names libc.so.6");
    }
  }

  const std::vector<std::string> symbols = readDynSymbols(image, sections);
  bool hasStartMain = false;
  for (const std::string& name : symbols) {
    if (name == "__libc_start_main") hasStartMain = true;
  }
  check(hasStartMain, "__libc_start_main is a dynamic symbol");

  const SectionView* start = findSection(sections, ".text.start");
  check(start != nullptr && start->size == 34, "the linker emitted a 34-byte _start");
  if (start != nullptr && start->size == 34) {
    check(image[start->offset + 0] == 0x31 && image[start->offset + 1] == 0xED,
          "_start begins with xor ebp, ebp");
    check(image[start->offset + 20] == 0x48 && image[start->offset + 21] == 0x8D &&
              image[start->offset + 22] == 0x3D,
          "_start loads main with lea rdi, [rip + main]");
    check(image[start->offset + 27] == 0xFF && image[start->offset + 28] == 0x15,
          "_start calls __libc_start_main through the GOT");
    check(image[start->offset + 33] == 0xF4, "_start ends with hlt");
  }
}

static void testExternalCall() {
  const std::vector<std::uint8_t> text = {0xE8, 0, 0, 0, 0, 0xC3};  // call write; ret
  ExternalRef reference;
  reference.patchAt = 1;
  reference.symbol = "write";
  reference.call = true;
  const std::vector<std::uint8_t> object =
      makeObject(text, {reference}, {{"main", 0}});
  const std::vector<std::uint8_t> image =
      linkHostedExecutable(object, LinkOptions{});

  const std::vector<SectionView> sections = readSections(image);
  const SectionView* textSection = findSection(sections, ".text");
  const SectionView* plt = findSection(sections, ".plt");
  const SectionView* rela = findSection(sections, ".rela.plt");
  check(textSection != nullptr && plt != nullptr && rela != nullptr,
        "the external call produced .text, .plt and .rela.plt");
  if (textSection == nullptr || plt == nullptr || rela == nullptr) return;

  check(rela->size == 48,
        "one JUMP_SLOT for the external call plus the synthetic __libc_start_main");
  const std::uint64_t info = read64(image, rela->offset + 8);
  check((info & 0xFFFFFFFFu) == 7, "the relocation is R_X86_64_JUMP_SLOT");
  const std::uint32_t symbolIndex = static_cast<std::uint32_t>(info >> 32);
  const std::vector<std::string> symbols = readDynSymbols(image, sections);
  check(symbolIndex >= 1 && symbolIndex <= symbols.size() &&
            symbols[symbolIndex - 1] == "write",
        "the relocation names 'write'");

  // The call displacement must land on the first PLT entry (PLT0 is 16 bytes).
  const std::uint64_t field = textSection->addr + 1;
  const std::int32_t displacement =
      static_cast<std::int32_t>(read32(image, textSection->offset + 1));
  const std::uint64_t target = field + 4 + displacement;
  const std::uint64_t pltEntry = plt->addr + 16;
  check(target == pltEntry, "the call resolves to the PLT entry for 'write'");

  // The PLT entry is `jmp qword ptr [rip + got]`, and the GOT slot is the
  // relocation's r_offset.
  check(image[plt->offset + 16] == 0xFF && image[plt->offset + 17] == 0x25,
        "the PLT entry is an indirect jump");
  const std::int32_t pltDisplacement =
      static_cast<std::int32_t>(read32(image, plt->offset + 18));
  check(static_cast<std::uint64_t>(pltEntry + 6) +
                static_cast<std::int64_t>(pltDisplacement) ==
            read64(image, rela->offset),
        "the PLT entry reads the JUMP_SLOT's GOT slot");
}

static void testErrors() {
  bool missingMain = false;
  try {
    const std::vector<std::uint8_t> object =
        makeObject({0xC3}, {}, {{"not_main", 0}});
    linkHostedExecutable(object, LinkOptions{});
  } catch (const LinkError& error) {
    missingMain = std::string(error.what()).find("main") != std::string::npos;
  }
  check(missingMain, "an object without 'main' is rejected");

  bool truncated = false;
  try {
    linkHostedExecutable({0x7F, 'E'}, LinkOptions{});
  } catch (const LinkError&) {
    truncated = true;
  }
  check(truncated, "a truncated object is rejected");

  bool undefined = false;
  try {
    ExternalRef reference;
    reference.patchAt = 1;
    reference.symbol = "oskel_no_such_symbol_for_test";
    reference.call = true;
    const std::vector<std::uint8_t> object =
        makeObject({0xE8, 0, 0, 0, 0, 0xC3}, {reference}, {{"main", 0}});
    linkHostedExecutable(object, LinkOptions{});
  } catch (const LinkError& error) {
    undefined = std::string(error.what()).find("undefined symbol") != std::string::npos;
  }
  check(undefined, "an undefined symbol is reported at link time");
}

static std::vector<std::string> readStaticSymbols(
    const std::vector<std::uint8_t>& image,
    const std::vector<SectionView>& sections) {
  std::vector<std::string> names;
  const SectionView* symtab = findSection(sections, ".symtab");
  const SectionView* strtab = findSection(sections, ".strtab");
  if (symtab == nullptr || strtab == nullptr) return names;
  const std::uint64_t count = symtab->size / 24;
  for (std::uint64_t i = 0; i < count; ++i) {
    const std::uint64_t entry = symtab->offset + i * 24;
    names.push_back(readString(image, strtab->offset + read32(image, entry)));
  }
  return names;
}

static void appendArchiveField(std::vector<std::uint8_t>& archive,
                               const std::string& text, std::size_t width) {
  std::string field = text;
  if (field.size() > width) field.resize(width);
  field.resize(width, ' ');
  archive.insert(archive.end(), field.begin(), field.end());
}

static void appendArchiveMember(std::vector<std::uint8_t>& archive,
                                const std::string& name,
                                const std::vector<std::uint8_t>& data) {
  appendArchiveField(archive, name, 16);
  appendArchiveField(archive, "0", 12);
  appendArchiveField(archive, "0", 6);
  appendArchiveField(archive, "0", 6);
  appendArchiveField(archive, "100644", 8);
  appendArchiveField(archive, std::to_string(data.size()), 10);
  archive.push_back('`');
  archive.push_back('\n');
  archive.insert(archive.end(), data.begin(), data.end());
  if ((data.size() & 1u) != 0) archive.push_back('\n');
}

// A System V/GNU archive with no symbol index: the linker reads each member's
// own `.symtab`. A long name goes through the `//` table, as `ar` writes it.
static std::vector<std::uint8_t> makeArchive(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& members,
    bool longNames = false) {
  std::vector<std::uint8_t> archive = {'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
  if (longNames) {
    std::string table;
    for (const auto& member : members) table += member.first + "/\n";
    appendArchiveMember(archive, "//", std::vector<std::uint8_t>(table.begin(), table.end()));
    std::size_t offset = 0;
    for (const auto& member : members) {
      appendArchiveMember(archive, "/" + std::to_string(offset), member.second);
      offset += member.first.size() + 2;  // name + "/\n"
    }
  } else {
    for (const auto& member : members) {
      appendArchiveMember(archive, member.first + "/", member.second);
    }
  }
  return archive;
}

static std::vector<std::uint8_t> makeCallingObject(const std::string& symbol) {
  ExternalRef reference;
  reference.patchAt = 1;
  reference.symbol = symbol;
  reference.call = true;
  return makeObject({0xE8, 0, 0, 0, 0, 0xC3}, {reference}, {{"main", 0}});
}

static void testMultipleObjects() {
  const std::vector<std::uint8_t> mainObject = makeCallingObject("helper");
  const std::vector<std::uint8_t> helperObject =
      makeObject({0xB8, 42, 0, 0, 0, 0xC3}, {}, {{"helper", 0}});

  const std::vector<std::uint8_t> image = linkHostedExecutable(
      std::vector<LinkInput>{{"main.o", mainObject}, {"helper.o", helperObject}},
      LinkOptions{});
  check(read16(image, 16) == 2, "two objects link into an ET_EXEC");

  const std::vector<SectionView> sections = readSections(image);
  const SectionView* text = findSection(sections, ".text");
  const SectionView* rela = findSection(sections, ".rela.plt");
  check(text != nullptr && text->size == 22,
        "the output .text concatenates both objects with 16-byte alignment");
  check(rela != nullptr && rela->size == 24,
        "a cross-object call resolves without a PLT entry");
  if (text != nullptr) {
    const std::int32_t displacement =
        static_cast<std::int32_t>(read32(image, text->offset + 1));
    const std::uint64_t target =
        text->addr + 5 + static_cast<std::int64_t>(displacement);
    check(target == text->addr + 16,
          "the call lands on the second object's helper");
  }

  bool duplicate = false;
  try {
    const std::vector<std::uint8_t> otherMain =
        makeObject({0xB8, 7, 0, 0, 0, 0xC3}, {}, {{"main", 0}});
    linkHostedExecutable(
        std::vector<LinkInput>{{"a.o", mainObject}, {"b.o", otherMain}},
        LinkOptions{});
  } catch (const LinkError& error) {
    duplicate = std::string(error.what()).find("duplicate symbol") != std::string::npos;
  }
  check(duplicate, "a duplicate global symbol across objects is rejected");
}

static void testArchive() {
  const std::vector<std::uint8_t> mainObject = makeCallingObject("helper");
  const std::vector<std::uint8_t> helperObject =
      makeObject({0xB8, 42, 0, 0, 0, 0xC3}, {}, {{"helper", 0}});
  // If this member were pulled in, its unresolved `missing` would fail the link.
  const std::vector<std::uint8_t> unusedObject =
      makeCallingObject("missing");

  const std::vector<std::uint8_t> archive = makeArchive(
      {{"helper.o", helperObject}, {"unused.o", unusedObject}});
  const std::vector<std::uint8_t> image = linkHostedExecutable(
      std::vector<LinkInput>{{"main.o", mainObject}, {"libhelper.a", archive}},
      LinkOptions{});
  check(read16(image, 16) == 2, "an archive member is extracted into an ET_EXEC");
  const std::vector<SectionView> sections = readSections(image);
  const SectionView* rela = findSection(sections, ".rela.plt");
  check(rela != nullptr && rela->size == 24,
        "the extracted member resolves the call without a PLT entry");

  const std::vector<std::uint8_t> longNameArchive = makeArchive(
      {{"a_very_long_member_name_helper.o", helperObject}}, true);
  const std::vector<std::uint8_t> longNameImage = linkHostedExecutable(
      std::vector<LinkInput>{{"main.o", mainObject},
                             {"liblong.a", longNameArchive}},
      LinkOptions{});
  check(read16(longNameImage, 16) == 2,
        "a long archive member name is resolved through the // table");

  bool missing = false;
  try {
    const std::vector<std::uint8_t> incomplete =
        makeArchive({{"unused.o", unusedObject}});
    linkHostedExecutable(
        std::vector<LinkInput>{{"main.o", mainObject}, {"libbad.a", incomplete}},
        LinkOptions{});
  } catch (const LinkError& error) {
    missing = std::string(error.what()).find("undefined symbol") != std::string::npos;
  }
  check(missing, "an archive that does not define the symbol is reported");
}

static void testRelocatable() {
  const std::vector<std::uint8_t> mainObject = makeCallingObject("helper");
  const std::vector<std::uint8_t> helperObject =
      makeObject({0xB8, 42, 0, 0, 0, 0xC3}, {}, {{"helper", 0}});

  const std::vector<std::uint8_t> combined = linkRelocatable(
      std::vector<LinkInput>{{"main.o", mainObject}, {"helper.o", helperObject}},
      LinkOptions{});
  check(read16(combined, 16) == 1, "linkRelocatable emits ET_REL");
  check(read16(combined, 56) == 0, "a relocatable object has no program headers");

  const std::vector<SectionView> sections = readSections(combined);
  const SectionView* text = findSection(sections, ".text");
  const SectionView* rela = findSection(sections, ".rela.text");
  const SectionView* symtab = findSection(sections, ".symtab");
  check(text != nullptr && text->size == 22,
        "the relocatable .text concatenates both objects");
  check(rela != nullptr && rela->size == 24,
        "the cross-object relocation is preserved");
  check(symtab != nullptr, "the relocatable object carries a .symtab");

  const std::vector<std::string> names = readStaticSymbols(combined, sections);
  bool hasMain = false;
  bool hasHelper = false;
  std::size_t helperIndex = 0;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (names[i] == "main") hasMain = true;
    if (names[i] == "helper") { hasHelper = true; helperIndex = i; }
  }
  check(hasMain && hasHelper, "main and helper are in the relocatable .symtab");
  if (rela != nullptr) {
    const std::uint64_t info = read64(combined, rela->offset + 8);
    check((info >> 32) == helperIndex,
          "the preserved relocation names the defined helper");
  }

  // The result is itself a valid linker input.
  const std::vector<std::uint8_t> image =
      linkHostedExecutable(combined, LinkOptions{});
  check(read16(image, 16) == 2,
        "the relocatable output can be linked into an executable");

  // An external reference survives `-r` and resolves in the final link.
  const std::vector<std::uint8_t> externalMain = makeCallingObject("write");
  const std::vector<std::uint8_t> externalCombined =
      linkRelocatable(std::vector<LinkInput>{{"write.o", externalMain}},
                      LinkOptions{});
  const std::vector<SectionView> externalSections = readSections(externalCombined);
  const std::vector<std::string> externalNames =
      readStaticSymbols(externalCombined, externalSections);
  bool hasWrite = false;
  for (const std::string& name : externalNames) {
    if (name == "write") hasWrite = true;
  }
  check(hasWrite, "an undefined symbol stays undefined across -r");
  const std::vector<std::uint8_t> externalImage =
      linkHostedExecutable(externalCombined, LinkOptions{});
  check(read16(externalImage, 16) == 2,
        "the final link resolves the preserved external symbol");

  // A relocatable link also accepts an archive.
  const std::vector<std::uint8_t> archive = makeArchive(
      {{"helper.o", helperObject}});
  const std::vector<std::uint8_t> archived = linkRelocatable(
      std::vector<LinkInput>{{"main.o", mainObject}, {"libhelper.a", archive}},
      LinkOptions{});
  check(read16(archived, 16) == 1, "linkRelocatable pulls an archive member");
}

#endif  // __linux__

int main() {
  testTargets();
#ifdef __linux__
  testMinimalImage();
  testExternalCall();
  testErrors();
  testMultipleObjects();
  testArchive();
  testRelocatable();
#else
  std::printf("skipping image checks: the own linker targets x86-64 Linux\n");
#endif
  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all linker checks passed\n");
  return 0;
}
