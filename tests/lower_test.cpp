// Tests for the x86-64 bytecode lowering (PHASE 7c.1).
//
// The lowering no longer maps bytecode registers to physical CPU registers:
// each register is a 16-byte slot in a register file on the stack, holding a
// boxed `AotValue`. The tests therefore run the generated code (Linux x86-64
// only) and read the `AotValue` the chunk returns in rax:rdx, instead of
// comparing bytes. Byte-exact coverage of the encoders lives in
// `codegen_test.cpp`; here the subject is the lowering's behaviour.
//
// Every opcode the slice does not lower must raise a LoweringError rather than
// emit wrong code, so the deferred cases are tested too.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "bytecode.h"
#include "compiler.h"
#include "lexer.h"
#include "native/codegen.h"
#include "native/lower.h"
#include "native/target.h"
#include "native/value.h"
#include "parser.h"

#if defined(__linux__) && defined(__x86_64__)
#define LOWER_EXEC_TESTS 1
#include <sys/mman.h>
#include <unistd.h>
#endif

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

static Bytecode makeProto(const std::vector<Instruction>& code,
                          const std::vector<Value>& constants, int numRegisters,
                          int numLocals = 0) {
  Bytecode proto;
  proto.name = "<test>";
  proto.code = code;
  proto.constants = constants;
  proto.numRegisters = numRegisters;
  proto.numLocals = numLocals;
  return proto;
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

static LoweredChunk lower(const Bytecode& proto) {
  return lowerToX64(Target::linuxX64(), proto, CodeGen{});
}

static bool loweringThrows(const Bytecode& proto, const char* needle) {
  try {
    lower(proto);
  } catch (const LoweringError& error) {
    return std::string(error.what()).find(needle) != std::string::npos;
  }
  return false;
}

// --- Structural tests ---

static void testImageIsSelfContained() {
  // The image carries the program, the runtime and `_start`, so it is larger
  // than the program alone and both entry points are inside it.
  LoweredChunk chunk =
      lower(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                      {Value(static_cast<int64_t>(1))}, 1));
  check(!chunk.code.empty(), "the lowered image is not empty");
  check(chunk.programEntry < chunk.code.size(), "the chunk entry is inside the image");
  check(chunk.processEntry < chunk.code.size(), "the _start entry is inside the image");
  check(chunk.programEntry != chunk.processEntry, "the chunk and _start entries differ");
}

// --- Error tests: the deferred opcodes must be rejected, not miscompiled ---

static void testCollectionsLower() {
  // 7c.3 lowers the collection opcodes, so NEWTABLE/GETTABLE/SETTABLE/IN/ITER
  // now compile instead of raising. A fresh array lowers and yields an image.
  LoweredChunk chunk =
      lower(makeProto({instr(OpCode::NEWTABLE, 0, 0, 0), instr(OpCode::HALT)}, {}, 1));
  check(!chunk.code.empty(), "NEWTABLE lowers to an image");
}

static void testIterOperandsLower() {
  // `ITER` writes the item (a), the paired value (d, -1 for a key-only loop)
  // and the done flag (e). Both shapes must lower: the value form reads a
  // real slot, the key-only form passes a null value slot.
  Instruction keyValue = instr(OpCode::ITER, 0, 1, 2);
  keyValue.d = 3;
  keyValue.e = 4;
  LoweredChunk valueForm = lower(makeProto({keyValue, instr(OpCode::HALT)},
                                           {Value()}, 5));
  check(!valueForm.code.empty(), "ITER with a value register lowers");

  Instruction keyOnly = instr(OpCode::ITER, 0, 1, 2);
  keyOnly.d = -1;
  keyOnly.e = 4;
  LoweredChunk keyForm = lower(makeProto({keyOnly, instr(OpCode::HALT)},
                                         {Value()}, 5));
  check(!keyForm.code.empty(), "ITER without a value register lowers");
}

static void testBadJumpTargetThrows() {
  auto proto = makeProto({instr(OpCode::JMP, 0, 0, 0, 99), instr(OpCode::HALT)}, {}, 1);
  check(loweringThrows(proto, "outside the code"), "an out-of-range jump target throws");
}

static void testUnassignedGlobalThrows() {
  // A GETGLOBAL whose name is never SETGLOBAL is a lowering error naming the
  // variable, the compile-time counterpart of the hosted engine's run-time
  // "undefined variable".
  auto proto = makeProto({instr(OpCode::GETGLOBAL, 0, 0), instr(OpCode::HALT)},
                         {Value(std::string("missing"))}, 1);
  check(loweringThrows(proto, "global 'missing' is read but never assigned"),
        "reading an unassigned global throws");
}

static void testHostBindingThrows() {
  // `native fn` and `extern "C"` compile to `GETGLOBAL N; SETGLOBAL N` (the
  // host registered N; the read resolves it). With no metadata on the proto the
  // shape is indistinguishable from a self-read, so it is reported as an
  // unassigned global, the same message a typo produces.
  auto proto = makeProto({instr(OpCode::GETGLOBAL, 0, 0), instr(OpCode::SETGLOBAL, 0, 0),
                          instr(OpCode::HALT)},
                         {Value(std::string("hostFn"))}, 1);
  check(loweringThrows(proto, "global 'hostFn' is read but never assigned"),
        "an undeclared host-binding shape throws");

  // A genuine self-assignment (`var x = x;`) has the same shape and the same
  // hosted behaviour (an undefined-variable error), so it throws too.
  auto self = makeProto({instr(OpCode::GETGLOBAL, 0, 0), instr(OpCode::SETGLOBAL, 0, 0),
                         instr(OpCode::HALT)},
                        {Value(std::string("x"))}, 1);
  check(loweringThrows(self, "global 'x' is read but never assigned"),
        "a self-referential declaration throws");

  // The shape is only a host binding when the names match: `GETGLOBAL a;
  // SETGLOBAL b` is an ordinary assignment of a real value, so it lowers.
  auto real = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::SETGLOBAL, 0, 1),
                         instr(OpCode::GETGLOBAL, 1, 1), instr(OpCode::HALT)},
                        {Value(static_cast<int64_t>(5)), Value(std::string("y"))}, 2);
  LoweredChunk chunk = lower(real);
  check(!chunk.code.empty(), "an ordinary global assignment still lowers");
}

static void testUnusedHostBindingLowers() {
  // A `native fn` the program never uses is a declaration and nothing more: the
  // hosted engine installs the host function, and the AOT backend binds the
  // name to nil (a slot with no seed). It must not fail the build, so a module
  // that declares host functions (`std.math` declares `pow`, `sin`, ...) can be
  // imported by a program that calls none of them. The metadata on the proto is
  // what tells the declaration apart from a self-read.
  auto proto = makeProto({instr(OpCode::GETGLOBAL, 0, 0), instr(OpCode::SETGLOBAL, 0, 0),
                          instr(OpCode::HALT)},
                         {Value(std::string("sqrt"))}, 1);
  proto.nativeFns.push_back("sqrt");
  LoweredChunk chunk = lower(proto);
  check(!chunk.code.empty(), "an unused native declaration lowers");

  // The same for an `extern "C"` declaration the program never calls.
  auto ext = makeProto({instr(OpCode::GETGLOBAL, 0, 0), instr(OpCode::SETGLOBAL, 0, 0),
                        instr(OpCode::HALT)},
                       {Value(std::string("getpid"))}, 1);
  ext.externs.push_back(ExternSymbol{"getpid", {}, ffi::TypeSpec{}});
  LoweredChunk extChunk = lower(ext);
  check(!extChunk.code.empty(), "an unused extern declaration lowers");
}

