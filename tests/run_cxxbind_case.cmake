# End-to-end test for `bindgen --cxx`.
#
# Step 1 (every platform): generate the module and the C++ shim from a
# descriptor, check both files, and feed the module to `oskel -c`.
# Step 2 (Linux, RUN_RUNTIME=ON): compile the shim with the host C++ compiler,
# run the VM with LD_PRELOAD, build the same program with the hosted AOT mode,
# and require both engines to print the same output.
#
# Expects:
#   -DBINDGEN_EXE=<bindgen>
#   -DOSKEL_EXE=<oskel>
#   -DCXX_COMPILER=<host C++ compiler>
#   -DSOURCE_DIR=<repository root>
#   -DBUILD_DIR=<scratch directory>
#   -DRUN_RUNTIME=ON|OFF

if(NOT DEFINED BINDGEN_EXE OR NOT DEFINED OSKEL_EXE OR NOT DEFINED CXX_COMPILER
   OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BUILD_DIR)
  message(FATAL_ERROR "run_cxxbind_case.cmake is missing a required variable")
endif()

file(REMOVE_RECURSE "${BUILD_DIR}")
file(MAKE_DIRECTORY "${BUILD_DIR}")

# --- 1. Generate the module and the shim. ---
execute_process(
  COMMAND "${BINDGEN_EXE}" --cxx "${SOURCE_DIR}/tests/cxxbind/counter.oskelbind"
          --shim "${BUILD_DIR}" -o "${BUILD_DIR}/counter.oskl"
  RESULT_VARIABLE gen_exit
  OUTPUT_VARIABLE gen_stdout
  ERROR_VARIABLE gen_stderr
)
if(NOT gen_exit EQUAL 0)
  message(FATAL_ERROR
    "bindgen --cxx exited ${gen_exit}\n--- stdout ---\n${gen_stdout}\n"
    "--- stderr ---\n${gen_stderr}")
endif()
foreach(path counter.oskl counter_shim.h counter_shim.cpp)
  if(NOT EXISTS "${BUILD_DIR}/${path}")
    message(FATAL_ERROR "bindgen --cxx did not create '${path}'")
  endif()
