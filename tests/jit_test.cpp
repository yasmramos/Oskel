// Tests for the development-engine JIT (slice 1: integer leaf protos).
//
// The JIT must be *observationally identical* to the interpreter for every
// proto it accepts, so every test here is differential: the same function is
// run through the bytecode VM and through the compiled native code, and the two
// results must match. The bytecode contract is untouched by the JIT (no new
// opcode, no superinstruction), so a mismatch is a JIT bug, never a language
// change.

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "bytecode.h"
#include "compiler.h"
#include "core/value.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "jit/jit.h"
#include "vm.h"

// The JIT emits x86-64 for the System V ABI and maps it with `mmap`, so it only
// runs on x86-64 Linux and macOS (`jitHostSupported` in `src/runtime/jit/jit.cpp`). On any other
// host `jitCompile` returns nullptr and the VM keeps interpreting, so the
// differential checks have nothing to compare against. Like `codegen_test`,
// `lower_test` and `freestanding_test`, the suite is skipped there.
#if !((defined(__linux__) || defined(__APPLE__)) && defined(__x86_64__))
int main() {
  std::printf("skipped: the JIT requires x86-64 Linux or macOS\n");
  return 0;
}
#else

static int g_failures = 0;

static void check(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what.c_str());
  }
}

static Bytecode compileSource(const std::string& source) {
  Parser parser(lex(source), "<jit-test>");
  return Compiler{}.compile(parser.parseProgram());
}

// Runs `source` on the VM and returns the value of the global `r`.
static Value runOnVm(const std::string& source) {
  Bytecode bytecode = compileSource(source);
  VM vm;
  vm.run(bytecode);
  const Value* result = vm.findGlobal("r");
  return result ? *result : Value();
}

// Finds the child proto named `name` in `bc` (the functions a program defines).
static std::shared_ptr<const Bytecode> findProto(const Bytecode& bc, const std::string& name) {
  for (const std::shared_ptr<Bytecode>& child : bc.children) {
    if (child && child->name == name) return child;
  }
  return nullptr;
}

// Compiles the function `name` defined by `source` and runs it on the JIT with
// `args`. Returns false when the JIT did not accept the proto.
static bool runOnJit(const std::string& source, const std::string& name,
                     const std::vector<Value>& args, Value& out) {
  Bytecode bc = compileSource(source);
  std::shared_ptr<const Bytecode> proto = findProto(bc, name);
  if (!proto) return false;
  std::unique_ptr<JitFunction> fn = jitCompile(*proto);
  if (!fn) return false;
  out = fn->call(args);
  return true;
}

// The heart of the suite: run `fnName(args)` on the VM and on the JIT and
// require the same value.
static void differential(const std::string& source, const std::string& fnName,
                         const std::vector<Value>& args, const char* what) {
  const std::string call = fnName + "(";
  std::string argList;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i) argList += ", ";
    argList += std::to_string(args[i].asInt());
  }
  argList += ")";

  Value expected = runOnVm("var r = " + call + argList + ";\n" + source);
  Value actual;
  const bool compiled = runOnJit(source, fnName, args, actual);
  const bool ok = compiled && actual.equals(expected);
  check(ok, what);
  if (!ok) {
    std::printf("      compiled=%d expected=%s actual=%s\n", compiled ? 1 : 0,
                expected.toString().c_str(), actual.toString().c_str());
  }
}

// A proto the JIT must reject: it returns nullptr so the VM keeps interpreting.
static void expectRejected(const std::string& source, const std::string& fnName, const char* what) {
  Bytecode bc = compileSource(source);
  std::shared_ptr<const Bytecode> proto = findProto(bc, fnName);
  if (!proto) {
    check(false, std::string(what) + " (proto not found)");
    return;
  }
  check(jitCompile(*proto) == nullptr, what);
}

// A tiny deterministic PRNG so the fuzz corpus is reproducible.
struct Rng {
  std::uint64_t state = 0x9E3779B97F4A7C15ULL;
  std::uint64_t next() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }
  int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
};

