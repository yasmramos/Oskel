# Verifies that the binary's `--version` matches the version declared in
# `CMakeLists.txt` and `include/version.h`. The configure step already fails on
# a header/CMake mismatch (see CMakeLists.txt); this guards the last hop, that
# the compiled binary really prints the header value, so a hard-coded literal
# creeping back into `src/main.cpp` fails the suite instead of shipping.
#
# Registered by CMakeLists.txt for both binaries; expects:
#   -DOSKEL_EXE=<path to the executable>
#   -DOSKEL_EXPECTED_VERSION=<x.y.z>
#
# `oskelc --version` prints `Oskel <version>` too: the version is a property of
# the toolchain, and `include/version.h` is its only source for both binaries.

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_EXPECTED_VERSION)
  message(FATAL_ERROR "check_version.cmake requires OSKEL_EXE and OSKEL_EXPECTED_VERSION")
endif()

execute_process(
  COMMAND "${OSKEL_EXE}" --version
  RESULT_VARIABLE actual_exit
  OUTPUT_VARIABLE version_out
  ERROR_VARIABLE version_err
)

if(NOT actual_exit EQUAL 0)
  message(FATAL_ERROR "`oskel --version` failed (exit ${actual_exit})\n${version_err}")
endif()

string(STRIP "${version_out}" version_out)

if(NOT version_out STREQUAL "Oskel ${OSKEL_EXPECTED_VERSION}")
  message(FATAL_ERROR
    "`--version` printed '${version_out}', expected "
    "'Oskel ${OSKEL_EXPECTED_VERSION}'; the binary and CMakeLists.txt disagree")
endif()
