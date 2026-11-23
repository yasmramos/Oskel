#include "native/pe_link.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "native/pe.h"

namespace {

// ---------------------------------------------------------------------------
// COFF constants (Windows x86-64).
// ---------------------------------------------------------------------------
constexpr std::uint16_t kCoffMachineAmd64 = 0x8664;

constexpr std::uint16_t kCoffRelAmd64Addr64 = 1;
constexpr std::uint16_t kCoffRelAmd64Addr32 = 2;
constexpr std::uint16_t kCoffRelAmd64Addr32Nb = 3;
constexpr std::uint16_t kCoffRelAmd64Rel32 = 4;
constexpr std::uint16_t kCoffRelAmd64Rel32_1 = 5;
constexpr std::uint16_t kCoffRelAmd64Rel32_5 = 9;

constexpr std::uint8_t kCoffSymClassExternal = 2;
constexpr std::uint8_t kCoffSymClassStatic = 3;
constexpr std::uint8_t kCoffSymClassFile = 103;
constexpr std::uint8_t kCoffSymClassWeakExternal = 105;

constexpr std::size_t kNoSymbol = std::numeric_limits<std::size_t>::max();

// ---------------------------------------------------------------------------
// PE32+ constants. `kPeImageBase`, the stack reserve and the stack commit live
// in `native/pe.h` so the freestanding and the linked images agree.
// ---------------------------------------------------------------------------
constexpr std::uint32_t kSectionAlignment = 0x1000;
constexpr std::uint32_t kFileAlignment = 0x200;
constexpr std::uint32_t kIdataRva = 0x1000;
constexpr std::uint32_t kDosSize = 0x40;
constexpr std::uint32_t kCoffHeaderSize = 20;
constexpr std::uint32_t kOptionalHeaderSize = 240;
constexpr std::uint32_t kSectionHeaderSize = 40;

constexpr std::uint32_t kSectionCharacteristicsIdata = 0xC0000040;
constexpr std::uint32_t kSectionCharacteristicsText = 0x60000020;
constexpr std::uint32_t kSectionCharacteristicsRdata = 0x40000040;
constexpr std::uint32_t kSectionCharacteristicsData = 0xC0000040;
constexpr std::uint32_t kSectionCharacteristicsBss = 0xC0000080;

constexpr std::uint16_t kCharacteristicsExecutable = 0x0002;
constexpr std::uint16_t kCharacteristicsRelocsStripped = 0x0001;
constexpr std::uint16_t kCharacteristicsLargeAddressAware = 0x0020;
constexpr std::uint16_t kDllCharacteristicsNxCompat = 0x0100;

constexpr std::uint32_t kStartSize = 22;

enum class SectionKind { Text, Rodata, Data, Bss, Ignored };

// A bounds-checked little-endian reader. A COFF object is untrusted input; a
// short or lying header must be a diagnostic, not an out-of-bounds read.
class Reader {
 public:
  explicit Reader(const std::vector<std::uint8_t>& data) : data_(data) {}

  std::uint8_t u8(std::uint64_t offset) const {
    check(offset, 1);
    return data_[offset];
  }
  std::uint16_t u16(std::uint64_t offset) const {
    check(offset, 2);
    return static_cast<std::uint16_t>(data_[offset]) |
           static_cast<std::uint16_t>(data_[offset + 1] << 8);
  }
  std::uint32_t u32(std::uint64_t offset) const {
    check(offset, 4);
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(data_[offset + i]) << (8 * i);
    }
    return value;
  }
  std::uint64_t u64(std::uint64_t offset) const {
    check(offset, 8);
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<std::uint64_t>(data_[offset + i]) << (8 * i);
    }
    return value;
  }

 private:
  void check(std::uint64_t offset, std::uint64_t size) const {
    if (offset > data_.size() || size > data_.size() - offset) {
      throw LinkError("truncated COFF data");
    }
  }

  const std::vector<std::uint8_t>& data_;
};

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) {
  const std::uint64_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}

void put8(std::vector<std::uint8_t>& out, std::uint64_t offset, std::uint8_t value) {
  if (offset >= out.size()) throw LinkError("internal: PE write out of range");
  out[offset] = value;
}

