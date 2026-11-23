// Tests for the `.oskc` bytecode image (PHASE 6i).
//
// The format's promise is portability: the same file reads on any host and
// lowers to a native image on every target the AOT backend supports. These
// tests pin the three things that promise rests on:
//
//  1. A round trip preserves the chunk exactly, so a compiled program behaves
//     the same whether it came from source or from a file.
//  2. Every byte is little-endian and fixed-width, so the file does not depend
//     on the host that wrote it.
//  3. A corrupt or hostile file is a clean `BytecodeIoError`, never a crash:
//     a bad opcode, a bad constant index, a bad jump target and a truncated
//     image are all rejected by the loader.
//
// The FFI side is tested separately: the file records what the chunk *needs*
// (not the superset), and a destination refuses exactly the capability it
// lacks.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "bytecode.h"
#include "bytecode_io.h"
#include "compiler.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "native/target.h"

namespace {

[[noreturn]] void fail(const std::string& message) {
  std::fprintf(stderr, "bytecode_io_test: %s\n", message.c_str());
  std::exit(1);
}

void check(bool condition, const std::string& message) {
  if (!condition) fail(message);
}

// Compiles a program to a chunk. The AOT capabilities are used so the chunk is
// the same one `oskelc -c` writes.
Bytecode compileToChunk(const std::string& source, const std::string& name) {
  std::vector<Token> tokens = lex(source);
  Parser parser(std::move(tokens), name);
  StmtPtr program = parser.parseProgram();
  Compiler compiler;
  compiler.setAbiCapabilities(ffi::kAotAbiCapabilities);
  return compiler.compile(program);
}

bool sameValue(const Value& left, const Value& right) {
  if (left.type() != right.type()) return false;
  switch (left.type()) {
    case VType::Nil:
      return true;
    case VType::Bool:
      return left.asBool() == right.asBool();
    case VType::Int:
      return left.asInt() == right.asInt();
    case VType::Float:
    case VType::Double: {
      const double leftNumber = left.asDouble();
      const double rightNumber = right.asDouble();
      return std::memcmp(&leftNumber, &rightNumber, sizeof(double)) == 0;
    }
    case VType::Str:
      return left.asStr() == right.asStr();
    default:
      return false;
  }
}

bool sameUpvalue(const UpvalDesc& left, const UpvalDesc& right) {
  return left.inParentLocal == right.inParentLocal && left.index == right.index;
}

bool sameInstruction(const Instruction& left, const Instruction& right) {
  return left.op == right.op && left.a == right.a && left.b == right.b &&
         left.c == right.c && left.jump == right.jump && left.d == right.d &&
         left.e == right.e && left.line == right.line && left.col == right.col;
}

bool sameTypeSpec(const ffi::TypeSpec& left, const ffi::TypeSpec& right) {
  return ffi::canonicalTypeSpec(left) == ffi::canonicalTypeSpec(right);
}

// A structural comparison. `children` and `externs` recurse, so a difference
// anywhere in the tree fails the round trip.
void checkSameChunk(const Bytecode& left, const Bytecode& right,
                    const std::string& where) {
  check(left.name == right.name, where + ": name");
  check(left.numParams == right.numParams, where + ": numParams");
  check(left.numRegisters == right.numRegisters, where + ": numRegisters");
  check(left.numLocals == right.numLocals, where + ": numLocals");
  check(left.variadic == right.variadic, where + ": variadic");

  check(left.code.size() == right.code.size(), where + ": code size");
  for (std::size_t i = 0; i < left.code.size(); ++i) {
    check(sameInstruction(left.code[i], right.code[i]),
          where + ": instruction " + std::to_string(i));
  }

  check(left.constants.size() == right.constants.size(), where + ": constant count");
  for (std::size_t i = 0; i < left.constants.size(); ++i) {
    check(sameValue(left.constants[i], right.constants[i]),
          where + ": constant " + std::to_string(i));
  }

  check(left.children.size() == right.children.size(), where + ": child count");
  for (std::size_t i = 0; i < left.children.size(); ++i) {
    checkSameChunk(*left.children[i], *right.children[i],
                   where + ".child" + std::to_string(i));
  }

  check(left.upvalues.size() == right.upvalues.size(), where + ": upvalue count");
  for (std::size_t i = 0; i < left.upvalues.size(); ++i) {
    check(sameUpvalue(left.upvalues[i], right.upvalues[i]),
          where + ": upvalue " + std::to_string(i));
  }

  check(left.externs.size() == right.externs.size(), where + ": extern count");
  for (std::size_t i = 0; i < left.externs.size(); ++i) {
    const ExternSymbol& a = left.externs[i];
    const ExternSymbol& b = right.externs[i];
    check(a.name == b.name, where + ": extern name");
    check(a.variadic == b.variadic, where + ": extern variadic");
    check(sameTypeSpec(a.returnType, b.returnType), where + ": extern return type");
    check(a.paramTypes.size() == b.paramTypes.size(), where + ": extern param count");
    for (std::size_t p = 0; p < a.paramTypes.size(); ++p) {
      check(sameTypeSpec(a.paramTypes[p], b.paramTypes[p]), where + ": extern param type");
    }
  }

  check(left.nativeFns == right.nativeFns, where + ": nativeFns");
  check(left.nativeUses == right.nativeUses, where + ": nativeUses");
  check(left.usedMethods == right.usedMethods, where + ": usedMethods");
}

Bytecode roundTrip(const Bytecode& chunk) {
  const std::vector<std::uint8_t> image = writeBytecodeImage(chunk);
  return readBytecodeImage(image);
}

// A program with the shapes the format has to carry: functions and closures
// (children, upvalues), collections, strings, ints, doubles and control flow.
const char* kRichProgram = R"(
fn makeCounter(start: int) {
  var count = start;
  fn bump(step: int): int {
    count = count + step;
    return count;
  }
  return bump;
}

var counter = makeCounter(10);
print(counter(5));

var numbers = [1, 2, 3];
var total = 0;
for (var i = 0; i < len(numbers); i += 1) {
  total = total + numbers[i];
}
print(total);

var config = {"name": "oskel", "version": 1, "ratio": 1.5};
print(config["name"]);
)";

