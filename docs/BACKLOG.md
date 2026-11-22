# Oskel backlog — things to fix

Consolidated, deduplicated list of known defects, divergences and hardening
gaps, ordered by area. Everything here is a task to resolve, not a limitation
to accept. No item is "by design, leave it": if it stays, it must be a
deliberate decision recorded in `DEVELOPMENT.md`, otherwise it is work.

Priority tags: **[P0]** correctness or memory-safety bug, **[P1]** semantic
divergence or robustness, **[P2]** performance, **[P3]** cleanup or tooling.

---

## 1. VM (`src/runtime/vm.cpp`)

1. **[P1] [done]** `ITER` over a map yielded only the key; `for k, v` then did
   a `$mapValue` lookup per iteration. `ITER` now takes two extra operands
   (`Instruction::d` for the paired value, `-1` when the loop has no value
   variable, and `Instruction::e` for the done flag) and yields the value
   directly, so the loop body holds no helper call. The retired `$mapValue`
   builtin is gone from the hosted engine and the AOT runtime.
2. **[P1] [done]** `ITER` (and `compileForIn`) used `instruction.jump` as the
   output register. The done flag now lives in the real operand `e` (and the
   value in `d`), so `jump` is a jump target only.
3. **[P2]** `namespaceHint` walks every global on each failed `GETGLOBAL`;
   a repeated typo in a large REPL is slow. Index the global names.
4. **[P2]** `callNative` always copies the arguments for reentrancy safety,
   even when the native never reenters. Let a native declare whether it can
   reenter.
5. **[P2]** `CALL` copies the arguments into a `std::vector` before entering
   the frame, one allocation per call. Pass a view into the register window.
6. **[P2]** `scratchCells_` grows without bound across nested calls and is
   never reclaimed until the next `run()`.
7. **[P3]** `JitFunction* jitFor` is not invalidated if the `Bytecode` changes;
   key the cache on the proto identity/version.
8. **[P3]** `VM::pushOskelFrame` — keep the frame-slot count derived from one
   place so the prologue and the VM agree.

---

## 2. Parser (`src/frontend/parser.cpp`)

1. ~~**[P0]** `resolveTopLevelConsts` builds `reordered` but it is identical to
   the input: the hoisting the comment promises is not implemented, and the
   inline comment is in Spanish (language-rule violation).~~ **Done**: the
   stable partition now hoists top-level `const` declarations to the front and
   the comments are in English.
2. ~~**[P1]** `parseStructLiteralBody` does not support the shorthand
   `Point { x, y }`.~~ **Done**: the field name alone (no `:`) desugars to a
   `VariableExpr` of the same name. No new token, node or opcode.
3. ~~**[P1]** `parseSwitch` accepts only literals and ranges as `case` labels;
   a `const` label (`case FOO:`) fails.~~ **Done**: a `case` label may now be a
   `const` name (a top-level `const`, or a local `const`/`let` in the same
   frame). The parser accepts a bare-name label and the compiler
   (`Compiler::checkCaseLabelName`) rejects a `var`, an unknown name or a
   captured binding, so the label stays a fixed value. No new opcode.
4. **[P1]** `parseImport` does not reject `from` being a user variable, so
   `import { x } from from;` is ambiguous.
5. **[P1]** `parseProgram` pre-scans `structNames_` without requiring `struct`
   to be followed by `{`; a malformed struct is skipped in the pre-scan and
   fails later with a less clear message.
6. **[P2]** `warn` in `foldConstant` does a linear scan of `warnings_` per
   warning (O(n²) with many overflowing consts). Use a set.
7. **[P2]** The unary overflow warning uses `expr.line/col` while the binary
    case uses `opLine/opCol`. Make them consistent.
8. **[P3]** `parseRange` rejects chained ranges (`1..2..3`, correctly) but the
    error does not suggest the alternative.
9. **[P3]** `atLambda` cannot distinguish `(a, b) => ...` from a parenthesized
    list followed by `=>` in an ambiguous context; fragile, add lookahead.
10. **[P3]** `parseTypeAnnotation` does not validate that a struct name is
    known at annotation time when the struct is declared later; the pre-scan
    covers structs only, not new FFI type names.
11. **[P3]** `parseFullExpression` accepts an optional `;` then requires `Eof`;
    in the REPL this can mask errors.

---

## 3. Compiler (`src/frontend/compiler.cpp`)

1. **[P1]** `checkAssignable` and `assignError` duplicate logic; a change in
   one can be missed in the other. Unify the path.
2. **[P1]** `compileBlockBody` concentrates hoisting, slot pre-reservation,
   visibility restore, nil-cell creation and three passes over `block.stmts`;
   it is the place a capture bug will hurt. Split it.
3. **[P1]** `predeclaredLetSlots_`/`predeclaredFnSlots_` key on the AST node
   pointer, an implicit "the AST never moves" coupling. Put the slot on the
   node.
4. **[P1]** `compileWhile`/`compileFor`/`compileDoWhile` repeat the same
   `loops_.push_back` / `deferMinDepth` / `compileScopedBlock` / `pop` pattern
   five times. Extract one helper.
5. **[P1] [done]** `compileImport` does not reject an alias that collides with a
   declared struct (an alias `Point` and a `struct Point` coexist; the winner
   is undefined). `recordImportBinding` now reports the collision, and
   `compileStructDecl` reports the reverse order for the REPL (where the import
   and the struct arrive in different entries). Covered by
   `testStructImportNameConflict` in `tests/module_test.cpp` and
   `tests/errors/struct_import_conflict.oskl`.
6. **[P1]** `compileTry`/`compileReturn` emit `emitDeferredBlocks(1)` before
   `RETURN`; the interaction of `try` inside a block with a `defer` that
   returns is subtle and untested. Define and test it.
7. **[P1]** `compileCatch` does not emit `emitDeferredBlocks` on the fallback
   path; whether a fallback block's `defer` runs is undefined and untested.
8. **[P1]** `compileExternBlock` does not reject a symbol that collides with a
   builtin (`print`, `len`, ...), which then shadows it silently.
9. **[P2]** `inferExprType` for a `CatchExpr` with one unknown side returns
   untyped, losing check opportunities. Infer the known side when possible.
10. **[P2]** `compileSwitch` emits `EQ` + `JMP_IF_TRUE` + `JMP` for the
    single-literal-case shape; fold it.
11. **[P2]** `checkAssignable` for unions is dense and hard to follow; a typed
    variant would make it safer.
12. **[P2]** `resolveFromFrame` does not detect upvalue cycles between mutually
    recursive functions; the recursion can be deep.
13. **[P2]** `compileFnDecl` for a `native fn` emits a `GETGLOBAL`; a name the
    host never registered fails only at run time. Warn when the host lacks it.
14. **[P3]** `compileModuleBody` exports only names that do not start with `_`;
    there is no way to export such a name.
15. **[P3]** `compileStructLiteral` iterates the declaration's fields (stable
    shape) while the comment can be read as iterating the literal's entries.
    Clarify the comment.
16. **[P3]** `compileReplEntry` does not handle a comment-only entry (empty
    block, emits `HALT`); add a test.
17. **[P3]** `compileExpression` (REPL) does not handle an entry that is a
    `let`/`fn`; the statement/expression split relies on the heuristic
    `isStatementStart`.

---

## 4. AST (`src/frontend/ast.h`)

1. **[P1]** The iterative teardown (`collectExprChildren` + `collectStmtChildren`
   + the destructor) needs each new node added in three places; a miss is a
   silent leak or a stack overflow. Make it one place.
2. **[P1]** `TypeRef` is a bag of mutually exclusive fields (`name`,
   `alternatives`, `nullable`, `callbackParams`, `callbackReturn`). Model it as
   a variant.
3. **[P1]** `MapEntry` has mutually exclusive `literalKey`/`keyExpr` with no
   type-level invariant.
4. **[P2]** `InterpPart` stores `ExprPtr` that may be null even though the
   parser never emits null; use a non-null pointer type.
