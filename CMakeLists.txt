cmake_minimum_required(VERSION 3.15)

project(oskel VERSION 0.1.0 LANGUAGES CXX)

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
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  set(OSKEL_WARNINGS /W4)
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

# The two entry points are not part of either library.
set(OSKEL_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
set(OSKEL_COMPILER_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/oskelc.cpp")

# Configure-time guard, the inverse of the old one: a `.cpp` dropped straight
# into `src/` belongs to no library and would be compiled by nothing, or — worse
# — could be added to the front end by accident. Every source must be in one of
# the three directories, so the classification cannot be skipped.
file(GLOB OSKEL_STRAY_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM OSKEL_STRAY_SOURCES "${OSKEL_MAIN}" "${OSKEL_COMPILER_MAIN}")
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
# `Platform::lookupSymbol` resolves `extern "C"` declarations with `dlsym`
# (PHASE 6.6). Empty on platforms that fold libdl into libc (glibc 2.34+,
# Windows), so linking it unconditionally is safe and portable.
target_link_libraries(oskel_frontend PUBLIC ${CMAKE_DL_LIBS})

add_library(oskel_lib STATIC ${OSKEL_RUNTIME_SOURCES})
target_link_libraries(oskel_lib PUBLIC oskel_frontend)
target_compile_options(oskel_lib PRIVATE ${OSKEL_WARNINGS})

# --------------------------------------------------------------------------- libffi (optional)
# FFI callbacks (PHASE 6.6) need a trampoline that C can call back into the VM.
# The development engine uses libffi for that; the AOT backend lowers callbacks
# to compile-time stubs and never links libffi (DEVELOPMENT.md §5.1/§6.6). The build
# stays green without libffi: `OSKEL_HAVE_LIBFFI` is only defined when found, and
# the callback path is compiled out otherwise (a clear runtime error instead).
#
# There is no FindLibFFI module and pkg-config may be absent, so probe the
# library and the header directly. The header lives under the multiarch dir on
# Debian/Ubuntu (/usr/include/<triplet>/ffi.h), so that path is hinted too.
option(OSKEL_ENABLE_LIBFFI "Use libffi for FFI callbacks in the dev engine" ON)
if(OSKEL_ENABLE_LIBFFI)
  find_library(FFI_LIBRARY NAMES ffi libffi)
  find_path(FFI_INCLUDE_DIR NAMES ffi.h
    PATH_SUFFIXES ${CMAKE_LIBRARY_ARCHITECTURE}
    PATHS /usr/include /usr/local/include)
  if(FFI_LIBRARY AND FFI_INCLUDE_DIR)
    set(OSKEL_LIBFFI_FOUND TRUE)
    # The compiler reads `OSKEL_HAVE_LIBFFI` through `include/ffi_types.h` to
    # decide which `extern "C"` shapes it may accept (a variadic, a MEMORY
    # struct), so the definition belongs to the front end and must reach both
    # binaries: `oskelc` has to reject exactly what `oskel` rejects.
    target_compile_definitions(oskel_frontend PUBLIC OSKEL_HAVE_LIBFFI)
    # Only the runtime's marshalling includes `<ffi.h>` and links the library.
    target_include_directories(oskel_lib PRIVATE ${FFI_INCLUDE_DIR})
    target_link_libraries(oskel_lib PUBLIC ${FFI_LIBRARY})
    message(STATUS "libffi found: FFI callbacks enabled in the dev engine")
  else()
    set(OSKEL_LIBFFI_FOUND FALSE)
    message(STATUS "libffi not found: FFI callbacks disabled (set OSKEL_ENABLE_LIBFFI=OFF to silence)")
  endif()
endif()

# Struct-by-value FFI is written for the System V AMD64 ABI only; the Microsoft
# x64 ABI passes a struct larger than 8 bytes through a hidden pointer, a shape
# the register thunks cannot express. The compiler rejects such a declaration
# with a clear error on other targets, and the test that exercises it is skipped.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$" AND NOT WIN32)
  set(OSKEL_FFI_STRUCT_BY_VALUE TRUE)
else()
  set(OSKEL_FFI_STRUCT_BY_VALUE FALSE)
endif()

# A struct larger than 16 bytes is class MEMORY (passed on the stack, returned
# through a hidden pointer). Only the libffi-backed call path expresses that, so
# it needs the System V AMD64 ABI *and* libffi in the dev engine; elsewhere the
# declaration is rejected at compile time and the test that exercises the
# rejection stays. This must mirror `OSKEL_FFI_MEMORY_STRUCTS` in
# `include/ffi_types.h`, which derives from `OSKEL_HAVE_LIBFFI`.
if(OSKEL_LIBFFI_FOUND AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$" AND NOT WIN32)
  set(OSKEL_FFI_MEMORY_STRUCTS TRUE)
else()
  set(OSKEL_FFI_MEMORY_STRUCTS FALSE)
endif()

# A C variadic call needs `ffi_prep_cif_var` and the System V AMD64 default
# argument promotions, so it needs libffi *and* that ABI; elsewhere the
# declaration is rejected at compile time and the test that exercises it is
# skipped. This mirrors `OSKEL_FFI_VARIADIC` in `include/ffi_types.h`.
if(OSKEL_LIBFFI_FOUND AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$" AND NOT WIN32)
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

# `oskelc`: the host compiler. Sources in, a native image out; it links the front
# end alone, so the shipped compiler carries neither the VM nor the JIT
# (DEVELOPMENT.md §2). `tests/check_compiler_boundary.cmake` keeps the include
# graph clean and `oskelc_has_no_vm` proves the link stays clean.
add_executable(oskelc "${OSKEL_COMPILER_MAIN}")
target_link_libraries(oskelc PRIVATE oskel_frontend)
target_compile_options(oskelc PRIVATE ${OSKEL_WARNINGS})

install(TARGETS oskel oskelc RUNTIME DESTINATION bin)

# --------------------------------------------------------------------------- bindgen
# The binding generator: a C header in, an Oskel `extern "C"` module out. It is
# a standalone executable that does not link the engine's runtime (it only
# writes text), so it can be built and shipped on its own. Its sources live
# under `tools/`, outside the three `src/` globs, so the generator never ends up
# inside `oskel`.
add_library(bindgen_lib STATIC "${CMAKE_CURRENT_SOURCE_DIR}/tools/bindgen.cpp")
target_include_directories(bindgen_lib PUBLIC include src)
target_compile_options(bindgen_lib PRIVATE ${OSKEL_WARNINGS})

add_executable(bindgen "${CMAKE_CURRENT_SOURCE_DIR}/tools/bindgen_main.cpp")
target_link_libraries(bindgen PRIVATE bindgen_lib)
target_compile_options(bindgen PRIVATE ${OSKEL_WARNINGS})

install(TARGETS bindgen RUNTIME DESTINATION bin)

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
  oskel_add_cpp_test(pe_test)
  oskel_add_cpp_test(lower_test)
  oskel_add_cpp_test(freestanding_test)
  oskel_add_cpp_test(bare_metal_test)

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

  # --- Installed layout: `std/` ships with the binary and resolves ---
  add_test(NAME install_stdlib
    COMMAND ${CMAKE_COMMAND}
      "-DOSKEL_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOSKEL_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DOSKEL_EXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/21_std_math.oskl"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_stdlib.cmake")

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
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_hello.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_mode.cmake")

    # A scalar `extern "C"` declaration resolves through the system linker in
    # the hosted mode (PHASE 7f.5): the backend emits a marshalling thunk per
    # symbol, seeds it as a function and lets `cc` resolve the name. Linux x86-64
    # only: the link needs a host C toolchain and the thunk marshalling is the
    # System V AMD64 ABI. The same fixture must still fail the freestanding build
    # (that backend has no host), which `oskel.build_extern_error` checks.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      add_test(NAME oskel.build_hosted_extern
        COMMAND ${CMAKE_COMMAND}
          "-DOSKEL_TEST_NAME=oskel.build_hosted_extern"
          "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
          "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_hosted.oskl"
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
      # A class-MEMORY struct (larger than two eightbytes) is stack-passed and
      # `sret`-returned, a shape no libc symbol exercises: the fixture links a
      # small C helper of its own.
      #
      # The compiler only *accepts* such a declaration when the development
      # engine has libffi (`OSKEL_FFI_MEMORY_STRUCTS`, mirroring
      # `OSKEL_FFI_MEMORY_STRUCTS` in `include/ffi_types.h`); without it the
      # build fails by design with the "needs libffi" message, so the test has
      # to be gated on the same flag or it fails on every machine without
      # libffi-dev.
      if(OSKEL_FFI_MEMORY_STRUCTS)
        add_test(NAME oskel.build_hosted_extern_memory_struct
          COMMAND ${CMAKE_COMMAND}
            "-DOSKEL_TEST_NAME=oskel.build_hosted_extern_memory_struct"
            "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
            "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_extern_memory_struct.oskl"
            "-DOSKEL_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/hosted/memory_struct_helper.c"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_hosted_memory_struct.cmake")
      endif()
    endif()
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

    # `std.collections` exists because the AOT backend does not bind every array
    # method yet -- `sort`/`sorted` are open (docs/BACKLOG.md §6 item 1); it is
    # plain Oskel over the
    # portable primitive surface, so it must build to a native binary and agree
    # with the VM. The fixture is the guard that keeps the module off the
    # methods, `pcall`/`error` and the rest of the AOT gaps.
    add_test(NAME oskel.build_binary_collections
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_collections"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_collections.oskl"
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

    # `clock()` reads the `entry.monotonic` handle and subtracts the origin the
    # chunk captured at entry, and `args()` reads the array the entry point
    # builds from `(argc, argv)`. Only the shape is compared: the two runs
    # happen at different times.
    add_test(NAME oskel.build_binary_clock_args
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_TEST_NAME=oskel.build_binary_clock_args"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_clock_args.oskl"
        "-DOSKEL_ARGS=alpha;beta;two words"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")

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
  endif()

  # --- .oskl script tests (auto-detected) ---
  file(GLOB oskel_tests CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.oskl"
  )
  foreach(script IN LISTS oskel_tests)
    get_filename_component(name "${script}" NAME_WE)
    # FFI callbacks need libffi in the dev engine; skip the callback test when
    # the build has no libffi (the feature reports a clear runtime error then).
    if(name STREQUAL "32_ffi_callback" AND NOT OSKEL_LIBFFI_FOUND)
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
        name STREQUAL "native_build_extern_hosted") AND WIN32)
      continue()
    endif()
    # C variadics need libffi plus the System V AMD64 ABI; the declaration is
    # rejected at compile time elsewhere, so the test is skipped.
    if(name STREQUAL "53_ffi_variadic" AND NOT OSKEL_FFI_VARIADIC)
      continue()
    endif()
    # Win32 `extern "C"` symbols live in kernel32.dll, which only exists on
    # Windows; elsewhere `lookupSymbol` cannot resolve them, so skip.
    if(name STREQUAL "54_ffi_win32" AND NOT WIN32)
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
    # which only holds where the libffi-backed marshalling is unavailable; where
    # it is available the declaration compiles and the rejection case is gone.
    if(name STREQUAL "extern_struct_too_large" AND OSKEL_FFI_MEMORY_STRUCTS)
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
    # A class-MEMORY struct only compiles where the libffi-backed marshalling is
    # available; elsewhere the same declaration is a compile error.
    if(name STREQUAL "ffi_memory_struct" AND NOT OSKEL_FFI_MEMORY_STRUCTS)
      continue()
    endif()
    # A callback that returns a struct needs the by-value struct marshalling
    # (and, for a struct over 16 bytes, the libffi path); elsewhere the same
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