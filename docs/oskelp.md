# oskelp — the project and package manager

`oskelp` is the Cargo of Oskel. It owns the project: a manifest, a lockfile,
dependency resolution, scaffolding and the build/run/test workflow. It is a
standalone executable, like `bindgen`: it links no engine code and drives
`oskel` (the development VM) and `oskelc` (the host compiler) as child
processes. The language, the bytecode, the VM and the AOT backend are untouched.

```text
oskelp new <dir>        scaffold a project
oskelp init [dir]       add a manifest to an existing directory
oskelp add <name>       add a path dependency to the manifest
oskelp remove <name>    remove a dependency from the manifest
oskelp build            compile the entry point to target/<name>
oskelp run              run the entry point on the VM (--native: build and run)
oskelp check            compile without running
oskelp test             run every tests/**/*.oskl
oskelp clean            remove target/ and .oskel/
oskelp tree             print the dependency tree
```

## Why a separate binary

`oskel` is the development engine and `oskelc` is the compiler; `oskelp` is the
project layer on top, exactly as `cargo` sits on `rustc`. Keeping it separate
means the engine carries no manifest, filesystem-layout or dependency policy,
`oskelc` stays a pure compiler, and the tool can be built and shipped on its
own. The sources live in `tools/`, outside the three `src/` globs, so `oskelp`
never ends up inside the engine.

## Project layout

```text
hello/
  oskel.toml                 the manifest
  oskel.lock                 the lockfile (generated, commit it)
  src/
    hello/
      main.oskl              the entry point
      greeting.oskl          another module of the same package
  tests/
    smoke.oskl               a test
  target/
    hello                    the native binary (generated)
  .oskel/
    deps/                    dependencies (generated)
      utils/
        text.oskl
  .gitignore                 /target and /.oskel
```

Every package lives under `src/<package>/`, and every import is qualified:

```oskel
import hello.greeting;          // src/hello/greeting.oskl
import { message } from hello.greeting;
import utils.text;              // .oskel/deps/utils/text.oskl
import std.math;                // the standard library
```

`oskelp` runs the compiler with `OSKEL_MODULE_ROOT=<project>/src` and
`OSKEL_PACKAGES=<project>/.oskel/deps`, so the two lookups agree on every
engine.

## The manifest: `oskel.toml`

```toml
[package]
name = "hello"
version = "0.1.0"
entry = "src/hello/main.oskl"
authors = ["Ada <ada@example.com>"]

[dependencies]
utils = { path = "../utils" }
```

| Key | Required | Meaning |
|-----|----------|---------|
| `package.name` | yes | A valid Oskel identifier; it is the import prefix. |
| `package.version` | yes | `major.minor.patch`, with an optional `-suffix` or `+build`. |
| `package.entry` | no | Defaults to `src/<name>/main.oskl`. |
| `package.authors` | no | An array of strings. |
| `dependencies.<name>` | — | `{ path = "..." }`, optionally `version = "..."`. |

The parser is a deliberately small TOML subset: comments, `[section]` and
`[section.sub]` headers, basic strings with the usual escapes, integers,
booleans, arrays and inline tables. A string dependency (`utils = "1.2.3"`)
is a registry dependency and is refused with an explanation; only `path`
dependencies are supported today. An unknown key or section is a warning, not
an error, so a manifest written for a later version still loads.

## Dependencies

A path dependency is resolved relative to the manifest that declares it, and
the whole graph is resolved recursively:

- The dependency's own `oskel.toml` must name the same package as the key
  (`utils = { path = "../utils" }` needs `name = "utils"`).
- A `version` in the dependency table must equal the dependency's version
  exactly.
- The graph must be acyclic; a cycle is reported with its path
  (`a -> b -> a`).
- The package namespace is **flat**: one version of a package name per graph.
  Two different locations for the same name are an error, not a silent pick.
- Every dependency must have `src/<its-name>/`; its modules are copied to
  `.oskel/deps/<its-name>/` before the compiler runs.

`oskelp add utils --path ../utils` writes the dependency into
`[dependencies]`, then refreshes `oskel.lock` and `.oskel/deps` in the same
step; `oskelp remove utils` reverses it. `--path` is resolved against the
current directory and stored relative to the manifest, so the same command
works from the project root or through `--manifest-path`. The editor preserves
comments and line endings, refuses a multi-line entry it cannot delete safely,
and validates the dependency's manifest (its name, and `--version` when given)
before writing anything: a failed `add` leaves `oskel.toml` untouched. Adding
a dependency that is already present is a no-op; passing a different
`--version` for the same path updates the entry.

`.oskel/deps` is regenerated on every build, so a dependency that changes on
disk is picked up. It is generated output and belongs in `.gitignore`.

### Registry dependencies

A network registry needs HTTP/TLS, which the project does not carry, and
delegating to `curl`/`git` adds a platform and supply-chain dependency. The
registry is therefore a later phase: a local registry directory first
(`$OSKEL_REGISTRY`), then a network one behind an explicit decision. Today a
string dependency fails with:

