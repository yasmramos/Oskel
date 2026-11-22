#pragma once

#include <string>

// Locates and loads the native libraries the standard UI backend needs.
//
// A normal Oskel installation ships SDL3 and the Skia bridge beside the
// executable, under `lib/oskel/ui`. Neither the VM nor `oskel build` should
// require the user to set `LD_PRELOAD`, `LD_LIBRARY_PATH` or `--link-lib`;
// this module is the single place that knows where those files live.
//
// The directory is chosen in this order:
//
//   1. `$OSKEL_UI_LIBDIR`
//   2. `<exe>/../lib/oskel/ui` (installed prefix)
//   3. `<exe>/lib/oskel/ui` and `<exe>` (Windows layout)
//   4. the build directory CMake baked in (`OSKEL_UI_LIBDIR_BUILD`)
//
// A missing bundle is not an error here: the caller reports the symbol it
// could not resolve, which keeps non-UI programs independent of the UI
// libraries.
namespace oskel::ui {

// True for a symbol the bundled UI backend exports. The list is deliberately
// narrow: `oskel_*` also names the project's own test helpers, and a user
// `extern "C"` symbol must not turn a build into a UI build by accident.
bool isUiSymbol(const std::string& name);

// The first existing bundle directory, or an empty string when none exists.
std::string libraryDirectory();

// Path to a loadable library (`libSDL3.so.0`, `SDL3.dll`, ...), or an empty
// string when the bundle does not provide it.
std::string runtimeLibraryPath(const std::string& name);

// Path a hosted link can pass to the C toolchain (`libSDL3.so`, `SDL3.lib`,
// ...). Falls back to the runtime path when no import/development file exists.
std::string linkLibraryPath(const std::string& name);

// Loads every bundled UI library once. Safe to call repeatedly and from any
// thread after the first call. Returns true when at least one library loaded.
bool preloadLibraries();

}  // namespace oskel::ui
