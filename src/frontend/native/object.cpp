#include "native/object.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ELF constants (System V gABI + the x86-64 psABI). Named here so the layout
// below reads like the specification instead of like magic numbers.
constexpr std::uint16_t kEtRel = 1;
constexpr std::uint16_t kEmX86_64 = 0x3E;
constexpr std::uint8_t kElfClass64 = 2;
constexpr std::uint8_t kElfData2Lsb = 1;
constexpr std::uint8_t kEvCurrent = 1;

constexpr std::uint32_t kShtNull = 0;
constexpr std::uint32_t kShtProgbits = 1;
constexpr std::uint32_t kShtSymtab = 2;
constexpr std::uint32_t kShtStrtab = 3;
constexpr std::uint32_t kShtRela = 4;

constexpr std::uint64_t kShfAlloc = 0x2;
constexpr std::uint64_t kShfExecInstr = 0x4;

constexpr std::uint8_t kStbLocal = 0;
constexpr std::uint8_t kStbGlobal = 1;
constexpr std::uint8_t kSttFunc = 2;
constexpr std::uint8_t kSttSection = 3;

constexpr std::uint16_t kShnUndef = 0;

// Relocation types the hosted link needs. Both are PC-relative against the
// displacement slot, so both carry an addend of -4: the CPU measures the
// displacement from the end of the instruction (the slot's address + 4), so
// `S + A - P` must become `S - (P + 4)`.
constexpr std::uint32_t kR_x86_64Pc32 = 2;
constexpr std::uint32_t kR_x86_64Plt32 = 4;
constexpr std::int64_t kPcRelativeAddend = -4;

// Section indices, fixed because every section is always emitted (an empty
// `.rodata` is a zero-length section, not an omission), so the section header
// table never has to renumber its links.
constexpr std::uint16_t kSecText = 1;
constexpr std::uint16_t kSecRelaText = 2;
constexpr std::uint16_t kSecRodata = 3;
constexpr std::uint16_t kSecSymtab = 4;
constexpr std::uint16_t kSecStrtab = 5;
constexpr std::uint16_t kSecShstrtab = 6;
constexpr std::uint16_t kSectionCount = 7;

// A string table that hands back the offset it stored each name at. ELF string
// tables start with a NUL byte, so offset 0 is the empty string.
struct StringTable {
  std::string data = std::string(1, '\0');

  std::uint32_t add(const std::string& text) {
    std::uint32_t offset = static_cast<std::uint32_t>(data.size());
    data += text;
    data += '\0';
    return offset;
  }
};

void write8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }

void write16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void write32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void write64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void padTo(std::vector<std::uint8_t>& out, std::size_t alignment) {
  while (out.size() % alignment != 0) out.push_back(0);
}

// One section header, filled into the table once every section's offset and
// size are known.
struct SectionHeader {
  std::uint32_t name = 0;
  std::uint32_t type = kShtNull;
  std::uint64_t flags = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint32_t link = 0;
  std::uint32_t info = 0;
  std::uint64_t addralign = 1;
  std::uint64_t entsize = 0;
};

void writeSectionHeader(std::vector<std::uint8_t>& out, const SectionHeader& sh) {
  write32(out, sh.name);
  write32(out, sh.type);
  write64(out, sh.flags);
  write64(out, 0);  // sh_addr: a relocatable object has no addresses yet
  write64(out, sh.offset);
  write64(out, sh.size);
  write32(out, sh.link);
  write32(out, sh.info);
  write64(out, sh.addralign);
  write64(out, sh.entsize);
}

// One symbol table entry.
struct Symbol {
  std::uint32_t name = 0;
  std::uint8_t info = 0;
  std::uint16_t shndx = kShnUndef;
  std::uint64_t value = 0;
};

void writeSymbol(std::vector<std::uint8_t>& out, const Symbol& sym) {
  write32(out, sym.name);
  write8(out, sym.info);
  write8(out, 0);  // st_other: default visibility
  write16(out, sym.shndx);
  write64(out, sym.value);
  write64(out, 0);  // st_size: the emitter does not track a symbol's extent
}

}  // namespace

