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
# The fixture declares structs and enum members, and reports the shapes it
# cannot express (a union, an array field, a nested struct, a bitfield, a
# returned callback) plus one commented variadic. The summary has to mention
# every group, or a regression in one of them would go unnoticed.
if(NOT bindgen_stderr MATCHES "skipped" OR NOT bindgen_stderr MATCHES "commented out"
   OR NOT bindgen_stderr MATCHES "struct\\(s\\)" OR NOT bindgen_stderr MATCHES "enum constant\\(s\\)")
  message(FATAL_ERROR
    "the bindgen summary does not report every group (skipped/commented/structs/constants):\n"
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
# Captured with OUTPUT_FILE, not OUTPUT_VARIABLE: CMake normalizes CRLF to LF
# when it captures into a variable, which would hide a text-mode stdout that
# turns every '\n' into "\r\n" on Windows. Comparing the raw bytes is what
# keeps the two output paths identical on every platform.
set(stdout_file "${CMAKE_CURRENT_BINARY_DIR}/bindgen_stdout.oskl")
file(REMOVE "${stdout_file}")
execute_process(
  COMMAND "${BINDGEN_EXE}" "${BINDGEN_HEADER}"
  OUTPUT_FILE "${stdout_file}"
  RESULT_VARIABLE pipe_exit
  ERROR_VARIABLE pipe_stderr
)
if(NOT pipe_exit EQUAL 0)
  message(FATAL_ERROR "bindgen (stdout) exited ${pipe_exit}\n--- stderr ---\n${pipe_stderr}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${generated}" "${stdout_file}"
  RESULT_VARIABLE compare_exit
)
if(NOT compare_exit EQUAL 0)
  message(FATAL_ERROR
    "bindgen stdout differs byte for byte from the -o file (a text-mode stdout "
    "would translate '\\n' to CRLF on Windows)")
endif()
file(READ "${generated}" generated_text)
# A non-empty generated module: the fixture is not silently empty.
if(NOT generated_text MATCHES "fn strlen")
  message(FATAL_ERROR "the generated module does not declare strlen:\n${generated_text}")
endif()
# A pointer to a callback is an opaque handle, never `fn(...)`.
if(NOT generated_text MATCHES "fn atexit\\(pp: ptr\\)")
  message(FATAL_ERROR
    "a pointer to a callback was not emitted as an opaque ptr:\n${generated_text}")
endif()
# A calling convention is dropped, not taken as the parameter's name.
if(NOT generated_text MATCHES "fn reg\\(cb: fn\\(int32\\): int32\\): void;")
  message(FATAL_ERROR
    "a calling convention inside a callback declarator was mishandled:\n${generated_text}")
endif()
if(NOT generated_text MATCHES "fn deliver\\(h: fn\\(int32\\): int32\\): void;")
  message(FATAL_ERROR
    "a callback typedef carrying a convention was not a callback:\n${generated_text}")
endif()
if(generated_text MATCHES "WINAPI: " OR generated_text MATCHES "CALLBACK: ")
  message(FATAL_ERROR
    "a calling convention leaked into the generated parameter names:\n${generated_text}")
endif()
# The structs reach the module before the `extern "C"` block, and a signature
# spells one by value.
if(NOT generated_text MATCHES "struct Point \\{")
  message(FATAL_ERROR "the generated module does not declare struct Point:\n${generated_text}")
endif()
# A struct by value is emitted on a build whose marshalling implements the
# System V AMD64 ABI, and written commented out where the ABI cannot carry it
# (the Microsoft x64 ABI passes a struct through a hidden pointer). Either way
# the declaration is there with its struct name, and step 2 above proves the
# module compiles on this platform -- which is the property that matters.
if(NOT generated_text MATCHES "(// )?fn make_point\\(x: int32, y: int32\\): Point;")
  message(FATAL_ERROR
    "a struct-by-value return was not emitted with its struct name:\n${generated_text}")
endif()
# An anonymous body is named by its typedef, and the signature uses that name.
if(NOT generated_text MATCHES "struct div_t \\{")
  message(FATAL_ERROR
    "an anonymous struct was not named by its typedef:\n${generated_text}")
endif()
if(NOT generated_text MATCHES "(// )?fn div\\(num: int32, denom: int32\\): div_t;")
  message(FATAL_ERROR
    "the typedef-named struct is not spelled in the signature:\n${generated_text}")
endif()
# An enum is an `int` at the ABI and its members are module constants.
if(NOT generated_text MATCHES "let GREEN = 5;")
  message(FATAL_ERROR "the enum members are not module constants:\n${generated_text}")
endif()
if(NOT generated_text MATCHES "fn pick_color\\(seed: int32\\): int32;")
  message(FATAL_ERROR "an enum-typed declaration is not int32:\n${generated_text}")
endif()
# A body the Oskel `struct` cannot lay out is reported, never declared with a
# wrong layout: the declaration, not the `// skipped:` comment that names it.
if(generated_text MATCHES "\nstruct Value \\{" OR generated_text MATCHES "\nstruct Named \\{"
   OR generated_text MATCHES "\nstruct Nested \\{" OR generated_text MATCHES "\nstruct Bits \\{")
  message(FATAL_ERROR
    "the generated module declares a struct whose layout cannot be expressed:\n${generated_text}")
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
