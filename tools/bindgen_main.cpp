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
#include "cxxbind.h"
#include "version.h"

namespace {

const char* kUsage =
    "usage:\n"
    "  bindgen <header.h>                 write the Oskel bindings to stdout\n"
    "  bindgen <header.h> -o <out.oskl>   write them to a file\n"
    "  bindgen --cxx <bridge.oskelbind> --shim <dir> -o <out.oskl>\n"
    "                                     generate a C++ bridge (module + shim)\n"
    "\n"
    "options:\n"
    "  -h, --help      show this help\n"
    "  -v, --version   show the version\n"
    "  -o, --output    output path (default: stdout)\n"
    "  --target <model>  C data model for long/wchar_t:\n"
    "                    lp64 (default) or llp64 (Windows)\n"
    "  --cxx           read a C++ bridge descriptor instead of a C header\n"
    "  --shim <dir>    directory for <stem>_shim.h and <stem>_shim.cpp\n";

std::string stemOf(const std::string& path) {
  std::string s = path;
  const std::size_t slash = s.find_last_of("/\\");
  if (slash != std::string::npos) s = s.substr(slash + 1);
  const std::size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  return s.empty() ? "bridge" : s;
}

bool writeText(const std::string& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file << text;
  return static_cast<bool>(file);
}

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
  std::string shimDir;
  bool cxx = false;
  bool targetSet = false;
  bindgen::Options options;
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
    if (arg == "--cxx") {
      cxx = true;
      continue;
    }
    if (arg == "--shim") {
      if (i + 1 >= arguments.size()) {
        std::cerr << "bindgen: option '" << arg << "' needs an argument\n";
        return 64;
      }
      shimDir = arguments[++i];
      continue;
    }
    if (arg == "--target") {
      if (i + 1 >= arguments.size()) {
        std::cerr << "bindgen: option '" << arg << "' needs an argument\n";
        return 64;
      }
      targetSet = true;
      const std::string target = arguments[++i];
      if (target == "lp64") {
        options.target = bindgen::Target::Lp64;
      } else if (target == "llp64" || target == "windows") {
        options.target = bindgen::Target::Llp64;
      } else {
        std::cerr << "bindgen: unknown target '" << target
                  << "' (expected lp64 or llp64)\n";
        return 64;
      }
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
    std::cerr << "bindgen: no input given\n\n" << kUsage;
    return 64;
  }
  if (cxx && shimDir.empty()) {
    std::cerr << "bindgen: --cxx needs --shim <dir>\n";
    return 64;
  }
  if (!cxx && !shimDir.empty()) {
    std::cerr << "bindgen: --shim is only valid with --cxx\n";
    return 64;
  }
  if (cxx && targetSet) {
    std::cerr << "bindgen: --target is only valid for C headers\n";
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

  if (cxx) {
    const cxxbind::Result result = cxxbind::generate(source, input);
    if (!result.error.empty()) {
      std::cerr << "bindgen: " << result.error << "\n";
      return 65;
    }
    if (output.empty()) {
      std::cout << result.text;
      if (!std::cout) {
        std::cerr << "bindgen: cannot write to stdout\n";
        return 66;
      }
    } else if (!writeText(output, result.text)) {
      std::cerr << "bindgen: cannot write file '" << output << "'\n";
      return 66;
    }
    const std::string stem = stemOf(input);
    const std::string headerPath = shimDir + "/" + stem + "_shim.h";
    const std::string sourcePath = shimDir + "/" + stem + "_shim.cpp";
    if (!writeText(headerPath, result.shimHeader)) {
      std::cerr << "bindgen: cannot write file '" << headerPath << "'\n";
      return 66;
    }
    if (!writeText(sourcePath, result.shimSource)) {
      std::cerr << "bindgen: cannot write file '" << sourcePath << "'\n";
      return 66;
    }
    std::cerr << "bindgen: generated " << result.classes << " class(es), "
              << result.methods << " method(s) and " << result.functions
              << " function(s); shim " << sourcePath << "\n";
    return 0;
  }

  const bindgen::Result result = bindgen::generate(source, input, options);

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
    if (result.macroConstants > 0) {
      std::cerr << ", " << result.macroConstants << " macro constant(s)";
    }
    std::cerr << ", " << result.commented
              << " commented out (this build cannot marshal it)";
    if (result.skipped > 0) {
      std::cerr << ", skipped " << result.skipped << " (see the '// skipped:' comments)";
    }
    std::cerr << "\n";
  }
  return 0;
}