5. **[P2]** `StructField` and `StructLitEntry` are near-identical; unify them.
6. **[P2]** `LambdaExpr::body` is always a `BlockStmt` but the type does not
   guarantee it.
7. **[P2]** `FnDeclStmt::body` is null for a native; `LetStmt::init` is null
   only for `var`; `ForStmt::init/condition/step` are nullable. Use
   `std::optional` to make each explicit.
8. **[P2]** `SwitchCase::values` accepts only literals/ranges but the type does
   not enforce it.
9. **[P3]** `Expr`/`Stmt` carry `line`/`col` but no unified `position()`, while
   nodes that need `opLine`/`opCol` carry extra fields. Unify.

---

## 5. JIT (`src/runtime/jit/jit.cpp`)

1. **[P1]** The JIT compiles only `nil`/`bool`/`int`: no floats, strings,
   containers, `CALL`, `GETGLOBAL`/`SETGLOBAL`. It accelerates very little.
2. **[P1]** `Jit::get` caches failures (a null function) with no size cap; the
   cache grows unbounded in a long REPL. Add an LRU or a cap.
3. **[P1]** `JitFunction::call` does not validate `args.size() == numParams`;
   `jitFor` checks first, but a direct `call` does not.
4. **[P1]** `Jit::get` keys on `proto.get()`; if a `Bytecode` is freed and
   another lands at the same address the cache returns the wrong JIT. Key on a
   stable identity.
5. **[P1]** `jitHostSupported()` does not cover macOS; add `defined(__APPLE__)`.
6. **[P1]** On Windows `JitCode::assign` always returns false; the user gets no
   JIT and no message. Report it at startup.
7. **[P2]** `emitProto` does not handle `MOD`/`POW` (`i % 2` in a loop is
   common), `CALL`, `GETGLOBAL`/`SETGLOBAL` or `NEWTABLE`/`GETTABLE`/`SETTABLE`;
   any function using a builtin or a collection is not compiled.
8. **[P2]** `emitProto` computes `frameSize = 16 * (numRegs + numLocals + 1)`
   with no overflow check; `subRegImm32` would wrap.
9. **[P2]** `analyze` does not verify `numParams <= numRegisters`; correct by
   accident via `numRegs = max(...)`, but make it explicit.
10. **[P2]** `successors` adds `in.jump` even when it is negative or beyond the
    end; filtered later in the fixpoint, but fragile.
11. **[P2]** `emitProto` for `LOADK` with `Ty::Top` falls through to
    `tagOf(Ty::Int)`; the verifier rejects it first, but add an assert.
12. **[P3]** `copySlot` does not touch refcounts; correct for the current
    types, but assert it stays that way.

---

## 6. AOT backend (`src/frontend/native/lower.cpp`, `src/frontend/native/runtime.cpp`)

1. **[P0] [done]** VM/AOT divergence in collection methods: the array methods
   bind in AOT (`push`/`pop` in PHASE 7c.3c, `shift`/`unshift`/`insert`/
   `removeAt`/`contains`/`indexOf`/`clear` in PHASE 7c.3e, the allocating
   `slice`/`concat`/`reversed`/`clone` in PHASE 7c.3f, and the callback-taking
   `map`/`filter`/`each`/`find`/`any`/`all` plus the folding `reduce`/`sum`/
   `join` in PHASE 7c.3h), and the ten map methods in PHASE 7c.3g.
   `sort`/`sorted` bind too, in PHASE 7c.3j: a bottom-up stable merge sort that
   calls the program's comparator back through `oskel_call`. It reproduces
   `std::stable_sort` exactly -- equal keys keep their input order -- and both
   forms sort a copy and commit only on success, so a comparator that raises
   leaves the receiver untouched, as in the VM. The pure string methods bind in
   PHASE 7c.3i: `upper`, `lower`, `trim`, `trimStart`, `trimEnd`, `contains`,
   `startsWith`, `endsWith`, `indexOf`, `substr`, `reversed` and `repeat` run
   on both engines over the same portable primitive surface. The string
   methods that return a collection bind too, in PHASE 7c.3k: `split` (all
   three modes -- whitespace runs, an empty separator yielding the `chars`
   array, and a non-empty separator keeping every field, the empty ones
   included), `lines` (one trailing `'\r'` dropped per segment, no empty tail),
   `chars`, `replace`, `replaceFirst`, `padStart` and `padEnd` (a `width` in
   code points, the fill repeated whole and then clipped, both ordering
   branches). There is no callback form on a string: `"abc".map`/`.each`/
   `.filter` are rejected by the VM too (`type 'string' has no member 'map'`),
   so the string method surface has no AOT gap left. The range methods bind in
   PHASE 7c.3l: `toArray` (half-open or closed, either direction, and a span
   over ten million items refused with the hosted message) and `contains` (the
   method form of `x in r`, integer-only like the operator). `emitMemberGet`
   only knew a range's `.length` before, so both answered "value has no such
   member". The same change closes the `.length` saturation: a span wider than
   INT64_MAX reports INT64_MAX, which the AOT arithmetic now computes in
   unsigned magnitude exactly like the hosted `Range::length`.
2. **[P0] [done]** VM/AOT divergence in structural equality: `[1,2] == [1,2]`
   is true in the VM (content) and false in AOT (identity). The equality helper
   in `oskel_compare` now descends into arrays and maps (equal length, equal
   items/entries), tracking the pairs already under comparison so a cyclic
   structure compares equal to an isomorphic one, matching the hosted
   `Value::equals`.
3. **[P0] [done]** VM/AOT divergence in `try`/`catch`: `$isError` in AOT used
   to always return false, so `try`/`catch` never caught. The AOT runtime now
   has a real `error` value tag (PHASE 7c.5c): `$isError`/`isError` see it, so
   `try` propagates and `catch` recovers in a native binary, matching the VM.
   A runtime error that is *not* a returned error value (an out-of-range index,
   a failed `assert`, ...) still exits 70: the AOT backend has no unwinding, so
   only value-based recovery is possible (by design, DEVELOPMENT.md §3).
4. **[P0] [done]** `pcall` is implemented in AOT (BACKLOG §6 item 4): the
   builtin calls the callee and turns a raised runtime error into a value, so
   `pcall` returns `{ok, value, error}` exactly like the VM. The AOT runtime had
   no unwinding, so the error sink (`oskel_fail_dynamic`) now checks a hidden
   `$pcallHandler` global before printing: while a `pcall` is active it stores
   the message in that handler's record, restores the `pcall` frame's `rsp`/`rbp`
   and jumps back to it -- the one non-local return in the call ABI. The record
   carries the previous handler, so a nested `pcall` and a normal return both
   restore the chain. The slot, the builtin body and the sink's handler check
   are emitted only when the program calls `pcall`, so every other image is byte
   for byte what it was. `tests/native_build_pcall.oskl` pins the parity (a
   success, a caught failure, a nested catch and a closure).
5. **[P0] [done]** `error()`/`makeError` is now implemented in AOT (PHASE
   7c.5c): `error(message, code?)` builds a real error value, and `type(err)`,
   `isError`, `err.message`/`err.code`, structural equality, `print`/`repr` and
   string interpolation all agree with the VM. `tests/native_build_error.oskl`
   pins the parity (built and run by `tests/build_binary.cmake`).
6. **[P1]** VM/AOT divergence in `map.get`: the VM uses `unordered_map` (O(1)),
   AOT uses a linear scan (O(n)), so a loop over a large map is O(n²).
7. **[P1]** VM/AOT divergence in string iteration: AOT allocates one
   `AotString` per character; a 1M-char string means 1M arena allocations.
8. **[P1] [done]** `emitContains` for arrays uses `AotCompareOp::Eq`, which
   used to be identity for arrays/maps; now that equality is structural, `in`
   over an array of arrays matches the VM (fixed with item 2).
9. **[P1] [done]** `emitMemberGet` for a string binds the twelve pure methods
   (PHASE 7c.3i, item 1). `split`/`lines`/`chars`/`replace`/`replaceFirst` and
   the callback forms stay open: they return a collection or call back into the
   program.
