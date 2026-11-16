# End-to-end test for `oskel build` (PHASE 7d/7e): compile a .oskl program to a
# native binary and run it.
#
# Registered by CMakeLists.txt on every host the AOT backend targets (Linux
# x86-64 and Windows x86-64). Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#
# It asserts:
#   1. `oskel build` exits 0 and writes the requested output,
#   2. the generated binary runs and prints exactly the VM's output,
#   3. the binary is statically linked (no dynamic dependencies) -- a check the
#      ELF target can make with `ldd` and the PE target cannot, so it is guarded
#      by `UNIX`. The PE is static by construction (kernel32.dll is its only
#      import), so nothing is lost there.

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT)
  message(FATAL_ERROR "build_binary.cmake requires OSKEL_EXE and OSKEL_SCRIPT")
endif()

# The output keeps the host's executable suffix: the Windows loader appends
# `.exe` when a command names an extension-less path, so the binary must be
# written and launched under the same name the platform expects. The script runs
# under `cmake -P`, where `CMAKE_EXECUTABLE_SUFFIX` is not populated, so the
# suffix is derived from the host name instead.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()
set(out "${CMAKE_CURRENT_BINARY_DIR}/oskel_build_output${exe_suffix}")
file(REMOVE "${out}")

execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" -o "${out}"
  RESULT_VARIABLE build_exit
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build exited ${build_exit}\n--- stdout ---\n${build_stdout}\n"
    "--- stderr ---\n${build_stderr}")
endif()
if(NOT EXISTS "${out}")
  message(FATAL_ERROR "oskel build did not create '${out}'")
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
  COMMAND "${out}"
  RESULT_VARIABLE run_exit
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
if(NOT run_exit EQUAL 0)
  message(FATAL_ERROR
    "the generated binary exited ${run_exit}\n--- stdout ---\n${run_stdout}\n"
    "--- stderr ---\n${run_stderr}")
endif()
if(NOT run_stdout STREQUAL vm_stdout)
  message(FATAL_ERROR
    "the generated binary disagrees with the VM\n"
    "--- vm ---\n${vm_stdout}\n--- binary ---\n${run_stdout}")
endif()

# A freestanding ELF binary must not depend on a shared runtime. The PE target
# has no `ldd` and needs none: a PE that imports only kernel32.dll is static by
# construction.
if(UNIX)
  execute_process(
    COMMAND ldd "${out}"
    RESULT_VARIABLE ldd_exit
    OUTPUT_VARIABLE ldd_out
    ERROR_VARIABLE ldd_err
  )
  set(ldd_text "${ldd_out}${ldd_err}")
  if(NOT ldd_text MATCHES "not a dynamic executable|statically linked")
    message(FATAL_ERROR "the generated binary is dynamically linked:\n${ldd_text}")
  endif()
endif()
