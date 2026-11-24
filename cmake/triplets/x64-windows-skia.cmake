# Static Skia for the bundled UI runtime on Windows. The CRT stays dynamic so
# it matches the shim CMake builds with /MD.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
