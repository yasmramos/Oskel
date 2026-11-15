# End-to-end test for a runtime error in an AOT binary: `oskel build` must
# succeed, the generated binary must run the program up to the failing
# instruction, report the same message the VM reports and exit with the same
# code (70), never reaching the instruction after it.
#
# Registered by CMakeLists.txt on Linux x86-64 only (the AOT backend targets
# x86_64-linux-sysv). Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DOSKEL_EXPECTED_EXIT=<the runtime-error code the VM also uses>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED OSKEL_EXPECTED_EXIT)
  message(FATAL_ERROR "aot_runtime_error.cmake requires OSKEL_EXE, OSKEL_SCRIPT and OSKEL_EXPECTED_EXIT")
endif()

set(out "${CMAKE_CURRENT_BINARY_DIR}/oskel_aot_runtime_error_output")
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
if(NOT run_stderr MATCHES "chr\\(\\) code point is a surrogate")
  message(FATAL_ERROR
    "the generated binary does not report the surrogate error:\n--- stderr ---\n${run_stderr}")
endif()
