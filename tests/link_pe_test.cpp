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

std::uint16_t read16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  if (offset + 2 > bytes.size()) return 0;
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

std::uint32_t read32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  if (offset + 4 > bytes.size()) return 0;
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
  }
  return value;
}

std::uint64_t read64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  if (offset + 8 > bytes.size()) return 0;
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
  }
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

std::size_t findSection(const std::vector<std::uint8_t>& pe, const char* name) {
  const std::size_t sectionCount = read16(pe, 0x46);
  const std::size_t table = 0x58 + 240;
  for (std::size_t i = 0; i < sectionCount; ++i) {
    const std::size_t base = table + i * 40;
    if (base + 40 > pe.size()) return 0;
    if (std::strncmp(reinterpret_cast<const char*>(pe.data() + base), name,
                     std::strlen(name)) == 0) {
      return base;
    }
  }
  return 0;
}

void testRelocatable() {
  // main: call helper; ret. The call field is at offset 1.
  const std::vector<std::uint8_t> mainText = {0xE8, 0x00, 0x00, 0x00,
                                              0x00, 0xC3};
  const std::vector<std::uint8_t> helperText = {0xB8, 42, 0x00, 0x00,
                                                0x00, 0xC3};
  const std::vector<std::uint8_t> mainObject = buildRelocatableObject(
      Target::winX64(), mainText, {}, {{1, "helper", true}}, {{"main", 0}});
  const std::vector<std::uint8_t> helperObject = buildRelocatableObject(
      Target::winX64(), helperText, {}, {}, {{"helper", 0}});

  LinkOptions options;
  options.target = Target::winX64();
  const std::vector<std::uint8_t> combined = linkRelocatable(
      std::vector<LinkInput>{{"main.obj", mainObject},
                             {"helper.obj", helperObject}},
      options);
  check(combined.size() > 20, "-r produced a COFF object");
  check(read16(combined, 0) == 0x8664, "-r machine is AMD64");
  check(read16(combined, 2) >= 1, "-r has sections");

  const std::vector<std::uint8_t> pe = linkHostedExecutable(
      std::vector<LinkInput>{{"combined.obj", combined}}, options);
  const std::size_t textSection = findSection(pe, ".text");
  check(textSection != 0, "the -r output links into a PE");
  if (textSection == 0) return;
  const std::uint32_t textOffset = read32(pe, textSection + 20);
  const std::int32_t displacement =
      static_cast<std::int32_t>(read32(pe, textOffset + 1));
  check(displacement == 11,
        "the -r output preserves the call relocation to helper");
  check(pe[textOffset + 16] == 0xB8 && pe[textOffset + 17] == 42,
        "the helper body is at its merged offset");
}


void appendArchiveMember(std::vector<std::uint8_t>& archive,
                         const std::string& name,
                         const std::vector<std::uint8_t>& data) {
  const std::size_t header = archive.size();
  archive.resize(header + 60, ' ');
  std::memcpy(archive.data() + header, name.data(),
              std::min<std::size_t>(name.size(), 16));
  const std::string size = std::to_string(data.size());
  std::memcpy(archive.data() + header + 48, size.data(),
              std::min<std::size_t>(size.size(), 10));
  archive[header + 58] = '`';
  archive[header + 59] = '\n';
  archive.insert(archive.end(), data.begin(), data.end());
  if ((data.size() & 1) != 0) archive.push_back('\n');
}

