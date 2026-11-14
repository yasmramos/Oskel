# Oskel backlog — things to fix

Consolidated, deduplicated list of known defects, divergences and hardening
gaps, ordered by area. Everything here is a task to resolve, not a limitation
to accept. No item is "by design, leave it": if it stays, it must be a
deliberate decision recorded in `DEVELOPMENT.md`, otherwise it is work.

Priority tags: **[P0]** correctness or memory-safety bug, **[P1]** semantic
divergence or robustness, **[P2]** performance, **[P3]** cleanup or tooling.

---

## 1. VM (`src/vm.cpp`)

1. **[P1]** `ITER` over a map yields only the key; `for k, v` then does a
   `$mapValue` lookup per iteration. Yield the value directly.
2. **[P1]** `ITER` (and `compileForIn`) uses `instruction.jump` as the output
   register. Replace the private convention with a real operand or a distinct
   opcode.
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

## 2. Parser (`src/parser.cpp`)

1. **[P0]** `resolveTopLevelConsts` builds `reordered` but it is identical to
   the input: the hoisting the comment promises is not implemented, and the
   inline comment is in Spanish (language-rule violation). Either implement the
   hoist or delete the dead buffer and the comment.
2. **[P1]** `parseStructLiteralBody` does not support the shorthand
   `Point { x, y }`.
3. **[P1]** `parseSwitch` accepts only literals and ranges as `case` labels;
   a `const` label (`case FOO:`) fails. Accept constant expressions.
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

## 3. Compiler (`src/compiler.cpp`)

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
5. **[P1]** `compileImport` does not reject an alias that collides with a
   declared struct (an alias `Point` and a `struct Point` coexist; the winner
   is undefined).
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

## 4. AST (`src/ast.h`)

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

## 5. JIT (`src/jit.cpp`)

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

## 6. AOT backend (`src/native/lower.cpp`, `src/native/runtime.cpp`)

1. **[P0]** VM/AOT divergence in collection methods: `arr.push`, `map.keys`,
   `str.upper`, ... work in the VM and fail in AOT with "no such member".
2. **[P0]** VM/AOT divergence in structural equality: `[1,2] == [1,2]` is true
   in the VM (content) and false in AOT (identity). A semantic change, not just
   speed.
3. **[P0]** VM/AOT divergence in `try`/`catch`: `$isError` in AOT always
   returns false, so `try`/`catch` never catches; a runtime error kills the
   process (exit 70) with no recovery.
4. **[P0]** `pcall` is not implemented in AOT; a program that uses it does not
   compile or fails at run time.
5. **[P0]** `error()`/`makeError` is not (or only partially) implemented in
   AOT; errors as values do not behave the same.
6. **[P1]** VM/AOT divergence in `map.get`: the VM uses `unordered_map` (O(1)),
   AOT uses a linear scan (O(n)), so a loop over a large map is O(n²).
7. **[P1]** VM/AOT divergence in string iteration: AOT allocates one
   `AotString` per character; a 1M-char string means 1M arena allocations.
8. **[P1]** `emitContains` for arrays uses `AotCompareOp::Eq`, which for
   arrays/maps is identity, not content — inconsistent with the VM.
9. **[P1]** `emitMemberGet` for array/string supports only `.length`, no
   methods — same divergence as item 1.
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

---

## 7. FFI (`src/ffi.cpp`)

1. **[P1]** `extern "C"` does not support variadics; `printf` cannot be
   declared. Provide a way (or a documented reason it is out of scope).
2. **[P1]** `extern "C"` does not support structs larger than 16 bytes by value
   (System V passes them via a hidden pointer / `sret`).
3. **[P1]** `extern "C"` does not support callbacks returning structs (scalars
   only).
4. **[P1]** `extern "C"` does not support nested callbacks (a callback that
   takes another callback).
5. **[P1]** `extern "C"` does not support `long double`.
6. **[P1]** `extern "C"` does not support `__int128`.
7. **[P1]** `extern "C"` does not support arrays by value.
8. **[P1]** `extern "C"` does not support C unions.
9. **[P1]** `extern "C"` does not support bitfields.
10. **[P1]** `extern "C"` does not support C enums (only `int`).
11. **[P1]** `installExtern` does not detect two `extern "C"` declarations of
    the same name in different modules; the last wins silently.
12. **[P1]** `wrapPointerValue` for `cstring` calls `strlen`, which reads past
    the buffer if the pointer is not NUL-terminated — UB from a host that
    returns a non-terminated `char *`. Bound it or reject.
13. **[P1]** `oskellCallbackTrampoline` catches `std::exception` with a generic
    message; a non-`RuntimeError` type is lost.
14. **[P1]** `CallbackObj::pendingError` is not reset between invocations; a
    second call sees the first call's pending error. Reset it per call.
15. **[P2]** `makeExternNative` does not validate `address != nullptr` (the
    caller does).
