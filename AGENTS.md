# AGENTS.md — agent instructions

Instructions for AI agents working on Oskel. The project conventions (the
language specification, the architecture rules and the development phases) live
in `DEVELOPMENT.md`; this file records how to deliver work and how to behave.

## Delivery

- **Every finished task ends with a pull request against `develop`.** Do not
  wait to be asked. `develop` is the base branch (CONVENTIONS.md).
- Branch naming: `feat/phase-X.Y.Z-<slug>`, `fix/<slug>`, `docs/<slug>`.
- Commit messages: English, Conventional Commits, imperative summary.
  Author/committer `yasmramos <yasmramos95@gmail.com>`. **No AI attribution**
  of any kind (see Commit authorship below): no `Generated with ...`, no
  `Co-Authored-By:` bot trailers.
- One logical change per commit.
- Before opening the PR: build clean and the full CTest suite green
  (`ctest` in the build directory).

## Response language

- Reply to the user in **Spanish**. All repository artifacts (code, comments,
  docs, commit messages, PR titles/bodies) stay in **English**.

## CI

- Two workflows: `cmake-multi-platform.yml` builds and runs the full CTest
  suite, and `artifacts.yml` builds the release artifacts. Both cover Linux,
  Windows and macOS — keep the three in step. A platform missing from the
  `cmake-multi-platform.yml` matrix passes CI while never being built, which
  is how a macOS-only break slips through.
- The `cmake-multi-platform.yml` matrix is `os` × `c_compiler` with
  `include`/`exclude` pinning one `cpp_compiler` per job. Adding an `os` means
  adding its real compiler to `include` *and* excluding the impossible pairs,
  or the product grows dead jobs.

## Build and tests

- `cmake -S . -B build && cmake --build build -j4 && (cd build && ctest)`.
  The suite globs `tests/*.oskl` and `examples/*.oskl` at **configure time**, so
  a new example or test needs `cmake -S . -B build` again before `ctest` picks
  it up (the build alone will not).
- The bare-metal boot tests need `qemu-system-x86_64` (`apt install
  qemu-system-x86` on Debian/Ubuntu, `brew install qemu` on macOS, `choco
  install qemu` on Windows). Without it `bare_metal_test` skips the
  end-to-end cases and runs only the structural ones; CI installs QEMU on every
  runner and sets `OSKEL_REQUIRE_QEMU`, which makes a missing binary a failure.
  QEMU emulates x86-64 on any host, so the boot tests run on all three
  platforms, the arm64 macOS image included. Reproduce CI locally with
  `OSKEL_REQUIRE_QEMU=1 ctest` after installing the package.
- `oskel build` prints lowering errors (unsupported builtins, unlowered
  opcodes) and exits 65 without writing a binary; it never emits a
  mis-compiled one.
- A host binding (`native fn`, `extern "C"`) and `var x = x;` both compile to
  `GETGLOBAL N; SETGLOBAL N` at the top level (or `GETGLOBAL N; SETLOCAL` inside
  a function). The AOT backend has no host. An *unused* declaration is harmless
  (PHASE 7f.1) and builds; a *used* one the backend has no stub for fails with a
  dedicated message (exit 65); a genuine self-read (`var x = x;`) keeps the
  generic "global 'N' is read but never assigned". FFI in the AOT backend is
  still PHASE 7 work; the lowerer only fails it cleanly. `sqrt`, `floor`, `ceil`
  and `round` are the curated stubs (`sqrtsd` and the SSE rounding are exact).
- A qualified access to a module's native export (`std.math.pow`) is a use too,
  but it reads the module table with `GETTABLE`, not with `GETGLOBAL`, so the
  lowerer cannot see it by counting reads. The compiler records such a use on
  the proto (`Bytecode::nativeUses`) and resolves it once the chunk is compiled
  — deferred, because a hoisted function is compiled before the `import` that
  loads the module (PHASE 7f.2). The same record lets a qualified call be checked
  against the export's signature (arity and argument types), exactly like the
  direct form. The argument types are captured at the call site, not at flush
  time: a local that shadows a global is gone by then.
