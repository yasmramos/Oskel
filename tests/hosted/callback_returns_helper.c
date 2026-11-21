// Exercises every scalar callback return class the marshalling supports:
// cstring (a `char *`), bool, double, float and an opaque pointer.
#include <stdbool.h>

const char *call_cstring(const char *(*cb)(void)) { return cb(); }
int call_bool(bool (*cb)(void)) { return cb() ? 1 : 0; }
double call_double(double (*cb)(double), double x) { return cb(x); }
float call_float(float (*cb)(float), float x) { return cb(x); }
void *call_ptr(void *(*cb)(void)) { return cb(); }
