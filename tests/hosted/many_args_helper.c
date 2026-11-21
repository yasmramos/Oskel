// C helpers for `tests/native_build_extern_many_args.oskl`. No libc symbol
// takes seven fixed integer arguments, so the seventh/eighth-GPR stack spill
// and a register-class struct that no longer fits the remaining registers need
// a helper of their own.
#include <stdint.h>

typedef struct {
  int64_t a;
  int64_t b;
} OskelPair;

typedef struct {
  int64_t a;
} OskelOne;

int64_t oskelTestSum7(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f,
                      int64_t g) {
  return a + b + c + d + e + f + g;
}

int64_t oskelTestMix8(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f,
                      int64_t g, double h) {
  return a + b + c + d + e + f + g + (int64_t)h;
}

int64_t oskelTestPairAfter6(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f,
                            OskelPair p) {
  return a + b + c + d + e + f + p.a + p.b;
}

int64_t oskelTestPairThenScalar(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                                OskelPair p, int64_t g) {
  return a + b + c + d + e + p.a + p.b + g;
}

int64_t oskelTestOneThenScalar(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                               int64_t f, OskelOne p, int64_t g) {
  return a + b + c + d + e + f + p.a + g;
}