std::vector<std::uint8_t> buildCoffArchive(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& members) {
  std::vector<std::uint8_t> archive = {'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
  const std::vector<std::uint8_t> emptyLinker = {0, 0, 0, 0};
  appendArchiveMember(archive, "/", emptyLinker);
  appendArchiveMember(archive, "/", emptyLinker);
  std::string longNames;
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> longMembers;
  for (const auto& member : members) {
    longMembers.push_back({std::to_string(longNames.size()), member.second});
    longNames += member.first;
    longNames += '\0';
  }
  appendArchiveMember(archive, "//",
                      std::vector<std::uint8_t>(longNames.begin(), longNames.end()));
  for (const auto& member : longMembers) {
    appendArchiveMember(archive, "/" + member.first, member.second);
  }
  return archive;
}

void testArchive() {
  const std::vector<std::uint8_t> mainText = {0xE8, 0x00, 0x00, 0x00,
                                              0x00, 0xC3};
  const std::vector<std::uint8_t> helperText = {0xB8, 42, 0x00, 0x00,
                                                0x00, 0xC3};
  const std::vector<std::uint8_t> mainObject = buildRelocatableObject(
      Target::winX64(), mainText, {}, {{1, "helper", true}}, {{"main", 0}});
  const std::vector<std::uint8_t> helperObject = buildRelocatableObject(
      Target::winX64(), helperText, {}, {}, {{"helper", 0}});
  const std::vector<std::uint8_t> archive = buildCoffArchive(
      {{"helper_object_with_a_long_name.obj", helperObject}});
  LinkOptions options;
  options.target = Target::winX64();

  const std::vector<std::uint8_t> pe = linkHostedExecutable(
      std::vector<LinkInput>{{"main.obj", mainObject},
                             {"helper.lib", archive}},
      options);
  const std::size_t textSection = findSection(pe, ".text");
  check(textSection != 0, "a .lib member links into a PE");
  if (textSection == 0) return;
  const std::uint32_t textOffset = read32(pe, textSection + 20);
  check(static_cast<std::int32_t>(read32(pe, textOffset + 1)) == 11,
        "the .lib member resolves the call");
  check(pe[textOffset + 16] == 0xB8 && pe[textOffset + 17] == 42,
        "the .lib helper body is linked");

  const std::vector<std::uint8_t> combined = linkRelocatable(
      std::vector<LinkInput>{{"main.obj", mainObject},
                             {"helper.lib", archive}},
      options);
  const std::vector<std::uint8_t> relocPe = linkHostedExecutable(
      std::vector<LinkInput>{{"combined.obj", combined}}, options);
  const std::size_t relocText = findSection(relocPe, ".text");
  check(relocText != 0, "a .lib member combines with -r");
  if (relocText != 0) {
    const std::uint32_t relocOffset = read32(relocPe, relocText + 20);
    check(static_cast<std::int32_t>(read32(relocPe, relocOffset + 1)) == 11,
          "-r resolves a .lib member");
  }
}

}  // namespace