static void testExternSymbolGetsDedicatedMessage() {
  // When the proto carries an `extern "C"` declaration for the name *and* the
  // program reads it beyond the declaration's own self-read, the read is a C
  // symbol the host would provide, not a typo: the lowerer names the real
  // problem (the AOT backend has no host) instead of the generic
  // unassigned-global message. `proto.externs` is the compiler's link metadata,
  // and the extra `GETGLOBAL` is the use.
  auto proto = makeProto({instr(OpCode::GETGLOBAL, 0, 0), instr(OpCode::SETGLOBAL, 0, 0),
                          instr(OpCode::GETGLOBAL, 1, 0), instr(OpCode::HALT)},
                         {Value(std::string("getpid"))}, 2);
  proto.externs.push_back(ExternSymbol{"getpid", {}, ffi::TypeSpec{}});
  check(loweringThrows(proto, "extern \"C\" symbol 'getpid' cannot be resolved"),
        "an extern symbol read names the AOT limitation, not a typo");

  // A `native fn` that is used (read beyond its declaration) is named too, with
  // a message that says `native fn` rather than `extern "C"`. `clock` is a host
  // binding with no curated stub (the libm ports cover `sqrt`/`pow`/`sin`/...).
  auto native = makeProto({instr(OpCode::GETGLOBAL, 0, 0), instr(OpCode::SETGLOBAL, 0, 0),
                           instr(OpCode::GETGLOBAL, 1, 0), instr(OpCode::HALT)},
                          {Value(std::string("clock"))}, 2);
  native.nativeFns.push_back("clock");
  check(loweringThrows(native, "native fn 'clock' cannot be resolved"),
        "a used native fn names the AOT limitation");

  // The dedicated message is only for a declared symbol: an undeclared read of
  // the same shape still gets the generic message.
  auto typo = makeProto({instr(OpCode::GETGLOBAL, 0, 0), instr(OpCode::SETGLOBAL, 0, 0),
                         instr(OpCode::GETGLOBAL, 1, 0), instr(OpCode::HALT)},
                        {Value(std::string("getpid"))}, 2);
  check(loweringThrows(typo, "global 'getpid' is read but never assigned"),
        "an undeclared read keeps the generic message");

  // A binding with a curated stub is resolved, not reported: `sqrt` is seeded
  // by the AOT runtime, so even a used declaration lowers.
  auto stubbed = makeProto({instr(OpCode::GETGLOBAL, 0, 0), instr(OpCode::SETGLOBAL, 0, 0),
                            instr(OpCode::GETGLOBAL, 1, 0), instr(OpCode::HALT)},
                           {Value(std::string("sqrt"))}, 2);
  stubbed.nativeFns.push_back("sqrt");
  LoweredChunk stubbedChunk = lower(stubbed);
  check(!stubbedChunk.code.empty(), "a used stub-backed binding lowers");
}

static void testInvalidUnopSelectorThrows() {
  auto proto = makeProto(
      {instr(OpCode::UNOP, 0, 0, 99), instr(OpCode::HALT)}, {}, 1);
  check(loweringThrows(proto, "UNOP selector"), "an invalid UNOP selector throws");
}

static void testHaltInChildThrows() {
  // `HALT` publishes R0 and runs the root epilogue. A child proto has its own
  // frame and ends in `RETURN`, so a `HALT` there would unwind the wrong frame;
  // the lowerer rejects it instead of emitting undefined behavior.
  auto child = std::make_shared<Bytecode>();
  child->name = "<child>";
  child->code = {instr(OpCode::HALT)};
  child->numRegisters = 1;
  Bytecode root = makeProto({instr(OpCode::CLOSURE, 0, 0), instr(OpCode::HALT)}, {}, 1);
  root.children.push_back(child);
  check(loweringThrows(root, "HALT inside a function proto"),
        "HALT inside a child proto throws");
}

static void testRootReturnLowers() {
  // A `RETURN` in the root chunk has no caller. The VM treats it exactly like
  // `HALT` (a REPL's last expression and a top-level `return` compile to it),
  // so the lowerer accepts it and runs the root epilogue rather than the
  // function one. No throw, and the image is non-empty.
  auto proto = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::RETURN, 0)},
                         {Value(static_cast<int64_t>(42))}, 1);
  LoweredChunk chunk = lower(proto);
  check(!chunk.code.empty(), "RETURN in the root chunk lowers to an image");
}

// --- Execution tests (Linux x86-64 only) ---

#ifdef LOWER_EXEC_TESTS

// Maps the image executable and returns a pointer to `offset` inside it.
static void* emitExecutable(const std::vector<std::uint8_t>& code,
                            std::uint64_t offset) {
  long pageSize = sysconf(_SC_PAGESIZE);
  std::size_t span = static_cast<std::size_t>(pageSize);
  while (span < code.size()) span += static_cast<std::size_t>(pageSize);
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    std::perror("mmap");
    std::exit(1);
  }
  std::memcpy(mem, code.data(), code.size());
  if (mprotect(mem, span, PROT_READ | PROT_EXEC) != 0) {
    std::perror("mprotect");
    std::exit(1);
  }
  return static_cast<std::uint8_t*>(mem) + offset;
}

// Runs the chunk and returns the `AotValue` it leaves in rax:rdx (R0). A chunk
// that hits a runtime error exits the process, so the error path is exercised
// end-to-end in `freestanding_test.cpp`, not here.
static AotValue run(const Bytecode& proto) {
  LoweredChunk chunk = lower(proto);
  auto fn = reinterpret_cast<AotValue (*)()>(
      emitExecutable(chunk.code, chunk.programEntry));
  return fn();
}

static void checkInt(const Bytecode& proto, std::int64_t expected, const char* what) {
  AotValue value = run(proto);
  bool ok = value.tag == static_cast<std::uint64_t>(AotTag::Int) &&
            static_cast<std::int64_t>(value.payload) == expected;
  check(ok, what);
  if (!ok) {
    std::printf("      got tag=%llu payload=%lld, expected Int %lld\n",
                static_cast<unsigned long long>(value.tag),
                static_cast<long long>(static_cast<std::int64_t>(value.payload)),
                static_cast<long long>(expected));
  }
}

static void checkBool(const Bytecode& proto, bool expected, const char* what) {
  AotValue value = run(proto);
  bool ok = value.tag == static_cast<std::uint64_t>(AotTag::Bool) &&
            (value.payload != 0) == expected;
  check(ok, what);
}

static void checkBoolValue(const AotValue& value, bool expected, const char* what) {
  bool ok = value.tag == static_cast<std::uint64_t>(AotTag::Bool) &&
            (value.payload != 0) == expected;
  check(ok, what);
}

// Compiles a source string with the real compiler, lowers it and runs the
// chunk. The compiled tests below drive the whole front end this way, so they
// lock the compiler/lowerer contract rather than a hand-built proto.
static AotValue compileAndRun(const std::string& source) {
  auto tokens = lex(source);
  Parser parser(std::move(tokens), "<lower-test>");
  Bytecode proto = compileProgram(parser.parseProgram());
  return run(proto);
}

static void checkIntValue(AotValue value, std::int64_t expected, const char* what) {
  bool ok = value.tag == static_cast<std::uint64_t>(AotTag::Int) &&
            static_cast<std::int64_t>(value.payload) == expected;
  check(ok, what);
  if (!ok) {
    std::printf("      got tag=%llu payload=%lld, expected Int %lld\n",
                static_cast<unsigned long long>(value.tag),
                static_cast<long long>(static_cast<std::int64_t>(value.payload)),
                static_cast<long long>(expected));
  }
}

