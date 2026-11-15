// Behavioral tests for the freestanding text-to-binary64 parser (7c.5b.2b).
//
// The emitted machine code is mapped as executable memory and called through a
// function pointer, exactly like the hosted engine would call it, and the
// result is compared bit for bit with `strtod` -- the reference the hosted
// `float(string)`/`double(string)` uses.
//
// On any platform other than Linux x86-64 the execution section is skipped.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "native/codegen.h"
#include "native/dtoa.h"
#include "native/parse_float.h"

#if defined(__linux__) && defined(__x86_64__)
#define PARSE_EXEC_TESTS 1
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

#if defined(PARSE_EXEC_TESTS)

// long parse(const char* data, long length, long* outBits, void* scratch)
using ParseFn = std::int64_t (*)(const char*, std::int64_t, std::int64_t*, void*);

static ParseFn emitParser() {
  CodeGen gen;
  dtoa::DtoaHandles dtoaHandles = dtoa::newDtoaHandles(gen);
  parse_float::ParseFloatHandles handles = parse_float::newParseFloatHandles(gen);
  dtoa::emitDtoa(gen, dtoaHandles);
  parse_float::emitParseFloat(gen, handles, dtoaHandles);
  gen.finalize();
  const std::vector<std::uint8_t>& bytes = gen.code();

  std::size_t span = bytes.size();
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    std::perror("mmap");
    std::exit(2);
  }
  std::memcpy(mem, bytes.data(), span);
  if (mprotect(mem, span, PROT_READ | PROT_EXEC) != 0) {
    std::perror("mprotect");
    std::exit(2);
  }
  auto base = reinterpret_cast<std::uint8_t*>(mem);
  return reinterpret_cast<ParseFn>(base + gen.labelOffset(handles.parseDouble));
}

static void* allocScratch() {
  const std::size_t alignment = 64;
  std::size_t bytes = parse_float::kParseScratchBytes + alignment;
  bytes = (bytes + alignment - 1) / alignment * alignment;
  return std::aligned_alloc(alignment, bytes);
}

// Models the hosted `parseFloatText`: `strtod` must consume the whole text
// (after a `trim`), and a non-finite result is only accepted when the text
// spells `inf`/`infinity`/`nan` (so a silent overflow is a clean error).
static bool hostedAccepts(const std::string& text, std::uint64_t& bits) {
  std::size_t lo = 0, hi = text.size();
  auto ws = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
  };
  while (lo < hi && ws(text[lo])) ++lo;
  while (hi > lo && ws(text[hi - 1])) --hi;
  std::string t = text.substr(lo, hi - lo);
  if (t.empty()) return false;
  char* end = nullptr;
  double v = std::strtod(t.c_str(), &end);
  if (end == t.c_str() || *end != '\0') return false;
  if (std::isinf(v) || std::isnan(v)) {
    std::string lower = t;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!lower.empty() && (lower[0] == '+' || lower[0] == '-')) lower.erase(0, 1);
    if (lower != "inf" && lower != "infinity" && lower != "nan") return false;
  }
  std::memcpy(&bits, &v, 8);
  return true;
}

// Returns true when the parser agrees with the hosted engine: the same bit
// pattern on success, and a failure whenever the hosted engine rejects.
static bool runOne(ParseFn fn, void* scratch, const std::string& text,
                   std::uint64_t& out) {
  std::int64_t bits = 0;
  std::int64_t rc = fn(text.data(), static_cast<std::int64_t>(text.size()), &bits, scratch);
  std::uint64_t want = 0;
  bool accepts = hostedAccepts(text, want);
  if (!accepts) return rc != 0;
  if (rc != 0) return false;
  out = static_cast<std::uint64_t>(bits);
  return out == want;
}

