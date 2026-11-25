// The AOT arena reports failure as `nullptr` because the freestanding runtime
// has no exceptions. This test pins that contract: an invalid alignment, an
// exhausted region and a null block all fail without throwing.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "native/arena.h"

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

int main() {
  Arena empty(nullptr, 0);
  check(empty.allocate(8) == nullptr, "a null block fails every allocation");
  check(empty.capacity() == 0, "a null block has no capacity");

  std::vector<std::uint8_t> block(64);
  Arena arena(block.data(), block.size());

  check(arena.allocate(8, 0) == nullptr, "alignment 0 is rejected");
  check(arena.allocate(8, 3) == nullptr, "a non-power-of-two alignment is rejected");
  check(arena.allocate(8, 32) != nullptr, "a larger power-of-two alignment is accepted");

  void* first = arena.allocate(8, 8);
  check(first != nullptr, "an aligned allocation succeeds");
  check(reinterpret_cast<std::uintptr_t>(first) % 8 == 0, "the result is aligned");

  const std::size_t beforeZero = arena.used();
  void* zero = arena.allocate(0, 8);
  check(zero != nullptr, "a zero-size allocation succeeds");
  check(zero != first, "a zero-size allocation has a distinct address");
  check(arena.used() > beforeZero, "a zero-size allocation consumes a slot");

  arena.reset();
  check(arena.used() == 0, "reset reclaims the whole region");

  // Exhaustion is a clean `nullptr`, not a wrap-around.
  Arena small(block.data(), 16);
  check(small.allocate(16, 1) != nullptr, "the whole region can be handed out");
  check(small.allocate(1, 1) == nullptr, "an exhausted region returns nullptr");

  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all arena tests passed\n");
  return 0;
}
