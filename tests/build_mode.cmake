# End-to-end test for `oskel build --mode` (PHASE 7f.3).
#
# The AOT backend has two link modes: `freestanding` (the default, self-contained)
# and `hosted` (links libc; the mode that makes FFI work). This runner pins the
# CLI surface of the mode flag, which is a deployment choice and never language
# syntax, and the (target, mode) matrix the CLI validates.
#
# Registered by CMakeLists.txt. Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DOSKEL_CC=<the C compiler the hosted link should drive, optional>
#
# It asserts:
#   1. `--mode freestanding` behaves exactly like the default: it builds a
#      self-contained binary that runs and agrees with the VM,
#   2. `--mode hosted` (PHASE 7f.4) links the object against libc and produces
#      a binary that runs and agrees with the VM; where the (target, mode) pair
#      has no backend it fails cleanly with exit 65 and writes no binary -- never
#      a half-linked image,
#   3. an unknown mode is a usage error (exit 64) that names the valid modes.

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT)
  message(FATAL_ERROR "build_mode.cmake requires OSKEL_EXE and OSKEL_SCRIPT")
endif()

# The hosted link reads `$CC` at run time. Point it at the compiler CMake found
# so the test does not depend on `cl` (Windows) or `cc` (Unix) being on PATH in
# the `ctest` environment.
include("${CMAKE_CURRENT_LIST_DIR}/hosted_cc.cmake")
oskel_prepare_hosted_cc()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()
get_filename_component(script_stem "${OSKEL_SCRIPT}" NAME_WE)

# --- 1. `--mode freestanding` matches the default ---
set(out_freestanding "${CMAKE_CURRENT_BINARY_DIR}/oskel_build_mode_freestanding_${script_stem}${exe_suffix}")
file(REMOVE "${out_freestanding}")
execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode freestanding -o "${out_freestanding}"
  RESULT_VARIABLE fs_exit
  OUTPUT_VARIABLE fs_stdout
  ERROR_VARIABLE fs_stderr
)
# The reference output is the development VM's: the AOT binary must agree.
execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}"
  RESULT_VARIABLE vm_exit
  OUTPUT_VARIABLE vm_stdout
)
if(NOT vm_exit EQUAL 0)
  message(FATAL_ERROR "the VM failed to run the fixture (exit ${vm_exit})")
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  # macOS has no freestanding backend: the default mode must be refused
  # cleanly, name the mode, and write no image.
  if(NOT fs_exit EQUAL 65)
    message(FATAL_ERROR
      "oskel build --mode freestanding exited ${fs_exit}, expected 65 on macOS\n"
      "--- stdout ---\n${fs_stdout}\n--- stderr ---\n${fs_stderr}")
  endif()
  if(EXISTS "${out_freestanding}")
    message(FATAL_ERROR "oskel build --mode freestanding wrote '${out_freestanding}' despite the error")
  endif()
  if(NOT fs_stderr MATCHES "freestanding")
    message(FATAL_ERROR
      "the unsupported-mode error does not name the mode:\n--- stderr ---\n${fs_stderr}")
  endif()
else()
  if(NOT fs_exit EQUAL 0)
    message(FATAL_ERROR
      "oskel build --mode freestanding exited ${fs_exit}\n--- stdout ---\n${fs_stdout}\n"
      "--- stderr ---\n${fs_stderr}")
  endif()
  if(NOT EXISTS "${out_freestanding}")
    message(FATAL_ERROR "oskel build --mode freestanding did not create '${out_freestanding}'")
  endif()
  execute_process(
    COMMAND "${out_freestanding}"
    RESULT_VARIABLE fs_run_exit
    OUTPUT_VARIABLE fs_run_stdout
    ERROR_VARIABLE fs_run_stderr
  )
  if(NOT fs_run_exit EQUAL 0)
    message(FATAL_ERROR
      "the freestanding binary exited ${fs_run_exit}\n--- stdout ---\n${fs_run_stdout}\n"
      "--- stderr ---\n${fs_run_stderr}")
  endif()
  if(NOT fs_run_stdout STREQUAL vm_stdout)
    message(FATAL_ERROR
      "the freestanding binary disagrees with the VM\n--- vm ---\n${vm_stdout}\n"
      "--- binary ---\n${fs_run_stdout}")
  endif()

  # A freestanding image must stay free of a shared runtime, exactly like the
  # default build. The check needs `ldd`, so it is guarded by UNIX.
  if(UNIX)
    execute_process(
      COMMAND ldd "${out_freestanding}"
      RESULT_VARIABLE ldd_exit
      OUTPUT_VARIABLE ldd_out
      ERROR_VARIABLE ldd_err
    )
    set(ldd_text "${ldd_out}${ldd_err}")
    if(NOT ldd_text MATCHES "not a dynamic executable|statically linked")
      message(FATAL_ERROR "the freestanding binary is dynamically linked:\n${ldd_text}")
    endif()
  endif()
endif()

