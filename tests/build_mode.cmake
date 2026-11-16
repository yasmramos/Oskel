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
#
# It asserts:
#   1. `--mode freestanding` behaves exactly like the default: it builds a
#      self-contained binary that runs and agrees with the VM,
#   2. `--mode hosted` is accepted by the CLI but, until the object emitter
#      lands (PHASE 7f.4), fails cleanly with exit 65 and writes no binary --
#      never a half-linked image,
#   3. an unknown mode is a usage error (exit 64) that names the valid modes.

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT)
  message(FATAL_ERROR "build_mode.cmake requires OSKEL_EXE and OSKEL_SCRIPT")
endif()

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
if(NOT fs_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build --mode freestanding exited ${fs_exit}\n--- stdout ---\n${fs_stdout}\n"
    "--- stderr ---\n${fs_stderr}")
endif()
if(NOT EXISTS "${out_freestanding}")
  message(FATAL_ERROR "oskel build --mode freestanding did not create '${out_freestanding}'")
endif()

# The reference output is the development VM's: the AOT binary must agree.
execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}"
  RESULT_VARIABLE vm_exit
  OUTPUT_VARIABLE vm_stdout
)
if(NOT vm_exit EQUAL 0)
  message(FATAL_ERROR "the VM failed to run the fixture (exit ${vm_exit})")
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

# --- 2. `--mode hosted` fails cleanly until the object emitter exists ---
set(out_hosted "${CMAKE_CURRENT_BINARY_DIR}/oskel_build_mode_hosted_${script_stem}${exe_suffix}")
file(REMOVE "${out_hosted}")
execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode hosted -o "${out_hosted}"
  RESULT_VARIABLE hosted_exit
  OUTPUT_VARIABLE hosted_stdout
  ERROR_VARIABLE hosted_stderr
)
if(NOT hosted_exit EQUAL 65)
  message(FATAL_ERROR
    "oskel build --mode hosted exited ${hosted_exit}, expected 65\n--- stdout ---\n"
    "${hosted_stdout}\n--- stderr ---\n${hosted_stderr}")
endif()
if(EXISTS "${out_hosted}")
  message(FATAL_ERROR "oskel build --mode hosted wrote '${out_hosted}' despite the error")
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