static void testEdgeCases(ParseFn fn, void* scratch) {
  const char* cases[] = {
      "0", "-0", "+0", "0.0", "-0.0",
      "1", "-1", "42", "1000000",
      "2.5", ".5", "5.", "+.5", "-.5e-3",
      "0.1", "0.2", "0.3", "0.1e1",
      "1e0", "1e1", "1e10", "1e308", "1e309", "1e-308", "1e-323", "1e-324",
      "1e-400", "5e-324", "2.4703282292062327e-324", "2.4703282292062328e-324",
      "1.7976931348623157e308", "1.7976931348623158e308",
      "2.2250738585072014e-308", "2.2250738585072011e-308",
      "9007199254740993", "9007199254740992",
      "inf", "-inf", "infinity", "-infinity", "INF", "Infinity", "nan", "NaN", "+nan",
      "0x1.8p1", "0x1p-1", "0x.8p1", "0x1.8", "0x1.8p+2", "0x1p-1074", "0x1p1024",
      "1_000", "abc", "", "   ", "2.5x", "2.5.5", "1e", "0x", "0x1p", "2,5",
      "1\u00002", "1\u0000abc",
  };
  for (const char* c : cases) {
    std::uint64_t got = 0;
    std::string text(c, std::strlen(c));
    bool ok = runOne(fn, scratch, text, got);
    char buf[128];
    std::snprintf(buf, sizeof buf, "edge case '%s'", c);
    check(ok, buf);
  }
}

static void testRandom(ParseFn fn, void* scratch) {
  std::mt19937_64 rng(0xC0FFEE);
  long bad = 0;
  for (int it = 0; it < 500000; ++it) {
    char b[2048];
    int shape = static_cast<int>(rng() % 7);
    if (shape == 0) {
      std::snprintf(b, sizeof b, "%lld", static_cast<long long>(rng()));
    } else if (shape == 1) {
      std::snprintf(b, sizeof b, "%d.%09d", static_cast<int>(rng() % 1000),
                    static_cast<int>(rng() % 1000000000));
    } else if (shape == 2) {
      std::snprintf(b, sizeof b, "%d.%de%+d", static_cast<int>(rng() % 10),
                    static_cast<int>(rng() % 1000000), static_cast<int>(rng() % 700) - 350);
    } else if (shape == 3) {
      int len = 1 + static_cast<int>(rng() % 60);
      for (int k = 0; k < len; ++k) b[k] = static_cast<char>('0' + rng() % 10);
      b[len] = '\0';
      char e[16];
      std::snprintf(e, sizeof e, "e%+d", static_cast<int>(rng() % 700) - 350);
      std::strcat(b, e);
    } else if (shape == 4) {
      double v;
      std::uint64_t r = rng();
      std::memcpy(&v, &r, 8);
      if (!std::isfinite(v)) std::snprintf(b, sizeof b, "%d", static_cast<int>(rng()));
      else std::snprintf(b, sizeof b, "%.17g", v);
    } else if (shape == 5) {
      std::snprintf(b, sizeof b, ".%de%+d", static_cast<int>(rng() % 1000000000),
                    static_cast<int>(rng() % 80) - 40);
    } else {
      // Hex float, sometimes with leading zeros and a long fraction.
      int z = static_cast<int>(rng() % 20);
      int n = 0;
      b[n++] = '0'; b[n++] = 'x';
      for (int k = 0; k < z; ++k) b[n++] = '0';
      int len = 1 + static_cast<int>(rng() % 25);
      for (int k = 0; k < len; ++k) {
        int dv = static_cast<int>(rng() % 16);
        b[n++] = static_cast<char>(dv < 10 ? '0' + dv : 'a' + dv - 10);
      }
      if (rng() % 2) {
        b[n++] = '.';
        int fl = 1 + static_cast<int>(rng() % 40);
        for (int k = 0; k < fl; ++k) {
          int dv = static_cast<int>(rng() % 16);
          b[n++] = static_cast<char>(dv < 10 ? '0' + dv : 'a' + dv - 10);
        }
      }
      b[n] = '\0';
      char e[16];
      std::snprintf(e, sizeof e, "p%+d", static_cast<int>(rng() % 2000) - 1000);
      std::strcat(b, e);
    }
    std::uint64_t got = 0;
    if (!runOne(fn, scratch, b, got)) {
      if (bad < 20) std::printf("MISMATCH [%s]\n", b);
      ++bad;
    }
  }
  char buf[64];
  std::snprintf(buf, sizeof buf, "random parse (%ld mismatches)", bad);
  check(bad == 0, buf);
}

#endif  // PARSE_EXEC_TESTS

int main() {
#if defined(PARSE_EXEC_TESTS)
  ParseFn fn = emitParser();
  void* scratch = allocScratch();
  testEdgeCases(fn, scratch);
  testRandom(fn, scratch);
  std::free(scratch);
#else
  std::printf("skipped: parse-float execution tests need Linux x86-64\n");
#endif
  if (g_failures == 0) {
    std::printf("parse_float_test: all checks passed\n");
    return 0;
  }
  std::printf("parse_float_test: %d failure(s)\n", g_failures);
  return 1;
}
