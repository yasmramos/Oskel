# Fails when `oskelc`'s front end carries an interpreter.
#
# `oskelc` is the host compiler: sources in, a native image out. It links the
# front end alone (lexer, parser, bytecode compiler, AOT backend), so neither its
# symbol table nor the archive it links from may contain the VM, the JIT or the
# builtin registry.
#
# The binary alone is not enough. A static library is linked by *pulling the
# objects a symbol reference needs*, so a runtime object that sits in
# `liboskel_frontend.a` but is never referenced never reaches the executable —
# `nm oskelc` stays clean while the object is in the compiler's input. The check
# therefore runs on the archive as well, which is the only place that sees it.
#
# Usage: cmake -DOSKELC_EXE=<path to oskelc> -DFRONTEND_ARCHIVE=<liboskel_frontend.a> \
#              -P tests/check_oskelc_no_vm.cmake

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

function(oskel_check_symbols target)
  execute_process(
    COMMAND "${OSKEL_NM}" -C "${target}"
    RESULT_VARIABLE nm_exit
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE nm_error
  )
  if(NOT nm_exit EQUAL 0)
    message(FATAL_ERROR "'${OSKEL_NM}' failed on '${target}': ${nm_error}")
  endif()

  set(found "")
  foreach(symbol IN LISTS forbidden_symbols)
    if(symbols MATCHES "(^|\\n)[^\\n]*${symbol}")
      string(APPEND found "  ${symbol}\n")
    endif()
  endforeach()

  if(found)
    get_filename_component(shown "${target}" NAME)
    message(FATAL_ERROR
      "'${shown}' carries the interpreter; the front end must not:\n${found}")
  endif()
  message(STATUS "'${target}' carries no interpreter")
endfunction()

oskel_check_symbols("${OSKELC_EXE}")

if(DEFINED FRONTEND_ARCHIVE AND FRONTEND_ARCHIVE)
  if(NOT EXISTS "${FRONTEND_ARCHIVE}")
    message(FATAL_ERROR "'${FRONTEND_ARCHIVE}' does not exist")
  endif()
  oskel_check_symbols("${FRONTEND_ARCHIVE}")
endif()
