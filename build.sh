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

# The engine's sources split in three, exactly as CMakeLists.txt splits them.
# The directory a file lives in is its classification, so a new file needs no
# edit here (and no edit to CMake, which globs the same directories):
#
#   core       - Value, the cycle collector, the platform layer. Shared.
#   front end  - lexer, parser, bytecode compiler and the AOT backend. `oskelc`
#                links core + front end, so the shipped compiler carries no
#                interpreter (DEVELOPMENT.md §2, toolchain boundary).
#   runtime    - the VM, the operators, the builtins, the FFI marshalling, the
#                case tables and the JIT. Only `oskel` links these.
FRONTEND_SOURCES=""
for source in src/core/*.cpp src/frontend/*.cpp src/frontend/native/*.cpp; do
    FRONTEND_SOURCES="$FRONTEND_SOURCES $source"
done
RUNTIME_SOURCES=""
for source in src/runtime/*.cpp src/runtime/jit/*.cpp; do
    RUNTIME_SOURCES="$RUNTIME_SOURCES $source"
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
