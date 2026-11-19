// The `bindgen` command-line tool: a C header in, an Oskel `extern "C"` module
// out. It is a standalone executable (it does not link the engine's runtime),
// so it can be built and shipped on its own.
//
//   bindgen <header.h>              write the generated module to stdout
//   bindgen <header.h> -o out.oskl  write it to a file
//
// The generator itself lives in `bindgen.cpp` (`bindgen::generate`); this file
// is only the CLI: argument parsing, file I/O and the exit codes the rest of
// the toolchain uses (64 for a usage error, 66 for an unreadable input).

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "bindgen.h"
#include "core/util.h"
#include "version.h"

namespace {

const char* kUsage =
    "usage:\n"
    "  bindgen <header.h>                 write the Oskel bindings to stdout\n"
    "  bindgen <header.h> -o <out.oskl>   write them to a file\n"
    "\n"
    "options:\n"
    "  -h, --help      show this help\n"
    "  -v, --version   show the version\n"
    "  -o, --output    output path (default: stdout)\n";

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // The module is compared byte for byte against the `-o` file (which is
  // opened in binary mode) and may be piped into another tool, so stdout must
  // not translate '\n' into "\r\n".
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  std::vector<std::string> arguments(argv + 1, argv + argc);

  std::string input;
  std::string output;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "bindgen " << OSKEL_VERSION << "\n\n" << kUsage;
      return 0;
    }
    if (arg == "-v" || arg == "--version") {
      std::cout << "bindgen " << OSKEL_VERSION << '\n';
      return 0;
    }
    if (arg == "-o" || arg == "--output") {
      if (i + 1 >= arguments.size()) {
        std::cerr << "bindgen: option '" << arg << "' needs an argument\n";
        return 64;
      }
      output = arguments[++i];
      continue;
    }
    if (!arg.empty() && arg[0] == '-' && arg != "-") {
      std::cerr << "bindgen: unknown option '" << arg << "'\n";
      return 64;
    }
    if (!input.empty()) {
      std::cerr << "bindgen: accepts a single header\n";
      return 64;
    }
    input = arg;
  }

  if (input.empty()) {
    std::cerr << "bindgen: no header given\n\n" << kUsage;
    return 64;
  }

  std::string source;
  if (input == "-") {
    // `-` reads the header from stdin, so the tool composes with a pipeline.
    source.assign(std::istreambuf_iterator<char>(std::cin),
                  std::istreambuf_iterator<char>());
  } else if (!readSourceFile(input, source)) {
    std::cerr << "bindgen: cannot read file '" << input << "'\n";
    return 66;
  }

  const bindgen::Result result = bindgen::generate(source, input);

  if (output.empty()) {
    std::cout << result.text;
    if (!std::cout) {
      std::cerr << "bindgen: cannot write to stdout\n";
      return 66;
    }
  } else {
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file) {
      std::cerr << "bindgen: cannot write file '" << output << "'\n";
      return 66;
    }
    file << result.text;
    if (!file) {
      std::cerr << "bindgen: cannot write file '" << output << "'\n";
      return 66;
    }
  }

  // A skipped declaration is not an error (the type is outside the FFI
  // vocabulary), but it must be visible, so the caller knows the output is
  // incomplete. The count goes to stderr to keep stdout a clean module.
  if (result.skipped > 0 || result.commented > 0) {
    std::cerr << "bindgen: generated " << result.emitted << " declaration(s)";
    if (result.structs > 0) std::cerr << ", " << result.structs << " struct(s)";
    if (result.constants > 0) std::cerr << ", " << result.constants << " enum constant(s)";
    if (result.commented > 0) {
      std::cerr << ", " << result.commented
                << " commented out (this build cannot marshal it)";
    }
    if (result.skipped > 0) {
      std::cerr << ", skipped " << result.skipped << " (see the '// skipped:' comments)";
    }
    std::cerr << "\n";
  }
  return 0;
}
