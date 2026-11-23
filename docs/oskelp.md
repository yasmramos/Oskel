# oskelp — the project and package manager

`oskelp` is the Cargo of Oskel. It owns the project: a manifest, a lockfile,
dependency resolution, scaffolding and the build/run/test workflow. It is a
standalone executable, like `bindgen`: it links no engine code and drives
`oskel` (the development VM) and `oskelc` (the host compiler) as child
processes. The language, the bytecode, the VM and the AOT backend are untouched.

```text
oskelp new <dir>        scaffold a project
oskelp init [dir]       add a manifest to an existing directory
oskelp add <name>       add a dependency (latest release without --version)
oskelp remove <name>    remove a dependency from the manifest
oskelp publish          copy the package into the local registry
oskelp yank <name>      hide a published version from new selections
oskelp update [<name>]  move the lockfile to newer registry versions
oskelp outdated         show the registry updates that are available
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
text = "^1.0.0"
```

| Key | Required | Meaning |
|-----|----------|---------|
| `package.name` | yes | A valid Oskel identifier; it is the import prefix. |
| `package.version` | yes | `major.minor.patch`, with an optional `-suffix` or `+build`. |
| `package.entry` | no | Defaults to `src/<name>/main.oskl`. |
| `package.authors` | no | An array of strings. |
| `dependencies.<name>` | — | `{ path = "..." }`, optionally `version = "..."`, or a version requirement string. |

The parser is a deliberately small TOML subset: comments, `[section]` and
`[section.sub]` headers, basic strings with the usual escapes, integers,
booleans, arrays and inline tables. A string dependency
(`utils = "^1.2.3"`) is a registry dependency, resolved from
`$OSKEL_REGISTRY`; see below. An unknown key or section is a warning, not
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

Without `--path`, `--version` names a **registry requirement**:
`oskelp add utils --version "^0.1.0"` writes `utils = "^0.1.0"`, resolves it
from `$OSKEL_REGISTRY` and refreshes the lockfile and `.oskel/deps`. A
registry add needs a name (there is no path manifest to derive it from); an
unsatisfiable requirement fails before the manifest is written. When
`--version` is omitted, `add` reads the registry, takes the highest
**release** and writes a caret requirement (`^0.2.0`). A package that has only
prereleases is not picked implicitly: pass the exact prerelease version
(`--version 0.2.0-beta.1`), because a caret requirement over a prerelease is
ambiguous.

`.oskel/deps` is regenerated on every build, so a dependency that changes on
disk is picked up. It is generated output and belongs in `.gitignore`.

### Registry dependencies

A network registry needs HTTP/TLS, which the project does not carry, and
delegating to `curl`/`git` adds a platform and supply-chain dependency. The
first step is a **local registry directory** named by `$OSKEL_REGISTRY`; a
network one stays a separate decision.

The layout is the same tree `oskelp publish` writes:

```text
$OSKEL_REGISTRY/
  utils/
    0.1.0/
      oskel.toml
      src/utils/text.oskl
    0.2.0/
      oskel.toml
      src/utils/text.oskl
```

A dependency declared as a string is resolved by:

- Reading the requirement: an exact version, `^`, `~`, `>=`, `>`, `<=`, `<`,
  `=`, `*`/`x` wildcards, and a comma- or space-separated conjunction of
  those (`>=1.0.0 <2.0.0`). A prerelease is selected only by an exact
  requirement for it (`1.2.3-beta.1`), never by a range.
- Listing `<registry>/<name>/<version>/` and keeping the highest version that
  satisfies the requirement.
- Preferring the version in `oskel.lock` when it still satisfies the
  requirement and still exists. A normal build therefore **does not** upgrade
  a registry dependency; `oskelp update` moves the pin.
- Loading the entry's `oskel.toml` and checking that it names the dependency
  and the directory version.

The registry is a flat namespace, like the rest of the graph: one version per
package name. If two parents require incompatible versions, the resolver
reports the version it already selected and the requirement that rejects it;
it does not backtrack to a lower version. Publish a compatible version, or
relax the requirements.

`oskelp publish` copies `oskel.toml` and `src/` into
`<registry>/<name>/<version>/`, refusing to overwrite an existing version. A
package with a **path** dependency cannot be published: the consumer would
resolve that path relative to the registry copy. Publish the dependency and
depend on its version first. A package that depends on itself is refused too.

`oskelp yank` writes `<registry>/<name>/<version>/.yanked`, which hides that
version from every new selection: `add`, `update` and `outdated` ignore it. The
files are not deleted, and a lockfile that already pins the yanked version keeps
building, so withdrawing a release does not break an existing project. A
`--precise <version>` that names a yanked version fails instead of silently
choosing another one. `--undo` removes the marker; both directions are
idempotent, so repeating the command is safe.

