// Tests for the bare-metal x86-64 (multiboot) backend (PHASE 7g).
//
// Two levels:
//   1. Structural: the emitted image is a well-formed ELF32/i386 multiboot
//      kernel -- correct header fields, the multiboot magic within the first
//      8 KiB, two PT_LOAD segments (code R+X, `.bss` R+W with no file bytes),
//      and the entry point inside the code segment. Portable, always runs.
//   2. End to end: if `qemu-system-x86_64` is on PATH, boot the image with
//      `-kernel`, capture the serial output and check the program's stdout and
//      exit code. The prelude publishes the exit code on QEMU's isa-debug-exit
//      port, so the boot terminates instead of spinning in `hlt`. CI installs
//      QEMU and sets `OSKEL_REQUIRE_QEMU`, which turns a missing binary into a
//      failure: a boot test that silently skips is a boot test that never runs.
//
// The program is compiled from real Oskel source (lex -> parse -> compile ->
// lower), so the whole pipeline is exercised, not a hand-built chunk.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "bytecode.h"
#include "compiler.h"
#include "lexer.h"
#include "native/bare_metal.h"
#include "native/codegen.h"
#include "native/lower.h"
#include "native/target.h"
#include "parser.h"

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

static std::uint32_t read32(const std::vector<std::uint8_t>& bytes, std::size_t off) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(bytes[off + i]) << (8 * i);
  }
  return value;
}

static std::uint16_t read16(const std::vector<std::uint8_t>& bytes, std::size_t off) {
  return static_cast<std::uint16_t>(bytes[off] | (bytes[off + 1] << 8));
}

static Bytecode compileSource(const std::string& source) {
  auto tokens = lex(source);
  Parser parser(std::move(tokens), "<bare-metal-test>");
  auto program = parser.parseProgram();
  return compileProgram(program);
}

// Builds the bare-metal image for `source`. Returns the ELF bytes and the
// entry address, so the structural checks can look at both.
struct Image {
  std::vector<std::uint8_t> elf;
  std::uint32_t entry = 0;
};

static Image buildImage(const std::string& source) {
  Bytecode bytecode = compileSource(source);
  LoweredChunk chunk = lowerToX64(Target::bareMetalX64(), bytecode, CodeGen{});
  Image image;
  image.elf = buildBareMetalElf(Target::bareMetalX64(), chunk.code, chunk.processEntry);
  image.entry = read32(image.elf, 24);
  return image;
}

