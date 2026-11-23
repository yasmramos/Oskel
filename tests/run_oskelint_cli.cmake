# End-to-end test for the `oskelint` command-line tool.
#
# Runs the binary on a real file and checks the whole contract: the exit code
# (0, a lint warning is not an error), the formatted diagnostic (file:line:col,
# the message and the rule in brackets) and the rule name. The unit test
# (`oskelint_test`) pins the rule's behaviour; this one pins the CLI around it.
#
# Expects:
#   -DOSKELINT_EXE=<path to the oskelint executable>
#   -DOSKELINT_SCRIPT=<the .oskl file to lint>

if(NOT DEFINED OSKELINT_EXE OR NOT DEFINED OSKELINT_SCRIPT)
  message(FATAL_ERROR "run_oskelint_cli.cmake requires OSKELINT_EXE and OSKELINT_SCRIPT")
endif()

execute_process(
  COMMAND "${OSKELINT_EXE}" "${OSKELINT_SCRIPT}"
  RESULT_VARIABLE lint_exit
  OUTPUT_VARIABLE lint_stdout
  ERROR_VARIABLE lint_stderr
)

# A lint warning must not fail the build: the tool exits 0 so it can sit in a
# CI step without turning every advisory into a red job.
if(NOT lint_exit EQUAL 0)
  message(FATAL_ERROR
    "oskelint exited ${lint_exit}, expected 0\n"
    "--- stdout ---\n${lint_stdout}\n--- stderr ---\n${lint_stderr}")
endif()

# The fixture is the AOT shadow fixture: it declares `var len = 3;` and
# `var len = (x) => 42;`, so the tool must report the shadow twice, in source
# order. Reusing it keeps one definition of "a shadow" for both the compiler
# tests and the linter test.
set(expected_count 2)
string(REGEX MATCHALL "\\[builtin-shadow\\]" found "${lint_stdout}")
list(LENGTH found count)
if(NOT count EQUAL expected_count)
  message(FATAL_ERROR
    "oskelint reported ${count} builtin-shadow diagnostic(s), expected ${expected_count}\n"
    "--- stdout ---\n${lint_stdout}")
endif()

# The formatted line is `path:line:col: warning: 'len' shadows the builtin 'len'
# [builtin-shadow]`. The path is whatever the caller passed, so it is not
# matched; the shape is.
if(NOT lint_stdout MATCHES ":[0-9]+:[0-9]+: warning: 'len' shadows the builtin 'len' \\[builtin-shadow\\]")
  message(FATAL_ERROR
    "oskelint did not print the expected diagnostic shape\n"
    "--- stdout ---\n${lint_stdout}")
endif()

# The same file, through the re-export idiom, must be silent: `std.sys` exports
# its builtins this way, so a false positive here would report the standard
# library against itself.
# Written beside the script's caller, never in the source tree.
set(reexport_file "${CMAKE_CURRENT_BINARY_DIR}/oskelint_cli_reexport.oskl")
file(WRITE "${reexport_file}" "let time = time;\nprint(time);\n")
execute_process(
  COMMAND "${OSKELINT_EXE}" "${reexport_file}"
  RESULT_VARIABLE reexport_exit
  OUTPUT_VARIABLE reexport_stdout
  ERROR_VARIABLE reexport_stderr
)
file(REMOVE "${reexport_file}")
if(NOT reexport_exit EQUAL 0)
  message(FATAL_ERROR "oskelint exited ${reexport_exit} on the re-export idiom, expected 0")
endif()
if(NOT reexport_stdout STREQUAL "")
  message(FATAL_ERROR
    "oskelint reported the re-export idiom, which is not a shadow\n"
    "--- stdout ---\n${reexport_stdout}")
endif()
