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
    "  oskelc -c <file.oskl> [-o <out>]  compile to a portable .oskc image\n"
    "  oskelc <file.oskc> [-o <out>]   build a native binary from a .oskc image\n"
    "\n"
    "options:\n"
    "  -h, --help                show this help\n"
    "  -v, --version             show the version\n"
    "  -c, --bytecode            write a .oskc bytecode image instead of a binary\n"
    "  -o, --output <file>       output path (default: the script's base name,\n"
    "                            or its base name plus .oskc under -c)\n"
    "  --target <name>           build target (default: the host);\n"
    "                            x86_64-linux-sysv, x86_64-windows-win64,\n"
    "                            x86_64-freestanding-sysv (bare metal) or\n"
    "                            wasm32-freestanding-wasm (WebAssembly)\n"
    "  --mode <name>             link mode (default: freestanding);\n"
    "                            freestanding (self-contained) or hosted (links libc)\n"
    "  --link-lib <name>         library for the hosted link (repeatable), e.g. SDL3\n"
    "  -L, --library-path <dir>  where the hosted link finds lib<name>.so (repeatable)\n"
    "  --link-object <file>      extra object or ar archive for the hosted link (repeatable)\n"
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
  }

  // `-c` selects the bytecode image: the front end runs and the image is
  // written, with no lowering. It is the "compile once" half of the format;
  // `oskelc <file.oskc>` is the "use anywhere" half.
  for (const std::string &arg : arguments) {
    if (arg == "-c" || arg == "--bytecode") {
      return runBytecodeCommand(arguments, argv[0], "oskelc", kUsage);
    }
    // `-pkg` (a package) still has no serializer, so it is refused by name
    // instead of being read as a typo.
    if (arg == "-pkg" || arg == "--package") {
      std::cerr << "oskelc: '" << arg
                << "' is not implemented yet\n";
      return 64;
    }
  }

  return runBuildCommand(arguments, argv[0], "oskelc", kUsage);
}
