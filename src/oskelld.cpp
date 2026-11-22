// `oskelld`: the project's own linker (PHASE 10).
//
// The core lives in `src/frontend/native/link.cpp`, inside `oskel_frontend`, so
// this binary, `oskel build --linker own` and `oskelc --linker own` are three
// entry points over one implementation instead of three linkers that drift.
// `oskelld` takes the objects `oskel build --emit-object` writes (and `ar`
// archives of them) and closes them into a dynamically linked x86-64 Linux
// executable, or combines them into a new relocatable object with `-r`. It
// never runs the program, so it can cross-link from any host with a reachable
// target libc (`-L`).
//
// The macOS Mach-O and Windows PE containers are PHASE 10.3; an input this
// phase does not understand is reported, never mis-linked.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "core/util.h"
#include "frontend/cli_internal.h"
#include "native/link.h"
#include "native/target.h"

namespace {

const char *kUsage =
    "usage:\n"
    "  oskelld <input>... [-o <out>] [options]\n"
    "  link objects and `ar` archives into a hosted executable\n"
    "\n"
    "options:\n"
    "  -h, --help                 show this help\n"
    "  -v, --version              show the version\n"
    "  -o, --output <file>        output path (default: a.out)\n"
    "  -r, --relocatable          combine the inputs into a new ET_REL object\n"
    "  --target <name>            target (default: x86_64-linux-sysv)\n"
    "  --link-lib <name>          shared library to link (repeatable), e.g. SDL3\n"
    "  -L, --library-path <dir>   where to find lib<name>.so (repeatable)\n"
    "\n"
    "Inputs are ELF64 relocatable objects (`oskel build --mode hosted\n"
    "--emit-object`) or System V/GNU `ar` archives. An archive member is\n"
    "pulled in only when it defines a still-undefined symbol. `-r` leaves\n"
    "external symbols undefined, so `--link-lib`/`-L` apply only to an\n"
    "executable link.\n";

std::vector<std::uint8_t> readFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0) return {};
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  in.read(reinterpret_cast<char *>(bytes.data()), size);
  if (!in) return {};
  return bytes;
}

}  // namespace

int main(int argc, char **argv) {
  const std::vector<std::string> arguments(argv + 1, argv + argc);
  std::vector<LinkInput> inputs;
  std::string output = "a.out";
  bool relocatable = false;
  Target target = Target::linuxX64();
  LinkOptions options;
  options.target = target;

  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string &arg = arguments[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Oskel " << kVersion << "\n\n" << kUsage;
      return 0;
    }
    if (arg == "-v" || arg == "--version") {
      std::cout << "Oskel " << kVersion << '\n';
      return 0;
    }
    if (arg == "-o" || arg == "--output") {
      if (i + 1 >= arguments.size()) {
        std::cerr << "oskelld: option '" << arg << "' needs an argument\n";
        return 64;
      }
      output = arguments[++i];
      continue;
    }
    if (arg == "-r" || arg == "--relocatable") {
      relocatable = true;
      continue;
    }
    if (arg == "--target") {
      if (i + 1 >= arguments.size()) {
        std::cerr << "oskelld: option '" << arg << "' needs an argument\n";
        return 64;
      }
      const std::string name = arguments[++i];
      if (!parseTargetName(name, target)) {
        std::cerr << "oskelld: unknown target '" << name << "'\n";
        return 64;
      }
      if (!isOwnLinkerTarget(target)) {
        std::cerr << "oskelld: the own linker supports "
                  << targetName(Target::linuxX64()) << " only\n";
        return 64;
      }
      options.target = target;
      continue;
    }
    if (arg == "--link-lib") {
      if (i + 1 >= arguments.size()) {
        std::cerr << "oskelld: option '" << arg << "' needs an argument\n";
        return 64;
      }
      options.libraries.push_back(arguments[++i]);
      continue;
    }
    if (arg == "-L" || arg == "--library-path") {
      if (i + 1 >= arguments.size()) {
        std::cerr << "oskelld: option '" << arg << "' needs an argument\n";
        return 64;
      }
      options.libraryPaths.push_back(arguments[++i]);
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "oskelld: unknown option '" << arg << "'\n";
      return 64;
    }
    inputs.push_back({arg, readFile(arg)});
  }

  if (inputs.empty()) {
    std::cerr << "oskelld: at least one input is required\n";
    return 64;
  }
  for (const LinkInput &input : inputs) {
    if (input.bytes.empty()) {
      std::cerr << "oskelld: cannot read '" << input.name << "'\n";
      return 65;
    }
  }

  std::vector<std::uint8_t> image;
  try {
    image = relocatable ? linkRelocatable(inputs, options)
                        : linkHostedExecutable(inputs, options);
  } catch (const LinkError &error) {
    std::cerr << "oskelld: " << error.what() << '\n';
    return 65;
  } catch (const std::exception &error) {
    std::cerr << "oskelld: " << error.what() << '\n';
    return 65;
  }

  if (!writeBinaryFile(output, image)) {
    std::cerr << "oskelld: cannot write '" << output << "'\n";
    return 74;
  }
#ifndef _WIN32
  if (!relocatable) ::chmod(output.c_str(), 0755);
#endif
  return 0;
}