static void testExecRootReturn() {
  // A root `RETURN` publishes R[a] and runs the root epilogue, exactly like
  // `HALT`, so the chunk result is still readable.
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::RETURN, 0)},
                     {Value(static_cast<int64_t>(42))}, 1),
           42, "a root RETURN behaves like HALT and yields R0");
}

static void testExecLoads() {
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(42))}, 1),
           42, "exec: LOADK int 42");
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(-7))}, 1),
           -7, "exec: LOADK int -7");
  checkBool(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                      {Value(true)}, 1),
            true, "exec: LOADK true");
  checkBool(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                      {Value(false)}, 1),
            false, "exec: LOADK false");

  AotValue nil = run(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                               {Value()}, 1));
  check(nil.tag == static_cast<std::uint64_t>(AotTag::Nil), "exec: LOADK nil");
}

static void testExecMove() {
  // R1 = R0; R0 = R1 -> the value moves through the register file.
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::MOVE, 1, 0),
                      instr(OpCode::MOVE, 0, 1), instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(9))}, 2),
           9, "exec: MOVE preserves tag and payload");
}

static void testExecArithmetic() {
  auto add = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                        instr(OpCode::ADD, 0, 0, 1), instr(OpCode::HALT)},
                       {Value(static_cast<int64_t>(2)), Value(static_cast<int64_t>(3))}, 2);
  checkInt(add, 5, "exec: 2 + 3 == 5");

  auto sub = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                        instr(OpCode::SUB, 0, 0, 1), instr(OpCode::HALT)},
                       {Value(static_cast<int64_t>(10)), Value(static_cast<int64_t>(3))}, 2);
  checkInt(sub, 7, "exec: 10 - 3 == 7");

  auto mul = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                        instr(OpCode::MUL, 0, 0, 1), instr(OpCode::HALT)},
                       {Value(static_cast<int64_t>(6)), Value(static_cast<int64_t>(7))}, 2);
  checkInt(mul, 42, "exec: 6 * 7 == 42");

  // Distinct destination: R2 = R0 + R1, then R0 = R2.
  auto distinct = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                             instr(OpCode::ADD, 2, 0, 1), instr(OpCode::MOVE, 0, 2),
                             instr(OpCode::HALT)},
                            {Value(static_cast<int64_t>(20)), Value(static_cast<int64_t>(22))}, 3);
  checkInt(distinct, 42, "exec: R2 = R0 + R1 (distinct destination)");
}

static void testExecBitwise() {
  auto mk = [](OpCode op, std::int64_t x, std::int64_t y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 0, 0, 1), instr(OpCode::HALT)},
                     {Value(x), Value(y)}, 2);
  };
  checkInt(mk(OpCode::BAND, 12, 10), 8, "exec: 12 & 10 == 8");
  checkInt(mk(OpCode::BOR, 12, 10), 14, "exec: 12 | 10 == 14");
  checkInt(mk(OpCode::BXOR, 12, 10), 6, "exec: 12 ^ 10 == 6");
}

static void testExecComparisonsAsValues() {
  auto mk = [](OpCode op, std::int64_t x, std::int64_t y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 2, 0, 1), instr(OpCode::MOVE, 0, 2), instr(OpCode::HALT)},
                     {Value(x), Value(y)}, 3);
  };
  checkBool(mk(OpCode::EQ, 5, 5), true, "exec: (5 == 5) is true");
  checkBool(mk(OpCode::EQ, 5, 6), false, "exec: (5 == 6) is false");
  checkBool(mk(OpCode::NEQ, 5, 6), true, "exec: (5 != 6) is true");
  checkBool(mk(OpCode::LT, 5, 9), true, "exec: (5 < 9) is true");
  checkBool(mk(OpCode::LT, 9, 5), false, "exec: (9 < 5) is false");
  checkBool(mk(OpCode::LTE, 5, 5), true, "exec: (5 <= 5) is true");
  checkBool(mk(OpCode::GT, 9, 5), true, "exec: (9 > 5) is true");
  checkBool(mk(OpCode::GTE, 5, 9), false, "exec: (5 >= 9) is false");

  // Equality is type-aware: an int never equals a bool with the same payload.
  auto mixed = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                          instr(OpCode::EQ, 2, 0, 1), instr(OpCode::MOVE, 0, 2),
                          instr(OpCode::HALT)},
                         {Value(static_cast<int64_t>(1)), Value(true)}, 3);
  checkBool(mixed, false, "exec: 1 == true is false (tags differ)");
}

static void testExecUnop() {
  auto neg = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::Negate)), instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(7))}, 1);
  checkInt(neg, -7, "exec: -7");

  auto bitNot = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::BitNot)), instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(0))}, 1);
  checkInt(bitNot, -1, "exec: ~0 == -1");

  // !0 -> false: the integer 0 is truthy, so its negation is false.
  auto notZero = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::Not)), instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(0))}, 1);
  checkBool(notZero, false, "exec: !0 is false (0 is truthy)");

  auto notNil = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::Not)), instr(OpCode::HALT)},
      {Value()}, 1);
  checkBool(notNil, true, "exec: !nil is true");
}

static void testExecJumps() {
  // R0=1; JMP +3; R0=99 (skipped); HALT -> 1
  auto jmp = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::JMP, 0, 0, 0, 3),
                        instr(OpCode::LOADK, 0, 1), instr(OpCode::HALT)},
                       {Value(static_cast<int64_t>(1)), Value(static_cast<int64_t>(99))}, 1);
  checkInt(jmp, 1, "exec: JMP skips an instruction");

  // The integer 0 is truthy, so JMP_IF_FALSE does not jump and R0 becomes 99.
  auto truthyZero = makeProto({instr(OpCode::LOADK, 0, 0),
                               instr(OpCode::JMP_IF_FALSE, 0, 0, 0, 3),
                               instr(OpCode::LOADK, 0, 1), instr(OpCode::HALT)},
                              {Value(static_cast<int64_t>(0)), Value(static_cast<int64_t>(99))}, 1);
  checkInt(truthyZero, 99, "exec: JMP_IF_FALSE does not jump on integer 0 (truthy)");

  // nil is falsy, so JMP_IF_FALSE jumps over the assignment and R0 stays nil.
  auto nilFalse = makeProto({instr(OpCode::LOADK, 0, 0),
                             instr(OpCode::JMP_IF_FALSE, 0, 0, 0, 3),
                             instr(OpCode::LOADK, 0, 1), instr(OpCode::HALT)},
                            {Value(), Value(static_cast<int64_t>(99))}, 1);
  AotValue nilResult = run(nilFalse);
  check(nilResult.tag == static_cast<std::uint64_t>(AotTag::Nil),
        "exec: JMP_IF_FALSE jumps on nil (falsy)");

  // true is truthy, so JMP_IF_TRUE jumps over the assignment.
  auto trueJump = makeProto({instr(OpCode::LOADK, 0, 0),
                             instr(OpCode::JMP_IF_TRUE, 0, 0, 0, 3),
                             instr(OpCode::LOADK, 0, 1), instr(OpCode::HALT)},
                            {Value(true), Value(static_cast<int64_t>(99))}, 1);
  checkBool(trueJump, true, "exec: JMP_IF_TRUE jumps on true");
}

