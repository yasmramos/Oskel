# Declarative UI (`std/ui.oskl`)

`std/ui.oskl` is the first declarative widget layer. It builds a tree of
widgets, measures and places it, and emits a **display list** of rectangles and
text. It does not open a window and does not call a graphics API: SDL3 owns the
window and the events, and Skia consumes the display list.

The release SDK bundles both native libraries under `lib/oskel/ui`, next to the
engine. Importing `std.ui.window` is enough: the engine finds SDL3 and the Skia
shim there, with no package to install and no environment variable to set.

The widget module is plain Oskel over the portable primitive surface, so the
tree, layout, display list and hit testing behave identically on the development
VM and under `oskel build` (`tests/native_build_ui.oskl`).

```oskel
import std.ui as ui;

let root = ui.Window("Demo", 320, 200,
  ui.Column(8, [
    ui.Text("Hello", 24),
    ui.Button("OK", () => print("clicked")),
    ui.Checkbox("Accept", false, (checked) => print(checked)),
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

let root = ui.Window("Demo", 320, 200,
  ui.Column(8, [ui.Text("Hello", 24), ui.Button("OK", () => print("clicked"))]));

// maxFrames <= 0 runs until the window closes or Escape is pressed.
ui_window.run("Demo", root, 320, 200, 0);
```

`run` returns the number of frames it presented. It lays the tree out, draws
the display list into a raster Skia surface, copies the whole surface into one
tightly packed BGRA buffer, uploads it to a streaming `SDL_Texture`, draws that
texture and presents the renderer. Mouse motion updates the hovered widget, a
left press marks it pressed, and a release on the same widget runs a button's
`onClick` or toggles a checkbox's `checked` and runs its `onChange`. Releasing
outside the widget cancels the click.

On an installed SDK the engine loads the bundled libraries itself:

```sh
oskel examples/ui/window.oskl
```

A native binary is just as direct. The compiler sees the `std.ui.window` import,
selects the hosted link mode, and adds the bundled SDL3 and Skia shim:

```sh
oskel build examples/ui/window.oskl -o ui-window
./ui-window
```

A source checkout that has not built the bundle can point at a directory with
the same files through `OSKEL_UI_LIBDIR`; `--link-lib` remains available for a
custom pair of libraries.

| Function | Meaning |
| --- | --- |
| `run(title, root, width, height, maxFrames)` | open a window and run the event loop |
| `click(root, x, y)` | dispatch a press and release through `hitTest`; returns whether a handler ran or a checkbox changed state |

The widget layer also exposes the pointer primitives the window loop uses, so
a headless test can drive the same state:

| Function | Meaning |
| --- | --- |
| `pointerMove(root, x, y)` | mark the interactive widget under `(x, y)` hovered; returns whether the state changed |
| `pointerDown(root, x, y)` | mark the interactive widget under `(x, y)` pressed; returns whether the state changed |
| `pointerUp(root, x, y)` | release the press; runs the handler or toggles the checkbox only if the release lands on the pressed widget |
| `pointerCancel(root)` | clear hover and pressed when the pointer leaves the window or focus is lost |
| `click(root, x, y)` | a complete press and release at one point |

`std.internal.sdl`, `std.internal.skia`, `std.internal.skia_ui` and
`std.internal.skia_window` are implementation details. They are importable, but
they are not part of the public API and are not documented for users.

## Widgets

| Class | Meaning |
| --- | --- |
| `Window(title, width, height, child)` | top-level frame and background |
| `Column(gap, children)` | vertical stack |
| `Row(gap, children)` | horizontal stack |
| `Padding(all, child)` | inset one child |
| `Spacer(height)` | empty vertical space |
| `Text(content, size)` | black text; `Text(...).withColor(color)` sets an ARGB colour |
| `Button(label, onClick)` | blue button with white text and distinct hover/pressed shades |
| `Button(...).withStyle(background, color)` | explicit colours; hover and pressed reuse the background |
| `Button(...).withStates(background, hoverBackground, pressedBackground, color)` | explicit colours for each pointer state |
| `Checkbox(label, checked, onChange)` | 16 px box plus label; a click toggles `checked` and calls `onChange(checked)` |

