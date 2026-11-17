// Tests for the `bindgen` tool (the C-header binding generator).
//
// The subject is `bindgen::generate`: a C header in, an Oskel module out. The
// tool is standalone (it does not link the engine), so this test links
// `bindgen_lib` only. Each case checks the generated text for the exact `fn`
// line the FFI vocabulary calls for, and the end-to-end CTest runner
// (`run_bindgen_case.cmake`) closes the loop by compiling the output with
// `oskel -c`.

#include <cstdio>
#include <string>
#include <vector>

#include "bindgen.h"

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

// Whether `haystack` contains `needle`.
static bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// One case: generate from `source` and assert each `expect` line is present.
static void expectGenerated(const char* what, const std::string& source,
                            const std::vector<std::string>& expects) {
  const bindgen::Result result = bindgen::generate(source, "test.h");
  for (const std::string& line : expects) {
    if (!contains(result.text, line)) {
      std::printf("FAIL: %s: missing '%s'\n---\n%s\n---\n", what, line.c_str(),
                  result.text.c_str());
      ++g_failures;
      return;
    }
  }
  std::printf("ok: %s\n", what);
}

int main() {
  // The scalar vocabulary: a C type maps onto the Oskel FFI name.
  expectGenerated("scalars",
                  "int a(char c, short s, int i, long l);\n"
                  "unsigned int u();\n"
                  "unsigned char uc();\n"
                  "long long ll();\n"
                  "float f();\n"
                  "double d();\n"
                  "void v();\n",
                  {"fn a(c: int8, s: int16, i: int32, l: int64): int32;",
                   "fn u(): uint32;", "fn uc(): uint8;", "fn ll(): int64;",
                   "fn f(): float;", "fn d(): double;", "fn v(): void;"});

  // Pointers: `void *` is `ptr`, `char *` is `cstring`, anything else is `ptr`.
  expectGenerated("pointers",
                  "void *alloc(unsigned long n);\n"
                  "const char *name(int id);\n"
                  "int *ints(void);\n"
                  "void **handles(void);\n",
                  {"fn alloc(n: uint64): ptr;", "fn name(id: int32): cstring;",
                   "fn ints(): ptr;", "fn handles(): ptr;"});

  // A typedef this header declared is resolved, including through pointers.
  expectGenerated("typedef",
                  "typedef unsigned long size_t;\n"
                  "typedef int myint;\n"
                  "typedef char *string_t;\n"
                  "size_t len(const char *s);\n"
                  "myint twice(myint x);\n"
                  "string_t copy(string_t s);\n",
                  {"fn len(s: cstring): size_t;", "fn twice(x: int32): int32;",
                   "fn copy(s: cstring): cstring;"});

  // The well-known `stdint.h`/`stddef.h` names resolve without a typedef.
  expectGenerated("stdint",
                  "uint8_t a(void);\nuint16_t b(void);\nuint32_t c(void);\n"
                  "uint64_t d(void);\nint8_t e(void);\nintptr_t g(void);\n",
                  {"fn a(): uint8;", "fn b(): uint16;", "fn c(): uint32;",
                   "fn d(): uint64;", "fn e(): int8;", "fn g(): int64;"});

  // Qualifiers, storage classes and attributes are dropped.
  expectGenerated("qualifiers",
                  "extern int f(const volatile int x);\n"
                  "__attribute__((visibility(\"default\"))) int g(int y);\n"
                  "static inline int h(int z) { return z; }\n",
                  {"fn f(x: int32): int32;", "fn g(y: int32): int32;"});
  {
    const bindgen::Result result =
        bindgen::generate("static inline int h(int z) { return z; }\n", "t.h");
    check(result.emitted == 0, "a function definition is not emitted");
  }

  // An array parameter decays to a pointer.
  expectGenerated("array parameter", "int sum(int values[], int n);\n",
                  {"fn sum(values: ptr, n: int32): int32;"});

  // `(void)` and `()` both mean no parameters.
  expectGenerated("no parameters", "int a(void);\nint b();\n",
                  {"fn a(): int32;", "fn b(): int32;"});

  // A variadic declaration is emitted commented out, so the module compiles
  // even where libffi (and the System V ABI) is absent.
  {
    const bindgen::Result result =
        bindgen::generate("int printf(const char *fmt, ...);\n", "t.h");
    check(contains(result.text, "// fn printf(fmt: cstring, ...): int32;"),
          "a variadic is commented out");
    check(result.commented == 1, "the variadic is counted as commented");
    check(result.emitted == 0, "a commented variadic is not counted as emitted");
  }

  // Unsupported declarations are skipped with a comment, never mis-translated.
  {
    const bindgen::Result result = bindgen::generate(
        "struct Point { int x; int y; };\n"
        "struct Point make(int x, int y);\n"
        "long double weird(long double x);\n"
        "void takes_cb(int (*cb)(int), int n);\n",
        "t.h");
    check(contains(result.text, "// skipped: make:"), "a struct-by-value return is skipped");
    check(contains(result.text, "// skipped: weird:"), "a long double is skipped");
    check(contains(result.text, "// skipped: takes_cb:"), "a callback is skipped");
    check(result.emitted == 0, "nothing unsupported is emitted as code");
    check(result.skipped == 3, "the three skips are counted");
  }

  // A declaration named after an Oskel keyword cannot be bound.
  {
    const bindgen::Result result = bindgen::generate("int for(int x);\n", "t.h");
    check(contains(result.text, "// skipped: for:"), "an Oskel keyword name is skipped");
    check(result.emitted == 0, "the keyword-named function is not emitted");
  }

  // A duplicate declaration is emitted once.
  {
    const bindgen::Result result = bindgen::generate("int f(int x);\nint f(int y);\n", "t.h");
    check(result.emitted == 1, "a duplicate is emitted once");
    check(result.skipped == 1, "the duplicate is counted as skipped");
  }

  // A header with no bindable declaration still produces a valid module.
  {
    const bindgen::Result result = bindgen::generate("int g_value;\n", "t.h");
    check(contains(result.text, "extern \"C\" {"), "an empty module still has the block");
    check(result.emitted == 0, "a variable is not a binding");
  }

  // A C++ header: the `extern "C" { ... }` wrapper is unwrapped, and a
  // declaration after the block is still seen.
  expectGenerated("c++ extern block",
                  "extern \"C\" {\nint inside(int x);\n}\nint outside(int y);\n",
                  {"fn inside(x: int32): int32;", "fn outside(y: int32): int32;"});

  // Preprocessor directives and comments are ignored.
  expectGenerated("preprocessor",
                  "#include <stddef.h>\n"
                  "#define API __attribute__((visibility(\"default\")))\n"
                  "// a comment\n"
                  "/* a block\n   comment */\n"
                  "API int f(int x);\n",
                  {"fn f(x: int32): int32;"});

  // A macro the tool does not expand, sitting before the type, is dropped
  // rather than swallowing the declaration.
  expectGenerated("macro prefix", "#define EXPORT\nEXPORT int f(int x);\n",
                  {"fn f(x: int32): int32;"});

  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all bindgen tests passed\n");
  return 0;
}
