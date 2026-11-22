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
  ctor(start: int)          # required for this slice
  method value(): int const # `const` marks a const C++ method
  method add(n: int): void
  dtor                      # required for this slice
}

function add(a: int, b: int): int
```

A class becomes an Oskel class:

```oskel
class Counter {
  handle: ptr;

  fn init(start: int) {
    self.handle = oskel_Counter_new(start);
  }

  fn valid(): bool { return self.handle != nil; }

  fn close() {
    if (self.handle != nil) {
      oskel_Counter_delete(self.handle);
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

A class parameter is passed as `other.handle`; the shim casts the opaque
pointer back to the C++ type.

### Current limits

- One constructor and one destructor per class; no class or `string` return
  values; no static factories yet.
- No overload sets (the descriptor names one method), no templates, no
  `std::vector`/`std::string` return values, no callbacks.
- `string` means `const char *`; a C++ `std::string` parameter accepts it by
  implicit conversion, but a `std::string` return is rejected.
- Exceptions are caught at the shim boundary: a constructor returns `nullptr`,
  a scalar method returns zero, a `void` method does nothing. A C++ exception
  never crosses into Oskel. The generated shim uses `try`/`catch`, so compile
  it with exceptions enabled; a library built with `-fno-exceptions` needs a
  future no-exceptions mode.
- The shim must be compiled with the same C++ ABI and standard library as the
  bridged library. Mixing libstdc++ and libc++, or MSVC and MinGW, is a link
  error at best and an ABI mismatch at worst.

## Tests

| CTest name | What it proves |
| --- | --- |
| `cxxbind.generate` | The descriptor generates the module, header and shim; `oskel -c` accepts the module; an unknown type exits 65. Runs on every platform. |
| `cxxbind.end_to_end` | The generated shim compiles with the host `c++`; the VM (via `LD_PRELOAD`) and the hosted AOT binary both print `cxx bridge ok`. Linux only, like every hosted AOT test. |

Fixtures: `tests/cxxbind/counter.h`, `counter.cpp`, `counter.oskelbind`,
`counter_test.oskl` and `invalid.oskelbind`.
