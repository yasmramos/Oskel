# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is `0`, the language surface and the bytecode contract
may change between minor versions.

## [Unreleased]

## [0.2.0] - 2026-11-20

### Added

- **Every builtin now exists in both engines.** Fourteen builtins ran only in
  the development engine: a program that called one built fine on the VM and
  failed `oskel build` with "global 'X' is read but never assigned" (exit 65).
  They are implemented in the AOT backend now, so the VM/AOT parity list in
  `tests/check_builtin_parity.cmake` is empty. The pointer builtins (`cstring`,
  `cstr`, `buffer`, `byteLen`, `loadByte`, `storeByte`, `intToPtr`, `ptrToInt`,
  `free`) back an `AotPtr` in the arena with the same four fields as the hosted
  `PtrObj`, and need nothing from the host, so every target emits them. The
  file builtins (`readFile`, `writeFile`, `fileExists`) are written against four
  prelude handles (`oskel_open`, `oskel_read_fd`, `oskel_write_fd`,
  `oskel_close`), so the Linux syscalls, the kernel32 calls and the hosted libc
  path stay in the prelude. `clock()` reads a new `monotonic` handle
  (`clock_gettime(CLOCK_MONOTONIC)`, libc, `GetTickCount64`, the bare-metal PIT
  counter) and subtracts the origin captured at entry, so it reports elapsed
  time; `args()` reads a hidden global the chunk fills from the `(argc, argv)`
  the process entry passes. A target that cannot back one of these now says so
  by name ("args() needs a process argument vector", "readFile() needs a file
  system", "input() needs a keyboard") instead of the unassigned-global error a
  typo produces. Three parity
  fixtures pin the result (`native_build_pointers.oskl`,
  `native_build_files.oskl`, `native_build_clock.oskl`, and
  `native_build_args.oskl` where the entry receives an argument vector), each
  compared with the VM byte for byte. Two latent bugs in the Windows prelude are fixed with
  them: the file thunks stored their saved path over the fifth outgoing argument
  and reused `rdx` for `lpBuffer` before reading the length out of it.

- **The AOT backend binds the callback-taking and folding array methods**
  (PHASE 7c.3h). `map`, `filter`, `each`, `find`, `any`, `all`, `reduce`,
  `sum` and `join` now work in an `oskel build` binary, closing the last
  array-method divergence between the VM and a compiled program except
  `sort`/`sorted` (docs/BACKLOG.md §6 item 1). The callback ones are the first
  emitted helpers that call back into the program: each invokes the closure it
  was handed through `oskel_call`, once per item, so a callback runs real
  Oskel code — allocating, recursing, capturing locals — inside the helper.
  `reduce` is the first to hand its callback two values, so the argument
  window is built with the accumulator at its highest address (a callee reads
  `args[i]` at `args - 16*i`, because the register file grows down); getting
  that backwards passed the callback the closure itself instead of the item.
  `sum` follows the hosted all-int fast path and switches to a binary64 total
  at the first float, and `join` renders both the items and the separator
  through `$str`, since the hosted `join` calls `toString` on whatever
  separator it is given. Covered by the `oskel build` fixtures
  `tests/native_build_array_callbacks.oskl` and
  `tests/native_build_array_folds.oskl`, which compare the VM's output with
  the binary's byte for byte.
- **The VS Code extension reports the compiler's diagnostics.** Saving an
  `.oskl` file now runs `oskel -c` and publishes the result as editor
  diagnostics, so a type error is a squiggle on the line that caused it instead
  of a message in a terminal. `Oskel: Check file` runs the same check on demand
  and `Oskel: Clear diagnostics` clears the Problems panel; three settings
  (`oskel.check.enable`, `oskel.check.onSave`, `oskel.check.path`) control it.
  There is no language server: the compiler already prints
  `file:line:col: severity: message` and exits 65, which is what a
  `DiagnosticCollection` needs, and a check costs about 2 ms. The binary is
  `oskel`, not `oskelc`, whose `-c` is not implemented — a non-zero exit with
  nothing parsed becomes a diagnostic of its own rather than a silent "no
  problems". An error inside an imported module is reported under the root
  file's name with the module's line and column, so a position past the end of
  the open document is clamped instead of dropped. `src/diagnostics.js` holds
  the parser and imports no `vscode`, so `node --test` covers it
  (`tests/check_vscode_diagnostics.cmake`, CTest `vscode_diagnostics`), with
  real compiler output as fixtures. Two editor-side races are covered by the
  same suite: a programmatic save is suppressed by counting the expected
  `onDidSaveTextDocument` events (`src/save-guard.js`) rather than clearing a
  flag in `finally`, because the event's ordering relative to `document.save()`
  resolving is not documented and a flag suppresses only one of the two; and a
  failing check is reported to the user instead of becoming an unhandled
  promise in the extension host log. The compiler counts its column in UTF-8
  **bytes** while a `Range` counts UTF-16 units, so the two disagree on any
  line with an accent or an emoji before the error; the column is converted
  against the document text, which is why `parseDiagnostics` takes the source
  as well as the output.

- **Compound bitwise assignment: `&=`, `|=`, `^=`, `<<=` and `>>=`.** The
  assignment operators had an arithmetic form (`+=`, `-=`, `*=`, `/=`, `%=`,
  `**=`) and a short-circuiting logical form (`&&=`, `||=`, `??=`), but the
  five bitwise operators had none, so `x &= 3` failed in the parser with
  "expected an expression, found '='" while the shipped VS Code grammar already
  highlighted it. `x op= y` now means exactly `x = x op y` on a variable, an
  index or a member target: it reuses `AssignExpr` with a new token, emits no
  new opcode (the compiler maps each token to the existing `BAND`/`BOR`/`BXOR`/
  `SHL`/`SHR`, so the budget stays 39/40) and inherits the binary operator's
  rules — `int` operands only, shift count 0..63. The AOT backend already lowers
  those five opcodes, so the feature is in the VM and in `oskel build` at once;
  `tests/native_build_compound_ops.oskl` pins the parity and
  `tests/compiler_test.cpp` pins the emitted shape (`GETGLOBAL; LOADK; op;
  SETGLOBAL`).

- **The VS Code extension gains snippets, editor defaults and a drift test.**
  The grammar was only a highlighter; the extension now also ships 26 snippets
  (`fn`, `class`, `struct`, `switch`, `extern "C"`, `@MustUse`, the three
  `import` forms, `catch`, `defer`, ...), a `snippets` contribution, and
  per-language editor defaults (`[oskel]`: two spaces, no indentation guessing,
  bracket-pair guides). `language-configuration.json` gained the indentation
  rules (a line opening `{`/`(`/`[` indents, a closing bracket dedents) and
  block-comment continuation, so pressing Enter inside `/* ... */` keeps the
  comment. `tests/check_vscode_grammar.cmake` (CTest `vscode_grammar`) derives
  the keyword, type, builtin and operator vocabulary from `src/lexer.cpp`,
  `src/ast.h`, `src/builtins.cpp` and `isAssignOp` in `src/parser.cpp`, and
  `tests/check_vscode_snippets.cmake` (CTest `vscode_snippets`) expands every
  snippet and compiles it with `oskel -c`, so neither the grammar nor the
  snippets can drift from the compiler again.

- **`oskelc`, the host compiler, split out of `oskel`.** The toolchain now
  builds two binaries from one front end: `oskel` (the development engine: run,
  REPL, `-c`, `build`) links the runtime on top of it, and `oskelc` links the
  front end alone, so the compiler you ship carries no VM, no JIT and no
  builtins. `oskel build` stays as a thin alias: both entry points call
  `runBuildCommand` in the new `src/cli.cpp`, so their flags, defaults,
  diagnostics and exit codes cannot drift, and the emitted image is byte for
  byte identical (`tests/oskelc_parity.cmake`). The sources are split by
  directory (see the entry below); three guards keep the boundary: the include
  graph of every front-end source (`tests/check_compiler_boundary.cmake`, which
  CMake feeds the exact source list), the archive `oskelc` links
  (`tests/check_oskelc_no_vm.cmake`: `nm` must show no `VM::`, `jitCompile`,
  `Jit::`, `registerBuiltins`, `ops::binary` or `Runtime::fail` in either the
  binary or `liboskel_frontend.a`) and the configure step, which refuses a
  source outside the three directories. `oskelc` ships in every package beside
  `oskel` and `bindgen`. `-c` (`.oskc`) and `-pkg` (`.oskpkg`) are refused by
  name with exit 64 until a bytecode serializer exists.

- **The engine's sources are split into `src/core/`, `src/frontend/` and
  `src/runtime/`, and the boundary is enforced in both directions.** The
  toolchain boundary used to be a hand-maintained `OSKEL_RUNTIME_SOURCES` list
  whose check only ran one way: it failed when a listed runtime file did not
  exist, but a new runtime file that nobody listed silently defaulted to the
  front end and was linked into `oskelc`. The directory a file lives in is now
  its classification — `core` (Value, cycle collector, platform layer),
  `frontend` (lexer, parser, bytecode compiler, AOT backend) and `runtime` (VM,
  JIT, builtins, operators, FFI marshalling) — so there is no list to forget,
  and the configure step fails on a `.cpp` dropped straight into `src/`.
  `src/native/` moved to `src/frontend/native/`, and every include now names its
  directory (`core/value.h`, `runtime/vm.h`, ...), which removes the silent
  resolution order that let `value.h` and `native/value.h` both answer to the
  same spelling. The `nm` check now inspects `liboskel_frontend.a` as well as
  the binary: a static library only pulls in the objects a reference needs, so
  an unreferenced runtime object never reaches `oskelc` and a binary-only check
  could not see it. `build.sh`, `build.bat` and the `tests/run_*.sh` scripts
  glob the same three directories. No behaviour changes; the emitted images stay
  byte for byte identical.

- **`bindgen` translates a `struct` by value and a C `enum` (PHASE 6.6).** A
  header that declares `struct Point { int x; int y; };` and
  `struct Point make_point(int x, int y);` now produces the matching
  `struct Point { x: int32; y: int32; }` before the `extern "C"` block plus
  `fn make_point(x: int32, y: int32): Point;`, so a struct crosses the boundary
  by value instead of being skipped. An anonymous body
  (`typedef struct { int quot; int rem; } div_t;`) takes the typedef's name,
  which is the only name the header gives it. The layout is validated with
  `ffi::buildStructLayout`, the same check the runtime applies, so a body Oskel
  cannot represent — a union, an array field, a bitfield, a nested struct — is
  reported as `// skipped:` with its real reason and never emitted with a wrong
  layout; a body the runtime rejects is not queued for emission at all. A C
  `enum` is an `int` at the ABI (BACKLOG §7.10), so a declaration that mentions
  one maps to `int32` and the members become module constants (`let GREEN = 5;`),
  which is what lets a program name them. `bindgen::Result` gained `structs`
  and `constants`, and the CLI summary reports both. Covered by four new
  `tests/bindgen_test.cpp` cases and the extended
  `tests/bindgen/sample.h` fixture, whose output is fed back to `oskel -c` by
  `tests/run_bindgen_case.cmake`. Verified end to end against libc: the
  generated `div_t div(int, int)` returns the right quotient and remainder and
  the enum constants print `5` and `6`.

- **`class` hardening: field defaults, the `init` completeness check, method
  reassignment rules and a typo warning (PHASE 6.8.1).** A `class` stays pure
  sugar over the map-plus-closures idiom, so every check below lives in the
  parser or the compiler and the AOT backend inherits it unchanged. A field may
  now carry a default (`n: int = 5;`), which the desugaring seeds into the
  instance map instead of `nil`, per instance. A class that declares `init` must
  assign every non-nullable annotated field that has no default, or the field
  would hold `nil` where the annotation promises a value; the check is
  syntactic, so an assignment inside an `if`, a loop or a `switch` counts and a
  lambda body inside `init` does not. Only `=` and `??=` count as setting a
  field: every other compound reads the field first, so `self.n += 1` on a field
  that is still `nil` is reported instead of crashing the constructor at run
  time (a compound on a field that has a default is fine). Replacing a declared
  method is allowed
  only with a function of the same arity, because a method body reads its
  parameters positionally. Assigning a name within two edits of a declared
  field is a **warning**, not an error: an instance is a map, so the key is
  really added, but the near miss is far more likely a typo than a deliberate
  new key. Covered by `tests/60_class_hardening.oskl`,
  `tests/61_class_field_typo.oskl`, six `tests/errors/class_*.oskl` cases and
  the `oskel build` fixtures that pin VM/AOT parity.

- **The class field-type check now reaches every statically known receiver.**
  `checkFieldAssign` could only see a direct `let p = Point(...)`; a call to a
  function with a declared return type (`let b = make(); b.n = "text"`) and a
  field of another instance (`o.inner.n = "text"`) fell through untyped even
  though both types are known at compile time. `inferExprType` now returns the
  declared return type of a user function and the declared type of a member of a
  statically known struct or class, so those writes are checked like a direct
  one, with the same message and the same exit 65 under `oskel build`, which
  writes no binary. A receiver whose type is genuinely unknown (an element of an
  array or a map, an unannotated parameter or function) stays unchecked, since
  the language is untyped by default.
- **Structs by value across `extern "C"` in the hosted AOT backend (PHASE
  7f.7).** A `struct` parameter or return now resolves through `oskel build
  --mode hosted` in both System V ABI classes. A struct of 16 bytes or fewer is
  flattened to its eightbytes and travels in registers; a larger one is class
  MEMORY, passed in the outgoing stack-argument area and returned through a
  hidden `sret` buffer in `rdi`. The thunk reads each field out of the `AotMap`
  the program holds and rebuilds a returned struct into a fresh map, so a struct
  behaves the same on the VM and in a hosted binary. The development engine
  gained the class-MEMORY half too (through `libffi`, so it needs libffi and the
  System V AMD64 ABI). Covered by `tests/native_build_extern_struct.oskl`,
  `tests/native_build_extern_memory_struct.oskl` and `tests/vm_test.cpp`.

- **Builtin `memSize()`: the machine's physical memory in bytes.** The RAM
  analogue of `time()` — a property of the machine, not of the program. On the
  development VM it comes from the platform (`sysconf(_SC_PHYS_PAGES)` on POSIX,
  `GlobalMemoryStatusEx` on Windows, `0` on a platform with no view of the
  machine). Under `oskel build` it is implemented for the bare-metal target
  (`x86_64-freestanding-sysv`): the multiboot header now requests the memory
  information, the prelude captures the multiboot info pointer from `EBX`, and
  the generated `oskel_mem_size` sums the available entries of the boot loader's
  memory map (falling back to `mem_lower`/`mem_upper`). On any hosted target the
  lowerer rejects a call with a message naming the target instead of emitting a
  binary that reads a nil handle; a user binding of the same name still shadows
  the builtin, so `var memSize = 5` compiles on every target. Covered by
  `tests/bare_metal_test.cpp` (a `-m 256M` boot asserting 255 MiB),
  `tests/lower_test.cpp` and `tests/vm_test.cpp`.


- **`bindgen` reads function-pointer declarators and calling conventions.**
  Three shapes a sweep over real headers (glibc, Windows-style SDK headers)
  turned up. A **calling convention** (`__stdcall`, `__cdecl`, `__fastcall`,
  `WINAPI`, `CALLBACK`, ...) ended the specifier list, so the whole declaration
  vanished with no skip report; it is now dropped like any other qualifier, and
  skipped inside a callback declarator (`int (WINAPI *cb)(int)`), where it used
  to become the parameter's name. A **pointer to a callback**
  (`int (**pp)(int)`, `int (*arr[4])(int)`, `cb_t *pcb` on a callback typedef)
  was spelled `fn(P...): R`, claiming the engine can call something it only
  passes through; it is an opaque `ptr`, as the pointer semantics require. And a
  function type with **redundant parentheses** (`int (cb)(int)`), which C
  adjusts to a function pointer exactly like `int cb(int)`, was skipped while
  its unparenthesized twin was generated.

- **`bindgen` never drops a declaration in silence.** A declaration prefixed by
  a **function-like macro** (`MY_API(int) f(void)`) vanished without a trace:
  without a preprocessor the argument may be the return type, so the tool now
  reports it as a skip and points at `cc -E` instead of guessing. A declaration
  whose **type the tool cannot resolve** (`Foo (*fp)(void);`) vanished the same
  way and is now reported too, under a best-effort declarator name.

- **`bindgen` translates a callback parameter.** A C function-pointer parameter
  becomes the Oskel `fn(P...): R` the engine accepts as a parameter (PHASE 6.6),
  in all three C spellings: the inline declarator (`int (*cmp)(const void *,
  const void *)`), the function *type* (`int cmp(int, int)`, which C adjusts to
  a pointer) and a function-pointer `typedef` used as a type. This unblocks the
  common C APIs — `qsort`, `atexit`, `signal`, `pthread_create` — that were
  skipped wholesale before. The shapes the vocabulary does not accept stay
  skipped with a reason: a callback *returned* by a function, one that takes
  another callback, a variadic callback, and a callback returning a struct by
  value. A pointer *to* a function pointer stays an opaque `ptr`. No token, AST
  node, parser rule, opcode, VM or compiler change: `bindgen` only writes the
  `extern "C"` the engine already accepts.

- **`bindgen` respects the FFI register budget.** A non-variadic declaration
  passing more than `kMaxExternArgs` (6) register-sized values is now skipped
  with a reason instead of being emitted; the compiler rejects such a signature,
  so the old output was a module `oskel -c` refused.

- **`std.collections`: functional helpers over arrays.** A new standard-library
  module for mapping, filtering, folding, searching, reshaping, grouping and
  sorting. It offers `map`, `filter`, `reject`, `each`, `fold`, `sum`, `sumBy`,
  `minimum`/`maximum`, `minBy`/`maxBy`, `count`, `find`/`findIndex`,
  `any`/`all`/`none`, `take`/`drop`, `partition`, `chunk`, `zip`, `flatten`,
  `reverse`, `unique`/`uniqueBy`, `groupBy`, `countBy` and
  `sort`/`sortBy`/`sortByKey`. Every function returns a new array or map and
  never mutates its input; `sort` is a stable merge sort. The module exists
  because the array methods (`xs.map(f)`, `xs.sort()`, ...) are VM-only today:
  the AOT backend does not bind them, so `oskel build` compiles a call and the
  binary fails at run time with `value has no such member` (BACKLOG §6 item 1).
  These are the same operations over the portable primitive surface, so they
  behave identically on the VM and under `oskel build`. No new runtime type,
  opcode or builtin.

- **`class`: named shapes with methods** (PHASE 6.8). A `class` declares fields
  and methods; instantiating is calling the class name (`Point(3, 4)`, no
  `new`), `self` is explicit inside a method, and there is no inheritance. The
  feature is **pure parser sugar**: the declaration is rewritten into an ordinary
  top-level constructor function that builds the instance as a map (each field a
  key, starting `nil`), assigns every method as a closure capturing `self`, runs
  the optional `init` (whose parameters are the constructor's) and returns the
  instance. It therefore adds one token (`class`) and no AST node, opcode,
  compiler path or VM logic, and the desugared program is plain Oskel the AOT
  backend already lowers — `class` behaves identically on the VM and in an
  `oskel build` binary with zero backend work (`tests/58_classes.oskl`,
  `tests/native_build_classes.oskl`). A class name is a valid type annotation
  (pre-scanned like a `struct`, and seeded across REPL entries). Rejected at
  parse time: a nested class, a duplicate class name, a class/`struct` name
  collision (in either order), a duplicate field or method, a field and a method
  sharing a name, `self` as a member name, and a variadic `init`. An instance is a
  map at runtime (`type(p)` is `"map"`), and two instances compare unequal even
  with identical fields because their method entries are distinct closures. A
  field annotation is enforced on assignment, like a `struct` literal's field:
  `self.field = value` inside a method and `instance.field = value` through a
  typed binding (annotated, or inferred from the constructor call) are checked
  against it, at compile time, so the AOT backend inherits the same diagnostics.
  An unannotated field, a key the class never declared and a compound assignment
  stay unchecked. `class` is now a reserved word.


- **`std.path`: filesystem paths.** A new standard-library module for joining,
  splitting and normalising filesystem paths. A path is an ordinary string, not
  a new type, so a result feeds the file builtins (`readFile`, `writeFile`,
  `fileExists`) directly and nests, compares and interpolates like any other
  string. The module is pure Oskel over the portable primitive surface and
  offers `join`, `basename`, `dirname`, `extension`, `stem`, `withExtension`,
  `withBasename`, `split`, `isAbsolute`/`isRelative`, `normalize` and
  `relative`. The separator is `/` on every platform and a `\` is folded to `/`,
  so the results are deterministic across Linux, macOS and Windows;
  `normalize`/`relative` match `posixpath.normpath`/`relpath` exactly, with
  three documented deviations elsewhere (`basename("a/b/")` is `b`,
  `dirname("a")` is `.`, and `join("a", "/b")` is `a/b`). No new runtime type,
  opcode or builtin; the VM and `oskel build` agree byte for byte.

- **AOT allocating array methods** (PHASE 7c.3f). The bound array methods
  `slice`, `concat`, `reversed` and `clone` were unbound in the AOT backend, so
  a native binary answered each with "value has no such member". They now bind
  like the in-place methods (`push`/`pop` in PHASE 7c.3c,
  `shift`/`unshift`/`insert`/`removeAt`/`contains`/`indexOf`/`clear` in PHASE
  7c.3e): the receiver goes into a shared cell, the method is an `AotClosure`
  over that one upvalue, and the body is reached through `oskel_call`. Each
  returns a *new* array and leaves the receiver untouched, so a shared
  `emitArrayAlloc` helper allocates a fresh `AotArray` header plus its items
  block once, sized to the exact item count. `slice(start?, end?)` follows the
  hosted binder (optional bounds, a negative bound counts from the end, every
  bound clamped and `end < start` raised to `start`), `concat(other)` appends
  another array (a non-array fails with "concat() expects an array"),
  `reversed()` copies the items in reverse and `clone()` copies them in order
  (a shallow copy). No new opcode and no compiler change: the member form of
  `GETTABLE` plus `CALL` already carries it, so the `.oskc` is untouched. The
  failure paths report the hosted messages and exit 70.

- **AOT error values** (PHASE 7c.5c). `error()` and `isError` were unbound in
  the AOT backend (`isError` returned a constant `false`), so value-based
  `try`/`catch` never caught in a native binary. `oskel build` now has a real
  `error` value: `error(message, code?)` builds an arena-backed `AotError`
  (message string + integer code, `0` by default; one or two arguments, else
  the wrong-arity error), and the tag flows through every shared helper so
  `type(err)` is `"error"`, `isError`/`$isError` see it, `==` compares by
  message and code, `print`/`toString` render the message, `repr` renders the
  `error("message")` / `error("message", code)` debug form, interpolation
  renders the message, and `err.message`/`err.code` read the fields. No new
  opcode and no bytecode change — it is a runtime value like the others. This
  is what makes `try` propagate and `catch` recover in a freestanding binary,
  matching the development VM; a raised *runtime* error (no unwinding in AOT)
  still exits 70. Covered by `tests/native_build_error.oskl` and
  `test_error_values` in `tests/freestanding_test.cpp`.

- **Bare-metal keyboard input: the IRQ1 handler and the `input` builtin.** The
  multiboot kernel now has a keyboard. After the PIC remap, IRQ1 is vector
  `0x21`; the handler reads the scancode from the i8042 data port (`0x60`),
  tracks the two shift keys, translates a set-1 *make* code to ASCII through a
  pair of 128-byte tables and appends the character to a 256-byte ring buffer in
  the zeroed kernel-data page. The master PIC is masked with `0xFC` (unmasking
  IRQ1 alongside IRQ0), and the handler acknowledges the PIC and `iretq`s like
  the timer handler. A new `oskel_read` thunk drains the buffer for the
  `input([prompt])` builtin, which reads one line (blocking with `hlt` on an
  empty buffer) and returns it as a string, an empty line included. `input` is
  seeded only for the bare-metal target — the one whose prelude binds
  `entry.read` — so on Linux and Windows the name stays unresolved and a
  program that calls it fails the build with the usual unseeded-global error.
  Backend-only: no token, AST node, parser rule, opcode, VM path or compiler
  path changes (`docs/bare-metal-interrupts-design.md` §9). The end-to-end test
  is deterministic and host-independent: the guest injects its own scancodes
  through the i8042 (`0xD2`), because a QEMU-monitor `sendkey` does not reliably
  reach the guest.

- **A VM/AOT parity fixture for `switch`, `defer` and `struct`.** The curated
  parity list covered functions, collections, ranges, slices and builtins but
  never exercised these three constructs, which the AOT backend lowers
  correctly. `tests/native_build_control_flow.oskl`
  (`oskel.build_binary_control_flow`) covers `switch` on an int, a string and a
  range, `defer` in LIFO order across nested scopes, and `struct` with typed and
  untyped fields, nesting, a struct return type and structural equality; the
  CMake script asserts the VM and the binary agree byte for byte.

- **`bindgen`, the C binding generator.** A standalone executable (built next to
  `oskel`, shipped in `bin/`, and *not* linked into the engine) that reads a C
  header and writes an Oskel `extern "C"` module, so a C library can be called
  from Oskel without spelling every signature by hand:
  `bindgen mylib.h -o mylib.oskl`. It translates the PHASE 6.6 FFI scalar
  vocabulary (pointers, `cstring`, the fixed-width/unsigned integers, `float`,
  `double`, `bool`, `void`), resolves the header's own `typedef`s, decays an
  array parameter to a pointer, drops qualifiers, storage classes and
  attributes, and recognizes the `stdint.h`/`stddef.h` names without reading
  them. It never runs the preprocessor and never emits something the compiler
  would reject: a declaration outside the vocabulary (a struct by value, a
  callback, `long double`, ...) becomes a `// skipped:` comment and is counted on
  stderr, and a C variadic is written commented out (it needs libffi and the
  System V AMD64 ABI). No lexer token, AST node, parser rule, opcode or VM
  change: the tool only writes the `extern "C"` the engine already accepts
  (`docs/bindgen.md`).

- **`std.datetime`: dates and time.** A new standard-library module
  for civil dates and wall-clock times. A `Date` is a calendar day and a
  `DateTime` adds a time of day plus a UTC offset in minutes; both are plain
  maps, so they print, compare and nest like any other value. The module is pure
  Oskel over the portable primitive surface and offers the `days_from_civil`
  algorithm (exact for every representable year, pre-epoch included),
  conversion to and from the Unix epoch, calendar arithmetic (`addDays`,
  `addMonths`, `addSeconds`), differences, components (`weekday`, `isoWeek`,
  `dayOfYear`, ...), a `strftime`-like `format`, ISO-8601 `parse`/`iso`, and
  `isValid`/`tryParse`. A `DateTime` carries its offset as data, so the module
  never consults a timezone database.

- **`time()` builtin, and a wall clock on every AOT target.** A new
  `time()` builtin returns the seconds since the Unix epoch as an `int`
  (`clock()` stays monotonic and is untouched). The AOT backend binds it as the
  `now` runtime handle on all four paths: a syscall on Linux (freestanding),
  `time(NULL)` in hosted mode, `GetSystemTimeAsFileTime` on Windows, and the
  CMOS real-time clock in a bare-metal kernel — the last converting the RTC's
  BCD/binary fields to a Unix timestamp in assembly, so `std.datetime.now()`
  works even with no OS and no libc.

- **The bare-metal interrupt path: the timer tick** (PHASE 7g). The
  bare-metal prelude now *services* interrupts, not just reports exceptions. It
  remaps the 8259 PIC off the CPU's exception vectors onto 0x20-0x2F (the
  standard ICW1-ICW4 sequence), programs the 8254 PIT channel 0 in mode 3
  (divisor 11931, 100 Hz), unmasks only IRQ0 and masks the rest, and enables
  interrupts with `sti` as the last step before the program runs -- so no
  interrupt arrives with the IDT or the PIC half-configured. The IDT now backs
  two handlers: the fatal *fault* handler (unchanged; every vector but the
  timer still reports and halts) and a *timer* handler for IRQ0 that
  acknowledges the PIC and returns with `iretq`. The handler saves and restores
  every register it touches and forces 16-byte stack alignment around its
  `call`, because an interrupt lands at an arbitrary instruction. It counts
  ticks in a new `.bss` kernel-data page and is silent on every tick -- a
  per-tick print would interleave with the program's own output, since a normal
  program already spans a tick -- so after 500 ticks (five seconds) a program
  that never returned gets one `tick limit` line and the dedicated exit code
  `0x77`. `CodeGen` gains `sti` (`FB`) and `iretq` (`48 CF`), two
  single-instruction primitives, each byte-checked in `tests/codegen_test.cpp`.
  The design is in `docs/bare-metal-interrupts-design.md`. No token, node,
  parser rule, opcode, VM or compiler change.

- **Bare-metal CPU-exception handler** (PHASE 7g). The entry point installs
  an interrupt descriptor table (256 gates, one stub per vector) before the
  program runs, so a CPU fault reaches a handler that prints `oskel: cpu
  exception 0xVV rip=0xRRRRRRRRRRRRRRRR` on COM1 and halts. Each stub pushes
  the vector and, for the ten vectors the CPU does not already tag with one, a
  dummy error code, so the frame the handler reads is uniform: vector, error
  code, faulting RIP. Without an IDT a `#UD` is a double fault and then a
  reset -- a silent hang, which is exactly how the missing `CR4.OSFXSR` hid.
  `tests/bare_metal_test.cpp` boots a `#UD` (vector 6, no error code) and a
  `#PF` (vector 14, with error code) under QEMU and checks both are reported
  with the right vector and RIP. No new opcode: the prelude drives the CPU with
  `lidt`, `rep movsb` and `push imm8`, primitives the emitter gained (each one
  instruction, so the 1:1 budget holds).
- **Two AOT link modes** (PHASE 7f.3). `oskel build --mode
  freestanding|hosted` (default `freestanding`) selects how the image is closed.
  `freestanding` is the self-contained binary every target already emits;
  `hosted` links against the host C library so an `extern "C"` symbol resolves
  to the real C function — the mode that makes FFI work. The mode is a
  deployment choice, never language syntax, and the (target, mode) matrix is
  validated (freestanding: all targets; hosted: Linux x86-64 only, for now) so
  an unsupported pair is reported instead of emitted. The freestanding binary is
  unchanged, byte for byte.
- **Hosted AOT image: relocatable object + link against libc** (PHASE 7f.4).
  `oskel build --mode hosted` now closes the image. The emitter produces an
  ELF64 `ET_REL` object (`native/object.h`): `.rela.text` carries one relocation
  per external reference (`R_X86_64_PLT32` for a call, `R_X86_64_PC32` for an
  address) and `.symtab` exports the program's `main`, while the entry prelude
  reaches the system through libc (`write`/`exit`/`malloc`) instead of a raw
  syscall. The host C compiler links the object against libc (`$CC`, default
  `cc`, `-no-pie -lm`). The freestanding output is byte-identical, and the
  hosted binary agrees with the development VM byte for byte. General
  `extern "C"` marshalling (a per-signature thunk) is PHASE 7f.5.
- **`std.json`**, a pure-Oskel standard-library module for parsing and
  serializing JSON. It maps JSON onto Oskel's own values (null → `nil`, number →
  `int`|`double`, object → `map`, array → `array`), so no new runtime type is
  introduced, and exposes `parse`, `isValid`, `tryParse` and `stringify`
  (compact, or pretty-printed with an `indent`). A JSON number is an `int` when
  the literal fits the signed 64-bit range and a `double` otherwise; `\uXXXX`
  escapes are decoded, with surrogate pairs combined and a lone surrogate
  rejected; the nesting depth is bounded at 256; and `stringify` refuses the
  values JSON cannot spell (a non-finite number, a function, a range). The
  module is written over the portable primitive surface, so it behaves
  identically on the development VM and under `oskel build` (the AOT backend),
  which the new `tests/native_build_json.oskl` fixture guards.