static void testExecBackwardJumpLoop() {
  // A countdown: R0 = 3; R1 = 1; R2 = 0;
  //   top(3): R0 = R0 - R1; R3 = (R0 != R2); if R3 goto top; HALT.
  // 3 -> 2 -> 1 -> 0, then NEQ yields false, JMP_IF_TRUE does not jump and the
  // loop exits with 0 in R0. Exercises a backward jump and a comparison value.
  auto proto = makeProto(
      {instr(OpCode::LOADK, 0, 0),             // 0: R0 = 3
       instr(OpCode::LOADK, 1, 1),             // 1: R1 = 1
       instr(OpCode::LOADK, 2, 2),             // 2: R2 = 0
       instr(OpCode::SUB, 0, 0, 1),            // 3: top: R0 = R0 - R1
       instr(OpCode::NEQ, 3, 0, 2),            // 4: R3 = (R0 != 0)
       instr(OpCode::JMP_IF_TRUE, 3, 0, 0, 3), // 5: if R3 goto 3
       instr(OpCode::HALT)},                   // 6
      {Value(static_cast<int64_t>(3)), Value(static_cast<int64_t>(1)),
       Value(static_cast<int64_t>(0))},
      4);
  checkInt(proto, 0, "exec: countdown loop terminates at 0");
}

static void testExecManyRegisters() {
  // The register file is on the stack, so the old 12-register ceiling is gone:
  // 40 registers must work. R39 = 5 + 6.
  auto proto = makeProto({instr(OpCode::LOADK, 38, 0), instr(OpCode::LOADK, 39, 1),
                          instr(OpCode::ADD, 39, 38, 39), instr(OpCode::MOVE, 0, 39),
                          instr(OpCode::HALT)},
                         {Value(static_cast<int64_t>(5)), Value(static_cast<int64_t>(6))}, 40);
  checkInt(proto, 11, "exec: 40 registers are usable (no physical-register ceiling)");
}

static void checkDouble(const Bytecode& proto, double expected, const char* what) {
  AotValue value = run(proto);
  bool ok = value.tag == static_cast<std::uint64_t>(AotTag::Double) &&
            value.payload == aotDouble(expected).payload;
  check(ok, what);
  if (!ok) {
    std::printf("      got tag=%llu payload=%016llx, expected Double %016llx\n",
                static_cast<unsigned long long>(value.tag),
                static_cast<unsigned long long>(value.payload),
                static_cast<unsigned long long>(aotDouble(expected).payload));
  }
}

// A `float` result stores the binary32 value widened to binary64, so the
// expected payload is `aotFloat`, not `aotDouble`.
static void checkFloat(const Bytecode& proto, double widened, const char* what) {
  AotValue value = run(proto);
  bool ok = value.tag == static_cast<std::uint64_t>(AotTag::Float) &&
            value.payload == aotFloat(widened).payload;
  check(ok, what);
  if (!ok) {
    std::printf("      got tag=%llu payload=%016llx, expected Float %016llx\n",
                static_cast<unsigned long long>(value.tag),
                static_cast<unsigned long long>(value.payload),
                static_cast<unsigned long long>(aotFloat(widened).payload));
  }
}

static void testExecFloatLoads() {
  checkDouble(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)}, {Value(1.5)}, 1),
              1.5, "exec: LOADK double 1.5");
  checkFloat(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                       {Value::fromFloat(0.1f)}, 1),
             static_cast<double>(0.1f), "exec: LOADK float 0.1f");
}

static void testExecFloatArithmetic() {
  auto mk = [](OpCode op, Value x, Value y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 0, 0, 1), instr(OpCode::HALT)},
                     {x, y}, 2);
  };
  checkDouble(mk(OpCode::ADD, Value(1.5), Value(2.5)), 4.0, "exec: 1.5 + 2.5 == 4.0 (double)");
  checkDouble(mk(OpCode::SUB, Value(5.5), Value(2.0)), 3.5, "exec: 5.5 - 2.0 == 3.5 (double)");
  checkDouble(mk(OpCode::MUL, Value(1.5), Value(2.0)), 3.0, "exec: 1.5 * 2.0 == 3.0 (double)");
  checkDouble(mk(OpCode::DIV, Value(7.0), Value(2.0)), 3.5, "exec: 7.0 / 2.0 == 3.5 (double)");

  // float + float stays binary32: 0.1f + 0.2f rounds to 0.3f.
  checkFloat(mk(OpCode::ADD, Value::fromFloat(0.1f), Value::fromFloat(0.2f)),
             static_cast<double>(0.3f), "exec: 0.1f + 0.2f == 0.3f (binary32 rounding)");

  // int widens to float: 1 + 1.5f is a float.
  checkFloat(mk(OpCode::ADD, Value(static_cast<int64_t>(1)), Value::fromFloat(1.5f)), 2.5,
             "exec: 1 + 1.5f == 2.5 (float)");

  // A float with a double promotes to double.
  checkDouble(mk(OpCode::ADD, Value::fromFloat(1.5f), Value(1.5)), 3.0,
              "exec: 1.5f + 1.5 == 3.0 (double)");
}

static void testExecFloatMod() {
  auto mk = [](OpCode op, Value x, Value y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 0, 0, 1), instr(OpCode::HALT)},
                     {x, y}, 2);
  };
  // `7 % 2.5` is 2.0: the truncated remainder, matching `std::fmod`.
  checkDouble(mk(OpCode::MOD, Value(7.0), Value(2.5)), 2.0, "exec: 7.0 % 2.5 == 2.0 (double)");
  // The sign follows the dividend.
  checkDouble(mk(OpCode::MOD, Value(-7.0), Value(3.0)), -1.0, "exec: -7.0 % 3.0 == -1.0 (double)");
  checkDouble(mk(OpCode::MOD, Value(7.0), Value(-3.0)), 1.0, "exec: 7.0 % -3.0 == 1.0 (double)");
  // A divisor larger than the dividend leaves the dividend unchanged.
  checkDouble(mk(OpCode::MOD, Value(1.0), Value(3.0)), 1.0, "exec: 1.0 % 3.0 == 1.0 (double)");
  // float % float stays binary32.
  checkFloat(mk(OpCode::MOD, Value::fromFloat(5.5f), Value::fromFloat(2.0f)),
             static_cast<double>(1.5f), "exec: 5.5f % 2.0f == 1.5f (float)");
  // An int operand widens: 7 % 2.5 is a double.
  checkDouble(mk(OpCode::MOD, Value(static_cast<int64_t>(7)), Value(2.5)), 2.0,
              "exec: 7 % 2.5 == 2.0 (int widens to double)");
}

static void testExecIntDivModPow() {
  auto mk = [](OpCode op, std::int64_t x, std::int64_t y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 0, 0, 1), instr(OpCode::HALT)},
                     {Value(x), Value(y)}, 2);
  };
  checkInt(mk(OpCode::DIV, 7, 2), 3, "exec: 7 / 2 == 3 (int)");
  checkInt(mk(OpCode::MOD, 7, 3), 1, "exec: 7 % 3 == 1 (int)");
  checkInt(mk(OpCode::POW, 2, 10), 1024, "exec: 2 ** 10 == 1024 (int)");
  checkInt(mk(OpCode::POW, 3, 0), 1, "exec: 3 ** 0 == 1 (int)");
  // INT64_MIN / -1 is the `idiv` overflow; the hosted engine returns a itself.
  checkInt(mk(OpCode::DIV, INT64_MIN, -1), INT64_MIN, "exec: INT64_MIN / -1 wraps (int)");
  checkInt(mk(OpCode::MOD, INT64_MIN, -1), 0, "exec: INT64_MIN % -1 == 0 (int)");
}

