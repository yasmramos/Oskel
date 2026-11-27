# End-to-end test for the `examples/ledger` project.
#
# Copies the project to a scratch directory so the source tree stays clean,
# then drives `oskelp` through check, the demo, the project's own tests, the
# file-backed commands and, where the AOT backend can run, the native binary.
# The VM and native trial balances must be byte-identical.
#
# Registered by CMakeLists.txt. Expects:
#   -DOSKELP_EXE=<path to the oskelp executable>
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKELC_EXE=<path to the oskelc executable>
#   -DOSKEL_AOT_RUNNABLE=<ON|OFF>
#   -DEXAMPLE_DIR=<path to examples/ledger>
#   -DWORK_DIR=<scratch directory>

if(NOT DEFINED OSKELP_EXE OR NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKELC_EXE
   OR NOT DEFINED OSKEL_AOT_RUNNABLE OR NOT DEFINED EXAMPLE_DIR
   OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR
    "run_example_ledger.cmake requires OSKELP_EXE, OSKEL_EXE, OSKELC_EXE, "
    "OSKEL_AOT_RUNNABLE, EXAMPLE_DIR and WORK_DIR")
endif()

if(NOT EXISTS "${OSKELP_EXE}")
  message(FATAL_ERROR "oskelp executable not found: ${OSKELP_EXE}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(COPY "${EXAMPLE_DIR}" DESTINATION "${WORK_DIR}"
  PATTERN "target" EXCLUDE
  PATTERN ".oskel" EXCLUDE)
set(project "${WORK_DIR}/ledger")

# A previous run may have left a ledger or an export behind.
file(GLOB stale "${project}/data/*.json" "${project}/data/*.csv")
if(stale)
  file(REMOVE ${stale})
endif()

set(ENV{OSKEL} "${OSKEL_EXE}")
set(ENV{OSKELC} "${OSKELC_EXE}")

set(exe_suffix "")
if(WIN32)
  set(exe_suffix ".exe")
endif()

macro(run_oskelp)
  execute_process(
    COMMAND "${OSKELP_EXE}" ${ARGN}
    WORKING_DIRECTORY "${project}"
    RESULT_VARIABLE run_exit
    OUTPUT_VARIABLE run_out
    ERROR_VARIABLE run_err)
endmacro()

macro(run_native)
  execute_process(
    COMMAND "${project}/target/ledger${exe_suffix}" ${ARGN}
    WORKING_DIRECTORY "${project}"
    RESULT_VARIABLE run_exit
    OUTPUT_VARIABLE run_out
    ERROR_VARIABLE run_err)
endmacro()

macro(require_success label)
  if(NOT run_exit EQUAL 0)
    message(FATAL_ERROR
      "${label} failed (exit ${run_exit})\n"
      "--- stdout ---\n${run_out}\n--- stderr ---\n${run_err}")
  endif()
endmacro()

macro(require_failure label expected)
  if(run_exit EQUAL 0)
    message(FATAL_ERROR
      "${label} unexpectedly succeeded\n"
      "--- stdout ---\n${run_out}\n--- stderr ---\n${run_err}")
  endif()
  if(NOT "${expected}" STREQUAL "" AND NOT run_exit EQUAL "${expected}")
    message(FATAL_ERROR
      "${label} exited ${run_exit}, expected ${expected}\n"
      "--- stderr ---\n${run_err}")
  endif()
endmacro()

