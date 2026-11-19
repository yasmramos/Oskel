# End-to-end test for class-MEMORY struct-by-value `extern "C"` marshalling in
# the hosted AOT backend (PHASE 7f.7).
#
# A struct larger than 16 bytes is stack-passed and `sret`-returned, a shape no
# libc symbol exercises, so the fixture binds functions from
# `tests/hosted/memory_struct_helper.c`. `oskel build --mode hosted` links with
# `$CC`, so the helper is compiled to an object and a wrapper script appends it
# to the link command; that keeps the backend's own command line untouched.
#
# Registered by CMakeLists.txt on Linux x86-64 only. Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DOSKEL_HELPER=<the C helper source to compile in>
#   -DOSKEL_TEST_NAME=<the CTest name, used for unique output paths>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED OSKEL_TEST_NAME OR
   NOT DEFINED OSKEL_HELPER)
  message(FATAL_ERROR
    "build_hosted_memory_struct.cmake requires OSKEL_EXE, OSKEL_SCRIPT, "
    "OSKEL_HELPER and OSKEL_TEST_NAME")
endif()

find_program(CC_BIN NAMES cc gcc clang REQUIRED)
set(helper_obj "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-helper.o")
set(out "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}")
file(REMOVE "${out}" "${helper_obj}")

execute_process(
  COMMAND "${CC_BIN}" -c -O2 -o "${helper_obj}" "${OSKEL_HELPER}"
  RESULT_VARIABLE cc_exit
  OUTPUT_VARIABLE cc_out
  ERROR_VARIABLE cc_err
)
if(NOT cc_exit EQUAL 0)
  message(FATAL_ERROR
    "compiling the C helper failed (exit ${cc_exit})\n--- stdout ---\n${cc_out}\n"
    "--- stderr ---\n${cc_err}")
endif()

# `oskel build` runs `$CC ... -o <out> <object>`, so a wrapper that forwards its
# arguments and appends the helper object adds the symbol definitions without
# changing the backend.
set(cc_wrapper "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-cc.sh")
file(WRITE "${cc_wrapper}" "#!/bin/sh\nexec \"${CC_BIN}\" \"$@\" \"${helper_obj}\"\n")
file(CHMOD "${cc_wrapper}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "CC=${cc_wrapper}"
          "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode hosted -o "${out}"
  RESULT_VARIABLE build_exit
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build --mode hosted exited ${build_exit}\n--- stdout ---\n${build_stdout}\n"
    "--- stderr ---\n${build_stderr}")
endif()
if(NOT EXISTS "${out}")
  message(FATAL_ERROR "oskel build --mode hosted did not create '${out}'")
endif()

execute_process(
  COMMAND "${out}"
  RESULT_VARIABLE run_exit
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
if(NOT run_exit EQUAL 0)
  message(FATAL_ERROR
    "the hosted binary exited ${run_exit}\n--- stdout ---\n${run_stdout}\n"
    "--- stderr ---\n${run_stderr}")
endif()

# The fixture asserts its own expectations, so its final line is the whole
# contract; the VM cannot run it (it has no registry entry for these symbols).
if(NOT run_stdout MATCHES "ffi memory struct ok")
  message(FATAL_ERROR "the hosted binary did not report success:\n${run_stdout}")
endif()
