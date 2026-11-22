# Declarative UI (`std/ui.oskl`)

`std/ui.oskl` is the first declarative widget layer. It builds a tree of
widgets, measures and places it, and emits a **display list** of rectangles and
text. It does not open a window and does not call a graphics API: SDL3 owns the
window and the events, and a backend such as Skia consumes the display list.

The module is plain Oskel over the portable primitive surface, so the tree,
layout, display list and hit testing behave identically on the development VM
and under `oskel build` (`tests/native_build_ui.oskl`).

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
| `button(label, onClick)` | blue button with white text |
| `buttonStyle(label, onClick, background, color)` | button with explicit colours |

A widget is a map with a `kind`, a frame (`x`, `y`, `w`, `h`) and the fields its
kind needs. `layout` mutates the tree and attaches the frame; `commands` reads
the frames and returns new maps, so drawing never mutates the tree.

## Layout

- A column gives each child the full width and stacks it by its measured height.
- A row gives each child its measured width and the full height.
- Padding insets its child by `all` on every side.
- Text is measured as `0.6 * size` per character and `1.4 * size` per line.
- A window uses its own `width`/`height`; any other root uses the viewport passed
  to `layout`.

The measurement is deliberately deterministic, not a font engine. A real
renderer can measure text with its own metrics later; the first slice keeps the
tree and the test independent of a font.

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

`bindings/skia/skia_ui.oskl` consumes the display list:

```oskel
import skia;
import skia_ui;
import std.ui as ui;

let root = ui.window("Demo", 320, 200, ui.column(8, [ui.text("Hello", 24)]));
ui.layout(root, 320, 200);

let surface = skia.Surfaces.raster(320, 200);
let canvas = surface.getCanvas();
skia_ui.render(ui.commands(root), canvas);
```

Build the Skia bridge first (`bindings/skia/README.md`). The backend creates and
closes the `Paint`/`Font` objects it needs; the `Canvas` remains borrowed from
the `Surface`.

## Limits

- One font metric, no wrapping, alignment, borders, scrolling, input widgets or
  animation yet.
- A button stores `onClick`; the application dispatches it after `hitTest`.
  There is no event loop in `std/ui` yet.
- SDL3 creates the OS window; a Skia surface that presents into that window is
  the next slice. Today the Skia backend renders to a raster surface, which is
  what the pixel test uses.

## Tests

| CTest name | What it proves |
| --- | --- |
| `oskel.native_build_ui` | The widget tree, layout, display list and hit test run on the VM. |
| `oskel.build_binary_ui` | The same fixture builds with `oskel build` and prints exactly the VM's output. |
| `cxxbind.skia` | Optional real-Skia test: the generated bridge renders the display list to a raster surface, checks pixels in the VM and the hosted AOT binary, and also runs `skia_ui_test.oskl`. Registered only when `OSKEL_SKIA_DIR`/`OSKEL_SKIA_LIB` point at a Skia build. |