- **Bare-metal AOT target** (PHASE 7g). `oskel build --target
  x86_64-freestanding-sysv` emits a multiboot 1 kernel that a boot loader
  (QEMU `-kernel`, GRUB, Limine) loads at 1 MiB and enters directly, with no
  operating system underneath. The image is an ELF32/i386 whose 32-bit
  trampoline builds an identity page table, turns on long mode and installs its
  own 64-bit GDT before jumping into the shared 64-bit runtime, so the program
  itself is the same machine code the Linux target emits. The runtime talks to
  a 16550 serial port at COM1 (`oskel_write`), ends the program with a `cli`/
  `hlt` loop (`oskel_exit`) and takes its bump-allocator arena from a `.bss`
  region the loader zeroes (`oskel_init_arena`). Verified end to end under
  `qemu-system-x86_64 -kernel` in `tests/bare_metal_test.cpp`.
- AOT: the public builtins `range`, `assertEq` and `exit` compile into the
  freestanding binary (PHASE 7c.5b.5), so a program that uses them builds with
  `oskel build` instead of failing on an unassigned global. `range` builds the
  same `Range` value as the hidden `$range` helper (one bound means `0..end`);
  `assertEq` compares with the shared structural `==` and, on a mismatch,
  renders the hosted `"<msg>: expected A, got B"` text through the embedded
  `repr` and string builder before exiting 70; `exit` leaves through the entry
  prelude's `oskel_exit`, so it stops the program with the given code. All three
  match the development VM byte for byte and are covered by
  `tests/native_build_builtins.oskl` and the `freestanding_test` suite.
