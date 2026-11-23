#include "oskelint.h"

#include <memory>
#include <utility>
#include <variant>

#include "frontend/ast.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "vm.h"

namespace oskelint {
namespace {

// Walks a parsed program and reports a declaration whose name is a builtin.
//
// Only *declarations* are reported: a `LetStmt`, a `FnDeclStmt`, a
// `StructDeclStmt` and a parameter all introduce a name. A `for ... in` loop
// variable and a `catch` binding are declarations too, and they are the easiest
// way to shadow a builtin by accident (`for len in xs`).
//
// The re-export idiom (`let time = time;`) reads the builtin in its own
// initializer. That is not a shadow -- the builtin is being surfaced, not
// hidden -- so it is skipped: the initializer is a `VariableExpr` with the same
// name as the declaration.
class ShadowWalker {
 public:
  ShadowWalker(const std::unordered_set<std::string>& builtins, std::vector<Diagnostic>& out)
      : builtins_(builtins), out_(out) {}

  void walkProgram(const StmtPtr& program) { walkStmt(program); }

 private:
  void report(const std::string& name, int line, int col) {
    if (builtins_.find(name) == builtins_.end()) return;
    Diagnostic diagnostic;
    diagnostic.rule = "builtin-shadow";
    diagnostic.message = "'" + name + "' shadows the builtin '" + name + "'";
    diagnostic.line = line;
    diagnostic.col = col;
    out_.push_back(std::move(diagnostic));
  }

  // A declaration is the re-export idiom when its initializer is exactly a read
  // of the same name. Anything else (a literal, a call, a different name) is a
  // real shadow.
  static bool isReexport(const std::string& name, const ExprPtr& init) {
    if (!init) return false;
    const auto* variable = std::get_if<VariableExpr>(&init->node);
    return variable != nullptr && variable->name == name;
  }

  void walkStmt(const StmtPtr& stmt) {
    if (!stmt) return;
    std::visit(
        [&](const auto& node) {
          using T = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<T, LetStmt>) {
            if (!isReexport(node.name, node.init)) report(node.name, stmt->line, stmt->col);
            walkExpr(node.init);
          } else if constexpr (std::is_same_v<T, FnDeclStmt>) {
            report(node.name, stmt->line, stmt->col);
            for (const Param& param : node.params) report(param.name, stmt->line, stmt->col);
            walkStmt(node.body);
          } else if constexpr (std::is_same_v<T, StructDeclStmt>) {
            report(node.name, stmt->line, stmt->col);
          } else if constexpr (std::is_same_v<T, BlockStmt>) {
            for (const StmtPtr& inner : node.stmts) walkStmt(inner);
          } else if constexpr (std::is_same_v<T, IfStmt>) {
            walkExpr(node.condition);
            walkStmt(node.thenBranch);
            walkStmt(node.elseBranch);
          } else if constexpr (std::is_same_v<T, WhileStmt>) {
            walkExpr(node.condition);
            walkStmt(node.body);
          } else if constexpr (std::is_same_v<T, DoWhileStmt>) {
            walkStmt(node.body);
            walkExpr(node.condition);
          } else if constexpr (std::is_same_v<T, ForStmt>) {
            walkStmt(node.init);
            walkExpr(node.condition);
            walkExpr(node.step);
            walkStmt(node.body);
          } else if constexpr (std::is_same_v<T, ForInStmt>) {
            report(node.var, stmt->line, stmt->col);
            report(node.valueVar, stmt->line, stmt->col);
            walkExpr(node.iterable);
            walkStmt(node.body);
          } else if constexpr (std::is_same_v<T, ReturnStmt>) {
            walkExpr(node.value);
          } else if constexpr (std::is_same_v<T, ExprStmt>) {
            walkExpr(node.expr);
          } else if constexpr (std::is_same_v<T, SwitchStmt>) {
            walkExpr(node.subject);
            for (const SwitchCase& entry : node.cases) {
              for (const ExprPtr& value : entry.values) walkExpr(value);
              walkStmt(entry.body);
            }
            walkStmt(node.defaultBody);
          } else if constexpr (std::is_same_v<T, DeferStmt>) {
            walkStmt(node.body);
          } else if constexpr (std::is_same_v<T, ExternBlockStmt>) {
            for (const FnDeclStmt& decl : node.decls) {
              for (const Param& param : decl.params) report(param.name, stmt->line, stmt->col);
            }
          }
          // BreakStmt, ContinueStmt and ImportStmt introduce no name.
        },
        stmt->node);
  }

