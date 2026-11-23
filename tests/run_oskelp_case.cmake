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

# Publish 0.1.0 and 0.2.0 into the local registry. The assertions use the
# filesystem, not stdout: `string(FIND` parses escapes in the expanded
# haystack, so a Windows path (`D:\a\...`) would fail with "Invalid character
# escape '\a'".
run_oskelp("${WORK_DIR}/utils" publish --registry "${REGISTRY}")
require_success("oskelp publish 0.1.0")
if(NOT EXISTS "${REGISTRY}/utils/0.1.0/oskel.toml" OR
   NOT EXISTS "${REGISTRY}/utils/0.1.0/src/utils/text.oskl")
  message(FATAL_ERROR "oskelp publish did not copy the manifest and src/")
endif()

run_oskelp("${WORK_DIR}/utils" publish --registry "${REGISTRY}")
require_failure("oskelp publish a duplicate" "1")
require_file_contains("${REGISTRY}/utils/0.1.0/oskel.toml"
                      "version = \"0.1.0\"")

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

# `add` without --version uses the latest release and writes a caret
# requirement. `outdated` then compares the lock with the registry; publishing
# a newer compatible and a newer incompatible release exercises both branches.
run_oskelp("${WORK_DIR}" new latest_app)
require_success("oskelp new latest_app")
run_oskelp("${WORK_DIR}/latest_app" add utils)
require_success("oskelp add without --version")
require_contains("oskelp add without --version" "${run_out}"
                 "added 'utils' (^0.2.0)")
require_file_contains("${WORK_DIR}/latest_app/oskel.toml"
                      "utils = \"^0.2.0\"")
require_file_contains("${WORK_DIR}/latest_app/oskel.lock"
                      "source = \"registry+utils@0.2.0\"")

run_oskelp("${WORK_DIR}/latest_app" outdated)
require_success("oskelp outdated with the latest pin")
require_contains("oskelp outdated with the latest pin" "${run_out}"
                 "All registry dependencies are up to date.")

file(READ "${WORK_DIR}/utils/oskel.toml" utils_manifest)
string(REPLACE "version = \"0.2.0\"" "version = \"0.2.1\"" utils_manifest
       "${utils_manifest}")
file(WRITE "${WORK_DIR}/utils/oskel.toml" "${utils_manifest}")
run_oskelp("${WORK_DIR}/utils" publish --registry "${REGISTRY}")
require_success("oskelp publish 0.2.1")

file(READ "${WORK_DIR}/utils/oskel.toml" utils_manifest)
string(REPLACE "version = \"0.2.1\"" "version = \"0.3.0\"" utils_manifest
       "${utils_manifest}")
file(WRITE "${WORK_DIR}/utils/oskel.toml" "${utils_manifest}")
run_oskelp("${WORK_DIR}/utils" publish --registry "${REGISTRY}")
require_success("oskelp publish 0.3.0")

run_oskelp("${WORK_DIR}/latest_app" outdated)
require_success("oskelp outdated finds a compatible update")
require_contains("oskelp outdated finds a compatible update" "${run_out}"
                 "utils 0.2.0 -> 0.2.1 (^0.2.0)")
require_contains("oskelp outdated finds a compatible update" "${run_out}"
                 "latest 0.3.0 needs a new requirement")

run_oskelp("${WORK_DIR}/latest_app" update utils --precise 0.2.1)
require_success("oskelp update --precise picks an allowed version")
require_file_contains("${WORK_DIR}/latest_app/oskel.lock"
                      "source = \"registry+utils@0.2.1\"")

run_oskelp("${WORK_DIR}/latest_app" outdated)
require_success("oskelp outdated reports the incompatible latest")
require_contains("oskelp outdated reports the incompatible latest" "${run_out}"
                 "utils 0.2.1 (latest 0.3.0 needs a new requirement")

run_oskelp("${WORK_DIR}/latest_app" update utils --precise 0.3.0)
require_failure("oskelp update --precise outside the requirement" "1")
require_contains("oskelp update --precise outside the requirement" "${run_err}"
                 "does not satisfy '^0.2.0'")
require_file_contains("${WORK_DIR}/latest_app/oskel.lock"
                      "source = \"registry+utils@0.2.1\"")

