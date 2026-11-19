/* A representative C header for the bindgen end-to-end test.
 *
 * It mixes the shapes the generator must translate (scalars, pointers,
 * `cstring`, typedefs, a callback, a variadic) with the ones it must skip (a
 * struct by value, a callback returned by value), so the generated module
 * exercises both the `fn` lines and the `// skipped:` comments. The end-to-end
 * runner feeds the output back to `oskel -c`, which is what proves the tool and
 * the compiler agree.
 */

#include <stddef.h>

#define API __attribute__((visibility("default")))

typedef unsigned long size_t;

struct Point {
  int x;
  int y;
};

API int strlen(const char *s);
API void *malloc(size_t n);
API void free(void *p);
API double sqrt(double x);
API float fabsf(float x);
API int strcmp(const char *a, const char *b);
API int crc32(const void *data, unsigned len);
API _Bool is_ready(int id);
API char *strdup(const char *s);

/* A callback parameter is translated to `fn(P...): R`; the shapes it cannot
 * express stay commented out: a struct by value and a callback returned. */
API struct Point make_point(int x, int y);
API int apply(int (*cb)(int), int n);
API int (*signal(int sig, void (*func)(int)))(int);
API void log_msg(const char *fmt, ...);

/* A pointer to a callback, an array of callbacks and a calling convention are
 * all translatable: the first two are opaque `ptr`, the convention is dropped. */
API int atexit(void (**pp)(void));
API void reg(int (WINAPI *cb)(int));
typedef int (CALLBACK *handler_t)(int);
API void deliver(handler_t h);
