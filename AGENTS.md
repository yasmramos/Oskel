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

### A pull request is not finished until it is mergeable

Creating the PR is the middle of the task, not the end. Before reporting a PR as
done, check all of the following, and fix what is not true rather than reporting
it as a caveat:

- **`mergeable` is true and `mergeable_state` is `clean`.** Read it back from the
  API (`GET /repos/{owner}/{repo}/pulls/{n}`) — `gh pr view` and the tool that
  creates the PR both report the state as computed *before* the last push, and
  GitHub needs a moment to recompute after a push, so re-read until the field is
  no longer `null`. A `dirty` state means a conflict with `develop`: merge
  `develop` in, resolve it, re-run the suite, push. A `behind` state means the
  branch needs the base merged in. Never open a PR and leave it conflicting.
- **The base is `develop`**, never `main` (only a release targets `main`).
- **Every CI job is green**, not just the one you expected to break. Read each
  failing job's log and name the real cause. A red job that is infrastructure
  (a registry 503, a runner image change) is re-run once and then reported as
  infrastructure, with the evidence — never as "probably flaky".
- **The suite is green *on the merge commit*.** CI tests `develop` merged with
  the branch, so a file another PR added after you branched is already in the
  tree CI builds. Run `git fetch && git merge origin/develop` locally and run
  `ctest` before pushing, or a hard-coded path in someone else's new test will
  fail all eight runners (that is exactly how `vscode_grammar` broke).
- **When several PRs are open at once, open them in dependency order and
  re-check the earlier ones after the later ones land.** A later PR that touches
  the same lines makes an earlier one `dirty` without any push to it. Re-read
  every open PR's `mergeable` at the end of the session, not only the one just
  created.

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
  the reference, and `std/datetime.oskl` with `tests/native_build_datetime.oskl`
  is a second one (it also exercises the `time()` wall clock, the one builtin
  whose AOT binding differs per target).
- `time()` (epoch seconds, an `int`) is bound as the `now` runtime handle on
  every AOT path: syscall 201 on Linux, `time(NULL)` hosted, `GetSystemTimeAsFileTime`
  on Windows (a kernel32 import), and the CMOS RTC in bare metal. `clock()`
  stays monotonic and has no AOT binding. A `divUnsigned` in the AOT emitters
  zeroes `rdx`, so a divisor must never live there.
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
- A `struct` crossing `extern "C"` by value is supported in the hosted backend
  in both ABI classes (PHASE 7f.7). A struct of 16 bytes or fewer is flattened
  to its System V eightbytes and travels in registers; a larger one is class
  MEMORY, passed in the outgoing stack-argument area and returned through a
  hidden `sret` buffer in `rdi`, which shifts every integer argument one register
  along. The thunk builds the C bytes field by field from the `AotMap` the
  program holds (a struct is a map at run time), reading each field with
  `oskel_member_get`, and rebuilds a returned struct into a fresh map with
  `oskel_member_set`. Both directions stay System V AMD64 only; the frame keeps
  the outgoing area, the `sret` buffer, the register-class staging buffers and
  the integer staging slots disjoint. `tests/native_build_extern_struct.oskl`
  covers the register class and `tests/native_build_extern_memory_struct.oskl`
  the MEMORY class (the latter links `tests/hosted/memory_struct_helper.c`,
  because no libc symbol exercises that shape). A callback or C variadic
  parameter is still PHASE 7f work.
- Windows code paths can be exercised locally without a Windows host: build the
  engine with `x86_64-w64-mingw32-g++ -std=c++17 -O2 -Iinclude -Isrc -static
  -o /tmp/win/oskel.exe src/main.cpp src/core/*.cpp src/frontend/*.cpp
  src/frontend/native/*.cpp src/runtime/*.cpp src/runtime/jit/*.cpp` and run it
  under Wine (`WINEPREFIX=$HOME/.wine-oskel wine /tmp/win/oskel.exe ...`). The
  `tests/*.oskl` suite runs there too. This is how `lookupSymbol` (kernel32) and
  the PE image were checked before CI. CI still gates the merge, since Wine is
  not Windows: a green Wine run is a signal, not a substitute for the
  `windows-latest` job.

## The VS Code extension