run_oskelp("${WORK_DIR}/latest_app" update utils --precise 0.2.9)
require_failure("oskelp update --precise for a missing version" "1")
require_contains("oskelp update --precise for a missing version" "${run_err}"
                 "has no 'utils' version '0.2.9'")

run_oskelp("${WORK_DIR}/latest_app" update --precise 0.2.1)
require_failure("oskelp update --precise without a package" "64")
require_contains("oskelp update --precise without a package" "${run_err}"
                 "--precise needs a package name")

run_oskelp("${WORK_DIR}/latest_app" update utils --precise nope)
require_failure("oskelp update --precise with a bad version" "64")
require_contains("oskelp update --precise with a bad version" "${run_err}"
                 "is not a semantic version")

run_oskelp("${WORK_DIR}/latest_app" update utils --precise)
require_failure("oskelp update --precise without a value" "64")

# A path dependency has no registry version to pin.
run_oskelp("${WORK_DIR}/demo" update utils --precise 0.1.0)
require_failure("oskelp update --precise on a path dependency" "1")
require_contains("oskelp update --precise on a path dependency" "${run_err}"
                 "is a path dependency")

# A prerelease-only package is not picked implicitly: the caret requirement
# `^<prerelease>` is ambiguous, so the user must name the version.
run_oskelp("${WORK_DIR}" new pre)
require_success("oskelp new pre")
run_oskelp("${WORK_DIR}" new pre_app)
require_success("oskelp new pre_app")
file(READ "${WORK_DIR}/pre/oskel.toml" pre_manifest)
string(REPLACE "version = \"0.1.0\"" "version = \"0.1.0-beta.1\""
       pre_manifest "${pre_manifest}")
file(WRITE "${WORK_DIR}/pre/oskel.toml" "${pre_manifest}")
run_oskelp("${WORK_DIR}/pre" publish --registry "${REGISTRY}")
require_success("oskelp publish a prerelease")
run_oskelp("${WORK_DIR}/pre_app" add pre)
require_failure("oskelp add a prerelease-only package" "1")
require_contains("oskelp add a prerelease-only package" "${run_err}"
                 "pass an explicit version")
run_oskelp("${WORK_DIR}/pre_app" add pre --version "0.1.0-beta.1")
require_success("oskelp add a prerelease with an exact version")
require_file_contains("${WORK_DIR}/pre_app/oskel.toml"
                      "pre = \"0.1.0-beta.1\"")

# A yanked release is hidden from new selections but an existing lockfile pin
# keeps working; --undo restores it.
run_oskelp("${WORK_DIR}" new yank_app)
require_success("oskelp new yank_app")
run_oskelp("${WORK_DIR}/yank_app" add utils --version "^0.2.0")
require_success("oskelp add a version that will be yanked")
require_file_contains("${WORK_DIR}/yank_app/oskel.lock"
                      "source = \"registry+utils@0.2.1\"")

run_oskelp("${WORK_DIR}/yank_app" yank utils --version 0.2.1)
require_success("oskelp yank a published version")
require_contains("oskelp yank a published version" "${run_out}"
                 "yanked utils 0.2.1")
run_oskelp("${WORK_DIR}/yank_app" yank utils --version 0.2.1)
require_success("oskelp yank is idempotent")
require_contains("oskelp yank is idempotent" "${run_out}"
                 "already yanked")

run_oskelp("${WORK_DIR}/yank_app" outdated)
require_success("oskelp outdated reports a yanked pin")
require_contains("oskelp outdated reports a yanked pin" "${run_out}"
                 "utils 0.2.1 (yanked")
require_contains("oskelp outdated reports a yanked pin" "${run_out}"
                 "latest 0.3.0")

run_oskelp("${WORK_DIR}/yank_app" update utils --precise 0.2.1)
require_failure("oskelp update --precise a yanked version" "1")
require_contains("oskelp update --precise a yanked version" "${run_err}"
                 "is yanked")

run_oskelp("${WORK_DIR}/yank_app" check --locked)
require_success("oskelp check --locked keeps a yanked pin")

run_oskelp("${WORK_DIR}" new yank_add)
require_success("oskelp new yank_add")
run_oskelp("${WORK_DIR}/yank_add" add utils --version "^0.2.0")
require_success("oskelp add skips a yanked version")
require_file_contains("${WORK_DIR}/yank_add/oskel.lock"
                      "source = \"registry+utils@0.2.0\"")

