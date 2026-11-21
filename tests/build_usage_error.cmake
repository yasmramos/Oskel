# Usage-error test for the `--link-lib` flag.
#
# `--link-lib` is a hosted-mode concept: the freestanding image has no host
# linker, so passing it without `--mode hosted` must be a usage error (exit 64)
# rather than a flag silently ignored. A library name becomes a `-l<name>`
# argument of the host linker, so a name that could smuggle a shell
# metacharacter must be rejected before the command is built.
#
# Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<a .oskl file to pass to `oskel build`>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT)
  message(FATAL_ERROR "build_usage_error.cmake requires OSKEL_EXE and OSKEL_SCRIPT")
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()
set(out "${CMAKE_CURRENT_BINARY_DIR}/link_lib_usage_error${exe_suffix}")
file(REMOVE "${out}")

execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --link-lib SDL3 -o "${out}"
  RESULT_VARIABLE no_hosted_exit
  OUTPUT_VARIABLE no_hosted_stdout
  ERROR_VARIABLE no_hosted_stderr
)
if(NOT no_hosted_exit EQUAL 64)
  message(FATAL_ERROR
    "expected exit 64 without --mode hosted, got ${no_hosted_exit}\n"
    "--- stderr ---\n${no_hosted_stderr}")
endif()
if(NOT no_hosted_stderr MATCHES "'--link-lib' requires '--mode hosted'")
  message(FATAL_ERROR
    "the usage error does not name the missing mode:\n${no_hosted_stderr}")
endif()
if(EXISTS "${out}")
  message(FATAL_ERROR "oskel build wrote '${out}' despite the usage error")
endif()

execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode hosted
          --link-lib "bad;rm" -o "${out}"
  RESULT_VARIABLE bad_name_exit
  OUTPUT_VARIABLE bad_name_stdout
  ERROR_VARIABLE bad_name_stderr
)
if(NOT bad_name_exit EQUAL 64)
  message(FATAL_ERROR
    "expected exit 64 for an invalid library name, got ${bad_name_exit}\n"
    "--- stderr ---\n${bad_name_stderr}")
endif()
if(NOT bad_name_stderr MATCHES "invalid library name 'bad;rm'")
  message(FATAL_ERROR
    "the invalid-name error does not name the value:\n${bad_name_stderr}")
endif()
if(EXISTS "${out}")
  message(FATAL_ERROR "oskel build wrote '${out}' despite the invalid name")
endif()
