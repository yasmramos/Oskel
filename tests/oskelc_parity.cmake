# Proves that `oskelc` and `oskel build` are one compiler, not two.
#
# Both entry points call `runBuildCommand` in `src/frontend/cli.cpp`, and this test is
# what keeps it that way: the same source is compiled by each, the two images
# must be byte for byte identical, and the `oskelc` image must agree with the
# development VM's output. A drift in the flags, the defaults, the module
# resolution or the lowering shows up here as a byte difference.
#
# Registered by CMakeLists.txt on every host the AOT backend targets. Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKELC_EXE=<path to the oskelc executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DOSKEL_TEST_NAME=<the CTest name, used for unique output paths>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKELC_EXE OR NOT DEFINED OSKEL_SCRIPT
   OR NOT DEFINED OSKEL_TEST_NAME)
  message(FATAL_ERROR
    "oskelc_parity.cmake requires OSKEL_EXE, OSKELC_EXE, OSKEL_SCRIPT and OSKEL_TEST_NAME")
endif()

# The host's executable suffix (see build_binary.cmake: the script runs under
# `cmake -P`, where CMAKE_EXECUTABLE_SUFFIX is not populated).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()
# Parallel tests share CMAKE_CURRENT_BINARY_DIR, so both names embed the test.
set(from_compiler "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-oskelc${exe_suffix}")
set(from_engine "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-oskel${exe_suffix}")
file(REMOVE "${from_compiler}" "${from_engine}")

execute_process(
  COMMAND "${OSKELC_EXE}" "${OSKEL_SCRIPT}" -o "${from_compiler}"
  RESULT_VARIABLE compiler_exit
  OUTPUT_VARIABLE compiler_stdout
  ERROR_VARIABLE compiler_stderr
)
if(NOT compiler_exit EQUAL 0)
  message(FATAL_ERROR
    "oskelc exited ${compiler_exit}\n--- stdout ---\n${compiler_stdout}\n"
    "--- stderr ---\n${compiler_stderr}")
endif()
if(NOT EXISTS "${from_compiler}")
  message(FATAL_ERROR "oskelc did not create '${from_compiler}'")
endif()

execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" -o "${from_engine}"
  RESULT_VARIABLE engine_exit
  OUTPUT_VARIABLE engine_stdout
  ERROR_VARIABLE engine_stderr
)
if(NOT engine_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build exited ${engine_exit}\n--- stdout ---\n${engine_stdout}\n"
    "--- stderr ---\n${engine_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${from_compiler}" "${from_engine}"
  RESULT_VARIABLE compare_exit
)
if(NOT compare_exit EQUAL 0)
  message(FATAL_ERROR
    "oskelc and `oskel build` produced different binaries for '${OSKEL_SCRIPT}'; "
    "the two entry points have drifted apart")
endif()

# The compiler's image must also behave like the VM's.
execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}"
  RESULT_VARIABLE vm_exit
  OUTPUT_VARIABLE vm_stdout
)
if(NOT vm_exit EQUAL 0)
  message(FATAL_ERROR "the VM failed to run the fixture (exit ${vm_exit})")
endif()

execute_process(
  COMMAND "${from_compiler}"
  RESULT_VARIABLE run_exit
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
if(NOT run_exit EQUAL 0)
  message(FATAL_ERROR
    "the oskelc binary exited ${run_exit}\n--- stdout ---\n${run_stdout}\n"
    "--- stderr ---\n${run_stderr}")
endif()
if(NOT run_stdout STREQUAL vm_stdout)
  message(FATAL_ERROR
    "the oskelc binary disagrees with the VM\n"
    "--- vm ---\n${vm_stdout}\n--- binary ---\n${run_stdout}")
endif()
