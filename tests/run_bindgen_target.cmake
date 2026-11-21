# End-to-end test for `bindgen --target`.
#
# `long` and `wchar_t` follow the C data model: LP64 has a 64-bit `long` and a
# 32-bit `wchar_t`, LLP64 the other way round. The generated module is fed back
# to `oskel -c`, so the target-dependent names are ones the compiler accepts.
#
# Expects:
#   -DBINDGEN_EXE=<path to the bindgen executable>
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DBINDGEN_HEADER=<the .h fixture>

if(NOT DEFINED BINDGEN_EXE OR NOT DEFINED OSKEL_EXE OR NOT DEFINED BINDGEN_HEADER)
  message(FATAL_ERROR "run_bindgen_target.cmake requires BINDGEN_EXE, OSKEL_EXE and BINDGEN_HEADER")
endif()

set(generated "${CMAKE_CURRENT_BINARY_DIR}/bindgen_target.oskl")

function(run_bindgen target)
  execute_process(
    COMMAND "${BINDGEN_EXE}" --target "${target}" "${BINDGEN_HEADER}" -o "${generated}"
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE stdout_text
    ERROR_VARIABLE stderr_text)
  if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR
      "bindgen --target ${target} exited ${exit_code}\n--- stdout ---\n${stdout_text}\n"
      "--- stderr ---\n${stderr_text}")
  endif()
  if(NOT stdout_text STREQUAL "")
    message(FATAL_ERROR "bindgen --target ${target} wrote to stdout despite -o")
  endif()
  file(READ "${generated}" text)
  set(generated_text "${text}" PARENT_SCOPE)
endfunction()

function(expect_lines text)
  foreach(expected IN LISTS ARGN)
    string(FIND "${text}" "${expected}" found)
    if(found EQUAL -1)
      message(FATAL_ERROR
        "the generated module is missing '${expected}'\n--- generated ---\n${text}")
    endif()
  endforeach()
endfunction()

run_bindgen(llp64)
set(text "${generated_text}")
expect_lines("${text}" "fn wide(): int32;" "fn uwide(): uint32;" "fn wider(): int64;"
             "fn uwider(): uint64;" "fn wide_char(): uint16;")

run_bindgen(lp64)
set(text "${generated_text}")
expect_lines("${text}" "fn wide(): int64;" "fn uwide(): uint64;" "fn wider(): int64;"
             "fn uwider(): uint64;" "fn wide_char(): int32;")

# An unknown target is a usage error, not a silent LP64 fallback.
execute_process(
  COMMAND "${BINDGEN_EXE}" --target bogus "${BINDGEN_HEADER}"
  RESULT_VARIABLE bad_exit
  OUTPUT_VARIABLE bad_stdout
  ERROR_VARIABLE bad_stderr)
if(NOT bad_exit EQUAL 64)
  message(FATAL_ERROR "an unknown --target exited ${bad_exit}, expected 64")
endif()

# The generated module compiles.
execute_process(
  COMMAND "${OSKEL_EXE}" -c "${generated}"
  RESULT_VARIABLE check_exit
  OUTPUT_VARIABLE check_stdout
  ERROR_VARIABLE check_stderr)
if(NOT check_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel -c rejected the generated module (exit ${check_exit})\n"
    "--- generated ---\n${generated}\n--- stderr ---\n${check_stderr}")
endif()