static std::vector<std::uint8_t> buildElfObject(
    const Target& target, const std::vector<std::uint8_t>& text,
    const std::vector<std::uint8_t>& rodata,
    const std::vector<ExternalRef>& externals,
    const std::vector<ExportedSymbol>& exports) {
  requireSupportedTarget(target);

  // --- Symbol table ---
  // Layout: the null symbol, then the section symbols (locals), then every
  // global symbol. ELF requires locals before globals, and `.symtab`'s
  // `sh_info` names the first global -- a linker uses that split to decide what
  // is visible outside the object.
  std::vector<Symbol> symbols;
  StringTable strtab;

  symbols.push_back(Symbol{});  // index 0: STN_UNDEF

  Symbol textSection;
  textSection.info = static_cast<std::uint8_t>((kStbLocal << 4) | kSttSection);
  textSection.shndx = kSecText;
  symbols.push_back(textSection);

  const std::uint32_t firstGlobal = static_cast<std::uint32_t>(symbols.size());

  // A name is looked up by its index so an external referenced twice (or both
  // referenced and defined) gets one symbol and two relocations, never two
  // symbols.
  std::unordered_map<std::string, std::uint32_t> symbolIndex;

  for (const ExportedSymbol& exported : exports) {
    if (symbolIndex.count(exported.name) != 0) continue;
    Symbol sym;
    sym.name = strtab.add(exported.name);
    sym.info = static_cast<std::uint8_t>((kStbGlobal << 4) | kSttFunc);
    sym.shndx = kSecText;
    sym.value = exported.offset;
    symbolIndex.emplace(exported.name, static_cast<std::uint32_t>(symbols.size()));
    symbols.push_back(sym);
  }

  for (const ExternalRef& ref : externals) {
    if (symbolIndex.count(ref.symbol) != 0) continue;
    Symbol sym;
    sym.name = strtab.add(ref.symbol);
    sym.info = static_cast<std::uint8_t>((kStbGlobal << 4) | kSttFunc);
    sym.shndx = kShnUndef;  // the linker resolves it against libc
    symbolIndex.emplace(ref.symbol, static_cast<std::uint32_t>(symbols.size()));
    symbols.push_back(sym);
  }

  // --- Relocations ---
  std::vector<std::uint8_t> rela;
  for (const ExternalRef& ref : externals) {
    const std::uint32_t symbol = symbolIndex.at(ref.symbol);
    const std::uint32_t type = ref.call ? kR_x86_64Plt32 : kR_x86_64Pc32;
    write64(rela, ref.patchAt);                                  // r_offset
    write64(rela, (static_cast<std::uint64_t>(symbol) << 32) | type);  // r_info
    write64(rela, static_cast<std::uint64_t>(kPcRelativeAddend));      // r_addend
  }

  // --- Section name table ---
  StringTable shstrtab;
  const std::uint32_t nameText = shstrtab.add(".text");
  const std::uint32_t nameRela = shstrtab.add(".rela.text");
  const std::uint32_t nameRodata = shstrtab.add(".rodata");
  const std::uint32_t nameSymtab = shstrtab.add(".symtab");
  const std::uint32_t nameStrtab = shstrtab.add(".strtab");
  const std::uint32_t nameShstrtab = shstrtab.add(".shstrtab");

  // --- Assemble the file: header, section data, section header table ---
  std::vector<std::uint8_t> out;
  out.resize(64, 0);  // ELF header, written last

  padTo(out, 16);
  const std::uint64_t textOffset = out.size();
  out.insert(out.end(), text.begin(), text.end());

  padTo(out, 8);
  const std::uint64_t rodataOffset = out.size();
  out.insert(out.end(), rodata.begin(), rodata.end());

  padTo(out, 8);
  const std::uint64_t relaOffset = out.size();
  out.insert(out.end(), rela.begin(), rela.end());

  padTo(out, 8);
  const std::uint64_t symtabOffset = out.size();
  for (const Symbol& sym : symbols) writeSymbol(out, sym);

  const std::uint64_t strtabOffset = out.size();
  out.insert(out.end(), strtab.data.begin(), strtab.data.end());

  const std::uint64_t shstrtabOffset = out.size();
  out.insert(out.end(), shstrtab.data.begin(), shstrtab.data.end());

  padTo(out, 8);
  const std::uint64_t shoff = out.size();

  SectionHeader null;
  writeSectionHeader(out, null);

  SectionHeader textHdr;
  textHdr.name = nameText;
  textHdr.type = kShtProgbits;
  textHdr.flags = kShfAlloc | kShfExecInstr;
  textHdr.offset = textOffset;
  textHdr.size = text.size();
  textHdr.addralign = 16;
  writeSectionHeader(out, textHdr);

  SectionHeader relaHdr;
  relaHdr.name = nameRela;
  relaHdr.type = kShtRela;
  relaHdr.offset = relaOffset;
  relaHdr.size = rela.size();
  relaHdr.link = kSecSymtab;  // the symbols the relocations name
  relaHdr.info = kSecText;    // the section the relocations apply to
  relaHdr.addralign = 8;
  relaHdr.entsize = 24;
  writeSectionHeader(out, relaHdr);

  SectionHeader rodataHdr;
  rodataHdr.name = nameRodata;
  rodataHdr.type = kShtProgbits;
  rodataHdr.flags = kShfAlloc;
  rodataHdr.offset = rodataOffset;
  rodataHdr.size = rodata.size();
  rodataHdr.addralign = 8;
  writeSectionHeader(out, rodataHdr);

  SectionHeader symtabHdr;
  symtabHdr.name = nameSymtab;
  symtabHdr.type = kShtSymtab;
  symtabHdr.offset = symtabOffset;
  symtabHdr.size = symbols.size() * 24;
  symtabHdr.link = kSecStrtab;
  symtabHdr.info = firstGlobal;
  symtabHdr.addralign = 8;
  symtabHdr.entsize = 24;
  writeSectionHeader(out, symtabHdr);

  SectionHeader strtabHdr;
  strtabHdr.name = nameStrtab;
  strtabHdr.type = kShtStrtab;
  strtabHdr.offset = strtabOffset;
  strtabHdr.size = strtab.data.size();
  writeSectionHeader(out, strtabHdr);

  SectionHeader shstrtabHdr;
  shstrtabHdr.name = nameShstrtab;
  shstrtabHdr.type = kShtStrtab;
  shstrtabHdr.offset = shstrtabOffset;
  shstrtabHdr.size = shstrtab.data.size();
  writeSectionHeader(out, shstrtabHdr);

  // --- ELF header ---
  std::vector<std::uint8_t> ehdr;
  ehdr.reserve(64);
  write8(ehdr, 0x7F);  // ELFMAG0
  write8(ehdr, 'E');
  write8(ehdr, 'L');
  write8(ehdr, 'F');
  write8(ehdr, kElfClass64);
  write8(ehdr, kElfData2Lsb);
  write8(ehdr, kEvCurrent);
  write8(ehdr, 0);  // ELFOSABI_SYSV
  for (int i = 0; i < 8; ++i) write8(ehdr, 0);
  write16(ehdr, kEtRel);        // e_type
  write16(ehdr, kEmX86_64);     // e_machine
  write32(ehdr, kEvCurrent);    // e_version
  write64(ehdr, 0);             // e_entry: the linker picks the entry
  write64(ehdr, 0);             // e_phoff: an object has no program headers
  write64(ehdr, shoff);         // e_shoff
  write32(ehdr, 0);             // e_flags
  write16(ehdr, 64);            // e_ehsize
  write16(ehdr, 0);             // e_phentsize
  write16(ehdr, 0);             // e_phnum
  write16(ehdr, 64);            // e_shentsize
  write16(ehdr, kSectionCount); // e_shnum
  write16(ehdr, kSecShstrtab);  // e_shstrndx
  if (ehdr.size() != 64) {
    throw std::logic_error("native/object: internal error, ELF header size mismatch");
  }
  std::memcpy(out.data(), ehdr.data(), ehdr.size());

  return out;
}

