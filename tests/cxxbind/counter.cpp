#include "counter.h"

Counter::Counter(int start) : value_(start) {}
Counter::~Counter() = default;

int Counter::value() const { return value_; }
void Counter::add(int n) { value_ += n; }

int add(int a, int b) { return a + b; }