16. **[P2]** `makeCallback` does not validate that the value is a `Func` before
    building the `CallbackObj` (the caller does).
17. **[P2]** `dispatch` for callbacks is limited to `kMaxExternArgs = 6`; the
    error message does not say so.
18. **[P2]** `pointerAddress` for a `str` uses `asStr().c_str()`; valid for the
    call, but a footgun if the host mutates it.
19. **[P3]** `installExterns` recurses over children; make it iterative for
    deep module trees.

---

## 8. GC (`src/gc.cpp`)

1. **[P1]** `gcCollect` does not run at `SETTABLE`/`GETTABLE` safe points, only
   at `CLOSURE` and `NEWTABLE`; a loop that only mutates tables with cycles
   does not collect until the next `NEWTABLE`. Add the table safe points.
2. **[P1]** `gcCollect` is not reentrant; the `collecting` flag silently
   ignores a nested call. Document it and assert.
3. **[P1]** `GcNode::track()` does not validate that the object is not already
   tracked (`link()` does).
4. **[P1]** `Env` uses `weak_from_this().use_count()` for its refcount, which is
   fragile if the object is not owned by a `make_shared`; the code assumes it.
5. **[P1]** `GcNode::ops->pin` for `Env` uses `shared_from_this()`, which throws
   if the `Env` is not owned by a `shared_ptr`.
6. **[P1]** `gcCollect` does not handle objects created during collection: a
   `clear` that creates an object moves `registryHead` and can invalidate the
   iterator; pins avoid it but there is no test.
7. **[P2]** `gcCollectIfNeeded` under `OSKEL_GC_STRESS` collects at every safe
   point but is not exercised by any test.
8. **[P2]** `threshold` grows with `trackedCount` without bound.

---

## 9. Value (`src/value.cpp`, `src/value.h`)

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
   the helper does not.
7. **[P3]** `quoteString` escapes `\xHH` for bytes < 0x20 but not for bytes
   > 0x7F; a UTF-8 string renders as bytes, not characters.
8. **[P3]** `formatFloat`/`formatFloat32` have no `-0.0` test.
9. **[P3]** `PtrObj::released` is not reset if a handle is reused; impossible
   today (the `PtrObj` is freed), but assert it.

---

## 10. Errors and diagnostics (`src/errors.h`, `src/diagnostic.cpp`)

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

## 11. Lexer (`src/lexer.cpp`)

1. **[P1]** `normalizeNewlines` rewrites the whole source before lexing, so
   `col` counts the normalized source, not the original (`\r\n` is 2 bytes but
   1 column). Make line/col refer to the original.
2. **[P2]** `skipQuoted` for an interpolated string is fragile when a `${...}`
   contains another string with `${...}`.
3. **[P2]** `readEscape` for `\uXXXX` does not handle surrogates (`\uD800`
   becomes invalid UTF-8).
4. **[P2]** `readEscape` for `\xHH` does not validate the byte in UTF-8.
5. **[P2]** `lex` does not validate that the source is UTF-8.
6. **[P3]** `Token::intNeedsNegation` exists on every token; an
   `std::optional<int64_t>` would be cleaner.
7. **[P3]** `number` treats `1.` as `1` then `.` (member access); correct for
   the language but surprising. Document or reject with a clear message.