run_oskelp("${WORK_DIR}/yank_app" yank utils --version 0.2.1 --undo)
require_success("oskelp yank --undo restores a version")
require_contains("oskelp yank --undo restores a version" "${run_out}"
                 "unyanked utils 0.2.1")
run_oskelp("${WORK_DIR}/yank_app" update utils --precise 0.2.1)
require_success("oskelp update --precise after --undo")

run_oskelp("${WORK_DIR}/yank_app" yank utils --version 0.2.1 --undo)
require_success("oskelp yank --undo is idempotent")
require_contains("oskelp yank --undo is idempotent" "${run_out}"
                 "is not yanked")

run_oskelp("${WORK_DIR}/yank_app" yank utils --version 9.9.9)
require_failure("oskelp yank a missing version" "1")
require_contains("oskelp yank a missing version" "${run_err}"
                 "has no 'utils' version '9.9.9'")

run_oskelp("${WORK_DIR}/yank_app" yank utils)
require_failure("oskelp yank without a version" "64")
require_contains("oskelp yank without a version" "${run_err}"
                 "--version <version> is required")

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


# --- 15. A `[workspace]` shares one lockfile and one target/ ---------------
set(ENV{OSKEL_REGISTRY} "${REGISTRY}")
file(MAKE_DIRECTORY "${WORK_DIR}/ws")
run_oskelp("${WORK_DIR}/ws" new alpha)
require_success("oskelp new alpha")
run_oskelp("${WORK_DIR}/ws" new beta)
require_success("oskelp new beta")
file(WRITE "${WORK_DIR}/ws/oskel.toml" [=[
[workspace]
members = ["alpha", "beta"]
]=])

run_oskelp("${WORK_DIR}/ws/beta" add alpha --path ../alpha)
require_success("oskelp add a workspace member as a path dependency")
require_file_contains("${WORK_DIR}/ws/beta/oskel.toml"
                      "alpha = { path = \"../alpha\" }")
file(WRITE "${WORK_DIR}/ws/beta/src/beta/main.oskl" [=[
import { message } from alpha.greeting;

print(message("beta"));
]=])

run_oskelp("${WORK_DIR}/ws" build)
require_success("oskelp build every workspace member")
require_contains("oskelp build every workspace member" "${run_out}"
                 "built target/alpha")
require_contains("oskelp build every workspace member" "${run_out}"
                 "built target/beta")
if(NOT EXISTS "${WORK_DIR}/ws/target/alpha${exe_suffix}" OR
   NOT EXISTS "${WORK_DIR}/ws/target/beta${exe_suffix}")
  message(FATAL_ERROR "the workspace binaries are not under the shared target/")
endif()

run_oskelp("${WORK_DIR}/ws" run -p beta)
require_success("oskelp run --package")
require_contains("oskelp run --package" "${run_out}" "Hello, beta!")

run_oskelp("${WORK_DIR}/ws" run)
require_failure("oskelp run in an ambiguous workspace" "1")
require_contains("oskelp run in an ambiguous workspace" "${run_err}"
                 "--package <name>")

run_oskelp("${WORK_DIR}/ws" add utils --version "0.1.0")
require_failure("oskelp add in an ambiguous workspace" "1")
require_contains("oskelp add in an ambiguous workspace" "${run_err}"
                 "--package <name>")

run_oskelp("${WORK_DIR}/ws" add utils -p alpha --version "0.1.0")
require_success("oskelp add a registry dependency to a member")
run_oskelp("${WORK_DIR}/ws" add utils -p beta --version "0.1.0")
require_success("oskelp add the same registry dependency to another member")
if(NOT EXISTS "${WORK_DIR}/ws/oskel.lock")
  message(FATAL_ERROR "the workspace did not write a root oskel.lock")
endif()
require_file_contains("${WORK_DIR}/ws/oskel.lock"
                      "source = \"registry+utils@0.1.0\"")
if(EXISTS "${WORK_DIR}/ws/alpha/oskel.lock" OR
   EXISTS "${WORK_DIR}/ws/beta/oskel.lock")
  message(FATAL_ERROR "a workspace member wrote its own oskel.lock")