# --- 2. `--mode hosted` links against libc (PHASE 7f.4) ---
#
# Hosted mode needs a C toolchain at build time and a host C library to link
# against; Linux, Windows and macOS x86-64 have a backend. The runner does not
# detect the platform itself (a CMake script has no reliable processor
# variable): it lets the build decide. A success must produce a binary that runs
# and agrees with the VM and is dynamically linked (the whole point of the
# mode); a refusal must be the clean exit 65 with no binary and a message that
# names the mode.
set(out_hosted "${CMAKE_CURRENT_BINARY_DIR}/oskel_build_mode_hosted_${script_stem}${exe_suffix}")
file(REMOVE "${out_hosted}")
execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode hosted -o "${out_hosted}"
  RESULT_VARIABLE hosted_exit
  OUTPUT_VARIABLE hosted_stdout
  ERROR_VARIABLE hosted_stderr
)

if(hosted_exit EQUAL 0)
  if(NOT EXISTS "${out_hosted}")
    message(FATAL_ERROR "oskel build --mode hosted did not create '${out_hosted}'")
  endif()
  # The backend emits x86-64. On an arm64 macOS host the image links, but it
  # only runs under Rosetta; do not make the test depend on that being
  # installed.
  set(can_run_hosted TRUE)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND
     NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(can_run_hosted FALSE)
  endif()
  if(can_run_hosted)
    execute_process(
      COMMAND "${out_hosted}"
      RESULT_VARIABLE hosted_run_exit
      OUTPUT_VARIABLE hosted_run_stdout
      ERROR_VARIABLE hosted_run_stderr
    )
    if(NOT hosted_run_exit EQUAL 0)
      message(FATAL_ERROR
        "the hosted binary exited ${hosted_run_exit}\n--- stdout ---\n${hosted_run_stdout}\n"
        "--- stderr ---\n${hosted_run_stderr}")
    endif()
    if(NOT hosted_run_stdout STREQUAL vm_stdout)
      message(FATAL_ERROR
        "the hosted binary disagrees with the VM\n--- vm ---\n${vm_stdout}\n"
        "--- binary ---\n${hosted_run_stdout}")
    endif()
  endif()
  # The hosted image depends on the host C library -- that is the difference
  # from the freestanding one, and what lets an `extern "C"` symbol resolve.
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    execute_process(
      COMMAND otool -L "${out_hosted}"
      RESULT_VARIABLE hosted_ldd_exit
      OUTPUT_VARIABLE hosted_ldd_out
      ERROR_VARIABLE hosted_ldd_err
    )
    set(hosted_ldd_text "${hosted_ldd_out}${hosted_ldd_err}")
    if(NOT hosted_ldd_text MATCHES "libSystem")
      message(FATAL_ERROR "the hosted binary is not linked against libSystem:\n${hosted_ldd_text}")
    endif()
  elseif(UNIX)
    execute_process(
      COMMAND ldd "${out_hosted}"
      RESULT_VARIABLE hosted_ldd_exit
      OUTPUT_VARIABLE hosted_ldd_out
      ERROR_VARIABLE hosted_ldd_err
    )
    set(hosted_ldd_text "${hosted_ldd_out}${hosted_ldd_err}")
    if(NOT hosted_ldd_text MATCHES "libc")
      message(FATAL_ERROR "the hosted binary is not linked against libc:\n${hosted_ldd_text}")
    endif()
  endif()
else()
  # The (target, mode) pair has no backend here: it must fail cleanly, naming
  # the mode, and never write a binary.
  if(NOT hosted_exit EQUAL 65)
    message(FATAL_ERROR
      "oskel build --mode hosted exited ${hosted_exit}, expected 0 or 65\n"
      "--- stdout ---\n${hosted_stdout}\n--- stderr ---\n${hosted_stderr}")
  endif()
  if(EXISTS "${out_hosted}")
    message(FATAL_ERROR "oskel build --mode hosted wrote '${out_hosted}' despite the error")
  endif()
  if(NOT hosted_stderr MATCHES "hosted")
    message(FATAL_ERROR
      "the unsupported-mode error does not name the mode:\n--- stderr ---\n${hosted_stderr}")
  endif()
endif()

# --- 3. An unknown mode is a usage error that names the valid modes ---
execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode definitely-not-a-mode
          -o "${out_hosted}"
  RESULT_VARIABLE unknown_exit
  OUTPUT_VARIABLE unknown_stdout
  ERROR_VARIABLE unknown_stderr
)
if(NOT unknown_exit EQUAL 64)
  message(FATAL_ERROR
    "oskel build --mode definitely-not-a-mode exited ${unknown_exit}, expected 64\n"
    "--- stdout ---\n${unknown_stdout}\n--- stderr ---\n${unknown_stderr}")
endif()
if(NOT unknown_stderr MATCHES "freestanding" OR NOT unknown_stderr MATCHES "hosted")
  message(FATAL_ERROR
    "the unknown-mode error does not name the valid modes:\n--- stderr ---\n${unknown_stderr}")
endif()
