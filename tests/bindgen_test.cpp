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
#include "ffi_types.h"  // the ABI capabilities that decide commented vs emitted

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

  // Only a plain `char *` is a NUL-terminated string; the explicitly signed
  // forms are byte buffers and cross as `ptr`.
  expectGenerated("byte pointers",
                  "void text(char *out);\n"
                  "void bytes(unsigned char *out);\n"
                  "void sbytes(signed char *out);\n"
                  "void texts(char **out);\n",
                  {"fn text(out: cstring): void;", "fn bytes(out: ptr): void;",
                   "fn sbytes(out: ptr): void;", "fn texts(out: ptr): void;"});

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

  // A variadic declaration is emitted where the build has an x86-64 descriptor
  // stub and commented out elsewhere, so the module compiles everywhere.
  {
    const bindgen::Result result =
        bindgen::generate("int printf(const char *fmt, ...);\n", "t.h");
    if (ffi::kCVariadicSupported) {
      check(contains(result.text, "fn printf(fmt: cstring, ...): int32;"),
            "a variadic is emitted");
      check(!contains(result.text, "// fn printf"), "an emitted variadic is not commented");
      check(result.commented == 0, "an emitted variadic is not counted as commented");
      check(result.emitted == 1, "an emitted variadic is counted as emitted");
    } else {
      check(contains(result.text, "// fn printf(fmt: cstring, ...): int32;"),
            "a variadic is commented out");
      check(result.commented == 1, "the variadic is counted as commented");
      check(result.emitted == 0, "a commented variadic is not counted as emitted");
    }
  }

  // Unsupported declarations are skipped with a comment, never mis-translated.
  // A struct by value and a callback parameter are both translated now.
  {
    const bindgen::Result result = bindgen::generate(
        "struct Point { int x; int y; };\n"
        "struct Point make(int x, int y);\n"
        "long double weird(long double x);\n"
        "void takes_cb(int (*cb)(int), int n);\n",
        "t.h");
    check(contains(result.text, "struct Point {"), "a bindable struct is declared");
    // A struct by value is in the vocabulary everywhere, but only a build whose
    // marshalling implements the System V AMD64 ABI can carry it: elsewhere the
    // tool writes the signature commented out instead of emitting a module the
    // compiler refuses (the end-to-end test feeds the output back to `oskel -c`).
    if (ffi::kStructByValueSupported) {
      check(contains(result.text, "fn make(x: int32, y: int32): Point;"),
            "a struct-by-value return is emitted with its struct name");
      check(result.emitted == 2, "the struct and the callback declarations are emitted");
      check(result.commented == 0, "nothing is commented out on a System V build");
    } else {
      check(contains(result.text, "// fn make(x: int32, y: int32): Point;"),
            "a struct-by-value return is commented out where the ABI cannot carry it");
      check(contains(result.text, "cannot cross the C boundary by value"),
            "the reason is the compiler's own, not a second guess");
      check(result.emitted == 1, "only the callback declaration is emitted");
      check(result.commented == 1, "the struct-by-value declaration is counted as commented");
    }
    check(contains(result.text, "// skipped: weird:"), "a long double is skipped");
    check(contains(result.text, "fn takes_cb(cb: fn(int32): int32, n: int32): void;"),
          "a callback parameter is generated as `fn(P...): R`");
    check(result.structs == 1, "the declared struct is counted");
    check(result.skipped == 1, "only the long double is skipped");
  }

  // An anonymous struct named by its typedef is declared under that name, and a
  // later bare reference reuses the body read earlier in the file.
  {
    const bindgen::Result result = bindgen::generate(
        "typedef struct { int quot; int rem; } div_t;\n"
        "div_t div(int num, int denom);\n",
        "t.h");
    check(contains(result.text, "struct div_t {"), "an anonymous struct takes its typedef name");
    check(contains(result.text, "fn div(num: int32, denom: int32): div_t;"),
          "the typedef names the struct in the signature");
    check(result.structs == 1, "the typedef-named struct is counted");
  }

  // A C enum is an `int` at the ABI: the members become module constants and the
  // declaration maps to `int32` (BACKLOG §7.10).
  {
    const bindgen::Result result = bindgen::generate(
        "enum Color { RED, GREEN = 5, BLUE };\n"
        "enum Color pick(int seed);\n",
        "t.h");
    check(contains(result.text, "let RED = 0;"), "the first enum member starts at zero");
    check(contains(result.text, "let GREEN = 5;"), "an explicit enum value is kept");
    check(contains(result.text, "let BLUE = 6;"), "the members after an explicit one continue");
    check(contains(result.text, "fn pick(seed: int32): int32;"),
          "an enum-typed function maps to int32");
    check(result.constants == 3, "the enum members are counted");
  }

  // An object-like macro whose replacement is a literal becomes a module
  // constant. A function-like macro and an expression are not constants.
  {
    const bindgen::Result result = bindgen::generate(
        "#define ANSWER 42\n"
        "#define HEX 0xFF\n"
        "#define OCT 0755\n"
        "#define NEG -7\n"
        "#define FLOAT 1.5f\n"
        "#define EXP 1e3\n"
        "#define STR \"ok\"\n"
        "#define CH 'A'\n"
        "#define FN(x) ((x)+1)\n"
        "#define EXPR (1 << 2)\n"
        "#define EMPTY\n"
        "#define ATTR __attribute__((visibility(\"default\")))\n",
        "t.h");
    check(contains(result.text, "let ANSWER = 42;"), "an integer macro is a constant");
    check(contains(result.text, "let HEX = 255;"), "a hex macro is emitted as decimal");
    check(contains(result.text, "let OCT = 493;"), "an octal macro is emitted as decimal");
    check(contains(result.text, "let NEG = -7;"), "a signed macro keeps its sign");
    check(contains(result.text, "let FLOAT = 1.5;"), "a float suffix is dropped");
    check(contains(result.text, "let EXP = 1.0e3;"), "an exponent-only float gets a dot");
    check(contains(result.text, "let STR = \"ok\";"), "a string macro is kept");
    check(contains(result.text, "let CH = 65;"), "a character macro is its integer code");
    check(!contains(result.text, "let FN"), "a function-like macro is not a constant");
    check(!contains(result.text, "let EXPR"), "an expression macro is not a constant");
    check(!contains(result.text, "let EMPTY"), "a value-less macro is not a constant");
    check(!contains(result.text, "let ATTR"), "an attribute macro is not a constant");
    check(result.macroConstants == 8, "the literal macros are counted");
  }

  // A macro that collides with a function keeps the function, and a macro whose
  // name is a reserved Oskel keyword cannot be a binding.
  {
    const bindgen::Result result = bindgen::generate(
        "#define ANSWER 42\nint ANSWER(void);\n#define for 1\n", "t.h");
    check(!contains(result.text, "let ANSWER"), "a colliding macro is not emitted");
    check(contains(result.text, "fn ANSWER(): int32;"), "the function keeps the name");
    check(!contains(result.text, "let for"), "a reserved keyword is not emitted");
    check(result.macroConstants == 0, "the colliding macro is not counted");
  }

  // `long` and `wchar_t` follow the C data model.
  {
    const std::string header =
        "long a(void);\nunsigned long b(void);\nlong long c(void);\n"
        "unsigned long long d(void);\nwchar_t e(void);\n";
    const bindgen::Result lp64 = bindgen::generate(header, "t.h");
    check(contains(lp64.text, "fn a(): int64;"), "LP64 long is 64-bit");
    check(contains(lp64.text, "fn b(): uint64;"), "LP64 unsigned long is 64-bit");
    check(contains(lp64.text, "fn c(): int64;"), "long long is 64-bit");
    check(contains(lp64.text, "fn d(): uint64;"), "unsigned long long is 64-bit");
    check(contains(lp64.text, "fn e(): int32;"), "LP64 wchar_t is 32-bit");
    bindgen::Options options;
    options.target = bindgen::Target::Llp64;
    const bindgen::Result llp64 = bindgen::generate(header, "t.h", options);
    check(contains(llp64.text, "fn a(): int32;"), "LLP64 long is 32-bit");
    check(contains(llp64.text, "fn b(): uint32;"), "LLP64 unsigned long is 32-bit");
    check(contains(llp64.text, "fn c(): int64;"), "long long stays 64-bit on LLP64");
    check(contains(llp64.text, "fn d(): uint64;"), "unsigned long long stays 64-bit on LLP64");
    check(contains(llp64.text, "fn e(): uint16;"), "LLP64 wchar_t is 16-bit");
  }

  // A body the Oskel `struct` (a map of whole scalars) cannot lay out is
  // reported, never declared with a wrong layout.
  {
    const bindgen::Result result = bindgen::generate(
        "struct Point { int x; int y; };\n"
        "union Value { int i; float f; };\n"
        "struct Named { char name[32]; int x; };\n"
        "struct Nested { struct Point inner; int x; };\n"
        "struct Bits { int a : 3; int b : 5; };\n"
        "union Value get_value(void);\n"
        "struct Named get_named(void);\n"
        "struct Nested get_nested(void);\n"
        "struct Bits get_bits(void);\n",
        "t.h");
    check(contains(result.text, "// skipped: get_value:"), "a union is reported");
    check(contains(result.text, "// skipped: get_named:"), "an array field is reported");
    check(contains(result.text, "// skipped: get_nested:"), "a nested struct is reported");
    check(contains(result.text, "// skipped: get_bits:"), "a bitfield is reported");
    check(!contains(result.text, "struct Value {"), "a union is not declared as a struct");
    check(!contains(result.text, "struct Named {"),
          "a struct with an array field is not declared");
    check(!contains(result.text, "struct Nested {"), "a nested struct is not declared");
    check(!contains(result.text, "struct Bits {"), "a struct with a bitfield is not declared");
    check(result.structs == 1, "only the bindable struct is declared");
    check(result.skipped == 4, "the four unbindable bodies are counted");
  }

  // A function *returning* a function pointer is a declaration the vocabulary
  // cannot express: it must be reported, not dropped in silence. A
  // function-pointer *variable* is not a binding and stays silent.
  {
    const bindgen::Result result = bindgen::generate(
        "int (*bsd_signal(int sig, void (*func)(int)))(int);\n"
        "char *(*strlist(int n))(int, int);\n"
        "int (*fp_var)(void);\n"
        "void (*handler)(int);\n"
        "int normal(int x);\n",
        "t.h");
    check(contains(result.text, "// skipped: bsd_signal: returns a callback"),
          "a function returning a function pointer is skipped, not dropped");
    check(contains(result.text, "// skipped: strlist: returns a callback"),
          "a function returning a pointer-to-function pointer is skipped too");
    check(!contains(result.text, "fp_var"), "a function-pointer variable is not reported");
    check(!contains(result.text, "handler"), "a function-pointer variable is not reported");
    check(contains(result.text, "fn normal(x: int32): int32;"),
          "the bindable declaration is still emitted");
    check(result.emitted == 1, "only the bindable declaration is emitted");
    check(result.skipped == 2, "the two function-pointer returns are counted");
  }

  // A type the vocabulary does not know is reported with a well-formed reason,
  // and a modifier before it does not make the declaration vanish.
  {
    const bindgen::Result result = bindgen::generate(
        "int uses_unknown(MyType x);\n"
        "unsigned __int64 win1(void);\n"
        "unsigned len(int x);\n",
        "t.h");
    check(contains(result.text, "// skipped: uses_unknown: parameter type 'MyType' is not a known type"),
          "an unknown parameter type is reported");
    check(contains(result.text, "// skipped: win1: return type '__int64' is not a known type"),
          "an unknown type after a modifier is reported, not dropped");
    check(contains(result.text, "fn len(x: int32): uint32;"),
          "'unsigned len' is still the implicit-int declarator");
    check(result.skipped == 2, "the two unknown types are counted");
  }

  // A function-pointer typedef used as a parameter type is resolved to a
  // callback, exactly like the declarator written inline.
  {
    const bindgen::Result result = bindgen::generate(
        "typedef int (*cb_t)(int);\nint register_cb(cb_t cb);\n", "t.h");
    check(contains(result.text, "fn register_cb(cb: fn(int32): int32): int32;"),
          "a function-pointer typedef type is generated as a callback");
    check(result.emitted == 1, "the typedef-typed declaration is emitted");
  }

  // The callback shapes the engine's vocabulary accepts: a `void` return, a
  // pointer parameter and return, and a function-*type* parameter (which C
  // adjusts to a function pointer).
  {
    const bindgen::Result result = bindgen::generate(
        "int atexit(void (*func)(void));\n"
        "void *(*start_fn)(void *);\n"
        "int pthread_create(void *t, const void *a, void *(*start)(void *), void *arg);\n"
        "void takes_fn_type(int foo(int, double));\n",
        "t.h");
    check(contains(result.text, "fn atexit(func: fn(): void): int32;"),
          "a callback with no parameters and a void return is generated");
    check(contains(result.text, "fn pthread_create(t: ptr, a: ptr, start: fn(ptr): ptr, arg: ptr): int32;"),
          "a callback with a pointer parameter and return is generated");
    check(contains(result.text, "fn takes_fn_type(foo: fn(int32, double): int32): void;"),
          "a function-type parameter is generated as a callback");
  }

  // The callback shapes the engine rejects: a callback as a return type, a
  // callback that takes a callback, and a C variadic callback.
  {
    const bindgen::Result result = bindgen::generate(
        "int (*signal(int, void (*)(int)))(int);\n"
        "void nested(int (*cb)(int (*inner)(int)));\n"
        "void variadic_cb(int (*cb)(int, ...));\n",
        "t.h");
    check(contains(result.text, "// skipped: signal: returns a callback"),
          "a function returning a callback is reported");
    check(contains(result.text, "// skipped: nested: a callback that takes a callback"),
          "a nested callback is reported");
    check(contains(result.text, "// skipped: variadic_cb: a C variadic callback"),
          "a variadic callback is reported");
    check(result.emitted == 0, "none of the rejected callback shapes is emitted");
    check(result.skipped == 3, "the three rejected shapes are counted");
  }

  // A struct by value inside a callback: the callback vocabulary is scalar-only,
  // so the compiler rejects the declaration however the struct is laid out. The
  // tool must quote that reason instead of emitting the callback.
  {
    const bindgen::Result result = bindgen::generate(
        "struct P { int x; int y; };\n"
        "void takes_cb(void (*cb)(struct P), int n);\n",
        "t.h");
    check(contains(result.text, "// skipped: takes_cb: callback parameter type 'P' "
                                "cannot cross the C boundary"),
          "a struct inside a callback is reported with the compiler's reason");
    check(!contains(result.text, "fn takes_cb("), "the rejected callback is not emitted");
    check(result.emitted == 0, "nothing is emitted for the rejected declaration");
  }

  // A pointer *to* a function pointer (a second `*`) and an array of function
  // pointers are opaque pointers, not callbacks: the FFI passes them as `ptr`.
  // The same holds when the callback comes from a `typedef`.
  {
    const bindgen::Result result = bindgen::generate(
        "void takes_pp(int (**pp)(int));\n"
        "void takes_arr(int (*arr[4])(int));\n"
        "typedef int (**ppcb_t)(int);\n"
        "int reg(ppcb_t p);\n"
        "typedef int (*cb_t)(int);\n"
        "void takes_pcb(cb_t *pcb);\n"
        "void takes_cb_arr(cb_t cbs[4]);\n"
        "void takes_ppcb(cb_t **ppcb);\n"
        "void takes_const_pcb(const cb_t *pcb);\n"
        "void takes_cb(cb_t cb);\n",
        "t.h");
    check(contains(result.text, "fn takes_pp(pp: ptr): void;"),
          "a pointer to a function pointer is an opaque ptr");
    check(contains(result.text, "fn takes_arr(arr: ptr): void;"),
          "an array of function pointers is an opaque ptr");
    check(contains(result.text, "fn reg(p: ptr): int32;"),
          "a pointer-to-function-pointer typedef is an opaque ptr");
    check(contains(result.text, "fn takes_pcb(pcb: ptr): void;"),
          "a pointer to a callback typedef is an opaque ptr");
    check(contains(result.text, "fn takes_cb_arr(cbs: ptr): void;"),
          "an array of callback typedefs decays to an opaque ptr");
    check(contains(result.text, "fn takes_ppcb(ppcb: ptr): void;"),
          "a pointer to a pointer to a callback typedef is an opaque ptr");
    check(contains(result.text, "fn takes_const_pcb(pcb: ptr): void;"),
          "a const-qualified pointer to a callback typedef is an opaque ptr");
    check(contains(result.text, "fn takes_cb(cb: fn(int32): int32): void;"),
          "the callback typedef itself is still a callback");
  }

  // A function *type* parameter with redundant parentheses is the same type as
  // the unparenthesized form: C adjusts `int (cb)(int)` and `int cb(int)` alike
  // to a function pointer, so both are callbacks.
  {
    const bindgen::Result result = bindgen::generate(
        "void paren(int (cb)(int));\n"
        "void paren_void(int (cb)(void));\n"
        "void plain(int cb(int));\n",
        "t.h");
    check(contains(result.text, "fn paren(cb: fn(int32): int32): void;"),
          "a parenthesized function-type parameter is a callback");
    check(contains(result.text, "fn paren_void(cb: fn(): int32): void;"),
          "a parenthesized function type with `(void)` takes no parameters");
    check(contains(result.text, "fn plain(cb: fn(int32): int32): void;"),
          "the unparenthesized form stays a callback");
  }

  // A calling convention is dropped, not treated as the parameter's name, and
  // it must not end the specifier list: a declaration carrying one is still
  // emitted, never dropped in silence.
  {
    const bindgen::Result result = bindgen::generate(
        "int __stdcall f1(int a);\n"
        "int __cdecl f2(int a);\n"
        "int __fastcall f3(int a);\n"
        "int WINAPI f4(int a);\n"
        "void reg1(int (WINAPI *cb)(int));\n"
        "void reg2(void (CALLBACK *cb)(void));\n"
        "typedef void (CALLBACK *handler_t)(int);\n"
        "void reg3(handler_t h);\n",
        "t.h");
    check(contains(result.text, "fn f1(a: int32): int32;"), "__stdcall is dropped, not the parameter name");
    check(contains(result.text, "fn f2(a: int32): int32;"), "__cdecl is dropped");
    check(contains(result.text, "fn f3(a: int32): int32;"), "__fastcall is dropped");
    check(contains(result.text, "fn f4(a: int32): int32;"), "a WINAPI macro is dropped");
    check(contains(result.text, "fn reg1(cb: fn(int32): int32): void;"),
          "a calling convention inside a callback declarator is dropped");
    check(contains(result.text, "fn reg2(cb: fn(): void): void;"),
          "a CALLBACK convention inside a callback declarator is dropped");
    check(contains(result.text, "fn reg3(h: fn(int32): void): void;"),
          "a callback typedef carrying a convention is still a callback");
    check(!contains(result.text, "WINAPI:"), "the convention never becomes a parameter name");
    check(!contains(result.text, "CALLBACK:"), "the convention never becomes a parameter name");
    check(result.skipped == 0, "no declaration carrying a convention is dropped");
  }

  // A declaration is never dropped in silence. A function-like macro prefix is
  // reported (the tool has no preprocessor to expand it), and a declaration
  // whose type it cannot resolve is reported too -- both used to vanish.
  {
    const bindgen::Result result = bindgen::generate(
        "#define MY_API(type) type\n"
        "MY_API(int) f2(int a);\n"
        "Foo (*fp)(void);\n"
        "Foo plain(int a);\n",
        "t.h");
    check(contains(result.text, "skipped:"), "a skipped declaration is reported");
    check(result.text.find("MY_API") != std::string::npos,
          "a function-like macro prefix is reported, not dropped in silence");
    check(contains(result.text, "preprocess"),
          "the macro skip tells the reader what to do");
    check(contains(result.text, "is not a known type"),
          "an unresolvable type is reported");
    check(result.skipped == 3, "each undecidable declaration is counted");
  }

  // A tag declaration (`enum E { ... };`) is not a function: it is consumed
  // without being reported as a skipped binding.
  {
    const bindgen::Result result = bindgen::generate(
        "enum Color { RED, GREEN };\n"
        "union U { int i; float f; };\n"
        "int f(enum Color *c);\n",
        "t.h");
    check(contains(result.text, "fn f(c: ptr): int32;"), "a pointer to an enum is a ptr");
    check(result.skipped == 0, "a tag declaration is not reported as a skipped binding");
  }

  // The register thunks cover at most `kMaxExternArgs` eightbytes; a longer
  // non-variadic signature would be rejected by the compiler, so it is skipped
  // rather than emitted. A variadic is exempt (it goes through the stub).
  {
    const bindgen::Result result = bindgen::generate(
        "int seven(int a, int b, int c, int d, int e, int f, int g);\n"
        "int nine(int a, int b, int c, int d, int e, int f, int g, int h, int i);\n"
        "void vlog(const char *fmt, ...);\n",
        "t.h");
    check(contains(result.text, "fn seven(a: int32, b: int32, c: int32, d: int32, e: int32, f: int32, g: int32): int32;"),
          "a seven-value signature is within the eightbyte budget");
    check(contains(result.text, "// skipped: nine: an 'extern' function may pass at most 8 "
                                "register-sized values"),
          "a nine-value signature is skipped");
    if (ffi::kCVariadicSupported) {
      check(contains(result.text, "fn vlog(fmt: cstring, ...): void;"),
            "a variadic is emitted on an x86-64 build");
      check(!contains(result.text, "// fn vlog"), "an emitted variadic is not commented");
      check(result.emitted == 2, "the in-budget and variadic declarations are emitted");
    } else {
      check(contains(result.text, "// fn vlog(fmt: cstring, ...): void;"),
            "a variadic is exempt from the budget");
      check(result.emitted == 1, "only the in-budget declaration is emitted");
    }
  }

  // The budget is on eightbytes, not parameters: a 16-byte struct spends two,
  // so seven scalars plus it is nine and over the eight the thunks cover.
  // Counting parameters instead would emit a signature `oskel -c` rejects.
  {
    const bindgen::Result result = bindgen::generate(
        "struct Q { double x; double y; };\n"
        "int seven(int a, int b, int c, int d, int e, int f, int g, struct Q q);\n"
        "int six(int a, int b, int c, int d, int e, int f, struct Q q);\n",
        "t.h");
    check(contains(result.text, "// skipped: seven: an 'extern' function may pass at most 8 "
                                "register-sized values"),
          "a struct's eightbytes count toward the budget");
    check(contains(result.text, "fn six(a: int32, b: int32, c: int32, d: int32, e: int32, f: int32, q: Q): int32;") ||
              contains(result.text, "// fn six(a: int32, b: int32, c: int32, d: int32, e: int32, f: int32, q: Q): int32;"),
          "six scalars plus a 16-byte struct is exactly eight");
  }

  // An anonymous `struct { ... }` inline has no name a signature could spell:
  // falling back to the empty spelling would pass it as a scalar `int32`.
  {
    const bindgen::Result result = bindgen::generate(
        "int g(struct { int x; } p);\n"
        "struct { int y; } h(void);\n",
        "t.h");
    check(contains(result.text, "// skipped: g: parameter type an anonymous 'struct'"),
          "an anonymous struct parameter is reported");
    check(contains(result.text, "// skipped: h: return type an anonymous 'struct'"),
          "an anonymous struct return is reported");
    check(!contains(result.text, "fn g(p: int32)"), "an anonymous struct is never a scalar");
    check(result.emitted == 0, "neither anonymous-struct declaration is emitted");
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
