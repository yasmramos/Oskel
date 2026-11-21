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
  generic "global 'N' is read but never assigned". `sqrt`, `floor`, `ceil` and
  `round` are the curated stubs (`sqrtsd` and the SSE rounding are exact).
- The compiler validates an `extern "C"` declaration against the **destination**,
  not against how the engine was built: `oskel build` passes
  `ffi::kAotAbiCapabilities` (struct-by-value and class-MEMORY on, variadic off)
  through `Compiler::setAbiCapabilities`, while the interpreter keeps
  `AbiCapabilities{}`, this build's. The backend emits its own thunks, so a
  class-MEMORY struct must not be rejected for `oskel build` just because the
  engine lacks libffi — that conflation made
  `oskel.build_hosted_extern_memory_struct` fail without `libffi-dev` even
  though the emitted code was right. A C variadic stays rejected for the AOT
  destination whatever the build has, because the backend does not lower a
  variable argument list (PHASE 7f.5).
- A qualified access to a module's native export (`std.math.pow`) is a use too,
  but it reads the module table with `GETTABLE`, not with `GETGLOBAL`, so the
  lowerer cannot see it by counting reads. The compiler records such a use on
  the proto (`Bytecode::nativeUses`) and resolves it once the chunk is compiled
  — deferred, because a hoisted function is compiled before the `import` that
  loads the module (PHASE 7f.2). The same record lets a qualified call be checked
  against the export's signature (arity and argument types), exactly like the
  direct form. The argument types are captured at the call site, not at flush
  time: a local that shadows a global is gone by then.
- An emitted helper that calls back into the program (`oskel_call` on a closure
  it was handed) saves only `rbx`/`r12`. `r13`/`r14` are the arena cursor and
  end: the callback allocates, so they must be left advanced by whatever it
  allocated — pushing and popping them rebinds the cursor and a later
  allocation overwrites a returned array. `r15` is the global table and is never
  written. An argument window is built *upward in memory* from its highest
  address: a callee reads `args[i]` at `args - 16*i`, because the register file
  grows down, so `args[0]` sits at the window's highest slot. Getting that
  ordering wrong passes the callback a plausible but wrong value (the callback
  itself, or `nil`) instead of the item, so every fold fixture asserts on both
  a one-item and a many-item receiver.
- `AotRuntimeHandles` (`include/native/runtime.h`) is all `CodeGen::Label`, and
  `newAotRuntimeHandles` fills them with one flat sweep guarded by a
  `static_assert` on the struct size. Adding a handle used to mean bumping a
  positional `{...}` initialiser by hand; a short one silently left the new
  labels unbound and `oskel build` failed with "codegen: binding an unknown
  label". A new non-label field must trip the `static_assert`, not shift every
  handle by one.
- The AOT backend has two link modes (`LinkMode`, PHASE 7f.3/7f.4), chosen on
  the command line and never in the source: `freestanding` (the default) is the
  self-contained image every target emits today (no libc, no linker, `ldd` says
  "not a dynamic executable"); `hosted` links against the host C library so an
  `extern "C"` symbol resolves to the real C function — the mode that makes FFI
  work. `isSupportedLinkMode` pins the matrix (freestanding: Linux, Windows and
  bare metal; hosted: Linux, Windows and macOS x86-64) and an unsupported pair is
  reported, never emitted. macOS has no freestanding image, so there the default
  `freestanding` mode is reported as unsupported and `--mode hosted` is
  required. A change to the freestanding path must keep the emitted binary byte
  for byte identical; `oskel build --mode freestanding` is the explicit form.
- A standard-library module that must work on both engines stays on the
  **portable primitive surface**: indexing and slicing (`s[i]`, `s[a:b]`),
  `len`, `ord`/`chr`, `int`/`double`/`str`/`type`, `panic`, `for ... in`,
  closures, variadic `rest...` and the operators. The AOT backend binds the
  array methods (`push`, `pop`, `shift`, `unshift`, `insert`, `removeAt`,
  `slice`, `concat`, `join`, `contains`, `indexOf`, `map`, `filter`, `each`,
  `find`, `any`, `all`, `reduce`, `sum`, `reversed`, `clone`, `clear`,
  `sort`, `sorted`), the ten map methods and every string method
  (`upper`, `lower`, `trim`, `trimStart`, `trimEnd`, `contains`, `startsWith`,
  `endsWith`, `indexOf`, `substr`, `reversed`, `repeat`, `split`, `lines`,
  `chars`, `replace`, `replaceFirst`, `padStart`, `padEnd`), so no string
  member builds and then faults at run time. It does not seed
  `pcall`/`error`/`readFile` at all, so a module function that calls one fails
  `oskel build` with "global 'pcall' is read but never assigned" (exit 65) even
  when the function is unused. A module written over the portable surface
  behaves identically on the VM and under `oskel build`; `std/json.oskl` and
  `tests/native_build_json.oskl` are the reference, and `std/datetime.oskl`
  with `tests/native_build_datetime.oskl` is a second one (it also exercises
  the `time()` wall clock, the one builtin whose AOT binding differs per
  target).