`oskelp update` re-resolves without the pin for the named package, or for
every package when no name is given, writes `oskel.lock` and refreshes
`.oskel/deps`. It prints `updated utils 0.1.0 -> 0.2.0` (or
`everything is up to date`). A named package must be a direct dependency or
already present in the lockfile. `--precise <version>` pins that package to
exactly one published registry version (a downgrade inside the requirement is
allowed) and fails when the version is missing, is not a registry dependency,
or falls outside the manifest requirement; it never silently falls back to
another version. `oskelp outdated` reads the lockfile, compares each registry
package with the registry, and prints the highest version still allowed by a
direct requirement plus a note when a newer version needs a manifest change:

```text
utils 0.2.0 -> 0.2.1 (^0.2.0); latest 0.3.0 needs a new requirement
```

It exits `0` whether or not updates exist; a script that wants to fail on an
outdated lock should inspect the output.

## The lockfile: `oskel.lock`

`oskelp build`, `run`, `check`, `test`, `add`, `remove` and `update` write
the lockfile when it is missing or stale:

```toml
# Generated by oskelp. Do not edit by hand.
version = 1

[packages.utils]
version = "0.1.0"
source = "path+../utils"

[packages.text]
version = "1.0.0"
source = "registry+text@1.0.0"
```

Commit it. A `registry+<name>@<version>` source pins the registry version: a
build reuses it while it still satisfies the manifest, and `oskelp update`
moves it. `--locked` makes the commands refuse to change the lockfile: a
missing entry, a version drift, a moved path, a changed registry source or an
entry the manifest no longer needs fails with the exact reason. `--locked`
never writes the lockfile.

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

### `oskelp add <name> [--version <requirement>]`

Adds a dependency to `[dependencies]`, creating the section when the manifest
has none. With `--path`, the name may be omitted
(`oskelp add --path ../utils`) and is then read from the dependency's
`oskel.toml`; `--version` asserts the dependency's exact version. Without
`--path`, `--version` is a registry requirement (`^0.1.0`, `~1.2.3`,
`>=1.0.0 <2.0.0`, `1.2.*`, …) and the package is resolved from
`$OSKEL_REGISTRY`. Without `--version`, the latest published release becomes
the requirement (`utils = "^0.2.0"`); a prerelease-only package needs an
explicit version. Adding a registry dependency that is already present
refreshes its requirement to the latest release, as `cargo add` does. The
command resolves the new graph before it writes, then
refreshes `oskel.lock` and `.oskel/deps`; a name mismatch, a self-dependency,
a cycle, a missing manifest or an unsatisfiable requirement fails without
touching `oskel.toml`.

### `oskelp remove <name>`

Removes the dependency from `[dependencies]` and refreshes `oskel.lock` and
`.oskel/deps`. It refuses a dependency that is not declared, and refuses to
delete an entry that spans multiple lines (edit that one by hand). A package
still required transitively by another dependency stays in the lockfile.

### `oskelp publish [--registry <dir>]`

Copies `oskel.toml` and `src/` to
`<registry>/<name>/<version>/`, where `<registry>` is `--registry` or
`$OSKEL_REGISTRY`. It refuses an existing version (versions are immutable), a
package with a path dependency, a package that depends on itself, and a
missing entry or `src/<name>/` directory. The registry directory is created
on demand.

### `oskelp update [<name>] [--precise <version>]`

Re-resolves the graph without the lockfile pin for `<name>`, or without any
pin when no name is given, then rewrites `oskel.lock` and refreshes
`.oskel/deps`. It prints each change (`updated utils 0.1.0 -> 0.2.0`) or
`everything is up to date`. `<name>` must be a direct dependency or already
present in the lockfile. `--precise <version>` requires `<name>` and pins it
to exactly that published registry version; the version must exist and satisfy
the manifest requirement, and a path dependency is rejected because it has no
registry version. `--precise` never falls back to another version, so a typo
or an out-of-range version fails instead of silently picking the newest one.

### `oskelp outdated [--manifest-path <file>]`

Compares the lockfile with `$OSKEL_REGISTRY` and prints every registry
dependency that can move. A direct dependency shows the highest version still
allowed by its requirement, plus the newest version when that one needs a
manifest change:

```text
utils 0.2.0 -> 0.2.1 (^0.2.0); latest 0.3.0 needs a new requirement
helper 1.0.0 -> 1.1.0
```

A transitive dependency has no requirement in the root manifest, so it shows
the registry's latest version. When everything is current it prints
`All registry dependencies are up to date.` The command only reads the
manifest and the registry; it does not write the lockfile. It exits `0` in
both cases.

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
| `OSKEL_REGISTRY` | The local registry root: `<root>/<name>/<version>/`. Required by a registry dependency, by `add` without `--version`, by `outdated`, by `update --precise`, and by `publish` unless `--registry` is given. |

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

- A local registry directory only (`$OSKEL_REGISTRY`); no network registry,
  checksums or signatures.
- A flat package namespace: one version per package name. The resolver does
  not backtrack; two incompatible requirements for one name are an error.
- A published package cannot keep a path dependency.
- No workspaces, no build profiles. `target/<name>` is the only output.
- Tests are whole files that exit `0`; there is no `@Test` attribute yet.

These are tracked in `docs/BACKLOG.md` §20.