int main() {
  // A minimal `main`: sub rsp, 0x28; call _write; add rsp, 0x28; ret.
  const std::vector<std::uint8_t> text = {
      0x48, 0x83, 0xEC, 0x28, 0xE8, 0x00, 0x00, 0x00,
      0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3};
  std::vector<ExternalRef> externals;
  externals.push_back(ExternalRef{5, "_write", true});
  std::vector<ExportedSymbol> exports;
  exports.push_back(ExportedSymbol{"main", 0});

  const std::vector<std::uint8_t> object = buildRelocatableObject(
      Target::winX64(), text, {}, externals, exports);
  LinkOptions options;
  options.target = Target::winX64();
  const std::vector<std::uint8_t> pe = linkHostedExecutable(
      std::vector<LinkInput>{{"main.obj", object}}, options);

  check(pe.size() > 0x200, "the PE image has a header and sections");
  check(read16(pe, 0) == 0x5A4D, "DOS magic is MZ");
  check(read32(pe, 0x3C) == 0x40, "e_lfanew points at the PE signature");
  check(read32(pe, 0x40) == 0x00004550, "PE signature is present");
  check(read16(pe, 0x44) == 0x8664, "machine is AMD64");
  check(read16(pe, 0x58) == 0x020B, "optional header is PE32+");
  check(read64(pe, 0x58 + 24) == 0x140000000, "image base is 0x140000000");
  check(read16(pe, 0x58 + 68) == 3, "subsystem is console");
  check(read32(pe, 0x58 + 16) != 0, "the image has an entry point");

  const std::size_t idata = findSection(pe, ".idata");
  const std::size_t textSection = findSection(pe, ".text");
  check(idata != 0, ".idata section is present");
  check(textSection != 0, ".text section is present");
  if (idata == 0 || textSection == 0) return 1;

  const std::uint32_t idataRva = read32(pe, idata + 12);
  const std::uint32_t idataSize = read32(pe, idata + 8);
  const std::uint32_t idataOffset = read32(pe, idata + 20);
  const std::uint32_t textRva = read32(pe, textSection + 12);
  const std::uint32_t textSize = read32(pe, textSection + 8);
  const std::uint32_t textOffset = read32(pe, textSection + 20);
  const std::uint32_t entry = read32(pe, 0x58 + 16);

  check(idataRva == 0x1000, ".idata is mapped at 0x1000");
  check(entry >= textRva && entry < textRva + textSize,
        "the entry point lies in .text");
  check(read32(pe, 0x58 + 112 + 8) == idataRva,
        "the import directory points at .idata");
  check(read32(pe, 0x58 + 112 + 12) != 0, "the import directory has a size");
  check(read32(pe, 0x58 + 112 + 12 * 8) != 0, "the IAT directory is present");

  check(contains(pe, idataOffset, idataSize, "_write"), "imports _write");
  check(contains(pe, idataOffset, idataSize, "ExitProcess"),
        "imports ExitProcess");
  check(contains(pe, idataOffset, idataSize, "__getmainargs"),
        "imports __getmainargs");
  check(contains(pe, idataOffset, idataSize, "ucrtbase.dll"),
        "imports ucrtbase.dll");
  check(contains(pe, idataOffset, idataSize, "kernel32.dll"),
        "imports kernel32.dll");
  check(contains(pe, idataOffset, idataSize, "msvcrt.dll"),
        "imports msvcrt.dll");

  // The first thunk follows the 14-byte program text.
  check(textSize >= 14 + 6 * 3 + 88, ".text holds the thunks and _start");
  check(pe[textOffset + 14] == 0xFF && pe[textOffset + 15] == 0x25,
        "the first import thunk is jmp [rip + IAT]");

  const std::uint32_t startOffset = entry - textRva;
  check(startOffset + 88 <= textSize, "_start lies inside .text");
  check(pe[textOffset + startOffset] == 0x48 &&
            pe[textOffset + startOffset + 1] == 0x83 &&
            pe[textOffset + startOffset + 2] == 0xEC,
        "_start aligns the stack");
  check(pe[textOffset + startOffset + 4] == 0x48 &&
            pe[textOffset + startOffset + 5] == 0x83 &&
            pe[textOffset + startOffset + 6] == 0xEC,
        "_start reserves the argument frame");
  std::size_t mainCall = 0;
  for (std::size_t i = startOffset; i + 5 <= textSize; ++i) {
    if (pe[textOffset + i] != 0xE8) continue;
    const std::int64_t target = static_cast<std::int64_t>(i) + 5 +
        static_cast<std::int32_t>(read32(pe, textOffset + i + 1));
    if (target == 0) {
      mainCall = i;
      break;
    }
  }
  check(mainCall != 0, "_start calls main");
  check(mainCall != 0 &&
            static_cast<std::int64_t>(mainCall) + 5 +
                static_cast<std::int32_t>(read32(pe, textOffset + mainCall + 1)) == 0,
        "the _start call lands on main");
  bool exitCall = false;
  if (mainCall != 0) {
    for (std::size_t i = mainCall + 5; i + 6 <= textSize && i < mainCall + 24; ++i) {
      if (pe[textOffset + i] == 0xFF && pe[textOffset + i + 1] == 0x15) {
        exitCall = true;
        break;
      }
    }
  }
  check(exitCall, "_start calls ExitProcess through the IAT");

  testRelocatable();
  testArchive();
  return failures == 0 ? 0 : 1;
}
