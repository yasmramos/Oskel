#include "frontend/parser.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/errors.h"

namespace {

// Collects the statements of a block-like node (BlockStmt) for rewriting.
const std::vector<StmtPtr>* blockStatements(const StmtNode& node) {
  if (const auto* block = std::get_if<BlockStmt>(&node)) return &block->stmts;
  return nullptr;
}

// Flattens a member chain `a.b.c` into its dotted spelling. Returns false when
// the base is not a plain variable, so `f().x` and `a[0].x` cannot be mistaken
// for an enum path.
bool flattenMemberPath(const Expr& expr, std::string& out) {
  if (const auto* variable = std::get_if<VariableExpr>(&expr.node)) {
    out = variable->name;
    return true;
  }
  const auto* member = std::get_if<MemberExpr>(&expr.node);
  if (member == nullptr || member->safe) return false;
  std::string base;
  if (!flattenMemberPath(*member->object, base)) return false;
  out = base + "." + member->name;
  return true;
}

// A `case` label is a literal, an integer range, or a signed numeric literal
// (`-1`, `+1`). Anything else (a variable, a call) would not be a fixed value
// to compare the subject against.
bool isCaseLabel(const ExprPtr& label) {
  if (std::holds_alternative<LiteralExpr>(label->node) ||
      std::holds_alternative<RangeExpr>(label->node)) {
    return true;
  }
  const auto* unary = std::get_if<UnaryExpr>(&label->node);
  return unary != nullptr && (unary->op == Tok::Minus || unary->op == Tok::Plus) &&
         std::holds_alternative<LiteralExpr>(unary->operand->node);
}

}  // namespace

// Parse-time resolution of top-level `const` declarations. Runs as a second
// pass over the finished program because `const` values are resolved while
// parsing: a const may legally reference another top-level const declared
// later in the file. The pass hoists top-level consts to the front of the
// program (in source order), folds each const initializer against the table
// of constants recorded so far, normalizes the node to a literal (so every
// backend gets the folded value without repeating the fold) and records the
// result for later references. Consts nested inside functions or blocks keep
// their folded values from the first parse pass and are not hoisted, so
// their scope rules stay honest.
void Parser::resolveTopLevelConsts(StmtPtr& program) {
  const std::vector<StmtPtr>* stmts = blockStatements(program->node);
  if (!stmts) return;
  // Stable partition: every top-level `const` moves to the front (keeping its
  // source order) and the other statements follow in their own source order.
  // A reference to a top-level `const` from another statement (a `var`, a
  // `let`, a call argument) compiles to `GETGLOBAL`, which reads the global
  // the const declaration binds; without the move the read can run before the
  // binding and fail with "undefined variable". The const initializers
  // themselves are folded below, so they never depend on this order.
  std::vector<StmtPtr> reordered;
  reordered.reserve(stmts->size());
  std::vector<StmtPtr> consts;
  std::vector<StmtPtr> rest;
  for (const StmtPtr& stmt : *stmts) {
    const auto* decl = std::get_if<LetStmt>(&stmt->node);
    if (decl && decl->kind == Tok::Const) {
      consts.push_back(stmt);
    } else {
      rest.push_back(stmt);
    }
  }
  if (consts.empty()) return;
  reordered.insert(reordered.end(), consts.begin(), consts.end());
  reordered.insert(reordered.end(), rest.begin(), rest.end());

  // Build the const table. A top-level const initializer may reference
  // another top-level const declared later in the file, so a single
  // sequential pass is not enough: iterate to a fixed point. Declarations
  // whose initializer still depends on an unresolved const are retried once
  // the missing value becomes available; cycles and genuinely non-constant
  // expressions are reported against the first declaration left unresolved.
  struct Pending {
    StmtPtr stmt;
    std::string name;
    std::string lastReason;
  };
  std::vector<Pending> pending;
  for (const StmtPtr& stmt : consts) {
    const auto& decl = std::get<LetStmt>(stmt->node);
    pending.push_back(Pending{stmt, decl.name, "initializer is not a constant expression"});
  }
  bool progressed = true;
  while (!pending.empty() && progressed) {
    progressed = false;
    for (size_t index = 0; index < pending.size();) {
      auto& item = pending[index];
      auto& decl = std::get<LetStmt>(item.stmt->node);
      Value foldedValue;
      std::string reason;
      if (foldConstant(*decl.init, foldedValue, reason)) {
        // Normalize to a literal so backends never re-evaluate the expression.
        decl.init = makeExpr(LiteralExpr{foldedValue}, item.stmt->line, item.stmt->col);
        constValues_[item.name] = std::move(foldedValue);
        pending.erase(pending.begin() + static_cast<long>(index));
        progressed = true;
      } else {
        item.lastReason = std::move(reason);
        ++index;
      }
    }
  }
  if (!pending.empty()) {
    const Pending& first = pending.front();
    throw CompileError{"'const' '" + first.name + "' initializer must be a constant expression: " +
                           first.lastReason,
                       first.stmt->line, first.stmt->col};
  }
  BlockStmt rebuilt{std::move(reordered)};
  program->node = std::move(rebuilt);
}

Parser::TopLevelScope::TopLevelScope(bool& flag, bool value) : flag_(flag), previous_(flag) {
  flag_ = value;
}

Parser::TopLevelScope::~TopLevelScope() { flag_ = previous_; }

StmtPtr Parser::parseProgram() {
  TopLevelScope topLevel(atTopLevel_);
  // A `struct` name is visible to the whole module, like a hoisted `fn`, so a
  // literal or an annotation may appear before the declaration. `struct` is a
  // keyword, so the only `struct Name` sequence is a declaration; a name that
  // is never declared this way stays out of the set, keeping `name { ... }` a
  // variable followed by a block.
  for (size_t i = 0; i + 1 < tokens_.size(); ++i) {
    if (tokens_[i].type == Tok::Struct && tokens_[i + 1].type == Tok::Ident) {
      structNames_.insert(tokens_[i + 1].lexeme);
    }
    // A `class` name is visible to the whole module too, so an annotation
    // written before the declaration is still accepted. It is kept apart from
    // `structNames_` so `Name { ... }` stays a struct literal only for a real
    // `struct` (a class is instantiated by calling its name, not with braces).
    if (tokens_[i].type == Tok::Class && tokens_[i + 1].type == Tok::Ident) {
      classNames_.insert(tokens_[i + 1].lexeme);
    }
    // An `enum` name is visible to the whole module too: `Color.RED` may be
    // written before the declaration, and `Color` is a valid annotation.
    if (tokens_[i].type == Tok::Enum && tokens_[i + 1].type == Tok::Ident) {
      enumNames_.insert(tokens_[i + 1].lexeme);
    }
  }
  // Imported enums are visible to the whole module too (PHASE 6.9 across
  // modules), so they are seeded before the first statement is parsed. The
  // compiler registers the same names when it compiles the imports.
  prescanImportedEnums();
  std::vector<StmtPtr> statements;
  int line = peek().line;
  int col = peek().col;
  while (!check(Tok::Eof)) {
    if (match(Tok::Semicolon)) continue;
    statements.push_back(parseStatement());
  }
  auto program = makeStmt(BlockStmt{std::move(statements)}, line, col);
  // Second pass: `const` values are resolved at parse time, so a const may
  // reference another top-level const declared later in the file. Hoisting
  // top-level consts to the front of the program makes their folded values
  // available to every subsequent const initializer. Re-running the pass is
  // idempotent (the parser state was consumed by the first pass).
  resolveTopLevelConsts(program);
  return program;
}

void Parser::prescanImportedEnums() {
  if (!enumScanner_) return;
  // Imports are top-level only, so the scan ignores everything inside braces.
  // It reads the import grammar from the token vector instead of consuming the
  // main parser state: the statements are parsed afterwards, in source order.
  int depth = 0;
  for (size_t i = 0; i < tokens_.size(); ++i) {
    const Token& token = tokens_[i];
    if (token.type == Tok::LBrace) {
      ++depth;
      continue;
    }
    if (token.type == Tok::RBrace) {
      if (depth > 0) --depth;
      continue;
    }
    if (depth != 0 || token.type != Tok::Import) continue;
    size_t j = i + 1;
    if (j >= tokens_.size()) break;
    if (tokens_[j].type == Tok::LBrace) {
      // Selective: `import { A, B } from path;`
      ++j;
      std::vector<std::string> names;
      while (j < tokens_.size() && tokens_[j].type != Tok::RBrace) {
        if (tokens_[j].type == Tok::Ident) names.push_back(tokens_[j].lexeme);
        ++j;
      }
      if (j < tokens_.size()) ++j;  // '}'
      if (j < tokens_.size() && tokens_[j].type == Tok::Ident &&
          tokens_[j].lexeme == "from") {
        ++j;
        std::string path;
        if (j < tokens_.size() && tokens_[j].type == Tok::Ident) {
          path = tokens_[j].lexeme;
          ++j;
          while (j + 1 < tokens_.size() && tokens_[j].type == Tok::Dot &&
                 tokens_[j + 1].type == Tok::Ident) {
            path += ".";
            path += tokens_[j + 1].lexeme;
            j += 2;
          }
        }
        if (!path.empty()) seedImportedEnums(path, names, "", false);
      }
      i = j;
      continue;
    }
    // Qualified or wildcard: `import path [.* | as alias];`
    std::string path;
    if (tokens_[j].type != Tok::Ident) continue;
    path = tokens_[j].lexeme;
    ++j;
    while (j + 1 < tokens_.size() && tokens_[j].type == Tok::Dot &&
           tokens_[j + 1].type == Tok::Ident) {
      path += ".";
      path += tokens_[j + 1].lexeme;
      j += 2;
    }
    if (j + 1 < tokens_.size() && tokens_[j].type == Tok::Dot &&
        tokens_[j + 1].type == Tok::Star) {
      seedImportedEnums(path, {}, "", true);
      i = j + 1;
      continue;
    }
    std::string alias;
    if (j + 1 < tokens_.size() && tokens_[j].type == Tok::Ident &&
        tokens_[j].lexeme == "as" && tokens_[j + 1].type == Tok::Ident) {
      alias = tokens_[j + 1].lexeme;
      j += 2;
    }
    if (!alias.empty()) {
      seedImportedEnums(path, {}, alias, false);
    } else {
      seedImportedEnums(path, {}, path, false);
      // `import a.b;` also binds the short name `b` (the compiler does the
      // same), so `b.Enum.MEMBER` is valid as well as `a.b.Enum.MEMBER`.
      const size_t dot = path.rfind('.');
      if (dot != std::string::npos) {
        const std::string shortName = path.substr(dot + 1);
        if (shortName != path.substr(0, path.find('.'))) {
          seedImportedEnums(path, {}, shortName, false);
        }
      }
    }
    i = j;
  }
}

void Parser::seedImportedEnums(const std::string& module,
                               const std::vector<std::string>& selected,
                               const std::string& alias, bool wildcard) {
  if (!enumScanner_) return;
  const auto enums = enumScanner_(module);
  if (enums.empty()) return;
  if (!selected.empty()) {
    for (const std::string& name : selected) {
      if (name.empty() || name[0] == '_') continue;
      auto found = enums.find(name);
      if (found != enums.end()) seedEnum(name, found->second, true);
    }
    return;
  }
  if (wildcard) {
    for (const auto& entry : enums) {
      if (entry.first.empty() || entry.first[0] == '_') continue;
      seedEnum(entry.first, entry.second, true);
    }
    return;
  }
  if (!alias.empty()) {
    for (const auto& entry : enums) {
      if (entry.first.empty() || entry.first[0] == '_') continue;
      seedEnum(alias + "." + entry.first, entry.second, false);
    }
  }
}

void Parser::seedEnum(const std::string& visibleName,
                      const std::vector<EnumMember>& members, bool bare) {
  if (visibleName.empty()) return;
  // A declaration already owns the name: a local enum, struct or class wins
  // over an imported one, exactly like a declared binding wins over an import.
  if (enumNames_.count(visibleName) != 0 || structNames_.count(visibleName) != 0 ||
      classNames_.count(visibleName) != 0) {
    return;
  }
  enumNames_.insert(visibleName);
  importedEnumNames_.insert(visibleName);
  for (const EnumMember& member : members) {
    enumValues_[visibleName + "." + member.name] = Value(member.value);
    if (bare) {
      auto& owners = enumMemberOwners_[member.name];
      if (std::find(owners.begin(), owners.end(), visibleName) == owners.end()) {
        owners.push_back(visibleName);
      }
    }
  }
}

