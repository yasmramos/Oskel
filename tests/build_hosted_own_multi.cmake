# End-to-end test for PHASE 10.2: multiple objects and `ar` archives.
#
# The main object is `oskel build --emit-object`; the helper is a plain C-ABI
# object built by CMake. The fixture is linked:
#
#   1. `oskelld main.o helper.o`
#   2. `oskel build --linker own --link-object helper.o`
#   3. `oskelld -r main.o helper.o` then `oskelld combined.o`
#   4. `oskelld main.o libhelper.a` (when `ar` is available)
#   5. `oskel build --linker host --link-object helper.o`
#
# Every executable must print the literal expected output, and the own-linker
# images must be byte-identical. `--link-object` with `--emit-object` or a
# freestanding mode is a usage error.
#
# Expects: -DOSKEL_EXE, -DOSKEL_SCRIPT, -DOSKEL_TEST_NAME, -DOSKELD_EXE,
# -DOSKELC_EXE, -DOSKEL_HELPER_OBJECT, -DOSKEL_EXPECTED_OUTPUT and optionally
# -DOSKEL_AR.

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED OSKEL_TEST_NAME
   OR NOT DEFINED OSKELD_EXE OR NOT DEFINED OSKELC_EXE
   OR NOT DEFINED OSKEL_HELPER_OBJECT OR NOT DEFINED OSKEL_EXPECTED_OUTPUT)
  message(FATAL_ERROR
    "build_hosted_own_multi.cmake requires OSKEL_EXE, OSKEL_SCRIPT, "
    "OSKEL_TEST_NAME, OSKELD_EXE, OSKELC_EXE, OSKEL_HELPER_OBJECT and "
    "OSKEL_EXPECTED_OUTPUT")
endif()

set(exe_suffix "")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
endif()

set(dir "${CMAKE_CURRENT_BINARY_DIR}")
set(main_object "${dir}/${OSKEL_TEST_NAME}.o")
set(combined "${dir}/${OSKEL_TEST_NAME}-combined.o")
set(archive "${dir}/${OSKEL_TEST_NAME}.a")
set(own "${dir}/${OSKEL_TEST_NAME}${exe_suffix}")
set(from_ld "${dir}/${OSKEL_TEST_NAME}-ld${exe_suffix}")
set(from_r "${dir}/${OSKEL_TEST_NAME}-r${exe_suffix}")
set(from_ar "${dir}/${OSKEL_TEST_NAME}-ar${exe_suffix}")
set(from_host "${dir}/${OSKEL_TEST_NAME}-host${exe_suffix}")
set(from_compiler "${dir}/${OSKEL_TEST_NAME}-oskelc${exe_suffix}")
file(REMOVE "${main_object}" "${combined}" "${archive}" "${own}" "${from_ld}"
     "${from_r}" "${from_ar}" "${from_host}" "${from_compiler}")

set(expected "${OSKEL_EXPECTED_OUTPUT}\n")

function(oskel_multi_run binary label)
  execute_process(
    COMMAND "${binary}"
    RESULT_VARIABLE run_exit
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr)
  if(NOT run_exit EQUAL 0)
    message(FATAL_ERROR
      "'${label}' exited ${run_exit}\n--- stdout ---\n${run_stdout}\n"
      "--- stderr ---\n${run_stderr}")
  endif()
  if(NOT run_stdout STREQUAL expected)
    message(FATAL_ERROR
      "'${label}' disagrees with the expected output\n"
      "--- expected ---\n${expected}\n--- actual ---\n${run_stdout}")
  endif()
endfunction()

function(oskel_multi_compare a b label)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${a}" "${b}"
    RESULT_VARIABLE compare_exit)
  if(NOT compare_exit EQUAL 0)
    message(FATAL_ERROR "${label}")
  endif()
endfunction()

