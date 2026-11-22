#!/bin/sh
# Build the Skia bridge. Set SKIA_DIR to a Skia checkout that has a built
# libskia (out/Release/libskia.a or out/Release/libskia.so).
set -eu

: "${SKIA_DIR:?set SKIA_DIR to a Skia checkout}"
here=$(cd "$(dirname "$0")" && pwd)
out="$here/build"
mkdir -p "$out"

bindgen --cxx "$here/skia.oskelbind" --shim "$out" -o "$out/skia.oskl"

c++ -std=c++17 -shared -fPIC \
  -I"$here" -I"$SKIA_DIR" -I"$out" \
  -o "$out/libskia_shim.so" \
  "$out/skia_shim.cpp" \
  -L"$SKIA_DIR/out/Release" -lskia

echo "generated $out/skia.oskl and $out/libskia_shim.so"
