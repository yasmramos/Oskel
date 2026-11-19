// Tests for the freestanding AOT runtime (PHASE 7c.0).
//
// Three levels:
//   1. Arena: alignment, exhaustion, reset, null block -- pure, portable.
//   2. PlatformFreestanding: a real write/read file roundtrip through direct
//      syscalls, plus alloc/dealloc.
//   3. End to end: build a real ELF from emitted machine code (a `main` that
//      writes a string and returns 0), write it to a temp file, run it, and
//      check its stdout, exit code and that it has no dynamic dependencies.
//
// Levels 2 and 3 need Linux x86-64; elsewhere they are skipped and only the
// arena runs.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "bytecode.h"
#include "compiler.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "native/arena.h"
#include "native/elf.h"
#include "native/entry.h"
#include "native/lower.h"
#include "native/platform_freestanding.h"
#include "native/runtime.h"
#include "native/value.h"

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

// --------------------------------------------------------------------- arena

static void test_arena() {
  alignas(64) unsigned char block[256];
  Arena arena(block, sizeof block);

  check(arena.capacity() == sizeof block, "arena reports its capacity");
  check(arena.used() == 0, "arena starts empty");

  void* a = arena.allocate(1, 1);
  check(a == block, "first allocation is at the block start");
  check(reinterpret_cast<std::uintptr_t>(a) % 1 == 0, "1-byte alignment");

  void* b = arena.allocate(8, 8);
  check(reinterpret_cast<std::uintptr_t>(b) % 8 == 0, "8-byte alignment honoured");
  check(b != a, "distinct allocations do not overlap");

  void* c = arena.allocate(16, 16);
  check(reinterpret_cast<std::uintptr_t>(c) % 16 == 0, "16-byte alignment honoured");

  // Zero-size allocations still get distinct addresses.
  void* z1 = arena.allocate(0, 8);
  void* z2 = arena.allocate(0, 8);
  check(z1 != nullptr && z2 != nullptr && z1 != z2, "zero-size allocations are distinct");

  // Exhaustion returns nullptr instead of throwing.
  void* huge = arena.allocate(1u << 20, 8);
  check(huge == nullptr, "exhausted arena returns nullptr");

  arena.reset();
  check(arena.used() == 0, "reset reclaims the region");
  void* again = arena.allocate(4, 4);
  check(again == block, "allocation after reset reuses the block start");

  // A bad alignment is rejected.
  Arena empty(nullptr, 0);
  check(empty.allocate(8, 8) == nullptr, "null-block arena fails every allocation");
  check(empty.allocate(8, 3) == nullptr, "non-power-of-two alignment rejected");
}

// --------------------------------------------------------------- platform

#if defined(__linux__) && defined(__x86_64__)
#define FREESTANDING_EXEC_TESTS 1
#include <cstdlib>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef FREESTANDING_EXEC_TESTS

static volatile sig_atomic_t gAlarmSeen = 0;

static void onAlarm(int /*sig*/) { gAlarmSeen = 1; }

static void test_platform_roundtrip() {
  PlatformFreestanding platform;
  const char* path = "/tmp/oskel_freestanding_roundtrip.txt";
  const char* payload = "hello freestanding\n";

  int fd = platform.open(path, Platform::OpenMode::Write);
  check(fd != Platform::kInvalidFd, "open(Write) returns a descriptor");
  platform.write(fd, payload, std::strlen(payload));
  platform.flush(fd);
  platform.close(fd);

  fd = platform.open(path, Platform::OpenMode::Read);
  check(fd != Platform::kInvalidFd, "open(Read) returns a descriptor");
  char buffer[64] = {0};
  std::size_t n = platform.read(fd, buffer, sizeof buffer);
  platform.close(fd);
  check(n == std::strlen(payload), "read returns the written length");
  check(std::strcmp(buffer, payload) == 0, "read returns the written bytes");

  // seek back to the start and re-read.
  fd = platform.open(path, Platform::OpenMode::Read);
  check(platform.seek(fd, 0, Platform::SeekOrigin::Start), "seek(Start) succeeds");
  char first = 0;
  check(platform.read(fd, &first, 1) == 1 && first == 'h', "seek repositions the cursor");
  platform.close(fd);

  // Missing file: Read does not create.
  check(platform.open("/tmp/oskel_does_not_exist_12345", Platform::OpenMode::Read) ==
            Platform::kInvalidFd,
        "open(Read) on a missing file fails");

  // Allocator hands out aligned, distinct memory and frees are no-ops.
  void* p1 = platform.alloc(32, 16);
  void* p2 = platform.alloc(32, 16);
  check(p1 != nullptr && p2 != nullptr && p1 != p2, "platform alloc returns distinct blocks");
  check(reinterpret_cast<std::uintptr_t>(p1) % 16 == 0, "platform alloc honours alignment");
  platform.dealloc(p1);  // no-op, must not crash
}

// A signal delivered while `read` blocks on a pipe returns EINTR. The platform
// must retry, not report the interruption as an error. A SIGALRM one tick after
// the blocking read starts is enough to exercise the retry path; the writer end
// of the pipe stays open until the read has returned, so the first `read` really
// does block and really is interrupted.
static void test_read_retries_on_eintr() {
  int fds[2];
  if (pipe(fds) != 0) {
    check(false, "pipe() for the EINTR test");
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    check(false, "fork() for the EINTR test");
    close(fds[0]);
    close(fds[1]);
    return;
  }
  if (pid == 0) {
    // Child: a plain PlatformFreestanding read on the blocking read end. The
    // handler is installed with `sigaction` and no SA_RESTART: `signal()` would
    // request restart-on-interrupt, hiding the EINTR path this test exercises.
    close(fds[1]);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = onAlarm;
    sigaction(SIGALRM, &sa, nullptr);
    alarm(1);
    PlatformFreestanding platform;
    char buffer[8] = {0};
    std::size_t n = platform.read(fds[0], buffer, sizeof buffer);
    bool ok = (n == 5) && std::strcmp(buffer, "hello") == 0 && gAlarmSeen == 1;
    _exit(ok ? 0 : 1);
  }
  // Parent: let the child block in read, signal it, then deliver the bytes.
  sleep(2);
  ssize_t ignored = ::write(fds[1], "hello", 5);
  (void)ignored;
  close(fds[1]);
  int status = 0;
  waitpid(pid, &status, 0);
  close(fds[0]);
  check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "read retries after EINTR instead of failing");
}

// Builds a tiny program in `code` that writes `message` (a rodata offset known
// to the caller) and returns `exitCode`, then appends the freestanding runtime.
// Returns the entry offset and the text/rodata images.
struct BuiltProgram {
  std::vector<std::uint8_t> text;
  std::vector<std::uint8_t> rodata;
  std::uint64_t entry = 0;
};

// `main(argc, argv)`: oskel_write(1, messageAddress, messageLength); return N.
static BuiltProgram buildHello(std::int64_t messageAddress, std::int64_t messageLength,
                               std::int64_t exitCode) {
  CodeGen code;
  RuntimeHandles handles = newRuntimeHandles(code);

  CodeGen::Label mainLabel = code.newLabel();
  code.bind(mainLabel);

  // oskel_write(1, message, length)
  code.movImm(X64Reg::Rdi, 1);
  code.movImm(X64Reg::Rsi, messageAddress);
  code.movImm(X64Reg::Rdx, messageLength);
  code.callLabel(handles.write);

  code.movImm(X64Reg::Rax, exitCode);
  code.retInsn();

  std::uint64_t entry = emitFreestandingRuntime(code, handles, mainLabel);
  code.finalize();

  BuiltProgram built;
  built.text = code.code();
  built.entry = entry;
  return built;
}

// Runs `path` and captures its stdout and exit code.
struct RunResult {
  std::string output;
  int exitCode = -1;
};

static RunResult runProgram(const std::string& path) {
  std::string command = path + " > /tmp/oskel_freestanding_out.txt 2>&1";
  int status = std::system(command.c_str());
  RunResult result;
  result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  std::FILE* file = std::fopen("/tmp/oskel_freestanding_out.txt", "rb");
  if (file) {
    char buffer[256];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
      result.output.append(buffer, n);
    }
    std::fclose(file);
  }
  return result;
}

static void writeFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return;
  std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  chmod(path.c_str(), 0755);
}

static void test_end_to_end() {
  // The .rodata section is loaded at 0x600000 (see elf.cpp); the message sits
  // at its very start.
  const char* message = "hello from a Oskel AOT binary\n";
  std::vector<std::uint8_t> rodata(message, message + std::strlen(message));
  const std::int64_t messageAddress = 0x600000;

  BuiltProgram built = buildHello(messageAddress, static_cast<std::int64_t>(rodata.size()), 0);
  built.rodata = rodata;

  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), built.text, built.rodata, built.entry);
  const std::string path = "/tmp/oskel_freestanding_hello";
  writeFile(path, elf);

  RunResult result = runProgram(path);
  check(result.output == message, "the generated binary prints the expected bytes");
  check(result.exitCode == 0, "the generated binary exits with the program's code");

  // A non-zero return value must become the process exit code.
  BuiltProgram failing = buildHello(messageAddress, static_cast<std::int64_t>(rodata.size()), 7);
  failing.rodata = rodata;
  std::vector<std::uint8_t> elfFail =
      buildStaticElf(Target::linuxX64(), failing.text, failing.rodata, failing.entry);
  writeFile("/tmp/oskel_freestanding_exit7", elfFail);
  RunResult failed = runProgram("/tmp/oskel_freestanding_exit7");
  check(failed.exitCode == 7, "the program's return value becomes the exit code");

  // No dynamic dependencies: this is a static, freestanding binary.
  std::string ldd = "ldd " + path + " 2>&1";
  std::FILE* pipe = popen(ldd.c_str(), "r");
  std::string lddOut;
  if (pipe) {
    char buffer[256];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) {
      lddOut.append(buffer, n);
    }
    pclose(pipe);
  }
  check(lddOut.find("not a dynamic executable") != std::string::npos ||
            lddOut.find("statically linked") != std::string::npos,
        "the generated binary has no dynamic dependencies");
}

// Builds a static ELF from a lowered chunk and runs it, returning its stdout
// and exit code. The chunk already contains `_start` and the runtime.
static RunResult buildAndRun(const std::string& path, const Bytecode& proto) {
  LoweredChunk chunk = lowerToX64(Target::linuxX64(), proto, CodeGen{});
  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), chunk.code, {}, chunk.processEntry);
  writeFile(path, elf);
  return runProgram(path);
}

static Instruction instr(OpCode op, int a = 0, int b = 0, int c = 0, int jump = 0) {
  Instruction i;
  i.op = op;
  i.a = a;
  i.b = b;
  i.c = c;
  i.jump = jump;
  return i;
}

static Bytecode makeProto(const std::vector<Instruction>& code,
                          const std::vector<Value>& constants, int numRegisters,
                          int numLocals = 0) {
  Bytecode proto;
  proto.name = "<freestanding-test>";
  proto.code = code;
  proto.constants = constants;
  proto.numRegisters = numRegisters;
  proto.numLocals = numLocals;
  return proto;
}

// Compiles a Oskel source string to bytecode, the way the dev engine does.
static Bytecode compileSource(const std::string& source) {
  auto tokens = lex(source);
  Parser parser(std::move(tokens), "<freestanding-test>");
  return compileProgram(parser.parseProgram());
}

// The `print` builtin, end to end: compile a real program that calls `print`,
// lower it, build a static ELF and run it. Before 7c.2d the backend had no
// builtins, so `print` was a lowering error and a compiled binary could not
// print anything (PHASE 7c.2d).
static void test_builtin_print() {
  Bytecode proto = compileSource(
      "print(\"hello\", 42, 3.5, true);"
      "write(\"no\", \"newline\");"
      "print();");
  RunResult result = buildAndRun("/tmp/oskel_builtin_print", proto);
  check(result.output == "hello 42 3.5 true\nno newline\n",
        "a compiled program prints through the print/write builtins");
  check(result.exitCode == 0, "a program using print exits 0 through _start");
}

