// `oskelp`: the project and package manager for Oskel (DEVELOPMENT.md, tools).
//
// It is a standalone executable that drives `oskel`/`oskelc` as child
// processes; it links no engine code. The project model and the manifest format
// are documented in `docs/oskelp.md`.

#include <iostream>
#include <string>
#include <vector>

#include "oskelp.h"
#include "version.h"

namespace {

const char* kUsage =
    "usage:\n"
    "  oskelp new <dir> [--name <name>] [--vcs none|git]\n"
    "  oskelp init [dir] [--name <name>] [--vcs none|git]\n"
    "  oskelp add <name> [--version <requirement>]\n"
    "             [--manifest-path <file>]\n"
    "  oskelp add <name> --path <path> [--version <v>]\n"
    "             [--manifest-path <file>]\n"
    "  oskelp add --path <path> [--version <v>] [--manifest-path <file>]\n"
    "  oskelp remove <name> [--manifest-path <file>]\n"
    "  oskelp publish [--registry <dir>] [--manifest-path <file>]\n"
    "  oskelp yank <name> --version <version> [--undo] [--registry <dir>]\n"
    "  oskelp update [<name>] [--precise <version>]\n"
    "                [--manifest-path <file>]\n"
    "  oskelp outdated [--manifest-path <file>]\n"
    "  oskelp build [--target <name>] [--mode <name>] [--locked]\n"
    "               [--manifest-path <file>]\n"
    "  oskelp run [--native] [--target <name>] [--mode <name>] [--locked]\n"
    "             [--manifest-path <file>] [-- <args>...]\n"
    "  oskelp check [--locked] [--manifest-path <file>]\n"
    "  oskelp test [--native] [--target <name>] [--mode <name>] [--locked]\n"
    "              [--manifest-path <file>]\n"
    "  oskelp clean [--manifest-path <file>]\n"
    "  oskelp tree [--manifest-path <file>]\n"
    "\n"
    "options:\n"
    "  -h, --help      show this help\n"
    "  -v, --version   show the version\n"
    "\n"
    "`oskelp add` and `oskelp remove` edit oskel.toml in place and refresh\n"
    "oskel.lock and .oskel/deps. `add` takes `--path` for a path dependency or\n"
    "`--version <requirement>` for one from the registry named by\n"
    "OSKEL_REGISTRY; without `--version` it uses the latest published release\n"
    "and writes a caret requirement. `oskelp publish` copies the package to\n"
    "that registry; `oskelp yank` hides a published version from new\n"
    "selections without deleting it (an existing lockfile pin keeps working).\n"
    "`oskelp update` re-resolves every package, or only <name>,\n"
    "and rewrites oskel.lock; `--precise <version>` pins that registry package\n"
    "to one version. `oskelp outdated` prints the available registry updates.\n"
    "`oskelp build` compiles the project's entry point to a native binary;\n"
    "`oskelp run` runs it on the VM unless --native is given. Both resolve the\n"
    "dependencies in oskel.toml and materialise them under .oskel/deps.\n";

bool takeValue(const std::vector<std::string>& arguments, size_t& index,
               const std::string& option, std::string& value) {
  if (index + 1 >= arguments.size()) {
    std::cerr << "oskelp: option '" << option << "' needs an argument\n";
    return false;
  }
  value = arguments[++index];
  return true;
}

int resolveManifest(const std::string& explicitPath, std::string& path) {
  if (!explicitPath.empty()) {
    path = oskelp::absolutePath(explicitPath);
    return oskelp::kOk;
  }
  if (!oskelp::findManifest(".", path)) {
    std::cerr << "oskelp: error: could not find 'oskel.toml' in '"
              << oskelp::absolutePath(".")
              << "' or any parent directory\n";
    return oskelp::kIoError;
  }
  return oskelp::kOk;
}

int commandNew(const std::vector<std::string>& arguments,
               const std::string& argv0) {
  std::string directory;
  std::string name;
  std::string vcs = "none";
  for (size_t i = 1; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "oskelp " << OSKEL_VERSION << "\n\n" << kUsage;
      return oskelp::kOk;
    }
    if (arg == "--name") {
      if (!takeValue(arguments, i, arg, name)) return oskelp::kUsage;
      continue;
    }
    if (arg == "--vcs") {
      if (!takeValue(arguments, i, arg, vcs)) return oskelp::kUsage;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "oskelp new: unknown option '" << arg << "'\n";
      return oskelp::kUsage;
    }
    if (!directory.empty()) {
      std::cerr << "oskelp new: a single directory is expected\n";
      return oskelp::kUsage;
    }
    directory = arg;
  }
  if (directory.empty()) {
    std::cerr << "oskelp new: a directory is required\n";
    return oskelp::kUsage;
  }
  return oskelp::commandNew(directory, name, vcs, argv0);
}