// Signed-overflow predicates for constant folding. The runtime wraps on
// overflow (docs/language.md §2.3), but a `const` whose folded value does not
// fit in an `int` is almost always a mistake, so the parser reports it — the
// same policy it already applies to division by zero and an out-of-range shift
// in a constant expression. All arithmetic is done in unsigned, so the checks
// themselves never overflow.
bool foldOverflowsAdd(int64_t left, int64_t right) {
  const int64_t sum =
      static_cast<int64_t>(static_cast<uint64_t>(left) + static_cast<uint64_t>(right));
  return ((left >= 0) == (right >= 0)) && ((sum >= 0) != (left >= 0));
}
bool foldOverflowsSub(int64_t left, int64_t right) {
  const int64_t diff =
      static_cast<int64_t>(static_cast<uint64_t>(left) - static_cast<uint64_t>(right));
  return ((left >= 0) != (right >= 0)) && ((diff >= 0) != (left >= 0));
}
bool foldOverflowsMul(int64_t left, int64_t right) {
  if (left == 0 || right == 0) return false;
  const bool negative = (left < 0) != (right < 0);
  const uint64_t leftMagnitude =
      left < 0 ? 0ULL - static_cast<uint64_t>(left) : static_cast<uint64_t>(left);
  const uint64_t rightMagnitude =
      right < 0 ? 0ULL - static_cast<uint64_t>(right) : static_cast<uint64_t>(right);
  const uint64_t limit = negative ? (1ULL << 63) : ((1ULL << 63) - 1);
  return leftMagnitude > limit / rightMagnitude;
}

// Binary exponentiation on integers (exponent assumed non-negative), mirroring
// the runtime's `intPow` exactly but also reporting overflow (true). The value
// still wraps like the runtime, so the folded constant is identical to what the
// same expression produces at run time; the flag only drives a warning. The
// loop squares `factor` only while higher exponent bits remain and stops at the
// first overflow, so it stays O(log exponent) even for a huge exponent.
bool foldIntPow(int64_t base, int64_t exponent, int64_t& out) {
  bool overflow = false;
  int64_t result = 1;
  int64_t factor = base;
  int64_t exp = exponent;
  while (exp > 0) {
    if (exp & 1) {
      if (foldOverflowsMul(result, factor)) overflow = true;
      result = static_cast<int64_t>(static_cast<uint64_t>(result) * static_cast<uint64_t>(factor));
    }
    exp >>= 1;
    if (exp > 0) {
      if (foldOverflowsMul(factor, factor)) overflow = true;
      factor = static_cast<int64_t>(static_cast<uint64_t>(factor) * static_cast<uint64_t>(factor));
    }
  }
  out = result;
  return overflow;
}

bool isComparisonOp(Tok op) {
  switch (op) {
    case Tok::EqEq:
    case Tok::BangEq:
    case Tok::Lt:
    case Tok::LtEq:
    case Tok::Gt:
    case Tok::GtEq:
      return true;
    default:
      return false;
  }
}

// Operators that bind tighter than a prefix operator: `**` and the postfix
// forms (`call`, `index`, `.member`, `?.member`, `!!`). Used to decide whether a
// `-9223372036854775808` literal is really the direct operand of the unary minus
// (`-2^63`), or the start of a tighter expression (`-2^63 ** 2` is
// `-(2^63 ** 2)`), where the bare magnitude is out of range.
bool bindsTighterThanUnary(Tok op) {
  switch (op) {
    case Tok::StarStar:
    case Tok::LParen:
    case Tok::LBracket:
    case Tok::Dot:
    case Tok::QuestionDot:
    case Tok::BangBang:
      return true;
    default:
      return false;
  }
}

bool isAssignOp(Tok type) {
  switch (type) {
    case Tok::Eq:
    case Tok::PlusEq:
    case Tok::MinusEq:
    case Tok::StarEq:
    case Tok::SlashEq:
    case Tok::PercentEq:
    case Tok::StarStarEq:
    case Tok::AmpEq:
    case Tok::PipeEq:
    case Tok::CaretEq:
    case Tok::ShlEq:
    case Tok::ShrEq:
    case Tok::AmpAmpEq:
    case Tok::PipePipeEq:
    case Tok::QuestionQuestionEq:
      return true;
    default:
      return false;
  }
}

bool isAssignable(const ExprPtr& expr) {
  if (const auto* member = std::get_if<MemberExpr>(&expr->node)) {
    // `a?.b` is a read-only safe access; it cannot be an assignment target.
    return !member->safe;
  }
  return std::holds_alternative<VariableExpr>(expr->node) ||
         std::holds_alternative<IndexExpr>(expr->node);
}

Parser::Parser(std::vector<Token> tokens, std::string where)
    : tokens_(std::move(tokens)), where_(std::move(where)) {}

void Parser::rejectEnumNameAsBinding(const Token& name) const {
  if (enumNames_.count(name.lexeme) == 0) return;
  throw CompileError{"'" + name.lexeme +
                         "' is an enum name and cannot be used as a variable or function name",
                     name.line, name.col};
}

namespace {
// Maximum nesting depth of the recursive-descent parser. It bounds the C++
// stack a single parse can consume so that a hostile input cannot crash the
// process with a stack overflow. The deepest construct (a nested string
// interpolation, which spawns a sub-parser per level) costs roughly 2 KB of
// stack per level with GCC; the smallest default thread stack we target is
// Windows' 1 MiB, and MSVC's frames are larger than GCC's, so the limit is
// kept well below what a 1 MiB stack could hold at the GCC rate. It stays far
// above any nesting a real program reaches.
constexpr int kMaxParseDepth = 128;
}  // namespace

Parser::DepthGuard::DepthGuard(const Parser& parser) : parser_(parser) {
  if (++parser_.depth_ > kMaxParseDepth) {
    --parser_.depth_;
    parser_.error(parser_.peek(), "expression nesting is too deep");
  }
}

Parser::DepthGuard::~DepthGuard() { --parser_.depth_; }

const Token& Parser::peek(size_t ahead) const {
  size_t index = pos_ + ahead;
  if (index >= tokens_.size()) return tokens_.back();
  return tokens_[index];
}

const Token& Parser::previous() const {
  return pos_ == 0 ? tokens_.front() : tokens_[pos_ - 1];
}

bool Parser::check(Tok type) const { return peek().type == type; }

bool Parser::match(Tok type) {
  if (!check(type)) return false;
  ++pos_;
  return true;
}

const Token& Parser::advance() {
  if (!check(Tok::Eof)) ++pos_;
  return previous();
}

const Token& Parser::consume(Tok type, const std::string& message) {
  if (check(type)) return advance();
  error(peek(), message);
}

std::string Parser::describe(const Token& token) {
  switch (token.type) {
    case Tok::Eof:
      return "end of input";
    case Tok::Ident:
      return "'" + token.lexeme + "'";
    case Tok::Int:
    case Tok::Float:
    case Tok::Double:
      return "'" + token.lexeme + "'";
    case Tok::Str:
      return "a string literal";
    default:
      return "'" + token.lexeme + "'";
  }
}

void Parser::error(const Token& token, const std::string& message) const {
  throw CompileError{message + ", found " + describe(token), token.line, token.col};
}

ExprPtr Parser::parseFullExpression() {
  auto expr = parseExpression();
  match(Tok::Semicolon);
  if (!check(Tok::Eof)) error(peek(), "expected end of input");
  return expr;
}

// ---------------------------------------------------------------- statements

StmtPtr Parser::parseStatement() {
  DepthGuard guard(*this);
  const Token& token = peek();

  if (match(Tok::Var) || match(Tok::Let) || match(Tok::Const)) return parseVarDecl();
  if (check(Tok::At)) return parseAttributedDecl();
  if (match(Tok::Async)) {
    consume(Tok::Fn, "expected 'fn' after 'async'");
    return parseFnDecl(false, true);
  }
  if (match(Tok::Fn)) return parseFnDecl();
  if (match(Tok::Native)) return parseNativeDecl();
  if (match(Tok::Extern)) return parseExternDecl();
  if (match(Tok::Struct)) return parseStructDecl();
  if (match(Tok::Class)) return parseClassDecl();
  if (match(Tok::Enum)) return parseEnumDecl();
  if (match(Tok::If)) return parseIf();
  if (match(Tok::While)) return parseWhile();
  if (match(Tok::Do)) return parseDoWhile();
  if (match(Tok::For)) return parseFor();
  if (match(Tok::Return)) return parseReturn();
  if (match(Tok::Defer)) return parseDefer();
  if (match(Tok::Import)) return parseImport();
  if (match(Tok::Switch)) return parseSwitch();
  if (check(Tok::Case) || check(Tok::Default)) {
    // Thrown directly, not through `error`, so the message matches the other
    // "only allowed here" diagnostics without the trailing ", found ...".
    throw CompileError{"'" + peek().lexeme + "' is only allowed inside a 'switch'", peek().line,
                       peek().col};
  }
  if (match(Tok::Break)) {
    consume(Tok::Semicolon, "expected ';' after 'break'");
    return makeStmt(BreakStmt{}, token.line, token.col);
  }
  if (match(Tok::Continue)) {
    consume(Tok::Semicolon, "expected ';' after 'continue'");
    return makeStmt(ContinueStmt{}, token.line, token.col);
  }
  if (check(Tok::LBrace)) return parseBlock();
  return parseExpressionStatement();
}

StmtPtr Parser::parseVarDecl() {
  const Token& keyword = previous();
  // Binding kinds (see LetStmt): `var` is mutable with an optional
  // initializer; `let` is immutable and evaluated at runtime; `const` is
  // immutable and must be initialized with a compile-time constant
  // expression. The parser enforces every rule so downstream phases only
  // need to honor immutability.
  const bool isImmutable = keyword.type != Tok::Var;
  const Token& name = consume(Tok::Ident, "expected a variable name");
  rejectEnumNameAsBinding(name);
  TypeRef type = parseTypeAnnotation();
  ExprPtr init;
  if (match(Tok::Eq)) {
    init = parseExpression();
    // A `const` initializer is folded as soon as it is known, so a later
    // declaration (an `enum` value in particular) can use it. A top-level
    // const may reference another top-level const declared later in the file,
    // so a failed fold there is left for resolveTopLevelConsts, which retries
    // to a fixed point after the whole program is parsed. A nested const has no
    // such second chance: it must fold here.
    if (keyword.type == Tok::Const) {
      Value constantValue;
      std::string reason;
      if (foldConstant(*init, constantValue, reason)) {
        // Normalize the node to a literal so every backend gets the folded
        // value without repeating the fold.
        init = makeExpr(LiteralExpr{constantValue}, keyword.line, keyword.col);
        constValues_[name.lexeme] = constantValue;
      } else if (!atTopLevel_) {
        error(keyword, "'const' initializer must be a constant expression: " + reason);
      }
    }
  } else if (isImmutable) {
    error(peek(), "'" + keyword.lexeme + "' declaration requires a value (use 'var' for a mutable binding)");
  }
  consume(Tok::Semicolon, "expected ';' after variable declaration");
  LetStmt stmt{name.lexeme, std::move(type), std::move(init), keyword.type};
  return makeStmt(std::move(stmt), keyword.line, keyword.col);
}

// Parses an optional `: <type>` annotation. The type name is an identifier,
// optionally followed by `?` (nullability, used by PHASE 6.5). Unknown type
// names are rejected here so the compiler only ever sees valid ones.
//
// PHASE 6.7.4 adds union types: `T | E | ...` in a type position. Each
// alternative is a known type name or a struct name, and the special name
// `nil` folds into the nullable flag rather than becoming a member, so
// `int | string | nil` is the union `int | string` with `nullable = true` and
// `string | nil` is simply `string?`.
//
// `void` is a return-only annotation (it maps to C's `void` in the FFI, PHASE
// 6.6), so `allowVoid` is true only in the `fn`/`native fn` return position.
TypeRef Parser::parseTypeAnnotation(bool allowVoid) {
  DepthGuard guard(*this);
  if (!match(Tok::Colon)) return TypeRef{};
  // A callback signature (PHASE 6.6): `fn(int, int): int`. `fn` is a keyword,
  // so it cannot be read as an ordinary type name; the parameter and return
  // types are scalar FFI names (a nested callback or a struct is rejected by
  // the FFI signature validation, not here).
  if (match(Tok::Fn)) {
    TypeRef type;
    type.name = "fn";
    consume(Tok::LParen, "expected '(' after 'fn' in a callback type");
    if (!check(Tok::RParen)) {
      while (true) {
        const Token& paramType = consume(Tok::Ident, "expected a parameter type in the callback");
        type.callbackParams.push_back(paramType.lexeme);
        if (!match(Tok::Comma)) break;
      }
    }
    consume(Tok::RParen, "expected ')' to close the callback parameter list");
    TypeRef ret = parseTypeAnnotation(/*allowVoid=*/true);
    type.callbackReturn = ret.name.empty() ? "void" : ret.name;
    return type;
  }

  TypeRef type = parseTypeAlternative(allowVoid);
  // Union: `T | E | ...`. A callback is not a union member, so a `fn(...)`
  // signature (which already consumed its own `: ret`) never reaches here.
  // `nil` is a keyword token, so it is recognized before an alternative is
  // parsed; it folds into the nullable flag instead of becoming a member.
  while (match(Tok::Pipe)) {
    if (match(Tok::Nil)) {
      type.nullable = true;
      continue;
    }
    TypeRef member = parseTypeAlternative(/*allowVoid=*/false);
    if (member.name == type.name ||
        std::find(type.alternatives.begin(), type.alternatives.end(), member.name) !=
            type.alternatives.end()) {
      continue;  // a repeated member is redundant, not an error
    }
    type.alternatives.push_back(member.name);
  }
  if (type.name == "void" && type.isUnion()) {
    const Token& where = previous();
    throw CompileError{"'void' cannot be a member of a union", where.line, where.col};
  }
  return type;
}