// ---------------------------------------------------------------------------
// Mach-O (macOS x86-64)
// ---------------------------------------------------------------------------
//
// A Mach-O `MH_OBJECT` is the container the macOS hosted link needs. It is
// structurally smaller than ELF: a 32-byte header, one `LC_SEGMENT_64` holding
// `__text` (and `__const` when there is read-only data), a `LC_BUILD_VERSION`
// that names macOS and a `LC_SYMTAB`. The code buffer is PC-relative
// throughout, so every external reference is a pcrel 32-bit relocation:
// `X86_64_RELOC_BRANCH` for a call and `X86_64_RELOC_SIGNED` for an address.
// The linker subtracts the field's own four bytes for both, so the field is
// zero, exactly as the ELF path leaves it.

namespace {

constexpr std::uint32_t kMachMagic64 = 0xFEEDFACF;
constexpr std::uint32_t kCpuTypeX86_64 = 0x01000007;
constexpr std::uint32_t kCpuSubtypeX86_64All = 3;
constexpr std::uint32_t kMachHeaderObject = 1;
constexpr std::uint32_t kMachSubsectionsViaSymbols = 0x2000;
constexpr std::uint32_t kLcSegment64 = 0x19;
constexpr std::uint32_t kLcSymtab = 0x2;
constexpr std::uint32_t kLcBuildVersion = 0x32;
constexpr std::uint32_t kMachPlatformMacos = 1;
// (major << 16) | (minor << 8) | patch: macOS 10.15, the first release whose
// SDK ships the libSystem symbols the hosted prelude names.
constexpr std::uint32_t kMacOsVersion10_15 = (10u << 16) | (15u << 8);

constexpr std::uint8_t kMachNExt = 0x01;
constexpr std::uint8_t kMachNUndf = 0x00;
constexpr std::uint8_t kMachNSect = 0x0E;

constexpr std::uint8_t kX86_64RelocSigned = 1;
constexpr std::uint8_t kX86_64RelocBranch = 2;

constexpr std::uint32_t kTextAttributes =
    0x80000400;  // S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS
constexpr std::uint32_t kConstAttributes = 0;  // S_REGULAR

// A Mach-O name field is 16 bytes, NUL-padded and not necessarily NUL-
// terminated when it fills the field.
void appendFixedName(std::vector<std::uint8_t>& out, const std::string& name,
                     std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    out.push_back(i < name.size() ? static_cast<std::uint8_t>(name[i]) : 0);
  }
}

