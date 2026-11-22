# End-to-end test for scalar `extern "C"` resolution in the hosted AOT backend
# (PHASE 7f.5): compile a program that calls real C functions and run it.
#
# Registered by CMakeLists.txt on Linux and macOS x86-64 (the System V surface)
# and Windows x86-64 (the scalar/pointer and C-variadic surface): hosted mode
# links against the host C library with a C toolchain. Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DOSKEL_TEST_NAME=<the CTest name, used for a unique output path>
#   -DOSKEL_CC=<the C compiler the hosted link should drive, optional>
#   -DOSKEL_SKIP_RUN=<skip the run, e.g. an x86-64 image on arm64 macOS>
#   -DOSKEL_EXPECTED_OUTPUT=<a line the binary must print, instead of the VM>
#
# It asserts:
#   1. `oskel build --mode hosted` exits 0 and writes the requested output,
#   2. the generated binary runs and prints exactly the VM's output (the
#      marshalling must reproduce the hosted engine, which resolves the same
#      symbols through its own host registry); when `OSKEL_EXPECTED_OUTPUT` is
#      given the VM is not run, because that engine cannot execute the fixture
#      on this host (a Windows VM has no variadic path, for instance),
#   3. the binary is dynamically linked against libc/libSystem -- the property
#      that lets the linker resolve the `extern "C"` symbol in the first place.

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED OSKEL_TEST_NAME)
  message(FATAL_ERROR "build_hosted_extern.cmake requires OSKEL_EXE, OSKEL_SCRIPT and OSKEL_TEST_NAME")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/hosted_cc.cmake")
oskel_prepare_hosted_cc()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()
set(out "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}${exe_suffix}")
file(REMOVE "${out}")

execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode hosted -o "${out}"
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

if(NOT DEFINED OSKEL_SKIP_RUN OR NOT OSKEL_SKIP_RUN)
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

  if(DEFINED OSKEL_EXPECTED_OUTPUT)
    # The development VM cannot run this fixture on this host (a Windows VM
    # cannot resolve its `snprintf`, for instance), so the fixture asserts its
    # own expectations and only its final line is the contract.
    if(NOT run_stdout MATCHES "${OSKEL_EXPECTED_OUTPUT}")
      message(FATAL_ERROR
        "the hosted binary did not report success:\n${run_stdout}")
    endif()
  else()
    # The reference output is the development VM's: the hosted binary resolves
    # the same C symbols and must agree.
    execute_process(
      COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}"
      RESULT_VARIABLE vm_exit
      OUTPUT_VARIABLE vm_stdout
    )
    if(NOT vm_exit EQUAL 0)
      message(FATAL_ERROR "the VM failed to run the fixture (exit ${vm_exit})")
    endif()
    if(NOT run_stdout STREQUAL vm_stdout)
      message(FATAL_ERROR
        "the hosted binary disagrees with the VM\n"
        "--- vm ---\n${vm_stdout}\n--- binary ---\n${run_stdout}")
    endif()
  endif()
endif()

# The hosted image depends on the host C library, which is what lets the linker
# resolve the `extern "C"` symbol.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  execute_process(
    COMMAND otool -L "${out}"
    RESULT_VARIABLE ldd_exit
    OUTPUT_VARIABLE ldd_out
    ERROR_VARIABLE ldd_err
  )
  set(ldd_text "${ldd_out}${ldd_err}")
  if(NOT ldd_text MATCHES "libSystem")
    message(FATAL_ERROR "the hosted binary is not linked against libSystem:\n${ldd_text}")
  endif()
elseif(UNIX)
  execute_process(
    COMMAND ldd "${out}"
    RESULT_VARIABLE ldd_exit
    OUTPUT_VARIABLE ldd_out
    ERROR_VARIABLE ldd_err
  )
  set(ldd_text "${ldd_out}${ldd_err}")
  if(NOT ldd_text MATCHES "libc")
    message(FATAL_ERROR "the hosted binary is not linked against libc:\n${ldd_text}")
  endif()
endif()