static void testExecFloatPow() {
  auto mk = [](Value x, Value y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(OpCode::POW, 0, 0, 1), instr(OpCode::HALT)},
                     {x, y}, 2);
  };
  // An int base with a negative exponent promotes to double, like the hosted
  // `ops::binary` (which falls to `std::pow`).
  checkDouble(mk(Value(static_cast<int64_t>(2)), Value(static_cast<int64_t>(-1))), 0.5,
              "exec: 2 ** -1 == 0.5 (double)");
  checkDouble(mk(Value(static_cast<int64_t>(2)), Value(2.0)), 4.0,
              "exec: 2 ** 2.0 == 4.0 (double)");
  checkDouble(mk(Value(4.0), Value(0.5)), 2.0, "exec: 4.0 ** 0.5 == 2.0 (double)");
  // float/float stays binary32.
  checkFloat(mk(Value::fromFloat(2.0f), Value::fromFloat(3.0f)),
             static_cast<double>(8.0f), "exec: 2.0f ** 3.0f == 8.0f (float)");
  // A float with a double promotes to double.
  checkDouble(mk(Value::fromFloat(2.0f), Value(3.0)), 8.0,
              "exec: 2.0f ** 3.0 == 8.0 (double)");
}

static void testExecShifts() {
  auto mk = [](OpCode op, std::int64_t x, std::int64_t y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 0, 0, 1), instr(OpCode::HALT)},
                     {Value(x), Value(y)}, 2);
  };
  checkInt(mk(OpCode::SHL, 1, 4), 16, "exec: 1 << 4 == 16");
  checkInt(mk(OpCode::SHR, -8, 1), -4, "exec: -8 >> 1 == -4 (arithmetic)");
  checkInt(mk(OpCode::SHL, 1, 63), INT64_MIN, "exec: 1 << 63 == INT64_MIN");
}

static void testExecFloatComparisons() {
  auto mk = [](OpCode op, Value x, Value y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 2, 0, 1), instr(OpCode::MOVE, 0, 2), instr(OpCode::HALT)},
                     {x, y}, 3);
  };
  checkBool(mk(OpCode::EQ, Value(1.5), Value(1.5)), true, "exec: 1.5 == 1.5 is true");
  checkBool(mk(OpCode::EQ, Value(1.5), Value(1.6)), false, "exec: 1.5 == 1.6 is false");
  checkBool(mk(OpCode::LT, Value(1.5), Value(1.6)), true, "exec: 1.5 < 1.6 is true");
  checkBool(mk(OpCode::GT, Value(1.5), Value(1.6)), false, "exec: 1.5 > 1.6 is false");
  // Mixed numeric equality is numeric: an int and a double compare by value.
  checkBool(mk(OpCode::EQ, Value(static_cast<int64_t>(1)), Value(1.0)), true,
            "exec: 1 == 1.0 is true (numeric equality)");
  checkBool(mk(OpCode::LT, Value(static_cast<int64_t>(1)), Value::fromFloat(1.5f)), true,
            "exec: 1 < 1.5f is true (mixed ordering)");
}

static void testExecFloatNegate() {
  auto neg = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::Negate)), instr(OpCode::HALT)},
      {Value(1.5)}, 1);
  checkDouble(neg, -1.5, "exec: -(1.5) == -1.5 (double)");
}

// Reads back the bytes of the `AotString` a Str result points at. The chunk's
// image is mapped in this process, so the pointer is directly dereferenceable.
static std::string strBytes(const AotValue& value) {
  if (value.tag != static_cast<std::uint64_t>(AotTag::Str)) return "<not a string>";
  const auto* header = reinterpret_cast<const std::uint64_t*>(
      static_cast<std::uintptr_t>(value.payload));
  const std::uint64_t length = *header;
  const char* bytes = reinterpret_cast<const char*>(header + 1);
  return std::string(bytes, static_cast<std::size_t>(length));
}

static void checkStr(const Bytecode& proto, const std::string& expected, const char* what) {
  AotValue value = run(proto);
  std::string actual = strBytes(value);
  bool ok = value.tag == static_cast<std::uint64_t>(AotTag::Str) && actual == expected;
  check(ok, what);
  if (!ok) {
    std::printf("      got tag=%llu bytes=\"%s\", expected \"%s\"\n",
                static_cast<unsigned long long>(value.tag), actual.c_str(),
                expected.c_str());
  }
}

// Reads back the int payloads of the `AotArray` an Array result points at. The
// chunk's image is mapped in this process, so the header and its items block
// are directly dereferenceable (see `AotArray` in `native/value.h`).
static std::vector<std::int64_t> arrIntItems(const AotValue& value) {
  std::vector<std::int64_t> items;
  if (value.tag != static_cast<std::uint64_t>(AotTag::Array)) return items;
  const auto* header = reinterpret_cast<const std::uint64_t*>(
      static_cast<std::uintptr_t>(value.payload));
  const std::uint64_t length = header[0];
  const auto* base = reinterpret_cast<const AotValue*>(
      static_cast<std::uintptr_t>(header[2]));
  for (std::uint64_t i = 0; i < length; ++i) {
    items.push_back(static_cast<std::int64_t>(base[i].payload));
  }
  return items;
}

static void checkArray(const AotValue& value, const std::vector<std::int64_t>& expected,
                       const char* what) {
  bool isArray = value.tag == static_cast<std::uint64_t>(AotTag::Array);
  std::vector<std::int64_t> actual = arrIntItems(value);
  bool ok = isArray && actual == expected;
  check(ok, what);
  if (!ok) {
    std::printf("      got tag=%llu length=%zu, expected an array of %zu\n",
                static_cast<unsigned long long>(value.tag), actual.size(), expected.size());
  }
}

static void testExecStringLoad() {
  checkStr(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                     {Value(std::string("hello"))}, 1),
           "hello", "exec: LOADK string \"hello\"");
  // An embedded NUL survives: the length is explicit, not NUL-terminated.
  checkStr(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                     {Value(std::string("a\0b", 3))}, 1),
           std::string("a\0b", 3), "exec: LOADK string with an embedded NUL");
  checkStr(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                     {Value(std::string(""))}, 1),
           "", "exec: LOADK empty string");
}

static void testExecStringConcat() {
  auto mk = [](const std::string& x, const std::string& y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(OpCode::ADD, 0, 0, 1), instr(OpCode::HALT)},
                     {Value(x), Value(y)}, 2);
  };
  checkStr(mk("foo", "bar"), "foobar", "exec: \"foo\" + \"bar\" == \"foobar\"");
  checkStr(mk("", "x"), "x", "exec: \"\" + \"x\" == \"x\"");
  checkStr(mk("x", ""), "x", "exec: \"x\" + \"\" == \"x\"");
}

static void testExecArrayConcat() {
  checkArray(compileAndRun("var r = [1, 2] + [3, 4];"), {1, 2, 3, 4},
             "compiled: [1, 2] + [3, 4] == [1, 2, 3, 4]");
  checkArray(compileAndRun("var r = [] + [];"), {},
             "compiled: [] + [] == []");
  checkArray(compileAndRun("var r = [] + [9];"), {9},
             "compiled: [] + [9] == [9]");
  checkArray(compileAndRun("var r = [9] + [];"), {9},
             "compiled: [9] + [] == [9]");
  checkArray(compileAndRun("var a = [1, 2]; var b = [3]; var r = a + b;"), {1, 2, 3},
             "compiled: a + b concatenates two arrays");
  // The operands are read only: `+` builds a fresh array, so both are intact.
  checkArray(compileAndRun("var a = [1, 2]; var b = [3]; var c = a + b; var r = b;"),
             {3}, "compiled: array concatenation leaves the right operand unchanged");
  checkArray(compileAndRun("var a = [1, 2]; var b = [3]; var c = a + b; var r = a;"),
             {1, 2}, "compiled: array concatenation leaves the left operand unchanged");
  // Growing an accumulator one element at a time, as a loop does.
  checkArray(compileAndRun("var acc = []; var i = 0;"
                           " while (i < 3) { acc = acc + [i]; i = i + 1; }"
                           " var r = acc;"),
             {0, 1, 2}, "compiled: an accumulator grows by concatenation in a loop");
  // Nested arrays concatenate element-wise; each item is itself an array, so
  // the result has three items (checked by length, not by int payloads).
  AotValue nested = compileAndRun("var r = [[1], [2]] + [[3]];");
  bool nestedOk = false;
  if (nested.tag == static_cast<std::uint64_t>(AotTag::Array)) {
    const auto* header = reinterpret_cast<const std::uint64_t*>(
        static_cast<std::uintptr_t>(nested.payload));
    nestedOk = header[0] == 3;
  }
  check(nestedOk, "compiled: nested arrays concatenate to three items");
}

