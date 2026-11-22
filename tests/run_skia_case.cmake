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

# Keep the ASan runtime first when the engine is instrumented: the Skia shim is
# not, but the process still needs libasan to satisfy the engine.
set(skia_preload "${BUILD_DIR}/libskia_shim.so")
execute_process(
  COMMAND ldd "${OSKEL_EXE}"
  OUTPUT_VARIABLE oskel_ldd
  ERROR_QUIET
)
set(skia_env "LD_PRELOAD=${skia_preload}")
if(oskel_ldd MATCHES "libasan\\.so[^ ]* => ([^ ]+)")
  set(skia_preload "${CMAKE_MATCH_1}:${skia_preload}")
  set(skia_env "LD_PRELOAD=${skia_preload}" "ASAN_OPTIONS=detect_leaks=0")
elseif(oskel_ldd MATCHES "libclang_rt\\.asan[^ ]* => ([^ ]+)")
  set(skia_preload "${CMAKE_MATCH_1}:${skia_preload}")
  set(skia_env "LD_PRELOAD=${skia_preload}" "ASAN_OPTIONS=detect_leaks=0")
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
  COMMAND "${CMAKE_COMMAND}" -E env "LIBRARY_PATH=${BUILD_DIR}"
          "${OSKEL_EXE}" build "${BUILD_DIR}/skia_test.oskl" --mode hosted
          --link-lib skia_shim -o "${BUILD_DIR}/skia_test"
  WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE build_exit
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build --mode hosted (Skia) exited ${build_exit}\n"
    "--- stdout ---\n${build_stdout}\n--- stderr ---\n${build_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${BUILD_DIR}"
          "${BUILD_DIR}/skia_test"
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
file(COPY "${SOURCE_DIR}/bindings/skia/skia_ui.oskl"
          "${SOURCE_DIR}/tests/cxxbind/skia_ui_test.oskl"
     DESTINATION "${BUILD_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${skia_env}
          "${OSKEL_EXE}" "${BUILD_DIR}/skia_ui_test.oskl"
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
  COMMAND "${CMAKE_COMMAND}" -E env "LIBRARY_PATH=${BUILD_DIR}"
          "${OSKEL_EXE}" build "${BUILD_DIR}/skia_ui_test.oskl" --mode hosted
          --link-lib skia_shim -o "${BUILD_DIR}/skia_ui_test"
  WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE ui_build_exit
  OUTPUT_VARIABLE ui_build_stdout
  ERROR_VARIABLE ui_build_stderr
)
if(NOT ui_build_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build --mode hosted (Skia UI) exited ${ui_build_exit}\n"
    "--- stdout ---\n${ui_build_stdout}\n--- stderr ---\n${ui_build_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${BUILD_DIR}"
          "${BUILD_DIR}/skia_ui_test"
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
