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
#   -DOSKEL_AOT_RUNNABLE=<ON|OFF, whether the AOT backend can run here>
#   -DWORK_DIR=<scratch directory>

if(NOT DEFINED OSKELP_EXE OR NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKELC_EXE
   OR NOT DEFINED OSKEL_AOT_RUNNABLE OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR
    "run_oskelp_case.cmake requires OSKELP_EXE, OSKEL_EXE, OSKELC_EXE, "
    "OSKEL_AOT_RUNNABLE and WORK_DIR")
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

macro(require_file_contains path needle)
  file(READ "${path}" contents)
  string(FIND "${contents}" "${needle}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR
      "expected '${needle}' in ${path}:\n${contents}")
  endif()
endmacro()

macro(require_file_not_contains path needle)
  file(READ "${path}" contents)
  string(FIND "${contents}" "${needle}" position)
  if(NOT position EQUAL -1)
    message(FATAL_ERROR
      "did not expect '${needle}' in ${path}:\n${contents}")
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
if(OSKEL_AOT_RUNNABLE)
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
endif()

run_oskelp("${WORK_DIR}/demo" run)
require_success("oskelp run")
require_contains("oskelp run" "${run_out}" "Hello, world!")

# --- 5. `test` runs the tests on both engines ------------------------------
run_oskelp("${WORK_DIR}/demo" test)
require_success("oskelp test")
require_contains("oskelp test" "${run_out}" "test result: ok")

if(OSKEL_AOT_RUNNABLE)
  run_oskelp("${WORK_DIR}/demo" test --native)
  require_success("oskelp test --native")
  require_contains("oskelp test --native" "${run_out}" "test result: ok")
endif()

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
]=])

file(WRITE "${WORK_DIR}/demo/src/demo/main.oskl" [=[
import utils.text;

print(utils.text.shout("hello"));
]=])

# `add` edits the manifest, refreshes the lockfile and materialises the tree.
run_oskelp("${WORK_DIR}/demo" add utils --path ../utils)
require_success("oskelp add a path dependency")
require_contains("oskelp add a path dependency" "${run_out}" "added 'utils'")
require_file_contains("${WORK_DIR}/demo/oskel.toml"
                      "utils = { path = \"../utils\" }")
require_file_contains("${WORK_DIR}/demo/oskel.lock" "packages.utils")

run_oskelp("${WORK_DIR}/demo" add utils --path ../utils)
require_success("oskelp add is idempotent")
require_contains("oskelp add is idempotent" "${run_out}" "already present")

run_oskelp("${WORK_DIR}/demo" add wrong --path ../utils)
require_failure("oskelp add a mismatched name" "1")
require_contains("oskelp add a mismatched name" "${run_err}" "is named 'utils'")

run_oskelp("${WORK_DIR}/demo" add demo --path .)
require_failure("oskelp add a self dependency" "1")
require_contains("oskelp add a self dependency" "${run_err}"
                 "cannot depend on itself")

run_oskelp("${WORK_DIR}/demo" add missing --path ../missing)
require_failure("oskelp add a missing path" "1")
require_contains("oskelp add a missing path" "${run_err}" "has no oskel.toml")

run_oskelp("${WORK_DIR}/demo" add utils --path ../utils --version 9.9.9)
require_failure("oskelp add a mismatched version" "1")
require_contains("oskelp add a mismatched version" "${run_err}"
                 "is version '0.1.0'")

run_oskelp("${WORK_DIR}/demo" add utils --path ../utils --version 0.1.0)
require_success("oskelp add an exact version")
require_contains("oskelp add an exact version" "${run_out}" "updated 'utils'")
require_file_contains("${WORK_DIR}/demo/oskel.toml"
                      "version = \"0.1.0\" }")

run_oskelp("${WORK_DIR}/demo" build)
require_success("oskelp build with a path dependency")
if(NOT EXISTS "${WORK_DIR}/demo/.oskel/deps/utils/text.oskl")
  message(FATAL_ERROR "the dependency was not materialised under .oskel/deps")
endif()

if(OSKEL_AOT_RUNNABLE)
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
endif()

# --- 7a. `remove` drops the dependency and refreshes the tree --------------
run_oskelp("${WORK_DIR}/demo" remove utils)
require_success("oskelp remove utils")
require_contains("oskelp remove utils" "${run_out}" "removed 'utils'")
require_file_not_contains("${WORK_DIR}/demo/oskel.toml" "utils = {")
require_file_not_contains("${WORK_DIR}/demo/oskel.lock" "packages.utils")
if(EXISTS "${WORK_DIR}/demo/.oskel/deps/utils")
  message(FATAL_ERROR "oskelp remove left .oskel/deps/utils behind")
endif()

