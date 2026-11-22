#include "native/link.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// ELF constants (System V gABI + the x86-64 psABI). Named so the layout below
// reads like the specification instead of like magic numbers.
// ---------------------------------------------------------------------------
constexpr std::uint64_t kBaseAddress = 0x400000;
constexpr std::uint64_t kPageSize = 0x1000;

constexpr std::uint8_t kElfClass64 = 2;
constexpr std::uint8_t kElfDataLsb = 1;
constexpr std::uint8_t kEvCurrent = 1;
constexpr std::uint16_t kEtRel = 1;
constexpr std::uint16_t kEtExec = 2;
constexpr std::uint16_t kEtDyn = 3;
constexpr std::uint16_t kEmX86_64 = 62;

constexpr std::uint32_t kShtNull = 0;
constexpr std::uint32_t kShtProgbits = 1;
constexpr std::uint32_t kShtSymtab = 2;
constexpr std::uint32_t kShtStrtab = 3;
constexpr std::uint32_t kShtRela = 4;
constexpr std::uint32_t kShtHash = 5;
constexpr std::uint32_t kShtDynamic = 6;
constexpr std::uint32_t kShtDynsym = 11;

constexpr std::uint64_t kShfWrite = 0x1;
constexpr std::uint64_t kShfAlloc = 0x2;
constexpr std::uint64_t kShfExecInstr = 0x4;

constexpr std::uint8_t kStbGlobal = 1;
constexpr std::uint8_t kSttNotype = 0;
constexpr std::uint8_t kSttSection = 3;
constexpr std::uint16_t kShnUndef = 0;

constexpr std::uint32_t kR_x86_64Pc32 = 2;
constexpr std::uint32_t kR_x86_64Plt32 = 4;
constexpr std::uint32_t kR_x86_64GlobDat = 6;
constexpr std::uint32_t kR_x86_64JumpSlot = 7;

constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtDynamic = 2;
constexpr std::uint32_t kPtInterp = 3;
constexpr std::uint32_t kPtPhdr = 6;
constexpr std::uint32_t kPtGnuStack = 0x6474e551;

constexpr std::uint32_t kPfX = 1;
constexpr std::uint32_t kPfW = 2;
constexpr std::uint32_t kPfR = 4;

constexpr std::int64_t kDtNull = 0;
constexpr std::int64_t kDtNeeded = 1;
constexpr std::int64_t kDtPltRelSz = 2;
constexpr std::int64_t kDtPltGot = 3;
constexpr std::int64_t kDtHash = 4;
constexpr std::int64_t kDtStrtab = 5;
constexpr std::int64_t kDtSymtab = 6;
constexpr std::int64_t kDtRela = 7;
constexpr std::int64_t kDtRelaSz = 8;
constexpr std::int64_t kDtRelaEnt = 9;
constexpr std::int64_t kDtStrsz = 10;
constexpr std::int64_t kDtSyment = 11;
constexpr std::int64_t kDtSoname = 14;
constexpr std::int64_t kDtPltRel = 20;
constexpr std::int64_t kDtDebug = 21;
constexpr std::int64_t kDtJmprel = 23;
constexpr std::int64_t kDtBindNow = 24;
constexpr std::int64_t kDtFlags = 30;
constexpr std::uint64_t kDfBindNow = 0x8;

constexpr std::uint64_t kEhdrSize = 64;
constexpr std::uint64_t kPhdrSize = 56;
constexpr std::uint64_t kShdrSize = 64;
constexpr std::uint64_t kSymSize = 24;
constexpr std::uint64_t kRelaSize = 24;
constexpr std::uint64_t kDynSize = 16;
constexpr std::uint64_t kStartSize = 34;

constexpr const char* kInterpreter = "/lib64/ld-linux-x86-64.so.2";

// A bounds-checked little-endian reader. Every offset the linker trusts comes
// from a file it did not write, so a short or lying header is a diagnostic, not
// an out-of-bounds read.
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
      throw LinkError("truncated ELF data");
    }
  }

  const std::vector<std::uint8_t>& data_;
};

std::string readCString(const Reader& reader, std::uint64_t offset) {
  std::string text;
  while (true) {
    const std::uint8_t byte = reader.u8(offset++);
    if (byte == 0) break;
    text.push_back(static_cast<char>(byte));
  }
  return text;
}

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) {
  const std::uint64_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}

