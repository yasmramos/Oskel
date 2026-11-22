# End-to-end test for a hosted `extern "C"` fixture that needs a C helper.
#
# The fixture binds functions no libc symbol provides: class-MEMORY structs
# (PHASE 7f.7) and seven/eight-argument stack spills (PHASE 7f.5). `oskel build
# --mode hosted` links with `$CC`, so the helper is compiled to an object and a
# wrapper script appends it to the link command; that keeps the backend's own
# command line untouched.
#
# Registered by CMakeLists.txt on Linux x86-64 and Windows x86-64 (the helper
# is compiled with the same compiler the hosted link drives). Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DOSKEL_HELPER=<the C helper source to compile in>
#   -DOSKEL_TEST_NAME=<the CTest name, used for unique output paths>
#   -DOSKEL_EXPECTED_OUTPUT=<the fixture's final line, default "ffi memory struct ok">

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED OSKEL_TEST_NAME OR
   NOT DEFINED OSKEL_HELPER)
  message(FATAL_ERROR
    "build_hosted_helper.cmake requires OSKEL_EXE, OSKEL_SCRIPT, "
    "OSKEL_HELPER and OSKEL_TEST_NAME")
endif()
if(NOT DEFINED OSKEL_EXPECTED_OUTPUT)
  set(OSKEL_EXPECTED_OUTPUT "ffi memory struct ok")
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  include("${CMAKE_CURRENT_LIST_DIR}/hosted_cc.cmake")
  oskel_prepare_hosted_cc()
  set(cc_prepared "$ENV{CC}")
  if(cc_prepared STREQUAL "")
    message(FATAL_ERROR "build_hosted_helper.cmake found no Windows C compiler")
  endif()
  set(helper_obj "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-helper.obj")
  set(out "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}.exe")
  file(REMOVE "${out}" "${helper_obj}")

  # `cl` is reached through the vcvarsall wrapper `hosted_cc.cmake` built; a
  # compiler that needed no wrapper is invoked directly.
  if(cc_prepared MATCHES "\\.bat$")
    set(cc_compile cmd /c call "${cc_prepared}")
  else()
    set(cc_compile "${cc_prepared}")
  endif()
  execute_process(
    COMMAND ${cc_compile} /c /O2 "/Fo${helper_obj}" "${OSKEL_HELPER}"
    RESULT_VARIABLE cc_exit
    OUTPUT_VARIABLE cc_out
    ERROR_VARIABLE cc_err
  )
  if(NOT cc_exit EQUAL 0)
    message(FATAL_ERROR
      "compiling the C helper failed (exit ${cc_exit})\n--- stdout ---\n${cc_out}\n"
      "--- stderr ---\n${cc_err}")
  endif()

  # The link command starts with `$CC`, so a wrapper that forwards its
  # arguments and appends the helper object adds the definitions without
  # changing the backend. `OSKEL_CC_KIND` makes the link builder use the MSVC
  # command line.
  set(cc_wrapper "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-cc.bat")
  file(TO_NATIVE_PATH "${cc_prepared}" native_prepared)
  file(TO_NATIVE_PATH "${helper_obj}" native_helper)
  if(cc_prepared MATCHES "\\.bat$")
    set(cc_link "call \"${native_prepared}\" %* \"${native_helper}\"")
  else()
    set(cc_link "\"${native_prepared}\" %* \"${native_helper}\"")
  endif()
  file(WRITE "${cc_wrapper}" "@echo off\r\n${cc_link}\r\n")
  set(ENV{CC} "${cc_wrapper}")
  if("$ENV{OSKEL_CC_KIND}" STREQUAL "")
    if(OSKEL_CC MATCHES "(cl|clang-cl)(\\.exe)?$")
      set(ENV{OSKEL_CC_KIND} "msvc")
    else()
      set(ENV{OSKEL_CC_KIND} "posix")
    endif()
  endif()
else()
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

  # `oskel build` runs `$CC ... -o <out> <object>`, so a wrapper that forwards
  # its arguments and appends the helper object adds the symbol definitions
  # without changing the backend.
  set(cc_wrapper "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-cc.sh")
  file(WRITE "${cc_wrapper}" "#!/bin/sh\nexec \"${CC_BIN}\" \"$@\" \"${helper_obj}\"\n")
  file(CHMOD "${cc_wrapper}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
  set(ENV{CC} "${cc_wrapper}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "CC=$ENV{CC}"
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
if(NOT run_stdout MATCHES "${OSKEL_EXPECTED_OUTPUT}")
  message(FATAL_ERROR "the hosted binary did not report success:\n${run_stdout}")
endif()
