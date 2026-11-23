cmake_minimum_required(VERSION 3.15)

project(oskel VERSION 0.3.0 LANGUAGES CXX)

# `include/version.h` is the version the binary prints; CMake's project version
# names the packages and the release tag. They must agree, so the configure step
# fails loudly when they drift instead of shipping a binary whose `--version`
# disagrees with its own package name. The direct build scripts (build.sh,
# build.bat) read only the header, so the header is the value that must match.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/version.h" _oskel_version_line
  REGEX "#define[ \t]+OSKEL_VERSION[ \t]+\"")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _oskel_version_match "${_oskel_version_line}")
set(OSKEL_HEADER_VERSION "${CMAKE_MATCH_1}")
if(NOT OSKEL_HEADER_VERSION STREQUAL PROJECT_VERSION)
  message(FATAL_ERROR
    "version drift: include/version.h says '${OSKEL_HEADER_VERSION}' but "
    "CMakeLists.txt says '${PROJECT_VERSION}'; update both to the same value")
endif()

set(CMAKE_CXX_STANDARD 17)

# The hosted link is a C link, but this project enables only CXX. The Visual
# Studio generator used by the artifacts workflow does not populate
# CMAKE_C_COMPILER, so the C++ driver is the fallback; it links the C runtime
# just the same. The Windows test helper needs a real path to derive
# `vcvarsall.bat`, which is why an empty value is not good enough there.
if(CMAKE_C_COMPILER)
  set(OSKEL_HOSTED_CC "${CMAKE_C_COMPILER}")
else()
  set(OSKEL_HOSTED_CC "${CMAKE_CXX_COMPILER}")
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The project enables only CXX, so `CMAKE_C_COMPILER` is empty unless the caller
# sets it. The hosted link drives a C toolchain; the C++ compiler links the same
# object and is always identified, so fall back to it when no C compiler was
# requested. Passing a bare `cl` here would leave the test without the MSVC
# environment and fail with "'cl' is not recognized".
if(CMAKE_C_COMPILER)
  set(OSKEL_HOSTED_CC "${CMAKE_C_COMPILER}")
else()
  set(OSKEL_HOSTED_CC "${CMAKE_CXX_COMPILER}")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  # `/bigobj` is required by `src/runtime/ffi.cpp`: the typed `dispatch`
  # instantiates one function per 3^N class sequence, and at the
  # eight-eightbyte bound MSVC's default object format runs out of sections
  # (C1128).
  set(OSKEL_WARNINGS /W4 /bigobj)
  # Match the 8 MiB main-thread stack Linux and macOS give by default. The
  # recursive-descent parser and the compiler's AST walk each hold a DepthGuard
  # (see src/frontend/parser.cpp and src/frontend/compiler.cpp) sized for that
  # budget; Windows' 1 MiB default would let a deeply nested source overflow the
  # stack before the guard fires, turning a clean compile error into a crash.
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /STACK:8388608")
else()
  set(OSKEL_WARNINGS -Wall -Wextra)
endif()

# Testing aid: run the cycle collector at every safe point instead of only when
# enough objects were allocated. Slow; meant for sanitizer runs.
option(OSKEL_GC_STRESS "Collect cycles at every safe point (testing only)" OFF)
if(OSKEL_GC_STRESS)
  add_compile_definitions(OSKEL_GC_STRESS)
endif()