// Collections, end to end (PHASE 7c.3): arrays, maps, indexing, `.length`,
// `in`, `for-in` and container printing all lower and run in a compiled binary.
static void test_collections() {
  Bytecode proto = compileSource(
      "let a = [1, 2, 3];\n"
      "a[1] = 20;\n"
      "a[3] = 4;\n"
      "print(a);\n"
      "print(a.length);\n"
      "print(a[0], a[1], a[-1]);\n"
      "let m = {\"x\": 1, \"y\": 2};\n"
      "m[\"z\"] = 3;\n"
      "m.x = 10;\n"
      "print(m);\n"
      "print(m[\"x\"], m.y, m[\"z\"]);\n"
      "print(2 in a, 99 in a, \"x\" in m, \"q\" in m);\n"
      "a[-1] = 9;\n"
      "a[1] = 20;\n"
      "m[\"x\"] = 5;\n"
      "m[\"new\"] = 7;\n"
      "print(a, m);\n"
      "let s = \"hello\";\n"
      "print(s.length, s[1], \"ell\" in s);\n"
      "print(\"\" in s, \"z\" in s, \"hello\" in s, \"helloo\" in s);\n"
      "var total = 0;\n"
      "for v in a { total = total + v; }\n"
      "print(total);\n"
      "var keys = \"\";\n"
      "for k in m { keys = keys + k; }\n"
      "print(keys);\n"
      "var pairKeys = \"\";\n"
      "var pairTotal = 0;\n"
      "for k, v in m { pairKeys = pairKeys + k; pairTotal = pairTotal + v; }\n"
      "print(pairKeys, pairTotal);\n"
      "var chars = \"\";\n"
      "for c in \"abc\" { chars = chars + c; }\n"
      "print(chars);\n");
  RunResult result = buildAndRun("/tmp/oskel_collections", proto);
  std::string expected =
      "[1, 20, 3, 4]\n"
      "4\n"
      "1 20 4\n"
      "{\"x\": 10, \"y\": 2, \"z\": 3}\n"
      "10 2 3\n"
      "false false true false\n"
      "[1, 20, 3, 9] {\"x\": 5, \"y\": 2, \"z\": 3, \"new\": 7}\n"
      "5 e true\n"
      "true false true false\n"
      "33\n"
      "xyznew\n"
      "xyznew 17\n"
      "abc\n";
  check(result.output == expected, "a compiled program runs collections and prints them");
  check(result.exitCode == 0, "a program using collections exits 0 through _start");
}

// A negative string index counts code points from the end, exactly like a
// negative array index (`ops::normalizeIndex`). `utf8CharAt` walks forward only,
// so the string branch of `oskel_table_get` must rebase the index against the
// code-point length first; before that it passed the raw index through and a
// program the VM accepts (`"abc"[-1]` is `"c"`) faulted with "index out of
// range". The out-of-range cases must still report the error and exit 70.
static void test_negative_string_index() {
  Bytecode proto = compileSource(
      "print(\"abc\"[-1]);\n"
      "print(\"abc\"[-3]);\n"
      "print(\"héllo\"[-1]);\n"
      "print(\"héllo\"[-5]);\n");
  RunResult result = buildAndRun("/tmp/oskel_negative_string_index", proto);
  check(result.output == "c\na\no\nh\n",
        "a negative string index counts code points from the end");
  check(result.exitCode == 0, "a program with a negative string index exits 0");

  Bytecode tooLow = compileSource("print(\"abc\"[-5]);\n");
  RunResult low = buildAndRun("/tmp/oskel_negative_string_index_low", tooLow);
  check(low.exitCode == 70, "a too-negative string index exits 70");
  check(low.output.find("index out of range") != std::string::npos,
        "a too-negative string index reports the range error");
}

// The in-place and query array methods (PHASE 7c.3e): `shift`, `unshift`,
// `insert`, `removeAt`, `contains`, `indexOf` and `clear`. Each binds like
// `push`/`pop` -- the receiver is captured in an upvalue -- so a call mutates
// or scans the same array. Before this slice the backend answered every one of
// them with "value has no such member". The error paths (bad arity, index out
// of range) must report the message and exit 70.
static void test_array_methods_inplace() {
  Bytecode proto = compileSource(
      "var a = [1, 2, 3];\n"
      "print(a.shift());\n"
      "print(a);\n"
      "print(a.unshift(0));\n"
      "print(a);\n"
      "print(a.insert(2, 9));\n"
      "print(a);\n"
      "print(a.removeAt(0));\n"
      "print(a);\n"
      "print(a.contains(9), a.contains(99));\n"
      "print(a.indexOf(9), a.indexOf(99));\n"
      "print(a.clear());\n"
      "print(a);\n");
  RunResult result = buildAndRun("/tmp/oskel_array_methods_inplace", proto);
  std::string expected =
      "1\n"
      "[2, 3]\n"
      "[0, 2, 3]\n"
      "[0, 2, 3]\n"
      "[0, 2, 9, 3]\n"
      "[0, 2, 9, 3]\n"
      "0\n"
      "[2, 9, 3]\n"
      "true false\n"
      "1 -1\n"
      "[]\n"
      "[]\n";
  check(result.output == expected, "a compiled program runs the in-place array methods");
  check(result.exitCode == 0, "a program using the in-place array methods exits 0");

  Bytecode badArity = compileSource("var a = [1];\nprint(a.shift(1));\n");
  RunResult arity = buildAndRun("/tmp/oskel_array_methods_inplace_arity", badArity);
  check(arity.exitCode == 70, "a wrong-arity in-place array method exits 70");
  check(arity.output.find("shift() expects 0 arguments") != std::string::npos,
        "a wrong-arity in-place array method reports the arity error");

  Bytecode badIndex = compileSource("var a = [1];\nprint(a.removeAt(5));\n");
  RunResult range = buildAndRun("/tmp/oskel_array_methods_inplace_range", badIndex);
  check(range.exitCode == 70, "removeAt out of range exits 70");
  check(range.output.find("removeAt() index out of range") != std::string::npos,
        "removeAt out of range reports the range error");
}

// The allocating array methods (PHASE 7c.3f): `slice`, `concat`, `reversed` and
// `clone`. Each allocates a fresh array from the arena and leaves the receiver
// untouched, so this exercises the arena allocation path under the real `_start`
// and pins the return values against the VM.
static void test_array_methods_alloc() {
  Bytecode proto = compileSource(
      "var a = [1, 2, 3, 4, 5];\n"
      "print(a.slice());\n"
      "print(a.slice(1, 3));\n"
      "print(a.slice(-2));\n"
      "print(a.concat([6, 7]));\n"
      "print(a.reversed());\n"
      "var c = a.clone();\n"
      "c.push(9);\n"
      "print(c);\n"
      "print(a);\n");
  RunResult result = buildAndRun("/tmp/oskel_array_methods_alloc", proto);
  std::string expected =
      "[1, 2, 3, 4, 5]\n"
      "[2, 3]\n"
      "[4, 5]\n"
      "[1, 2, 3, 4, 5, 6, 7]\n"
      "[5, 4, 3, 2, 1]\n"
      "[1, 2, 3, 4, 5, 9]\n"
      "[1, 2, 3, 4, 5]\n";
  check(result.output == expected, "a compiled program runs the allocating array methods");
  check(result.exitCode == 0, "a program using the allocating array methods exits 0");

  Bytecode badArity = compileSource("var a = [1];\nprint(a.concat());\n");
  RunResult arity = buildAndRun("/tmp/oskel_array_methods_alloc_arity", badArity);
  check(arity.exitCode == 70, "a wrong-arity allocating array method exits 70");
  check(arity.output.find("concat() expects 1 argument") != std::string::npos,
        "a wrong-arity allocating array method reports the arity error");

  Bytecode badType = compileSource("var a = [1];\nprint(a.concat(5));\n");
  RunResult type = buildAndRun("/tmp/oskel_array_methods_alloc_type", badType);
  check(type.exitCode == 70, "concat of a non-array exits 70");
  check(type.output.find("concat() expects an array") != std::string::npos,
        "concat of a non-array reports the type error");

  Bytecode badStart = compileSource("var a = [1];\nprint(a.slice(\"x\"));\n");
  RunResult start = buildAndRun("/tmp/oskel_array_methods_alloc_start", badStart);
  check(start.exitCode == 70, "slice with a non-integer start exits 70");
  check(start.output.find("slice() expects an integer start") != std::string::npos,
        "slice with a non-integer start reports the type error");
}

// The bound map methods (PHASE 7c.3g): `keys`, `values`, `entries`, `has`,
// `get`, `set`, `delete`, `clear`, `merge` and `clone`. Each binds like the
// array methods -- the receiver is captured in an upvalue -- so a call mutates
// or scans the same map. Before this slice the backend answered every one of
// them with "value has no such member". The error paths (bad arity, a non-string
// key, a non-map `merge`) must report the message and exit 70.
static void test_map_methods() {
  Bytecode proto = compileSource(
      "var m = {\"a\": 1, \"b\": 2, \"c\": 3};\n"
      "print(m.keys());\n"
      "print(m.values());\n"
      "print(m.entries());\n"
      "print(m.has(\"a\"), m.has(\"z\"));\n"
      "print(m.get(\"a\"), m.get(\"z\"), m.get(\"z\", 99));\n"
      "print(m.set(\"d\", 4));\n"
      "print(m.delete(\"a\"), m);\n"
      "print(m.merge({\"b\": 20, \"e\": 5}));\n"
      "print(m.length);\n"
      "var c = m.clone();\n"
      "c.set(\"f\", 6);\n"
      "print(c);\n"
      "print(m);\n"
      "print(m.clear(), m);\n"
      "var keys = m.keys;\n"
      "m.set(\"g\", 7);\n"
      "print(keys());\n");
  RunResult result = buildAndRun("/tmp/oskel_map_methods", proto);
  std::string expected =
      "[\"a\", \"b\", \"c\"]\n"
      "[1, 2, 3]\n"
      "[[\"a\", 1], [\"b\", 2], [\"c\", 3]]\n"
      "true false\n"
      "1 nil 99\n"
      "{\"a\": 1, \"b\": 2, \"c\": 3, \"d\": 4}\n"
      "true {\"b\": 2, \"c\": 3, \"d\": 4}\n"
      "{\"b\": 20, \"c\": 3, \"d\": 4, \"e\": 5}\n"
      "4\n"
      "{\"b\": 20, \"c\": 3, \"d\": 4, \"e\": 5, \"f\": 6}\n"
      "{\"b\": 20, \"c\": 3, \"d\": 4, \"e\": 5}\n"
      "{} {}\n"
      "[\"g\"]\n";
  check(result.output == expected, "a compiled program runs the bound map methods");
  check(result.exitCode == 0, "a program using the bound map methods exits 0");

  Bytecode badArity = compileSource("var m = {};\nprint(m.has());\n");
  RunResult arity = buildAndRun("/tmp/oskel_map_methods_arity", badArity);
  check(arity.exitCode == 70, "a wrong-arity map method exits 70");
  check(arity.output.find("has() expects 1 argument") != std::string::npos,
        "a wrong-arity map method reports the arity error");

  Bytecode badType = compileSource("var m = {};\nprint(m.get(5));\n");
  RunResult type = buildAndRun("/tmp/oskel_map_methods_type", badType);
  check(type.exitCode == 70, "a non-string map key exits 70");
  check(type.output.find("get() expects a string key") != std::string::npos,
        "a non-string map key reports the type error");

  Bytecode badMerge = compileSource("var m = {};\nprint(m.merge(5));\n");
  RunResult merge = buildAndRun("/tmp/oskel_map_methods_merge", badMerge);
  check(merge.exitCode == 70, "merge of a non-map exits 70");
  check(merge.output.find("merge() expects a map") != std::string::npos,
        "merge of a non-map reports the type error");
}