endif()
if(NOT EXISTS "${WORK_DIR}/ws/.oskel/deps/utils/text.oskl")
  message(FATAL_ERROR "the workspace did not materialise the registry package")
endif()

run_oskelp("${WORK_DIR}/ws" outdated)
require_success("oskelp outdated in a workspace")
require_contains("oskelp outdated in a workspace" "${run_out}"
                 "utils 0.1.0 (latest 0.3.0")

run_oskelp("${WORK_DIR}/ws" check -p alpha)
require_success("oskelp check --package")
require_contains("oskelp check --package" "${run_out}" "checked alpha:")

run_oskelp("${WORK_DIR}/ws" test)
require_success("oskelp test every workspace member")
require_contains("oskelp test every workspace member" "${run_out}"
                 "2 passed")

run_oskelp("${WORK_DIR}/ws" tree)
string(REPLACE "\\" "/" run_out "${run_out}")
require_success("oskelp tree in a workspace")
require_contains("oskelp tree in a workspace" "${run_out}" "workspace ")
require_contains("oskelp tree in a workspace" "${run_out}" "alpha v0.1.0")
require_contains("oskelp tree in a workspace" "${run_out}" "beta v0.1.0")

# The invoked member wins when the command runs inside it.
run_oskelp("${WORK_DIR}/ws/alpha" build)
require_success("oskelp build from a member")
require_contains("oskelp build from a member" "${run_out}" "built target/alpha")
if(EXISTS "${WORK_DIR}/ws/alpha/target")
  message(FATAL_ERROR "a member wrote its own target/")
endif()

run_oskelp("${WORK_DIR}/ws" clean)
require_success("oskelp clean in a workspace")
if(EXISTS "${WORK_DIR}/ws/target" OR EXISTS "${WORK_DIR}/ws/.oskel")
  message(FATAL_ERROR "oskelp clean left the workspace target/ or .oskel/")
endif()

# --- 15a. A root package is the default member -----------------------------
file(MAKE_DIRECTORY "${WORK_DIR}/wsroot")
run_oskelp("${WORK_DIR}" init wsroot --name root)
require_success("oskelp init a root package")
run_oskelp("${WORK_DIR}/wsroot" new member)
require_success("oskelp new a root workspace member")
file(APPEND "${WORK_DIR}/wsroot/oskel.toml"
     "\n[workspace]\nmembers = [\"member\"]\n")

run_oskelp("${WORK_DIR}/wsroot" build)
require_success("oskelp build defaults to the root package")
require_contains("oskelp build defaults to the root package" "${run_out}"
                 "built target/root")
if(EXISTS "${WORK_DIR}/wsroot/target/member${exe_suffix}")
  message(FATAL_ERROR "the root package build also built a member")
endif()
run_oskelp("${WORK_DIR}/wsroot" build -p member)
require_success("oskelp build -p selects a member of a root package workspace")
require_contains("oskelp build -p selects a member of a root package workspace"
                 "${run_out}" "built target/member")

run_oskelp("${WORK_DIR}/wsroot" add utils --version "0.1.0")
require_success("oskelp add defaults to the root package")
require_file_contains("${WORK_DIR}/wsroot/oskel.toml" "utils = \"0.1.0\"")
run_oskelp("${WORK_DIR}/wsroot" add utils -p member --version "0.1.0")
require_success("oskelp add -p edits the member")
require_file_contains("${WORK_DIR}/wsroot/member/oskel.toml" "utils = \"0.1.0\"")

# --- 15b. An invalid workspace is rejected --------------------------------
file(MAKE_DIRECTORY "${WORK_DIR}/badws")
file(WRITE "${WORK_DIR}/badws/oskel.toml" [=[
[workspace]
members = ["missing"]
]=])
run_oskelp("${WORK_DIR}/badws" build)
require_failure("oskelp build with a missing member" "66")
require_contains("oskelp build with a missing member" "${run_err}"
                 "has no oskel.toml")

run_oskelp("${WORK_DIR}/ws/alpha" add beta --version "0.1.0")
require_failure("oskelp add a registry requirement on a member" "1")
require_contains("oskelp add a registry requirement on a member" "${run_err}"
                 "is a workspace member")

unset(ENV{OSKEL_REGISTRY})

message(STATUS "oskelp end-to-end: all checks passed")
