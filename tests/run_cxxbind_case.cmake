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

# The factory descriptor covers the ownership slice: factory objects, smart
# pointers, value returns, borrowed handles and class-reference parameters.
execute_process(
  COMMAND "${BINDGEN_EXE}" --cxx "${SOURCE_DIR}/tests/cxxbind/factory.oskelbind"
          --shim "${BUILD_DIR}" -o "${BUILD_DIR}/factory.oskl"
  RESULT_VARIABLE factory_gen_exit
  OUTPUT_VARIABLE factory_gen_stdout
  ERROR_VARIABLE factory_gen_stderr
)
if(NOT factory_gen_exit EQUAL 0)
  message(FATAL_ERROR
    "bindgen --cxx factory exited ${factory_gen_exit}\n--- stdout ---\n${factory_gen_stdout}\n"
    "--- stderr ---\n${factory_gen_stderr}")
endif()
foreach(path factory.oskl factory_shim.h factory_shim.cpp)
  if(NOT EXISTS "${BUILD_DIR}/${path}")
    message(FATAL_ERROR "bindgen --cxx did not create '${path}'")
  endif()
endforeach()
file(READ "${BUILD_DIR}/factory.oskl" factory_module_text)
foreach(expected
    "fn oskel_Surface_delete(self: ptr): void;"
    "fn oskel_Surface_canvas(self: ptr): ptr;"
    "fn oskel_Surfaces_make(w: int32, h: int32): ptr;"
    "fn oskel_Ref_unref(self: ptr): void;"
    "class Surface {"
    "fn canvas(): Canvas {"
    "return Canvas(oskel_Surface_canvas(self.handle), false);"
    "return Image(oskel_Surface_snapshot(self.handle), true);"
    "let Surfaces = {"
    "make: (w: int, h: int) => Surface(oskel_Surfaces_make(w, h), true),"
    "make: (l: float, t: float, r: float, b: float) => Rect(oskel_Rects_make(l, t, r, b), true),"
    "let Refs = {"
    "make: (value: int) => Ref(oskel_Refs_make(value), true),")
  string(FIND "${factory_module_text}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR
      "the generated factory module is missing '${expected}'\n--- module ---\n${factory_module_text}")
  endif()
endforeach()
file(READ "${BUILD_DIR}/factory_shim.cpp" factory_shim_text)
foreach(expected
    "const_cast<Canvas *>(static_cast<Surface *>(self)->canvas())"
    "auto result = static_cast<Surface *>(self)->snapshot();"
    "return static_cast<void *>(result.release());"
    "new Rect(result)"
    "*static_cast<const Rect *>(rect)"
    "*static_cast<const Box *>(box)"
    "static_cast<Ref *>(self)->unref()")
  string(FIND "${factory_shim_text}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR
      "the generated factory shim is missing '${expected}'\n--- shim ---\n${factory_shim_text}")
  endif()
endforeach()
string(FIND "${factory_shim_text}" "oskel_Canvas_delete" canvas_delete)
if(NOT canvas_delete EQUAL -1)
  message(FATAL_ERROR "a release \"none\" class emitted a release thunk")
endif()

foreach(module counter.oskl factory.oskl)
  execute_process(
    COMMAND "${OSKEL_EXE}" -c "${BUILD_DIR}/${module}"
    RESULT_VARIABLE check_exit
    OUTPUT_VARIABLE check_stdout
    ERROR_VARIABLE check_stderr
  )
  if(NOT check_exit EQUAL 0)
    message(FATAL_ERROR
      "oskel -c rejected ${module} (exit ${check_exit})\n--- stderr ---\n${check_stderr}")
  endif()
endforeach()

# The Skia descriptor is generated on every platform; compiling its shim needs
# a Skia checkout, so only generation and `oskel -c` run here.
file(MAKE_DIRECTORY "${BUILD_DIR}/skia")
execute_process(
  COMMAND "${BINDGEN_EXE}" --cxx "${SOURCE_DIR}/bindings/skia/skia.oskelbind"
          --shim "${BUILD_DIR}/skia" -o "${BUILD_DIR}/skia.oskl"
  RESULT_VARIABLE skia_gen_exit
  OUTPUT_VARIABLE skia_gen_stdout
  ERROR_VARIABLE skia_gen_stderr
)
if(NOT skia_gen_exit EQUAL 0)
  message(FATAL_ERROR
    "bindgen --cxx skia exited ${skia_gen_exit}\n--- stdout ---\n${skia_gen_stdout}\n"
    "--- stderr ---\n${skia_gen_stderr}")
endif()
file(READ "${BUILD_DIR}/skia.oskl" skia_module_text)
foreach(expected
    "fn oskel_Surface_unref(self: ptr): void;"
    "class Surface {"
    "fn getCanvas(): Canvas {"
    "return Canvas(oskel_Surface_getCanvas(self.handle), false);"
    "let Surfaces = {"
    "raster: (w: int, h: int) => Surface(oskel_Surfaces_raster(w, h), true),")
  string(FIND "${skia_module_text}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR
      "the generated Skia module is missing '${expected}'\n--- module ---\n${skia_module_text}")
  endif()
