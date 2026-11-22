# SDL3 bindings (`std/sdl.oskl`)

`std/sdl.oskl` binds [SDL3](https://wiki.libsdl.org/SDL3/)'s window, renderer and
event surface, so an Oskel program can open a window and draw into it. It is a
low-level module: it exposes the `SDL_*` functions and a few helpers. The
widget layer is `std/ui.oskl` (`docs/ui.md`); it emits a display list that Skia
renders through the C++ bridge (`bindings/skia/skia_ui.oskl`,
`docs/cxxbind.md`). SDL3 owns the window and the events.

```oskel
import std.sdl;

fn main() {
  if (!std.sdl.initVideo()) {
    panic("SDL_Init: " + std.sdl.lastError());
  }
  defer { std.sdl.quit(); }

  let window = std.sdl.SDL_CreateWindow("Hello", 640, 480, 0);
  let renderer = std.sdl.SDL_CreateRenderer(window, nil);
  let rect = std.sdl.fRectBuffer();
  std.sdl.setFRect(rect, 40, 80, 120, 90);

  std.sdl.setColor(renderer, 220, 80, 60, 255);
  std.sdl.clear(renderer);
  std.sdl.fillRect(renderer, rect);
  std.sdl.present(renderer);

  std.sdl.delayMs(1000);
  std.sdl.SDL_DestroyRenderer(renderer);
  std.sdl.SDL_DestroyWindow(window);
}

main();
```

The complete example, with an event loop and an animation, is
`examples/sdl/window.oskl`.

## Requirements

- The SDL3 development package (`libsdl3-dev` on Debian/Ubuntu,
  `brew install sdl3` on macOS).
- CMake finds SDL3 (`find_package(SDL3)`) and links the development engine
  against `SDL3::SDL3`. `build.sh` probes `pkg-config --exists sdl3`. If the
  library is absent, the engine still builds; only the `std.sdl` runtime tests
  are skipped (`OSKEL_ENABLE_SDL3=OFF` silences the status line).
- The module itself is always installed; `import std.sdl` type-checks on every
  platform, with or without SDL3.

The development engine resolves `SDL_*` through `Platform::lookupSymbol`, which
on Linux is `dlsym(RTLD_DEFAULT)`. That only sees a shared object the process
actually loaded, and the linker's `--as-needed` would drop a library nothing
references at link time, so the build forces it in (`-Wl,--no-as-needed`). A
hand-written build line needs the same flag.

## Running

On the development engine:

```sh
oskel examples/sdl/window.oskl
```

Without a display (CI, a container, a headless server), SDL3's `dummy` driver
creates an off-screen window and a software renderer:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy oskel examples/sdl/window.oskl
```

## Native binaries (AOT)

A native binary needs the hosted link mode, because SDL3 is a host library:

```sh
oskel build examples/sdl/window.oskl --mode hosted --link-lib SDL3 -o window
./window
```

`--link-lib SDL3` becomes `-lSDL3` in the host linker command. It is repeatable,
it requires `--mode hosted` (the freestanding image has no linker), and the name
is restricted to letters, digits, `_`, `+`, `-` and `.`. See
`docs/language.md` for the flag and `docs/BACKLOG.md` §7 for the FFI limits.
The hosted link is available on Linux, Windows and macOS x86-64. The SDL3
module is exercised on Linux; a target whose FFI surface the module needs (a
struct by value or a callback) is rejected with a dedicated message before an
image is written.

A freestanding build of a program that uses SDL3 fails with the dedicated
message and exit 65, never with a mis-compiled binary:

```
lowering: extern "C" symbol 'SDL_Init' cannot be resolved by the AOT backend (PHASE 7f)
```

## What is bound

`bindings/sdl/sdl3_subset.h` is the curated C surface; `bindgen` translates it
into the `extern "C"` block at the top of `std/sdl.oskl`. The functions are
SDL3's exact spellings, so a reader of the SDL3 documentation can call them
directly:

- Initialization: `SDL_Init`, `SDL_Quit`, `SDL_GetError`.
- Window: `SDL_CreateWindow`, `SDL_DestroyWindow`, `SDL_SetWindowTitle`,
  `SDL_SetWindowPosition`, `SDL_GetWindowSize`.
- Renderer: `SDL_CreateRenderer`, `SDL_DestroyRenderer`, `SDL_SetRenderVSync`,
  `SDL_SetRenderDrawColor`, `SDL_RenderClear`, `SDL_RenderPoint`,
  `SDL_RenderLine`, `SDL_RenderRect`, `SDL_RenderFillRect`,
  `SDL_RenderPresent`.
- Events: `SDL_PollEvent`, `SDL_PushEvent`.
- Time: `SDL_GetTicks`, `SDL_Delay`.

SDL3 differs from SDL2 in ways the module reflects: `SDL_CreateWindow` takes no
x/y (use `SDL_SetWindowPosition`), `SDL_CreateRenderer` takes only the window
and a driver name (`nil` for the default), the predicates return `bool`, the
window flags are `Uint64`, and the renderer's rectangle is `SDL_FRect` (floats)
rather than `SDL_Rect` (ints).

The hand-written helpers keep the call sites short:

| Helper | Purpose |
| --- | --- |
| `initVideo()` / `quit()` | `SDL_Init(SDL_INIT_VIDEO)` / `SDL_Quit()`. |
| `lastError()` | The last `SDL_GetError()` as an Oskel `string`. |
| `ticks()` / `delayMs(ms)` | Epoch milliseconds / sleep. |
| `eventBuffer()` | A reusable 128-byte `SDL_Event` buffer. |
| `fRectBuffer()` | A reusable 16-byte `SDL_FRect` buffer. |
| `setFRect(rect, x, y, w, h)` | Writes the four floats (`int` or `double` accepted). |
| `setColor(renderer, r, g, b, a)` | `SDL_SetRenderDrawColor`. |
| `clear`, `fillRect`, `drawRect`, `drawLine`, `drawPoint`, `present` | The matching renderer calls. |
| `eventType`, `keyCode`, `keyScancode`, `mouseX`, `mouseY`, `mouseButton`, `mouseButtonDown` | Read an event. |

The constants mirror SDL3: `SDL_INIT_VIDEO`, the `SDL_WINDOW_*` flags, the
`SDL_EVENT_*` types used by the helpers, and `SDLK_ESCAPE`/`SDLK_RETURN`/
`SDLK_SPACE`. Add the ones a program needs to the module; the generated block
does not carry C enum values.

## Events

`SDL_Event` is a C union, so it is **not** modelled as an Oskel `struct`: the
module keeps it as an opaque `ptr` and reads fixed offsets out of a 128-byte
buffer. The layout is the same on every target because SDL's types are
fixed-width:

| Field | Offset | Type |
| --- | --- | --- |
| `type` | 0 | `Uint32` |
| `key.scancode` | 24 | `Sint32` |
| `key.key` | 28 | `Sint32` |
| `motion.x` | 28 | `float` |
| `motion.y` | 32 | `float` |
| `button.button` | 24 | `Uint8` |
| `button.down` | 25 | `bool` |
| `button.x` | 28 | `float` |
| `button.y` | 32 | `float` |

`tests/sdl_bindings.oskl` pins these offsets by pushing a synthetic event of
each kind and reading it back, so an SDL3 update that changed the layout fails
the suite instead of silently returning garbage.

A frame loop allocates the event buffer and the rectangle **once** and reuses
them. The AOT arena only grows, so a buffer per frame would leak the previous
one until the program exits.

## Floats

`SDL_FRect` and the event coordinates are binary32. Oskel's `float` is binary32,
but the FFI marshals a `float` value into an SSE register; writing one into a
byte buffer is a different operation. `storeF32`/`loadF32` implement IEEE 754 in
Oskel itself (normal numbers, which is what coordinates are), so the VM and the
AOT backend share one implementation and no builtin was added for SDL. The test
asserts the exact bit patterns (`10.0f` is `0x41200000`, `-1.0f` is
`0xBF800000`, ...), not only a round-trip. Infinity and NaN are encoded
explicitly, so a malformed coordinate cannot spin the normalization loop.

## Limits

- **Eight register-sized values per `extern "C"` call.** `SDL_ReadSurfacePixel`
  and `SDL_WriteSurfacePixel` have seven arguments; the seventh GPR spills to
  the outgoing stack area in the AOT backend, and the VM's typed call does the
  same. `tests/sdl_bindings.oskl` writes and reads a 2x2 surface pixel to pin
  it. A ninth value would need a wider `kMaxExternArgs` (`docs/BACKLOG.md` §7
  item 17).
- **Windows execution searches loaded DLLs.** The development engine's
  `lookupSymbol` walks every module the process has loaded, so a `std.sdl`
  program linked against `SDL3.dll` resolves `SDL_*` there too; the
  Windows-only `platform_lookup_test` covers the walk. The module compiles
  everywhere.
- **`SDL_Event`'s size is 128 bytes in SDL3 3.x.** The module reads the fields
  above; it does not need the rest of the union.

## Tests

| CTest name | What it proves |
| --- | --- |
| `sdl.bindgen` | The module's `extern "C"` block is exactly what `bindgen` emits from `bindings/sdl/sdl3_subset.h` (a header edit that was not regenerated fails here). |
| `sdl.bindings` | The VM opens a window and a software renderer under `SDL_VIDEODRIVER=dummy`, draws, pushes/polls every event kind, reads the `SDL_FRect` bits, and writes/reads a surface pixel (the seven-argument `SDL_ReadSurfacePixel`/`SDL_WriteSurfacePixel`, the AOT stack spill). |
| `oskel.build_hosted_sdl` | The same script compiled with `--mode hosted --link-lib SDL3` prints byte-for-byte the VM's output, and `ldd` shows `libSDL3`. |
| `oskel.check.sdl_import` | `oskel -c` type-checks the module on every platform, without SDL3. |
| `oskel.build_sdl_freestanding_error` | A freestanding build rejects a used `SDL_*` symbol with the dedicated message, exit 65, and writes no binary. |
| `oskel.build_link_lib_usage_error` | `--link-lib` without `--mode hosted`, and an invalid library name, are usage errors (exit 64). |

## Regenerating the generated block

```sh
./build/bindgen bindings/sdl/sdl3_subset.h
```

Replace the `extern "C" { ... }` block in `std/sdl.oskl` with the output. The
helpers below the `// --- Hand-written helpers` marker are not generated and are
not touched. When a new function is needed, add its C prototype to
`bindings/sdl/sdl3_subset.h` first, then regenerate; that keeps the module's
signatures tied to the C declarations instead of hand-written guesses.
