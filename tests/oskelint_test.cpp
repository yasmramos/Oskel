// Tests for the `oskelint` tool.
//
// The subject is `oskelint::lint`: an Oskel source in, a list of diagnostics
// out. Each case pins one shape of shadowing (or one shape that must stay
// silent) so a walker that stops descending into a construct is caught here
// rather than by a user.

#include <cstdio>
#include <string>
#include <vector>

#include "oskelint.h"

static int g_failures = 0;

static void check(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what.c_str());
  }
}

// Lints `source` and returns the messages, so a case asserts on the rule name
// and the shadowed builtin rather than on a position it would have to count.
static std::vector<std::string> lintMessages(const std::string& source) {
  std::string parseError;
  const std::vector<oskelint::Diagnostic> diagnostics =
      oskelint::lint(source, "test.oskl", parseError);
  std::vector<std::string> messages;
  for (const oskelint::Diagnostic& diagnostic : diagnostics) {
    messages.push_back(diagnostic.rule + ": " + diagnostic.message);
  }
  return messages;
}

// Asserts that `source` produces exactly `expected` messages, in order.
static void expectDiagnostics(const std::string& what, const std::string& source,
                              const std::vector<std::string>& expected) {
  const std::vector<std::string> actual = lintMessages(source);
  if (actual != expected) {
    std::printf("FAIL: %s\n", what.c_str());
    std::printf("  expected %zu, got %zu\n", expected.size(), actual.size());
    for (const std::string& message : expected) {
      std::printf("    expected: %s\n", message.c_str());
    }
    for (const std::string& message : actual) {
      std::printf("    actual:   %s\n", message.c_str());
    }
    ++g_failures;
    return;
  }
  std::printf("ok: %s\n", what.c_str());
}

static void expectSilent(const std::string& what, const std::string& source) {
  expectDiagnostics(what, source, {});
}

int main() {
  // The engine's own list is the source of truth: a name the rule claims is a
  // builtin must be one `registerBuiltins` installs, and a name that is not
  // installed must not be reported.
  check(oskelint::builtinNames().count("print") == 1, "print is a builtin");
  check(oskelint::builtinNames().count("len") == 1, "len is a builtin");
  check(oskelint::builtinNames().count("sqrt") == 1, "sqrt is a builtin");
  check(oskelint::builtinNames().count("definitelyNotABuiltin") == 0,
        "an ordinary name is not a builtin");

  // Every declaration shape that introduces a name.
  expectDiagnostics("var", "var len = 3;", {"builtin-shadow: 'len' shadows the builtin 'len'"});
  expectDiagnostics("let", "let sqrt = 5;", {"builtin-shadow: 'sqrt' shadows the builtin 'sqrt'"});
  expectDiagnostics("const", "const print = 1;",
                    {"builtin-shadow: 'print' shadows the builtin 'print'"});
  expectDiagnostics("fn", "fn str(x) { return x; }",
                    {"builtin-shadow: 'str' shadows the builtin 'str'"});
  expectDiagnostics("fn parameter", "fn f(args) { return args; }",
                    {"builtin-shadow: 'args' shadows the builtin 'args'"});
  expectDiagnostics("lambda parameter", "var f = (ord) => ord;",
                    {"builtin-shadow: 'ord' shadows the builtin 'ord'"});
  expectDiagnostics("struct", "struct type { x: int; }",
                    {"builtin-shadow: 'type' shadows the builtin 'type'"});
  expectDiagnostics("for-in variable", "for len in [1] { print(len); }",
                    {"builtin-shadow: 'len' shadows the builtin 'len'"});
  expectDiagnostics("for-in key and value", "for k, chr in {} { print(k); }",
                    {"builtin-shadow: 'chr' shadows the builtin 'chr'"});

  // Nesting: a declaration inside a block, an if, a loop, a switch, a lambda
  // body or a function body is still reached. A walker that forgot to descend
  // into one of these would report nothing here.
  expectDiagnostics("inside a block", "{ var len = 1; }",
                    {"builtin-shadow: 'len' shadows the builtin 'len'"});
  expectDiagnostics("inside an if", "if (1) { var len = 1; }",
                    {"builtin-shadow: 'len' shadows the builtin 'len'"});
  expectDiagnostics("inside a while", "while (0) { var len = 1; }",
                    {"builtin-shadow: 'len' shadows the builtin 'len'"});
  expectDiagnostics("inside a for", "for (var i = 0; i < 1; i = i + 1) { var len = 1; }",
                    {"builtin-shadow: 'len' shadows the builtin 'len'"});
  expectDiagnostics("inside a switch", "switch (1) { case 1: { var len = 1; } }",
                    {"builtin-shadow: 'len' shadows the builtin 'len'"});
  expectDiagnostics("inside a function body", "fn f() { var len = 1; return len; }",
                    {"builtin-shadow: 'len' shadows the builtin 'len'"});
  expectDiagnostics("inside a lambda body", "var f = () => { var len = 1; return len; };",
                    {"builtin-shadow: 'len' shadows the builtin 'len'"});

  // The re-export idiom is not a shadow: the builtin is being surfaced, not
  // hidden. This is how `std.sys` exports `time`, and a rule that fired here
  // would report the standard library against itself.
  expectSilent("re-export with let", "let time = time;");
  expectSilent("re-export with var", "var print = print;");
  expectSilent("re-export with const", "const len = len;");
  expectDiagnostics("a real shadow after a re-export",
                    "let time = time;\nvar clock = 5;",
                    {"builtin-shadow: 'clock' shadows the builtin 'clock'"});

  // An ordinary name is not reported, and a program with no declaration at all
  // is silent.
  expectSilent("an ordinary name", "var total = 0;");
  expectSilent("an empty program", "");

  // A program the compiler rejects is not a lint question: the parse error is
  // reported through `parseError` and no diagnostics are produced, so the tool
  // never disagrees with `oskel` about whether a program is valid.
  {
    std::string parseError;
    const std::vector<oskelint::Diagnostic> diagnostics =
        oskelint::lint("var x = ;", "test.oskl", parseError);
    check(!parseError.empty(), "a syntax error sets parseError");
    check(diagnostics.empty(), "a syntax error produces no diagnostics");
  }

  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all oskelint tests passed\n");
  return 0;
}