# --------------------------------------------------------------------------- sources
# The engine is split across three directories, and the directory a file lives
# in *is* its classification — there is no hand-maintained list to forget:
#
#   src/core/      shared vocabulary with no engine state: Value, the cycle
#                  collector, diagnostics plumbing, the platform layer.
#   src/frontend/  everything a compiler needs: lexer, parser, bytecode
#                  compiler, module resolver and the whole AOT backend.
#   src/runtime/   the interpreter: VM, JIT, builtins, operators, FFI marshalling.
#
# `oskelc` links core + frontend and never runtime, so the shipped compiler
# cannot carry an interpreter (DEVELOPMENT.md §2, toolchain boundary).
file(GLOB_RECURSE OSKEL_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")
file(GLOB_RECURSE OSKEL_FRONTEND_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/*.cpp")
file(GLOB_RECURSE OSKEL_RUNTIME_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/*.cpp")

# The entry points are not part of any library.
set(OSKEL_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
set(OSKEL_COMPILER_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/oskelc.cpp")
set(OSKEL_LINKER_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/oskelld.cpp")

# Configure-time guard, the inverse of the old one: a `.cpp` dropped straight
# into `src/` belongs to no library and would be compiled by nothing, or — worse
# — could be added to the front end by accident. Every source must be in one of
# the three directories, so the classification cannot be skipped.
file(GLOB OSKEL_STRAY_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM OSKEL_STRAY_SOURCES "${OSKEL_MAIN}" "${OSKEL_COMPILER_MAIN}"
  "${OSKEL_LINKER_MAIN}")
if(OSKEL_STRAY_SOURCES)
  foreach(stray IN LISTS OSKEL_STRAY_SOURCES)
    file(RELATIVE_PATH shown "${CMAKE_CURRENT_SOURCE_DIR}" "${stray}")
    message(SEND_ERROR
      "'${shown}' is not in src/core, src/frontend or src/runtime; move it into "
      "the directory that matches what it is (DEVELOPMENT.md §2)")
  endforeach()
  message(FATAL_ERROR
    "every source under src/ must be classified by its directory")
endif()

# --------------------------------------------------------------------------- library
# Two static libraries: the front end (core + compiler, everything a compiler
# needs) and the runtime on top of it. `oskel_lib` keeps its name for the tests,
# which drive the VM; `oskelc` links `oskel_frontend` alone.
add_library(oskel_frontend STATIC ${OSKEL_CORE_SOURCES} ${OSKEL_FRONTEND_SOURCES})
target_include_directories(oskel_frontend PUBLIC src include)
target_compile_options(oskel_frontend PRIVATE ${OSKEL_WARNINGS})

# `ui_runtime.cpp` looks for the bundled UI libraries in the build tree while
# running from a checkout, and beside the executable after `cmake --install`.
set(OSKEL_UI_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/ui")
target_compile_definitions(oskel_frontend PRIVATE
  OSKEL_UI_LIBDIR_BUILD="${OSKEL_UI_BUILD_DIR}")
# `Platform::lookupSymbol` resolves `extern "C"` declarations with `dlsym`
# (PHASE 6.6). Empty on platforms that fold libdl into libc (glibc 2.34+,
# Windows), so linking it unconditionally is safe and portable.
target_link_libraries(oskel_frontend PUBLIC ${CMAKE_DL_LIBS})

add_library(oskel_lib STATIC ${OSKEL_RUNTIME_SOURCES})
target_link_libraries(oskel_lib PUBLIC oskel_frontend)
target_compile_options(oskel_lib PRIVATE ${OSKEL_WARNINGS})

# Struct-by-value FFI is written for the System V AMD64 ABI only; the Microsoft
# x64 ABI passes a struct larger than 8 bytes through a hidden pointer, a shape
# the register thunks cannot express. The compiler rejects such a declaration
# with a clear error on other targets, and the test that exercises it is skipped.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$" AND NOT WIN32)
  set(OSKEL_FFI_STRUCT_BY_VALUE TRUE)
  set(OSKEL_FFI_CALLBACKS TRUE)
else()
  set(OSKEL_FFI_STRUCT_BY_VALUE FALSE)
  set(OSKEL_FFI_CALLBACKS FALSE)
endif()

# A struct larger than 16 bytes is class MEMORY (passed on the stack, returned
# through a hidden pointer). The development engine's descriptor stub expresses
# that on the System V AMD64 ABI; elsewhere the declaration is rejected at
# compile time and the test that exercises the rejection stays. This must mirror
# `OSKEL_FFI_MEMORY_STRUCTS` in `include/ffi_types.h`.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$" AND NOT WIN32)
  set(OSKEL_FFI_MEMORY_STRUCTS TRUE)
else()
  set(OSKEL_FFI_MEMORY_STRUCTS FALSE)
endif()

# A C variadic call goes through the development engine's descriptor stub, which
# is emitted for both x86-64 ABIs; elsewhere the declaration is rejected at
# compile time and the test that exercises it is skipped. This mirrors
# `OSKEL_FFI_VARIADIC` in `include/ffi_types.h`.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(OSKEL_FFI_VARIADIC TRUE)
else()
  set(OSKEL_FFI_VARIADIC FALSE)
endif()

# --------------------------------------------------------------------------- binaries
# `oskel`: the development engine. Links the runtime on top of the front end, so
# it can run bytecode on the VM and host the REPL.
add_executable(oskel "${OSKEL_MAIN}")
target_link_libraries(oskel PRIVATE oskel_lib)
target_compile_options(oskel PRIVATE ${OSKEL_WARNINGS})

# --------------------------------------------------------------------------- SDL3 (optional)
# `std/internal/sdl.oskl` binds SDL3's window, renderer and event surface. The
# module is always installed; the engine loads the bundled library lazily when
# the program asks for an `SDL_*` symbol, so a program that does not import
# `std.ui` never pays for SDL3.
#
# A static SDL3 is linked into the engine (it cannot be `dlopen`ed); a shared
# one is copied beside the build so `ui_runtime.cpp` finds it in a checkout, and
# installed under `lib/oskel/ui` for the released engine.
option(OSKEL_ENABLE_SDL3 "Enable SDL3 for the standard UI backend" ON)
set(OSKEL_SDL3_FOUND FALSE)
if(OSKEL_ENABLE_SDL3)
  find_package(SDL3 QUIET)
  if(SDL3_FOUND)
    set(OSKEL_SDL3_FOUND TRUE)
    get_target_property(_sdl3_type SDL3::SDL3 TYPE)
    if(_sdl3_type STREQUAL "STATIC_LIBRARY")
      target_link_libraries(oskel PRIVATE SDL3::SDL3)
      set_target_properties(oskel PROPERTIES ENABLE_EXPORTS ON)
      if(WIN32)
        set(_sdl3_bundle_name "SDL3.lib")
      else()
        set(_sdl3_bundle_name "libSDL3.a")
      endif()
      add_custom_command(TARGET oskel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OSKEL_UI_BUILD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3>"
                "${OSKEL_UI_BUILD_DIR}/${_sdl3_bundle_name}"
        VERBATIM)
      install(FILES "$<TARGET_FILE:SDL3::SDL3>" DESTINATION lib/oskel/ui
              RENAME "${_sdl3_bundle_name}")
    else()
      add_custom_command(TARGET oskel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OSKEL_UI_BUILD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3>"
                "${OSKEL_UI_BUILD_DIR}/$<TARGET_FILE_NAME:SDL3::SDL3>"
        VERBATIM)
      if(UNIX AND NOT APPLE)
        # The linked image records SDL's SONAME (`libSDL3.so.0`), not the
        # versioned file the target names. Give the bundle the names both the
        # loader and `-lSDL3` look for.
        add_custom_command(TARGET oskel POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E rm -f
                  "${OSKEL_UI_BUILD_DIR}/libSDL3.so.0"
                  "${OSKEL_UI_BUILD_DIR}/libSDL3.so"
          COMMAND ${CMAKE_COMMAND} -E create_symlink
                  "$<TARGET_FILE_NAME:SDL3::SDL3>"
                  "${OSKEL_UI_BUILD_DIR}/libSDL3.so.0"
          COMMAND ${CMAKE_COMMAND} -E create_symlink
                  "libSDL3.so.0" "${OSKEL_UI_BUILD_DIR}/libSDL3.so"
          VERBATIM)
      endif()
      if(WIN32)
        add_custom_command(TARGET oskel POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "$<TARGET_LINKER_FILE:SDL3::SDL3>"
                  "${OSKEL_UI_BUILD_DIR}/$<TARGET_LINKER_FILE_NAME:SDL3::SDL3>"
          VERBATIM)
        install(FILES "$<TARGET_FILE:SDL3::SDL3>" DESTINATION lib/oskel/ui)
        install(FILES "$<TARGET_LINKER_FILE:SDL3::SDL3>" DESTINATION lib/oskel/ui)
      elseif(UNIX AND NOT APPLE)
        install(FILES "$<TARGET_FILE:SDL3::SDL3>" DESTINATION lib/oskel/ui
                RENAME "libSDL3.so.0")
        install(FILES "$<TARGET_FILE:SDL3::SDL3>" DESTINATION lib/oskel/ui
                RENAME "libSDL3.so")
      else()
        install(FILES "$<TARGET_FILE:SDL3::SDL3>" DESTINATION lib/oskel/ui)
      endif()
    endif()
    message(STATUS "SDL3 found: std.internal.sdl enabled in the development engine")
  else()
    message(STATUS "SDL3 not found: std.internal.sdl tests skipped (set OSKEL_ENABLE_SDL3=OFF to silence)")
  endif()
endif()

# --------------------------------------------------------------------------- Skia (optional)
# `std/internal/skia.oskl` is the raster backend for `std.ui`. The checked-in
# module needs no Skia to compile; building the C++ shim needs a Skia root with
# headers and a built `libskia.a`/`libskia.so`.
#
# When both are present CMake builds `skia_shim` into `build/ui`, which is the
# directory `ui_runtime.cpp` searches while running from a checkout. The
# installed engine finds the same file under `lib/oskel/ui`.
set(OSKEL_SKIA_DIR "" CACHE PATH
    "Skia root with include/core/SkSurface.h (enables the UI backend)")
set(OSKEL_SKIA_LIB "" CACHE FILEPATH
    "Path to libskia.a or libskia.so (enables the UI backend)")
if(WIN32)
  # MSVC links system libraries by name; the POSIX `-l` spellings are invalid.
  set(OSKEL_SKIA_LIBS "" CACHE STRING
      "Extra link arguments for the Skia shim")
else()
  set(OSKEL_SKIA_LIBS "-lpthread -ldl -lm" CACHE STRING
      "Extra link arguments for the Skia shim")
endif()
if(OSKEL_SKIA_DIR AND NOT OSKEL_SKIA_LIB)
  foreach(_skia_candidate
      "${OSKEL_SKIA_DIR}/out/Release/libskia.a"
      "${OSKEL_SKIA_DIR}/out/Release-x64/libskia.a"
      "${OSKEL_SKIA_DIR}/out/Release/libskia.so"
      "${OSKEL_SKIA_DIR}/out/Release-x64/libskia.so"
      "${OSKEL_SKIA_DIR}/out/Release/skia.lib"
      "${OSKEL_SKIA_DIR}/out/Release-x64/skia.lib")
    if(EXISTS "${_skia_candidate}")
      set(OSKEL_SKIA_LIB "${_skia_candidate}" CACHE FILEPATH
          "Path to libskia.a or libskia.so (enables the UI backend)" FORCE)
      break()
    endif()
  endforeach()
endif()

# `oskelc`: the host compiler. Sources in, a native image out; it links the front
# end alone, so the shipped compiler carries neither the VM nor the JIT
# (DEVELOPMENT.md §2). `tests/check_compiler_boundary.cmake` keeps the include
# graph clean and `oskelc_has_no_vm` proves the link stays clean.
add_executable(oskelc "${OSKEL_COMPILER_MAIN}")
target_link_libraries(oskelc PRIVATE oskel_frontend)
target_compile_options(oskelc PRIVATE ${OSKEL_WARNINGS})

# `oskelld`: the project's own linker (PHASE 10.1). The linker core lives in
# `src/frontend/native/link.cpp`, so this is a thin third entry point over the
# front end: it carries no interpreter, and `oskel build --linker own` calls the
# same code in process.
add_executable(oskelld "${OSKEL_LINKER_MAIN}")
target_link_libraries(oskelld PRIVATE oskel_frontend)
target_compile_options(oskelld PRIVATE ${OSKEL_WARNINGS})

install(TARGETS oskel oskelc oskelld RUNTIME DESTINATION bin)

# --------------------------------------------------------------------------- bindgen
# The binding generator: a C header in, an Oskel `extern "C"` module out. It is
# a standalone executable that does not link the engine's runtime (it only
# writes text), so it can be built and shipped on its own. Its sources live
# under `tools/`, outside the three `src/` globs, so the generator never ends up
# inside `oskel`.
add_library(bindgen_lib STATIC "${CMAKE_CURRENT_SOURCE_DIR}/tools/bindgen.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/tools/cxxbind.cpp")
target_include_directories(bindgen_lib PUBLIC include src)
target_compile_options(bindgen_lib PRIVATE ${OSKEL_WARNINGS})

add_executable(bindgen "${CMAKE_CURRENT_SOURCE_DIR}/tools/bindgen_main.cpp")
target_link_libraries(bindgen PRIVATE bindgen_lib)
target_compile_options(bindgen PRIVATE ${OSKEL_WARNINGS})

install(TARGETS bindgen RUNTIME DESTINATION bin)

# --------------------------------------------------------------------------- UI runtime
# Build the Skia bridge when a Skia root is available. The generated module
# stays in the build tree; the checked-in `std/internal/skia.oskl` is the
# module the compiler installs, and `cxxbind.generate` keeps the two identical.
if(OSKEL_SKIA_DIR AND OSKEL_SKIA_LIB)
  if(NOT EXISTS "${OSKEL_SKIA_DIR}/include/core/SkSurface.h")
    message(FATAL_ERROR
      "OSKEL_SKIA_DIR does not contain include/core/SkSurface.h: ${OSKEL_SKIA_DIR}")
  endif()
  if(NOT EXISTS "${OSKEL_SKIA_LIB}")
    message(FATAL_ERROR "OSKEL_SKIA_LIB does not exist: ${OSKEL_SKIA_LIB}")
  endif()

  set(OSKEL_SKIA_GENERATED_DIR "${OSKEL_UI_BUILD_DIR}/generated")
  set(OSKEL_SKIA_SHIM_CPP "${OSKEL_SKIA_GENERATED_DIR}/skia_shim.cpp")
  add_custom_command(
    OUTPUT "${OSKEL_SKIA_SHIM_CPP}" "${OSKEL_SKIA_GENERATED_DIR}/skia_shim.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OSKEL_SKIA_GENERATED_DIR}"
    COMMAND $<TARGET_FILE:bindgen>
            --cxx "${CMAKE_CURRENT_SOURCE_DIR}/bindings/skia/skia.oskelbind"
            --shim "${OSKEL_SKIA_GENERATED_DIR}"
            -o "${OSKEL_SKIA_GENERATED_DIR}/skia.oskl"
    DEPENDS bindgen
            "${CMAKE_CURRENT_SOURCE_DIR}/bindings/skia/skia.oskelbind"
    VERBATIM)

  add_library(skia_shim SHARED "${OSKEL_SKIA_SHIM_CPP}")
  # `extern "C"` alone does not export a symbol from a Windows DLL. The shim
  # has no macro in the generated source, so let CMake write the `.def`.
  set_target_properties(skia_shim PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_include_directories(skia_shim PRIVATE
    "${OSKEL_SKIA_GENERATED_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/bindings/skia"
    "${OSKEL_SKIA_DIR}")
  if(WIN32)
    separate_arguments(OSKEL_SKIA_LINK_ARGS NATIVE_COMMAND "${OSKEL_SKIA_LIBS}")
  else()
    separate_arguments(OSKEL_SKIA_LINK_ARGS UNIX_COMMAND "${OSKEL_SKIA_LIBS}")
  endif()
  target_link_libraries(skia_shim PRIVATE "${OSKEL_SKIA_LIB}" ${OSKEL_SKIA_LINK_ARGS})
  if(APPLE)
    target_link_options(skia_shim PRIVATE "-Wl,-rpath,@loader_path")
  elseif(UNIX)
    target_link_options(skia_shim PRIVATE "-Wl,-rpath,$ORIGIN")
  endif()
  if(UNIX AND CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel|RelWithDebInfo)$")
    # Static Skia carries its object debug information into the shared bridge;
    # the shipped file is the machine code only.
    add_custom_command(TARGET skia_shim POST_BUILD
      COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:skia_shim>"
      VERBATIM)
  endif()
  set_target_properties(skia_shim PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${OSKEL_UI_BUILD_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${OSKEL_UI_BUILD_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${OSKEL_UI_BUILD_DIR}")
  foreach(_config Debug Release RelWithDebInfo MinSizeRel)
    set_target_properties(skia_shim PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${_config} "${OSKEL_UI_BUILD_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_${_config} "${OSKEL_UI_BUILD_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY_${_config} "${OSKEL_UI_BUILD_DIR}")
  endforeach()

  install(TARGETS skia_shim
    LIBRARY DESTINATION lib/oskel/ui
    RUNTIME DESTINATION lib/oskel/ui
    ARCHIVE DESTINATION lib/oskel/ui)
  if(OSKEL_SKIA_LIB MATCHES "\\.(so|dylib|dll)$")
    install(FILES "${OSKEL_SKIA_LIB}" DESTINATION lib/oskel/ui)
  endif()
  message(STATUS "Skia found: std.ui native backend enabled")
else()
  message(STATUS "Skia not found: std.ui native backend not built")
endif()

# --------------------------------------------------------------------------- oskelp
# The project and package manager. Like `bindgen`, it is a standalone
# executable: it links no engine code and drives `oskel`/`oskelc` as child
# processes. Its sources live under `tools/`, outside the three `src/` globs, so
# it never ends up inside `oskel` or `oskelc`.
add_library(oskelp_lib STATIC "${CMAKE_CURRENT_SOURCE_DIR}/tools/oskelp.cpp")
target_include_directories(oskelp_lib PUBLIC include src)
target_compile_options(oskelp_lib PRIVATE ${OSKEL_WARNINGS})

add_executable(oskelp "${CMAKE_CURRENT_SOURCE_DIR}/tools/oskelp_main.cpp")
target_link_libraries(oskelp PRIVATE oskelp_lib)
target_compile_options(oskelp PRIVATE ${OSKEL_WARNINGS})

# `std::filesystem` needs an extra library on GCC 8; modern toolchains fold it
# into libstdc++.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  target_link_libraries(oskelp_lib PUBLIC stdc++fs)
endif()

install(TARGETS oskelp RUNTIME DESTINATION bin)

# The standard library ships next to the binary: `standardLibraryDir` looks for
# `../share/oskel/std` relative to the executable, so `import std.math` resolves
# after `make install` and not only when running from the source tree.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/std" DESTINATION share/oskel
  FILES_MATCHING PATTERN "*.oskl")

# --------------------------------------------------------------------------- tests
include(CTest)

if(BUILD_TESTING)

  # Registers one script in CTest; the expected exit code comes from the
  # first-line `// expect-exit: N` comment, defaulting to 0.
  function(oskel_add_script_test name script)
    set(expected 0)
    file(STRINGS "${script}" first_line LIMIT_COUNT 1)
    string(REGEX MATCH "expect-exit:[ \t]*([0-9]+)" declared_exit "${first_line}")
    if(declared_exit)
      set(expected "${CMAKE_MATCH_1}")
    endif()
    add_test(NAME "${name}"
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${script}"
        "-DEXPECTED_EXIT=${expected}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_oskel_case.cmake")
  endfunction()

  # Registers one C++ test. The test file is `tests/<name>.cpp`; it links
  # against `oskel_lib` so it does not repeat the source list.
  # Run from the repository root so a test can read fixtures by their path
  # relative to the root (e.g. `tests/modules/util.oskl`).
  function(oskel_add_cpp_test name)
    add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.cpp")
    target_link_libraries(${name} PRIVATE oskel_lib)
    target_compile_options(${name} PRIVATE ${OSKEL_WARNINGS})
    add_test(NAME ${name} COMMAND ${name}
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endfunction()

  # --- C++ tests (one line each) ---
  oskel_add_cpp_test(compiler_test)
  oskel_add_cpp_test(module_test)
  oskel_add_cpp_test(vm_test)
  oskel_add_cpp_test(jit_test)
  oskel_add_cpp_test(value_test)
  oskel_add_cpp_test(repl_test)
  oskel_add_cpp_test(gc_test)
  oskel_add_cpp_test(codegen_test)
  oskel_add_cpp_test(dtoa_test)
  oskel_add_cpp_test(parse_float_test)
  oskel_add_cpp_test(mathlib_test)
  oskel_add_cpp_test(elf_test)
  oskel_add_cpp_test(object_test)
  oskel_add_cpp_test(link_test)
  oskel_add_cpp_test(link_pe_test)

  # PHASE 10.3a: the own linker emits a PE32+ image on Windows. The fixture is
  # a plain `print`, so the test exercises the UCRT imports and the generated
  # `_start` end to end.
  if(WIN32)
    add_test(NAME oskel.build_hosted_own_windows
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_link_windows.oskl"
        "-DOSKEL_EXPECTED_OUTPUT=windows own linker"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_own_windows.cmake")
    # PHASE 10.3b.3: `_start` obtains argc/argv with `__getmainargs`; the
    # quoted argument with a space survives as one element.
    add_test(NAME oskel.build_hosted_own_windows_args
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_link_windows_args.oskl"
        "-DOSKEL_EXPECTED_OUTPUT=2 <one><two words>"
        "-DOSKEL_ARGS=one;two words"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_own_windows.cmake")
  endif()
  oskel_add_cpp_test(pe_test)
  oskel_add_cpp_test(lower_test)
  oskel_add_cpp_test(bytecode_io_test)
  oskel_add_cpp_test(freestanding_test)
  oskel_add_cpp_test(bare_metal_test)

  # Windows only: `lookupSymbol` must search a DLL loaded after startup, not
  # just the process image, kernel32 and the CRT. The test loads the DLL CMake
  # builds beside it and resolves its export.
  if(WIN32)
    add_library(oskel_lookup_test_dll SHARED
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/platform_lookup_dll.cpp")
    add_executable(platform_lookup_test
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/platform_lookup_test.cpp")
    target_link_libraries(platform_lookup_test PRIVATE oskel_lib)
    target_compile_options(platform_lookup_test PRIVATE ${OSKEL_WARNINGS})
    add_test(NAME platform_lookup_test
      COMMAND platform_lookup_test "$<TARGET_FILE:oskel_lookup_test_dll>"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()

  # The binding generator is a standalone tool, not part of `oskel_lib`, so its
  # unit test links `bindgen_lib` directly instead of the helper above.
  add_executable(bindgen_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/bindgen_test.cpp")
  target_link_libraries(bindgen_test PRIVATE bindgen_lib)
  target_compile_options(bindgen_test PRIVATE ${OSKEL_WARNINGS})
  add_test(NAME bindgen_test COMMAND bindgen_test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

  # End-to-end: run the `bindgen` binary on a header, then feed the generated
  # module back to `oskel -c`, so the tool and the compiler cannot drift apart.
  add_test(NAME bindgen.end_to_end
    COMMAND ${CMAKE_COMMAND}
      "-DBINDGEN_EXE=$<TARGET_FILE:bindgen>"
      "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
      "-DBINDGEN_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/tests/bindgen/sample.h"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_bindgen_case.cmake")

  # `--target` selects the C data model for `long`/`wchar_t`; the module it
  # produces must still compile.
  add_test(NAME bindgen.target
    COMMAND ${CMAKE_COMMAND}
      "-DBINDGEN_EXE=$<TARGET_FILE:bindgen>"
      "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
      "-DBINDGEN_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/tests/bindgen/target.h"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_bindgen_target.cmake")

  # The Skia bridge needs a real Skia root for the shim and the pixel test.
  # Both are opt-in: a checkout without Skia still runs the generator half.
  # The cache variables are declared above, outside `BUILD_TESTING`, because
  # they also build the UI runtime that a release ships.

  # `--cxx` generates the module, the C header and the C++ shim. The generation
  # and `oskel -c` half runs everywhere; the shim compile plus VM/AOT execution
  # is Linux-only (LD_PRELOAD, hosted AOT).
  add_test(NAME cxxbind.generate
    COMMAND ${CMAKE_COMMAND}
      "-DBINDGEN_EXE=$<TARGET_FILE:bindgen>"
      "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cxxbind-generate"
      "-DRUN_RUNTIME=OFF"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_cxxbind_case.cmake")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
     CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_test(NAME cxxbind.end_to_end
      COMMAND ${CMAKE_COMMAND}
        "-DBINDGEN_EXE=$<TARGET_FILE:bindgen>"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cxxbind-end-to-end"
        "-DRUN_RUNTIME=ON"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_cxxbind_case.cmake")
  endif()

  if(OSKEL_SKIA_DIR AND OSKEL_SKIA_LIB AND
     CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
     CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(OSKEL_TEST_SDL3_LIBRARY "")
    if(TARGET SDL3::SDL3)
      set(OSKEL_TEST_SDL3_LIBRARY "$<TARGET_FILE:SDL3::SDL3>")
    endif()
    add_test(NAME cxxbind.skia
      COMMAND ${CMAKE_COMMAND}
        "-DBINDGEN_EXE=$<TARGET_FILE:bindgen>"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cxxbind-skia"
        "-DSKIA_DIR=${OSKEL_SKIA_DIR}"
        "-DSKIA_LIB=${OSKEL_SKIA_LIB}"
        "-DSKIA_LIBS=${OSKEL_SKIA_LIBS}"
        "-DSDL3_FOUND=${OSKEL_SDL3_FOUND}"
        "-DSDL3_LIBRARY=${OSKEL_TEST_SDL3_LIBRARY}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_skia_case.cmake")
  endif()

  # `oskelp` is standalone too, so its unit test links `oskelp_lib` directly.
  add_executable(oskelp_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/oskelp_test.cpp")
  target_link_libraries(oskelp_test PRIVATE oskelp_lib)
  target_compile_options(oskelp_test PRIVATE ${OSKEL_WARNINGS})
  add_test(NAME oskelp_test COMMAND oskelp_test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

  # End-to-end: the real binary through new/check/build/run/test, a path
  # dependency, --locked, a cycle, a failing test, clean and init. The AOT
  # backend only runs on an x86-64 host (Linux, Windows and Intel macOS), so
  # the native half of the test is gated the same way as the `oskel build`
  # fixtures.
  if((CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32 OR
      CMAKE_SYSTEM_NAME STREQUAL "Darwin") AND
     CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(OSKELP_AOT_RUNNABLE ON)
  else()
    set(OSKELP_AOT_RUNNABLE OFF)
  endif()
  add_test(NAME oskelp.end_to_end
    COMMAND ${CMAKE_COMMAND}
      "-DOSKELP_EXE=$<TARGET_FILE:oskelp>"
      "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
      "-DOSKELC_EXE=$<TARGET_FILE:oskelc>"
      "-DOSKEL_AOT_RUNNABLE=${OSKELP_AOT_RUNNABLE}"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/oskelp-e2e"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_oskelp_case.cmake")

  # --- REPL scripted test (clear) ---
  add_test(NAME repl.clear
    COMMAND ${CMAKE_COMMAND}
      "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
      "-DREPL_INPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/repl/clear.in"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_repl_case.cmake")

  # --- Compiler boundary check (DEVELOPMENT.md toolchain boundary) ---
  # Every source `oskelc` compiles: core plus front end.
  add_test(NAME compiler_boundary
    COMMAND ${CMAKE_COMMAND}
      -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
      "-DFRONTEND_SOURCES=${OSKEL_CORE_SOURCES};${OSKEL_FRONTEND_SOURCES}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_compiler_boundary.cmake")

  # --- The VS Code extension matches the language ---
  # The grammar keeps its own keyword/type/builtin lists, so it rots whenever the
  # language grows a word. The check derives the vocabulary from the lexer, the
  # type names in `ast.h` and the builtin registry, and also rejects syntax the
  # lexer never produces. It reads the repository only, so it runs everywhere.
  add_test(NAME vscode_grammar
    COMMAND ${CMAKE_COMMAND}
      -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_vscode_grammar.cmake")

  # --- The VS Code snippets compile ---
  # The grammar check proves the extension does not highlight a word the
  # language lacks; this one proves the snippets do not emit one. Each body is
  # expanded the way VS Code expands it and fed to the compiler with `-c`, so a
  # snippet that drifts (a `defer` without a block, a `case` label that is not a
  # literal) fails the suite instead of shipping.
  add_test(NAME vscode_snippets
    COMMAND ${CMAKE_COMMAND}
      -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -DOSKEL=$<TARGET_FILE:oskel>
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_vscode_snippets.cmake")

  # --- The VS Code extension reports the compiler's own diagnostics ---
  # `editors/vscode` is no longer declaration-only: on save it runs `oskel -c`
  # and publishes the output as editor diagnostics. The parser that turns the
  # compiler's stderr into a range is pure text in, plain objects out (it does
  # not import `vscode`), so `node --test` drives it without an editor, and
  # `node --check` covers the file that does import it. Node is not a build
  # dependency of the engine, so the check skips where it is absent.
  add_test(NAME vscode_diagnostics
    COMMAND ${CMAKE_COMMAND}
      -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_vscode_diagnostics.cmake")

  # --- `oskelc` links no interpreter ---
  # The include graph is checked above; this one checks the *archive* `oskelc`
  # links, not the linked binary: an unreferenced runtime object sitting in
  # `liboskel_frontend.a` never reaches the executable, so a symbol check on the
  # binary cannot see it. `nm` is a POSIX tool, so the test is skipped where it
  # is absent.
  add_test(NAME oskelc_has_no_vm
    COMMAND ${CMAKE_COMMAND}
      "-DOSKELC_EXE=$<TARGET_FILE:oskelc>"
      "-DOSKELD_EXE=$<TARGET_FILE:oskelld>"
      "-DFRONTEND_ARCHIVE=$<TARGET_FILE:oskel_frontend>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_oskelc_no_vm.cmake")

  # --- Every builtin exists in both engines ---
  # A builtin registered by `registerBuiltins` but not seeded into the AOT chunk
  # runs in the VM and fails `oskel build`. Each engine is self-consistent, so
  # only a check that reads both registries can see the gap (AGENTS.md: "VM/AOT
  # parity is part of every feature"). The names that are VM-only on purpose are
  # listed in the script and tracked in BACKLOG §6 item 25.
  add_test(NAME builtin_parity
    COMMAND ${CMAKE_COMMAND}
      -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_builtin_parity.cmake")

  # --- Version consistency: `oskel --version` matches the declared version ---
  add_test(NAME version_consistency
    COMMAND ${CMAKE_COMMAND}
      "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
      "-DOSKEL_EXPECTED_VERSION=${PROJECT_VERSION}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_version.cmake")

  # `oskelc` prints the same version from the same header; a hard-coded literal
  # in `src/oskelc.cpp` would otherwise ship unnoticed.
  add_test(NAME version_consistency_oskelc
    COMMAND ${CMAKE_COMMAND}
      "-DOSKEL_EXE=$<TARGET_FILE:oskelc>"
      "-DOSKEL_EXPECTED_VERSION=${PROJECT_VERSION}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_version.cmake")

  add_test(NAME version_consistency_oskelld
    COMMAND ${CMAKE_COMMAND}
      "-DOSKEL_EXE=$<TARGET_FILE:oskelld>"
      "-DOSKEL_EXPECTED_VERSION=${PROJECT_VERSION}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_version.cmake")

  # --- Installed layout: `std/` ships with the binary and resolves ---
  add_test(NAME install_stdlib
    COMMAND ${CMAKE_COMMAND}
      "-DOSKEL_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOSKEL_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DOSKEL_EXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/21_std_math.oskl"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_stdlib.cmake")

  # --- SDL3 bindings (`std/internal/sdl.oskl`) ---
  # The module is always installed; these tests need the development engine to
  # have SDL3 loaded, which CMake arranges above when it finds the library.
  # `run_sdl_case.cmake` sets SDL_VIDEODRIVER=dummy, so no display is needed.
  # The hosted fixture is Linux x86-64 only, like every hosted AOT test.
  add_test(NAME sdl.bindgen
    COMMAND ${CMAKE_COMMAND}
      "-DBINDGEN=$<TARGET_FILE:bindgen>"
      "-DHEADER=${CMAKE_CURRENT_SOURCE_DIR}/bindings/sdl/sdl3_subset.h"
      "-DMODULE=${CMAKE_CURRENT_SOURCE_DIR}/std/internal/sdl.oskl"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_sdl_bindings.cmake")
  if(OSKEL_SDL3_FOUND)
    add_test(NAME sdl.bindings
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/sdl_bindings.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_sdl_case.cmake")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
      add_test(NAME oskel.build_hosted_sdl
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_sdl"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/sdl_bindings.oskl"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_sdl.cmake")
    endif()
  endif()

  # --- `oskel build`: compile to a native binary and run it (PHASE 7d/7e) ---
  # The AOT backend has a Linux and a Windows target. One fixture list drives
  # both: every case builds for the host's target and runs the image, and the
  # runner (`tests/build_binary.cmake`) is portable. Keeping a single list is the
  # point -- the Windows side used to register two cases against Linux's ten, and
  # a Windows-only stack overflow slipped through that gap.
  if((CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32) AND
     CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_test(NAME oskel.build_binary
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_hello.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `oskelc` and `oskel build` are one compiler behind two entry points: the
    # same fixture is compiled by each, the images must be byte for byte
    # identical, and the compiler's image must agree with the VM. This is the
    # guard that keeps `oskel build` a thin alias instead of a fork.
    add_test(NAME oskelc.parity
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskelc.parity"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKELC_EXE=$<TARGET_FILE:oskelc>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_hello.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/oskelc_parity.cmake")

    # The `.oskc` image is lossless: a program written to an image, run on the
    # VM and lowered to a binary must agree with the source on both. The two
    # binaries are compared byte for byte, which is what proves the image kept
    # everything the lowerer reads.
    add_test(NAME oskelc.oskc_parity
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskelc.oskc_parity"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKELC_EXE=$<TARGET_FILE:oskelc>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_oskc_parity.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/oskc_parity.cmake")

    # The same round trip on a real program: closures, collections and string
    # formatting, so a field the format carries but the small fixture never
    # exercises (an upvalue, a map constant) cannot break silently.
    add_test(NAME oskelc.oskc_parity_full
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskelc.oskc_parity_full"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKELC_EXE=$<TARGET_FILE:oskelc>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_oskc_full.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/oskc_parity.cmake")

    # The UI requirement must survive the round trip too. A program that imports
    # `std.ui.window` records SDL and Skia in the image header, and both paths
    # must then agree: build, or refuse with exit 65 and the same message. A
    # regression that dropped the bits would let the image path emit a
    # freestanding binary that cannot open a window while the source path
    # refused it.
    add_test(NAME oskelc.oskc_ui_requirement
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKELC_EXE=$<TARGET_FILE:oskelc>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_oskc_ui.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/oskc_ui_requirement.cmake")

    # The flagship example must also build and agree with the VM, so the AOT
    # backend is exercised by a real program (a lexer, a parser and an
    # evaluator), not only by the small fixture above.
    add_test(NAME oskel.build_binary_calc
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_calc"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/examples/calc.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The floating-point example builds too, so the numeric paths of the backend
    # (float arithmetic, comparisons, modulo) are checked against the VM by a
    # real program.
    add_test(NAME oskel.build_binary_mandelbrot
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_mandelbrot"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/examples/mandelbrot.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `std.math` declares ten `native fn` host bindings. An unused declaration
    # must not fail the build (PHASE 7f.1), and `sqrt` resolves from a curated
    # stub that matches the VM bit for bit.
    add_test(NAME oskel.build_binary_math
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_math"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_math.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The shape of the original gap: `import std.math` with only its pure Oskel
    # members used. Before 7f.1 the declarations alone failed the build.
    add_test(NAME oskel.build_binary_math_pure
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_math_pure"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_math_pure.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The numeric conversions, checked against the VM here because the script
    # tests that cover them (`tests/26_float_double.oskl`) only run on the
    # hosted engine. A boxed `Float` is stored widened to binary64, so the
    # `double`/`float`/`int` builtins must not widen it a second time.
    add_test(NAME oskel.build_binary_conversions
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_conversions"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_conversions.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `+` on two arrays concatenates them; the hosted engine builds a fresh
    # array and leaves both operands unchanged. Checked against the VM here
    # because the script test that covers array concatenation
    # (`tests/06_operators.oskl`) only runs on the hosted engine, so the AOT
    # backend used to reject the pair at runtime.
    add_test(NAME oskel.build_binary_array_concat
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_array_concat"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_array_concat.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `arr.push`/`arr.pop` are bound methods: the receiver is captured, `push`
    # returns the array and `pop` the removed item. Checked against the VM here
    # because the script tests that cover them only run on the hosted engine,
    # so the AOT backend used to answer `arr.push` with "value has no such
    # member".
    add_test(NAME oskel.build_binary_array_methods
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_array_methods"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_array_methods.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The in-place and query array methods (PHASE 7c.3e): `shift`, `unshift`,
    # `insert`, `removeAt`, `contains`, `indexOf` and `clear`. Same binding
    # mechanism as `push`/`pop`, so the receiver is captured and the call
    # mutates or scans it; the backend used to answer each with "value has no
    # such member". Checked against the VM, so both the return value and the
    # mutation are pinned.
    add_test(NAME oskel.build_binary_array_methods_inplace
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_array_methods_inplace"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_array_methods_inplace.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The exact wording of the guards that assemble their message at run time
    # (`array index 9 out of range (length 3)`, `loadByte() index 9 is out of
    # range`, `insert() index out of range (length 3)`, ...). Each one used to
    # trap with a bare "index out of range", so a program that printed the
    # caught message differed between the engines; the fixture catches every
    # error with `pcall` and prints its text, so a word, a space or a number
    # that drifts fails the comparison against the VM.
    add_test(NAME oskel.build_binary_error_messages
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_error_messages"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_error_messages.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The allocating array methods (PHASE 7c.3f): `slice`, `concat`, `reversed`
    # and `clone`. Each returns a new array and leaves the receiver untouched,
    # so the backend must allocate a fresh header plus items block. Checked
    # against the VM, so the returned arrays and the receiver's immutability are
    # pinned.
    add_test(NAME oskel.build_binary_array_methods_alloc
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_array_methods_alloc"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_array_methods_alloc.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The bound range methods (PHASE 7c.3l): `r.toArray()` and `r.contains(x)`.
    # `emitMemberGet` only knew a range's `.length`, so the backend answered
    # both with "value has no such member". `toArray` materialises the range
    # (half-open or closed, either direction) and refuses a span over ten
    # million items with the hosted message; `contains` is the method form of
    # `x in r`. The `.length` saturation is pinned here too: a span wider than
    # INT64_MAX reports INT64_MAX on both engines instead of overflowing.
    add_test(NAME oskel.build_binary_range_methods
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_range_methods"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_range_methods.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The bound string methods: `upper`, `lower`, `trim`/`trimStart`/`trimEnd`,
    # `contains`, `startsWith`, `endsWith`, `indexOf`, `substr`, `reversed` and
    # `repeat`. Same binding mechanism as `arr.push`, so the receiver is
    # captured and each call returns a fresh string or a query result; the
    # backend used to answer each with "value has no such member". Checked
    # against the VM, so the Unicode case mapping, the code-point indexing and
    # the byte-exact result are all pinned.
    add_test(NAME oskel.build_binary_string_methods
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_string_methods"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_string_methods.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `sort`/`sorted` (PHASE 7c.3j): the only collection methods whose comparator
    # is arbitrary program code, so the AOT body calls back through `oskel_call`
    # from inside a bottom-up stable merge sort. Checked against the VM, so the
    # default order, the stability on equal keys, the receiver mutation of
    # `sort` versus the fresh array of `sorted`, and the strong guarantee (a
    # raising comparator leaves the receiver untouched) are all pinned.
    add_test(NAME oskel.build_binary_sort_methods
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_sort_methods"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_sort_methods.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The bound map methods (PHASE 7c.3g), the map twin of the array methods
    # above. Same binding mechanism as `arr.push`, so the receiver is captured
    # and the call mutates or scans it; the backend used to answer each with
    # "value has no such member". Checked against the VM, so both the return
    # value and the mutation are pinned.
    add_test(NAME oskel.build_binary_map_methods
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_map_methods"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_map_methods.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `repr`/`str` of a function value (BACKLOG §6.22). The VM renders
    # `<fn name>` for a user function and `<native fn name>` for a native one,
    # bound methods included; the AOT backend used to render the bare literal
    # `<fn>`. The closure now carries a `display` pointer to a static
    # `AotString`, so the name survives and the fixture compares both engines
    # byte for byte.
    add_test(NAME oskel.build_binary_function_repr
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_function_repr"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_function_repr.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `**` with a float/double operand or a negative exponent (PHASE 7c.1d.2).
    # It used to lower only for int/int with a non-negative exponent; every
    # other numeric pair failed with the unsupported-operand error, so a program
    # the VM accepted did not run under `oskel build`. The float path shares the
    # embedded musl `pow` with the `pow` builtin and follows the hosted
    # `makeNumeric` type rule; the binary must agree with the VM, `type` included.
    add_test(NAME oskel.build_binary_pow
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_pow"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_pow.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # A recursive program with a non-trivial frame checks the process stack the
    # loader hands the binary: the AOT runtime has no call-depth guard, so the
    # reserve must be large enough for the recursion the VM accepts. The
    # fixture is sized to overflow a 1 MiB reserve (the Windows default) and
    # pass with the 8 MiB the emitter sets, so this is the regression guard for
    # `kPeStackReserve` -- on Windows, where the two platforms used to differ.
    add_test(NAME oskel.build_binary_deep_recursion
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_binary_deep_recursion"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_deep_recursion.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The remaining public builtins (PHASE 7c.5b.5) -- `range`, `assertEq` and
    # `exit` -- build and agree with the VM. `assertEq` exercises the shared
    # comparison and the recursive repr; `exit` ends the program before the
    # statement after it, which the fixture asserts by leaving it unreachable.
    add_test(NAME oskel.build_binary_builtins
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_builtins"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_builtins.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # Compound bitwise assignment (`&=`, `|=`, `^=`, `<<=`, `>>=`): no new
    # opcode, only the existing BAND/BOR/BXOR/SHL/SHR behind the `GET; op; SET`
    # shape the arithmetic compounds already use. The fixture covers a
    # variable, an index and a member target and the expression value, so the
    # binary must agree with the VM byte for byte.
    add_test(NAME oskel.build_binary_compound_ops
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_compound_ops"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_compound_ops.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # Error values (PHASE 7c.5c): `error(message, code?)`, `isError`, `type`,
    # `.message`/`.code`, structural equality, `print`/`repr` and interpolation
    # must agree with the VM. This is what makes value-based `try`/`catch` work
    # in a native binary (`$isError` now sees a real error tag).
    add_test(NAME oskel.build_binary_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_error"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_error.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # A runtime error message is observable through `pcall`, so the two engines
    # must word it the same way. This pins the messages without a dynamic tail
    # exactly (`string index must be an integer`, `map keys must be strings`,
    # `range membership requires an integer`) and the fixed prefix of the ones
    # that quote a type or member name (BACKLOG §6 item 25).
    add_test(NAME oskel.build_binary_runtime_error_messages
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_runtime_error_messages"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_runtime_error_messages.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `switch`, `defer` and `struct` lower in AOT but no earlier fixture
    # exercised them: `switch` on an int, a string and a range, `defer` in LIFO
    # order across nested scopes, and `struct` with typed/untyped fields,
    # nesting, a struct return type and structural equality. All of it stays on
    # the portable surface, so the VM and the binary must agree byte for byte.
    add_test(NAME oskel.build_binary_control_flow
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_control_flow"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_control_flow.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The link mode is a deployment choice, not language syntax (PHASE 7f.3).
    # `--mode freestanding` must behave exactly like the default (self-contained,
    # agrees with the VM), `--mode hosted` links against libc and must agree with
    # the VM too (7f.4), and an unknown mode must be a usage error that names the
    # valid ones.
    add_test(NAME oskel.build_mode
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_CC=${OSKEL_HOSTED_CC}"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_hello.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_mode.cmake")

    # The hosted prelude reaches the position through `lseek`/`_lseeki64` and
    # opens an append stream with `O_APPEND`/`_O_APPEND`; the freestanding
    # fixture above covers the syscall/kernel32 path.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)
      add_test(NAME oskel.build_fs_hosted
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_fs_hosted"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_fs.oskl"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_extern.cmake")
    endif()

    # A scalar `extern "C"` declaration resolves through the system linker in
    # the hosted mode (PHASE 7f.5): the backend emits a marshalling thunk per
    # symbol, seeds it as a function and lets the host C compiler resolve the
    # name. These fixtures cover the full System V AMD64 surface (structs,
    # callbacks and a C variadic), so they stay Linux x86-64 only; Windows has
    # its own positional-ABI fixtures below. The same fixture must still fail
    # the freestanding build (that backend has no host), which
    # `oskel.build_extern_error` checks.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      add_test(NAME oskel.build_hosted_extern
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_hosted.oskl"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_extern.cmake")
      # An opaque `ptr` return (PHASE 7f.6b) is boxed as a `Void` handle whose
      # payload is the address, and a NULL becomes nil -- the hosted engine's
      # own rule. `getenv`/`malloc` hand one out and `memset` takes it back, so
      # the fixture proves the address is real rather than a synthetic token.
      add_test(NAME oskel.build_hosted_extern_ptr_return
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_ptr_return"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_ptr_return.oskl"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_extern.cmake")
      # A struct crossing the boundary by value (PHASE 7f.7) goes through the
      # same thunk machinery, so the hosted binary must agree with the VM there
      # too: `div`/`ldiv` return one and `cabs` takes one.
      add_test(NAME oskel.build_hosted_extern_struct
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_struct"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_struct.oskl"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_extern.cmake")
      # The project's own linker (PHASE 10.1) closes the hosted object without a
      # C toolchain. Each fixture is linked three ways -- in process, through
      # `--emit-object` + `oskelld`, and through `oskelc` -- and every output
      # must match the VM byte for byte.
      foreach(own_fixture
          native_build_hello
          native_build_extern_hosted
          native_build_extern_ptr_return
          native_build_extern_struct)
        add_test(NAME oskel.build_hosted_own_${own_fixture}
          COMMAND ${CMAKE_COMMAND}
            "-DOSKEL_TEST_NAME=oskel.build_hosted_own_${own_fixture}"
            "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
            "-DOSKELC_EXE=$<TARGET_FILE:oskelc>"
            "-DOSKELD_EXE=$<TARGET_FILE:oskelld>"
            "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/${own_fixture}.oskl"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_own.cmake")
      endforeach()
      # A shared library no system library provides: `--link-lib` is what makes
      # the symbol resolve, so this exercises the SONAME lookup and the
      # `.dynsym` check instead of libm (which the linker detects on its own).
      add_library(oskel_link_lib_helper SHARED
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/link_lib_helper.cpp")
      set_target_properties(oskel_link_lib_helper PROPERTIES
        OUTPUT_NAME oskel_link_lib_helper
        SOVERSION 1)
      # The fixture is loaded into a plain emitted binary (`LD_LIBRARY_PATH`)
      # and preloaded into the VM (`LD_PRELOAD`). An instrumented copy would
      # drag the sanitizer runtime into a process that did not start with it,
      # which ASan rejects ("runtime does not come first in initial library
      # list"). Keep the C ABI fixture uninstrumented; the linker and the VM
      # are what the sanitizer job is testing.
      if(CMAKE_CXX_FLAGS MATCHES "-fsanitize")
        target_compile_options(oskel_link_lib_helper PRIVATE -fno-sanitize=all)
        target_link_options(oskel_link_lib_helper PRIVATE -fno-sanitize=all)
      endif()
      add_test(NAME oskel.build_hosted_own_link_lib
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_own_link_lib"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKELC_EXE=$<TARGET_FILE:oskelc>"
          "-DOSKELD_EXE=$<TARGET_FILE:oskelld>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_link_lib.oskl"
          "-DOSKEL_LINK_LIB=oskel_link_lib_helper"
          "-DOSKEL_LIBRARY_PATH=$<TARGET_FILE_DIR:oskel_link_lib_helper>"
          "-DOSKEL_EXPECTED_OUTPUT=link-lib ok"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_own.cmake")
      # PHASE 10.2: a second object (`--link-object`, `oskelld` with several
      # inputs, `-r` and an `ar` archive). The helper is a plain C-ABI object;
      # its long member name in the archive exercises the `//` table.
      add_library(oskel_link_obj_helper OBJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/link_obj_helper.cpp")
      if(CMAKE_CXX_FLAGS MATCHES "-fsanitize")
        # The object is linked into a plain emitted binary, which does not
        # carry the sanitizer runtime.
        target_compile_options(oskel_link_obj_helper PRIVATE -fno-sanitize=all)
      endif()
      find_program(OSKEL_AR NAMES ar llvm-ar)
      if(NOT OSKEL_AR)
        set(OSKEL_AR "")
      endif()
      add_test(NAME oskel.build_hosted_own_link_obj
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_own_link_obj"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKELC_EXE=$<TARGET_FILE:oskelc>"
          "-DOSKELD_EXE=$<TARGET_FILE:oskelld>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_link_obj.oskl"
          "-DOSKEL_HELPER_OBJECT=$<TARGET_OBJECTS:oskel_link_obj_helper>"
          "-DOSKEL_EXPECTED_OUTPUT=42"
          "-DOSKEL_AR=${OSKEL_AR}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_own_multi.cmake")
      # PHASE 10.2b: .rodata, .data and .bss, including a pointer table whose
      # entries are R_X86_64_64 relocations against .rodata section symbols.
      # The second helper is -fPIC, so its globals travel through the GOT.
      add_library(oskel_link_data_helper OBJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/link_data_helper.cpp")
      set_target_properties(oskel_link_data_helper PROPERTIES
        POSITION_INDEPENDENT_CODE OFF)
      target_compile_options(oskel_link_data_helper PRIVATE -fno-pie)
      add_library(oskel_link_data_pic_helper OBJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/link_data_helper.cpp")
      set_target_properties(oskel_link_data_pic_helper PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
      target_compile_options(oskel_link_data_pic_helper PRIVATE -fPIC)
      if(CMAKE_CXX_FLAGS MATCHES "-fsanitize")
        target_compile_options(oskel_link_data_helper PRIVATE -fno-sanitize=all)
        target_compile_options(oskel_link_data_pic_helper PRIVATE -fno-sanitize=all)
      endif()
      add_test(NAME oskel.build_hosted_own_link_data
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_own_link_data"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKELC_EXE=$<TARGET_FILE:oskelc>"
          "-DOSKELD_EXE=$<TARGET_FILE:oskelld>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_link_data.oskl"
          "-DOSKEL_HELPER_OBJECT=$<TARGET_OBJECTS:oskel_link_data_helper>"
          "-DOSKEL_EXPECTED_OUTPUT=data ok 8 9 0 alpha beta"
          "-DOSKEL_AR=${OSKEL_AR}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_own_multi.cmake")
      add_test(NAME oskel.build_hosted_own_link_data_pic
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_own_link_data_pic"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKELC_EXE=$<TARGET_FILE:oskelc>"
          "-DOSKELD_EXE=$<TARGET_FILE:oskelld>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_link_data.oskl"
          "-DOSKEL_HELPER_OBJECT=$<TARGET_OBJECTS:oskel_link_data_pic_helper>"
          "-DOSKEL_EXPECTED_OUTPUT=data ok 8 9 0 alpha beta"
          "-DOSKEL_AR=${OSKEL_AR}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_own_multi.cmake")
      # A class-MEMORY struct (larger than two eightbytes) is stack-passed and
      # `sret`-returned, a shape no libc symbol exercises: the fixture links a
      # small C helper of its own.
      #
      # The compiler only *accepts* such a declaration when the development
      # engine has the descriptor stub (`OSKEL_FFI_MEMORY_STRUCTS`, mirroring
      # `OSKEL_FFI_MEMORY_STRUCTS` in `include/ffi_types.h`); without it the
      # build fails by design with the "needs the x86-64 FFI trampoline"
      # message, so the test has to be gated on the same flag.
      if(OSKEL_FFI_MEMORY_STRUCTS)
        add_test(NAME oskel.build_hosted_extern_memory_struct
          COMMAND ${CMAKE_COMMAND}
            "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_memory_struct"
            "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
            "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_memory_struct.oskl"
            "-DOSKEL_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/memory_struct_helper.c"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_helper.cmake")
      endif()
      # The seventh and eighth integer arguments do not fit the six System V
      # GPRs, so the thunk spills them to the outgoing stack area; a
      # register-class struct that no longer fits is passed wholly on the stack.
      # No libc symbol exercises those shapes, so the fixture links its own C
      # helper. It needs only the hosted link mode.
      add_test(NAME oskel.build_hosted_extern_many_args
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_many_args"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_many_args.oskl"
          "-DOSKEL_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/many_args_helper.c"
          "-DOSKEL_EXPECTED_OUTPUT=ffi many args ok"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_helper.cmake")
      # A callback is a C function pointer. The VM emits a closure stub per
      # registration whose body boxes the C arguments and calls the Oskel
      # closure; the AOT backend does the same. `qsort` invokes the comparator
      # from libc, so the reference is the VM.
      if(OSKEL_FFI_CALLBACKS)
        add_test(NAME oskel.build_hosted_extern_callback
          COMMAND ${CMAKE_COMMAND}
            "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_callback"
            "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
            "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_callback.oskl"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_extern.cmake")
      endif()
      # Registration and invocation are separate extern calls, so the callback
      # runs after the program has allocated again: the body must resume from
      # the program's current arena cursor, not the one at registration.
      add_test(NAME oskel.build_hosted_extern_callback_stored
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_callback_stored"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_callback_stored.oskl"
          "-DOSKEL_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/callback_helper.c"
          "-DOSKEL_EXPECTED_OUTPUT=stored callback ok"
          "-DOSKEL_VM_PRELOAD=ON"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_helper.cmake")
      # Seven arguments fill the six GPRs and the first stack slot; eight reach
      # the second. The body reads both from its caller's frame.
      add_test(NAME oskel.build_hosted_extern_callback_many_args
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_callback_many_args"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_callback_many_args.oskl"
          "-DOSKEL_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/callback_many_args_helper.c"
          "-DOSKEL_EXPECTED_OUTPUT=many args callback ok"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_helper.cmake")
      # The stub page must be read+execute, never writable+executable.
      add_test(NAME oskel.build_hosted_extern_callback_wx
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_callback_wx"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_callback_wx.oskl"
          "-DOSKEL_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/callback_wx_helper.c"
          "-DOSKEL_EXPECTED_OUTPUT=callback stub wx ok"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_helper.cmake")
      # C invokes this callback from an `atexit` handler after the chunk
      # returns; the root epilogue publishes the final arena state for it.
      add_test(NAME oskel.build_hosted_extern_callback_exit
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_callback_exit"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_callback_exit.oskl"
          "-DOSKEL_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/callback_exit_helper.c"
          "-DOSKEL_EXPECTED_OUTPUT=late callback 1"
          "-DOSKEL_VM_PRELOAD=ON"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_helper.cmake")
      # Every scalar return class the marshalling supports, including a
      # `cstring` (a copy in the arena) and an opaque `ptr`.
      add_test(NAME oskel.build_hosted_extern_callback_returns
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_callback_returns"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_callback_returns.oskl"
          "-DOSKEL_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/callback_returns_helper.c"
          "-DOSKEL_EXPECTED_OUTPUT=callback returns ok"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_helper.cmake")
      # A callback may return a struct by value: the register class (8- and
      # 16-byte structs, INTEGER and SSE eightbytes) and class MEMORY (through
      # the hidden `sret` pointer). The fixture links its own C helper and also
      # runs the VM with that helper preloaded, so the two engines are compared
      # on the same input (BACKLOG §6 item 28).
      add_test(NAME oskel.build_hosted_extern_callback_struct
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_callback_struct"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_callback_struct.oskl"
          "-DOSKEL_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/callback_struct_helper.c"
          "-DOSKEL_EXPECTED_OUTPUT=callback struct return ok"
          "-DOSKEL_VM_PRELOAD=ON"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_helper.cmake")
      add_test(NAME oskel.build_hosted_callback_struct_param_error
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_callback_struct_param_error"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/errors/callback_struct_param.oskl"
          "-DOSKEL_BUILD_MODE=hosted"
          "-DEXPECTED_MESSAGE=callback parameter type 'Point' cannot cross the C boundary"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_error.cmake")
      # A C variadic is walked at run time by the thunk (PHASE 7f.5): the tail
      # continues the GPR/SSE allocation after the fixed prefix, spills to the
      # outgoing stack area and sets `%al` to the vector-register count. The
      # reference is the VM, whose descriptor stub lowers the same tail, so the
      # test is gated on the VM capability.
      if(OSKEL_FFI_VARIADIC)
        add_test(NAME oskel.build_hosted_extern_variadic
          COMMAND ${CMAKE_COMMAND}
            "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_variadic"
            "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
            "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_variadic.oskl"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_extern.cmake")
      endif()
      # A variadic with a mixed fixed prefix. System V assigns the integer and
      # SSE classes independently, so this is the reference the Win64
      # positional path must match; the helper returns a sum, so a wrong
      # register is a wrong number. The helper harness does not run the VM.
      add_test(NAME oskel.build_hosted_extern_variadic_mixed
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_variadic_mixed"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_variadic_mixed.oskl"
          "-DOSKEL_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/variadic_mixed_helper.c"
          "-DOSKEL_EXPECTED_OUTPUT=ffi variadic mixed ok"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_helper.cmake")
    endif()

    # macOS uses the same System V thunks as Linux but a Mach-O object and
    # libSystem. The image is x86-64, so an arm64 host builds it but does not
    # run it (that would need Rosetta); the link and `otool -L` still prove the
    # Mach-O relocation and the libSystem dependency.
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
      set(OSKEL_MACOS_SKIP_RUN 0)
      if(NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
        set(OSKEL_MACOS_SKIP_RUN 1)
      endif()
      add_test(NAME oskel.build_hosted_extern_macos
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_macos"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_CC=${OSKEL_HOSTED_CC}"
          "-DOSKEL_SKIP_RUN=${OSKEL_MACOS_SKIP_RUN}"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_hosted.oskl"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_extern.cmake")
      # The System V variadic walk is shared with Linux; this fixture proves the
      # Mach-O object and the libSystem link carry it too. On an arm64 host the
      # x86-64 image is built and linked but not run, so the expected output is
      # used instead of the VM (the Linux fixture already pins VM parity).
      add_test(NAME oskel.build_hosted_extern_variadic_macos
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_variadic_macos"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_CC=${OSKEL_HOSTED_CC}"
          "-DOSKEL_SKIP_RUN=${OSKEL_MACOS_SKIP_RUN}"
          "-DOSKEL_EXPECTED_OUTPUT=ffi variadic aot ok"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_variadic.oskl"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_extern.cmake")
    endif()

    # Windows x86-64 has the same hosted link, but the Microsoft x64 ABI: the
    # scalar/cstring/pointer surface and a C variadic are supported there, with
    # argument positions selecting the registers. Structs and callbacks are
    # still rejected with a dedicated message (PHASE 7f.5), so the Linux
    # fixtures keep covering those aggregate shapes.
    if(WIN32)
      add_test(NAME oskel.build_hosted_extern_windows
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_windows"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_CC=${OSKEL_HOSTED_CC}"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_hosted_windows.oskl"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_extern.cmake")
      # A C variadic is walked at run time by the thunk (PHASE 7f.10): on
      # Win64 the tail continues the argument-position allocation, so a float
      # in a register position goes in both the SSE and the integer register,
      # and position four onward spills to the outgoing stack area after the
      # shadow space. The Windows VM's symbol lookup does not resolve the
      # fixture's `snprintf`, so the fixture asserts its own expectations and
      # the binary's final line is the contract.
      add_test(NAME oskel.build_hosted_extern_variadic_windows
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_variadic_windows"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_CC=${OSKEL_HOSTED_CC}"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_variadic.oskl"
          "-DOSKEL_EXPECTED_OUTPUT=ffi variadic aot ok"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_extern.cmake")
      # A variadic with a mixed fixed prefix is the shape that pins the
      # positional Win64 assignment: the fixed double must be in xmm1 (and
      # rdx) and the tail double in xmm3 (and r9), not in xmm0/xmm1. The
      # helper is compiled by the same `cl` the hosted link drives.
      add_test(NAME oskel.build_hosted_extern_variadic_mixed_windows
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_variadic_mixed_windows"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_CC=${OSKEL_HOSTED_CC}"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_variadic_mixed.oskl"
          "-DOSKEL_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/variadic_mixed_helper.c"
          "-DOSKEL_EXPECTED_OUTPUT=ffi variadic mixed ok"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_helper.cmake")
    endif()

    # The call-site checks (arity and argument types) are compile-time metadata:
    # a program that passes them must build and agree with the VM byte for byte,
    # proving the checks are erased and never change the emitted code.
    add_test(NAME oskel.build_binary_call_checks
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_call_checks"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_call_checks.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `std.json` is plain Oskel over the portable primitive surface, so it must
    # build to a native binary and agree with the VM. The fixture is the guard
    # that keeps the module off the collection/string methods and
    # `pcall`/`error`, which the AOT backend does not implement yet (PHASE 7f).
    add_test(NAME oskel.build_binary_json
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_json"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_json.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `std.datetime` is plain Oskel over the portable primitive surface, plus
    # the `time()` builtin the AOT runtime binds as the `now` handle (a syscall,
    # libc, a Win32 call or the CMOS RTC, one per target). The fixture is the
    # guard that keeps the module off the collection/string methods and
    # `pcall`/`error`, and that proves the wall clock works on every AOT path.
    add_test(NAME oskel.build_binary_datetime
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_datetime"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_datetime.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `std.ui` is plain Oskel over the portable primitive surface: the widget
    # tree, layout, display list and hit testing must agree with the VM.
    add_test(NAME oskel.build_binary_ui
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_ui"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_ui.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `std.path` is plain Oskel over the portable primitive surface, so it must
    # build to a native binary and agree with the VM. The fixture is the guard
    # that keeps the module off the collection/string methods and
    # `pcall`/`error`, which the AOT backend does not implement yet (PHASE 7f).
    add_test(NAME oskel.build_binary_path
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_path"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_path.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `random()`/`seed()` are implemented in the AOT backend as `mt19937_64` in
    # machine code, with the same reduction formulas as the VM (PHASE 7c.5b.6).
    # The fixture reseeds every stream, because only a seeded sequence is
    # reproducible across the two engines: the VM seeds from `std::random_device`
    # and a native binary from the standard's default seed (BACKLOG §6 item 25).
    add_test(NAME oskel.build_binary_random
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_random"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_random.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `pcall` is the only way to recover from a `panic` raised inside a function
    # chosen at run time (BACKLOG §6 item 4): the AOT runtime reports a failure
    # by exiting, so the builtin installs a handler the error sink jumps back
    # into. The fixture exercises a success, a caught failure, a nested catch and
    # a closure, and must agree with the VM line for line.
    add_test(NAME oskel.build_binary_pcall
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_pcall"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_pcall.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # Every fixed-arity builtin rejects a call with the wrong argument count,
    # like the VM (BACKLOG §6 item 25). Without the guard the AOT builtin read
    # its operand from an argument window nobody wrote, so `type()` returned a
    # garbage name and `chr(65, 1)` returned "A" while the VM raised. `pcall`
    # turns each raised error into `{ok, value, error}`, so the fixture can
    # assert the failure end to end and still agree with the VM line for line.
    add_test(NAME oskel.build_binary_builtin_arity
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_builtin_arity"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_builtin_arity.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The callback-taking array methods -- `map`, `filter`, `each`, `find`,
    # `any`, `all` (PHASE 7c.3h). They are the first emitted helpers that call
    # back into the program through `oskel_call`, so the callback runs real
    # Oskel code inside the helper. The fixture pins the whole surface,
    # including `each` returning the receiver and `find` returning nil.
    add_test(NAME oskel.build_binary_array_callbacks
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_array_callbacks"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_array_callbacks.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The folding array methods -- `reduce`, `sum`, `join` (PHASE 7c.3h).
    # `reduce` is the first emitted helper that hands its callback two values,
    # so the fixture pins the argument-window ordering; `sum` pins the all-int
    # fast path and its switch to a double, and `join` pins the `toString`
    # rendering of both the items and the separator.
    add_test(NAME oskel.build_binary_array_folds
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_array_folds"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_array_folds.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `std.collections` is plain Oskel over the portable primitive surface, so
    # it must build to a native binary and agree with the VM. The fixture is the
    # guard that keeps the module off `pcall`/`error` and the rest of the AOT
    # gaps.
    add_test(NAME oskel.build_binary_collections
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_collections"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_collections.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `std.strings` is plain Oskel over the portable primitive surface, so it
    # must build to a native binary and agree with the VM. The fixture pins
    # slicing, code-point indices and the padding/shortening arithmetic.
    add_test(NAME oskel.build_binary_strings
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_strings"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_strings.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `std.bytes` is plain Oskel over arrays and code points, so the UTF-8,
    # hexadecimal, Base64 and fixed-width codecs must build to a native binary
    # and agree with the VM.
    add_test(NAME oskel.build_binary_bytes
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_bytes"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_bytes.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `std.decimal` is a pure-Oskel BigDecimal over arrays and strings, so its
    # arbitrary-precision arithmetic must build to a native binary and agree
    # with the VM.
    add_test(NAME oskel.build_binary_decimal
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_decimal"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_decimal.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `std.io` is plain Oskel over strings, and its readers and writers are
    # ordinary classes (maps with closures), so it must build to a native binary
    # and agree with the VM. The fixture also pins that an instance travels
    # through a function.
    add_test(NAME oskel.build_binary_io
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_io"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_io.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # Regression for the string-builder corruption: `oskel_sb_append_escaped`
    # kept its loop index in r13, the arena cursor (`kAotArenaCursor`, entry.h),
    # and pushed/popped it around the allocating `oskel_sb_append`, so a
    # collection whose rendered text crossed the builder's 64-byte initial
    # capacity came out garbled. This fixture renders arrays and maps of strings
    # well past that size; the harness compares the binary with the VM byte for
    # byte, so the corruption cannot come back silently.
    add_test(NAME oskel.build_binary_string_repr
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_string_repr"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_string_repr.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # A `class` (PHASE 6.8) is sugar the parser erases into a map plus closures
    # capturing `self`, so the AOT backend lowers it with no backend work. The
    # fixture is the guard that keeps the desugaring on the portable primitive
    # surface, so `class` stays identical on the VM and in a native binary.
    add_test(NAME oskel.build_binary_classes
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_classes"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_classes.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # The pointer builtins (BACKLOG §6 item 25): `cstring`/`cstr`/`buffer`/
    # `byteLen`/`loadByte`/`storeByte`/`intToPtr`/`ptrToInt`/`free` are opaque
    # handles an `AotPtr` in the arena backs, so every target emits them. The
    # fixture compares the binary with the VM byte for byte.
    add_test(NAME oskel.build_binary_pointers
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_pointers"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_pointers.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `readFile`/`writeFile`/`fileExists` are written against the prelude's
    # `open`/`readFd`/`writeFd`/`close` handles, so the syscall (Linux) or
    # kernel32 (Windows) difference stays in the prelude. Every target with a
    # file system emits them; bare metal rejects them by name.
    add_test(NAME oskel.build_binary_files
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_files"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_files.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # Streaming file handles behind `std.fs.File` (PHASE 7g): open/read/write/
    # seek/close, append, and the whole-file helpers. The fixture uses the same
    # path in both runs, so `writeText` truncates the file before each one.
    add_test(NAME oskel.build_binary_fs
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_fs"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_fs.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `clock()` reads the `entry.monotonic` handle and subtracts the origin the
    # chunk captured at entry, so it reports elapsed time. Every target backs it,
    # and only the shape is compared: the two runs happen at different times.
    add_test(NAME oskel.build_binary_clock
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_clock"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_clock.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `std.os`/`std.sys` are facades over builtins every target seeds:
    # `platform()`/`arch()` are compile-time names, `time()`/`clock()` read the
    # prelude handles, and `exit` is the shared process exit. The fixture keeps
    # the VM and the binary on the same names.
    add_test(NAME oskel.build_binary_os_sys
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_os_sys"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_os_sys.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # `args()` reads the array the entry point builds from `(argc, argv)`, so it
    # is seeded only where the entry receives one: Linux freestanding and hosted.
    # Windows calls the entry with no arguments and bare metal has no command
    # line, and there the build must reject it by name instead.
    if(NOT WIN32)
      add_test(NAME oskel.build_binary_args
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_TEST_NAME=oskel.build_binary_args"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_args.oskl"
          "-DOSKEL_ARGS=alpha;beta;two words"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")
    else()
      add_test(NAME oskel.build_args_error
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_args_error"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_args_error.oskl"
          "-DEXPECTED_MESSAGE=args\\(\\) needs a process argument vector"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_error.cmake")
    endif()

    # The field-type check runs in the compiler, so `oskel build` rejects a
    # mistyped field assignment exactly as the VM does, with the same message
    # and exit 65, and writes no binary (PHASE 6.8).
    add_test(NAME oskel.build_class_field_type_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_class_field_type_error"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/errors/class_field_type_self.oskl"
        "-DEXPECTED_MESSAGE=cannot assign 'string' to 'int' in field 'x' of class 'Point'"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_error.cmake")

    # A method reassignment and the forgotten-field check both live in the
    # compiler, so `oskel build` rejects them exactly as the VM does, with the
    # same message and exit 65, and writes no binary (PHASE 6.8.1).
    add_test(NAME oskel.build_class_method_reassign_type_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_class_method_reassign_type_error"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/errors/class_method_reassign_type.oskl"
        "-DEXPECTED_MESSAGE=cannot assign 'int' to method 'get' of class 'Point'"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_error.cmake")
    add_test(NAME oskel.build_class_method_reassign_arity_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_class_method_reassign_arity_error"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/errors/class_method_reassign_arity.oskl"
        "-DEXPECTED_MESSAGE=a replacement for method 'get' of class 'Point' must take 0 parameters, got 1"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_error.cmake")
    add_test(NAME oskel.build_class_uninitialized_field_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_class_uninitialized_field_error"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/errors/class_uninitialized_field.oskl"
        "-DEXPECTED_MESSAGE=field 'timeout' of class 'Config' is declared 'int' but 'init' never assigns it"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_error.cmake")
    add_test(NAME oskel.build_class_compound_init_field_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_class_compound_init_field_error"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/errors/class_compound_init_field.oskl"
        "-DEXPECTED_MESSAGE=field 'n' of class 'Counter' is declared 'int' but 'init' only updates it in place"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_error.cmake")

    # The hardened `class` surface must stay on the portable primitive surface,
    # so this fixture proves field defaults, method replacement and the
    # initializer check all build and agree with the VM (PHASE 6.8.1).
    add_test(NAME oskel.build_binary_class_hardening
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_class_hardening"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/60_class_hardening.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # An `extern "C"` symbol is a host binding the AOT backend cannot resolve
    # (the FFI lowering is PHASE 7f), so `oskel build` must fail with the
    # dedicated message and write no binary.
    add_test(NAME oskel.build_extern_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_extern_error"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern.oskl"
        "-DEXPECTED_MESSAGE=extern \"C\" symbol 'getpid' cannot be resolved by the AOT backend"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_error.cmake")
    # A UI program selects the hosted link mode automatically, so an explicit
    # `--mode freestanding` is the remaining rejection: it must fail with the
    # dedicated message and write no binary. The fixture imports the module, so
    # it also pins that the mode is chosen from the program, not from a flag.
    add_test(NAME oskel.build_sdl_freestanding_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_sdl_freestanding_error"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_sdl_freestanding.oskl"
        "-DOSKEL_BUILD_MODE=freestanding"
        "-DEXPECTED_MESSAGE=std\\.ui requires a hosted binary"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_error.cmake")
    # `--link-lib` without `--mode hosted`, and a name that is not a library
    # name, are usage errors (exit 64), not silently accepted flags.
    add_test(NAME oskel.build_link_lib_usage_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_sdl_freestanding.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_usage_error.cmake")
    # A `native fn` the AOT backend has no stub for fails the same way, with a
    # message that says `native fn` rather than `extern "C"` (PHASE 7f.1).
    add_test(NAME oskel.build_native_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_native_error"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_native_error.oskl"
        "-DEXPECTED_MESSAGE=native fn 'getpid' cannot be resolved by the AOT backend"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_error.cmake")

    # A qualified access to a module's native export
    # (`modules.hostclock.getpid()`) reads the module table with `GETTABLE`, so
    # before PHASE 7f.2 the lowerer could not see the use and emitted a binary
    # that faulted at run time. The compiler now records the use, so the build
    # must fail with the same dedicated message.
    add_test(NAME oskel.build_native_qualified_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_native_qualified_error"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_qualified_error.oskl"
        "-DEXPECTED_MESSAGE=native fn 'getpid' cannot be resolved by the AOT backend"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_error.cmake")

    # A runtime error in a generated binary must behave exactly like the VM:
    # the same stderr text and the same exit code (70), with no recovery. The
    # fixture also runs on the VM as an ordinary script (oskel.aot_runtime_error
    # below), which is the reference this test compares against.
    add_test(NAME oskel.build_runtime_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_runtime_error"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/aot_runtime_error.oskl"
        "-DOSKEL_EXPECTED_EXIT=70"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/aot_runtime_error.cmake")

    # A read of a global before the assignment that fills it must fail at run
    # time with the hosted engine's wording and exit 70, not read the seeded slot
    # as `nil` (BACKLOG §6 item 30). The fixture's assignment exists, so the
    # program compiles and the failure is the runtime one the VM raises.
    add_test(NAME oskel.build_unset_global
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_unset_global"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_unset_global.oskl"
        "-DOSKEL_EXPECTED_EXIT=70"
        "-DOSKEL_EXPECTED_MESSAGE=undefined variable 'first'"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/aot_runtime_error.cmake")

    # The same read whose only assignment sits in a function that is never
    # called: the assignment exists in the program, so the read still compiles,
    # and the slot is unset at run time. This is why the analysis runs across
    # every proto and not just the root chunk.
    add_test(NAME oskel.build_unset_global_unreached
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_unset_global_unreached"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_unset_global_fn.oskl"
        "-DOSKEL_EXPECTED_EXIT=70"
        "-DOSKEL_EXPECTED_MESSAGE=undefined variable 'later'"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/aot_runtime_error.cmake")

    # The other side of the check: `nil` is an assignable value, and a read after
    # an assignment reads the value. Both keep the unchecked `globalGet`; a
    # regression that checked every read would fail the first case.
    add_test(NAME oskel.build_global_nil
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_global_nil"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_global_nil.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # A builtin is a global, so a declaration with a builtin's name replaces it
    # (language.md §4.2). `collectGlobals` pre-assigns the builtin slots first,
    # so the user's assignment reuses that slot: the value read back is the
    # user's, not the builtin's. A backend that allocated a second slot would
    # keep the builtin reachable and disagree with the VM.
    add_test(NAME oskel.build_builtin_shadow
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_builtin_shadow"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_builtin_shadow.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

    # Calling a shadowed builtin reaches the shadowing value, so a non-function
    # callee fails at run time with the VM's wording and exit code -- never by
    # falling back to the builtin the name used to hold.
    add_test(NAME oskel.build_builtin_shadow_error
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_builtin_shadow_error"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_builtin_shadow_error.oskl"
        "-DOSKEL_EXPECTED_EXIT=70"
        "-DOSKEL_EXPECTED_MESSAGE=cannot call a value of type"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/aot_runtime_error.cmake")
  endif()

  # --- WebAssembly backend (PHASE 9) ---
  # The WASM target is platform-independent: the module runs on any host with a
  # WebAssembly engine, so these tests are not behind the x86-64 guard above.
  # They need Node (the host for the two imports the module declares); where
  # Node is absent the tests are skipped, like `vscode_diagnostics`.
  find_program(OSKEL_NODE_EXE NAMES node nodejs)
  if(OSKEL_NODE_EXE)
    add_test(NAME oskel.build_wasm
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_wasm.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `range` takes one or two bounds: `range(5)` starts at 0. The fixture also
    # pins `assertEq` and `exit`, so a wrong arity check in the WASM builtin
    # cannot pass unnoticed.
    add_test(NAME oskel.build_wasm_builtins
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_builtins"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_builtins.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `push` and `pop` are bound method closures that capture their receiver,
    # so a detached method still mutates the same array.
    add_test(NAME oskel.build_wasm_array_methods
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_array_methods"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_array_methods.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `shift`, `unshift`, `insert`, `removeAt`, `contains`, `indexOf` and
    # `clear` mutate or scan the captured receiver in place.
    add_test(NAME oskel.build_wasm_array_methods_inplace
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_array_methods_inplace"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_array_methods_inplace.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `slice`, `concat`, `reversed` and `clone` allocate a fresh array and
    # leave the receiver untouched.
    add_test(NAME oskel.build_wasm_array_methods_alloc
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_array_methods_alloc"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_array_methods_alloc.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `map`, `filter`, `each`, `find`, `any`, `all` and `reduce` call back into
    # the program through the dispatcher.
    add_test(NAME oskel.build_wasm_array_callbacks
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_array_callbacks"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_array_callbacks.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `reduce`, `sum` and `join`, the folds over the captured receiver.
    add_test(NAME oskel.build_wasm_array_folds
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_array_folds"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_array_folds.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `sort`/`sorted`, including stability and a comparator that returns a
    # non-bool. `pcall` is still VM-only, so the strong-guarantee case lives in
    # the AOT fixture only.
    add_test(NAME oskel.build_wasm_sort_methods
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_sort_methods"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_wasm_sort_methods.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # The map twin of the array methods: `keys`, `values`, `entries`, `has`,
    # `get`, `set`, `delete`, `merge`, `clone` and `clear`, all receiver-bound.
    add_test(NAME oskel.build_wasm_map_methods
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_map_methods"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_map_methods.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `std.datetime` is deterministic apart from `time()`/`clock()`, which the
    # fixture only observes through stable predicates, so the module must agree
    # with the VM byte for byte too.
    add_test(NAME oskel.build_wasm_datetime
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_datetime"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_datetime.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `std.collections` is pure Oskel over arrays, maps, slicing and closures,
    # so the WASM backend must reproduce it without a special case.
    add_test(NAME oskel.build_wasm_collections
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_collections"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_collections.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `std.strings` is pure Oskel over indexing, slicing, `len` and closures, so
    # the WASM backend must reproduce it without a special case.
    add_test(NAME oskel.build_wasm_strings
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_strings"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_strings.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `std.bytes` is pure Oskel over arrays and code points, so the UTF-8,
    # hexadecimal, Base64 and fixed-width codecs must also match the VM in a
    # WebAssembly module.
    add_test(NAME oskel.build_wasm_bytes
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_bytes"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_bytes.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `std.decimal` is pure Oskel over arrays and strings, so its
    # arbitrary-precision arithmetic must also match the VM in WebAssembly.
    add_test(NAME oskel.build_wasm_decimal
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_decimal"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_decimal.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # The bound string methods (`upper`, `trim`, `split`, `replace`,
    # `padStart`, ...) over ASCII and multibyte UTF-8. The fixture is the same
    # one the AOT backend compares with the VM, so both backends share the
    # expectations.
    add_test(NAME oskel.build_wasm_string_methods
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_string_methods"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_string_methods.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # The bound range methods (`toArray`, `contains`) and the saturated
    # `.length` of a span wider than INT64_MAX. The error cases stay in the
    # AOT fixture because `pcall` is VM-only.
    add_test(NAME oskel.build_wasm_range_methods
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_range_methods"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_wasm_range_methods.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # Pointer handles (`cstring`, `buffer`, `cstr`, `byteLen`, `loadByte`,
    # `storeByte`, `intToPtr`, `ptrToInt`, `free`) in linear memory. The error
    # cases stay in the AOT fixture because `pcall` is VM-only.
    add_test(NAME oskel.build_wasm_pointers
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_pointers"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_wasm_pointers.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # Runtime tree-shaking (PHASE 9.4): a program that never reads a bound
    # method must not carry the method helpers or the Unicode case tables, and
    # a static member read must keep the helper it needs. The fixture is
    # generated by the script so it does not enter the VM's script glob.
    add_test(NAME oskel.build_wasm_tree_shake
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_tree_shake.cmake")

    # `std.io` is pure Oskel over strings and classes (maps with closures), so
    # the WASM backend must reproduce it without a special case.
    add_test(NAME oskel.build_wasm_io
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_io"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_io.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # `std.math` exercises the shared shortest round-trip formatter through
    # `PI`, `E` and `sqrt`, including the scientific and subnormal forms.
    add_test(NAME oskel.build_wasm_math
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_math"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_math.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # The shortest round-trip formatter itself, over the whole double range.
    add_test(NAME oskel.build_wasm_double_format
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_TEST_NAME=oskel.build_wasm_double_format"
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_double_format.oskl"
        "-DNODE_EXE=${OSKEL_NODE_EXE}"
        "-DWASM_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_wasm.cjs"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_case.cmake")

    # A mis-lowered loop must fail the test, not hang the suite: the Node run
    # gets a hard timeout in `wasm_case.cmake`, and CTest bounds the driver.
    set_tests_properties(oskel.build_wasm oskel.build_wasm_builtins
      oskel.build_wasm_array_methods oskel.build_wasm_array_methods_inplace
      oskel.build_wasm_array_methods_alloc oskel.build_wasm_array_callbacks
      oskel.build_wasm_array_folds oskel.build_wasm_sort_methods
      oskel.build_wasm_map_methods oskel.build_wasm_datetime
      oskel.build_wasm_collections oskel.build_wasm_strings
      oskel.build_wasm_string_methods oskel.build_wasm_range_methods
      oskel.build_wasm_pointers
      oskel.build_wasm_io oskel.build_wasm_math
      oskel.build_wasm_double_format oskel.build_wasm_tree_shake PROPERTIES TIMEOUT 120)
  endif()

  # --- .oskl script tests (auto-detected) ---
  file(GLOB oskel_tests CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.oskl"
  )
  foreach(script IN LISTS oskel_tests)
    get_filename_component(name "${script}" NAME_WE)
    # `sdl_bindings` needs SDL3 loaded and the dummy video driver; it is
    # registered above with its own runner, and skipped when the build has no
    # SDL3.
    if(name STREQUAL "sdl_bindings")
      continue()
    endif()
    # `native_build_sdl_freestanding` is a `oskel build` rejection fixture, not
    # a VM test: running it would try to open a window (and needs SDL3 loaded).
    if(name STREQUAL "native_build_sdl_freestanding")
      continue()
    endif()
    # `native_build_oskc_ui` imports `std.ui.window` only to make the compiler
    # load the UI modules, so the `.oskc` header records their requirement. It
    # is driven by `tests/oskc_ui_requirement.cmake`, which compares the source
    # and image paths; running it on the VM would need SDL3 and a video driver.
    if(name STREQUAL "native_build_oskc_ui")
      continue()
    endif()
    # FFI callbacks need the x86-64 closure stub; skip the callback test when
    # this build does not emit one (the feature reports a clear error then).
    if(name STREQUAL "32_ffi_callback" AND NOT OSKEL_FFI_CALLBACKS)
      continue()
    endif()
    # Struct-by-value FFI needs the System V AMD64 ABI; see the capability above.
    if((name STREQUAL "31_ffi_struct" OR name STREQUAL "native_build_extern_struct") AND
       NOT OSKEL_FFI_STRUCT_BY_VALUE)
      continue()
    endif()
    # The class-MEMORY fixture binds functions from `tests/hosted/`, which the
    # VM cannot resolve (its host registry knows libc, not the test helper). It
    # is covered end-to-end by `oskel.build_hosted_extern_memory_struct`.
    if(name STREQUAL "native_build_extern_memory_struct")
      continue()
    endif()
    # The many-args fixture binds a helper too; the VM has no registry entry for
    # those symbols, so it is covered by `oskel.build_hosted_extern_many_args`.
    if(name STREQUAL "native_build_extern_many_args")
      continue()
    endif()
    # The mixed variadic fixture binds a helper the VM cannot resolve; its
    # hosted tests (`..._variadic_mixed` and the Windows twin) are the coverage.
    if(name STREQUAL "native_build_extern_variadic_mixed")
      continue()
    endif()
    # `native_build_link_lib` binds a symbol only the helper shared library
    # provides, so the VM would need it preloaded. The own-linker test is the
    # coverage: it preloads the library into the VM and sets `LD_LIBRARY_PATH`
    # for the linked binary.
    if(name STREQUAL "native_build_link_lib")
      continue()
    endif()
    # `native_build_link_obj` binds a helper object the VM cannot resolve;
    # `oskel.build_hosted_own_link_obj` is the coverage.
    if(name STREQUAL "native_build_link_obj")
      continue()
    endif()
    # `native_build_link_data` binds a helper object with .data/.bss the VM
    # cannot resolve; `oskel.build_hosted_own_link_data` is the coverage.
    if(name STREQUAL "native_build_link_data")
      continue()
    endif()
    # The stored/many-args/returns/struct callback fixtures bind helpers the VM
    # cannot resolve on its own; their hosted tests are the coverage, and the
    # stored, exit and struct ones also run the VM with the helper preloaded.
    if(name STREQUAL "native_build_extern_callback_stored" OR
       name STREQUAL "native_build_extern_callback_exit" OR
       name STREQUAL "native_build_extern_callback_wx" OR
       name STREQUAL "native_build_extern_callback_many_args" OR
       name STREQUAL "native_build_extern_callback_returns" OR
       name STREQUAL "native_build_extern_callback_struct")
      continue()
    endif()
    # The qsort callback fixture needs the x86-64 closure stub in the
    # development engine; the hosted test is gated on the same flag.
    if(name STREQUAL "native_build_extern_callback" AND NOT OSKEL_FFI_CALLBACKS)
      continue()
    endif()
    # The two `native fn` rejection fixtures are inputs for
    # `tests/build_error.cmake`. They are build-only on purpose: the VM resolves
    # a `native fn` from the builtin registry, so the symbol has to be a VM
    # builtin to run there -- and since BACKLOG §6 item 25 every VM builtin is
    # also seeded in AOT, which is exactly what makes the rejection
    # unreachable for one. `lower_test` covers the message and
    # `native_build_extern.oskl` covers the VM-runnable shape.
    if(name STREQUAL "native_build_native_error" OR
       name STREQUAL "native_build_qualified_error")
      continue()
    endif()
    # `native_build_extern` is an input for `tests/build_error.cmake`. It also
    # runs on the VM as an ordinary script, but only where the host resolves
    # its `extern "C"` symbol (`getpid`): on Windows the VM cannot bind it, so
    # the script is skipped there. The hosted fixture binds the same libc
    # symbols, so it is skipped there for the same reason.
    if((name STREQUAL "native_build_extern" OR
        name STREQUAL "native_build_extern_hosted" OR
        name STREQUAL "native_build_extern_ptr_return") AND WIN32)
      continue()
    endif()
    # The variadic fixture uses `snprintf`, which the VM's Windows symbol lookup
    # does not resolve; its Windows hosted twin covers the native shape.
    if(name STREQUAL "native_build_extern_variadic" AND WIN32)
      continue()
    endif()
    # C variadics need the x86-64 descriptor stub; the declaration is rejected
    # at compile time elsewhere, so the test is skipped.
    if((name STREQUAL "53_ffi_variadic" OR name STREQUAL "native_build_extern_variadic") AND
       NOT OSKEL_FFI_VARIADIC)
      continue()
    endif()
    # Win32 `extern "C"` symbols live in kernel32.dll, which only exists on
    # Windows; elsewhere `lookupSymbol` cannot resolve them, so skip.
    if(name STREQUAL "54_ffi_win32" AND NOT WIN32)
      continue()
    endif()
    # The unset-global fixtures are inputs for the AOT runtime-error harness:
    # they exit 70 by design, so the VM glob (which expects 0) must skip them.
    # `native_build_global_nil` is the passing counterpart and stays in the glob.
    # `native_build_builtin_shadow_error` is skipped for the same reason: it
    # exits 70 on purpose, while its passing counterpart stays in the glob.
    if(name STREQUAL "native_build_unset_global" OR
       name STREQUAL "native_build_unset_global_fn" OR
       name STREQUAL "native_build_builtin_shadow_error")
      continue()
    endif()
    oskel_add_script_test("oskel.${name}" "${script}")
  endforeach()

  # --- Script arguments (`args()`) ---
  # `args()` is the arguments that follow the script name, not the script name
  # itself (like Lua's `arg` table, unlike C's argv). The runner passes extra
  # arguments after the script path, which the CLI treats as script arguments.
  add_test(NAME oskel.args
    COMMAND ${CMAKE_COMMAND}
      "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
      "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/args/args.oskl"
      "-DOSKEL_ARGS=one two three"
      -DEXPECTED_EXIT=0
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_oskel_case.cmake")

  # --- examples/*.oskl (auto-detected) ---
  file(GLOB oskel_examples CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.oskl"
  )
  foreach(script IN LISTS oskel_examples)
    get_filename_component(name "${script}" NAME_WE)
    # `tui_editor` is a POSIX-only interactive program (it drives the terminal
    # through `extern "C"` and reads from `/tmp`); it is registered separately
    # for UNIX below, and skipped here so Windows stays green.
    if(name STREQUAL "tui_editor")
      continue()
    endif()
    oskel_add_script_test("examples.${name}" "${script}")
  endforeach()

  # --- examples/tui_editor.oskl (UNIX only) ---
  # The terminal editor needs `stty`, `isatty` and `read`; on a non-tty stdin it
  # runs its deterministic self-test, which is what CTest executes here.
  if(UNIX)
    oskel_add_script_test("examples.tui_editor"
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/tui_editor.oskl")
  endif()

  # --- tests/errors/*.oskl (auto-detected) ---
  file(GLOB oskel_error_tests CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/errors/*.oskl"
  )
  foreach(script IN LISTS oskel_error_tests)
    get_filename_component(name "${script}" NAME_WE)
    # `extern_struct_too_large` asserts the *rejection* of a class-MEMORY struct,
    # which only holds where the descriptor stub is unavailable; where it is
    # available the declaration compiles and the rejection case is gone.
    if(name STREQUAL "extern_struct_too_large" AND OSKEL_FFI_MEMORY_STRUCTS)
      continue()
    endif()
    # This one is a `oskel build --mode hosted` rejection input, not a VM
    # script: the struct-param declaration does not compile. Its coverage is
    # `oskel.build_hosted_callback_struct_param_error`.
    if(name STREQUAL "callback_struct_param")
      continue()
    endif()
    oskel_add_script_test("oskel.errors.${name}" "${script}")
  endforeach()

  # --- tests/check/*.oskl: run under `-c` (compile only, never execute) ---
  # `-c` must run the whole compiler (types and imports included) and print
  # nothing; the runner checks the exit code and that stdout is empty.
  file(GLOB oskel_check_tests CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/check/*.oskl"
  )
  foreach(script IN LISTS oskel_check_tests)
    get_filename_component(name "${script}" NAME_WE)
    # A class-MEMORY struct only compiles where the descriptor stub is
    # available; elsewhere the same declaration is a compile error.
    if(name STREQUAL "ffi_memory_struct" AND NOT OSKEL_FFI_MEMORY_STRUCTS)
      continue()
    endif()
    # A callback that returns a struct needs the by-value struct marshalling
    # (and, for a struct over 16 bytes, the descriptor stub); elsewhere the same
    # declaration is a compile error.
    if(name STREQUAL "ffi_callback_struct_return" AND NOT OSKEL_FFI_STRUCT_BY_VALUE)
      continue()
    endif()
    set(expected 0)
    file(STRINGS "${script}" first_line LIMIT_COUNT 1)
    string(REGEX MATCH "expect-exit:[ \t]*([0-9]+)" declared_exit "${first_line}")
    if(declared_exit)
      set(expected "${CMAKE_MATCH_1}")
    endif()
    add_test(NAME "oskel.check.${name}"
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${script}"
        "-DEXPECTED_EXIT=${expected}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_check_case.cmake")
  endforeach()

endif()