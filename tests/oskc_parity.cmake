# Proves the `.oskc` round trip is lossless end to end.
#
# A `.oskc` file is the compiled form of a program: it must run on the VM and
# lower to a native image, and both must behave exactly like the source did.
# This test drives the four paths and compares them:
#
#   1. source -> VM              (the reference)
#   2. source -> .oskc -> VM
#   3. source -> AOT binary
#   4. source -> .oskc -> AOT binary
#
# The two binaries must be byte for byte identical -- the `.oskc` path reaches
# the same `emitNativeImage` as the source path, so any difference means the
# format lost something. Every run's stdout must match the VM's, which catches a
# chunk that round-trips structurally but no longer executes the same way.
#
# Registered by CMakeLists.txt on every host the AOT backend targets. Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKELC_EXE=<path to the oskelc executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DOSKEL_TEST_NAME=<the CTest name, used for unique output paths>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKELC_EXE OR NOT DEFINED OSKEL_SCRIPT
   OR NOT DEFINED OSKEL_TEST_NAME)
  message(FATAL_ERROR
    "oskc_parity.cmake requires OSKEL_EXE, OSKELC_EXE, OSKEL_SCRIPT and OSKEL_TEST_NAME")
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()

set(image "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}.oskc")
set(from_source "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-src${exe_suffix}")
set(from_image "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-img${exe_suffix}")
file(REMOVE "${image}" "${from_source}" "${from_image}")

# 1. The reference: run the source on the VM.
execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}"
  RESULT_VARIABLE vm_exit
  OUTPUT_VARIABLE vm_stdout
  ERROR_VARIABLE vm_stderr
)
if(NOT vm_exit EQUAL 0)
  message(FATAL_ERROR
    "the VM failed to run the fixture (exit ${vm_exit})\n--- stderr ---\n${vm_stderr}")
endif()

# 2. Write the image, then run it on the VM.
execute_process(
  COMMAND "${OSKELC_EXE}" -c "${OSKEL_SCRIPT}" -o "${image}"
  RESULT_VARIABLE bytecode_exit
  OUTPUT_VARIABLE bytecode_stdout
  ERROR_VARIABLE bytecode_stderr
)
if(NOT bytecode_exit EQUAL 0)
  message(FATAL_ERROR
    "oskelc -c exited ${bytecode_exit}\n--- stdout ---\n${bytecode_stdout}\n"
    "--- stderr ---\n${bytecode_stderr}")
endif()
if(NOT EXISTS "${image}")
  message(FATAL_ERROR "oskelc -c did not create '${image}'")
endif()

execute_process(
  COMMAND "${OSKEL_EXE}" "${image}"
  RESULT_VARIABLE image_vm_exit
  OUTPUT_VARIABLE image_vm_stdout
  ERROR_VARIABLE image_vm_stderr
)
if(NOT image_vm_exit EQUAL 0)
  message(FATAL_ERROR
    "the VM failed to run the .oskc image (exit ${image_vm_exit})\n"
    "--- stderr ---\n${image_vm_stderr}")
endif()
if(NOT image_vm_stdout STREQUAL vm_stdout)
  message(FATAL_ERROR
    "the .oskc image disagrees with the source on the VM\n"
    "--- source ---\n${vm_stdout}\n--- image ---\n${image_vm_stdout}")
endif()

# 3. Build the binary from source.
execute_process(
  COMMAND "${OSKELC_EXE}" "${OSKEL_SCRIPT}" -o "${from_source}"
  RESULT_VARIABLE source_build_exit
  OUTPUT_VARIABLE source_build_stdout
  ERROR_VARIABLE source_build_stderr
)
if(NOT source_build_exit EQUAL 0)
  message(FATAL_ERROR
    "building from source exited ${source_build_exit}\n--- stdout ---\n"
    "${source_build_stdout}\n--- stderr ---\n${source_build_stderr}")
endif()

# 4. Build the binary from the image.
execute_process(
  COMMAND "${OSKELC_EXE}" "${image}" -o "${from_image}"
  RESULT_VARIABLE image_build_exit
  OUTPUT_VARIABLE image_build_stdout
  ERROR_VARIABLE image_build_stderr
)
if(NOT image_build_exit EQUAL 0)
  message(FATAL_ERROR
    "building from the .oskc image exited ${image_build_exit}\n--- stdout ---\n"
    "${image_build_stdout}\n--- stderr ---\n${image_build_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${from_source}" "${from_image}"
  RESULT_VARIABLE compare_exit
)
if(NOT compare_exit EQUAL 0)
  message(FATAL_ERROR
    "the binary built from '${OSKEL_SCRIPT}' and the one built from its .oskc "
    "image differ; the image lost something the source had")
endif()

# The image-built binary must also behave like the VM's.
execute_process(
  COMMAND "${from_image}"
  RESULT_VARIABLE run_exit
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
if(NOT run_exit EQUAL 0)
  message(FATAL_ERROR
    "the image-built binary exited ${run_exit}\n--- stdout ---\n${run_stdout}\n"
    "--- stderr ---\n${run_stderr}")
endif()
if(NOT run_stdout STREQUAL vm_stdout)
  message(FATAL_ERROR
    "the image-built binary disagrees with the VM\n"
    "--- vm ---\n${vm_stdout}\n--- binary ---\n${run_stdout}")
endif()