- `sort`/`sorted` are the only collection methods whose comparator is arbitrary
  program code, so their AOT body calls back through `oskel_call` from inside a
  bottom-up stable merge sort (a recursive one would deepen the program's stack
  by log2(n) frames). The algorithm must stay *stable*: the VM sorts with
  `std::stable_sort`, so an unstable sort would reorder equal keys and diverge
  on any comparator that treats distinct items as equal. Both forms sort a
  *copy* and commit only on success — `sort` copies the sorted block back over
  the receiver and returns it, `sorted` returns the new array — so a comparator
  that raises leaves the receiver untouched, exactly as in the VM. The auxiliary
  block is allocated once before the first callback, because a callback
  allocates and the arena cursor must not be snapshotted across it.
- `time()` (epoch seconds, an `int`) is bound as the `now` runtime handle on
  every AOT path: syscall 201 on Linux, `time(NULL)` hosted, `GetSystemTimeAsFileTime`
  on Windows (a kernel32 import), and the CMOS RTC in bare metal. `clock()` is
  bound too, as the separate `monotonic` handle (`clock_gettime(CLOCK_MONOTONIC)`,
  libc, `GetTickCount64`, the PIT tick counter), and the chunk subtracts the
  value it captured at entry, so it reports elapsed time -- not the VM's CPU
  time, but the same observable shape (a non-negative, non-decreasing double).
  A `divUnsigned` in the AOT emitters zeroes `rdx`, so a divisor must never live
  there. A thunk that reaches a Windows API with five or more arguments must
  keep its saved state *above* the outgoing stack slots (`[rbp-16]` in a 72-byte
  frame), never at `[rbp-48]`, and must read a length out of `rdx` before reusing
  `rdx` for `lpBuffer` -- both were real bugs in the file thunks.
- The hosted image is an object plus a link (PHASE 7f.4/7f.9), not a closed
  binary the emitter writes. `native/object.h` emits the target's container:
  ELF64 `ET_REL` on Linux (`.rela.text`, `R_X86_64_PLT32` for a call and
  `R_X86_64_PC32` for an address, addend `-4`), Mach-O `MH_OBJECT` on macOS
  (`LC_SEGMENT_64` with `__text`/`__const`, `LC_BUILD_VERSION`, `LC_SYMTAB`,
  pcrel `X86_64_RELOC_BRANCH`/`X86_64_RELOC_SIGNED`, underscore-prefixed names)
  and COFF `IMAGE_FILE_MACHINE_AMD64` on Windows (`.text`/`.rdata`,
  `IMAGE_REL_AMD64_REL32`, the 18-byte symbol table and its string table). The
  `.symtab`/COFF symbol table exports the program's `main`; `emitHostedRuntime`
  binds the same entry handles as the freestanding prelude but reaches the
  system through libc (`write`/`exit`/`malloc`) on Linux and macOS, while
  `emitWindowsHostedRuntime` uses the UCRT's `_write`/`_read`/`_open`/`_close`
  and `GetTickCount64`. `oskel build --mode hosted` then invokes the host C
  compiler: `$CC` (default `cc`) with `-no-pie -lm` on Linux, `-arch x86_64
  -lm` on macOS and `cl /Fe:... /link /SUBSYSTEM:CONSOLE` on Windows, writing
  the temporary object beside the output and removing it either way. This makes
  a C toolchain a *build-time* dependency of the hosted mode only; the
  freestanding path still needs no toolchain and emits no relocation. The
  extern thunk takes the ABI as a parameter: System V (Linux, macOS) keeps the
  six GPRs/eight SSE registers and the struct/callback/variadic surface;
  Win64 uses four of each, 32 bytes of shadow space and stack arguments from
  offset 32, supports the scalar/`cstring`/`ptr` classes and rejects a struct,
  a callback or a C variadic with a dedicated message. `main` on Windows moves
  the CRT's `(argc, argv)` from rcx/rdx to rdi/rsi before the chunk so `args()`
  works there too. The UCRT opens stdout/stderr in text mode, so
  `oskel_init_arena` calls `_setmode(1/2, _O_BINARY)` before the first write;
  without it every `\n` becomes `\r\n` and the hosted binary diverges from the
  VM. `tests/hosted_cc.cmake` wraps `cl` in a batch file that calls
  `vcvarsall.bat` first, because CTest does not inherit the `PATH`/`INCLUDE`/
  `LIB` the build step had -- a bare `cl` link fails with "cannot open
  libcmt.lib" even when `cl.exe` is found.
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
- A builtin whose argument window the AOT backend walks by hand has to read
  `args[i]` **downward**: `slotOffset` is `-16 * (index + 1)`, so `args[1]` sits
  16 bytes *below* `args[0]` (`-16` tag, `-8` payload), not at `+16`/`+24`. A
  `+16` read passes a smoke test whenever the two arguments happen to be equal
  and fails as "bound must be positive" otherwise — `random(low, high)` was
  shipped with exactly that bug.
