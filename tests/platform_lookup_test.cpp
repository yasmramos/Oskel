// Windows-only: `PlatformPosix::lookupSymbol` must search every loaded module,
// not just the process image, kernel32 and the CRT. The test loads a DLL built
// beside it and resolves an export that only that DLL provides.

#include <cstdio>

#ifdef _WIN32
#include <windows.h>

#include "platform_posix.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <dll>\n", argv[0]);
    return 2;
  }
  HMODULE module = LoadLibraryA(argv[1]);
  if (module == nullptr) {
    std::fprintf(stderr, "LoadLibraryA(%s) failed: %lu\n", argv[1], GetLastError());
    return 1;
  }
  using Symbol = int (*)();
  auto symbol = reinterpret_cast<Symbol>(platformPosix().lookupSymbol("oskelLoadedDllSymbol"));
  if (symbol == nullptr) {
    std::fprintf(stderr, "lookupSymbol did not find oskelLoadedDllSymbol\n");
    FreeLibrary(module);
    return 1;
  }
  const int value = symbol();
  FreeLibrary(module);
  if (value != 4242) {
    std::fprintf(stderr, "oskelLoadedDllSymbol returned %d, expected 4242\n", value);
    return 1;
  }
  std::printf("loaded dll symbol ok\n");
  return 0;
}
#else
#include "platform_posix.h"

// A symbol the test executable itself defines. `dlsym(RTLD_DEFAULT)` only
// finds it when the executable exports its dynamic symbols, which is what
// `ENABLE_EXPORTS` (CMake) or `-rdynamic` adds.
extern "C" int oskelOwnExecutableSymbol() { return 4242; }

int main() {
  using Symbol = int (*)();
  auto symbol =
      reinterpret_cast<Symbol>(platformPosix().lookupSymbol("oskelOwnExecutableSymbol"));
  if (symbol == nullptr) {
    std::fprintf(stderr, "lookupSymbol did not find oskelOwnExecutableSymbol\n");
    return 1;
  }
  if (symbol() != 4242) {
    std::fprintf(stderr, "oskelOwnExecutableSymbol returned the wrong value\n");
    return 1;
  }
  std::printf("own executable symbol ok\n");
  return 0;
}
#endif
