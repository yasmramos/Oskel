# End-to-end test for the WebAssembly backend (PHASE 9).
#
# Compiles a .oskl program to a `.wasm` module with
# `oskel build --target wasm32-freestanding-wasm`, runs it under Node with the
# two host imports the module declares (`oskel.write` and `oskel.exit`), and
# compares its output with the development VM's, byte for byte.
#
# Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>
#   -DOSKEL_TEST_NAME=<the CTest name, used for unique output paths>
#   -DNODE_EXE=<path to node>
#   -DWASM_RUNNER=<path to tests/run_wasm.cjs>
#   -DWORK_DIR=<directory for the temporary module and outputs>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED OSKEL_TEST_NAME
   OR NOT DEFINED NODE_EXE OR NOT DEFINED WASM_RUNNER OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR
    "wasm_case.cmake requires OSKEL_EXE, OSKEL_SCRIPT, OSKEL_TEST_NAME, NODE_EXE, WASM_RUNNER and WORK_DIR")
endif()

set(module "${WORK_DIR}/${OSKEL_TEST_NAME}.wasm")
set(vm_stdout "${WORK_DIR}/${OSKEL_TEST_NAME}.vm.out")
set(wasm_stdout "${WORK_DIR}/${OSKEL_TEST_NAME}.wasm.out")
set(wasm_stderr "${WORK_DIR}/${OSKEL_TEST_NAME}.wasm.err")
file(REMOVE "${module}" "${vm_stdout}" "${wasm_stdout}" "${wasm_stderr}")

execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}"
  RESULT_VARIABLE vm_exit
  OUTPUT_FILE "${vm_stdout}"
  TIMEOUT 60
)
if(NOT vm_exit EQUAL 0)
  message(FATAL_ERROR "the VM failed to run the fixture (exit ${vm_exit})")
endif()

execute_process(
  COMMAND "${OSKEL_EXE}" build --target wasm32-freestanding-wasm "${OSKEL_SCRIPT}" -o "${module}"
  RESULT_VARIABLE build_exit
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_exit EQUAL 0)
  message(FATAL_ERROR
    "the WebAssembly build exited ${build_exit}\n--- stdout ---\n${build_stdout}\n"
    "--- stderr ---\n${build_stderr}")
endif()
if(NOT EXISTS "${module}")
  message(FATAL_ERROR "oskel build did not create '${module}'")
endif()

execute_process(
  COMMAND "${NODE_EXE}" "${WASM_RUNNER}" "${module}"
  RESULT_VARIABLE wasm_exit
  OUTPUT_FILE "${wasm_stdout}"
  ERROR_FILE "${wasm_stderr}"
  TIMEOUT 60
)
file(READ "${vm_stdout}" vm_text)
file(READ "${wasm_stdout}" wasm_text)
file(READ "${wasm_stderr}" wasm_error_text)
if(NOT wasm_exit EQUAL 0)
  message(FATAL_ERROR
    "the WebAssembly module exited ${wasm_exit}\n--- stdout ---\n${wasm_text}\n"
    "--- stderr ---\n${wasm_error_text}")
endif()
if(NOT wasm_text STREQUAL vm_text)
  message(FATAL_ERROR
    "the WebAssembly module disagrees with the VM\n"
    "--- vm ---\n${vm_text}\n--- wasm ---\n${wasm_text}\n"
    "--- wasm stderr ---\n${wasm_error_text}")
endif()