int commandInit(const std::vector<std::string>& arguments,
                const std::string& argv0) {
  std::string directory;
  std::string name;
  std::string vcs = "none";
  for (size_t i = 1; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "oskelp " << OSKEL_VERSION << "\n\n" << kUsage;
      return oskelp::kOk;
    }
    if (arg == "--name") {
      if (!takeValue(arguments, i, arg, name)) return oskelp::kUsage;
      continue;
    }
    if (arg == "--vcs") {
      if (!takeValue(arguments, i, arg, vcs)) return oskelp::kUsage;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "oskelp init: unknown option '" << arg << "'\n";
      return oskelp::kUsage;
    }
    if (!directory.empty()) {
      std::cerr << "oskelp init: a single directory is expected\n";
      return oskelp::kUsage;
    }
    directory = arg;
  }
  return oskelp::commandInit(directory, name, vcs, argv0);
}

int commandAdd(const std::vector<std::string>& arguments) {
  std::string name;
  std::string path;
  std::string version;
  std::string manifestPath;
  for (size_t i = 1; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "oskelp " << OSKEL_VERSION << "\n\n" << kUsage;
      return oskelp::kOk;
    }
    if (arg == "--path") {
      if (!takeValue(arguments, i, arg, path)) return oskelp::kUsage;
      continue;
    }
    if (arg == "--version") {
      if (!takeValue(arguments, i, arg, version)) return oskelp::kUsage;
      continue;
    }
    if (arg == "--manifest-path") {
      if (!takeValue(arguments, i, arg, manifestPath)) return oskelp::kUsage;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "oskelp add: unknown option '" << arg << "'\n";
      return oskelp::kUsage;
    }
    if (!name.empty()) {
      std::cerr << "oskelp add: a single dependency name is expected\n";
      return oskelp::kUsage;
    }
    name = arg;
  }
  if (name.empty() && path.empty()) {
    std::cerr << "oskelp add: a dependency name or --path is required\n";
    return oskelp::kUsage;
  }
  std::string manifest;
  int status = resolveManifest(manifestPath, manifest);
  if (status != oskelp::kOk) return status;
  return oskelp::commandAdd(manifest, name, path, version);
}

int commandRemove(const std::vector<std::string>& arguments) {
  std::string name;
  std::string manifestPath;
  for (size_t i = 1; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "oskelp " << OSKEL_VERSION << "\n\n" << kUsage;
      return oskelp::kOk;
    }
    if (arg == "--manifest-path") {
      if (!takeValue(arguments, i, arg, manifestPath)) return oskelp::kUsage;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "oskelp remove: unknown option '" << arg << "'\n";
      return oskelp::kUsage;
    }
    if (!name.empty()) {
      std::cerr << "oskelp remove: a single dependency name is expected\n";
      return oskelp::kUsage;
    }
    name = arg;
  }
  if (name.empty()) {
    std::cerr << "oskelp remove: a dependency name is required\n";
    return oskelp::kUsage;
  }
  std::string manifest;
  int status = resolveManifest(manifestPath, manifest);
  if (status != oskelp::kOk) return status;
  return oskelp::commandRemove(manifest, name);
}

