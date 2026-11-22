// `oskelc`: the host compiler (DEVELOPMENT.md §2).
//
// Sources in, a native image out. It links the lexer, the parser, the bytecode
// compiler and the AOT backend, and never the VM or the JIT, so the shipped
// compiler carries no interpreter. The pipeline itself is `buildBinary` in
// `src/frontend/cli.cpp`, the same function `oskel build` calls, so the two cannot
// diverge in what they emit.

#include <iostream>
#include <string>
#include <vector>

#include "frontend/cli_internal.h"
#include "native/target.h"
#include "version.h"

namespace {

const char *kUsage =
    "usage:\n"
    "  oskelc <file.oskl> [-o <out>]   compile to a native binary (AOT)\n"
    "\n"
    "options:\n"
    "  -h, --help                show this help\n"
    "  -v, --version             show the version\n"
    "  -o, --output <file>       output path (default: the script's base name)\n"
    "  --target <name>           build target (default: the host);\n"
    "                            x86_64-linux-sysv, x86_64-windows-win64,\n"
    "                            x86_64-freestanding-sysv (bare metal) or\n"
    "                            wasm32-freestanding-wasm (WebAssembly)\n"
    "  --mode <name>             link mode (default: freestanding);\n"
    "                            freestanding (self-contained) or hosted (links libc)\n"
    "  --link-lib <name>         library for the hosted link (repeatable), e.g. SDL3\n"
    "  -L, --library-path <dir>  where the hosted link finds lib<name>.so (repeatable)\n"
    "  --linker <name>           how a hosted build is closed: host (default, $CC)\n"
    "                            or own (the project's linker, x86_64-linux-sysv)\n"
    "  --emit-object             write the hosted relocatable object (input of oskelld)\n"
    "\n"
    "`oskel <file.oskl>` is the development engine that runs the same sources\n"
    "on the VM.\n";

} // namespace

int main(int argc, char **argv) {
  std::vector<std::string> arguments(argv + 1, argv + argc);

  for (const std::string &arg : arguments) {
    if (arg == "-v" || arg == "--version") {
      std::cout << "Oskel " << kVersion << '\n';
      return 0;
    }
    // `-c` (a bytecode image) and `-pkg` (a package) are planned and have no
    // serializer yet, so they are refused by name instead of as a typo.
    if (arg == "-c" || arg == "--check" || arg == "-pkg") {
      std::cerr << "oskelc: '" << arg
                << "' is not implemented yet; this compiler emits native binaries\n";
      return 64;
    }
  }

  return runBuildCommand(arguments, argv[0], "oskelc", kUsage);
}