// One alternative of a union type (or a whole type when there is no `|`): a
// known type name or a struct name, with an optional `?`. `nil` is accepted as
// a pseudo-name here and folded by the caller.
TypeRef Parser::parseTypeAlternative(bool allowVoid) {
  const Token& typeName = consume(Tok::Ident, "expected a type name after ':'");
  TypeRef type;
  type.name = typeName.lexeme;
  // A qualified type name (`geom.Color`, PHASE 6.9 across modules): the
  // segments name a module and an enum it exports. The compiler resolved the
  // import before this parse, so the dotted name is in `enumNames_` when it is
  // real; an unknown dotted name is still rejected here.
  while (check(Tok::Dot) && peek(1).is(Tok::Ident)) {
    advance();  // '.'
    type.name += ".";
    type.name += advance().lexeme;
  }
  if (type.name == "void" && !allowVoid) {
    // Thrown directly, not through `error`, so the message matches the other
    // "only allowed here" diagnostics without the trailing ", found ...".
    throw CompileError{"'void' is only allowed as a return type", typeName.line,
                       typeName.col};
  }
  // A struct name is a valid annotation too (PHASE 6.5): `let p: Point = ...`.
  // A class name is also a valid annotation (it denotes the map an instance is),
  // so the same lookup covers both. An enum name is a compile-time integer type
  // (PHASE 6.9): the value is an `int` at run time, but the name keeps the
  // annotation readable and lets the compiler reject mixing two enums.
  if (!isKnownTypeName(type.name) && structNames_.count(type.name) == 0 &&
      classNames_.count(type.name) == 0 && enumNames_.count(type.name) == 0) {
    error(typeName, "unknown type name");
  }
  if (match(Tok::Question)) type.nullable = true;
  if (type.name == "void" && type.nullable) {
    throw CompileError{"'void' cannot be nullable", typeName.line, typeName.col};
  }
  return type;
}

// Compile-time constant folding for `const` initializers. Supported:
// literals (int/float/string/bool/nil), references to previously declared
// consts, unary +/-/! and binary arithmetic/comparison over constants.
// Anything else (calls, arrays/maps, non-const variables) is rejected with
// a human-readable reason.
bool Parser::foldConstant(const Expr& expr, Value& out, std::string& reason) const {
  DepthGuard guard(*this);
  // Variant alternatives are addressed by type (never by raw index): the
  // order of ExprNode alternatives changes as nodes are added to the AST.
  if (const auto* lit = std::get_if<LiteralExpr>(&expr.node)) {
    out = lit->value;
    return true;
  }
  if (const auto* var = std::get_if<VariableExpr>(&expr.node)) {
    auto it = constValues_.find(var->name);
    if (it != constValues_.end()) {
      out = it->second;
      return true;
    }
    // A bare enum member is a constant too (PHASE 6.9), as long as exactly one
    // enum declares it. `const X = RED;` folds like `const X = Color.RED;`.
    auto owners = enumMemberOwners_.find(var->name);
    if (owners != enumMemberOwners_.end()) {
      if (owners->second.size() > 1) {
        reason = "ambiguous enum member '" + var->name + "'; write Enum.MEMBER";
        return false;
      }
      auto value = enumValues_.find(owners->second.front() + "." + var->name);
      if (value != enumValues_.end()) {
        out = value->second;
        return true;
      }
    }
    reason = "'" + var->name + "' is not a previously declared 'const'";
    return false;
  }
  if (const auto* member = std::get_if<MemberExpr>(&expr.node)) {
    if (!member->safe) {
      // A member chain that names an enum member is a constant: `Color.RED`
      // locally, `geom.Color.RED` through an import. A qualified chain has a
      // `MemberExpr` object, so the whole path is flattened, not just a
      // `VariableExpr` object.
      std::string path;
      if (flattenMemberPath(expr, path)) {
        auto it = enumValues_.find(path);
        if (it != enumValues_.end()) {
          out = it->second;
          return true;
        }
        const size_t dot = path.rfind('.');
        if (dot != std::string::npos) {
          const std::string owner = path.substr(0, dot);
          if (enumNames_.count(owner) != 0) {
            reason = "enum '" + owner + "' has no member '" + path.substr(dot + 1) + "'";
            return false;
          }
        }
      }
    }
    reason = "member access is not a compile-time constant";
    return false;
  }
  if (const auto* u = std::get_if<UnaryExpr>(&expr.node)) {
    Value operand;
    if (!foldConstant(*u->operand, operand, reason)) return false;
    switch (u->op) {
        case Tok::Bang:
          out = Value(!operand.truthy());
          return true;
        case Tok::Minus:
          if (operand.isInt()) {
            const int64_t value = operand.asInt();
            if (value == INT64_MIN) {
              warn("integer overflow in a constant expression; the value wraps around", expr.line,
                   expr.col);
            }
            out = Value(static_cast<int64_t>(0ULL - static_cast<uint64_t>(value)));
            return true;
          }
          if (operand.isFloat()) {
            out = Value::fromFloat(-static_cast<float>(operand.asFloat()));
            return true;
          }
          if (operand.isDouble()) { out = Value(-operand.asDouble()); return true; }
          reason = "unary '-' needs a numeric constant";
          return false;
        case Tok::Plus:
          if (operand.isNumber()) { out = operand; return true; }
          reason = "unary '+' needs a numeric constant";
          return false;
        case Tok::Tilde:
          if (operand.isInt()) { out = Value(~operand.asInt()); return true; }
          reason = "unary '~' needs an integer constant";
          return false;
        default:
          reason = "unsupported unary operator in a constant expression";
          return false;
      }
  }
  if (const auto* bPtr = std::get_if<BinaryExpr>(&expr.node)) {
      const BinaryExpr& b = *bPtr;
      // Short-circuit operators fold their operands and apply the same
      // lazy semantics as the interpreter.
      if (b.op == Tok::AmpAmp || b.op == Tok::PipePipe) {
        Value left;
        if (!foldConstant(*b.lhs, left, reason)) return false;
        const bool takeRight = (b.op == Tok::AmpAmp) ? left.truthy() : !left.truthy();
        if (!takeRight) { out = left; return true; }
        return foldConstant(*b.rhs, out, reason);
      }
      // `a ?? b`: `a` unless it is nil. Only nil triggers the fallback, so a
      // falsy-but-non-nil left side (0, false, "") is folded as-is.
      if (b.op == Tok::QuestionQuestion) {
        Value left;
        if (!foldConstant(*b.lhs, left, reason)) return false;
        if (!left.isNil()) { out = left; return true; }
        return foldConstant(*b.rhs, out, reason);
      }
      Value left;
      Value right;
      if (!foldConstant(*b.lhs, left, reason)) return false;
      if (!foldConstant(*b.rhs, right, reason)) return false;
      // String concatenation of two constant strings.
      if (b.op == Tok::Plus && left.isStr() && right.isStr()) {
        out = Value(left.asStr() + right.asStr());
        return true;
      }
      // Comparisons mirror the interpreter's ordering for numbers, strings
      // and booleans.
      if (isComparisonOp(b.op)) {
        if (b.op == Tok::EqEq) { out = Value(left.equals(right)); return true; }
        if (b.op == Tok::BangEq) { out = Value(!left.equals(right)); return true; }
        bool ordered = false;
        int64_t cmpRank = 0;  // -1: left < right, 0: equal, 1: left > right
        if (left.isNumber() && right.isNumber()) {
          if (left.isInt() && right.isInt()) {
            cmpRank = left.asInt() < right.asInt() ? -1 : (right.asInt() < left.asInt() ? 1 : 0);
          } else {
            cmpRank = left.number() < right.number() ? -1 : (right.number() < left.number() ? 1 : 0);
          }
          ordered = true;
        } else if (left.isStr() && right.isStr()) {
          cmpRank = left.asStr() < right.asStr() ? -1 : (right.asStr() < left.asStr() ? 1 : 0);
          ordered = true;
        } else if (left.isBool() && right.isBool()) {
          cmpRank = (!left.asBool() && right.asBool()) ? -1
                    : ((left.asBool() && !right.asBool()) ? 1 : 0);
          ordered = true;
        }
        if (!ordered) {
          reason = "cannot compare '" + std::string(left.typeName()) + "' with '" +
                   std::string(right.typeName()) + "' in a constant expression";
          return false;
        }
        switch (b.op) {
          case Tok::Lt: out = Value(cmpRank < 0); return true;
          case Tok::LtEq: out = Value(cmpRank <= 0); return true;
          case Tok::Gt: out = Value(cmpRank > 0); return true;
          case Tok::GtEq: out = Value(cmpRank >= 0); return true;
          default: return false;
        }
      }
      // Numeric promotion mirrors the interpreter: int op float -> float.
      if (!left.isNumber() || !right.isNumber()) {
        reason = "arithmetic in a constant expression needs numeric operands";
        return false;
      }
      if (left.isInt() && right.isInt()) {
        const int64_t a = left.asInt();
        const int64_t c = right.asInt();
        // `foldOverflow` records a warning at the operator's position but keeps
        // the wrapped value, so the bytecode is unchanged and the program runs.
        auto foldOverflow = [&]() {
          warn("integer overflow in a constant expression; the value wraps around",
               b.opLine, b.opCol);
        };
        switch (b.op) {
          case Tok::Plus:
            if (foldOverflowsAdd(a, c)) foldOverflow();
            out = Value(static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(c)));
            return true;
          case Tok::Minus:
            if (foldOverflowsSub(a, c)) foldOverflow();
            out = Value(static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(c)));
            return true;
          case Tok::Star:
            if (foldOverflowsMul(a, c)) foldOverflow();
            out = Value(static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(c)));
            return true;
          case Tok::Slash:
            if (c == 0) { reason = "division by zero in a constant expression"; return false; }
            if (a == INT64_MIN && c == -1) { out = Value(a); return true; }
            out = Value(a / c); return true;
          case Tok::Percent:
            if (c == 0) { reason = "division by zero in a constant expression"; return false; }
            if (a == INT64_MIN && c == -1) { out = Value(static_cast<int64_t>(0)); return true; }
            out = Value(a % c); return true;
          case Tok::StarStar: {
            if (c >= 0) {
              int64_t powered = 0;
              if (foldIntPow(a, c, powered)) foldOverflow();
              out = Value(powered);
              return true;
            }
            out = Value(std::pow(static_cast<double>(a), static_cast<double>(c)));
            return true;
          }
          case Tok::Amp: out = Value(a & c); return true;
          case Tok::Pipe: out = Value(a | c); return true;
          case Tok::Caret: out = Value(a ^ c); return true;
          case Tok::Shl:
            if (c < 0 || c > 63) { reason = "shift count must be between 0 and 63"; return false; }
            out = Value(static_cast<int64_t>(static_cast<uint64_t>(a) << static_cast<uint64_t>(c)));
            return true;
          case Tok::Shr:
            if (c < 0 || c > 63) { reason = "shift count must be between 0 and 63"; return false; }
            out = Value(a >> c);
            return true;
          default:
            reason = "unsupported binary operator in a constant expression";
            return false;
        }
      }
      const double a = left.number();
      const double c = right.number();
      // Java-style promotion: int/float -> float, anything with a double -> double.
      const bool asFloat = !left.isDouble() && !right.isDouble();
      auto makeNum = [&](double result) {
        return asFloat ? Value::fromFloat(static_cast<float>(result)) : Value(result);
      };
      switch (b.op) {
        case Tok::Plus: out = makeNum(a + c); return true;
        case Tok::Minus: out = makeNum(a - c); return true;
        case Tok::Star: out = makeNum(a * c); return true;
        case Tok::Slash:
          if (c == 0) { reason = "division by zero in a constant expression"; return false; }
          out = makeNum(a / c); return true;
        case Tok::Percent:
          if (c == 0) { reason = "division by zero in a constant expression"; return false; }
          out = makeNum(std::fmod(a, c)); return true;
        case Tok::StarStar: out = makeNum(std::pow(a, c)); return true;
        default:
          reason = "unsupported binary operator in a constant expression";
          return false;
      }
  }
  reason = "expression is not a compile-time constant";
  return false;
}

