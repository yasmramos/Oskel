// Calls a callback with seven integer arguments, then with eight. The eight-arg
// case passes the values through an array so the registration itself stays a
// two-value `extern "C"` call; the callback still receives all eight in
// registers and on the stack, which is what the generated body has to marshal.
int call7(int (*cb)(int, int, int, int, int, int, int), int a, int b, int c,
          int d, int e, int f, int g) {
  return cb(a, b, c, d, e, f, g);
}

int call8(int (*cb)(int, int, int, int, int, int, int, int), const int *args) {
  return cb(args[0], args[1], args[2], args[3], args[4], args[5], args[6],
            args[7]);
}
