// Behavioral tests for the freestanding math library (PHASE 7c.5b.4a).
//
// The emitted machine code is mapped as executable memory and called through a
// function pointer. `exp`/`log`/`pow` are ports of musl libm; the reference is
// the *host* `std::exp`/`std::log`/`std::pow` (glibc), which is what the hosted
// engine uses. glibc is not correctly rounded, so the two disagree on a small
// fraction of inputs, always by a single ulp; the test therefore requires every
// disagreement to be within 1 ulp rather than an exact match, and prints the
// rate so a regression in accuracy is visible.
//
// On any platform other than Linux x86-64 the execution section is skipped.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "native/codegen.h"
#include "native/mathlib.h"

#if defined(__linux__) && defined(__x86_64__)
#define MATH_EXEC_TESTS 1
#include <cmath>
#include <cstdlib>
#include <random>
#include <sys/mman.h>
#endif

static int g_failures = 0;

static void check(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

#if defined(MATH_EXEC_TESTS)

using UnaryFn = double (*)(double);
using BinaryFn = double (*)(double, double);

static std::uint64_t bitsOf(double v) {
  std::uint64_t b;
  std::memcpy(&b, &v, sizeof b);
  return b;
}

// Ordered-integer distance between two doubles (ulp distance).
static std::uint64_t ulpDiff(double a, double b) {
  auto key = [](std::uint64_t u) -> std::uint64_t {
    return (u & 0x8000000000000000ULL) ? ~u : (u | 0x8000000000000000ULL);
  };
  std::uint64_t ka = key(bitsOf(a)), kb = key(bitsOf(b));
  return ka > kb ? ka - kb : kb - ka;
}

static void testUnary(UnaryFn fn, double (*ref)(double), const char* name,
                      std::mt19937_64& rng) {
  std::uint64_t differ = 0, total = 0, worst = 0;
  const bool isLog = std::string(name) == "log";
  for (int i = 0; i < 200000; ++i) {
    double x;
    if (i % 2 == 0) {
      x = std::uniform_real_distribution<double>(-20.0, 20.0)(rng);
    } else {
      std::uint64_t r = rng();
      std::memcpy(&x, &r, sizeof x);
    }
    if (std::isnan(x) || std::isinf(x)) continue;
    if (isLog && x <= 0.0) continue;
    double got = fn(x);
    double want = ref(x);
    if (std::isnan(want) || std::isinf(want)) continue;
    ++total;
    if (bitsOf(got) != bitsOf(want)) {
      ++differ;
      std::uint64_t d = ulpDiff(got, want);
      if (d > worst) worst = d;
      if (d > 1) {
        std::printf("FAIL: %s(%.17g): got %.17g want %.17g (%llu ulp)\n", name, x, got,
                    want, (unsigned long long)d);
        ++g_failures;
      }
    }
  }
  std::printf("  %-4s: %llu/%llu differ from glibc (%.4f%%), worst %llu ulp\n", name,
              (unsigned long long)differ, (unsigned long long)total,
              100.0 * differ / (total ? total : 1), (unsigned long long)worst);
  check(worst <= 1, std::string(name) + " within 1 ulp");
}

static void testPow(BinaryFn fn, std::mt19937_64& rng) {
  std::uint64_t differ = 0, total = 0, worst = 0;
  for (int i = 0; i < 200000; ++i) {
    double b = std::uniform_real_distribution<double>(0.0, 100.0)(rng);
    double e = std::uniform_real_distribution<double>(-5.0, 5.0)(rng);
    if (i % 3 == 0) e = static_cast<double>(static_cast<int>(rng() % 17) - 8);
    double got = fn(b, e);
    double want = std::pow(b, e);
    if (std::isnan(want) || std::isinf(want)) continue;
    ++total;
    if (bitsOf(got) != bitsOf(want)) {
      ++differ;
      std::uint64_t d = ulpDiff(got, want);
      if (d > worst) worst = d;
      if (d > 1) {
        std::printf("FAIL: pow(%g,%g): got %.17g want %.17g (%llu ulp)\n", b, e, got, want,
                    (unsigned long long)d);
        ++g_failures;
      }
    }
  }
  std::printf("  pow : %llu/%llu differ from glibc (%.4f%%), worst %llu ulp\n",
              (unsigned long long)differ, (unsigned long long)total,
              100.0 * differ / (total ? total : 1), (unsigned long long)worst);
  check(worst <= 1, "pow within 1 ulp");
}

// The special-case branches of `pow` are not reached by `testPow`, whose base is
// always non-negative and whose `nan` results are skipped. This table pins them
// down: a negative base (with an integer, an odd and a non-integer exponent,
// which is the only `nan` case), the `pow(1, y)` and `pow(x, 0)` shortcuts, and
// the `0^0`, `0^-1`, `x^-n` and overflow edges. Every value is exactly
// representable (or `nan`), so glibc and musl agree bit for bit.
static void testPowSpecialCases(BinaryFn fn) {
  struct Case {
    double base;
    double exponent;
  };
  const Case cases[] = {
      {1.0, 5.0},   {1.0, 0.5},   {1.0, -3.0},  {-3.0, 0.0},  {0.0, 0.0},
      {5.0, 0.0},   {-2.0, 2.0},  {-2.0, 3.0},  {-2.0, 4.0},  {-2.0, -3.0},
      {0.0, 2.0},   {0.0, -1.0},  {2.0, -2.0},  {3.0, -1.0},  {0.5, 0.5},
      {4.0, 0.5},   {10.0, 300.0}, {10.0, -300.0}, {-2.0, 0.5}, {-2.0, 1.5},
      {-1.0, 0.5},  {-2.0, 0.25},
  };
  for (const Case& c : cases) {
    double got = fn(c.base, c.exponent);
    double want = std::pow(c.base, c.exponent);
    bool ok = std::isnan(want) ? std::isnan(got) : bitsOf(got) == bitsOf(want);
    if (!ok) {
      std::printf("FAIL: pow(%g,%g): got %.17g want %.17g\n", c.base, c.exponent, got,
                  want);
      ++g_failures;
    }
  }
  check(true, "pow special cases checked");
}

int main() {
  CodeGen gen;
  mathlib::MathHandles h = mathlib::newMathHandles(gen);
  mathlib::emitMathLib(gen, h);
  gen.finalize();
  const std::vector<std::uint8_t>& bytes = gen.code();

  std::size_t span = bytes.size();
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    std::perror("mmap");
    return 2;
  }
  std::memcpy(mem, bytes.data(), span);
  if (mprotect(mem, span, PROT_READ | PROT_EXEC) != 0) {
    std::perror("mprotect");
    return 2;
  }
  auto base = reinterpret_cast<std::uint8_t*>(mem);
  auto expF = reinterpret_cast<UnaryFn>(base + gen.labelOffset(h.exp));
  auto logF = reinterpret_cast<UnaryFn>(base + gen.labelOffset(h.log));
  auto powF = reinterpret_cast<BinaryFn>(base + gen.labelOffset(h.pow));

  // Spot checks first, so an obvious breakage is loud.
  check(std::fabs(expF(1.0) - 2.718281828459045) < 1e-15, "exp(1)");
  check(expF(0.0) == 1.0, "exp(0) == 1");
  check(logF(1.0) == 0.0, "log(1) == 0");
  check(std::fabs(logF(2.718281828459045) - 1.0) < 1e-15, "log(e)");
  check(powF(2.0, 10.0) == 1024.0, "pow(2,10)");
  check(powF(9.0, 0.5) == 3.0, "pow(9,0.5)");

  std::mt19937_64 rng(12345);
  testUnary(expF, std::exp, "exp", rng);
  testUnary(logF, std::log, "log", rng);
  testPow(powF, rng);
  testPowSpecialCases(powF);

  if (g_failures == 0) {
    std::printf("all mathlib tests passed\n");
  } else {
    std::printf("%d mathlib test(s) failed\n", g_failures);
  }
  return g_failures == 0 ? 0 : 1;
}

#else
int main() {
  std::printf("mathlib execution tests skipped (not linux x86-64)\n");
  return 0;
}
#endif