void put8(std::vector<std::uint8_t>& out, std::uint64_t offset, std::uint8_t value) {
  if (offset >= out.size()) throw LinkError("internal: image write out of range");
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

// ELF's string table hash. Used for the SysV `.hash` section; the loader only
// needs it to be a valid table, but a correct one keeps lookup cost sane.
std::uint32_t elfHash(const std::string& name) {
  std::uint32_t hash = 0;
  for (const char c : name) {
    hash = (hash << 4) + static_cast<std::uint8_t>(c);
    const std::uint32_t high = hash & 0xF0000000u;
    if (high != 0) hash ^= high >> 24;
    hash &= ~high;
  }
  return hash;
}

// A string table that hands back the offset it stored each name at. ELF string
// tables start with a NUL byte, so offset 0 is the empty string.
struct StringTable {
  std::string data = std::string(1, '\0');
  std::unordered_map<std::string, std::uint32_t> offsets;

  std::uint32_t add(const std::string& text) {
    const auto found = offsets.find(text);
    if (found != offsets.end()) return found->second;
    const std::uint32_t offset = static_cast<std::uint32_t>(data.size());
    data += text;
    data.push_back('\0');
    offsets.emplace(text, offset);
    return offset;
  }
};

struct Section {
  std::string name;
  std::uint32_t type = kShtNull;
  std::uint64_t flags = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint32_t link = 0;
  std::uint32_t info = 0;
  std::uint64_t addralign = 1;
  std::uint64_t entsize = 0;
};

std::vector<Section> readSections(const Reader& reader, std::uint64_t shoff,
                                  std::uint16_t shentsize, std::uint16_t shnum,
                                  std::uint16_t shstrndx) {
  std::vector<Section> sections(shnum);
  for (std::uint16_t i = 0; i < shnum; ++i) {
    const std::uint64_t base = shoff + static_cast<std::uint64_t>(i) * shentsize;
    Section& section = sections[i];
    section.type = reader.u32(base + 4);
    section.flags = reader.u64(base + 8);
    section.offset = reader.u64(base + 24);
    section.size = reader.u64(base + 32);
    section.link = reader.u32(base + 40);
    section.info = reader.u32(base + 44);
    section.addralign = reader.u64(base + 48);
    section.entsize = reader.u64(base + 56);
  }
  const Section& names = sections[shstrndx];
  for (std::uint16_t i = 0; i < shnum; ++i) {
    const std::uint64_t base = shoff + static_cast<std::uint64_t>(i) * shentsize;
    const std::uint32_t nameOffset = reader.u32(base);
    if (nameOffset < names.size) {
      sections[i].name = readCString(reader, names.offset + nameOffset);
    }
  }
  return sections;
}

const Section* findSection(const std::vector<Section>& sections,
                           const std::string& name) {
  for (const Section& section : sections) {
    if (section.name == name) return &section;
  }
  return nullptr;
}

// Rejects a section whose declared extent leaves the file before any offset is
// computed from it. The input is usually our own object, but the linker also
// reads system shared libraries, and a lying `sh_size` must be a diagnostic,
// not an out-of-range read.
void requireSectionInFile(const Section& section, std::size_t fileSize,
                          const std::string& name) {
  if (section.offset > fileSize || section.size > fileSize - section.offset) {
    throw LinkError("section '" + name + "' lies outside the file");
  }
}

struct ObjectSymbol {
  std::string name;
  std::uint8_t info = 0;
  std::uint16_t shndx = kShnUndef;
  std::uint64_t value = 0;
};

struct Rela {
  std::uint64_t offset = 0;
  std::uint32_t symbol = 0;
  std::uint32_t type = 0;
  std::int64_t addend = 0;
};

struct ObjectFile {
  std::vector<std::uint8_t> text;
  std::vector<std::uint8_t> rodata;
  std::vector<ObjectSymbol> symbols;
  std::vector<Rela> relas;
};

ObjectFile parseObject(const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < kEhdrSize) throw LinkError("object is too small to be ELF64");
  const Reader reader(bytes);
  if (reader.u8(0) != 0x7F || reader.u8(1) != 'E' || reader.u8(2) != 'L' ||
      reader.u8(3) != 'F') {
    throw LinkError("object is not an ELF file");
  }
  if (reader.u8(4) != kElfClass64) throw LinkError("object is not ELF64");
  if (reader.u8(5) != kElfDataLsb) throw LinkError("object is not little-endian");
  if (reader.u16(16) != kEtRel) {
    throw LinkError("object is not relocatable (ET_REL)");
  }
  if (reader.u16(18) != kEmX86_64) throw LinkError("object is not x86-64");

  const std::uint64_t shoff = reader.u64(40);
  const std::uint16_t shentsize = reader.u16(58);
  const std::uint16_t shnum = reader.u16(60);
  const std::uint16_t shstrndx = reader.u16(62);
  if (shnum == 0) throw LinkError("object has no section table");
  if (shentsize < kShdrSize) {
    throw LinkError("object has an unsupported section header size");
  }
  if (shstrndx >= shnum) {
    throw LinkError("object has an invalid section name table");
  }
  const std::vector<Section> sections =
      readSections(reader, shoff, shentsize, shnum, shstrndx);

  const Section* text = findSection(sections, ".text");
  const Section* symtab = findSection(sections, ".symtab");
  const Section* strtab = findSection(sections, ".strtab");
  const Section* rela = findSection(sections, ".rela.text");
  const Section* rodata = findSection(sections, ".rodata");
  if (text == nullptr) throw LinkError("object has no .text section");
  if (symtab == nullptr || strtab == nullptr) {
    throw LinkError("object has no symbol table");
  }
  if (symtab->size % kSymSize != 0) {
    throw LinkError("object has a malformed symbol table");
  }
  if (rela != nullptr && rela->size % kRelaSize != 0) {
    throw LinkError("object has a malformed relocation table");
  }

  requireSectionInFile(*text, bytes.size(), ".text");
  requireSectionInFile(*symtab, bytes.size(), ".symtab");
  requireSectionInFile(*strtab, bytes.size(), ".strtab");
  if (rela != nullptr) requireSectionInFile(*rela, bytes.size(), ".rela.text");
  ObjectFile object;
  object.text.assign(bytes.begin() + static_cast<std::ptrdiff_t>(text->offset),
                     bytes.begin() + static_cast<std::ptrdiff_t>(text->offset + text->size));
  if (rodata != nullptr) {
    requireSectionInFile(*rodata, bytes.size(), ".rodata");
    object.rodata.assign(
        bytes.begin() + static_cast<std::ptrdiff_t>(rodata->offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(rodata->offset + rodata->size));
  }

  const std::uint64_t symbolCount = symtab->size / kSymSize;
  object.symbols.resize(symbolCount);
  for (std::uint64_t i = 0; i < symbolCount; ++i) {
    const std::uint64_t base = symtab->offset + i * kSymSize;
    const std::uint32_t nameOffset = reader.u32(base);
    ObjectSymbol& symbol = object.symbols[i];
    symbol.info = reader.u8(base + 4);
    symbol.shndx = reader.u16(base + 6);
    symbol.value = reader.u64(base + 8);
    if (nameOffset != 0) {
      if (nameOffset >= strtab->size) {
        throw LinkError("object has a symbol name outside .strtab");
      }
      symbol.name = readCString(reader, strtab->offset + nameOffset);
    }
  }

  if (rela != nullptr) {
    const std::uint64_t relaCount = rela->size / kRelaSize;
    object.relas.resize(relaCount);
    for (std::uint64_t i = 0; i < relaCount; ++i) {
      const std::uint64_t base = rela->offset + i * kRelaSize;
      const std::uint64_t info = reader.u64(base + 8);
      Rela& entry = object.relas[i];
      entry.offset = reader.u64(base);
      entry.symbol = static_cast<std::uint32_t>(info >> 32);
      entry.type = static_cast<std::uint32_t>(info & 0xFFFFFFFFu);
      entry.addend = static_cast<std::int64_t>(reader.u64(base + 16));
    }
  }
  return object;
}

std::vector<std::uint8_t> readFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0) return {};
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  in.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!in) return {};
  return bytes;
}

