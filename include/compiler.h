#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bytecode.h"
#include "core/errors.h"
#include "ffi_types.h"
#include "frontend/ast.h"
#include "frontend/parser.h"

// Signature of a `native fn`/`extern "C"` declaration (PHASE 6.5), keyed by
// name. A native declaration emits no call to the compiler: it only records the
// signature so a call can be checked (arity and argument types) and so the
// return type can be inferred.
struct NativeSig {
  std::vector<Param> params;
  TypeRef returnType;
  bool mustUse = false;
  // True for a C variadic (`extern "C" fn printf(fmt: cstring, ...): int`).
  // `params` then holds the named prefix; a call may pass any number of extra
  // arguments, which the call-site check does not type-check (C applies the
  // default promotions at run time).
  bool cVariadic = false;
};

// The argument types and positions of one native call, captured while the call
// site's scopes are still open. `checkNativeCall` needs them for the arity and
// argument-type check, but a qualified call to a module's native export
// (`std.math.pow(...)`) can only be checked once the module is loaded, by which
// time the scopes are gone. Capturing the inferred types up front keeps the
// check identical to the direct form: inferring at flush time would resolve a
// local that shadows a global to the global (PHASE 7f.2).
struct NativeCallArgs {
  std::vector<TypeRef> types;
  std::vector<int> lines;
  std::vector<int> cols;
};

// One resolved import: a module's compiled body plus the names it exports.
//
// `code` is a proto that, when run, evaluates the module's top-level bindings
// as locals of its own frame and returns a table mapping each public name to
// its value. Keeping the bindings local to the module frame is what stops a
// module's internals from leaking into the importing program's globals.
struct ImportedModule {
  std::string path;                    // dotted module path, e.g. "std.math"
  std::shared_ptr<Bytecode> code;      // module body, run once per program
  std::vector<std::string> symbols;    // public names, in declaration order
  // A class module is a module whose last path segment starts with an uppercase
  // letter. It must export exactly one public name, equal to that segment; a
  // qualified import binds the export itself (the class constructor), not the
  // module table, so `import std.io.StringReader;` makes `StringReader(...)`
  // callable. `className` is the single public name when `classModule` is true.
  bool classModule = false;
  std::string className;
  // Signature of every public native export, keyed by export name (PHASE 7f.2).
  // The public names that are host bindings (`native fn` or `extern "C"`) are
  // exactly the keys: the AOT backend has no host to resolve one against, so a
  // qualified access to one (`std.math.pow(...)`) is a use of a binding that may
  // have no stub. Propagating the signature (not just the name) is what lets a
  // qualified call be checked at compile time — arity and argument types —
  // exactly like the direct form (`pow(2.0)`), instead of failing at run time.
  std::unordered_map<std::string, NativeSig> nativeSigs;
  // The enums the module declares, keyed by enum name (PHASE 6.9 across
  // modules). A module's enums are compile-time metadata, not values in the
  // module table, so they travel beside `symbols` instead of inside it.
  std::unordered_map<std::string, std::vector<EnumMember>> enums;
};

// AST to bytecode compiler (PHASE 2-5).
//
// Translates the language into the register-based instructions of
// `include/bytecode.h`. When it meets a node that does not belong to the
// current phase yet, it throws `CompileError` with line/column, so the
// pipeline can report it in the same format as the parser.
//
// Current scope:
//   - nil, boolean, numeric and string literals
//   - global variables and block-local variables
//   - simple and compound assignment
//   - arithmetic (+ - * / % **), comparisons, unary operators and bitwise ops
//   - if/else, while, classic for, break and continue
//   - short-circuit operators && and ||, and the ternary operator
//   - print(expr) with a single argument
//   - top-level `fn` declarations, parameters, `return` and general calls
//   - lambdas, nested/local `fn` declarations and closures over captured
//     locals (upvalues resolved at compile time)
//   - arrays, maps, ranges, interpolation, indexing, members and `in`
//   - `for-in` over arrays, strings, maps and ranges
//   - `switch` on literals and integer ranges (no fallthrough)
//   - Java-style `import` (qualified, wildcard, alias and selective forms)
//     with compile-time conflict detection
class Compiler {
 public:
  Compiler() = default;

  // Compiles a whole program. The resulting bytecode ends with HALT.
  Bytecode compile(const StmtPtr& program);

  // Identifiers of every module the last `compile` loaded: a dotted name
  // (`std.ui`) or a resolved path for a file import. A build uses this to see
  // whether the program reaches the native UI backend, which the bytecode
  // alone cannot tell: those modules export ordinary `oskel_*` functions, a
  // shape user bindings also use.
  std::vector<std::string> loadedModulePaths() const;

  // Compiles a single expression into a chunk that ends with RETURN, so the
  // VM can hand the value back to the REPL. Globals persist across chunks.
  Bytecode compileExpression(const ExprPtr& expr);

  // Compiles a REPL entry. When the entry ends with an expression statement,
  // the chunk ends with RETURN so the VM hands its value back; otherwise it
  // ends with HALT like a regular program. Top-level declarations are hoisted
  // exactly as in `compile`.
  Bytecode compileRepl(const StmtPtr& program);

  // Forgets the definitions carried from one compilation to the next: global
  // immutability and types, `struct` shapes, native signatures, and the
  // imported modules and namespaces they brought in. `compile` resets the
  // session automatically, so a file always starts clean. The REPL keeps it
  // across entries and clears it on `:clear`, so its behaviour matches one
  // accumulating program instead of a sequence of unrelated ones.
  void clearSession();

  // The `struct` names declared so far in the session. The REPL hands them to
  // the next entry's parser (see Parser::seedStructNames) so a literal or an
  // annotation written after the declaration is still recognized.
  std::unordered_set<std::string> structNames() const {
    std::unordered_set<std::string> names;
    for (const auto& entry : structDefs_) names.insert(entry.first);
    return names;
  }

  // The `class` names declared so far in the session, for the same reason as
  // `structNames`: a class is erased to a plain constructor `fn`, so the parser
  // only learns the name denotes a type if the REPL seeds it (see
  // Parser::seedClassNames).
  const std::unordered_set<std::string>& classNames() const { return classNames_; }

  // The `enum` names declared so far in the session (PHASE 6.9). The REPL hands
  // them to the next entry's parser (see Parser::seedEnumNames) so `Color.RED`
  // and a bare member keep resolving after the entry that declared the enum.
  const std::unordered_set<std::string>& enumNames() const { return enumNames_; }

