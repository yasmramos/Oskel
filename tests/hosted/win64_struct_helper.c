/* Microsoft x64 by-value struct shapes for the AOT Win64 fixture. A 1/2/4/8-byte
 * aggregate travels in one integer register; a 16-byte one travels by reference
 * and returns through a hidden `sret` pointer. */
typedef struct {
  int a;
  int b;
} Pair;

typedef struct {
  long long a;
  long long b;
} Big;

__declspec(dllexport) int pair_sum(Pair p) { return p.a + p.b; }

__declspec(dllexport) Pair pair_make(int a, int b) {
  Pair p;
  p.a = a;
  p.b = b;
  return p;
}

__declspec(dllexport) long long big_sum(Big b) { return b.a + b.b; }

__declspec(dllexport) Big big_make(long long a, long long b) {
  Big r;
  r.a = a;
  r.b = b;
  return r;
}