StmtPtr Parser::parseAttributedDecl() {
  // `@name` before a declaration (PHASE 6.7.5). Only `@MustUse` is known; an
  // unknown attribute is rejected rather than ignored, so a typo cannot
  // silently drop the check. Attributes are metadata: they set a flag on the
  // declaration and add no bytecode.
  advance();  // '@'
  bool mustUse = parseAttributes();
  if (match(Tok::Async)) {
    consume(Tok::Fn, "expected 'fn' after 'async'");
    return parseFnDecl(mustUse, true);
  }
  if (match(Tok::Fn)) return parseFnDecl(mustUse);
  if (match(Tok::Native)) return parseNativeDecl(mustUse);
  error(peek(), "an attribute must precede a 'fn', 'async fn' or 'native fn' declaration");
}

// One or more `@name` attributes. The caller has already consumed the first
// `@`. Returns whether `@MustUse` appeared; a duplicate is harmless.
bool Parser::parseAttributes() {
  bool mustUse = false;
  for (;;) {
    const Token& name = consume(Tok::Ident, "expected an attribute name after '@'");
    if (name.lexeme == "MustUse") {
      mustUse = true;
    } else {
      throw CompileError{"unknown attribute '@" + name.lexeme + "'", name.line, name.col};
    }
    if (!check(Tok::At)) break;
    advance();  // consume the next '@'
  }
  return mustUse;
}

StmtPtr Parser::parseFnDecl(bool mustUse, bool isAsync) {
  const Token& keyword = previous();
  if (check(Tok::LParen)) {
    error(peek(),
          "'fn' declares a named function; write an anonymous function as a lambda: "
          "'x => expr' or '(a, b) => expr'");
  }
  const Token& name = consume(Tok::Ident, "expected a function name");
  rejectEnumNameAsBinding(name);
  auto params = parseParams();
  TypeRef returnType = parseTypeAnnotation(/*allowVoid=*/true);
  StmtPtr body;
  if (check(Tok::Arrow)) {
    const Token& arrow = advance();
    const bool isBlock = check(Tok::LBrace);
    if (isAsync) {
      AsyncScope scope(asyncDepth_, asyncDepth_ + 1);
      body = parseFunctionBody(arrow);
    } else {
      body = parseFunctionBody(arrow);
    }
    // An expression body is a statement of its own and needs a terminator.
    if (isBlock) {
      match(Tok::Semicolon);
    } else {
      consume(Tok::Semicolon, "expected ';' after the function body");
    }
  } else {
    if (isAsync) {
      AsyncScope scope(asyncDepth_, asyncDepth_ + 1);
      body = parseBlock();
    } else {
      body = parseBlock();
    }
    match(Tok::Semicolon);
  }
  FnDeclStmt decl{name.lexeme, std::move(params), std::move(returnType), std::move(body), false,
                  mustUse};
  decl.isAsync = isAsync;
  return makeStmt(std::move(decl), keyword.line, keyword.col);
}

// `native fn name(params): ret;` (PHASE 6.5): a bodyless declaration that binds
// `name` to a native function the host registered (the same registry
// `registerBuiltins` fills). The signature is checked like a normal function
// and the binding is resolved at runtime, so no opcode is added.
//
// Like `struct` and `import`, it is module-level metadata, so it is only
// allowed at the top level. It has no body: a `{ ... }` or `=>` body is
// rejected, and the declaration ends with `;`.
StmtPtr Parser::parseNativeDecl(bool mustUse) {
  const Token& keyword = previous();
  if (nestingDepth_ > 0) {
    throw CompileError{"'native fn' is only allowed at the top level of a module", keyword.line,
                       keyword.col};
  }
  consume(Tok::Fn, "expected 'fn' after 'native'");
  const Token& name = consume(Tok::Ident, "expected a function name");
  rejectEnumNameAsBinding(name);
  auto params = parseParams();
  TypeRef returnType = parseTypeAnnotation(/*allowVoid=*/true);
  if (check(Tok::Arrow) || check(Tok::LBrace)) {
    throw CompileError{"a 'native fn' has no body; it binds to a function the runtime provides",
                       peek().line, peek().col};
  }
  consume(Tok::Semicolon, "expected ';' after the 'native fn' declaration");
  FnDeclStmt decl{name.lexeme, std::move(params), std::move(returnType), nullptr};
  decl.isNative = true;
  decl.mustUse = mustUse;
  return makeStmt(std::move(decl), keyword.line, keyword.col);
}

// `extern "C" { fn name(params): ret; ... }` or `extern fn name(params): ret;`
// (PHASE 6.6). A declaration of C functions. Two spellings are accepted:
//
//   extern "C" { fn sqrt(x: float): float; }   // block: explicit ABI
//   extern fn sqrt(x: float): float;           // single: implicit "C" ABI
//
// Both desugar to the same `ExternBlockStmt`, so hoisting, signature checking
// and the runtime binding are shared and no new AST node or opcode is needed.
// The ABI is a plain string literal (no interpolation): only `"C"` is
// supported. Inside a block each entry is a bodyless `fn ...;`, parsed with the
// same grammar as `native fn` and stored with `isNative` set, so the compiler
// handles it exactly like a top-level `native fn`. Like `import`, `struct` and
// `native fn`, the declaration is module-level metadata: a nested one is a
// compile error, and it emits no bytecode.
StmtPtr Parser::parseExternDecl() {
  const Token& keyword = previous();
  if (nestingDepth_ > 0) {
    throw CompileError{"'extern' is only allowed at the top level of a module", keyword.line,
                       keyword.col};
  }

  // `extern fn ...;`: the ABI is implicit, so the declaration is a one-entry
  // block. `fn` cannot start an ABI string, so it is unambiguous.
  if (match(Tok::Fn)) {
    std::vector<FnDeclStmt> decls;
    decls.push_back(parseExternFnDecl());
    return makeStmt(ExternBlockStmt{"C", std::move(decls)}, keyword.line, keyword.col);
  }

  const Token& abiToken = consume(Tok::Str, "expected an ABI string or 'fn' after 'extern'");
  // A plain string literal has exactly one text part; an interpolated one has
  // an expression part and is meaningless as an ABI name.
  if (abiToken.parts.size() != 1 || abiToken.parts[0].isExpr) {
    throw CompileError{"the 'extern' ABI must be a plain string literal", abiToken.line,
                       abiToken.col};
  }
  const std::string abi = abiToken.parts[0].text;
  if (abi != "C") {
    throw CompileError{"unknown ABI '" + abi + "'; only \"C\" is supported", abiToken.line,
                       abiToken.col};
  }

  // `extern "C" fn foo();`: the explicit ABI with a single declaration, sugar
  // for a one-entry block, exactly like the implicit-ABI `extern fn ...;` form.
  if (match(Tok::Fn)) {
    std::vector<FnDeclStmt> decls;
    decls.push_back(parseExternFnDecl());
    return makeStmt(ExternBlockStmt{abi, std::move(decls)}, keyword.line, keyword.col);
  }

  consume(Tok::LBrace, "expected '{' after the 'extern' ABI");
  std::vector<FnDeclStmt> decls;
  while (!check(Tok::RBrace)) {
    if (check(Tok::Eof)) error(peek(), "expected '}' to close the 'extern' block");
    if (match(Tok::Semicolon)) continue;
    // Only bodyless `fn` declarations are valid inside an `extern` block.
    if (!match(Tok::Fn)) {
      error(peek(), "only 'fn' declarations are allowed inside an 'extern' block");
    }
    decls.push_back(parseExternFnDecl());
  }
  consume(Tok::RBrace, "expected '}' to close the 'extern' block");

  return makeStmt(ExternBlockStmt{abi, std::move(decls)}, keyword.line, keyword.col);
}

// One bodyless `fn name(params): ret;` in an `extern` declaration (the caller
// has already consumed the `fn`). Shared by the block and the single-line form,
// so both accept exactly the same grammar and produce the same `FnDeclStmt`.
FnDeclStmt Parser::parseExternFnDecl() {
  const Token& name = consume(Tok::Ident, "expected a function name");
  rejectEnumNameAsBinding(name);
  bool cVariadic = false;
  auto params = parseParams(/*allowBareEllipsis=*/true, &cVariadic);
  TypeRef returnType = parseTypeAnnotation(/*allowVoid=*/true);
  if (check(Tok::Arrow) || check(Tok::LBrace)) {
    throw CompileError{"an 'extern' declaration has no body; it declares a C function",
                       peek().line, peek().col};
  }
  consume(Tok::Semicolon, "expected ';' after the 'extern' declaration");
  // A Oskel `rest...` collects the arguments into an array, which a C call has
  // no way to pass; the C spelling is a bare `...`, already recorded in
  // `cVariadic`. Rejecting it here keeps the two variadic notions apart.
  for (const Param& param : params) {
    if (param.variadic) {
      throw CompileError{"'" + param.name + "...' collects arguments into an array; a C variadic "
                         "is written as a trailing '...'",
                         name.line, name.col};
    }
  }
  FnDeclStmt decl{name.lexeme, std::move(params), std::move(returnType), nullptr};
  decl.isNative = true;
  decl.cVariadic = cVariadic;
  return decl;
}

// `struct Name { field: T; other; ... }` (PHASE 6.5). A data-only type: fields
// are separated (and optionally terminated) by ';', and the type annotation is
// optional (`struct P { x: int; y; }`). Declaring a name twice in the same
// struct is an error. The declaration is registered so later annotations and
// literals can refer to it.
StmtPtr Parser::parseStructDecl() {
  const Token& keyword = previous();
  if (nestingDepth_ > 0) {
    // Like `import`, a struct is module-level metadata: declaring it inside a
    // block or a function body is rejected, so its shape is the same for every
    // literal in the module.
    throw CompileError{"'struct' is only allowed at the top level of a module", keyword.line,
                       keyword.col};
  }
  const Token& name = consume(Tok::Ident, "expected a struct name");
  rejectEnumNameAsBinding(name);
  if (declaredStructs_.count(name.lexeme) != 0) {
    throw CompileError{"struct '" + name.lexeme + "' is already declared", name.line, name.col};
  }
  if (declaredClasses_.count(name.lexeme) != 0) {
    throw CompileError{"'" + name.lexeme + "' is already declared as a class", name.line,
                       name.col};
  }
  consume(Tok::LBrace, "expected '{' to start the struct body");

  StructDeclStmt decl;
  decl.name = name.lexeme;
  std::unordered_set<std::string> seen;
  while (!check(Tok::RBrace)) {
    if (check(Tok::Eof)) error(peek(), "expected '}' to close the struct body");
    if (match(Tok::Semicolon)) continue;  // stray separators are harmless
    const Token& field = consume(Tok::Ident, "expected a field name");
    if (!seen.insert(field.lexeme).second) {
      throw CompileError{
          "duplicate field '" + field.lexeme + "' in struct '" + decl.name + "'", field.line,
          field.col};
    }
    StructField entry;
    entry.name = field.lexeme;
    entry.line = field.line;
    entry.col = field.col;
    entry.type = parseTypeAnnotation();
    decl.fields.push_back(std::move(entry));
    if (!match(Tok::Semicolon) && !check(Tok::RBrace)) {
      error(peek(), "expected ';' or '}' after the struct field");
    }
  }
  consume(Tok::RBrace, "expected '}' to close the struct body");
  match(Tok::Semicolon);

  structNames_.insert(decl.name);
  declaredStructs_.insert(decl.name);
  return makeStmt(std::move(decl), keyword.line, keyword.col);
}

