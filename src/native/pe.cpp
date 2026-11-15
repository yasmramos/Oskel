#include "native/pe.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint16_t kDosMagic = 0x5A4D;
constexpr std::uint32_t kPeSignature = 0x00004550;
constexpr std::uint16_t kMachineAmd64 = 0x8664;
constexpr std::uint16_t kOptionalMagicPe32Plus = 0x020B;
constexpr std::uint32_t kSectionAlignment = 0x1000;
constexpr std::uint32_t kFileAlignment = 0x200;

// The import directory sits at a fixed RVA, before .text, so the IAT slot
// addresses are constant and the prelude can call through them with a
// constant RIP-relative displacement regardless of the program's size.
constexpr std::uint32_t kIdataRva = 0x1000;
constexpr std::uint32_t kIdataFileOffset = 0x200;

// COFF characteristics: a 64-bit executable that is large-address-aware and,
// because the emitter produces no relocations, has them declared stripped. A
// modern Windows with "Force ASLR" refuses an image without relocations unless
// IMAGE_FILE_RELOCS_STRIPPED is set; without it the loader aborts the process
// with a page fault instead of mapping it at the preferred base.
constexpr std::uint16_t kCharacteristicsExecutable = 0x0002;      // EXECUTABLE_IMAGE
constexpr std::uint16_t kCharacteristicsLargeAddressAware = 0x0020;
constexpr std::uint16_t kCharacteristicsRelocsStripped = 0x0001;

constexpr int kImportCount = static_cast<int>(PeImport::Count);

// The image imports from a single DLL (kernel32.dll). The import directory
// table holds one descriptor per DLL, not one per imported function, so this
// is 1 even though `kImportCount` functions are resolved from it.
constexpr int kDllCount = 1;

const char* importName(PeImport import) {
  switch (import) {
    case PeImport::WriteFile:
      return "WriteFile";
    case PeImport::GetStdHandle:
      return "GetStdHandle";
    case PeImport::ExitProcess:
      return "ExitProcess";
    case PeImport::VirtualAlloc:
      return "VirtualAlloc";
    case PeImport::Count:
      break;
  }
  return "";
}

constexpr const char* kKernel32 = "kernel32.dll";

// RVAs of the pieces of .idata. The order in the file is descriptors, the
// import lookup table (ILT), the import address table (IAT), the hint/name
// entries and finally the DLL name. Every piece is a multiple of the previous
// one's alignment, so the layout is fixed and can be computed before the bytes
// are written.
struct IdataLayout {
  std::uint32_t descriptors;
  std::uint32_t ilt;
  std::uint32_t iat;
  std::uint32_t hintName;
  std::uint32_t dllName;
  std::uint32_t entryRva[kImportCount];  // hint/name entry per import
};

IdataLayout idataLayout() {
  IdataLayout layout{};
  const std::uint32_t descriptorBytes =
      static_cast<std::uint32_t>((kDllCount + 1) * 20);
  const std::uint32_t thunkBytes =
      static_cast<std::uint32_t>((kImportCount + 1) * 8);

  layout.descriptors = kIdataRva;
  layout.ilt = layout.descriptors + descriptorBytes;
  layout.iat = layout.ilt + thunkBytes;

  // Hint/name entries, each aligned to an even address (the name follows a
  // 2-byte hint, and Windows keeps the table word-aligned).
  std::uint32_t cursor = layout.iat + thunkBytes;
  layout.hintName = cursor;
  for (int i = 0; i < kImportCount; ++i) {
    layout.entryRva[i] = cursor;
    cursor += 2 + static_cast<std::uint32_t>(std::strlen(importName(static_cast<PeImport>(i)))) + 1;
    if (cursor & 1u) ++cursor;
  }
  layout.dllName = cursor;
  return layout;
}

void write16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void write32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void write64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void padTo(std::vector<std::uint8_t>& out, std::size_t alignment) {
  while (out.size() % alignment != 0) out.push_back(0);
}