static void test_elf_structure() {
  Image image = buildImage("print(\"hi\");\n");
  const std::vector<std::uint8_t>& elf = image.elf;

  check(elf.size() >= 0x1000, "the image has a header and code");
  check(elf[0] == 0x7F && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F',
        "ELF magic is 7F 45 4C 46");
  check(elf[4] == 1, "ELF class is 32-bit (multiboot enters in 32-bit mode)");
  check(elf[5] == 1, "ELF data is little-endian");
  check(read16(elf, 16) == 2, "e_type is ET_EXEC");
  check(read16(elf, 18) == 3, "e_machine is EM_386");
  check(read16(elf, 44) == 2, "e_phnum is 2");
  // The header is exactly 52 bytes and its 16-bit tail (e_ehsize .. e_shstrndx)
  // must not have been written with the 32-bit helper, which would spill into
  // the program headers. `_GLIBCXX_ASSERTIONS` (Ubuntu's default build flags)
  // catches the out-of-range write; these reads catch the wrong field width.
  check(read16(elf, 40) == 52, "e_ehsize is the 52-byte ELF32 header");
  check(read16(elf, 46) == 40, "e_shentsize is the ELF32 section header size");
  check(read16(elf, 48) == 0, "e_shnum is zero (no section table)");
  check(read16(elf, 50) == 0, "e_shstrndx is zero (no section table)");

  // The multiboot header is the first thing in the *code* buffer, which lands
  // at file offset 0x1000 (after the ELF header and the program headers), and
  // multiboot requires it within the first 8 KiB of the file.
  constexpr std::size_t kCodeFileOffset = 0x1000;
  check(read32(elf, kCodeFileOffset) == kMultibootMagic,
        "the code segment starts with the multiboot magic");
  const std::uint32_t flags = read32(elf, kCodeFileOffset + 4);
  const std::uint32_t checksum = read32(elf, kCodeFileOffset + 8);
  check(static_cast<std::uint32_t>(kMultibootMagic + flags + checksum) == 0,
        "the multiboot checksum makes the three dwords sum to zero");

  // Program headers start at e_phoff (52).
  const std::size_t ph = read32(elf, 28);
  check(ph == 52, "the program headers follow the ELF header");

  // Segment 1: the code at 1 MiB, readable and executable.
  check(read32(elf, ph + 0) == 1, "segment 1 is PT_LOAD");
  check(read32(elf, ph + 8) == kBMCodeBase, "segment 1 loads at the code base");
  check(read32(elf, ph + 24) == 5, "segment 1 is read + execute");
  check(read32(elf, ph + 16) > 0, "segment 1 has file bytes");
  check(read32(elf, ph + 20) == read32(elf, ph + 16),
        "segment 1 has no .bss (filesz == memsz)");

  // Segment 2: the `.bss` at 2 MiB, read + write, no file bytes.
  const std::size_t ph2 = ph + 32;
  check(read32(elf, ph2 + 0) == 1, "segment 2 is PT_LOAD");
  check(read32(elf, ph2 + 8) == kBMDataBase, "segment 2 loads at the data base");
  check(read32(elf, ph2 + 16) == 0, "segment 2 has no file bytes (p_filesz == 0)");
  check(read32(elf, ph2 + 20) == kBMBssBytes, "segment 2 zeroes the whole .bss");
  check(read32(elf, ph2 + 24) == 6, "segment 2 is read + write");

  // The entry point must land inside the code segment.
  check(image.entry >= kBMCodeBase &&
            image.entry < kBMCodeBase + read32(elf, ph + 16),
        "e_entry points inside the code segment");

  // The trampoline enables SSE (CR4.OSFXSR | CR4.OSXMMEXCPT, bits 9 and 10)
  // alongside PAE before it enters the runtime. Without them the first SSE
  // instruction the float path emits faults, and with no IDT installed that is
  // a triple fault (a silent hang). Look for the `mov eax, cr4; or eax,
  // 0x620; mov cr4, eax` sequence the trampoline uses.
  bool enablesSse = false;
  const std::uint8_t cr4Seq[] = {0x0F, 0x20, 0xE0, 0x0D, 0x20, 0x06, 0x00, 0x00,
                                 0x0F, 0x22, 0xE0};
  for (std::size_t i = 0; i + sizeof cr4Seq <= elf.size(); ++i) {
    if (std::memcmp(&elf[i], cr4Seq, sizeof cr4Seq) == 0) {
      enablesSse = true;
      break;
    }
  }
  check(enablesSse, "the trampoline sets CR4.PAE | OSFXSR | OSXMMEXCPT");

  // The multiboot magic must be within the first 8192 bytes (multiboot spec).
  bool magicInFirst8K = false;
  for (std::size_t i = 0; i + 4 <= elf.size() && i < 8192; ++i) {
    if (read32(elf, i) == kMultibootMagic) {
      magicInFirst8K = true;
      break;
    }
  }
  check(magicInFirst8K, "the multiboot magic is inside the first 8 KiB");
}

static void test_rejects_wrong_target() {
  bool rejected = false;
  try {
    buildBareMetalElf(Target::linuxX64(), {0xC3}, 0);
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "the bare-metal emitter rejects a non-bare-metal target");
}

// The IDT must be present in the image. The strongest portable check is that
// the entry point's `lidt` and the gate table are consistent: the gate for a
// vector points into the code segment and has the interrupt-gate type byte.
static void test_idt_is_present() {
  Image image = buildImage("print(\"hi\");\n");
  const std::vector<std::uint8_t>& elf = image.elf;

  // The IDT pseudo-descriptor is 10 bytes (2-byte limit + 8-byte base) and the
  // gate table it points at is 256 * 16 = 4096 bytes, so look for a plausible
  // descriptor: limit 0x0FFF followed by a base inside the code segment.
  const std::size_t codeBase = static_cast<std::size_t>(kBMCodeBase);
  bool found = false;
  for (std::size_t i = 0; i + 10 <= elf.size(); ++i) {
    if (read16(elf, i) != 0x0FFF) continue;
    std::uint64_t base = 0;
    for (int b = 0; b < 8; ++b) base |= static_cast<std::uint64_t>(elf[i + 2 + b]) << (8 * b);
    if (base < codeBase || base >= codeBase + elf.size()) continue;
    found = true;
    break;
  }
  check(found, "the image carries an IDT pseudo-descriptor (limit 0x0FFF)");

  // `lidt [rip + disp]` is 0F 01 1D followed by a disp32. Find one and check
  // the displacement lands on a descriptor above.
  bool lidtFound = false;
  for (std::size_t i = 0; i + 7 <= elf.size(); ++i) {
    if (elf[i] != 0x0F || elf[i + 1] != 0x01 || elf[i + 2] != 0x1D) continue;
    lidtFound = true;
    break;
  }
  check(lidtFound, "the entry point installs the IDT with lidt");
}

