// Benchmark: append n integers to an array, then sum them. Same algorithm as
// `arrays.oskl`; the growth policy is C's `realloc` doubling.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  const long n = 200000;
  long cap = 0;
  long len = 0;
  long long* xs = NULL;
  for (long i = 0; i < n; i++) {
    if (len == cap) {
      cap = cap == 0 ? 16 : cap * 2;
      xs = realloc(xs, (size_t)cap * sizeof *xs);
      if (xs == NULL) return 70;
    }
    xs[len++] = i;
  }
  long long sum = 0;
  for (long i = 0; i < len; i++) sum += xs[i];
  printf("arrays %lld %ld\n", sum, len);
  free(xs);
  return 0;
}