- A `switch` case label may be a `const` name (a top-level `const`, or a local
  `const`/`let` in the same frame), not only a literal or a range, so a shared
  constant reads as a label instead of a repeated magic number. The name is
  checked to be an immutable binding; a `var`, an unknown name or a captured
  binding is a compile error, so a label stays a fixed value. No new opcode:
  the label lowers to the same `EQ` (or `IN` for a range) against the subject.
- AOT: `oskel build` no longer fails on an unused host declaration (PHASE 7f.1).
  A `native fn`/`extern "C"` declaration binds a name to a host function; the
  lowerer now counts reads against declarations, so a program that imports a
  module declaring host functions (`std.math` declares ten) builds even when it
  calls none of them — the original gap, where `import std.math` broke the build
  for a program that used only the pure Oskel `abs`/`min`/`max`/`clamp`. A *used*
  declaration without a stub fails with a dedicated message
  (`native fn 'X' cannot be resolved by the AOT backend (PHASE 7f)`) instead of
  the generic unassigned-global error a typo produces. `sqrt` is seeded from the
  exact `sqrtsd` stub, so `native fn sqrt` resolves without a host; the libm
  primitives (`exp`, `log`, `pow`, `sin`, `cos`, `tan`) resolve from the embedded
  musl ports (PHASE 7c.5b.4).