  // The folded value of every enum member in the session, keyed by its
  // qualified name (`Color.RED`), and the bare-member index. The REPL hands
  // both to the next entry's parser so a `const` initializer keeps folding.
  std::unordered_map<std::string, Value> enumValues() const;
  const std::unordered_map<std::string, std::vector<std::string>>& enumMemberOwners() const {
    return enumMemberOwners_;
  }

  // Supplies the cached module-enum scanner the parser already uses, so
  // `registerImportedEnums` parses each module at most once per session. The
  // scanner returns a module's own enum declarations, keyed by enum name.
  void setEnumScanner(Parser::EnumScanner scanner) { enumScanner_ = std::move(scanner); }

  // The warnings collected by the last compilation (PHASE 6.7.5). Warnings are
  // non-fatal: the program still runs. The CLI prints them to stderr; tests
  // inspect them. Cleared at the start of every compilation.
  const std::vector<Warning>& warnings() const { return warnings_; }

  // Provides the module loader used to resolve `import` declarations. The
  // callback receives a dotted module path and the position of the import
  // declaration, and returns the module's parsed program, or throws
  // `CompileError` ("module 'X' not found") when it cannot be resolved. The
  // compiler stays free of any file-system or resolver dependency, which keeps
  // the toolchain boundary (DEVELOPMENT.md §2) intact; the CLI injects the loader,
  // and tests inject their own. Without a loader, `import` reports that the
  // module cannot be resolved.
  using ImportLoader = std::function<StmtPtr(const std::string& path, int line, int col)>;
  void setImportLoader(ImportLoader loader) { importLoader_ = std::move(loader); }

  // Makes a module's compile-time interfaces visible to `compileImport`
  // (PHASE 12.2). A selective import may name an interface: it is a valid
  // export but has no runtime table entry, so the compiler validates it and
  // emits no binding for it. The CLI wires this to the same cached scanner the
  // parser uses.
  void setInterfaceScanner(Parser::InterfaceScanner scanner) {
    interfaceScanner_ = std::move(scanner);
  }

  // The ABI capabilities `extern "C"` declarations are validated against. The
  // default is this build's, which is what the interpreter marshals; the AOT
  // path sets `ffi::kAotAbiCapabilities` because the backend emits its own
  // thunks and does not depend on how the engine was built. Without this, a
  // shape the backend can emit (a class-MEMORY struct) would be rejected by
  // `oskel build` on an engine without a host FFI library.
  void setAbiCapabilities(ffi::AbiCapabilities caps) { abiCapabilities_ = caps; }

  // Compiles a program that runs *in the current session* instead of starting
  // a clean one. `:load` uses this so a loaded file sees the session's structs,
  // types and imports, and merges its own definitions into the session, unlike
  // `compile` which clears it. `loader` resolves the file's own imports
  // relative to the file; it is used only while this call compiles. The
  // session is snapshotted first, so a file that fails to compile leaves it
  // untouched. `name` labels the chunk (the file path) for diagnostics.
  Bytecode compileLoaded(const StmtPtr& program, ImportLoader loader, const std::string& name);

  // Compiles a module body into a standalone proto: its top-level bindings are
  // locals of the proto's frame (not globals) and the proto returns a table of
  // its public names. `path` is the dotted module path. Used by the module
  // loader when it compiles an imported file.
  ImportedModule importModule(const StmtPtr& program, const std::string& path);

 private:
  struct LocalVar {
    std::string name;
    int slot = 0;
    bool isImmutable = false;
    // Declared or inferred type (PHASE 6.5). Empty means "unknown"/any. It is
    // compile-time metadata only: it never reaches the bytecode.
    TypeRef type;
    // True when this binding is a `native fn` declaration. A call to the name
    // is checked against the native's signature; a plain local that shadows
    // the name is a different function, so the check is skipped for it.
    bool isNative = false;
    // True when the binding was declared `@MustUse` (PHASE 6.7.5). Discarding
    // a call to it is a warning. A local that shadows the name is a different
    // function, so it carries its own flag.
    bool isMustUse = false;
  };

  struct Scope {
    std::unordered_map<std::string, LocalVar> vars;
    int baseSlot = 0;
    // Deferred blocks (`defer { ... }`) declared directly in this scope, in
    // source order. They are not emitted where they appear: the compiler runs
    // them (in reverse order) at every point where the scope exits -- normal
    // fall-through, `return`, or `break`/`continue` that leaves the scope.
    std::vector<StmtPtr> defers;
  };

  struct Loop {
    std::vector<int> breaks;
    std::vector<int> continues;
    // Deepest scope whose deferred blocks `break`/`continue` must run before
    // jumping out: a 1-based scope count, so `emitDeferredBlocks(deferMinDepth)`
    // runs every scope from the innermost down to and including it.
    size_t deferMinDepth = 0;
  };

  // Marks that the deferred body currently being emitted is a `defer`
  // boundary. A `break`/`continue` that would leave that boundary is rejected,
  // because the deferred block runs at a scope exit and letting it jump out
  // would make the execution order ambiguous. `loopDepth` is `loops_.size()`
  // when the body started, so a loop declared *inside* the deferred body (which
  // raises the count) may still be broken out of. `return` needs no field: the
  // whole stack is cleared while compiling a nested function, so a `return`
  // there always belongs to that function.
  struct DeferCtx {
    size_t loopDepth = 0;
  };

  // One entry of the proto stack. `compileFunctionProto` saves the current
  // state, compiles the child body into a fresh chunk and restores the parent
  // afterwards, so upvalue resolution can look at every enclosing function at
  // once (a lambda inside a function body sees that body's locals as capture
  // candidates). The stack holds every frame except the one being compiled,
  // which lives in the plain members below.
  struct FuncState {
    Bytecode bc;
    std::vector<Scope> scopes;
    std::vector<Loop> loops;
    std::vector<DeferCtx> deferStack;
    int nextReg = 0;
    int maxReg = 0;
    int nextLocal = 0;
    int maxLocal = 0;
    int line = 1;
    int col = 1;
    bool inFunction = false;
    // Capture list of this frame, keyed by variable name and parallel to
    // `bc.upvalues`, so a name that is used twice is captured once.
    std::unordered_map<std::string, int> upvalueNames;
    // The frame one level out, or null when this is the outermost function.
    // Valid pointers into `funcStack_` (deque nodes are stable); rebuilt on
    // every push/pop of the proto stack.
    FuncState* enclosing = nullptr;
  };

