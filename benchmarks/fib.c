// Benchmark: naive recursive fibonacci. Same algorithm as `fib.oskl`.
#include <stdio.h>

static long long fib(long long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main(void) {
  printf("fib %lld\n", fib(30));
  return 0;
}