void testRoundTripPreservesEverything() {
  const Bytecode original = compileToChunk(kRichProgram, "rich.oskl");
  const Bytecode restored = roundTrip(original);
  checkSameChunk(original, restored, "<main>");

  // The rich program must actually exercise the interesting fields, otherwise
  // the round trip proves nothing. A closure lives in a child proto with an
  // upvalue; the map and the array produce string constants.
  check(!original.children.empty(), "fixture should produce at least one child proto");
  check(!original.constants.empty(), "fixture should produce constants");
}

void testRoundTripIsDeterministic() {
  const Bytecode chunk = compileToChunk(kRichProgram, "rich.oskl");
  const std::vector<std::uint8_t> first = writeBytecodeImage(chunk);
  const std::vector<std::uint8_t> second = writeBytecodeImage(chunk);
  check(first == second, "writing the same chunk twice must produce the same bytes");
}

void testHeaderIsLittleEndianAndFixed() {
  const Bytecode chunk = compileToChunk("print(1);", "small.oskl");
  const std::vector<std::uint8_t> image = writeBytecodeImage(chunk);

  check(image.size() > kOskcHeaderSize, "image must have a header");
  check(image[0] == 'O' && image[1] == 'S' && image[2] == 'K' && image[3] == 'C',
        "magic must be 'OSKC'");
  check(isBytecodeImage(image), "isBytecodeImage must accept a valid image");

  // The version is little-endian at offset 4.
  const std::uint32_t version = readBytecodeImageVersion(image);
  check(version == kOskcVersion, "version must match kOskcVersion");
  check(image[4] == static_cast<std::uint8_t>(version & 0xFF), "version low byte first");

  // The flags field is reserved and must be zero.
  check(image[8] == 0 && image[9] == 0 && image[10] == 0 && image[11] == 0,
        "reserved flags must be zero");
}

void testNoFfiNeedsNoCapability() {
  const Bytecode chunk = compileToChunk("print(1);", "plain.oskl");
  const std::vector<std::uint8_t> image = writeBytecodeImage(chunk);
  check(readBytecodeImageAbi(image) == 0,
        "a program without FFI must claim no optional ABI capability");
  check(requiredAbiCapabilities(chunk).structByValue == false,
        "requiredAbiCapabilities must start empty");
}

