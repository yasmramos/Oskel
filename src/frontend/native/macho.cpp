#include "native/macho.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint32_t kMagic64 = 0xFEEDFACF;
constexpr std::uint32_t kCpuX86_64 = 0x01000007;
constexpr std::uint32_t kCpuSubtypeX86_64All = 3;
constexpr std::uint32_t kFileTypeObject = 1;
constexpr std::uint32_t kFileTypeExecute = 2;
constexpr std::uint32_t kSegment64 = 0x19;
constexpr std::uint32_t kSymtab = 0x2;
constexpr std::uint32_t kDysymtab = 0xB;
constexpr std::uint32_t kLoadDylib = 0xC;
constexpr std::uint32_t kLoadDylinker = 0xE;
constexpr std::uint32_t kUuid = 0x1B;
constexpr std::uint32_t kBuildVersion = 0x32;
constexpr std::uint32_t kMain = 0x80000028;
constexpr std::uint32_t kDyldInfoOnly = 0x80000022;
constexpr std::uint32_t kHeaderSize = 32;
constexpr std::uint32_t kSegmentCommandSize = 72;
constexpr std::uint32_t kSectionSize = 80;
constexpr std::uint32_t kPageSize = 0x1000;
constexpr std::uint64_t kTextVmaddr = 0x100000000;
constexpr std::uint32_t kSectionTypeNonLazySymbolPointers = 6;
constexpr std::uint32_t kSectionTypeSymbolStubs = 8;
constexpr std::uint32_t kSectionAttributesPureInstructions = 0x80000000;
constexpr std::uint32_t kSectionAttributesSomeInstructions = 0x400;
constexpr std::uint32_t kTextAttributes =
    kSectionAttributesPureInstructions | kSectionAttributesSomeInstructions;
constexpr std::uint32_t kStubSize = 6;
constexpr std::uint32_t kPlatformMacOs = 1;
constexpr std::uint32_t kMacOs10_15 = (10u << 16) | (15u << 8);
constexpr std::uint32_t kMhFlags = 0x4 | 0x80;  // MH_DYLDLINK | MH_TWOLEVEL
constexpr std::uint8_t kNExt = 0x01;
constexpr std::uint8_t kNUndef = 0x00;
constexpr std::uint8_t kNSect = 0x0E;
constexpr std::uint8_t kRelocBranch = 2;

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