endforeach()
file(READ "${BUILD_DIR}/counter.oskl" module_text)
foreach(expected
    "extern \"C\" {"
    "fn oskel_Counter_new(start: int32): ptr;"
    "class Counter {"
    "fn init(start: int) {"
    "fn value(): int {"
    "fn close() {"
    "fn add(a: int, b: int): int {")
  string(FIND "${module_text}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "the generated module is missing '${expected}'\n--- module ---\n${module_text}")
  endif()
endforeach()
file(READ "${BUILD_DIR}/counter_shim.cpp" shim_text)
foreach(expected "new Counter(" "static_cast<const Counter *>(self)->value()" "delete static_cast<Counter *>(self)" "return add(a, b);")
  string(FIND "${shim_text}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "the generated shim is missing '${expected}'\n--- shim ---\n${shim_text}")
  endif()
endforeach()

execute_process(
  COMMAND "${OSKEL_EXE}" -c "${BUILD_DIR}/counter.oskl"
  RESULT_VARIABLE check_exit
  OUTPUT_VARIABLE check_stdout
  ERROR_VARIABLE check_stderr
)
if(NOT check_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel -c rejected the generated module (exit ${check_exit})\n"
    "--- module ---\n${module_text}\n--- stderr ---\n${check_stderr}")
endif()

# A descriptor error is a data error (65), not a usage error.
execute_process(
  COMMAND "${BINDGEN_EXE}" --cxx "${SOURCE_DIR}/tests/cxxbind/invalid.oskelbind"
          --shim "${BUILD_DIR}" -o "${BUILD_DIR}/invalid.oskl"
  RESULT_VARIABLE invalid_exit
  ERROR_VARIABLE invalid_stderr
)
if(NOT invalid_exit EQUAL 65)
  message(FATAL_ERROR "an invalid descriptor exited ${invalid_exit}, expected 65\n${invalid_stderr}")
endif()
if(NOT invalid_stderr MATCHES "unknown type")
  message(FATAL_ERROR "the invalid-descriptor error is not reported:\n${invalid_stderr}")
endif()

if(NOT RUN_RUNTIME)
  return()
endif()

# --- 2. Compile the shim and run both engines. ---
execute_process(
  COMMAND "${CXX_COMPILER}" -std=c++17 -shared -fPIC
          "-I${SOURCE_DIR}/tests/cxxbind" "-I${BUILD_DIR}"
          -o "${BUILD_DIR}/libcounter_shim.so"
          "${BUILD_DIR}/counter_shim.cpp" "${SOURCE_DIR}/tests/cxxbind/counter.cpp"
  RESULT_VARIABLE cxx_exit
  OUTPUT_VARIABLE cxx_stdout
  ERROR_VARIABLE cxx_stderr
)
if(NOT cxx_exit EQUAL 0)
  message(FATAL_ERROR
    "the host C++ compiler rejected the generated shim (exit ${cxx_exit})\n"
    "--- stdout ---\n${cxx_stdout}\n--- stderr ---\n${cxx_stderr}")
endif()

file(COPY "${SOURCE_DIR}/tests/cxxbind/counter_test.oskl" DESTINATION "${BUILD_DIR}")

# An ASan/UBSan build links its runtime into `oskel`. `LD_PRELOAD` puts the
# shim first, so the sanitizer runtime must be preloaded before it; otherwise
# ASan refuses to start ("does not come first in initial library list").
execute_process(
  COMMAND ldd "${OSKEL_EXE}"
  OUTPUT_VARIABLE oskel_ldd
  ERROR_QUIET
)
set(preload "${BUILD_DIR}/libcounter_shim.so")
if(oskel_ldd MATCHES "libasan\\.so[^ ]* => ([^ ]+)")
  set(preload "${CMAKE_MATCH_1}:${preload}")
elseif(oskel_ldd MATCHES "libclang_rt\\.asan[^ ]* => ([^ ]+)")
  set(preload "${CMAKE_MATCH_1}:${preload}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LD_PRELOAD=${preload}"
          "${OSKEL_EXE}" "${BUILD_DIR}/counter_test.oskl"
  WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE vm_exit
  OUTPUT_VARIABLE vm_stdout
  ERROR_VARIABLE vm_stderr
)
if(NOT vm_exit EQUAL 0)
  message(FATAL_ERROR
    "the VM bridge test exited ${vm_exit}\n--- stdout ---\n${vm_stdout}\n"
    "--- stderr ---\n${vm_stderr}")
endif()
if(NOT vm_stdout MATCHES "cxx bridge ok")
  message(FATAL_ERROR "the VM bridge test printed '${vm_stdout}', expected 'cxx bridge ok'")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LIBRARY_PATH=${BUILD_DIR}"
          "${OSKEL_EXE}" build "${BUILD_DIR}/counter_test.oskl" --mode hosted
          --link-lib counter_shim -o "${BUILD_DIR}/counter_test"
  WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE build_exit
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build --mode hosted exited ${build_exit}\n--- stdout ---\n${build_stdout}\n"
    "--- stderr ---\n${build_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${BUILD_DIR}"
          "${BUILD_DIR}/counter_test"
  RESULT_VARIABLE native_exit
  OUTPUT_VARIABLE native_stdout
  ERROR_VARIABLE native_stderr
)
if(NOT native_exit EQUAL 0)
  message(FATAL_ERROR
    "the hosted bridge binary exited ${native_exit}\n--- stdout ---\n${native_stdout}\n"
    "--- stderr ---\n${native_stderr}")
endif()
if(NOT native_stdout STREQUAL vm_stdout)
  message(FATAL_ERROR
    "the hosted bridge disagrees with the VM\n--- vm ---\n${vm_stdout}\n"
    "--- binary ---\n${native_stdout}")
endif()