bool fileExists(const std::string& path) {
  std::ifstream probe(path, std::ios::binary);
  return probe.good();
}

std::string baseName(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct SharedLibrary {
  std::string fileName;
  std::string soname;
  std::unordered_set<std::string> exports;
};

SharedLibrary loadSharedLibrary(const std::string& path) {
  const std::vector<std::uint8_t> bytes = readFileBytes(path);
  if (bytes.empty()) throw LinkError("cannot read '" + path + "'");
  if (bytes.size() < kEhdrSize) throw LinkError("'" + path + "' is too small");
  const Reader reader(bytes);
  if (reader.u8(0) != 0x7F || reader.u8(1) != 'E' || reader.u8(2) != 'L' ||
      reader.u8(3) != 'F') {
    throw LinkError("'" + path + "' is not an ELF file");
  }
  if (reader.u8(4) != kElfClass64 || reader.u8(5) != kElfDataLsb) {
    throw LinkError("'" + path + "' is not a little-endian ELF64 file");
  }
  const std::uint16_t type = reader.u16(16);
  if (type != kEtDyn && type != kEtExec) {
    throw LinkError("'" + path + "' is not a shared object");
  }
  if (reader.u16(18) != kEmX86_64) throw LinkError("'" + path + "' is not x86-64");

  const std::uint64_t shoff = reader.u64(40);
  const std::uint16_t shentsize = reader.u16(58);
  const std::uint16_t shnum = reader.u16(60);
  const std::uint16_t shstrndx = reader.u16(62);
  if (shnum == 0 || shentsize < kShdrSize || shstrndx >= shnum) {
    throw LinkError("'" + path + "' has no usable section table");
  }
  const std::vector<Section> sections =
      readSections(reader, shoff, shentsize, shnum, shstrndx);
  const Section* dynsym = findSection(sections, ".dynsym");
  const Section* dynstr = findSection(sections, ".dynstr");
  const Section* dynamic = findSection(sections, ".dynamic");
  if (dynsym == nullptr || dynstr == nullptr) {
    throw LinkError("'" + path + "' has no dynamic symbol table");
  }
  requireSectionInFile(*dynsym, bytes.size(), ".dynsym");
  requireSectionInFile(*dynstr, bytes.size(), ".dynstr");
  if (dynamic != nullptr) requireSectionInFile(*dynamic, bytes.size(), ".dynamic");
  if (dynsym->size % kSymSize != 0) {
    throw LinkError("'" + path + "' has a malformed dynamic symbol table");
  }

  SharedLibrary library;
  library.fileName = baseName(path);
  const std::uint64_t count = dynsym->size / kSymSize;
  for (std::uint64_t i = 0; i < count; ++i) {
    const std::uint64_t base = dynsym->offset + i * kSymSize;
    const std::uint32_t nameOffset = reader.u32(base);
    const std::uint16_t shndx = reader.u16(base + 6);
    if (nameOffset == 0 || shndx == kShnUndef || nameOffset >= dynstr->size) {
      continue;
    }
    library.exports.insert(readCString(reader, dynstr->offset + nameOffset));
  }

  if (dynamic != nullptr && dynamic->size % kDynSize == 0) {
    const std::uint64_t entries = dynamic->size / kDynSize;
    for (std::uint64_t i = 0; i < entries; ++i) {
      const std::uint64_t base = dynamic->offset + i * kDynSize;
      const std::int64_t tag = static_cast<std::int64_t>(reader.u64(base));
      const std::uint64_t value = reader.u64(base + 8);
      if (tag == kDtNull) break;
      if (tag == kDtSoname && value < dynstr->size) {
        library.soname = readCString(reader, dynstr->offset + value);
        break;
      }
    }
  }
  return library;
}

void appendPathList(std::vector<std::string>& out, const char* value) {
  if (value == nullptr) return;
  const std::string list(value);
  std::size_t start = 0;
  while (start <= list.size()) {
    const std::size_t end = list.find(':', start);
    const std::string dir =
        list.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!dir.empty()) out.push_back(dir);
    if (end == std::string::npos) break;
    start = end + 1;
  }
}

