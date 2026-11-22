// A C variadic with a mixed fixed prefix: `int, double, int, ...`.
//
// The Microsoft x64 ABI assigns argument positions to registers regardless of
// class, so the fixed `double` must reach xmm1 (not xmm0) and the tail
// `double` must reach xmm3 (and its integer duplicate r9), while the tail
// `int` spills to the stack. System V assigns the classes independently, so the
// same helper exercises both rules and must return the same sum on either.
#include <stdarg.h>

double mixed_var_sum(int a, double b, int c, ...) {
  va_list ap;
  va_start(ap, c);
  double x = va_arg(ap, double);
  int y = va_arg(ap, int);
  va_end(ap);
  return (double)a + b + (double)c + x + (double)y;
}
