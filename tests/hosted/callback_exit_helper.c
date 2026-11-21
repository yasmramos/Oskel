// Registers the callback with `atexit`, so C invokes it after the Oskel chunk
// has returned. `atexit` lives in the C library the hosted binary already links,
// not in the Oskel symbol registry, which is why the test drives it from here.
#include <stdlib.h>

static void (*exit_callback)(void);

static void run_exit_callback(void) {
  if (exit_callback) exit_callback();
}

void register_exit_callback(void (*cb)(void)) {
  exit_callback = cb;
  atexit(run_exit_callback);
}