- A fixed-arity builtin has to check `argc` (the `rsi` the entry receives)
  **before** it reads `args[0]`, and reject any other count. The window grows
  down from `args`, so `argc == 0` means the slot `args[0]` names was never
  written: reading it is an out-of-bounds read of the caller's frame, not a
  benign default. Eight builtins shipped without the check — `len`, `type`,
  `str`, `repr`, `bool`, `isError`, `ord`, `chr` — so `type()` returned a type
  name built from whatever happened to sit there (`int` after a four-`int` call)
  and `chr(65, 1)` returned `"A"` while the VM raised "expects 1 argument".
  The VM's `need(in, name, args, 1)` is the counterpart; a new builtin gets the
  guard in the same commit. The message keeps only its fixed prefix in AOT
  (`type() expects 1 argument`), which is the documented convention for a tail
  that would need a per-message builder (BACKLOG §6 item 25);
  `tests/native_build_builtin_arity.oskl` drives every wrong-count call through
  `pcall`, because a raised runtime error is not a value and `try`/`catch` never
  sees it.
- `random`/`seed` are AOT-backed (PHASE 7g): `mt19937_64` is emitted as machine
  code, so a native binary reproduces the VM's stream after the same `seed(n)`.
  Three rules keep it correct, and each was a real bug first: only `rbx` and
  `r12` survive the runtime calls (never `r13`/`r14`/`r15` — the arena cursor
  and the global base live there); every helper keeps `rsp` 16-byte aligned at
  its `call`s, including a thunk that just forwards; and `divUnsigned` leaves the
  remainder in `rdx`, so an integer draw adds the low bound to `rdx` rather than
  reloading the quotient. The 312-word state is a hidden global the lowerer
  reserves only when the program mentions `random`/`seed`, so a program that
  does not keeps its byte-for-byte binary. `tests/native_build_random.oskl`
  compares VM and native output line for line. The argument-error messages
  (`bound must be positive`, `needs an upper bound greater than the lower bound`,
  `expects between 0 and 2 arguments`, `expects an integer argument`) are
  matched to the VM's but cannot go in that fixture: a raised runtime error is
  not a value, so `try`/`catch` does not capture it and `build_binary.cmake`
  requires exit 0 from both engines.
