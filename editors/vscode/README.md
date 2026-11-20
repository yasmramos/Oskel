# Oskel for VS Code

Language support for the [Oskel](../..) programming language (`.oskl`):
syntax highlighting, editor configuration, snippets and compiler diagnostics.

This is a small, dependency-free extension: a language id, a TextMate grammar,
an editor configuration, a snippet file and one CommonJS file that runs the
compiler in check mode. No bundler and no runtime dependency; the only thing it
needs is the `oskel` binary, and only for diagnostics.

## What it highlights

- Keywords: `let`, `var`, `const`, `fn`, `struct`, `class`, `native`, `extern`,
  `import`, `if`, `else`, `while`, `do`, `for`, `in`, `switch`, `case`,
  `default`, `try`, `catch`, `defer`, `break`, `continue`, `return`.
- Constants: `true`, `false`, `nil`.
- Types: the primitive and FFI vocabulary (`int`, `float`, `double`, `string`,
  `bool`, `array`, `map`, `error`, `range`, `void`, `any`, `ptr`, `cstring`,
  `size_t`, the fixed-width integers, ...), the nullable marker `T?`, a user
  type in an annotation (`let p: Point`) and a struct literal or constructor
  call (`Point { x: 1 }`, `Point(3, 4)`).
- Numbers: integers, doubles, `f`/`F` floats, `_` separators and the `0x`/`0b`/
  `0o` prefixes.
- Strings with `\n`, `\t`, `\xNN`, `\uNNNN` escapes and `${ ... }`
  interpolation (the embedded expression is highlighted as Oskel).
- Comments: `//` and nestable `/* ... */`.
- Imports: `import std.math;`, `import a.b.*;`, `import a.b as m;` and
  `import { x, y } from a.b;`.
- `@MustUse`, `extern "C" { fn ... }`, `extern fn ...;` and `native fn ...;`.
- Builtins (`print`, `len`, `sqrt`, `time`, `memSize`, `args`, ...) and calls.
- `self` inside a class method.
- Operators, including `**`, `??`, `?.`, `!!`, `..`, `..=`, `...`, `=>` and the
  compound assignments the language actually has.

Syntax the language does **not** have is deliberately not highlighted: Oskel has
no `===`, no `!==`, no `->`, and no `<<=`, `>>=`, `&=`, `|=` or `^=`.

## Diagnostics

Saving an `.oskl` file runs the compiler in check mode (`oskel -c`) and shows
what it reports as editor diagnostics, so a type error is a squiggle on the line
that caused it instead of a message in a terminal:

```
bad.oskl:3:7: error: cannot assign 'string' to 'int' in 'x'
```

becomes an error at line 3, column 7, sourced as `oskel` in the Problems panel.
Warnings are shown as warnings (an unused `@MustUse` result, for example). A
check costs about 2 ms, so one per save is free.

There is no language server: the compiler already prints `file:line:col: severity:
message` and exits 65, which is everything a diagnostic needs. Hover, completion
and go-to-definition are not provided.

### Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `oskel.check.enable` | `true` | Report diagnostics at all. |
| `oskel.check.onSave` | `true` | Check on every save. |
| `oskel.check.path` | `""` | The compiler. Empty means the `oskel` on `PATH`; a relative path is resolved against the first workspace folder. |

### Commands

| Command | What it does |
| --- | --- |
| `Oskel: Check file` | Check the active file now. |
| `Oskel: Clear diagnostics` | Clear the Problems panel for Oskel. |

### What the compiler cannot tell you

`oskelc` is **not** usable here: its `-c` is not implemented, and it exits 64
with "not implemented yet". The extension runs `oskel`, the development engine.

An error inside an imported module is reported by the compiler under the *root*
file's name with the module's line and column, so the position can point past
the end of the file you have open. The diagnostic is clamped to a line that
exists and the message keeps the module's name (`in module 'std.math': ...`), so
nothing is lost except the exact column.

