# End-to-end test for the `bindgen` tool.
#
# It runs the real `bindgen` binary on a C header and feeds the generated Oskel
# module back to `oskel -c` (compile only). This is the property that matters:
# whatever `bindgen` emits, the compiler must accept, so the tool and the
# language cannot drift apart. It also pins the CLI contract (stdout by default,
# `-o` for a file, exit 64 on a usage error, exit 66 on an unreadable input).
#
# Registered by CMakeLists.txt. Expects:
#   -DBINDGEN_EXE=<path to the bindgen executable>
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DBINDGEN_HEADER=<the .h fixture>

if(NOT DEFINED BINDGEN_EXE OR NOT DEFINED OSKEL_EXE OR NOT DEFINED BINDGEN_HEADER)
  message(FATAL_ERROR "run_bindgen_case.cmake requires BINDGEN_EXE, OSKEL_EXE and BINDGEN_HEADER")
endif()

set(generated "${CMAKE_CURRENT_BINARY_DIR}/bindgen_generated.oskl")
file(REMOVE "${generated}")

# --- 1. `bindgen <header> -o <out>` writes a module and reports on stderr ---
execute_process(
  COMMAND "${BINDGEN_EXE}" "${BINDGEN_HEADER}" -o "${generated}"
  RESULT_VARIABLE bindgen_exit
  OUTPUT_VARIABLE bindgen_stdout
  ERROR_VARIABLE bindgen_stderr
)
if(NOT bindgen_exit EQUAL 0)
  message(FATAL_ERROR
    "bindgen exited ${bindgen_exit}\n--- stdout ---\n${bindgen_stdout}\n"
    "--- stderr ---\n${bindgen_stderr}")
endif()
if(NOT EXISTS "${generated}")
  message(FATAL_ERROR "bindgen -o did not create '${generated}'")
endif()
if(NOT bindgen_stdout STREQUAL "")
  message(FATAL_ERROR
    "bindgen wrote to stdout despite -o\n--- stdout ---\n${bindgen_stdout}")
endif()
# The fixture has one skipped declaration (the callback) and one commented
# variadic, so the summary must mention both.
if(NOT bindgen_stderr MATCHES "skipped" OR NOT bindgen_stderr MATCHES "variadic")
  message(FATAL_ERROR
    "the bindgen summary does not report the skipped/commented counts:\n"
    "--- stderr ---\n${bindgen_stderr}")
endif()

# --- 2. The generated module is accepted by the compiler ---
execute_process(
  COMMAND "${OSKEL_EXE}" -c "${generated}"
  RESULT_VARIABLE check_exit
  OUTPUT_VARIABLE check_stdout
  ERROR_VARIABLE check_stderr
)
if(NOT check_exit EQUAL 0)
  message(FATAL_ERROR
    "oskel -c rejected the generated module (exit ${check_exit})\n"
    "--- generated ---\n${generated}\n--- stderr ---\n${check_stderr}")
endif()

# --- 3. `bindgen <header>` writes the same module to stdout ---
execute_process(
  COMMAND "${BINDGEN_EXE}" "${BINDGEN_HEADER}"
  RESULT_VARIABLE pipe_exit
  OUTPUT_VARIABLE pipe_stdout
  ERROR_VARIABLE pipe_stderr
)
if(NOT pipe_exit EQUAL 0)
  message(FATAL_ERROR "bindgen (stdout) exited ${pipe_exit}\n--- stderr ---\n${pipe_stderr}")
endif()
file(READ "${generated}" generated_text)
if(NOT pipe_stdout STREQUAL generated_text)
  message(FATAL_ERROR "bindgen stdout disagrees with the -o file")
endif()
# A non-empty generated module: the fixture is not silently empty.
if(NOT generated_text MATCHES "fn strlen")
  message(FATAL_ERROR "the generated module does not declare strlen:\n${generated_text}")
endif()

# --- 4. Usage errors use the toolchain's exit codes ---
execute_process(
  COMMAND "${BINDGEN_EXE}"
  RESULT_VARIABLE no_arg_exit
  ERROR_QUIET
)
if(NOT no_arg_exit EQUAL 64)
  message(FATAL_ERROR "bindgen with no argument exited ${no_arg_exit}, expected 64")
endif()

execute_process(
  COMMAND "${BINDGEN_EXE}" "${CMAKE_CURRENT_BINARY_DIR}/does-not-exist-$$.h"
  RESULT_VARIABLE missing_exit
  ERROR_QUIET
)
if(NOT missing_exit EQUAL 66)
  message(FATAL_ERROR "bindgen on a missing file exited ${missing_exit}, expected 66")
endif()