- A C callback (`fn(...): R` parameter) is a generated stub in the hosted
  backend (PHASE 7f.8), not a libffi closure. `oskel_callback_create` allocates
  a 32-byte context (closure tag/payload, the Oskel global base, a pointer to a
  16-byte shared arena-state block) and a private `mmap` page holding
  `movabs r10, context; movabs r11, body; jmp r11`. The page is mapped
  read+write only while the stub is copied in, then `mprotect`ed to
  read+execute (W^X); it is never writable and executable at the same time.
  The body saves the C arguments, boxes `ptr`/`cstring`, calls the closure
  through `oskel_call` and
  converts the result to the declared C return class. Three rules were bugs
  first: a C caller expects r13/r14/r15 preserved, so the body saves them, loads
  the Oskel arena cursor/end and global base from the context/state, runs, parks
  the advanced cursor/end back and restores the C values; the stage base must
  clear the five saved registers, or the SSE slots overwrite the saved r15; and
  every extern thunk in a program with a callback publishes the current
  cursor/end before the C call and reloads it afterwards (the root epilogue
  publishes the final state), so a callback C stores for a later call resumes
  from the program's current arena. The global table is an array of 16-byte
  `AotValue` slots, so the state pointer lives at `16 * slot + 8`; using
  `8 * slot` read the wrong slot and crashed. A callback returning a struct by
  value and a callback with a struct parameter are rejected with dedicated
  messages; a stored callback works in AOT but not in the VM (the VM frees the
  libffi closure when the extern call returns), and both are BACKLOG §6 items
  26/27. The stub page is process-lifetime: C may store the pointer (a stored
  callback, including an `atexit` handler), so freeing it when the extern call
  returns would break that contract; BACKLOG §6 item 28 tracks a pool or an
  explicit release contract. Tests: `tests/native_build_extern_callback.oskl`
  (`qsort`, VM parity) and
  `tests/native_build_extern_callback_{stored,exit,wx,many_args,returns}.oskl`
  (AOT-only, `tests/hosted/*_helper.c`), plus
  `tests/errors/callback_struct_{return,param}.oskl`.
- An `extern "C"` return class has a *box* per class, and adding one means
  adding its box: the thunk calls a helper that turns the raw register contents
  into an `AotValue`. `cstring` had one (`emitCStringBox`) and `ptr` did not, so
  `getenv(name: cstring): ptr` — the natural shape of an accessor — was rejected
  at build time while `strchr` returning a `cstring` worked. A `ptr` return
  boxes as an `AotPtr` of kind `Void` whose `data` is the address (what
  `intToPtr` builds), and **NULL must map to `nil`, not to a `Void` handle of
  zero**: `wrapPointerValue` in the hosted engine returns an empty `Value`
  before it reads the type name, so `type()` says `"nil"` there and must say the
  same here. `tests/native_build_extern_ptr_return.oskl` pins both halves, and
  `memset` through a handle rebuilt by `ptrToInt`/`intToPtr` is what proves the
  payload is the real address rather than a synthetic token — a box that stored
  an invented id would pass every `type`/`== nil` assertion and fail only when C
  dereferences it.
- Windows code paths can be exercised locally without a Windows host: build the
  engine with `x86_64-w64-mingw32-g++ -std=c++17 -O2 -Iinclude -Isrc -static
  -o /tmp/win/oskel.exe src/main.cpp src/core/*.cpp src/frontend/*.cpp
  src/frontend/native/*.cpp src/runtime/*.cpp src/runtime/jit/*.cpp` and run it
  under Wine (`WINEPREFIX=$HOME/.wine-oskel wine /tmp/win/oskel.exe ...`). The
  `tests/*.oskl` suite runs there too. This is how `lookupSymbol` (kernel32) and
  the PE image were checked before CI. CI still gates the merge, since Wine is
  not Windows: a green Wine run is a signal, not a substitute for the
  `windows-latest` job.

## The WebAssembly backend (PHASE 9)

- `oskel build --target wasm32-freestanding-wasm` emits a `.wasm` module. The
  target is `Target::wasm32()` (`Arch::Wasm32`, `Os::Freestanding`, `Abi::Wasm`)
  and the backend is `include/native/wasm.h` + `src/frontend/native/wasm.cpp`.
  `isWasmTarget()` routes it in `src/frontend/cli.cpp`; `isSupportedTarget()`
  deliberately stays x86-64-only, because `requireSupportedTarget` guards the
  x86-64 emitter.
- The module imports `oskel.write(fd, ptr, len) -> i32`, `oskel.exit(code)`,
  `oskel.time() -> i64` and `oskel.clock() -> f64`, and exports `_start`.
  `tests/run_wasm.cjs` is the Node host that supplies all four; the tests are
  skipped where Node is absent.
- The runtime helpers (`print`, `toStr`, `repr`, the operators, iteration,
  member access, the builtins) are ordinary WASM functions emitted by
  `Runtime` in the same file, not a hand-written blob. Registers are `(i64 tag,
  i64 payload)` local pairs; the globals table is 16 bytes per slot at offset 0,
  followed by the static pool and the bump arena.
