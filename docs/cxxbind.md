# C++ bridge (`bindgen --cxx`)

Skia, Qt, most game engines and a large part of the desktop C++ ecosystem do not
ship a C API. `bindgen --cxx` lets Oskel call them without a hand-written C
shim, and without putting a C++ ABI inside the engine:

```text
bridge.oskelbind + library.h
        ↓  bindgen --cxx
bridge.oskl        Oskel module: extern "C" block + classes
bridge_shim.h      C declarations of the thunks
bridge_shim.cpp    C++ thunks, compiled with the library
```

The Oskel side only ever sees the C ABI. The C++ ABI is confined to
`bridge_shim.cpp`, which is compiled with the same C++ compiler, standard
library and flags as the library it bridges.

## Why a descriptor

`bindgen` is a C text transformer; C++ headers use namespaces, classes,
templates, overloads and `sk_sp`, and the C++ ABI differs between Itanium
(Linux/macOS) and MSVC (Windows). A descriptor makes the binding explicit and
lets the C++ compiler check it: if the descriptor disagrees with the real
header, compiling the generated shim fails instead of emitting a wrong call.
The descriptor also records the one thing C++ does not expose for reflection:
ownership (constructor/destructor, handle lifetime).

## Usage

```sh
bindgen --cxx bridge.oskelbind --shim build -o build/bridge.oskl
```

`--shim <dir>` must exist. The tool writes `<stem>_shim.h` and
`<stem>_shim.cpp` there, where `<stem>` is the descriptor's file name without
its extension. The module goes to `-o`, or to stdout when `-o` is omitted.
Exit codes: `64` usage, `65` invalid descriptor, `66` I/O.

Compile the shim and the C++ library together:

```sh
c++ -std=c++17 -shared -fPIC -I include -I build \
    -o build/libbridge_shim.so build/bridge_shim.cpp src/bridge.cpp
```

The development engine resolves the `oskel_*` symbols through
`Platform::lookupSymbol`; preload the shim:

```sh
LD_PRELOAD=$PWD/build/libbridge_shim.so oskel app.oskl
```

For a native binary use the hosted AOT mode, which links against the shim and
therefore against the C++ library:

```sh
LIBRARY_PATH=$PWD/build oskel build app.oskl --mode hosted \
    --link-lib bridge_shim -o app
LD_LIBRARY_PATH=$PWD/build ./app
```

The C++ bridge is hosted-only. A freestanding build has no C++ runtime or
linker and rejects the generated symbols with the existing `extern "C"`
message (exit 65).

## Descriptor format

```text
# comments start with # or //
header "counter.h"          # required; included by the generated shim
prefix "oskel_"             # optional; C symbol prefix (default oskel_)

class Counter {
  cpp "demo::Counter"       # optional; qualified C++ name (default: Counter)
  ctor(start: int)          # a class with a ctor is constructed with `Counter(...)`
  method value(): int const # `const` marks a const C++ method
  method add(n: int): void
  dtor                      # shorthand for `release "delete"`
}

function add(a: int, b: int): int
```

A class with a C++ constructor becomes an Oskel class whose `init` forwards to
that constructor:

```oskel
class Counter {
  handle: ptr;
  owned: bool = true;

  fn init(start: int) {
    self.handle = oskel_Counter_new(start);
  }

  fn valid(): bool { return self.handle != nil; }

  fn close() {
    if (self.handle != nil) {
      if (self.owned) {
        oskel_Counter_delete(self.handle);
      }
      self.handle = nil;
    }
  }

  fn value(): int { return oskel_Counter_value(self.handle); }
  fn add(n: int) { oskel_Counter_add(self.handle, n); }
}
```

Oskel has no destructors, so `close` is explicit; use `defer { c.close(); }`
where the object's lifetime is scoped. `valid()` is false when the constructor
returned `nullptr` (for example after a caught C++ exception).

A free function becomes a thin Oskel wrapper:

```oskel
fn add(a: int, b: int): int {
  return oskel_add(a, b);
}
```

### Factories, class returns and ownership