- A qualified call to a module's native export is now checked at compile time
  and rejected by `oskel build` (PHASE 7f.2). `std.math.pow(...)` reads the
  module table with `GETTABLE`, not with `GETGLOBAL`, so the AOT lowerer never
  saw the use: a program that called a host binding through its namespace
  compiled to a binary that faulted at run time (`cannot call a non-function
  value`) instead of failing the build. The compiler propagates each public
  native export's signature on the imported module, records the qualified access
  when it emits the member read, and resolves it once the chunk is compiled
  (deferred, because a hoisted function is compiled before the `import` that
  loads the module). The build now rejects the use with the same message as the
  direct form, and a qualified call is checked against the export's signature
  (arity and argument types) exactly like `pow(2.0)`, so `std.math.pow(2.0)`
  fails with `'std.math.pow' expects 2 arguments, got 1`.
- AOT: the transcendental builtins `exp`, `log` and `pow` (PHASE 7c.5b.4a) and
  the trigonometric builtins `sin`, `cos` and `tan` (PHASE 7c.5b.4b) are now
  available in a freestanding binary. The development engine resolves them
  through the host's libm, which a freestanding AOT binary cannot link, so the
  musl routines (MIT, Arm Limited) are emitted directly as x86-64 through
  `CodeGen` (the approach already used for `dtoa` and `parse_float`) and the
  runtime stays libc-free. `exp`/`log`/`pow` port the musl kernels and the
  `pow` special cases; the trigonometric family ports `__rem_pio2` (with its
  large-argument helper), the `__sin`/`__cos`/`__tan` kernels and the public
  entry points. The wrappers do the arity/type checks and the `log`
  non-positive check, and are seeded into their global slots like the other
  builtins, so neither the compiler nor the bytecode changes. glibc is not
  correctly rounded for these functions, so the contract is "agrees to 1 ulp".
