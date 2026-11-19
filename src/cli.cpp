#include "cli_internal.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "ast.h"
#include "compiler.h"
#include "diagnostic.h"
#include "errors.h"
#include "lexer.h"
#include "module_resolver.h"
#include "native/bare_metal.h"
#include "native/codegen.h"
#include "native/elf.h"
#include "native/lower.h"
#include "native/object.h"
#include "native/pe.h"
#include "native/target.h"
#include "parser.h"
#include "platform.h"
#include "util.h"
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
ModuleResolver resolverFor(const std::string &name, const std::string &argv0) {
  bool isFile = !name.empty() && name[0] != '<';
  std::string projectDir = isFile ? directoryOf(name) : std::string(".");
  return ModuleResolver(projectDir, standardLibraryDir(argv0), packagesDir());
}

// Builds the module loader the compiler uses for `import` declarations. It
// resolves the dotted path to a file, reads and parses it, and returns the
// program. A path that cannot be resolved becomes the compile error
// "module 'X' not found"; a parse error inside the module is reported with the
// module's own file name in the message.
Compiler::ImportLoader makeImportLoader(ModuleResolver resolver) {
  // The loader owns its resolver: it outlives the statement that built it.
  return [resolver](const std::string &path, int line, int col) -> StmtPtr {
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
  if (name == targetName(Target::bareMetalX64())) {
    out = Target::bareMetalX64();
    return true;
  }
  return false;
}

Target hostTarget() {
#ifdef _WIN32
  return Target::winX64();
#else
  return Target::linuxX64();
#endif
}

// The link modes `--mode` accepts, keyed by the name `linkModeName()` prints.
// The default is `freestanding`, the mode every target supports and the one
// that keeps the "not a dynamic executable" invariant, so an existing command
// line keeps producing the same binary.
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

// The C compiler the hosted link drives (PHASE 7f.4). It is a *build-time*
// dependency, never a runtime one: the compiler turns the relocatable object
// into an executable by linking it against the host C library, which is what
// lets an `extern "C"` symbol resolve. `$CC` overrides it (so a musl or
// cross toolchain can be selected), falling back to `cc`.
std::string hostedLinkerCommand() {
  if (const char *cc = std::getenv("CC")) {
    if (*cc != '\0') return cc;
  }
  return "cc";
}

// Quotes a path for a POSIX shell (the hosted link only runs on Linux today).
// A single quote is the only character a single-quoted string cannot contain,
// so each one is closed, escaped and reopened.
std::string shellQuote(const std::string &path) {
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
}

// Links a relocatable object into a hosted executable by invoking the host C
// compiler (PHASE 7f.4). `objectPath` is the `.o` just written and `output` the
// requested executable; `-no-pie` keeps the fixed-address assumptions of the
// backend and `-lm` provides the libm symbols a `std.math` extern may name.
// Returns 0 on success, 65 when the toolchain is missing or the link fails --
// never a half-linked image.
int linkHostedObject(const std::string &objectPath, const std::string &output,
                     const std::string &tool) {
  const std::string command = hostedLinkerCommand() + " " + shellQuote(objectPath) +
                              " -no-pie -o " + shellQuote(output) + " -lm";
  const int status = std::system(command.c_str());
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
// Returns 0 on success, 65 for compile errors and 74 for a write failure.
int buildBinary(const std::string &source, const std::string &name,
                const std::string &output, const std::string &argv0,
                const Target &target, LinkMode mode, const std::string &tool) {
  std::vector<Token> tokens;
  try {
    tokens = lex(source);
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }

  StmtPtr program;
  std::vector<Warning> warnings;
  try {
    Parser parser(std::move(tokens), name);
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
  try {
    compiler.setImportLoader(makeImportLoader(resolverFor(name, argv0)));
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
    requireSupportedLinkMode(target, mode);
    LoweredChunk chunk = lowerToX64(target, bytecode, CodeGen{}, mode);
    if (mode == LinkMode::Hosted) {
      // The hosted image is an object the linker closes against libc (PHASE
      // 7f.4). The object emitter turns the code buffer's external references
      // into relocations; `cc` resolves them and writes the executable itself,
      // so there is no in-memory image to write here. The object is a temporary
      // beside the output and is removed whether the link succeeds or fails.
      std::vector<std::uint8_t> object = buildRelocatableObject(
          target, chunk.code, {}, chunk.externals, chunk.exports);
      const std::string objectPath = output + ".oskel.tmp.o";
      if (!writeBinaryFile(objectPath, object)) {
        std::string message = tool + ": cannot write temporary object '" + objectPath + "'\n";
        platformPosix().writeErr(message.data(), message.size());
        return 74;
      }
      const int linkStatus = linkHostedObject(objectPath, output, tool);
      std::remove(objectPath.c_str());
      return linkStatus;
    }
    if (target.os == Os::Windows) {
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

// Parses `<file.oskl> [-o <out>] [--target T] [--mode M]` and writes the
// binary. Both entry points call this, so `oskel build` and `oskelc` cannot
// drift apart in their flags, their defaults or their exit codes.
int runBuildCommand(const std::vector<std::string> &arguments,
                    const std::string &argv0, const std::string &tool,
                    const std::string &usage) {
  std::string buildFile;
  std::string buildOutput;
  Target buildTarget = hostTarget();
  LinkMode buildMode = LinkMode::Freestanding;
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
                  << targetName(Target::bareMetalX64()) << ")\n";
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
  if (buildOutput.empty()) {
    buildOutput = stripExtension(buildFile);
    // The output name follows the target: a Windows image is an `.exe`.
    if (buildTarget.os == Os::Windows) buildOutput += ".exe";
  }
  std::string buildSource;
  if (!readSourceFile(buildFile, buildSource)) {
    std::cerr << tool << ": cannot read file '" << buildFile << "'\n";
    return 66;
  }
  return buildBinary(buildSource, buildFile, buildOutput, argv0, buildTarget,
                     buildMode, tool);
}
