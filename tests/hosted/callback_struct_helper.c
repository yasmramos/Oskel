// C functions that take a callback returning a struct by value. The shape is
// what the System V AMD64 ABI distinguishes: `Point` (8 bytes, INTEGER
// eightbyte) and `Mixed` (INTEGER + SSE) come back in registers, while `Big`
// (24 bytes) is class MEMORY and travels through the hidden `sret` pointer.
typedef struct {
  int x;
  int y;
} Point;

typedef struct {
  long a;
  long b;
  long c;
} Big;

typedef struct {
  int a;
  double b;
} Mixed;

Point call_point(Point (*cb)(void)) { return cb(); }
Big call_big(Big (*cb)(void)) { return cb(); }
Mixed call_mixed(Mixed (*cb)(void)) { return cb(); }