- `editors/vscode` holds a dependency-free extension (language id, TextMate
  grammar, `language-configuration.json`, `snippets/oskel.json`, and
  `extension.js` for diagnostics). It has no npm dependency and no build step;
  `vsce package` in that folder is how CI produces the `.vsix`.
- The grammar keeps its **own** keyword, type and builtin lists, because a
  TextMate grammar cannot read the compiler. `tests/check_vscode_grammar.cmake`
  (CTest `vscode_grammar`) derives the vocabulary from the single sources of
  truth — `kKeywords` in `src/lexer.cpp`, the `name == "..."` type names in
  `src/ast.h`, the `addNative` registry in `src/builtins.cpp` — and fails when
  the grammar misses a word, when a `#include` names a missing entry, or when
  the grammar highlights syntax the lexer never produces. Adding a keyword, a
  type or a builtin means updating `syntaxes/oskel.tmLanguage.json` in the same
  change; the test names the missing word, so run
  `cmake -DROOT=. -P tests/check_vscode_grammar.cmake` before opening the PR.
- The same test owns the **operators**, and in both directions. It rebuilds the
  spelling of every operator from the `if (c1 == 'a' ...)` guard that returns it
  in `src/lexer.cpp`, and it takes the assignment set from `isAssignOp` in
  `src/parser.cpp`, so a new `x op= y` cannot reach the compiler while the
  grammar stays silent. The grammar's own rules are reduced to literal spellings
  (character class, `\|`, lookahead, one line per key) before the comparison,
  and if that reduction finds fewer than 15 operators it fails rather than
  passing on an empty set. This is how the compound bitwise forms were caught:
  `&=`, `|=`, `^=`, `<<=` and `>>=` shipped in the compiler while the grammar
  still called them invalid.
- `tests/check_vscode_snippets.cmake` (CTest `vscode_snippets`) expands each
  snippet the way VS Code does and compiles the result with `oskel -c`, wrapped
  in the context the construct needs (a method in a class, `defer` in a
  function). A snippet that emits invalid Oskel fails the suite; four did while
  the check was written — `defer` with a bare statement instead of a block, a
  `switch` whose `case` label was an identifier instead of a literal, and two
  with an empty expression body.
- `$`-prefixed builtins (`$str`, `$slice`, `$range`, `$isError`, `$nonNull`)
  are compiler-private: Oskel source cannot spell them, so the grammar must not
  offer them and the test skips them on purpose.
- **Diagnostics** are the compiler's own, not a language server's. On save (and
  on `Oskel: Check file`) `extension.js` runs `oskel -c <file>` and turns its
  stderr into a `DiagnosticCollection`; `src/diagnostics.js` does the parsing and
  imports no `vscode`, which is why `node --test` can cover it
  (`tests/check_vscode_diagnostics.cmake`, CTest `vscode_diagnostics`, skipped
  where Node is absent). Two traps: the binary must be `oskel`, because
  `oskelc -c` is not implemented and exits 64 — a stale path there would look
  like "no problems", so a non-zero exit with nothing parsed becomes a
  diagnostic of its own; and an error inside an imported module is reported
  under the *root* file's name with the module's line and column, so the parser
  clamps a position that falls past the end of the open document instead of
  dropping it. Keep `package.json` free of a `dependencies` block: the extension
  ships as plain CommonJS.

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
- `oskelc` (the host compiler) is the second entry point over the same front
  end: `oskel` links the runtime on top of it (VM, JIT, builtins, FFI
  marshalling) and `oskelc` links the front end alone, so the shipped compiler
  carries no interpreter. The directory a file lives in decides which library it
  belongs to: `src/core/`
  (Value, cycle collector, platform layer), `src/frontend/` (lexer, parser,
  bytecode compiler, AOT backend) and `src/runtime/` (VM, JIT, builtins,
  operators, FFI marshalling). There is no hand-maintained list, so a new file
  cannot silently default to the front end; the configure step refuses a `.cpp`
  dropped straight into `src/`. `build.sh` and `build.bat` glob the same three
  directories. `oskel build` and `oskelc` both call `runBuildCommand` in
  `src/frontend/cli.cpp`, so they cannot drift: `tests/oskelc_parity.cmake`
  compares the two images byte for byte and against the VM's output, and
  `tests/check_oskelc_no_vm.cmake` fails when `nm` finds `VM::`, `jitCompile`,
  `Jit::`, `registerBuiltins`, `ops::binary` or `Runtime::fail` in the compiler
  **or in `liboskel_frontend.a`** — the archive matters because an unreferenced
  object never reaches the binary, so a binary-only check cannot see it.
  Every packaging format ships `oskelc` beside `oskel` and `bindgen`.
