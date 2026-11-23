// The `oskelint` command-line tool: lint an Oskel source file.
//
//   oskelint <file.oskl>     report the diagnostics on stdout
//   oskelint --list-rules    print the rule names, one per line
//
// The lint itself lives in `oskelint.cpp` (`oskelint::lint`); this file is only
// the CLI: argument parsing, file I/O and the exit codes the rest of the
// toolchain uses (64 for a usage error, 65 for a file the compiler rejects, 66
// for an unreadable input). A file with diagnostics exits 0: a lint warning is
// not an error, the same way the compiler's own warnings do not fail a build.

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "oskelint.h"
#include "version.h"

namespace {

const char* kUsage =
    "usage:\n"
    "  oskelint <file.oskl>     lint one file\n"
    "  oskelint --list-rules    list the rule names\n"
    "\n"
    "options:\n"
    "  -h, --help      show this help\n"
    "  -v, --version   show the version\n";

bool readText(const std::string& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "oskelint " << OSKEL_VERSION << "\n\n" << kUsage;
      return 0;
    }
    if (arg == "-v" || arg == "--version") {
      std::cout << "oskelint " << OSKEL_VERSION << "\n";
      return 0;
    }
    if (arg == "--list-rules") {
      std::cout << "builtin-shadow\n";
      return 0;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "oskelint: unknown option '" << arg << "'\n\n" << kUsage;
      return 64;
    }
    files.push_back(arg);
  }
  if (files.empty()) {
    std::cerr << "oskelint: no input given\n\n" << kUsage;
    return 64;
  }

  int exitCode = 0;
  for (const std::string& path : files) {
    std::string source;
    if (!readText(path, source)) {
      std::cerr << "oskelint: cannot read file '" << path << "'\n";
      exitCode = 66;
      continue;
    }
    std::string parseError;
    const std::vector<oskelint::Diagnostic> diagnostics = oskelint::lint(source, path, parseError);
    if (!parseError.empty()) {
      // The compiler's own message, unmodified: the linter does not re-word a
      // syntax error it did not detect.
      std::cerr << path << ": " << parseError << "\n";
      exitCode = 65;
      continue;
    }
    for (const oskelint::Diagnostic& diagnostic : diagnostics) {
      std::cout << oskelint::format(diagnostic, path) << "\n";
    }
  }
  return exitCode;
}
