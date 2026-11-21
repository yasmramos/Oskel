# End-to-end test for the `oskel build` rejection of a host binding the AOT
# backend cannot resolve (PHASE 7f): the backend has no host, so the build must
# fail with the dedicated message and exit 65, never emit a binary.
#
# Registered by CMakeLists.txt on every host the AOT backend targets. Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DOSKEL_TEST_NAME=<the CTest name, used for a unique output path>
#   -DEXPECTED_MESSAGE=<regex the stderr must match>
#   -DOSKEL_TARGET=<optional --target value, e.g. wasm32-freestanding-wasm>
#   -DOSKEL_BUILD_MODE=<optional `--mode` value; the default is freestanding>
#
# It asserts:
#   1. `oskel build` exits 65,
#   2. stderr matches the expected message (which names the symbol and the AOT
#      backend, not a typo),
#   3. no output binary is written.

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED OSKEL_TEST_NAME OR NOT DEFINED EXPECTED_MESSAGE)
  message(FATAL_ERROR "build_error.cmake requires OSKEL_EXE, OSKEL_SCRIPT, OSKEL_TEST_NAME and EXPECTED_MESSAGE")
endif()

# Match the host's executable suffix, so the "no binary was written" check looks
# for the name `oskel build` would actually produce (see build_binary.cmake).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()
if(DEFINED OSKEL_TARGET AND OSKEL_TARGET MATCHES "wasm32")
  set(exe_suffix ".wasm")
endif()
# The output path embeds the test name (see build_binary.cmake): parallel tests
# share `CMAKE_CURRENT_BINARY_DIR`, so a fixed name would collide.
set(out "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}${exe_suffix}")
file(REMOVE "${out}")

if(DEFINED OSKEL_TARGET)
  set(target_args --target "${OSKEL_TARGET}")
else()
  set(target_args)
endif()
set(mode_args "")
if(DEFINED OSKEL_BUILD_MODE AND NOT OSKEL_BUILD_MODE STREQUAL "")
  set(mode_args "--mode" "${OSKEL_BUILD_MODE}")
endif()

execute_process(
  COMMAND "${OSKEL_EXE}" build ${target_args} "${OSKEL_SCRIPT}" ${mode_args} -o "${out}"
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
