#pragma once

#include <string>
#include <unordered_set>
#include <vector>

// `oskelint`: the Oskel linter.
//
// A lint rule answers a question about a program that is legal to write, so it
// belongs in a tool rather than in the compiler. The compiler must stay clear
// of the builtin names (`tests/check_compiler_boundary.cmake` forbids the front
// end from reaching `runtime/builtins.h`), and a diagnostic that fires on legal
// code would make the language reject programs it accepts. A separate tool can
// hold both facts without bending either rule.
//
// This first rule, `builtin-shadow`, reports a declaration whose name is a
// builtin (`print`, `len`, ...). Shadowing is legal and the standard library
// relies on it -- `std.sys` re-exports `time` with `let time = time;` -- so the
// rule is advisory, never an error, and it never fires on the re-export idiom
// (a declaration whose initializer reads the same name, which is how a builtin
// is surfaced under its own name). It is the Oskel equivalent of pylint's
// W0622 (`redefined-builtin`) and ruff's A001.
namespace oskelint {

// One diagnostic. `line`/`col` are the position of the declaration's name.
struct Diagnostic {
  std::string rule;
  std::string message;
  int line = 1;
  int col = 1;
};

// The names `registerBuiltins` installs, read from the engine itself rather
// than copied, so the rule cannot drift from the standard library when a
// builtin is added or renamed. `oskelint` links the runtime for this one
// question; it never runs a program.
const std::unordered_set<std::string>& builtinNames();

// Lints `source` as a standalone file. `where` names the file in the messages
// (the entry file's path, so a diagnostic reads like the compiler's).
//
// Returns the diagnostics in source order. A file that does not parse is not
// the linter's business: `parseError` is set and the caller reports the
// compiler's own message, so `oskelint` never disagrees with `oskel` about
// whether a program is valid.
std::vector<Diagnostic> lint(const std::string& source, const std::string& where,
                             std::string& parseError);

// Formats one diagnostic the way the compiler formats its own: the file, the
// position, then the message, with the rule in brackets so a caller can filter.
std::string format(const Diagnostic& diagnostic, const std::string& where);

}  // namespace oskelint
