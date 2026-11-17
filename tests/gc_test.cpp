// Tests for the cycle collector (src/gc.h).
// They count tracked objects instead of measuring memory, so the results are
// deterministic and independent of the allocator.

// Release builds define NDEBUG, which would compile every assert() out and
// leave the suite green without testing anything.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdio>
#include <memory>

#include "gc.h"
#include "value.h"

namespace {

// Collects first so that garbage left by earlier tests does not skew a count.
std::size_t settledCount() {
  gcCollect();
  return gcTrackedCount();
}

void testSelfReferencingArray() {
  const std::size_t base = settledCount();
  {
    ArrayPtr array = makeArray();
    array->items.push_back(Value(array));
  }
  // Reference counting alone leaks it: the array still owns itself.
  assert(gcTrackedCount() == base + 1);
  gcCollect();
  assert(gcTrackedCount() == base);
}

void testSelfReferencingMap() {
  const std::size_t base = settledCount();
  {
    MapPtr map = makeMap();
    map->set("self", Value(map));
  }
  assert(gcTrackedCount() == base + 1);
  gcCollect();
  assert(gcTrackedCount() == base);
}

void testMutualCycle() {
  const std::size_t base = settledCount();
  {
    ArrayPtr a = makeArray();
    MapPtr b = makeMap();
    a->items.push_back(Value(b));
    b->set("back", Value(a));
  }
  assert(gcTrackedCount() == base + 2);
  gcCollect();
  assert(gcTrackedCount() == base);
}

void testAcyclicObjectsFreedWithoutTheCollector() {
  const std::size_t base = settledCount();
  {
    ArrayPtr outer = makeArray();
    outer->items.push_back(Value(makeArray()));
    outer->items.push_back(Value(makeMap()));
  }
  assert(gcTrackedCount() == base);
}

void testReferencedCycleSurvives() {
  const std::size_t base = settledCount();
  ArrayPtr array = makeArray();
  array->items.push_back(Value(array));
  array->items.push_back(Value(int64_t{7}));
  gcCollect();
  assert(gcTrackedCount() == base + 1);
  assert(array->items.size() == 2);
  assert(array->items[1].asInt() == 7);
  assert(array->items[0].isArray() && &array->items[0].arr() == array.get());

  array = ArrayPtr();
  gcCollect();
  assert(gcTrackedCount() == base);
}

void testEverythingReachableFromAnOutsideReferenceSurvives() {
  const std::size_t base = settledCount();
  ArrayPtr root = makeArray();
  {
    MapPtr child = makeMap();
    root->items.push_back(Value(child));
    child->set("parent", Value(root));
  }
  gcCollect();
  assert(gcTrackedCount() == base + 2);
  assert(root->items[0].isMap());
  assert(root->items[0].map().get("parent") != nullptr);

  root = ArrayPtr();
  gcCollect();
  assert(gcTrackedCount() == base);
}

void testGarbageNextToLiveObjects() {
  const std::size_t base = settledCount();
  ArrayPtr live = makeArray();
  live->items.push_back(Value(int64_t{1}));
  {
    ArrayPtr garbage = makeArray();
    garbage->items.push_back(Value(garbage));
  }
  gcCollect();
  assert(gcTrackedCount() == base + 1);
  assert(live->items.size() == 1);
}

// The shape behind every leak this collector exists for: a function stored in
// the cell it captures (a recursive closure).
void testFunctionAndCellCycle() {
  const std::size_t base = settledCount();
  {
    CellPtr cell = makeRef<Cell>();
    FuncPtr function = makeRef<Function>();
    function->name = "f";
    cell->value = Value(function);
    function->upvalues.push_back(cell);
    cell->track();
  }
  assert(gcTrackedCount() == base + 2);
  gcCollect();
  assert(gcTrackedCount() == base);
}

void testCellStaysAliveThroughAClosure() {
  const std::size_t base = settledCount();
  FuncPtr escaped;
  {
    CellPtr cell = makeRef<Cell>();
    cell->track();
    ArrayPtr captured = makeArray();
    captured->items.push_back(Value(int64_t{42}));
    cell->value = Value(captured);
    // The closure stores itself in the cell it captures, so cell and function
    // form a cycle that only the collector can break.
    escaped = makeRef<Function>();
    escaped->upvalues.push_back(cell);
    captured->items.push_back(Value(escaped));
  }
  gcCollect();
  // The outside handle keeps the function, its cell and the captured array
  // alive: three tracked objects reachable from `escaped`.
  assert(gcTrackedCount() == base + 3);
  assert(escaped->upvalues.size() == 1);
  assert(escaped->upvalues[0]->value.isArray());
  assert(escaped->upvalues[0]->value.arr().items[0].asInt() == 42);

  escaped = FuncPtr();
  gcCollect();
  assert(gcTrackedCount() == base);
}

void testRecursiveFunctionsSharingACellIsCollected() {
  const std::size_t base = settledCount();
  for (int i = 0; i < 100; ++i) {
    CellPtr cell = makeRef<Cell>();
    cell->track();
    FuncPtr even = makeRef<Function>();
    FuncPtr odd = makeRef<Function>();
    even->name = "even";
    odd->name = "odd";
    even->upvalues.push_back(cell);
    odd->upvalues.push_back(cell);
    // The cell holds a scope binding both functions, so the two functions and
    // the cell reference each other.
    MapPtr scope = makeMap();
    scope->set("even", Value(even));
    scope->set("odd", Value(odd));
    cell->value = Value(scope);
  }
  assert(gcTrackedCount() == base + 400);
  gcCollect();
  assert(gcTrackedCount() == base);
}

// State captured inside a std::function is invisible to the collector. Such an
// object looks referenced from outside, so it is kept (a leak at worst), never
// freed while something can still reach it.
void testReferencesHiddenInNativesAreNeverFreed() {
  const std::size_t base = settledCount();
  FuncPtr method = makeRef<Function>();
  {
    ArrayPtr array = makeArray();
    array->items.push_back(Value(method));
    method->native = [array](Runtime&, std::vector<Value>&) { return Value(array); };
  }
  method = FuncPtr();
  gcCollect();
  assert(gcTrackedCount() == base + 2);
}

#ifndef OSKEL_GC_STRESS
void testCollectionRunsAutomaticallyAfterEnoughAllocations() {
  const std::size_t base = settledCount();
  for (int i = 0; i < 5000; ++i) {
    ArrayPtr array = makeArray();
    array->items.push_back(Value(array));
    gcCollectIfNeeded();
  }
  // Below the threshold nothing has been collected yet.
  assert(gcTrackedCount() == base + 5000);

  for (int i = 0; i < 20000; ++i) {
    ArrayPtr array = makeArray();
    array->items.push_back(Value(array));
    gcCollectIfNeeded();
  }
  // Past it, the garbage is reclaimed without an explicit gcCollect().
  assert(gcTrackedCount() < base + 15000);
  gcCollect();
  assert(gcTrackedCount() == base);
}
#endif

}  // namespace

int main() {
  testSelfReferencingArray();
  testSelfReferencingMap();
  testMutualCycle();
  testAcyclicObjectsFreedWithoutTheCollector();
  testReferencedCycleSurvives();
  testEverythingReachableFromAnOutsideReferenceSurvives();
  testGarbageNextToLiveObjects();
  testFunctionAndCellCycle();
  testCellStaysAliveThroughAClosure();
  testRecursiveFunctionsSharingACellIsCollected();
  testReferencesHiddenInNativesAreNeverFreed();
#ifndef OSKEL_GC_STRESS
  testCollectionRunsAutomaticallyAfterEnoughAllocations();
#endif
  std::puts("gc tests passed");
  return 0;
}