struct MachSymbol {
  std::string name;  // raw name, without the leading underscore
  bool defined = false;
  std::uint64_t value = 0;
};

}  // namespace

static std::vector<std::uint8_t> buildMachOObject(
    const Target& target, const std::vector<std::uint8_t>& text,
    const std::vector<std::uint8_t>& rodata,
    const std::vector<ExternalRef>& externals,
    const std::vector<ExportedSymbol>& exports) {
  requireSupportedTarget(target);

  // The symbol table always starts with the null entry. Defined symbols come
  // from the exports; undefined ones from the external references. Mach-O
  // names carry a leading underscore, so the code buffer keeps the C spelling
  // and only this container adds it.
  std::vector<MachSymbol> symbols;
  symbols.push_back(MachSymbol{});  // index 0: the null symbol
  std::unordered_map<std::string, std::uint32_t> symbolIndex;
  for (const ExportedSymbol& exported : exports) {
    if (symbolIndex.count(exported.name) != 0) continue;
    MachSymbol symbol;
    symbol.name = exported.name;
    symbol.defined = true;
    symbol.value = exported.offset;
    symbolIndex.emplace(exported.name, static_cast<std::uint32_t>(symbols.size()));
    symbols.push_back(std::move(symbol));
  }
  for (const ExternalRef& ref : externals) {
    if (symbolIndex.count(ref.symbol) != 0) continue;
    MachSymbol symbol;
    symbol.name = ref.symbol;
    symbol.defined = false;
    symbolIndex.emplace(ref.symbol, static_cast<std::uint32_t>(symbols.size()));
    symbols.push_back(std::move(symbol));
  }

  StringTable strtab;
  std::vector<std::uint8_t> symtab;
  for (const MachSymbol& symbol : symbols) {
    const std::uint32_t nameOffset =
        symbol.name.empty() ? 0 : strtab.add("_" + symbol.name);
    write32(symtab, nameOffset);
    write8(symtab, static_cast<std::uint8_t>(
                       symbol.defined ? (kMachNSect | kMachNExt) : kMachNUndf | kMachNExt));
    write8(symtab, static_cast<std::uint8_t>(symbol.defined ? 1 : 0));
    write16(symtab, 0);  // n_desc
    write64(symtab, symbol.defined ? symbol.value : 0);
  }

  std::vector<std::uint8_t> relocs;
  for (const ExternalRef& ref : externals) {
    write32(relocs, static_cast<std::uint32_t>(ref.patchAt));
    const std::uint32_t symbol = symbolIndex.at(ref.symbol);
    const std::uint32_t type = ref.call ? kX86_64RelocBranch : kX86_64RelocSigned;
    // r_symbolnum:24, r_pcrel:1, r_length:2 (four bytes), r_extern:1, r_type:4.
    const std::uint32_t word =
        symbol | (1u << 24) | (2u << 25) | (1u << 27) | (type << 28);
    write32(relocs, word);
  }

  const bool hasConst = !rodata.empty();
  const std::uint32_t sectionCount = hasConst ? 2u : 1u;
  const std::uint32_t segmentCommandSize = 72u + 80u * sectionCount;
  const std::uint32_t sizeofcmds = segmentCommandSize + 24u + 24u;

  std::uint64_t textOffset = 32u + sizeofcmds;
  textOffset = (textOffset + 15u) / 16u * 16u;
  std::uint64_t constOffset = (textOffset + text.size() + 7u) / 8u * 8u;
  std::uint64_t relocOffset = (constOffset + rodata.size() + 7u) / 8u * 8u;
  std::uint64_t symtabOffset = (relocOffset + relocs.size() + 7u) / 8u * 8u;
  const std::uint64_t strtabOffset = symtabOffset + symtab.size();

  const std::uint64_t sectionBytes = constOffset + rodata.size() - textOffset;

  std::vector<std::uint8_t> out;
  out.reserve(static_cast<std::size_t>(strtabOffset + strtab.data.size()));

  write32(out, kMachMagic64);
  write32(out, kCpuTypeX86_64);
  write32(out, kCpuSubtypeX86_64All);
  write32(out, kMachHeaderObject);
  write32(out, 3);  // ncmds: segment, build version, symtab
  write32(out, sizeofcmds);
  write32(out, kMachSubsectionsViaSymbols);
  write32(out, 0);  // reserved

  // LC_SEGMENT_64 with one or two sections. In an object file the segment's
  // `fileoff` is where the section bytes start and every section `addr` is
  // relative to the segment's `vmaddr` (zero here).
  write32(out, kLcSegment64);
  write32(out, segmentCommandSize);
  appendFixedName(out, "", 16);
  write64(out, 0);  // vmaddr
  write64(out, sectionBytes);
  write64(out, textOffset);
  write64(out, sectionBytes);
  write32(out, 7);  // maxprot: read/write/execute
  write32(out, 7);  // initprot
  write32(out, sectionCount);
  write32(out, 0);  // flags

  appendFixedName(out, "__text", 16);
  appendFixedName(out, "__TEXT", 16);
  write64(out, 0);  // addr
  write64(out, text.size());
  write32(out, static_cast<std::uint32_t>(textOffset));
  write32(out, 4);  // align: 2^4
  write32(out, static_cast<std::uint32_t>(relocOffset));
  write32(out, static_cast<std::uint32_t>(externals.size()));
  write32(out, kTextAttributes);
  write32(out, 0);  // reserved1
  write32(out, 0);  // reserved2
  write32(out, 0);  // reserved3

  if (hasConst) {
    appendFixedName(out, "__const", 16);
    appendFixedName(out, "__TEXT", 16);
    write64(out, constOffset - textOffset);  // addr relative to the segment
    write64(out, rodata.size());
    write32(out, static_cast<std::uint32_t>(constOffset));
    write32(out, 3);  // align: 2^3
    write32(out, 0);  // reloff
    write32(out, 0);  // nreloc
    write32(out, kConstAttributes);
    write32(out, 0);
    write32(out, 0);
    write32(out, 0);
  }

  // LC_BUILD_VERSION: macOS, 10.15. The object itself is platform-tagged so a
  // linker does not have to infer the platform from the host.
  write32(out, kLcBuildVersion);
  write32(out, 24);
  write32(out, kMachPlatformMacos);
  write32(out, kMacOsVersion10_15);  // minos
  write32(out, kMacOsVersion10_15);  // sdk
  write32(out, 0);                   // ntools

  write32(out, kLcSymtab);
  write32(out, 24);
  write32(out, static_cast<std::uint32_t>(symtabOffset));
  write32(out, static_cast<std::uint32_t>(symbols.size()));
  write32(out, static_cast<std::uint32_t>(strtabOffset));
  write32(out, static_cast<std::uint32_t>(strtab.data.size()));

  padTo(out, 16);
  if (out.size() != textOffset) {
    throw std::logic_error("native/object: internal error, Mach-O header size mismatch");
  }
  out.insert(out.end(), text.begin(), text.end());
  padTo(out, 8);
  out.insert(out.end(), rodata.begin(), rodata.end());
  padTo(out, 8);
  out.insert(out.end(), relocs.begin(), relocs.end());
  padTo(out, 8);
  out.insert(out.end(), symtab.begin(), symtab.end());
  out.insert(out.end(), strtab.data.begin(), strtab.data.end());
  return out;
}