run_oskelp("${WORK_DIR}/demo" remove utils)
require_failure("oskelp remove an absent dependency" "1")
require_contains("oskelp remove an absent dependency" "${run_err}"
                 "is not declared")

run_oskelp("${WORK_DIR}/demo" add --path ../utils)
require_success("oskelp add derives the name")
require_contains("oskelp add derives the name" "${run_out}" "added 'utils'")
require_file_contains("${WORK_DIR}/demo/oskel.toml"
                      "utils = { path = \"../utils\" }")

run_oskelp("${WORK_DIR}/demo" tree)
require_success("oskelp tree with a dependency")
require_contains("oskelp tree with a dependency" "${run_out}" "utils v0.1.0")

# --- 7b. `--manifest-path` runs a project from outside its directory -------
run_oskelp("${WORK_DIR}" remove utils --manifest-path "${WORK_DIR}/demo/oskel.toml")
require_success("oskelp remove --manifest-path")
run_oskelp("${WORK_DIR}" add utils --path utils --manifest-path "${WORK_DIR}/demo/oskel.toml")
require_success("oskelp add --manifest-path")
require_file_contains("${WORK_DIR}/demo/oskel.toml"
                      "utils = { path = \"../utils\" }")
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

# `add` resolves the graph before it writes, so a cycle it would create leaves
# the manifest untouched.
run_oskelp("${WORK_DIR}/a" add b --path ../b)
require_success("oskelp add b to a")
run_oskelp("${WORK_DIR}/b" add a --path ../a)
require_failure("oskelp add a dependency cycle" "1")
require_contains("oskelp add a dependency cycle" "${run_err}"
                 "dependency cycle")
require_file_not_contains("${WORK_DIR}/b/oskel.toml" "a = {")

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

# --- 14. The local registry: publish, consume, update, --locked ------------
set(REGISTRY "${WORK_DIR}/registry")
unset(ENV{OSKEL_REGISTRY})

# A registry dependency without OSKEL_REGISTRY fails with the variable name,
# not with the generic "global is read but never assigned".
run_oskelp("${WORK_DIR}" new registry_app)
require_success("oskelp new registry_app")
file(WRITE "${WORK_DIR}/registry_app/src/registry_app/main.oskl" [=[
import utils.text;

print(utils.text.shout("registry"));
]=])
run_oskelp("${WORK_DIR}/registry_app" add utils --version "0.1.0")
require_failure("oskelp add without OSKEL_REGISTRY" "1")
require_contains("oskelp add without OSKEL_REGISTRY" "${run_err}"
                 "OSKEL_REGISTRY")

# Publish 0.1.0 and 0.2.0 into the local registry.
run_oskelp("${WORK_DIR}/utils" publish --registry "${REGISTRY}")
require_success("oskelp publish 0.1.0")
require_contains("oskelp publish 0.1.0" "${run_out}" "published utils 0.1.0")
if(NOT EXISTS "${REGISTRY}/utils/0.1.0/oskel.toml" OR
   NOT EXISTS "${REGISTRY}/utils/0.1.0/src/utils/text.oskl")
  message(FATAL_ERROR "oskelp publish did not copy the manifest and src/")
endif()

run_oskelp("${WORK_DIR}/utils" publish --registry "${REGISTRY}")
require_failure("oskelp publish a duplicate" "1")
require_contains("oskelp publish a duplicate" "${run_err}" "already published")

file(READ "${WORK_DIR}/utils/oskel.toml" utils_manifest)
string(REPLACE "version = \"0.1.0\"" "version = \"0.2.0\"" utils_manifest
       "${utils_manifest}")
file(WRITE "${WORK_DIR}/utils/oskel.toml" "${utils_manifest}")
run_oskelp("${WORK_DIR}/utils" publish --registry "${REGISTRY}")
require_success("oskelp publish 0.2.0")

# A path dependency cannot be published: the consumer would resolve it
# relative to the registry copy.
run_oskelp("${WORK_DIR}/demo" publish --registry "${REGISTRY}")
require_failure("oskelp publish with a path dependency" "1")
require_contains("oskelp publish with a path dependency" "${run_err}"
                 "path dependency 'utils'")

# Consume the registry package by version.
set(ENV{OSKEL_REGISTRY} "${REGISTRY}")
run_oskelp("${WORK_DIR}/registry_app" add utils --version "0.1.0")
require_success("oskelp add a registry dependency")
require_contains("oskelp add a registry dependency" "${run_out}"
                 "added 'utils'")
require_file_contains("${WORK_DIR}/registry_app/oskel.toml"
                      "utils = \"0.1.0\"")
require_file_contains("${WORK_DIR}/registry_app/oskel.lock"
                      "source = \"registry+utils@0.1.0\"")
if(NOT EXISTS "${WORK_DIR}/registry_app/.oskel/deps/utils/text.oskl")
  message(FATAL_ERROR "the registry dependency was not materialised")