  // Upvalue resolution helpers (PHASE 6b). A function captures a variable it
  // uses but does not declare. The capture is recorded in the capture list of
  // the function that uses the variable (`proto`, with the name-keyed index
  // `names`), and its descriptor says where `CLOSURE` finds the cell when it
  // instantiates that function:
  //   - the variable is a local of the enclosing function (`parent`): the
  //     cell is that function's local slot (`inParentLocal`);
  //   - otherwise the enclosing function captures it too, and the cell is one
  //     of its upvalues, so nested closures share one cell per variable.
  // Returns -1 when no enclosing function declares the name, i.e. it is a
  // global.
  static bool resolveLocalIn(const std::vector<Scope>& scopes, const std::string& name,
                             LocalVar& out);
  static int resolveFromFrame(Bytecode& proto, std::unordered_map<std::string, int>& names,
                              FuncState* parent, const std::string& name);

  Bytecode bc_;
  int nextReg_ = 0;  // next free temporary register
  int maxReg_ = 0;   // maximum number of registers in use at once
  int nextLocal_ = 0;
  int maxLocal_ = 0;

  std::vector<Scope> scopes_;
  std::vector<Loop> loops_;
  // One entry per `defer` body currently being emitted (innermost last). A
  // `break`/`continue` that would leave the innermost defer boundary is
  // rejected; the whole stack is cleared while compiling a nested function.
  std::vector<DeferCtx> deferStack_;
  // Set by `compileTry` when it emits the conditional `RETURN` of a `try`. It
  // lets a trailing `return try f();` recover the normal return: the last
  // instruction is a RETURN, but it only runs on the error path, so the
  // "already returns" shortcut in `compileReturn` must not apply.
  bool lastWasConditionalReturn_ = false;

  // Slots reserved for the direct declarations of the block currently being
  // compiled, keyed by the statement node. The pre-pass in `compileBlockBody`
  // allocates them in source order and creates their cells before hoisted
  // functions are compiled, so a function declared after a block-local `let`
  // can capture it. `compileLet`/`compileFnDecl` reuse the reserved slot
  // instead of allocating another one.
  std::unordered_map<const LetStmt*, int> predeclaredLetSlots_;
  std::unordered_map<const FnDeclStmt*, int> predeclaredFnSlots_;
  // Proto stack: every enclosing function except the one being compiled.
  // A deque keeps FuncState addresses stable, so `enclosing` pointers stay
  // valid while more frames are pushed.
  std::deque<FuncState> funcStack_;
  std::unordered_set<std::string> globalConsts_;

  // Names the importing module declares itself (`let`/`var`/`fn`) in its global
  // scope. A wildcard or selective import must not silently overwrite one of
  // them: the local binding wins, whatever the order of the declarations. Kept
  // separate from `globalConsts_`, which is about immutability (const folding),
  // not about shadowing. Reset per chunk.
  std::unordered_set<std::string> userGlobals_;

  // Type of every global binding, for annotation checks (PHASE 6.5). Kept
  // parallel to `globalConsts_`; both are reset per chunk.
  std::unordered_map<std::string, TypeRef> globalTypes_;

  // Field shapes of the `struct` types declared so far (PHASE 6.5), keyed by
  // type name. A struct is compile-time metadata: the declaration emits no
  // bytecode, and a `Point { ... }` literal is checked against this table
  // before it is lowered to a plain map. Reset per chunk.
  std::unordered_map<std::string, std::vector<StructField>> structDefs_;

  // Names of the `class` types declared so far (PHASE 6.8), keyed by name. A
  // class is erased to a top-level constructor `fn`, so this set is the only
  // record that a global name denotes a class; the REPL reads it to seed the
  // next entry's parser. Session state, like `structDefs_`: it survives a REPL
  // entry and is cleared only by `clearSession`.
  std::unordered_set<std::string> classNames_;

  // `enum` declarations (PHASE 6.9), keyed by enum name. The declaration emits
  // no bytecode: each member is resolved to a `LOADK` of its integer at every
  // use. Session state, like `classNames_`: it survives a REPL entry and is
  // cleared only by `clearSession`.
  std::unordered_set<std::string> enumNames_;
  std::unordered_map<std::string, std::vector<EnumMember>> enumMembers_;
  // Every enum that declares a given bare member name, keyed by that name. A
  // bare `RED` resolves only when exactly one enum owns it; otherwise the
  // qualified `Color.RED` is required.
  std::unordered_map<std::string, std::vector<std::string>> enumMemberOwners_;
  // The cached module-enum scanner the parser uses, so an imported enum is
  // parsed once per session and not again when `registerImportedEnums` runs.
  Parser::EnumScanner enumScanner_;
  // Top-level binding names (`let`/`var`/`const`/`fn`/import alias) of the
  // chunk being compiled, collected before anything is emitted. A declared
  // binding shadows a bare enum member, whatever the declaration order, so the
  // resolution of `RED` is deterministic.
  std::unordered_set<std::string> declaredGlobals_;

  // Declared fields of every class, keyed by class name (PHASE 6.8). A class is
  // erased to a constructor `fn`, but its field annotations still describe the
  // instance, so the compiler keeps them to check `self.field = value` inside a
  // method and `instance.field = value` through a typed binding. Session state,
  // like `structDefs_`.
  std::unordered_map<std::string, std::vector<StructField>> classDefs_;

  // Declared signature of every class method, keyed by class name (PHASE
  // 6.8.1). A method is a map field holding a closure, so an assignment could
  // otherwise replace it with any value and the method would vanish with no
  // diagnostic. Session state, like `classDefs_`.
  std::unordered_map<std::string, std::vector<ClassMethodSig>> classMethodDefs_;

  // The class whose method body is being compiled, empty outside one. A method
  // is a closure capturing `self`, so this is what types `self` for the field
  // checks; it is saved and restored around every proto.
  std::string currentOwnerClass_;

  // Signature of every `native fn` declared so far (PHASE 6.5), keyed by name.
  // A native declaration emits no call to the compiler: it only records the
  // signature so a call can be checked (arity and argument types) and so the
  // return type can be inferred. Reset per chunk. `NativeSig` is defined at file
  // scope because `ImportedModule` carries the same signature across the module
  // boundary (PHASE 7f.2).
  std::unordered_map<std::string, NativeSig> nativeSigs_;

  // Canonical signature of every `extern "C"` symbol declared so far in the
  // chunk, keyed by the C symbol name (PHASE 6.6). Two declarations of one
  // symbol must agree; a mismatch is a compile error instead of a silent
  // last-wins (the marshalling thunk is built from the last declaration).
  std::unordered_map<std::string, std::string> externSignatures_;

  // Names declared `@MustUse` that are not natives (plain `fn`, or a global
  // holding one). Kept parallel to the binding tables and part of the session
  // state, like `nativeSigs_`: a REPL entry must still see the annotation of a
  // function declared by an earlier entry. A local that shadows the name
  // carries its own flag on the `LocalVar`, so it is not consulted here
  // (PHASE 6.7.5).
  std::unordered_set<std::string> mustUseGlobals_;

