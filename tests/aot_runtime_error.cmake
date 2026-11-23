# End-to-end test for a runtime error in an AOT binary: `oskel build` must
# succeed, the generated binary must run the program up to the failing
# instruction, report the same message the VM reports and exit with the same
# code (70), never reaching the instruction after it.
#
# Registered by CMakeLists.txt on every host the AOT backend targets. Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DOSKEL_TEST_NAME=<the CTest name, used for a unique output path>
#   -DOSKEL_EXPECTED_EXIT=<the runtime-error code the VM also uses>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED OSKEL_TEST_NAME OR NOT DEFINED OSKEL_EXPECTED_EXIT)
  message(FATAL_ERROR "aot_runtime_error.cmake requires OSKEL_EXE, OSKEL_SCRIPT, OSKEL_TEST_NAME and OSKEL_EXPECTED_EXIT")
endif()

# Match the host's executable suffix, so the binary is written and launched
# under the name the platform expects (see build_binary.cmake).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()
# The output path embeds the test name (see build_binary.cmake): parallel tests
# share `CMAKE_CURRENT_BINARY_DIR`, so a fixed name would collide.
set(out "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}${exe_suffix}")
file(REMOVE "${out}")

execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" -o "${out}"
  RESULT_VARIABLE build_exit
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build exited ${build_exit}\n--- stdout ---\n${build_stdout}\n"
    "--- stderr ---\n${build_stderr}")
endif()

# The reference is the development VM: same stderr text, same exit code.
execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}"
  RESULT_VARIABLE vm_exit
  OUTPUT_VARIABLE vm_stdout
  ERROR_VARIABLE vm_stderr
)
if(NOT vm_exit EQUAL OSKEL_EXPECTED_EXIT)
  message(FATAL_ERROR "the VM exited ${vm_exit}, expected ${OSKEL_EXPECTED_EXIT}")
endif()

execute_process(
  COMMAND "${out}"
  RESULT_VARIABLE run_exit
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
if(NOT run_exit EQUAL OSKEL_EXPECTED_EXIT)
  message(FATAL_ERROR
    "the generated binary exited ${run_exit}, expected ${OSKEL_EXPECTED_EXIT}\n"
    "--- stdout ---\n${run_stdout}\n--- stderr ---\n${run_stderr}")
endif()
if(NOT run_stdout STREQUAL vm_stdout)
  message(FATAL_ERROR
    "the generated binary disagrees with the VM on stdout\n"
    "--- vm ---\n${vm_stdout}\n--- binary ---\n${run_stdout}")
endif()
# The expected wording is the caller's: a fixture pins its own message, so one
# harness serves every runtime error and a new fixture never has to fork it.
if(DEFINED OSKEL_EXPECTED_MESSAGE)
  set(expected_message "${OSKEL_EXPECTED_MESSAGE}")
else()
  set(expected_message "chr\\(\\) code point is a surrogate")
endif()
if(NOT run_stderr MATCHES "${expected_message}")
  message(FATAL_ERROR
    "the generated binary does not report the expected error:\n"
    "--- expected (regex) ---\n${expected_message}\n--- stderr ---\n${run_stderr}")
endif()