void put16(std::vector<std::uint8_t>& out, std::uint64_t offset, std::uint16_t value) {
  for (int i = 0; i < 2; ++i) {
    put8(out, offset + i, static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
  }
}

void put32(std::vector<std::uint8_t>& out, std::uint64_t offset, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    put8(out, offset + i, static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
  }
}

void put64(std::vector<std::uint8_t>& out, std::uint64_t offset, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    put8(out, offset + i, static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
  }
}

std::string readCString(const Reader& reader, std::uint64_t offset,
                        std::uint64_t limit) {
  std::string text;
  while (true) {
    if (offset >= limit) throw LinkError("unterminated COFF string");
    const std::uint8_t byte = reader.u8(offset++);
    if (byte == 0) break;
    text.push_back(static_cast<char>(byte));
  }
  return text;
}

struct CoffSection {
  std::string name;
  std::uint32_t virtualSize = 0;
  std::uint32_t rawSize = 0;
  std::uint32_t rawOffset = 0;
  std::uint32_t relocOffset = 0;
  std::uint16_t relocCount = 0;
  std::uint32_t characteristics = 0;
  SectionKind kind = SectionKind::Ignored;
  std::vector<std::uint8_t> bytes;
  std::uint64_t outputOffset = 0;  // within the object's section-kind block
};

struct CoffSymbol {
  std::string name;
  std::uint32_t value = 0;
  std::int16_t section = 0;
  std::uint16_t type = 0;
  std::uint8_t storageClass = 0;
  std::uint8_t auxCount = 0;
  std::uint32_t rawIndex = 0;
};

struct CoffRelocation {
  std::uint32_t offset = 0;
  std::uint32_t symbolIndex = 0;
  std::uint16_t type = 0;
  std::uint16_t sectionIndex = 0;
};

struct Definition {
  std::uint64_t offset = 0;
  SectionKind section = SectionKind::Text;
};

struct CoffObject {
  std::string name;
  std::vector<CoffSection> sections;
  std::vector<CoffSymbol> symbols;
  std::vector<std::size_t> symbolByRawIndex;
  std::vector<CoffRelocation> relocations;
  std::vector<std::uint8_t> text;
  std::vector<std::uint8_t> rodata;
  std::vector<std::uint8_t> data;
  std::uint64_t bssSize = 0;
  std::uint64_t textOffset = 0;
  std::uint64_t rodataOffset = 0;
  std::uint64_t dataOffset = 0;
  std::uint64_t bssOffset = 0;
  std::unordered_map<std::string, Definition> definitions;
};

bool nameHasPrefix(const std::string& name, const char* prefix) {
  const std::size_t length = std::strlen(prefix);
  if (name.size() < length || name.compare(0, length, prefix) != 0) return false;
  return name.size() == length || name[length] == '$' || name[length] == '.';
}

SectionKind classifySection(const std::string& name) {
  if (nameHasPrefix(name, ".text")) return SectionKind::Text;
  if (nameHasPrefix(name, ".rdata")) return SectionKind::Rodata;
  if (nameHasPrefix(name, ".data")) return SectionKind::Data;
  if (nameHasPrefix(name, ".bss")) return SectionKind::Bss;
  return SectionKind::Ignored;
}

std::string coffName(const Reader& reader, std::uint64_t base,
                     std::uint64_t strtabOffset, std::uint64_t strtabSize) {
  if (reader.u32(base) == 0) {
    const std::uint32_t offset = reader.u32(base + 4);
    if (strtabSize == 0 || offset >= strtabSize) {
      throw LinkError("COFF name points outside the string table");
    }
    return readCString(reader, strtabOffset + offset, strtabOffset + strtabSize);
  }
  std::string name;
  for (int i = 0; i < 8; ++i) {
    const std::uint8_t c = reader.u8(base + i);
    if (c == 0) break;
    name.push_back(static_cast<char>(c));
  }
  return name;
}

std::string coffSectionName(const Reader& reader, std::uint64_t base,
                            std::uint64_t strtabOffset, std::uint64_t strtabSize) {
  const std::string name = coffName(reader, base, strtabOffset, strtabSize);
  if (name.size() > 1 && name[0] == '/') {
    bool digits = true;
    for (std::size_t i = 1; i < name.size(); ++i) {
      if (std::isdigit(static_cast<unsigned char>(name[i])) == 0) {
        digits = false;
        break;
      }
    }
    if (digits) {
      std::uint64_t offset = 0;
      for (std::size_t i = 1; i < name.size(); ++i) {
        offset = offset * 10 + static_cast<std::uint64_t>(name[i] - '0');
      }
      if (strtabSize == 0 || offset >= strtabSize) {
        throw LinkError("COFF section name points outside the string table");
      }
      return readCString(reader, strtabOffset + offset, strtabOffset + strtabSize);
    }
  }
  return name;
}

bool isSectionSymbol(const CoffSymbol& symbol, const CoffSection& section) {
  return symbol.storageClass == kCoffSymClassStatic && symbol.value == 0 &&
         symbol.name == section.name;
}

std::uint64_t sectionOffsetOf(const CoffObject& object, SectionKind kind) {
  switch (kind) {
    case SectionKind::Text: return object.textOffset;
    case SectionKind::Rodata: return object.rodataOffset;
    case SectionKind::Data: return object.dataOffset;
    case SectionKind::Bss: return object.bssOffset;
    default: return 0;
  }
}

void layoutObject(CoffObject& object) {
  std::uint64_t textSize = 0;
  std::uint64_t rodataSize = 0;
  std::uint64_t dataSize = 0;
  std::uint64_t bssSize = 0;
  for (CoffSection& section : object.sections) {
    switch (section.kind) {
      case SectionKind::Text: {
        section.outputOffset = alignUp(textSize, 16);
        textSize = section.outputOffset + section.bytes.size();
        object.text.resize(static_cast<std::size_t>(textSize), 0);
        if (!section.bytes.empty()) {
          std::memcpy(object.text.data() + section.outputOffset,
                      section.bytes.data(), section.bytes.size());
        }
        break;
      }
      case SectionKind::Rodata: {
        section.outputOffset = alignUp(rodataSize, 16);
        rodataSize = section.outputOffset + section.bytes.size();
        object.rodata.resize(static_cast<std::size_t>(rodataSize), 0);
        if (!section.bytes.empty()) {
          std::memcpy(object.rodata.data() + section.outputOffset,
                      section.bytes.data(), section.bytes.size());
        }
        break;
      }
      case SectionKind::Data: {
        section.outputOffset = alignUp(dataSize, 16);
        dataSize = section.outputOffset + section.bytes.size();
        object.data.resize(static_cast<std::size_t>(dataSize), 0);
        if (!section.bytes.empty()) {
          std::memcpy(object.data.data() + section.outputOffset,
                      section.bytes.data(), section.bytes.size());
        }
        break;
      }
      case SectionKind::Bss: {
        section.outputOffset = alignUp(bssSize, 8);
        bssSize = section.outputOffset + section.virtualSize;
        object.bssSize = bssSize;
        break;
      }
      case SectionKind::Ignored:
        break;
    }
  }
}

CoffObject parseCoffObject(const std::vector<std::uint8_t>& bytes,
                           const std::string& name) {
  if (bytes.size() < 20) throw LinkError("'" + name + "' is too small to be COFF");
  const Reader reader(bytes);
  if (reader.u16(0) != kCoffMachineAmd64) {
    throw LinkError("'" + name + "' is not an x86-64 COFF object");
  }
  const std::uint16_t sectionCount = reader.u16(2);
  const std::uint32_t symtabOffset = reader.u32(8);
  const std::uint32_t symbolCount = reader.u32(12);
  const std::uint16_t optionalSize = reader.u16(16);
  if (optionalSize != 0) {
    throw LinkError("'" + name + "' is not a COFF object (it has an optional header)");
  }
  if (sectionCount == 0) throw LinkError("'" + name + "' has no sections");
  const std::uint64_t sectionHeaders = 20;
  if (sectionHeaders + static_cast<std::uint64_t>(sectionCount) * 40 > bytes.size()) {
    throw LinkError("'" + name + "' has a truncated section table");
  }

  std::uint64_t strtabOffset = 0;
  std::uint64_t strtabSize = 0;
  if (symtabOffset != 0 && symbolCount != 0) {
    const std::uint64_t candidate =
        symtabOffset + static_cast<std::uint64_t>(symbolCount) * 18;
    if (candidate + 4 <= bytes.size()) {
      const std::uint32_t size = reader.u32(candidate);
      if (size >= 4 && candidate + size <= bytes.size()) {
        strtabOffset = candidate;
        strtabSize = size;
      }
    }
  }

  CoffObject object;
  object.name = name;
  object.sections.resize(sectionCount);
  for (std::uint16_t i = 0; i < sectionCount; ++i) {
    const std::uint64_t base = sectionHeaders + static_cast<std::uint64_t>(i) * 40;
    CoffSection& section = object.sections[i];
    section.name = coffSectionName(reader, base, strtabOffset, strtabSize);
    section.virtualSize = reader.u32(base + 8);
    section.rawSize = reader.u32(base + 16);
    section.rawOffset = reader.u32(base + 20);
    section.relocOffset = reader.u32(base + 24);
    section.relocCount = reader.u16(base + 32);
    section.characteristics = reader.u32(base + 36);
    section.kind = classifySection(section.name);
    if (section.kind == SectionKind::Ignored) continue;
    if (section.kind == SectionKind::Bss) {
      const std::uint32_t size =
          section.virtualSize != 0 ? section.virtualSize : section.rawSize;
      section.virtualSize = size;
      section.rawSize = 0;
      continue;
    }
    const std::uint32_t size = std::max(section.rawSize, section.virtualSize);
    if (size == 0) continue;
    if (section.rawSize != 0) {
      if (section.rawOffset > bytes.size() ||
          section.rawSize > bytes.size() - section.rawOffset) {
        throw LinkError("section '" + section.name + "' lies outside '" + name + "'");
      }
      section.bytes.assign(bytes.begin() + section.rawOffset,
                           bytes.begin() + section.rawOffset + section.rawSize);
    }
    section.bytes.resize(size, 0);
  }

  if (symbolCount != 0) {
    if (symtabOffset > bytes.size() ||
        static_cast<std::uint64_t>(symbolCount) * 18 > bytes.size() - symtabOffset) {
      throw LinkError("'" + name + "' has a truncated symbol table");
    }
    object.symbolByRawIndex.assign(symbolCount, kNoSymbol);
    std::uint32_t raw = 0;
    while (raw < symbolCount) {
      const std::uint64_t base = symtabOffset + static_cast<std::uint64_t>(raw) * 18;
      CoffSymbol symbol;
      symbol.name = coffName(reader, base, strtabOffset, strtabSize);
      symbol.value = reader.u32(base + 8);
      symbol.section = static_cast<std::int16_t>(reader.u16(base + 12));
      symbol.type = reader.u16(base + 14);
      symbol.storageClass = reader.u8(base + 16);
      symbol.auxCount = reader.u8(base + 17);
      symbol.rawIndex = raw;
      object.symbolByRawIndex[raw] = object.symbols.size();
      object.symbols.push_back(std::move(symbol));
      for (std::uint8_t aux = 1; aux <= object.symbols.back().auxCount; ++aux) {
        if (static_cast<std::uint64_t>(raw) + aux < symbolCount) {
          object.symbolByRawIndex[raw + aux] = kNoSymbol;
        }
      }
      raw += 1 + object.symbols.back().auxCount;
    }
  }

  for (std::uint16_t i = 0; i < sectionCount; ++i) {
    const CoffSection& section = object.sections[i];
    if (section.kind == SectionKind::Ignored || section.relocCount == 0) continue;
    if (section.kind == SectionKind::Bss) {
      throw LinkError("section '" + section.name + "' has relocations in BSS");
    }
    const std::uint64_t needed = static_cast<std::uint64_t>(section.relocCount) * 10;
    if (section.relocOffset > bytes.size() ||
        needed > bytes.size() - section.relocOffset) {
      throw LinkError("relocations for '" + section.name + "' lie outside '" + name + "'");
    }
    for (std::uint16_t r = 0; r < section.relocCount; ++r) {
      const std::uint64_t base = section.relocOffset + static_cast<std::uint64_t>(r) * 10;
      CoffRelocation rel;
      rel.offset = reader.u32(base);
      rel.symbolIndex = reader.u32(base + 4);
      rel.type = reader.u16(base + 8);
      rel.sectionIndex = i;
      object.relocations.push_back(rel);
    }
  }

  layoutObject(object);
  return object;
}

std::unordered_map<std::string, Definition> buildDefinitions(
    std::vector<CoffObject>& objects) {
  std::unordered_map<std::string, Definition> globals;
  for (CoffObject& object : objects) {
    for (const CoffSymbol& symbol : object.symbols) {
      if (symbol.name.empty() || symbol.section <= 0) continue;
      if (symbol.storageClass == kCoffSymClassFile) continue;
      if (static_cast<std::size_t>(symbol.section) > object.sections.size()) {
        throw LinkError("symbol '" + symbol.name + "' has an invalid section index");
      }
      const CoffSection& section =
          object.sections[static_cast<std::size_t>(symbol.section - 1)];
      if (section.kind == SectionKind::Ignored) continue;
      if (isSectionSymbol(symbol, section)) continue;
      Definition definition;
      definition.section = section.kind;
      definition.offset = sectionOffsetOf(object, section.kind) +
                          section.outputOffset + symbol.value;
      const bool global = symbol.storageClass == kCoffSymClassExternal ||
                          symbol.storageClass == kCoffSymClassWeakExternal;
      if (global) {
        const auto inserted = globals.emplace(symbol.name, definition);
        if (!inserted.second) {
          throw LinkError("duplicate global symbol '" + symbol.name + "'");
        }
      } else {
        const auto inserted = object.definitions.emplace(symbol.name, definition);
        if (!inserted.second) {
          throw LinkError("duplicate local symbol '" + symbol.name + "' in '" +
                          object.name + "'");
        }
      }
    }
  }
  return globals;
}

struct Patch {
  std::uint64_t writeOffset = 0;  // within the merged output section
  SectionKind section = SectionKind::Text;
  std::uint16_t type = 0;
  bool defined = false;
  SectionKind targetSection = SectionKind::Text;
  std::uint64_t targetOffset = 0;
  std::string import;
  std::int64_t addend = 0;
};

const char* dllForImport(const std::string& name) {
  static const std::unordered_set<std::string> kernel = {
      "ExitProcess", "GetTickCount64", "GetSystemTimeAsFileTime",
      "VirtualAlloc", "WriteFile", "GetStdHandle", "CreateFileA",
      "ReadFile", "CloseHandle", "SetFilePointerEx", "GetCommandLineA",
      "GetCommandLineW", "GetLastError", "Sleep", "QueryPerformanceCounter",
      "QueryPerformanceFrequency", "GetProcessHeap", "HeapAlloc", "HeapFree",
      "DeleteFileA", "MoveFileA", "CreateDirectoryA", "RemoveDirectoryA",
      "SetCurrentDirectoryA", "GetCurrentDirectoryA", "FindFirstFileA",
      "FindNextFileA", "FindClose", "GetFileAttributesExA"};
  static const std::unordered_set<std::string> ucrt = {
      "_write", "_read", "_open", "_close", "_setmode", "malloc", "free",
      "calloc", "realloc", "exit", "abort", "time", "strlen", "strcmp",
      "strcpy", "strcat", "memcpy", "memset", "memmove", "printf", "fprintf",
      "snprintf", "puts", "putchar", "getchar", "fopen", "fclose", "fread",
      "fwrite", "fflush", "getenv", "atoi", "atof", "strtod", "strtol",
      "qsort", "sqrt", "pow", "sin", "cos", "tan", "log", "exp", "floor",
      "ceil", "round", "fabs", "rand", "srand", "system", "remove", "rename",
      "perror"};
  if (kernel.count(name) != 0) return "kernel32.dll";
  if (ucrt.count(name) != 0) return "ucrtbase.dll";
  if (!name.empty() && name[0] == '_') return "ucrtbase.dll";
  throw LinkError("undefined symbol '" + name + "' has no known import DLL");
}

struct ImportDll {
  std::string name;
  std::vector<std::uint32_t> importIndices;
};

struct IdataLayout {
  std::uint32_t descriptors = 0;
  std::vector<std::uint32_t> ilt;
  std::vector<std::uint32_t> iat;
  std::vector<std::uint32_t> hintName;
  std::vector<std::uint32_t> dllName;
  std::vector<std::uint32_t> iatSlot;
  std::uint32_t size = 0;
};

IdataLayout layoutIdata(const std::vector<std::string>& imports,
                        const std::vector<ImportDll>& dlls) {
  IdataLayout layout;
  layout.descriptors = kIdataRva;
  std::uint32_t cursor =
      layout.descriptors + static_cast<std::uint32_t>((dlls.size() + 1) * 20);
  layout.ilt.resize(dlls.size());
  for (std::size_t i = 0; i < dlls.size(); ++i) {
    layout.ilt[i] = cursor;
    cursor += static_cast<std::uint32_t>((dlls[i].importIndices.size() + 1) * 8);
  }
  layout.iat.resize(dlls.size());
  for (std::size_t i = 0; i < dlls.size(); ++i) {
    layout.iat[i] = cursor;
    cursor += static_cast<std::uint32_t>((dlls[i].importIndices.size() + 1) * 8);
  }
  layout.hintName.resize(imports.size());
  for (std::size_t i = 0; i < imports.size(); ++i) {
    layout.hintName[i] = cursor;
    cursor += 2 + static_cast<std::uint32_t>(imports[i].size()) + 1;
    if ((cursor & 1u) != 0) ++cursor;
  }
  layout.dllName.resize(dlls.size());
  for (std::size_t i = 0; i < dlls.size(); ++i) {
    layout.dllName[i] = cursor;
    cursor += static_cast<std::uint32_t>(dlls[i].name.size()) + 1;
    if ((cursor & 1u) != 0) ++cursor;
  }
  layout.size = cursor - kIdataRva;
  layout.iatSlot.resize(imports.size());
  for (std::size_t d = 0; d < dlls.size(); ++d) {
    for (std::size_t j = 0; j < dlls[d].importIndices.size(); ++j) {
      layout.iatSlot[dlls[d].importIndices[j]] =
          layout.iat[d] + static_cast<std::uint32_t>(j * 8);
    }
  }
  return layout;
}

std::vector<std::uint8_t> buildIdata(const std::vector<std::string>& imports,
                                     const std::vector<ImportDll>& dlls,
                                     const IdataLayout& layout) {
  std::vector<std::uint8_t> out(layout.size, 0);
  // The layout stores RVAs (the import descriptors and thunks are RVAs); the
  // section bytes are indexed from zero.
  const auto at = [](std::uint32_t rva) -> std::uint64_t {
    return rva - kIdataRva;
  };
  const std::uint64_t descriptors = at(layout.descriptors);
  for (std::size_t d = 0; d < dlls.size(); ++d) {
    const std::uint64_t offset = descriptors + d * 20;
    put32(out, offset, layout.ilt[d]);
    put32(out, offset + 4, 0);
    put32(out, offset + 8, 0);
    put32(out, offset + 12, layout.dllName[d]);
    put32(out, offset + 16, layout.iat[d]);
  }
  // The null descriptor is already zero.
  for (std::size_t d = 0; d < dlls.size(); ++d) {
    std::uint64_t ilt = at(layout.ilt[d]);
    for (const std::uint32_t index : dlls[d].importIndices) {
      put64(out, ilt, layout.hintName[index]);
      ilt += 8;
    }
    std::uint64_t iat = at(layout.iat[d]);
    for (const std::uint32_t index : dlls[d].importIndices) {
      put64(out, iat, layout.hintName[index]);
      iat += 8;
    }
  }
  for (std::size_t i = 0; i < imports.size(); ++i) {
    const std::uint64_t hintName = at(layout.hintName[i]);
    put16(out, hintName, 0);
    std::memcpy(out.data() + hintName + 2, imports[i].data(), imports[i].size());
  }
  for (std::size_t d = 0; d < dlls.size(); ++d) {
    std::memcpy(out.data() + at(layout.dllName[d]), dlls[d].name.data(),
                dlls[d].name.size());
  }
  return out;
}

std::vector<std::string> resolvePatches(
    const std::vector<CoffObject>& objects,
    const std::unordered_map<std::string, Definition>& definitions,
    std::vector<Patch>& patches) {
  std::vector<std::string> imports;
  std::unordered_map<std::string, std::uint32_t> importIndex;
  for (const CoffObject& object : objects) {
    for (const CoffRelocation& rel : object.relocations) {
      if (rel.symbolIndex >= object.symbolByRawIndex.size()) {
        throw LinkError("relocation in '" + object.name +
                        "' names an out-of-range symbol");
      }
      const std::size_t symbolIndex = object.symbolByRawIndex[rel.symbolIndex];
      if (symbolIndex == kNoSymbol) {
        throw LinkError("relocation in '" + object.name +
                        "' names an auxiliary symbol");
      }
      const CoffSymbol& symbol = object.symbols[symbolIndex];
      const CoffSection& source = object.sections[rel.sectionIndex];
      if (source.kind == SectionKind::Ignored || source.kind == SectionKind::Bss) {
        throw LinkError("relocation in an unsupported section of '" + object.name + "'");
      }
      std::size_t width = 0;
      switch (rel.type) {
        case kCoffRelAmd64Addr64: width = 8; break;
        case kCoffRelAmd64Addr32:
        case kCoffRelAmd64Addr32Nb:
        case kCoffRelAmd64Rel32:
        case kCoffRelAmd64Rel32_1:
        case 6:
        case 7:
        case 8:
        case kCoffRelAmd64Rel32_5:
          width = 4;
          break;
        default:
          throw LinkError("unsupported COFF relocation type " +
                          std::to_string(rel.type) + " for '" +
                          (symbol.name.empty() ? "<section>" : symbol.name) + "'");
      }
      if (rel.offset > source.bytes.size() ||
          width > source.bytes.size() - rel.offset) {
        throw LinkError("relocation for '" +
                        (symbol.name.empty() ? "<section>" : symbol.name) +
                        "' lies outside its section");
      }
      std::int64_t addend = 0;
      if (width == 8) {
        std::uint64_t raw = 0;
        for (int i = 0; i < 8; ++i) {
          raw |= static_cast<std::uint64_t>(source.bytes[rel.offset + i]) << (8 * i);
        }
        addend = static_cast<std::int64_t>(raw);
      } else {
        std::uint32_t raw = 0;
        for (int i = 0; i < 4; ++i) {
          raw |= static_cast<std::uint32_t>(source.bytes[rel.offset + i]) << (8 * i);
        }
        const bool pcRelative =
            rel.type == kCoffRelAmd64Rel32 ||
            (rel.type >= kCoffRelAmd64Rel32_1 && rel.type <= kCoffRelAmd64Rel32_5);
        addend = pcRelative ? static_cast<std::int64_t>(static_cast<std::int32_t>(raw))
                            : static_cast<std::int64_t>(raw);
      }

      Patch patch;
      patch.writeOffset = sectionOffsetOf(object, source.kind) +
                          source.outputOffset + rel.offset;
      patch.section = source.kind;
      patch.type = rel.type;
      patch.addend = addend;

      if (symbol.section > 0) {
        if (static_cast<std::size_t>(symbol.section) > object.sections.size()) {
          throw LinkError("symbol '" + symbol.name + "' has an invalid section index");
        }
        const CoffSection& targetSection =
            object.sections[static_cast<std::size_t>(symbol.section - 1)];
        if (isSectionSymbol(symbol, targetSection)) {
          if (targetSection.kind == SectionKind::Ignored) {
            throw LinkError("relocation targets an unsupported section in '" +
                            object.name + "'");
          }
          patch.defined = true;
          patch.targetSection = targetSection.kind;
          patch.targetOffset = sectionOffsetOf(object, targetSection.kind) +
                               targetSection.outputOffset;
        } else {
          const auto local = object.definitions.find(symbol.name);
          const auto global = definitions.find(symbol.name);
          if (local != object.definitions.end()) {
            patch.defined = true;
            patch.targetSection = local->second.section;
            patch.targetOffset = local->second.offset;
          } else if (global != definitions.end()) {
            patch.defined = true;
            patch.targetSection = global->second.section;
            patch.targetOffset = global->second.offset;
          } else {
            throw LinkError("symbol '" + symbol.name +
                            "' is defined in an unsupported section");
          }
        }
      } else if (symbol.section == 0) {
        const auto global = definitions.find(symbol.name);
        if (global != definitions.end()) {
          patch.defined = true;
          patch.targetSection = global->second.section;
          patch.targetOffset = global->second.offset;
        } else {
          if (rel.type != kCoffRelAmd64Rel32) {
            throw LinkError("undefined data symbol '" + symbol.name + "'");
          }
          // MSVC's UCRT header maps `time` to `_time64`; `ucrtbase.dll` exports
          // `_time64`, not `time`. The AOT runtime spells the call `time`, so
          // rename it at the import boundary or the Windows loader fails with
          // STATUS_ENTRYPOINT_NOT_FOUND.
          const std::string importName =
              symbol.name == "time" ? "_time64" : symbol.name;
          if (importIndex.count(importName) == 0) {
            importIndex.emplace(importName,
                               static_cast<std::uint32_t>(imports.size()));
            imports.push_back(importName);
          }
          patch.import = importName;
        }
      } else {
        throw LinkError("relocation names an absolute or debug symbol");
      }
      patches.push_back(std::move(patch));
    }
  }
  if (importIndex.count("ExitProcess") == 0) {
    importIndex.emplace("ExitProcess", static_cast<std::uint32_t>(imports.size()));
    imports.push_back("ExitProcess");
  }
  return imports;
}

}  // namespace