function(oskel_multi_own_build output)
  execute_process(
    COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode hosted --linker own
            --link-object "${OSKEL_HELPER_OBJECT}" -o "${output}"
    RESULT_VARIABLE build_exit
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)
  if(NOT build_exit EQUAL 0 OR NOT EXISTS "${output}")
    message(FATAL_ERROR
      "oskel build --linker own --link-object exited ${build_exit}\n"
      "--- stdout ---\n${build_stdout}\n--- stderr ---\n${build_stderr}")
  endif()
endfunction()

# 0. The main object.
execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode hosted --emit-object
          -o "${main_object}"
  RESULT_VARIABLE object_exit
  OUTPUT_VARIABLE object_stdout
  ERROR_VARIABLE object_stderr)
if(NOT object_exit EQUAL 0 OR NOT EXISTS "${main_object}")
  message(FATAL_ERROR
    "oskel build --emit-object exited ${object_exit}\n"
    "--- stdout ---\n${object_stdout}\n--- stderr ---\n${object_stderr}")
endif()

# 1. `oskelld` with two objects.
execute_process(
  COMMAND "${OSKELD_EXE}" "${main_object}" "${OSKEL_HELPER_OBJECT}"
          -o "${from_ld}"
  RESULT_VARIABLE ld_exit
  OUTPUT_VARIABLE ld_stdout
  ERROR_VARIABLE ld_stderr)
if(NOT ld_exit EQUAL 0)
  message(FATAL_ERROR
    "oskelld exited ${ld_exit}\n--- stdout ---\n${ld_stdout}\n"
    "--- stderr ---\n${ld_stderr}")
endif()
oskel_multi_run("${from_ld}" "oskelld with two objects")

# 2. The in-process link with `--link-object`.
oskel_multi_own_build("${own}")
oskel_multi_run("${own}" "oskel build --link-object")
oskel_multi_compare("${own}" "${from_ld}"
                    "oskelld and `oskel build --link-object` disagree")

# 3. `oskelc` is the same compiler.
execute_process(
  COMMAND "${OSKELC_EXE}" "${OSKEL_SCRIPT}" --mode hosted --linker own
          --link-object "${OSKEL_HELPER_OBJECT}" -o "${from_compiler}"
  RESULT_VARIABLE compiler_exit
  OUTPUT_VARIABLE compiler_stdout
  ERROR_VARIABLE compiler_stderr)
if(NOT compiler_exit EQUAL 0)
  message(FATAL_ERROR
    "oskelc --linker own --link-object exited ${compiler_exit}\n"
    "--- stdout ---\n${compiler_stdout}\n--- stderr ---\n${compiler_stderr}")
endif()
oskel_multi_compare("${own}" "${from_compiler}"
                    "oskelc and `oskel build` disagree")

# 4. `-r` combines the objects, and the result links again.
execute_process(
  COMMAND "${OSKELD_EXE}" -r "${main_object}" "${OSKEL_HELPER_OBJECT}"
          -o "${combined}"
  RESULT_VARIABLE reloc_exit
  OUTPUT_VARIABLE reloc_stdout
  ERROR_VARIABLE reloc_stderr)
if(NOT reloc_exit EQUAL 0 OR NOT EXISTS "${combined}")
  message(FATAL_ERROR
    "oskelld -r exited ${reloc_exit}\n--- stdout ---\n${reloc_stdout}\n"
    "--- stderr ---\n${reloc_stderr}")
endif()
execute_process(
  COMMAND "${OSKELD_EXE}" "${combined}" -o "${from_r}"
  RESULT_VARIABLE relink_exit
  OUTPUT_VARIABLE relink_stdout
  ERROR_VARIABLE relink_stderr)
if(NOT relink_exit EQUAL 0)
  message(FATAL_ERROR
    "oskelld (relocatable input) exited ${relink_exit}\n"
    "--- stdout ---\n${relink_stdout}\n--- stderr ---\n${relink_stderr}")
endif()
oskel_multi_run("${from_r}" "oskelld -r then link")
oskel_multi_compare("${own}" "${from_r}"
                    "the -r path produced a different image")

