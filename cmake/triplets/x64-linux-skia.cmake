# Static Skia for the bundled UI runtime. The shim links it into one shared
# object, so the release ships libskia_shim.so without a separate libskia.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
