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
  }
  const std::size_t dysymtab = findCommand(pe, 0xB);
  check(dysymtab != 0 && read32(pe, dysymtab + 60) == 2, "two indirect symbols");
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

}  // namespace

int main() {
  testHello();
  testRelocatable();
  return failures == 0 ? 0 : 1;
}