```text
oskelp: error: dependency 'utils': registry dependencies are not supported yet;
use { path = "..." }
```

`oskelp add` therefore requires `--path`; a bare `oskelp add utils` is a usage
error (exit 64).

## The lockfile: `oskel.lock`

`oskelp build`, `run`, `check`, `test`, `add` and `remove` write the lockfile
when it is missing or stale:

```toml
# Generated by oskelp. Do not edit by hand.
version = 1

[packages.utils]
version = "0.1.0"
source = "path+../utils"
```

Commit it. `--locked` makes the commands refuse to change it: a missing entry,
a version drift, a moved path or an entry the manifest no longer needs fails
with the exact reason. `--locked` never writes the lockfile.

## Commands

### `oskelp new <dir>`

Creates the layout above: a manifest, `src/<name>/main.oskl`, a
`src/<name>/greeting.oskl`, a `tests/smoke.oskl` and `.gitignore`. The package
name comes from the directory: `-`, `.` and space become `_`, other invalid
characters are dropped, and a result that starts with a digit is rejected
(pass `--name` to choose one). `--vcs git` runs `git init`.

### `oskelp init [dir]`

Adds the same layout to an existing directory. It refuses to overwrite an
existing `oskel.toml` and leaves existing source files alone.

### `oskelp add <name> --path <path> [--version <v>]`

Adds a path dependency to `[dependencies]`, creating the section when the
manifest has none. `--path` is required today; the name may be omitted
(`oskelp add --path ../utils`) and is then read from the dependency's
`oskel.toml`. `--version` asserts the dependency's exact version. The command
resolves the new graph before it writes, then refreshes `oskel.lock` and
`.oskel/deps`; a name mismatch, a self-dependency, a cycle or a missing
manifest fails without touching `oskel.toml`.

### `oskelp remove <name>`

Removes the dependency from `[dependencies]` and refreshes `oskel.lock` and
`.oskel/deps`. It refuses a dependency that is not declared, and refuses to
delete an entry that spans multiple lines (edit that one by hand). A package
still required transitively by another dependency stays in the lockfile.

### `oskelp build`

Resolves the graph, writes or checks the lockfile, materialises
`.oskel/deps`, and runs `oskelc` on the entry point. The binary is
`target/<name>` (`target/<name>.exe` for a Windows target). `--target` and
`--mode` are passed through to the compiler; see `oskelc`'s flags.

If `oskelc` is not installed, `oskelp` falls back to `oskel build`, which is
the same pipeline.

### `oskelp run [--native] [-- <args>...]`

Runs the entry point on the VM (`oskel`), which is the fast development loop.
`--native` builds first and runs the native binary. Arguments after `--` are
passed to the program.

### `oskelp check`

Runs `oskel -c` on the entry point, so syntax, type and import errors are
reported without executing anything.

### `oskelp test [--native]`

Runs every `.oskl` file under `tests/`, recursively, in sorted order. A test
passes when its process exits with `0`; any other exit code is a failure. The
summary is `test result: ok. N passed; 0 failed`, and the command exits `101`
when at least one test fails (the cargo convention). `--native` compiles each
test with `oskelc` and runs the binary, which is the AOT parity check.

A test imports the package under test, not a sibling test file:
`import { message } from hello.greeting;` works because the module root is
`src/`.

### `oskelp clean`

Removes `target/` and `.oskel/`.

### `oskelp tree`

Prints the dependency tree:

```text
hello v0.1.0
`-- utils v0.1.0
```

### `--manifest-path <file>`

Every project command accepts `--manifest-path <file>` and runs from the
manifest's directory, so a script does not have to `cd` first:

```sh
oskelp build --manifest-path ../hello/oskel.toml
```

## Environment variables

| Variable | Meaning |
|----------|---------|
| `OSKEL` | Path to the `oskel` binary (default: a sibling of `oskelp`, then `PATH`). |
| `OSKELC` | Path to the `oskelc` binary (same rule). |
| `OSKEL_MODULE_ROOT` | Overrides the module root for `oskel`/`oskelc`; `oskelp` sets it to `<project>/src`. |
| `OSKEL_PACKAGES` | The installed-packages root; `oskelp` sets it to `<project>/.oskel/deps`. |

`OSKEL_MODULE_ROOT` is the only compiler-side hook the tool relies on. Both
engines go through `resolverFor`, so the VM and the AOT backend resolve
imports identically.

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success. |
| `1` | A dependency, lockfile or build failure. |
| `64` | Command-line usage error. |
| `65` | Compile error (propagated from the compiler). |
| `66` | A manifest or project file could not be read or parsed. |
| `70` | Runtime error (propagated from the VM). |
| `101` | At least one test failed. |

## Current limitations

- Path dependencies only; no registry, `update` or `publish`. `add` and
  `remove` edit `[dependencies]` for path dependencies today.
- A flat package namespace: one version per package name.
- No workspaces, no build profiles. `target/<name>` is the only output.
- Tests are whole files that exit `0`; there is no `@Test` attribute yet.

These are tracked in `docs/BACKLOG.md` §20.