// ---------------------------------------------------------------------------
// COFF (Windows x86-64)
// ---------------------------------------------------------------------------
//
// A Windows hosted image is a COFF `IMAGE_FILE_MACHINE_AMD64` object linked by
// the Microsoft toolchain. It carries `.text` (and `.rdata` when there is
// read-only data), one 10-byte `IMAGE_REL_AMD64_REL32` per external reference
// (the same relocation serves a call and an address, because both are a
// 32-bit displacement from the byte after the field) and an 18-byte symbol
// table. Names longer than eight bytes go through the trailing string table.

namespace {

constexpr std::uint16_t kCoffMachineAmd64 = 0x8664;
constexpr std::uint16_t kCoffRelAmd64Rel32 = 4;
constexpr std::uint16_t kCoffTypeFunction = 0x20;
constexpr std::uint8_t kCoffStorageExternal = 2;
constexpr std::uint32_t kCoffTextCharacteristics = 0x60000020;  // code | execute | read
constexpr std::uint32_t kCoffRdataCharacteristics = 0x40000040;  // initialized data | read

struct CoffSymbol {
  std::string name;
  bool defined = false;
  std::uint64_t value = 0;
};

// COFF's string table starts with a 4-byte length (including itself), so the
// first name lives at offset 4.
struct CoffStrings {
  std::string data = std::string(4, '\0');