// Little-endian writes into an already-sized buffer (the PE header is laid out
// by field offset, so the writers must not append).
void write16At(std::vector<std::uint8_t>& out, std::size_t off, std::uint16_t v) {
  out[off + 0] = static_cast<std::uint8_t>(v & 0xFF);
  out[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void write32At(std::vector<std::uint8_t>& out, std::size_t off, std::uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out[off + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

void write64At(std::vector<std::uint8_t>& out, std::size_t off, std::uint64_t v) {
  for (int i = 0; i < 8; ++i)
    out[off + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

void appendCString(std::vector<std::uint8_t>& out, const char* text) {
  for (const char* c = text; *c; ++c)
    out.push_back(static_cast<std::uint8_t>(*c));
  out.push_back(0);
}

// Builds the whole .idata section from the layout.
std::vector<std::uint8_t> buildIdata(const IdataLayout& layout) {
  std::vector<std::uint8_t> idata;

  // 1. Import Directory Table: one descriptor per DLL (kernel32.dll is the
  //    only one), then a null terminator. The single descriptor's ILT/IAT span
  //    every imported function.
  write32(idata, layout.ilt);        // OriginalFirstThunk (ILT RVA)
  write32(idata, 0);                 // TimeDateStamp
  write32(idata, 0);                 // ForwarderChain
  write32(idata, layout.dllName);    // Name (DLL name RVA)
  write32(idata, layout.iat);        // FirstThunk (IAT RVA)
  for (int i = 0; i < 5; ++i) write32(idata, 0);  // terminator (20 bytes)

  // 2. Import Lookup Table: the RVA of each hint/name entry, then a terminator.
  for (int i = 0; i < kImportCount; ++i) write64(idata, layout.entryRva[i]);
  write64(idata, 0);

  // 3. Import Address Table: the loader overwrites each slot with the resolved
  //    address, so it starts out identical to the ILT.
  for (int i = 0; i < kImportCount; ++i) write64(idata, layout.entryRva[i]);
  write64(idata, 0);

  // 4. Hint/Name entries: a 2-byte hint, the name and a NUL, word-aligned.
  for (int i = 0; i < kImportCount; ++i) {
    write16(idata, 0);  // hint
    appendCString(idata, importName(static_cast<PeImport>(i)));
    if (idata.size() & 1u) idata.push_back(0);
  }

  // 5. DLL name.
  appendCString(idata, kKernel32);

  return idata;
}

}  // namespace

std::uint32_t importIatRva(PeImport import) {
  const IdataLayout layout = idataLayout();
  return layout.iat + static_cast<std::uint32_t>(import) * 8;
}

std::vector<std::uint8_t> buildStaticPe(const Target& target,
                                        const std::vector<std::uint8_t>& text,
                                        const std::vector<std::uint8_t>& rdata,
                                        std::uint64_t entryOffset) {
  (void)rdata;  // The backend emits its data islands inside .text; no .rdata.

  if (target.arch != Arch::X86_64 || target.os != Os::Windows ||
      target.abi != Abi::Win64) {
    throw std::runtime_error("native/pe: only x86_64-windows-win64 is supported");
  }
  if (text.empty()) {
    throw std::runtime_error("native/pe: text section is empty");
  }
  if (entryOffset >= text.size()) {
    throw std::runtime_error("native/pe: entry offset out of range");
  }

  const IdataLayout layout = idataLayout();
  std::vector<std::uint8_t> idata = buildIdata(layout);
  const std::uint32_t idataSize = static_cast<std::uint32_t>(idata.size());
  const std::uint32_t textSize = static_cast<std::uint32_t>(text.size());

  const std::uint32_t idataRawSize = alignUp(idataSize, kFileAlignment);
  const std::uint32_t textRawSize = alignUp(textSize, kFileAlignment);
  const std::uint32_t textRva = kPeTextRva;
  const std::uint32_t textFileOffset = kIdataFileOffset + idataRawSize;
  const std::uint32_t sizeOfImage = alignUp(textRva + textSize, kSectionAlignment);
  const std::uint32_t entryRva = textRva + static_cast<std::uint32_t>(entryOffset);

  std::vector<std::uint8_t> out;

  // --- DOS header (64 bytes) + e_lfanew ---
  out.resize(64, 0);
  out[0] = static_cast<std::uint8_t>(kDosMagic & 0xFF);
  out[1] = static_cast<std::uint8_t>((kDosMagic >> 8) & 0xFF);
  out[0x3C] = 0x40;  // e_lfanew = 0x40

  // --- PE signature + COFF file header ---
  out.resize(0x40 + 4 + 20, 0);
  std::memcpy(out.data() + 0x40, "PE\0\0", 4);

  const std::size_t fh = 0x44;
  write16At(out, fh + 0, kMachineAmd64);
  write16At(out, fh + 2, 2);  // NumberOfSections = .idata, .text
  write16At(out, fh + 16, 240);  // SizeOfOptionalHeader (PE32+)
  write16At(out, fh + 18,
            static_cast<std::uint16_t>(kCharacteristicsExecutable |
                                       kCharacteristicsLargeAddressAware |
                                       kCharacteristicsRelocsStripped));

  // --- Optional header (PE32+, 240 bytes) ---
  const std::size_t oh = fh + 20;
  out.resize(oh + 240, 0);

  write16At(out, oh + 0, kOptionalMagicPe32Plus);
  out[oh + 2] = 1;  // MajorLinkerVersion
  write32At(out, oh + 4, textRawSize);   // SizeOfCode
  write32At(out, oh + 8, idataRawSize);  // SizeOfInitializedData
  write32At(out, oh + 16, entryRva);     // AddressOfEntryPoint
  write32At(out, oh + 20, textRva);      // BaseOfCode
  write64At(out, oh + 24, kPeImageBase); // ImageBase
  write32At(out, oh + 32, kSectionAlignment);
  write32At(out, oh + 36, kFileAlignment);
  write16At(out, oh + 40, 6);  // MajorOperatingSystemVersion
  write16At(out, oh + 48, 6);  // MajorSubsystemVersion
  write32At(out, oh + 56, sizeOfImage);
  write32At(out, oh + 60, kIdataFileOffset);  // SizeOfHeaders
  write16At(out, oh + 68, 3);                 // Subsystem = console
  write64At(out, oh + 72, 0x100000);          // SizeOfStackReserve
  write64At(out, oh + 80, 0x1000);            // SizeOfStackCommit
  write64At(out, oh + 88, 0x100000);          // SizeOfHeapReserve
  write64At(out, oh + 96, 0x1000);            // SizeOfHeapCommit
  write32At(out, oh + 108, 16);               // NumberOfRvaAndSizes

  // DataDirectory[1] = Import Table.
  write32At(out, oh + 112 + 8, layout.descriptors);
  write32At(out, oh + 112 + 12,
            static_cast<std::uint32_t>((kDllCount + 1) * 20));

  // DataDirectory[12] = Import Address Table. Windows reads it to find the
  // bound thunks; a zero entry is tolerated by some loaders but not all, so
  // publish the real range (every slot, including the null terminator).
  write32At(out, oh + 112 + 12 * 8, layout.iat);
  write32At(out, oh + 112 + 12 * 8 + 4,
            static_cast<std::uint32_t>((kImportCount + 1) * 8));

  // --- Section headers (2 * 40 bytes) ---
  const std::size_t sh = oh + 240;
  out.resize(sh + 80, 0);

  // .idata: initialized data, readable and writable (the loader binds the IAT).
  std::memcpy(out.data() + sh, ".idata\0\0", 8);
  write32At(out, sh + 8, idataSize);       // VirtualSize
  write32At(out, sh + 12, kIdataRva);      // VirtualAddress
  write32At(out, sh + 16, idataRawSize);   // SizeOfRawData
  write32At(out, sh + 20, kIdataFileOffset);  // PointerToRawData
  write32At(out, sh + 36, 0xC0000040);     // INITIALIZED_DATA | READ | WRITE

  // .text: code, executable and readable.
  std::memcpy(out.data() + sh + 40, ".text\0\0\0", 8);
  write32At(out, sh + 40 + 8, textSize);   // VirtualSize
  write32At(out, sh + 40 + 12, textRva);   // VirtualAddress
  write32At(out, sh + 40 + 16, textRawSize);
  write32At(out, sh + 40 + 20, textFileOffset);
  write32At(out, sh + 40 + 36, 0x60000020);  // CODE | EXECUTE | READ

  // --- Section contents ---
  padTo(out, kIdataFileOffset);
  out.insert(out.end(), idata.begin(), idata.end());
  padTo(out, textFileOffset);
  out.insert(out.end(), text.begin(), text.end());
  // The file must hold the whole of .text's raw data. SizeOfRawData is the
  // aligned size, and a loader that finds the file short of it rejects the
  // image (Windows returns ERROR_BAD_EXE_FORMAT; Wine is lenient, so an
  // unpadded image can look fine under Wine and still fail on Windows).
  out.resize(static_cast<std::size_t>(textFileOffset) + textRawSize, 0);

  return out;
}
