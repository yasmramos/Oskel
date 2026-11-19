# Oskel for VS Code

Language support for the [Oskel](../..) programming language (`.oskl`):
syntax highlighting, editor configuration and snippets.

This is a small, dependency-free extension: it contributes a language id, a
TextMate grammar, an editor configuration and a snippet file. It has no runtime
code, so it loads instantly and needs no build step.

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
code --install-extension oskel-0.1.0.vsix
```

## Files

| File | Purpose |
| --- | --- |
| `package.json` | Extension manifest: language id, file extension, grammar, snippets, editor defaults. |
| `language-configuration.json` | Comments, brackets, auto-closing pairs, indentation, folding. |
| `syntaxes/oskel.tmLanguage.json` | The TextMate grammar. |
| `snippets/oskel.json` | The snippets above. |

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