void testStructFfiNeedsStructCapability() {
  const Bytecode chunk = compileToChunk(
      "struct Pair { a: int32; b: int32; }\n"
      "extern \"C\" { fn f(p: Pair): int32; }\n",
      "ffi.oskl");
  const ffi::AbiCapabilities needs = requiredAbiCapabilities(chunk);
  check(needs.structByValue, "a struct parameter must require structByValue");
  check(!needs.variadic, "a non-variadic declaration must not require variadic");
}

void testVariadicFfiNeedsVariadicCapability() {
  const Bytecode chunk = compileToChunk(
      "extern \"C\" { fn printf(fmt: cstring, ...): int; }\n", "var.oskl");
  check(requiredAbiCapabilities(chunk).variadic,
        "a C variadic must require the variadic capability");
}

void testAbiSatisfiesIsMonotonic() {
  ffi::AbiCapabilities nothing;
  nothing.structByValue = false;
  nothing.memoryStructs = false;
  nothing.variadic = false;
  nothing.callbacks = false;

  ffi::AbiCapabilities everything = nothing;
  everything.structByValue = true;
  everything.memoryStructs = true;
  everything.variadic = true;
  everything.callbacks = true;

  check(abiSatisfies(everything, nothing), "everything satisfies nothing");
  check(!abiSatisfies(nothing, everything), "nothing does not satisfy everything");

  ffi::AbiCapabilities onlyStruct = nothing;
  onlyStruct.structByValue = true;
  check(abiSatisfies(onlyStruct, onlyStruct), "a set satisfies itself");
  check(!abiSatisfies(nothing, onlyStruct), "an empty set lacks a struct capability");
}

void testTargetCapabilitiesMatchBackends() {
  // System V has the whole surface; Win64 has the scalar and variadic surface
  // but not structs by value or callbacks; WebAssembly has nothing.
  const ffi::AbiCapabilities sysv = targetAbiCapabilities(Target::linuxX64());
  check(sysv.structByValue && sysv.memoryStructs && sysv.variadic && sysv.callbacks,
        "System V must support the full FFI surface");

  const ffi::AbiCapabilities win = targetAbiCapabilities(Target::winX64());
  check(!win.structByValue && !win.memoryStructs && !win.callbacks,
        "Win64 must not claim structs by value, memory structs or callbacks");
  check(win.variadic, "Win64 must support a C variadic");

  const ffi::AbiCapabilities wasm = targetAbiCapabilities(Target::wasm32());
  check(!wasm.structByValue && !wasm.memoryStructs && !wasm.variadic &&
            !wasm.callbacks,
        "WebAssembly must claim no FFI capability");

  // A struct-bearing chunk is accepted by System V and refused by Win64: the
  // portability guarantee, checked at the level the loader uses.
  const Bytecode chunk = compileToChunk(
      "struct Pair { a: int32; b: int32; }\n"
      "extern \"C\" { fn f(p: Pair): int32; }\n",
      "ffi.oskl");
  const ffi::AbiCapabilities needs = requiredAbiCapabilities(chunk);
  check(abiSatisfies(sysv, needs), "System V must accept a struct-bearing chunk");
  check(!abiSatisfies(win, needs), "Win64 must refuse a struct-bearing chunk");
}

void expectLoadFailure(const std::vector<std::uint8_t>& image,
                       const std::string& what) {
  try {
    readBytecodeImage(image);
  } catch (const BytecodeIoError&) {
    return;
  }
  fail("expected a BytecodeIoError for " + what);
}

void testRejectsBadMagic() {
  std::vector<std::uint8_t> image = writeBytecodeImage(compileToChunk("print(1);", "x.oskl"));
  image[0] = 'J';
  expectLoadFailure(image, "a wrong magic");
  check(!isBytecodeImage(image), "isBytecodeImage must reject a wrong magic");
}

void testRejectsWrongVersion() {
  std::vector<std::uint8_t> image = writeBytecodeImage(compileToChunk("print(1);", "x.oskl"));
  image[4] = 99;  // version low byte
  expectLoadFailure(image, "an unknown version");
}