// ------------------------------------------------------------ QEMU end to end

// CI sets `OSKEL_REQUIRE_QEMU` on the runners, where the package is installed
// on purpose. Without the variable a developer box may skip the boot tests;
// with it a missing QEMU is a failure, not a silent skip, because a test that
// quietly disappears is how the bare-metal path rots. Defined outside the
// platform guard so a platform without the emulator still honours it.
static bool qemuRequired() {
  const char* value = std::getenv("OSKEL_REQUIRE_QEMU");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

// The image is x86-64, but QEMU emulates that on any host, so the boot tests
// run on every platform the emulator supports -- including an arm64 macOS
// runner -- not only on an x86-64 host.
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
#define BARE_METAL_QEMU_TESTS 1
#endif

#if defined(__linux__) || defined(__APPLE__)
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef BARE_METAL_QEMU_TESTS

// `std::system` reports the child's status differently per platform: POSIX
// packs it into a wait status (`WEXITSTATUS`), Windows returns the exit code
// as-is.
static int systemExitCode(int status) {
#if defined(_WIN32)
  return status;
#else
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// The null device differs: `NUL` on Windows, `/dev/null` everywhere else.
#if defined(_WIN32)
static const char* const kNullDevice = "NUL";
#else
static const char* const kNullDevice = "/dev/null";
#endif

static bool haveQemu() {
  // Probing with `--version` beats `command -v`: the latter is a shell
  // builtin and is absent from Windows' `cmd`.
  const std::string command =
      std::string("qemu-system-x86_64 --version > ") + kNullDevice + " 2>&1";
  return systemExitCode(std::system(command.c_str())) == 0;
}

// A writable directory that exists on every platform: the environment names it
// (`TMPDIR` on macOS, `TEMP`/`TMP` on Windows), with `/tmp` as the Linux
// fallback.
static std::string tempDir() {
  for (const char* name : {"TMPDIR", "TEMP", "TMP"}) {
    const char* value = std::getenv(name);
    if (value != nullptr && *value != '\0') return std::string(value);
  }
  return "/tmp";
}

static std::string tempPath(const std::string& name) {
  std::string dir = tempDir();
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
  // QEMU and `cmd` both take forward slashes on Windows, and they sidestep the
  // backslash escaping in `-serial file:C:\...`.
  for (char& c : dir) {
    if (c == '\\') c = '/';
  }
  return dir + name;
}

// Quote an argument so a path with spaces survives both `sh` and `cmd`.
static std::string quote(const std::string& value) { return "\"" + value + "\""; }

static void writeFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return;
  std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
#if !defined(_WIN32)
  // QEMU only reads the file, so the executable bit is a POSIX nicety.
  chmod(path.c_str(), 0755);
#endif
}

// Boots `path` in QEMU and returns the serial output and the exit code the
// program published on the isa-debug-exit port. QEMU maps the port value to
// `(value << 1) | 1`, so a program exit code of N shows up as 2N + 1.
struct BootResult {
  std::string output;
  int exitCode = -1;
  bool booted = false;
};

static BootResult boot(const std::string& path) {
  const std::string serial = path + ".serial";
  std::remove(serial.c_str());
  std::string command = "qemu-system-x86_64 -kernel " + quote(path) + " -serial " +
                        quote("file:" + serial) + " -display none -no-reboot" +
                        " -device isa-debug-exit,iobase=0x501,iosize=0x04" +
                        " > " + kNullDevice + " 2>&1";
  int status = std::system(command.c_str());

  BootResult result;
  const int code = systemExitCode(status);
  // 2N + 1: recover N. Anything else means the boot did not reach the exit.
  if (code >= 1 && code % 2 == 1) {
    result.exitCode = (code - 1) / 2;
    result.booted = true;
  }

  std::FILE* file = std::fopen(serial.c_str(), "rb");
  if (file) {
    char buffer[256];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
      result.output.append(buffer, n);
    }
    std::fclose(file);
  }
  return result;
}