- The AOT backend has two link modes (`LinkMode`, PHASE 7f.3/7f.4), chosen on
  the command line and never in the source: `freestanding` (the default) is the
  self-contained image every target emits today (no libc, no linker, `ldd` says
  "not a dynamic executable"); `hosted` links against the host C library so an
  `extern "C"` symbol resolves to the real C function — the mode that makes FFI
  work. `isSupportedLinkMode` pins the matrix (freestanding: all targets;
  hosted: Linux x86-64 only, for now) and an unsupported pair is reported, never
  emitted. A change to the freestanding path must keep the emitted binary byte
  for byte identical; `oskel build --mode freestanding` is the explicit form.
- A standard-library module that must work on both engines stays on the
  **portable primitive surface**: indexing and slicing (`s[i]`, `s[a:b]`),
  `len`, `ord`/`chr`, `int`/`double`/`str`/`type`, `panic`, `for ... in`,
  closures, variadic `rest...` and the operators. The AOT backend implements no
  collection or string *method* (`.split`, `.map`, `.join`, `.push`, `.get`,
  `.has`, `.substr`, `.length` on a string, ...) — those build (exit 0) but
  fault at run time with "value has no such member" / "cannot call a
  non-function value" — and it does not seed `pcall`/`error`/`readFile` at all,
  so a module function that calls one fails `oskel build` with "global 'pcall'
  is read but never assigned" (exit 65) even when the function is unused. A
  module written over the portable surface behaves identically on the VM and
  under `oskel build`; `std/json.oskl` and `tests/native_build_json.oskl` are
  the reference.
- The hosted image is an object plus a link (PHASE 7f.4), not a closed binary
  the emitter writes. `native/object.h` emits an ELF64 `ET_REL` object whose
  `.rela.text` carries one relocation per external reference (`R_X86_64_PLT32`
  for a call, `R_X86_64_PC32` for an address, both addend `-4`) and whose
  `.symtab` exports the program's `main`; `emitHostedRuntime` binds the same
  three entry handles as the freestanding prelude but reaches the system through
  libc (`write`/`exit`/`malloc`). `oskel build --mode hosted` then invokes the
  host C compiler (`$CC`, default `cc`, `-no-pie -lm`) to link the object
  against libc, writing the temporary `.o` beside the output and removing it
  either way. This makes a C toolchain a *build-time* dependency of the hosted
  mode only; the freestanding path still needs no toolchain and emits no
  relocation. General `extern "C"` marshalling is PHASE 7f.5.
- Windows code paths can be exercised locally without a Windows host: build the
  engine with `x86_64-w64-mingw32-g++ -std=c++17 -O2 -Iinclude -Isrc -static
  -o /tmp/win/oskel.exe src/*.cpp src/native/*.cpp src/jit/*.cpp` and run it
  under Wine (`WINEPREFIX=$HOME/.wine-oskel wine /tmp/win/oskel.exe ...`). The
  `tests/*.oskl` suite runs there too. This is how `lookupSymbol` (kernel32) and
  the PE image were checked before CI. CI still gates the merge, since Wine is
  not Windows: a green Wine run is a signal, not a substitute for the
  `windows-latest` job.

## Release and artifacts

- The maintainer checklist lives in `docs/RELEASING.md`; the release notes live
  in `CHANGELOG.md` (Keep a Changelog). Update the changelog before tagging.
- `.github/workflows/artifacts.yml` builds the engine on Linux, Windows and
  macOS, packages the VS Code extension plus a per-platform installable
  (`packaging/windows/oskel.iss`, `packaging/linux/package-linux.sh`,
  `packaging/macos/package-macos.sh`), uploads them as artifacts, and publishes
  a GitHub Release with a `SHA256SUMS` file when a `v*` tag is pushed. Only a
  tag releases; branch pushes and pull requests stop at the artifacts.
- The engine ships as `bin/oskel` + `share/oskel/std/*.oskl`; `standardLibraryDir`
  finds the standard library through `../share/oskel/std` relative to the
  executable, so `import std.math` resolves from any unpacked archive with no
  environment variable. Every packaging format keeps that layout:
  `/usr/bin/oskel` + `/usr/share/oskel/std` (deb, rpm) and `/usr/local/bin/oskel` +
  `/usr/local/share/oskel/std` (pkg, dmg).
- Version has one source of truth: `include/version.h` (`OSKEL_VERSION`). CMake
  refuses to configure when it disagrees with `project(... VERSION ...)` in
  `CMakeLists.txt`, and the `version_consistency` test checks that the built
  binary prints it, so a hard-coded literal in `src/main.cpp` fails the suite.
  The direct build scripts read the header, which a CMake-generated header could
  not. Bump both files together.