  std::uint32_t add(const std::string& text) {
    const std::uint32_t offset = static_cast<std::uint32_t>(data.size());
    data += text;
    data += '\0';
    return offset;
  }

  void finish() {
    const std::uint32_t size = static_cast<std::uint32_t>(data.size());
    for (int i = 0; i < 4; ++i) {
      data[static_cast<std::size_t>(i)] = static_cast<char>((size >> (8 * i)) & 0xFF);
    }
  }
};

}  // namespace

static std::vector<std::uint8_t> buildCoffObject(
    const Target& target, const std::vector<std::uint8_t>& text,
    const std::vector<std::uint8_t>& rodata,
    const std::vector<ExternalRef>& externals,
    const std::vector<ExportedSymbol>& exports) {
  requireSupportedTarget(target);

  std::vector<CoffSymbol> symbols;
  std::unordered_map<std::string, std::uint32_t> symbolIndex;
  for (const ExportedSymbol& exported : exports) {
    if (symbolIndex.count(exported.name) != 0) continue;
    CoffSymbol symbol;
    symbol.name = exported.name;
    symbol.defined = true;
    symbol.value = exported.offset;
    symbolIndex.emplace(exported.name, static_cast<std::uint32_t>(symbols.size()));
    symbols.push_back(std::move(symbol));
  }
  for (const ExternalRef& ref : externals) {
    if (symbolIndex.count(ref.symbol) != 0) continue;
    CoffSymbol symbol;
    symbol.name = ref.symbol;
    symbol.defined = false;
    symbolIndex.emplace(ref.symbol, static_cast<std::uint32_t>(symbols.size()));
    symbols.push_back(std::move(symbol));
  }

  CoffStrings strtab;
  std::vector<std::uint8_t> symtab;
  for (const CoffSymbol& symbol : symbols) {
    if (symbol.name.size() <= 8) {
      appendFixedName(symtab, symbol.name, 8);
    } else {
      write32(symtab, 0);  // the name is in the string table
      write32(symtab, strtab.add(symbol.name));
    }
    write32(symtab, static_cast<std::uint32_t>(symbol.value));
    write16(symtab, static_cast<std::uint16_t>(symbol.defined ? 1 : 0));
    write16(symtab, kCoffTypeFunction);
    write8(symtab, kCoffStorageExternal);
    write8(symtab, 0);  // no auxiliary records
  }
  strtab.finish();

  std::vector<std::uint8_t> relocs;
  for (const ExternalRef& ref : externals) {
    write32(relocs, static_cast<std::uint32_t>(ref.patchAt));
    write32(relocs, symbolIndex.at(ref.symbol));
    write16(relocs, kCoffRelAmd64Rel32);
  }

  const bool hasRdata = !rodata.empty();
  const std::uint16_t sectionCount = hasRdata ? 2 : 1;
  std::uint64_t textOffset = 20u + 40u * sectionCount;
  textOffset = (textOffset + 15u) / 16u * 16u;
  std::uint64_t rdataOffset = (textOffset + text.size() + 15u) / 16u * 16u;
  std::uint64_t relocOffset = (rdataOffset + rodata.size() + 3u) / 4u * 4u;
  std::uint64_t symtabOffset = (relocOffset + relocs.size() + 3u) / 4u * 4u;
  const std::uint64_t strtabOffset = symtabOffset + symtab.size();

  std::vector<std::uint8_t> out;
  out.reserve(static_cast<std::size_t>(strtabOffset + strtab.data.size()));

  // IMAGE_FILE_HEADER.
  write16(out, kCoffMachineAmd64);
  write16(out, sectionCount);
  write32(out, 0);  // TimeDateStamp
  write32(out, static_cast<std::uint32_t>(symtabOffset));
  write32(out, static_cast<std::uint32_t>(symbols.size()));
  write16(out, 0);  // SizeOfOptionalHeader
  write16(out, 0);  // Characteristics

  // IMAGE_SECTION_HEADER for .text.
  appendFixedName(out, ".text", 8);
  write32(out, 0);  // VirtualSize
  write32(out, 0);  // VirtualAddress
  write32(out, static_cast<std::uint32_t>(text.size()));
  write32(out, static_cast<std::uint32_t>(textOffset));
  write32(out, static_cast<std::uint32_t>(externals.empty() ? 0 : relocOffset));
  write32(out, 0);  // PointerToLinenumbers
  write16(out, static_cast<std::uint16_t>(externals.size()));
  write16(out, 0);  // NumberOfLinenumbers
  write32(out, kCoffTextCharacteristics);

  if (hasRdata) {
    appendFixedName(out, ".rdata", 8);
    write32(out, 0);
    write32(out, 0);
    write32(out, static_cast<std::uint32_t>(rodata.size()));
    write32(out, static_cast<std::uint32_t>(rdataOffset));
    write32(out, 0);
    write32(out, 0);
    write16(out, 0);
    write16(out, 0);
    write32(out, kCoffRdataCharacteristics);
  }

  padTo(out, 16);
  if (out.size() != textOffset) {
    throw std::logic_error("native/object: internal error, COFF header size mismatch");
  }
  out.insert(out.end(), text.begin(), text.end());
  padTo(out, 16);
  out.insert(out.end(), rodata.begin(), rodata.end());
  padTo(out, 4);
  out.insert(out.end(), relocs.begin(), relocs.end());
  padTo(out, 4);
  out.insert(out.end(), symtab.begin(), symtab.end());
  out.insert(out.end(), strtab.data.begin(), strtab.data.end());
  return out;
}

std::vector<std::uint8_t> buildRelocatableObject(
    const Target& target, const std::vector<std::uint8_t>& text,
    const std::vector<std::uint8_t>& rodata,
    const std::vector<ExternalRef>& externals,
    const std::vector<ExportedSymbol>& exports) {
  requireSupportedTarget(target);
  if (target.os == Os::MacOs) {
    return buildMachOObject(target, text, rodata, externals, exports);
  }
  if (target.os == Os::Windows) {
    return buildCoffObject(target, text, rodata, externals, exports);
  }
  return buildElfObject(target, text, rodata, externals, exports);
}