endif()

run_oskelp("${WORK_DIR}/registry_app" add utils --version "0.1.0")
require_success("oskelp add a registry dependency is idempotent")
require_contains("oskelp add a registry dependency is idempotent" "${run_out}"
                 "already present")

run_oskelp("${WORK_DIR}/registry_app" add utils --version ">=9.0.0")
require_failure("oskelp add an unsatisfiable requirement" "1")
require_contains("oskelp add an unsatisfiable requirement" "${run_err}"
                 "no version of 'utils' satisfies")
require_file_contains("${WORK_DIR}/registry_app/oskel.toml"
                      "utils = \"0.1.0\"")

run_oskelp("${WORK_DIR}/registry_app" build)
require_success("oskelp build with a registry dependency")
if(OSKEL_AOT_RUNNABLE)
  execute_process(
    COMMAND "${WORK_DIR}/registry_app/target/registry_app${exe_suffix}"
    WORKING_DIRECTORY "${WORK_DIR}/registry_app"
    RESULT_VARIABLE registry_exit
    OUTPUT_VARIABLE registry_out
    ERROR_VARIABLE registry_err)
  if(NOT registry_exit EQUAL 0)
    message(FATAL_ERROR
      "the registry binary failed (exit ${registry_exit})\n"
      "--- stderr ---\n${registry_err}")
  endif()
  require_contains("registry binary" "${registry_out}" "registry!")
endif()

# The lockfile pins 0.1.0: a later 0.2.0 does not upgrade a build.
run_oskelp("${WORK_DIR}/registry_app" build --locked)
require_success("oskelp build --locked with a registry pin")

# A manifest change to 0.2.0 no longer satisfies the pin; --locked fails
# instead of silently upgrading.
file(READ "${WORK_DIR}/registry_app/oskel.toml" registry_manifest)
string(REPLACE "utils = \"0.1.0\"" "utils = \"0.2.0\"" registry_manifest
       "${registry_manifest}")
file(WRITE "${WORK_DIR}/registry_app/oskel.toml" "${registry_manifest}")
run_oskelp("${WORK_DIR}/registry_app" build --locked)
require_failure("oskelp build --locked after a requirement change" "1")
require_contains("oskelp build --locked after a requirement change" "${run_err}"
                 "pins 'utils' at 0.1.0")

# `update` moves the pin and refreshes the materialised tree.
run_oskelp("${WORK_DIR}/registry_app" update utils)
require_success("oskelp update utils")
require_contains("oskelp update utils" "${run_out}"
                 "updated utils 0.1.0 -> 0.2.0")
require_file_contains("${WORK_DIR}/registry_app/oskel.lock"
                      "source = \"registry+utils@0.2.0\"")
run_oskelp("${WORK_DIR}/registry_app" build --locked)
require_success("oskelp build --locked after update")

run_oskelp("${WORK_DIR}/registry_app" update)
require_success("oskelp update everything")
require_contains("oskelp update everything" "${run_out}"
                 "everything is up to date")

run_oskelp("${WORK_DIR}/registry_app" update missing)
require_failure("oskelp update an unknown package" "1")
require_contains("oskelp update an unknown package" "${run_err}"
                 "not in the dependency graph")

run_oskelp("${WORK_DIR}/registry_app" tree)
require_success("oskelp tree with a registry dependency")
require_contains("oskelp tree with a registry dependency" "${run_out}"
                 "utils v0.2.0")

# Removing the last registry dependency drops the lock entry.
run_oskelp("${WORK_DIR}/registry_app" remove utils)
require_success("oskelp remove a registry dependency")
require_file_not_contains("${WORK_DIR}/registry_app/oskel.toml" "utils =")
require_file_not_contains("${WORK_DIR}/registry_app/oskel.lock"
                          "packages.utils")
if(EXISTS "${WORK_DIR}/registry_app/.oskel/deps/utils")
  message(FATAL_ERROR
    "oskelp remove left the registry dependency materialised")
endif()

# Without OSKEL_REGISTRY a registry dependency fails with the variable name.
unset(ENV{OSKEL_REGISTRY})
file(READ "${WORK_DIR}/registry_app/oskel.toml" registry_manifest)
string(REPLACE "[dependencies]" "[dependencies]\nutils = \"0.1.0\""
       registry_manifest "${registry_manifest}")
file(WRITE "${WORK_DIR}/registry_app/oskel.toml" "${registry_manifest}")
run_oskelp("${WORK_DIR}/registry_app" build)
require_failure("oskelp build without OSKEL_REGISTRY" "1")
require_contains("oskelp build without OSKEL_REGISTRY" "${run_err}"
                 "OSKEL_REGISTRY")

message(STATUS "oskelp end-to-end: all checks passed")
