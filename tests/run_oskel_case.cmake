# Runs one Oskel script under CTest and checks its exit code.
# Registered by CMakeLists.txt; expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<path to the .oskl file>
#   -DEXPECTED_EXIT=<exit code declared by the script, 0 by default>
# Optional:
#   -DOSKEL_ARGS=<extra arguments passed to the script, shell-quoted>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED EXPECTED_EXIT)
  message(FATAL_ERROR "run_oskel_case.cmake requires OSKEL_EXE, OSKEL_SCRIPT and EXPECTED_EXIT")
endif()

if(DEFINED OSKEL_ARGS)
  separate_arguments(extra_args UNIX_COMMAND "${OSKEL_ARGS}")
else()
  set(extra_args "")
endif()

execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}" ${extra_args}
  # A test must never inherit the developer's terminal: a script that reads
  # stdin (or that switches on `isatty`) would otherwise block or take a
  # different path under an interactive `ctest`. An empty stdin makes every
  # script test deterministic.
  INPUT_FILE "/dev/null"
  RESULT_VARIABLE actual_exit
  OUTPUT_VARIABLE script_stdout
  ERROR_VARIABLE script_stderr
)

if(NOT actual_exit EQUAL EXPECTED_EXIT)
  message(FATAL_ERROR
    "${OSKEL_SCRIPT}: expected exit ${EXPECTED_EXIT}, got ${actual_exit}\n"
    "--- stdout ---\n${script_stdout}\n"
    "--- stderr ---\n${script_stderr}")
endif()