// The hidden helpers for PHASE 6.5 features that lower to a call (PHASE 7c.4a):
// `$nonNull` (`a!!`) and `$isError` (`try` / `x catch y`). Before this slice the
// backend rejected both with `global '$nonNull'/'$isError' is read but never
// assigned`; now they are seeded builtins and a compiled program runs them.
static void test_hidden_helpers() {
  Bytecode proto = compileSource(
      "var a = 7;\n"
      "var b = a!!;\n"
      "print(b);\n"
      "var c = b catch 0;\n"
      "print(c);\n"
      "fn f() { var x = 9; return try x; }\n"
      "print(f());\n");
  RunResult result = buildAndRun("/tmp/oskel_hidden_helpers", proto);
  check(result.output == "7\n7\n9\n",
        "a compiled program runs the non-null and catch helpers");
  check(result.exitCode == 0, "a program using the hidden helpers exits 0");
}

// `$nonNull` on nil must fail with the hosted helper's message and exit 70,
// proving the AOT helper takes the same error path as the VM.
static void test_hidden_helpers_non_null_failure() {
  Bytecode proto = compileSource(
      "var a = nil;\n"
      "var b = a!!;\n"
      "print(b);\n");
  RunResult result = buildAndRun("/tmp/oskel_hidden_helpers_nil", proto);
  check(result.exitCode == 70, "a failed '!!' exits 70");
  check(result.output.find("non-null assertion") != std::string::npos,
        "the failed '!!' names the non-null assertion");
}

// The `$str` helper (PHASE 7c.4b) renders the scalars a compiled program
// interpolates: int, negative int, float, double, bool, nil and string must
// match the hosted `Value::toString` byte for byte.
static void test_str_helper() {
  Bytecode proto = compileSource(
      "var n = 3;\n"
      "print(\"n=${n}\");\n"
      "var f = 1.0 / 3.0;\n"
      "print(\"f=${f}\");\n"
      "var d = 0.1 + 0.2;\n"
      "print(\"d=${d}\");\n"
      "var b = true;\n"
      "print(\"b=${b}\");\n"
      "var s = \"hi\";\n"
      "print(\"s=${s}\");\n"
      "var z = nil;\n"
      "print(\"z=${z}\");\n"
      "var neg = -42;\n"
      "print(\"neg=${neg}\");\n");
  RunResult result = buildAndRun("/tmp/oskel_str_helper", proto);
  std::string expected =
      "n=3\n"
      "f=0.3333333333333333\n"
      "d=0.30000000000000004\n"
      "b=true\n"
      "s=hi\n"
      "z=nil\n"
      "neg=-42\n";
  check(result.output == expected,
        "a compiled program interpolates scalars like the VM");
  check(result.exitCode == 0, "a program using interpolation exits 0");
}

// The `$range` helper (PHASE 7c.4c) builds a range value from two integer
// bounds. A compiled program must print, iterate (ascending and descending,
// inclusive and exclusive), take `.length`, test membership with `in` and
// interpolate a range exactly like the VM.
static void test_range_helper() {
  Bytecode proto = compileSource(
      "print(0..5);\n"
      "print(0..=5);\n"
      "print(5..0);\n"
      "print(5..=0);\n"
      "print((0..=5).length);\n"
      "var r = 0..3;\n"
      "print(r.length);\n"
      "for i in 0..3 { print(i); }\n"
      "for i in 3..0 { print(i); }\n"
      "for i in 0..=2 { print(i); }\n"
      "print(2 in 0..5);\n"
      "print(5 in 0..5);\n"
      "print(5 in 0..=5);\n"
      "print(1 in 5..0);\n"
      "print(5 in 5..0);\n"
      "print(\"${0..=3}\");\n");
  RunResult result = buildAndRun("/tmp/oskel_range_helper", proto);
  std::string expected =
      "0..5\n"
      "0..=5\n"
      "5..0\n"
      "5..=0\n"
      "6\n"
      "3\n"
      "0\n"
      "1\n"
      "2\n"
      "3\n"
      "2\n"
      "1\n"
      "0\n"
      "1\n"
      "2\n"
      "true\n"
      "false\n"
      "true\n"
      "true\n"
      "false\n"
      "0..=3\n";
  check(result.output == expected,
        "a compiled program handles ranges like the VM");
  check(result.exitCode == 0, "a program using ranges exits 0");
}

// A range with a non-integer bound must fail with the hosted helper's message
// and exit 70.
static void test_range_helper_bad_bounds() {
  Bytecode proto = compileSource(
      "var s = \"x\";\n"
      "var r = s..3;\n"
      "print(r);\n");
  RunResult result = buildAndRun("/tmp/oskel_range_bad", proto);
  check(result.exitCode == 70, "a non-integer range bound exits 70");
  check(result.output.find("range bounds must be integers") != std::string::npos,
        "the failed range names the integer-bounds rule");
}

// A nested container prints with the recursive repr, and a self-referential one
// stops at the depth cap instead of recursing forever.
static void test_collection_repr() {
  Bytecode proto = compileSource(
      "print([[1, 2], {\"a\": [3]}]);\n"
      "print([\"a\", \"b\\\"c\", \"d\\ne\", \"tab\\tx\", \"\\x01\"]);\n"
      "let m = {};\n"
      "m[\"self\"] = m;\n"
      "print(m);\n");
  RunResult result = buildAndRun("/tmp/oskel_collection_repr", proto);
  check(result.output.find("[[1, 2], {\"a\": [3]}]\n") == 0,
        "a nested array/map prints with the recursive repr");
  check(result.output.find("[\"a\", \"b\\\"c\", \"d\\ne\", \"tab\\tx\", \"\\x01\"]\n") !=
            std::string::npos,
        "nested strings print with the interpreter's escapes");
  check(result.output.find("...") != std::string::npos,
        "a self-referential map stops at the depth cap");
  check(result.exitCode == 0, "a self-referential program still exits 0");
}

// `$str` renders a collection or function with the recursive repr (PHASE
// 7c.4b.2), so `"${[1, 2]}"` reads exactly like `print([1, 2])`: nested strings
// quoted, maps as `{k: v}`, functions as `<fn>`, and a self-referential
// structure stops at the depth cap instead of recursing forever.
static void test_str_helper_collections() {
  Bytecode proto = compileSource(
      "print(\"${[1, 2, 3]}\");\n"
      "print(\"${[\"a\", \"b\"]}\");\n"
      "print(\"${[[1, 2], {\"k\": 3}]}\");\n"
      "var m = {\"x\": 1, \"y\": [1, 2]};\n"
      "print(\"${m}\");\n"
      "fn f() {}\n"
      "print(\"${f}\");\n"
      "print(\"${0..=3}\");\n"
      "let self = {};\n"
      "self[\"me\"] = self;\n"
      "print(\"${self}\");\n"
      "print(\"sum=${[1, 2, 3].length}\");\n");
  RunResult result = buildAndRun("/tmp/oskel_str_collections", proto);
  check(result.output.find(
            "[1, 2, 3]\n"
            "[\"a\", \"b\"]\n"
            "[[1, 2], {\"k\": 3}]\n"
            "{\"x\": 1, \"y\": [1, 2]}\n"
            "<fn>\n"
            "0..=3\n") == 0,
        "a compiled program interpolates collections and functions like print");
  check(result.output.find("{\"me\":") != std::string::npos &&
            result.output.find("...") != std::string::npos,
        "interpolating a self-referential map stops at the depth cap");
  check(result.output.find("sum=3\n") != std::string::npos,
        "a collection expression interpolates with the recursive repr");
  check(result.exitCode == 0, "a program interpolating a collection exits 0");
}

// The `$slice` helper (PHASE 7c.4d) slices an array by element and a string by
// UTF-8 code point, following the hosted rule: an omitted bound is a step-aware
// default, a negative bound counts from the end, and both clamp so a reverse
// slice never wraps. A compiled program must agree with the VM.
static void test_slice_helper() {
  Bytecode proto = compileSource(
      "let a = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];\n"
      "print(a[1:4]);\n"
      "print(a[:3]);\n"
      "print(a[7:]);\n"
      "print(a[:]);\n"
      "print(a[::2]);\n"
      "print(a[::-1]);\n"
      "print(a[::-2]);\n"
      "print(a[8:2:-1]);\n"
      "print(a[-3:]);\n"
      "print(a[:-3]);\n"
      "print(a[-100:100]);\n"
      "print(a[100:]);\n"
      "print(a[5:5]);\n"
      "print(a[1:8:3]);\n"
      "let s = \"abcdef\";\n"
      "print(s[1:4]);\n"
      "print(s[::-1]);\n"
      "print(s[1:4:2]);\n"
      "print(s[-2:]);\n"
      "print(s[:-2]);\n"
      "print(s[10:]);\n"
      "let u = \"caf\\u00e9\\u00e8\";\n"
      "print(u.length);\n"
      "print(u[1:3]);\n"
      "print(u[::-1]);\n"
      "print(u[::2]);\n");
  RunResult result = buildAndRun("/tmp/oskel_slice_helper", proto);
  std::string expected =
      "[1, 2, 3]\n"
      "[0, 1, 2]\n"
      "[7, 8, 9]\n"
      "[0, 1, 2, 3, 4, 5, 6, 7, 8, 9]\n"
      "[0, 2, 4, 6, 8]\n"
      "[9, 8, 7, 6, 5, 4, 3, 2, 1, 0]\n"
      "[9, 7, 5, 3, 1]\n"
      "[8, 7, 6, 5, 4, 3]\n"
      "[7, 8, 9]\n"
      "[0, 1, 2, 3, 4, 5, 6]\n"
      "[0, 1, 2, 3, 4, 5, 6, 7, 8, 9]\n"
      "[]\n"
      "[]\n"
      "[1, 4, 7]\n"
      "bcd\n"
      "fedcba\n"
      "bd\n"
      "ef\n"
      "abcd\n"
      "\n"
      "5\n"
      "af\n"
      "\u00e8\u00e9fac\n"
      "cf\u00e8\n";
  check(result.output == expected,
        "a compiled program slices arrays and strings like the VM");
  check(result.exitCode == 0, "a program using slices exits 0");
}

// A bad slice bound, a zero step and a non-sliceable operand must fail with the
// hosted helper's message and exit 70.
static void test_slice_helper_errors() {
  Bytecode badStart = compileSource(
      "let a = [1, 2, 3];\n"
      "print(a[1.5:]);\n");
  RunResult start = buildAndRun("/tmp/oskel_slice_bad_start", badStart);
  check(start.exitCode == 70, "a non-integer slice start exits 70");
  check(start.output.find("slice start must be an integer") != std::string::npos,
        "the failed slice names the integer-start rule");

  Bytecode zeroStep = compileSource(
      "let a = [1, 2, 3];\n"
      "print(a[::0]);\n");
  RunResult step = buildAndRun("/tmp/oskel_slice_zero_step", zeroStep);
  check(step.exitCode == 70, "a zero slice step exits 70");
  check(step.output.find("slice step must not be zero") != std::string::npos,
        "the failed slice names the non-zero-step rule");

  Bytecode notSliceable = compileSource(
      "print(5[1:2]);\n");
  RunResult value = buildAndRun("/tmp/oskel_slice_not_sliceable", notSliceable);
  check(value.exitCode == 70, "slicing a non-collection exits 70");
  check(value.output.find("not sliceable") != std::string::npos,
        "the failed slice names the non-sliceable operand");
}

