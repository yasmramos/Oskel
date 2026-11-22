# End-to-end test for the project's own linker (PHASE 10.1).
#
# Registers one hosted fixture on x86-64 Linux. It asserts that the three entry
# points over the same linker core agree byte for byte and that the image
# behaves like the VM:
#
#   1. `oskel build --mode hosted --linker own` produces an executable that
#      prints exactly what the VM prints;
#   2. the executable is dynamically linked against libc.so.6 (the property that
#      lets the loader resolve the external symbols);
#   3. two builds of the same source are byte-identical (the linker emits no
#      timestamp or build id);
#   4. `--emit-object` + `oskelld` produces the same bytes as the in-process
#      link, and the result runs;
#   5. `oskelc --mode hosted --linker own` produces the same bytes as
#      `oskel build` (one compiler, two entry points);
#   6. the binary runs under `env -i`, so it does not depend on an environment
#      variable the test happened to inherit.
#
# Expects: -DOSKEL_EXE, -DOSKEL_SCRIPT, -DOSKEL_TEST_NAME, -DOSKELD_EXE,
# -DOSKELC_EXE and optionally -DOSKEL_ARGS.

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED OSKEL_TEST_NAME
   OR NOT DEFINED OSKELD_EXE OR NOT DEFINED OSKELC_EXE)
  message(FATAL_ERROR
    "build_hosted_own.cmake requires OSKEL_EXE, OSKEL_SCRIPT, OSKEL_TEST_NAME, "
    "OSKELD_EXE and OSKELC_EXE")
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()

set(out "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}${exe_suffix}")
set(out_repeat "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-repeat${exe_suffix}")
set(from_ld "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-ld${exe_suffix}")
set(from_compiler "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}-oskelc${exe_suffix}")
set(object "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}.o")
file(REMOVE "${out}" "${out_repeat}" "${from_ld}" "${from_compiler}" "${object}")

# Optional `--link-lib` plumbing: the library search directory, the library the
# binary must load, and the same library preloaded into the VM (which resolves
# the symbol with `dlsym(RTLD_DEFAULT)`).
set(own_args "")
if(DEFINED OSKEL_LINK_LIB AND NOT OSKEL_LINK_LIB STREQUAL "")
  list(APPEND own_args --link-lib "${OSKEL_LINK_LIB}")
endif()
if(DEFINED OSKEL_LIBRARY_PATH AND NOT OSKEL_LIBRARY_PATH STREQUAL "")
  list(APPEND own_args -L "${OSKEL_LIBRARY_PATH}")
  set(ENV{LD_LIBRARY_PATH} "${OSKEL_LIBRARY_PATH}")
endif()

function(oskel_own_build output)
  execute_process(
    COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode hosted --linker own
            ${own_args} -o "${output}"
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR
      "oskel build --mode hosted --linker own exited ${exit_code}\n"
      "--- stdout ---\n${stdout}\n--- stderr ---\n${stderr}")
  endif()
  if(NOT EXISTS "${output}")
    message(FATAL_ERROR "the own linker did not create '${output}'")
  endif()
endfunction()

function(oskel_expect_vm_output binary)
  execute_process(
    COMMAND "${binary}" ${OSKEL_ARGS}
    RESULT_VARIABLE run_exit
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr
  )
  if(NOT run_exit EQUAL 0)
    message(FATAL_ERROR
      "'${binary}' exited ${run_exit}\n--- stdout ---\n${run_stdout}\n"
      "--- stderr ---\n${run_stderr}")
  endif()
  if(NOT run_stdout STREQUAL vm_stdout)
    message(FATAL_ERROR
      "'${binary}' disagrees with the VM\n--- vm ---\n${vm_stdout}\n"
      "--- binary ---\n${run_stdout}")
  endif()
endfunction()

# The VM is the reference.
if(DEFINED OSKEL_VM_LD_PRELOAD AND NOT OSKEL_VM_LD_PRELOAD STREQUAL "")
  set(ENV{LD_PRELOAD} "${OSKEL_VM_LD_PRELOAD}")
endif()
execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}" ${OSKEL_ARGS}
  RESULT_VARIABLE vm_exit
  OUTPUT_VARIABLE vm_stdout
  ERROR_VARIABLE vm_stderr
)
if(NOT vm_exit EQUAL 0)
  message(FATAL_ERROR "the VM failed to run the fixture (exit ${vm_exit}):\n${vm_stderr}")
