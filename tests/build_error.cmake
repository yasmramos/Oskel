# End-to-end test for the `oskel build` rejection of an `extern "C"` symbol
# (PHASE 7f gap): the AOT backend has no host, so the build must fail with the
# dedicated message and exit 65, never emit a binary.
#
# Registered by CMakeLists.txt on Linux x86-64 only (the AOT backend targets
# x86_64-linux-sysv). Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#
# It asserts:
#   1. `oskel build` exits 65,
#   2. stderr names the extern symbol and the AOT backend, not a typo,
#   3. no output binary is written.

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT)
  message(FATAL_ERROR "build_error.cmake requires OSKEL_EXE and OSKEL_SCRIPT")
endif()

set(out "${CMAKE_CURRENT_BINARY_DIR}/oskel_build_error_output")
file(REMOVE "${out}")

execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" -o "${out}"
  RESULT_VARIABLE build_exit
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_exit EQUAL 65)
  message(FATAL_ERROR
    "oskel build exited ${build_exit}, expected 65\n--- stdout ---\n${build_stdout}\n"
    "--- stderr ---\n${build_stderr}")
endif()
if(NOT build_stderr MATCHES "extern \"C\" symbol 'getpid' cannot be resolved by the AOT backend")
  message(FATAL_ERROR
    "the build error does not name the extern symbol:\n--- stderr ---\n${build_stderr}")
endif()
if(EXISTS "${out}")
  message(FATAL_ERROR "oskel build wrote '${out}' despite the error")
endif()
