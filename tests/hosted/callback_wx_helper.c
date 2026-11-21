// Reports whether the page holding `cb` is writable and executable at the same
// time. The AOT callback stub must be W^X: read+execute after `mprotect`, never
// read+write+execute.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int callback_stub_is_wx(void (*cb)(void)) {
  FILE* maps = fopen("/proc/self/maps", "r");
  if (!maps) return -1;
  char line[512];
  uintptr_t addr = (uintptr_t)cb;
  while (fgets(line, sizeof line, maps)) {
    unsigned long start = 0, end = 0;
    char perms[8] = {0};
    if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
    if (addr < start || addr >= end) continue;
    fclose(maps);
    return strchr(perms, 'w') != NULL && strchr(perms, 'x') != NULL;
  }
  fclose(maps);
  return -1;
}