A class **without** a `ctor` is *adopt-only*: the generated class takes an
existing handle.

```text
class Surface {
  cpp "SkSurface"
  release "unref"           # `delete`, `unref` or `none`
  method getCanvas(): Canvas borrow
  method makeImageSnapshot(): Image sp
}

class Canvas {
  cpp "SkCanvas"
  release "none"            # the SkSurface owns the canvas
  method clear(color: uint32): void
}
```

An adopt-only class gets:

```oskel
class Surface {
  handle: ptr;
  owned: bool;

  fn init(handle: ptr, owned: bool) {
    self.handle = handle;
    self.owned = owned;
  }
  // valid(), close() as above
}
```

`owned` is true for an owned handle and false for a borrowed one. `close()`
only releases when `owned` is true; a `release "none"` class never releases.

A factory is a module-level **object** of functions. The generated module is a
map, so the call reads like a namespace:

```text
object Surfaces {
  function raster(w: int, h: int): Surface sp cpp "skia_bridge::makeRaster"
}

object Rects {
  function make(l: float, t: float, r: float, b: float): Rect value cpp "SkRect::MakeLTRB"
}
```

```oskel
let Surfaces = {
  raster: (w: int, h: int) => Surface(oskel_Surfaces_raster(w, h), true),
};

let Rects = {
  make: (l: float, t: float, r: float, b: float) =>
    Rect(oskel_Rects_make(l, t, r, b), true),
};
```

The return policy says how the C++ result becomes a handle:

| Policy | C++ returns | Shim | Oskel |
|---|---|---|---|
| `owned` (default) | `T *` owned by the caller | returns the pointer | `Class(handle, true)` |
| `borrow` | `T *` or `const T *` owned by someone else | `const_cast` and return | `Class(handle, false)` |
| `sp` | `sk_sp<T>`, `std::unique_ptr<T>` or anything with `release()` | `result.release()` | `Class(handle, true)` |
| `value` | `T` by value | `new T(result)` | `Class(handle, true)` |

A class return needs an **adopt-only target**: the wrapper constructs
`Target(handle, owned)`, and a class with a `ctor` has no such constructor. The
generator rejects it with exit 65 instead of emitting a wrapper that cannot
compile. `value` also requires the target's release policy to be `delete`,
because the shim allocates with `new`.

A class-reference parameter crosses as its handle and the shim dereferences it:

```text
method drawRect(rect: const Rect&, paint: const Paint&): void
```

```cpp
static_cast<Canvas *>(self)->drawRect(
    *static_cast<const Rect *>(rect), *static_cast<const Paint *>(paint));
```

Use `const Class&` for the common `const T&` C++ parameter, `Class&` for a
mutable reference, and `Class`/`const Class` for a pointer.

### Types

| Descriptor | C++ | C ABI | Oskel class/API |
|---|---|---|---|
| `void` | `void` | `void` | `void` |
| `bool` | `bool` | `bool` | `bool` |
| `int`, `int32` | `int`/`int32_t` | `int32_t` | `int` |
| `int64` | `int64_t` | `int64_t` | `int` |
| `uint32` | `uint32_t` | `uint32_t` | `int` |
| `uint64` | `uint64_t` | `uint64_t` | `int` |
| `float` | `float` | `float` | `float` |
| `double` | `double` | `double` | `double` |
| `string` | `const char *` (UTF-8) | `const char *` | `string` |
| `ptr` | `void *` | `void *` | `ptr` |
| `Class` | `Class *` | `void *` | `Class` (passes `.handle`) |
| `const Class` | `const Class *` | `void *` | `Class` |
| `Class&` | `Class &` | `void *` | `Class` (shim dereferences) |
| `const Class&` | `const Class &` | `void *` | `Class` (shim dereferences) |

A class parameter is passed as `other.handle`; the shim casts the opaque
pointer back to the C++ type and, for a reference, dereferences it.

### Release policies

