# Skia bridge

`skia.oskelbind` is the descriptor for the Skia subset the widget layer uses.
`skia_facade.h` is the only hand-written C++: the raster-surface factory, the
font/paint text call and the pixel readback. The generated bridge does not model
default arguments, `SkImageInfo`, `SkFont` or `drawString`, so those three
helpers stay in the facade.

`skia_ui.oskl` is the backend for `std/ui`'s display list.

Build it with a Skia root that has headers and a built `libskia`:

```sh
SKIA_DIR=/path/to/skia ./build.sh
```

`build.sh` looks for `out/Release/libskia.a`, `out/Release-x64/libskia.a`, or
the matching `.so`; set `SKIA_LIB` to override. Extra link arguments go in
`SKIA_LIBS`. A prebuilt Skia built with fontconfig/FreeType/GL needs them:

```sh
SKIA_DIR=/path/to/prebuilt SKIA_LIB=$SKIA_DIR/out/Release-x64/libskia.a SKIA_LIBS='-lpthread -ldl -lm -Wl,--no-as-needed -l:libfontconfig.so.1 -l:libfreetype.so.6 -l:libGL.so.1' ./build.sh
```

That writes `build/skia.oskl`, `build/skia_ui.oskl` and
`build/libskia_shim.so`. The modules are ordinary Oskel; `oskel -c` accepts them
without Skia installed. Compiling the shim needs the Skia headers and
`libskia`; the generated thunks use the same C++ ABI as that library.

## Scope

The subset covers `SkSurface`, `SkCanvas`, `SkPaint`, `SkFont`, `SkRect` and
`SkImage`: raster surface creation, clearing, drawing a rectangle/circle,
drawing text, and an image snapshot with pixel readback. `SkCanvas` is borrowed
from its `SkSurface`; `SkSurface` and `SkImage` are refcounted (`unref`).
`SkRect` is a value type allocated once by the bridge so it can cross as an
opaque handle; `SkPaint` and `SkFont` have public constructors.

`skia_ui.oskl` renders the `std.ui` commands (`rect`, `text`). It creates a
`Paint` per command and a `Font` per text command, and closes both.

## Tests

`tests/cxxbind/skia_test.oskl` clears a surface, draws a rectangle and a circle,
and reads pixels back. `tests/cxxbind/skia_ui_test.oskl` builds a small
`std.ui` tree, renders it, and checks the window and button pixels. Both run on
the VM and the hosted AOT binary when CMake is configured with:

```sh
cmake -S . -B build   -DOSKEL_SKIA_DIR=/path/to/skia   -DOSKEL_SKIA_LIB=/path/to/libskia.a   -DOSKEL_SKIA_LIBS='-lpthread -ldl -lm ...'
```

The CTest is `cxxbind.skia`. Without those options it is not registered, so a
checkout without Skia keeps the suite green. Under AddressSanitizer the test
sets `ASAN_OPTIONS=detect_leaks=0` for the VM run: fontconfig keeps process-wide
caches that LeakSanitizer reports as leaks, and the test's job is the pixel
assertions, not third-party allocator bookkeeping.
