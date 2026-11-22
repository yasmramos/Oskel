#pragma once

// A tiny C++ library for the bridge end-to-end test. The class is deliberately
// non-trivial (constructor, destructor, const method, mutating method) and the
// free function exercises the non-class path.
class Counter {
 public:
  explicit Counter(int start);
  ~Counter();
  int value() const;
  void add(int n);

 private:
  int value_;
};

int add(int a, int b);
