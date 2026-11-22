# Runs `oskel build --linker own` on Windows and executes the resulting PE.
# The fixture is a plain `print`, so the test covers the UCRT imports
# (`_write`, `_setmode`, `malloc`, `exit`) and the generated `_start`.
if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT)
  message(FATAL_ERROR "OSKEL_EXE and OSKEL_SCRIPT are required")
endif()

set(work "${CMAKE_CURRENT_BINARY_DIR}/own_windows")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
set(output "${work}/app.exe")

execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --linker own --mode hosted
          -o "${output}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
    "oskel build --linker own failed (${build_result})\n${build_output}${build_error}")
endif()
if(NOT EXISTS "${output}")
  message(FATAL_ERROR "oskel build did not write '${output}'")
endif()

execute_process(
  COMMAND "${output}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  # The Windows loader reports STATUS_ENTRYPOINT_NOT_FOUND as 0xc0000139 when
  # an imported symbol is missing. Dump the import table and check each UCRT
  # and kernel32 export so the failing name is in the CI log.
  find_program(DUMPBIN_EXECUTABLE dumpbin)
  if(DUMPBIN_EXECUTABLE)
    execute_process(COMMAND "${DUMPBIN_EXECUTABLE}" /imports "${output}"
      OUTPUT_VARIABLE imports_output ERROR_QUIET)
    message(STATUS "own-linker imports:\n${imports_output}")
    execute_process(COMMAND "${DUMPBIN_EXECUTABLE}" /exports
      "C:/Windows/System32/ucrtbase.dll" OUTPUT_VARIABLE ucrt_exports ERROR_QUIET)
    execute_process(COMMAND "${DUMPBIN_EXECUTABLE}" /exports
      "C:/Windows/System32/kernel32.dll" OUTPUT_VARIABLE kernel_exports ERROR_QUIET)
    foreach(symbol _write exit _setmode malloc time _open _read _close _lseeki64)
      string(FIND "${ucrt_exports}" "${symbol}" found)
      message(STATUS "ucrtbase ${symbol}: ${found}")
    endforeach()
    foreach(symbol GetTickCount64 ExitProcess)
      string(FIND "${kernel_exports}" "${symbol}" found)
      message(STATUS "kernel32 ${symbol}: ${found}")
    endforeach()
  else()
    message(STATUS "dumpbin not found on PATH")
  endif()
  execute_process(COMMAND powershell -NoProfile -Command
    "Get-WinEvent -LogName Application -MaxEvents 3 | Format-List TimeCreated,ProviderName,Message"
    OUTPUT_VARIABLE events_output ERROR_QUIET)
  message(STATUS "application events:\n${events_output}")
  message(FATAL_ERROR
    "the own-linker image failed (${run_result})\n${run_output}${run_error}")
endif()

string(STRIP "${run_output}" actual)
if(NOT actual STREQUAL "${OSKEL_EXPECTED_OUTPUT}")
  message(FATAL_ERROR
    "expected '${OSKEL_EXPECTED_OUTPUT}', got '${actual}'")
endif()
