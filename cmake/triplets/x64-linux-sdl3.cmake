# Shared SDL3 for the bundled UI runtime. The engine loads it lazily from
# lib/oskel/ui, so it must stay a shared library even though Linux's default
# vcpkg triplet is static.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