10. **[P1]** `emitIterNext` for `str` allocates a string per character — same
    as item 7.
11. **[P1]** `collectGlobals` does not detect name-resolution cycles: a
    `GETGLOBAL` of a name assigned only inside a never-called function is
    accepted and reads nil at run time. Report it.
12. **[P1]** `collectGlobals` pre-assigns builtin slots in first-read order; a
    `var print` then overwrites the builtin. Make the precedence explicit.
13. **[P2]** `emitArith`'s float path depends on `emitToDouble` not clobbering
    `Rdi`/`Rdx`; a change there breaks it. Document the invariant and assert.
14. **[P2]** `emitCompare`'s `orderMixed` uses `cg.ucomisd(1, 0)` with magic
    numbers; use named constants.
15. **[P2]** `emitStrBuiltin` for `Float`/`Double` allocates ~1KB of stack per
    interpolation; in a loop that is 1KB per iteration.
16. **[P2]** `emitStrBuiltin` for a range does not handle `start > end`; a
    descending range is not tested.
17. **[P3]** `emitRootPrologue` reserves `frameBytes + 8` for `rsp` alignment,
    and `emitFunctionPrologue` reserves `fileLocals + 1` for `self`; document
    the extra slot at the site.
18. **[P3]** `emitArith` handles `INT64_MIN / -1` on the int path but not on the
    float path; consistent semantics, inconsistent code.
19. **[P0]** The AOT backend has no FFI: an `extern "C"` symbol (or a `native
    fn`) is a host binding with no host, so `oskel build` cannot lower it. The
    read now fails with a dedicated message (`extern "C" symbol 'X' cannot be
    resolved by the AOT backend (PHASE 7f)`) instead of the generic
    unassigned-global one, but the lowering itself is PHASE 7f work: a design
    decision (curated syscall stubs, an object + external link, or a dynamic
    ELF) is still open, since the generated binary is freestanding today
    (`ldd` reports "not a dynamic executable"). `tests/build_error.cmake` pins
    the current behaviour. **Partial (7f.1)**: an *unused* declaration no longer
    fails the build (the lowerer counts reads against declarations), and `sqrt`
    is provided by a curated stub that matches the VM bit for bit. **Partial
    (7c.5b.4)**: `std.math`'s libm surface (`exp`, `log`, `pow`, `sin`, `cos`,
    `tan`) now resolves from the embedded musl ports, so a program that calls
    them builds. **Partial (7f.2)**: a qualified access to a module's native
    export (`std.math.pow`) is recorded on the proto and rejected the same way
    (the member read is a `GETTABLE`, so the read count alone could not see it),
    and a qualified call is checked against the export's signature like the
    direct form. **Partial (7f.3)**: the two link modes are now a CLI concept
    (`LinkMode`, `oskel build --mode freestanding|hosted`, default
    `freestanding`); `hosted` is the mode that links libc so an `extern "C"`
    symbol resolves at link time, with the (target, mode) matrix validated.
    **Partial (7f.4)**: the hosted *image* exists. The emitter produces an
    ELF64 `ET_REL` object (`native/object.h`) whose `.rela.text` carries one
    relocation per external reference (`R_X86_64_PLT32` for a call,
    `R_X86_64_PC32` for an address) and whose `.symtab` exports the program's
    `main`; the hosted entry prelude (`emitHostedRuntime`) reaches the system
    through libc (`write`/`exit`/`malloc`) instead of a raw syscall, and
    `oskel build --mode hosted` links the object against libc with the host C
    compiler (`$CC`, default `cc`, `-no-pie -lm`). The freestanding output is
    byte-identical to before. General `extern "C"` *marshalling* (a per-signature
    thunk) is still open (7f.5): the lowering still rejects an `extern "C"` read.
    **Partial (7f.5)**: a *scalar* `extern "C"` signature (integer/bool/float/
    double parameter or return) now resolves in the hosted mode — the lowerer
    narrows it to an `AotExternThunk` (`ffi::ParamClass`/`ffi::ReturnClass` per
    slot) and `emitExternThunk` marshals it and calls the symbol through a
    linker-resolved relocation. **Partial (7f.6)**: a `cstring`/`ptr` parameter
    and a `cstring` return now resolve too (a pointer argument materialises a
    NUL-terminated arena copy; a `cstring` return is scanned back into an
    `AotString`). **Partial (7f.6b)**: an opaque `ptr` return now resolves in
    the hosted mode too — the address comes back in `rax` and `emitVoidPtrBox`
    boxes it as an `AotPtr` of kind `Void` carrying that address, which is
    exactly what `intToPtr` builds, so `ptrToInt` reads it back and a later call
    passes the real address. A NULL address becomes `nil`, the hosted engine's
    own rule (`wrapPointerValue` returns an empty `Value` before it looks at the
    type name), so `type()` reports `"nil"` and not `"ptr"`. A struct and a
    callback still reject the Windows build; a C variadic is lowered on all
    hosted targets (7f.10).
20. **[P0] [done]** VM/AOT divergence in string iteration: `emitIterNext`'s
    `str` branch built the character but fell through into the `range` block,
    which overwrote `rax:rdx` with the cursor index, so `for c in "abc"`
    printed `0 1 2` instead of `a b c` on the AOT backend (the VM was
    correct). A missing `jmp storeCursor` after the string was built. Fixed
    with the `ITER` redesign (items 1-2 of §1) and pinned by
    `tests/freestanding_test.cpp`.
21. **[P0] [done]** VM/AOT divergence in negative string indexing: `"abc"[-1]`
    is `"c"` in the VM (`ops::normalizeIndex` rebases a negative index against
    the code-point length), but the string branch of `oskel_table_get` passed
    the raw index straight to `utf8CharAt`, which walks forward only, so a
    compiled binary reported "index out of range" and exited 70 on a program the
    VM accepts. The branch now rebases a negative index against the code-point
    length (via `oskel_utf8_len`) before the lookup; an out-of-range index still
    reports the error. Pinned by `tests/freestanding_test.cpp`
    (`test_negative_string_index`).
22. **[P1] [done]** VM/AOT divergence in a function's text representation:
    `repr`/`str` of a named function is `<fn name>` in the VM (`<native fn
    name>` for a host binding) but used to be the bare `<fn>` in the AOT
    binary, because the AOT closure object carried no name. The closure header
    gained a fourth word, `display`, holding the address of a static
    `AotString` with the exact text to render (`"<fn add>"`,
    `"<native fn array.map>"`, `"<native fn getpid>"`); a null `display` keeps
    the old `<fn>` fallback for a closure built by a path with no name. The
    text is a data island, so the closure stays four words and `emitReprInto`
    renders it with one append -- no per-value allocation. One island per proto
    is emitted by the lowerer (`<fn ` + `Bytecode::name` + `>`), one per builtin
    and one per `extern "C"` thunk (`<native fn ` + symbol + `>`), and the
    bound array/map/string methods carry their `owner.method` form from the
    binder. `tests/native_build_function_repr.oskl` pins the parity for a
    declared function, a lambda, a class method, a builtin, a `native fn`, a
    bound method of each family and a function nested in a container.
