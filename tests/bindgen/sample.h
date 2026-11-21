/* A representative C header for the bindgen end-to-end test.
 *
 * It mixes the shapes the generator must translate (scalars, pointers,
 * `cstring`, typedefs, a struct by value, an anonymous struct named by its
 * typedef, an enum, a callback, a variadic) with the ones it must skip (a
 * union, a struct with an array field, a nested struct, a bitfield, a callback
 * returned by value), so the generated module exercises the `struct` and `let`
 * lines, the `fn` lines and the `// skipped:` comments. The end-to-end runner
 * feeds the output back to `oskel -c`, which is what proves the tool and the
 * compiler agree.
 */

#include <stddef.h>

#define API __attribute__((visibility("default")))

/* Object-like macros whose replacement is a single literal become module
 * constants. The function-like macro and the attribute above are not constants
 * and must not be emitted. */
#define SAMPLE_ANSWER 42
#define SAMPLE_HEX 0x2A
#define SAMPLE_NAME "sample"
#define SAMPLE_FN(x) ((x) + 1)

typedef unsigned long size_t;

struct Point {
  int x;
  int y;
};

/* An anonymous body takes the typedef's name, and an enum is an `int` at the
 * ABI whose members become module constants. */
typedef struct {
  int quot;
  int rem;
} div_t;

enum Color { RED, GREEN = 5, BLUE };

API int strlen(const char *s);
API void *malloc(size_t n);
API void free(void *p);
API double sqrt(double x);
API float fabsf(float x);
API int strcmp(const char *a, const char *b);
API int crc32(const void *data, unsigned len);
API _Bool is_ready(int id);
API char *strdup(const char *s);

/* A struct by value, an enum and a callback parameter are all translated. */
API struct Point make_point(int x, int y);
API div_t div(int num, int denom);
API enum Color pick_color(int seed);
API int apply(int (*cb)(int), int n);
API void log_msg(const char *fmt, ...);

/* A pointer to a callback, an array of callbacks and a calling convention are
 * all translatable: the first two are opaque `ptr`, the convention is dropped. */
API int atexit(void (**pp)(void));
API void reg(int (WINAPI *cb)(int));
typedef int (CALLBACK *handler_t)(int);
API void deliver(handler_t h);

/* Reported instead of mistranslated. */
union Value {
  int i;
  float f;
};

struct Named {
  char name[32];
  int x;
};

struct Nested {
  struct Point inner;
  int x;
};

struct Bits {
  int a : 3;
  int b : 5;
};

API int (*signal(int sig, void (*func)(int)))(int);
API union Value get_value(void);
API struct Named get_named(void);
API struct Nested get_nested(void);
API struct Bits get_bits(void);