  // Non-fatal diagnostics collected while compiling (PHASE 6.7.5). The CLI
  // prints them to stderr; the program still runs. A hard error would violate
  // the §4.6 principle that inference never rejects a program that would run.
  std::vector<Warning> warnings_;

  // Records a warning at the current node position.
  void warn(const std::string& message, int line, int col);

  // Whether the name resolves to a `@MustUse` binding. Resolution mirrors a
  // variable read: a local or upvalue that shadows the name is a different
  // function and yields its own flag, so the check is skipped for it.
  bool mustUseFor(const std::string& name) const;

  // Emits the `@MustUse` warning (PHASE 6.7.5) when `expr`, used as a
  // statement, is a call to a `@MustUse` function whose result is discarded.
  // `_ = f();` is an assignment, not a bare call, so it acknowledges the
  // discard and never reaches this check.
  void checkDiscardedCall(const Expr& expr);

  // Checks `object.field = value` against the field's annotation when the
  // object's type is known (PHASE 6.8). A class instance is a map at run time,
  // so without this a mistyped field assignment would go unnoticed; the struct
  // literal already gets the same check. Only a class field is known, so an
  // ordinary map assignment is left alone, as is a compound assignment whose
  // result type is not tracked statically.
  void checkFieldAssign(const MemberExpr& member, const AssignExpr& assign);

  // Checks that a value assigned to a declared method can stand in for it
  // (PHASE 6.8.1). A method is a map field holding a closure, so replacing it
  // with a non-function makes it silently uncallable; the replacement must be a
  // function of the declared arity.
  void checkMethodReassign(const std::string& className, const ClassMethodSig& method,
                           const AssignExpr& assign);
  void checkMethodArity(const ClassMethodSig& method, const std::vector<Param>& params, int line,
                        int col, const std::string& context);

  // Declared return types of the user functions in scope, keyed by name, so a
  // call to one has a type and `let c = make(); c.field = ...` is checked
  // against the field's annotation (PHASE 6.8.1). A binding that shadows the
  // function clears the entry, exactly like `nativeSigs_`.
  std::unordered_map<std::string, TypeRef> functionReturnTypes_;

  // The inner payload type of every `async fn` in scope, keyed by name
  // (PHASE 11.1). The declared annotation on an async function is the payload
  // `T`; a call returns `Future`, and `await f()` recovers `T` from this table
  // without a generic `Future<T>` in the syntax.
  std::unordered_map<std::string, TypeRef> asyncReturnTypes_;

  // Declared parameters of the user functions in scope, keyed by name, so a
  // direct call is checked — arity and argument types — exactly like a `native
  // fn` call. Without it the annotation on a parameter was erased without ever
  // being compared to an argument, so `fn f(x: int) {}` accepted `f("s")` and
  // failed only when the body used `x`. Shares the lifecycle of
  // `functionReturnTypes_`: a shadowing binding, a native of the same name and
  // a module boundary all clear or save it.
  std::unordered_map<std::string, std::vector<Param>> functionParams_;

  // The declared field of `fields` closest to `name` within two edits, or an
  // empty string when none is close enough. Used to turn a mistyped field
  // assignment on a class instance into a warning (PHASE 6.8.1).
  static std::string closestDeclaredName(const std::string& name,
                                         const std::vector<StructField>& fields);
  static size_t editDistance(const std::string& a, const std::string& b);

  // The declared fields of a struct or class, or null when `name` is neither.
  const std::vector<StructField>* lookupAggregateFields(const std::string& name) const;

  // The member `member` of enum `enumName`, or null when the enum or the member
  // does not exist (PHASE 6.9). The declaration emits no bytecode: the caller
  // emits a `LOADK` of the returned value.
  const EnumMember* lookupEnumMember(const std::string& enumName,
                                     const std::string& member) const;
  // Every member of `enumName`, or null when the enum is not declared. Used by
  // the exhaustive-`switch` check, which needs the full set at once.
  const std::vector<EnumMember>* lookupEnumMembers(const std::string& enumName) const;
  // Resolves a member chain `Enum.MEMBER` (local or imported) to its constant.
  // `enumName` receives the visible enum type even when the member is missing,
  // so the caller can report the right diagnostic; it stays empty when the
  // chain does not name an enum at all.
  const EnumMember* resolveEnumMemberPath(const MemberExpr& member,
                                          std::string& enumName) const;
  // Resolves a bare enum member name. Returns null and leaves `reason` empty
  // when no enum declares it; returns null and sets `reason` when several do
  // (the name is ambiguous). `reason` is otherwise cleared.
  const EnumMember* lookupBareEnumMember(const std::string& name, std::string& reason) const;
  // True when `name` is a declared enum type (PHASE 6.9).
  bool isEnumTypeName(const std::string& name) const;

  // Declared return type of the function currently being compiled, checked
  // against every `return` value. Empty means "unannotated".
  TypeRef currentReturnType_;
  // True while compiling an `async fn` body (PHASE 11.1). Every `return` and
  // the implicit epilogue wrap their value in a future map, so the function's
  // call-site value is a `Future` while its annotation describes the payload.
  bool currentIsAsync_ = false;
  // True while compiling the generated step function of an async body
  // (PHASE 11.2.2). A `return` produces a `{kind: "done", value: ...}` status
  // for the scheduler instead of a future.
  bool currentAsyncBody_ = false;
  // True when this chunk needs the hidden scheduler helpers, so the end of the
  // chunk drains pending tasks (PHASE 11.2.2).
  bool asyncPrelude_ = false;
  // True once the helpers were emitted in this compilation session. They are
  // globals shared by every module, so a second chunk (an imported module, a
  // REPL entry) reuses them instead of resetting the ready queue.
  bool asyncPreludeEmitted_ = false;

  // Infers the static type of an expression when it is evident from its shape
  // (a literal, or a variable whose type is known). Returns an empty TypeRef
  // when the type cannot be determined, which means "no check".
  TypeRef inferExprType(const Expr& expr) const;

  // Rejects indexing, slicing, iteration and member access on a `Future`-typed
  // value (PHASE 11.2.1). A future is a map at run time, so without this check
  // user code could read or overwrite the `state`/`value` protocol that `await`
  // owns. A value erased to `any` still reaches the map; the runtime tag that
  // closes that hole is PHASE 11.2.5.
  void checkFutureAccess(const Expr& object, int line, int col) const;

  // Capture list of the frame currently being compiled, keyed by variable
  // name and parallel to `bc_.upvalues`.
  std::unordered_map<std::string, int> currentUpvalueNames_;