- The collection half of the language is supported too: `NEWTABLE` creates an
  array or a map, `GETTABLE`/`SETTABLE` dispatch to `indexGet`/`indexSet`
  (array/string/map) or `memberGet`/`memberSet` (error fields, map fields,
  `length`), `for ... in` walks arrays and maps (with the key/value pair), `in`
  covers both, `==` is structural (with the cycle-safe visited list), and a
  variadic proto packs its extras into an array bound to the last parameter.
  `$slice` and the string-to-number forms of `int`/`float`/`double` are bound,
  so `std/json`, `std/datetime` and `std/collections` build and match the VM
  byte for byte; `time()`/`clock()` read the host imports.
- A construct the backend cannot lower yet must fail cleanly, never emit a
  mis-compiled module: an unbound builtin is seeded as a trapping closure, and
  a used `extern "C"`/`native fn` symbol with no stub is a build error (exit
  65). `tests/native_build_wasm.oskl` pins the supported surface against the VM
  byte for byte.
- `double` printing is the same shortest round-trip form `std::to_chars`
  produces, not a fixed-point approximation: `fmtDouble` generates 18
  significant digits by scaling with the 633-entry `pow10` table
  (`1e-324`..`1e308`, built with `strtod` so every entry is the correctly
  rounded double), rounds the 18th into the 17th, then tries every shorter
  precision and re-parses the candidate with `parseDoubleStr`. It emits the
  shorter of fixed and scientific (fixed on a tie), and handles `-0`,
  subnormals and `e±NN`. `parseDoubleStr` combines the fraction into one
  `mantissa / scale` division (a term-by-term sum rounds twice and accepts a
  15-digit prefix that does not round-trip) and applies the exponent with one
  table multiply/divide. `tests/native_build_double_format.oskl` pins the
  whole range and `tests/native_build_math.oskl` pins `PI`/`E`/`sqrt`.

## SDL3 bindings (`std/sdl.oskl`)

- The module binds **SDL3**, not SDL2: `SDL_CreateWindow(title, w, h, flags)`,
  `SDL_CreateRenderer(window, name)`, `bool` predicates, `Uint64` window flags
  and `SDL_FRect` (floats). The `extern "C"` block is generated by `bindgen`
  from `bindings/sdl/sdl3_subset.h`; regenerate it rather than editing it.
  `sdl.bindgen` compares the block against a fresh `bindgen` run, so a header
  edit that was not regenerated fails the suite.
- `SDL_Event` is a union, so it stays an opaque `ptr` and the helpers read a
  128-byte buffer at fixed offsets. `tests/sdl_bindings.oskl` pins them by
  pushing a synthetic event of each kind.
- `SDL_FRect` and the event coordinates are encoded in Oskel
  (`storeF32`/`loadF32`), not with a builtin, so the VM and the AOT backend
  agree by construction and `check_builtin_parity` needs no entry.
- The development engine links `SDL3::SDL3` when CMake finds it, with
  `-Wl,--no-as-needed` on Linux, because `dlsym(RTLD_DEFAULT)` only sees a
  loaded shared object and `--as-needed` would drop a library nothing references
  at link time. `build.sh` probes `pkg-config --exists sdl3`. `oskelc` does not
  link it.
- A native binary uses `oskel build --mode hosted --link-lib SDL3`;
  freestanding rejects a used `SDL_*` with the dedicated message and exit 65.
  `--link-lib` requires hosted, is repeatable and accepts only
  `[A-Za-z0-9_+.-]`.
- The SDL tests need `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy`
  (`tests/run_sdl_case.cmake` sets them). `sdl_bindings.oskl` is skipped by the
  `tests/*.oskl` glob and registered explicitly, because the environment has to
  be set and a build without SDL3 must skip it. The freestanding-rejection
  fixture is skipped too: it is a `oskel build` fixture, not a VM test.
- Windows `lookupSymbol` walks every loaded module with
  `CreateToolhelp32Snapshot`/`GetProcAddress`, so a third-party DLL the process
  loaded resolves. The Windows-only `platform_lookup_test` loads a DLL with
  `LoadLibrary` and resolves an export only it provides.