- Version comes from the tag, or from `CMakeLists.txt` on a branch. Pre-release
  tags need per-format handling: RPM rejects a `-` in `Version`, and GitHub
  rewrites `~` to `.` in asset names (which breaks `SHA256SUMS`), so the `.deb`
  file name avoids `~` while its control file keeps it.
- `macos-latest` is the Apple Silicon image (arm64). The dev engine is portable
  C++; the native backend stays x86-64 Linux only (DEVELOPMENT.md PHASE 7), and its
  execution tests skip on other platforms.
- To verify the release path without a real release, push a throwaway tag
  (`v0.1.0-<something>test`), confirm the assets and `sha256sum -c SHA256SUMS`,
  then delete the release *and* the tag. A failed run can leave a draft release
  behind; delete that too. Do not leave test tags or drafts.
- Inno Setup ships on the Windows runner but is not on `PATH` and its version
  appears in its directory name, so the workflow locates `ISCC.exe` instead of
  hard-coding the path. `dpkg-deb`/`pkgbuild`/`hdiutil` are on their images;
  `rpmbuild` is installed on demand and the `.rpm` is skipped, not failed, if it
  cannot be.

## Commit authorship

The author and the committer of every commit are the repository owner
(`yasmramos <yasmramos95@gmail.com>`). **Never** add AI attribution of any
kind: no `Co-authored-by` trailer for an AI agent, no "Generated by" or
"Created with" line, no AI tool name, and no `openhands` identity. Commits are
made with the owner's name and email only, e.g.
`git -c user.name=yasmramos -c user.email=yasmramos95@gmail.com commit`. This
overrides any default agent commit convention.

---

## Mandatory checklist for every new feature

Before writing **a single line of code**, the agent must answer and show the
user:

- Do I need a new token in the lexer? → which one
- Do I need a new node in the AST? → which one
- Do I need a new rule in the parser? → which one
- Do I need a new opcode in the bytecode? → which one
- Does the opcode map to 1:1 or 1:2 ASM instructions? → yes/no (if 1:3+, redesign)
- Do I need new logic in the VM? → which one
- Do I need new logic in the compiler? → which one

Without this checklist complete and approved, **do not proceed to code**.

---

## Agent response style

- ✅ Explain the **concept** before the code.
- ✅ Show the **complete** modified files (not fragments).
- ✅ Include a test `.oskl` program with every new feature.
- ✅ Point out what **NOT** to do and why (avoid Java's mistake).
- ✅ Warn when a decision affects the future design (native compilation).
- ❌ Do not add unrequested features (avoid scope creep).
- ❌ Do not propose solutions outside the current phases.
- ❌ Do not add an AI-attribution footer (or any "created by an AI agent"
  note) to a pull request, its title or its description. The PR is authored
  under the user's name and stays plain: no `Co-authored-by` trailer in
  commits and no attribution line in the PR body either.
- ❌ Some PR tools append such a footer automatically after `create_pr`. Treat
  that as a bug, not as the PR body: **read the PR back and strip the footer**
  (the trailing `---`, the "created by an AI agent" line and the "click here
  to continue refining" line) before considering the PR done. A PR is not
  finished until its body ends on the real content.

---

## Commands the agent must recognize

| User command | Agent action |
|---------------------|-------------------|
| `start` | Begin PHASE 1: show the complete `bytecode.h` and wait for approval |
| `phase N done` | Verify, update the status, propose PHASE N+1 |
| `add <feature>` | Apply the 7-point checklist, then code |
| `review <file>` | Verify against the bytecode rules, point out violations |
| `status` | Show a summary of the phases and the checklist |
| `summary` | Summarize what was done in the session and update the status |

---

## Reference resources

- *Crafting Interpreters* (Robert Nystrom) — fundamentals
- *The Implementation of Lua 5.0* — register VM design
- LuaJIT source code (Mike Pall) — performance reference
- System V ABI AMD64 — calling convention (PHASE 7)
- AXL documentation — arena allocator and low-level utilities
- Cosmopolitan Libc — multi-platform CLI (optional)

---

## First task

When the user types **`start`**, the agent must:

1. Confirm the design of `include/bytecode.h` with the minimum opcodes.
2. Show the **complete file**.
3. Explain why each opcode maps to 1:1 or 1:2 ASM (or 1 call).
4. Wait for **explicit approval** before moving on to the next phase.