static void testExecArrayMethods() {
  // `push` appends in place and returns the receiver array.
  checkArray(compileAndRun("var a = [1, 2]; var r = a.push(3);"), {1, 2, 3},
             "compiled: a.push(3) returns the array with the new item");
  checkArray(compileAndRun("var a = [1, 2]; a.push(3); var r = a;"), {1, 2, 3},
             "compiled: push mutates the receiver in place");
  checkArray(compileAndRun("var a = []; var r = a.push(9);"), {9},
             "compiled: push on an empty array");
  // Several arguments append in order.
  checkArray(compileAndRun("var a = [1]; var r = a.push(2, 3, 4);"), {1, 2, 3, 4},
             "compiled: push appends every argument in order");

  // `pop` removes and returns the last item.
  checkIntValue(compileAndRun("var a = [1, 2, 3]; var r = a.pop();"), 3,
                "compiled: pop returns the last item");
  checkArray(compileAndRun("var a = [1, 2, 3]; a.pop(); var r = a;"), {1, 2},
             "compiled: pop removes the last item in place");
  // An empty array's `pop` is nil, not an error.
  AotValue popped = compileAndRun("var a = []; var r = a.pop();");
  check(popped.tag == static_cast<std::uint64_t>(AotTag::Nil),
        "compiled: pop on an empty array is nil");

  // The methods are first-class: a detached binder keeps mutating the same
  // array, so binding `a.push` and calling it later is visible on `a`.
  checkArray(compileAndRun("var a = [1]; var p = a.push; p(2); var r = a;"), {1, 2},
             "compiled: a detached push mutates the receiver it captured");
  checkIntValue(compileAndRun("var a = [1, 2]; var p = a.pop; var r = p();"), 2,
                "compiled: a detached pop returns the receiver's last item");

  // Growing and draining an accumulator in a loop exercises the amortised
  // growth of `push` and the in-place shrink of `pop`.
  checkArray(compileAndRun("var a = []; var i = 0;"
                           " while (i < 5) { a.push(i); i = i + 1; }"
                           " var r = a;"),
             {0, 1, 2, 3, 4}, "compiled: push grows an accumulator in a loop");
  checkIntValue(compileAndRun("var a = [1, 2, 3, 4]; var s = 0;"
                              " while (a.length > 0) { s = s + a.pop(); }"
                              " var r = s;"),
                10, "compiled: draining an array with pop sums its items");
}

// The in-place and query array methods (PHASE 7c.3e): `shift`, `unshift`,
// `insert`, `removeAt`, `contains`, `indexOf` and `clear`. Each binds like
// `push`/`pop` -- the receiver is captured -- so a call mutates or scans the
// same array the VM sees.
static void testExecArrayMethodsInPlace() {
  // `shift` removes and returns the first item; an empty array yields nil.
  checkIntValue(compileAndRun("var a = [1, 2, 3]; var r = a.shift();"), 1,
                "compiled: shift returns the first item");
  checkArray(compileAndRun("var a = [1, 2, 3]; a.shift(); var r = a;"), {2, 3},
             "compiled: shift removes the first item in place");
  AotValue shifted = compileAndRun("var a = []; var r = a.shift();");
  check(shifted.tag == static_cast<std::uint64_t>(AotTag::Nil),
        "compiled: shift on an empty array is nil");

  // `unshift` inserts at the front and returns the receiver.
  checkArray(compileAndRun("var a = [3, 4]; var r = a.unshift(2);"), {2, 3, 4},
             "compiled: unshift returns the array with the new front item");
  checkArray(compileAndRun("var a = [3, 4]; a.unshift(2); var r = a;"), {2, 3, 4},
             "compiled: unshift mutates the receiver in place");
  checkArray(compileAndRun("var a = []; var r = a.unshift(1);"), {1},
             "compiled: unshift on an empty array");

  // `insert` places an item at an index and returns the receiver. A negative
  // index counts from the end.
  checkArray(compileAndRun("var a = [1, 3]; var r = a.insert(1, 2);"), {1, 2, 3},
             "compiled: insert places the item at the index");
  checkArray(compileAndRun("var a = [1, 2, 3]; var r = a.insert(-1, 9);"), {1, 2, 9, 3},
             "compiled: insert with a negative index counts from the end");
  checkArray(compileAndRun("var a = [1, 2]; var r = a.insert(2, 3);"), {1, 2, 3},
             "compiled: insert at the end appends");

  // `removeAt` removes and returns the item at an index; a negative index
  // counts from the end.
  checkIntValue(compileAndRun("var a = [1, 2, 3]; var r = a.removeAt(0);"), 1,
                "compiled: removeAt returns the item at the index");
  checkArray(compileAndRun("var a = [1, 2, 3]; a.removeAt(0); var r = a;"), {2, 3},
             "compiled: removeAt removes the item in place");
  checkIntValue(compileAndRun("var a = [1, 2, 3]; var r = a.removeAt(-1);"), 3,
                "compiled: removeAt with a negative index counts from the end");

  // `contains`/`indexOf` scan with structural equality, so an array needle
  // compares by content.
  checkBoolValue(compileAndRun("var a = [1, 2, 3]; var r = a.contains(2);"), true,
                 "compiled: contains finds a present item");
  checkBoolValue(compileAndRun("var a = [1, 2, 3]; var r = a.contains(9);"), false,
                 "compiled: contains misses an absent item");
  checkIntValue(compileAndRun("var a = [1, 2, 3, 2]; var r = a.indexOf(2);"), 1,
                "compiled: indexOf returns the first matching index");
  checkIntValue(compileAndRun("var a = [1, 2, 3]; var r = a.indexOf(9);"), -1,
                "compiled: indexOf is -1 when there is no match");
  checkBoolValue(compileAndRun("var a = [[1], [2, 3]]; var r = a.contains([2, 3]);"), true,
                 "compiled: contains compares nested arrays by content");
  checkIntValue(compileAndRun("var a = [[1], [2, 3]]; var r = a.indexOf([2, 3]);"), 1,
                "compiled: indexOf compares nested arrays by content");

  // `clear` empties the array in place and returns the receiver.
  checkArray(compileAndRun("var a = [1, 2, 3]; var r = a.clear();"), {},
             "compiled: clear returns the emptied array");
  checkIntValue(compileAndRun("var a = [1, 2, 3]; a.clear(); var r = a.length;"), 0,
                "compiled: clear empties the receiver in place");

  // The methods are first-class: a detached binder keeps mutating the same
  // array.
  checkIntValue(compileAndRun("var a = [1, 2]; var s = a.shift; var r = s();"), 1,
                "compiled: a detached shift returns the receiver's first item");
  checkArray(compileAndRun("var a = [2, 3]; var u = a.unshift; u(1); var r = a;"), {1, 2, 3},
             "compiled: a detached unshift mutates the receiver it captured");
  checkBoolValue(compileAndRun("var a = [1, 2]; var c = a.contains; var r = c(2);"), true,
                 "compiled: a detached contains scans the receiver it captured");
}