std::string findLibraryFile(const std::string& fileName,
                            const std::vector<std::string>& directories) {
  for (const std::string& directory : directories) {
    if (directory.empty()) continue;
    std::string candidate = directory;
    if (candidate.back() != '/' && candidate.back() != '\\') candidate += '/';
    candidate += fileName;
    if (fileExists(candidate)) return candidate;
  }
  return {};
}

bool isSafeLibraryName(const std::string& name) {
  if (name.empty()) return false;
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '+' ||
                    c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

bool isOwnLinkerTarget(const Target& target) {
  return target.arch == Arch::X86_64 && target.os == Os::Linux &&
         target.abi == Abi::SysV;
}

std::vector<std::uint8_t> linkHostedExecutable(
    const std::vector<std::uint8_t>& object, const LinkOptions& options) {
  if (!isOwnLinkerTarget(options.target)) {
    throw LinkError("the own linker supports x86_64-linux-sysv only");
  }
  const ObjectFile parsed = parseObject(object);

  // --- Defined symbols ---
  std::unordered_map<std::string, std::uint64_t> defined;
  for (const ObjectSymbol& symbol : parsed.symbols) {
    if (symbol.name.empty() || symbol.shndx == kShnUndef) continue;
    if ((symbol.info & 0x0F) == kSttSection) continue;
    if (defined.count(symbol.name) != 0) {
      throw LinkError("duplicate symbol '" + symbol.name + "'");
    }
    defined.emplace(symbol.name, symbol.value);
  }
  const auto mainSymbol = defined.find("main");
  if (mainSymbol == defined.end()) {
    throw LinkError("the object does not export 'main'");
  }
  if (mainSymbol->second >= parsed.text.size()) {
    throw LinkError("the 'main' symbol lies outside .text");
  }

  // --- Relocations, and the PLT/GOT slots they need ---
  struct Patch {
    std::uint64_t offset = 0;
    std::uint32_t type = 0;
    std::string symbol;
    bool defined = false;
    std::uint64_t value = 0;
    std::int64_t addend = 0;
  };
  std::vector<Patch> patches;
  std::vector<std::string> pltSymbols;
  std::vector<std::string> gotSymbols;
  std::unordered_map<std::string, int> pltIndex;
  std::unordered_map<std::string, int> gotIndex;

  const auto addPlt = [&](const std::string& name) {
    if (pltIndex.count(name) != 0) return;
    pltIndex.emplace(name, static_cast<int>(pltSymbols.size()));
    pltSymbols.push_back(name);
  };
  const auto addGot = [&](const std::string& name) {
    if (gotIndex.count(name) != 0) return;
    gotIndex.emplace(name, static_cast<int>(gotSymbols.size()));
    gotSymbols.push_back(name);
  };

  for (const Rela& rela : parsed.relas) {
    if (rela.symbol >= parsed.symbols.size()) {
      throw LinkError("relocation names an out-of-range symbol");
    }
    const ObjectSymbol& symbol = parsed.symbols[rela.symbol];
    if (symbol.name.empty()) {
      throw LinkError("relocation names an unnamed symbol");
    }
    if (rela.type != kR_x86_64Plt32 && rela.type != kR_x86_64Pc32) {
      throw LinkError("unsupported relocation type " + std::to_string(rela.type) +
                      " for '" + symbol.name + "'");
    }
    if (rela.offset + 4 > parsed.text.size()) {
      throw LinkError("relocation for '" + symbol.name + "' lies outside .text");
    }
    const bool isDefined = defined.count(symbol.name) != 0;
    if (!isDefined) {
      if (rela.type == kR_x86_64Plt32) {
        addPlt(symbol.name);
      } else {
        addGot(symbol.name);
      }
    }
    Patch patch;
    patch.offset = rela.offset;
    patch.type = rela.type;
    patch.symbol = symbol.name;
    patch.defined = isDefined;
    patch.value = isDefined ? defined.at(symbol.name) : 0;
    patch.addend = rela.addend;
    patches.push_back(std::move(patch));
  }
  // `_start` calls libc's entry point, which the program itself never names.
  addPlt("__libc_start_main");

  // --- Dynamic symbols (PLT order, then GOT-only order) ---
  std::vector<std::string> dynSymbols = pltSymbols;
  std::unordered_map<std::string, std::uint32_t> dynIndex;
  for (std::size_t i = 0; i < dynSymbols.size(); ++i) {
    dynIndex.emplace(dynSymbols[i], static_cast<std::uint32_t>(i + 1));
  }
  for (const std::string& name : gotSymbols) {
    if (dynIndex.count(name) != 0) continue;
    dynIndex.emplace(name, static_cast<std::uint32_t>(dynSymbols.size() + 1));
    dynSymbols.push_back(name);
  }

  // --- Locate the libraries and validate every undefined symbol ---
  std::vector<std::string> searchPaths = options.libraryPaths;
  appendPathList(searchPaths, std::getenv("LIBRARY_PATH"));
  appendPathList(searchPaths, std::getenv("LD_LIBRARY_PATH"));
  searchPaths.push_back("/lib/x86_64-linux-gnu");
  searchPaths.push_back("/usr/lib/x86_64-linux-gnu");
  searchPaths.push_back("/lib64");
  searchPaths.push_back("/usr/lib64");
  searchPaths.push_back("/lib");
  searchPaths.push_back("/usr/lib");
  searchPaths.push_back("/usr/local/lib");

  const std::string libcPath = findLibraryFile("libc.so.6", searchPaths);
  if (libcPath.empty()) {
    throw LinkError("cannot find libc.so.6; pass -L <directory>");
  }
  const SharedLibrary libc = loadSharedLibrary(libcPath);

  std::vector<SharedLibrary> extras;
  for (const std::string& name : options.libraries) {
    if (!isSafeLibraryName(name)) {
      throw LinkError("invalid library name '" + name + "'");
    }
    const std::string fileName = "lib" + name + ".so";
    const std::string path = findLibraryFile(fileName, searchPaths);
    if (path.empty()) {
      throw LinkError("cannot find " + fileName + "; pass -L <directory>");
    }
    extras.push_back(loadSharedLibrary(path));
  }

  bool needLibm = false;
  bool libmLoaded = false;
  SharedLibrary libm;
  for (const std::string& name : dynSymbols) {
    if (libc.exports.count(name) != 0) continue;
    if (!libmLoaded) {
      const std::string path = findLibraryFile("libm.so.6", searchPaths);
      if (!path.empty()) {
        libm = loadSharedLibrary(path);
        libmLoaded = true;
      }
    }
    if (libmLoaded && libm.exports.count(name) != 0) {
      needLibm = true;
      continue;
    }
    bool found = false;
    for (const SharedLibrary& extra : extras) {
      if (extra.exports.count(name) != 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      throw LinkError("undefined symbol '" + name +
                      "' (not in libc, libm or a --link-lib library)");
    }
  }

  std::vector<std::string> needed;
  needed.push_back(libc.soname.empty() ? "libc.so.6" : libc.soname);
  if (needLibm) {
    needed.push_back(libm.soname.empty() ? "libm.so.6" : libm.soname);
  }
  for (const SharedLibrary& extra : extras) {
    needed.push_back(extra.soname.empty() ? extra.fileName : extra.soname);
  }

  // --- Sizes ---
  StringTable dynstr;
  for (const std::string& name : dynSymbols) dynstr.add(name);
  std::vector<std::uint32_t> neededOffsets;
  for (const std::string& name : needed) neededOffsets.push_back(dynstr.add(name));

  const std::uint64_t interpSize = std::strlen(kInterpreter) + 1;
  const std::uint64_t dynstrSize = dynstr.data.size();
  const std::uint64_t dynsymSize = (1 + dynSymbols.size()) * kSymSize;
  const std::uint64_t chainCount = 1 + dynSymbols.size();
  const std::uint64_t bucketCount = std::max<std::uint64_t>(1, chainCount);
  const std::uint64_t hashSize = 8 + 4 * bucketCount + 4 * chainCount;
  const std::uint64_t relaPltSize = pltSymbols.size() * kRelaSize;
  const std::uint64_t relaDynSize = gotSymbols.size() * kRelaSize;
  const std::uint64_t pltSize = 16 * (1 + pltSymbols.size());
  const std::uint64_t gotSize = 8 * (3 + pltSymbols.size() + gotSymbols.size());
  const std::uint64_t dynamicEntries =
      needed.size() + 12 + (gotSymbols.empty() ? 0 : 3);
  const std::uint64_t dynamicSize = (dynamicEntries + 1) * kDynSize;
  const std::uint64_t textSize = parsed.text.size();
  const std::uint64_t rodataSize = parsed.rodata.size();
  const bool hasRodata = rodataSize != 0;
  const std::uint64_t phnum = hasRodata ? 8 : 7;

  // --- Layout. `vaddr(offset) == kBaseAddress + offset` for every allocated
  // block, because each segment starts page-aligned with p_offset == p_vaddr -
  // kBaseAddress. ---
  std::uint64_t offset = kEhdrSize + phnum * kPhdrSize;
  const std::uint64_t interpOff = offset;
  offset += interpSize;
  const std::uint64_t dynstrOff = offset;
  offset += dynstrSize;
  offset = alignUp(offset, 8);
  const std::uint64_t dynsymOff = offset;
  offset += dynsymSize;
  offset = alignUp(offset, 8);
  const std::uint64_t hashOff = offset;
  offset += hashSize;
  offset = alignUp(offset, 8);
  const std::uint64_t relaPltOff = offset;
  offset += relaPltSize;
  offset = alignUp(offset, 8);
  const std::uint64_t relaDynOff = offset;
  offset += relaDynSize;
  const std::uint64_t readOnlyEnd = offset;

  const std::uint64_t rxStart = alignUp(readOnlyEnd, kPageSize);
  const std::uint64_t textOff = rxStart;
  offset = textOff + textSize;
  offset = alignUp(offset, 16);
  const std::uint64_t pltOff = offset;
  offset += pltSize;
  const std::uint64_t startOff = offset;
  offset += kStartSize;
  const std::uint64_t rxEnd = offset;

  const std::uint64_t roStart = alignUp(rxEnd, kPageSize);
  const std::uint64_t rodataOff = roStart;
  offset = roStart + rodataSize;

  const std::uint64_t rwStart = alignUp(offset, kPageSize);
  const std::uint64_t gotOff = rwStart;
  offset = gotOff + gotSize;
  offset = alignUp(offset, 8);
  const std::uint64_t dynamicOff = offset;
  offset += dynamicSize;
  const std::uint64_t rwEnd = offset;

  // --- Section table ---
  struct SectionSpec {
    std::string name;
    std::uint32_t nameOffset = 0;
    std::uint32_t type = kShtNull;
    std::uint64_t flags = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t link = 0;
    std::uint32_t info = 0;
    std::uint64_t addralign = 1;
    std::uint64_t entsize = 0;
  };
  std::vector<SectionSpec> sections(1);  // index 0: the null section
  const auto addSection = [&](const std::string& name, std::uint32_t type,
                              std::uint64_t flags, std::uint64_t sectionOffset,
                              std::uint64_t size, std::uint64_t alignment,
                              std::uint64_t entsize) {
    SectionSpec spec;
    spec.name = name;
    spec.type = type;
    spec.flags = flags;
    spec.offset = sectionOffset;
    spec.size = size;
    spec.addralign = alignment;
    spec.entsize = entsize;
    sections.push_back(std::move(spec));
    return static_cast<std::uint32_t>(sections.size() - 1);
  };

  addSection(".interp", kShtProgbits, kShfAlloc, interpOff, interpSize, 1, 0);
  const std::uint32_t dynstrSection =
      addSection(".dynstr", kShtStrtab, kShfAlloc, dynstrOff, dynstrSize, 1, 0);
  const std::uint32_t dynsymSection =
      addSection(".dynsym", kShtDynsym, kShfAlloc, dynsymOff, dynsymSize, 8, kSymSize);
  const std::uint32_t hashSection =
      addSection(".hash", kShtHash, kShfAlloc, hashOff, hashSize, 8, 4);
  const std::uint32_t relaPltSection =
      addSection(".rela.plt", kShtRela, kShfAlloc, relaPltOff, relaPltSize, 8, kRelaSize);
  std::uint32_t relaDynSection = 0;
  if (!gotSymbols.empty()) {
    relaDynSection =
        addSection(".rela.dyn", kShtRela, kShfAlloc, relaDynOff, relaDynSize, 8, kRelaSize);
  }
  addSection(".text", kShtProgbits, kShfAlloc | kShfExecInstr, textOff, textSize, 16, 0);
  addSection(".plt", kShtProgbits, kShfAlloc | kShfExecInstr, pltOff, pltSize, 16, 16);
  addSection(".text.start", kShtProgbits, kShfAlloc | kShfExecInstr, startOff,
             kStartSize, 16, 0);
  if (hasRodata) {
    addSection(".rodata", kShtProgbits, kShfAlloc, rodataOff, rodataSize, 8, 0);
  }
  const std::uint32_t gotSection =
      addSection(".got.plt", kShtProgbits, kShfAlloc | kShfWrite, gotOff, gotSize, 8, 8);
  const std::uint32_t dynamicSection = addSection(
      ".dynamic", kShtDynamic, kShfAlloc | kShfWrite, dynamicOff, dynamicSize, 8, kDynSize);
  const std::uint64_t shstrOff = rwEnd;
  const std::uint32_t shstrSection =
      addSection(".shstrtab", kShtStrtab, 0, shstrOff, 0, 1, 0);

  StringTable shstr;
  for (SectionSpec& spec : sections) {
    spec.nameOffset = spec.name.empty() ? 0 : shstr.add(spec.name);
  }
  sections[shstrSection].size = shstr.data.size();
  const std::uint64_t shoff = alignUp(shstrOff + shstr.data.size(), 8);
  const std::uint64_t fileSize = shoff + sections.size() * kShdrSize;

  // --- Section links ---
  sections[dynsymSection].link = dynstrSection;
  sections[dynsymSection].info = 1;  // the first non-local symbol
  sections[hashSection].link = dynsymSection;
  sections[relaPltSection].link = dynsymSection;
  sections[relaPltSection].info = gotSection;
  if (relaDynSection != 0) sections[relaDynSection].link = dynsymSection;
  sections[dynamicSection].link = dynstrSection;

  // --- Emit ---
  std::vector<std::uint8_t> out(fileSize, 0);

  // ELF header.
  put8(out, 0, 0x7F);
  put8(out, 1, 'E');
  put8(out, 2, 'L');
  put8(out, 3, 'F');
  put8(out, 4, kElfClass64);
  put8(out, 5, kElfDataLsb);
  put8(out, 6, kEvCurrent);
  put8(out, 7, 0);
  put16(out, 16, kEtExec);
  put16(out, 18, kEmX86_64);
  put32(out, 20, kEvCurrent);
  put64(out, 24, kBaseAddress + startOff);  // e_entry
  put64(out, 32, kEhdrSize);                // e_phoff
  put64(out, 40, shoff);                    // e_shoff
  put32(out, 48, 0);                        // e_flags
  put16(out, 52, static_cast<std::uint16_t>(kEhdrSize));
  put16(out, 54, static_cast<std::uint16_t>(kPhdrSize));
  put16(out, 56, static_cast<std::uint16_t>(phnum));
  put16(out, 58, static_cast<std::uint16_t>(kShdrSize));
  put16(out, 60, static_cast<std::uint16_t>(sections.size()));
  put16(out, 62, static_cast<std::uint16_t>(shstrSection));

  // Program headers.
  std::uint64_t ph = kEhdrSize;
  const auto phdr = [&](std::uint32_t type, std::uint32_t flags,
                        std::uint64_t fileOffset, std::uint64_t vaddr,
                        std::uint64_t filesz, std::uint64_t memsz,
                        std::uint64_t alignment) {
    put32(out, ph, type);
    put32(out, ph + 4, flags);
    put64(out, ph + 8, fileOffset);
    put64(out, ph + 16, vaddr);
    put64(out, ph + 24, vaddr);
    put64(out, ph + 32, filesz);
    put64(out, ph + 40, memsz);
    put64(out, ph + 48, alignment);
    ph += kPhdrSize;
  };
  phdr(kPtPhdr, kPfR, kEhdrSize, kBaseAddress + kEhdrSize, phnum * kPhdrSize,
       phnum * kPhdrSize, 8);
  phdr(kPtInterp, kPfR, interpOff, kBaseAddress + interpOff, interpSize, interpSize, 1);
  phdr(kPtLoad, kPfR, 0, kBaseAddress, readOnlyEnd, readOnlyEnd, kPageSize);
  phdr(kPtLoad, kPfR | kPfX, rxStart, kBaseAddress + rxStart, rxEnd - rxStart,
       rxEnd - rxStart, kPageSize);
  if (hasRodata) {
    phdr(kPtLoad, kPfR, roStart, kBaseAddress + roStart, rodataSize, rodataSize,
         kPageSize);
  }
  phdr(kPtLoad, kPfR | kPfW, rwStart, kBaseAddress + rwStart, rwEnd - rwStart,
       rwEnd - rwStart, kPageSize);
  phdr(kPtDynamic, kPfR | kPfW, dynamicOff, kBaseAddress + dynamicOff, dynamicSize,
       dynamicSize, 8);
  phdr(kPtGnuStack, kPfR | kPfW, 0, 0, 0, 0, 0x10);

  // .interp
  std::memcpy(out.data() + interpOff, kInterpreter, interpSize - 1);

  // .dynstr
  std::memcpy(out.data() + dynstrOff, dynstr.data.data(), dynstr.data.size());

  // .dynsym
  std::uint64_t sym = dynsymOff + kSymSize;
  for (const std::string& name : dynSymbols) {
    put32(out, sym, dynstr.offsets.at(name));
    put8(out, sym + 4, static_cast<std::uint8_t>((kStbGlobal << 4) | kSttNotype));
    put8(out, sym + 5, 0);
    put16(out, sym + 6, kShnUndef);
    put64(out, sym + 8, 0);
    put64(out, sym + 16, 0);
    sym += kSymSize;
  }

  // .hash
  std::vector<std::uint32_t> buckets(bucketCount, 0);
  std::vector<std::uint32_t> chain(chainCount, 0);
  for (std::uint32_t i = 1; i <= static_cast<std::uint32_t>(dynSymbols.size()); ++i) {
    const std::uint32_t bucket =
        elfHash(dynSymbols[i - 1]) % static_cast<std::uint32_t>(bucketCount);
    chain[i] = buckets[bucket];
    buckets[bucket] = i;
  }
  put32(out, hashOff, static_cast<std::uint32_t>(bucketCount));
  put32(out, hashOff + 4, static_cast<std::uint32_t>(chainCount));
  const std::uint64_t bucketsOff = hashOff + 8;
  const std::uint64_t chainOff = bucketsOff + 4 * bucketCount;
  for (std::uint64_t i = 0; i < bucketCount; ++i) {
    put32(out, bucketsOff + 4 * i, buckets[i]);
  }
  for (std::uint64_t i = 0; i < chainCount; ++i) {
    put32(out, chainOff + 4 * i, chain[i]);
  }

  // .rela.plt and .rela.dyn
  std::uint64_t relaOff = relaPltOff;
  for (std::size_t i = 0; i < pltSymbols.size(); ++i) {
    put64(out, relaOff, kBaseAddress + gotOff + 8 * (3 + i));
    put64(out, relaOff + 8,
          (static_cast<std::uint64_t>(dynIndex.at(pltSymbols[i])) << 32) |
              kR_x86_64JumpSlot);
    put64(out, relaOff + 16, 0);
    relaOff += kRelaSize;
  }
  relaOff = relaDynOff;
  for (std::size_t i = 0; i < gotSymbols.size(); ++i) {
    put64(out, relaOff, kBaseAddress + gotOff + 8 * (3 + pltSymbols.size() + i));
    put64(out, relaOff + 8,
          (static_cast<std::uint64_t>(dynIndex.at(gotSymbols[i])) << 32) |
              kR_x86_64GlobDat);
    put64(out, relaOff + 16, 0);
    relaOff += kRelaSize;
  }

  // .text and its patches.
  std::memcpy(out.data() + textOff, parsed.text.data(), parsed.text.size());
  for (const Patch& patch : patches) {
    const std::uint64_t place = kBaseAddress + textOff + patch.offset;
    std::uint64_t target = 0;
    if (patch.defined) {
      target = kBaseAddress + textOff + patch.value;
    } else if (patch.type == kR_x86_64Plt32) {
      target = kBaseAddress + pltOff + 16 * (1 + pltIndex.at(patch.symbol));
    } else {
      target = kBaseAddress + gotOff +
               8 * (3 + pltSymbols.size() + gotIndex.at(patch.symbol));
    }
    const std::int64_t value = static_cast<std::int64_t>(target) + patch.addend -
                               static_cast<std::int64_t>(place);
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
      throw LinkError("relocation for '" + patch.symbol + "' does not fit in 32 bits");
    }
    put32(out, textOff + patch.offset, static_cast<std::uint32_t>(value));
  }

  // .plt
  const std::uint64_t pltVaddr = kBaseAddress + pltOff;
  const std::uint64_t gotVaddr = kBaseAddress + gotOff;
  put8(out, pltOff + 0, 0xFF);
  put8(out, pltOff + 1, 0x35);
  put32(out, pltOff + 2, static_cast<std::uint32_t>((gotVaddr + 8) - (pltVaddr + 6)));
  put8(out, pltOff + 6, 0xFF);
  put8(out, pltOff + 7, 0x25);
  put32(out, pltOff + 8, static_cast<std::uint32_t>((gotVaddr + 16) - (pltVaddr + 12)));
  put8(out, pltOff + 12, 0x0F);
  put8(out, pltOff + 13, 0x1F);
  put8(out, pltOff + 14, 0x40);
  put8(out, pltOff + 15, 0x00);
  for (std::size_t i = 0; i < pltSymbols.size(); ++i) {
    const std::uint64_t entry = pltOff + 16 * (1 + i);
    const std::uint64_t entryVaddr = pltVaddr + 16 * (1 + i);
    put8(out, entry + 0, 0xFF);
    put8(out, entry + 1, 0x25);
    put32(out, entry + 2,
          static_cast<std::uint32_t>((gotVaddr + 8 * (3 + i)) - (entryVaddr + 6)));
    put8(out, entry + 6, 0x68);
    put32(out, entry + 7, static_cast<std::uint32_t>(i));
    put8(out, entry + 11, 0xE9);
    put32(out, entry + 12,
          static_cast<std::uint32_t>(pltVaddr - (entryVaddr + 16)));
  }

  // .text.start: the canonical crt1 `_start`, so libc is initialised before the
  // program's `malloc` runs.
  const std::uint64_t startVaddr = kBaseAddress + startOff;
  const std::uint64_t mainVaddr = kBaseAddress + textOff + mainSymbol->second;
  const std::uint64_t startGot =
      gotVaddr + 8 * (3 + static_cast<std::uint64_t>(pltIndex.at("__libc_start_main")));
  const std::uint8_t startBytes[kStartSize] = {
      0x31, 0xED,              // xor ebp, ebp
      0x49, 0x89, 0xD1,        // mov r9, rdx
      0x5E,                    // pop rsi
      0x48, 0x89, 0xE2,        // mov rdx, rsp
      0x48, 0x83, 0xE4, 0xF0,  // and rsp, -16
      0x50,                    // push rax
      0x54,                    // push rsp
      0x45, 0x31, 0xC0,        // xor r8d, r8d
      0x31, 0xC9,              // xor ecx, ecx
      0x48, 0x8D, 0x3D, 0, 0, 0, 0,  // lea rdi, [rip + main]
      0xFF, 0x15, 0, 0, 0, 0,        // call qword ptr [rip + __libc_start_main@got]
      0xF4                           // hlt
  };
  std::memcpy(out.data() + startOff, startBytes, kStartSize);
  put32(out, startOff + 23,
        static_cast<std::uint32_t>(mainVaddr - (startVaddr + 27)));
  put32(out, startOff + 29,
        static_cast<std::uint32_t>(startGot - (startVaddr + 33)));

  // .rodata
  if (hasRodata) {
    std::memcpy(out.data() + rodataOff, parsed.rodata.data(), parsed.rodata.size());
  }

  // .got.plt. Slot 0 is the address of `_DYNAMIC`; slots 1 and 2 are the
  // resolver's, filled by the loader. Binding is eager (`DT_BIND_NOW`), so the
  // PLT slots can start at zero.
  put64(out, gotOff, kBaseAddress + dynamicOff);

  // .dynamic
  std::uint64_t dynamic = dynamicOff;
  const auto dyn = [&](std::int64_t tag, std::uint64_t value) {
    put64(out, dynamic, static_cast<std::uint64_t>(tag));
    put64(out, dynamic + 8, value);
    dynamic += kDynSize;
  };
  for (const std::uint32_t neededOffset : neededOffsets) {
    dyn(kDtNeeded, neededOffset);
  }
  dyn(kDtHash, kBaseAddress + hashOff);
  dyn(kDtStrtab, kBaseAddress + dynstrOff);
  dyn(kDtSymtab, kBaseAddress + dynsymOff);
  dyn(kDtStrsz, dynstrSize);
  dyn(kDtSyment, kSymSize);
  dyn(kDtPltGot, gotVaddr);
  dyn(kDtPltRelSz, relaPltSize);
  dyn(kDtPltRel, kDtRela);
  dyn(kDtJmprel, kBaseAddress + relaPltOff);
  if (!gotSymbols.empty()) {
    dyn(kDtRela, kBaseAddress + relaDynOff);
    dyn(kDtRelaSz, relaDynSize);
    dyn(kDtRelaEnt, kRelaSize);
  }
  dyn(kDtBindNow, 0);
  dyn(kDtFlags, kDfBindNow);
  dyn(kDtDebug, 0);
  dyn(kDtNull, 0);

  // .shstrtab
  std::memcpy(out.data() + shstrOff, shstr.data.data(), shstr.data.size());

  // Section headers.
  std::uint64_t sh = shoff;
  for (const SectionSpec& spec : sections) {
    put32(out, sh + 0, spec.nameOffset);
    put32(out, sh + 4, spec.type);
    put64(out, sh + 8, spec.flags);
    put64(out, sh + 16, (spec.flags & kShfAlloc) != 0 ? kBaseAddress + spec.offset : 0);
    put64(out, sh + 24, spec.offset);
    put64(out, sh + 32, spec.size);
    put32(out, sh + 40, spec.link);
    put32(out, sh + 44, spec.info);
    put64(out, sh + 48, spec.addralign);
    put64(out, sh + 56, spec.entsize);
    sh += kShdrSize;
  }

  return out;
}