// `enum Name { A, B = 5, C }` (PHASE 6.9). A scoped set of integer constants.
// The declaration is compile-time metadata: it emits no bytecode, and each
// member is folded here to the `int64_t` the compiler will load at a use.
StmtPtr Parser::parseEnumDecl() {
  const Token& keyword = previous();
  if (nestingDepth_ > 0) {
    throw CompileError{"'enum' is only allowed at the top level of a module", keyword.line,
                       keyword.col};
  }
  const Token& name = consume(Tok::Ident, "expected an enum name");
  if (declaredEnums_.count(name.lexeme) != 0) {
    throw CompileError{"enum '" + name.lexeme + "' is already declared", name.line, name.col};
  }
  if (declaredStructs_.count(name.lexeme) != 0) {
    throw CompileError{"'" + name.lexeme + "' is already declared as a struct", name.line,
                       name.col};
  }
  if (declaredClasses_.count(name.lexeme) != 0) {
    throw CompileError{"'" + name.lexeme + "' is already declared as a class", name.line,
                       name.col};
  }
  consume(Tok::LBrace, "expected '{' to start the enum body");

  EnumDeclStmt decl;
  decl.name = name.lexeme;
  std::unordered_set<std::string> seen;
  int64_t next = 0;
  bool nextOverflows = false;
  // Bare member names are visible while the enum body is parsed, so
  // `B = A + 1` works; the entries are restored after the closing brace so a
  // bare name does not leak into the surrounding const scope.
  std::vector<std::pair<std::string, std::optional<Value>>> savedBare;
  auto restoreBare = [&]() {
    for (auto it = savedBare.rbegin(); it != savedBare.rend(); ++it) {
      if (it->second.has_value()) {
        constValues_[it->first] = *it->second;
      } else {
        constValues_.erase(it->first);
      }
    }
    savedBare.clear();
  };

  while (!check(Tok::RBrace)) {
    if (check(Tok::Eof)) error(peek(), "expected '}' to close the enum body");
    if (match(Tok::Semicolon)) continue;  // stray separators are harmless
    const Token& member = consume(Tok::Ident, "expected an enum member name");
    if (!seen.insert(member.lexeme).second) {
      throw CompileError{
          "duplicate member '" + member.lexeme + "' in enum '" + decl.name + "'", member.line,
          member.col};
    }
    int64_t value = next;
    if (match(Tok::Eq)) {
      ExprPtr init = parseExpression();
      Value folded;
      std::string reason;
      if (!foldConstant(*init, folded, reason)) {
        throw CompileError{"enum member '" + member.lexeme +
                               "' value must be a constant expression: " + reason,
                           member.line, member.col};
      }
      if (!folded.isInt()) {
        throw CompileError{"enum member '" + member.lexeme + "' value must be an int",
                           member.line, member.col};
      }
      value = folded.asInt();
      nextOverflows = false;
    } else if (nextOverflows) {
      throw CompileError{"enum member '" + member.lexeme + "' value overflows int", member.line,
                         member.col};
    }

    EnumMember entry;
    entry.name = member.lexeme;
    entry.value = value;
    entry.line = member.line;
    entry.col = member.col;
    decl.members.push_back(entry);

    const std::string qualified = decl.name + "." + entry.name;
    enumValues_[qualified] = Value(entry.value);
    enumMemberOwners_[entry.name].push_back(decl.name);
    auto previous = constValues_.find(entry.name);
    savedBare.emplace_back(entry.name,
                           previous == constValues_.end() ? std::nullopt
                                                          : std::optional<Value>(previous->second));
    constValues_[entry.name] = Value(entry.value);

    if (value == INT64_MAX) {
      nextOverflows = true;
    } else {
      next = value + 1;
    }
    if (!match(Tok::Comma) && !check(Tok::RBrace)) {
      error(peek(), "expected ',' or '}' after the enum member");
    }
  }
  consume(Tok::RBrace, "expected '}' to close the enum body");
  match(Tok::Semicolon);
  restoreBare();

  enumNames_.insert(decl.name);
  declaredEnums_.insert(decl.name);
  return makeStmt(std::move(decl), keyword.line, keyword.col);
}

StmtPtr Parser::parseFunctionBody(const Token& arrow) {
  if (check(Tok::LBrace)) return parseBlock();
  auto result = parseExpression();
  std::vector<StmtPtr> statements;
  statements.push_back(makeStmt(ReturnStmt{std::move(result)}, arrow.line, arrow.col));
  return makeStmt(BlockStmt{std::move(statements)}, arrow.line, arrow.col);
}

// `class Name { field: T; ...; fn method(params): ret { ... } }`.
//
// A class is sugar over the map-plus-closures idiom the language already has,
// so it adds one token (`class`), no AST node, no opcode and no engine code.
// The parser rewrites the declaration into an ordinary top-level constructor
// function that
//
//   1. builds the instance as a map whose keys are the fields (each nil),
//   2. stores every method as a closure capturing `self` (explicit `self`),
//   3. runs `init` when present, forwarding the constructor arguments,
//   4. returns the instance.
//
// Instantiating is calling the class name (`Point(3, 4)`); there is no `new`.
// The `init` method's parameters are the constructor's parameters, so the
// arity the caller writes is the arity `init` declares. There is no
// inheritance. Because the desugared form is plain Oskel, the VM and the AOT
// backend run it identically with no backend work (docs/language.md §4.8).

// What `init` does to the `self.<field>` targets it writes. `initialized` holds
// the fields written without reading them first; `readFirst` holds the ones a
// compound operator reads before writing (`self.n += 1` on a field that is
// still `nil` faults), which therefore do not initialize the instance. A nested
// function or lambda is not entered: its body runs later, if at all, so an
// assignment there does not initialize the instance being constructed
// (PHASE 6.8.1).
struct SelfWrites {
  std::unordered_set<std::string> initialized;
  std::unordered_set<std::string> readFirst;
};

// `??=` initializes: it falls back to its right operand when the left is nil,
// so it tolerates an unset field. Every other compound reads the previous value
// first and needs one.
static bool initializesField(Tok op) { return op == Tok::Eq || op == Tok::QuestionQuestionEq; }

static void collectSelfWritesInto(const Stmt& stmt, SelfWrites& out) {
  if (const auto* exprStmt = std::get_if<ExprStmt>(&stmt.node)) {
    if (const auto* assign = std::get_if<AssignExpr>(&exprStmt->expr->node)) {
      if (const auto* member = std::get_if<MemberExpr>(&assign->target->node)) {
        if (const auto* self = std::get_if<VariableExpr>(&member->object->node)) {
          if (self->name == "self") {
            auto& set = initializesField(assign->op) ? out.initialized : out.readFirst;
            set.insert(member->name);
          }
        }
      }
    }
    return;
  }
  if (const auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const StmtPtr& child : block->stmts) collectSelfWritesInto(*child, out);
    return;
  }
  if (const auto* branch = std::get_if<IfStmt>(&stmt.node)) {
    collectSelfWritesInto(*branch->thenBranch, out);
    if (branch->elseBranch) collectSelfWritesInto(*branch->elseBranch, out);
    return;
  }
  if (const auto* loop = std::get_if<WhileStmt>(&stmt.node)) {
    collectSelfWritesInto(*loop->body, out);
    return;
  }
  if (const auto* loop = std::get_if<DoWhileStmt>(&stmt.node)) {
    collectSelfWritesInto(*loop->body, out);
    return;
  }
  if (const auto* loop = std::get_if<ForStmt>(&stmt.node)) {
    if (loop->init) collectSelfWritesInto(*loop->init, out);
    collectSelfWritesInto(*loop->body, out);
    return;
  }
  if (const auto* loop = std::get_if<ForInStmt>(&stmt.node)) {
    collectSelfWritesInto(*loop->body, out);
    return;
  }
  if (const auto* sw = std::get_if<SwitchStmt>(&stmt.node)) {
    for (const SwitchCase& branch : sw->cases) {
      if (branch.body) collectSelfWritesInto(*branch.body, out);
    }
    if (sw->defaultBody) collectSelfWritesInto(*sw->defaultBody, out);
    return;
  }
  if (const auto* deferred = std::get_if<DeferStmt>(&stmt.node)) {
    // A deferred block runs when the scope exits, which is still inside `init`.
    collectSelfWritesInto(*deferred->body, out);
  }
}

static SelfWrites collectSelfWrites(const Stmt& stmt) {
  SelfWrites out;
  collectSelfWritesInto(stmt, out);
  return out;
}

StmtPtr Parser::parseClassDecl() {
  const Token& keyword = previous();
  if (nestingDepth_ > 0) {
    // Like `struct`, a class is module-level metadata.
    throw CompileError{"'class' is only allowed at the top level of a module", keyword.line,
                       keyword.col};
  }
  const Token& name = consume(Tok::Ident, "expected a class name");
  rejectEnumNameAsBinding(name);
  if (declaredClasses_.count(name.lexeme) != 0) {
    throw CompileError{"class '" + name.lexeme + "' is already declared", name.line, name.col};
  }
  if (declaredStructs_.count(name.lexeme) != 0) {
    throw CompileError{"'" + name.lexeme + "' is already declared as a struct", name.line,
                       name.col};
  }
  consume(Tok::LBrace, "expected '{' to start the class body");

  struct Method {
    std::string name;
    std::vector<Param> params;
    TypeRef returnType;
    StmtPtr body;
    int line;
    int col;
    bool isAsync = false;
  };
  std::vector<StructField> fields;
  std::vector<Method> methods;
  std::unordered_set<std::string> seenFields;
  std::unordered_set<std::string> seenMethods;

  while (!check(Tok::RBrace)) {
    if (check(Tok::Eof)) error(peek(), "expected '}' to close the class body");
    if (match(Tok::Semicolon)) continue;  // stray separators are harmless
    bool isAsync = match(Tok::Async);
    if (isAsync) consume(Tok::Fn, "expected 'fn' after 'async'");
    if (isAsync || match(Tok::Fn)) {
      const Token& methodName = consume(Tok::Ident, "expected a method name");
      if (methodName.lexeme == "self") {
        error(methodName, "'self' is reserved inside a class");
      }
      if (isAsync && methodName.lexeme == "init") {
        error(methodName, "'init' cannot be async: the constructor calls it without awaiting");
      }
      if (!seenMethods.insert(methodName.lexeme).second) {
        throw CompileError{
            "duplicate method '" + methodName.lexeme + "' in class '" + name.lexeme + "'",
            methodName.line, methodName.col};
      }
      Method method;
      method.name = methodName.lexeme;
      method.line = methodName.line;
      method.col = methodName.col;
      method.isAsync = isAsync;
      method.params = parseParams();
      for (const Param& param : method.params) {
        if (param.name == "self") error(methodName, "'self' is reserved inside a class");
      }
      method.returnType = parseTypeAnnotation(/*allowVoid=*/true);
      if (check(Tok::Arrow)) {
        const Token& arrow = advance();
        const bool isBlock = check(Tok::LBrace);
        if (isAsync) {
          AsyncScope scope(asyncDepth_, asyncDepth_ + 1);
          method.body = parseFunctionBody(arrow);
        } else {
          method.body = parseFunctionBody(arrow);
        }
        if (isBlock) {
          match(Tok::Semicolon);
        } else {
          consume(Tok::Semicolon, "expected ';' after the method body");
        }
      } else {
        if (isAsync) {
          AsyncScope scope(asyncDepth_, asyncDepth_ + 1);
          method.body = parseBlock();
        } else {
          method.body = parseBlock();
        }
        match(Tok::Semicolon);
      }
      methods.push_back(std::move(method));
      continue;
    }
    const Token& field = consume(Tok::Ident, "expected a field or a method in the class body");
    if (field.lexeme == "self") error(field, "'self' is reserved inside a class");
    if (!seenFields.insert(field.lexeme).second) {
      throw CompileError{
          "duplicate field '" + field.lexeme + "' in class '" + name.lexeme + "'", field.line,
          field.col};
    }
    StructField entry;
    entry.name = field.lexeme;
    entry.line = field.line;
    entry.col = field.col;
    entry.type = parseTypeAnnotation();
    // `x: int = 5;` (PHASE 6.8.1): an optional default, so a field that never
    // changes does not need an `init` line. The parser only records it; the
    // desugaring below writes it into the instance map instead of `nil`.
    if (match(Tok::Eq)) entry.initializer = parseExpression();
    fields.push_back(std::move(entry));
    if (!match(Tok::Semicolon) && !check(Tok::RBrace)) {
      error(peek(), "expected ';' or '}' after the class field");
    }
  }
  consume(Tok::RBrace, "expected '}' to close the class body");
  match(Tok::Semicolon);

  // A field and a method cannot share a name: both are keys of the same map, so
  // one would silently overwrite the other.
  for (const Method& method : methods) {
    if (seenFields.count(method.name) != 0) {
      throw CompileError{"'" + method.name + "' is both a field and a method of class '" +
                             name.lexeme + "'",
                         method.line, method.col};
    }
  }

  // `init` defines the constructor: its parameters are the ones the caller
  // writes after the class name. A variadic `init` cannot be forwarded (the
  // constructor would pass the collected array as a single argument), so it is
  // rejected rather than silently mis-called.
  const Method* init = nullptr;
  for (const Method& method : methods) {
    if (method.name == "init") init = &method;
  }
  if (init != nullptr) {
    for (const Param& param : init->params) {
      if (param.variadic) {
        error(keyword, "'init' cannot take a variadic parameter: the constructor forwards its "
                       "arguments one by one");
      }
    }
    // A field with a non-nullable annotation and no default must be assigned by
    // `init`: the instance starts with `nil` there, so forgetting one leaves a
    // `nil` inside a slot the annotation says is never nil (PHASE 6.8.1). The
    // scan is syntactic — it accepts an assignment anywhere in `init`, including
    // one under a branch that may not run — so it catches the forgotten field,
    // not every path. A class without `init` is left alone: its fields start
    // `nil` by design (documented), and `init` is what opts into the contract.
    SelfWrites writes = collectSelfWrites(*init->body);
    for (const StructField& field : fields) {
      if (!field.type.isPresent() || field.type.nullable || field.type.isUnion()) continue;
      if (field.initializer) continue;
      if (writes.initialized.count(field.name) != 0) continue;
      const std::string tail = "'" + field.name + ": " + field.type.name +
                               " = ...') or assign 'self." + field.name + "' in 'init'";
      if (writes.readFirst.count(field.name) != 0) {
        throw CompileError{"field '" + field.name + "' of class '" + name.lexeme +
                               "' is declared '" + field.type.name +
                               "' but 'init' only updates it in place, and a compound "
                               "assignment reads the field first, so it is still nil; give it a "
                               "default (" + tail,
                           field.line, field.col};
      }
      throw CompileError{"field '" + field.name + "' of class '" + name.lexeme +
                             "' is declared '" + field.type.name +
                             "' but 'init' never assigns it; give it a default (" + tail,
                         field.line, field.col};
    }
  }

  std::vector<StmtPtr> body;
  // 1. `let self = { field: <default>, ... };`. A field with a declared default
  //    starts with it; the rest start `nil` and `init` fills them.
  {
    MapExpr map;
    for (const StructField& field : fields) {
      MapEntry entry;
      entry.literalKey = field.name;
      if (field.initializer) {
        entry.value = field.initializer;
      } else {
        entry.value = makeExpr(LiteralExpr{Value()}, keyword.line, keyword.col);
      }
      map.entries.push_back(std::move(entry));
    }
    LetStmt binding;
    binding.name = "self";
    binding.kind = Tok::Let;
    binding.init = makeExpr(std::move(map), keyword.line, keyword.col);
    body.push_back(makeStmt(std::move(binding), keyword.line, keyword.col));
  }
  // 2. `self.method = (params): ret => { ... };` for every method. The lambda
  //    captures `self`, which is why the receiver is explicit in the body.
  for (const Method& method : methods) {
    LambdaExpr lambda;
    lambda.params = method.params;
    lambda.body = method.body;
    lambda.name = name.lexeme + "." + method.name;
    lambda.returnType = method.returnType;
    lambda.ownerClass = name.lexeme;
    lambda.isAsync = method.isAsync;
    ExprPtr target = makeExpr(
        MemberExpr{makeExpr(VariableExpr{"self"}, method.line, method.col), method.name, false},
        method.line, method.col);
    AssignExpr assign;
    assign.target = std::move(target);
    assign.op = Tok::Eq;
    assign.value = makeExpr(std::move(lambda), method.line, method.col);
    assign.opLine = method.line;
    assign.opCol = method.col;
    body.push_back(
        makeStmt(ExprStmt{makeExpr(std::move(assign), method.line, method.col)}, method.line,
                 method.col));
  }
  // 3. `self.init(<constructor arguments>);`
  if (init != nullptr) {
    CallExpr call;
    call.callee = makeExpr(
        MemberExpr{makeExpr(VariableExpr{"self"}, keyword.line, keyword.col), "init", false},
        keyword.line, keyword.col);
    for (const Param& param : init->params) {
      call.args.push_back(makeExpr(VariableExpr{param.name}, keyword.line, keyword.col));
    }
    body.push_back(makeStmt(ExprStmt{makeExpr(std::move(call), keyword.line, keyword.col)},
                            keyword.line, keyword.col));
  }
  // 4. `return self;`
  body.push_back(makeStmt(
      ReturnStmt{makeExpr(VariableExpr{"self"}, keyword.line, keyword.col)}, keyword.line,
      keyword.col));

  FnDeclStmt constructor;
  constructor.name = name.lexeme;
  if (init != nullptr) constructor.params = init->params;
  // The constructor's return type is left open: the instance is a map at run
  // time, so a declared `: Point` would ask the type checker to equate the two.
  // The class name stays a valid *annotation* (the parser accepts it), which is
  // what `let p: Point = Point(3, 4)` needs.
  constructor.body = makeStmt(BlockStmt{std::move(body)}, keyword.line, keyword.col);
  constructor.isClassConstructor = true;
  constructor.classFields = fields;
  // The method signatures ride along so the compiler can reject an assignment
  // that would replace a method with a value of the wrong shape (PHASE 6.8.1).
  for (const Method& method : methods) {
    ClassMethodSig sig;
    sig.name = method.name;
    sig.params = method.params;
    sig.returnType = method.returnType;
    sig.isAsync = method.isAsync;
    constructor.classMethods.push_back(std::move(sig));
  }

  classNames_.insert(name.lexeme);
  declaredClasses_.insert(name.lexeme);
  return makeStmt(std::move(constructor), keyword.line, keyword.col);
}