static void testExecStringCompare() {
  auto mk = [](OpCode op, const std::string& x, const std::string& y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 2, 0, 1), instr(OpCode::MOVE, 0, 2), instr(OpCode::HALT)},
                     {Value(x), Value(y)}, 3);
  };
  checkBool(mk(OpCode::EQ, "abc", "abc"), true, "exec: \"abc\" == \"abc\" is true");
  checkBool(mk(OpCode::EQ, "abc", "abd"), false, "exec: \"abc\" == \"abd\" is false");
  checkBool(mk(OpCode::EQ, "abc", "ab"), false, "exec: \"abc\" == \"ab\" is false (lengths differ)");
  checkBool(mk(OpCode::NEQ, "abc", "abd"), true, "exec: \"abc\" != \"abd\" is true");
  checkBool(mk(OpCode::LT, "abc", "abd"), true, "exec: \"abc\" < \"abd\" is true");
  checkBool(mk(OpCode::LT, "ab", "abc"), true, "exec: \"ab\" < \"abc\" is true (prefix)");
  checkBool(mk(OpCode::GT, "abd", "abc"), true, "exec: \"abd\" > \"abc\" is true");
  checkBool(mk(OpCode::LTE, "abc", "abc"), true, "exec: \"abc\" <= \"abc\" is true");
  checkBool(mk(OpCode::GTE, "abc", "abc"), true, "exec: \"abc\" >= \"abc\" is true");
  // Lexicographic over unsigned bytes: 'Z' (0x5A) < 'a' (0x61).
  checkBool(mk(OpCode::LT, "Z", "a"), true, "exec: \"Z\" < \"a\" (unsigned byte order)");
}

// Global variables (PHASE 7c.2a). The compiler names a global by a string
// constant; the lowerer assigns each a slot in a flat table the chunk
// allocates at entry. A program assigns a global and reads it back; a second
// assignment overwrites it; two globals keep separate slots; and a read of an
// unassigned global is rejected at lowering time.
static void testExecGlobals() {
  // let x = 5; (R0) then return x (R1): R0 <- 5, SETGLOBAL x, GETGLOBAL x, HALT.
  // The global's name is the string constant at index 1.
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::SETGLOBAL, 0, 1),
                      instr(OpCode::GETGLOBAL, 1, 1), instr(OpCode::MOVE, 0, 1),
                      instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(5)), Value(std::string("x"))}, 2),
           5, "exec: a global assigned 5 reads back 5");

  // Reassignment: x = 5; x = 7; return x -> 7. Name at index 2.
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::SETGLOBAL, 0, 2),
                      instr(OpCode::LOADK, 1, 1), instr(OpCode::SETGLOBAL, 1, 2),
                      instr(OpCode::GETGLOBAL, 2, 2), instr(OpCode::MOVE, 0, 2),
                      instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(5)), Value(static_cast<int64_t>(7)),
                      Value(std::string("x"))}, 3),
           7, "exec: reassigning a global keeps the last value");

  // Two distinct globals do not share a slot: a = 3; b = 4; return a -> 3.
  // Names "a" and "b" are at indices 1 and 3.
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::SETGLOBAL, 0, 1),
                      instr(OpCode::LOADK, 1, 2), instr(OpCode::SETGLOBAL, 1, 3),
                      instr(OpCode::GETGLOBAL, 2, 1), instr(OpCode::MOVE, 0, 2),
                      instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(3)), Value(std::string("a")),
                      Value(static_cast<int64_t>(4)), Value(std::string("b"))}, 3),
           3, "exec: distinct globals keep separate slots");

  // A global holding a string: s = "hi"; return s.
  checkStr(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::SETGLOBAL, 0, 1),
                      instr(OpCode::GETGLOBAL, 1, 1), instr(OpCode::MOVE, 0, 1),
                      instr(OpCode::HALT)},
                     {Value(std::string("hi")), Value(std::string("s"))}, 2),
           "hi", "exec: a global holds a string");
}

// Locals (PHASE 7c.2b). A local is a 16-byte slot in the second region of the
// chunk frame. A program declares a local and reads it back; `SETLOCAL`
// overwrites it; two locals keep separate slots; and a local coexists with a
// global. Unlike a global, a `GETLOCAL` of a slot the chunk never declares is
// not rejected: a stack slot cannot hold a null cell, so a well-formed chunk
// (the compiler always emits `DECLLOCAL` first) is assumed.
static void testExecLocals() {
  // { var x = 5; } then read it back: R0 <- 5, DECLLOCAL 0, GETLOCAL 0, HALT.
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::DECLLOCAL, 0, 0),
                      instr(OpCode::GETLOCAL, 1, 0), instr(OpCode::MOVE, 0, 1),
                      instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(5))}, 2, 1),
           5, "exec: a local declared 5 reads back 5");

  // Overwrite through SETLOCAL: x = 5; x = 7; return x -> 7.
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::DECLLOCAL, 0, 0),
                      instr(OpCode::LOADK, 1, 1), instr(OpCode::SETLOCAL, 1, 0),
                      instr(OpCode::GETLOCAL, 2, 0), instr(OpCode::MOVE, 0, 2),
                      instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(5)), Value(static_cast<int64_t>(7))}, 3, 1),
           7, "exec: overwriting a local keeps the last value");

  // Two distinct locals do not share a slot: a = 3; b = 4; return a -> 3.
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::DECLLOCAL, 0, 0),
                      instr(OpCode::LOADK, 1, 1), instr(OpCode::DECLLOCAL, 1, 1),
                      instr(OpCode::GETLOCAL, 2, 0), instr(OpCode::MOVE, 0, 2),
                      instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(3)), Value(static_cast<int64_t>(4))}, 3, 2),
           3, "exec: distinct locals keep separate slots");

  // A local and a global coexist: g = 10; { var x = 4; g = g + x; } return g.
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::SETGLOBAL, 0, 1),
                      instr(OpCode::LOADK, 1, 2), instr(OpCode::DECLLOCAL, 1, 0),
                      instr(OpCode::GETGLOBAL, 0, 1), instr(OpCode::GETLOCAL, 1, 0),
                      instr(OpCode::ADD, 0, 0, 1), instr(OpCode::SETGLOBAL, 0, 1),
                      instr(OpCode::GETGLOBAL, 1, 1), instr(OpCode::MOVE, 0, 1),
                      instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(10)), Value(std::string("g")),
                      Value(static_cast<int64_t>(4))}, 2, 1),
           14, "exec: a local and a global do not collide");

  // A local holding a string survives the slot round trip.
  checkStr(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::DECLLOCAL, 0, 0),
                      instr(OpCode::GETLOCAL, 1, 0), instr(OpCode::MOVE, 0, 1),
                      instr(OpCode::HALT)},
                     {Value(std::string("hi"))}, 2, 1),
           "hi", "exec: a local holds a string");
}

// The real compiler drives the lowering: compile source, then lower it and run
// the chunk. This locks the compiler/lowerer contract for globals -- in
// particular that a `let` and its later reads share one string constant, so the
// lowerer's name-to-slot map finds them.
static void testExecCompiledGlobals() {
  AotValue value = compileAndRun("let x = 5; let y = x + 2;");
  check(value.tag == static_cast<std::uint64_t>(AotTag::Int) &&
            static_cast<std::int64_t>(value.payload) == 7,
        "compiled: let x = 5; let y = x + 2; yields 7 in R0");
}