endif()
unset(ENV{LD_PRELOAD})

# 1. The in-process link.
oskel_own_build("${out}")
oskel_expect_vm_output("${out}")

# 2. A libc-backed image.
find_program(OSKEL_LDD NAMES ldd)
if(OSKEL_LDD)
  execute_process(
    COMMAND "${OSKEL_LDD}" "${out}"
    RESULT_VARIABLE ldd_exit
    OUTPUT_VARIABLE ldd_stdout
    ERROR_VARIABLE ldd_stderr
  )
  set(ldd_text "${ldd_stdout}${ldd_stderr}")
  if(NOT ldd_text MATCHES "libc[.]so[.]6")
    message(FATAL_ERROR
      "the own linker did not produce a libc-backed image:\n${ldd_text}")
  endif()
endif()

# 3. Determinism.
oskel_own_build("${out_repeat}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${out}" "${out_repeat}"
  RESULT_VARIABLE compare_exit
)
if(NOT compare_exit EQUAL 0)
  message(FATAL_ERROR "two own-linker builds of the same source differ")
endif()

# 4. `--emit-object` + `oskelld`.
execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode hosted --emit-object
          ${own_args} -o "${object}"
  RESULT_VARIABLE object_exit
  OUTPUT_VARIABLE object_stdout
  ERROR_VARIABLE object_stderr
)
if(NOT object_exit EQUAL 0 OR NOT EXISTS "${object}")
  message(FATAL_ERROR
    "oskel build --emit-object exited ${object_exit}\n"
    "--- stdout ---\n${object_stdout}\n--- stderr ---\n${object_stderr}")
endif()
execute_process(
  COMMAND "${OSKELD_EXE}" "${object}" ${own_args} -o "${from_ld}"
  RESULT_VARIABLE ld_exit
  OUTPUT_VARIABLE ld_stdout
  ERROR_VARIABLE ld_stderr
)
if(NOT ld_exit EQUAL 0)
  message(FATAL_ERROR
    "oskelld exited ${ld_exit}\n--- stdout ---\n${ld_stdout}\n"
    "--- stderr ---\n${ld_stderr}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${out}" "${from_ld}"
  RESULT_VARIABLE ld_compare
)
if(NOT ld_compare EQUAL 0)
  message(FATAL_ERROR "oskelld and `oskel build --linker own` disagree")
endif()
oskel_expect_vm_output("${from_ld}")

# 5. `oskelc` is the same compiler.
execute_process(
  COMMAND "${OSKELC_EXE}" "${OSKEL_SCRIPT}" --mode hosted --linker own
          ${own_args} -o "${from_compiler}"
  RESULT_VARIABLE compiler_exit
  OUTPUT_VARIABLE compiler_stdout
  ERROR_VARIABLE compiler_stderr
)
if(NOT compiler_exit EQUAL 0)
  message(FATAL_ERROR
    "oskelc --linker own exited ${compiler_exit}\n"
    "--- stdout ---\n${compiler_stdout}\n--- stderr ---\n${compiler_stderr}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${out}" "${from_compiler}"
  RESULT_VARIABLE compiler_compare
)
if(NOT compiler_compare EQUAL 0)
  message(FATAL_ERROR "oskelc and `oskel build` produced different own-linker images")
endif()

# 6. No inherited environment. A `--link-lib` fixture legitimately needs
# `LD_LIBRARY_PATH`, so this check applies only to the self-contained cases.
find_program(OSKEL_ENV NAMES env)
if(OSKEL_ENV AND (NOT DEFINED OSKEL_LINK_LIB OR OSKEL_LINK_LIB STREQUAL ""))
  execute_process(
    COMMAND "${OSKEL_ENV}" -i "${out}" ${OSKEL_ARGS}
    RESULT_VARIABLE clean_exit
    OUTPUT_VARIABLE clean_stdout
    ERROR_VARIABLE clean_stderr
  )
  if(NOT clean_exit EQUAL 0)
    message(FATAL_ERROR
      "the image failed under 'env -i' (exit ${clean_exit}):\n${clean_stderr}")
  endif()
  if(NOT clean_stdout STREQUAL vm_stdout)
    message(FATAL_ERROR
      "the image disagrees with the VM under 'env -i'\n"
      "--- vm ---\n${vm_stdout}\n--- binary ---\n${clean_stdout}")
  endif()
endif()