# 5. An `ar` archive. GNU `ar` writes the long member name through `//`.
if(DEFINED OSKEL_AR AND NOT OSKEL_AR STREQUAL "")
  execute_process(
    COMMAND "${OSKEL_AR}" rcs "${archive}" "${OSKEL_HELPER_OBJECT}"
    RESULT_VARIABLE ar_exit
    OUTPUT_VARIABLE ar_stdout
    ERROR_VARIABLE ar_stderr)
  if(NOT ar_exit EQUAL 0 OR NOT EXISTS "${archive}")
    message(FATAL_ERROR
      "ar exited ${ar_exit}\n--- stdout ---\n${ar_stdout}\n"
      "--- stderr ---\n${ar_stderr}")
  endif()
  execute_process(
    COMMAND "${OSKELD_EXE}" "${main_object}" "${archive}" -o "${from_ar}"
    RESULT_VARIABLE ar_ld_exit
    OUTPUT_VARIABLE ar_ld_stdout
    ERROR_VARIABLE ar_ld_stderr)
  if(NOT ar_ld_exit EQUAL 0)
    message(FATAL_ERROR
      "oskelld with an archive exited ${ar_ld_exit}\n"
      "--- stdout ---\n${ar_ld_stdout}\n--- stderr ---\n${ar_ld_stderr}")
  endif()
  oskel_multi_run("${from_ar}" "oskelld with an ar archive")
  oskel_multi_compare("${own}" "${from_ar}"
                      "the archive path produced a different image")
endif()

# 6. The host linker also accepts the extra object.
execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode hosted --linker host
          --link-object "${OSKEL_HELPER_OBJECT}" -o "${from_host}"
  RESULT_VARIABLE host_exit
  OUTPUT_VARIABLE host_stdout
  ERROR_VARIABLE host_stderr)
if(NOT host_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel build --linker host --link-object exited ${host_exit}\n"
    "--- stdout ---\n${host_stdout}\n--- stderr ---\n${host_stderr}")
endif()
oskel_multi_run("${from_host}" "oskel build --linker host --link-object")

# 7. The own image is libc-backed.
find_program(OSKEL_LDD NAMES ldd)
if(OSKEL_LDD)
  execute_process(
    COMMAND "${OSKEL_LDD}" "${own}"
    RESULT_VARIABLE ldd_exit
    OUTPUT_VARIABLE ldd_stdout
    ERROR_VARIABLE ldd_stderr)
  set(ldd_text "${ldd_stdout}${ldd_stderr}")
  if(NOT ldd_text MATCHES "libc[.]so[.]6")
    message(FATAL_ERROR "the own linker did not produce a libc-backed image:\n${ldd_text}")
  endif()
endif()

# 8. No inherited environment.
find_program(OSKEL_ENV NAMES env)
if(OSKEL_ENV)
  execute_process(
    COMMAND "${OSKEL_ENV}" -i "${own}"
    RESULT_VARIABLE clean_exit
    OUTPUT_VARIABLE clean_stdout
    ERROR_VARIABLE clean_stderr)
  if(NOT clean_exit EQUAL 0 OR NOT clean_stdout STREQUAL expected)
    message(FATAL_ERROR
      "the image failed under 'env -i' (exit ${clean_exit}):\n${clean_stderr}")
  endif()
endif()

# 9. Usage errors.
execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode hosted --emit-object
          --link-object "${OSKEL_HELPER_OBJECT}" -o "${dir}/bad-emit.o"
  RESULT_VARIABLE bad_emit_exit
  OUTPUT_QUIET ERROR_QUIET)
if(NOT bad_emit_exit EQUAL 64)
  message(FATAL_ERROR
    "'--link-object' with '--emit-object' should be a usage error (exit 64), got ${bad_emit_exit}")
endif()
execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --mode freestanding
          --link-object "${OSKEL_HELPER_OBJECT}" -o "${dir}/bad-mode"
  RESULT_VARIABLE bad_mode_exit
  OUTPUT_QUIET ERROR_QUIET)
if(NOT bad_mode_exit EQUAL 64)
  message(FATAL_ERROR
    "'--link-object' with freestanding should be a usage error (exit 64), got ${bad_mode_exit}")
endif()