  // True while compiling a function body. It keeps block locals local even
  // when the body's own scope is the innermost one.
  bool inFunction_ = false;

  // Position of the node being compiled; stamped onto every emitted
  // instruction so the VM can report runtime errors with line/column.
  int currentLine_ = 1;
  int currentCol_ = 1;

  // Current recursion depth of the AST walk. `compileExpr`, `compileStmt` and
  // `inferExprType` recurse once per nested node, so a deeply nested program
  // (for example a very long chain of left-associative operators, which the
  // parser accepts iteratively) would otherwise overflow the stack and crash
  // instead of reporting an error. Every recursive entry point holds a
  // `DepthGuard`, which raises a `CompileError` past `kMaxCompileDepth`.
  // `mutable` so the guard also works from the const type-inference walk.
  mutable int depth_ = 0;

  // RAII guard for the recursive AST walk. Constructing it enters one level and
  // throws `CompileError` past `kMaxCompileDepth`; destroying it leaves the
  // level.
  class DepthGuard {
   public:
    explicit DepthGuard(const Compiler& compiler);
    ~DepthGuard();
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;

   private:
    const Compiler& compiler_;
  };

  int allocReg();
  void freeReg(int count = 1);

  int addConstant(const Value& value);
  int addString(const std::string& value);

  int emit(OpCode op, int a = 0, int b = 0, int c = 0);
  int emitJump(OpCode op, int a = 0);
  void patchJump(int index, int target);
  int currentOffset() const;

  // Emits `SETGLOBAL` with the write mode in `c`, so the VM can tell a fresh
  // declaration from a later assignment (and enforce `let` immutability at run
  // time). `declareGlobal` is the companion that updates the compiler's own
  // immutability table.
  int emitDeclareGlobal(int reg, const std::string& name, bool isImmutable);
  int emitAssignGlobal(int reg, const std::string& name);

  // Scopes and variables. A module body is compiled at scope depth 0 but its
  // bindings must stay local to the module frame, so it is not a global scope.
  bool inGlobalScope() const {
    return !inFunction_ && !moduleBody_ && scopes_.size() <= 1;
  }
  void pushScope();
  void popScope();
  int allocLocalSlot();
  int declareLocal(const std::string& name, bool isImmutable, const TypeRef& type = TypeRef{});
  bool resolveLocal(const std::string& name, LocalVar& out) const;
  void declareGlobal(const std::string& name, bool isImmutable);
  // Checks a `case` label that is a bare name (PHASE 6.5): it must resolve to
  // an immutable binding in the current frame (a `const`/`let` local) or to a
  // top-level `const`, so the label is a fixed value. A `var`, an unknown name
  // or a captured binding is rejected. A bare enum member (PHASE 6.9) is also
  // accepted and returned, so `compileSwitch` emits its constant directly; an
  // ambiguous bare member is rejected with a message naming the qualified form.
  // See `compileSwitch`.
  const EnumMember* checkCaseLabelName(const std::string& name, int line, int col);

  // Type annotations (PHASE 6.5). Types are compile-time metadata: they are
  // inferred from literals, checked against annotations, and then erased, so
  // the bytecode is identical with or without them.
  void declareGlobalType(const std::string& name, const TypeRef& type);
  bool lookupGlobalType(const std::string& name, TypeRef& out) const;
  // Checks that a value of type `from` may be stored in a slot of type `to`,
  // reporting a compile error at `line`/`col` when it may not. `context`
  // names the target for the message.
  void checkAssignable(const TypeRef& from, const TypeRef& to, const std::string& context,
                       int line, int col);
  // The assignment rule as a message ("" means allowed), unions included. It is
  // the single implementation: `checkAssignable` only reports its verdict.
  std::string assignError(const TypeRef& from, const TypeRef& to,
                          const std::string& context) const;

  // Checks a call to a declared `native fn` against its recorded signature
  // (PHASE 6.5): the arity must match and every argument must be assignable to
  // its parameter type. An unannotated parameter (or `any`) skips the type
  // check for that argument. The argument types arrive pre-inferred
  // (`NativeCallArgs`), so the same routine serves the direct form (checked at
  // the call site) and a qualified call to a module's native export (checked at
  // flush time, once the module is loaded).
  void checkNativeCall(const std::string& name, const NativeSig& sig, const NativeCallArgs& args,
                       int line, int col);

  // One argument against one parameter, with the FFI exception a string
  // argument enjoys against a `cstring` parameter. Shared by both call checks.
  void checkArgumentAssignable(const TypeRef& argument, const TypeRef& param,
                               const std::string& context, int line, int col);

  // Checks a direct call to a user function against its recorded parameter
  // list. Kept separate from `checkNativeCall` because the two signatures have
  // a different shape (`Param` with `variadic` vs `NativeSig` with `cVariadic`),
  // but they word every diagnostic the same way.
  void checkUserCall(const std::string& name, const std::vector<Param>& params,
                     const NativeCallArgs& args, int line, int col);

  // Infers the type of every argument of `call` and records its position. The
  // types must be captured while the call site's scopes are open, because a
  // qualified call is checked later, when the scopes are gone.
  NativeCallArgs captureCallArgs(const CallExpr& call) const;

  // Returns the signature of the `native fn` that `name` refers to, or null
  // when it is not a native binding. The name is resolved like a variable
  // read: a local or an upvalue that shadows a native name is a different
  // function and yields null. A name that resolves to nothing is a global, so
  // it matches when it is a declared native. Types are metadata only.
  const NativeSig* nativeSigFor(const std::string& name) const;

  // Records that the current proto uses the host binding `name` (PHASE 7f.2).
  // Called wherever a `native fn`/`extern "C"` binding is read: a direct read
  // (a call or a first-class reference) and a capture by an enclosing function.
  // The AOT backend reads the resulting list to reject a use it cannot resolve
  // from a stub, instead of emitting a binary that calls nil. Idempotent per
  // proto, so the same name is listed once.
  void recordNativeUse(const std::string& name);

  // Records a qualified access to a module's native export (`std.math.pow`).
  // The member read goes through the module table with `GETTABLE`, so it leaves
  // no `GETGLOBAL` behind; recording the export name here is what lets the AOT
  // backend reject the use when the module declared it `native fn`. The access
  // is queued and resolved by `flushModuleNativeUses` once the whole chunk is
  // compiled, so a hoisted function that reads the export before the `import`
  // statement is compiled is still caught. `callArgs` is non-null when the
  // access is the callee of a call: the argument types were captured at the call
  // site, and the resolved signature is checked against them (arity and argument
  // types), exactly like the direct form.
  void recordModuleNativeUse(const MemberExpr& member, const NativeCallArgs* callArgs, int line,
                             int col);