void put16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value) {
  out[offset] = static_cast<std::uint8_t>(value);
  out[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void put64(std::vector<std::uint8_t>& out, std::size_t offset, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void appendFixedName(std::vector<std::uint8_t>& out, const std::string& name,
                     std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    out.push_back(i < name.size() ? static_cast<std::uint8_t>(name[i]) : 0);
  }
}

void padTo(std::vector<std::uint8_t>& out, std::size_t alignment) {
  while (out.size() % alignment != 0) out.push_back(0);
}

class Reader {
 public:
  explicit Reader(const std::vector<std::uint8_t>& data) : data_(data) {}
  std::uint8_t u8(std::uint64_t offset) const { check(offset, 1); return data_[offset]; }
  std::uint16_t u16(std::uint64_t offset) const {
    check(offset, 2);
    return static_cast<std::uint16_t>(data_[offset] | (data_[offset + 1] << 8));
  }
  std::uint32_t u32(std::uint64_t offset) const {
    check(offset, 4);
    return static_cast<std::uint32_t>(data_[offset]) |
           (static_cast<std::uint32_t>(data_[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(data_[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(data_[offset + 3]) << 24);
  }
  std::uint64_t u64(std::uint64_t offset) const {
    check(offset, 8);
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<std::uint64_t>(data_[offset + i]) << (8 * i);
    }
    return value;
  }
  std::string string(std::uint64_t offset) const {
    std::string value;
    while (offset < data_.size() && data_[offset] != 0) {
      value.push_back(static_cast<char>(data_[offset++]));
    }
    return value;
  }
  std::vector<std::uint8_t> bytes(std::uint64_t offset, std::uint64_t size) const {
    check(offset, size);
    return std::vector<std::uint8_t>(data_.begin() + offset, data_.begin() + offset + size);
  }

 private:
  void check(std::uint64_t offset, std::uint64_t size) const {
    if (offset > data_.size() || size > data_.size() - offset) {
      throw LinkError("truncated Mach-O data");
    }
  }
  const std::vector<std::uint8_t>& data_;
};

struct MachReloc {
  std::uint32_t address = 0;
  std::string symbol;
  bool branch = false;
};

struct MachObject {
  std::vector<std::uint8_t> text;
  std::vector<std::uint8_t> rodata;
  std::vector<MachReloc> relocs;
  std::vector<std::string> imports;
  std::uint32_t entryOffset = 0;
};

MachObject parseMachObject(const LinkInput& input) {
  const Reader reader(input.bytes);
  if (reader.u32(0) != kMagic64) {
    throw LinkError("'" + input.name + "' is not a 64-bit Mach-O object");
  }
  if (reader.u32(12) != kFileTypeObject) {
    throw LinkError("'" + input.name + "' is not an MH_OBJECT");
  }
  const std::uint32_t commandCount = reader.u32(16);
  std::uint64_t command = kHeaderSize;
  std::uint64_t textOffset = 0;
  std::uint64_t textSize = 0;
  std::uint64_t constOffset = 0;
  std::uint64_t constSize = 0;
  std::uint64_t relocOffset = 0;
  std::uint32_t relocCount = 0;
  std::uint64_t symOffset = 0;
  std::uint64_t symCount = 0;
  std::uint64_t strOffset = 0;
  bool haveText = false;
  for (std::uint32_t i = 0; i < commandCount; ++i) {
    const std::uint32_t cmd = reader.u32(command);
    const std::uint32_t size = reader.u32(command + 4);
    if (size < 8 || command + size > input.bytes.size()) {
      throw LinkError("'" + input.name + "' has a malformed load command");
    }
    if (cmd == kSegment64) {
      const std::uint32_t sectionCount = reader.u32(command + 64);
      std::uint64_t section = command + kSegmentCommandSize;
      for (std::uint32_t s = 0; s < sectionCount; ++s) {
        const std::string sectionName = reader.string(section);
        const std::string segmentName = reader.string(section + 16);
        const std::uint32_t offset = reader.u32(section + 48);
        const std::uint32_t bytes = reader.u32(section + 40);
        if (segmentName == "__TEXT" && sectionName == "__text") {
          textOffset = offset;
          textSize = bytes;
          relocOffset = reader.u32(section + 56);
          relocCount = reader.u32(section + 60);
          haveText = true;
        } else if (segmentName == "__TEXT" && sectionName == "__const") {
          constOffset = offset;
          constSize = bytes;
        }
        section += kSectionSize;
      }
    } else if (cmd == kSymtab) {
      symOffset = reader.u32(command + 8);
      symCount = reader.u32(command + 12);
      strOffset = reader.u32(command + 16);
    }
    command += size;
  }
  if (!haveText) throw LinkError("'" + input.name + "' has no __text section");
  MachObject object;
  object.text = reader.bytes(textOffset, textSize);
  if (constSize != 0) object.rodata = reader.bytes(constOffset, constSize);
  std::vector<std::string> names;
  std::vector<bool> defined;
  names.reserve(static_cast<std::size_t>(symCount));
  defined.reserve(static_cast<std::size_t>(symCount));
  for (std::uint64_t i = 0; i < symCount; ++i) {
    const std::uint64_t base = symOffset + i * 16;
    const std::uint32_t nameOffset = reader.u32(base);
    const std::uint8_t type = reader.u8(base + 4);
    const std::uint8_t section = reader.u8(base + 5);
    const std::uint64_t value = reader.u64(base + 8);
    std::string name = nameOffset == 0 ? std::string() : reader.string(strOffset + nameOffset);
    if (!name.empty() && name[0] == '_') name.erase(name.begin());
    names.push_back(name);
    const bool isDefined = (type & kNSect) != 0;
    defined.push_back(isDefined);
    if (isDefined && name == "main") {
      if (section != 1) throw LinkError("'main' is not in __text");
      object.entryOffset = static_cast<std::uint32_t>(value);
    }
  }
  for (std::uint64_t i = 0; i < relocCount; ++i) {
    const std::uint64_t base = relocOffset + i * 8;
    const std::uint32_t address = reader.u32(base);
    const std::uint32_t info = reader.u32(base + 4);
    const std::uint32_t symbolIndex = info & 0x00FFFFFF;
    const bool external = ((info >> 27) & 1) != 0;
    const std::uint8_t type = static_cast<std::uint8_t>(info >> 28);
    if (!external) throw LinkError("the Mach-O linker does not support internal relocations yet");
    if (symbolIndex >= names.size()) throw LinkError("a Mach-O relocation names an out-of-range symbol");
    if (type != kRelocBranch) {
      throw LinkError("the Mach-O linker does not support external data imports yet: '" +
                      names[symbolIndex] + "'");
    }
    MachReloc reloc;
    reloc.address = address;
    reloc.symbol = names[symbolIndex];
    reloc.branch = true;
    object.relocs.push_back(reloc);
  }
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (!defined[i] && !names[i].empty() &&
        std::find(object.imports.begin(), object.imports.end(), names[i]) ==
            object.imports.end()) {
      object.imports.push_back(names[i]);
    }
  }
  return object;
}

}  // namespace

std::vector<std::uint8_t> linkMachOExecutable(
    const std::vector<LinkInput>& inputs, const LinkOptions& options) {
  if (inputs.empty()) throw LinkError("no input objects");
  if (inputs.size() != 1) {
    throw LinkError("the Mach-O linker takes one object for now");
  }
  if (options.target.arch != Arch::X86_64 || options.target.os != Os::MacOs) {
    throw LinkError("the Mach-O linker supports x86_64-macos-sysv only");
  }
  const MachObject object = parseMachObject(inputs[0]);
  const std::vector<std::string>& imports = object.imports;
  const bool hasConst = !object.rodata.empty();
  const bool hasImports = !imports.empty();
  const std::string dylinkerName = "/usr/lib/dyld";
  const std::string dylibName = "/usr/lib/libSystem.B.dylib";

  const std::uint32_t textSections = 1u + (hasConst ? 1u : 0u) + (hasImports ? 1u : 0u);
  const std::uint32_t dataSections = hasImports ? 1u : 0u;
  const std::uint32_t dylinkerSize =
      static_cast<std::uint32_t>(alignUp(12 + dylinkerName.size() + 1, 8));
  const std::uint32_t dylibSize =
      static_cast<std::uint32_t>(alignUp(24 + dylibName.size() + 1, 8));
  const std::uint32_t sizeofcmds =
      kSegmentCommandSize + (kSegmentCommandSize + kSectionSize * textSections) +
      (kSegmentCommandSize + kSectionSize * dataSections) + kSegmentCommandSize + 24 +
      dylinkerSize + dylibSize + 24 + 24 + 24 + 80 + 48;

  const std::uint64_t textFileOffset = alignUp(kHeaderSize + sizeofcmds, 16);
  const std::uint64_t constFileOffset =
      hasConst ? alignUp(textFileOffset + object.text.size(), 8) : 0;
  const std::uint64_t stubsFileOffset =
      hasImports
          ? alignUp(hasConst ? constFileOffset + object.rodata.size()
                             : textFileOffset + object.text.size(), 8)
          : 0;
  const std::uint64_t stubsSize = imports.size() * kStubSize;
  const std::uint64_t gotSize = imports.size() * 8;
  const std::uint64_t textFileSize = alignUp(
      hasImports ? stubsFileOffset + stubsSize
                 : hasConst ? constFileOffset + object.rodata.size()
                            : textFileOffset + object.text.size(),
      kPageSize);
  const std::uint64_t dataFileOffset = textFileSize;
  const std::uint64_t dataFileSize = hasImports ? alignUp(gotSize, kPageSize) : 0;
  const std::uint64_t dataVmaddr = kTextVmaddr + textFileSize;
  const std::uint64_t linkeditFileOffset = dataFileOffset + dataFileSize;
  const std::uint64_t linkeditVmaddr = dataVmaddr + dataFileSize;

  std::vector<std::uint8_t> strtab(1, 0);
  const auto addName = [&](const std::string& name) -> std::uint32_t {
    const std::uint32_t offset = static_cast<std::uint32_t>(strtab.size());
    strtab.insert(strtab.end(), name.begin(), name.end());
    strtab.push_back(0);
    return offset;
  };
  struct Sym {
    std::uint32_t name;
    std::uint8_t type;
    std::uint8_t sect;
    std::uint16_t desc;
    std::uint64_t value;
  };
  std::vector<Sym> symbols;
  symbols.push_back(Sym{0, 0, 0, 0, 0});
  symbols.push_back(Sym{addName("__mh_execute_header"), kNExt | kNSect, 1, 0, kTextVmaddr});
  symbols.push_back(Sym{addName("_main"), kNExt | kNSect, 1, 0,
                        kTextVmaddr + textFileOffset + object.entryOffset});
  for (const std::string& import : imports) {
    symbols.push_back(Sym{addName("_" + import), kNExt | kNUndef, 0, 1, 0});
  }
  std::vector<std::uint8_t> symtab(symbols.size() * 16, 0);
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    const std::size_t base = i * 16;
    put32(symtab, base, symbols[i].name);
    symtab[base + 4] = symbols[i].type;
    symtab[base + 5] = symbols[i].sect;
    put16(symtab, base + 6, symbols[i].desc);
    put64(symtab, base + 8, symbols[i].value);
  }

  std::vector<std::uint8_t> indirect(imports.size() * 8, 0);
  for (std::size_t i = 0; i < imports.size(); ++i) {
    const std::uint32_t symbolIndex = static_cast<std::uint32_t>(3 + i);
    put32(indirect, i * 4, symbolIndex);
    put32(indirect, (imports.size() + i) * 4, symbolIndex);
  }
  std::vector<std::uint8_t> bind;
  if (hasImports) {
    for (std::size_t i = 0; i < imports.size(); ++i) {
      bind.push_back(0x11);  // SET_DYLIB_ORDINAL_IMM, libSystem
      bind.push_back(0x40);  // SET_SYMBOL_TRAILING_FLAGS_IMM
      const std::string name = "_" + imports[i];
      bind.insert(bind.end(), name.begin(), name.end());
      bind.push_back(0);
      bind.push_back(0x51);  // SET_TYPE_IMM, pointer
      bind.push_back(0x32);  // SET_SEGMENT_AND_OFFSET_ULEB, __DATA
      std::uint64_t value = i * 8;
      do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7F);
        value >>= 7;
        if (value != 0) byte |= 0x80;
        bind.push_back(byte);
      } while (value != 0);
      bind.push_back(0x90);  // DO_BIND
    }
    bind.push_back(0x00);  // DONE
  }

  std::vector<std::uint8_t> text = object.text;
  std::unordered_map<std::string, std::size_t> importIndex;
  for (std::size_t i = 0; i < imports.size(); ++i) importIndex.emplace(imports[i], i);
  for (const MachReloc& reloc : object.relocs) {
    const auto found = importIndex.find(reloc.symbol);
    if (found == importIndex.end()) {
      throw LinkError("unresolved Mach-O symbol '" + reloc.symbol + "'");
    }
    const std::uint64_t stub =
        kTextVmaddr + stubsFileOffset + found->second * kStubSize;
    const std::uint64_t field = kTextVmaddr + textFileOffset + reloc.address;
    const std::int32_t displacement =
        static_cast<std::int32_t>(stub - (field + 4));
    put32(text, reloc.address, static_cast<std::uint32_t>(displacement));
  }
  std::vector<std::uint8_t> stubs(stubsSize, 0);
  for (std::size_t i = 0; i < imports.size(); ++i) {
    const std::uint64_t stub = kTextVmaddr + stubsFileOffset + i * kStubSize;
    const std::uint64_t slot = dataVmaddr + i * 8;
    const std::int32_t displacement =
        static_cast<std::int32_t>(slot - (stub + kStubSize));
    stubs[i * kStubSize] = 0xFF;
    stubs[i * kStubSize + 1] = 0x25;
    put32(stubs, i * kStubSize + 2, static_cast<std::uint32_t>(displacement));
  }
  const std::vector<std::uint8_t> got(gotSize, 0);
  const std::uint64_t symoff = linkeditFileOffset;
  const std::uint64_t stroff = symoff + symtab.size();
  const std::uint64_t indirectsymoff = stroff + strtab.size();
  const std::uint64_t bindoff = indirectsymoff + indirect.size();
  const std::uint64_t linkeditSize = bindoff + bind.size() - linkeditFileOffset;
  const std::uint64_t linkeditVmsize = alignUp(linkeditSize, kPageSize);

  std::vector<std::uint8_t> out(static_cast<std::size_t>(linkeditFileOffset + linkeditSize), 0);
  put32(out, 0, kMagic64);
  put32(out, 4, kCpuX86_64);
  put32(out, 8, kCpuSubtypeX86_64All);
  put32(out, 12, kFileTypeExecute);
  put32(out, 16, 12);  // ncmds
  put32(out, 20, sizeofcmds);
  put32(out, 24, kMhFlags);
  put32(out, 28, 0);
  const auto writeName = [&](std::size_t offset, const std::string& name, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
      out[offset + i] = i < name.size() ? static_cast<std::uint8_t>(name[i]) : 0;
    }
  };
  std::size_t cmd = kHeaderSize;
  const auto segment = [&](const std::string& name, std::uint64_t vmaddr,
                           std::uint64_t vmsize, std::uint64_t fileoff,
                           std::uint64_t filesize, std::uint32_t maxprot,
                           std::uint32_t initprot, std::uint32_t nsects) {
    const std::size_t base = cmd;
    put32(out, base, kSegment64);
    put32(out, base + 4, kSegmentCommandSize + kSectionSize * nsects);
    writeName(base + 8, name, 16);
    put64(out, base + 24, vmaddr);
    put64(out, base + 32, vmsize);
    put64(out, base + 40, fileoff);
    put64(out, base + 48, filesize);
    put32(out, base + 56, maxprot);
    put32(out, base + 60, initprot);
    put32(out, base + 64, nsects);
    put32(out, base + 68, 0);
    cmd += kSegmentCommandSize + kSectionSize * nsects;
    return base;
  };
  segment("__PAGEZERO", 0, 0x100000000ull, 0, 0, 0, 0, 0);
  const std::size_t textCommand = segment("__TEXT", kTextVmaddr, textFileSize, 0,
                                          textFileSize, 5, 5, textSections);
  std::size_t section = textCommand + kSegmentCommandSize;
  const auto writeSection = [&](const std::string& name, const std::string& seg,
                                std::uint64_t addr, std::uint64_t size,
                                std::uint32_t offset, std::uint32_t align,
                                std::uint32_t flags, std::uint32_t reserved1,
                                std::uint32_t reserved2) {
    writeName(section, name, 16);
    writeName(section + 16, seg, 16);
    put64(out, section + 32, addr);
    put64(out, section + 40, size);
    put32(out, section + 48, offset);
    put32(out, section + 52, align);
    put32(out, section + 56, 0);
    put32(out, section + 60, 0);
    put32(out, section + 64, flags);
    put32(out, section + 68, reserved1);
    put32(out, section + 72, reserved2);
    put32(out, section + 76, 0);
    section += kSectionSize;
  };
  writeSection("__text", "__TEXT", kTextVmaddr + textFileOffset, object.text.size(),
               static_cast<std::uint32_t>(textFileOffset), 4, kTextAttributes, 0, 0);
  if (hasConst) {
    writeSection("__const", "__TEXT", kTextVmaddr + constFileOffset,
                 object.rodata.size(), static_cast<std::uint32_t>(constFileOffset), 3, 0,
                 0, 0);
  }
  if (hasImports) {
    writeSection("__stubs", "__TEXT", kTextVmaddr + stubsFileOffset, stubsSize,
                 static_cast<std::uint32_t>(stubsFileOffset), 1,
                 kSectionTypeSymbolStubs | kSectionAttributesPureInstructions |
                     kSectionAttributesSomeInstructions,
                 static_cast<std::uint32_t>(imports.size()), kStubSize);
  }
  const std::size_t dataCommand = segment("__DATA", dataVmaddr, dataFileSize,
                                          dataFileOffset, dataFileSize, 3, 3,
                                          dataSections);
  section = dataCommand + kSegmentCommandSize;
  if (hasImports) {
    writeSection("__got", "__DATA", dataVmaddr, gotSize,
                 static_cast<std::uint32_t>(dataFileOffset), 3,
                 kSectionTypeNonLazySymbolPointers, 0, 0);
  }
  segment("__LINKEDIT", linkeditVmaddr, linkeditVmsize, linkeditFileOffset,
          linkeditSize, 1, 1, 0);

  put32(out, cmd, kMain);
  put32(out, cmd + 4, 24);
  put64(out, cmd + 8, textFileOffset + object.entryOffset);
  put64(out, cmd + 16, 0);
  cmd += 24;
  put32(out, cmd, kLoadDylinker);
  put32(out, cmd + 4, dylinkerSize);
  put32(out, cmd + 8, 12);
  writeName(cmd + 12, dylinkerName, dylinkerName.size() + 1);
  cmd += dylinkerSize;
  put32(out, cmd, kLoadDylib);
  put32(out, cmd + 4, dylibSize);
  put32(out, cmd + 8, 24);
  put32(out, cmd + 12, 0);
  put32(out, cmd + 16, 0x10000);
  put32(out, cmd + 20, 0x10000);
  writeName(cmd + 24, dylibName, dylibName.size() + 1);
  cmd += dylibSize;
  put32(out, cmd, kUuid);
  put32(out, cmd + 4, 24);
  const std::uint8_t uuid[16] = {0x4f, 0x53, 0x4b, 0x45, 0x4c, 0x2d, 0x4d, 0x41,
                                 0x43, 0x4f, 0x53, 0x2d, 0x31, 0x30, 0x33, 0x64};
  std::memcpy(out.data() + cmd + 8, uuid, 16);
  cmd += 24;
  put32(out, cmd, kBuildVersion);
  put32(out, cmd + 4, 24);
  put32(out, cmd + 8, kPlatformMacOs);
  put32(out, cmd + 12, kMacOs10_15);
  put32(out, cmd + 16, kMacOs10_15);
  put32(out, cmd + 20, 0);
  cmd += 24;
  put32(out, cmd, kSymtab);
  put32(out, cmd + 4, 24);
  put32(out, cmd + 8, static_cast<std::uint32_t>(symoff));
  put32(out, cmd + 12, static_cast<std::uint32_t>(symbols.size()));
  put32(out, cmd + 16, static_cast<std::uint32_t>(stroff));
  put32(out, cmd + 20, static_cast<std::uint32_t>(strtab.size()));
  cmd += 24;
  put32(out, cmd, kDysymtab);
  put32(out, cmd + 4, 80);
  put32(out, cmd + 8, 0);   // ilocalsym
  put32(out, cmd + 12, 0);  // nlocalsym
  put32(out, cmd + 16, 1);  // iextdefsym
  put32(out, cmd + 20, 2);  // nextdefsym
  put32(out, cmd + 24, 3);  // iundefsym
  put32(out, cmd + 28, static_cast<std::uint32_t>(imports.size()));
  put32(out, cmd + 32, 0);
  put32(out, cmd + 36, 0);
  put32(out, cmd + 40, 0);
  put32(out, cmd + 44, 0);
  put32(out, cmd + 48, 0);
  put32(out, cmd + 52, 0);
  put32(out, cmd + 56, static_cast<std::uint32_t>(indirectsymoff));
  put32(out, cmd + 60, static_cast<std::uint32_t>(imports.size() * 2));
  put32(out, cmd + 64, 0);
  put32(out, cmd + 68, 0);
  put32(out, cmd + 72, 0);
  put32(out, cmd + 76, 0);
  cmd += 80;
  put32(out, cmd, kDyldInfoOnly);
  put32(out, cmd + 4, 48);
  put32(out, cmd + 8, 0);
  put32(out, cmd + 12, 0);
  put32(out, cmd + 16, static_cast<std::uint32_t>(bindoff));
  put32(out, cmd + 20, static_cast<std::uint32_t>(bind.size()));
  put32(out, cmd + 24, 0);
  put32(out, cmd + 28, 0);
  put32(out, cmd + 32, 0);
  put32(out, cmd + 36, 0);
  put32(out, cmd + 40, 0);
  put32(out, cmd + 44, 0);

  std::memcpy(out.data() + textFileOffset, text.data(), text.size());
  if (hasConst) std::memcpy(out.data() + constFileOffset, object.rodata.data(), object.rodata.size());
  if (hasImports) std::memcpy(out.data() + stubsFileOffset, stubs.data(), stubs.size());
  if (hasImports) std::memcpy(out.data() + dataFileOffset, got.data(), got.size());
  std::memcpy(out.data() + symoff, symtab.data(), symtab.size());
  std::memcpy(out.data() + stroff, strtab.data(), strtab.size());
  if (!indirect.empty()) std::memcpy(out.data() + indirectsymoff, indirect.data(), indirect.size());
  if (!bind.empty()) std::memcpy(out.data() + bindoff, bind.data(), bind.size());
  return out;
}
