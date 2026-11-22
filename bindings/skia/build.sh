#!/bin/sh
# Build the Skia bridge. Set SKIA_DIR to a Skia root with headers and a built
# libskia (out/Release/libskia.a or out/Release-x64/libskia.a by default).
set -eu

: "${SKIA_DIR:?set SKIA_DIR to a Skia root}"
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out="$here/build"
mkdir -p "$out" "$root/std/internal"

# The generated module is checked in under std/internal so `std.ui.window`
# resolves in a clean checkout; regenerating it here keeps the descriptor and
# the committed module in step. The C++ shim stays a build artifact.
(cd "$root" && bindgen --cxx bindings/skia/skia.oskelbind --shim "$out" \
  -o std/internal/skia.oskl)

skia_lib=${SKIA_LIB:-}
if [ -z "$skia_lib" ]; then
  for candidate in \
    "$SKIA_DIR/out/Release/libskia.a" \
    "$SKIA_DIR/out/Release-x64/libskia.a" \
    "$SKIA_DIR/out/Release/libskia.so" \
    "$SKIA_DIR/out/Release-x64/libskia.so"; do
    if [ -f "$candidate" ]; then
      skia_lib=$candidate
      break
    fi
  done
fi
if [ -z "$skia_lib" ]; then
  echo "build.sh: no libskia under $SKIA_DIR/out; set SKIA_LIB" >&2
  exit 1
fi

skia_libs=${SKIA_LIBS:--lpthread -ldl -lm}
# shellcheck disable=SC2086
c++ -std=c++17 -shared -fPIC \
  -I"$here" -I"$SKIA_DIR" -I"$out" \
  -o "$out/libskia_shim.so" \
  "$out/skia_shim.cpp" "$skia_lib" $skia_libs

echo "generated $root/std/internal/skia.oskl and $out/libskia_shim.so"