  // Pre-scans `block`'s import statements and records, for every qualified or
  // aliased import, the visible binding path that names the module (for
  // example "std.math" or an alias "m") mapped to the module's path in
  // `moduleByBindingPath_`. It runs before the block's functions are hoisted,
  // so a hoisted function that reads a module's native export
  // (`std.math.pow(...)`) resolves the binding path even though the `import`
  // statement is compiled later. It does not load the module: loading here
  // would compile it out of source order and disturb the order in which module
  // bodies are materialized.
  void registerImportBindings(const BlockStmt& block);

  // Splits a member chain (`root` plus `members`, as `flattenMemberPath`
  // produces) into the imported module it names and the export member, when the
  // chain is rooted at a module binding path: `std.math.pow` (root "std",
  // members {"math","pow"}) yields ("std.math", "pow") and an alias (`m.pow`,
  // root "m") yields ("std.math", "pow") too. Returns false when the chain does
  // not name a module export.
  bool moduleExportOf(const std::string& root, const std::vector<std::string>& members,
                      std::string& modulePath, std::string& member) const;

  // Resolves every recorded module-export access (`pendingModuleUses_`) against
  // the loaded modules and records the ones that name a native export as a use
  // on the current proto. Called once the whole chunk is compiled, when every
  // import has been loaded.
  void flushModuleNativeUses();

  // Upvalue resolution (PHASE 6b). Returns the index of `name` in the current
  // proto's capture list, adding the entry (and, transitively, the re-export
  // entries in every function in between) when it is not there yet. Returns -1
  // when the name is not a local or upvalue of any enclosing function, i.e. it
  // resolves to a global instead.
  int resolveUpvalue(const std::string& name);

  int compileExpr(const Expr& expr);
  int compileVariable(const std::string& name);
  int compileLambda(const LambdaExpr& lambda, int line, int col);
  int compileAssign(const AssignExpr& assign, int line, int col);
  // `&&=`, `||=` and `??=`: short-circuiting compound assignment. `targetReg`
  // already holds the target's current value (read once by the caller) and
  // `store` emits the write-back of `targetReg` for that target's shape.
  int compileLogicalAssign(const AssignExpr& assign, int targetReg,
                           const std::function<void()>& store);
  int compileTernary(const TernaryExpr& expr);
  int compileTry(const TryExpr& node, int line, int col);
  int compileCatch(const CatchExpr& node, int line, int col);
  int compileCall(const CallExpr& call, int line, int col);
  int compileArray(const ArrayExpr& array);
  int compileMap(const MapExpr& map);
  int compileStructLiteral(const StructLitExpr& literal, int line, int col);
  int compileRange(const RangeExpr& range, int line, int col);
  int compileSlice(const SliceExpr& slice, int line, int col);
  int compileInterpolation(const InterpExpr& interpolation, int line, int col);
  // `await expr` (PHASE 11.1): reads the future's state and yields its payload,
  // or raises the carried error. It is a few existing opcodes, not an opcode of
  // its own, so the VM, AOT and WASM share one implementation.
  int compileAwait(const AwaitExpr& awaited, int line, int col);
  // PHASE 11.2.3: splits an async body at every `await`/`yield` into a chain of
  // continuation closures. The result ends with a scheduler status map; the
  // payload of every `return` is checked against `returnType` before the return
  // is rewritten to `__oskel_async_done`. A construct the splitter cannot lower
  // yet (a loop, `switch`, `defer` or `try` across a suspension) is a compile
  // error naming the next slice.
  StmtPtr lowerAsyncBody(const StmtPtr& body, const TypeRef& returnType, int line, int col);
  // Builds the future map an `async fn` returns: `{ state: "fulfilled", value:
  // value, error: nil }`. The map is the portable representation; no runtime
  // tag or builtin is added.
  int compileMakeStatus(const std::string& kind, int value, int line, int col);
  // Converts `source` to a string through the compiler's private runtime
  // helper and returns the register holding the result.
  int compileToString(int source, int line, int col);
  // Emits a call to a hidden runtime helper (name not writable by user code).
  int compileHelperCall(const std::string& helper, const std::vector<int>& args, int line,
                        int col);

  void compileStmt(const Stmt& stmt);
  void compileBlockBody(const BlockStmt& block);
  void compileScopedBlock(const Stmt& stmt);
  void compileLet(const LetStmt& stmt);
  void compileFnDecl(const FnDeclStmt& fn, int line, int col);
  // Registers every `struct` declared directly in `block` before anything else
  // in it is compiled, so a hoisted function that builds a struct literal sees
  // the shape even when the declaration appears later in the file.
  void registerStructDecls(const BlockStmt& block);
  void compileStructDecl(const StructDeclStmt& decl, int line, int col);
  // Registers every `enum` declared directly in `block` before anything else in
  // it is compiled, so a hoisted function that uses `Color.RED` (or a bare
  // member) resolves it whatever the declaration order. Metadata only: it
  // emits nothing.
  void registerEnumDecls(const BlockStmt& block);
  // Registers the enums the block's imports make visible (PHASE 6.9 across
  // modules). A selective or wildcard import binds each enum by its own name;
  // a qualified import binds it as `<path>.Enum` (and, for a dotted path, by
  // the short last segment too). The module is parsed through the cached enum
  // scanner, not loaded, so a hoisted function can use `geom.Color.RED` before
  // the `import` statement is reached; the module body is still materialized
  // at its import statement.
  void registerImportedEnums(const BlockStmt& block);
  // Collects the top-level binding names of `block` before anything is emitted,
  // so a bare enum member never shadows a global declared later in the file.
  void registerDeclaredGlobals(const BlockStmt& block);
  // Registers the fields of every `class` declared directly in `block`, so a
  // hoisted method body (compiled before the constructor's own declaration is
  // reached) already knows the shape it assigns into. Like `registerStructDecls`
  // this is metadata only and emits nothing.
  void registerClassDecls(const BlockStmt& block);
  // Registers every `native fn` declared directly in `block` before anything
  // else in it is compiled, so a hoisted function that calls one is checked
  // against its signature. It also descends into `extern "C"` blocks: their
  // declarations are natives too.
  void registerNativeDecls(const BlockStmt& block);
  // Registers the name of every top-level immutable `let`/`const` binding of
  // `block` before it is compiled, so a `case` label that names a top-level
  // `const` is recognized as a fixed value even when a hoisted function that
  // uses it is compiled first. See `checkCaseLabelName`.
  void registerGlobalConstDecls(const BlockStmt& block);
  // Registers the `@MustUse` flag of non-native functions declared directly in
  // `block` before it is compiled, so a call from an earlier (hoisted) function
  // is checked whatever the declaration order (PHASE 6.7.5).
  void registerMustUseDecls(const BlockStmt& block);
  // Compiles an `extern "C" { ... }` block (PHASE 6.6): each declaration
  // follows the `native fn` path, so the block emits no bytecode of its own.
  void compileExternBlock(const ExternBlockStmt& block, int line, int col);
  // Resolves an `extern "C"` type name to a scalar or struct spec (PHASE 6.6).
  ffi::TypeSpec resolveTypeSpec(const TypeRef& type);
  // Rejects a union where a type must cross the FFI boundary (PHASE 6.7.4).
  void rejectUnionForFfi(const TypeRef& type, const std::string& position, int line, int col);
  void compileReturn(const ReturnStmt& stmt);
  void compileIf(const IfStmt& stmt);
  void compileWhile(const WhileStmt& stmt);
  void compileDoWhile(const DoWhileStmt& stmt);
  void compileFor(const ForStmt& stmt);
  void compileForIn(const ForInStmt& stmt);
  void compileSwitch(const SwitchStmt& stmt);
  void compileBreak(const Stmt& stmt);
  void compileContinue(const Stmt& stmt);
  void compileDefer(const DeferStmt& stmt);
  // Emits the deferred blocks of every scope from the innermost down to (and
  // including) `minDepth`, in LIFO order, so the code at that exit point runs
  // them. Called at normal scope exit, before `return`, and before the jump
  // emitted by `break`/`continue`. `minDepth` is a 1-based scope count.
  void emitDeferredBlocks(size_t minDepth);

