# End-to-end test for `oskelp`, the project and package manager.
#
# It drives the real binary through the workflow a user follows: create a
# project, check it, build it, run the native binary and the VM, run the tests,
# add a path dependency, exercise `--locked`, a dependency cycle, a failing
# test, `clean` and `init`. Every step runs in a scratch directory, so the
# repository is never touched.
#
# Registered by CMakeLists.txt. Expects:
#   -DOSKELP_EXE=<path to the oskelp executable>
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKELC_EXE=<path to the oskelc executable>
#   -DWORK_DIR=<scratch directory>

if(NOT DEFINED OSKELP_EXE OR NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKELC_EXE
   OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR
    "run_oskelp_case.cmake requires OSKELP_EXE, OSKEL_EXE, OSKELC_EXE and WORK_DIR")
endif()

if(NOT EXISTS "${OSKELP_EXE}")
  message(FATAL_ERROR "oskelp executable not found: ${OSKELP_EXE}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(exe_suffix "")
if(WIN32)
  set(exe_suffix ".exe")
endif()

macro(run_oskelp working_dir)
  execute_process(
    COMMAND "${OSKELP_EXE}" ${ARGN}
    WORKING_DIRECTORY "${working_dir}"
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

macro(require_contains label haystack needle)
  string(FIND "${haystack}" "${needle}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR
      "${label}: expected '${needle}' in:\n${haystack}")
  endif()
endmacro()

# --- 1. `new` creates the layout -------------------------------------------
run_oskelp("${WORK_DIR}" new demo)
require_success("oskelp new demo")

run_oskelp("${WORK_DIR}" new 123bad)
require_failure("oskelp new rejects a digit-leading name" "1")
require_contains("oskelp new rejects a digit-leading name" "${run_err}"
                 "not a valid package name")
foreach(file oskel.toml .gitignore src/demo/main.oskl src/demo/greeting.oskl tests/smoke.oskl)
  if(NOT EXISTS "${WORK_DIR}/demo/${file}")
    message(FATAL_ERROR "oskelp new did not create '${file}'")
  endif()
endforeach()

# --- 2. `check` compiles without running -----------------------------------
run_oskelp("${WORK_DIR}/demo" check)
require_success("oskelp check")
require_contains("oskelp check" "${run_out}" "checked src/demo/main.oskl")

# --- 3. `build` writes target/<name> ---------------------------------------
run_oskelp("${WORK_DIR}/demo" build)
require_success("oskelp build")
if(NOT EXISTS "${WORK_DIR}/demo/target/demo${exe_suffix}")
  message(FATAL_ERROR "oskelp build did not write target/demo${exe_suffix}")
endif()
if(NOT EXISTS "${WORK_DIR}/demo/oskel.lock")
  message(FATAL_ERROR "oskelp build did not write oskel.lock")
endif()

# --- 4. The native binary and the VM print the same greeting ---------------
execute_process(
  COMMAND "${WORK_DIR}/demo/target/demo${exe_suffix}"
  WORKING_DIRECTORY "${WORK_DIR}/demo"
  RESULT_VARIABLE native_exit
  OUTPUT_VARIABLE native_out
  ERROR_VARIABLE native_err)
if(NOT native_exit EQUAL 0)
  message(FATAL_ERROR
    "the built binary failed (exit ${native_exit})\n--- stderr ---\n${native_err}")
endif()
require_contains("native binary" "${native_out}" "Hello, world!")

run_oskelp("${WORK_DIR}/demo" run)
require_success("oskelp run")
require_contains("oskelp run" "${run_out}" "Hello, world!")

# --- 5. `test` runs the tests on both engines ------------------------------
run_oskelp("${WORK_DIR}/demo" test)
require_success("oskelp test")
require_contains("oskelp test" "${run_out}" "test result: ok")

run_oskelp("${WORK_DIR}/demo" test --native)
require_success("oskelp test --native")
require_contains("oskelp test --native" "${run_out}" "test result: ok")

# --- 6. `tree` shows the root package --------------------------------------
run_oskelp("${WORK_DIR}/demo" tree)
require_success("oskelp tree")
require_contains("oskelp tree" "${run_out}" "demo v0.1.0")

# --- 7. A path dependency is resolved and materialised ---------------------
run_oskelp("${WORK_DIR}" new utils)
require_success("oskelp new utils")

file(WRITE "${WORK_DIR}/utils/src/utils/text.oskl" [=[
fn shout(text) {
  return text + "!";
}
]=])

file(WRITE "${WORK_DIR}/demo/oskel.toml" [=[
[package]
name = "demo"
version = "0.1.0"
entry = "src/demo/main.oskl"

[dependencies]
utils = { path = "../utils" }
]=])

file(WRITE "${WORK_DIR}/demo/src/demo/main.oskl" [=[
import utils.text;

print(utils.text.shout("hello"));
]=])

run_oskelp("${WORK_DIR}/demo" build)
require_success("oskelp build with a path dependency")
if(NOT EXISTS "${WORK_DIR}/demo/.oskel/deps/utils/text.oskl")
  message(FATAL_ERROR "the dependency was not materialised under .oskel/deps")
endif()

execute_process(
  COMMAND "${WORK_DIR}/demo/target/demo${exe_suffix}"
  WORKING_DIRECTORY "${WORK_DIR}/demo"
  RESULT_VARIABLE dependency_exit
  OUTPUT_VARIABLE dependency_out
  ERROR_VARIABLE dependency_err)
if(NOT dependency_exit EQUAL 0)
  message(FATAL_ERROR
    "the dependency binary failed (exit ${dependency_exit})\n"
    "--- stderr ---\n${dependency_err}")
endif()
require_contains("dependency binary" "${dependency_out}" "hello!")

run_oskelp("${WORK_DIR}/demo" tree)
require_success("oskelp tree with a dependency")
require_contains("oskelp tree with a dependency" "${run_out}" "utils v0.1.0")

# --- 7b. `--manifest-path` runs a project from outside its directory -------
run_oskelp("${WORK_DIR}" build --manifest-path "${WORK_DIR}/demo/oskel.toml")
require_success("oskelp build --manifest-path")

# --- 8. `--locked` refuses a missing or stale lockfile ---------------------
file(REMOVE "${WORK_DIR}/demo/oskel.lock")
run_oskelp("${WORK_DIR}/demo" build --locked)
require_failure("oskelp build --locked without a lockfile" "1")
require_contains("oskelp build --locked without a lockfile" "${run_err}"
                 "requires oskel.lock")

run_oskelp("${WORK_DIR}/demo" build)
require_success("oskelp build writes the lockfile again")

run_oskelp("${WORK_DIR}/demo" build --locked)
require_success("oskelp build --locked with a matching lockfile")

# --- 9. A failing test makes `test` exit 101 -------------------------------
file(WRITE "${WORK_DIR}/demo/tests/failing.oskl" [=[
print("this test fails");
exit(1);
]=])
run_oskelp("${WORK_DIR}/demo" test)
require_failure("oskelp test with a failing test" "101")
require_contains("oskelp test with a failing test" "${run_out}" "FAILED")
file(REMOVE "${WORK_DIR}/demo/tests/failing.oskl")

# --- 10. A dependency cycle is rejected ------------------------------------
run_oskelp("${WORK_DIR}" new a)
require_success("oskelp new a")
run_oskelp("${WORK_DIR}" new b)
require_success("oskelp new b")

file(WRITE "${WORK_DIR}/a/oskel.toml" [=[
[package]
name = "a"
version = "0.1.0"
entry = "src/a/main.oskl"

[dependencies]
b = { path = "../b" }
]=])
file(WRITE "${WORK_DIR}/b/oskel.toml" [=[
[package]
name = "b"
version = "0.1.0"
entry = "src/b/main.oskl"

[dependencies]
a = { path = "../a" }
]=])

run_oskelp("${WORK_DIR}/a" build)
require_failure("oskelp build with a dependency cycle" "1")
require_contains("oskelp build with a dependency cycle" "${run_err}"
                 "dependency cycle")

# --- 11. `clean` removes the generated directories -------------------------
run_oskelp("${WORK_DIR}/demo" clean)
require_success("oskelp clean")
if(EXISTS "${WORK_DIR}/demo/target" OR EXISTS "${WORK_DIR}/demo/.oskel")
  message(FATAL_ERROR "oskelp clean left target/ or .oskel/ behind")
endif()

# --- 12. `init` fills an existing directory --------------------------------
file(MAKE_DIRECTORY "${WORK_DIR}/existing")
run_oskelp("${WORK_DIR}" init existing)
require_success("oskelp init existing")
if(NOT EXISTS "${WORK_DIR}/existing/oskel.toml")
  message(FATAL_ERROR "oskelp init did not create oskel.toml")
endif()
if(NOT EXISTS "${WORK_DIR}/existing/src/existing/main.oskl")
  message(FATAL_ERROR "oskelp init did not create src/existing/main.oskl")
endif()

# --- 13. `--version` and `--help` ------------------------------------------
run_oskelp("${WORK_DIR}" --version)
require_success("oskelp --version")
require_contains("oskelp --version" "${run_out}" "oskelp")

run_oskelp("${WORK_DIR}" --help)
require_success("oskelp --help")
require_contains("oskelp --help" "${run_out}" "usage:")

# --- 14. A registry dependency is refused with an explanation --------------
file(WRITE "${WORK_DIR}/demo/oskel.toml" [=[
[package]
name = "demo"
version = "0.1.0"
entry = "src/demo/main.oskl"

[dependencies]
utils = "1.2.3"
]=])
run_oskelp("${WORK_DIR}/demo" build)
require_failure("a registry dependency" "66")
require_contains("a registry dependency" "${run_err}"
                 "registry dependencies are not supported")

message(STATUS "oskelp end-to-end: all checks passed")