// Builds a random integer-only expression. Every operator here yields an int,
// so the expression never leaves the JIT's subset. `depth` bounds the size.
static std::string randomExpr(Rng& rng, int depth) {
  if (depth <= 0) {
    switch (rng.below(4)) {
      case 0: return "a";
      case 1: return "b";
      case 2: return "c";
      default: return std::to_string(rng.below(21) - 10);
    }
  }
  const int choice = rng.below(10);
  if (choice == 0) return "(-" + randomExpr(rng, depth - 1) + ")";
  if (choice == 1) return "(~" + randomExpr(rng, depth - 1) + ")";
  static const char* ops[] = {"+", "-", "*", "&", "|", "^"};
  const char* op = ops[rng.below(6)];
  return "(" + randomExpr(rng, depth - 1) + " " + op + " " + randomExpr(rng, depth - 1) + ")";
}

// A random comparison, used only as a branch condition.
static std::string randomCond(Rng& rng, int depth) {
  static const char* cmps[] = {"<", "<=", ">", ">=", "==", "!="};
  const char* cmp = cmps[rng.below(6)];
  return randomExpr(rng, depth) + " " + cmp + " " + randomExpr(rng, depth);
}

// Generates a whole leaf function that is guaranteed to terminate and to stay
// inside the integer subset, then checks the JIT against the VM over several
// argument tuples. This is the differential fuzz: a mismatch is a JIT bug.
static void fuzzDifferential(Rng& rng, int rounds) {
  int mismatches = 0;
  int compiled = 0;
  for (int r = 0; r < rounds; ++r) {
    std::string body =
        "fn f(a, b, c) {\n"
        "  var t = " + randomExpr(rng, 3) + ";\n"
        "  if (" + randomCond(rng, 2) + ") { t = t + (" + randomExpr(rng, 2) + "); }\n"
        "  else { t = t - (" + randomExpr(rng, 2) + "); }\n"
        "  var i = 0;\n"
        "  while (i < " + std::to_string(rng.below(6)) + ") { t = t + (i * " +
        std::to_string(rng.below(5) - 2) + "); i = i + 1; }\n"
        "  if (" + randomCond(rng, 1) + ") { return -t; }\n"
        "  return t;\n"
        "}\n";
    const std::vector<Value> args = {
        Value(static_cast<int64_t>(rng.below(41) - 20)),
        Value(static_cast<int64_t>(rng.below(41) - 20)),
        Value(static_cast<int64_t>(rng.below(41) - 20))};
    Value expected = runOnVm("var r = f(" + std::to_string(args[0].asInt()) + ", " +
                             std::to_string(args[1].asInt()) + ", " +
                             std::to_string(args[2].asInt()) + ");\n" + body);
    Value actual;
    const bool ok = runOnJit(body, "f", args, actual);
    if (ok) ++compiled;
    if (!ok || !actual.equals(expected)) {
      ++mismatches;
      if (mismatches <= 3) {
        std::printf("      fuzz mismatch\n%s      expected=%s actual=%s\n", body.c_str(),
                    expected.toString().c_str(), actual.toString().c_str());
      }
    }
  }
  check(mismatches == 0, "random integer leaf functions match the VM");
  check(compiled == rounds, "every random integer leaf function was compiled");
}