## Editor configuration

`language-configuration.json` supplies the comment tokens, the bracket pairs,
the auto-closing pairs (including `/* */`), the indentation rules (a line that
opens `{`, `(` or `[` indents the next one, and a closing bracket dedents) and
the `// region` folding markers.

`package.json` also sets per-language editor defaults: two-space indentation,
`insertSpaces`, no indentation guessing, bracket-pair guides and a space after
`//`. These apply only to `.oskl` files and can be overridden per workspace.

## Snippets

Type a prefix and press `Tab` in an `.oskl` file:

| Prefix | Expands to |
| --- | --- |
| `fn` | a named function |
| `fnx` | a function with an expression body |
| `lambda` | `x => x * 2` |
| `struct` | a struct declaration |
| `class` | a class with a field and `init` |
| `method` | a method inside a class |
| `let` / `var` | a variable declaration |
| `if`, `ife`, `while`, `do`, `for`, `forin`, `forinmap` | control flow |
| `switch` | a switch with a `case` and a `default` |
| `import`, `import*`, `importfrom` | the three import forms |
| `extern` | an `extern "C"` block |
| `native` | a `native fn` declaration |
| `mustuse` | an `@MustUse` function |
| `catch`, `try`, `defer` | error handling and `defer` |
| `print`, `assertEq` | the common builtin calls |

## Install (development)

Copy or symlink this folder into your VS Code extensions directory:

```sh
ln -s "$(pwd)/editors/vscode" ~/.vscode/extensions/oskel
```

Then reload the window. Open any `.oskl` file and pick "Oskel" as the language
mode if it is not detected automatically (the `.oskl` extension is registered).

Alternatively, press `F5` in VS Code with this folder open to launch an
Extension Development Host.

## Package a `.vsix` (optional)

```sh
npm install -g @vscode/vsce
cd editors/vscode
vsce package
code --install-extension oskel-0.2.0.vsix
```

## Files

| File | Purpose |
| --- | --- |
| `package.json` | Extension manifest: language id, file extension, grammar, snippets, editor defaults, commands, settings. |
| `language-configuration.json` | Comments, brackets, auto-closing pairs, indentation, folding. |
| `syntaxes/oskel.tmLanguage.json` | The TextMate grammar. |
| `snippets/oskel.json` | The snippets above. |
| `extension.js` | Runs `oskel -c` on save and publishes the diagnostics. |
| `src/diagnostics.js` | Parses the compiler's stderr into positions, converting its byte column to UTF-16. No `vscode` import, so it is unit-tested. |
| `src/save-guard.js` | Keeps a programmatic save from starting a second check. Also `vscode`-free. |
| `test/diagnostics.test.js` | `node --test` over the parser, with real compiler output as fixtures. |
| `test/save-guard.test.js` | `node --test` over both save-event orderings. |

## Keeping it in step with the compiler

The grammar keeps its own keyword, type and builtin lists, because a TextMate
grammar cannot read the compiler at run time. Those lists are derived from a
single source of truth per vocabulary — the `kKeywords` table in
`src/lexer.cpp`, the type names in `src/ast.h` and the `addNative` registry in
`src/builtins.cpp` — and `tests/check_vscode_grammar.cmake` fails the CTest
suite when the grammar misses a word or highlights syntax the lexer never
produces. Run it alone with:

```sh
cmake -DROOT=. -P tests/check_vscode_grammar.cmake
```

When the language gains a keyword, a type or a builtin, update
`syntaxes/oskel.tmLanguage.json` in the same change; the test names the missing
word.

The diagnostic parser is checked the same way, and for the same reason: it
depends on the compiler's output shape, so `tests/check_vscode_diagnostics.cmake`
drives it with the real stderr. Run it alone with:

```sh
cmake -DROOT=. -P tests/check_vscode_diagnostics.cmake
```

It skips when `node` is absent, since Node is not a build dependency of the
engine.
