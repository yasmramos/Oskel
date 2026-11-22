# End-to-end test for the real Skia bridge.
#
# The descriptor and `oskel -c` half needs no Skia. Compiling the shim and
# running the VM/AOT halves needs a Skia root with headers and a built libskia;
# CMake registers this test only when `OSKEL_SKIA_DIR` and `OSKEL_SKIA_LIB`
# point at one, so a checkout without Skia keeps the suite green.
#
# Inputs:
#   BINDGEN_EXE, OSKEL_EXE, CXX_COMPILER, SOURCE_DIR, BUILD_DIR
#   SKIA_DIR    Skia root (contains include/core/SkSurface.h)
#   SKIA_LIB    path to libskia.a or libskia.so
#   SKIA_LIBS   extra link arguments (default: -lpthread -ldl -lm)
#   SDL3_FOUND  whether the build found SDL3 (enables the window half)
#   SDL3_LIBRARY path to the SDL3 shared library, copied into BUILD_DIR so the
#               engine's lazy UI loader finds it

if(NOT EXISTS "${SKIA_DIR}/include/core/SkSurface.h")
  message(FATAL_ERROR "SKIA_DIR does not contain include/core/SkSurface.h: ${SKIA_DIR}")
endif()
if(NOT EXISTS "${SKIA_LIB}")
  message(FATAL_ERROR "SKIA_LIB does not exist: ${SKIA_LIB}")
endif()

file(MAKE_DIRECTORY "${BUILD_DIR}")

execute_process(
  COMMAND "${BINDGEN_EXE}" --cxx "${SOURCE_DIR}/bindings/skia/skia.oskelbind"
          --shim "${BUILD_DIR}" -o "${BUILD_DIR}/skia.oskl"
  RESULT_VARIABLE generate_exit
  OUTPUT_VARIABLE generate_stdout
  ERROR_VARIABLE generate_stderr
)
if(NOT generate_exit EQUAL 0)
  message(FATAL_ERROR
    "bindgen --cxx skia exited ${generate_exit}\n--- stdout ---\n${generate_stdout}\n"
    "--- stderr ---\n${generate_stderr}")
endif()

execute_process(
  COMMAND "${OSKEL_EXE}" -c "${BUILD_DIR}/skia.oskl"
  RESULT_VARIABLE check_exit
  OUTPUT_VARIABLE check_stdout
  ERROR_VARIABLE check_stderr
)
if(NOT check_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel -c rejected the generated Skia module (exit ${check_exit})\n"
    "--- stderr ---\n${check_stderr}")
endif()

# The bridge module is checked in under std/internal, so the descriptor and
# the committed module cannot drift. Only the generated header line differs
# (it names the descriptor path).
file(READ "${SOURCE_DIR}/std/internal/skia.oskl" committed_module)
file(READ "${BUILD_DIR}/skia.oskl" generated_module)
string(REGEX REPLACE "^// Generated[^\n]*\n" "" committed_body "${committed_module}")
string(REGEX REPLACE "^// Generated[^\n]*\n" "" generated_body "${generated_module}")
if(NOT committed_body STREQUAL generated_body)
  message(FATAL_ERROR
    "std/internal/skia.oskl is out of date with bindings/skia/skia.oskelbind.\n"
    "Run bindgen --cxx bindings/skia/skia.oskelbind -o std/internal/skia.oskl.")
endif()

separate_arguments(skia_libs UNIX_COMMAND "${SKIA_LIBS}")
execute_process(
  COMMAND "${CXX_COMPILER}" -std=c++17 -shared -fPIC
          "-I${SKIA_DIR}" "-I${SOURCE_DIR}/bindings/skia" "-I${BUILD_DIR}"
          -o "${BUILD_DIR}/libskia_shim.so"
          "${BUILD_DIR}/skia_shim.cpp" "${SKIA_LIB}" ${skia_libs}
  RESULT_VARIABLE compile_exit
  OUTPUT_VARIABLE compile_stdout
  ERROR_VARIABLE compile_stderr
)
if(NOT compile_exit EQUAL 0)
  message(FATAL_ERROR
    "the host C++ compiler rejected the Skia shim (exit ${compile_exit})\n"
    "--- stdout ---\n${compile_stdout}\n--- stderr ---\n${compile_stderr}")