int main() {
  // --- Arithmetic and locals -------------------------------------------------
  const char* arith =
      "fn add(a, b) { return a + b; }\n"
      "fn sub(a, b) { return a - b; }\n"
      "fn mul(a, b) { return a * b; }\n"
      "fn mix(a, b, c) { return a * b - c; }\n"
      "fn negate(a) { return -a; }\n"
      "fn bits(a, b) { return (a & b) | (a ^ b); }\n";

  differential(arith, "add", {Value(2), Value(3)}, "add(2,3) matches the VM");
  differential(arith, "sub", {Value(10), Value(40)}, "sub(10,40) matches the VM");
  differential(arith, "mul", {Value(7), Value(6)}, "mul(7,6) matches the VM");
  differential(arith, "mix", {Value(4), Value(5), Value(3)}, "mix(4,5,3) matches the VM");
  differential(arith, "negate", {Value(9)}, "negate(9) matches the VM");
  differential(arith, "bits", {Value(12), Value(10)}, "bits(12,10) matches the VM");

  // --- Comparisons, booleans and truthiness ---------------------------------
  const char* cmp =
      "fn lt(a, b) { return a < b; }\n"
      "fn le(a, b) { return a <= b; }\n"
      "fn gt(a, b) { return a > b; }\n"
      "fn ge(a, b) { return a >= b; }\n"
      "fn eq(a, b) { return a == b; }\n"
      "fn ne(a, b) { return a != b; }\n"
      "fn notx(a) { return !a; }\n"
      "fn notBool(a) { if (a) { return !true; } return !false; }\n";

  differential(cmp, "lt", {Value(3), Value(5)}, "a < b matches the VM");
  differential(cmp, "lt", {Value(5), Value(3)}, "a < b (false) matches the VM");
  differential(cmp, "le", {Value(5), Value(5)}, "a <= b matches the VM");
  differential(cmp, "gt", {Value(5), Value(3)}, "a > b matches the VM");
  differential(cmp, "ge", {Value(3), Value(5)}, "a >= b (false) matches the VM");
  differential(cmp, "eq", {Value(4), Value(4)}, "a == b matches the VM");
  differential(cmp, "ne", {Value(4), Value(5)}, "a != b matches the VM");
  differential(cmp, "notx", {Value(0)}, "!0 is false (0 is truthy) like the VM");
  differential(cmp, "notx", {Value(7)}, "!7 is false like the VM");
  differential(cmp, "notBool", {Value(1)}, "!true through a branch matches the VM");

  // --- Control flow and loops ------------------------------------------------
  const char* loops =
      "fn sumTo(n) { var s = 0; var i = 1; while (i <= n) { s = s + i; i = i + 1; } return s; }\n"
      "fn fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }\n";

  differential(loops, "sumTo", {Value(10)}, "sumTo(10) loop matches the VM");
  differential(loops, "sumTo", {Value(0)}, "sumTo(0) loop matches the VM");
  differential(loops, "sumTo", {Value(1000)}, "sumTo(1000) loop matches the VM");
  // `fib` recurses, so its proto has a CALL and must be rejected; the VM runs it.
  expectRejected(loops, "fib", "a proto with a recursive CALL is rejected");

  // --- Nested branches and mixed nil/bool/int --------------------------------
  const char* branches =
      "fn classify(a) { if (a < 0) { return 0 - 1; } if (a == 0) { return 0; } return 1; }\n"
      "fn abs(a) { if (a < 0) { return -a; } return a; }\n";

  differential(branches, "classify", {Value(-5)}, "classify(-5) matches the VM");
  differential(branches, "classify", {Value(0)}, "classify(0) matches the VM");
  differential(branches, "classify", {Value(9)}, "classify(9) matches the VM");
  differential(branches, "abs", {Value(-42)}, "abs(-42) matches the VM");
  differential(branches, "abs", {Value(42)}, "abs(42) matches the VM");

  // --- Wide/edge integer payloads -------------------------------------------
  differential(arith, "add",
               {Value(static_cast<int64_t>(900000000000LL)), Value(static_cast<int64_t>(700000000000LL))},
               "large ints add like the VM");
  differential(arith, "negate", {Value(-1)}, "negate(-1) matches the VM");

  // --- Register allocation: pool overflow and mixed scalar kinds -------------
  // `wide` has ten live parameters, more than the physical-register pool, so
  // the extra slots stay on the stack; the two representations must agree.
  const char* wide =
      "fn wide(a, b, c, d, e, f, g, h, i, j) {\n"
      "  var s = a + b + c + d + e + f + g + h + i + j;\n"
      "  var t = s;\n"
      "  while (t > 100) { t = t - 3; }\n"
      "  return t;\n"
      "}\n";
  differential(wide, "wide", {Value(1), Value(2), Value(3), Value(4), Value(5), Value(6),
                              Value(7), Value(8), Value(9), Value(10)},
               "ten-parameter function (register pool overflow) matches the VM");

  // `mixed` reuses one register for both a `bool` (the `==`) and an `int` (the
  // `!` of that bool and the loop counter), which a single-kind-per-slot rule
  // would have kept on the stack; the register path must still be exact.
  const char* mixed =
      "fn mixed(n) {\n"
      "  var acc = 0;\n"
      "  var i = 0;\n"
      "  while (i < n) { acc = acc + i; i = i + 1; }\n"
      "  var odd = (acc & 1) == 1;\n"
      "  return odd;\n"
      "}\n";
  differential(mixed, "mixed", {Value(0)}, "mixed(0) matches the VM");
  differential(mixed, "mixed", {Value(7)}, "mixed(7) matches the VM");
  differential(mixed, "mixed", {Value(8)}, "mixed(8) matches the VM");

  // --- Coalescing: a copy that should share a register -----------------------
  // `copies` builds a value in one temporary, copies it into a local, copies the
  // local into a second temporary and returns that. With web coalescing the
  // three names share a register (no `mov` between them); the result must equal
  // the VM's either way, which is what this pins down.
  const char* coalesce =
      "fn copies(a, b) {\n"
      "  var t = a * b + 3;\n"
      "  var u = t;\n"
      "  var v = u;\n"
      "  return v;\n"
      "}\n";
  differential(coalesce, "copies", {Value(4), Value(5)}, "copies(4,5) matches the VM");
  differential(coalesce, "copies", {Value(-3), Value(7)}, "copies(-3,7) matches the VM");

  // --- Constant folding and strength reduction -------------------------------
  // Every one of these exercises a slice-2 rewrite: an all-constant subtree
  // that folds away, a constant operand applied as an immediate, a `const - var`
  // that must not be commuted, a power-of-two multiply turned into a shift, an
  // algebraic identity (`x + 0`, `x * 1`, `x & -1`), and a constant branch. The
  // differential check proves each rewrite keeps the interpreter's result.
  const char* folds =
      "fn folds(a, b) {\n"
      "  var c = (2 * 3) + (10 - 4);\n"        // all-constant subtree
      "  var d = a + 7 - 3;\n"                 // immediate add/sub
      "  var e = 100 - b;\n"                   // const - var (non-commutative)
      "  var f = b * 8;\n"                     // power-of-two -> shift
      "  var g = b * 3;\n"                     // small immediate multiply
      "  var h = (a & 255) | (b ^ 5);\n"       // immediate and/or/xor
      "  var i = a + 0 + b * 1 + (a & -1);\n"  // identities
      "  if (2 < 3) { i = i + 1; } else { i = i - 100; }\n"  // constant branch
      "  while (false) { i = i + 999; }\n"     // never-taken loop
      "  return c + d + e + f + g + h + i - -a - ~b;\n"
      "}\n";
  differential(folds, "folds", {Value(0), Value(0)}, "folds(0,0) matches the VM");
  differential(folds, "folds", {Value(12), Value(5)}, "folds(12,5) matches the VM");
  differential(folds, "folds", {Value(-7), Value(100)}, "folds(-7,100) matches the VM");

  // A big immediate (outside imm32) and a constant that must be materialized.
  const char* bigImm =
      "fn big(n) {\n"
      "  var x = n + 5000000000;\n"    // > 2^32, forces a register
      "  var y = n * 1000000007;\n"
      "  var z = n - 4294967296;\n"
      "  return x + y + z;\n"
      "}\n";
  differential(bigImm, "big", {Value(3)}, "large immediates match the VM");
  differential(bigImm, "big", {Value(-9)}, "large immediates with a negative arg match the VM");

  // Comparisons with a constant operand on either side.
  const char* cmpConst =
      "fn cmp(n) {\n"
      "  var r = 0;\n"
      "  if (n < 10) { r = r + 1; }\n"
      "  if (10 <= n) { r = r + 2; }\n"
      "  if (n == 5) { r = r + 4; }\n"
      "  if (5 != n) { r = r + 8; }\n"
      "  if (n > -3) { r = r + 16; }\n"
      "  return r;\n"
      "}\n";
  differential(cmpConst, "cmp", {Value(5)}, "const comparisons (n == 5) match the VM");
  differential(cmpConst, "cmp", {Value(20)}, "const comparisons (n == 20) match the VM");
  differential(cmpConst, "cmp", {Value(-9)}, "const comparisons (n == -9) match the VM");

  // A constant unary operand must fold too (`-` and `~`), not just a binary one.
  const char* unaryConst =
      "fn unary(n) {\n"
      "  var a = -5 + n;\n"
      "  var b = ~(2 + 3) + n;\n"
      "  var c = -(-n);\n"
      "  var d = !(1 + 1);\n"
      "  var e = n + (1 < 2 ? 1 : 1);\n"
      "  return a + b + c + (d ? 100 : 0) + e;\n"
      "}\n";
  differential(unaryConst, "unary", {Value(3)}, "constant unary operands match the VM");
  differential(unaryConst, "unary", {Value(-8)}, "constant unary operands (negative) match the VM");

  // --- Compare-branch fusion ------------------------------------------------
  // A condition whose boolean result only feeds the branch right after it must
  // compile to `cmp; jcc`, and the result must be identical to the VM. The
  // shapes below cover a `while`, an `if`/`else`, a `JMP_IF_TRUE` (an `||`),
  // and a comparison whose result *is* reused afterwards (the last one must
  // stay unfused, and still be correct).
  const char* branchFusion =
      "fn loops(n) {\n"
      "  var s = 0;\n"
      "  var i = 0;\n"
      "  while (i <= n) { s = s + i; i = i + 1; }\n"
      "  if (s > 100) { s = s - 1; } else { s = s + 1; }\n"
      "  return s;\n"
      "}\n"
      "fn orTest(a, b) { if (a > 0 || b > 0) { return a + b; } return a - b; }\n"
      "fn reuse(a, b) { var t = a < b; if (t) { return a; } return b + (t ? 1 : 0); }\n";
  differential(branchFusion, "loops", {Value(0)}, "while + if/else fusion matches the VM (n=0)");
  differential(branchFusion, "loops", {Value(10)}, "while + if/else fusion matches the VM (n=10)");
  differential(branchFusion, "loops", {Value(30)}, "while + if/else fusion matches the VM (n=30)");
  differential(branchFusion, "orTest", {Value(3), Value(-1)}, "JMP_IF_TRUE fusion matches the VM (a)");
  differential(branchFusion, "orTest", {Value(-3), Value(-1)}, "JMP_IF_TRUE fusion matches the VM (b)");
  differential(branchFusion, "reuse", {Value(1), Value(9)}, "a reused comparison result stays correct");
  differential(branchFusion, "reuse", {Value(9), Value(1)}, "a reused comparison result stays correct (b)");

  // --- Rejections: everything outside the integer-leaf subset ----------------
  expectRejected("fn greet(n) { return \"hi\"; }\n", "greet", "a string return is rejected");
  expectRejected("fn useGlobal(n) { return n + 1.5; }\n", "useGlobal",
                 "a float literal is rejected");
  expectRejected("fn callIt(n) { return n; }\nfn g(n) { return callIt(n); }\n", "g",
                 "a proto that CALLs is rejected");
  expectRejected("fn mk(n) { return (x) => x; }\n", "mk",
                 "a proto with a child closure is rejected");

  // --- Host support is reported, not silently false --------------------------
  {
    // `JitCode::assign` fails without a word on a host with no backend, so the
    // CLI now asks these two and prints the reason. A supported host must
    // report no reason and an unsupported one must report something; the two
    // cannot disagree (BACKLOG §5 item 6).
    const bool supported = jitHostSupported();
    const std::string reason = jitUnsupportedReason();
    check(supported == reason.empty(),
          "jitUnsupportedReason is empty exactly when the backend exists");
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    check(supported, "the JIT backend is available on x86-64 Linux");
#else
    check(!supported, "no JIT backend outside x86-64 Linux");
#endif
  }

  // --- The cache compiles each proto once and stays correct ------------------
  {
    Bytecode bc = compileSource(arith);
    std::shared_ptr<const Bytecode> proto = findProto(bc, "add");
    Jit jit;
    JitFunction* first = jit.get(proto);
    JitFunction* second = jit.get(proto);
    check(first != nullptr && first == second, "Jit::get caches the compiled proto");
    check(jit.compiledCount() == 1, "the cache holds one entry after two gets");
    if (first) {
      Value out = first->call({Value(20), Value(22)});
      check(out.isInt() && out.asInt() == 42, "the cached function still computes");
    }
  }

  // --- The cache is bounded and evicts the oldest entry ----------------------
  {
    Jit jit;
    // Fill past the cap with distinct protos. A REPL entry creates a fresh
    // `Bytecode`, so distinct protos are the real workload; the cache must not
    // grow with the session (BACKLOG §5 item 2).
    std::vector<std::shared_ptr<const Bytecode> > protos;
    for (std::size_t i = 0; i < Jit::kMaxEntries + 10; ++i) {
      const std::string source =
          "fn f" + std::to_string(i) + "(a) { return a + " + std::to_string(i) + "; }\n";
      Bytecode bc = compileSource(source);
      protos.push_back(findProto(bc, "f" + std::to_string(i)));
      jit.get(protos.back());
    }
    check(jit.size() <= Jit::kMaxEntries, "the cache never exceeds its cap");
    check(jit.size() == Jit::kMaxEntries, "the cache fills up to the cap");

    // The oldest protos were dropped and the newest are still cached. A dropped
    // proto is recompiled on demand, so the second `get` must compile again.
    std::shared_ptr<const Bytecode> oldest = protos.front();
    std::shared_ptr<const Bytecode> newest = protos.back();
    const std::size_t before = jit.size();
    JitFunction* newestAgain = jit.get(newest);
    check(jit.size() == before, "a cached proto is a hit, not a new entry");
    check(newestAgain != nullptr, "the newest proto is still cached");
    JitFunction* oldestAgain = jit.get(oldest);
    check(oldestAgain != nullptr, "an evicted proto recompiles on demand");
    check(jit.size() <= Jit::kMaxEntries, "recompiling an evicted proto keeps the cap");

    // The cache keys on the proto address, so it must own each proto it
    // compiled: otherwise a freed proto could leave a stale key behind and a
    // different `Bytecode` allocated at the same address would resolve to the
    // old compiled code (BACKLOG §5 item 4). The entry's `shared_ptr` is what
    // makes that impossible, so assert the reference count actually rises.
    {
      Jit jit;
      std::shared_ptr<const Bytecode> owned = protos.front();
      const long before = owned.use_count();
      jit.get(owned);
      check(owned.use_count() == before + 1,
            "the cache holds its own reference to a compiled proto");
      owned.reset();
      check(jit.size() == 1, "the entry outlives the caller's reference");
    }

    // `clear` empties both the map and the insertion order.
    jit.clear();
    check(jit.size() == 0, "clear drops every entry");
    check(jit.compiledCount() == 0, "no compiled function survives clear");
  }

  // --- The VM with the JIT enabled must agree with the VM without it ---------
  {
    const std::string program =
        "fn add(a, b) { return a + b; }\n"
        "fn work(n) { var s = 0; var i = 0; while (i < n) { s = s + (i * 2); i = i + 1; } return s; }\n"
        "var r = work(100) + add(2, 3);\n";

    auto runWith = [&](bool jit) {
      Bytecode bc = compileSource(program);
      VM vm;
      vm.setJitEnabled(jit);
      vm.run(bc);
      const Value* result = vm.findGlobal("r");
      return std::pair<Value, std::size_t>(result ? *result : Value(), vm.jitCompiledCount());
    };

    auto plain = runWith(false);
    auto jitted = runWith(true);
    check(plain.first.equals(jitted.first), "VM result is identical with the JIT on");
    check(plain.second == 0, "the JIT compiles nothing while disabled");
    check(jitted.second >= 1, "the JIT compiles an accepted proto while enabled");
  }

  // --- A direct call with the wrong argument count is rejected --------------
  {
    Bytecode bc = compileSource("fn add(a, b) { return a + b; }\n");
    std::shared_ptr<const Bytecode> proto = findProto(bc, "add");
    std::unique_ptr<JitFunction> fn = proto ? jitCompile(*proto) : nullptr;
    check(fn != nullptr, "arity guard: the two-parameter proto compiles");
    if (fn) {
      auto throwsOnCount = [&](const std::vector<Value>& args) {
        try {
          fn->call(args);
        } catch (const std::logic_error&) {
          return true;
        }
        return false;
      };
      check(throwsOnCount({}), "call with no arguments throws");
      check(throwsOnCount({Value(static_cast<int64_t>(1))}), "call with too few arguments throws");
      check(throwsOnCount({Value(static_cast<int64_t>(1)), Value(static_cast<int64_t>(2)),
                           Value(static_cast<int64_t>(3))}),
            "call with too many arguments throws");
      check(fn->call({Value(static_cast<int64_t>(2)), Value(static_cast<int64_t>(3))})
                .equals(Value(static_cast<int64_t>(5))),
            "call with the exact argument count still returns the result");
    }
  }

  // --- Differential fuzz over random integer leaf functions ------------------
  {
    Rng rng;
    fuzzDifferential(rng, 200);
  }

  if (g_failures != 0) {
    std::printf("\n%d JIT test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("\nall JIT tests passed\n");
  return 0;
}

#endif  // x86-64 Linux or macOS