int commandPublish(const std::vector<std::string>& arguments) {
  std::string registry;
  std::string manifestPath;
  for (size_t i = 1; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "oskelp " << OSKEL_VERSION << "\n\n" << kUsage;
      return oskelp::kOk;
    }
    if (arg == "--registry") {
      if (!takeValue(arguments, i, arg, registry)) return oskelp::kUsage;
      continue;
    }
    if (arg == "--manifest-path") {
      if (!takeValue(arguments, i, arg, manifestPath)) return oskelp::kUsage;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "oskelp publish: unknown option '" << arg << "'\n";
      return oskelp::kUsage;
    }
    std::cerr << "oskelp publish: unexpected argument '" << arg << "'\n";
    return oskelp::kUsage;
  }
  std::string manifest;
  int status = resolveManifest(manifestPath, manifest);
  if (status != oskelp::kOk) return status;
  return oskelp::commandPublish(manifest, registry);
}

int commandYank(const std::vector<std::string>& arguments) {
  std::string name;
  std::string version;
  std::string registry;
  bool undo = false;
  for (size_t i = 1; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "oskelp " << OSKEL_VERSION << "\n\n" << kUsage;
      return oskelp::kOk;
    }
    if (arg == "--version") {
      if (!takeValue(arguments, i, arg, version)) return oskelp::kUsage;
      continue;
    }
    if (arg == "--registry") {
      if (!takeValue(arguments, i, arg, registry)) return oskelp::kUsage;
      continue;
    }
    if (arg == "--undo") {
      undo = true;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "oskelp yank: unknown option '" << arg << "'\n";
      return oskelp::kUsage;
    }
    if (!name.empty()) {
      std::cerr << "oskelp yank: a single package name is expected\n";
      return oskelp::kUsage;
    }
    name = arg;
  }
  if (name.empty()) {
    std::cerr << "oskelp yank: a package name is required\n";
    return oskelp::kUsage;
  }
  if (version.empty()) {
    std::cerr << "oskelp yank: --version <version> is required\n";
    return oskelp::kUsage;
  }
  return oskelp::commandYank(name, version, registry, undo);
}

int commandUpdate(const std::vector<std::string>& arguments) {
  std::string name;
  std::string precise;
  std::string manifestPath;
  for (size_t i = 1; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "oskelp " << OSKEL_VERSION << "\n\n" << kUsage;
      return oskelp::kOk;
    }
    if (arg == "--precise") {
      if (!takeValue(arguments, i, arg, precise)) return oskelp::kUsage;
      continue;
    }
    if (arg == "--manifest-path") {
      if (!takeValue(arguments, i, arg, manifestPath)) return oskelp::kUsage;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "oskelp update: unknown option '" << arg << "'\n";
      return oskelp::kUsage;
    }
    if (!name.empty()) {
      std::cerr << "oskelp update: a single package name is expected\n";
      return oskelp::kUsage;
    }
    name = arg;
  }
  std::string manifest;
  int status = resolveManifest(manifestPath, manifest);
  if (status != oskelp::kOk) return status;
  return oskelp::commandUpdate(manifest, name, precise);
}

int commandOutdated(const std::vector<std::string>& arguments) {
  std::string manifestPath;
  for (size_t i = 1; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "oskelp " << OSKEL_VERSION << "\n\n" << kUsage;
      return oskelp::kOk;
    }
    if (arg == "--manifest-path") {
      if (!takeValue(arguments, i, arg, manifestPath)) return oskelp::kUsage;
      continue;
    }
    std::cerr << "oskelp outdated: unexpected argument '" << arg << "'\n";
    return oskelp::kUsage;
  }
  std::string manifest;
  int status = resolveManifest(manifestPath, manifest);
  if (status != oskelp::kOk) return status;
  return oskelp::commandOutdated(manifest);
}