std::vector<std::uint8_t> linkWindowsExecutable(
    const std::vector<LinkInput>& inputs, const LinkOptions& options) {
  if (options.target.arch != Arch::X86_64 || options.target.os != Os::Windows ||
      options.target.abi != Abi::Win64) {
    throw LinkError("the Windows linker supports x86_64-windows-win64 only");
  }

  std::vector<CoffObject> objects;
  for (const LinkInput& input : inputs) {
    if (input.bytes.size() >= 8 &&
        std::memcmp(input.bytes.data(), "!<arch>\n", 8) == 0) {
      throw LinkError("'" + input.name +
                      "' is an archive; COFF archives are PHASE 10.3b");
    }
    if (input.bytes.size() >= 2 && input.bytes[0] == 'M' && input.bytes[1] == 'Z') {
      throw LinkError("'" + input.name + "' is a PE image, not a COFF object");
    }
    objects.push_back(parseCoffObject(input.bytes, input.name));
  }
  if (objects.empty()) throw LinkError("no input objects");

  std::uint64_t textSize = 0;
  std::uint64_t rodataSize = 0;
  std::uint64_t dataSize = 0;
  std::uint64_t bssSize = 0;
  for (CoffObject& object : objects) {
    object.textOffset = alignUp(textSize, 16);
    textSize = object.textOffset + object.text.size();
    object.rodataOffset = alignUp(rodataSize, 16);
    rodataSize = object.rodataOffset + object.rodata.size();
    object.dataOffset = alignUp(dataSize, 16);
    dataSize = object.dataOffset + object.data.size();
    object.bssOffset = alignUp(bssSize, 8);
    bssSize = object.bssOffset + object.bssSize;
  }

  const std::unordered_map<std::string, Definition> definitions =
      buildDefinitions(objects);
  const auto mainDefinition = definitions.find("main");
  if (mainDefinition == definitions.end()) {
    throw LinkError("the inputs do not export 'main'");
  }
  if (mainDefinition->second.section != SectionKind::Text) {
    throw LinkError("'main' is not in .text");
  }

  std::vector<std::uint8_t> text(static_cast<std::size_t>(textSize), 0);
  std::vector<std::uint8_t> rodata(static_cast<std::size_t>(rodataSize), 0);
  std::vector<std::uint8_t> data(static_cast<std::size_t>(dataSize), 0);
  for (const CoffObject& object : objects) {
    if (!object.text.empty()) {
      std::memcpy(text.data() + object.textOffset, object.text.data(),
                  object.text.size());
    }
    if (!object.rodata.empty()) {
      std::memcpy(rodata.data() + object.rodataOffset, object.rodata.data(),
                  object.rodata.size());
    }
    if (!object.data.empty()) {
      std::memcpy(data.data() + object.dataOffset, object.data.data(),
                  object.data.size());
    }
  }

  std::vector<Patch> patches;
  const std::vector<std::string> imports = resolvePatches(objects, definitions, patches);

  std::vector<ImportDll> dlls;
  std::unordered_map<std::string, std::size_t> dllIndex;
  for (std::size_t i = 0; i < imports.size(); ++i) {
    const std::string dll = dllForImport(imports[i]);
    auto found = dllIndex.find(dll);
    if (found == dllIndex.end()) {
      const std::size_t index = dlls.size();
      dllIndex.emplace(dll, index);
      dlls.push_back(ImportDll{dll, {}});
      found = dllIndex.find(dll);
    }
    dlls[found->second].importIndices.push_back(static_cast<std::uint32_t>(i));
  }
  const IdataLayout idataLayout = layoutIdata(imports, dlls);
  const std::vector<std::uint8_t> idata = buildIdata(imports, dlls, idataLayout);
  std::unordered_map<std::string, std::uint32_t> importIat;
  for (std::size_t i = 0; i < imports.size(); ++i) {
    importIat.emplace(imports[i], idataLayout.iatSlot[i]);
  }
  const auto exitIat = importIat.find("ExitProcess");
  if (exitIat == importIat.end()) throw LinkError("internal: ExitProcess import missing");

  const std::uint64_t thunkBase = textSize;
  const std::uint64_t startOffset =
      alignUp(textSize + 6 * static_cast<std::uint64_t>(imports.size()), 16);
  const std::uint64_t textVirtualSize = startOffset + kStartSize;

  const std::uint64_t sectionCount =
      2 + (rodataSize != 0 ? 1 : 0) + (dataSize != 0 ? 1 : 0) +
      (bssSize != 0 ? 1 : 0);
  const std::uint64_t headersSize = kDosSize + 4 + kCoffHeaderSize +
                                    kOptionalHeaderSize +
                                    sectionCount * kSectionHeaderSize;
  const std::uint64_t sizeOfHeaders = alignUp(headersSize, kFileAlignment);
  const std::uint64_t idataRawOffset = sizeOfHeaders;
  const std::uint64_t idataRawSize = alignUp(idata.size(), kFileAlignment);
  const std::uint32_t textRva =
      static_cast<std::uint32_t>(alignUp(kIdataRva + idata.size(), kSectionAlignment));
  const std::uint64_t textRawOffset =
      alignUp(idataRawOffset + idataRawSize, kFileAlignment);
  const std::uint64_t textRawSize = alignUp(textVirtualSize, kFileAlignment);

  std::uint64_t rdataRva = 0;
  std::uint64_t rdataRawOffset = 0;
  std::uint64_t rdataRawSize = 0;
  std::uint64_t nextRva = textRva + textVirtualSize;
  std::uint64_t nextRaw = textRawOffset + textRawSize;
  if (rodataSize != 0) {
    rdataRva = alignUp(nextRva, kSectionAlignment);
    rdataRawOffset = alignUp(nextRaw, kFileAlignment);
    rdataRawSize = alignUp(rodataSize, kFileAlignment);
    nextRva = rdataRva + rodataSize;
    nextRaw = rdataRawOffset + rdataRawSize;
  }
  std::uint64_t dataRva = 0;
  std::uint64_t dataRawOffset = 0;
  std::uint64_t dataRawSize = 0;
  if (dataSize != 0) {
    dataRva = alignUp(nextRva, kSectionAlignment);
    dataRawOffset = alignUp(nextRaw, kFileAlignment);
    dataRawSize = alignUp(dataSize, kFileAlignment);
    nextRva = dataRva + dataSize;
    nextRaw = dataRawOffset + dataRawSize;
  }
  std::uint64_t bssRva = 0;
  if (bssSize != 0) {
    bssRva = alignUp(nextRva, kSectionAlignment);
    nextRva = bssRva + bssSize;
  }
  const std::uint64_t sizeOfImage = alignUp(nextRva, kSectionAlignment);
  const std::uint64_t fileSize = nextRaw;

  const auto sectionBase = [&](SectionKind kind) -> std::uint64_t {
    switch (kind) {
      case SectionKind::Text: return textRva;
      case SectionKind::Rodata: return rdataRva;
      case SectionKind::Data: return dataRva;
      case SectionKind::Bss: return bssRva;
      default: return 0;
    }
  };
  const auto sectionBuffer = [&](SectionKind kind) -> std::vector<std::uint8_t>& {
    switch (kind) {
      case SectionKind::Text: return text;
      case SectionKind::Rodata: return rodata;
      case SectionKind::Data: return data;
      default: throw LinkError("internal: relocation in an unsupported section");
    }
  };

  std::unordered_map<std::string, std::uint64_t> thunkOffset;
  for (std::size_t i = 0; i < imports.size(); ++i) {
    thunkOffset.emplace(imports[i],
                        thunkBase + 6 * static_cast<std::uint64_t>(i));
  }

  for (const Patch& patch : patches) {
    const std::uint64_t fieldRva = sectionBase(patch.section) + patch.writeOffset;
    const std::uint64_t fieldVaddr = kPeImageBase + fieldRva;
    std::uint64_t targetVaddr = 0;
    std::uint64_t targetRva = 0;
    if (patch.defined) {
      targetRva = sectionBase(patch.targetSection) + patch.targetOffset;
      targetVaddr = kPeImageBase + targetRva;
    } else {
      const auto found = thunkOffset.find(patch.import);
      if (found == thunkOffset.end()) {
        throw LinkError("internal: no thunk for '" + patch.import + "'");
      }
      targetRva = textRva + found->second;
      targetVaddr = kPeImageBase + targetRva;
    }
    std::vector<std::uint8_t>& buffer = sectionBuffer(patch.section);
    const std::uint64_t writeAt = patch.writeOffset;
    const std::uint64_t extra =
        (patch.type >= kCoffRelAmd64Rel32_1 &&
         patch.type <= kCoffRelAmd64Rel32_5)
            ? patch.type - kCoffRelAmd64Rel32
            : 0;
    switch (patch.type) {
      case kCoffRelAmd64Rel32:
      case kCoffRelAmd64Rel32_1:
      case 6:
      case 7:
      case 8:
      case kCoffRelAmd64Rel32_5: {
        const std::int64_t value =
            static_cast<std::int64_t>(targetVaddr) + patch.addend -
            static_cast<std::int64_t>(fieldVaddr + 4 + extra);
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) {
          throw LinkError("relocation for '" + patch.import + "' does not fit in 32 bits");
        }
        put32(buffer, writeAt, static_cast<std::uint32_t>(value));
        break;
      }
      case kCoffRelAmd64Addr64: {
        put64(buffer, writeAt,
              static_cast<std::uint64_t>(static_cast<std::int64_t>(targetVaddr) +
                                         patch.addend));
        break;
      }
      case kCoffRelAmd64Addr32: {
        const std::int64_t value =
            static_cast<std::int64_t>(targetVaddr) + patch.addend;
        if (value < 0 ||
            value > static_cast<std::int64_t>(
                        std::numeric_limits<std::uint32_t>::max())) {
          throw LinkError("relocation for '" + patch.import +
                          "' does not fit in 32 bits");
        }
        put32(buffer, writeAt, static_cast<std::uint32_t>(value));
        break;
      }
      case kCoffRelAmd64Addr32Nb: {
        const std::int64_t value =
            static_cast<std::int64_t>(targetRva) + patch.addend;
        if (value < 0 ||
            value > static_cast<std::int64_t>(
                        std::numeric_limits<std::uint32_t>::max())) {
          throw LinkError("relocation for '" + patch.import +
                          "' does not fit in 32 bits");
        }
        put32(buffer, writeAt, static_cast<std::uint32_t>(value));
        break;
      }
      default:
        throw LinkError("internal: unsupported COFF relocation");
    }
  }

  std::vector<std::uint8_t> textSection(static_cast<std::size_t>(textVirtualSize), 0);
  if (!text.empty()) std::memcpy(textSection.data(), text.data(), text.size());
  for (std::size_t i = 0; i < imports.size(); ++i) {
    const std::uint64_t offset = thunkBase + 6 * static_cast<std::uint64_t>(i);
    const std::uint32_t slot = idataLayout.iatSlot[i];
    const std::uint64_t thunkRva = textRva + offset;
    const std::int64_t displacement =
        static_cast<std::int64_t>(slot) -
        static_cast<std::int64_t>(thunkRva + 6);
    textSection[offset] = 0xFF;
    textSection[offset + 1] = 0x25;
    put32(textSection, offset + 2, static_cast<std::uint32_t>(displacement));
  }

  const std::uint64_t startRva = textRva + startOffset;
  const std::uint64_t mainRva = textRva + mainDefinition->second.offset;
  textSection[startOffset + 0] = 0x48;  // sub rsp, 8
  textSection[startOffset + 1] = 0x83;
  textSection[startOffset + 2] = 0xEC;
  textSection[startOffset + 3] = 0x08;
  textSection[startOffset + 4] = 0x31;  // xor ecx, ecx
  textSection[startOffset + 5] = 0xC9;
  textSection[startOffset + 6] = 0x31;  // xor edx, edx
  textSection[startOffset + 7] = 0xD2;
  textSection[startOffset + 8] = 0xE8;  // call main
  put32(textSection, startOffset + 9,
        static_cast<std::uint32_t>(static_cast<std::int64_t>(mainRva) -
                                   static_cast<std::int64_t>(startRva + 13)));
  textSection[startOffset + 13] = 0x89;  // mov ecx, eax
  textSection[startOffset + 14] = 0xC1;
  textSection[startOffset + 15] = 0xFF;  // call [rip + ExitProcess IAT]
  textSection[startOffset + 16] = 0x15;
  put32(textSection, startOffset + 17,
        static_cast<std::uint32_t>(static_cast<std::int64_t>(exitIat->second) -
                                   static_cast<std::int64_t>(startRva + 21)));
  textSection[startOffset + 21] = 0xF4;  // hlt

  std::vector<std::uint8_t> out(static_cast<std::size_t>(fileSize), 0);
  out[0] = 'M';
  out[1] = 'Z';
  out[0x3C] = 0x40;
  std::memcpy(out.data() + 0x40, "PE\0\0", 4);
  const std::size_t fh = 0x44;
  put16(out, fh + 0, kCoffMachineAmd64);
  put16(out, fh + 2, static_cast<std::uint16_t>(sectionCount));
  put32(out, fh + 4, 0);
  put32(out, fh + 8, 0);
  put32(out, fh + 12, 0);
  put16(out, fh + 16, static_cast<std::uint16_t>(kOptionalHeaderSize));
  put16(out, fh + 18,
        static_cast<std::uint16_t>(kCharacteristicsExecutable |
                                   kCharacteristicsRelocsStripped |
                                   kCharacteristicsLargeAddressAware));

  const std::size_t oh = fh + 20;
  put16(out, oh + 0, 0x020B);
  out[oh + 2] = 1;
  put32(out, oh + 4, static_cast<std::uint32_t>(textRawSize));
  put32(out, oh + 8,
        static_cast<std::uint32_t>(idataRawSize + rdataRawSize + dataRawSize));
  put32(out, oh + 12, static_cast<std::uint32_t>(bssSize));
  put32(out, oh + 16, static_cast<std::uint32_t>(startRva));
  put32(out, oh + 20, textRva);
  put64(out, oh + 24, kPeImageBase);
  put32(out, oh + 32, kSectionAlignment);
  put32(out, oh + 36, kFileAlignment);
  put16(out, oh + 40, 6);
  put16(out, oh + 48, 6);
  put32(out, oh + 56, static_cast<std::uint32_t>(sizeOfImage));
  put32(out, oh + 60, static_cast<std::uint32_t>(sizeOfHeaders));
  put16(out, oh + 68, 3);
  put16(out, oh + 70, kDllCharacteristicsNxCompat);
  put64(out, oh + 72, kPeStackReserve);
  put64(out, oh + 80, kPeStackCommit);
  put64(out, oh + 88, 0x100000);
  put64(out, oh + 96, 0x1000);
  put32(out, oh + 104, 0);
  put32(out, oh + 108, 16);
  put32(out, oh + 112 + 8, idataLayout.descriptors);
  put32(out, oh + 112 + 12,
        static_cast<std::uint32_t>((dlls.size() + 1) * 20));
  put32(out, oh + 112 + 12 * 8, idataLayout.iat.empty() ? 0 : idataLayout.iat[0]);
  std::uint32_t iatSize = 0;
  for (const ImportDll& dll : dlls) {
    iatSize += static_cast<std::uint32_t>((dll.importIndices.size() + 1) * 8);
  }
  put32(out, oh + 112 + 12 * 8 + 4, iatSize);

  const std::size_t sh = oh + kOptionalHeaderSize;
  std::size_t cursor = sh;
  const auto addSection = [&](const char* name, std::uint32_t virtualSize,
                              std::uint32_t virtualAddress, std::uint32_t rawSize,
                              std::uint32_t rawOffset,
                              std::uint32_t characteristics) {
    std::memset(out.data() + cursor, 0, 40);
    std::memcpy(out.data() + cursor, name, std::strlen(name));
    put32(out, cursor + 8, virtualSize);
    put32(out, cursor + 12, virtualAddress);
    put32(out, cursor + 16, rawSize);
    put32(out, cursor + 20, rawOffset);
    put32(out, cursor + 36, characteristics);
    cursor += 40;
  };
  addSection(".idata", static_cast<std::uint32_t>(idata.size()), kIdataRva,
             static_cast<std::uint32_t>(idataRawSize),
             static_cast<std::uint32_t>(idataRawOffset),
             kSectionCharacteristicsIdata);
  addSection(".text", static_cast<std::uint32_t>(textVirtualSize), textRva,
             static_cast<std::uint32_t>(textRawSize),
             static_cast<std::uint32_t>(textRawOffset),
             kSectionCharacteristicsText);
  if (rodataSize != 0) {
    addSection(".rdata", static_cast<std::uint32_t>(rodataSize),
               static_cast<std::uint32_t>(rdataRva),
               static_cast<std::uint32_t>(rdataRawSize),
               static_cast<std::uint32_t>(rdataRawOffset),
               kSectionCharacteristicsRdata);
  }
  if (dataSize != 0) {
    addSection(".data", static_cast<std::uint32_t>(dataSize),
               static_cast<std::uint32_t>(dataRva),
               static_cast<std::uint32_t>(dataRawSize),
               static_cast<std::uint32_t>(dataRawOffset),
               kSectionCharacteristicsData);
  }
  if (bssSize != 0) {
    addSection(".bss", static_cast<std::uint32_t>(bssSize),
               static_cast<std::uint32_t>(bssRva), 0, 0,
               kSectionCharacteristicsBss);
  }

  std::memcpy(out.data() + idataRawOffset, idata.data(), idata.size());
  std::memcpy(out.data() + textRawOffset, textSection.data(), textSection.size());
  if (rodataSize != 0) {
    std::memcpy(out.data() + rdataRawOffset, rodata.data(), rodata.size());
  }
  if (dataSize != 0) {
    std::memcpy(out.data() + dataRawOffset, data.data(), data.size());
  }
  return out;
}

