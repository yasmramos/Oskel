# Skia bridge

`skia.oskelbind` is the descriptor for the Skia subset the widget layer uses.
`skia_facade.h` is the only hand-written C++: one inline factory for
`SkSurfaces::Raster`, because the generated bridge does not model default
arguments or `SkImageInfo` yet.

Build it with a Skia checkout:

```sh
SKIA_DIR=/path/to/skia ./build.sh
```

That writes `build/skia.oskl` (the Oskel module) and `build/libskia_shim.so`
(the C++ thunks linked against Skia). The module is ordinary Oskel; `oskel -c`
accepts it without Skia installed. Compiling the shim needs the Skia headers and
`libskia`; the generated thunks use the same C++ ABI as that library.

## Scope

The subset covers `SkSurface`, `SkCanvas`, `SkPaint`, `SkRect` and `SkImage`:
raster surface creation, clearing, drawing a rectangle/circle, and an image
snapshot. `SkCanvas` is borrowed from its `SkSurface`; `SkSurface` and `SkImage`
are refcounted (`unref`). `SkRect` is a value type allocated once by the bridge
so it can cross as an opaque handle; `SkPaint` has a public constructor.

The declarative widget layer (`Window`, `Column`, `Text`, `Button`, ...) is the
next slice. SDL3 owns the window and events; Skia owns the pixels.
