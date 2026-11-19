# Keeps the VS Code snippets compiling.
#
# A snippet that expands to invalid Oskel is worse than no snippet: it looks
# authoritative and then fails to compile. Each body is expanded the way VS Code
# expands it (a placeholder becomes its default text, `$0` becomes empty) and
# the result is checked with the real compiler, wrapped in the context the
# construct needs (a method needs a class, `defer` needs a function body, ...).
#
# This is the half `check_vscode_grammar.cmake` cannot do: that test proves the
# grammar does not *highlight* a word the language lacks, this one proves the
# snippets do not *emit* one.
#
# `string(JSON)` needs CMake 3.19 and the project supports 3.15, so the file is
# parsed by hand. A `;` is CMake's list separator and every snippet body has
# one, so semicolons are swapped for a placeholder before any list operation and
# restored afterwards.
#
# Usage: cmake -DROOT=<root> -DOSKEL=<built oskel> -P tests/check_vscode_snippets.cmake

if(NOT DEFINED ROOT)
  get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
if(NOT DEFINED OSKEL)
  message(FATAL_ERROR "check_vscode_snippets.cmake: OSKEL (path to the built binary) is required")
endif()

set(snippets_path "${ROOT}/editors/vscode/snippets/oskel.json")
foreach(required "${snippets_path}" "${OSKEL}")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "check_vscode_snippets.cmake: missing ${required}")
  endif()
endforeach()

file(READ "${snippets_path}" snippets)
string(REPLACE ";" "@OSKEL_SEMI@" snippets "${snippets}")

string(REGEX MATCHALL "\"[^\"]+\"[ \t\r\n]*:[ \t\r\n]*\\{" snippet_titles "${snippets}")
list(LENGTH snippet_titles snippet_count)
if(snippet_count LESS 10)
  message(FATAL_ERROR
    "check_vscode_snippets.cmake: found only ${snippet_count} snippets in "
    "${snippets_path}; the file moved or its shape changed")
endif()

# Undo the JSON escapes of one line, then the snippet escape `\$` (a literal
# dollar, which is how a snippet writes `${` without interpolating).
macro(unescape_line var)
  string(REPLACE "\\n" "\n" ${var} "${${var}}")
  string(REPLACE "\\t" "\t" ${var} "${${var}}")
  string(REPLACE "\\\"" "\"" ${var} "${${var}}")
  string(REPLACE "\\\\" "\\" ${var} "${${var}}")
  string(REPLACE "\\$" "$" ${var} "${${var}}")
  string(REPLACE "@OSKEL_SEMI@" ";" ${var} "${${var}}")
endmacro()

# Expand a body the way VS Code does: `${N:default}` -> `default`, then `${N}`
# and `$0` -> empty. The loop repeats because a default may hold a placeholder.
macro(expand_body var)
  while(${var} MATCHES "\\$\\{[0-9]+:([^{}]*)\\}")
    string(REGEX REPLACE "\\$\\{[0-9]+:([^{}]*)\\}" "\\1" ${var} "${${var}}")
  endwhile()
  string(REGEX REPLACE "\\$\\{[0-9]+\\}" "" ${var} "${${var}}")
  string(REPLACE "$0" "" ${var} "${${var}}")
endmacro()

# The context each construct needs to be a whole program. A snippet not listed
# here is expected to be a complete top-level declaration.
set(context_Method "class C {\n%s\n}")
set(context_Error_and_catch "fn f(): int { return %s; }")
set(context_Try_propagation "fn f(): int { %s return 0; }")
set(context_Defer "fn f() { %s }")
set(context_Print_interpolation "fn f() { let value = 1; %s }")
set(context_Assert_equal "fn f() { let actual = 1; let expected = 1; %s }")
set(context_Lambda "fn f() { let g = %s; }")
set(context_Switch "fn f() { let subject = 0; %s }")
set(context_Import_module "fn f() {}")
set(context_Import_wildcard "fn f() {}")
set(context_Import_selective "fn f() {}")
set(context_Extern_C_block "fn f() {}")
set(context_Native_function "fn f() {}")

set(problems "")
set(checked 0)

set(index 0)
while(index LESS snippet_count)
  list(GET snippet_titles ${index} title_raw)
  string(REGEX REPLACE "[{}\":]" "" title "${title_raw}")
  string(STRIP "${title}" title)

  # The text of this snippet runs to the next title, or to the end of the file.
  math(EXPR next_index "${index} + 1")
  string(FIND "${snippets}" "${title_raw}" this_at)
  if(next_index LESS snippet_count)
    list(GET snippet_titles ${next_index} next_raw)
    string(FIND "${snippets}" "${next_raw}" next_at)
  else()
    string(LENGTH "${snippets}" next_at)
  endif()
  math(EXPR index "${index} + 1")
  math(EXPR body_len "${next_at} - ${this_at}")
  string(SUBSTRING "${snippets}" ${this_at} ${body_len} snippet_text)

  if(NOT snippet_text MATCHES "\"body\"[ \t\r\n]*:[ \t\r\n]*\\[([^]]*)\\]")
    list(APPEND problems "  '${title}': no \"body\" array")
    continue()
  endif()

  # Join the quoted lines of the body into one string. `string(APPEND)` keeps a
  # `;` intact where `list(APPEND)` would split on it.
  string(REGEX MATCHALL "\"([^\"\\\\]|\\\\.)*\"" body_literals "${CMAKE_MATCH_1}")
  if(NOT body_literals)
    list(APPEND problems "  '${title}': empty body")
    continue()
  endif()
  set(expanded "")
  foreach(literal IN LISTS body_literals)
    string(REGEX REPLACE "^\"(.*)\"$" "\\1" line "${literal}")
    unescape_line(line)
    string(APPEND expanded "${line}\n")
  endforeach()
  expand_body(expanded)

  # Wrap it in the context it needs. The key comes from the title with the
  # spaces and slashes turned into underscores (`If / else` -> `If___else`).
  string(REPLACE " " "_" ctx_key "${title}")
  string(REPLACE "/" "_" ctx_key "${ctx_key}")
  set(wrapper "%s")
  if(DEFINED context_${ctx_key})
    set(wrapper "${context_${ctx_key}}")
  endif()
  string(REPLACE "%s" "${expanded}" source "${wrapper}")

  set(scratch "${CMAKE_CURRENT_BINARY_DIR}/vscode_snippet_${index}.oskl")
  file(WRITE "${scratch}" "${source}\n")
  execute_process(
    COMMAND "${OSKEL}" -c "${scratch}"
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
  file(REMOVE "${scratch}")

  if(NOT exit_code EQUAL 0)
    string(STRIP "${out}${err}" message)
    list(APPEND problems "  '${title}' expands to code the compiler rejects:\n${message}")
  else()
    math(EXPR checked "${checked} + 1")
  endif()
endwhile()

if(problems)
  string(REPLACE ";" "\n" problems "${problems}")
  message(FATAL_ERROR
    "the VS Code snippets are out of step with the compiler:\n${problems}")
endif()

message(STATUS "VS Code snippets compile: ${checked} of ${snippet_count}")
