// Stored callbacks for the AOT callback fixtures: registration and invocation
// are separate extern calls, so the callback runs after the program has
// allocated again.
static void (*stored_void)(void);
static int (*stored_int)(int);

void store_void(void (*cb)(void)) { stored_void = cb; }
void invoke_void(void) {
  if (stored_void) stored_void();
}
void store_int(int (*cb)(int)) { stored_int = cb; }
int invoke_int(int x) { return stored_int ? stored_int(x) : -1; }