- The FFI accepts up to **eight** register-sized values (`kMaxExternArgs`).
  The VM's typed `dispatch` lets the C++ compiler spill the seventh and eighth
  GPR arguments; the AOT thunk writes them to the outgoing stack area
  (`AotThunkParam::stackOffset`). `XmmScratch` is xmm15 so it never collides
  with an SSE argument, and a register-class struct that no longer fits the
  remaining registers is passed wholly on the stack. `SDL_ReadSurfacePixel` and
  `SDL_WriteSurfacePixel` are bound, and
  `tests/native_build_extern_many_args.oskl` pins the 7/8-argument and
  stack-struct shapes. A ninth value needs a wider bound and a check on the
  VM's `3^N` template budget; MSVC also needs `/bigobj` (already in
  `OSKEL_WARNINGS`) at the eight-eightbyte bound.

## The VS Code extension

- `editors/vscode` holds a dependency-free extension (language id, TextMate
  grammar, `language-configuration.json`, `snippets/oskel.json`, and
  `extension.js` for diagnostics). It has no npm dependency and no build step;
  `vsce package` in that folder is how CI produces the `.vsix`.
- The grammar keeps its **own** keyword, type and builtin lists, because a
  TextMate grammar cannot read the compiler. `tests/check_vscode_grammar.cmake`
  (CTest `vscode_grammar`) derives the vocabulary from the single sources of
  truth — `kKeywords` in `src/frontend/lexer.cpp`, the `name == "..."` type names in
  `src/frontend/ast.h`, the `addNative` registry in `src/runtime/builtins.cpp` — and fails when
  the grammar misses a word, when a `#include` names a missing entry, or when
  the grammar highlights syntax the lexer never produces. Adding a keyword, a
  type or a builtin means updating `syntaxes/oskel.tmLanguage.json` in the same
  change; the test names the missing word, so run
  `cmake -DROOT=. -P tests/check_vscode_grammar.cmake` before opening the PR.
- The same test owns the **operators**, and in both directions. It rebuilds the
  spelling of every operator from the `if (c1 == 'a' ...)` guard that returns it
  in `src/frontend/lexer.cpp`, and it takes the assignment set from `isAssignOp` in
  `src/frontend/parser.cpp`, so a new `x op= y` cannot reach the compiler while the
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
  dropping it. A third trap is the **unit**: the compiler counts its column in
  UTF-8 bytes and a VS Code `Range` counts UTF-16 code units, so the two agree
  only on ASCII — an accent or an emoji earlier in the line moves the squiggle
  off the token by two positions either way. `byteColumnToUtf16` converts
  against the document text, which is why `parseDiagnostics` takes the source as
  well as the output; do not "simplify" it back to a bare column.
- A programmatic `document.save()` fires `onDidSaveTextDocument`, and whether
  that event arrives before or after the promise resolves is **not documented**.
  A flag cleared in `finally` therefore suppresses it in one ordering only; in
  the other, "Save and check" starts two compiler processes and the second kills
  the first. `src/save-guard.js` counts the expected events instead, so the
  ordering stops mattering, and a save that throws cancels its own expectation.
  Both orderings are unit-tested — a stub that fires the event synchronously
  only ever exercises the one that happens to work.
- `check` is async and its callers are event handlers VS Code does not await, so
  every entry point goes through `checkSafely`; otherwise a rejection is visible
  only as an unhandled promise in the extension host log.
- Keep `package.json` free of a `dependencies` block: the extension ships as
  plain CommonJS.

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
  C++; the native backend emits x86-64 (Linux, Windows and hosted macOS, plus
  bare metal), and its execution tests run only where that image can run. The
  macOS arm64 runner builds the hosted Mach-O image and checks its libSystem
  dependency, but skips running it (that needs Rosetta).
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
  Every packaging format ships `oskelc` beside `oskel`, `oskelp` and `bindgen`.