// The public standard-library builtins (PHASE 7c.5a) through the real process
// entry: `len`, `type`, `str`, `repr`, `bool`, `ord`, `chr`, `assert`,
// `panic` and `isError`. The program prints one line per result and the output
// is asserted byte for byte against the hosted engine's semantics.
static void test_public_builtins() {
  Bytecode proto = compileSource(
      "print(len(\"h\\u00e9llo\"));\n"      // 5 code points, not 6 bytes
      "print(len([1, 2, 3]));\n"
      "print(len({\"a\": 1, \"b\": 2}));\n"
      "print(len(0..=5));\n"
      "print(type(nil));\n"
      "print(type(true));\n"
      "print(type(1));\n"
      "print(type(1.5));\n"
      "print(type(\"x\"));\n"
      "print(type([1]));\n"
      "print(type({\"k\": 1}));\n"
      "print(type(0..1));\n"
      "print(str(42), str(3.5), str(true), str(nil));\n"
      "print(repr(\"hi\"), repr([1, \"two\"]), repr(7));\n"
      "print(bool(0), bool(1), bool(nil), bool(\"\"), bool([]));\n"
      "print(ord(\"A\"), ord(\"\\u00e9\"), ord(\"\U0001F600\"));\n"
      "print(chr(65), chr(233), chr(128512));\n"
      "assert(true);\n"
      "assert(1 == 1, \"should pass\");\n"
      "print(\"after asserts\");\n"
      "print(isError(5), isError(\"x\"));\n");
  RunResult result = buildAndRun("/tmp/oskel_public_builtins", proto);
  std::string expected =
      "5\n"
      "3\n"
      "2\n"
      "6\n"
      "nil\n"
      "bool\n"
      "int\n"
      "double\n"
      "string\n"
      "array\n"
      "map\n"
      "range\n"
      "42 3.5 true nil\n"
      "\"hi\" [1, \"two\"] 7\n"
      "true true false true true\n"
      "65 233 128512\n"
      "A \u00e9 \U0001F600\n"
      "after asserts\n"
      "false false\n";
  check(result.output == expected,
        "the public builtins compute like the hosted engine");
  check(result.exitCode == 0, "a program using the public builtins exits 0");
}

// The failure paths of the public builtins must report a message and exit 70,
// matching the hosted engine's runtime-error code.
static void test_public_builtin_errors() {
  Bytecode badLen = compileSource("print(len(42));\n");
  RunResult len = buildAndRun("/tmp/oskel_builtin_bad_len", badLen);
  check(len.exitCode == 70, "len of a non-collection exits 70");
  check(len.output.find("len() is not defined") != std::string::npos,
        "the failed len names the unsupported type");

  Bytecode badOrd = compileSource("print(ord(5));\n");
  RunResult ord = buildAndRun("/tmp/oskel_builtin_bad_ord", badOrd);
  check(ord.exitCode == 70, "ord of a non-string exits 70");
  check(ord.output.find("ord() expects a string") != std::string::npos,
        "the failed ord names the string argument");

  Bytecode longOrd = compileSource("print(ord(\"ab\"));\n");
  RunResult multi = buildAndRun("/tmp/oskel_builtin_long_ord", longOrd);
  check(multi.exitCode == 70, "ord of a multi-character string exits 70");
  check(multi.output.find("ord() expects a single character") != std::string::npos,
        "the failed ord names the single-character rule");

  Bytecode badChr = compileSource("print(chr(2000000));\n");
  RunResult chr = buildAndRun("/tmp/oskel_builtin_bad_chr", badChr);
  check(chr.exitCode == 70, "chr of an out-of-range code point exits 70");
  check(chr.output.find("chr() code point out of range") != std::string::npos,
        "the failed chr names the code-point range");

  // A surrogate is a valid code point numerically but not a Unicode scalar
  // value; encoding it would produce CESU-8, which is not valid UTF-8.
  Bytecode surrogateChr = compileSource("print(chr(0xD800));\n");
  RunResult surrogate = buildAndRun("/tmp/oskel_builtin_surrogate_chr", surrogateChr);
  check(surrogate.exitCode == 70, "chr of a surrogate exits 70");
  check(surrogate.output.find("chr() code point is a surrogate") != std::string::npos,
        "the failed chr names the surrogate rule");

  Bytecode boundaryChr = compileSource("print(chr(0xD7FF), chr(0xE000));\n");
  RunResult boundary = buildAndRun("/tmp/oskel_builtin_boundary_chr", boundaryChr);
  check(boundary.exitCode == 0, "chr of the scalar values around the range succeeds");

  Bytecode failedAssert = compileSource("assert(false);\n");
  RunResult assertion = buildAndRun("/tmp/oskel_builtin_assert", failedAssert);
  check(assertion.exitCode == 70, "a failed assert exits 70");
  check(assertion.output.find("oskel: assertion failed") != std::string::npos,
        "a failed assert reports the default message");

  Bytecode customAssert = compileSource("assert(false, \"custom message\");\n");
  RunResult custom = buildAndRun("/tmp/oskel_builtin_assert_msg", customAssert);
  check(custom.exitCode == 70, "a failed assert with a message exits 70");
  check(custom.output.find("oskel: custom message") != std::string::npos,
        "a failed assert reports the caller's message");

  Bytecode panicked = compileSource("panic(\"boom\");\n");
  RunResult panic = buildAndRun("/tmp/oskel_builtin_panic", panicked);
  check(panic.exitCode == 70, "panic exits 70");
  check(panic.output.find("oskel: boom") != std::string::npos,
        "panic reports the caller's message");

  Bytecode barePanic = compileSource("panic();\n");
  RunResult bare = buildAndRun("/tmp/oskel_builtin_panic_bare", barePanic);
  check(bare.exitCode == 70, "a bare panic exits 70");
  check(bare.output.find("oskel: panic") != std::string::npos,
        "a bare panic reports the default message");
}

// The numeric conversions `int`/`float`/`double` (PHASE 7c.5b.1) for numeric and
// boolean operands must match the hosted engine: `int` truncates toward zero, a
// `float` rounds to binary32 (then stores the widened value), a `double` is the
// binary64 form. The string forms are deferred to 7c.5b.2 and are covered by
// the error test below.
static void test_numeric_conversions() {
  Bytecode proto = compileSource(
      "print(int(3.9), int(-3.9), int(7), int(true), int(false));\n"
      "print(int(9007199254740993.0), int(-9007199254740993.0));\n"
      "print(float(1), float(2.5), float(0.1));\n"
      "print(float(16777217), float(16777216));\n"  // 2^24+1 is not a binary32
      "print(double(1), double(1.5), double(true), double(false));\n"
      "print(float(1) + float(2));\n"
      "print(type(int(1.5)), type(float(1)), type(double(1)));\n");
  RunResult result = buildAndRun("/tmp/oskel_numeric_conversions", proto);
  std::string expected =
      "3 -3 7 1 0\n"
      "9007199254740992 -9007199254740992\n"
      "1 2.5 0.1\n"
      "16777216 16777216\n"
      "1 1.5 1 0\n"
      "3\n"
      "int float double\n";
  check(result.output == expected,
        "the numeric conversions match the hosted engine");
  check(result.exitCode == 0, "a program using the numeric conversions exits 0");
}

// A nil, a collection and a wrong argument count fail with a message and exit
// 70, matching the hosted engine's runtime-error code. (The string operand of
// `int` parses since 7c.5b.2a; see `test_int_string_conversion`.)
static void test_numeric_conversion_errors() {
  Bytecode fromNil = compileSource("print(int(nil));\n");
  RunResult nilInt = buildAndRun("/tmp/oskel_convert_int_nil", fromNil);
  check(nilInt.exitCode == 70, "int of nil exits 70");
  check(nilInt.output.find("cannot convert this value to int") != std::string::npos,
        "the failed int reports the conversion message");

  Bytecode fromFloatNil = compileSource("print(float(nil));\n");
  RunResult nil = buildAndRun("/tmp/oskel_convert_nil", fromFloatNil);
  check(nil.exitCode == 70, "float of nil exits 70");
  check(nil.output.find("cannot convert this value to float") != std::string::npos,
        "the failed float reports the conversion message");

  Bytecode fromArray = compileSource("print(double([1]));\n");
  RunResult array = buildAndRun("/tmp/oskel_convert_array", fromArray);
  check(array.exitCode == 70, "double of an array exits 70");
  check(array.output.find("cannot convert this value to double") != std::string::npos,
        "the failed double reports the conversion message");

  Bytecode noArgs = compileSource("print(int());\n");
  RunResult arity = buildAndRun("/tmp/oskel_convert_arity", noArgs);
  check(arity.exitCode == 70, "int with no argument exits 70");
}

// `int(string)` parses like the hosted engine (PHASE 7c.5b.2a): an optional
// sign, a `0x`/`0b`/`0o` base prefix (base 10 with a plain leading zero), the
// surrounding whitespace trimmed, and `strtoll`'s second prefix re-detection
// inside the body (`"0x0x10"` is 16). A malformed text or an out-of-range value
// fails with a message and exit 70, matching the hosted runtime-error code.
static void test_int_string_conversion() {
  Bytecode proto = compileSource(
      "print(int(\"42\"), int(\"-7\"), int(\"+7\"), int(\"0x10\"), int(\"0X1F\"));\n"
      "print(int(\"0b101\"), int(\"0o17\"), int(\"0\"), int(\"007\"), int(\" 42 \"), int(\"-0x10\"));\n"
      "print(int(\"9223372036854775807\"), int(\"-9223372036854775808\"));\n"
      "print(int(\"0x7fffffffffffffff\"), int(\"-0x8000000000000000\"));\n"
      "print(int(\"0x0x10\"), int(\"0b0b101\"), int(\"0x0b1\"), int(\"  0x1f  \"));\n"
      "print(int(\"4\\u00002\"));\n"
      "print(int(\"42\") + int(\"8\"));\n");
  RunResult result = buildAndRun("/tmp/oskel_int_string", proto);
  std::string expected =
      "42 -7 7 16 31\n"
      "5 15 0 7 42 -16\n"
      "9223372036854775807 -9223372036854775808\n"
      "9223372036854775807 -9223372036854775808\n"
      "16 5 177 31\n"
      "4\n"
      "50\n";
  check(result.output == expected,
        "int(string) parses like the hosted engine");
  check(result.exitCode == 0, "a program using int(string) exits 0");
}

// A malformed or out-of-range `int(string)` must fail with a message and exit
// 70, the hosted runtime-error code.
static void test_int_string_conversion_errors() {
  Bytecode empty = compileSource("print(int(\"\"));\n");
  RunResult blank = buildAndRun("/tmp/oskel_int_string_empty", empty);
  check(blank.exitCode == 70, "int of an empty string exits 70");

  Bytecode junk = compileSource("print(int(\"12ab\"));\n");
  RunResult trailing = buildAndRun("/tmp/oskel_int_string_junk", junk);
  check(trailing.exitCode == 70, "int of a string with trailing text exits 70");
  check(trailing.output.find("cannot convert") != std::string::npos,
        "the failed int(string) reports the conversion message");

  Bytecode noDigits = compileSource("print(int(\"0x\"));\n");
  RunResult barePrefix = buildAndRun("/tmp/oskel_int_string_prefix", noDigits);
  check(barePrefix.exitCode == 70, "int of a bare base prefix exits 70");

  Bytecode overflow = compileSource("print(int(\"99999999999999999999999\"));\n");
  RunResult big = buildAndRun("/tmp/oskel_int_string_overflow", overflow);
  check(big.exitCode == 70, "int of an out-of-range string exits 70");

  Bytecode fromFloatString = compileSource("print(float(\"abc\"));\n");
  RunResult bad = buildAndRun("/tmp/oskel_float_string", fromFloatString);
  check(bad.exitCode == 70, "float of a malformed string exits 70");
  check(bad.output.find("cannot convert this value to float") != std::string::npos,
        "the failed float(string) reports the conversion message");
}