A widget is an instance of a class implementing `Widget`. Each class owns its
`measure`, `place`, `collect`, `hitTest` and pointer-state methods; `layout`
and `commands` are thin entry points. `type(node)` reports the class name, so a
`Checkbox` is a `Checkbox`, not a map with a `kind`. Oskel erases classes to
maps with a method table, so the VM and `oskel build` run the same code.

`layout` mutates the tree and attaches the frame (`x`, `y`, `w`, `h`);
`commands` reads the frames and returns new maps, so drawing never mutates the
tree. A `Button` and a `Checkbox` carry `hover` and `pressed`, which the pointer
primitives mutate; their fill comes from `background`, `hoverBackground` or
`pressedBackground` in that precedence. A checked checkbox paints a filled
square inside the box.

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
emits its background rectangle followed by its label, a checkbox emits its box
(and an inner square when checked) followed by its label, and containers emit
their children.

## Skia backend

`std/internal/skia_ui.oskl` consumes the display list, and `std.ui.window`
drives the surface, the SDL3 texture and the event loop. Public code only names
the widgets and the window entry point:

```oskel
import std.ui as ui;
import std.ui.window as ui_window;

let root = ui.Window("Demo", 320, 200,
  ui.Column(8, [ui.Text("Hello", 24)]));
ui_window.run("Demo", root, 320, 200, 0);
```

The release SDK ships the bridge. A source build produces the same file with
`python3 tools/prepare_ui_deps.py` and a normal CMake build; `bindings/skia/README.md`
describes the manual Skia-root path. The internal backend creates and closes the
`Paint`/`Font` objects it needs; the `Canvas` remains borrowed from the
`Surface`. The bridge module is checked in at `std/internal/skia.oskl`, so
`std.ui.window` type-checks in a clean checkout; only the C++ shim needs a Skia
build.

## Limits

- No text wrapping, alignment, borders, scrolling, text input or animation yet.
  `std.ui.window` uses Skia's real advance width through `layoutWith`; headless
  `layout` keeps the deterministic fixed advance. A checkbox is pointer-only:
  the loop does not give it keyboard focus or toggle it on Space yet.
- `std.ui` itself has no event loop; `std.ui.window` owns it. A `Button` stores
  `onClick`, `hover` and `pressed`; a `Checkbox` stores `checked`, `onChange`,
  `hover` and `pressed`. The window loop updates the state from mouse events,
  runs the button handler or toggles the checkbox on release over the pressed
  widget.
- The window loop redraws on every frame and sleeps 16 ms; it has no dirty-region
  optimisation, resize handling or keyboard focus yet.
- The native backend emits x86-64, so `std.ui.window` builds and runs with the
  hosted linker on Linux, Windows and Intel macOS. The macOS arm64 runner runs
  the VM half of `cxxbind.skia`; its AOT half is skipped because Rosetta is not
  a test dependency. The VM path works wherever the installed SDK ships the
  bundled SDL3 and Skia shim.

## Tests

| CTest name | What it proves |
| --- | --- |
| `oskel.native_build_ui` | The widget tree, layout, display list, hit test and checkbox toggle run on the VM. |
| `oskel.build_binary_ui` | The same fixture builds with `oskel build` and prints exactly the VM's output. |
| `install_stdlib` | The installed tree contains `lib/oskel/ui`, and the installed engine runs `skia_ui_test.oskl` with no `OSKEL_UI_LIBDIR`. |
| `cxxbind.skia` | Real-Skia test: the bundled bridge renders the display list to a raster surface, checks button and checkbox pixels in the VM and the hosted AOT binary, runs `skia_ui_test.oskl`, and drives `skia_window_test.oskl` through the SDL dummy driver. Registered when CMake finds Skia (the bundled target or `OSKEL_SKIA_DIR`) on Linux, Windows and macOS; the AOT half runs on x86-64 hosts, while macOS arm64 runs the VM half only. The window half also needs SDL3. |