bool Parser::atLambda() const {
  if (check(Tok::Ident)) return peek(1).type == Tok::Arrow;
  if (!check(Tok::LParen)) return false;
  // `(` [ident [':' type] {, ident [':' type]} [,]] `)` `=>`. Parameters are
  // plain identifiers (with an optional type annotation), so this cannot be
  // confused with a parenthesized expression.
  size_t i = 1;
  while (peek(i).type == Tok::Ident) {
    ++i;
    if (peek(i).type == Tok::Colon) {
      // Skip `: type` (and its optional trailing `?`).
      ++i;
      if (peek(i).type != Tok::Ident) return false;
      ++i;
      if (peek(i).type == Tok::Question) ++i;
    }
    // A trailing `...` marks a variadic parameter; it must end the list.
    if (peek(i).type == Tok::Ellipsis) {
      ++i;
      break;
    }
    if (peek(i).type != Tok::Comma) break;
    ++i;
  }
  return peek(i).type == Tok::RParen && peek(i + 1).type == Tok::Arrow;
}

ExprPtr Parser::parseLambda() {
  const Token& first = peek();
  std::vector<Param> params;
  if (check(Tok::Ident)) {
    Param param;
    param.name = advance().lexeme;
    param.type = parseTypeAnnotation();
    params.push_back(std::move(param));
  } else {
    params = parseParams();
  }
  const Token& arrow = consume(Tok::Arrow, "expected '=>' after the lambda parameters");
  StmtPtr body;
  {
    // A lambda is not an async function: `await` cannot cross its boundary,
    // even when the lambda is written inside an async function body.
    AsyncScope scope(asyncDepth_, 0);
    body = parseFunctionBody(arrow);
  }
  LambdaExpr lambda;
  lambda.params = std::move(params);
  lambda.body = std::move(body);
  return makeExpr(std::move(lambda), first.line, first.col);
}

std::vector<Param> Parser::parseParams(bool allowBareEllipsis, bool* outCVariadic) {
  consume(Tok::LParen, "expected '(' to start the parameter list");
  std::vector<Param> params;
  if (outCVariadic != nullptr) *outCVariadic = false;
  if (!check(Tok::RParen)) {
    while (true) {
      if (check(Tok::RParen)) break;
      // A bare `...` is the C variadic tail of an `extern "C"` declaration
      // (`fn printf(fmt: cstring, ...): int`). It is only accepted where the
      // caller allows it, and it must be last. A Oskel `rest...` names the
      // collected array, so the two spellings never collide.
      if (allowBareEllipsis && check(Tok::Ellipsis)) {
        advance();
        if (outCVariadic != nullptr) *outCVariadic = true;
        match(Tok::Comma);
        if (!check(Tok::RParen)) {
          error(peek(), "'...' must be the last parameter of an 'extern' declaration");
        }
        break;
      }
      const Token& param = consume(Tok::Ident, "expected a parameter name");
      rejectEnumNameAsBinding(param);
      Param parsed;
      parsed.name = param.lexeme;
      // `rest...` collects the extra arguments into an array. The `...` comes
      // after the optional type annotation (`rest...` or `parts: string...`)
      // and must be the last parameter, so anything after it is rejected.
      if (match(Tok::Ellipsis)) {
        parsed.variadic = true;
      } else {
        parsed.type = parseTypeAnnotation();
        if (match(Tok::Ellipsis)) parsed.variadic = true;
      }
      params.push_back(std::move(parsed));
      if (parsed.variadic) {
        // A trailing comma is allowed after the last parameter (including the
        // variadic one), so `fn f(a, rest...,)` parses. Anything else after the
        // variadic is still an error: it must be the last parameter.
        match(Tok::Comma);
        if (!check(Tok::RParen)) {
          error(peek(), "a variadic parameter must be the last one");
        }
        break;
      }
      if (!match(Tok::Comma)) break;
    }
  }
  consume(Tok::RParen, "expected ')' after the parameter list");
  return params;
}

StmtPtr Parser::parseIf() {
  const Token& keyword = previous();
  consume(Tok::LParen, "expected '(' after 'if'");
  auto condition = parseExpression();
  consume(Tok::RParen, "expected ')' after the condition");
  auto thenBranch = parseBody();
  StmtPtr elseBranch;
  if (match(Tok::Else)) {
    if (match(Tok::If)) {
      elseBranch = parseIf();
    } else {
      elseBranch = parseBody();
    }
  }
  match(Tok::Semicolon);
  return makeStmt(IfStmt{std::move(condition), std::move(thenBranch), std::move(elseBranch)},
                  keyword.line, keyword.col);
}

StmtPtr Parser::parseWhile() {
  const Token& keyword = previous();
  consume(Tok::LParen, "expected '(' after 'while'");
  auto condition = parseExpression();
  consume(Tok::RParen, "expected ')' after the condition");
  auto body = parseBody();
  match(Tok::Semicolon);
  return makeStmt(WhileStmt{std::move(condition), std::move(body)}, keyword.line, keyword.col);
}

// `do { ... } while (cond);` (PHASE 6.5): a post-test loop. The body runs
// before the condition is tested, so it always runs at least once. The `while`
// and the condition are required; the trailing `;` is optional.
StmtPtr Parser::parseDoWhile() {
  const Token& keyword = previous();
  auto body = parseBlock();
  consume(Tok::While, "expected 'while' after the 'do' body");
  consume(Tok::LParen, "expected '(' after 'while'");
  auto condition = parseExpression();
  consume(Tok::RParen, "expected ')' after the condition");
  match(Tok::Semicolon);
  return makeStmt(DoWhileStmt{std::move(body), std::move(condition)}, keyword.line, keyword.col);
}

StmtPtr Parser::parseFor() {
  const Token& keyword = previous();

  // `for-in` accepts an optional pair of parentheses around the loop variable
  // and the iterable: `for x in xs` and `for (x in xs)` are the same. The
  // parenthesized form is told apart from a classic `for` by the lookahead
  // `( IDENT in` (or `( IDENT , IDENT in` for the key/value form), which a
  // classic header (`( init; cond; step )`) never starts with: an initializer
  // is a declaration, an assignment or an expression, none of which is a bare
  // identifier followed by `in`.
  const bool parenthesizedForIn =
      check(Tok::LParen) && peek(1).type == Tok::Ident &&
      (peek(2).type == Tok::In ||
       (peek(2).type == Tok::Comma && peek(3).type == Tok::Ident && peek(4).type == Tok::In));

  if (parenthesizedForIn || !check(Tok::LParen)) {
    if (parenthesizedForIn) consume(Tok::LParen, "expected '(' after 'for'");
    const Token& var = consume(Tok::Ident, "expected a loop variable or '(' after 'for'");
    rejectEnumNameAsBinding(var);
    // `for k, v in map`: an optional second identifier binds the value of each
    // pair, so the loop walks a map as key/value entries. Only meaningful for
    // maps; the compiler reports a clear error for the other iterables.
    std::string valueVar;
    if (match(Tok::Comma)) {
      const Token& valueName = consume(Tok::Ident, "expected a second loop variable after ','");
      rejectEnumNameAsBinding(valueName);
      valueVar = valueName.lexeme;
    }
    consume(Tok::In, "expected 'in' in the for-in loop");
    auto iterable = parseExpression();
    if (parenthesizedForIn) consume(Tok::RParen, "expected ')' after the for-in iterable");
    auto body = parseBody();
    match(Tok::Semicolon);
    return makeStmt(ForInStmt{var.lexeme, std::move(valueVar), std::move(iterable), std::move(body)},
                    keyword.line, keyword.col);
  }

  consume(Tok::LParen, "expected '(' after 'for'");
  StmtPtr init;
  if (match(Tok::Var) || match(Tok::Let) || match(Tok::Const)) {
    init = parseVarDecl();  // consumes the ';'
  } else if (match(Tok::Semicolon)) {
    init = nullptr;
  } else {
    const Token& start = peek();
    auto expr = parseExpression();
    consume(Tok::Semicolon, "expected ';' after the for-loop initializer");
    init = makeStmt(ExprStmt{std::move(expr)}, start.line, start.col);
  }

  ExprPtr condition;
  if (!check(Tok::Semicolon)) condition = parseExpression();
  consume(Tok::Semicolon, "expected ';' after the for-loop condition");

  ExprPtr step;
  if (!check(Tok::RParen)) step = parseExpression();
  consume(Tok::RParen, "expected ')' after the for-loop clauses");

  auto body = parseBody();
  match(Tok::Semicolon);
  return makeStmt(ForStmt{std::move(init), std::move(condition), std::move(step), std::move(body)},
                  keyword.line, keyword.col);
}

