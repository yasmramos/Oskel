// Tests for the cycle collector (src/core/gc.h).
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

#include "core/gc.h"
#include "core/value.h"

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

// `track()` is idempotent: an object already in the registry must not be linked
// a second time. A second link would splice the doubly linked list onto
// itself (the node would point at itself, `prev`/`next` corrupting the
// neighbours), and `trackedCount` would count it twice -- the collector would
// then free an object that is still reachable. Only `link()` lacks the guard;
// `track()` has it, and this pins it (BACKLOG §8 item 3).
void testTrackIsIdempotent() {
  const std::size_t base = settledCount();
  {
    ArrayPtr array = makeArray();
    array->track();
    array->track();
    array->track();
    assert(gcTrackedCount() == base + 1);
  }
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

// A `clear` that allocates. `GcNode::ops` is a public pointer, so a plain map
// can be given a table whose clear registers a new tracked array, which moves
// `registryHead` mid-pass. The collector gathers the garbage into vectors and
// pins it before clearing, so nothing is walking the live registry while it
// grows (BACKLOG §8 item 4).
std::vector<ArrayPtr>* spawnGraveyard = nullptr;

void allocatingClear(GcNode& node) {
  static_cast<Map&>(node).clear();
  if (spawnGraveyard) spawnGraveyard->push_back(makeArray());
}

std::shared_ptr<void> retainingPin(GcNode& node) {
  Ref<Map> handle = Ref<Map>::retaining(static_cast<Map*>(&node));
  return std::shared_ptr<void>(new Ref<Map>(std::move(handle)),
                               [](void* p) { delete static_cast<Ref<Map>*>(p); });
}

std::size_t plainRefCount(const GcNode& node) { return static_cast<const Map&>(node).refs; }

void mapVisitChildren(const GcNode& node, GcNode::Visitor visitor, void* context) {
  for (const auto& entry : static_cast<const Map&>(node).entries) {
    gcVisit(entry.second, visitor, context);
  }
}

const GcNode::Ops allocatingMapOps = {plainRefCount, mapVisitChildren, allocatingClear,
                                      retainingPin};

void testObjectsCreatedDuringCollection() {
  const std::size_t base = settledCount();
  {
    MapPtr map = makeMap();
    map->set("self", Value(map));
    map->ops = &allocatingMapOps;
  }
  assert(gcTrackedCount() == base + 1);

  // The map is garbage, so the pass clears it, and each clear allocates a
  // tracked array. Those arrays are held outside the collector, so they are
  // alive; the pass must finish with them intact and the count honest.
  std::vector<ArrayPtr> spawned;
  spawnGraveyard = &spawned;
  gcCollect();
  spawnGraveyard = nullptr;

  assert(spawned.size() == 1);
  assert(gcTrackedCount() == base + 1);
  // Once the outside reference is gone the next pass reclaims it, so an object
  // created during a collection is not leaked.
  spawned.clear();
  gcCollect();
  assert(gcTrackedCount() == base);
}

}  // namespace

int main() {
  testSelfReferencingArray();
  testTrackIsIdempotent();
  testSelfReferencingMap();
  testMutualCycle();
  testObjectsCreatedDuringCollection();
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
