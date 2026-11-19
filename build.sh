#!/usr/bin/env sh
# Builds the development engine into build/oskel, the host compiler into
# build/oskelc and the binding generator into build/bindgen.
# Usage: ./build.sh
#   CXX=clang++ ./build.sh
#   CXXFLAGS="-O0 -g" ./build.sh

set -eu

cd "$(dirname "$0")"

if [ -z "${CXX:-}" ]; then
    if command -v g++ >/dev/null 2>&1; then
        CXX=g++
    elif command -v clang++ >/dev/null 2>&1; then
        CXX=clang++
    else
        echo "error: no C++ compiler found; set CXX or install g++/clang++" >&2
        exit 1
    fi
fi

CXXFLAGS=${CXXFLAGS:--O2}

mkdir -p build

# The engine's sources split in two, exactly as CMakeLists.txt splits them:
#
#   front end  - lexer, parser, bytecode compiler and the AOT backend. `oskelc`
#                links only these, so the shipped compiler carries no
#                interpreter (DEVELOPMENT.md §2, toolchain boundary).
#   runtime    - the VM, the operators, the builtins, the FFI marshalling, the
#                case tables and the JIT. Only `oskel` links these.
#
# Every other .cpp under src/ belongs to the front end, so a new file needs no
# edit here (CMake uses GLOB_RECURSE for the same reason).
RUNTIME_SOURCES="src/vm.cpp src/ops.cpp src/builtins.cpp src/ffi.cpp src/unicode_case.cpp src/jit/jit.cpp"
FRONTEND_SOURCES=""
for source in src/*.cpp src/native/*.cpp; do
    case "$source" in
        src/main.cpp|src/oskelc.cpp|src/vm.cpp|src/ops.cpp|src/builtins.cpp|src/ffi.cpp|src/unicode_case.cpp|src/jit/jit.cpp)
            continue
            ;;
    esac
    FRONTEND_SOURCES="$FRONTEND_SOURCES $source"
done

# shellcheck disable=SC2086
"$CXX" -std=c++17 $CXXFLAGS -Wall -Wextra -Iinclude -Isrc -o build/oskel \
    src/main.cpp $FRONTEND_SOURCES $RUNTIME_SOURCES
echo "Built build/oskel"

# The host compiler: the same front end, no interpreter.
# shellcheck disable=SC2086
"$CXX" -std=c++17 $CXXFLAGS -Wall -Wextra -Iinclude -Isrc -o build/oskelc \
    src/oskelc.cpp $FRONTEND_SOURCES
echo "Built build/oskelc"

# The binding generator is a standalone executable (it does not link the
# engine): `tools/bindgen.cpp` plus its CLI in `tools/bindgen_main.cpp`.
# shellcheck disable=SC2086
"$CXX" -std=c++17 $CXXFLAGS -Wall -Wextra -Iinclude -Isrc -o build/bindgen tools/bindgen.cpp tools/bindgen_main.cpp
echo "Built build/bindgen"