void testRejectsNonZeroFlags() {
  std::vector<std::uint8_t> image = writeBytecodeImage(compileToChunk("print(1);", "x.oskl"));
  image[8] = 1;  // reserved flags
  expectLoadFailure(image, "a non-zero reserved flags field");
}

void testRejectsTruncation() {
  const std::vector<std::uint8_t> image =
      writeBytecodeImage(compileToChunk("print(1);", "x.oskl"));
  // Every proper prefix must be a clean error, never a crash or a partial read.
  for (std::size_t cut = 0; cut < image.size(); ++cut) {
    std::vector<std::uint8_t> partial(image.begin(),
                                      image.begin() + static_cast<std::ptrdiff_t>(cut));
    expectLoadFailure(partial, "a prefix of length " + std::to_string(cut));
  }
}

void testRejectsTrailingBytes() {
  std::vector<std::uint8_t> image = writeBytecodeImage(compileToChunk("print(1);", "x.oskl"));
  image.push_back(0);
  expectLoadFailure(image, "trailing bytes");
}

void testRejectsBadOpcode() {
  const Bytecode chunk = compileToChunk("print(1);", "x.oskl");
  std::vector<std::uint8_t> image = writeBytecodeImage(chunk);
  // The first instruction's opcode follows the header and the chunk prologue:
  // name (u32 length + bytes), numParams, numRegisters, numLocals, variadic,
  // then the code count. Compute the offset from the writer's own layout so the
  // test does not hard-code the prologue size.
  const std::uint32_t nameLength =
      static_cast<std::uint32_t>(image[kOskcHeaderSize]) |
      static_cast<std::uint32_t>(image[kOskcHeaderSize + 1]) << 8 |
      static_cast<std::uint32_t>(image[kOskcHeaderSize + 2]) << 16 |
      static_cast<std::uint32_t>(image[kOskcHeaderSize + 3]) << 24;
  const std::size_t codeCountOffset =
      kOskcHeaderSize + 4 + nameLength + 4 + 4 + 4 + 1;
  const std::size_t firstOpcode = codeCountOffset + 4;
  image[firstOpcode] = 200;  // beyond kOpcodeCount
  expectLoadFailure(image, "an unknown opcode");
}

void testRejectsBadConstantIndex() {
  // `LOADK` with a constant index past the pool. Build the chunk by hand so the
  // defect is exactly one field, not a side effect of compiling something.
  Bytecode chunk;
  chunk.name = "<main>";
  chunk.numRegisters = 1;
  Instruction load;
  load.op = OpCode::LOADK;
  load.a = 0;
  load.b = 7;  // no such constant
  chunk.code.push_back(load);
  const std::vector<std::uint8_t> image = writeBytecodeImage(chunk);
  expectLoadFailure(image, "a constant index past the pool");
}

void testRejectsBadJumpTarget() {
  Bytecode chunk;
  chunk.name = "<main>";
  chunk.numRegisters = 1;
  Instruction jump;
  jump.op = OpCode::JMP;
  jump.jump = 99;  // past the single-instruction code
  chunk.code.push_back(jump);
  const std::vector<std::uint8_t> image = writeBytecodeImage(chunk);
  expectLoadFailure(image, "a jump target past the code");
}

void testRejectsBadRegister() {
  Bytecode chunk;
  chunk.name = "<main>";
  chunk.numRegisters = 1;
  Instruction move;
  move.op = OpCode::MOVE;
  move.a = 0;
  move.b = 5;  // only register 0 exists
  chunk.code.push_back(move);
  const std::vector<std::uint8_t> image = writeBytecodeImage(chunk);
  expectLoadFailure(image, "a register outside the frame");
}

void testAcceptsJumpToEnd() {
  // A jump to `code.size()` is the exit of a loop and must stay legal.
  Bytecode chunk;
  chunk.name = "<main>";
  chunk.numRegisters = 1;
  Instruction jump;
  jump.op = OpCode::JMP;
  jump.jump = 1;
  chunk.code.push_back(jump);
  const std::vector<std::uint8_t> image = writeBytecodeImage(chunk);
  const Bytecode restored = readBytecodeImage(image);
  check(restored.code.size() == 1, "a jump to the end must round-trip");
}