// Parses the options shared by build/run/check/test. `allowArguments` lets
// `run` collect the program's own arguments; every other command rejects a
// positional argument.
int parseProjectOptions(const std::vector<std::string>& arguments,
                        bool allowArguments, oskelp::BuildOptions& options,
                        std::string& manifestPath, bool& native,
                        std::vector<std::string>& programArguments,
                        const std::string& command) {
  native = false;
  bool afterSeparator = false;
  for (size_t i = 1; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];
    if (afterSeparator) {
      programArguments.push_back(arg);
      continue;
    }
    if (arg == "--") {
      if (!allowArguments) {
        std::cerr << "oskelp " << command << ": unexpected argument '" << arg
                  << "'\n";
        return oskelp::kUsage;
      }
      afterSeparator = true;
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      std::cout << "oskelp " << OSKEL_VERSION << "\n\n" << kUsage;
      return oskelp::kOk;
    }
    if (arg == "--native") {
      if (command != "run" && command != "test") {
        std::cerr << "oskelp " << command << ": unknown option '" << arg << "'\n";
        return oskelp::kUsage;
      }
      native = true;
      continue;
    }
    if (arg == "--locked") {
      options.locked = true;
      continue;
    }
    if (arg == "--target") {
      if (!takeValue(arguments, i, arg, options.target)) return oskelp::kUsage;
      continue;
    }
    if (arg == "--mode") {
      if (!takeValue(arguments, i, arg, options.mode)) return oskelp::kUsage;
      continue;
    }
    if (arg == "--manifest-path") {
      if (!takeValue(arguments, i, arg, manifestPath)) return oskelp::kUsage;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "oskelp " << command << ": unknown option '" << arg << "'\n";
      return oskelp::kUsage;
    }
    if (allowArguments) {
      programArguments.push_back(arg);
      continue;
    }
    std::cerr << "oskelp " << command << ": unexpected argument '" << arg
              << "'\n";
    return oskelp::kUsage;
  }
  return oskelp::kOk;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments(argv + 1, argv + argc);
  if (arguments.empty()) {
    std::cout << "oskelp " << OSKEL_VERSION << "\n\n" << kUsage;
    return oskelp::kUsage;
  }

  const std::string command = arguments[0];
  if (command == "-h" || command == "--help") {
    std::cout << "oskelp " << OSKEL_VERSION << "\n\n" << kUsage;
    return oskelp::kOk;
  }
  if (command == "-v" || command == "--version") {
    std::cout << "oskelp " << OSKEL_VERSION << '\n';
    return oskelp::kOk;
  }

  if (command == "new") return commandNew(arguments, argv[0]);
  if (command == "init") return commandInit(arguments, argv[0]);
  if (command == "add") return commandAdd(arguments);
  if (command == "remove") return commandRemove(arguments);
  if (command == "publish") return commandPublish(arguments);
  if (command == "yank") return commandYank(arguments);
  if (command == "update") return commandUpdate(arguments);
  if (command == "outdated") return commandOutdated(arguments);

  oskelp::BuildOptions options;
  std::string manifestPath;
  bool native = false;
  std::vector<std::string> programArguments;
  const bool allowArguments = command == "run";
  int status = parseProjectOptions(arguments, allowArguments, options,
                                   manifestPath, native, programArguments,
                                   command);
  if (status != oskelp::kOk) return status;

  if (command != "build" && command != "run" && command != "check" &&
      command != "test" && command != "clean" && command != "tree") {
    std::cerr << "oskelp: unknown command '" << command << "'\n\n" << kUsage;
    return oskelp::kUsage;
  }

  std::string path;
  status = resolveManifest(manifestPath, path);
  if (status != oskelp::kOk) return status;

  if (command == "build") {
    return oskelp::commandBuild(path, options, argv[0]);
  }
  if (command == "run") {
    return oskelp::commandRun(path, options, native, programArguments, argv[0]);
  }
  if (command == "check") {
    return oskelp::commandCheck(path, options, argv[0]);
  }
  if (command == "test") {
    return oskelp::commandTest(path, options, native, argv[0]);
  }
  if (command == "clean") {
    return oskelp::commandClean(path, argv[0]);
  }
  return oskelp::commandTree(path, argv[0]);
}