- `struct` literals support field shorthand: `Point { x, y }` is sugar for
  `Point { x: x, y: y }`, taking each value from the variable of the same name
  in scope. The shorthand and the `name: value` form mix freely in one literal
  (`Point { x, y: 5 }`). It is a parser-level desugaring — no new token, node,
  opcode or runtime change — so the lowered bytecode is identical to the
  explicit form and the VM and the AOT backend are unaffected.
- **Windows AOT target** (PHASE 7e). `oskel build --target
  x86_64-windows-win64` emits a static Windows PE32+ executable; the default
  target follows the host, and a Windows output name gets the `.exe` suffix.
  The lowering and the embedded runtime are shared with the Linux target
  unchanged — only the entry prelude differs, reaching kernel32
  (`GetStdHandle`/`WriteFile`, `ExitProcess`, `VirtualAlloc`) through the
  Microsoft x64 ABI. The PE emitter now writes a real import table with a
  `.idata` section pinned at a fixed RVA and declares
  `IMAGE_FILE_RELOCS_STRIPPED`, so the non-PIE image is mapped by a Windows
  with "Force ASLR" instead of aborting.
- Variadic functions (`rest...`) now compile to a static binary (PHASE 7c.6).
  The AOT prologue accepts `numParams - 1` or more arguments and builds the
  packed array with the new runtime helper `oskel_pack_rest`, so a program
  using variadic parameters behaves the same on the VM and in an AOT binary.
- FFI: a struct larger than 16 bytes (System V class MEMORY) now crosses the C
  boundary by value. Such a struct travels on the stack and comes back through
  a hidden `sret` pointer, a shape the register thunks cannot express, so the
  call goes through `libffi` where the development engine links it on the
  System V AMD64 ABI; elsewhere the declaration is rejected at compile time as
  before.
- FFI: a callback (`fn(P...): R`) may now return a struct by value. The
  trampoline serialises the map the Oskel function returns into the C return
  storage using the struct layout, so C reads the fields back through the
  registers the ABI assigns.
- AOT: `oskel build` reports an `extern "C"` symbol with a dedicated message
  (`extern "C" symbol 'X' cannot be resolved by the AOT backend (PHASE 7f)`)
  instead of the generic unassigned-global error a typo produces. The symbol
  still cannot be lowered (PHASE 7f work), but the failure now names the real
  problem. Pinned by `tests/build_error.cmake`.
- FFI: `extern "C"` declarations support a C variadic. A trailing bare `...`
  (`fn printf(fmt: cstring, ...): int`) declares one; the named parameters are
  the fixed prefix and the extra arguments pass through the default argument
  promotions. Like a class-MEMORY struct, the call goes through `libffi` and the
  declaration needs the System V AMD64 ABI, so off that it is rejected at
  compile time instead of mis-called.

### Changed

- **The status table and the phase line catch up to the AOT error values
  (PHASE 7c.5c).** `docs/language.md` still said `pcall`/`error` "and real error
  catching do not" lower, which the 7c.5c work made false: `error` values and
  value-based `try`/`catch` now behave identically on the VM and under `oskel
  build` (`error`, `isError`, `type`, `.message`/`.code`, equality, `print`,
  `repr`, interpolation, `catch` recovering a returned error). Only `pcall` and
  raised runtime errors stay out, the latter because the AOT runtime has no
  unwinding. The row now says so. `DEVELOPMENT.md`'s Phase 7 status line also
  omitted the completed `7c.4a-d`, `7c.5a`, `7c.5c` and `7c.6` slices; it lists
  them, and keeps `7c.5b` marked in progress as its section says. The same stale
  claim survived in five module notes (`std/json.oskl`, `std/path.oskl`, and the
  `std.json`/`std.datetime`/`std.path` bullets in `DEVELOPMENT.md`), which listed
  `pcall`/`error` together as unimplemented in AOT; they now name only `pcall`,
  which is what actually remains.

- **The docs stop promising what the tree does not do.** `DEVELOPMENT.md` §8 now
  marks the `.oskc`/`.oskpkg` formats and their bytecode verification as
  *planned* (nothing serializes a `Bytecode` to disk yet), and `docs/language.md`
  §19 says the same for the extension table. `DEVELOPMENT.md` §8 also states
  what the JIT's "1:1" rule actually means: the invariant is over the bytecode
  opcodes, not the native instruction count, so the slice-3 compare-branch
  fusion is a peephole rewrite of the emission, not a superinstruction.

- **FFI is documented as hosted-only under `oskel build`.** The decision (PHASE
  7f) is recorded in `docs/language.md` §4.5 and §16 and BACKLOG §7.20: a program
  using `extern "C"` builds with `--mode hosted`; the freestanding default, which
  is self-contained by construction, reports the unresolved symbol instead of
  emitting a broken binary. This is the design, not a temporary gap.

- **`docs/BACKLOG.md` reflects the tree.** The Env and parity items are marked
  done (VM/AOT parity already runs through `tests/build_binary.cmake`), the JIT
  item is decided as *keep* (it ships on by default in `oskel` and gives ~420x
  on its integer leaf subset), and §6.22 plus `docs/language.md` §16 record the
  one known parity gap: a function renders as `<fn name>` in the VM but `<fn>`
  in the AOT binary, since the AOT closure object has no name field.

- **`docs/language.md` now states the real implementation status.** The
  reference claimed "collections (PHASE 6 done)" and "No JIT ... planned for
  PHASE 8+", both stale: PHASE 6.5/6.6/6.7 are complete on the development
  engine and the JIT slices 1-4 ship in `oskel`. It also said the compiler
  emits "40 opcodes" (it is 39, with 40 the budget ceiling), presented
  `oskelc` as if it existed (`oskel build` is the AOT entry point and `oskelc`
  is not part of 0.1.0), and described the AOT backend as lowering pointers and
  callbacks to stubs when PHASE 7f only resolves a scalar `extern "C"` call in
  the hosted link mode. A new "Implementation status" table opens the document
  and §16 and §4.6/§15 are corrected to match the tree. Documentation only.

- **Bare-metal boot tests now run in CI instead of skipping, on all three
  platforms.** The `bare_metal_test` end-to-end cases boot the kernel under
  `qemu-system-x86_64`, but CI never installed the package, so `haveQemu()`
  returned false and the boot, float and CPU-fault tests silently skipped —
  the new IDT exception handler was never exercised on a runner. The boot tests
  were also compiled out on Windows and macOS, where the harness assumed POSIX
  (`/tmp`, `chmod`, `WEXITSTATUS`, `command -v`). The compile guard now covers
  the three platforms, the harness picks its temp directory, QEMU probe,
  exit-code decoding and null device per platform, and the image stays x86-64
  because QEMU emulates it anywhere (the arm64 macOS image included). Every CI
  job installs QEMU (apt/brew/choco) and exports `OSKEL_REQUIRE_QEMU`, which
  makes a missing binary a test failure rather than a skip; a developer box
  without the variable still skips the boot tests and runs the structural
  ones. No product code changes.

- **JIT (slice 4): web-based register allocation with copy coalescing.** The JIT
  used to tie every slot to a fixed register by index (`R0` to the first pool
  register, `R1` to the second, ...), so a temporary reused for two values forced
  a `mov` at every reuse and each `GETLOCAL`/`SETLOCAL` copied between the
  temporary's register and the local's. The emitter now resolves each read and
  write through a per-instruction allocation table, and an allocator fills it:
  it builds a web per slot (the definitions a single live use can observe),
  interferes webs that are live at the same point, coalesces the copies that do
  not interfere (a `MOVE`, a local copy, or an in-place `s = s + x`), and colors
  the result greedily over the seven-register pool; a web the pool cannot cover
  keeps its 16-byte stack slot, exactly like the old overflow path. A liveness
  pass that mirrors the emitter's fold decisions also drops a definition nothing
  observes, which removes the constant a folded expression left behind. The
  integer loop drops from 17 instructions to 9 (a hand-written `-O2` C++ loop is
  7) and from 0.051s to 0.039s, within ~1.08x of the C++ loop and about 420x the
  interpreter. Still 1:1 with the bytecode -- no opcode, superinstruction or
  runtime specialization is added, and the differential suite (including a
  20k-round fuzz over 12-local bodies) still proves the compiled body identical
  to the interpreter.

