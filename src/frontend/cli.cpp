#include "frontend/cli_internal.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "compiler.h"
#include "core/errors.h"
#include "core/util.h"
#include "frontend/ast.h"
#include "frontend/diagnostic.h"
#include "frontend/lexer.h"
#include "frontend/module_resolver.h"
#include "frontend/parser.h"
#include "native/bare_metal.h"
#include "native/codegen.h"
#include "native/elf.h"
#include "native/link.h"
#include "native/lower.h"
#include "native/object.h"
#include "native/pe.h"
#include "native/target.h"
#include "native/wasm.h"
#include "platform.h"
#include "ui_runtime.h"
#include "version.h"

const char *kVersion = OSKEL_VERSION;

namespace {

// Joins a directory and a relative path without doubling the separator.
std::string joinPath(const std::string &dir, const std::string &relative) {
  if (dir.empty()) return relative;
  char last = dir.back();
  if (last == '/' || last == '\\') return dir + relative;
  return dir + "/" + relative;
}

// Absolute path of the running executable, so `standardLibraryDir` can locate
// the shipped `std/` even when the toolchain was invoked through `PATH` and
// `argv[0]` has no directory part. Falls back to `argv0` when it cannot be
// determined.
std::string executablePath(const std::string &argv0) {
  if (argv0.find('/') != std::string::npos ||
      argv0.find('\\') != std::string::npos) {
    return argv0;
  }
#ifdef _WIN32
  char buffer[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return std::string(buffer, n);
#elif defined(__linux__)
  char buffer[4096];
  ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (n > 0) return std::string(buffer, static_cast<size_t>(n));
#endif
  // Last resort: search `PATH` for the bare name.
  if (const char *path = std::getenv("PATH")) {
    std::string dirs(path);
    size_t start = 0;
    while (start <= dirs.size()) {
      size_t end = dirs.find(':', start);
      std::string dir = dirs.substr(start, end == std::string::npos
                                               ? std::string::npos
                                               : end - start);
      if (!dir.empty()) {
        std::string candidate = joinPath(dir, argv0);
        std::ifstream probe(candidate, std::ios::binary);
        if (probe.good()) return candidate;
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  return argv0;
}

} // namespace

// Directory of the standard library shipped with the toolchain. `std.*`
// imports resolve here (see docs/language.md §18: `std.` is reserved).
//
// `$OSKEL_STDLIB` overrides the search. Otherwise the standard library is
// looked for in the working directory and, walking up from the executable's
// directory, in `<dir>/std`. The upward walk is what lets an import resolve
// whether the binary sits in the source tree, in a single-config build
// (`<root>/build`), in a multi-config build (`<root>/build/<config>`, MSVC) or
// in an install prefix; the `share/oskel/std` location covers the install
// layout explicitly.
std::string standardLibraryDir(const std::string &argv0) {
  if (const char *env = std::getenv("OSKEL_STDLIB")) return env;

  auto hasStd = [](const std::string &dir) {
    std::string ignored;
    return readSourceFile(joinPath(dir, "math.oskl"), ignored);
  };

  if (hasStd("std")) return "std";

  std::string dir = directoryOf(executablePath(argv0));
  for (int depth = 0; depth < 4; ++depth) {
    std::string candidate = joinPath(dir, "std");
    if (hasStd(candidate)) return candidate;
    dir = joinPath(dir, "..");
  }

  std::string installed = joinPath(directoryOf(executablePath(argv0)), "../share/oskel/std");
  if (hasStd(installed)) return installed;

  return "std";  // Nothing found: imports report "module not found".
}

// Installed-packages root: `$OSKEL_PACKAGES`, else `$HOME/.oskel/packages`.
// Empty when the home directory cannot be determined, which disables the
// package lookup step.
std::string packagesDir() {
  if (const char *explicitDir = std::getenv("OSKEL_PACKAGES")) return explicitDir;
  const char *home = std::getenv("HOME");
#ifdef _WIN32
  if (home == nullptr || *home == '\0') home = std::getenv("USERPROFILE");
#endif
  if (home == nullptr || *home == '\0') return std::string();
  return std::string(home) + "/.oskel/packages";
}

// Resolver for a program named `name`. Imports resolve from the project root:
// the directory of the file being compiled, or the current directory for
// `-e`/stdin. A nested import resolves from the same root, so `modules.util`
// names `<root>/modules/util.oskl` no matter which module declares it.
//
// `$OSKEL_MODULE_ROOT` overrides that root. `oskelp` sets it to a package's
// `src/`, so `import pkg.mod` resolves to `<project>/src/pkg/mod.oskl` even
// though the entry point lives in `src/pkg/`. Both engines go through this
// function, so the VM and the AOT backend resolve imports identically.
ModuleResolver resolverFor(const std::string &name, const std::string &argv0) {
  bool isFile = !name.empty() && name[0] != '<';
  const char *moduleRoot = std::getenv("OSKEL_MODULE_ROOT");
  std::string projectDir = (moduleRoot != nullptr && *moduleRoot != '\0')
                               ? std::string(moduleRoot)
                               : (isFile ? directoryOf(name) : std::string("."));
  return ModuleResolver(projectDir, standardLibraryDir(argv0), packagesDir());
}

namespace {

// Parses a module once and returns the enums it declares. The parser needs the
// names before the compiler loads the module (so an imported enum is a valid
// annotation and switch label), which is why this lives beside the import
// loader rather than in the compiler. `visiting_` breaks import cycles; a
// module that cannot be parsed here is reported by the compiler later.
class EnumScannerImpl {
 public:
  explicit EnumScannerImpl(ModuleResolver resolver) : resolver_(std::move(resolver)) {}

  std::unordered_map<std::string, std::vector<EnumMember>> scan(const std::string &path) {
    auto cached = cache_.find(path);
    if (cached != cache_.end()) return cached->second;
    if (!visiting_.insert(path).second) return {};
    std::unordered_map<std::string, std::vector<EnumMember>> result;
    std::string file;
    std::string message;
    if (resolver_.resolve(path, file, message)) {
      std::string source;
      if (readSourceFile(file, source)) {
        try {
          Parser parser(lex(source), file);
          parser.setEnumScanner([this](const std::string &nested) { return scan(nested); });
          StmtPtr program = parser.parseProgram();
          if (const auto *block =
                  program ? std::get_if<BlockStmt>(&program->node) : nullptr) {
            for (const auto &child : block->stmts) {
              if (!child) continue;
              if (const auto *decl = std::get_if<EnumDeclStmt>(&child->node)) {
                result[decl->name] = decl->members;
              }
            }
          }
        } catch (const CompileError &) {
          // The compiler reports the module's real error when it loads it.
        }
      }
    }
    visiting_.erase(path);
    cache_[path] = result;
    return result;
  }

 private:
  ModuleResolver resolver_;
  std::unordered_map<std::string,
                     std::unordered_map<std::string, std::vector<EnumMember>>>
      cache_;
  std::unordered_set<std::string> visiting_;
};

}  // namespace

Parser::EnumScanner makeEnumScanner(ModuleResolver resolver) {
  auto scanner = std::make_shared<EnumScannerImpl>(std::move(resolver));
  return [scanner](const std::string &path) { return scanner->scan(path); };
}

// Builds the module loader the compiler uses for `import` declarations. It
// resolves the dotted path to a file, reads and parses it, and returns the
// program. A path that cannot be resolved becomes the compile error
// "module 'X' not found"; a parse error inside the module is reported with the
// module's own file name in the message.
Compiler::ImportLoader makeImportLoader(ModuleResolver resolver) {
  // The loader owns its resolver: it outlives the statement that built it. The
  // same resolver backs the parser's enum scanner, so a module's own imports
  // make their enums visible before the compiler compiles the module.
  auto enumScanner = makeEnumScanner(resolver);
  return [resolver, enumScanner](const std::string &path, int line,
                                 int col) -> StmtPtr {
    std::string file;
    std::string message;
    if (!resolver.resolve(path, file, message)) {
      throw CompileError{message, line, col};
    }
    std::string moduleSource;
    if (!readSourceFile(file, moduleSource)) {
      throw CompileError{"cannot read module '" + path + "' from '" + file + "'", line, col};
    }
    try {
      Parser parser(lex(moduleSource), file);
      parser.setEnumScanner(enumScanner);
      StmtPtr program = parser.parseProgram();
      for (const Warning &warning : parser.warnings())
        printCompileWarning(file, moduleSource, warning, platformPosix());
      return program;
    } catch (const CompileError &error) {
      throw CompileError{"in module '" + path + "': " + error.message, error.line, error.col};
    }
  };
}

// The AOT targets `--target` accepts, keyed by the canonical backend name from
// `targetName()`. The default is the host's own target, so a build on Linux
// produces an ELF and on Windows an EXE without a flag.
bool parseTargetName(const std::string &name, Target &out) {
  if (name == targetName(Target::linuxX64())) {
    out = Target::linuxX64();
    return true;
  }
  if (name == targetName(Target::winX64())) {
    out = Target::winX64();
    return true;
  }
  if (name == targetName(Target::macX64())) {
    out = Target::macX64();
    return true;
  }
  if (name == targetName(Target::bareMetalX64())) {
    out = Target::bareMetalX64();
    return true;
  }
  if (name == targetName(Target::wasm32())) {
    out = Target::wasm32();
    return true;
  }
  return false;
}

Target hostTarget() {
#ifdef _WIN32
  return Target::winX64();
#elif defined(__APPLE__)
  // macOS only has the hosted backend: the image is Mach-O and the host
  // toolchain closes it. Returning the macOS target makes `oskel build --mode
  // hosted` produce a native image without a `--target` flag.
  return Target::macX64();
#else
  return Target::linuxX64();
#endif
}

// The link modes `--mode` accepts, keyed by the name `linkModeName()` prints.
// The default is `freestanding`, the mode Linux, Windows and bare metal support
// and the one that keeps the "not a dynamic executable" invariant, so an
// existing command line keeps producing the same binary. macOS has only the
// hosted backend, so its default is `hosted` (and an explicit `--mode
// freestanding` is reported as unsupported).
bool parseLinkModeName(const std::string &name, LinkMode &out) {
  if (name == linkModeName(LinkMode::Freestanding)) {
    out = LinkMode::Freestanding;
    return true;
  }
  if (name == linkModeName(LinkMode::Hosted)) {
    out = LinkMode::Hosted;
    return true;
  }
  return false;
}

bool parseLinkerName(const std::string &name, Linker &out) {
  if (name == linkerName(Linker::Host)) {
    out = Linker::Host;
    return true;
  }
  if (name == linkerName(Linker::Own)) {
    out = Linker::Own;
    return true;
  }
  return false;
}

const char *linkerName(Linker linker) {
  switch (linker) {
    case Linker::Host:
      return "host";
    case Linker::Own:
      return "own";
  }
  return "unknown";
}

// The C compiler the hosted link drives (PHASE 7f.4). It is a *build-time*
// dependency, never a runtime one: the compiler turns the relocatable object
// into an executable by linking it against the host C library, which is what
// lets an `extern "C"` symbol resolve. `$CC` overrides it (so a musl or cross
// toolchain can be selected), falling back to `cl` on Windows (the MSVC
// toolchain the CI and a default Visual Studio install provide) and `cc`
// elsewhere.
std::string hostedLinkerCommand() {
  if (const char *cc = std::getenv("CC")) {
    if (*cc != '\0') return cc;
  }
#ifdef _WIN32
  return "cl";
#else
  return "cc";
#endif
}

// Quotes a path for the shell the link command runs under. POSIX gets the
// single-quote form (the only character a single-quoted string cannot contain
// is the quote itself, so each one is closed, escaped and reopened); Windows
// `cmd.exe` gets double quotes, which is what it understands.
std::string shellQuote(const std::string &path) {
#ifdef _WIN32
  std::string out = "\"";
  for (char c : path) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  out += "\"";
  return out;
#else
  std::string out = "'";
  for (char c : path) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
#endif
}

// Whether the linker command speaks the Microsoft compiler driver's command
// line. `cl` has a different command line from a POSIX `cc` (an output flag
// `/Fe:` instead of `-o`, and no `-lm`), so the link builder has to know which
// one it is. `$OSKEL_CC_KIND=msvc` names the dialect explicitly, because `$CC`
// may be a wrapper (`vcvarsall.bat` plus `cl`) whose file name says nothing.
bool isMicrosoftCompiler(const std::string &compiler) {
  if (const char *kind = std::getenv("OSKEL_CC_KIND")) {
    if (std::string(kind) == "msvc") return true;
    if (std::string(kind) == "posix") return false;
  }
  std::string name = compiler;
  const std::size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos) name = name.substr(slash + 1);
  for (char &c : name) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) {
    name.resize(name.size() - 4);
  }
  return name == "cl" || name == "clang-cl";
}

// A library name `--link-lib` accepts. It becomes a `-l<name>` argument of the
// host linker, so it must not smuggle a shell metacharacter or a path
// separator into the command.
bool isSafeLinkLibName(const std::string &name) {
  if (name.empty()) return false;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '+' ||
                    c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}

// Runs a link command and returns its exit status. On Windows `std::system`
// routes through `cmd.exe`, whose `/c` quote handling strips the first and last
// quote of a command that starts with a quoted path -- every path here is
// quoted, and the compiler path itself may contain spaces. `CreateProcess`
// passes the command line through unchanged, so the documented `cmd /c
// "<command>"` form is used and cmd removes exactly the outer pair.
int runHostedLinkCommand(const std::string &command) {
#ifdef _WIN32
  std::string line = "cmd.exe /c \"" + command + "\"";
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::vector<char> mutableLine(line.begin(), line.end());
  mutableLine.push_back('\0');
  if (!CreateProcessA(nullptr, mutableLine.data(), nullptr, nullptr, TRUE, 0,
                      nullptr, nullptr, &startup, &process)) {
    return -1;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD status = 1;
  GetExitCodeProcess(process.hProcess, &status);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  return static_cast<int>(status);
#else
  return std::system(command.c_str());
#endif
}

struct UiRequirements {
  bool sdl = false;
  bool skia = false;

  bool any() const { return sdl || skia; }

  // The requirement bitset a `.oskc` header records. The mapping lives here so
  // the writer and the reader cannot disagree about which bit means what.
  std::uint32_t bits() const {
    std::uint32_t value = 0;
    if (sdl) value |= kOskcNeedsSdl;
    if (skia) value |= kOskcNeedsSkia;
    return value;
  }

  static UiRequirements fromBits(std::uint32_t value) {
    UiRequirements needs;
    needs.sdl = (value & kOskcNeedsSdl) != 0;
    needs.skia = (value & kOskcNeedsSkia) != 0;
    return needs;
  }
};

// The UI backend is reached through a module import, not through a symbol
// name: `std.ui` and its Skia/SDL modules export ordinary `oskel_*` functions,
// which a user binding may also spell (`oskel_Rect_new`). The compiler records
// every module it loaded, including the nested ones, so that is the signal.
// `std.internal.sdl` needs only SDL3; the Skia modules need the shim, and the
// window one needs both.
UiRequirements uiRequirements(const std::vector<std::string> &modulePaths,
                             const Bytecode &root) {
  UiRequirements needs;
  for (const std::string &path : modulePaths) {
    const std::string generic = std::filesystem::path(path).generic_string();
    auto isModule = [&generic](const char *name) {
      const std::string dotted = name;
      if (generic == dotted) return true;
      const std::string suffix = "/" + dotted + ".oskl";
      return generic.size() >= suffix.size() &&
             generic.compare(generic.size() - suffix.size(), suffix.size(),
                             suffix) == 0;
    };
    if (isModule("std.internal.sdl")) needs.sdl = true;
    if (isModule("std.internal.skia") || isModule("std.internal.skia_ui")) {
      needs.skia = true;
    }
    if (isModule("std.internal.skia_window")) {
      needs.skia = true;
      needs.sdl = true;
    }
  }

  // A program may also declare an `SDL_*`/`skia_*` symbol directly, without
  // the standard modules. Those prefixes are unambiguous; `oskel_*` is not.
  std::vector<const Bytecode *> pending{&root};
  while (!pending.empty()) {
    const Bytecode *current = pending.back();
    pending.pop_back();
    for (const ExternSymbol &symbol : current->externs) {
      if (symbol.name.rfind("SDL_", 0) == 0) needs.sdl = true;
      if (symbol.name.rfind("skia_", 0) == 0) needs.skia = true;
    }
    for (const std::shared_ptr<Bytecode> &child : current->children) {
      if (child) pending.push_back(child.get());
    }
  }
  return needs;
}

void appendUnique(std::vector<std::string> &values, const std::string &value) {
  for (const std::string &existing : values) {
    if (existing == value) return;
  }
  values.push_back(value);
}

std::string directoryOfPath(const std::string &path) {
  const std::size_t separator = path.find_last_of("/\\");
  return separator == std::string::npos ? std::string()
                                        : path.substr(0, separator);
}

std::string fileNameOfPath(const std::string &path) {
  const std::size_t separator = path.find_last_of("/\\");
  return separator == std::string::npos ? path : path.substr(separator + 1);
}

// Windows resolves a DLL through the executable's directory, so a hosted image
// gets the bundled runtime copied beside it. POSIX images record an rpath
// instead and keep the installation as the single copy.
void copyUiRuntimeBeside(const std::string &output) {
  const std::string directory = directoryOfPath(output);
  for (const char *name : {"SDL3", "skia", "skia_shim"}) {
    const std::string source = oskel::ui::runtimeLibraryPath(name);
    if (source.empty()) continue;
    std::string bytes;
    if (!readSourceFile(source, bytes)) continue;
    const std::string destination =
        (directory.empty() ? std::string() : directory + "/") +
        fileNameOfPath(source);
    std::vector<std::uint8_t> data(bytes.begin(), bytes.end());
    writeBinaryFile(destination, data);
  }
}

// Links a relocatable object into a hosted executable by invoking the host C
// compiler (PHASE 7f.4). `objectPath` is the object just written and `output`
// the requested executable. On Linux `-no-pie` keeps the fixed-address
// assumptions of the backend and `-lm` provides the libm symbols a `std.math`
// extern may name; macOS needs neither (`-lm` is part of libSystem and the
// Mach-O relocations are PC-relative) and gets `-arch x86_64` so an arm64 host
// can cross-link the x86-64 object. Windows drives `cl`, whose output flag is
// `/Fe:` and whose default libraries already carry the CRT. Each entry of
// `linkLibs` is appended after the object, so the linker sees the undefined
// symbols before it is asked for the library that defines them. Returns 0 on
// success, 65 when the toolchain is missing or the link fails -- never a
// half-linked image.
int linkHostedObject(const std::string &objectPath, const std::string &output,
                     const Target &target, const std::string &tool,
                     const std::vector<std::string> &linkLibs,
                     const std::vector<std::string> &libraryPaths,
                     const std::vector<std::string> &linkObjects,
                     const std::vector<std::string> &runPaths) {
  const std::string compiler = hostedLinkerCommand();
  auto isPath = [](const std::string &lib) {
    return lib.find('/') != std::string::npos ||
           lib.find('\\') != std::string::npos;
  };
  std::string command;
  if (target.os == Os::Windows && isMicrosoftCompiler(compiler)) {
    // The object was not produced by `cl`, so it carries no `.drectve` section
    // asking the linker for the CRT. Name the static CRT and kernel32
    // explicitly: without them every runtime symbol (`mainCRTStartup`,
    // `_write`, `malloc`, `GetTickCount64`, ...) stays unresolved.
    command = shellQuote(compiler) + " /nologo /MT " + shellQuote(objectPath);
    for (const std::string &object : linkObjects) {
      command += " " + shellQuote(object);
    }
    command += " /Fe:" + shellQuote(output) +
               " /link /SUBSYSTEM:CONSOLE"
               " libcmt.lib libucrt.lib libvcruntime.lib kernel32.lib";
    for (const std::string &dir : libraryPaths) {
      command += " /LIBPATH:" + shellQuote(dir);
    }
    for (const std::string &lib : linkLibs) {
      const std::string name =
          isPath(lib) || (lib.size() > 4 && lib.substr(lib.size() - 4) == ".lib")
              ? lib
              : lib + ".lib";
      command += " " + shellQuote(name);
    }
  } else {
    command = shellQuote(compiler) + " " + shellQuote(objectPath);
    for (const std::string &object : linkObjects) {
      command += " " + shellQuote(object);
    }
    command += " -o " + shellQuote(output);
    if (target.os == Os::Linux) command += " -no-pie";
    if (target.os == Os::MacOs) command += " -arch x86_64";
    command += " -lm";
    for (const std::string &dir : libraryPaths) command += " -L" + shellQuote(dir);
    for (const std::string &dir : runPaths) {
      command += " -Wl,-rpath," + shellQuote(dir);
    }
    for (const std::string &lib : linkLibs) {
      command += " " + (isPath(lib) ? shellQuote(lib) : "-l" + lib);
    }
  }
  const int status = runHostedLinkCommand(command);
  if (status != 0) {
    std::string message = tool + ": linking the hosted binary failed (command: " +
                          command + ")\n";
    platformPosix().writeErr(message.data(), message.size());
    return 65;
  }
  return 0;
}

// Compiles `source` (resolving imports) and writes a native binary to `output`.
// The whole pipeline is the compiler's: lex, parse, compile to bytecode, then
// lower the bytecode to machine code and wrap it in a static executable. The
// emitted binary embeds only the AOT runtime, never the compiler, so it cannot
// `eval` (DEVELOPMENT.md §3, the GraalVM pitfall).
//
// `target` selects the executable format: Linux gets a static ELF, Windows a
// static PE32+ (PHASE 7e). Both share the lowering and the runtime; only the
// entry prelude and the container differ.
//
// `mode` selects how the image is closed (PHASE 7f.3). `Freestanding` (the
// default) is the self-contained image described above. `Hosted` links against
// the host C library so an `extern "C"` symbol resolves to the real C function;
// it needs a C toolchain at build time and is the mode that makes FFI work.
//
// `tool` names the binary in diagnostics (`oskel build` or `oskelc`).
//
// `linkLibs` names the libraries the hosted link resolves `extern "C"` symbols
// from (`SDL3` becomes `-lSDL3`); it is empty in freestanding mode.
//
// Emits the native image for an already compiled chunk.
//
// This is the shared half of `buildBinary` and of `buildBinaryFromBytecode`:
// once a `Bytecode` exists, lowering it does not care whether it came from the
// parser or from a `.oskc` file. Keeping it in one place is what makes
// "compile once, use anywhere" true: the AOT output of a `.oskc` is produced by
// the same code that produced it from source, so the two cannot drift.
//
// `target` selects the executable format: Linux gets a static ELF, Windows a
// static PE32+ (PHASE 7e). Both share the lowering and the runtime; only the
// entry prelude and the container differ.
//
// `mode` selects how the image is closed (PHASE 7f.3). `Freestanding` (the
// default) is the self-contained image. `Hosted` links against the host C
// library so an `extern "C"` symbol resolves to the real C function; it needs a
// C toolchain at build time and is the mode that makes FFI work.
//
// `autoHostedForUi` lets a program that needs the UI backend build with no
// flags: that backend needs a host, so an unspecified mode becomes `hosted` and
// the bundled libraries are added to the link. An explicit `--mode freestanding`
// still wins, with a diagnostic that names the requirement.
//
// `ui` is what the chunk needs from the UI backend. It is a parameter rather
// than something derived here because the two callers learn it differently: the
// source path reads the modules the compiler loaded, and the `.oskc` path reads
// the requirement bits the writer recorded. Both then run the same code, so a
// program that imports `std.ui` links the same way whichever path built it.
//
// `tool` names the binary in diagnostics (`oskel build` or `oskelc`).
//
// Returns 0 on success, 65 for a lowering error and 74 for a write failure.
int emitNativeImage(const Bytecode &bytecode, const std::string &output,
                    const Target &target, LinkMode mode, Linker linker,
                    bool emitObject, bool autoHostedForUi,
                    const UiRequirements &ui,
                    const std::vector<std::string> &linkLibs,
                    const std::vector<std::string> &libraryPaths,
                    const std::vector<std::string> &linkObjects,
                    const std::string &tool) {
  std::vector<std::uint8_t> image;
  const bool usesUi = ui.any() && !isWasmTarget(target);
  std::vector<std::string> effectiveLinkLibs = linkLibs;
  std::vector<std::string> effectiveLibraryPaths = libraryPaths;
  std::vector<std::string> runPaths;
  try {
    if (usesUi) {
      if (autoHostedForUi && mode == LinkMode::Freestanding) mode = LinkMode::Hosted;
      if (mode != LinkMode::Hosted) {
        throw LoweringError(
            "std.ui requires a hosted binary; remove '--mode freestanding' "
            "(the default is chosen automatically)");
      }
      if (linker == Linker::Own) {
        throw LoweringError(
            "std.ui requires the host C linker; remove '--linker own'");
      }
      const std::string sdl =
          ui.sdl ? oskel::ui::linkLibraryPath("SDL3") : std::string();
      const std::string shim =
          ui.skia ? oskel::ui::linkLibraryPath("skia_shim") : std::string();
      if ((ui.sdl && sdl.empty()) || (ui.skia && shim.empty())) {
        const std::string required =
            ui.sdl && ui.skia ? "SDL3 and skia_shim"
                              : (ui.sdl ? "SDL3" : "skia_shim");
        throw LoweringError(
            "std.ui is installed without its native runtime; reinstall Oskel "
            "with UI support or set OSKEL_UI_LIBDIR to the directory that "
            "contains " +
            required);
      }
      if (!sdl.empty()) appendUnique(effectiveLinkLibs, sdl);
      if (!shim.empty()) appendUnique(effectiveLinkLibs, shim);
      const std::string directory = oskel::ui::libraryDirectory();
      if (!directory.empty()) {
        appendUnique(effectiveLibraryPaths, directory);
        appendUnique(runPaths, directory);
      }
    }
    requireSupportedLinkMode(target, mode);
    if (isWasmTarget(target)) {
      image = lowerToWasm(target, bytecode);
      if (!writeBinaryFile(output, image)) {
        std::string message = tool + ": cannot write '" + output + "'\n";
        platformPosix().writeErr(message.data(), message.size());
        return 74;
      }
      return 0;
    }
    LoweredChunk chunk = lowerToX64(target, bytecode, CodeGen{}, mode);
    if (mode == LinkMode::Hosted) {
      // The hosted image is an object a linker closes against libc (PHASE 7f.4).
      // The object emitter turns the code buffer's external references into
      // relocations; from here either the host C compiler or the project's own
      // linker resolves them and writes the executable.
      std::vector<std::uint8_t> object = buildRelocatableObject(
          target, chunk.code, {}, chunk.externals, chunk.exports);
      if (emitObject) {
        // `--emit-object` stops before the link. The object is the input of
        // `oskelld`, and in PHASE 10.2 it becomes the unit of a relocatable
        // link, so it is written under the requested name and nothing else.
        if (!writeBinaryFile(output, object)) {
          std::string message = tool + ": cannot write '" + output + "'\n";
          platformPosix().writeErr(message.data(), message.size());
          return 74;
        }
        return 0;
      }
      if (linker == Linker::Own) {
        // The project's own linker closes the object itself (PHASE 10.1). It
        // produces the image in memory, so the common write below still runs.
        // Extra `--link-object` files may be objects or `ar` archives.
        std::vector<LinkInput> inputs;
        inputs.push_back({"<main>", object});
        for (const std::string &path : linkObjects) {
          std::string bytes;
          if (!readSourceFile(path, bytes)) {
            std::string message =
                tool + ": cannot read link object '" + path + "'\n";
            platformPosix().writeErr(message.data(), message.size());
            return 65;
          }
          inputs.push_back(
              {path, std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
        }
        LinkOptions options;
        options.target = target;
        options.libraries = effectiveLinkLibs;
        options.libraryPaths = effectiveLibraryPaths;
        image = linkHostedExecutable(inputs, options);
      } else {
        // The historical path: the host C compiler resolves libc and writes the
        // executable. The object is a temporary beside the output and is
        // removed whether the link succeeds or fails.
        const std::string objectPath =
            output + (target.os == Os::Windows ? ".oskel.tmp.obj" : ".oskel.tmp.o");
        if (!writeBinaryFile(objectPath, object)) {
          std::string message =
              tool + ": cannot write temporary object '" + objectPath + "'\n";
          platformPosix().writeErr(message.data(), message.size());
          return 74;
        }
        const int linkStatus = linkHostedObject(
            objectPath, output, target, tool, effectiveLinkLibs,
            effectiveLibraryPaths, linkObjects, runPaths);
        std::remove(objectPath.c_str());
        if (linkStatus == 0 && usesUi && target.os == Os::Windows) {
          copyUiRuntimeBeside(output);
        }
        return linkStatus;
      }
    } else if (target.os == Os::Windows) {
      image = buildStaticPe(target, chunk.code, {}, chunk.processEntry);
    } else if (target.os == Os::Freestanding) {
      image = buildBareMetalElf(target, chunk.code, chunk.processEntry);
    } else {
      image = buildStaticElf(target, chunk.code, {}, chunk.processEntry);
    }
  } catch (const LoweringError &error) {
    std::string message = error.what();
    message += '\n';
    platformPosix().writeErr(message.data(), message.size());
    return 65;
  } catch (const std::exception &error) {
    std::string message = tool + ": " + error.what() + "\n";
    platformPosix().writeErr(message.data(), message.size());
    return 65;
  }

  if (!writeBinaryFile(output, image)) {
    std::string message = tool + ": cannot write '" + output + "'\n";
    platformPosix().writeErr(message.data(), message.size());
    return 74;
  }
#ifndef _WIN32
  ::chmod(output.c_str(), 0755);
#endif
  return 0;
}

// Compiles `source` (resolving imports) and writes a native binary to `output`.
// The whole pipeline is the compiler's: lex, parse, compile to bytecode, then
// lower the bytecode to machine code and wrap it in a static executable. The
// emitted binary embeds only the AOT runtime, never the compiler, so it cannot
// `eval` (DEVELOPMENT.md §3, the GraalVM pitfall).
//
// The lowering half is `emitNativeImage`, shared with the `.oskc` path, so a
// binary built from a chunk is the same binary the source would have produced.
//
// Returns 0 on success, 65 for compile errors and 74 for a write failure.
int buildBinary(const std::string &source, const std::string &name,
                const std::string &output, const std::string &argv0,
                const Target &target, LinkMode mode, Linker linker,
                bool emitObject, bool autoHostedForUi,
                const std::vector<std::string> &linkLibs,
                const std::vector<std::string> &libraryPaths,
                const std::vector<std::string> &linkObjects,
                const std::string &tool) {
  std::vector<Token> tokens;
  try {
    tokens = lex(source);
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }

  StmtPtr program;
  std::vector<Warning> warnings;
  ModuleResolver resolver = resolverFor(name, argv0);
  auto enumScanner = makeEnumScanner(resolver);
  try {
    Parser parser(std::move(tokens), name);
    parser.setEnumScanner(enumScanner);
    program = parser.parseProgram();
    warnings = parser.warnings();
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }
  for (const Warning &warning : warnings) {
    printCompileWarning(name, source, warning, platformPosix());
  }

  Bytecode bytecode;
  Compiler compiler;
  // The backend emits its own `extern "C"` thunks, so the shapes it can honour
  // do not depend on how this engine was built. Validating against the build's
  // capabilities would reject a class-MEMORY struct that the backend marshals
  // correctly, on any engine without a host FFI library.
  compiler.setAbiCapabilities(ffi::kAotAbiCapabilities);
  compiler.setEnumScanner(enumScanner);
  try {
    compiler.setImportLoader(makeImportLoader(resolver));
    bytecode = compiler.compile(program);
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }
  for (const Warning &warning : compiler.warnings()) {
    printCompileWarning(name, source, warning, platformPosix());
  }

  return emitNativeImage(
      bytecode, output, target, mode, linker, emitObject, autoHostedForUi,
      uiRequirements(compiler.loadedModulePaths(), bytecode), linkLibs,
      libraryPaths, linkObjects, tool);
}

// Reads a `.oskc` image and prepares it for a destination.
//
// The file records the FFI capabilities it was validated against. Checking them
// against the destination *here*, before any lowering, is what keeps the format
// portable without lying: a chunk that marshals a struct by value travels in
// one file to every platform, and a target whose ABI cannot carry it says so
// once, with a message, instead of failing halfway through codegen or -- worse
// -- emitting a binary that mis-marshals. `destination` is `kAotAbiCapabilities`
// for the AOT backend and the full set for the VM, which has the host to fall
// back on.
//
// Returns true and fills `chunk` on success; prints a diagnostic and returns
// false otherwise.
bool loadBytecodeImage(const std::string &path, const ffi::AbiCapabilities &destination,
                       Bytecode &chunk, std::uint32_t &needs,
                       const std::string &tool) {
  std::string bytes;
  if (!readSourceFile(path, bytes)) {
    std::cerr << tool << ": cannot read file '" << path << "'\n";
    return false;
  }
  const std::vector<std::uint8_t> image(bytes.begin(), bytes.end());

  std::uint32_t recordedAbi = 0;
  try {
    recordedAbi = readBytecodeImageAbi(image);
  } catch (const BytecodeIoError &error) {
    std::cerr << tool << ": " << path << ": " << error.what() << '\n';
    return false;
  }

  const ffi::AbiCapabilities required = decodeAbiCapabilities(recordedAbi);
  if (!abiSatisfies(destination, required)) {
    std::string missing;
    const auto add = [&missing](const char *name) {
      if (!missing.empty()) missing += ", ";
      missing += name;
    };
    if (required.structByValue && !destination.structByValue) add("structs by value");
    if (required.memoryStructs && !destination.memoryStructs) add("class-MEMORY structs");
    if (required.variadic && !destination.variadic) add("C variadics");
    if (required.callbacks && !destination.callbacks) add("callbacks");
    std::cerr << tool << ": " << path
              << ": the bytecode needs FFI support this target lacks: " << missing
              << '\n';
    return false;
  }

  try {
    chunk = readBytecodeImage(image);
    needs = readBytecodeImageNeeds(image);
  } catch (const BytecodeIoError &error) {
    std::cerr << tool << ": " << path << ": " << error.what() << '\n';
    return false;
  }
  return true;
}

// Compiles an already loaded chunk to a native image.
//
// The chunk comes from a `.oskc` file, so the front end (lex, parse, compile)
// already ran -- possibly on another machine, in another OS. Everything from
// here on is `emitNativeImage`, the same code the source path uses, which is
// what makes the two outputs identical.
//
// Returns 0 on success, 65 for a lowering error, 66 for an unreadable or
// malformed file and 74 for a write failure.
int buildBinaryFromBytecode(const std::string &path, const std::string &output,
                            const Target &target, LinkMode mode, Linker linker,
                            bool emitObject,
                            const std::vector<std::string> &linkLibs,
                            const std::vector<std::string> &libraryPaths,
                            const std::vector<std::string> &linkObjects,
                            const std::string &tool) {
  Bytecode chunk;
  std::uint32_t needs = 0;
  if (!loadBytecodeImage(path, targetAbiCapabilities(target), chunk, needs, tool))
    return 66;
  return emitNativeImage(chunk, output, target, mode, linker, emitObject,
                         /*autoHostedForUi=*/true,
                         UiRequirements::fromBits(needs), linkLibs,
                         libraryPaths, linkObjects, tool);
}

// Compiles `source` to a `.oskc` image and writes it to `output`.
//
// The FFI surface is validated against `kAotAbiCapabilities`, not against the
// host's macros. That is the whole point of the format: a `.oskc` must be the
// same file whichever machine wrote it, so the validation cannot consult
// `kStructByValueSupported` (a compile-time property of the *writing* host).
// Pinning the superset means a chunk that mentions a struct by value is written
// everywhere, and a destination whose ABI cannot carry it refuses it at load
// time with a message (`buildBinaryFromBytecode`), which is where the decision
// belongs.
//
// Returns 0 on success, 65 for a compile error and 74 for a write failure.
int emitBytecodeFile(const std::string &source, const std::string &name,
                     const std::string &output, const std::string &argv0,
                     const std::string &tool) {
  std::vector<Token> tokens;
  try {
    tokens = lex(source);
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }

  StmtPtr program;
  std::vector<Warning> warnings;
  ModuleResolver resolver = resolverFor(name, argv0);
  auto enumScanner = makeEnumScanner(resolver);
  try {
    Parser parser(std::move(tokens), name);
    parser.setEnumScanner(enumScanner);
    program = parser.parseProgram();
    warnings = parser.warnings();
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }
  for (const Warning &warning : warnings) {
    printCompileWarning(name, source, warning, platformPosix());
  }

  Bytecode bytecode;
  Compiler compiler;
  compiler.setAbiCapabilities(ffi::kAotAbiCapabilities);
  compiler.setEnumScanner(enumScanner);
  try {
    compiler.setImportLoader(makeImportLoader(resolver));
    bytecode = compiler.compile(program);
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }
  for (const Warning &warning : compiler.warnings()) {
    printCompileWarning(name, source, warning, platformPosix());
  }

  std::vector<std::uint8_t> image;
  try {
    image = writeBytecodeImage(
        bytecode, uiRequirements(compiler.loadedModulePaths(), bytecode).bits());
  } catch (const BytecodeIoError &error) {
    std::string message = tool + ": " + error.what() + "\n";
    platformPosix().writeErr(message.data(), message.size());
    return 65;
  }

  if (!writeBinaryFile(output, image)) {
    std::string message = tool + ": cannot write '" + output + "'\n";
    platformPosix().writeErr(message.data(), message.size());
    return 74;
  }
  return 0;
}

// Parses `<file.oskl> [-o <out>]` and writes a `.oskc` image. The default output
// replaces the source extension with `.oskc`, so `oskelc -c app.oskl` writes
// `app.oskc` beside it.
int runBytecodeCommand(const std::vector<std::string> &arguments,
                       const std::string &argv0, const std::string &tool,
                       const std::string &usage) {
  std::string file;
  std::string output;
  for (size_t i = 0; i < arguments.size(); ++i) {
    const std::string &arg = arguments[i];
    if (arg == "-o" || arg == "--output") {
      if (i + 1 >= arguments.size()) {
        std::cerr << tool << ": option '" << arg << "' needs an argument\n";
        return 64;
      }
      output = arguments[++i];
      continue;
    }
    if (arg == "-c" || arg == "--check" || arg == "--bytecode") continue;
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << tool << ": unknown option '" << arg << "'\n" << usage;
      return 64;
    }
    if (!file.empty()) {
      std::cerr << tool << ": more than one input file\n" << usage;
      return 64;
    }
    file = arg;
  }
  if (file.empty()) {
    std::cerr << tool << ": no input file\n" << usage;
    return 64;
  }
  if (output.empty()) {
    output = stripExtension(file) + ".oskc";
  }
  std::string source;
  if (!readSourceFile(file, source)) {
    std::cerr << tool << ": cannot read file '" << file << "'\n";
    return 66;
  }
  return emitBytecodeFile(source, file, output, argv0, tool);
}

// Parses `<file.oskl> [-o <out>] [--target T] [--mode M] [--link-lib L]` and
// writes the binary. Both entry points call this, so `oskel build` and
// `oskelc` cannot drift apart in their flags, their defaults or their exit
// codes.
int runBuildCommand(const std::vector<std::string> &arguments,
                    const std::string &argv0, const std::string &tool,
                    const std::string &usage) {
  std::string buildFile;
  std::string buildOutput;
  Target buildTarget = hostTarget();
  LinkMode buildMode = LinkMode::Freestanding;
  bool buildModeExplicit = false;
  Linker buildLinker = Linker::Host;
  bool buildEmitObject = false;
  std::vector<std::string> buildLinkLibs;
  std::vector<std::string> buildLibraryPaths;
  std::vector<std::string> buildLinkObjects;
  for (size_t i = 0; i < arguments.size(); ++i) {
    const std::string &arg = arguments[i];
    if (arg == "-o" || arg == "--output") {
      if (i + 1 >= arguments.size()) {
        std::cerr << tool << ": option '" << arg << "' needs an argument\n";
        return 64;
      }
      buildOutput = arguments[++i];
      continue;
    }
    if (arg == "--target") {
      if (i + 1 >= arguments.size()) {
        std::cerr << tool << ": option '" << arg << "' needs an argument\n";
        return 64;
      }
      const std::string name = arguments[++i];
      if (!parseTargetName(name, buildTarget)) {
        std::cerr << tool << ": unknown target '" << name << "' (supported: "
                  << targetName(Target::linuxX64()) << ", "
                  << targetName(Target::winX64()) << ", "
                  << targetName(Target::macX64()) << ", "
                  << targetName(Target::bareMetalX64()) << ", "
                  << targetName(Target::wasm32()) << ")\n";
        return 64;
      }
      continue;
    }
    if (arg == "--mode") {
      if (i + 1 >= arguments.size()) {
        std::cerr << tool << ": option '" << arg << "' needs an argument\n";
        return 64;
      }
      const std::string name = arguments[++i];
      if (!parseLinkModeName(name, buildMode)) {
        std::cerr << tool << ": unknown mode '" << name << "' (supported: "
                  << linkModeName(LinkMode::Freestanding) << ", "
                  << linkModeName(LinkMode::Hosted) << ")\n";
        return 64;
      }
      buildModeExplicit = true;
      continue;
    }
    if (arg == "--link-lib") {
      if (i + 1 >= arguments.size()) {
        std::cerr << tool << ": option '" << arg << "' needs an argument\n";
        return 64;
      }
      const std::string name = arguments[++i];
      if (!isSafeLinkLibName(name)) {
        std::cerr << tool << ": invalid library name '" << name
                  << "' (letters, digits, '_', '+', '-', '.' only)\n";
        return 64;
      }
      buildLinkLibs.push_back(name);
      continue;
    }
    if (arg == "-L" || arg == "--library-path") {
      if (i + 1 >= arguments.size()) {
        std::cerr << tool << ": option '" << arg << "' needs an argument\n";
        return 64;
      }
      buildLibraryPaths.push_back(arguments[++i]);
      continue;
    }
    if (arg == "--link-object") {
      if (i + 1 >= arguments.size()) {
        std::cerr << tool << ": option '" << arg << "' needs an argument\n";
        return 64;
      }
      buildLinkObjects.push_back(arguments[++i]);
      continue;
    }
    if (arg == "--linker") {
      if (i + 1 >= arguments.size()) {
        std::cerr << tool << ": option '" << arg << "' needs an argument\n";
        return 64;
      }
      const std::string name = arguments[++i];
      if (!parseLinkerName(name, buildLinker)) {
        std::cerr << tool << ": unknown linker '" << name << "' (supported: "
                  << linkerName(Linker::Host) << ", " << linkerName(Linker::Own)
                  << ")\n";
        return 64;
      }
      continue;
    }
    if (arg == "--emit-object") {
      buildEmitObject = true;
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      std::cout << "Oskel " << kVersion << "\n\n" << usage;
      return 0;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << tool << ": unknown option '" << arg << "'\n";
      return 64;
    }
    if (!buildFile.empty()) {
      std::cerr << tool << ": a single source file is expected\n";
      return 64;
    }
    buildFile = arg;
  }
  if (buildFile.empty()) {
    std::cerr << tool << ": a source file is required\n";
    return 64;
  }
  // macOS has no freestanding image, so its default is the mode it can close.
  // An explicit `--mode freestanding` still reaches the support check and is
  // reported there instead of being silently replaced.
  if (!buildModeExplicit && buildTarget.os == Os::MacOs &&
      !isWasmTarget(buildTarget)) {
    buildMode = LinkMode::Hosted;
  }
  // `--link-lib` is a hosted-mode concept: the freestanding image has no host
  // linker, so accepting it there would silently ignore a flag the user needs.
  if (!buildLinkLibs.empty() && buildMode != LinkMode::Hosted) {
    std::cerr << tool << ": '--link-lib' requires '--mode "
              << linkModeName(LinkMode::Hosted) << "'\n";
    return 64;
  }
  if (!buildLibraryPaths.empty() && buildMode != LinkMode::Hosted) {
    std::cerr << tool << ": '-L' requires '--mode "
              << linkModeName(LinkMode::Hosted) << "'\n";
    return 64;
  }
  if (!buildLinkObjects.empty() && buildMode != LinkMode::Hosted) {
    std::cerr << tool << ": '--link-object' requires '--mode "
              << linkModeName(LinkMode::Hosted) << "'\n";
    return 64;
  }
  if (!buildLinkObjects.empty() && buildEmitObject) {
    std::cerr << tool << ": '--link-object' and '--emit-object' are mutually "
                 "exclusive (use 'oskelld -r' to combine objects)\n";
    return 64;
  }
  // `--emit-object` writes the relocatable object a linker consumes, so it
  // needs a mode that produces one. The freestanding image is closed by the
  // emitter and WebAssembly has no object format here.
  if (buildEmitObject && isWasmTarget(buildTarget)) {
    std::cerr << tool << ": '--emit-object' is not available for the WebAssembly target\n";
    return 64;
  }
  if (buildEmitObject && buildMode != LinkMode::Hosted) {
    std::cerr << tool << ": '--emit-object' requires '--mode "
              << linkModeName(LinkMode::Hosted) << "'\n";
    return 64;
  }
  if (buildLinker == Linker::Own) {
    if (buildEmitObject) {
      std::cerr << tool << ": '--linker own' and '--emit-object' are mutually exclusive\n";
      return 64;
    }
    if (buildMode != LinkMode::Hosted) {
      std::cerr << tool << ": '--linker own' requires '--mode "
                << linkModeName(LinkMode::Hosted) << "'\n";
      return 64;
    }
    if (!isOwnLinkerTarget(buildTarget)) {
      std::cerr << tool << ": '--linker own' supports "
                << targetName(Target::linuxX64()) << " and "
                << targetName(Target::winX64()) << " (PHASE 10.1/10.3)\n";
      return 64;
    }
  }
  if (isWasmTarget(buildTarget) && buildMode != LinkMode::Freestanding) {
    std::cerr << tool << ": the WebAssembly target supports only '--mode "
              << linkModeName(LinkMode::Freestanding) << "'\n";
    return 64;
  }
  if (buildOutput.empty()) {
    buildOutput = stripExtension(buildFile);
    // The output name follows the target: a Windows image is an `.exe`.
    if (buildTarget.os == Os::Windows) buildOutput += ".exe";
    if (isWasmTarget(buildTarget)) buildOutput += ".wasm";
  }
  // A `.oskc` argument skips the front end entirely: the file is the compiled
  // form of a program, so `oskel build app.oskc` and `oskelc app.oskc` lower it
  // exactly as the source would have been lowered. This is the "compile once,
  // use anywhere" path -- the file written on one machine builds on another.
  if (isBytecodeImagePath(buildFile)) {
    return buildBinaryFromBytecode(buildFile, buildOutput, buildTarget, buildMode,
                                   buildLinker, buildEmitObject, buildLinkLibs,
                                   buildLibraryPaths, buildLinkObjects, tool);
  }
  std::string buildSource;
  if (!readSourceFile(buildFile, buildSource)) {
    std::cerr << tool << ": cannot read file '" << buildFile << "'\n";
    return 66;
  }
  return buildBinary(buildSource, buildFile, buildOutput, argv0, buildTarget,
                     buildMode, buildLinker, buildEmitObject,
                     !buildModeExplicit, buildLinkLibs, buildLibraryPaths,
                     buildLinkObjects, tool);
}
