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

std::vector<std::uint8_t> buildRelocatableObject(
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