// `float(string)`/`double(string)` (7c.5b.2b) parse decimal and hex text to the
// nearest binary64 with the freestanding parser and must match the hosted
// engine bit for bit, including the subnormal and overflow edges.
static void test_float_string_conversion() {
  Bytecode proto = compileSource(
      "print(double(\"3.14\"), float(\"3.14\"), double(\"1e10\"), double(\"-2.5\"));\n"
      "print(double(\"0.1\") == 0.1, float(\"0.1\") == 0.1);\n"
      "print(double(\"  42  \"), double(\"+.5\"), double(\".5\"), double(\"5.\"));\n"
      "print(double(\"9007199254740993\"), double(\"1.7976931348623157e308\"));\n"
      "print(double(\"5e-324\"), double(\"2.4703282292062328e-324\"));\n"
      "print(double(\"inf\"), double(\"-inf\"), double(\"infinity\"), double(\"NaN\"));\n"
      "print(double(\"0x1.8p1\"), double(\"0x1p-1074\"), double(\"0x1.8\"));\n"
      "print(float(\"0.1\") + float(\"0.2\"));\n");
  RunResult result = buildAndRun("/tmp/oskel_float_string_ok", proto);
  std::string expected =
      "3.14 3.14 1e+10 -2.5\n"
      "true false\n"
      "42 0.5 0.5 5\n"
      "9007199254740992 1.7976931348623157e+308\n"
      "5e-324 5e-324\n"
      "inf -inf inf nan\n"
      "3 5e-324 1.5\n"
      "0.3\n";
  check(result.output == expected,
        "float(string)/double(string) parse like the hosted engine");
  check(result.exitCode == 0, "a program using float(string) exits 0");

  // A silent overflow (`1e400`) is a clean error, not an infinity, exactly as
  // the hosted engine rejects it.
  Bytecode overflow = compileSource("print(double(\"1e400\"));\n");
  RunResult big = buildAndRun("/tmp/oskel_float_string_overflow", overflow);
  check(big.exitCode == 70, "double of an overflowing string exits 70");
  check(big.output.find("cannot convert this value to double") != std::string::npos,
        "the overflow reports the conversion message");
}

// The exact math builtins (PHASE 7c.5b.3): `floor`, `ceil`, `round` and `sqrt`
// are correctly rounded from SSE alone, so a compiled program matches the
// hosted engine bit for bit, negative zero included. The values below cover the
// classic traps: a value just under a half (`0.49999999999999994`, whose sum
// with 0.5 rounds up and would break a naive `trunc(x + 0.5)`), the int
// passthrough, the `|x| >= 2^63` bypass and `sqrt(-0.0)`.
static void test_math_builtins() {
  Bytecode proto = compileSource(
      "print(floor(2.5), floor(-2.5), ceil(2.1), ceil(-2.1), round(2.5), round(-2.5));\n"
      "print(floor(2), ceil(-3), round(7), sqrt(9), sqrt(0));\n"
      "print(type(floor(1.5)), type(floor(1)), type(sqrt(4)));\n"
      "print(round(0.49999999999999994), round(4503599627370495.5));\n"
      "print(round(0.5), round(1.5), round(2.5), round(-0.5), round(-1.5));\n"
      "print(sqrt(2.0));\n"
      "print(floor(1e300), ceil(-1e300), round(1e300));\n");
  RunResult result = buildAndRun("/tmp/oskel_math_builtins", proto);
  std::string expected =
      "2 -3 3 -2 3 -3\n"
      "2 -3 7 3 0\n"
      "double int double\n"
      "0 4503599627370496\n"
      "1 2 3 -1 -2\n"
      "1.4142135623730951\n"
      "1e+300 -1e+300 1e+300\n";
  check(result.output == expected, "the math builtins match the hosted engine");
  check(result.exitCode == 0, "a program using the math builtins exits 0");

  // The negative-zero sign is preserved, so `str` renders "-0" as it does in
  // the hosted engine (a plain `== 0.0` comparison cannot see the sign).
  Bytecode zeros = compileSource(
      "print(floor(-0.0), ceil(-0.3), round(-0.5), sqrt(-0.0));\n"
      "print(floor(0.0), ceil(0.0), round(0.0));\n");
  RunResult zero = buildAndRun("/tmp/oskel_math_negative_zero", zeros);
  check(zero.output == "-0 -0 -1 -0\n0 0 0\n",
        "the math builtins preserve the sign of a zero result");
}

// A wrong operand type or a negative `sqrt` fails with a message and exit 70,
// matching the hosted engine's runtime-error code.
static void test_math_builtin_errors() {
  Bytecode boolArg = compileSource("print(floor(true));\n");
  RunResult fromBool = buildAndRun("/tmp/oskel_math_bool", boolArg);
  check(fromBool.exitCode == 70, "floor of a bool exits 70");
  check(fromBool.output.find("floor() expects a number argument") != std::string::npos,
        "the failed floor reports its type message");

  Bytecode negative = compileSource("print(sqrt(-1.0));\n");
  RunResult neg = buildAndRun("/tmp/oskel_math_sqrt_negative", negative);
  check(neg.exitCode == 70, "sqrt of a negative number exits 70");
  check(neg.output.find("sqrt() of a negative number") != std::string::npos,
        "the failed sqrt reports the negative-operand message");

  Bytecode noArgs = compileSource("print(round());\n");
  RunResult arity = buildAndRun("/tmp/oskel_math_arity", noArgs);
  check(arity.exitCode == 70, "round with no argument exits 70");
}

// The transcendental builtins (PHASE 7c.5b.4a): `exp`, `log` and `pow` call the
// embedded musl routines. glibc is not correctly rounded for these, so the
// contract is "agrees to 1 ulp"; the values below are chosen where the two
// roundings coincide, so the emitted code reproduces the hosted output exactly.
static void test_transcendental_builtins() {
  Bytecode proto = compileSource(
      "print(exp(0), exp(1), exp(2), exp(-1), exp(0.5));\n"
      "print(log(1), log(2), log(10), log(100));\n"
      "print(pow(2, 10), pow(9, 0.5), pow(2, 0.5), pow(3, 3), pow(2, -1));\n"
      "print(type(exp(1)), type(log(2)), type(pow(2, 3)));\n"
      "print(exp(log(5)), log(exp(3)));\n");
  RunResult result = buildAndRun("/tmp/oskel_transcendental_builtins", proto);
  std::string expected =
      "1 2.718281828459045 7.38905609893065 0.36787944117144233 1.6487212707001282\n"
      "0 0.6931471805599453 2.302585092994046 4.605170185988092\n"
      "1024 3 1.4142135623730951 27 0.5\n"
      "double double double\n"
      "4.999999999999999 3\n";
  check(result.output == expected, "the transcendental builtins match the hosted engine");
  check(result.exitCode == 0, "a program using exp/log/pow exits 0");
}

// A wrong operand type fails with the hosted message and exit 70; `log` of a
// non-positive operand fails like the hosted `if (value <= 0)`.
static void test_transcendental_builtin_errors() {
  Bytecode expBool = compileSource("print(exp(true));\n");
  RunResult fromBool = buildAndRun("/tmp/oskel_exp_bool", expBool);
  check(fromBool.exitCode == 70, "exp of a bool exits 70");
  check(fromBool.output.find("exp() expects a number argument") != std::string::npos,
        "the failed exp reports its type message");

  Bytecode powStr = compileSource("print(pow(2, \"x\"));\n");
  RunResult fromStr = buildAndRun("/tmp/oskel_pow_str", powStr);
  check(fromStr.exitCode == 70, "pow of a string exits 70");
  check(fromStr.output.find("pow() expects a number argument") != std::string::npos,
        "the failed pow reports its type message");

  Bytecode logZero = compileSource("print(log(0.0));\n");
  RunResult zero = buildAndRun("/tmp/oskel_log_zero", logZero);
  check(zero.exitCode == 70, "log of zero exits 70");
  check(zero.output.find("log() of a non-positive number") != std::string::npos,
        "the failed log reports the non-positive message");

  Bytecode powArity = compileSource("print(pow(2));\n");
  RunResult arity = buildAndRun("/tmp/oskel_pow_arity", powArity);
  check(arity.exitCode == 70, "pow with one argument exits 70");
}

// The trigonometric builtins (PHASE 7c.5b.4b): `sin`, `cos` and `tan` call the
// embedded musl routines, which reduce the argument with `__rem_pio2`. glibc is
// not correctly rounded, so the contract is "agrees to 1 ulp"; the values below
// are chosen where the two roundings coincide, so the emitted code reproduces
// the hosted output exactly. The last two lines drive the large-argument branch
// of the reduction (>= 2^20 * pi/2), which a small input never reaches.
static void test_trigonometric_builtins() {
  Bytecode proto = compileSource(
      "print(sin(0), cos(0), tan(0));\n"
      "print(sin(0.5), cos(0.5), tan(0.5));\n"
      "print(sin(-0.5), cos(-0.5), tan(-0.5));\n"
      "print(type(sin(1)), type(cos(1)), type(tan(1)));\n"
      "print(sin(-2.0203875408674627e+175), cos(-2.0203875408674627e+175));\n"
      "print(tan(-2.0203875408674627e+175), sin(1.2857398720274479e+182));\n");
  RunResult result = buildAndRun("/tmp/oskel_trigonometric_builtins", proto);
  std::string expected =
      "0 1 0\n"
      "0.479425538604203 0.8775825618903728 0.5463024898437905\n"
      "-0.479425538604203 0.8775825618903728 -0.5463024898437905\n"
      "double double double\n"
      "-0.6788283655659167 0.7342969767764989\n"
      "-0.9244602484214429 0.989033555430505\n";
  check(result.output == expected, "the trigonometric builtins match the hosted engine");
  check(result.exitCode == 0, "a program using sin/cos/tan exits 0");
}

// A wrong operand type and a wrong arity both fail with the hosted message and
// exit 70.
static void test_trigonometric_builtin_errors() {
  Bytecode sinStr = compileSource("print(sin(\"x\"));\n");
  RunResult fromStr = buildAndRun("/tmp/oskel_sin_str", sinStr);
  check(fromStr.exitCode == 70, "sin of a string exits 70");
  check(fromStr.output.find("sin() expects a number argument") != std::string::npos,
        "the failed sin reports its type message");

  Bytecode cosArity = compileSource("print(cos());\n");
  RunResult arity = buildAndRun("/tmp/oskel_cos_arity", cosArity);
  check(arity.exitCode == 70, "cos with no argument exits 70");

  Bytecode tanBool = compileSource("print(tan(true));\n");
  RunResult fromBool = buildAndRun("/tmp/oskel_tan_bool", tanBool);
  check(fromBool.exitCode == 70, "tan of a bool exits 70");
  check(fromBool.output.find("tan() expects a number argument") != std::string::npos,
        "the failed tan reports its type message");
}

