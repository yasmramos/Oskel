#ifndef OSKEL_VERSION_H
#define OSKEL_VERSION_H

// The single source of truth for the version string. `CMakeLists.txt` declares
// `project(oskel VERSION ...)` and refuses to configure when the two disagree
// (see the version check at the top of CMakeLists.txt), so the binary's
// `--version`, the package names and the release tag can never drift. The
// direct build scripts (`build.sh`, `build.bat`) read this header too, which a
// CMake-generated header could not: the project builds with or without CMake.
#define OSKEL_VERSION "0.1.0"

#endif // OSKEL_VERSION_H