// `print` is a builtin the chunk seeds, not a user global: before 7c.2d a call
// to it was a lowering error ("global 'print' is read but never assigned"). The
// call now lowers and returns nil; the printed bytes are checked end to end in
// `freestanding_test.cpp`.
static void testExecCompiledPrint() {
  AotValue value = compileAndRun("print(1);");
  check(value.tag == static_cast<std::uint64_t>(AotTag::Nil),
        "compiled: print(1) lowers and returns nil");
}

// Same, for locals: the compiler emits DECLLOCAL/GETLOCAL/SETLOCAL for a nested
// block, and the chunk runs it. A nested block is a local scope even at the top
// level, so this exercises the local region without a call frame.
static void testExecCompiledLocals() {
  AotValue value = compileAndRun("{ var x = 5; x = x + 2; }");
  check(value.tag == static_cast<std::uint64_t>(AotTag::Int) &&
            static_cast<std::int64_t>(value.payload) == 7,
        "compiled: a nested block with a local yields 7 in R0");

  // A classic `for` keeps its induction variable in a local slot and loops.
  // The trailing `var r = s;` reads the sum back into R0, since the loop's last
  // value (its exit condition) is what would otherwise sit there.
  AotValue sum =
      compileAndRun("var s = 0; for (var i = 0; i < 4; i = i + 1) { s = s + i; } var r = s;");
  check(sum.tag == static_cast<std::uint64_t>(AotTag::Int) &&
            static_cast<std::int64_t>(sum.payload) == 6,
        "compiled: a classic for over a local induction variable sums 0..3");
}

// Compiled functions: the compiler emits CLOSURE (to bind the `fn` global),
// CALL and RETURN; the lowerer emits each proto as its own machine function and
// dispatches through `oskel_call`. `var r = ...;` reads the result back into R0.
static void testExecCompiledCalls() {
  checkIntValue(compileAndRun("fn add(a, b) { return a + b; } var r = add(2, 3);"), 5,
                "compiled: add(2, 3) == 5 through a real call");

  // Recursion: `fact` calls itself through the global name.
  checkIntValue(compileAndRun("fn fact(n) { if (n <= 1) { return 1; }"
                              " return n * fact(n - 1); } var r = fact(5);"),
                120, "compiled: fact(5) == 120 (recursion)");

  // Mutual recursion: two globals calling each other.
  checkIntValue(compileAndRun("fn isEven(n) { if (n == 0) { return 1; }"
                              " return isOdd(n - 1); }"
                              " fn isOdd(n) { if (n == 0) { return 0; }"
                              " return isEven(n - 1); } var r = isEven(10);"),
                1, "compiled: isEven(10) == 1 (mutual recursion)");

  // A function called twice with different arguments (each call gets a fresh
  // frame, so no state leaks between them).
  checkIntValue(compileAndRun("fn double(x) { return x + x; }"
                              " var a = double(3); var r = double(7);"),
                14, "compiled: double(7) == 14 after an earlier double(3)");
}

// A capture-free lambda: `() => 42` is a child proto with no upvalues, so
// CLOSURE lowers it and CALL dispatches it.
static void testExecCompiledClosure() {
  checkIntValue(compileAndRun("var f = () => 42; var r = f();"), 42,
                "compiled: a capture-free lambda returns 42");
}

// Captured locals (PHASE 7c.2c.2): a lambda that reads a local of its defining
// function captures the *cell*, so it still sees the variable after the
// defining frame is gone. The captured slot lives in a shared cell, not a
// copy.
static void testExecCompiledCapture() {
  // A closure that reads a captured local after the defining function returned.
  checkIntValue(compileAndRun("fn make() { let x = 7; return () => x; }"
                              " var f = make(); var r = f();"),
                7, "compiled: a closure reads a captured local (7)");

  // Shared mutation: the closure writes the captured local and the caller
  // observes the new value (one cell, not two copies).
  checkIntValue(compileAndRun("fn make() { var x = 0;"
                              " return () => { x = x + 1; return x; }; }"
                              " var c = make(); var a = c(); var r = c();"),
                2, "compiled: a counter closure increments a shared captured cell");

  // Two closures over the same variable share one cell: the second sees the
  // first's write.
  checkIntValue(compileAndRun("fn make() { var x = 0;"
                              " var inc = () => { x = x + 1; };"
                              " var get = () => x;"
                              " inc(); inc(); return get(); }"
                              " var r = make();"),
                2, "compiled: two closures share one captured cell");

  // Nested capture: the inner lambda captures through the outer function, so
  // the middle frame re-exports its own upvalue.
  checkIntValue(compileAndRun("fn outer() { let x = 41;"
                              " fn mid() { return () => x + 1; }"
                              " return mid(); }"
                              " var f = outer(); var r = f();"),
                42, "compiled: a nested closure captures through two frames");

  // A captured parameter is a cell too: `let` parameters are locals.
  checkIntValue(compileAndRun("fn adder(n) { return (m) => n + m; }"
                              " var add5 = adder(5); var r = add5(37);"),
                42, "compiled: a closure captures a parameter");

  // Two closures created at different points capture the same variable, so both
  // read the value it holds when they are *called* (a shared cell), not a
  // snapshot taken at creation time.
  checkIntValue(compileAndRun("fn make() { var a = 1;"
                              " var f1 = () => a; a = 2; var f2 = () => a;"
                              " return f1() + f2(); }"
                              " var r = make();"),
                4, "compiled: closures created at different times share one cell");

  // A local function that recurses: `g` captures its own slot (predeclared as a
  // nil cell before the closure is built), so the cell must exist and be the
  // one the declaration fills in.
  checkIntValue(compileAndRun("fn outer() {"
                              " fn g(n) { if (n <= 0) { return 0; } return n + g(n - 1); }"
                              " return g(4); }"
                              " var r = outer();"),
                10, "compiled: a local recursive function captures its own cell");
}

#endif  // LOWER_EXEC_TESTS

int main() {
  testImageIsSelfContained();
  testCollectionsLower();
  testIterOperandsLower();
  testBadJumpTargetThrows();
  testUnassignedGlobalThrows();
  testHostBindingThrows();
  testUnusedHostBindingLowers();
  testExternSymbolGetsDedicatedMessage();
  testInvalidUnopSelectorThrows();
  testHaltInChildThrows();
  testRootReturnLowers();

#ifdef LOWER_EXEC_TESTS
  testExecRootReturn();
  testExecLoads();
  testExecMove();
  testExecArithmetic();
  testExecBitwise();
  testExecComparisonsAsValues();
  testExecUnop();
  testExecJumps();
  testExecBackwardJumpLoop();
  testExecManyRegisters();
  testExecFloatLoads();
  testExecFloatArithmetic();
  testExecFloatMod();
  testExecIntDivModPow();
  testExecFloatPow();
  testExecShifts();
  testExecFloatComparisons();
  testExecFloatNegate();
  testExecStringLoad();
  testExecStringConcat();
  testExecArrayConcat();
  testExecArrayMethods();
  testExecArrayMethodsInPlace();
  testExecStringCompare();
  testExecGlobals();
  testExecLocals();
  testExecCompiledGlobals();
  testExecCompiledPrint();
  testExecCompiledLocals();
  testExecCompiledCalls();
  testExecCompiledClosure();
  testExecCompiledCapture();
#else
  std::printf("skipped: execution tests require Linux x86-64\n");
#endif

  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all lower tests passed\n");
  return 0;
}