StmtPtr Parser::parseReturn() {
  const Token& keyword = previous();
  ExprPtr value;
  if (!check(Tok::Semicolon)) {
    value = parseExpression();
  }
  consume(Tok::Semicolon, "expected ';' after 'return'");
  return makeStmt(ReturnStmt{std::move(value)}, keyword.line, keyword.col);
}

// `defer <block>`. The body must be a `{ ... }` block: a bare expression would
// be evaluated where it appears instead of at scope exit, which is not what
// `defer` means. A trailing `;` after the block is accepted, like other
// statement-position blocks.
StmtPtr Parser::parseDefer() {
  const Token& keyword = previous();
  if (!check(Tok::LBrace)) {
    error(peek(), "expected '{' after 'defer'");
  }
  StmtPtr body = parseBlock();
  match(Tok::Semicolon);
  return makeStmt(DeferStmt{std::move(body)}, keyword.line, keyword.col);
}

StmtPtr Parser::parseBody() {
  if (check(Tok::LBrace)) return parseBlock();
  // Brace-less body: exactly one statement. Wrapping it in a block keeps the
  // AST, the compiler and the bytecode identical to the brace form, so the
  // single statement is a scope of its own (a `let` declared in it does not
  // leak) and a `defer` in it runs when the body exits.
  const Token& start = peek();
  auto statement = parseStatement();
  std::vector<StmtPtr> statements;
  statements.push_back(std::move(statement));
  return makeStmt(BlockStmt{std::move(statements)}, start.line, start.col);
}

StmtPtr Parser::parseBlock() {
  DepthGuard guard(*this);
  // A block is never the program's top level, even when it is a function body
  // parsed while the program flag is set: a `const` declared here is local, so
  // its initializer must be folded now (and a non-constant one is rejected
  // here, not left for `resolveTopLevelConsts`, which never sees it).
  TopLevelScope nested(atTopLevel_, false);
  const Token& open = consume(Tok::LBrace, "expected '{'");
  ++nestingDepth_;
  std::vector<StmtPtr> statements;
  while (!check(Tok::RBrace)) {
    if (check(Tok::Eof)) error(peek(), "expected '}' to close the block");
    if (match(Tok::Semicolon)) continue;
    statements.push_back(parseStatement());
  }
  --nestingDepth_;
  consume(Tok::RBrace, "expected '}' to close the block");
  return makeStmt(BlockStmt{std::move(statements)}, open.line, open.col);
}

// Parses a Java-style import declaration. The grammar is:
//
//   import <dotted.path>;                 // qualified access
//   import <dotted.path>.*;               // wildcard: names enter the scope
//   import <dotted.path> as <alias>;      // qualified access under `alias`
//   import { a, b } from <dotted.path>;   // selective: only those names
//
// `import` is a top-level declaration: a nested import (inside a block or a
// function body) is a compile error, since the imported names must be visible
// to the whole module.
StmtPtr Parser::parseImport() {
  const Token& keyword = previous();
  if (nestingDepth_ > 0) {
    // Thrown directly, not through `error`, so the message matches the other
    // "only allowed here" diagnostics without the trailing ", found ...".
    throw CompileError{"'import' is only allowed at the top level of a module", keyword.line,
                       keyword.col};
  }

  ImportStmt stmt;

  if (match(Tok::LBrace)) {
    // Selective: `import { sqrt, pow } from std.math;`
    stmt.mode = ImportStmt::Mode::Selective;
    if (!check(Tok::RBrace)) {
      while (true) {
        if (check(Tok::RBrace)) break;
        const Token& name = consume(Tok::Ident, "expected an imported name");
        // An imported enum is a name the import introduces, not a binding that
        // shadows one; a local enum of the same name still wins and is rejected.
        if (importedEnumNames_.count(name.lexeme) == 0) rejectEnumNameAsBinding(name);
        stmt.names.push_back(name.lexeme);
        if (!match(Tok::Comma)) break;
      }
    }
    consume(Tok::RBrace, "expected '}' to close the import list");
    if (!check(Tok::Ident) || peek().lexeme != "from") {
      error(peek(), "expected 'from' after the imported names");
    }
    advance();
    stmt.module = parseImportPath();
    consume(Tok::Semicolon, "expected ';' after the import declaration");
    return makeStmt(std::move(stmt), keyword.line, keyword.col);
  }

  stmt.module = parseImportPath();

  if (match(Tok::Dot)) {
    // Wildcard: the only valid continuation after the dot is `*`.
    consume(Tok::Star, "expected '*' after '.' in a wildcard import");
    stmt.mode = ImportStmt::Mode::Wildcard;
  } else if (check(Tok::Ident) && peek().lexeme == "as") {
    advance();
    stmt.mode = ImportStmt::Mode::Qualified;
    const Token& alias = consume(Tok::Ident, "expected an alias name after 'as'");
    rejectEnumNameAsBinding(alias);
    stmt.alias = alias.lexeme;
  } else {
    stmt.mode = ImportStmt::Mode::Qualified;
  }

  consume(Tok::Semicolon, "expected ';' after the import declaration");
  return makeStmt(std::move(stmt), keyword.line, keyword.col);
}

std::string Parser::parseImportPath() {
  const Token& first = consume(Tok::Ident, "expected a module name");
  std::string path = first.lexeme;
  while (check(Tok::Dot)) {
    // A dot only continues the path when an identifier follows; otherwise the
    // dot belongs to the caller (the wildcard `.*` case).
    if (!peek(1).is(Tok::Ident)) break;
    advance();  // '.'
    path += '.';
    path += advance().lexeme;
  }
  return path;
}

// Parses a `switch` statement:
//
//   switch (subject) {
//     case 1: { ... }
//     case 2: case 3: { ... }
//     case 10..=20: { ... }
//     default: { ... }
//   }
//
// A case label is a literal (nil, bool, number, string) or an integer range.
// There is no fallthrough: the body is a block and only the matching case runs.
StmtPtr Parser::parseSwitch() {
  const Token& keyword = previous();
  consume(Tok::LParen, "expected '(' after 'switch'");
  auto subject = parseExpression();
  consume(Tok::RParen, "expected ')' after the switch subject");
  consume(Tok::LBrace, "expected '{' to start the switch body");

  SwitchStmt stmt;
  stmt.subject = std::move(subject);
  bool seenDefault = false;

  while (!check(Tok::RBrace)) {
    if (check(Tok::Eof)) error(peek(), "expected '}' to close the switch body");
    // A stray ';' between clauses is harmless, as inside a block. This also
    // keeps a multi-line REPL entry working: the REPL retries a program by
    // appending ';', which can land right after a case body.
    if (match(Tok::Semicolon)) continue;

    if (match(Tok::Default)) {
      if (seenDefault) error(previous(), "duplicate 'default' clause");
      seenDefault = true;
      consume(Tok::Colon, "expected ':' after 'default'");
      stmt.defaultBody = parseBlock();
      match(Tok::Semicolon);
      continue;
    }

    if (!match(Tok::Case)) error(peek(), "expected 'case' or 'default' in the switch body");
    SwitchCase entry;
    // One or more labels may share a body: `case 2: case 3: { ... }`.
    while (true) {
      auto label = parseExpression();
      // A case label is a fixed value: a literal (optionally signed), an
      // integer range, a bare name the compiler resolves to an immutable
      // `const`/`let` binding or a unique enum member, or `Enum.MEMBER`
      // (PHASE 6.9). A call or any other expression is not a fixed value and
      // is rejected here; the compiler rejects a name that is not fixed.
      bool enumMember = false;
      if (const auto* member = std::get_if<MemberExpr>(&label->node)) {
        if (!member->safe) {
          // `Color.RED`, and the qualified `geom.Color.RED`: the object chain
          // names the enum. The compiler resolves the value and rejects a
          // member of the wrong enum.
          std::string objectName;
          if (flattenMemberPath(*member->object, objectName)) {
            enumMember = enumNames_.count(objectName) != 0;
          }
        }
      }
      if (!isCaseLabel(label) && !std::holds_alternative<VariableExpr>(label->node) &&
          !enumMember) {
        throw CompileError{"a 'case' label must be a literal, a range, a 'const' name or an enum member",
                           label->line, label->col};
      }
      entry.values.push_back(std::move(label));
      consume(Tok::Colon, "expected ':' after the case label");
      if (!check(Tok::Case)) break;
      advance();  // another label for the same body
    }
    entry.body = parseBlock();
    match(Tok::Semicolon);
    stmt.cases.push_back(std::move(entry));
  }

  consume(Tok::RBrace, "expected '}' to close the switch body");
  match(Tok::Semicolon);
  return makeStmt(std::move(stmt), keyword.line, keyword.col);
}

StmtPtr Parser::parseExpressionStatement() {
  const Token& start = peek();
  auto expr = parseExpression();
  consume(Tok::Semicolon, "expected ';' after expression");
  return makeStmt(ExprStmt{std::move(expr)}, start.line, start.col);
}

// --------------------------------------------------------------- expressions

ExprPtr Parser::parseExpression() { return parseAssignment(); }

ExprPtr Parser::parseAssignment() {
  DepthGuard guard(*this);
  auto target = parseTernary();
  if (isAssignOp(peek().type)) {
    const Token& op = advance();
    if (!isAssignable(target)) error(op, "invalid assignment target");
    auto value = parseAssignment();
    return makeExpr(AssignExpr{std::move(target), op.type, std::move(value), op.line, op.col},
                    op.line, op.col);
  }
  return target;
}

ExprPtr Parser::parseTernary() {
  const Token& start = peek();
  auto condition = parseCatch();
  if (match(Tok::Question)) {
    auto thenExpr = parseAssignment();
    consume(Tok::Colon, "expected ':' in ternary expression");
    auto elseExpr = parseAssignment();
    return makeExpr(TernaryExpr{std::move(condition), std::move(thenExpr), std::move(elseExpr)},
                    start.line, start.col);
  }
  return condition;
}

// `expr catch default` (PHASE 6.7.3). Binds looser than `??` so
// `a ?? b catch c` recovers from the result of the coalescing, and
// left-associatively so `a() catch b() catch c` recovers at the first
// failure. The fallback is a full `??`-level expression, so a chained
// `catch` groups to the left.
ExprPtr Parser::parseCatch() {
  auto lhs = parseNullCoalesce();
  while (match(Tok::Catch)) {
    const Token& op = previous();
    auto rhs = parseNullCoalesce();
    lhs = makeExpr(CatchExpr{std::move(lhs), std::move(rhs)}, op.line, op.col);
  }
  return lhs;
}