// The remaining public builtins (PHASE 7c.5b.5): `range`, `assertEq` and
// `exit`. `range` must match the hosted builtin (one bound means `0..end`),
// `assertEq` must return on a match and fail with the hosted message on a
// mismatch, and `exit` must leave the process with the given code.
static void test_remaining_public_builtins() {
  Bytecode range = compileSource(
      "print(range(3));\n"
      "print(range(1, 5));\n"
      "print(range(2, 2));\n"
      "print(range(-2, 2));\n"
      "print(range(0, 4).length);\n");
  RunResult ranges = buildAndRun("/tmp/oskel_range_fn", range);
  check(ranges.output == "0..3\n1..5\n2..2\n-2..2\n4\n",
        "range matches the hosted builtin");
  check(ranges.exitCode == 0, "range exits 0");

  Bytecode assertOk = compileSource(
      "assertEq(1 + 2, 3);\n"
      "assertEq([1, 2], [1, 2]);\n"
      "assertEq(\"a\", \"a\");\n"
      "assertEq(nil, nil);\n"
      "print(\"ok\");\n");
  RunResult passed = buildAndRun("/tmp/oskel_assert_eq_ok", assertOk);
  check(passed.output == "ok\n", "assertEq returns nil when the values match");
  check(passed.exitCode == 0, "a passing assertEq exits 0");

  Bytecode assertFail = compileSource("assertEq(1, 2);\n");
  RunResult failed = buildAndRun("/tmp/oskel_assert_eq_fail", assertFail);
  check(failed.exitCode == 70, "a failing assertEq exits 70");
  check(failed.output.find("oskel: assertion failed: expected 1, got 2") !=
            std::string::npos,
        "a failing assertEq reports the default message and both operands");

  Bytecode assertFailMsg = compileSource("assertEq(\"a\", \"b\", \"my check\");\n");
  RunResult custom = buildAndRun("/tmp/oskel_assert_eq_msg", assertFailMsg);
  check(custom.exitCode == 70, "a failing assertEq with a message exits 70");
  check(custom.output.find("oskel: my check: expected \"a\", got \"b\"") !=
            std::string::npos,
        "a failing assertEq reports the caller's message and the reprs");

  Bytecode assertArity = compileSource("assertEq(1);\n");
  RunResult arity = buildAndRun("/tmp/oskel_assert_eq_arity", assertArity);
  check(arity.exitCode == 70, "assertEq with one argument exits 70");
  check(arity.output.find("assertEq() expects between 2 and 3 arguments") !=
            std::string::npos,
        "the failed assertEq reports its arity message");

  Bytecode exitCode = compileSource("print(\"before\");\nexit(3);\nprint(\"after\");\n");
  RunResult stopped = buildAndRun("/tmp/oskel_exit_code", exitCode);
  check(stopped.output == "before\n", "exit stops before the next statement");
  check(stopped.exitCode == 3, "exit uses the given code");

  Bytecode exitZero = compileSource("exit();\n");
  RunResult zero = buildAndRun("/tmp/oskel_exit_zero", exitZero);
  check(zero.exitCode == 0, "exit without an argument exits 0");
}

// A wrong argument count or a non-integer bound fails with the hosted message
// and exit code 70, matching the hosted engine.
static void test_remaining_public_builtin_errors() {
  Bytecode rangeArgc = compileSource("print(range());\n");
  RunResult rangeArity = buildAndRun("/tmp/oskel_range_fn_arity", rangeArgc);
  check(rangeArity.exitCode == 70, "range with no argument exits 70");
  check(rangeArity.output.find("range() expects between 1 and 2 arguments") !=
            std::string::npos,
        "the failed range reports its arity message");

  Bytecode rangeEnd = compileSource("print(range(\"x\"));\n");
  RunResult end = buildAndRun("/tmp/oskel_range_fn_end", rangeEnd);
  check(end.exitCode == 70, "range of a non-integer end exits 70");
  check(end.output.find("range() expects an integer end") != std::string::npos,
        "the failed range names the end bound");

  Bytecode rangeStart = compileSource("print(range(\"x\", 3));\n");
  RunResult start = buildAndRun("/tmp/oskel_range_fn_start", rangeStart);
  check(start.exitCode == 70, "range of a non-integer start exits 70");
  check(start.output.find("range() expects an integer start") != std::string::npos,
        "the failed range names the start bound");

  Bytecode exitType = compileSource("exit(\"x\");\n");
  RunResult type = buildAndRun("/tmp/oskel_exit_type", exitType);
  check(type.exitCode == 70, "exit of a non-integer code exits 70");
  check(type.output.find("exit() expects an integer argument") != std::string::npos,
        "the failed exit reports its type message");

  Bytecode exitArity = compileSource("exit(1, 2);\n");
  RunResult arity = buildAndRun("/tmp/oskel_exit_arity", exitArity);
  check(arity.exitCode == 70, "exit with two arguments exits 70");
  check(arity.output.find("exit() expects between 0 and 1 arguments") !=
            std::string::npos,
        "the failed exit reports its arity message");
}

// Drives the boxed printer directly: the runtime's `oskel_print_value` is what
// the `print` builtin will call once `CALL` lands (7c.2), so this checks the
// integer-to-text routine (including the negative case) and the boolean/nil
// rendering today.
static void test_boxed_runtime_print() {
  CodeGen cg(Target::linuxX64());
  RuntimeHandles entry = newRuntimeHandles(cg);
  AotRuntimeHandles rt = newAotRuntimeHandles(cg);
  CodeGen::Label mainLabel = cg.newLabel();

  auto printValue = [&](AotTag tag, std::int64_t payload) {
    cg.movImm(X64Reg::Rdi, static_cast<std::int64_t>(tag));
    cg.movImm(X64Reg::Rsi, payload);
    cg.callLabel(rt.printValue);
  };

  cg.bind(mainLabel);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  printValue(AotTag::Int, 42);
  printValue(AotTag::Int, -7);
  printValue(AotTag::Bool, 1);
  printValue(AotTag::Bool, 0);
  printValue(AotTag::Nil, 0);
  // Float/Double render through the embedded dtoa with the hosted engine's
  // shortest round-trip text: a double uses binary64, a float binary32.
  printValue(AotTag::Double, static_cast<std::int64_t>(aotDouble(0.1 + 0.2).payload));
  printValue(AotTag::Double, static_cast<std::int64_t>(aotDouble(1.0 / 3.0).payload));
  printValue(AotTag::Double, static_cast<std::int64_t>(aotDouble(-2.5).payload));
  printValue(AotTag::Float, static_cast<std::int64_t>(aotFloat(1.0f / 3.0f).payload));
  printValue(AotTag::Float, static_cast<std::int64_t>(aotFloat(0.1f).payload));
  cg.xorReg(X64Reg::Rax, X64Reg::Rax);
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();

  std::uint64_t start = emitFreestandingRuntime(cg, entry, mainLabel);
  emitAotRuntime(cg, rt, entry);
  cg.finalize();

  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), cg.code(), {}, start);
  const std::string path = "/tmp/oskel_boxed_print";
  writeFile(path, elf);
  RunResult result = runProgram(path);
  check(result.output == "42\n-7\ntrue\nfalse\nnil\n"
                         "0.30000000000000004\n0.3333333333333333\n-2.5\n"
                         "0.33333334\n0.1\n",
        "the boxed runtime prints int, bool, nil, double and float");
  check(result.exitCode == 0, "the printing program exits 0");
}

// The global table (PHASE 7c.2a) inside a real static ELF: the chunk allocates
// it from the arena at entry, every slot starts nil, and set/get round-trip
// through r15. The helpers are driven directly, like the printer above.
static void test_boxed_runtime_globals() {
  CodeGen cg(Target::linuxX64());
  RuntimeHandles entry = newRuntimeHandles(cg);
  AotRuntimeHandles rt = newAotRuntimeHandles(cg);
  CodeGen::Label mainLabel = cg.newLabel();

  auto printSlot = [&](int slot) {
    cg.movImm(X64Reg::Rdi, slot);
    cg.callLabel(rt.globalGet);
    cg.movReg(X64Reg::Rdi, X64Reg::Rax);
    cg.movReg(X64Reg::Rsi, X64Reg::Rdx);
    cg.callLabel(rt.printValue);
  };
  auto setInt = [&](int slot, std::int64_t value) {
    cg.movImm(X64Reg::Rdi, slot);
    cg.movImm(X64Reg::Rsi, static_cast<std::int64_t>(AotTag::Int));
    cg.movImm(X64Reg::Rdx, value);
    cg.callLabel(rt.globalSet);
  };

  cg.bind(mainLabel);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.callLabel(entry.initArena);
  cg.movImm(X64Reg::Rdi, 3);
  cg.callLabel(rt.initGlobals);
  setInt(0, 7);
  setInt(2, -9);
  printSlot(0);  // 7
  printSlot(2);  // -9
  printSlot(1);  // nil: never assigned
  cg.xorReg(X64Reg::Rax, X64Reg::Rax);
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();

  std::uint64_t start = emitFreestandingRuntime(cg, entry, mainLabel);
  emitAotRuntime(cg, rt, entry);
  cg.finalize();

  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), cg.code(), {}, start);
  const std::string path = "/tmp/oskel_boxed_globals";
  writeFile(path, elf);
  RunResult result = runProgram(path);
  check(result.output == "7\n-9\nnil\n",
        "the boxed runtime keeps globals in distinct slots and starts them nil");
  check(result.exitCode == 0, "the global program exits 0");
}

// A type mismatch in a lowered operation must report an error and exit 70,
// matching the development engine's runtime-error exit code.
static void test_boxed_runtime_error() {
  // R0 = true; R1 = false; R0 = R0 - R1 -> subtraction is int-only, so this
  // must hit the runtime's "unsupported operand types" path and exit 70.
  Bytecode proto = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                              instr(OpCode::SUB, 0, 0, 1), instr(OpCode::HALT)},
                             {Value(true), Value(false)}, 2);
  RunResult result = buildAndRun("/tmp/oskel_boxed_error", proto);
  check(result.exitCode == 70, "a runtime type error exits 70");
  check(result.output.find("unsupported operand types") != std::string::npos,
        "the runtime error names the unsupported operand types");
}

// A division by zero in a lowered operation must report an error and exit 70,
// exercising the new error path in a real static ELF.
static void test_boxed_runtime_div_zero() {
  Bytecode proto = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                              instr(OpCode::DIV, 0, 0, 1), instr(OpCode::HALT)},
                             {Value(static_cast<int64_t>(1)), Value(static_cast<int64_t>(0))}, 2);
  RunResult result = buildAndRun("/tmp/oskel_boxed_divzero", proto);
  check(result.exitCode == 70, "a division by zero exits 70");
  check(result.output.find("division by zero") != std::string::npos,
        "the runtime error names the division by zero");
}

// Calls (PHASE 7c.2c.1) through the real process entry: a hand-built chunk with
// a child proto, a CLOSURE, a CALL and a RETURN must build, run under `_start`
// and return the callee's value in R0. The exact value is checked by printing
// it, so this also exercises the runtime dispatch path end to end.
static void test_boxed_runtime_call() {
  // Root: R0 = closure(add); R1 = 20; R2 = 22; R0 = R0(R1, R2); print R0; HALT.
  Bytecode root = makeProto({instr(OpCode::CLOSURE, 0, 0), instr(OpCode::LOADK, 1, 0),
                             instr(OpCode::LOADK, 2, 1), instr(OpCode::CALL, 0, 2),
                             instr(OpCode::HALT)},
                            {Value(static_cast<int64_t>(20)), Value(static_cast<int64_t>(22))}, 3);
  root.name = "<call-root>";
  auto child = std::make_shared<Bytecode>();
  child->name = "add";
  child->numParams = 2;
  child->numRegisters = 2;
  child->numLocals = 2;
  child->code = {instr(OpCode::DECLLOCAL, 0, 0), instr(OpCode::DECLLOCAL, 1, 1),
                 instr(OpCode::GETLOCAL, 0, 0), instr(OpCode::GETLOCAL, 1, 1),
                 instr(OpCode::ADD, 0, 0, 1), instr(OpCode::RETURN, 0)};
  root.children.push_back(child);

  // Run the chunk directly and read the AotValue it leaves in R0. Printing the
  // result needs the `print` builtin, which lands with the library (7c.2c.2), so
  // this test reads R0 through the program entry like `lower_test` does.
  LoweredChunk chunk = lowerToX64(Target::linuxX64(), root, CodeGen{});
  long pageSize = sysconf(_SC_PAGESIZE);
  std::size_t span = static_cast<std::size_t>(pageSize);
  while (span < chunk.code.size()) span += static_cast<std::size_t>(pageSize);
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    check(false, "mmap for the call test");
    return;
  }
  std::memcpy(mem, chunk.code.data(), chunk.code.size());
  mprotect(mem, span, PROT_READ | PROT_EXEC);
  auto fn = reinterpret_cast<AotValue (*)()>(
      static_cast<std::uint8_t*>(mem) + chunk.programEntry);
  AotValue value = fn();
  check(value.tag == static_cast<std::uint64_t>(AotTag::Int) &&
            static_cast<std::int64_t>(value.payload) == 42,
        "a lowered CALL returns the callee's result (20 + 22 == 42)");

  // The same chunk through `_start` must exit 0 (R0 is discarded by `main`).
  RunResult result = buildAndRun("/tmp/oskel_boxed_call", root);
  check(result.exitCode == 0, "a chunk with a call exits 0 through _start");
}