8. **[P3]** `operatorToken` does not handle `....` (fails with "unexpected
   character").

---

## 12. Builtins (`src/builtins.cpp`)

1. **[P1]** `bindString` `upper`/`lower` use `std::tolower`/`std::toupper` with
   `unsigned char` (ASCII only); `"Á".upper()` does not work. Use a Unicode
   case table.
2. **[P1]** `registerBuiltins` for `pcall` catches `RuntimeError` but not
   `CompileError`; decide and handle.
3. **[P2]** `sortItems` uses `std::stable_sort` with a comparator that calls
   `in.call`; if it throws, the array can be left inconsistent. Guard it and
   test a throwing comparator.
4. **[P2]** `bindArray` `map`/`filter`/`reduce`/`each` invoke the callback
   through `in.call`, which reenters the VM; add a direct test with a VM
   closure.
5. **[P2]** `bindString` `split` with no argument uses `std::istringstream`
   (whitespace, no quotes).
6. **[P2]** `bindString` `padStart`/`padEnd` build the padding then trim it
   with `utf8Chars`, O(n) extra for large paddings.
7. **[P2]** `args()` returns `scriptArgs` without the script name; decide the
    contract (Lua-style) and document it.
8. **[P3]** `bindArray` `push` returns `self`, not `nil`; `sort`/`merge` mutate
    in place and return `self`. Decide the return-value contract.
9. **[P3]** `registerBuiltins` for `random`/`seed` share one global engine, so
    two programs in the same process share state; correct in a REPL, document
    it.
10. **[P3]** `cstring` copies the string with `\0`, truncating at an embedded
    `\0`; expected for C, but document it.

---

## 13. Ops (`src/ops.cpp`)

1. **[P1]** `unary` `Minus` on `INT64_MIN` wraps to `INT64_MIN` while the
   compiler's const folding warns; the runtime does not warn. Make the runtime
   consistent with the compile-time policy.
2. **[P1]** `binary` `StarStar` with an int base and a negative exponent falls
   to `std::pow` and yields a double, not an int. Decide and document the type
   rule.
3. **[P1]** `binary` `Shl`/`Shr` reject `b < 0 || b > 63`; verify the same rule
   in the AOT path.
4. **[P2]** `indexGet` for a map with a missing key returns `nil` (JS-style)
   while Python raises; decide the language rule and keep VM/AOT in step.
5. **[P2]** `indexSet` for an array with `index == length` pushes (Lua-style);
   decide and test.
6. **[P2]** `memberGet` for a map with a missing non-method member returns
   `nil`; verify the AOT path matches.
7. **[P2]** `binary` `Plus` on two arrays concatenates; decide and document.
8. **[P3]** `lessThan` for bool (`!left && right`) is correct but subtle; add a
   test.

---

## 14. dtoa (`src/native/dtoa.cpp`)

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

## 15. Codegen (`src/native/codegen.cpp`)

1. **[P2]** No execution test for `testAhImm8`, `fldM64`/`fstpM64`/`fprem`/
   `fnstswAx` (`test_x87` covers them byte-exactly only).
2. **[P3]** `put8` is a free function, not a member; keep the single byte path
   enforced.
3. **[P3]** `emitBytes` emits byte by byte so fixups keep exact offsets; slow
   for large payloads. Optimize while preserving offsets.

---

## 16. ELF and PE emitters (`src/native/elf.cpp`, `src/native/pe.cpp`)

1. **[P1]** `buildStaticElf` never receives `rodata` (always `{}`); either use
   it or drop the parameter.
2. **[P1]** `buildStaticElf` does not set `p_memsz` correctly for `.rodata` when
   its size is not a page multiple; Linux rounds up today, but fix it.
3. **[P1]** `buildStaticPe` has no relocations; a modern PE without `.reloc` is
   rejected by Windows "Force ASLR". Emit `.reloc` or set the image flags.
4. **[P1]** `buildStaticElf`/`buildStaticPe` error paths (`text.empty()`,
   `entryOffset` out of range) throw `std::runtime_error` but have no tests.
5. **[P2]** `buildStaticPe` hardcodes `.idata` at 99 bytes and several offsets
   (`0x2028`, `0x2056`, `0x2038`, `0x2048`); compute them from the layout.
6. **[P2]** `buildStaticPe` ignores `rdata`; either use it or drop it.
7. **[P3]** `writeProgramHeader` hardcodes `p_align = 0x1000`; make it a
   parameter for other page sizes.
8. **[P3]** `padTo` assumes a power-of-two alignment; assert it.

---

## 17. Arena (`src/native/arena.cpp`)

1. **[P1]** `Arena::allocate` returns null for `alignment == 0` or a
   non-power-of-two alignment; callers must know. Return an explicit error.
2. **[P2]** `Arena::allocate` with `size == 0` consumes one byte; decide and
   document.
3. **[P2]** `Arena` has no `reset()` that keeps the block (`PlatformFreestanding`
   does); add one for symmetry.

---

## 18. Platform

1. **[P1]** `PlatformPosix::lookupSymbol` on Windows does not search
   `kernel32.dll` or the executable; extend the search.
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

---

## 19. Tests

1. **[P0]** Add `parity_test.cpp` comparing VM and AOT on the same programs;
   the divergences in §6 are only catchable this way.
2. **[P0]** Add an AOT test that `try`/`catch` catches an error, `pcall`
   recovers, `error()` works and structural equality matches the VM.
3. **[P1]** Add an AOT wrong-arity test.
4. **[P1]** Add `map`/`filter`/`reduce` tests with a VM closure callback.
5. **[P1]** Add an `ITER`-over-a-non-iterable test.
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
3. **[P1]** Decide the fate of `Env` (`src/env.h`/`src/env.cpp`): it is still
   referenced from `value.*`; either use it or delete it.
4. **[P2]** Decide the fate of the JIT: it is limited and unused in the
   product. Freeze it behind a flag or extend it; do not leave it half-done.
5. **[P2]** Add `CHANGELOG.md`.
6. **[P2]** Add `CONTRIBUTING.md` (the rules now live only in
   `DEVELOPMENT.md`/`AGENTS.md`).
7. **[P2]** Add a `oskel.toml` for native dependencies.
8. **[P3]** Add a "hello world with UI" tutorial (needs the post-1.0 UI work).
9. **[P3]** Add a C wrapper for Skia/SDL2 (post-1.0 UI).
10. **[P3]** Add `dlopen`/`dlsym` as a builtin for dynamic FFI.
