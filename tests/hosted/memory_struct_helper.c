/* C helpers for the class-MEMORY struct-by-value test (PHASE 7f.7).
 *
 * A struct larger than 16 bytes is passed on the stack and returned through a
 * hidden `sret` pointer, which no libc function exercises, so the test needs
 * functions of its own. They are deliberately trivial: the point is the ABI,
 * not the arithmetic. */

#include <stdint.h>

typedef struct {
  int64_t a, b, c;
} Triple;

typedef struct {
  double x, y, z;
} TripleD;

typedef struct {
  double a, b;
} PairD;

int64_t oskelTestSumTriple(Triple t) { return t.a + t.b + t.c; }

double oskelTestSumTripleD(TripleD t) { return t.x + t.y + t.z; }

Triple oskelTestMakeTriple(int64_t a, int64_t b, int64_t c) {
  Triple t = {a, b, c};
  return t;
}

TripleD oskelTestMakeTripleD(double x, double y, double z) {
  TripleD t = {x, y, z};
  return t;
}

/* A stack-passed struct after a scalar: the scalar must still reach its
 * register while the struct sits at the bottom of the outgoing area. */
int64_t oskelTestScalarThenTriple(int64_t x, Triple t) {
  return x + t.a + t.b + t.c;
}

/* Two stack-passed structs: their places in the outgoing area must not overlap. */
int64_t oskelTestTwoTriples(Triple t, Triple u) {
  return t.a + t.b + t.c + u.a + u.b + u.c;
}

/* A stack-passed struct, a register-class struct and a scalar together, so the
 * two argument paths are exercised in one call. */
double oskelTestTriplePairScalar(Triple t, PairD p, int64_t k) {
  return (double)(t.a + t.b + t.c) + p.a + p.b + (double)k;
}

/* Six integer arguments fill every GPR, so the struct behind them must be the
 * only thing left for the stack. */
int64_t oskelTestSixThenTriple(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                               int64_t f, Triple t) {
  return a + b + c + d + e + f + t.a + t.b + t.c;
}
