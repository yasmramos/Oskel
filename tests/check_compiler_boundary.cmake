# Fails when the compiler's front end reaches the interpreter.
#
# `oskelc` links only the front end (core + lexer, parser, bytecode compiler, AOT
# backend) and must never carry the VM, the JIT or the builtins: the product is
# an AOT compiler, and a compiler that can interpret is both a size problem and
# the GraalVM pitfall DEVELOPMENT.md §3 warns about.
#
# The first pass checks the include graph of `src/frontend/compiler.*` and
# `src/frontend/native/*` (the rule that made the split a second `main`). The
# second pass, when `FRONTEND_SOURCES` is provided, checks every source `oskelc`
# actually links, so a front-end file that pulls in `vm.h` through a helper
# header is caught too.
#
# Usage: cmake -DROOT=<repository root> -P tests/check_compiler_boundary.cmake
#        cmake -DROOT=<root> "-DFRONTEND_SOURCES=<a;b;c>" -P ...

if(NOT DEFINED ROOT)
  get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

file(GLOB boundary_files
  "${ROOT}/src/frontend/compiler.cpp"
  "${ROOT}/src/frontend/compiler.h"
  "${ROOT}/include/compiler.h"
  "${ROOT}/src/frontend/native/*.cpp"
  "${ROOT}/src/frontend/native/*.h"
  "${ROOT}/include/native/*.h"
)

# The patterns match the *basename* of the include, not the whole spelling: the
# headers are now qualified by directory (`runtime/vm.h`, `frontend/lexer.h`), so
# an anchored `vm[.]h` would silently stop matching the moment a path gained a
# prefix — the exact failure mode a boundary check exists to prevent.
# `(.*/)?` is an optional directory prefix: it makes `vm.h`, `runtime/vm.h`
# and any future `x/vm.h` all match. It cannot be `(^|/)`, because `^` anchors to
# the start of the whole line and the include pattern has already consumed the
# opening quote, so `^` can never match where the path begins.
# `[.]` keeps the regex readable: no escaping of the dot inside CMake strings.
set(forbidden "(.*/)?(interpreter|vm|jit/jit)[.]h")
# The compiler and the native backend must also stay clear of the FFI *runtime*
# half. `ffi_types.h` is the shared, dependency-free source of truth and is
# allowed; `ffi_marshal.h` pulls in `runtime.h`/`value.h`, and `ffi.cpp` is the
# only place that includes the FFI marshalling. It is a development-engine-only
# component (DEVELOPMENT.md §5.1/§6): the AOT backend lowers callbacks to
# compile-time stubs and never links a host FFI library, so the include must not
# be reachable from the compiler or the native backend.
set(forbidden_ffi "(.*/)?ffi_marshal[.]h|(.*/)?ffi[.]h|libffi")

# What the front end as a whole may not reach: the runtime half of the engine.
# `runtime[.]h` is spelled with its directory on purpose: `include/native/runtime.h`
# is the AOT freestanding runtime, which the backend must and does include, and a
# bare `(.*/)?runtime[.]h` would flag it. The interpreter's header is only ever
# reached as `runtime/runtime.h`.
set(forbidden_frontend
  "(.*/)?(vm|jit/jit|builtins|ops|ffi_marshal)[.]h|(.*/)?runtime/runtime[.]h|libffi")

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
    "(ffi_marshal.h) or a host FFI library (see DEVELOPMENT.md, toolchain boundary).")
endif()

list(LENGTH boundary_files checked)
message(STATUS
  "compiler boundary intact (${checked} file(s) checked, ${frontend_checked} front-end source(s))")