- **JIT (slice 3): compare-branch fusion.** When a comparison's boolean result
  only feeds the conditional branch right after it, the JIT no longer builds the
  bool (`cmp; setcc; movzx; mov; test; jcc`) and jumps straight off the flags
  (`cmp; jcc`). A backward liveness pass proves the result dead past the branch
  and that the branch is reached by fall-through alone (never a jump target), so
  the re-test the VM would perform is never lost; a result that is reused
  afterwards stays unfused and correct. The integer loop drops from 0.062s to
  0.051s (about 320x the interpreter, within ~1.4x of a hand-written `-O2` C++
  loop). Still 1:1 with the bytecode — the fusion rewrites how one opcode and its
  branch are emitted, and no opcode, superinstruction or runtime specialization
  is added. The differential suite (including the fuzz corpus) still proves the
  compiled body identical to the interpreter.

- **JIT (slice 2): constant folding, strength reduction and register-direct
  codegen.** The development engine's JIT now folds an expression whose operands
  are compile-time constants (`2 * 3` becomes `6`), applies a constant operand as
  a machine immediate (`n + 7` is one `add`, `i * 2` is one `shl`), and computes
  in the destination register instead of copying operands through scratch, so a
  tight integer loop is another ~2x faster than slice 1 (about 260x the
  interpreter, within ~1.7x of a hand-written `-O2` C++ loop). Everything stays
  1:1 with the bytecode — a constant propagation over the proto feeds the same
  emitter, and no opcode, superinstruction or runtime specialization is added.
  The differential suite (including the fuzz corpus) still proves the compiled
  body identical to the interpreter.


- The development-engine JIT keeps each scalar slot in a physical CPU register
  instead of its 16-byte stack slot. The bytecode contract is untouched (no new
  opcode, no superinstruction, no runtime specialization): the JIT still lowers
  each opcode 1:1, but a slot the dataflow proves only ever holds
  `nil`/`bool`/`int` now lives in `rax`/`rcx`/`rdx`/`rsi`/`rdi`/`r8`/`r9` with
  the raw payload, and the tag is materialized only at the frame boundary. A
  slot that changes scalar kind (a `bool` comparison feeding an `int` slot and
  back) stays in its register, because the kind at each use is read from the
  dataflow state rather than fixed per slot. A tight integer loop is now about
  130x faster than the interpreter (about 5x faster than the previous
  stack-slot JIT), and the differential suite still proves the compiled body
  identical to the interpreter for every accepted proto.
- `ITER` now yields a map's paired value directly. The opcode took two new
  operands: `Instruction::d` is the register that receives the value (`-1` when
  the loop has no value variable) and `Instruction::e` is the done flag, which
  used to overload `Instruction::jump` (backlog §1.2). `jump` is now a jump
  target only. `for k, v in map` no longer calls the hidden `$mapValue` helper
  (retired from the hosted engine and the AOT runtime), so the value form is one
  `ITER` per turn instead of an `ITER` plus a call.
- The `oskel build` CTest cases now run the same fixture list on Windows as on
  Linux. Windows registered only two cases (`build_binary` and
  `build_binary_deep_recursion`) against Linux's ten, so the Windows PE path was
  exercised by two programs while the shared AOT backend was covered by ten —
  the gap the 1 MiB stack reserve slipped through. The runners
  (`tests/build_binary.cmake`, `tests/build_error.cmake`,
  `tests/aot_runtime_error.cmake`) are now portable: the Windows output keeps
  its `.exe` suffix and the static-link `ldd` check is Linux-only. The
  Windows-only `tests/build_binary_pe.cmake` is gone, and one list registers
  every case on both hosts.
- The AOT backend compares two arrays or maps by content, like the hosted
  engine, instead of by pointer identity. `[1, 2] == [1, 2]` is now true in a
  compiled binary, and `in` over an array of arrays matches the VM. The
  equality helper tracks the pairs already under comparison on the stack, the
  same `visited` rule as `Value::equals`, so a cyclic structure compares equal
  to an isomorphic one and terminates instead of recursing forever.
- `examples/tui_editor.oskl` now exercises every feature its header claims:
  the non-null assertion `!!`, safe access `?.` with `??`, a union return type
  (`configValue`), `byteLen`/`buffer`/`cstring` and a range, all checked by the
  self-test. The header's FFI list is corrected (`system`, not `stty`), it
  documents both ways to run the example, and the untestable parts
  (`render`/`readKey`/`enableRawMode`/`restoreTerminal`) are called out. The
  self-test cleans up its temporary file, and the editor reports a clear status
  for `Ctrl-S`/`:wq` with no file name and for a failed `:wq`.
- The README documents the VM/AOT split explicitly (a "What runs where" section):
  the AOT backend covers the language core, so collection methods (`arr.push`,
  `map.keys`, `str.upper`), `pcall`, the remaining standard-library builtins and
  catchable `try`/`catch` are VM-only in 0.1.0, while structural equality and
  `map.get` behave differently in an AOT binary. The FFI callback dependency on
  `libffi` and the JIT's narrow scope are now stated in the README too.
- `DEVELOPMENT.md` marks PHASE 4 completed and notes that `env.cpp` is unused
  dead code (kept only by `tests/gc_test.cpp`).

### Removed

- **The unused `Env` scope chain and `Function::closure`.** The VM keeps locals
  in reference-counted `Cell`s and `Function::upvalues`, so `Env` was never
  constructed and `closure` was only ever assigned by the GC test. `src/env.h`
  and `src/env.cpp` are deleted, the field is dropped from `Function`, and the
  `envGcOps` table and the closure edge in the `value.cpp` function visitor go
  with them. The three `gc_test.cpp` cases are rewritten over `Cell` and
  `Function::upvalues`, which is what the VM actually uses.

### Fixed

- **A `cstring` returned by a C function is a `cstring` in the AOT backend
  too.** `extern "C" fn strchr(...): cstring` came back boxed as a string, so
  `type()` said `"string"` where the VM says `"cstring"`, `found == "llo"` was
  true where the VM says false (a `cstring` compares as a handle), and
  `"x" + found` built a string where the VM raises "unsupported operand
  types" — a silent divergence, not a crash. The thunk now boxes the address as
  a `Ptr` of kind `CString`, the same handle `cstring(text)` produces, and a
  `Ptr` argument crosses as its own NUL-terminated bytes so the handle can be
  fed back into another C function. A `cstring` field of a returned struct is
  boxed the same way. The freestanding image is untouched (it emits no thunk)
  and stays byte for byte identical. Pinned by
  `tests/native_build_extern_hosted.oskl` and
  `tests/native_build_extern_memory_struct.oskl` (BACKLOG §6 item 23).
- **`clock()` in a compiled binary now reports elapsed seconds instead of
  `inf`.** The AOT emitter converted the nanosecond count to a double with
  `cvtsi2sd` but loaded the divisor `1000000000` with `movqToXmm`, which copies
  the *bit pattern* of the integer into the XMM register. Read as a binary64
  that pattern is a denormal near `4.9e-315`, so the division overflowed to
  `+inf` and every difference of two readings was `nan`. The divisor is now
  converted with `cvtsi2sd` as well. The existing fixture only asserted
  `clock() >= 0.0` and monotonicity, and both hold for `inf`; it now bounds the
  value, so the regression fails the suite.
- **`oskel build` no longer rejects a class-MEMORY `extern "C"` struct on an
  engine built without libffi.** The compiler validated every declaration
  against the *build's* ABI capabilities, which derive from `OSKEL_HAVE_LIBFFI`;
  the AOT backend, however, emits its own thunks and marshals a struct larger
  than 16 bytes natively (stack-passed, `sret`-returned), so it never needed
  libffi for it. The two were conflated, and the hosted fixture failed on any
  machine without `libffi-dev` even though the emitted code was correct. The
  compiler now asks `validateSignature` with the destination's capabilities
  (`Compiler::setAbiCapabilities`): `oskel build` passes
  `ffi::kAotAbiCapabilities`, the interpreter keeps this build's
  `AbiCapabilities{}`. A variadic stays rejected for the AOT destination
  regardless of libffi, because the backend does not lower a variable argument
  list (PHASE 7f.5). `oskel.build_hosted_extern_memory_struct` now passes with
  no libffi installed.


- **The VS Code grammar highlighted syntax the language does not have and
  missed three real builtins.** `0x10`, `0b1010` and `0o17` were split into `0`
  and a name, because the decimal rule ran first; the lexer accepts all three
  prefixes. `time`, `memSize` and `args` were builtins the grammar never
  highlighted, `@MustUse` and `self` were plain text, and a struct literal
  (`Point { x: 1 }`) or a constructor call (`Point(3, 4)`) did not name a type.
  In the other direction the grammar advertised `===`, `!==` and `->`, none of
  which the lexer produces, so a file using them looked valid and failed to
  compile. The auto-closing pair for `/*` also inserted a space (`" */"`), and
  there was no indentation rule at all.

