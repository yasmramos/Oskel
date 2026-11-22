#include "ui_runtime.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace oskel::ui {
namespace {

namespace fs = std::filesystem;

std::string joinPath(const std::string& directory, const std::string& name) {
  if (directory.empty()) return name;
  const char last = directory.back();
  if (last == '/' || last == '\\') return directory + name;
  return directory + "/" + name;
}

std::string executableDirectory() {
#ifdef _WIN32
  char buffer[MAX_PATH];
  const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    return fs::path(std::string(buffer, length)).parent_path().string();
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size + 1);
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    std::error_code error;
    fs::path resolved = fs::weakly_canonical(buffer.data(), error);
    return (error ? fs::path(buffer.data()) : resolved).parent_path().string();
  }
#else
  char buffer[4096];
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length > 0) {
    return fs::path(std::string(buffer, static_cast<std::size_t>(length)))
        .parent_path()
        .string();
  }
#endif
  return {};
}

bool isRegularFile(const std::string& path) {
  std::error_code error;
  return fs::is_regular_file(path, error);
}

std::vector<std::string> candidateDirectories() {
  std::vector<std::string> directories;
  auto add = [&directories](const std::string& directory) {
    if (directory.empty()) return;
    for (const std::string& existing : directories) {
      if (existing == directory) return;
    }
    directories.push_back(directory);
  };

  if (const char* explicitDir = std::getenv("OSKEL_UI_LIBDIR")) add(explicitDir);
  const std::string executable = executableDirectory();
  if (!executable.empty()) {
    add(joinPath(joinPath(executable, ".."), "lib/oskel/ui"));
    add(joinPath(executable, "lib/oskel/ui"));
    add(executable);
  }
  // Last, because an installed binary still carries the build path it was
  // compiled with; the installation beside the executable must win.
#ifdef OSKEL_UI_LIBDIR_BUILD
  add(OSKEL_UI_LIBDIR_BUILD);
#endif
  return directories;
}

std::vector<std::string> runtimeNames(const std::string& name) {
#if defined(_WIN32)
  return {name + ".dll"};
#elif defined(__APPLE__)
  return {"lib" + name + ".dylib", "lib" + name + ".0.dylib"};
#else
  return {"lib" + name + ".so", "lib" + name + ".so.0"};
#endif
}

std::vector<std::string> linkNames(const std::string& name) {
#if defined(_WIN32)
  return {name + ".lib", "lib" + name + ".a"};
#elif defined(__APPLE__)
  return {"lib" + name + ".dylib", "lib" + name + ".a"};
#else
  return {"lib" + name + ".so", "lib" + name + ".a"};
#endif
}

std::string findInDirectories(const std::vector<std::string>& directories,
                              const std::vector<std::string>& names) {
  for (const std::string& directory : directories) {
    for (const std::string& name : names) {
      const std::string candidate = joinPath(directory, name);
      if (isRegularFile(candidate)) return candidate;
    }
  }
  return {};
}

// A distribution may install the versioned file only (`libSDL3.so.0.2.10`),
// with no unversioned development symlink. Match the version suffix instead of
// guessing the SONAME, which changes with every SDL release.
std::string findVersioned(const std::vector<std::string>& directories,
                          const std::string& name) {
#if defined(_WIN32)
  (void)directories;
  (void)name;
  return {};
#else
  const std::string prefix =
#ifdef __APPLE__
      "lib" + name + ".";
#else
      "lib" + name + ".so";
#endif
  for (const std::string& directory : directories) {
    std::error_code error;
    fs::directory_iterator iterator(directory, error);
    if (error) continue;
    for (const fs::directory_entry& entry : iterator) {
      const std::string candidate = entry.path().string();
      const std::string file = entry.path().filename().string();
      if (file.rfind(prefix, 0) != 0) continue;
#ifdef __APPLE__
      if (file.size() < 5 || file.substr(file.size() - 5) != ".dylib") continue;
#endif
      if (isRegularFile(candidate)) return candidate;
    }
  }
  return {};
#endif
}

std::mutex& preloadMutex() {
  static std::mutex mutex;
  return mutex;
}

bool& preloadAttempted() {
  static bool attempted = false;
  return attempted;
}

bool& preloadSucceeded() {
  static bool succeeded = false;
  return succeeded;
}

}  // namespace

bool isUiSymbol(const std::string& name) {
  if (name.rfind("SDL_", 0) == 0) return true;
  if (name.rfind("skia_", 0) == 0) return true;
  // A user binding may share an `oskel_*` prefix (`oskel_Rect_new` in the C++
  // factory test). That only costs a preload attempt: `lookupSymbol` searches
  // the already-loaded modules before it calls this, so the user's own symbol
  // still wins.
  static const char* const kSkiaPrefixes[] = {
      "oskel_Canvas_", "oskel_Font_",    "oskel_Image_",   "oskel_Paint_",
      "oskel_Rect_",   "oskel_Rects_",   "oskel_Surface_", "oskel_Surfaces_",
      "oskel_drawText", "oskel_imagePixel", "oskel_measureText",
      "oskel_readPixels",
  };
  for (const char* prefix : kSkiaPrefixes) {
    if (name.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

std::string libraryDirectory() {
  const std::vector<std::string> directories = candidateDirectories();
  for (const std::string& directory : directories) {
    for (const char* name : {"SDL3", "skia_shim"}) {
      if (!findInDirectories({directory}, runtimeNames(name)).empty()) {
        return directory;
      }
      if (!findVersioned({directory}, name).empty()) return directory;
    }
  }
  return directories.empty() ? std::string() : directories.front();
}

std::string runtimeLibraryPath(const std::string& name) {
  const std::vector<std::string> directories = candidateDirectories();
  std::string path = findInDirectories(directories, runtimeNames(name));
  if (!path.empty()) return path;
  return findVersioned(directories, name);
}

std::string linkLibraryPath(const std::string& name) {
  const std::vector<std::string> directories = candidateDirectories();
  const std::string path = findInDirectories(directories, linkNames(name));
  if (!path.empty()) return path;
#if defined(_WIN32)
  // A DLL is not a link input; an installation that has only the runtime file
  // is a packaging error, not a fallback the linker can use.
  return {};
#else
  std::string runtime = findInDirectories(directories, runtimeNames(name));
  if (!runtime.empty()) return runtime;
  return findVersioned(directories, name);
#endif
}

bool preloadLibraries() {
  std::lock_guard<std::mutex> lock(preloadMutex());
  if (preloadAttempted()) return preloadSucceeded();
  preloadAttempted() = true;

  // `skia_shim` is self-contained when it links the static Skia archive, but
  // loading `skia` first also covers a bundle that ships it as a shared object.
  for (const char* name : {"SDL3", "skia", "skia_shim"}) {
    const std::string path = runtimeLibraryPath(name);
    if (path.empty()) continue;
#ifdef _WIN32
    if (LoadLibraryA(path.c_str()) != nullptr) preloadSucceeded() = true;
#else
    if (dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL) != nullptr) {
      preloadSucceeded() = true;
    }
#endif
  }
  return preloadSucceeded();
}

}  // namespace oskel::ui
