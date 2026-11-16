# End-to-end test for the `oskel build` rejection of a host binding the AOT
# backend cannot resolve (PHASE 7f): the backend has no host, so the build must
# fail with the dedicated message and exit 65, never emit a binary.
#
# Registered by CMakeLists.txt on every host the AOT backend targets. Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DEXPECTED_MESSAGE=<regex the stderr must match>
#
# It asserts:
#   1. `oskel build` exits 65,
#   2. stderr matches the expected message (which names the symbol and the AOT
#      backend, not a typo),
#   3. no output binary is written.

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED EXPECTED_MESSAGE)
  message(FATAL_ERROR "build_error.cmake requires OSKEL_EXE, OSKEL_SCRIPT and EXPECTED_MESSAGE")
endif()

# Match the host's executable suffix, so the "no binary was written" check looks
# for the name `oskel build` would actually produce (see build_binary.cmake).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()
# The output name includes the fixture's stem: several cases register this
# runner, and a shared name would let a parallel `ctest` overwrite another
# case's file (see build_binary.cmake).
get_filename_component(script_stem "${OSKEL_SCRIPT}" NAME_WE)
set(out "${CMAKE_CURRENT_BINARY_DIR}/oskel_build_error_output_${script_stem}${exe_suffix}")
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
if(NOT build_stderr MATCHES "${EXPECTED_MESSAGE}")
  message(FATAL_ERROR
    "the build error does not match '${EXPECTED_MESSAGE}':\n--- stderr ---\n${build_stderr}")
endif()
if(EXISTS "${out}")
  message(FATAL_ERROR "oskel build wrote '${out}' despite the error")
endif()
