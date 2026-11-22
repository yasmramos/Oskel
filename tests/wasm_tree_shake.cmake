# Size and parity test for the WebAssembly runtime tree-shaking (PHASE 9.4).
#
# A program that never reads a bound method must not carry the method helpers
# or the Unicode case tables. The same program plus one static method read
# (`"abc".upper`) must still run and must be larger, which proves the analysis
# kept the helper it needs instead of disabling the whole runtime.
#
# Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DNODE_EXE=<path to node>
#   -DWASM_RUNNER=<path to tests/run_wasm.cjs>
#   -DWORK_DIR=<directory for the temporary modules and outputs>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED NODE_EXE OR NOT DEFINED WASM_RUNNER
   OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR
    "wasm_tree_shake.cmake requires OSKEL_EXE, NODE_EXE, WASM_RUNNER and WORK_DIR")
endif()

set(min_script "${WORK_DIR}/wasm_tree_shake_min.oskl")
set(method_script "${WORK_DIR}/wasm_tree_shake_method.oskl")
file(WRITE "${min_script}"
  "var total = 0;\n"
  "for (var i = 0; i < 10; i = i + 1) { total = total + i; }\n"
  "print(total);\n")
file(WRITE "${method_script}"
  "var total = 0;\n"
  "for (var i = 0; i < 10; i = i + 1) { total = total + i; }\n"
  "print(total);\n"
  "var f = \"abc\".upper;\n"
  "print(type(f));\n")

function(run_case name script)
  set(module "${WORK_DIR}/wasm_tree_shake_${name}.wasm")
  set(vm_out "${WORK_DIR}/wasm_tree_shake_${name}.vm.out")
  set(wasm_out "${WORK_DIR}/wasm_tree_shake_${name}.wasm.out")
  set(wasm_err "${WORK_DIR}/wasm_tree_shake_${name}.wasm.err")
  file(REMOVE "${module}" "${vm_out}" "${wasm_out}" "${wasm_err}")

  execute_process(
    COMMAND "${OSKEL_EXE}" "${script}"
    RESULT_VARIABLE vm_exit
    OUTPUT_FILE "${vm_out}"
    TIMEOUT 60)
  if(NOT vm_exit EQUAL 0)
    message(FATAL_ERROR "the VM failed to run ${name} (exit ${vm_exit})")
  endif()

  execute_process(
    COMMAND "${OSKEL_EXE}" build --target wasm32-freestanding-wasm "${script}" -o "${module}"
    RESULT_VARIABLE build_exit
    OUTPUT_VARIABLE build_out
    ERROR_VARIABLE build_err)
  if(NOT build_exit EQUAL 0)
    message(FATAL_ERROR
      "the WebAssembly build of ${name} exited ${build_exit}\n--- stdout ---\n${build_out}\n"
      "--- stderr ---\n${build_err}")
  endif()

  execute_process(
    COMMAND "${NODE_EXE}" "${WASM_RUNNER}" "${module}"
    RESULT_VARIABLE wasm_exit
    OUTPUT_FILE "${wasm_out}"
    ERROR_FILE "${wasm_err}"
    TIMEOUT 60)
  file(READ "${vm_out}" vm_text)
  file(READ "${wasm_out}" wasm_text)
  file(READ "${wasm_err}" wasm_error_text)
  if(NOT wasm_exit EQUAL 0)
    message(FATAL_ERROR
      "the WebAssembly module of ${name} exited ${wasm_exit}\n--- stdout ---\n${wasm_text}\n"
      "--- stderr ---\n${wasm_error_text}")
  endif()
  if(NOT wasm_text STREQUAL vm_text)
    message(FATAL_ERROR
      "the WebAssembly module of ${name} disagrees with the VM\n"
      "--- vm ---\n${vm_text}\n--- wasm ---\n${wasm_text}")
  endif()

  file(SIZE "${module}" size)
  set(${name}_size "${size}" PARENT_SCOPE)
endfunction()

run_case(min "${min_script}")
run_case(method "${method_script}")

# The full runtime is ~60 KB; the tree-shaken minimum must stay well below it.
# The upper helper plus the upper case table is ~8 KB, so the method program
# must be larger than the minimum.
if(min_size GREATER 45000)
  message(FATAL_ERROR
    "the no-method module is ${min_size} bytes, above the 45000-byte tree-shake budget")
endif()
if(NOT min_size LESS method_size)
  message(FATAL_ERROR
    "the no-method module (${min_size} bytes) is not smaller than the method module "
    "(${method_size} bytes); the method helpers were not tree-shaken")
endif()