# A `function`, not a `macro`: a Windows path in the output (`D:\a\...`) reaches
# `string(FIND)` as an escape sequence when substituted textually.
function(require_contains label haystack needle)
  string(FIND "${haystack}" "${needle}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "${label}: expected '${needle}' in:\n${haystack}")
  endif()
endfunction()

function(require_same label left right)
  # A Windows VM and the binary runtime can differ only in CRLF translation.
  string(REPLACE "\r\n" "\n" left "${left}")
  string(REPLACE "\r\n" "\n" right "${right}")
  if(NOT "${left}" STREQUAL "${right}")
    message(FATAL_ERROR
      "${label}: the outputs differ\n"
      "--- left ---\n${left}\n--- right ---\n${right}")
  endif()
endfunction()

# --- 1. the project compiles ------------------------------------------------
run_oskelp(check)
require_success("oskelp check")
require_contains("oskelp check" "${run_out}" "checked src/ledger/main.oskl")

# --- 2. the demo is the default command ------------------------------------
run_oskelp(run)
require_success("oskelp run")
require_contains("demo" "${run_out}" "Trial balance")
require_contains("demo" "${run_out}" "Net income")
require_contains("demo" "${run_out}" "5,650.00")
require_contains("demo" "${run_out}" "Balanced: yes")
set(vm_demo "${run_out}")

# --- 3. the project's own test suite ---------------------------------------
run_oskelp(test)
require_success("oskelp test")
require_contains("tests" "${run_out}" "test result: ok. 5 passed")

# --- 4. the file-backed workflow on the VM ---------------------------------
run_oskelp(run -- --data data/vm.json init)
require_success("init")
require_contains("init" "${run_out}" "initialized data/vm.json")

run_oskelp(run -- --data data/vm.json post 2026-02-01 Consulting 1100:1000: 4000::1000)
require_success("post")
require_contains("post" "${run_out}" "posted")

run_oskelp(run -- --data data/vm.json trial)
require_success("trial")
require_contains("trial" "${run_out}" "1,000.00")
require_contains("trial" "${run_out}" "Balanced: yes")
set(vm_trial "${run_out}")

run_oskelp(run -- --data data/vm.json export data/export.csv)
require_success("export")
if(NOT EXISTS "${project}/data/export.csv")
  message(FATAL_ERROR "export did not write data/export.csv")
endif()

run_oskelp(run -- --data data/imported.json init)
require_success("import init")
run_oskelp(run -- --data data/imported.json import data/export.csv)
require_success("import")
require_contains("import" "${run_out}" "imported 1 entries")
run_oskelp(run -- --data data/imported.json trial)
require_success("import trial")
require_same("VM import round trip" "${vm_trial}" "${run_out}")

run_oskelp(run -- --data data/vm.json account add 1300 "Prepaid Rent" asset)
require_success("account add")
require_contains("account add" "${run_out}" "added account 1300 Prepaid Rent (asset)")
run_oskelp(run -- --data data/vm.json balance 1300)
require_success("balance")
require_contains("balance" "${run_out}" "0.00")

run_oskelp(run -- --data data/vm.json post 2026-02-02 Unbalanced 1000:10: 4000::5)
require_failure("unbalanced post" 1)
require_contains("unbalanced post" "${run_err}" "does not balance")

run_oskelp(run -- --data data/vm.json post 2026-02-02 Bad 1000:1.234: 4000::1.234)
require_failure("bad amount" 2)
require_contains("bad amount" "${run_err}" "money:")

run_oskelp(run -- nope)
require_failure("unknown command" 2)
require_contains("unknown command" "${run_err}" "unknown command 'nope'")

# --- 5. the native binary agrees with the VM -------------------------------
# The freestanding Windows image has no process argument vector, so a program
# that reads `args()` cannot be built there; the VM is the Windows path for
# this project, and a hosted build needs a C toolchain the CTest does not set
# up. Linux and macOS run the freestanding/hosted native half.
if(OSKEL_AOT_RUNNABLE AND NOT WIN32)
  run_oskelp(build)
  require_success("oskelp build")
  set(native "${project}/target/ledger${exe_suffix}")
  if(NOT EXISTS "${native}")
    message(FATAL_ERROR "oskelp build did not write target/ledger${exe_suffix}")
  endif()

  run_native()
  require_success("native demo")
  require_same("native demo parity" "${vm_demo}" "${run_out}")

  run_native(--data data/native.json init)
  require_success("native init")
  run_native(--data data/native.json post 2026-02-01 Consulting 1100:1000: 4000::1000)
  require_success("native post")
  run_native(--data data/native.json trial)
  require_success("native trial")
  require_same("native trial parity" "${vm_trial}" "${run_out}")

  run_native(--data data/native.json export data/native-export.csv)
  require_success("native export")
  run_native(--data data/native-imported.json init)
  require_success("native import init")
  run_native(--data data/native-imported.json import data/native-export.csv)
  require_success("native import")
  run_native(--data data/native-imported.json trial)
  require_success("native import trial")
  require_same("native import round trip" "${vm_trial}" "${run_out}")
endif()

message(STATUS "example ledger: ok")
