# Fails when a builtin exists in one engine and not the other.
#
# The development engine and the AOT backend are two front ends of one language
# (AGENTS.md: "VM/AOT parity is part of every feature"). A builtin registered by
# `registerBuiltins` but not seeded into the AOT chunk runs in the VM and fails
# `oskel build` with "global 'X' is read but never assigned" — a divergence that
# no single-engine test can see, because each engine is self-consistent.
#
# This check reads both registries from the source and compares the name sets:
#
#   * the VM side is every `addNative("name", ...)` in `src/runtime/builtins.cpp`
#     (plus the hidden `$` helpers, which are registered the same way);
#   * the AOT side is every entry of the `builtins` map in
#     `src/frontend/native/lower.cpp`, in both its literal (`{"name", rt.x}`)
#     and its conditional (`builtins.emplace("name", ...)`,
#     `builtins["name"] = ...`) forms.
#
# The names that legitimately exist on one side only are listed below with the
# reason. Adding a builtin to one engine without the other fails the check until
# the name is either seeded or listed — which is the moment to decide, not to
# discover it from a user's failing build. BACKLOG §6 tracks the list.
#
# Usage: cmake -DROOT=<repository root> -P tests/check_builtin_parity.cmake

if(NOT DEFINED ROOT)
  get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(vm_source "${ROOT}/src/runtime/builtins.cpp")
set(aot_source "${ROOT}/src/frontend/native/lower.cpp")
foreach(f IN ITEMS "${vm_source}" "${aot_source}")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "'${f}' does not exist")
  endif()
endforeach()

# --- the VM registry -------------------------------------------------------
file(READ "${vm_source}" vm_text)
string(REGEX MATCHALL "addNative\\(\"([^\"]+)\"" vm_matches "${vm_text}")
set(vm_builtins "")
foreach(m IN LISTS vm_matches)
  string(REGEX REPLACE "addNative\\(\"([^\"]+)\"" "\\1" name "${m}")
  list(APPEND vm_builtins "${name}")
endforeach()
list(REMOVE_DUPLICATES vm_builtins)
list(SORT vm_builtins)

# --- the AOT registry ------------------------------------------------------
file(READ "${aot_source}" aot_text)
set(aot_builtins "")

# The map literal: {"print", rt.printBuiltin},
string(REGEX MATCHALL "\\{\"([^\"]+)\",[ \t]*rt\\." aot_matches "${aot_text}")
foreach(m IN LISTS aot_matches)
  string(REGEX REPLACE "\\{\"([^\"]+)\",.*" "\\1" name "${m}")
  list(APPEND aot_builtins "${name}")
endforeach()

# The conditional seeds: builtins.emplace("input", rt.inputBuiltin) and
# builtins["memSize"] = rt.memSizeBuiltin.
string(REGEX MATCHALL "builtins\\.emplace\\(\"([^\"]+)\"" aot_emplace "${aot_text}")
foreach(m IN LISTS aot_emplace)
  string(REGEX REPLACE "builtins\\.emplace\\(\"([^\"]+)\"" "\\1" name "${m}")
  list(APPEND aot_builtins "${name}")
endforeach()
string(REGEX MATCHALL "builtins\\[\"([^\"]+)\"\\]" aot_index "${aot_text}")
foreach(m IN LISTS aot_index)
  string(REGEX REPLACE "builtins\\[\"([^\"]+)\"\\]" "\\1" name "${m}")
  list(APPEND aot_builtins "${name}")
endforeach()

list(REMOVE_DUPLICATES aot_builtins)
list(SORT aot_builtins)

if(NOT vm_builtins)
  message(FATAL_ERROR "found no addNative() call in ${vm_source}; the parser is stale")
endif()
if(NOT aot_builtins)
  message(FATAL_ERROR "found no seeded builtin in ${aot_source}; the parser is stale")
endif()

# --- the documented exceptions ---------------------------------------------
# Each name here is a builtin that exists on one side only *on purpose*. The
# value is the side it lives on, so a name that moves engines also fails.
# Every builtin the VM registers is now seeded by the AOT backend, so this list
# is empty: the pointer, file, clock and argument builtins of BACKLOG §6 item 25
# landed in both engines. It stays as the place to record a *new* divergence --
# with its reason -- rather than letting one go unnoticed.
set(vm_only "")
set(aot_only "")

set(problems "")

foreach(name IN LISTS vm_builtins)
  list(FIND aot_builtins "${name}" idx)
  if(idx EQUAL -1)
    list(FIND vm_only "${name}" documented)
    if(documented EQUAL -1)
      list(APPEND problems
        "builtin '${name}' is registered in the VM but never seeded in AOT\n"
        "    seed it in src/frontend/native/lower.cpp, or add it to `vm_only`\n"
        "    here and to BACKLOG §6 item 25 with the reason it cannot be")
    endif()
  endif()
endforeach()

foreach(name IN LISTS aot_builtins)
  list(FIND vm_builtins "${name}" idx)
  if(idx EQUAL -1)
    list(FIND aot_only "${name}" documented)
    if(documented EQUAL -1)
      list(APPEND problems
        "builtin '${name}' is seeded in AOT but not registered in the VM\n"
        "    the two engines disagree about what the language provides")
    endif()
  endif()
endforeach()

# A name listed as an exception that is no longer an exception is stale
# documentation, and stale exceptions hide the next real divergence.
foreach(name IN LISTS vm_only)
  list(FIND vm_builtins "${name}" v)
  list(FIND aot_builtins "${name}" a)
  if(v EQUAL -1)
    list(APPEND problems
      "exception '${name}' is listed as VM-only but the VM does not register it")
  endif()
  if(NOT a EQUAL -1)
    list(APPEND problems
      "exception '${name}' is listed as VM-only but AOT seeds it now\n"
      "    remove it from `vm_only` and from BACKLOG §6 item 25")
  endif()
endforeach()

list(LENGTH vm_builtins vm_count)
list(LENGTH aot_builtins aot_count)
list(LENGTH vm_only vm_only_count)

if(problems)
  string(REPLACE ";" "\n  " pretty "${problems}")
  message(FATAL_ERROR
    "VM/AOT builtin parity is broken (${vm_count} in the VM, ${aot_count} in "
    "AOT, ${vm_only_count} documented VM-only):\n  ${pretty}")
endif()

message(STATUS
  "builtin parity: ${vm_count} in the VM, ${aot_count} in AOT, "
  "${vm_only_count} documented VM-only (BACKLOG §6 item 25)")
