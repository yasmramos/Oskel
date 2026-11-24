# Skia bridge

`skia.oskelbind` is the descriptor for the Skia subset the widget layer uses.
`skia_facade.h` is the only hand-written C++: the raster-surface factory, the
font/paint text call and the pixel readback. The generated bridge does not model
default arguments, `SkImageInfo`, `SkFont` or `drawString`, so those three
helpers stay in the facade.

`std/internal/skia_ui.oskl` is the backend for `std/ui`'s display list, and
`std/internal/skia_window.oskl` presents it through SDL3; `std.ui.window` is
the public entry point.

The release SDK ships the bridge; a source build gets it from vcpkg:

```sh
python3 tools/prepare_ui_deps.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

That links the static `unofficial::skia::skia` target into `build/ui`, so the
shipped shim needs no separate `libskia`. A hand-built Skia root still works:

```sh
SKIA_DIR=/path/to/skia ./build.sh
```

`build.sh` looks for `out/Release/libskia.a`, `out/Release-x64/libskia.a`, or
the matching `.so`; set `SKIA_LIB` to override. Extra link arguments go in
`SKIA_LIBS`. A prebuilt Skia built with fontconfig/FreeType/GL needs them:

```sh
SKIA_DIR=/path/to/prebuilt SKIA_LIB=$SKIA_DIR/out/Release-x64/libskia.a SKIA_LIBS='-lpthread -ldl -lm -Wl,--no-as-needed -l:libfontconfig.so.1 -l:libfreetype.so.6 -l:libGL.so.1' ./build.sh
```

That writes `std/internal/skia.oskl` and `build/libskia_shim.so`. The module is
ordinary Oskel and is checked in, so `oskel -c` accepts it without Skia
installed. Compiling the shim needs the Skia headers and `libskia`; the
generated thunks use the same C++ ABI as that library.

## Scope

The subset covers `SkSurface`, `SkCanvas`, `SkPaint`, `SkFont`, `SkRect` and
`SkImage`: raster surface creation, clearing, drawing a rectangle/circle,
drawing text, measuring a text advance, and an image snapshot with pixel
readback. `SkCanvas` is borrowed
from its `SkSurface`; `SkSurface` and `SkImage` are refcounted (`unref`).
`SkRect` is a value type allocated once by the bridge so it can cross as an
opaque handle; `SkPaint` and `SkFont` have public constructors.

`std/internal/skia_ui.oskl` renders the `std.ui` commands (`rect`, `text`) and
exposes `measure`, the same `SkFont::measureText` advance the renderer uses. It
creates a `Paint` per command and a `Font` per text command, and closes both.
`std/internal/skia_window.oskl` owns the SDL window, the streaming texture and
the raster surface, and passes `skia_ui.measure` to `ui.layoutWith` so layout
and drawing share one font metric; `std.ui.window` is its public wrapper.

## Tests

`tests/cxxbind/skia_test.oskl` clears a surface, draws a rectangle and a circle,
and reads pixels back in bulk. `tests/cxxbind/skia_ui_test.oskl` builds a small
`std.ui` tree, renders it, and checks the window and button pixels.
`tests/cxxbind/skia_window_test.oskl` drives the full `std.ui.window` loop
through SDL3's dummy video driver. All three run on the VM and the hosted AOT
binary when CMake finds Skia, either from the bundled vcpkg target or from a
hand-built root:

```sh
cmake -S . -B build -DOSKEL_SKIA_DIR=/path/to/skia -DOSKEL_SKIA_LIB=/path/to/libskia.a -DOSKEL_SKIA_LIBS='-lpthread -ldl -lm ...'
```

The CTest is `cxxbind.skia`. Without Skia it is not registered, so a checkout
without the backend keeps the suite green. The installed-layout test
`install_stdlib` also runs the real shim from `lib/oskel/ui`, which is what
proves the SDK needs no user-installed Skia. Under AddressSanitizer the test
sets `ASAN_OPTIONS=detect_leaks=0` for the VM run: fontconfig keeps process-wide
caches that LeakSanitizer reports as leaks, and the test's job is the pixel
assertions, not third-party allocator bookkeeping.
