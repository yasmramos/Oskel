# Fails when `oskelc` carries an interpreter.
#
# `oskelc` is the host compiler: sources in, a native image out. It links the
# front end alone (lexer, parser, bytecode compiler, AOT backend), so its symbol
# table must not contain the VM, the JIT or the builtin registry. The include
# check (`tests/check_compiler_boundary.cmake`) proves the source graph is clean;
# this proves the *link* is, which also catches a source that reaches the VM
# through a header without naming `vm.h`.
#
# Usage: cmake -DOSKELC_EXE=<path to oskelc> -P tests/check_oskelc_no_vm.cmake

if(NOT DEFINED OSKELC_EXE)
  message(FATAL_ERROR "check_oskelc_no_vm.cmake requires OSKELC_EXE")
endif()
if(NOT EXISTS "${OSKELC_EXE}")
  message(FATAL_ERROR "'${OSKELC_EXE}' does not exist")
endif()

# `nm` and `grep` are POSIX tools; the Windows runner uses `dumpbin`, which the
# CI does not require. Skip there instead of failing for a missing tool.
find_program(OSKEL_NM NAMES nm llvm-nm)
if(NOT OSKEL_NM)
  message(STATUS "no nm found: skipping the oskelc symbol check")
  return()
endif()

execute_process(
  COMMAND "${OSKEL_NM}" -C "${OSKELC_EXE}"
  RESULT_VARIABLE nm_exit
  OUTPUT_VARIABLE symbols
  ERROR_VARIABLE nm_error
)
if(NOT nm_exit EQUAL 0)
  message(FATAL_ERROR "'${OSKEL_NM}' failed on '${OSKELC_EXE}': ${nm_error}")
endif()

# The runtime types and entry points that only the development engine may carry.
# `jitCompile` and `Jit::` are the two shapes the JIT's symbols take; the
# mangled names do not contain a `jit::` namespace.
set(forbidden_symbols
  "VM::"
  "jitCompile"
  "Jit::"
  "registerBuiltins"
  "ops::binary"
  "Runtime::fail"
)

set(found "")
foreach(symbol IN LISTS forbidden_symbols)
  if(symbols MATCHES "(^|\\n)[^\\n]*${symbol}")
    string(APPEND found "  ${symbol}\n")
  endif()
endforeach()

if(found)
  message(FATAL_ERROR
    "oskelc links the interpreter; it must carry only the front end:\n${found}")
endif()

message(STATUS "oskelc carries no interpreter (${forbidden_symbols} absent)")