- **A builtin must exist in both engines.** `tests/check_builtin_parity.cmake`
  reads `addNative(...)` out of `src/runtime/builtins.cpp` and the `builtins`
  map out of `src/frontend/native/lower.cpp` and compares the name sets. Each
  engine is self-consistent, so a builtin only the VM has is invisible to every
  other test: it runs in `oskel` and fails `oskel build` with "global 'X' is
  read but never assigned". The names that are VM-only on purpose live in a list
  in that script and in BACKLOG §6 item 25; the check also fails when a listed
  exception stops being one, because a stale exception hides the next real gap.
  The list is **empty** today: BACKLOG §6 item 25 landed the pointer, file,
  `clock` and `args` builtins, so every name `addNative` registers is seeded by
  the AOT backend on at least one target. Four names are seeded per target
  rather than everywhere, and each has a lowerer gate with its own message: the
  file builtins and `input` on anything but bare metal, `args` where the entry
  receives an argument vector (Linux freestanding and hosted), and `memSize`
  which only bare metal backs. The gate matters because the fallback message --
  "global 'X' is read but never assigned" -- is what a *typo* produces, so a
  program that spelled a real builtin correctly must not see it. A `native fn`
  used as a rejection fixture therefore has to be a symbol the backend has *no*
  binding for (`getpid`, not `clock`) -- `clock` became a builtin and stopped
  working as that fixture.
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
  `isKnownTypeName`/`isIntegerTypeName` from `src/frontend/ast.h` so the vocabulary it
  emits cannot drift from the compiler's; keep that the single source of truth.
  It must never emit a declaration the compiler rejects — an unsupported type
  becomes a `// skipped:` comment, and a C variadic is written commented out.
  Its stdout is byte for byte the same module as its `-o` file, so it is set to
  binary mode on Windows (`_setmode(_fileno(stdout), _O_BINARY)`); a text-mode
  stdout would turn every `\n` into `\r\n` and the piped module would differ
  from the written one. A CMake test cannot see that by capturing stdout with
  `OUTPUT_VARIABLE` — CMake normalizes CRLF to LF there — so a byte-exact
  comparison must go through `OUTPUT_FILE` and `cmake -E compare_files`.
- `oskelp` (the project and package manager, `docs/oskelp.md`) is a third
  standalone executable under `tools/`. It links no engine code and drives
  `oskel`/`oskelc` as child processes; `nm build/oskel | grep oskelp` must stay
  empty. Every packaging format must ship it (`/usr/bin/oskelp`,
  `/usr/local/bin/oskelp`, `{app}\bin\oskelp.exe`). The only compiler-side hook
  is `OSKEL_MODULE_ROOT`, honoured by `resolverFor`, which makes a package's
  `src/` the module root for both engines. `oskelp` generates `oskel.lock`,
  `target/` and `.oskel/deps/`; the last two belong in `.gitignore`, while
  `oskel.lock` is committed. Dependencies are either `path` tables or a
  version requirement string resolved from the **local registry** named by
  `OSKEL_REGISTRY` (`<root>/<name>/<version>/`, holding `oskel.toml` and
  `src/`); a network registry is still BACKLOG §20 work. The semver parser
  accepts an exact version, `^`, `~`, `>=`/`>`/`<=`/`<`/`=`, `*`/`x`
  wildcards and a comma/space-separated conjunction; a prerelease is selected
  only by an exact requirement for it. The lockfile records
  `registry+<name>@<version>` and a normal build prefers that pin while it
  still satisfies the manifest, so `oskelp update [<name>]` is the explicit
  upgrade. The namespace is flat and the resolver does not backtrack: two
  incompatible requirements for one name are an error, not a silent pick.
  `oskelp add` and `oskelp remove` edit `[dependencies]` as text, never by
  rewriting the manifest: comments, blank lines and CRLF survive, a duplicate
  add is a no-op (a new `--version` for the same path updates the entry), and a
  failed add leaves the file untouched because the graph is resolved before
  the write. `add` without `--path` writes a registry requirement.
  `remove` refuses an entry that spans multiple lines instead of deleting a
  partial one. `oskelp publish [--registry <dir>]` copies the manifest and
  `src/` into the registry and refuses to overwrite a version or publish a
  package that keeps a path dependency. A registry dependency has no `path`,
  so `oskelp tree` must load each child from the **resolution**
  (`ResolvedPackage::dir`), never from `manifest.dir / dependency.path`; the
  latter reads the root manifest again and prints a bogus `(cycle)`.
  `oskelp build` uses
  `oskelc` (falling back to `oskel build`), `oskelp run` uses the VM unless
  `--native` is given, and `oskelp test` runs every `tests/**/*.oskl` and exits
  101 when one fails. `tests/run_oskelp_case.cmake` drives the real binary
  through the whole workflow, including publish/consume/update/`--locked`, so a
  change to the manifest, the lockfile or a command must keep that test green.

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
  the VM byte for byte, so a divergence cannot ship silently. The harness passes
  the same `-DOSKEL_ARGS` vector to both runs, so a fixture can assert `args()`
  content rather than only its shape -- one element with a space in it is the
  case that catches a naive tokeniser.
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
