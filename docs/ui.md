# Declarative UI (`std/ui.oskl`)

`std/ui.oskl` is the first declarative widget layer. It builds a tree of
widgets, measures and places it, and emits a **display list** of rectangles and
text. It does not open a window and does not call a graphics API: SDL3 owns the
window and the events, and Skia consumes the display list.

The widget module is plain Oskel over the portable primitive surface, so the
tree, layout, display list and hit testing behave identically on the development
VM and under `oskel build` (`tests/native_build_ui.oskl`).

```oskel
import std.ui as ui;

let root = ui.window("Demo", 320, 200,
  ui.column(8, [
    ui.text("Hello", 24),
    ui.button("OK", () => print("clicked")),
  ]));

ui.layout(root, 320, 200);

for cmd in ui.commands(root) {
  // cmd.kind is "rect" or "text"; a backend draws it.
}

let button = ui.hitTest(root, 10, 40);
if (button != nil) {
  let onClick = button.onClick;
  onClick();
}
```

## Window and event loop

`std/ui/window.oskl` is the public presentation entry point. It imports the
internal SDL3 and Skia modules, so user code never names them:

```oskel
import std.ui as ui;
import std.ui.window as ui_window;

let root = ui.window("Demo", 320, 200,
  ui.column(8, [ui.text("Hello", 24), ui.button("OK", () => print("clicked"))]));

// maxFrames <= 0 runs until the window closes or Escape is pressed.
ui_window.run("Demo", root, 320, 200, 0);
```

`run` returns the number of frames it presented. It lays the tree out, draws
the display list into a raster Skia surface, copies the whole surface into one
tightly packed BGRA buffer, uploads it to a streaming `SDL_Texture`, draws that
texture and presents the renderer. Mouse motion updates the hovered button, a
left press marks it pressed, and a release on the same button runs `onClick`
and lays the tree out again. Releasing outside the button cancels the click.

On the development engine, load the Skia shim and let SDL3 resolve from the
linked library:

```sh
LD_PRELOAD=bindings/skia/build/libskia_shim.so oskel examples/ui/window.oskl
```

A native binary needs the hosted link mode and both libraries:

```sh
LIBRARY_PATH=bindings/skia/build oskel build examples/ui/window.oskl \
  --mode hosted --link-lib SDL3 --link-lib skia_shim -o ui-window
LD_LIBRARY_PATH=bindings/skia/build ./ui-window
```

| Function | Meaning |
| --- | --- |
| `run(title, root, width, height, maxFrames)` | open a window and run the event loop |
| `click(root, x, y)` | dispatch a press and release through `hitTest`; returns whether a handler ran |

The widget layer also exposes the pointer primitives the window loop uses, so
a headless test can drive the same state:

| Function | Meaning |
| --- | --- |
| `pointerMove(root, x, y)` | mark the button under `(x, y)` hovered; returns whether the state changed |
| `pointerDown(root, x, y)` | mark the button under `(x, y)` pressed; returns whether the state changed |
| `pointerUp(root, x, y)` | release the press; runs the handler only if the release lands on the pressed button |
| `pointerCancel(root)` | clear hover and pressed when the pointer leaves the window or focus is lost |
| `click(root, x, y)` | a complete press and release at one point |

`std.internal.sdl`, `std.internal.skia`, `std.internal.skia_ui` and
`std.internal.skia_window` are implementation details. They are importable, but
they are not part of the public API and are not documented for users.

## Widgets

| Constructor | Meaning |
| --- | --- |
| `window(title, width, height, child)` | top-level frame and background |
| `column(gap, children)` | vertical stack |
| `row(gap, children)` | horizontal stack |
| `padding(all, child)` | inset one child |
| `spacer(height)` | empty vertical space |
| `text(content, size)` | black text |
| `textColor(content, size, color)` | text with an ARGB colour |
| `button(label, onClick)` | blue button with white text and distinct hover/pressed shades |
| `buttonStyle(label, onClick, background, color)` | button with explicit colours; hover and pressed reuse the background |
| `buttonStates(label, onClick, background, hoverBackground, pressedBackground, color)` | button with explicit colours for each pointer state |

A widget is a map with a `kind`, a frame (`x`, `y`, `w`, `h`) and the fields its
kind needs. `layout` mutates the tree and attaches the frame; `commands` reads
the frames and returns new maps, so drawing never mutates the tree. A button
also carries `hover` and `pressed`, which the pointer primitives mutate; its
fill comes from `background`, `hoverBackground` or `pressedBackground` in that
precedence.

## Layout

- A column gives each child the full width and stacks it by its measured height.
- A row gives each child its measured width and the full height.
- Padding insets its child by `all` on every side.
- `layout` measures text as `0.6 * size` per character and `1.4 * size` per
  line, so a headless layout stays deterministic.
- `layoutWith(root, width, height, measure)` takes a renderer-supplied
  `measure(text, size)` that returns the advance width in pixels. The Skia
  window backend passes `skia_ui.measure`, which asks the same font it draws
  with, so layout and drawing share one metric.
- A window uses its own `width`/`height`; any other root uses the viewport passed
  to `layout`/`layoutWith`.

## Display list

`commands(root)` returns an array in paint order:

| `kind` | Fields |
| --- | --- |
| `rect` | `x`, `y`, `w`, `h`, `color` |
| `text` | `text`, `x`, `y`, `size`, `color` |

`y` is the text baseline. The window emits its background rectangle, a button
emits its background rectangle followed by its label, and containers emit their
children.

## Skia backend

`std/internal/skia_ui.oskl` consumes the display list, and `std.ui.window`
drives the surface, the SDL3 texture and the event loop. Public code only names
the widgets and the window entry point:

```oskel
import std.ui as ui;
import std.ui.window as ui_window;

let root = ui.window("Demo", 320, 200,
  ui.column(8, [ui.text("Hello", 24)]));
ui_window.run("Demo", root, 320, 200, 0);
```

Build the Skia bridge first (`bindings/skia/README.md`). The internal backend
creates and closes the `Paint`/`Font` objects it needs; the `Canvas` remains
borrowed from the `Surface`. The bridge module is checked in at
`std/internal/skia.oskl`, so `std.ui.window` type-checks in a clean checkout;
only the C++ shim needs a Skia build.

## Limits

- No text wrapping, alignment, borders, scrolling, input widgets or animation
  yet. `std.ui.window` uses Skia's real advance width through `layoutWith`;
  headless `layout` keeps the deterministic fixed advance.
- `std.ui` itself has no event loop; `std.ui.window` owns it. A button stores
  `onClick`, `hover` and `pressed`; the window loop updates the state from mouse
  events and runs the handler on release over the pressed button.
- The window loop redraws on every frame and sleeps 16 ms; it has no dirty-region
  optimisation, resize handling or keyboard focus yet.
- `std.ui.window` is Linux x86-64 hosted AOT for now, because the native backend
  is hosted-only on that target; the VM path works wherever SDL3 and the Skia
  shim are available.

## Tests

| CTest name | What it proves |
| --- | --- |
| `oskel.native_build_ui` | The widget tree, layout, display list and hit test run on the VM. |
| `oskel.build_binary_ui` | The same fixture builds with `oskel build` and prints exactly the VM's output. |
| `cxxbind.skia` | Optional real-Skia test: the generated bridge renders the display list to a raster surface, checks pixels in the VM and the hosted AOT binary, runs `skia_ui_test.oskl`, and drives `skia_window_test.oskl` through the SDL dummy driver. Registered only when `OSKEL_SKIA_DIR`/`OSKEL_SKIA_LIB` point at a Skia build; the window half also needs SDL3. |