void testRejectsNonSerializableConstant() {
  // An array constant is not something `LOADK` can load, so the writer refuses
  // it rather than emit a file the loader could not rebuild.
  Bytecode chunk;
  chunk.name = "<main>";
  chunk.numRegisters = 1;
  chunk.constants.push_back(Value(makeArray({Value(1), Value(2)})));
  bool threw = false;
  try {
    writeBytecodeImage(chunk);
  } catch (const BytecodeIoError&) {
    threw = true;
  }
  check(threw, "an array constant must be refused by the writer");
}

void testRejectsNullChild() {
  Bytecode chunk;
  chunk.name = "<main>";
  chunk.children.push_back(nullptr);
  bool threw = false;
  try {
    writeBytecodeImage(chunk);
  } catch (const BytecodeIoError&) {
    threw = true;
  }
  check(threw, "a null child proto must be refused by the writer");
}

void testPathDetection() {
  check(isBytecodeImagePath("app.oskc"), "app.oskc must be detected");
  check(isBytecodeImagePath("APP.OSKC"), "the extension check must be case-insensitive");
  check(!isBytecodeImagePath("app.oskl"), "app.oskl must not be detected");
  check(!isBytecodeImagePath("oskc"), "a bare name must not be detected");
  check(!isBytecodeImagePath(".oskc"), "the extension alone is a valid path");
}

void testAbiRoundTripThroughBits() {
  ffi::AbiCapabilities original;
  original.structByValue = true;
  original.memoryStructs = false;
  original.variadic = true;
  original.callbacks = false;
  const ffi::AbiCapabilities restored =
      decodeAbiCapabilities(encodeAbiCapabilities(original));
  check(restored.structByValue == original.structByValue, "structByValue bit");
  check(restored.memoryStructs == original.memoryStructs, "memoryStructs bit");
  check(restored.variadic == original.variadic, "variadic bit");
  check(restored.callbacks == original.callbacks, "callbacks bit");
}

void testNeedsBitsRoundTrip() {
  // A plain program needs nothing from the UI backend, so it writes zero and
  // loads on a build without the native runtime.
  const Bytecode plain = compileToChunk("print(1);", "plain.oskl");
  const std::vector<std::uint8_t> plainImage = writeBytecodeImage(plain);
  check(readBytecodeImageNeeds(plainImage) == 0,
        "a program without UI must record no requirement");

  // The writer takes the bits as an argument because the modules a program
  // imported are the compiler's knowledge, not the chunk's. Both bits must
  // survive the header unchanged.
  const std::uint32_t needs = kOskcNeedsSdl | kOskcNeedsSkia;
  const std::vector<std::uint8_t> uiImage = writeBytecodeImage(plain, needs);
  check(readBytecodeImageNeeds(uiImage) == needs,
        "the requirement bits must round trip through the header");
  check(readBytecodeImageNeeds(uiImage) != readBytecodeImageNeeds(plainImage),
        "a UI program and a plain one must be distinguishable");
}

} // namespace

int main() {
  testRoundTripPreservesEverything();
  testRoundTripIsDeterministic();
  testHeaderIsLittleEndianAndFixed();
  testNoFfiNeedsNoCapability();
  testStructFfiNeedsStructCapability();
  testVariadicFfiNeedsVariadicCapability();
  testAbiSatisfiesIsMonotonic();
  testTargetCapabilitiesMatchBackends();
  testRejectsBadMagic();
  testRejectsWrongVersion();
  testRejectsNonZeroFlags();
  testRejectsTruncation();
  testRejectsTrailingBytes();
  testRejectsBadOpcode();
  testRejectsBadConstantIndex();
  testRejectsBadJumpTarget();
  testRejectsBadRegister();
  testAcceptsJumpToEnd();
  testRejectsNonSerializableConstant();
  testRejectsNullChild();
  testPathDetection();
  testAbiRoundTripThroughBits();
  testNeedsBitsRoundTrip();
  std::puts("bytecode_io tests passed");
  return 0;
}
