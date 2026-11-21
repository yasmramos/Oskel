# End-to-end test for SDL3 in the hosted AOT backend.
#
# Registered by CMakeLists.txt on Linux x86-64 only, and only when the build
# found SDL3: hosted mode links against the host C library with a host C
# toolchain, and the thunk marshalling is the System V AMD64 ABI.
#
# It asserts:
#   1. `oskel build --mode hosted --link-lib SDL3` exits 0 and writes the
#      requested output,
#   2. the binary runs headless (`SDL_VIDEODRIVER=dummy`) and prints exactly the
#      VM's output, which is the reference the same script produces,
#   3. the binary is dynamically linked against SDL3 -- the property that lets
#      the linker resolve the `SDL_*` symbols.
#
# Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DOSKEL_TEST_NAME=<the CTest name, used for a unique output path>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED OSKEL_TEST_NAME)
  message(FATAL_ERROR "build_hosted_sdl.cmake requires OSKEL_EXE, OSKEL_SCRIPT and OSKEL_TEST_NAME")
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()
set(out "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}${exe_suffix}")
file(REMOVE "${out}")

execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode hosted --link-lib SDL3
          -o "${out}"
  RESULT_VARIABLE build_exit
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build --mode hosted --link-lib SDL3 exited ${build_exit}\n"
    "--- stdout ---\n${build_stdout}\n--- stderr ---\n${build_stderr}")
endif()
if(NOT EXISTS "${out}")
  message(FATAL_ERROR "oskel build --mode hosted did not create '${out}'")
endif()

set(ENV{SDL_VIDEODRIVER} "dummy")
set(ENV{SDL_AUDIODRIVER} "dummy")

# The reference output is the development VM's: the hosted binary resolves the
# same SDL3 symbols and must agree.
execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}"
  INPUT_FILE "/dev/null"
  RESULT_VARIABLE vm_exit
  OUTPUT_VARIABLE vm_stdout
  ERROR_VARIABLE vm_stderr
)
if(NOT vm_exit EQUAL 0)
  message(FATAL_ERROR
    "the VM failed to run the fixture (exit ${vm_exit})\n"
    "--- stderr ---\n${vm_stderr}")
endif()

execute_process(
  COMMAND "${out}"
  INPUT_FILE "/dev/null"
  RESULT_VARIABLE run_exit
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
if(NOT run_exit EQUAL 0)
  message(FATAL_ERROR
    "the hosted binary exited ${run_exit}\n"
    "--- stdout ---\n${run_stdout}\n--- stderr ---\n${run_stderr}")
endif()
if(NOT run_stdout STREQUAL vm_stdout)
  message(FATAL_ERROR
    "the hosted binary disagrees with the VM\n"
    "--- vm ---\n${vm_stdout}\n--- binary ---\n${run_stdout}")
endif()

# The hosted image depends on SDL3, which is what lets the linker resolve the
# symbols. `ldd` is POSIX; the test is registered Linux-only.
execute_process(
  COMMAND ldd "${out}"
  RESULT_VARIABLE ldd_exit
  OUTPUT_VARIABLE ldd_out
  ERROR_VARIABLE ldd_err
)
set(ldd_text "${ldd_out}${ldd_err}")
if(NOT ldd_text MATCHES "SDL3")
  message(FATAL_ERROR "the hosted binary is not linked against SDL3:\n${ldd_text}")
endif()