// Calling a non-function value is a fatal runtime error: build a chunk that
// calls an integer and check the error message and exit code 70.
static void test_boxed_runtime_call_non_function() {
  // R0 = 5; R1 = 7; R0 = R0(R1) -> R0 is not a function.
  Bytecode proto = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                              instr(OpCode::CALL, 0, 1), instr(OpCode::HALT)},
                             {Value(static_cast<int64_t>(5)), Value(static_cast<int64_t>(7))}, 2);
  RunResult result = buildAndRun("/tmp/oskel_boxed_call_nonfunc", proto);
  check(result.exitCode == 70, "calling a non-function exits 70");
  check(result.output.find("non-function") != std::string::npos,
        "the runtime error names the non-function call");
}

// Captured locals through the real process entry (PHASE 7c.2c.2): a child
// proto that captures a local of its parent. The closure escapes its defining
// frame and is called afterwards, so the captured cell must outlive the frame.
// The root reads the result back into R0 (checked by direct execution) and the
// same chunk runs cleanly under `_start`.
static void test_boxed_runtime_closure_capture() {
  // Root: R0 = closure(inner)  -- `inner` captures the root's local slot 0.
  //       R1 = 40; R0 = R0(); R0 = R0 + R1; HALT.  -> 40 + 40 = 80 in R0.
  Bytecode root = makeProto({instr(OpCode::LOADK, 1, 0), instr(OpCode::DECLLOCAL, 1, 0),
                             instr(OpCode::CLOSURE, 0, 0), instr(OpCode::CALL, 0, 0),
                             instr(OpCode::ADD, 0, 0, 1), instr(OpCode::HALT)},
                            {Value(static_cast<int64_t>(40))}, 3, 1);
  root.name = "<capture-root>";
  auto inner = std::make_shared<Bytecode>();
  inner->name = "inner";
  inner->numParams = 0;
  inner->numRegisters = 1;
  inner->numLocals = 0;
  inner->upvalues.push_back(UpvalDesc{true, 0});  // capture the parent's slot 0
  inner->code = {instr(OpCode::GETUPVAL, 0, 0), instr(OpCode::RETURN, 0)};
  root.children.push_back(inner);

  LoweredChunk chunk = lowerToX64(Target::linuxX64(), root, CodeGen{});
  long pageSize = sysconf(_SC_PAGESIZE);
  std::size_t span = static_cast<std::size_t>(pageSize);
  while (span < chunk.code.size()) span += static_cast<std::size_t>(pageSize);
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    check(false, "mmap for the closure capture test");
    return;
  }
  std::memcpy(mem, chunk.code.data(), chunk.code.size());
  mprotect(mem, span, PROT_READ | PROT_EXEC);
  auto fn = reinterpret_cast<AotValue (*)()>(
      static_cast<std::uint8_t*>(mem) + chunk.programEntry);
  AotValue value = fn();
  check(value.tag == static_cast<std::uint64_t>(AotTag::Int) &&
            static_cast<std::int64_t>(value.payload) == 80,
        "a lowered closure reads a captured local (40 + 40 == 80)");

  RunResult result = buildAndRun("/tmp/oskel_boxed_capture", root);
  check(result.exitCode == 0, "a chunk with a captured local exits 0 through _start");
}

// Locals (PHASE 7c.2b) through the real process entry: a chunk with a local
// region must build, allocate its larger frame and run cleanly under `_start`.
// The value itself is covered by direct execution in `lower_test.cpp`; here the
// subject is the frame-size math on the freestanding path, so a clean exit and
// no output is the assertion.
static void test_boxed_runtime_locals() {
  Bytecode proto = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::DECLLOCAL, 0, 0),
                              instr(OpCode::LOADK, 1, 1), instr(OpCode::SETLOCAL, 1, 0),
                              instr(OpCode::GETLOCAL, 0, 0), instr(OpCode::HALT)},
                             {Value(static_cast<int64_t>(5)), Value(static_cast<int64_t>(7))},
                             2, 1);
  RunResult result = buildAndRun("/tmp/oskel_boxed_locals", proto);
  check(result.output.empty(), "a locals-only chunk prints nothing");
  check(result.exitCode == 0, "a chunk with a local region exits 0 through _start");
}

// Strings over the arena, end to end: lay out two `AotString`s in the image,
// concatenate them with the runtime's `oskel_concat` (which bumps the mmap'd
// arena) and print the result. Exercises the arena mapping in `_start`, the
// allocator, the byte copy and the string print path in one static binary.
static void test_boxed_runtime_string() {
  CodeGen cg(Target::linuxX64());
  RuntimeHandles entry = newRuntimeHandles(cg);
  AotRuntimeHandles rt = newAotRuntimeHandles(cg);
  CodeGen::Label mainLabel = cg.newLabel();

  // Data islands: each is an 8-byte length followed by the bytes.
  const std::string a = "hello, ";
  const std::string b = "world!";
  CodeGen::Label aLabel = cg.newLabel();
  CodeGen::Label bLabel = cg.newLabel();

  cg.bind(mainLabel);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.callLabel(entry.initArena);  // map the arena the concatenation allocates from
  // rdi:rsi = a, rdx:rcx = b
  cg.movImm(X64Reg::Rdi, static_cast<std::int64_t>(AotTag::Str));
  cg.leaRipLabel(X64Reg::Rsi, aLabel);
  cg.movImm(X64Reg::Rdx, static_cast<std::int64_t>(AotTag::Str));
  cg.leaRipLabel(X64Reg::Rcx, bLabel);
  cg.callLabel(rt.concat);          // rax:rdx = "hello, world!"
  cg.movReg(X64Reg::Rdi, X64Reg::Rax);
  cg.movReg(X64Reg::Rsi, X64Reg::Rdx);
  cg.callLabel(rt.printValue);
  cg.xorReg(X64Reg::Rax, X64Reg::Rax);
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();

  std::uint64_t start = emitFreestandingRuntime(cg, entry, mainLabel);
  emitAotRuntime(cg, rt, entry);

  // Bind the string data islands after all code, then finalize.
  auto emitString = [&](CodeGen::Label label, const std::string& text) {
    cg.bind(label);
    const std::uint64_t length = text.size();
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(&length), sizeof length);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  };
  emitString(aLabel, a);
  emitString(bLabel, b);
  cg.finalize();

  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), cg.code(), {}, start);
  const std::string path = "/tmp/oskel_boxed_string";
  writeFile(path, elf);
  RunResult result = runProgram(path);
  check(result.output == "hello, world!\n",
        "the boxed runtime concatenates and prints strings from the arena");
  check(result.exitCode == 0, "the string program exits 0");
}

// Float `%` end to end: the x87 `fprem` path runs inside a real static ELF.
// The remainder is compared against the expected value and the boolean is
// printed, since float-to-text (`print` of a double) arrives with the dtoa
// slice. A correct remainder prints "true".
static void test_boxed_runtime_float_mod() {
  CodeGen cg(Target::linuxX64());
  RuntimeHandles entry = newRuntimeHandles(cg);
  AotRuntimeHandles rt = newAotRuntimeHandles(cg);
  CodeGen::Label mainLabel = cg.newLabel();

  // Emits `print((a % b) == expected)` for three double pairs.
  auto checkMod = [&](double a, double b, double expected) {
    cg.movImm(X64Reg::Rdi, static_cast<std::int64_t>(AotTag::Double));
    cg.movImm(X64Reg::Rsi, static_cast<std::int64_t>(aotDouble(a).payload));
    cg.movImm(X64Reg::Rdx, static_cast<std::int64_t>(AotTag::Double));
    cg.movImm(X64Reg::Rcx, static_cast<std::int64_t>(aotDouble(b).payload));
    cg.movImm(X64Reg::R8, static_cast<std::int64_t>(AotArithOp::Mod));
    cg.callLabel(rt.arithValues);  // rax:rdx = a % b
    cg.movReg(X64Reg::Rdi, X64Reg::Rax);
    cg.movReg(X64Reg::Rsi, X64Reg::Rdx);
    cg.movImm(X64Reg::Rdx, static_cast<std::int64_t>(AotTag::Double));
    cg.movImm(X64Reg::Rcx, static_cast<std::int64_t>(aotDouble(expected).payload));
    cg.movImm(X64Reg::R8, static_cast<std::int64_t>(AotCompareOp::Eq));
    cg.callLabel(rt.compareValues);  // rax:rdx = bool
    cg.movReg(X64Reg::Rdi, X64Reg::Rax);
    cg.movReg(X64Reg::Rsi, X64Reg::Rdx);
    cg.callLabel(rt.printValue);
  };

  cg.bind(mainLabel);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  checkMod(7.0, 2.5, 2.0);
  checkMod(-7.0, 3.0, -1.0);
  checkMod(7.0, -3.0, 1.0);
  cg.xorReg(X64Reg::Rax, X64Reg::Rax);
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();

  std::uint64_t start = emitFreestandingRuntime(cg, entry, mainLabel);
  emitAotRuntime(cg, rt, entry);
  cg.finalize();

  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), cg.code(), {}, start);
  const std::string path = "/tmp/oskel_boxed_float_mod";
  writeFile(path, elf);
  RunResult result = runProgram(path);
  check(result.output == "true\ntrue\ntrue\n",
        "the boxed runtime computes float remainders with fprem");
  check(result.exitCode == 0, "the float mod program exits 0");
}

// The string builder must grow its block when the current capacity is short.
// Two edge cases a caller can reach but the hosted engine never does: a
// capacity of zero (doubling 0 never reaches a positive value, so the loop
// would spin forever) and a capacity whose double overflows int64. The first
// must append the exact bytes; the second must stop growing and fail cleanly
// instead of looping.
static void test_sb_append_zero_capacity() {
  CodeGen cg(Target::linuxX64());
  RuntimeHandles entry = newRuntimeHandles(cg);
  AotRuntimeHandles rt = newAotRuntimeHandles(cg);
  CodeGen::Label mainLabel = cg.newLabel();
  CodeGen::Label data = cg.newLabel();

  const std::string first = "abc";
  const std::string second = "defgh";

  cg.bind(mainLabel);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.subRegImm32(X64Reg::Rsp, 32);          // room for the builder header
  cg.callLabel(entry.initArena);
  // Builder header at [rbp-24] (length), [rbp-16] (capacity), [rbp-8] (data).
  cg.leaMem(X64Reg::Rdi, X64Reg::Rbp, -24);
  cg.xorReg(X64Reg::Rsi, X64Reg::Rsi);      // data = null, unused while capacity is 0
  cg.xorReg(X64Reg::Rdx, X64Reg::Rdx);      // capacity = 0
  cg.callLabel(rt.sbInit);
  // Append "abc", then "defgh".
  cg.leaMem(X64Reg::Rdi, X64Reg::Rbp, -24);
  cg.leaRipLabel(X64Reg::Rsi, data);
  cg.movImm(X64Reg::Rdx, static_cast<std::int64_t>(first.size()));
  cg.callLabel(rt.sbAppend);
  cg.leaMem(X64Reg::Rdi, X64Reg::Rbp, -24);
  cg.leaRipLabel(X64Reg::Rsi, data);
  cg.addImm32(X64Reg::Rsi, static_cast<std::int32_t>(first.size()));
  cg.movImm(X64Reg::Rdx, static_cast<std::int64_t>(second.size()));
  cg.callLabel(rt.sbAppend);
  // Write the accumulated bytes to stdout.
  cg.movImm(X64Reg::Rdi, 1);
  cg.movFromMem(X64Reg::Rsi, X64Reg::Rbp, -8);   // data
  cg.movFromMem(X64Reg::Rdx, X64Reg::Rbp, -24);  // length
  cg.callLabel(entry.write);
  cg.xorReg(X64Reg::Rax, X64Reg::Rax);
  cg.addImm32(X64Reg::Rsp, 32);
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();

  std::uint64_t start = emitFreestandingRuntime(cg, entry, mainLabel);
  emitAotRuntime(cg, rt, entry);
  cg.bind(data);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>(first.data()), first.size());
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>(second.data()), second.size());
  cg.finalize();

  std::vector<std::uint8_t> elf = buildStaticElf(Target::linuxX64(), cg.code(), {}, start);
  const std::string path = "/tmp/oskel_sb_zero_capacity";
  writeFile(path, elf);
  RunResult result = runProgram(path);
  check(result.output == "abcdefgh",
        "a zero-capacity builder grows from the needed size and keeps the bytes");
  check(result.exitCode == 0, "the zero-capacity builder program exits 0");
}

