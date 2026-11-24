# oskelint — the linter

`oskelint` reports suspicious but legal Oskel code. It is a separate executable
because a lint rule answers a question the compiler deliberately does not ask:
the compiler must accept every program the language defines, and it must stay
clear of the builtin names (`tests/check_compiler_boundary.cmake` forbids the
front end from reaching `runtime/builtins.h`). A tool can hold both facts.

```sh
oskelint <file.oskl>     # report the diagnostics on stdout
oskelint --list-rules    # print the rule names, one per line
```

A file with diagnostics exits 0. A lint warning is not an error: the compiler's
own warnings do not fail a build either, and a linter that turned every
advisory into a red job would be switched off. Exit 64 is a usage error, 65 a
file the compiler rejects, 66 an unreadable input.

## `builtin-shadow`

A builtin is an ordinary global — `addNative` is `defineGlobal` — so declaring
a name that is a builtin replaces it for the rest of the scope. That is legal
and the standard library relies on it, so the rule is advisory:

```oskel
var len = 3;          // warning: 'len' shadows the builtin 'len'
print(len);           // 3, the builtin is gone for this scope
```

It is the Oskel equivalent of pylint's `redefined-builtin` (W0622) and ruff's
`A001`.

The rule reports a **declaration**: `var`, `let`, `const`, `fn`, a `struct`, an
`enum`, a parameter, a lambda parameter and a `for ... in` variable. It does not
report a use.

A bodyless declaration is the exception. The parameters of an `extern "C"` or a
`native fn` are names for the signature — the C side's, or the host's — so they
bind nothing here and shadow nothing; the declaration's own name does bind a
global and is reported like a `fn`'s.

### The re-export idiom is not a shadow

A declaration whose initializer reads the same name surfaces the builtin
instead of hiding it. This is how `std.sys` exports `time`, `clock`, `arch`,
`platform` and `exit`:

```oskel
let time = time;      // no diagnostic: the builtin is being re-exported
```

A rule that fired here would report the standard library against itself.

## Where the builtin names come from

`oskelint` links the engine for one question: it constructs a `VM` and reads
`globals()`, the names `registerBuiltins` actually installs. There is no second
list to drift when a builtin is added or renamed. The tool never runs a program,
so `oskelc` — which must not carry the interpreter — is untouched, and
`nm build/oskel | grep oskelint` stays empty.
