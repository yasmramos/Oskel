# End-to-end test for a runtime error in an AOT binary: `oskel build` must
# succeed, the generated binary must run the program up to the failing
# instruction, report the same message the VM reports and exit with the same
# code (70), never reaching the instruction after it.
#
# Registered by CMakeLists.txt on every host the AOT backend targets. Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DOSKEL_TEST_NAME=<the CTest name, used for a unique output path>
#   -DOSKEL_EXPECTED_EXIT=<the runtime-error code the VM also uses>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED OSKEL_TEST_NAME OR NOT DEFINED OSKEL_EXPECTED_EXIT)
  message(FATAL_ERROR "aot_runtime_error.cmake requires OSKEL_EXE, OSKEL_SCRIPT, OSKEL_TEST_NAME and OSKEL_EXPECTED_EXIT")
endif()

# Match the host's executable suffix, so the binary is written and launched
# under the name the platform expects (see build_binary.cmake).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()
# The output path embeds the test name (see build_binary.cmake): parallel tests
# share `CMAKE_CURRENT_BINARY_DIR`, so a fixed name would collide.
set(out "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}${exe_suffix}")
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

# The reference is the development VM: same stderr text, same exit code.
execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}"
  RESULT_VARIABLE vm_exit
  OUTPUT_VARIABLE vm_stdout
  ERROR_VARIABLE vm_stderr
)
if(NOT vm_exit EQUAL OSKEL_EXPECTED_EXIT)
  message(FATAL_ERROR "the VM exited ${vm_exit}, expected ${OSKEL_EXPECTED_EXIT}")
endif()

execute_process(
  COMMAND "${out}"
  RESULT_VARIABLE run_exit
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
if(NOT run_exit EQUAL OSKEL_EXPECTED_EXIT)
  message(FATAL_ERROR
    "the generated binary exited ${run_exit}, expected ${OSKEL_EXPECTED_EXIT}\n"
    "--- stdout ---\n${run_stdout}\n--- stderr ---\n${run_stderr}")
endif()
if(NOT run_stdout STREQUAL vm_stdout)
  message(FATAL_ERROR
    "the generated binary disagrees with the VM on stdout\n"
    "--- vm ---\n${vm_stdout}\n--- binary ---\n${run_stdout}")
endif()
# The expected wording is the caller's: a fixture pins its own message, so one
# harness serves every runtime error and a new fixture never has to fork it.
if(DEFINED OSKEL_EXPECTED_MESSAGE)
  set(expected_message "${OSKEL_EXPECTED_MESSAGE}")
else()
  set(expected_message "chr\\(\\) code point is a surrogate")
endif()
if(NOT run_stderr MATCHES "${expected_message}")
  message(FATAL_ERROR
    "the generated binary does not report the expected error:\n"
    "--- expected (regex) ---\n${expected_message}\n--- stderr ---\n${run_stderr}")
endif()

# PHASE 7g.3: the binary must name the source position, like the VM does, so a
# runtime error is locatable without a debugger. The reference is the VM's own
# report, which the harness already ran: `FILE:LINE:` there, `oskel: FILE:LINE:`
# here.
if(NOT vm_stderr STREQUAL "")
  string(REGEX MATCH "^[:\n]*([^:\n]*):([0-9]+):" vm_position "${vm_stderr}")
  if(vm_position)
    set(vm_line "${CMAKE_MATCH_2}")
    get_filename_component(vm_file "${OSKEL_SCRIPT}" NAME)
    if(NOT run_stderr MATCHES "oskel: [^:\n]*${vm_file}:${vm_line}:")
      message(FATAL_ERROR
        "the generated binary does not name the failing position "
        "(${vm_file}:${vm_line}):\n--- stderr ---\n${run_stderr}")
    endif()
  endif()
endif()

# `--no-source-positions` is the escape hatch that reproduces the frozen
# freestanding image byte for byte (AGENTS.md). It must drop the position from
# the report *and* change the emitted bytes -- a flag that silently did nothing
# would leave the frozen-image guarantee untested.
if(DEFINED OSKEL_CHECK_NO_POSITIONS)
  set(frozen "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-frozen${exe_suffix}")
  file(REMOVE "${frozen}")
  execute_process(
    COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" -o "${frozen}"
            --no-source-positions
    RESULT_VARIABLE frozen_build_exit
    OUTPUT_VARIABLE frozen_build_stdout
    ERROR_VARIABLE frozen_build_stderr
  )
  if(NOT frozen_build_exit EQUAL 0)
    message(FATAL_ERROR
      "oskel build --no-source-positions exited ${frozen_build_exit}\n"
      "--- stderr ---\n${frozen_build_stderr}")
  endif()
  execute_process(
    COMMAND "${frozen}"
    RESULT_VARIABLE frozen_exit
    OUTPUT_VARIABLE frozen_stdout
    ERROR_VARIABLE frozen_stderr
  )
  if(NOT frozen_exit EQUAL OSKEL_EXPECTED_EXIT)
    message(FATAL_ERROR
      "the frozen binary exited ${frozen_exit}, expected ${OSKEL_EXPECTED_EXIT}")
  endif()
  if(frozen_stderr MATCHES "oskel: [^:\n]*:[0-9]+:")
    message(FATAL_ERROR
      "--no-source-positions still names a position:\n${frozen_stderr}")
  endif()
  if(NOT frozen_stderr MATCHES "oskel: ")
    message(FATAL_ERROR
      "the frozen binary lost the position but not the tool prefix:\n"
      "${frozen_stderr}")
  endif()
  file(SIZE "${out}" debug_size)
  file(SIZE "${frozen}" frozen_size)
  if(debug_size EQUAL frozen_size)
    message(FATAL_ERROR "--no-source-positions did not change the image")
  endif()
endif()