// ---------------------------------------------------------------------------
// PHASE 10.3b: relocatable COFF link (`oskelld -r` on Windows).
//
// The output is a normal COFF object: defined symbols are placed at their
// merged section offsets, local symbols are renamed (`name$object.index`) so a
// second link cannot confuse two statics with the same name, section symbols
// carry their aux record, and every relocation keeps its type and addend. A
// relocation against an input section symbol is retargeted to the output
// section symbol and its addend absorbs the input section's offset inside the
// merged section.
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> linkRelocatableCoff(
    const std::vector<LinkInput>& inputs, const LinkOptions& options) {
  if (options.target.arch != Arch::X86_64 || options.target.os != Os::Windows ||
      options.target.abi != Abi::Win64) {
    throw LinkError("the Windows linker supports x86_64-windows-win64 only");
  }

  std::vector<CoffObject> objects;
  for (const LinkInput& input : inputs) {
    if (input.bytes.size() >= 8 &&
        std::memcmp(input.bytes.data(), "!<arch>\n", 8) == 0) {
      throw LinkError("'" + input.name +
                      "' is an archive; MSVC .lib archives are PHASE 10.3b");
    }
    if (input.bytes.size() >= 2 && input.bytes[0] == 'M' && input.bytes[1] == 'Z') {
      throw LinkError("'" + input.name + "' is a PE image, not a COFF object");
    }
    objects.push_back(parseCoffObject(input.bytes, input.name));
  }
  if (objects.empty()) throw LinkError("no input objects");

  std::uint64_t textSize = 0;
  std::uint64_t rodataSize = 0;
  std::uint64_t dataSize = 0;
  std::uint64_t bssSize = 0;
  for (CoffObject& object : objects) {
    object.textOffset = alignUp(textSize, 16);
    textSize = object.textOffset + object.text.size();
    object.rodataOffset = alignUp(rodataSize, 16);
    rodataSize = object.rodataOffset + object.rodata.size();
    object.dataOffset = alignUp(dataSize, 16);
    dataSize = object.dataOffset + object.data.size();
    object.bssOffset = alignUp(bssSize, 8);
    bssSize = object.bssOffset + object.bssSize;
  }

  struct OutSection {
    struct Relocation {
      std::uint32_t offset = 0;
      std::uint32_t symbol = 0;
      std::uint16_t type = 0;
    };
    SectionKind kind = SectionKind::Text;
    std::string name;
    std::uint64_t size = 0;
    std::uint32_t index = 0;
    std::vector<std::uint8_t> bytes;
    std::vector<Relocation> relocations;
    std::uint32_t rawOffset = 0;
    std::uint32_t relocOffset = 0;
  };
  struct OutSymbol {
    std::string name;
    std::uint32_t value = 0;
    std::int16_t section = 0;
    std::uint16_t type = 0;
    std::uint8_t storage = 0;
    std::uint8_t aux = 0;
  };

  std::vector<std::uint8_t> text(static_cast<std::size_t>(textSize), 0);
  std::vector<std::uint8_t> rodata(static_cast<std::size_t>(rodataSize), 0);
  std::vector<std::uint8_t> data(static_cast<std::size_t>(dataSize), 0);
  for (const CoffObject& object : objects) {
    if (!object.text.empty()) {
      std::memcpy(text.data() + object.textOffset, object.text.data(),
                  object.text.size());
    }
    if (!object.rodata.empty()) {
      std::memcpy(rodata.data() + object.rodataOffset, object.rodata.data(),
                  object.rodata.size());
    }
    if (!object.data.empty()) {
      std::memcpy(data.data() + object.dataOffset, object.data.data(),
                  object.data.size());
    }
  }

  std::vector<OutSection> outSections;
  const auto addOutSection = [&](SectionKind kind, const char* name,
                                 std::uint64_t size,
                                 std::vector<std::uint8_t> bytes) {
    OutSection section;
    section.kind = kind;
    section.name = name;
    section.size = size;
    section.index = static_cast<std::uint32_t>(outSections.size() + 1);
    section.bytes = std::move(bytes);
    outSections.push_back(std::move(section));
  };
  if (textSize != 0) addOutSection(SectionKind::Text, ".text", textSize, std::move(text));
  if (rodataSize != 0) addOutSection(SectionKind::Rodata, ".rdata", rodataSize, std::move(rodata));
  if (dataSize != 0) addOutSection(SectionKind::Data, ".data", dataSize, std::move(data));
  if (bssSize != 0) addOutSection(SectionKind::Bss, ".bss", bssSize, {});
  if (outSections.empty()) throw LinkError("the inputs have no loadable sections");

  const auto outSection = [&](SectionKind kind) -> OutSection* {
    for (OutSection& section : outSections) {
      if (section.kind == kind) return &section;
    }
    return nullptr;
  };
  const auto outSectionIndex = [&](SectionKind kind) -> std::uint32_t {
    for (const OutSection& section : outSections) {
      if (section.kind == kind) return section.index;
    }
    return 0;
  };
  const auto kindIndex = [](SectionKind kind) -> std::size_t {
    return static_cast<std::size_t>(kind);
  };
  constexpr std::uint32_t kNoSymbol32 = std::numeric_limits<std::uint32_t>::max();

  std::vector<OutSymbol> symbols;
  std::vector<std::uint32_t> sectionSymbol(4, kNoSymbol32);
  for (const OutSection& section : outSections) {
    sectionSymbol[kindIndex(section.kind)] = static_cast<std::uint32_t>(symbols.size());
    OutSymbol symbol;
    symbol.name = section.name;
    symbol.value = 0;
    symbol.section = static_cast<std::int16_t>(section.index);
    symbol.type = 0;
    symbol.storage = kCoffSymClassStatic;
    symbol.aux = 1;
    symbols.push_back(std::move(symbol));
  }

  std::unordered_map<std::string, std::uint32_t> globalOut;
  std::vector<std::vector<std::uint32_t>> localOut(objects.size());
  for (std::size_t o = 0; o < objects.size(); ++o) {
    localOut[o].assign(objects[o].symbols.size(), kNoSymbol32);
  }

  // Defined globals first: a later undefined reference to the same name uses
  // the definition instead of creating an undefined duplicate.
  for (std::size_t o = 0; o < objects.size(); ++o) {
    const CoffObject& object = objects[o];
    for (const CoffSymbol& symbol : object.symbols) {
      if (symbol.name.empty() || symbol.section <= 0) continue;
      if (symbol.storageClass == kCoffSymClassFile) continue;
      if (static_cast<std::size_t>(symbol.section) > object.sections.size()) {
        throw LinkError("symbol '" + symbol.name + "' has an invalid section index");
      }
      const CoffSection& section =
          object.sections[static_cast<std::size_t>(symbol.section - 1)];
      if (section.kind == SectionKind::Ignored || isSectionSymbol(symbol, section)) {
        continue;
      }
      const bool global = symbol.storageClass == kCoffSymClassExternal ||
                          symbol.storageClass == kCoffSymClassWeakExternal;
      if (!global) continue;
      const std::uint32_t index = outSectionIndex(section.kind);
      if (index == 0) throw LinkError("global '" + symbol.name + "' is in an unsupported section");
      OutSymbol out;
      out.name = symbol.name;
      out.value = static_cast<std::uint32_t>(
          sectionOffsetOf(object, section.kind) + section.outputOffset + symbol.value);
      out.section = static_cast<std::int16_t>(index);
      out.type = symbol.type;
      out.storage = kCoffSymClassExternal;
      const auto inserted =
          globalOut.emplace(symbol.name, static_cast<std::uint32_t>(symbols.size()));
      if (!inserted.second) {
        throw LinkError("duplicate global symbol '" + symbol.name + "'");
      }
      symbols.push_back(std::move(out));
    }
  }

  // Defined locals. The suffix is not cosmetic: a relocatable output can hold
  // two statics with the same source name, and the second link resolves them
  // by symbol index, not by name.
  for (std::size_t o = 0; o < objects.size(); ++o) {
    const CoffObject& object = objects[o];
    for (std::size_t s = 0; s < object.symbols.size(); ++s) {
      const CoffSymbol& symbol = object.symbols[s];
      if (symbol.name.empty() || symbol.section <= 0) continue;
      if (symbol.storageClass == kCoffSymClassFile) continue;
      if (static_cast<std::size_t>(symbol.section) > object.sections.size()) {
        throw LinkError("symbol '" + symbol.name + "' has an invalid section index");
      }
      const CoffSection& section =
          object.sections[static_cast<std::size_t>(symbol.section - 1)];
      if (section.kind == SectionKind::Ignored || isSectionSymbol(symbol, section)) {
        continue;
      }
      const bool global = symbol.storageClass == kCoffSymClassExternal ||
                          symbol.storageClass == kCoffSymClassWeakExternal;
      if (global) continue;
      const std::uint32_t index = outSectionIndex(section.kind);
      if (index == 0) throw LinkError("local '" + symbol.name + "' is in an unsupported section");
      OutSymbol out;
      out.name = symbol.name + "$" + std::to_string(o) + "." +
                 std::to_string(symbol.rawIndex);
      out.value = static_cast<std::uint32_t>(
          sectionOffsetOf(object, section.kind) + section.outputOffset + symbol.value);
      out.section = static_cast<std::int16_t>(index);
      out.type = symbol.type;
      out.storage = kCoffSymClassStatic;
      localOut[o][s] = static_cast<std::uint32_t>(symbols.size());
      symbols.push_back(std::move(out));
    }
  }

  std::unordered_map<std::string, std::uint32_t> undefinedOut;
  for (const CoffObject& object : objects) {
    for (const CoffSymbol& symbol : object.symbols) {
      if (symbol.name.empty() || symbol.section != 0) continue;
      if (symbol.storageClass == kCoffSymClassFile) continue;
      if (globalOut.count(symbol.name) != 0 || undefinedOut.count(symbol.name) != 0) {
        continue;
      }
      OutSymbol out;
      out.name = symbol.name;
      out.value = 0;
      out.section = 0;
      out.type = symbol.type;
      out.storage = kCoffSymClassExternal;
      undefinedOut.emplace(symbol.name, static_cast<std::uint32_t>(symbols.size()));
      symbols.push_back(std::move(out));
    }
  }

  std::vector<std::uint32_t> rawIndex(symbols.size(), 0);
  std::uint32_t totalSymbols = 0;
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    rawIndex[i] = totalSymbols;
    totalSymbols += 1 + symbols[i].aux;
  }

  const auto addFieldAddend = [&](SectionKind sourceKind, std::uint64_t offset,
                                  std::uint16_t type, std::uint64_t base) {
    OutSection* source = outSection(sourceKind);
    if (source == nullptr) throw LinkError("internal: missing source section");
    std::size_t width = 0;
    switch (type) {
      case kCoffRelAmd64Addr64:
        width = 8;
        break;
      case kCoffRelAmd64Addr32:
      case kCoffRelAmd64Addr32Nb:
      case kCoffRelAmd64Rel32:
      case kCoffRelAmd64Rel32_1:
      case 6:
      case 7:
      case 8:
      case kCoffRelAmd64Rel32_5:
        width = 4;
        break;
      default:
        throw LinkError("unsupported COFF relocation type " +
                        std::to_string(type));
    }
    if (offset > source->bytes.size() ||
        width > source->bytes.size() - offset) {
      throw LinkError("relocation lies outside its section");
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
      value |= static_cast<std::uint64_t>(source->bytes[offset + i]) << (8 * i);
    }
    value += base;
    for (std::size_t i = 0; i < width; ++i) {
      source->bytes[offset + i] =
          static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
  };

  for (std::size_t o = 0; o < objects.size(); ++o) {
    const CoffObject& object = objects[o];
    for (const CoffRelocation& rel : object.relocations) {
      if (rel.symbolIndex >= object.symbolByRawIndex.size()) {
        throw LinkError("relocation in '" + object.name +
                        "' names an out-of-range symbol");
      }
      const std::size_t symbolIndex = object.symbolByRawIndex[rel.symbolIndex];
      if (symbolIndex == kNoSymbol) {
        throw LinkError("relocation in '" + object.name +
                        "' names an auxiliary symbol");
      }
      const CoffSymbol& symbol = object.symbols[symbolIndex];
      const CoffSection& source = object.sections[rel.sectionIndex];
      if (source.kind == SectionKind::Ignored || source.kind == SectionKind::Bss) {
        throw LinkError("relocation in an unsupported section of '" + object.name + "'");
      }
      OutSection* sourceOut = outSection(source.kind);
      if (sourceOut == nullptr) throw LinkError("internal: missing source section");
      const std::uint64_t fieldOffset =
          sectionOffsetOf(object, source.kind) + source.outputOffset + rel.offset;
      std::uint32_t outputSymbol = 0;
      if (symbol.section > 0) {
        if (static_cast<std::size_t>(symbol.section) > object.sections.size()) {
          throw LinkError("symbol '" + symbol.name + "' has an invalid section index");
        }
        const CoffSection& target =
            object.sections[static_cast<std::size_t>(symbol.section - 1)];
        if (isSectionSymbol(symbol, target)) {
          const std::uint32_t targetIndex = outSectionIndex(target.kind);
          if (targetIndex == 0) {
            throw LinkError("relocation targets an unsupported section in '" +
                            object.name + "'");
          }
          const std::uint32_t sectionOut = sectionSymbol[kindIndex(target.kind)];
          if (sectionOut == kNoSymbol32) {
            throw LinkError("internal: missing section symbol");
          }
          outputSymbol = rawIndex[sectionOut];
          addFieldAddend(source.kind, fieldOffset, rel.type,
                         sectionOffsetOf(object, target.kind) + target.outputOffset);
        } else {
          const bool global = symbol.storageClass == kCoffSymClassExternal ||
                              symbol.storageClass == kCoffSymClassWeakExternal;
          std::uint32_t outIndex = kNoSymbol32;
          if (global) {
            const auto found = globalOut.find(symbol.name);
            if (found != globalOut.end()) outIndex = found->second;
          } else {
            outIndex = localOut[o][symbolIndex];
          }
          if (outIndex == kNoSymbol32) {
            throw LinkError("symbol '" + symbol.name + "' was not placed");
          }
          outputSymbol = rawIndex[outIndex];
        }
      } else if (symbol.section == 0) {
        const auto defined = globalOut.find(symbol.name);
        if (defined != globalOut.end()) {
          outputSymbol = rawIndex[defined->second];
        } else {
          const auto undefined = undefinedOut.find(symbol.name);
          if (undefined == undefinedOut.end()) {
            throw LinkError("undefined symbol '" + symbol.name + "' was not placed");
          }
          outputSymbol = rawIndex[undefined->second];
        }
      } else {
        throw LinkError("relocation names an absolute or debug symbol");
      }
      OutSection::Relocation out;
      out.offset = static_cast<std::uint32_t>(fieldOffset);
      out.symbol = outputSymbol;
      out.type = rel.type;
      sourceOut->relocations.push_back(out);
    }
  }

  for (const OutSection& section : outSections) {
    if (section.relocations.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())) {
      throw LinkError("too many relocations in section '" + section.name + "'");
    }
  }

  const std::uint64_t sectionCount = outSections.size();
  const std::uint64_t headersSize = 20 + sectionCount * 40;
  std::uint64_t cursor = alignUp(headersSize, 16);
  for (OutSection& section : outSections) {
    if (section.kind == SectionKind::Bss) {
      section.rawOffset = 0;
      continue;
    }
    section.rawOffset = static_cast<std::uint32_t>(cursor);
    cursor = alignUp(cursor + section.bytes.size(), 16);
  }
  std::uint64_t relocCursor = alignUp(cursor, 4);
  for (OutSection& section : outSections) {
    if (section.relocations.empty()) {
      section.relocOffset = 0;
      continue;
    }
    section.relocOffset = static_cast<std::uint32_t>(relocCursor);
    relocCursor += static_cast<std::uint64_t>(section.relocations.size()) * 10;
  }
  const std::uint64_t symtabOffset = alignUp(relocCursor, 4);

  std::string strtab(4, '\0');
  std::vector<std::uint32_t> nameOffset(symbols.size(), 0);
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    if (symbols[i].name.size() <= 8) continue;
    nameOffset[i] = static_cast<std::uint32_t>(strtab.size());
    strtab += symbols[i].name;
    strtab += '\0';
  }
  const std::uint32_t strtabSize = static_cast<std::uint32_t>(strtab.size());
  for (int i = 0; i < 4; ++i) {
    strtab[static_cast<std::size_t>(i)] =
        static_cast<char>((strtabSize >> (8 * i)) & 0xFF);
  }
  const std::uint64_t symtabSize = static_cast<std::uint64_t>(totalSymbols) * 18;
  const std::uint64_t fileSize = symtabOffset + symtabSize + strtab.size();
  std::vector<std::uint8_t> out(static_cast<std::size_t>(fileSize), 0);

  put16(out, 0, kCoffMachineAmd64);
  put16(out, 2, static_cast<std::uint16_t>(sectionCount));
  put32(out, 4, 0);
  put32(out, 8, static_cast<std::uint32_t>(symtabOffset));
  put32(out, 12, totalSymbols);
  put16(out, 16, 0);
  put16(out, 18, 0);

  std::uint64_t sectionHeader = 20;
  for (const OutSection& section : outSections) {
    std::memset(out.data() + sectionHeader, 0, 40);
    std::memcpy(out.data() + sectionHeader, section.name.data(),
                std::min<std::size_t>(section.name.size(), 8));
    put32(out, sectionHeader + 8,
          section.kind == SectionKind::Bss ? static_cast<std::uint32_t>(section.size) : 0);
    put32(out, sectionHeader + 12, 0);
    put32(out, sectionHeader + 16,
          section.kind == SectionKind::Bss ? 0
                                           : static_cast<std::uint32_t>(section.bytes.size()));
    put32(out, sectionHeader + 20, section.rawOffset);
    put32(out, sectionHeader + 24, section.relocOffset);
    put32(out, sectionHeader + 28, 0);
    put16(out, sectionHeader + 32,
          static_cast<std::uint16_t>(section.relocations.size()));
    put16(out, sectionHeader + 34, 0);
    std::uint32_t characteristics = 0;
    switch (section.kind) {
      case SectionKind::Text: characteristics = 0x60000020; break;
      case SectionKind::Rodata: characteristics = 0x40000040; break;
      case SectionKind::Data: characteristics = 0xC0000040; break;
      case SectionKind::Bss: characteristics = 0xC0000080; break;
      default: break;
    }
    put32(out, sectionHeader + 36, characteristics);
    sectionHeader += 40;
  }

  for (const OutSection& section : outSections) {
    if (section.kind == SectionKind::Bss || section.bytes.empty()) continue;
    std::memcpy(out.data() + section.rawOffset, section.bytes.data(),
                section.bytes.size());
  }
  for (const OutSection& section : outSections) {
    std::uint64_t offset = section.relocOffset;
    for (const OutSection::Relocation& rel : section.relocations) {
      put32(out, offset, rel.offset);
      put32(out, offset + 4, rel.symbol);
      put16(out, offset + 8, rel.type);
      offset += 10;
    }
  }

  std::uint64_t symbolOffset = symtabOffset;
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    const OutSymbol& symbol = symbols[i];
    if (symbol.name.size() <= 8) {
      std::memset(out.data() + symbolOffset, 0, 8);
      std::memcpy(out.data() + symbolOffset, symbol.name.data(),
                  symbol.name.size());
    } else {
      put32(out, symbolOffset, 0);
      put32(out, symbolOffset + 4, nameOffset[i]);
    }
    put32(out, symbolOffset + 8, symbol.value);
    put16(out, symbolOffset + 12, static_cast<std::uint16_t>(symbol.section));
    put16(out, symbolOffset + 14, symbol.type);
    put8(out, symbolOffset + 16, symbol.storage);
    put8(out, symbolOffset + 17, symbol.aux);
    symbolOffset += 18;
    if (symbol.aux != 0) {
      if (symbol.section <= 0 ||
          static_cast<std::size_t>(symbol.section) > outSections.size()) {
        throw LinkError("internal: section symbol has no section");
      }
      const OutSection& section = outSections[static_cast<std::size_t>(symbol.section - 1)];
      put32(out, symbolOffset + 0, static_cast<std::uint32_t>(section.size));
      put16(out, symbolOffset + 4,
            static_cast<std::uint16_t>(section.relocations.size()));
      put16(out, symbolOffset + 6, 0);
      put32(out, symbolOffset + 8, 0);
      put16(out, symbolOffset + 12, 0);
      put8(out, symbolOffset + 14, 0);
      symbolOffset += 18;
    }
  }
  std::memcpy(out.data() + symtabOffset + symtabSize, strtab.data(), strtab.size());
  return out;
}