endif()

file(COPY "${SOURCE_DIR}/tests/cxxbind/skia_test.oskl" DESTINATION "${BUILD_DIR}")

# The engine loads the UI bundle by directory, so the test's directory must
# contain both libraries. SDL3 keeps its SONAME (`libSDL3.so.0`), which the
# loader asks for; create the two names a normal installation has.
if(SDL3_FOUND AND SDL3_LIBRARY AND EXISTS "${SDL3_LIBRARY}")
  file(COPY "${SDL3_LIBRARY}" DESTINATION "${BUILD_DIR}")
  get_filename_component(sdl3_name "${SDL3_LIBRARY}" NAME)
  file(REMOVE "${BUILD_DIR}/libSDL3.so.0" "${BUILD_DIR}/libSDL3.so")
  file(CREATE_LINK "${sdl3_name}" "${BUILD_DIR}/libSDL3.so.0" SYMBOLIC)
  file(CREATE_LINK "libSDL3.so.0" "${BUILD_DIR}/libSDL3.so" SYMBOLIC)
endif()

# The test must prove the zero-configuration path: `OSKEL_UI_LIBDIR` points at
# this directory, but no `LD_PRELOAD`, `LD_LIBRARY_PATH` or `--link-lib` is set.
set(skia_env "OSKEL_UI_LIBDIR=${BUILD_DIR}")
execute_process(
  COMMAND ldd "${OSKEL_EXE}"
  OUTPUT_VARIABLE oskel_ldd
  ERROR_QUIET
)
if(oskel_ldd MATCHES "libasan\\.so[^ ]* => " OR
   oskel_ldd MATCHES "libclang_rt\\.asan[^ ]* => ")
  list(APPEND skia_env "ASAN_OPTIONS=detect_leaks=0")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${skia_env}
          "${OSKEL_EXE}" "${BUILD_DIR}/skia_test.oskl"
  WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE vm_exit
  OUTPUT_VARIABLE vm_stdout
  ERROR_VARIABLE vm_stderr
)
if(NOT vm_exit EQUAL 0)
  message(FATAL_ERROR
    "the Skia VM test exited ${vm_exit}\n--- stdout ---\n${vm_stdout}\n"
    "--- stderr ---\n${vm_stderr}")
endif()
if(NOT vm_stdout MATCHES "skia ok")
  message(FATAL_ERROR "the Skia VM test printed '${vm_stdout}', expected 'skia ok'")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${skia_env}
          "${OSKEL_EXE}" build "${BUILD_DIR}/skia_test.oskl"
          -o "${BUILD_DIR}/skia_test"
  WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE build_exit
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build (Skia) exited ${build_exit}\n"
    "--- stdout ---\n${build_stdout}\n--- stderr ---\n${build_stderr}")
endif()

execute_process(
  COMMAND "${BUILD_DIR}/skia_test"
  RESULT_VARIABLE native_exit
  OUTPUT_VARIABLE native_stdout
  ERROR_VARIABLE native_stderr
)
if(NOT native_exit EQUAL 0)
  message(FATAL_ERROR
    "the hosted Skia binary exited ${native_exit}\n--- stdout ---\n${native_stdout}\n"
    "--- stderr ---\n${native_stderr}")
endif()
if(NOT native_stdout STREQUAL vm_stdout)
  message(FATAL_ERROR
    "the hosted Skia binary disagrees with the VM\n--- vm ---\n${vm_stdout}\n"
    "--- binary ---\n${native_stdout}")
endif()

# --- 2. The widget layer: std.ui -> display list -> Skia. ---
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${skia_env}
          "${OSKEL_EXE}" "${SOURCE_DIR}/tests/cxxbind/skia_ui_test.oskl"
  WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE ui_vm_exit
  OUTPUT_VARIABLE ui_vm_stdout
  ERROR_VARIABLE ui_vm_stderr
)
if(NOT ui_vm_exit EQUAL 0)
  message(FATAL_ERROR
    "the Skia UI VM test exited ${ui_vm_exit}\n--- stdout ---\n${ui_vm_stdout}\n"
    "--- stderr ---\n${ui_vm_stderr}")