23. **[P1] [done]** VM/AOT divergence in the value of a returned `cstring`
    (PHASE 7f.6). `emitExternThunk` scanned a returned `const char *` and boxed
    it as the existing `Str`, so `type()` reported `"string"` where the VM
    reports `"cstring"`, and the more permissive string operations then
    accepted it: `r == "llo"` was true in AOT and false in the VM (a `cstring`
    compares as a handle), and `"x" + r` built a string where the VM raised
    "unsupported operand types". `emitCStringBox` now builds an `AotPtr` of
    kind `CString` -- the same handle `cstring(text)` produces and the pointer
    builtins already understand -- so `type()`, handle equality, `print`/`str`/
    `repr`, `byteLen` and the rejection of `+` all agree with the VM. The same
    box is used for a `cstring` field of a returned struct. The parameter side
    had to follow, in both places that marshal a `cstring` argument: the scalar
    parameter and the `cstring` field of a struct passed by value. A `Ptr`
    argument now crosses as its own NUL-terminated bytes instead of failing the
    type check, which is what lets a `cstring` returned by one C function -- or
    held by a returned struct -- be fed back into another. The scan cap (16 MiB, the hosted
    engine's `kMaxCStringBytes`) still matches. Pinned by
    `tests/native_build_extern_hosted.oskl` (`type`, `== nil`, handle identity,
    `==` against a string, `byteLen`, and the handle reused as an argument) and
    by the `cstring`-field case in `tests/native_build_extern_memory_struct.oskl`.
    The freestanding image is unaffected: it emits no thunk, and its output
    stays byte for byte identical.
24. **[P0] [done]** Silent memory corruption when a rendered collection crossed
    the string builder's 64-byte initial capacity. `oskel_sb_append_escaped`
    kept its loop index in `r13` and pushed/popped it around the loop; `r13` is
    `kAotArenaCursor` (entry.h) and the loop calls the allocating
    `oskel_sb_append`, so the allocator read a clobbered cursor and the closing
    pop rewound it past live blocks. `str()`/`repr()`/`print`/interpolation of
    any array or map of strings past 64 bytes came out garbled or crashed,
    nondeterministically. The index now lives in a caller-saved scratch (`r11`),
    reloaded from its slot after every call. Regression:
    `tests/native_build_string_repr.oskl`.
25. **[P1] [done]** Fourteen builtins existed in the development engine and
    were not seeded by the AOT backend, so a program that called one failed
    `oskel build` with "global 'X' is read but never assigned" (exit 65) while
    the VM ran it. `tests/check_builtin_parity.cmake` enumerates the set from
    both sides, so a new builtin cannot be added to one engine alone. Its
    exception list now holds only the `std.fs.dir` directory builtins below.
    Every original one is implemented, each with the parity fixture that pins
    it:
    - **Pointers** (`cstring`, `cstr`, `buffer`, `byteLen`, `loadByte`,
      `storeByte`, `intToPtr`, `ptrToInt`, `free`). The VM keeps them in a
      reference-counted `PtrObj`; the image keeps an `AotPtr` in the arena
      (`native/value.h`), with the same four fields and the same observable
      behaviour, `free` marking the handle released instead of reclaiming it.
      Nothing in these needs a host, so they are seeded on every target.
      Fixture: `tests/native_build_pointers.oskl`.
    - **Files** (`readFile`, `writeFile`, `fileExists`). Written against four
      prelude handles -- `oskel_open`/`oskel_read_fd`/`oskel_write_fd`/
      `oskel_close` -- so the syscall (Linux `openat`/`read`/`write`/`close`),
      the kernel32 calls (`CreateFileA`/`ReadFile`/`WriteFile`/`CloseHandle`)
      and the hosted libc path all stay in the prelude and the builtins
      themselves are target-independent. A bare-metal image has no file system,
      so it binds none of the handles and the lowerer rejects the three by
      name. Fixture: `tests/native_build_files.oskl`.
    - **Streaming files** (`fsOpen`, `fsRead`, `fsWrite`, `fsSeek`, `fsClose`),
      the primitives behind `std.fs.File`. `oskel_open` gained the append mode
      (`O_APPEND`/`_O_APPEND`/`FILE_APPEND_DATA`) and the prelude gained
      `oskel_seek`/`oskel_tell` (`lseek`, `SetFilePointerEx`, `_lseeki64`), so
      the builtins stay target-independent. They are seeded wherever
      `readFile` is, and rejected by name on bare metal. Fixtures:
      `tests/native_build_fs.oskl` (freestanding, VM parity) and
      `oskel.build_fs_hosted` (the same fixture through libc/`_lseeki64`).
      The directory half (`fsRemove`, `fsRename`, `fsMkdir`, `fsRmdir`,
      `fsList`, `fsStat`, `fsCwd`, `fsChdir`) is **VM-only for now**; it is the
      documented `vm_only` list in `tests/check_builtin_parity.cmake`, and a
      used name makes `oskel build` fail with "std.fs.dir is VM-only for now"
      instead of the generic unassigned-global error. `PlatformFreestanding`
      already implements the Linux syscalls, but the AOT prelude does not emit
      those handles and no builtin is seeded; wiring that side is the next
      step.
    - **`clock`**. Reads the `entry.monotonic` handle, which every prelude
      binds (Linux `clock_gettime(CLOCK_MONOTONIC)`, hosted the same through
      libc, Windows `GetTickCount64`, bare metal the PIT tick counter), and
      subtracts the value the chunk captured at entry, so it reports elapsed
      time rather than an absolute counter. It is deliberately *not* the hosted
      engine's `clock()` (CPU time) and does not need libm.
    - **`args`**. Reads a hidden `$args` global the chunk fills from the
      `(argc, argv)` the process entry passes, skipping `argv[0]` exactly like
      the VM. Only the targets whose entry receives an argument vector seed it
      -- freestanding Linux and every hosted target (the C runtime passes
      `argc`/`argv` to `main`) -- while freestanding Windows (the loader calls
      the entry with no arguments) and bare metal reject it by name.
      Fixture: `tests/native_build_args.oskl` (Linux and hosted only;
      freestanding Windows and bare metal pin the rejection with
      `tests/native_build_args_error.oskl` instead), and
      `tests/native_build_clock.oskl` covers `clock`. The hosted Windows path
      reads the CRT's `argv`, which is in the process code page rather than
      UTF-8; non-ASCII arguments are a known gap there.
    - **Done earlier**: `pcall` (§6 item 4) and `random`/`seed` (PHASE
      7c.5b.6).
    A target that cannot back a builtin now fails with a message that names the
    *target* ("args() needs a process argument vector", "readFile() needs a file
    system", "input() needs a keyboard", "memSize() is only available on the
    bare-metal target") instead of the generic unassigned-global error a typo
    produces; `lower_test` pins each message and the shadowing case
    (`var args = 1` still lowers).

    **Arity is checked, not assumed.** Every fixed-arity builtin now validates
    `argc` before it touches the argument window, because it did not and that
    was a memory-safety bug rather than a message mismatch: the window grows
    *down* from `args`, so `type()` with no arguments read a slot nobody had
    written and returned a type name built from whatever was there (`int` after
    a four-int call, `nil` before it), and `chr(65, 1)` returned `"A"` while the
    VM raised. Eight builtins were affected: `len`, `type`, `str`, `repr`,
    `bool`, `isError`, `ord` and `chr`. Each now compares `argc` first and fails
    with the fixed prefix `X() expects 1 argument`. `$str` shares `str`'s entry
    and always passes one argument, so the guard is safe for interpolation and
    computed map keys too. Pinned by `tests/native_build_builtin_arity.oskl`,
    which drives all sixteen wrong-count calls through `pcall` (a raised error
    is not a value, so `try`/`catch` cannot see it) and still compares line for
    line with the VM.

    One divergence remains and is deliberate: a builtin's *runtime* error
    message that ends in a dynamic tail keeps only its fixed prefix in AOT
    (`free() expects a pointer` against `free() expects a pointer, got 'int'`;
    likewise `expects an integer index, got '...'` and `index 7 is out of
    range`), because the AOT sink writes one fixed message blob and does not
    format values into it. The function name and the condition are identical in
    both engines, which is what a program can branch on; assembling the tail
    would mean a per-message builder in the runtime and is not worth it until a
    program is shown to depend on the text. `ord`/`len`/`chr` set this
    convention before the pointer builtins did.

    **A wrong argument count is now worded identically on both engines**
    (PHASE 7c.5b.6). The guards used to stop at the fixed prefix, so the VM said
    `len() expects 1 argument, got 0` where the binary said
    `len() expects 1 argument`, and a user function reported the generic
    `wrong number of arguments` with no name and no counts. One runtime helper,
    `oskel_arity_fail(msg, len, argc)`, appends `, got N` to a constant prefix
    through the arena string builder and hands the result to `failDynamic`, so
    the `pcall` presentation is the same one every other dynamic message uses.
    Every guard reaches it: the builtins and bound methods pass their own name
    and expected count, a user function's prologue builds
    `name() expects N argument(s)` (with ` or more` when it is variadic) from the
    name it already has, and `error()` passes its `between 1 and 2` range.
    Three further divergences fell out of the same work: `floor`, `ceil`,
    `round`, `exp`, `log`, `pow`, `sin`, `cos` and `tan` reported their *type*
    message (`floor() expects a number argument`) when the count was wrong, and
    `int()`, `float()` and `double()` still used the old prefix-only path.

    The numeric conversions are the exception that proves the rule
    (PHASE 7c.5b.2c): `int`, `float` and `double` now assemble their message in
    the arena from a prefix, the operand's own bytes, the target and an
    optional cause, so the AOT text matches the VM character for character
    (`cannot convert 'abc' to int`, `cannot convert an empty string to float`,
    `cannot convert '1e40' to float (out of range)`,
    `cannot convert '1e400' to double (use inf, -inf or nan)`,
    `cannot convert a non-finite float to int`,
    `float is out of the integer range`). The parsers report *why* they failed
    (malformed, overflow, empty) so the cause can be named, and a non-string
    operand quotes its type name. The builder is worth it here because a
    conversion failure is the error a program is most likely to print and
    branch on. Their arity guard follows the shared `, got N` helper above.

26. **[P1]** WebAssembly backend gaps (PHASE 9). The backend behind
    `Target::wasm32()` (`src/frontend/native/wasm.cpp`) covers the scalar core,
    arrays, maps, slicing, variadic functions, the array/map/string/range
    methods, `random`/`seed`, `std/json`, `std/datetime`, `std/collections`,
    `std/path` and the bound half of `std.math`, and its `double` printing is
    the same shortest round-trip form as `std::to_chars`. What is still open:
    - A builtin with no WASM body is seeded as a trapping closure: declaring it
      is harmless, calling it reports the gap. The unbound names are the
      complement of `supportedBuiltins()`; the transcendental `pow`, `sin`,
      `cos`, `tan`, `log` and `exp`, plus `input`, `pcall`, `args`, `memSize`,
      the file builtins and the pointer builtins are in that set today.
    - A used `extern "C"` or `native fn` symbol with no builtin stub is a build
      error, mirroring the AOT backend's message.
    Each gap fails cleanly (build error 65 or a runtime error), never a
    mis-compiled module. `tests/native_build_wasm.oskl`,
    `tests/native_build_datetime.oskl`, `tests/native_build_collections.oskl`,
    `tests/native_build_math.oskl`,
    `tests/native_build_wasm_range_methods.oskl` and
    `tests/native_build_double_format.oskl` pin the supported surface against
    the VM byte for byte.
27. **[P1]** A callback that C stores for a later call works in AOT but not in
    the VM: the development engine's `CallbackObj` is released when the
    `extern "C"` call returns, while the AOT stub and its context live in the
    arena. The AOT-only fixtures `tests/native_build_extern_callback_stored.oskl`
    and `..._exit.oskl` (an `atexit` handler after the chunk returns) cover it;
    keeping the VM closure alive would close the gap.
28. **[P1]** A callback returning a struct by value is supported by the VM
    (the descriptor/closure stub) and rejected by the AOT backend with a dedicated message; the AOT
    callback body does not rebuild the returned registers into a map. A struct
    *parameter* is rejected by both. `tests/errors/callback_struct_{return,param}.oskl`
    pin the AOT rejection.
28. **[P2]** A callback stub is one `mmap` page per registration and lives
    until the process exits. Freeing it when the extern call returns is not
    safe: C may store the function pointer (a stored callback, including an
    `atexit` handler), and Oskel has no reference count for the C side. A pool
    of stubs (one page per N registrations) or an explicit release contract
    would bound the memory of a long-running program. The page is W^X today:
    read+write while the stub is written, then `mprotect`ed to read+execute;
    `tests/native_build_extern_callback_wx.oskl` asserts the final mapping is
    not writable.

29. **[P1]** The own linker (PHASE 10.1/10.2) closes one or more x86-64 Linux
    objects (and System V/GNU `ar` archives) into a dynamic ELF, and `-r`
    combines them into a new `ET_REL`. It loads `.text` and `.rodata` only: a
    symbol in `.data`/`.bss`, a `.rela.data`/`.rela.rodata`, or a relocation
    other than `R_X86_64_PLT32`/`R_X86_64_PC32` is reported by name (PHASE
    10.2b), as is a GCC object that references a section symbol for a string
    literal. `.eh_frame`/`.note.*`/`.comment` are ignored (not loaded). Thin
    archives are rejected; only regular System V/GNU `ar` archives are read.
    The macOS Mach-O / Windows PE containers are PHASE 10.3. The image is
    non-PIE (fixed `0x400000`), so PIE/ASLR is later work. The hash is SysV
    `.hash`, not `.gnu.hash` (glibc accepts both). `--link-lib` needs the dev
    symlink (`libSDL3.so`) at link time to read the SONAME, while the binary
    records the SONAME (`libSDL3.so.0`).

---

## 7. FFI (`src/runtime/ffi.cpp`)

1. ~~**[P1]** `extern "C"` does not support variadics; `printf` cannot be
   declared. Provide a way (or a documented reason it is out of scope).~~ **Done**:
   a trailing bare `...` declares a C variadic (`fn printf(fmt: cstring, ...):
   int`). The compiler records the flag on the `ExternSymbol` and checks only the
   fixed prefix; the runtime builds the outgoing register/stack layout per call
   and applies the default argument promotions. Needs the x86-64 descriptor stub
   (System V AMD64 or Microsoft x64, `kCVariadicSupported`), so off that the
   declaration is rejected at compile time. The hosted AOT backend lowers the
   tail itself on System V and Win64 (PHASE 7f.10), independent of the
   development engine. See `tests/53_ffi_variadic.oskl`,
   `vm_test.cpp::testExternVariadic*`, `tests/native_build_extern_variadic.oskl`
   and `docs/language.md` §4.6.
2. ~~**[P1]** `extern "C"` does not support structs larger than 16 bytes by value
   (System V passes them via a hidden pointer / `sret`).~~ **Done**: a struct
   larger than 16 bytes (class MEMORY) is passed on the stack and returned through
   the hidden `sret` pointer via the descriptor stub (`kMemoryStructsSupported`).
   Off System V x86-64 the declaration is rejected at compile time. See
   `vm_test.cpp::testExternMarshalsMemoryStruct*` and `docs/language.md` §4.6.
3. ~~**[P1]** `extern "C"` does not support callbacks returning structs (scalars
   only).~~ **Done**: a callback may return a struct by value. The compiler
   resolves the struct name of the callback return to its fields
   (`TypeSpec::callbackReturnsStruct`/`callbackReturnFields`), `makeCallback`
   builds the struct `ffi_type` for the `ffi_cif`, and the trampoline serialises
   the returned map into the C return storage using the layout (a shape that
   does not match is a re-raised error, not a wild write). A struct over 16
   bytes (class MEMORY) needs the System V descriptor stub, like a large struct
   argument; elsewhere the declaration is rejected at compile time
   (`tests/check/ffi_callback_struct_return.oskl`,
   `tests/errors/extern_callback_struct_return_nested.oskl`,
   `vm_test.cpp::testExternCallbackReturnsStruct`/`ReturnsMixedStruct`).
4. **[P1]** `extern "C"` does not support nested callbacks (a callback that
   takes another callback). **Out of scope**: `TypeSpec` stores a callback's
   signature as scalar type names, so supporting a callback parameter would need
   a recursive `TypeSpec`, and the closure stub path would have to build a
   nested C callback context. A nested callback has no Oskel spelling either (the type
   grammar reads a callback parameter as a scalar name), so the declaration is
   rejected with a clear message rather than mis-parsed.
5. **[P1]** `extern "C"` does not support `long double`. **Out of scope**: Oskel
   has no 80-bit type, so there is no way to hold the value on either side of
   the boundary. `double` (binary64) is the widest float Oskel exposes.
6. **[P1]** `extern "C"` does not support `__int128`. **Out of scope**: Oskel's
   integer is 64-bit; a 128-bit argument would need a type the language does not
   have. Pass a pointer to the 128-bit value instead.
7. **[P1]** `extern "C"` does not support arrays by value. **Out of scope**: a C
   array parameter decays to a pointer at the ABI, so `ptr`/`cstring` already
   spells it; a struct field that is an array is not expressible in Oskel's
   `struct` (fields are scalars), which is the same rule as a nested struct.
8. **[P1]** `extern "C"` does not support C unions. **Out of scope**: a C union
   is a memory layout, not a type; Oskel's `struct` is a map and has no field
   aliasing. A union crosses the boundary as a `ptr`/buffer the caller fills.
9. **[P1]** `extern "C"` does not support bitfields. **Out of scope**: a bitfield
   is an intra-struct packing detail; Oskel's `struct` fields are whole scalars
   at natural alignment, and bit manipulation is done with `&`/`|`/shifts.
10. **[P1]** `extern "C"` does not support C enums (only `int`). **Not a gap**: a
    C enum is an `int` at the ABI, so `int` is the exact spelling; the enum's
    named constants are a header concern the program supplies itself.
11. ~~**[P1]** `installExtern` does not detect two `extern "C"` declarations of
    the same name in different modules; the last wins silently.~~ **Done**: the
    compiler now keeps a session-wide canonical signature per C symbol and
    rejects a mismatch (`tests/errors/extern_conflicting_signature.oskl`,
    `module_test.cpp::testConflictingExternAcrossModules`).
12. ~~**[P1]** `wrapPointerValue` for `cstring` calls `strlen`, which reads past
    the buffer if the pointer is not NUL-terminated — UB from a host that
    returns a non-terminated `char *`. Bound it or reject.~~ **Done**: the scan
    is capped (`kMaxCStringBytes`) and a host that breaks the `const char *`
    contract is a clear runtime error, not an out-of-bounds walk
    (`vm_test.cpp::testExternUnterminatedCStringReturnIsBounded`).
13. ~~**[P1]** `oskellCallbackTrampoline` catches `std::exception` with a generic
    message; a non-`RuntimeError` type is lost.~~ **Done**: the trampoline now
    catches `std::exception` and keeps `what()`.
14. ~~**[P1]** `CallbackObj::pendingError` is not reset between invocations; a
    second call sees the first call's pending error. Reset it per call.~~
    **Not a bug**: the trampoline short-circuits while a pending error is set
    and the extern call re-raises it before any second call can run.
15. ~~**[P2]** `makeExternNative` does not validate `address != nullptr` (the
    caller does).~~ **Done**: the factory rejects a null address itself.
16. ~~**[P2]** `makeCallback` does not validate that the value is a `Func` before
    building the `CallbackObj` (the caller does).~~ **Done**: `makeCallback`
    checks `isFunc()` itself.
17. ~~**[P2]** `dispatch` for callbacks is limited to `kMaxExternArgs = 6`; the
    error message does not say so.~~ **Not a bug**: callbacks go through the
    closure stub, not `dispatch`, so they are not bound by `kMaxExternArgs`; an 8-parameter
    callback compiles and runs. **Done**: the scalar bound is now eight
    eightbytes. The VM's typed call lets the C++ compiler spill the seventh and
    eighth GPR arguments; the AOT thunk writes them to the outgoing stack area
    and passes a register-class struct that no longer fits wholly on the stack.
    `SDL_ReadSurfacePixel`/`SDL_WriteSurfacePixel` are bound, and
    `tests/native_build_extern_many_args.oskl` pins the AOT spill.
18. **[P2]** `pointerAddress` for a `str` uses `asStr().c_str()`; valid for the
    call, but a footgun if the host mutates it.
19. ~~**[P3]** `installExterns` recurses over children; make it iterative for
    deep module trees.~~ **Done**: `VM::installExterns` now walks the module
    tree with an explicit stack.
20. **[P0] Decided: FFI under `oskel build` is hosted-only.** A program that
    uses `extern "C"` compiles with `oskel build --mode hosted` (Linux x86-64,
    Windows x86-64 and macOS x86-64), which links it against the host C library
    so the symbol resolves to the real C function. The freestanding mode — the
    default on Linux and Windows — has no host to resolve against and reports
    the unresolved symbol (exit 65) instead of emitting a broken binary. macOS
    has no freestanding image at all. This is the design, not a temporary gap:
    the freestanding image is self-contained by construction and a general
    `extern "C"` call is what breaks that. The full 7f surface (structs by
    value and callbacks) is implemented on the System V targets (Linux and
    macOS); Windows supports the scalar, `cstring` and `ptr` classes and a C
    variadic, and rejects the aggregate and callback shapes with a dedicated
    message. A C variadic follows the target's allocation rule: independent
    GPR/SSE counters on System V, argument positions on Win64 (including the
    integer duplicate of a floating value). Documented in `docs/language.md`
    §4.5 and §16.

---

## 8. GC (`src/core/gc.cpp`)

1. **[P1]** `gcCollect` does not run at `SETTABLE`/`GETTABLE` safe points, only
   at `CLOSURE` and `NEWTABLE`; a loop that only mutates tables with cycles
   does not collect until the next `NEWTABLE`. Add the table safe points.
2. **[P1]** `gcCollect` is not reentrant; the `collecting` flag silently
   ignores a nested call. Document it and assert.
3. **[P1]** `GcNode::track()` does not validate that the object is not already
   tracked (`link()` does).
4. **[P1]** `gcCollect` does not handle objects created during collection: a
   `clear` that creates an object moves `registryHead` and can invalidate the
   iterator; pins avoid it but there is no test.
5. **[P2]** `gcCollectIfNeeded` under `OSKEL_GC_STRESS` collects at every safe
   point but is not exercised by any test.
6. **[P2]** `threshold` grows with `trackedCount` without bound.

---

## 9. Value (`src/core/value.cpp`, `src/core/value.h`)

1. **[P2]** `Value::equals` for `Array`/`Map` uses a `visited` vector with a
   linear scan, O(n²) for large cycles. Use a set.
2. **[P2]** `Map::erase` calls `rebuildIndex()` (O(n)), O(n²) for many
   deletions. Maintain the index incrementally.
3. **[P2]** `Range::length` saturates to `INT64_MAX`; `toArray` then fails for
   ranges larger than 10,000,000. Decide the materialization policy.
4. **[P2]** `Value::repr` caps depth at 8, so `print` of a deep array loses
   information at level 9.
5. **[P3]** `utf8Chars` does not validate UTF-8; invalid bytes split into
   non-characters.
6. **[P3]** `appendUtf8` does not validate `cp <= 0x10FFFF`; `chr` checks it,
   the helper does not. The helper has no error channel (it is a low-level
   string encoder), so the range check stays at the call sites that do: the
   lexer's `\u` escape and `chr`. `chr` now also rejects a surrogate
   (`0xD800`..`0xDFFF`) — it used to encode CESU-8, invalid UTF-8 — in the VM
   and in the AOT backend, matching the lexer.
7. **[P3]** `quoteString` escapes `\xHH` for bytes < 0x20 but not for bytes
   > 0x7F; a UTF-8 string renders as bytes, not characters.
8. **[P3]** `formatFloat`/`formatFloat32` have no `-0.0` test.
9. **[P3]** `PtrObj::released` is not reset if a handle is reused; impossible
   today (the `PtrObj` is freed), but assert it.

---

## 10. Errors and diagnostics (`src/core/errors.h`, `src/frontend/diagnostic.cpp`)

1. **[P2]** `RuntimeError::stack` is a `vector<Frame>` copied on every raise; a
   deep stack trace copies O(n) per error.
2. **[P2]** `printSnippet` computes `displayColumns` in bytes while `col` is in
   bytes; for UTF-8 lines the caret can point wrong. The existing test is
   ASCII-only, so it does not catch it.
3. **[P2]** `Warning` has no code/id, so warnings cannot be suppressed
   selectively.
4. **[P2]** `CompileError` has no code, so callers must match strings to
   classify errors.
5. **[P3]** `printRuntimeError` elides frames after 10+10; a 20-frame stack is
   not elided — verify the boundary.

---

## 11. Lexer (`src/frontend/lexer.cpp`)

1. **[P1]** `normalizeNewlines` rewrites the whole source before lexing, so
   `col` counts the normalized source, not the original (`\r\n` is 2 bytes but
   1 column). Make line/col refer to the original.
2. **[P2]** `skipQuoted` for an interpolated string is fragile when a `${...}`
   contains another string with `${...}`.
3. ~~**[P2]** `readEscape` for `\uXXXX` does not handle surrogates (`\uD800`
   becomes invalid UTF-8).~~ **Done**: the lexer rejects the surrogate range
   `U+D800`..`U+DFFF` with `surrogate code point in '\u' escape is not valid
   UTF-8`. The check lives in the lexer, so the VM and the AOT backend reject
   the same source identically. Surrogate pairs stay unsupported.
4. **[P2]** `readEscape` for `\xHH` does not validate the byte in UTF-8.
5. **[P2]** `lex` does not validate that the source is UTF-8.
6. **[P3]** `Token::intNeedsNegation` exists on every token; an
   `std::optional<int64_t>` would be cleaner.
7. **[P3]** `number` treats `1.` as `1` then `.` (member access); correct for
   the language but surprising. Document or reject with a clear message.
8. **[P3]** `operatorToken` does not handle `....` (fails with "unexpected
   character").

---

## 12. Builtins (`src/runtime/builtins.cpp`)

1. **[P3]** `registerBuiltins` for `random`/`seed` share one global engine, so
    two programs in the same process share state; correct in a REPL, document
    it. The engine is `std::mt19937_64`, whose stream the standard specifies;
    the *reduction* is a stated formula in `builtins.cpp` (not a library
    distribution) so it matches the AOT backend bit for bit.
## 13. Ops (`src/runtime/ops.cpp`)

1. **[P1]** `unary` `Minus` on `INT64_MIN` wraps to `INT64_MIN` while the
   compiler's const folding warns; the runtime does not warn. Make the runtime
   consistent with the compile-time policy.
2. **[P1]** `binary` `StarStar` with an int base and a negative exponent falls
   to `std::pow` and yields a double, not an int. The type rule is now
   documented and implemented in both engines (PHASE 7c.1d.2): rank is the
   widest operand's, so a negative int exponent yields a double. Reconsider
   whether it should stay an int (the rule is settled, the choice is not).
3. **[P1]** `binary` `Shl`/`Shr` reject `b < 0 || b > 63`; verify the same rule
   in the AOT path.
4. **[P2]** `indexGet` for a map with a missing key returns `nil` (JS-style)
   while Python raises; decide the language rule and keep VM/AOT in step.
5. **[P2]** `indexSet` for an array with `index == length` pushes (Lua-style);
   decide and test.
6. **[P2]** `memberGet` for a map with a missing non-method member returns
   `nil`; verify the AOT path matches.
7. **[P2]** `binary` `Plus` on two arrays concatenates; done in PHASE 7c.3b
   (`oskel_array_concat`); the VM is the reference and the AOT path matches it.
8. **[P3]** `lessThan` for bool (`!left && right`) is correct but subtle; add a
   test.

---

## 14. dtoa (`src/frontend/native/dtoa.cpp`)

1. **[P1]** No NaN/Inf tests; the special path in `emitFormatDouble` is
   uncovered.
2. **[P1]** No explicit subnormal tests; the `fSub` path
   (`(2^52 + frac52) >> (926 - be)`) is delicate.
3. **[P1]** No stability test across invocations; if the scratch is not cleaned
   the second call can return garbage.
4. **[P2]** `emitBnShl` uses `r9` as scratch in `moveLoop` and reloads it before
   `remLoop`; fragile, assert the invariant.
5. **[P2]** The fixed-vs-scientific rule in `emitFormatDouble` is undocumented
   (`1e6 -> 1e+06`, `1e4 -> 10000`, `1e-4 -> 1e-04`); document the rule.
6. **[P3]** `emitFormatDouble` returns `-0` for `-0.0`; matches
   `std::to_chars`, but add a test.
7. **[P3]** `emitGenerate` is large and complex; needs exhaustive tests.

---

## 15. Codegen (`src/frontend/native/codegen.cpp`)

1. **[P2]** No execution test for `testAhImm8`, `fldM64`/`fstpM64`/`fprem`/
   `fnstswAx` (`test_x87` covers them byte-exactly only).
2. **[P3]** `put8` is a free function, not a member; keep the single byte path
   enforced.
3. **[P3]** `emitBytes` emits byte by byte so fixups keep exact offsets; slow
   for large payloads. Optimize while preserving offsets.

---

## 16. ELF and PE emitters (`src/frontend/native/elf.cpp`, `src/frontend/native/pe.cpp`)

1. **[P1]** `buildStaticElf` never receives `rodata` (always `{}`); either use
   it or drop the parameter.
2. **[P1]** `buildStaticElf` does not set `p_memsz` correctly for `.rodata` when
   its size is not a page multiple; Linux rounds up today, but fix it.
3. **[P1]** `buildStaticElf` error paths (`text.empty()`, `entryOffset` out of
   range) throw `std::runtime_error` but have no tests.
4. **[P2]** `buildStaticPe` ignores `rdata`; either use it or drop it.
5. **[P3]** `writeProgramHeader` hardcodes `p_align = 0x1000`; make it a
   parameter for other page sizes.
6. **[P3]** `padTo` assumes a power-of-two alignment; assert it.

---

## 17. Arena (`src/frontend/native/arena.cpp`)

1. **[P1]** `Arena::allocate` returns null for `alignment == 0` or a
   non-power-of-two alignment; callers must know. Return an explicit error.
2. **[P2]** `Arena::allocate` with `size == 0` consumes one byte; decide and
   document.
3. **[P2]** `Arena` has no `reset()` that keeps the block (`PlatformFreestanding`
   does); add one for symmetry.

---

## 18. Platform

1. **[P1] [done]** `PlatformPosix::lookupSymbol` on Windows did not search
   `kernel32.dll`, so an `extern "C"` to the Win32 API (`GetTickCount`,
   `Sleep`, ...) failed at run time. The search now walks the process image,
   `kernel32.dll` and the C runtime in turn; the executable was already covered
   by `GetModuleHandleA(nullptr)`. Covered by `tests/54_ffi_win32.oskl`.
2. **[P1]** `PlatformPosix::lookupSymbol` on POSIX relies on `dlsym(RTLD_DEFAULT)`,
   which needs `-rdynamic` for the executable's own symbols; make it work.
3. **[P2]** `PlatformFreestanding::alloc` does not validate `alignment == 0` or
   non-power-of-two (the `Arena` caller does).
4. **[P2]** `PlatformFreestanding::open` and `PlatformPosix::open` hardcode mode
   `0644`; make it configurable.
5. **[P2]** `PlatformPosix::write` for `kStdout`/`kStderr` uses buffered
   `std::cout`/`std::cerr`; verify the flush points.
6. **[P3]** `rawSyscall` uses inline asm with a `"memory"` clobber and is not
   MSVC-portable; note the constraint where it is used.
7. ~~**[P2]** `PlatformPosix::lookupSymbol` on Windows does not search the other
   DLLs the process has loaded, so a `std.internal.sdl` program linked against
   `SDL3.dll` still fails to resolve `SDL_*` at run time.~~ **Done**: after the
   process image, `kernel32.dll` and the CRT, the Windows path walks every
   loaded module with `CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ...)` and
   `GetProcAddress`. `platform_lookup_test` loads a DLL with `LoadLibrary` and
   resolves an export that only that DLL provides.

---

## 19. Tests

1. **[P0] [done]** VM/AOT parity exists: `tests/build_binary.cmake` runs each
   `tests/native_build_*.oskl` fixture on both engines (the VM is the
   reference) and asserts identical stdout and exit code, plus the static-link
   check. The list is curated, not exhaustive: a sweep of `tests/*.oskl` shows
   48 of them build in AOT and agree with the VM, while the rest use a
   collection/string method the AOT backend does not implement (`pcall` is no
   longer a gap, see §6.4). Widening the curated list to every agreeing
   script is the remaining work; `tests/native_build_control_flow.oskl`
   (registered as `oskel.build_binary_control_flow`) closes the `switch`/
   `defer`/`struct` hole the earlier fixtures left.
2. **[P0]** Add an AOT test that `try`/`catch` catches an error, `pcall`
   recovers, `error()` works and structural equality matches the VM.
   **Partial**: `tests/native_build_error.oskl` covers `error()`, `isError`,
   `type`, `.message`/`.code`, equality, `print`/`repr`, interpolation and
   `catch` recovering a returned error, and it is built and run against the VM
   by `tests/build_binary.cmake`. `pcall` is still unimplemented in AOT
   (backlog §6.4).
3. **[P1]** Add an AOT wrong-arity test.
4. **[P1]** Add `map`/`filter`/`reduce` tests with a VM closure callback.
5. **[P1] [done]** Add an `ITER`-over-a-non-iterable test.
   `tests/errors/for_in_not_iterable.oskl` pins that `for x in 5` is a runtime
   error naming the type (`cannot iterate over a value of type 'int'`), and
   `testIterValueFormRejectsNonMap` in `tests/vm_test.cpp` covers the key/value
   form over an array, a string and a range.
6. **[P1]** Add dtoa NaN/Inf, explicit subnormal and cross-invocation stability
   tests.
7. **[P1]** Add an `OSKEL_GC_STRESS` test run.
8. **[P1]** Add `lower_test.cpp` cases for `RETURN` in the root chunk and for a
   `JMP` to exactly `size`.
9. **[P1]** Add `vm_test.cpp` cases for `GETUPVAL`/`SETUPVAL` with a null cell,
   `runFrames` with `fail()` outside `call()`, and `RETURN` in the root chunk.
10. **[P1]** Add `compiler_test.cpp` cases for a `Point` alias plus a
    `struct Point`, `try` inside a block with a `defer`, and `catch` with a
    fallback block that has a `defer`.
11. **[P1]** Add `module_test.cpp` cases for two `extern "C"` of the same name
    in different modules and for `import` with `from` as a user variable.
12. **[P2]** Add `repl_test.cpp` for "import X, fail, import X again runs the
    body once".
13. **[P2]** Add `elf_test.cpp` for non-empty `rodata`, `entryOffset != 0` and
    the error paths.
14. **[P2]** Add `pe_test.cpp` error-path tests.
15. **[P2]** Add `codegen_test.cpp` for `callRel`, `emitBytes`, `ucomisd`,
    all-register `push`/`pop`, and execution of the x87 ops.
16. **[P3]** Remove the debug `printf`s in `vm_test.cpp`
    (`testRegisterCount`), `pe_test.cpp` and `compiler_test.cpp`
    (`testLeftAssociativePrecedence`).

---

## 20. Tooling and project

1. **[P1]** Add `clang-format` with a committed config and a check in CI.
2. **[P1]** Add `-Werror` to the build (at least in CI) so warnings cannot
   accumulate.
3. **[P1] Done.** `Env` (`src/env.h`/`src/env.cpp`) was deleted: the VM keeps
   locals in reference-counted `Cell`s, so `Env` and `Function::closure` were
   dead code kept only by `tests/gc_test.cpp`. The three `gc_test.cpp` cases
   were rewritten over `Cell` and `Function::upvalues`, the mechanism the VM
   actually uses, and the `envGcOps` table was removed from `value.cpp`.
4. **[P2] Decided: keep it.** The JIT is on by default in the development
   engine (`oskel`/REPL/`-e`; `--no-jit` disables it) and gives ~420x on the
   integer leaf subset it covers (measured on a tight `while` loop, after the
   slice-4 register allocation). It is not "unused": it ships in `oskel`. It
   stays a development tool and is never in `oskelc`/`oskel build`. The
   remaining work is the later PHASE 8 slices
   (floats, strings, the boxed runtime, calls), not a freeze.
5. **[P2] [done]** `CHANGELOG.md` exists and follows Keep a Changelog; keep it
   updated before each tag (see `docs/RELEASING.md`).
6. **[P2]** Add `CONTRIBUTING.md` (the rules now live only in
   `DEVELOPMENT.md`/`AGENTS.md`).
7. **[P2] [done]** Add an `oskel.toml` for native dependencies.
   `oskelp` (the project and package manager) landed with `oskel.toml`,
   `oskel.lock`, path dependencies and
   `new`/`init`/`add`/`remove`/`build`/`run`/`check`/`test`/`clean`/`tree`. The
   manifest is parsed by a small strict TOML subset in `tools/oskelp.cpp`; the
   resolver rejects cycles, two locations for one package name and a name that
   disagrees with its manifest, and resolves a local-registry dependency from
   `$OSKEL_REGISTRY`. The tool is standalone and drives `oskel`/`oskelc` as
   child processes; `OSKEL_MODULE_ROOT` is the only compiler-side hook.
8. **[P3]** Add a "hello world with UI" tutorial. The widget layer
   (`std/ui.oskl`) and the public window loop (`std.ui.window`) exist now, so
   the tutorial can use `examples/ui/window.oskl` as its starting point.
9. **[P2] [partial]** SDL3 bindings landed (post-1.0 UI): `std/internal/sdl.oskl` binds
   the window, renderer and event surface, `docs/sdl.md` documents it, and
   `oskel build --mode hosted --link-lib SDL3` compiles it. The C++ bridge
   (`bindgen --cxx`, `docs/cxxbind.md`) now generates Oskel classes plus a C++
   shim from a descriptor. Factory objects, `sk_sp`/`unique_ptr` and value
   returns, borrowed handles and `Class&` parameters landed, and
   `bindings/skia/skia.oskelbind` compiles against a real Skia build; the
   opt-in `cxxbind.skia` test draws, reads pixels and renders a widget tree on
   the VM and the hosted AOT binary. `std/ui.oskl` is the first declarative
   layer (`window`, `column`, `row`, `padding`, `spacer`, `text`, `button`;
   layout, `rect`/`text` display list and hit testing), and
   `std/internal/skia_ui.oskl` renders it. `std.ui.window` now presents the
   raster surface through a streaming `SDL_Texture`, runs the event loop and
   dispatches left clicks through `hitTest`; `std.internal.sdl` and the Skia
   modules stay internal. Still pending: input widgets beyond a button, real
   font metrics, text wrapping and a dirty-region redraw.
10. **[P3]** Add `dlopen`/`dlsym` as a builtin for dynamic FFI.
11. **[P1] [done]** `oskelp`: the local registry (`$OSKEL_REGISTRY`) landed
    with semver requirements (exact, `^`, `~`, comparators, wildcards and a
    conjunction), so a dependency can be `utils = "^1.2.0"` instead of a path.
    `oskel.lock` records `registry+<name>@<version>` and keeps the pin while it
    satisfies the manifest. A network registry is still a separate decision: it
    needs HTTP/TLS, which the standard library does not carry, plus checksums
    and signatures.
12. **[P1] [done]** `oskelp`: `add`, `remove`, `publish` and `update` landed.
    `add` and `remove` use an in-place editor that preserves comments, blank
    lines and CRLF; a failed add resolves the graph before it writes, so the
    manifest is never left half-edited. `add` without `--path` writes a registry
    requirement, `publish` copies a package into `$OSKEL_REGISTRY`, and
    `update [<name>]` moves the lockfile pin and prints each change.
13. **[P2]** `oskelp`: add build profiles. `target/<name>` is the only output
    today because the AOT backend has no optimization level to select; a
    `--release` that changes nothing would be a cosmetic lie.
14. **[P2]** `oskelp`: add workspaces (several packages sharing one lockfile)
    and a `@Test` attribute, so a test file is not limited to a whole program
    that exits `0`.