  void resetChunk(const std::string& name = "<main>");

  // A name an import introduces into the current scope, and where it came
  // from. One map holds every form (qualified namespace, alias, wildcard or
  // selective name) so a collision between any two forms is caught, not only
  // the wildcard-vs-wildcard case (docs/language.md §18.3). Two namespace roots merge
  // (sibling modules under the same root); any other pair that binds the same
  // name is a conflict unless it is the very same binding imported twice.
  struct ImportBinding {
    enum class Kind { Namespace, Module, Name };
    Kind kind;
    std::string module;
    // The convenience binding of a qualified import's last segment. It is
    // weaker than an explicit import: an explicit binding of the same name
    // replaces it, and a declaration already owns the name.
    bool implicit = false;
  };

  // Everything that outlives one chunk: the definitions a REPL session
  // carries from one entry to the next. `snapshotSession`/`restoreSession`
  // make a compile transactional (a definition is committed only when its
  // entry compiles) and let `compileLoaded` merge a loaded file's state into
  // the session.
  struct SessionSnapshot {
    std::unordered_set<std::string> globalConsts;
    std::unordered_set<std::string> userGlobals;
    std::unordered_map<std::string, TypeRef> globalTypes;
    std::unordered_map<std::string, std::vector<StructField>> structDefs;
    std::unordered_set<std::string> classNames;
    std::unordered_map<std::string, std::vector<StructField>> classDefs;
    std::unordered_map<std::string, std::vector<ClassMethodSig>> classMethodDefs;
    std::unordered_set<std::string> enumNames;
    std::unordered_map<std::string, std::vector<EnumMember>> enumMembers;
    std::unordered_map<std::string, std::vector<std::string>> enumMemberOwners;
    std::unordered_map<std::string, TypeRef> functionReturnTypes;
    std::unordered_map<std::string, TypeRef> asyncReturnTypes;
    std::unordered_map<std::string, std::vector<Param>> functionParams;
    std::unordered_map<std::string, NativeSig> nativeSigs;
    std::unordered_map<std::string, std::string> externSignatures;
    std::unordered_set<std::string> mustUseGlobals;
    std::map<std::string, ImportedModule> loadedModules;
    std::set<std::string> materializedModules;
    std::set<std::string> namespaces;
    std::unordered_map<std::string, ImportBinding> importedNames;
    std::unordered_map<std::string, std::string> moduleByBindingPath;
  };

  SessionSnapshot snapshotSession() const;
  void restoreSession(SessionSnapshot snapshot);

  // The shared body of `compile` and `compileLoaded`: compiles a program in the
  // global scope with whatever session state the caller has set up. It does not
  // touch the session or the import loader, so `compile` (clean session) and
  // `compileLoaded` (current session, file loader) both delegate here.
  Bytecode compileTopLevelBody(const StmtPtr& program);

  // The body of `compileRepl`, split out so the public wrapper can restore the
  // session state when the entry throws (a definition is committed only when
  // its entry compiles).
  Bytecode compileReplEntry(const StmtPtr& program);

  // Compiles `body` as a child proto of the chunk being built and returns its
  // index in `bc_.children`. The parent compiler state is saved and restored.
  // `returnType` is the declared return annotation, checked against every
  // `return` (PHASE 6.5); an empty TypeRef means "unannotated".
  // `ownerClass` names the class a method body belongs to, so `self` is typed
  // inside it; empty for an ordinary function.
  int compileFunctionProto(const std::string& name, const std::vector<Param>& params,
                           const StmtPtr& body, int line, int col, const TypeRef& returnType,
                           const std::string& ownerClass = std::string(), bool isAsync = false,
                           bool asyncBody = false);

  // Compiles an `async fn` declaration into a factory closure (PHASE 11.2.2).
  // The factory returns a pending future carrying the generated step function;
  // the body itself is a separate proto that reads its parameters from the
  // task frame. Returns a register holding the factory closure.
  int compileAsyncFunctionValue(const std::string& name, const std::vector<Param>& params,
                                const StmtPtr& body, int line, int col, const TypeRef& returnType,
                                const std::string& ownerClass);

  // Emits the hidden scheduler helpers once per chunk that contains async code
  // (PHASE 11.2.2). They are ordinary Oskel functions over maps and arrays, so
  // the VM, AOT and WASM share one implementation.
  void compileAsyncPrelude();

  // Compiles a module's top-level statements into `bc_` (bindings stay
  // frame-local) and returns its public surface as a table. Called by
  // `importModule` while `moduleBody_` is set.
  void compileModuleBody(const StmtPtr& program);

  // Resolves an import path once per compiler and caches the result, so two
  // imports of the same module share one proto and one symbols list.
  const ImportedModule& loadModule(const std::string& path, int line, int col);

  // Compiles an `import` declaration: loads the module, then binds its names
  // according to the import mode, detecting wildcard conflicts.
  void compileImport(const ImportStmt& import, int line, int col);

  // Emits (once) the code that runs a module body and stores its exported
  // table in the hidden global `$module$<path>`. Later imports of the same
  // module reuse that global instead of running the body again.
  void materializeModule(const ImportedModule& module, int line, int col);