endif()
if(NOT ui_vm_stdout MATCHES "skia ui ok")
  message(FATAL_ERROR "the Skia UI VM test printed '${ui_vm_stdout}', expected 'skia ui ok'")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${skia_env}
          "${OSKEL_EXE}" build "${SOURCE_DIR}/tests/cxxbind/skia_ui_test.oskl"
          -o "${BUILD_DIR}/skia_ui_test"
  WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE ui_build_exit
  OUTPUT_VARIABLE ui_build_stdout
  ERROR_VARIABLE ui_build_stderr
)
if(NOT ui_build_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build (Skia UI) exited ${ui_build_exit}\n"
    "--- stdout ---\n${ui_build_stdout}\n--- stderr ---\n${ui_build_stderr}")
endif()

execute_process(
  COMMAND "${BUILD_DIR}/skia_ui_test"
  RESULT_VARIABLE ui_native_exit
  OUTPUT_VARIABLE ui_native_stdout
  ERROR_VARIABLE ui_native_stderr
)
if(NOT ui_native_exit EQUAL 0)
  message(FATAL_ERROR
    "the hosted Skia UI binary exited ${ui_native_exit}\n"
    "--- stdout ---\n${ui_native_stdout}\n--- stderr ---\n${ui_native_stderr}")
endif()
if(NOT ui_native_stdout STREQUAL ui_vm_stdout)
  message(FATAL_ERROR
    "the hosted Skia UI binary disagrees with the VM\n--- vm ---\n${ui_vm_stdout}\n"
    "--- binary ---\n${ui_native_stdout}")
endif()

# --- 3. The window loop: std.ui.window -> Skia -> SDL3 texture. ---
# The dummy video driver creates an off-screen window and a software renderer;
# `maxFrames` bounds the loop, so this half never waits for a display or input.
if(SDL3_FOUND)
  set(window_env ${skia_env} "SDL_VIDEODRIVER=dummy" "SDL_AUDIODRIVER=dummy")

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${window_env}
            "${OSKEL_EXE}" "${SOURCE_DIR}/tests/cxxbind/skia_window_test.oskl"
    WORKING_DIRECTORY "${BUILD_DIR}"
    RESULT_VARIABLE window_vm_exit
    OUTPUT_VARIABLE window_vm_stdout
    ERROR_VARIABLE window_vm_stderr
  )
  if(NOT window_vm_exit EQUAL 0)
    message(FATAL_ERROR
      "the Skia window VM test exited ${window_vm_exit}\n--- stdout ---\n${window_vm_stdout}\n"
      "--- stderr ---\n${window_vm_stderr}")
  endif()
  if(NOT window_vm_stdout MATCHES "skia window ok")
    message(FATAL_ERROR
      "the Skia window VM test printed '${window_vm_stdout}', expected 'skia window ok'")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${skia_env}
            "${OSKEL_EXE}" build "${SOURCE_DIR}/tests/cxxbind/skia_window_test.oskl"
            -o "${BUILD_DIR}/skia_window_test"
    WORKING_DIRECTORY "${BUILD_DIR}"
    RESULT_VARIABLE window_build_exit
    OUTPUT_VARIABLE window_build_stdout
    ERROR_VARIABLE window_build_stderr
  )
  if(NOT window_build_exit EQUAL 0)
    message(FATAL_ERROR
      "oskel build (Skia window) exited ${window_build_exit}\n"
      "--- stdout ---\n${window_build_stdout}\n--- stderr ---\n${window_build_stderr}")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "SDL_VIDEODRIVER=dummy" "SDL_AUDIODRIVER=dummy"
            "${BUILD_DIR}/skia_window_test"
    RESULT_VARIABLE window_native_exit
    OUTPUT_VARIABLE window_native_stdout
    ERROR_VARIABLE window_native_stderr
  )
  if(NOT window_native_exit EQUAL 0)
    message(FATAL_ERROR
      "the hosted Skia window binary exited ${window_native_exit}\n"
      "--- stdout ---\n${window_native_stdout}\n--- stderr ---\n${window_native_stderr}")
  endif()
  if(NOT window_native_stdout STREQUAL window_vm_stdout)
    message(FATAL_ERROR
      "the hosted Skia window binary disagrees with the VM\n--- vm ---\n${window_vm_stdout}\n"
      "--- binary ---\n${window_native_stdout}")
  endif()
endif()