- **`bindgen` no longer emits a declaration the compiler rejects on a
  non-System V ABI.** The tool promises that every declaration it writes as
  active code compiles, and three shapes broke that promise — all of them on
  Windows and macOS, which is why only the `windows-latest` job caught it. The
  register budget was counted in *parameters* while the compiler counts
  *eightbytes*, so `struct Q { double x; double y; }` plus five scalars came out
  as six parameters and seven eightbytes, and the generated module failed
  `oskel -c` with "may pass at most 6 register-sized values". A struct by value
  was emitted regardless of the target ABI, although `OSKEL_FFI_STRUCT_BY_VALUE`
  is true only on System V AMD64 without Windows. A struct inside a callback was
  emitted even though the compiler always rejects it. The fix removes the
  duplicated rules instead of patching each one: `ffi::validateSignature` and
  `ffi::validateTypeSpec` take an `ffi::AbiCapabilities` (this build's by
  default, `ffi::kFullAbiCapabilities` for "is the shape in the vocabulary at
  all?") and `bindgen` asks twice — a shape the vocabulary cannot express is
  skipped with the compiler's own reason, while one only some builds can
  marshal is written commented out, which is how a variadic was already
  handled. A fourth divergence went with them: an anonymous
  `struct { int x; }` inline in a signature has no name to spell and used to be
  emitted as a scalar `int32`; it is now skipped. Covered by new
  `tests/bindgen_test.cpp` cases (the eightbyte budget, the anonymous struct)
  and by making the existing struct-by-value and variadic assertions
  ABI-aware, so the suite is green on both a System V and a Microsoft x64
  build; the end-to-end test still feeds the generated module to `oskel -c`,
  which is the property that actually failed.


- **`bindgen` no longer drops a declaration in silence.** A function returning
  a function pointer (`int (*signal(int, void (*)(int)))(int);`) vanished
  without a `// skipped:` comment and without being counted, which broke the
  tool's "never silently mis-translate" contract; it is now reported as a
  callback the vocabulary cannot express, while a function-pointer *variable*
  (`int (*fp)(void);`) stays silently ignored, since it is not a function. Two
  malformed diagnostics are corrected as well: the skip reasons read as noun
  phrases (`'cb_t' is not expressible in the FFI vocabulary (callbacks are not
  generated)`, `'MyType' is not a known type`) instead of concatenating a
  sentence fragment after `parameter type `, and a type the vocabulary does not
  know *after a modifier* (`unsigned __int64 x`) is now reported instead of
  discarding the whole declaration. No token, AST node, parser rule, opcode,
  VM or compiler change: `bindgen` only writes the `extern "C"` the engine
  already accepts, and the end-to-end fixture is unchanged.

- **AOT: silent memory corruption when a rendered collection crossed 64
  bytes.** `oskel_sb_append_escaped` (the helper that renders a quoted string
  into a collection's text) kept its loop index in `r13` and pushed/popped it
  around the loop. `r13` is `kAotArenaCursor` — the arena's next-free pointer
  (`include/native/entry.h`) — and the loop calls `oskel_sb_append`, which
  *allocates* a fresh block once the builder outgrows its 64-byte initial
  capacity. Two things then went wrong: the allocator read the clobbered `r13`,
  and the closing `pop r13` rewound the cursor past blocks already handed out.
  The symptom was silent corruption of `str()`/`repr()`/`print`/interpolation
  output (a garbled line, or a crash) for any array or map of strings whose
  rendering passed 64 bytes — nondeterministically, since it depended on the
  arena layout. The loop index now lives in a caller-saved scratch (`r11`) and
  is reloaded from its stack slot after every call. Covered by
  `tests/native_build_string_repr.oskl`, which renders arrays and maps of
  strings well past the initial capacity and is compared with the VM byte for
  byte.

- AOT: the remaining in-place and query array methods now work in a compiled
  binary: `shift`, `unshift`, `insert`, `removeAt`, `contains`, `indexOf` and
  `clear`. They bind like `push`/`pop` (the receiver goes into a shared cell and
  the method body is an ordinary closure reached through `oskel_call`), so no
  opcode and no compiler change are needed. The AOT `oskel_member_get` answered
  every one of them with "value has no such member", a VM/AOT divergence. The
  member dispatch now resolves the nine bound array names through a
  selector-indexed table, `shift`/`removeAt` return the removed item (or nil
  when empty), `unshift`/`insert`/`clear` return the receiver, `contains`/
  `indexOf` scan with structural equality (an array or map needle compares by
  content) and a negative index counts from the end, matching the hosted
  binder's messages and exit code on the failure paths (bad arity,
  `insert()/removeAt() index out of range`). Covered by `tests/lower_test.cpp`
  (`testExecArrayMethodsInPlace`), `tests/freestanding_test.cpp`
  (`test_array_methods_inplace`) and a `tests/build_binary` fixture.
- Tests: the bare-metal wall-clock boot test now writes its image through the
  portable `tempPath()` helper instead of a hard-coded `/tmp/...`, so it runs on
  Windows too. `test_qemu_wall_clock` was the only one of the five boot tests
  that still named `/tmp` directly; `writeFile` failed silently there, QEMU
  booted a kernel that did not exist and the guest printed nothing, which the
  test reported as an empty integer. `writeFile` now fails loudly when it cannot
  open the image, so a bad path reads as a write error instead of a silent
  empty guest.
- AOT: `**` now accepts a float/double operand or a negative exponent, like the
  hosted engine. `oskel_pow` only lowered int/int with a non-negative exponent;
  every other numeric pair failed with "unsupported operand types for '**'", so
  `2 ** 0.5`, `2.0 ** 3`, `2 ** -1` and `2.0f ** 3.0f` ran in the VM but not in
  a compiled binary — a VM/AOT divergence. The helper now takes the same
  int/float/double promotion as `ops::binary`: int/int with a non-negative
  exponent keeps the wrapping integer loop, and every other numeric pair
  converts to binary64, calls the embedded musl `pow` already used by the `pow`
  builtin, and re-tags the result with `makeNumeric`'s rank rule (binary32 only
  when at least one operand is a float and none is a double). The result type is
  decided before the call, since the embedded `pow` clobbers the volatile
  registers. Covered by `tests/lower_test.cpp` and a `tests/build_binary`
  fixture (`tests/native_build_pow.oskl`).
- AOT: a negative string index now counts code points from the end, like the
  hosted engine. `"abc"[-1]` is `"c"` and `"héllo"[-1]` is `"o"` in the VM, but
  the string branch of `oskel_table_get` passed the raw index straight to
  `utf8CharAt`, which walks forward only, so a compiled binary reported "index
  out of range" and exited 70 on a program the VM accepts. The branch now
  rebases a negative index against the code-point length (the same rule
  `ops::normalizeIndex` applies to an array index), so the VM and the AOT
  backend agree; an out-of-range index still reports the error. Covered by
  `tests/freestanding_test.cpp` (`test_negative_string_index`).
- AOT: `arr.push`/`arr.pop` now work in a compiled binary. The hosted engine
  binds both to a native closure that captures its receiver, so `a.push(3)`
  appends in place and returns the array, and `a.pop()` removes and returns the
  last item (or nil when empty); the AOT `oskel_member_get` only knew `.length`,
  so `a.push` failed at run time with "value has no such member" — a
  VM/AOT divergence. `oskel_member_get` now returns a bound method built the
  same way the hosted `bindArray` does: the receiver is stored in a shared cell
  (a closure upvalue) and the method body is reached through the ordinary
  `oskel_call` path, so the bytecode and the compiler are unchanged. A
  `tests/build_binary` fixture pins the binary against the VM on Linux and
  Windows.
- The `oskel build` end-to-end tests (`tests/build_binary.cmake`,
  `tests/build_error.cmake`, `tests/aot_runtime_error.cmake`) wrote their
  generated binary to a fixed name under `CMAKE_CURRENT_BINARY_DIR`. That
  variable is the same in every `cmake -P` run, so under `ctest -j` the
  registered tests (each a separate process) clobbered one another's output:
  one test removed the file another had just built, or ran a binary produced
  from a different fixture, and the suite failed intermittently with "oskel
  build did not create ..." or "the generated binary disagrees with the VM".
  Each test now passes its unique CTest name (`-DOSKEL_TEST_NAME`) and writes
  to `${OSKEL_TEST_NAME}${exe_suffix}`, so the output paths never collide. A
  parallel run is now deterministic.
- AOT bare metal: the trampoline now sets `CR4.OSFXSR` and `CR4.OSXMMEXCPT`
  (bits 9 and 10) in addition to PAE, so the SSE instructions the float path
  emits execute instead of raising `#UD`. With no IDT installed that fault was
  a triple fault, so any program doing float *arithmetic* (`1.0 / 3.0`, a
  `float` comparison) hung the machine silently, while printing a float
  *literal* worked because it only exercises the integer `dtoa`. The 64-bit
  entry point also brings COM1 up (mask interrupts, divisor 1 for 115200 baud,
  8N1, enable the FIFO) before the program runs, so a real 16550 transmits
  rather than only QEMU's emulated one. `tests/bare_metal_test.cpp` pins both:
  a structural check for the CR4 sequence and an end-to-end float-arithmetic
  boot under QEMU.