endforeach()
execute_process(
  COMMAND "${OSKEL_EXE}" -c "${BUILD_DIR}/skia.oskl"
  RESULT_VARIABLE skia_check_exit
  OUTPUT_VARIABLE skia_check_stdout
  ERROR_VARIABLE skia_check_stderr
)
if(NOT skia_check_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel -c rejected the generated Skia module (exit ${skia_check_exit})\n"
    "--- stderr ---\n${skia_check_stderr}")
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

# Each ownership mistake must be rejected before any file is written.
set(invalid_files
    invalid_factory.oskelbind
    invalid_release.oskelbind
    invalid_value.oskelbind
    invalid_ref.oskelbind)
set(invalid_errors
    "adopt-only"
    "no release policy"
    "needs release \"delete\""
    "reference to a non-class type")
list(LENGTH invalid_files invalid_count)
math(EXPR invalid_last "${invalid_count} - 1")
foreach(index RANGE 0 ${invalid_last})
  list(GET invalid_files ${index} file)
  list(GET invalid_errors ${index} expected_error)
  execute_process(
    COMMAND "${BINDGEN_EXE}" --cxx "${SOURCE_DIR}/tests/cxxbind/${file}"
            --shim "${BUILD_DIR}" -o "${BUILD_DIR}/${file}.oskl"
    RESULT_VARIABLE case_exit
    ERROR_VARIABLE case_stderr
  )
  if(NOT case_exit EQUAL 65)
    message(FATAL_ERROR
      "${file} exited ${case_exit}, expected 65\n${case_stderr}")
  endif()
  if(NOT case_stderr MATCHES "${expected_error}")
    message(FATAL_ERROR
      "${file} did not report '${expected_error}':\n${case_stderr}")
  endif()
endforeach()

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

# --- 3. The factory fixture: VM and hosted AOT. ---
execute_process(
  COMMAND "${CXX_COMPILER}" -std=c++17 -shared -fPIC
          "-I${SOURCE_DIR}/tests/cxxbind" "-I${BUILD_DIR}"
          -o "${BUILD_DIR}/libfactory_shim.so"
          "${BUILD_DIR}/factory_shim.cpp" "${SOURCE_DIR}/tests/cxxbind/factory.cpp"
  RESULT_VARIABLE factory_cxx_exit
  OUTPUT_VARIABLE factory_cxx_stdout
  ERROR_VARIABLE factory_cxx_stderr
)
if(NOT factory_cxx_exit EQUAL 0)
  message(FATAL_ERROR
    "the host C++ compiler rejected the factory shim (exit ${factory_cxx_exit})\n"
    "--- stdout ---\n${factory_cxx_stdout}\n--- stderr ---\n${factory_cxx_stderr}")
endif()

file(COPY "${SOURCE_DIR}/tests/cxxbind/factory_test.oskl" DESTINATION "${BUILD_DIR}")

set(factory_preload "${BUILD_DIR}/libfactory_shim.so")
if(oskel_ldd MATCHES "libasan\\.so[^ ]* => ([^ ]+)")
  set(factory_preload "${CMAKE_MATCH_1}:${factory_preload}")
elseif(oskel_ldd MATCHES "libclang_rt\\.asan[^ ]* => ([^ ]+)")
  set(factory_preload "${CMAKE_MATCH_1}:${factory_preload}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LD_PRELOAD=${factory_preload}"
          "${OSKEL_EXE}" "${BUILD_DIR}/factory_test.oskl"
  WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE factory_vm_exit
  OUTPUT_VARIABLE factory_vm_stdout
  ERROR_VARIABLE factory_vm_stderr
)
if(NOT factory_vm_exit EQUAL 0)
  message(FATAL_ERROR
    "the factory VM test exited ${factory_vm_exit}\n--- stdout ---\n${factory_vm_stdout}\n"
    "--- stderr ---\n${factory_vm_stderr}")
endif()
if(NOT factory_vm_stdout MATCHES "cxx factory ok")
  message(FATAL_ERROR
    "the factory VM test printed '${factory_vm_stdout}', expected 'cxx factory ok'")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LIBRARY_PATH=${BUILD_DIR}"
          "${OSKEL_EXE}" build "${BUILD_DIR}/factory_test.oskl" --mode hosted
          --link-lib factory_shim -o "${BUILD_DIR}/factory_test"
  WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE factory_build_exit
  OUTPUT_VARIABLE factory_build_stdout
  ERROR_VARIABLE factory_build_stderr
)
if(NOT factory_build_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build --mode hosted (factory) exited ${factory_build_exit}\n"
    "--- stdout ---\n${factory_build_stdout}\n--- stderr ---\n${factory_build_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${BUILD_DIR}"
          "${BUILD_DIR}/factory_test"
  RESULT_VARIABLE factory_native_exit
  OUTPUT_VARIABLE factory_native_stdout
  ERROR_VARIABLE factory_native_stderr
)
if(NOT factory_native_exit EQUAL 0)
  message(FATAL_ERROR
    "the hosted factory binary exited ${factory_native_exit}\n"
    "--- stdout ---\n${factory_native_stdout}\n--- stderr ---\n${factory_native_stderr}")
endif()
if(NOT factory_native_stdout STREQUAL factory_vm_stdout)
  message(FATAL_ERROR
    "the hosted factory binary disagrees with the VM\n"
    "--- vm ---\n${factory_vm_stdout}\n--- binary ---\n${factory_native_stdout}")
endif()