static void test_qemu_boot() {
  // A program that exercises the runtime: a string literal, integer and float
  // formatting, a recursive call, an array and a map -- all through the
  // embedded runtime, whose only difference from Linux is the prelude.
  const std::string source =
      "fn fib(n) {\n"
      "  if (n < 2) { return n; }\n"
      "  return fib(n - 1) + fib(n - 2);\n"
      "}\n"
      "var xs = [1, 2, 3, 4, 5];\n"
      "var total = 0;\n"
      "for x in xs { total = total + x; }\n"
      "print(\"sum =\", total);\n"
      "print(\"fib(10) =\", fib(10));\n"
      "print(\"pi ~\", 3.14159);\n"
      "var m = {\"a\": 1};\n"
      "print(\"a =\", m[\"a\"]);\n";

  Image image = buildImage(source);
  const std::string path = tempPath("oskel_bare_metal_boot.elf");
  writeFile(path, image.elf);

  BootResult result = boot(path);
  check(result.booted, "the image boots and reaches the exit port");

  const std::string expected =
      "sum = 15\nfib(10) = 55\npi ~ 3.14159\na = 1\n";
  check(result.output == expected,
        ("the booted program prints the expected bytes [" + result.output + "]").c_str());
  check(result.exitCode == 0, "the booted program exits with code 0");
}

// The kernel reads the wall clock from the CMOS RTC and converts it to a Unix
// timestamp in assembly (no syscall, no libc). QEMU's default RTC tracks the
// host clock, so `time()` inside the guest must land close to the host's own
// time -- a wrong epoch base, a BCD misread or a 12-hour slip would be off by
// days, not seconds.
static void test_qemu_wall_clock() {
  const std::string source = "print(time());\n";
  Image image = buildImage(source);
  const std::string path = "/tmp/oskel_bare_metal_clock.elf";
  writeFile(path, image.elf);

  const std::int64_t before = static_cast<std::int64_t>(std::time(nullptr));
  BootResult result = boot(path);
  const std::int64_t after = static_cast<std::int64_t>(std::time(nullptr));
  check(result.booted, "the clock image boots and reaches the exit port");

  // The guest prints one integer and a newline.
  std::int64_t guest = 0;
  bool parsed = !result.output.empty();
  std::size_t i = 0;
  if (parsed) {
    for (; i < result.output.size() && result.output[i] != '\n'; ++i) {
      if (result.output[i] < '0' || result.output[i] > '9') {
        parsed = false;
        break;
      }
      guest = guest * 10 + (result.output[i] - '0');
    }
  }
  check(parsed, ("the guest printed an integer [" + result.output + "]").c_str());
  if (parsed) {
    // A generous window: the boot itself takes a moment, but a wrong epoch base
    // is off by years and a wrong hour by thousands of seconds.
    check(guest >= before - 60 && guest <= after + 60,
          ("the RTC clock agrees with the host [" + std::to_string(guest) + " vs " +
           std::to_string(before) + ".." + std::to_string(after) + "]").c_str());
  }
}

static void test_qemu_float_arithmetic() {
  // The trampoline must set CR4.OSFXSR and CR4.OSXMMEXCPT before the runtime
  // runs, or the first SSE instruction is a #UD: with no IDT that is a double
  // fault and then a triple fault (a silent hang). Printing a float literal
  // only exercises the integer `dtoa`, so the float *arithmetic* path is what
  // pins the fix. `1.0 / 3.0` reproduces the original hang exactly.
  const std::string source =
      "print(\"add\", 1.0 + 1.0);\n"
      "print(\"div\", 1.0 / 3.0);\n"
      "print(\"mul\", 2.0 * 3.5);\n"
      "print(\"cmp\", 1.5 < 2.5, 3.0 >= 3.0);\n";

  Image image = buildImage(source);
  const std::string path = tempPath("oskel_bare_metal_float.elf");
  writeFile(path, image.elf);

  BootResult result = boot(path);
  check(result.booted, "the float program boots and reaches the exit port");

  const std::string expected =
      "add 2\ndiv 0.3333333333333333\nmul 7\ncmp true true\n";
  check(result.output == expected,
        ("the booted float program prints the expected bytes [" + result.output + "]")
            .c_str());
  check(result.exitCode == 0, "the booted float program exits with code 0");
}

