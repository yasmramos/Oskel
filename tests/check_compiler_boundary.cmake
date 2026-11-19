# Fails when the compiler's front end reaches the interpreter.
#
# `oskelc` links only the front end (lexer, parser, bytecode compiler, AOT
# backend) and must never carry the VM, the JIT or the builtins: the product is
# an AOT compiler, and a compiler that can interpret is both a size problem and
# the GraalVM pitfall DEVELOPMENT.md §3 warns about.
#
# The first pass checks the include graph of `src/compiler.*` and `src/native/*`
# (the rule that made the split a second `main`). The second pass, when
# `FRONTEND_SOURCES` is provided, checks every source `oskelc` actually links, so
# a front-end file that pulls in `vm.h` through a helper header is caught too.
#
# Usage: cmake -DROOT=<repository root> -P tests/check_compiler_boundary.cmake
#        cmake -DROOT=<root> "-DFRONTEND_SOURCES=<a;b;c>" -P ...

if(NOT DEFINED ROOT)
  get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

file(GLOB boundary_files
  "${ROOT}/src/compiler.cpp"
  "${ROOT}/src/compiler.h"
  "${ROOT}/include/compiler.h"
  "${ROOT}/src/native/*.cpp"
  "${ROOT}/src/native/*.h"
  "${ROOT}/include/native/*.h"
)

# `[.]` keeps the regex readable: no escaping of the dot inside CMake strings.
set(forbidden "interpreter[.]h|vm[.]h|jit/jit[.]h")
# The compiler and the native backend must also stay clear of the FFI *runtime*
# half. `ffi_types.h` is the shared, dependency-free source of truth and is
# allowed; `ffi_marshal.h` pulls in `runtime.h`/`value.h`, and `ffi.cpp` is the
# only place that includes libffi. libffi is a development-engine-only
# dependency (DEVELOPMENT.md §5.1/§6): the AOT backend lowers callbacks to compile-time
# stubs and must never link it, so the include must not be reachable from the
# compiler or the native backend.
set(forbidden_ffi "ffi_marshal[.]h|ffi[.]h|libffi")

# What the front end as a whole may not reach: the runtime half of the engine.
set(forbidden_frontend "vm[.]h|jit/jit[.]h|builtins[.]h|ops[.]h|ffi_marshal[.]h|libffi")

set(violations "")
foreach(file IN LISTS boundary_files)
  file(STRINGS "${file}" source_lines)
  foreach(line IN LISTS source_lines)
    # A commented-out include documents the rule, it does not violate it.
    string(STRIP "${line}" candidate)
    if(candidate MATCHES "^//" OR candidate MATCHES "^/[*]" OR candidate MATCHES "^[*]")
      continue()
    endif()
    if(line MATCHES "#[ \t]*include[ \t]*[<\"](${forbidden})[>\"]" OR
       line MATCHES "#[ \t]*include[ \t]*[<\"](${forbidden_ffi})[>\"]")
      file(RELATIVE_PATH shown "${ROOT}" "${file}")
      string(STRIP "${line}" shown_line)
      list(APPEND violations "${shown}: ${shown_line}")
    endif()
  endforeach()
endforeach()

set(frontend_checked 0)
if(DEFINED FRONTEND_SOURCES AND FRONTEND_SOURCES)
  foreach(file IN LISTS FRONTEND_SOURCES)
    if(NOT EXISTS "${file}")
      continue()
    endif()
    math(EXPR frontend_checked "${frontend_checked} + 1")
    file(STRINGS "${file}" source_lines)
    foreach(line IN LISTS source_lines)
      string(STRIP "${line}" candidate)
      if(candidate MATCHES "^//" OR candidate MATCHES "^/[*]" OR candidate MATCHES "^[*]")
        continue()
      endif()
      if(line MATCHES "#[ \t]*include[ \t]*[<\"](${forbidden_frontend})[>\"]")
        file(RELATIVE_PATH shown "${ROOT}" "${file}")
        string(STRIP "${line}" shown_line)
        list(APPEND violations "${shown}: ${shown_line}")
      endif()
    endforeach()
  endforeach()
endif()

if(violations)
  list(LENGTH violations count)
  foreach(violation IN LISTS violations)
    message(SEND_ERROR "forbidden include: ${violation}")
  endforeach()
  message(FATAL_ERROR
    "${count} forbidden include(s): the compiler, the native backend and every "
    "source `oskelc` links must not include vm.h, the JIT (jit/jit.h), the "
    "builtins (builtins.h), the operator library (ops.h), the FFI runtime "
    "(ffi_marshal.h) or libffi (see DEVELOPMENT.md, toolchain boundary).")
endif()

list(LENGTH boundary_files checked)
message(STATUS
  "compiler boundary intact (${checked} file(s) checked, ${frontend_checked} front-end source(s))")