  void walkExpr(const ExprPtr& expr) {
    if (!expr) return;
    std::visit(
        [&](const auto& node) {
          using T = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<T, InterpExpr>) {
            for (const InterpPart& part : node.parts) {
              if (const auto* inner = std::get_if<ExprPtr>(&part.content)) walkExpr(*inner);
            }
          } else if constexpr (std::is_same_v<T, ArrayExpr>) {
            for (const ExprPtr& element : node.elements) walkExpr(element);
          } else if constexpr (std::is_same_v<T, MapExpr>) {
            for (const MapEntry& entry : node.entries) {
              walkExpr(entry.keyExpr);
              walkExpr(entry.value);
            }
          } else if constexpr (std::is_same_v<T, StructLitExpr>) {
            for (const StructLitEntry& entry : node.entries) walkExpr(entry.value);
          } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            walkExpr(node.operand);
          } else if constexpr (std::is_same_v<T, BinaryExpr>) {
            walkExpr(node.lhs);
            walkExpr(node.rhs);
          } else if constexpr (std::is_same_v<T, AssignExpr>) {
            walkExpr(node.target);
            walkExpr(node.value);
          } else if constexpr (std::is_same_v<T, TernaryExpr>) {
            walkExpr(node.condition);
            walkExpr(node.thenExpr);
            walkExpr(node.elseExpr);
          } else if constexpr (std::is_same_v<T, CallExpr>) {
            walkExpr(node.callee);
            for (const ExprPtr& arg : node.args) walkExpr(arg);
          } else if constexpr (std::is_same_v<T, IndexExpr>) {
            walkExpr(node.object);
            walkExpr(node.index);
          } else if constexpr (std::is_same_v<T, SliceExpr>) {
            walkExpr(node.object);
            walkExpr(node.start);
            walkExpr(node.end);
            walkExpr(node.step);
          } else if constexpr (std::is_same_v<T, MemberExpr>) {
            walkExpr(node.object);
          } else if constexpr (std::is_same_v<T, NonNullExpr>) {
            walkExpr(node.operand);
          } else if constexpr (std::is_same_v<T, RangeExpr>) {
            walkExpr(node.start);
            walkExpr(node.end);
          } else if constexpr (std::is_same_v<T, TryExpr>) {
            walkExpr(node.operand);
          } else if constexpr (std::is_same_v<T, CatchExpr>) {
            walkExpr(node.operand);
            walkExpr(node.fallback);
          } else if constexpr (std::is_same_v<T, LambdaExpr>) {
            for (const Param& param : node.params) report(param.name, expr->line, expr->col);
            walkStmt(node.body);
          }
          // LiteralExpr and VariableExpr are leaves.
        },
        expr->node);
  }

  const std::unordered_set<std::string>& builtins_;
  std::vector<Diagnostic>& out_;
};

}  // namespace

const std::unordered_set<std::string>& builtinNames() {
  static const std::unordered_set<std::string> names = [] {
    // The engine installs the standard library itself, so asking it is the one
    // way to get the names without a second list that could drift.
    VM vm;
    std::unordered_set<std::string> result;
    for (const auto& entry : vm.globals()) result.insert(entry.first);
    return result;
  }();
  return names;
}

std::vector<Diagnostic> lint(const std::string& source, const std::string& where,
                             std::string& parseError) {
  parseError.clear();
  std::vector<Diagnostic> diagnostics;
  try {
    Parser parser(lex(source), where);
    StmtPtr program = parser.parseProgram();
    ShadowWalker walker(builtinNames(), diagnostics);
    walker.walkProgram(program);
  } catch (const std::exception& error) {
    // A program the compiler rejects is not a lint question: report its error
    // and let the caller print it, so `oskelint` and `oskel` agree on validity.
    parseError = error.what();
    diagnostics.clear();
  }
  return diagnostics;
}

std::string format(const Diagnostic& diagnostic, const std::string& where) {
  return where + ":" + std::to_string(diagnostic.line) + ":" +
         std::to_string(diagnostic.col) + ": warning: " + diagnostic.message + " [" +
         diagnostic.rule + "]";
}

}  // namespace oskelint