- A check that reads the source tree must name the *basename* of the file it
  wants, never a fixed `src/<name>.cpp` path, and must fail loudly when the file
  is missing. `tests/check_vscode_grammar.cmake` reads the keyword table, the
  type vocabulary, the builtin registry and `isAssignOp` out of the sources so
  the grammar cannot drift; when the split moved those files it failed on all
  eight runners with "missing src/lexer.cpp". The same trap sits in the include
  guards: `check_compiler_boundary.cmake` matched a bare `vm[.]h`, which stops
  matching the moment an include gains a directory prefix — it silently passed a
  front-end file that included `runtime/builtins.h`. Both patterns now take an
  optional directory prefix, and `runtime/runtime.h` is spelled with its
  directory so `include/native/runtime.h` (the AOT freestanding runtime) stays
  allowed. When you move a file, grep for its old path across `tests/`,
  `CMakeLists.txt`, the build scripts and the workflows, then plant a violation
  and watch the check fail before trusting it.
- `bindgen` (the C binding generator, `docs/bindgen.md`) is a second executable
  shipped beside `oskel`. It is standalone — it links no engine code — and its
  sources live under `tools/`, outside the three `src/` globs, so it is never
  pulled into `oskel`; `nm build/oskel | grep bindgen` must stay empty. Every
  packaging format must ship it (`/usr/bin/bindgen`, `/usr/local/bin/bindgen`,
  `{app}\bin\bindgen.exe`) or the three platforms drift apart. It reuses
  `isKnownTypeName`/`isIntegerTypeName` from `src/ast.h` so the vocabulary it
  emits cannot drift from the compiler's; keep that the single source of truth.
  It must never emit a declaration the compiler rejects — an unsupported type
  becomes a `// skipped:` comment, and a C variadic is written commented out.
  Its stdout is byte for byte the same module as its `-o` file, so it is set to
  binary mode on Windows (`_setmode(_fileno(stdout), _O_BINARY)`); a text-mode
  stdout would turn every `\n` into `\r\n` and the piped module would differ
  from the written one. A CMake test cannot see that by capturing stdout with
  `OUTPUT_VARIABLE` — CMake normalizes CRLF to LF there — so a byte-exact
  comparison must go through `OUTPUT_FILE` and `cmake -E compare_files`.

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
- Does the feature need AOT support, or is it VM-only? → the AOT backend
  (PHASE 7) is a first-class target, not an afterthought

Without this checklist complete and approved, **do not proceed to code**.

### VM/AOT parity is part of every feature

Every new feature lands in the **VM and the AOT backend in the same change**.
The development engine (`oskel`) and a compiled binary (`oskel build`) are two
front ends of one language; a feature that works only in the VM is unfinished,
and a feature that works only in AOT is a bug. Concretely:

- A new opcode, builtin or standard-library entry is lowered by the AOT backend
  in the same commit (or, when the backend genuinely cannot support it yet, it
  must **fail cleanly** with a dedicated message and exit 65 — never emit a
  mis-compiled binary and never silently diverge).
- A new standard-library module is written over the portable primitive surface
  so it runs identically on both, unless it deliberately needs a VM-only
  builtin; in that case the gap is recorded in `docs/BACKLOG.md` §6 and the
  module avoids it.
- The tests prove the parity: an AOT fixture (`tests/native_build_*.oskl`,
  built and run by `tests/build_binary.cmake`) compares the compiled output with
  the VM byte for byte, so a divergence cannot ship silently.
- Known VM/AOT divergences live in `docs/BACKLOG.md` §6 and shrink over time;
  do not add a new one without an entry and a reason.

When the feature does break AOT parity, the same change must add the AOT
support (or the clean failure and its fixture). Leaving it for later is not an
option.

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