  // Compiles `import <path>;` / `import <path> as <alias>;`. The module table
  // becomes a real runtime value reachable through the dotted path (`std.math`
  // or the alias `m`), so qualified access uses the ordinary member chain.
  // `implicitShortAlias` marks the extra binding of the last segment
  // (`math` in `import std.math;`): it is skipped when a declaration owns the
  // name, instead of reporting the collision an explicit binding would.
  void compileQualifiedImport(const ImportedModule& module, const std::string& boundName,
                              int line, int col, bool implicitShortAlias = false);

  // Compiles a qualified import of a class module: the module table is read
  // once, but the binding is the single export (`module.className`) instead of
  // the table. A dotted `boundName` binds that value at the namespace leaf; a
  // bare name (alias or the convenience short name) binds it as a global.
  void compileClassImport(const ImportedModule& module, const std::string& boundName,
                          int line, int col, bool implicitShortAlias = false);

  // Compiles a wildcard or selective import, binding each imported name to a
  // global. Reports an ambiguity when two `.*` imports export the same name.
  void compileNameImport(const ImportedModule& module, const std::vector<std::string>& names,
                         int line, int col);

  // Records that the visible name `visible` is now bound by an import of the
  // given kind, coming from `module`. This is the single place conflict
  // detection lives, so every pair of forms is checked, not only two
  // wildcards. Returns false when the binding must be skipped: a name the
  // importing module declares itself always wins, and re-importing the very
  // same binding is a no-op. An `implicitShortAlias` (the last segment of a
  // qualified import) is also skipped when a struct owns the name; an explicit
  // binding reports that collision instead. Reports a compile error on a
  // genuine collision (docs/language.md §18.3).
  bool recordImportBinding(const std::string& visible, ImportBinding::Kind kind,
                           const std::string& module, int line, int col,
                           bool implicitShortAlias = false);

  // Creates the namespace prefix `prefix` (e.g. "std" or "std.math") as a
  // global map if it was not created by an earlier import, and returns the
  // register holding the deepest existing map on the path.
  int ensureNamespace(const std::string& prefix, int line, int col);

  // The global name a visible import binding maps to while a module body is
  // being compiled. At the top level this is the visible name itself; inside a
  // module it is a hidden per-module name (`$imp$<module>$<name>`), so a
  // module's own imports never reach the importing program's globals (DEVELOPMENT.md
  // §18.2). Every global read/write and every import binding goes through this
  // so the two agree.
  std::string importBindingName(const std::string& visible) const;

  // Walks a module body's top-level imports and records the hidden global each
  // introduced name maps to (see `importBindingName`). Runs before the body is
  // compiled, so a hoisted function that references an imported name already
  // resolves it to the module-local binding.
  void predeclareModuleImports(const BlockStmt& block);

  ImportLoader importLoader_;
  Parser::InterfaceScanner interfaceScanner_;
  // The ABI `extern "C"` declarations are validated against; see
  // `setAbiCapabilities`. Defaults to this build's, which is what the
  // interpreter marshals.
  ffi::AbiCapabilities abiCapabilities_{};
  std::map<std::string, ImportedModule> loadedModules_;
  // Module paths currently being loaded, outermost first. A path that is
  // already on the stack when it is requested again closes a cycle
  // (`a -> b -> a`), which is reported as a compile error instead of
  // recursing until the compiler overflows the stack.
  std::vector<std::string> loadingStack_;
  // Set while compiling a module body: its top-level declarations become
  // frame locals and are exported instead of becoming globals.
  bool moduleBody_ = false;
  // Module paths whose exported table has already been stored in its hidden
  // global, so the module body runs at most once per program.
  std::set<std::string> materializedModules_;
  // Namespace prefixes already created as runtime maps ("std", "std.math"),
  // so repeated imports reuse the same table instead of replacing it.
  std::set<std::string> namespaces_;
  // Every name an import has bound in the current scope, whatever its form,
  // mapped to where it came from. This is the single place conflict detection
  // consults, so a collision between any two import forms (namespace vs alias,
  // alias vs wildcard, ...) is caught, not only wildcard-vs-wildcard
  // (docs/language.md §18.3).
  std::unordered_map<std::string, ImportBinding> importedNames_;

  // Visible binding path of every qualified/aliased import in the current
  // scope ("std.math" for `import std.math;`, "m" for `import std.math as m;`),
  // mapped to the module's path. `recordModuleNativeUse` walks a member chain
  // against it to tell a qualified access to a module's native export
  // (`std.math.pow`) from an ordinary member access, so the AOT backend can
  // reject the use when the export has no stub (PHASE 7f.2).
  std::unordered_map<std::string, std::string> moduleByBindingPath_;

  // Qualified accesses to a module export recorded while a chunk is compiled
  // (`std.math.pow` -> root "std", members {"math","pow"}), resolved against the
  // loaded modules by `flushModuleNativeUses` once the whole chunk is compiled.
  // Resolution is deferred because a hoisted function is compiled before the
  // `import` statement that loads the module, so the module's native exports are
  // not known yet when the access is seen. `callArgs` is set when the access is
  // the callee of a call: the argument types are captured at the call site
  // (while the scopes are open) so `flushModuleNativeUses` can check the call
  // against the resolved signature.
  struct PendingModuleUse {
    std::string root;
    std::vector<std::string> members;
    bool isCall = false;         // true when this is a call callee
    NativeCallArgs callArgs;     // valid only when `isCall`
    int line = 0;
    int col = 0;
  };
  std::vector<PendingModuleUse> pendingModuleUses_;

  // The `MemberExpr` node currently being compiled as the callee of a call, and
  // that call's argument types, captured while the scopes are open. Set by
  // `compileCall` around `compileExpr(*call.callee)` so the `MemberExpr` case
  // knows the member read is a call site: the recorded module-export use then
  // carries the captured arguments, and `flushModuleNativeUses` checks it
  // against the propagated signature (PHASE 7f.2). Null for an ordinary member
  // read.
  const MemberExpr* callCalleeMember_ = nullptr;
  NativeCallArgs callCalleeArgs_;

  // Path of the module body currently being compiled. Empty at the top level.
  // Used to build the hidden global names a module's own imports bind to.
  std::string modulePath_;
  // Visible names that the module being compiled introduced through `import`.
  // A read/write of one of these names inside the module goes to its hidden
  // per-module global instead of the program's global table, so a module's
  // imports never leak into the importing program (docs/language.md §18.2).
  std::unordered_set<std::string> moduleImportNames_;
};

// Shortcut equivalent to `Compiler{}.compile(program)`.
Bytecode compileProgram(const StmtPtr& program);