static void test_qemu_fault_reports() {
  // A fault must report, not hang. Build a normal image and overwrite the
  // program's entry with `ud2` (0F 0B): that is a #UD (vector 6) with no error
  // code, exactly the class of fault that used to escalate to a triple fault.
  // The IDT should catch it, print `oskel: cpu exception 0x06 rip=0x...` and
  // publish the fault exit code on the isa-debug-exit port.
  Bytecode bytecode = compileSource("print(\"unreachable\");\n");
  LoweredChunk chunk = lowerToX64(Target::bareMetalX64(), bytecode, CodeGen{});
  const std::uint64_t entry = chunk.programEntry;
  chunk.code[entry] = 0x0F;
  chunk.code[entry + 1] = 0x0B;

  Image image;
  image.elf = buildBareMetalElf(Target::bareMetalX64(), chunk.code, chunk.processEntry);
  const std::string path = tempPath("oskel_bare_metal_fault.elf");
  writeFile(path, image.elf);

  BootResult result = boot(path);
  check(result.booted, "a faulting program reaches the fault handler's exit");

  // The vector is 6 (#UD) and the RIP is in the code segment (0x10xxxx).
  const bool reported =
      result.output.rfind("oskel: cpu exception 0x06 rip=0x", 0) == 0 &&
      result.output.find("rip=0x000000000010") != std::string::npos;
  check(reported, ("the fault handler reports the vector and RIP ["
                   + result.output + "]").c_str());

  // A fault *with* an error code must frame the same way. Write to an unmapped
  // address (`movabs rax, 0x100000000; mov [rax], rax`): a #PF (vector 14),
  // which the CPU pushes with an error code, so the stub must not push a dummy
  // one. If it did, the vector the handler reads would be the error code and
  // the report would name the wrong exception.
  LoweredChunk pf = lowerToX64(Target::bareMetalX64(), bytecode, CodeGen{});
  const std::uint64_t at = pf.programEntry;
  const std::uint8_t pageFault[] = {
      0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,  // movabs rax, 0x100000000
      0x48, 0x89, 0x00,                                            // mov [rax], rax
  };
  std::memcpy(&pf.code[at], pageFault, sizeof pageFault);

  Image pfImage;
  pfImage.elf = buildBareMetalElf(Target::bareMetalX64(), pf.code, pf.processEntry);
  const std::string pfPath = tempPath("oskel_bare_metal_pf.elf");
  writeFile(pfPath, pfImage.elf);

  BootResult pfResult = boot(pfPath);
  check(pfResult.booted, "a page fault reaches the fault handler's exit");
  check(pfResult.output.rfind("oskel: cpu exception 0x0e rip=0x", 0) == 0,
        ("the error-code fault is reported as vector 14 ["
         + pfResult.output + "]").c_str());
}

#endif  // BARE_METAL_QEMU_TESTS

int main() {
  test_elf_structure();
  test_rejects_wrong_target();
  test_idt_is_present();

#ifdef BARE_METAL_QEMU_TESTS
  if (haveQemu()) {
    test_qemu_boot();
    test_qemu_wall_clock();
    test_qemu_float_arithmetic();
    test_qemu_fault_reports();
  } else if (qemuRequired()) {
    // CI installs QEMU on purpose: its absence there means the runner setup
    // broke, so fail loudly instead of passing with the boot tests gone.
    std::printf("FAIL: OSKEL_REQUIRE_QEMU is set but qemu-system-x86_64 is missing\n");
    ++g_failures;
  } else {
    std::printf("skip: qemu-system-x86_64 not found, boot test skipped\n");
  }
#else
  if (qemuRequired()) {
    std::printf("FAIL: OSKEL_REQUIRE_QEMU is set but this platform cannot boot "
                "the bare-metal image\n");
    ++g_failures;
  }
#endif

  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all bare-metal tests passed\n");
  return 0;
}
