# Keeps the extension's diagnostic parser honest.
#
# `editors/vscode/src/diagnostics.js` turns the compiler's stderr into
# diagnostics, and the extension has no other logic: a wrong regex here is a
# wrong squiggle in the editor, on a line the author will read as the compiler's
# word. `test/diagnostics.test.js` drives it with the real stderr captured from
# `oskel -c`, so a change in the compiler's output shape fails the suite instead
# of silently producing no diagnostics.
#
# The parser does not import `vscode`, which is what makes this runnable without
# an editor: the test is plain `node --test`. `node --check` also covers
# `extension.js`, which does import `vscode` and so cannot be loaded here.
#
# Node is not a build dependency of the engine; where it is absent the test
# skips rather than fails, the same way `check_oskelc_no_vm.cmake` skips without
# `nm`.
#
# Usage: cmake -DROOT=<root> -P tests/check_vscode_diagnostics.cmake

if(NOT DEFINED ROOT)
  get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
# `-DROOT=.` is common and would leave every path below relative to the build
# directory, which is not where the extension lives.
get_filename_component(ROOT "${ROOT}" ABSOLUTE)

set(extension_dir "${ROOT}/editors/vscode")
set(test_file "${extension_dir}/test/diagnostics.test.js")
foreach(required "${test_file}" "${extension_dir}/src/diagnostics.js" "${extension_dir}/extension.js")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "check_vscode_diagnostics.cmake: missing ${required}")
  endif()
endforeach()

find_program(OSKEL_NODE NAMES node nodejs)
if(NOT OSKEL_NODE)
  message(STATUS "no node found: skipping the VS Code diagnostic checks")
  return()
endif()

# `extension.js` requires `vscode`, which only exists inside a running editor,
# so it is syntax-checked and not loaded. A parse error still fails the build.
execute_process(
  COMMAND "${OSKEL_NODE}" --check "${extension_dir}/extension.js"
  RESULT_VARIABLE check_exit
  OUTPUT_VARIABLE check_out
  ERROR_VARIABLE check_err)
if(NOT check_exit EQUAL 0)
  message(FATAL_ERROR "editors/vscode/extension.js does not parse:\n${check_out}${check_err}")
endif()

execute_process(
  COMMAND "${OSKEL_NODE}" --test "${test_file}"
  WORKING_DIRECTORY "${extension_dir}"
  RESULT_VARIABLE test_exit
  OUTPUT_VARIABLE test_out
  ERROR_VARIABLE test_err)
if(NOT test_exit EQUAL 0)
  message(FATAL_ERROR
    "the VS Code diagnostic parser is out of step with the compiler:\n${test_out}${test_err}")
endif()

string(REGEX MATCH "pass ([0-9]+)" passed "${test_out}")
message(STATUS "VS Code diagnostics: ${CMAKE_MATCH_1} parser tests passed")