| `release` | Generated thunk | Use |
|---|---|---|
| `delete` (default) | `delete static_cast<T *>(self)` | a class created with `new`, `value` or a `unique_ptr` |
| `unref` | `static_cast<T *>(self)->unref()` | a refcounted class such as `SkSurface` or `SkImage` |
| `none` | none | a borrowed class such as `SkCanvas`, owned by another object |

`dtor` is the short form of `release "delete"`. A class with a `ctor` and
`release "none"` is rejected, because its constructor would leak.

### Current limits

- One constructor per class; a class return target must be adopt-only, so a
  class with a `ctor` cannot be returned from a factory or a method.
- No class or `string` return values; no `std::string` returns.
- No overload sets (the descriptor names one method), no templates, no
  `std::vector`/`std::string` return values, no callbacks, no default arguments
  and no C++ enums yet.
- `string` means `const char *`; a C++ `std::string` parameter accepts it by
  implicit conversion, but a `std::string` return is rejected.
- A C++ value struct (for example `SkRect`) crosses as an opaque handle
  allocated by the bridge, not as a field-by-field Oskel struct. A `value`
  factory creates that handle.
- Exceptions are caught at the shim boundary: a constructor or class return
  returns `nullptr`, a scalar method returns zero, a `void` method does nothing.
  A C++ exception never crosses into Oskel. The generated shim uses
  `try`/`catch`, so compile it with exceptions enabled; a library built with
  `-fno-exceptions` needs a future no-exceptions mode.
- The shim must be compiled with the same C++ ABI and standard library as the
  bridged library. Mixing libstdc++ and libc++, or MSVC and MinGW, is a link
  error at best and an ABI mismatch at worst.

## Skia

`bindings/skia/skia.oskelbind` is the first real descriptor. It covers
`SkSurface`, `SkCanvas`, `SkPaint`, `SkFont`, `SkRect` and `SkImage`: raster
surface creation, clearing, rectangles, circles, text, an image snapshot and a
one-pixel readback. The hand-written C++ is `bindings/skia/skia_facade.h`: the
`SkSurfaces::Raster` factory (the API moved from `SkSurface::MakeRasterN32Premul`
in milestone 115), the `drawString` call (it needs an explicit `SkFont`), and
the pixel readback (it needs an `SkImageInfo`). Everything else is generated.

```sh
cd bindings/skia
SKIA_DIR=/path/to/skia ./build.sh
```

The generated modules are `build/skia.oskl` and `build/skia_ui.oskl`;
`oskel -c` accepts them without Skia installed. Compiling the shim needs Skia's
headers and `libskia`. `skia_ui.oskl` is the backend for `std/ui`'s display
list; `docs/ui.md` describes the widget layer. SDL3 owns the window and events,
Skia owns the pixels.

## Tests

| CTest name | What it proves |
| --- | --- |
| `cxxbind.generate` | The counter, factory and Skia descriptors generate the module, header and shim; `oskel -c` accepts the modules; unknown types and every ownership mistake exit 65. Runs on every platform. |
| `cxxbind.end_to_end` | The counter and factory shims compile with the host `c++`; the VM (via `LD_PRELOAD`) and the hosted AOT binary both print `cxx bridge ok` / `cxx factory ok`. Linux only, like every hosted AOT test. |
| `cxxbind.skia` | Optional: the real Skia shim compiles against a Skia build; `skia_test.oskl` clears, draws and reads pixels, and `skia_ui_test.oskl` renders a `std.ui` tree. Both agree on the VM and the hosted AOT binary. Registered only with `OSKEL_SKIA_DIR`/`OSKEL_SKIA_LIB`; see `bindings/skia/README.md`. |

Fixtures: `tests/cxxbind/counter.*` (constructor/destructor and a free
function), `tests/cxxbind/factory.*` (factory objects, `sp`/`value`/`borrow`/
`owned` returns, `release "none"`/`"unref"`, class-reference parameters) and the
invalid
descriptors `invalid.oskelbind`, `invalid_factory.oskelbind`,
`invalid_release.oskelbind`, `invalid_value.oskelbind` and
`invalid_ref.oskelbind`.