- AOT: `+` on two arrays now concatenates in a compiled binary. The hosted
  engine builds a fresh array with the left items followed by the right items
  and leaves both operands unchanged (`docs/language.md`: "concatenation of
  strings or arrays"; `tests/06_operators.oskl` asserts `[1] + [2] == [1, 2]`),
  but the AOT `oskel_arith` rejected the pair as "unsupported operand types", so
  a program that ran on the VM exited 70 as an AOT binary — a mis-compiled
  image, which the backend must never emit. `oskel_arith` now dispatches `+` on
  two arrays to the new `oskel_array_concat` helper (one `ADD` path, no new
  opcode), and a `tests/build_binary` fixture pins the binary against the VM on
  Linux and Windows.
- A compiled binary printed `0 1 2` for `for c in "abc"` instead of `a b c`.
  The `str` branch of `oskel_iter_next` built the character but fell through
  into the `range` block, which overwrote `rax:rdx` with the cursor index; a
  missing `jmp` after the string was built. The VM was correct, so this was an
  AOT-only divergence, now pinned by `tests/freestanding_test.cpp`.
- A `struct` name and an import binding the same name no longer coexist
  silently. `struct Point { ... }` plus `import std.math as Point;` used to
  leave both alive: `Point { ... }` stayed a struct literal while
  `Point.sqrt(9.0)` resolved to the alias, so the winner was undefined. The
  collision is now a compile error in both source orders and for every import
  form (alias, wildcard, selective), reported by `recordImportBinding` within a
  chunk and by `compileStructDecl` across REPL entries. A struct is
  compile-time metadata, not a value binding, so it is not covered by the
  "a local `let`/`var`/`fn` wins over an import" rule.
- A Windows AOT binary was killed with no message when a program recursed a few
  thousand frames with a non-trivial frame. The PE emitter reserved only 1 MiB
  of stack (the Windows default) while the AOT runtime has no call-depth guard,
  so the recursion a native binary reaches is bounded only by the OS stack;
  Linux hands a process 8 MiB (`ulimit -s`), so the same program ran there and
  was torn down silently on Windows (exit code 1, no output). The emitter now
  reserves 8 MiB, matching Linux, and `pe_test` pins the value. Covered by
  `tests/native_build_deep_recursion.oskl` (`oskel.build_binary_deep_recursion`
  on Linux and Windows).
- `chr(code)` now rejects a lone surrogate (U+D800..U+DFFF) instead of
  encoding it as CESU-8 (three bytes, invalid UTF-8), matching the `\uXXXX`
  escape. The check lives in the builtin, so the VM and the AOT backend reject
  the same value with the same message (`chr() code point is a surrogate`) and
  the same exit code. The scalar values on either side of the range
  (U+D7FF, U+E000) still encode.
- A `\uXXXX` string escape whose value is a lone surrogate (U+D800..U+DFFF) is
  now a compile error instead of producing a string with invalid UTF-8. The
  lexer previously encoded the code point as CESU-8 (three bytes), so
  `"\uD800"` yielded bytes the rest of the language (`len`, indexing, `==`)
  treated as one character but that are not valid UTF-8. The check lives in the
  lexer, so the VM and the AOT backend reject the same source identically.
- `extern "C"` declarations on Windows could not name the Win32 API. The
  development engine's `lookupSymbol` searched only the process image and the
  C runtime, so a declaration such as `GetTickCount` or `Sleep` — which live in
  `kernel32.dll` and have no CRT equivalent — failed at run time with
  `extern "C" symbol '...' was not found in the host`. The search now also
  covers `kernel32.dll`, matching the set of modules `dlsym(RTLD_DEFAULT)`
  reaches on POSIX; the CRT names still resolve.
- AOT: `double(f)` on a `float` operand returned `0` instead of the widened
  value. A boxed `Float` already holds its value widened to binary64, but the
  `double` builtin widened the payload a second time, reading the binary32 bits
  as binary64. The `sqrt` stub had the same shape, so both are fixed together.
  `tests/native_build_conversions.oskl` pins the conversions against the VM
  (the script tests that cover them only run on the hosted engine).
- The PE emitter padded the image only to the end of `.text`'s bytes, not to
  the `SizeOfRawData` the section header declares, so the file could end up to
  `FileAlignment` bytes short of the image it described. Windows rejected such
  an `.exe` with `ERROR_BAD_EXE_FORMAT` (Wine tolerated it, which hid the bug);
  the same short image made `pe_test` hang on Windows when it launched the
  rejected binary and the loader's error dialog blocked. The emitter now pads
  to the declared size and publishes the IAT data directory
  (`DataDirectory[12]`), which was left zero. `pe_test` also measured its
  `call [ExitProcess]` displacement from two bytes past the call site, so the
  stub faulted once the image loaded; the offset is now exact. `pe_test`
  asserts the image size and the IAT directory on every platform.
- The PE import directory table held one descriptor per imported function
  (four identical kernel32.dll descriptors) instead of one per DLL, which the
  PE/COFF specification requires ("one entry for each DLL"). Windows tolerated
  the duplicates, so it never surfaced at run time, but the table was
  non-conformant and `objdump` interpreted it as four kernel32.dll imports.
  The emitter now writes a single descriptor for kernel32.dll followed by the
  null terminator, and `pe_test` pins the descriptor count.
- FFI: two `extern "C"` declarations of the same C symbol with different
  signatures are now a compile error instead of a silent last-wins. The check
  spans the whole program, so a conflict between two imported modules is caught
  as well.
- Removed leftover debug `printf` output from the compiler, VM and PE test
  suites.
## [0.1.0] - 2026-11-14

The first public release. Oskel is a dynamic, general-purpose language
implemented from scratch in C++17: lexer, parser, AST, bytecode compiler,
register-based VM, AOT backend, JIT and standard library, with no external
dependencies beyond the C++ standard library.

### Added

- **Language core**: `nil`, `bool`, `int` (64-bit), `float` (binary32),
  `double` (binary64), `string`, `error`, `array`, `map`, `function` and
  `range` values; `var`, `let` and compile-time `const` bindings with block
  scope; first-class functions, recursion, `=>` lambdas and variadic
  `rest...` parameters; real closures over shared cells.
- **Operators**: arithmetic, logical, bitwise, comparison, ternary and
  compound assignment, including the short-circuiting `&&=`, `||=` and `??=`.
- **Control flow**: `if`/`else`, `while`, classic `for`, `for ... in`
  (arrays, maps, strings and ranges, with the `for k, v in map` key/value
  form), `switch` on literals and integer ranges, `break`/`continue`, the
  post-test `do { ... } while (cond);` loop, `defer { ... }` and brace-less
  single-statement bodies.
- **Types and nullability**: optional annotations (`let x: int = 5`), local
  inference, nullable `T?`, unions (`T | E`), `@MustUse`, and the null-safe
  expressions `?.`, `??` and `!!`.
- **Strings and collections**: `"${expression}"` interpolation with UTF-8
  support, ordered arrays and maps with insertion-order iteration, ranges
  (`1..5`, `0..=10`, `5..0`) and slicing (`s[1:3]`, `a[::2]`, `a[::-1]`).
- **Error handling as values**: `error(message, code?)`, `isError`, `pcall`,
  `try expr`, `expr catch default` and the dedicated `error` value type.
- **Modules**: Java-style `import` (qualified, wildcard `.*`, alias `as` and
  selective `{ ... } from`) with compile-time conflict detection and a
  reserved `std.` prefix.
- **C interop**: `extern "C" { ... }` blocks and `native fn` declarations,
  marshalling scalars, strings, pointers and structs by value without
  `libffi`, plus callbacks from C into Oskel through `libffi` in the
  development engine.
- **Standard library**: `std.math` and the builtins for strings, arrays,
  maps, math, files, console and utilities.
- **Development engine**: a register-based bytecode VM, an interactive REPL
  with session state (`:help`, `:load`, `:vars`, `:clear`), and a JIT that
  compiles integer leaf functions to native x86-64.
- **AOT backend**: `oskel build file.oskl` compiles the bytecode to a
  self-contained, statically linked executable with no libc and no runtime to
  install (`ldd` reports "not a dynamic executable").
- **Tooling**: `oskel -c` for a full compile check, diagnostics with file,
  line, column, code snippet and stack trace, and a VS Code
  syntax-highlighting extension.
- **Packaging**: `.deb`/`.rpm` (Linux), an Inno Setup installer (Windows),
  `.pkg`/`.dmg` (macOS) and the VS Code extension (`.vsix`), published as
  GitHub Release assets with a `SHA256SUMS` file on a `v*` tag.

### Notes

- The development engine ships a JIT; the AOT binary never contains it, and
  the JIT never changes the result of a program.
- The AOT backend targets `x86_64-linux-sysv`; on any other target `oskel
  build` reports the unsupported target instead of emitting bytes.

[Unreleased]: https://github.com/yasmramos/Oskel/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/yasmramos/Oskel/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/yasmramos/Oskel/releases/tag/v0.1.0