// A capacity past INT64_MAX/2 cannot double without overflowing, so the grow
// loop must fall back to the exact size needed. That size is far past the
// arena, so the result is a clean "out of memory" exit, never a hang.
static void test_sb_append_capacity_overflow() {
  CodeGen cg(Target::linuxX64());
  RuntimeHandles entry = newRuntimeHandles(cg);
  AotRuntimeHandles rt = newAotRuntimeHandles(cg);
  CodeGen::Label mainLabel = cg.newLabel();
  CodeGen::Label data = cg.newLabel();

  // capacity = INT64_MAX/2 + 1 (doubling it overflows) and needed one past it,
  // so the grow loop is entered and must stop before `capacity * 2`.
  const std::int64_t capacity = std::numeric_limits<std::int64_t>::max() / 2 + 1;
  const std::int64_t len = capacity + 1;

  cg.bind(mainLabel);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.subRegImm32(X64Reg::Rsp, 32);
  cg.callLabel(entry.initArena);
  cg.leaMem(X64Reg::Rdi, X64Reg::Rbp, -24);
  cg.leaRipLabel(X64Reg::Rsi, data);
  cg.movImm(X64Reg::Rdx, capacity);
  cg.callLabel(rt.sbInit);
  cg.leaMem(X64Reg::Rdi, X64Reg::Rbp, -24);
  cg.leaRipLabel(X64Reg::Rsi, data);
  cg.movImm(X64Reg::Rdx, len);
  cg.callLabel(rt.sbAppend);
  cg.xorReg(X64Reg::Rax, X64Reg::Rax);
  cg.addImm32(X64Reg::Rsp, 32);
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();

  std::uint64_t start = emitFreestandingRuntime(cg, entry, mainLabel);
  emitAotRuntime(cg, rt, entry);
  cg.bind(data);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("x"), 1);
  cg.finalize();

  std::vector<std::uint8_t> elf = buildStaticElf(Target::linuxX64(), cg.code(), {}, start);
  const std::string path = "/tmp/oskel_sb_capacity_overflow";
  writeFile(path, elf);
  RunResult result = runProgram(path);
  check(result.output == "oskel: out of memory\n",
        "a capacity that cannot double falls back to the needed size and reports OOM");
  check(result.exitCode == 70, "the overflow case exits through the runtime error path");
}

// Variadic parameters (PHASE 7c.6), end to end: a trailing `rest...` parameter
// binds an array of the extras. The AOT prologue binds the fixed parameters and
// calls `oskel_pack_rest` to build the array, so a compiled program behaves like
// the VM for zero, one and many extras.
static void test_variadic() {
  Bytecode proto = compileSource(
      "fn sum(first, rest...) {\n"
      "  var total = first;\n"
      "  for x in rest { total = total + x; }\n"
      "  return total;\n"
      "}\n"
      "print(sum(1), sum(1, 2), sum(1, 2, 3, 4, 5));\n"
      "fn count(rest...) { return len(rest); }\n"
      "print(count(), count(7), count(1, 2, 3));\n"
      "fn join(sep, parts...) {\n"
      "  var out = \"\";\n"
      "  var first = true;\n"
      "  for p in parts {\n"
      "    if (first) { out = out + p; first = false; }\n"
      "    else { out = out + sep + p; }\n"
      "  }\n"
      "  return out;\n"
      "}\n"
      "print(join(\",\"), join(\",\", \"a\"), join(\",\", \"a\", \"b\", \"c\"));\n");
  RunResult result = buildAndRun("/tmp/oskel_variadic", proto);
  check(result.output == "1 3 15\n0 1 3\n a a,b,c\n",
        "a compiled program packs variadic extras into an array");
  check(result.exitCode == 0, "a program using variadic parameters exits 0");
}

// A variadic function called with fewer than its fixed parameters is an arity
// error and exits 70, matching the VM's arity check.
static void test_variadic_arity_error() {
  Bytecode proto = compileSource(
      "fn join(sep, parts...) { return len(parts); }\n"
      "print(join());\n");
  RunResult result = buildAndRun("/tmp/oskel_variadic_arity", proto);
  check(result.exitCode == 70, "a variadic call below the fixed arity exits 70");
  check(result.output.find("wrong number of arguments") != std::string::npos,
        "the variadic arity error names the wrong argument count");
}

// Structural equality of arrays and maps (PHASE 7 divergence fix). The hosted
// engine compares two collections by content (`Value::equals`), so a compiled
// binary must too: `[1, 2] == [1, 2]` is true, not pointer identity. The
// recursion in the equality helper also fixes `in` over an array of arrays.
static void test_structural_equality() {
  Bytecode proto = compileSource(
      "print([1, 2] == [1, 2]);\n"
      "print([1, 2] == [1, 3]);\n"
      "print([1, 2] == [1, 2, 3]);\n"
      "print([[1, 2]] == [[1, 2]]);\n"
      "print([[1, 2]] == [[1, 3]]);\n"
      "print({\"a\": 1, \"b\": 2} == {\"b\": 2, \"a\": 1});\n"
      "print({\"a\": 1} == {\"a\": 2});\n"
      "print({\"a\": 1} == {\"a\": 1, \"b\": 2});\n"
      "print([1, \"x\", true, nil] == [1, \"x\", true, nil]);\n"
      "print([1, \"x\", true, nil] == [1, \"x\", true, 0]);\n"
      "print([1.0] == [1]);\n"
      "print([] == []);\n"
      "print({} == {});\n"
      "print(1 == 1.0);\n"
      "print(nil == nil);\n"
      "print([1, 2] != [1, 3]);\n"
      "print([1, 2] != [1, 2]);\n"
      "print([1, 2] in [[1, 2], [3, 4]]);\n"
      "print([9] in [[1, 2], [3, 4]]);\n"
      "let a = [1, 2];\n"
      "a[0] = a;\n"
      "let b = [1, 2];\n"
      "b[0] = b;\n"
      "print(a == a);\n"
      "print(a == b);\n"
      "print(a == [1, 2]);\n"
      "let m1 = {\"x\": 1};\n"
      "m1[\"self\"] = m1;\n"
      "let m2 = {\"x\": 1};\n"
      "m2[\"self\"] = m2;\n"
      "print(m1 == m2);\n"
      "var deepA = [0];\n"
      "var deepB = [1];\n"
      "var n = 0;\n"
      "while (n < 50) { deepA = [deepA]; deepB = [deepB]; n = n + 1; }\n"
      "print(deepA == deepB);\n"
      "print(deepA == deepA);\n");
  RunResult result = buildAndRun("/tmp/oskel_structural_equality", proto);
  std::string expected =
      "true\nfalse\nfalse\ntrue\nfalse\n"
      "true\nfalse\nfalse\ntrue\nfalse\n"
      "true\ntrue\ntrue\ntrue\ntrue\n"
      "true\nfalse\ntrue\nfalse\n"
      "true\ntrue\nfalse\ntrue\n"
      "false\ntrue\n";
  check(result.output == expected,
        "a compiled program compares arrays and maps by content");
  check(result.exitCode == 0, "structural equality exits 0 through _start");
}

// Error values (PHASE 7c.5c): `error(message, code?)`, `isError`, `type`,
// `.message`/`.code`, structural equality, printing/repr and interpolation must
// match the hosted engine. This is what makes value-based `try`/`catch` work in
// a native binary, so the same program also propagates and recovers.
static void test_error_values() {
  Bytecode proto = compileSource(
      "let e = error(\"boom\");\n"
      "print(type(e));\n"
      "print(isError(e), isError(42), isError(nil), isError(\"x\"));\n"
      "print(e.message, e.code);\n"
      "let coded = error(\"bad\", 404);\n"
      "print(coded.message, coded.code);\n"
      "print(error(123).message, error(true).message);\n"
      "print(e);\n"
      "print(repr(e));\n"
      "print(repr(coded));\n"
      "print(repr(error(\"neg\", -3)));\n"
      "print(\"interp = ${e}\");\n"
      "print(error(\"x\") == error(\"x\"));\n"
      "print(error(\"x\") == error(\"y\"));\n"
      "print(error(\"x\", 1) == error(\"x\"));\n"
      "print(error(\"x\", 1) == error(\"x\", 1));\n"
      "print(error(\"x\") == \"x\");\n"
      "print([error(\"a\"), error(\"b\", 2)][1].code);\n"
      "fn failWith(m) { return error(m); }\n"
      "print(failWith(\"nope\") catch 0);\n"
      "print((error(\"bad\") catch 7));\n"
      "print(\"done\");\n");
  RunResult result = buildAndRun("/tmp/oskel_error_values", proto);
  std::string expected =
      "error\n"
      "true false false false\n"
      "boom 0\n"
      "bad 404\n"
      "123 true\n"
      "boom\n"
      "error(\"boom\")\n"
      "error(\"bad\", 404)\n"
      "error(\"neg\", -3)\n"
      "interp = boom\n"
      "true\n"
      "false\n"
      "false\n"
      "true\n"
      "false\n"
      "2\n"
      "0\n"
      "7\n"
      "done\n";
  check(result.output == expected,
        "a compiled program computes error values like the hosted engine");
  check(result.exitCode == 0, "error values exit 0 through _start");
}

#endif  // FREESTANDING_EXEC_TESTS

int main() {
  test_arena();
#ifdef FREESTANDING_EXEC_TESTS
  test_platform_roundtrip();
  test_read_retries_on_eintr();
  test_end_to_end();
  test_boxed_runtime_print();
  test_boxed_runtime_globals();
  test_boxed_runtime_error();
  test_boxed_runtime_div_zero();
  test_boxed_runtime_locals();
  test_boxed_runtime_call();
  test_boxed_runtime_call_non_function();
  test_boxed_runtime_closure_capture();
  test_boxed_runtime_string();
  test_boxed_runtime_float_mod();
  test_builtin_print();
  test_collections();
  test_array_methods_inplace();
  test_array_methods_alloc();
  test_map_methods();
  test_negative_string_index();
  test_hidden_helpers();
  test_hidden_helpers_non_null_failure();
  test_str_helper();
  test_range_helper();
  test_range_helper_bad_bounds();
  test_collection_repr();
  test_str_helper_collections();
  test_slice_helper();
  test_slice_helper_errors();
  test_public_builtins();
  test_public_builtin_errors();
  test_numeric_conversions();
  test_numeric_conversion_errors();
  test_int_string_conversion();
  test_int_string_conversion_errors();
  test_float_string_conversion();
  test_math_builtins();
  test_math_builtin_errors();
  test_transcendental_builtins();
  test_transcendental_builtin_errors();
  test_trigonometric_builtins();
  test_trigonometric_builtin_errors();
  test_remaining_public_builtins();
  test_remaining_public_builtin_errors();
  test_sb_append_zero_capacity();
  test_sb_append_capacity_overflow();
  test_variadic();
  test_variadic_arity_error();
  test_structural_equality();
  test_error_values();
#else
  std::printf("skipped: platform and execution tests require Linux x86-64\n");
#endif
  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all freestanding tests passed\n");
  return 0;
}
