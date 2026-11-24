#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "frontend/ast.h"
#include "frontend/lexer.h"

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens, std::string where = "<source>");

  // Parses a whole program: a sequence of statements until end of input.
  StmtPtr parseProgram();

  // Fold `const` declarations after the program has been parsed: evaluate
  // each const initializer at compile time and replace uses of the constant
  // in the top-level statements with literal nodes (see foldConstant).
  void resolveTopLevelConsts(StmtPtr& program);

  // RAII helper: sets the parser's "program statement level" flag, so top-level
  // `const` initializers skip eager folding (they are folded by
  // resolveTopLevelConsts after the whole program is parsed). Nested blocks
  // clear the flag again, keeping their eager per-statement folding.
  class TopLevelScope {
   public:
    explicit TopLevelScope(bool& flag, bool value = true);
    ~TopLevelScope();
    TopLevelScope(const TopLevelScope&) = delete;
    TopLevelScope& operator=(const TopLevelScope&) = delete;

   private:
    bool& flag_;
    bool previous_;
  };

  // Parses a single expression (optionally followed by ';') and requires the
  // end of input. Used by the REPL and by string interpolation.
  ExprPtr parseFullExpression();

  // Makes the `struct` names declared by earlier REPL entries visible to this
  // parser, so a literal (`Point { ... }`) or an annotation written in a later
  // entry is still recognized. A file parse never needs this: every struct is
  // declared in the same source. Only `structNames_` is seeded, so re-declaring
  // a struct in a later entry stays allowed, exactly like redefining a `let`
  // or a `fn`.
  void seedStructNames(const std::unordered_set<std::string>& names) {
    structNames_.insert(names.begin(), names.end());
  }

  // Makes the `class` names declared by earlier REPL entries visible to this
  // parser, so a class name used as a type annotation in a later entry is still
  // recognized (a class desugars to a top-level `fn`, so the parser has no
  // other way to know the name denotes a type). A file parse never needs this.
  void seedClassNames(const std::unordered_set<std::string>& names) {
    classNames_.insert(names.begin(), names.end());
  }

  // Makes the `enum` names declared by earlier REPL entries visible to this
  // parser, so `Color.RED` and a bare member keep resolving after the entry
  // that declared the enum. A file parse never needs this.
  void seedEnumNames(const std::unordered_set<std::string>& names) {
    enumNames_.insert(names.begin(), names.end());
  }

  // Restores the folded member values and the bare-member index from an
  // earlier REPL entry, so `const X = Color.RED;` and `const X = RED;` keep
  // folding after the entry that declared the enum. `names` above only
  // restores the enum namespace; this restores the constants behind it.
  void seedEnumValues(
      const std::unordered_map<std::string, Value>& values,
      const std::unordered_map<std::string, std::vector<std::string>>& owners) {
    for (const auto& entry : values) enumValues_[entry.first] = entry.second;
    for (const auto& entry : owners) {
      auto& dest = enumMemberOwners_[entry.first];
      for (const std::string& owner : entry.second) {
        if (std::find(dest.begin(), dest.end(), owner) == dest.end()) {
          dest.push_back(owner);
        }
      }
    }
  }

  // Supplies the module's enums to the parser without making the parser depend
  // on the filesystem (PHASE 6.9 across modules). The CLI wires it to a
  // resolver-backed scanner, so `import { Color } from geom;` makes `Color` a
  // type annotation and `geom.Color.RED` a fixed switch label before the
  // compiler loads the module. The returned map is keyed by the module's own
  // enum names; an empty map means the module declares none (or cannot be
  // read here, in which case the compiler reports the real error).
  using EnumScanner =
      std::function<std::unordered_map<std::string, std::vector<EnumMember>>(
          const std::string& modulePath)>;
  void setEnumScanner(EnumScanner scanner) { enumScanner_ = std::move(scanner); }

  const std::string& where() const { return where_; }

  // Non-fatal diagnostics raised while folding `const` initializers. An integer
  // overflow in a constant expression wraps around (docs/language.md §2.3), so
  // the program still compiles and runs; the warning just surfaces a value that
  // is almost always a mistake. The CLI prints these to stderr.
  const std::vector<Warning>& warnings() const { return warnings_; }

 private:
  std::vector<Token> tokens_;
  size_t pos_ = 0;
  std::string where_;
  // Records a fold-time warning, ignoring an exact duplicate so a const that
  // `resolveTopLevelConsts` retries does not report the same site twice.
  void warn(const std::string& message, int line, int col) const {
    for (const Warning& existing : warnings_) {
      if (existing.line == line && existing.col == col && existing.message == message) return;
    }
    warnings_.push_back(Warning{message, line, col});
  }

  // Rejects a binding whose name is an enum name: `let Color = ...`,
  // `fn Color`, a parameter or a loop variable would shadow the `Color.RED`
  // namespace, so the qualified form would silently change meaning.
  void rejectEnumNameAsBinding(const Token& name) const;
  // Fold-time warnings (see `warn`). `mutable` because `foldConstant` is const.
  mutable std::vector<Warning> warnings_;
  // Folded values of `const` declarations seen so far, used by the parser to
  // evaluate constant expressions at parse time (see foldConstant).
  std::unordered_map<std::string, Value> constValues_;
  // Names of the `struct` types declared so far (PHASE 6.5). A struct name is
  // accepted as a type annotation and lets `Name { ... }` be recognized as a
  // struct literal instead of a variable followed by a block (which would
  // otherwise be ambiguous in `for x in name { ... }`).
  std::unordered_set<std::string> structNames_;
  // Names of the `struct` types actually declared during this parse. Kept
  // apart from `structNames_` (which also holds pre-scanned names) so a
  // redeclaration in the same file is caught while a name merely made visible
  // by the pre-scan is not mistaken for one.
  std::unordered_set<std::string> declaredStructs_;
  // Names of the `class` types declared during this parse, for the same reason
  // as `declaredStructs_`. A class name is a valid type annotation, so it is
  // kept in `classNames_` (not `structNames_`); a class and a struct may still
  // not share a name, and the two sets are checked against each other in both
  // declaration orders.
  std::unordered_set<std::string> declaredClasses_;
  // An `interface`/`trait` declaration (PHASE 12.1). Methods are name plus
  // arity; a class that says `implements` must provide each of them.
  struct InterfaceMethodInfo {
    std::string name;
    std::size_t arity = 0;
    bool variadic = false;
    int line = 1;
    int col = 1;
  };
  struct InterfaceInfo {
    std::string name;
    std::unordered_map<std::string, InterfaceMethodInfo> methods;
    int line = 1;
    int col = 1;
  };
  std::unordered_map<std::string, InterfaceInfo> interfaces_;
  // A class and the interfaces it claims, validated after the whole module is
  // parsed so an interface may be declared after the class that implements it.
  struct PendingImplementation {
    std::string className;
    std::vector<std::string> interfaceNames;
    std::unordered_map<std::string, InterfaceMethodInfo> methods;
    int line = 1;
    int col = 1;
  };
  std::vector<PendingImplementation> pendingImplementations_;
  void validateInterfaces() const;
  // Names of the `class` types visible as type annotations. Pre-scanned in
  // `parseProgram` (like `structNames_`) so an annotation written before the
  // declaration is accepted, and seeded by the REPL across entries.
  std::unordered_set<std::string> classNames_;
  // Names of the `enum` types declared in this source (PHASE 6.9). Pre-scanned
  // in `parseProgram` so `Color.RED` is recognized before the declaration and
  // `Color` is accepted as a type annotation. A binding may not reuse one of
  // these names: the qualified form would become ambiguous.
  std::unordered_set<std::string> enumNames_;
  // The enums actually declared during this parse, so a redeclaration in the
  // same file is caught while a name merely made visible by the pre-scan is
  // not mistaken for one.
  std::unordered_set<std::string> declaredEnums_;
  // Folded value of every enum member, keyed by its qualified name
  // (`Color.RED`). Used by `foldConstant` so `const X = Color.RED;` folds.
  std::unordered_map<std::string, Value> enumValues_;
  // Every enum that declares a given member name, keyed by the bare name. A
  // bare `RED` folds only when exactly one enum owns it.
  std::unordered_map<std::string, std::vector<std::string>> enumMemberOwners_;
  // Enum names made visible by an import, as opposed to a local declaration.
  // A selective import of one of these names is the import itself, so it is
  // not rejected as "an enum name used as a binding".
  std::unordered_set<std::string> importedEnumNames_;
  // Module enums made visible by an import, resolved before the main parse so
  // a type annotation or a switch label written before the import statement
  // is still accepted (PHASE 6.9 across modules). Null when no scanner is set.
  EnumScanner enumScanner_;
  // Seeds the enums of every top-level import in the token stream. Runs once
  // at the start of `parseProgram`, before any statement is parsed.
  void prescanImportedEnums();
  // Seeds the enums an import makes visible. `selected` is the selective list
  // (empty for a qualified/wildcard import); `alias` is the qualified binding
  // path (`geom`, `g`, `pkg.geom`), empty for selective/wildcard. A bare enum
  // is visible by its own name; a qualified one by `alias.Enum`. `wildcard`
  // makes every exported enum visible by its own name.
  void seedImportedEnums(const std::string& module,
                         const std::vector<std::string>& selected,
                         const std::string& alias, bool wildcard);
  // Registers one visible enum and its members in the parser's folding state.
  // `bare` also makes the members resolvable without the enum prefix.
  void seedEnum(const std::string& visibleName, const std::vector<EnumMember>& members,
                bool bare);
  // True while parseProgram is running at statement level: top-level consts
  // are folded later in resolveTopLevelConsts (after the whole program was
  // parsed), so nested blocks keep folding eagerly during parsing.
  bool atTopLevel_ = false;
  // Number of enclosing `{ ... }` blocks being parsed. `import` is only valid
  // at depth 0 (a program's top level), so an import nested in a block or a
  // function body is rejected at parse time.
  int nestingDepth_ = 0;
  // Number of enclosing `async fn` bodies being parsed. `await` is only valid
  // while it is positive; a lambda resets it to zero, so a lambda written inside
  // an async function cannot await (a user lambda has no async form).
  int asyncDepth_ = 0;
  // Current recursion depth of the expression/statement/type walk. The
  // recursive-descent parser consumes one C++ frame per nested construct, so a
  // deeply nested source (e.g. thousands of `(` or `[`) would otherwise
  // overflow the stack and crash instead of reporting an error. Every
  // recursive entry point raises a `CompileError` once this exceeds
  // `kMaxParseDepth` (see DepthGuard). `mutable` so the guard also works from
  // the `const` constant-folding walk.
  mutable int depth_ = 0;

  // RAII guard for the recursive walk. Constructing it enters one nesting level
  // and throws `CompileError` past `kMaxParseDepth`; destroying it leaves the
  // level. Every recursive entry point (expression, statement and type parsing)
  // holds one for the duration of its call, so the depth tracks the real C++
  // stack usage and no input can overflow it.
  class DepthGuard {
   public:
    explicit DepthGuard(const Parser& parser);
    ~DepthGuard();
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;

   private:
    const Parser& parser_;
  };

  // Sets `asyncDepth_` for the duration of a function body and restores the
  // previous value on every exit path, including a `CompileError`. An async
  // function body enters with the current depth plus one; a lambda enters with
  // zero, because `await` cannot cross a lambda boundary.
  class AsyncScope {
   public:
    AsyncScope(int& depth, int value) : depth_(depth), saved_(depth) { depth_ = value; }
    ~AsyncScope() { depth_ = saved_; }
    AsyncScope(const AsyncScope&) = delete;
    AsyncScope& operator=(const AsyncScope&) = delete;

   private:
    int& depth_;
    int saved_;
  };

  bool foldConstant(const Expr& expr, Value& out, std::string& reason) const;

  const Token& peek(size_t ahead = 0) const;
  const Token& previous() const;
  bool check(Tok type) const;
  bool match(Tok type);
  const Token& advance();
  const Token& consume(Tok type, const std::string& message);
  [[noreturn]] void error(const Token& token, const std::string& message) const;
  static std::string describe(const Token& token);

  StmtPtr parseStatement();
  StmtPtr parseVarDecl();
  // `@MustUse fn f() ...` (PHASE 6.7.5): `mustUse` is true when an attribute
  // was parsed before the declaration. Attributes are metadata, so they are
  // stored on the node and never reach the bytecode.
  StmtPtr parseFnDecl(bool mustUse = false, bool isAsync = false);
  // `native fn name(params): ret;` (PHASE 6.5): a bodyless declaration that
  // binds a name to a host-registered native function.
  StmtPtr parseNativeDecl(bool mustUse = false);
  // One or more `@name` attributes before a declaration (PHASE 6.7.5). Only
  // `@MustUse` is known; any other attribute is a compile error. The caller has
  // already consumed the `@`.
  bool parseAttributes();
  // `@MustUse fn f() ...` / `@MustUse native fn f();` (PHASE 6.7.5): parses the
  // attribute list and then the declaration it annotates.
  StmtPtr parseAttributedDecl();
  // `extern "C" { fn ...; }` or `extern fn ...;` (PHASE 6.6): C declarations.
  StmtPtr parseExternDecl();
  // One bodyless `fn name(params): ret;` in an `extern` declaration. The caller
  // has already consumed the `fn`.
  FnDeclStmt parseExternFnDecl();
  // `struct Name { field: T; ... }` (PHASE 6.5). A data-only declaration.
  StmtPtr parseStructDecl();
  // `enum Name { A, B = 5, C }` (PHASE 6.9). Compile-time metadata: each
  // member is an integer constant, resolved to a `LOADK` at its use.
  StmtPtr parseEnumDecl();
  // `class Name { field: T; ...; fn method(...) { ... } }`. A class is sugar
  // over the map-plus-closures idiom: the parser desugars it into an ordinary
  // top-level constructor function, so no AST node, opcode, compiler or VM
  // change is involved (see the definition).
  StmtPtr parseClassDecl();
  // `interface Name { fn method(params): ret; ... }` (PHASE 12.1). A bodyless
  // contract; `trait` is accepted as an alias. Emits no bytecode.
  StmtPtr parseInterfaceDecl();
  // Body that follows a `=>`: a `{ ... }` block, or a single expression that
  // is desugared into a block holding one `return`. `arrow` is the `=>` token.
  StmtPtr parseFunctionBody(const Token& arrow);
  StmtPtr parseIf();
  StmtPtr parseWhile();
  StmtPtr parseDoWhile();
  StmtPtr parseFor();
  // Body of a control-flow statement: a `{ ... }` block, or a single statement
  // wrapped in a block, so the AST/compiler/bytecode match the brace form.
  StmtPtr parseBody();
  StmtPtr parseReturn();
  StmtPtr parseDefer();
  StmtPtr parseImport();
  StmtPtr parseSwitch();
  std::string parseImportPath();
  StmtPtr parseBlock();
  StmtPtr parseExpressionStatement();

  ExprPtr parseExpression();
  ExprPtr parseAssignment();
  ExprPtr parseTernary();
  ExprPtr parseCatch();
  ExprPtr parseNullCoalesce();
  ExprPtr parseOr();
  ExprPtr parseAnd();
  ExprPtr parseBitOr();
  ExprPtr parseBitXor();
  ExprPtr parseBitAnd();
  ExprPtr parseEquality();
  ExprPtr parseComparison();
  ExprPtr parseShift();
  ExprPtr parseRange();
  ExprPtr parseAdditive();
  ExprPtr parseMultiplicative();
  ExprPtr parsePower();
  ExprPtr parseUnary();
  ExprPtr parsePostfix();
  ExprPtr parsePrimary();
  // Parses the `{ field: value, ... }` body of a struct literal whose type
  // name has already been consumed. The parser only builds the node; checking
  // the field names and types against the declaration is the compiler's job.
  ExprPtr parseStructLiteralBody(const std::string& typeName, int line, int col);
  // True when the upcoming tokens start a lambda: `x =>` or `(a, b) =>`.
  bool atLambda() const;
  ExprPtr parseLambda();

  ExprPtr buildString(const Token& token);
  std::vector<Param> parseParams(bool allowBareEllipsis = false, bool* outCVariadic = nullptr);
  // Parses an optional `: <type>` annotation after a name (variable, parameter
  // or return type). Returns an empty TypeRef when no `:` is present. Rejects
  // unknown type names at parse time.
  TypeRef parseTypeAnnotation(bool allowVoid = false);
  // One alternative of a union type (or a whole type when there is no `|`).
  TypeRef parseTypeAlternative(bool allowVoid);
};