// `??` (null coalescing, PHASE 6.5). Binds looser than `||` so `a || b ?? c`
// groups as `(a || b) ?? c`, and left-associatively so `a ?? b ?? c` is
// `(a ?? b) ?? c`. It is compiled as a short-circuiting binary, exactly like
// `&&`/`||`, so the right-hand side is evaluated only when the left is nil.
ExprPtr Parser::parseNullCoalesce() {
  auto lhs = parseOr();
  while (match(Tok::QuestionQuestion)) {
    const Token& op = previous();
    auto rhs = parseOr();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseOr() {
  auto lhs = parseAnd();
  while (match(Tok::PipePipe)) {
    const Token& op = previous();
    auto rhs = parseAnd();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseAnd() {
  auto lhs = parseBitOr();
  while (match(Tok::AmpAmp)) {
    const Token& op = previous();
    auto rhs = parseBitOr();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseBitOr() {
  auto lhs = parseBitXor();
  while (match(Tok::Pipe)) {
    const Token& op = previous();
    auto rhs = parseBitXor();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseBitXor() {
  auto lhs = parseBitAnd();
  while (match(Tok::Caret)) {
    const Token& op = previous();
    auto rhs = parseBitAnd();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseBitAnd() {
  auto lhs = parseEquality();
  while (match(Tok::Amp)) {
    const Token& op = previous();
    auto rhs = parseEquality();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseEquality() {
  auto lhs = parseComparison();
  while (check(Tok::EqEq) || check(Tok::BangEq)) {
    const Token& op = advance();
    auto rhs = parseComparison();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseComparison() {
  auto lhs = parseShift();
  while (check(Tok::Lt) || check(Tok::LtEq) || check(Tok::Gt) || check(Tok::GtEq) ||
         check(Tok::In)) {
    const Token& op = advance();
    auto rhs = parseShift();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseShift() {
  auto lhs = parseRange();
  while (check(Tok::Shl) || check(Tok::Shr)) {
    const Token& op = advance();
    auto rhs = parseRange();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseRange() {
  const Token& start = peek();
  auto lhs = parseAdditive();
  if (check(Tok::DotDot) || check(Tok::DotDotEq)) {
    const Token& op = advance();
    auto rhs = parseAdditive();
    if (check(Tok::DotDot) || check(Tok::DotDotEq)) {
      error(peek(), "range bounds cannot be ranges (use a variable for a chained range)");
    }
    return makeExpr(RangeExpr{std::move(lhs), std::move(rhs), op.type == Tok::DotDotEq}, start.line,
                    start.col);
  }
  return lhs;
}

ExprPtr Parser::parseAdditive() {
  auto lhs = parseMultiplicative();
  while (check(Tok::Plus) || check(Tok::Minus)) {
    const Token& op = advance();
    auto rhs = parseMultiplicative();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseMultiplicative() {
  auto lhs = parseUnary();
  while (check(Tok::Star) || check(Tok::Slash) || check(Tok::Percent)) {
    const Token& op = advance();
    auto rhs = parseUnary();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parsePower() {
  auto lhs = parsePostfix();
  if (match(Tok::StarStar)) {
    const Token& op = previous();
    auto rhs = parseUnary();
    return makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                    op.col);
  }
  return lhs;
}

ExprPtr Parser::parseUnary() {
  DepthGuard guard(*this);
  // `await expr` (PHASE 11.1): unwrap a `Future`. It binds like the other
  // prefix operators, so `await f().x` awaits the call and then reads `.x`.
  // The parser rejects it outside an `async fn`; a lambda resets the depth.
  if (check(Tok::Await)) {
    const Token& op = advance();
    if (asyncDepth_ == 0) {
      throw CompileError{"'await' is only allowed inside an 'async fn'", op.line, op.col};
    }
    auto operand = parseUnary();
    return makeExpr(AwaitExpr{std::move(operand)}, op.line, op.col);
  }
  // `try expr` (PHASE 6.7.3): prefix propagation. It binds like the other
  // prefix operators (`-`, `!`, `~`), so it applies to the whole postfix
  // operand: `try f().x` propagates the result of the member access.
  if (check(Tok::Try)) {
    const Token& op = advance();
    auto operand = parseUnary();
    return makeExpr(TryExpr{std::move(operand)}, op.line, op.col);
  }
  if (check(Tok::Bang) || check(Tok::Minus) || check(Tok::Plus) || check(Tok::Tilde)) {
    const Token& op = advance();
    // `-9223372036854775808` is the smallest `int`, but its magnitude (2^63)
    // has no positive 64-bit representation. The lexer flags such a literal,
    // and a unary minus is the one place it is valid: fold the sign into the
    // literal here so no wrapping negation is emitted. `**` and the postfix
    // operators bind tighter than unary minus, so `-2^63 ** 2` is
    // `-(2^63 ** 2)`: there the literal is not directly under the minus and the
    // ordinary range check must fire.
    if (op.type == Tok::Minus && check(Tok::Int) && peek().intNeedsNegation &&
        !bindsTighterThanUnary(peek(1).type)) {
      const Token& number = advance();
      return makeExpr(LiteralExpr{Value(number.intValue)}, op.line, op.col);
    }
    auto operand = parseUnary();
    return makeExpr(UnaryExpr{op.type, std::move(operand)}, op.line, op.col);
  }
  return parsePower();
}

ExprPtr Parser::parsePostfix() {
  auto expr = parsePrimary();
  while (true) {
    if (match(Tok::LParen)) {
      const Token& open = previous();
      std::vector<ExprPtr> args;
      if (!check(Tok::RParen)) {
        while (true) {
          if (check(Tok::RParen)) break;
          args.push_back(parseExpression());
          if (!match(Tok::Comma)) break;
        }
      }
      consume(Tok::RParen, "expected ')' after the arguments");
      expr = makeExpr(CallExpr{std::move(expr), std::move(args)}, open.line, open.col);
    } else if (match(Tok::LBracket)) {
      const Token& open = previous();
      // `obj[start:end:step]` (slicing): a ':' inside the brackets starts a
      // slice. `obj[:end]` has no start, `obj[start:]` has no end, and the
      // `step` after a second ':' may be omitted too (`obj[::2]`).
      ExprPtr first = check(Tok::Colon) ? nullptr : parseExpression();
      if (match(Tok::Colon)) {
        ExprPtr end;
        if (!check(Tok::RBracket) && !check(Tok::Colon)) end = parseExpression();
        ExprPtr step;
        if (match(Tok::Colon) && !check(Tok::RBracket)) step = parseExpression();
        consume(Tok::RBracket, "expected ']' after the slice");
        expr = makeExpr(
            SliceExpr{std::move(expr), std::move(first), std::move(end), std::move(step)},
            open.line, open.col);
      } else {
        consume(Tok::RBracket, "expected ']' after the index");
        expr = makeExpr(IndexExpr{std::move(expr), std::move(first)}, open.line, open.col);
      }
    } else if (match(Tok::Dot)) {
      const Token& dot = previous();
      const Token& name = consume(Tok::Ident, "expected a member name after '.'");
      expr = makeExpr(MemberExpr{std::move(expr), name.lexeme}, dot.line, dot.col);
    } else if (match(Tok::QuestionDot)) {
      // `a?.b` (PHASE 6.5): safe member access; yields nil when `a` is nil.
      const Token& dot = previous();
      const Token& name = consume(Tok::Ident, "expected a member name after '?.'");
      expr = makeExpr(MemberExpr{std::move(expr), name.lexeme, true}, dot.line, dot.col);
    } else if (match(Tok::BangBang)) {
      // `a!!` (PHASE 6.5): non-null assertion; raises at runtime if `a` is nil.
      const Token& op = previous();
      expr = makeExpr(NonNullExpr{std::move(expr)}, op.line, op.col);
    } else {
      break;
    }
  }
  return expr;
}

ExprPtr Parser::parsePrimary() {
  const Token& token = peek();

  if (match(Tok::Int)) {
    if (token.intNeedsNegation) {
      throw CompileError{
          "integer literal is out of range; at this magnitude only "
          "'-9223372036854775808' fits in an int",
          token.line, token.col};
    }
    return makeExpr(LiteralExpr{Value(token.intValue)}, token.line, token.col);
  }
  if (match(Tok::Double)) {
    return makeExpr(LiteralExpr{Value(token.floatValue)}, token.line, token.col);
  }
  if (match(Tok::Float)) {
    return makeExpr(LiteralExpr{Value::fromFloat(static_cast<float>(token.floatValue))},
                    token.line, token.col);
  }
  if (match(Tok::Str)) return buildString(token);
  if (match(Tok::True)) return makeExpr(LiteralExpr{Value(true)}, token.line, token.col);
  if (match(Tok::False)) return makeExpr(LiteralExpr{Value(false)}, token.line, token.col);
  if (match(Tok::Nil)) return makeExpr(LiteralExpr{Value()}, token.line, token.col);
  if (atLambda()) return parseLambda();
  if (match(Tok::Ident)) {
    // An enum name is a namespace, not a value: `Color` alone is meaningless,
    // only `Color.MEMBER` (or a bare member) names an integer.
    if (enumNames_.count(token.lexeme) != 0 && !check(Tok::Dot)) {
      throw CompileError{"enum '" + token.lexeme + "' is not a value; use '" + token.lexeme +
                             ".MEMBER'",
                         token.line, token.col};
    }
    // `Point { x: 1, y: 2 }` (PHASE 6.5): an identifier that names a declared
    // struct, immediately followed by `{`, is a struct literal. Gating on the
    // declared name keeps `for x in name { ... }` unambiguous: `name` is not a
    // struct, so it stays a plain variable and the `{` opens the loop body.
    if (structNames_.count(token.lexeme) != 0 && check(Tok::LBrace)) {
      return parseStructLiteralBody(token.lexeme, token.line, token.col);
    }
    return makeExpr(VariableExpr{token.lexeme}, token.line, token.col);
  }
  if (match(Tok::LParen)) {
    auto expr = parseExpression();
    consume(Tok::RParen, "expected ')' after the expression");
    return expr;
  }
  if (match(Tok::LBracket)) {
    std::vector<ExprPtr> elements;
    if (!check(Tok::RBracket)) {
      while (true) {
        if (check(Tok::RBracket)) break;
        elements.push_back(parseExpression());
        if (!match(Tok::Comma)) break;
      }
    }
    consume(Tok::RBracket, "expected ']' to close the array literal");
    return makeExpr(ArrayExpr{std::move(elements)}, token.line, token.col);
  }
  if (match(Tok::LBrace)) {
    std::vector<MapEntry> entries;
    if (!check(Tok::RBrace)) {
      while (true) {
        if (check(Tok::RBrace)) break;
        // Key forms: identifier, string literal, numeric/bool/nil literal
        // (stored as literalKey), or `[expr]` for computed keys (keyExpr).
        std::string key;
        ExprPtr keyExpr;
        if (check(Tok::Ident)) {
          key = advance().lexeme;
        } else if (check(Tok::Str)) {
          const Token& keyToken = advance();
          if (keyToken.parts.size() != 1 || keyToken.parts[0].isExpr) {
            error(keyToken, "map keys must be plain strings or identifiers");
          }
          key = keyToken.parts[0].text;
        } else if (check(Tok::Int)) {
          const Token& keyToken = advance();
          if (keyToken.intNeedsNegation) {
            throw CompileError{"integer literal is out of range", keyToken.line, keyToken.col};
          }
          key = std::to_string(keyToken.intValue);
        } else if (check(Tok::Double)) {
          key = Value(advance().floatValue).toString();
        } else if (check(Tok::Float)) {
          key = Value::fromFloat(static_cast<float>(advance().floatValue)).toString();
        } else if (check(Tok::True)) {
          advance();
          key = "true";
        } else if (check(Tok::False)) {
          advance();
          key = "false";
        } else if (check(Tok::Nil)) {
          advance();
          key = "nil";
        } else if (check(Tok::LBracket)) {
          advance();
          keyExpr = parseExpression();
          consume(Tok::RBracket, "expected ']' after the computed map key");
        } else {
          error(peek(), "expected a string, identifier, scalar literal or [expr] map key");
        }
        consume(Tok::Colon, "expected ':' after the map key");
        entries.push_back(MapEntry{std::move(key), std::move(keyExpr), parseExpression()});
        if (!match(Tok::Comma)) break;
      }
    }
    consume(Tok::RBrace, "expected '}' to close the map literal");
    return makeExpr(MapExpr{std::move(entries)}, token.line, token.col);
  }
  if (check(Tok::Fn)) {
    error(peek(),
          "'fn' only declares named functions; write a lambda as 'x => expr' or "
          "'(a, b) => expr'");
  }

  error(token, "expected an expression");
}

// Body of a struct literal, the `{ ... }` after a struct type name (the name
// is already consumed). Field values are full expressions; keys must be
// identifiers (the field names). Duplicate keys are rejected here so the
// compiler can rely on them being unique.
ExprPtr Parser::parseStructLiteralBody(const std::string& typeName, int line, int col) {
  consume(Tok::LBrace, "expected '{' after the struct name");
  std::vector<StructLitEntry> entries;
  std::unordered_set<std::string> seen;
  if (!check(Tok::RBrace)) {
    while (true) {
      if (check(Tok::RBrace)) break;
      const Token& field = consume(Tok::Ident, "expected a field name in the struct literal");
      if (!seen.insert(field.lexeme).second) {
        throw CompileError{"duplicate field '" + field.lexeme + "' in the struct literal",
                           field.line, field.col};
      }
      // Field shorthand: `Point { x, y }` is sugar for `Point { x: x, y: y }`,
      // so the field's value is the variable of the same name in scope. The
      // colon form and the shorthand may mix freely (`Point { x, y: 2 }`).
      if (match(Tok::Colon)) {
        entries.push_back(StructLitEntry{field.lexeme, parseExpression()});
      } else {
        entries.push_back(StructLitEntry{
            field.lexeme, makeExpr(VariableExpr{field.lexeme}, field.line, field.col)});
      }
      if (!match(Tok::Comma)) break;
    }
  }
  consume(Tok::RBrace, "expected '}' to close the struct literal");
  return makeExpr(StructLitExpr{typeName, std::move(entries)}, line, col);
}

ExprPtr Parser::buildString(const Token& token) {
  if (token.parts.size() == 1 && !token.parts[0].isExpr) {
    return makeExpr(LiteralExpr{Value(token.parts[0].text)}, token.line, token.col);
  }

  InterpExpr node;
  for (const auto& part : token.parts) {
    InterpPart piece;
    piece.line = part.line;
    piece.col = part.col;
    if (part.isExpr) {
      try {
        auto tokens = lex(part.text, part.line, part.col);
        Parser sub(std::move(tokens), where_);
        // Carry the current depth over: a nested interpolation (`"${"${...}"}"`)
        // recurses through a fresh sub-parser per level, so without this the
        // guard would reset to zero at every level and never fire.
        sub.depth_ = depth_;
        piece.content = sub.parseFullExpression();
      } catch (const CompileError& err) {
        throw CompileError{"in string interpolation: " + err.message, err.line, err.col};
      }
    } else {
      piece.content = part.text;
    }
    node.parts.push_back(std::move(piece));
  }
  return makeExpr(std::move(node), token.line, token.col);
}
