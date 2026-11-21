#include "native/wasm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/value.h"
#include "native/lower.h"
#include "native/value.h"

namespace {

using Bytes = std::vector<std::uint8_t>;

// ------------------------------------------------------------------ encoding
void uleb(Bytes& out, std::uint64_t value) {
  do {
    std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7F);
    value >>= 7;
    if (value) byte |= 0x80;
    out.push_back(byte);
  } while (value);
}

void sleb(Bytes& out, std::int64_t value) {
  bool more = true;
  while (more) {
    std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7F);
    value >>= 7;
    const bool sign = (byte & 0x40) != 0;
    if ((value == 0 && !sign) || (value == -1 && sign)) {
      more = false;
    } else {
      byte |= 0x80;
    }
    out.push_back(byte);
  }
}

void writeName(Bytes& out, const std::string& text) {
  uleb(out, text.size());
  out.insert(out.end(), text.begin(), text.end());
}

void writeF64Bits(Bytes& out, std::uint64_t bits) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
}

// WASM value types.
constexpr std::uint8_t kI32 = 0x7F;
constexpr std::uint8_t kI64 = 0x7E;
constexpr std::uint8_t kF64 = 0x7C;

// Value tags, shared with the x86-64 runtime (`native/value.h`).
constexpr std::uint64_t kNil = static_cast<std::uint64_t>(AotTag::Nil);
constexpr std::uint64_t kBool = static_cast<std::uint64_t>(AotTag::Bool);
constexpr std::uint64_t kInt = static_cast<std::uint64_t>(AotTag::Int);
constexpr std::uint64_t kFloat = static_cast<std::uint64_t>(AotTag::Float);
constexpr std::uint64_t kDouble = static_cast<std::uint64_t>(AotTag::Double);
constexpr std::uint64_t kStr = static_cast<std::uint64_t>(AotTag::Str);
constexpr std::uint64_t kFunc = static_cast<std::uint64_t>(AotTag::Func);
constexpr std::uint64_t kRange = static_cast<std::uint64_t>(AotTag::Range);
constexpr std::uint64_t kError = static_cast<std::uint64_t>(AotTag::Error);
constexpr std::uint64_t kArray = static_cast<std::uint64_t>(AotTag::Array);
constexpr std::uint64_t kMap = static_cast<std::uint64_t>(AotTag::Map);

// On-image layouts, shared with `include/native/value.h`. An array is
// {length, capacity, items}; a map is {length, capacity, entries}, where an
// entry is {key AotString*, value tag, value payload} (24 bytes).
constexpr std::uint32_t kArrayLengthOffset = 0;
constexpr std::uint32_t kArrayCapacityOffset = 8;
constexpr std::uint32_t kArrayItemsOffset = 16;
constexpr std::uint32_t kMapLengthOffset = 0;
constexpr std::uint32_t kMapCapacityOffset = 8;
constexpr std::uint32_t kMapEntriesOffset = 16;
constexpr std::uint32_t kMapEntryKeyOffset = 0;
constexpr std::uint32_t kMapEntryTagOffset = 8;
constexpr std::uint32_t kMapEntryPayloadOffset = 16;
constexpr std::uint32_t kMapEntrySize = 24;

// mt19937_64 state (PHASE 9.3): 312 words followed by the cursor, the same
// layout the x86-64 runtime uses, so a seeded stream is identical on both
// backends and in the VM.
constexpr std::uint32_t kMtWords = 312;
constexpr std::uint32_t kMtMiddleWord = 156;
constexpr std::uint32_t kMtStateBytes = kMtWords * 8;
constexpr std::uint32_t kMtCursorOffset = kMtStateBytes;
constexpr std::uint32_t kMtStateTotal = kMtStateBytes + 8;
constexpr std::uint64_t kMtDefaultSeed = 5489ULL;

// --------------------------------------------------------------- module model
struct FuncType {
  std::vector<std::uint8_t> params;
  std::vector<std::uint8_t> results;
  bool operator==(const FuncType& other) const {
    return params == other.params && results == other.results;
  }
};

struct Body {
  std::uint32_t type = 0;
  std::vector<std::uint8_t> localTypes;
  Bytes code;
};

class ModuleBuilder {
 public:
  std::vector<FuncType> types;
  std::vector<std::string> importModules;
  std::vector<std::string> importFields;
  std::vector<std::uint32_t> importTypes;
  std::vector<Body> funcs;
  std::vector<std::uint32_t> tableFuncs;
  std::vector<std::pair<std::uint32_t, Bytes>> data;
  std::uint32_t arenaStart = 0;
  std::uint32_t memoryPages = 1;

  std::uint32_t typeFor(const std::vector<std::uint8_t>& params,
                        const std::vector<std::uint8_t>& results) {
    for (std::size_t i = 0; i < types.size(); ++i) {
      if (types[i].params == params && types[i].results == results) {
        return static_cast<std::uint32_t>(i);
      }
    }
    types.push_back(FuncType{params, results});
    return static_cast<std::uint32_t>(types.size() - 1);
  }

  std::uint32_t importCount() const { return static_cast<std::uint32_t>(importTypes.size()); }

  std::uint32_t addImport(const std::string& module, const std::string& field,
                          std::uint32_t type) {
    importModules.push_back(module);
    importFields.push_back(field);
    importTypes.push_back(type);
    return static_cast<std::uint32_t>(importTypes.size() - 1);
  }

  // Reserves a function so its index is known before its body is emitted (a
  // caller may reference a callee that is emitted later, and a closure stores
  // the table slot of a child proto before that proto's body exists).
  std::uint32_t reserveFunction(std::uint32_t type) {
    funcs.push_back(Body{type, {}, {}});
    return importCount() + static_cast<std::uint32_t>(funcs.size() - 1);
  }

  Bytes build() const;
};

// Emits into one reserved function body.
class Func {
 public:
  Func(ModuleBuilder& module, std::uint32_t functionIndex,
       std::vector<std::uint8_t> paramTypes)
      : module_(module),
        body_(module.funcs[functionIndex - module.importCount()]),
        localBase_(static_cast<std::uint32_t>(paramTypes.size())) {}

  std::uint32_t addLocal(std::uint8_t type) {
    body_.localTypes.push_back(type);
    return localBase_ + static_cast<std::uint32_t>(body_.localTypes.size() - 1);
  }

  Bytes& code() { return body_.code; }

  void op(std::uint8_t byte) { body_.code.push_back(byte); }
  void i32c(std::int32_t value) {
    op(0x41);
    sleb(body_.code, value);
  }
  void i64c(std::int64_t value) {
    op(0x42);
    sleb(body_.code, value);
  }
  void f64c(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    op(0x44);
    writeF64Bits(body_.code, bits);
  }
  void lget(std::uint32_t index) {
    op(0x20);
    uleb(body_.code, index);
  }
  void lset(std::uint32_t index) {
    op(0x21);
    uleb(body_.code, index);
  }
  void ltee(std::uint32_t index) {
    op(0x22);
    uleb(body_.code, index);
  }
  void gget(std::uint32_t index) {
    op(0x23);
    uleb(body_.code, index);
  }
  void gset(std::uint32_t index) {
    op(0x24);
    uleb(body_.code, index);
  }
  void call(std::uint32_t index) {
    op(0x10);
    uleb(body_.code, index);
  }
  void callIndirect(std::uint32_t type) {
    op(0x11);
    uleb(body_.code, type);
    uleb(body_.code, 0);
  }
  void block() {
    op(0x02);
    op(0x40);
  }
  void loop() {
    op(0x03);
    op(0x40);
  }
  void if_() {
    op(0x04);
    op(0x40);
  }
  void ifResult(std::uint8_t type) {
    op(0x04);
    op(type);
  }
  void else_() { op(0x05); }
  void end() { op(0x0B); }
  void br(std::uint32_t depth) {
    op(0x0C);
    uleb(body_.code, depth);
  }
  void brIf(std::uint32_t depth) {
    op(0x0D);
    uleb(body_.code, depth);
  }
  void brTable(const std::vector<std::uint32_t>& targets, std::uint32_t fallback) {
    op(0x0E);
    uleb(body_.code, targets.size());
    for (std::uint32_t target : targets) uleb(body_.code, target);
    uleb(body_.code, fallback);
  }
  void ret() { op(0x0F); }
  void unreachable() { op(0x00); }
  void drop() { op(0x1A); }
  void select() { op(0x1B); }
  void i32Load(std::uint32_t offset) {
    op(0x28);
    uleb(body_.code, 2);
    uleb(body_.code, offset);
  }
  void i64Load(std::uint32_t offset) {
    op(0x29);
    uleb(body_.code, 3);
    uleb(body_.code, offset);
  }
  void i32Load8u(std::uint32_t offset) {
    op(0x2D);
    uleb(body_.code, 0);
    uleb(body_.code, offset);
  }
  void i64Load8u(std::uint32_t offset) {
    op(0x30);
    uleb(body_.code, 0);
    uleb(body_.code, offset);
  }
  void i32Store(std::uint32_t offset) {
    op(0x36);
    uleb(body_.code, 2);
    uleb(body_.code, offset);
  }
  void f64Load(std::uint32_t offset) {
    op(0x2B);
    uleb(body_.code, 3);
    uleb(body_.code, offset);
  }
  void i64Store(std::uint32_t offset) {
    op(0x37);
    uleb(body_.code, 3);
    uleb(body_.code, offset);
  }
  void i32Store8(std::uint32_t offset) {
    op(0x3A);
    uleb(body_.code, 0);
    uleb(body_.code, offset);
  }
  void memoryCopy() {
    op(0xFC);
    uleb(body_.code, 0x0A);
    uleb(body_.code, 0);
    uleb(body_.code, 0);
  }
  void memorySize() {
    op(0x3F);
    uleb(body_.code, 0);
  }
  void memoryGrow() {
    op(0x40);
    uleb(body_.code, 0);
  }

  // A compact set of named opcodes so helper bodies read as WASM, not as hex.
  void i32Add() { op(0x6A); }
  void i32Sub() { op(0x6B); }
  void i32Mul() { op(0x6C); }
  void i32DivS() { op(0x6D); }
  void i32DivU() { op(0x6E); }
  void i32RemS() { op(0x6F); }
  void i32RemU() { op(0x70); }
  void i32And() { op(0x71); }
  void i32Or() { op(0x72); }
  void i32Xor() { op(0x73); }
  void i32Shl() { op(0x74); }
  void i32ShrS() { op(0x75); }
  void i32ShrU() { op(0x76); }
  void i32Eq() { op(0x46); }
  void i32Ne() { op(0x47); }
  void i32LtS() { op(0x48); }
  void i32LtU() { op(0x49); }
  void i32GtS() { op(0x4A); }
  void i32GtU() { op(0x4B); }
  void i32LeS() { op(0x4C); }
  void i32LeU() { op(0x4D); }
  void i32GeS() { op(0x4E); }
  void i32GeU() { op(0x4F); }
  void i32Eqz() { op(0x45); }
  void i64Add() { op(0x7C); }
  void i64Sub() { op(0x7D); }
  void i64Mul() { op(0x7E); }
  void i64DivS() { op(0x7F); }
  void i64DivU() { op(0x80); }
  void i64RemS() { op(0x81); }
  void i64RemU() { op(0x82); }
  void i64And() { op(0x83); }
  void i64Or() { op(0x84); }
  void i64Xor() { op(0x85); }
  void i64Shl() { op(0x86); }
  void i64ShrS() { op(0x87); }
  void i64ShrU() { op(0x88); }
  void i64Eq() { op(0x51); }
  void i64Ne() { op(0x52); }
  void i64LtS() { op(0x53); }
  void i64LtU() { op(0x54); }
  void i64GtS() { op(0x55); }
  void i64GtU() { op(0x56); }
  void i64LeS() { op(0x57); }
  void i64LeU() { op(0x58); }
  void i64GeS() { op(0x59); }
  void i64GeU() { op(0x5A); }
  void i64Eqz() { op(0x50); }
  void i64ExtendI32S() { op(0xAC); }
  void i64ExtendI32U() { op(0xAD); }
  void i32WrapI64() { op(0xA7); }
  void f64Add() { op(0xA0); }
  void f64Sub() { op(0xA1); }
  void f64Mul() { op(0xA2); }
  void f64Div() { op(0xA3); }
  void f64Neg() { op(0x9A); }
  void f64Abs() { op(0x99); }
  void f64Ceil() { op(0x9B); }
  void f64Floor() { op(0x9C); }
  void f64Trunc() { op(0x9D); }
  void f64Nearest() { op(0x9E); }
  void f64Sqrt() { op(0x9F); }
  void f64Eq() { op(0x61); }
  void f64Ne() { op(0x62); }
  void f64Lt() { op(0x63); }
  void f64Gt() { op(0x64); }
  void f64Le() { op(0x65); }
  void f64Ge() { op(0x66); }
  void f64ConvertI32S() { op(0xB7); }
  void f64ConvertI32U() { op(0xB8); }
  void f64ConvertI64S() { op(0xB9); }
  void f64ConvertI64U() { op(0xBA); }
  void f64PromoteF32() { op(0xBB); }
  void f32DemoteF64() { op(0xB6); }
  void f64ReinterpretI64() { op(0xBF); }
  void i64ReinterpretF64() { op(0xBD); }
  void i64TruncSatF64S() {
    op(0xFC);
    uleb(body_.code, 0x06);
  }

 private:
  ModuleBuilder& module_;
  Body& body_;
  std::uint32_t localBase_;
};

Bytes ModuleBuilder::build() const {
  Bytes out = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
  auto section = [&out](std::uint8_t id, const Bytes& payload) {
    out.push_back(id);
    uleb(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
  };

  Bytes typeSec;
  uleb(typeSec, types.size());
  for (const FuncType& type : types) {
    typeSec.push_back(0x60);
    uleb(typeSec, type.params.size());
    typeSec.insert(typeSec.end(), type.params.begin(), type.params.end());
    uleb(typeSec, type.results.size());
    typeSec.insert(typeSec.end(), type.results.begin(), type.results.end());
  }
  section(1, typeSec);

  if (!importTypes.empty()) {
    Bytes importSec;
    uleb(importSec, importTypes.size());
    for (std::size_t i = 0; i < importTypes.size(); ++i) {
      writeName(importSec, importModules[i]);
      writeName(importSec, importFields[i]);
      importSec.push_back(0x00);
      uleb(importSec, importTypes[i]);
    }
    section(2, importSec);
  }

  Bytes funcSec;
  uleb(funcSec, funcs.size());
  for (const Body& body : funcs) uleb(funcSec, body.type);
  section(3, funcSec);

  const std::uint32_t tableSize =
      std::max<std::uint32_t>(1, static_cast<std::uint32_t>(tableFuncs.size()));
  Bytes tableSec;
  uleb(tableSec, 1);
  tableSec.push_back(0x70);
  tableSec.push_back(0x00);
  uleb(tableSec, tableSize);
  section(4, tableSec);

  Bytes memSec;
  uleb(memSec, 1);
  memSec.push_back(0x00);
  uleb(memSec, memoryPages);
  section(5, memSec);

  Bytes globalSec;
  uleb(globalSec, 1);
  globalSec.push_back(kI32);
  globalSec.push_back(0x01);
  globalSec.push_back(0x41);
  sleb(globalSec, arenaStart);
  globalSec.push_back(0x0B);
  section(6, globalSec);

  const std::uint32_t startIndex = importCount() + static_cast<std::uint32_t>(funcs.size() - 1);
  Bytes exportSec;
  uleb(exportSec, 2);
  writeName(exportSec, "_start");
  exportSec.push_back(0x00);
  uleb(exportSec, startIndex);
  writeName(exportSec, "memory");
  exportSec.push_back(0x02);
  uleb(exportSec, 0);
  section(7, exportSec);

  Bytes elemSec;
  uleb(elemSec, 1);
  uleb(elemSec, 0);
  elemSec.push_back(0x41);
  sleb(elemSec, 0);
  elemSec.push_back(0x0B);
  uleb(elemSec, tableFuncs.size());
  for (std::uint32_t index : tableFuncs) uleb(elemSec, index);
  section(9, elemSec);

  Bytes codeSec;
  uleb(codeSec, funcs.size());
  for (const Body& body : funcs) {
    Bytes encoded;
    std::size_t i = 0;
    std::vector<std::pair<std::uint32_t, std::uint8_t>> runs;
    while (i < body.localTypes.size()) {
      std::uint8_t type = body.localTypes[i];
      std::uint32_t count = 1;
      while (i + count < body.localTypes.size() && body.localTypes[i + count] == type) ++count;
      runs.emplace_back(count, type);
      i += count;
    }
    uleb(encoded, runs.size());
    for (const auto& run : runs) {
      uleb(encoded, run.first);
      encoded.push_back(run.second);
    }
    encoded.insert(encoded.end(), body.code.begin(), body.code.end());
    encoded.push_back(0x0B);
    uleb(codeSec, encoded.size());
    codeSec.insert(codeSec.end(), encoded.begin(), encoded.end());
  }
  section(10, codeSec);

  if (!data.empty()) {
    Bytes dataSec;
    uleb(dataSec, data.size());
    for (const auto& segment : data) {
      dataSec.push_back(0x00);
      dataSec.push_back(0x41);
      sleb(dataSec, segment.first);
      dataSec.push_back(0x0B);
      uleb(dataSec, segment.second.size());
      dataSec.insert(dataSec.end(), segment.second.begin(), segment.second.end());
    }
    section(11, dataSec);
  }
  return out;
}

// --------------------------------------------------------------- static data
// A blob of read-only data laid out after the global table. Every address is an
// absolute linear-memory address, so a value can carry it as its payload.
struct StaticPool {
  std::uint32_t base = 0;
  Bytes bytes;

  std::uint32_t alignTo(std::uint32_t alignment) {
    while ((base + bytes.size()) % alignment != 0) bytes.push_back(0);
    return base + static_cast<std::uint32_t>(bytes.size());
  }

  std::uint32_t addString(const std::string& text) {
    alignTo(8);
    std::uint32_t address = base + static_cast<std::uint32_t>(bytes.size());
    std::uint64_t length = text.size();
    for (int i = 0; i < 8; ++i) {
      bytes.push_back(static_cast<std::uint8_t>(length >> (8 * i)));
    }
    bytes.insert(bytes.end(), text.begin(), text.end());
    return address;
  }

  std::uint32_t addDoubles(const std::vector<double>& values) {
    alignTo(8);
    std::uint32_t address = base + static_cast<std::uint32_t>(bytes.size());
    for (double value : values) {
      std::uint64_t bits = 0;
      std::memcpy(&bits, &value, sizeof bits);
      for (int i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
      }
    }
    return address;
  }

  // A closure header: code slot, arity, captured-cell count, name, flags.
  std::uint32_t addClosure(std::uint64_t tableIndex, std::uint64_t arity,
                           std::uint64_t upvalueCount, std::uint64_t name,
                           std::uint64_t flags) {
    alignTo(8);
    std::uint32_t address = base + static_cast<std::uint32_t>(bytes.size());
    const std::uint64_t fields[5] = {tableIndex, arity, upvalueCount, name, flags};
    for (std::uint64_t field : fields) {
      for (int i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(field >> (8 * i)));
      }
    }
    return address;
  }

  std::uint32_t end() const { return base + static_cast<std::uint32_t>(bytes.size()); }
};

// ------------------------------------------------------------ runtime helpers
// The runtime is emitted as WASM functions over the same boxed value model as
// the x86-64 runtime: a value is a (tag, payload) pair of i64. A helper that
// takes values takes the four i64 of the two operands and returns the two i64 of
// the result, so no operand is ever boxed in linear memory.
struct Runtime {
  Runtime(ModuleBuilder& module, StaticPool& staticPool) : m(module), pool(staticPool) {}

  ModuleBuilder& m;
  StaticPool& pool;

  std::uint32_t tWrite = 0, tExit = 0, tTime = 0, tClock = 0, tValue = 0,
                tStart = 0, tBin = 0,
                tUn = 0, tTruthy = 0, tAlloc = 0, tIter = 0, tFmtInt = 0,
                tNewArray = 0, tNewMap = 0, tArrayPush = 0, tArrayGet = 0,
                tArraySet = 0, tMapFind = 0, tMapSet = 0, tMapGet = 0,
                tMapDelete = 0, tEquals = 0, tToStrDepth = 0, tInvoke = 0,
                tBindMethod = 0;

  std::uint32_t writeImport = 0, exitImport = 0, timeImport = 0,
                clockImport = 0;

  // Static strings the helpers and the builtins share.
  std::uint32_t sNil = 0, sTrue = 0, sFalse = 0, sNan = 0, sInf = 0, sNegInf = 0,
                sZero = 0, sNegZero = 0, pow10Table = 0,
                sDotDot = 0, sDotDotEq = 0, sPrefix = 0, sNewline = 0,
                sSpace = 0, sQuote = 0, sErrorPrefix = 0;
  std::uint32_t sTypeNil = 0, sTypeBool = 0, sTypeInt = 0, sTypeFloat = 0,
                sTypeDouble = 0, sTypeString = 0, sTypeFunction = 0,
                sTypeRange = 0, sTypeError = 0, sTypeArray = 0, sTypeMap = 0,
                sTypePtr = 0, sUnknown = 0;
  std::uint32_t sDivZero = 0, sBadShift = 0, sNonIntegral = 0, sLargeDouble = 0,
                sNotIterable = 0, sNotComparable = 0, sUnsupportedOperands = 0,
                sBadPower = 0, sNotCallable = 0;
  std::uint32_t sFnOpen = 0, sNativeOpen = 0, sAngleClose = 0, sComma = 0,
                sCloseParen = 0;
  std::uint32_t sBadArity = 0, sRangeBounds = 0, sChrRange = 0, sChrSurrogate = 0,
                sOrdSingle = 0, sSqrtNegative = 0, sIntString = 0, sIntNonFinite = 0,
                sRandomHigh = 0, sRandomBounds = 0, sRandomArgc = 0,
                sRandomType = 0, sSeedArgc = 0, sSeedType = 0,
                sIntRange = 0, sFloatString = 0, sAssertFailed = 0, sPanic = 0,
                sNonNull = 0, sLenType = 0;
  std::uint32_t sMessage = 0, sCode = 0, sNoMember = 0, sCannotIndex = 0,
                sStringImmutable = 0, sIndexRange = 0, sKeyValue = 0;
  std::uint32_t sOpenBracket = 0, sCloseBracket = 0, sOpenBrace = 0,
                sCloseBrace = 0, sColonSpace = 0, sEllipsis = 0, sLength = 0,
                sCannotSetField = 0, sCannotSlice = 0, sSliceStartInt = 0,
                sSliceEndInt = 0, sSliceStepInt = 0, sSliceStepZero = 0;
  std::uint32_t sArrayIndexInt = 0, sMapKeyString = 0, sStringIndexInt = 0,
                sArrayRange = 0, sMapMembership = 0, sStringMembership = 0,
                sCannotAssignIndex = 0;

  std::uint32_t randomState = 0, randomSeedFn = 0, randomTwistFn = 0,
                randomNextFn = 0;
  int randomStateSlot = -1;

  std::uint32_t alloc = 0, fail = 0, writeStr = 0, utf8Len = 0, newStr = 0,
                concat = 0, strCmp = 0, fmtInt = 0, fmtDouble = 0, fmtRange = 0,
                toStr = 0, quote = 0, reprStr = 0, truthy = 0, iterStep = 0,
                typeName = 0, fmtFunc = 0, memberGet = 0, memberSet = 0,
                utf8ByteOffset = 0, indexGet = 0, indexSet = 0;
  std::uint32_t newArray = 0, newMap = 0, arrayPush = 0, arrayGet = 0,
                arraySet = 0, mapFind = 0, mapSet = 0, mapGet = 0,
                mapDelete = 0, mapClear = 0, equals = 0, toStrDepth = 0,
                reprDepth = 0, slice = 0, parseIntStr = 0,
                parseDoubleStr = 0;

  std::uint32_t opAdd = 0, opSub = 0, opMul = 0, opDiv = 0, opMod = 0,
                opPow = 0, opBand = 0, opBor = 0, opBxor = 0, opShl = 0,
                opShr = 0, opEq = 0, opLt = 0, opIn = 0;
  std::uint32_t unNeg = 0, unNot = 0, unBitNot = 0;

  void declareTypes() {
    tWrite = m.typeFor({kI32, kI32, kI32}, {kI32});
    tExit = m.typeFor({kI32}, {});
    tTime = m.typeFor({}, {kI64});
    tClock = m.typeFor({}, {kF64});
    tValue = m.typeFor({kI32, kI32, kI32}, {kI64, kI64});
    tStart = m.typeFor({}, {});
    tBin = m.typeFor({kI64, kI64, kI64, kI64}, {kI64, kI64});
    tUn = m.typeFor({kI64, kI64}, {kI64, kI64});
    tTruthy = m.typeFor({kI64, kI64}, {kI32});
    tAlloc = m.typeFor({kI32}, {kI32});
    tIter = m.typeFor({kI64, kI64, kI64, kI64, kI32},
                      {kI64, kI64, kI64, kI64, kI32, kI64, kI64});
    tFmtInt = m.typeFor({kI64}, {kI32});
    tNewArray = m.typeFor({kI64}, {kI64, kI64});
    tNewMap = m.typeFor({}, {kI64, kI64});
    tArrayPush = m.typeFor({kI32, kI64, kI64}, {});
    tArrayGet = m.typeFor({kI32, kI64}, {kI64, kI64});
    tArraySet = m.typeFor({kI32, kI64, kI64, kI64}, {});
    tMapFind = m.typeFor({kI32, kI32}, {kI32});
    tMapSet = m.typeFor({kI32, kI32, kI64, kI64}, {});
    tMapGet = m.typeFor({kI32, kI32}, {kI64, kI64});
    tMapDelete = m.typeFor({kI32, kI32}, {kI32});
    tEquals = m.typeFor({kI64, kI64, kI64, kI64, kI32}, {kI32});
    tToStrDepth = m.typeFor({kI64, kI64, kI32}, {kI32});
    tInvoke = m.typeFor({kI64, kI64, kI32, kI32}, {kI64, kI64});
    tBindMethod = m.typeFor({kI64, kI64, kI64}, {kI64, kI64});
  }

  void addImports() {
    writeImport = m.addImport("oskel", "write", tWrite);
    exitImport = m.addImport("oskel", "exit", tExit);
    timeImport = m.addImport("oskel", "time", tTime);
    clockImport = m.addImport("oskel", "clock", tClock);
  }

  void addStaticStrings() {
    sNil = pool.addString("nil");
    sTrue = pool.addString("true");
    sFalse = pool.addString("false");
    sNan = pool.addString("nan");
    sInf = pool.addString("inf");
    sNegInf = pool.addString("-inf");
    sZero = pool.addString("0");
    sNegZero = pool.addString("-0");
    std::vector<double> powers;
    powers.reserve(633);
    for (int k = -324; k <= 308; ++k) {
      char text[32];
      std::snprintf(text, sizeof text, "1e%d", k);
      powers.push_back(std::strtod(text, nullptr));
    }
    pow10Table = pool.addDoubles(powers);
    sDotDot = pool.addString("..");
    sDotDotEq = pool.addString("..=");
    sPrefix = pool.addString("runtime error: ");
    sNewline = pool.addString("\n");
    sSpace = pool.addString(" ");
    sQuote = pool.addString("\"");
    sErrorPrefix = pool.addString("error(");
    sTypeNil = pool.addString("nil");
    sTypeBool = pool.addString("bool");
    sTypeInt = pool.addString("int");
    sTypeFloat = pool.addString("float");
    sTypeDouble = pool.addString("double");
    sTypeString = pool.addString("string");
    sTypeFunction = pool.addString("function");
    sTypeRange = pool.addString("range");
    sTypeError = pool.addString("error");
    sTypeArray = pool.addString("array");
    sTypeMap = pool.addString("map");
    sTypePtr = pool.addString("ptr");
    sUnknown = pool.addString("unknown");
    sDivZero = pool.addString("division by zero");
    sBadShift = pool.addString("shift count must be between 0 and 63");
    sNonIntegral = pool.addString(
        "the WebAssembly backend cannot format a non-integral double yet (PHASE 9)");
    sLargeDouble = pool.addString(
        "the WebAssembly backend cannot format an out-of-range double yet (PHASE 9)");
    sNotIterable = pool.addString("cannot iterate over this value");
    sNotComparable = pool.addString("cannot compare these values");
    sUnsupportedOperands = pool.addString("unsupported operand types for operator");
    sBadPower = pool.addString("pow: unsupported exponent in the WebAssembly backend");
    sNotCallable = pool.addString("attempt to call a non-function value");
    sFnOpen = pool.addString("<fn ");
    sNativeOpen = pool.addString("<native fn ");
    sAngleClose = pool.addString(">");
    sComma = pool.addString(", ");
    sCloseParen = pool.addString(")");
    sBadArity = pool.addString("wrong number of arguments to a builtin");
    sRangeBounds = pool.addString("range bounds must be integers");
    sChrRange = pool.addString("chr() code point out of range");
    sChrSurrogate = pool.addString("chr() code point is a surrogate");
    sOrdSingle = pool.addString("ord() expects a single character");
    sSqrtNegative = pool.addString("sqrt() of a negative number");
    sIntString = pool.addString("cannot convert this string to int");
    sIntNonFinite = pool.addString("cannot convert a non-finite float to int");
    sIntRange = pool.addString("float is out of the integer range");
    sFloatString = pool.addString("cannot convert this string to double");
    sAssertFailed = pool.addString("assertion failed");
    sPanic = pool.addString("panic");
    sNonNull = pool.addString("unexpected nil (non-null assertion '!!' failed)");
    sLenType = pool.addString("len() is not defined for this value");
    sMessage = pool.addString("message");
    sCode = pool.addString("code");
    sNoMember = pool.addString("no such member on this value");
    sCannotIndex = pool.addString("cannot index a value of this type");
    sStringImmutable = pool.addString("strings are immutable");
    sIndexRange = pool.addString("index out of range");
    sKeyValue = pool.addString(
        "key/value iteration ('for k, v in ...') requires a map");
    sOpenBracket = pool.addString("[");
    sCloseBracket = pool.addString("]");
    sOpenBrace = pool.addString("{");
    sCloseBrace = pool.addString("}");
    sColonSpace = pool.addString(": ");
    sEllipsis = pool.addString("...");
    sLength = pool.addString("length");
    sCannotSetField = pool.addString("cannot set a field on this value");
    sCannotSlice = pool.addString("cannot slice a value of this type");
    sSliceStartInt = pool.addString("slice start must be an integer");
    sSliceEndInt = pool.addString("slice end must be an integer");
    sSliceStepInt = pool.addString("slice step must be an integer");
    sSliceStepZero = pool.addString("slice step must not be zero");
    sArrayIndexInt = pool.addString("array index must be an integer");
    sMapKeyString = pool.addString("map keys must be strings");
    sStringIndexInt = pool.addString("string index must be an integer");
    sArrayRange = pool.addString("array index out of range");
    sMapMembership = pool.addString("map membership requires a string key");
    sStringMembership = pool.addString("string membership requires a string operand");
    sCannotAssignIndex = pool.addString("cannot assign to an index of this value");
    if (randomStateSlot >= 0) {
      sRandomHigh = pool.addString("random() bound must be positive");
      sRandomBounds = pool.addString(
          "random() needs an upper bound greater than the lower bound");
      sRandomArgc = pool.addString("random() expects between 0 and 2 arguments");
      sRandomType = pool.addString("random() expects an integer argument");
      sSeedArgc = pool.addString("seed() expects 1 argument");
      sSeedType = pool.addString("seed() expects an integer argument");
    }
  }

  // (tag, payload) -> bool value on the stack.
  static void emitBoolValue(Func& f, int cond) {
    f.i64c(static_cast<std::int64_t>(kBool));
    if (cond == 0) f.i32c(0);
    f.i64ExtendI32U();
  }

  // Emits a call to `fail` and marks the path unreachable. The message is a
  // static string at `address` (a header; `fail` takes the bytes and a length).
  void emitFailStatic(Func& f, std::uint32_t address) {
    f.i32c(static_cast<std::int32_t>(address + 8));
    f.i32c(static_cast<std::int32_t>(strlenAt(address)));
    f.call(fail);
    f.unreachable();
  }

  // Length of a static string blob, recorded when the pool adds it. The pool
  // does not store lengths separately, so recompute from the blob bytes.
  std::uint32_t strlenAt(std::uint32_t address) const {
    std::uint64_t length = 0;
    for (int i = 0; i < 8; ++i) {
      length |= static_cast<std::uint64_t>(pool.bytes[address - pool.base + i]) << (8 * i);
    }
    return static_cast<std::uint32_t>(length);
  }

  std::uint32_t emitAlloc();
  std::uint32_t emitRandomSeed();
  std::uint32_t emitRandomTwist();
  std::uint32_t emitRandomNext();
  std::uint32_t emitRandomState();
  std::uint32_t emitFail();
  std::uint32_t emitWriteStr();
  std::uint32_t emitUtf8Len();
  std::uint32_t emitNewStr();
  std::uint32_t emitConcat();
  std::uint32_t emitStrCmp();
  std::uint32_t emitFmtInt();
  std::uint32_t emitFmtDouble();
  std::uint32_t emitFmtRange();
  std::uint32_t emitToStr();
  std::uint32_t emitQuote();
  std::uint32_t emitReprStr();
  std::uint32_t emitTruthy();
  std::uint32_t emitIterStep();
  std::uint32_t emitTypeName();
  std::uint32_t emitFmtFunc();
  std::uint32_t emitNewArray();
  std::uint32_t emitNewMap();
  std::uint32_t emitArrayPush();
  std::uint32_t emitArrayGet();
  std::uint32_t emitArraySet();
  std::uint32_t emitMapFind();
  std::uint32_t emitMapSet();
  std::uint32_t emitMapGet();
  std::uint32_t emitMapDelete();
  std::uint32_t emitMapClear();
  std::uint32_t emitEquals();
  std::uint32_t emitToStrDepthBody(std::uint32_t index);
  std::uint32_t emitReprDepthBody(std::uint32_t index);
  std::uint32_t emitIndexGet();
  std::uint32_t emitIndexSet();
  std::uint32_t emitSlice();
  std::uint32_t emitParseIntStr();
  std::uint32_t emitParseDoubleStr();
  std::uint32_t emitBinary(int op, std::uint32_t failAddress, const char* failText);
  std::uint32_t emitUnary(int op);
  std::uint32_t emitCompare(int op);
  std::uint32_t emitIn();
  std::uint32_t emitBuiltin(const std::string& name);
  std::uint32_t emitMemberGet();
  std::uint32_t emitMemberSet();
  std::uint32_t emitUtf8ByteOffset();
  std::uint32_t emitTrapBuiltin(const std::string& name);
  // Stack helpers shared by the operator emitters.
  void emitIsNumeric(Func& f, std::uint32_t tagLocal);
  void emitRank(Func& f, std::uint32_t tagLocal);
  void emitToF64(Func& f, std::uint32_t tagLocal, std::uint32_t payloadLocal);
  void emitCore();
};

// ------------------------------------------------------------------ helpers
std::uint32_t Runtime::emitAlloc() {
  std::uint32_t index = m.reserveFunction(tAlloc);
  Func f(m, index, {kI32});
  std::uint32_t cur = f.addLocal(kI32), need = f.addLocal(kI32), size = f.addLocal(kI32);
  f.gget(0);
  f.lset(cur);
  f.lget(cur);
  f.lget(0);
  f.i32Add();
  f.lset(need);
  f.memorySize();
  f.i32c(65536);
  f.i32Mul();
  f.lset(size);
  f.lget(need);
  f.lget(size);
  f.i32GtU();
  f.if_();
  f.lget(need);
  f.lget(size);
  f.i32Sub();
  f.i32c(65535);
  f.i32Add();
  f.i32c(65536);
  f.i32DivU();
  f.memoryGrow();
  f.drop();
  f.end();
  f.lget(need);
  f.gset(0);
  f.lget(cur);
  return index;
}

std::uint32_t Runtime::emitWriteStr() {
  std::uint32_t type = m.typeFor({kI32, kI32}, {});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32, kI32});
  f.lget(0);
  f.lget(1);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i64Load(0);
  f.i32WrapI64();
  f.call(writeImport);
  f.drop();
  return index;
}

std::uint32_t Runtime::emitFail() {
  std::uint32_t type = m.typeFor({kI32, kI32}, {});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32, kI32});
  f.i32c(2);
  f.i32c(static_cast<std::int32_t>(sPrefix));
  f.call(writeStr);
  f.i32c(2);
  f.lget(0);
  f.lget(1);
  f.call(writeImport);
  f.drop();
  f.i32c(2);
  f.i32c(static_cast<std::int32_t>(sNewline));
  f.call(writeStr);
  f.i32c(70);
  f.call(exitImport);
  f.unreachable();
  return index;
}

std::uint32_t Runtime::emitUtf8Len() {
  std::uint32_t type = m.typeFor({kI32}, {kI64});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32});
  std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64), count = f.addLocal(kI64),
                byte = f.addLocal(kI32);
  f.lget(0);
  f.i64Load(0);
  f.lset(len);
  f.i64c(0);
  f.lset(i);
  f.i64c(0);
  f.lset(count);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(len);
  f.i64GeU();
  f.brIf(1);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(i);
  f.i32WrapI64();
  f.i32Add();
  f.i32Load8u(0);
  f.lset(byte);
  f.lget(byte);
  f.i32c(0xC0);
  f.i32And();
  f.i32c(0x80);
  f.i32Ne();
  f.if_();
  f.lget(count);
  f.i64c(1);
  f.i64Add();
  f.lset(count);
  f.end();
  f.lget(i);
  f.i64c(1);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.lget(count);
  return index;
}

std::uint32_t Runtime::emitNewStr() {
  std::uint32_t index = m.reserveFunction(tAlloc);
  Func f(m, index, {kI32});
  std::uint32_t address = f.addLocal(kI32);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.call(alloc);
  f.lset(address);
  f.lget(address);
  f.lget(0);
  f.i64ExtendI32U();
  f.i64Store(0);
  f.lget(address);
  return index;
}

std::uint32_t Runtime::emitConcat() {
  std::uint32_t type = m.typeFor({kI32, kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32, kI32});
  std::uint32_t la = f.addLocal(kI64), lb = f.addLocal(kI64), total = f.addLocal(kI32),
                out = f.addLocal(kI32);
  f.lget(0);
  f.i64Load(0);
  f.lset(la);
  f.lget(1);
  f.i64Load(0);
  f.lset(lb);
  f.lget(la);
  f.lget(lb);
  f.i64Add();
  f.i32WrapI64();
  f.lset(total);
  f.lget(total);
  f.call(newStr);
  f.lset(out);
  f.lget(out);
  f.i32c(8);
  f.i32Add();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(la);
  f.i32WrapI64();
  f.memoryCopy();
  f.lget(out);
  f.i32c(8);
  f.i32Add();
  f.lget(la);
  f.i32WrapI64();
  f.i32Add();
  f.lget(1);
  f.i32c(8);
  f.i32Add();
  f.lget(lb);
  f.i32WrapI64();
  f.memoryCopy();
  f.lget(out);
  return index;
}

std::uint32_t Runtime::emitStrCmp() {
  std::uint32_t type = m.typeFor({kI32, kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32, kI32});
  std::uint32_t la = f.addLocal(kI64), lb = f.addLocal(kI64), n = f.addLocal(kI64),
                i = f.addLocal(kI64), ca = f.addLocal(kI32), cb = f.addLocal(kI32);
  f.lget(0);
  f.i64Load(0);
  f.lset(la);
  f.lget(1);
  f.i64Load(0);
  f.lset(lb);
  f.lget(la);
  f.lget(lb);
  f.lget(la);
  f.lget(lb);
  f.i64LtU();
  f.select();
  f.lset(n);
  f.i64c(0);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(n);
  f.i64GeU();
  f.brIf(1);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(i);
  f.i32WrapI64();
  f.i32Add();
  f.i32Load8u(0);
  f.lset(ca);
  f.lget(1);
  f.i32c(8);
  f.i32Add();
  f.lget(i);
  f.i32WrapI64();
  f.i32Add();
  f.i32Load8u(0);
  f.lset(cb);
  f.lget(ca);
  f.lget(cb);
  f.i32LtU();
  f.if_();
  f.i32c(-1);
  f.ret();
  f.end();
  f.lget(ca);
  f.lget(cb);
  f.i32GtU();
  f.if_();
  f.i32c(1);
  f.ret();
  f.end();
  f.lget(i);
  f.i64c(1);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.lget(la);
  f.lget(lb);
  f.i64LtU();
  f.if_();
  f.i32c(-1);
  f.ret();
  f.end();
  f.lget(la);
  f.lget(lb);
  f.i64GtU();
  f.if_();
  f.i32c(1);
  f.ret();
  f.end();
  f.i32c(0);
  return index;
}

std::uint32_t Runtime::emitFmtInt() {
  std::uint32_t index = m.reserveFunction(tFmtInt);
  Func f(m, index, {kI64});
  std::uint32_t neg = f.addLocal(kI32), u = f.addLocal(kI64), buf = f.addLocal(kI32),
                end = f.addLocal(kI32), p = f.addLocal(kI32), digit = f.addLocal(kI64),
                len = f.addLocal(kI32), out = f.addLocal(kI32);
  f.lget(0);
  f.i64c(0);
  f.i64LtS();
  f.lset(neg);
  // u = neg ? 0 - v : v (wrapping; handles INT64_MIN)
  f.i64c(0);
  f.lget(0);
  f.i64Sub();
  f.lget(0);
  f.lget(0);
  f.i64c(0);
  f.i64LtS();
  f.select();
  f.lset(u);
  f.i32c(24);
  f.call(alloc);
  f.lset(buf);
  f.lget(buf);
  f.i32c(24);
  f.i32Add();
  f.lset(end);
  f.lget(end);
  f.lset(p);
  f.block();
  f.loop();
  f.lget(u);
  f.i64c(10);
  f.i64RemU();
  f.lset(digit);
  f.lget(u);
  f.i64c(10);
  f.i64DivU();
  f.lset(u);
  f.lget(p);
  f.i32c(1);
  f.i32Sub();
  f.lset(p);
  f.lget(p);
  f.lget(digit);
  f.i32WrapI64();
  f.i32c(48);
  f.i32Add();
  f.i32Store8(0);
  f.lget(u);
  f.i64c(0);
  f.i64Ne();
  f.brIf(0);
  f.end();
  f.end();
  f.lget(neg);
  f.if_();
  f.lget(p);
  f.i32c(1);
  f.i32Sub();
  f.lset(p);
  f.lget(p);
  f.i32c(45);
  f.i32Store8(0);
  f.end();
  f.lget(end);
  f.lget(p);
  f.i32Sub();
  f.lset(len);
  f.lget(len);
  f.call(newStr);
  f.lset(out);
  f.lget(out);
  f.i32c(8);
  f.i32Add();
  f.lget(p);
  f.lget(len);
  f.memoryCopy();
  f.lget(out);
  return index;
}

std::uint32_t Runtime::emitFmtDouble() {
  std::uint32_t index = m.reserveFunction(tFmtInt);
  Func f(m, index, {kI64});
  std::uint32_t bits = f.addLocal(kI64), d = f.addLocal(kF64),
                sign = f.addLocal(kI32), e10 = f.addLocal(kI32),
                m10 = f.addLocal(kF64), digits = f.addLocal(kI64),
                count = f.addLocal(kI32), p = f.addLocal(kI32),
                q = f.addLocal(kI64), qq = f.addLocal(kI64),
                pow10 = f.addLocal(kI64), rem = f.addLocal(kI64),
                half = f.addLocal(kI64), tenP = f.addLocal(kI64),
                i = f.addLocal(kI32), bestP = f.addLocal(kI32),
                bestQ = f.addLocal(kI64), bestE = f.addLocal(kI32),
                candE = f.addLocal(kI32), cand = f.addLocal(kI32),
                pos = f.addLocal(kI32), outFixed = f.addLocal(kI32),
                outSci = f.addLocal(kI32), lenFixed = f.addLocal(kI32),
                lenSci = f.addLocal(kI32), exp = f.addLocal(kI32),
                digit = f.addLocal(kI64), noSign = f.addLocal(kI32);

  auto put = [&](std::uint32_t str, std::uint32_t at, int ch) {
    f.lget(str);
    f.i32c(8);
    f.i32Add();
    f.lget(at);
    f.i32Add();
    f.i32c(ch);
    f.i32Store8(0);
  };
  auto storeLen = [&](std::uint32_t str, std::uint32_t len) {
    f.lget(str);
    f.lget(len);
    f.i64ExtendI32U();
    f.i64Store(0);
  };
  // Writes the fixed-point form of `q` (p digits, first digit at 10^e) into
  // `str`, prefixing `sign` (0 or 1) bytes. Leaves the length in `len`.
  auto emitFixed = [&](std::uint32_t str, std::uint32_t qLocal,
                       std::uint32_t pLocal, std::uint32_t eLocal,
                       std::uint32_t signLocal, std::uint32_t lenLocal) {
    f.lget(signLocal);
    f.if_();
    f.i32c(0);
    f.lset(i);
    put(str, i, 45);
    f.end();
    f.lget(qLocal);
    f.lset(qq);
    f.lget(pLocal);
    f.i32c(1);
    f.i32Sub();
    f.lset(pos);
    f.block();
    f.loop();
    f.lget(pos);
    f.i32c(0);
    f.i32LtS();
    f.brIf(1);
    f.lget(qq);
    f.i64c(10);
    f.i64RemU();
    f.lset(digit);
    f.lget(qq);
    f.i64c(10);
    f.i64DivU();
    f.lset(qq);
    f.lget(eLocal);
    f.i32c(0);
    f.i32GeS();
    f.ifResult(kI32);
    f.lget(pos);
    f.lget(eLocal);
    f.i32LeS();
    f.ifResult(kI32);
    f.lget(pos);
    f.else_();
    f.lget(pos);
    f.i32c(1);
    f.i32Add();
    f.end();
    f.else_();
    f.lget(eLocal);
    f.i32c(-1);
    f.i32Mul();
    f.i32c(1);
    f.i32Sub();
    f.i32c(2);
    f.i32Add();
    f.lget(pos);
    f.i32Add();
    f.end();
    f.lget(signLocal);
    f.i32Add();
    f.lset(i);
    f.lget(str);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.lget(digit);
    f.i32WrapI64();
    f.i32c(48);
    f.i32Add();
    f.i32Store8(0);
    f.lget(pos);
    f.i32c(1);
    f.i32Sub();
    f.lset(pos);
    f.br(0);
    f.end();
    f.end();
    f.lget(eLocal);
    f.i32c(0);
    f.i32GeS();
    f.if_();
    f.lget(pLocal);
    f.lget(eLocal);
    f.i32LeS();
    f.if_();
    f.lget(pLocal);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(eLocal);
    f.i32GtS();
    f.brIf(1);
    f.lget(str);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.lget(signLocal);
    f.i32Add();
    f.i32c(48);
    f.i32Store8(0);
    f.lget(i);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.end();
    f.lget(pLocal);
    f.lget(eLocal);
    f.i32c(1);
    f.i32Add();
    f.i32GtS();
    f.if_();
    f.lget(eLocal);
    f.i32c(1);
    f.i32Add();
    f.lget(signLocal);
    f.i32Add();
    f.lset(i);
    put(str, i, 46);
    f.end();
    f.else_();
    put(str, signLocal, 48);
    f.lget(signLocal);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    put(str, i, 46);
    f.i32c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(eLocal);
    f.i32c(-1);
    f.i32Mul();
    f.i32c(1);
    f.i32Sub();
    f.i32GeS();
    f.brIf(1);
    f.lget(signLocal);
    f.i32c(2);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.lset(pos);
    put(str, pos, 48);
    f.lget(i);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.end();
    f.lget(eLocal);
    f.i32c(0);
    f.i32GeS();
    f.ifResult(kI32);
    f.lget(pLocal);
    f.lget(eLocal);
    f.i32c(1);
    f.i32Add();
    f.i32GtS();
    f.ifResult(kI32);
    f.lget(pLocal);
    f.i32c(1);
    f.i32Add();
    f.else_();
    f.lget(eLocal);
    f.i32c(1);
    f.i32Add();
    f.end();
    f.else_();
    f.lget(eLocal);
    f.i32c(-1);
    f.i32Mul();
    f.i32c(1);
    f.i32Sub();
    f.i32c(2);
    f.i32Add();
    f.lget(pLocal);
    f.i32Add();
    f.end();
    f.lget(signLocal);
    f.i32Add();
    f.lset(lenLocal);
  };
  // Writes the scientific form `d.ddde±XX` (at least two exponent digits).
  auto emitSci = [&](std::uint32_t str, std::uint32_t qLocal,
                     std::uint32_t pLocal, std::uint32_t eLocal,
                     std::uint32_t signLocal, std::uint32_t lenLocal) {
    f.lget(signLocal);
    f.if_();
    f.i32c(0);
    f.lset(i);
    put(str, i, 45);
    f.end();
    f.lget(qLocal);
    f.lset(qq);
    f.lget(pLocal);
    f.i32c(1);
    f.i32Sub();
    f.lset(pos);
    f.block();
    f.loop();
    f.lget(pos);
    f.i32c(0);
    f.i32LtS();
    f.brIf(1);
    f.lget(qq);
    f.i64c(10);
    f.i64RemU();
    f.lset(digit);
    f.lget(qq);
    f.i64c(10);
    f.i64DivU();
    f.lset(qq);
    f.lget(pos);
    f.i32c(0);
    f.i32Eq();
    f.ifResult(kI32);
    f.i32c(0);
    f.else_();
    f.lget(pos);
    f.i32c(1);
    f.i32Add();
    f.end();
    f.lget(signLocal);
    f.i32Add();
    f.lset(i);
    f.lget(str);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.lget(digit);
    f.i32WrapI64();
    f.i32c(48);
    f.i32Add();
    f.i32Store8(0);
    f.lget(pos);
    f.i32c(1);
    f.i32Sub();
    f.lset(pos);
    f.br(0);
    f.end();
    f.end();
    f.lget(pLocal);
    f.i32c(1);
    f.i32GtS();
    f.if_();
    f.lget(signLocal);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    put(str, i, 46);
    f.end();
    f.lget(signLocal);
    f.lget(pLocal);
    f.i32c(1);
    f.i32GtS();
    f.ifResult(kI32);
    f.lget(pLocal);
    f.i32c(1);
    f.i32Add();
    f.else_();
    f.i32c(1);
    f.end();
    f.i32Add();
    f.lset(i);
    put(str, i, 101);
    f.lget(i);
    f.i32c(1);
    f.i32Add();
    f.lset(pos);
    f.lget(eLocal);
    f.i32c(0);
    f.i32LtS();
    f.if_();
    put(str, pos, 45);
    f.lget(eLocal);
    f.i32c(-1);
    f.i32Mul();
    f.lset(exp);
    f.else_();
    put(str, pos, 43);
    f.lget(eLocal);
    f.lset(exp);
    f.end();
    f.lget(exp);
    f.i32c(10);
    f.i32LtS();
    f.if_();
    f.lget(pos);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    put(str, i, 48);
    f.lget(pos);
    f.i32c(2);
    f.i32Add();
    f.lset(i);
    f.lget(str);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.lget(exp);
    f.i32c(48);
    f.i32Add();
    f.i32Store8(0);
    f.lget(pos);
    f.i32c(3);
    f.i32Add();
    f.lset(lenLocal);
    f.else_();
    f.lget(exp);
    f.i32c(100);
    f.i32LtS();
    f.if_();
    f.lget(pos);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    f.lget(str);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.lget(exp);
    f.i32c(10);
    f.i32DivS();
    f.i32c(48);
    f.i32Add();
    f.i32Store8(0);
    f.lget(pos);
    f.i32c(2);
    f.i32Add();
    f.lset(i);
    f.lget(str);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.lget(exp);
    f.i32c(10);
    f.i32RemS();
    f.i32c(48);
    f.i32Add();
    f.i32Store8(0);
    f.lget(pos);
    f.i32c(3);
    f.i32Add();
    f.lset(lenLocal);
    f.else_();
    f.lget(pos);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    f.lget(str);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.lget(exp);
    f.i32c(100);
    f.i32DivS();
    f.i32c(48);
    f.i32Add();
    f.i32Store8(0);
    f.lget(pos);
    f.i32c(2);
    f.i32Add();
    f.lset(i);
    f.lget(str);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.lget(exp);
    f.i32c(10);
    f.i32DivS();
    f.i32c(10);
    f.i32RemS();
    f.i32c(48);
    f.i32Add();
    f.i32Store8(0);
    f.lget(pos);
    f.i32c(3);
    f.i32Add();
    f.lset(i);
    f.lget(str);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.lget(exp);
    f.i32c(10);
    f.i32RemS();
    f.i32c(48);
    f.i32Add();
    f.i32Store8(0);
    f.lget(pos);
    f.i32c(4);
    f.i32Add();
    f.lset(lenLocal);
    f.end();
    f.end();
  };

  f.lget(0);
  f.lset(bits);
  f.lget(bits);
  f.f64ReinterpretI64();
  f.lset(d);
  f.lget(bits);
  f.i64c(63);
  f.i64ShrU();
  f.i32WrapI64();
  f.lset(sign);
  f.lget(d);
  f.lget(d);
  f.f64Ne();
  f.if_();
  f.i32c(static_cast<std::int32_t>(sNan));
  f.ret();
  f.end();
  f.lget(bits);
  f.i64c(static_cast<std::int64_t>(0x7FF0000000000000ULL));
  f.i64And();
  f.i64c(static_cast<std::int64_t>(0x7FF0000000000000ULL));
  f.i64Eq();
  f.if_();
  f.lget(sign);
  f.if_();
  f.i32c(static_cast<std::int32_t>(sNegInf));
  f.ret();
  f.else_();
  f.i32c(static_cast<std::int32_t>(sInf));
  f.ret();
  f.end();
  f.end();
  f.lget(bits);
  f.i64c(static_cast<std::int64_t>(0x7FFFFFFFFFFFFFFFULL));
  f.i64And();
  f.i64Eqz();
  f.if_();
  f.lget(sign);
  f.if_();
  f.i32c(static_cast<std::int32_t>(sNegZero));
  f.ret();
  f.else_();
  f.i32c(static_cast<std::int32_t>(sZero));
  f.ret();
  f.end();
  f.end();

  f.lget(d);
  f.f64Abs();
  f.lset(m10);
  f.i32c(0);
  f.lset(e10);
  f.block();
  f.loop();
  f.lget(m10);
  f.f64c(10.0);
  f.f64Ge();
  f.i32Eqz();
  f.brIf(1);
  f.lget(m10);
  f.f64c(10.0);
  f.f64Div();
  f.lset(m10);
  f.lget(e10);
  f.i32c(1);
  f.i32Add();
  f.lset(e10);
  f.br(0);
  f.end();
  f.end();
  f.block();
  f.loop();
  f.lget(m10);
  f.f64c(1.0);
  f.f64Ge();
  f.brIf(1);
  f.lget(m10);
  f.f64c(10.0);
  f.f64Mul();
  f.lset(m10);
  f.lget(e10);
  f.i32c(1);
  f.i32Sub();
  f.lset(e10);
  f.br(0);
  f.end();
  f.end();
  f.lget(e10);
  f.i32c(-323);
  f.i32GeS();
  f.if_();
  f.lget(d);
  f.f64Abs();
  f.i32c(static_cast<std::int32_t>(pow10Table));
  f.lget(e10);
  f.i32c(324);
  f.i32Add();
  f.i32c(3);
  f.i32Shl();
  f.i32Add();
  f.f64Load(0);
  f.f64Div();
  f.lset(m10);
  f.end();

  f.i64c(0);
  f.lset(digits);
  f.i32c(0);
  f.lset(count);
  f.block();
  f.loop();
  f.lget(count);
  f.i32c(18);
  f.i32GeS();
  f.brIf(1);
  f.lget(m10);
  f.f64Floor();
  f.i64TruncSatF64S();
  f.lset(digit);
  f.lget(digits);
  f.i64c(10);
  f.i64Mul();
  f.lget(digit);
  f.i64Add();
  f.lset(digits);
  f.lget(m10);
  f.lget(digit);
  f.f64ConvertI64S();
  f.f64Sub();
  f.f64c(10.0);
  f.f64Mul();
  f.lset(m10);
  f.lget(count);
  f.i32c(1);
  f.i32Add();
  f.lset(count);
  f.lget(m10);
  f.f64c(0.0);
  f.f64Eq();
  f.brIf(1);
  f.br(0);
  f.end();
  f.end();
  f.lget(count);
  f.i32c(18);
  f.i32Eq();
  f.if_();
  f.lget(digits);
  f.i64c(10);
  f.i64RemU();
  f.lset(rem);
  f.lget(digits);
  f.i64c(10);
  f.i64DivU();
  f.lset(digits);
  f.lget(count);
  f.i32c(1);
  f.i32Sub();
  f.lset(count);
  f.lget(rem);
  f.i64c(5);
  f.i64GeU();
  f.if_();
  f.lget(digits);
  f.i64c(1);
  f.i64Add();
  f.lset(digits);
  f.lget(digits);
  f.i64c(100000000000000000LL);
  f.i64GeU();
  f.if_();
  f.lget(digits);
  f.i64c(10);
  f.i64DivU();
  f.lset(digits);
  f.lget(e10);
  f.i32c(1);
  f.i32Add();
  f.lset(e10);
  f.end();
  f.end();
  f.end();
  f.block();
  f.loop();
  f.lget(count);
  f.i32c(1);
  f.i32LeS();
  f.brIf(1);
  f.lget(digits);
  f.i64c(10);
  f.i64RemU();
  f.i64Eqz();
  f.i32Eqz();
  f.brIf(1);
  f.lget(digits);
  f.i64c(10);
  f.i64DivU();
  f.lset(digits);
  f.lget(count);
  f.i32c(1);
  f.i32Sub();
  f.lset(count);
  f.br(0);
  f.end();
  f.end();

  f.lget(count);
  f.lset(bestP);
  f.lget(digits);
  f.lset(bestQ);
  f.lget(e10);
  f.lset(bestE);
  f.lget(count);
  f.i32c(1);
  f.i32GtS();
  f.if_();
  f.i32c(1);
  f.lset(p);
  f.block();
  f.loop();
  f.lget(p);
  f.lget(count);
  f.i32GtS();
  f.brIf(1);
  f.i64c(1);
  f.lset(pow10);
  f.i32c(0);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(count);
  f.lget(p);
  f.i32Sub();
  f.i32GeS();
  f.brIf(1);
  f.lget(pow10);
  f.i64c(10);
  f.i64Mul();
  f.lset(pow10);
  f.lget(i);
  f.i32c(1);
  f.i32Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.lget(digits);
  f.lget(pow10);
  f.i64DivU();
  f.lset(q);
  f.lget(digits);
  f.lget(pow10);
  f.i64RemU();
  f.lset(rem);
  f.lget(pow10);
  f.i64c(2);
  f.i64DivU();
  f.lset(half);
  f.lget(pow10);
  f.i64c(1);
  f.i64GtU();
  f.lget(rem);
  f.lget(half);
  f.i64GtU();
  f.lget(rem);
  f.lget(half);
  f.i64Eq();
  f.lget(q);
  f.i64c(2);
  f.i64RemU();
  f.i64c(1);
  f.i64Eq();
  f.i32And();
  f.i32Or();
  f.i32And();
  f.if_();
  f.lget(q);
  f.i64c(1);
  f.i64Add();
  f.lset(q);
  f.end();
  f.i64c(1);
  f.lset(tenP);
  f.i32c(0);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(p);
  f.i32GeS();
  f.brIf(1);
  f.lget(tenP);
  f.i64c(10);
  f.i64Mul();
  f.lset(tenP);
  f.lget(i);
  f.i32c(1);
  f.i32Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.lget(e10);
  f.lset(candE);
  f.lget(q);
  f.lget(tenP);
  f.i64GeU();
  f.if_();
  f.lget(q);
  f.i64c(10);
  f.i64DivU();
  f.lset(q);
  f.lget(candE);
  f.i32c(1);
  f.i32Add();
  f.lset(candE);
  f.end();
  f.lget(cand);
  f.i32Eqz();
  f.if_();
  f.i32c(64);
  f.call(newStr);
  f.lset(cand);
  f.end();
  f.i32c(0);
  f.lset(noSign);
  f.lget(candE);
  f.i32c(-20);
  f.i32GeS();
  f.lget(candE);
  f.i32c(17);
  f.i32LeS();
  f.i32And();
  f.if_();
  emitFixed(cand, q, p, candE, noSign, pos);
  f.else_();
  emitSci(cand, q, p, candE, noSign, pos);
  f.end();
  storeLen(cand, pos);
  f.lget(cand);
  f.call(parseDoubleStr);
  f.i64ReinterpretF64();
  f.lget(bits);
  f.i64Eq();
  f.if_();
  f.lget(p);
  f.lset(bestP);
  f.lget(q);
  f.lset(bestQ);
  f.lget(candE);
  f.lset(bestE);
  f.br(2);
  f.end();
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.br(0);
  f.end();
  f.end();
  f.end();
  f.block();
  f.loop();
  f.lget(bestP);
  f.i32c(1);
  f.i32LeS();
  f.brIf(1);
  f.lget(bestQ);
  f.i64c(10);
  f.i64RemU();
  f.i64Eqz();
  f.i32Eqz();
  f.brIf(1);
  f.lget(bestQ);
  f.i64c(10);
  f.i64DivU();
  f.lset(bestQ);
  f.lget(bestP);
  f.i32c(1);
  f.i32Sub();
  f.lset(bestP);
  f.br(0);
  f.end();
  f.end();

  f.i32c(64);
  f.call(newStr);
  f.lset(outFixed);
  f.i32c(64);
  f.call(newStr);
  f.lset(outSci);
  emitFixed(outFixed, bestQ, bestP, bestE, sign, lenFixed);
  storeLen(outFixed, lenFixed);
  emitSci(outSci, bestQ, bestP, bestE, sign, lenSci);
  storeLen(outSci, lenSci);
  f.lget(lenFixed);
  f.lget(lenSci);
  f.i32LeS();
  f.ifResult(kI32);
  f.lget(outFixed);
  f.else_();
  f.lget(outSci);
  f.end();
  return index;
}

std::uint32_t Runtime::emitFmtFunc() {
  std::uint32_t type = m.typeFor({kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32});
  std::uint32_t open = f.addLocal(kI32), name = f.addLocal(kI32);
  f.i32c(static_cast<std::int32_t>(sNativeOpen));
  f.i32c(static_cast<std::int32_t>(sFnOpen));
  f.lget(0);
  f.i64Load(32);
  f.i32WrapI64();
  f.i32c(1);
  f.i32And();
  f.select();
  f.lset(open);
  f.lget(0);
  f.i64Load(24);
  f.i32WrapI64();
  f.lset(name);
  f.lget(open);
  f.lget(name);
  f.call(concat);
  f.i32c(static_cast<std::int32_t>(sAngleClose));
  f.call(concat);
  return index;
}

std::uint32_t Runtime::emitFmtRange() {
  std::uint32_t type = m.typeFor({kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32});
  std::uint32_t first = f.addLocal(kI32), sep = f.addLocal(kI32);
  f.lget(0);
  f.i64Load(0);
  f.call(fmtInt);
  f.lset(first);
  f.i32c(static_cast<std::int32_t>(sDotDotEq));
  f.i32c(static_cast<std::int32_t>(sDotDot));
  f.lget(0);
  f.i64Load(16);
  f.i32WrapI64();
  f.select();
  f.lset(sep);
  f.lget(first);
  f.lget(sep);
  f.call(concat);
  f.lget(0);
  f.i64Load(8);
  f.call(fmtInt);
  f.call(concat);
  return index;
}

std::uint32_t Runtime::emitToStr() {
  std::uint32_t index = m.reserveFunction(m.typeFor({kI64, kI64}, {kI32}));
  Func f(m, index, {kI64, kI64});
  f.lget(0);
  f.lget(1);
  f.i32c(0);
  f.call(toStrDepth);
  return index;
}

std::uint32_t Runtime::emitQuote() {
  std::uint32_t type = m.typeFor({kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32});
  std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64), raw = f.addLocal(kI32),
                w = f.addLocal(kI32), c = f.addLocal(kI32), out = f.addLocal(kI32),
                digit = f.addLocal(kI32);

  // Each byte escapes to at most four characters, plus the two quotes.
  f.lget(0);
  f.i64Load(0);
  f.i32WrapI64();
  f.i32c(4);
  f.i32Mul();
  f.i32c(2);
  f.i32Add();
  f.call(alloc);
  f.lset(raw);
  f.lget(raw);
  f.i32c(34);
  f.i32Store8(0);
  f.i32c(1);
  f.lset(w);
  f.i64c(0);
  f.lset(i);
  f.lget(0);
  f.i64Load(0);
  f.lset(len);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(len);
  f.i64GeU();
  f.brIf(1);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(i);
  f.i32WrapI64();
  f.i32Add();
  f.i32Load8u(0);
  f.lset(c);
  f.lget(c);
  f.i32c(34);
  f.i32Eq();
  f.if_();
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(92);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(34);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.else_();
  f.lget(c);
  f.i32c(92);
  f.i32Eq();
  f.if_();
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(92);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(92);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.else_();
  f.lget(c);
  f.i32c(10);
  f.i32Eq();
  f.if_();
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(92);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(110);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.else_();
  f.lget(c);
  f.i32c(9);
  f.i32Eq();
  f.if_();
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(92);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(116);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.else_();
  f.lget(c);
  f.i32c(13);
  f.i32Eq();
  f.if_();
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(92);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(114);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.else_();
  f.lget(c);
  f.i32c(0x20);
  f.i32LtU();
  f.if_();
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(92);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(120);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.lget(c);
  f.i32c(4);
  f.i32ShrU();
  f.i32c(15);
  f.i32And();
  f.lset(digit);
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.lget(digit);
  f.i32c(10);
  f.i32LtU();
  f.ifResult(kI32);
  f.lget(digit);
  f.i32c(48);
  f.i32Add();
  f.else_();
  f.lget(digit);
  f.i32c(87);
  f.i32Add();
  f.end();
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.lget(c);
  f.i32c(15);
  f.i32And();
  f.lset(digit);
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.lget(digit);
  f.i32c(10);
  f.i32LtU();
  f.ifResult(kI32);
  f.lget(digit);
  f.i32c(48);
  f.i32Add();
  f.else_();
  f.lget(digit);
  f.i32c(87);
  f.i32Add();
  f.end();
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.else_();
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.lget(c);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.end();
  f.end();
  f.end();
  f.end();
  f.end();
  f.end();
  f.lget(i);
  f.i64c(1);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.lget(raw);
  f.lget(w);
  f.i32Add();
  f.i32c(34);
  f.i32Store8(0);
  f.lget(w);
  f.i32c(1);
  f.i32Add();
  f.lset(w);
  f.lget(w);
  f.call(newStr);
  f.lset(out);
  f.lget(out);
  f.i32c(8);
  f.i32Add();
  f.lget(raw);
  f.lget(w);
  f.memoryCopy();
  f.lget(out);
  return index;
}

std::uint32_t Runtime::emitReprStr() {
  std::uint32_t index = m.reserveFunction(m.typeFor({kI64, kI64}, {kI32}));
  Func f(m, index, {kI64, kI64});
  f.lget(0);
  f.lget(1);
  f.i32c(0);
  f.call(reprDepth);
  return index;
}

std::uint32_t Runtime::emitTruthy() {
  std::uint32_t index = m.reserveFunction(tTruthy);
  Func f(m, index, {kI64, kI64});
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kBool));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.i32c(0);
  f.i32Ne();
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kNil));
  f.i64Eq();
  f.if_();
  f.i32c(0);
  f.ret();
  f.end();
  f.i32c(1);
  return index;
}

std::uint32_t Runtime::emitTypeName() {
  std::uint32_t type = m.typeFor({kI64}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI64});
  struct Entry {
    std::uint64_t tag;
    std::uint32_t address;
  };
  const Entry entries[] = {
      {kNil, sTypeNil},       {kBool, sTypeBool},     {kInt, sTypeInt},
      {kFloat, sTypeFloat},   {kDouble, sTypeDouble}, {kStr, sTypeString},
      {kFunc, sTypeFunction}, {kRange, sTypeRange},   {kError, sTypeError},
      {static_cast<std::uint64_t>(AotTag::Array), sTypeArray},
      {static_cast<std::uint64_t>(AotTag::Map), sTypeMap},
      {static_cast<std::uint64_t>(AotTag::Ptr), sTypePtr},
  };
  for (const Entry& entry : entries) {
    f.lget(0);
    f.i64c(static_cast<std::int64_t>(entry.tag));
    f.i64Eq();
    f.if_();
    f.i32c(static_cast<std::int32_t>(entry.address));
    f.ret();
    f.end();
  }
  f.i32c(static_cast<std::int32_t>(sUnknown));
  return index;
}

std::uint32_t Runtime::emitIterStep() {
  std::uint32_t index = m.reserveFunction(tIter);
  Func f(m, index, {kI64, kI64, kI64, kI64, kI32});
  std::uint32_t addr = f.addLocal(kI32), start = f.addLocal(kI64), end = f.addLocal(kI64),
                incl = f.addLocal(kI32), asc = f.addLocal(kI32), cur = f.addLocal(kI64),
                len = f.addLocal(kI64), done = f.addLocal(kI32),
                itemTag = f.addLocal(kI64), itemPayload = f.addLocal(kI64),
                nextTag = f.addLocal(kI64), nextPayload = f.addLocal(kI64),
                byte = f.addLocal(kI32), clen = f.addLocal(kI32),
                itemStr = f.addLocal(kI32), vTag = f.addLocal(kI64),
                vPayload = f.addLocal(kI64), items = f.addLocal(kI32),
                entries = f.addLocal(kI32), entry = f.addLocal(kI32),
                idx = f.addLocal(kI32);
  f.i64c(static_cast<std::int64_t>(kNil));
  f.lset(itemTag);
  f.i64c(0);
  f.lset(itemPayload);
  f.lget(2);
  f.lset(nextTag);
  f.lget(3);
  f.lset(nextPayload);
  f.i32c(0);
  f.lset(done);
  f.i64c(static_cast<std::int64_t>(kNil));
  f.lset(vTag);
  f.i64c(0);
  f.lset(vPayload);
  // `for k, v in ...` needs a map: a paired value from any other iterable is
  // the same runtime error the VM raises.
  f.lget(4);
  f.if_();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.i64Eq();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sKeyValue);
  f.end();
  f.end();

  // Range: walk the cursor from `start` towards `end`, inclusive or exclusive.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kRange));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(0);
  f.lset(start);
  f.lget(addr);
  f.i64Load(8);
  f.lset(end);
  f.lget(addr);
  f.i64Load(16);
  f.i32WrapI64();
  f.lset(incl);
  f.lget(start);
  f.lget(end);
  f.i64LeS();
  f.lset(asc);
  f.lget(start);
  f.lget(3);
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kNil));
  f.i64Eq();
  f.select();
  f.lset(cur);
  // inBounds
  f.lget(asc);
  f.ifResult(kI32);
  f.lget(incl);
  f.ifResult(kI32);
  f.lget(cur);
  f.lget(end);
  f.i64LeS();
  f.else_();
  f.lget(cur);
  f.lget(end);
  f.i64LtS();
  f.end();
  f.else_();
  f.lget(incl);
  f.ifResult(kI32);
  f.lget(cur);
  f.lget(end);
  f.i64GeS();
  f.else_();
  f.lget(cur);
  f.lget(end);
  f.i64GtS();
  f.end();
  f.end();
  f.if_();
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lset(itemTag);
  f.lget(cur);
  f.lset(itemPayload);
  f.lget(cur);
  f.i64c(1);
  f.i64Add();
  f.lget(cur);
  f.i64c(1);
  f.i64Sub();
  f.lget(asc);
  f.select();
  f.lset(nextPayload);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lset(nextTag);
  f.else_();
  f.i32c(1);
  f.lset(done);
  f.end();
  f.end();

  // String: the cursor is a byte offset; each step yields one UTF-8 code point.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(0);
  f.lset(len);
  f.i64c(0);
  f.lget(3);
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kNil));
  f.i64Eq();
  f.select();
  f.lset(cur);
  f.lget(cur);
  f.lget(len);
  f.i64GeU();
  f.if_();
  f.i32c(1);
  f.lset(done);
  f.else_();
  f.lget(addr);
  f.i32c(8);
  f.i32Add();
  f.lget(cur);
  f.i32WrapI64();
  f.i32Add();
  f.i32Load8u(0);
  f.lset(byte);
  f.lget(byte);
  f.i32c(0x80);
  f.i32LtU();
  f.ifResult(kI32);
  f.i32c(1);
  f.else_();
  f.lget(byte);
  f.i32c(0xE0);
  f.i32And();
  f.i32c(0xC0);
  f.i32Eq();
  f.ifResult(kI32);
  f.i32c(2);
  f.else_();
  f.lget(byte);
  f.i32c(0xF0);
  f.i32And();
  f.i32c(0xE0);
  f.i32Eq();
  f.ifResult(kI32);
  f.i32c(3);
  f.else_();
  f.i32c(4);
  f.end();
  f.end();
  f.end();
  f.lset(clen);
  f.lget(clen);
  f.call(newStr);
  f.lset(itemStr);
  f.lget(itemStr);
  f.i32c(8);
  f.i32Add();
  f.lget(addr);
  f.i32c(8);
  f.i32Add();
  f.lget(cur);
  f.i32WrapI64();
  f.i32Add();
  f.lget(clen);
  f.memoryCopy();
  f.i64c(static_cast<std::int64_t>(kStr));
  f.lset(itemTag);
  f.lget(itemStr);
  f.i64ExtendI32U();
  f.lset(itemPayload);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lset(nextTag);
  f.lget(cur);
  f.lget(clen);
  f.i64ExtendI32U();
  f.i64Add();
  f.lset(nextPayload);
  f.end();
  f.end();

  // Array: the cursor is an index into the items block.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(kArrayLengthOffset);
  f.lset(len);
  f.lget(addr);
  f.i64Load(kArrayItemsOffset);
  f.i32WrapI64();
  f.lset(items);
  f.i64c(0);
  f.lget(3);
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kNil));
  f.i64Eq();
  f.select();
  f.lset(cur);
  f.lget(cur);
  f.lget(len);
  f.i64GeU();
  f.if_();
  f.i32c(1);
  f.lset(done);
  f.else_();
  f.lget(items);
  f.lget(cur);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(0);
  f.lset(itemTag);
  f.lget(addr);
  f.i64Load(8);
  f.lset(itemPayload);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lset(nextTag);
  f.lget(cur);
  f.i64c(1);
  f.i64Add();
  f.lset(nextPayload);
  f.end();
  f.end();

  // Map: the cursor is an entry index; the item is the key and the paired
  // value (when the loop asked for one) is the entry's value.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(kMapLengthOffset);
  f.lset(len);
  f.lget(addr);
  f.i64Load(kMapEntriesOffset);
  f.i32WrapI64();
  f.lset(entries);
  f.i64c(0);
  f.lget(3);
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kNil));
  f.i64Eq();
  f.select();
  f.lset(cur);
  f.lget(cur);
  f.lget(len);
  f.i64GeU();
  f.if_();
  f.i32c(1);
  f.lset(done);
  f.else_();
  f.lget(entries);
  f.lget(cur);
  f.i32WrapI64();
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.i32Add();
  f.lset(entry);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.lset(itemTag);
  f.lget(entry);
  f.i64Load(kMapEntryKeyOffset);
  f.lset(itemPayload);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lset(nextTag);
  f.lget(cur);
  f.i64c(1);
  f.i64Add();
  f.lset(nextPayload);
  f.lget(entry);
  f.i64Load(kMapEntryTagOffset);
  f.lset(vTag);
  f.lget(entry);
  f.i64Load(kMapEntryPayloadOffset);
  f.lset(vPayload);
  f.end();
  f.end();

  // Anything else is a clean runtime error rather than a silent empty loop.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kRange));
  f.i64Eq();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.i32Or();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.i32Or();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.i64Eq();
  f.i32Or();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sNotIterable);
  f.end();

  f.lget(itemTag);
  f.lget(itemPayload);
  f.lget(nextTag);
  f.lget(nextPayload);
  f.lget(done);
  f.lget(vTag);
  f.lget(vPayload);
  return index;
}

void Runtime::emitIsNumeric(Func& f, std::uint32_t tagLocal) {
  f.lget(tagLocal);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.lget(tagLocal);
  f.i64c(static_cast<std::int64_t>(kFloat));
  f.i64Eq();
  f.i32Or();
  f.lget(tagLocal);
  f.i64c(static_cast<std::int64_t>(kDouble));
  f.i64Eq();
  f.i32Or();
}

void Runtime::emitRank(Func& f, std::uint32_t tagLocal) {
  f.lget(tagLocal);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.ifResult(kI32);
  f.i32c(0);
  f.else_();
  f.lget(tagLocal);
  f.i64c(static_cast<std::int64_t>(kFloat));
  f.i64Eq();
  f.ifResult(kI32);
  f.i32c(1);
  f.else_();
  f.i32c(2);
  f.end();
  f.end();
}

void Runtime::emitToF64(Func& f, std::uint32_t tagLocal, std::uint32_t payloadLocal) {
  f.lget(tagLocal);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.ifResult(kF64);
  f.lget(payloadLocal);
  f.f64ConvertI64S();
  f.else_();
  f.lget(payloadLocal);
  f.f64ReinterpretI64();
  f.end();
}

// Binary operator selectors, in the order the lowering passes them.
enum BinKind {
  kBinAdd = 0,
  kBinSub = 1,
  kBinMul = 2,
  kBinDiv = 3,
  kBinMod = 4,
  kBinPow = 5,
  kBinBand = 6,
  kBinBor = 7,
  kBinBxor = 8,
  kBinShl = 9,
  kBinShr = 10,
};

std::uint32_t Runtime::emitBinary(int op, std::uint32_t failAddress, const char*) {
  (void)failAddress;
  std::uint32_t index = m.reserveFunction(tBin);
  Func f(m, index, {kI64, kI64, kI64, kI64});
  std::uint32_t rank = f.addLocal(kI32), a = f.addLocal(kF64), b = f.addLocal(kF64),
                r = f.addLocal(kF64), base = f.addLocal(kI64), exp = f.addLocal(kI64),
                acc = f.addLocal(kI64), neg = f.addLocal(kI32),
                resTag = f.addLocal(kI64), resPay = f.addLocal(kI64),
                itemsA = f.addLocal(kI32), itemsB = f.addLocal(kI32),
                addrA = f.addLocal(kI32), addrB = f.addLocal(kI32),
                li = f.addLocal(kI64);

  auto emitResultTag = [&]() {
    f.lget(rank);
    f.i32c(1);
    f.i32Eq();
    f.ifResult(kI64);
    f.i64c(static_cast<std::int64_t>(kFloat));
    f.else_();
    f.i64c(static_cast<std::int64_t>(kDouble));
    f.end();
  };
  auto emitRound = [&]() {
    f.lget(rank);
    f.i32c(1);
    f.i32Eq();
    f.if_();
    f.lget(r);
    f.f32DemoteF64();
    f.f64PromoteF32();
    f.lset(r);
    f.end();
  };
  auto emitMaxRank = [&]() {
    emitRank(f, 0);
    emitRank(f, 2);
    emitRank(f, 0);
    emitRank(f, 2);
    f.i32GtS();
    f.select();
    f.lset(rank);
  };

  if (op == kBinAdd || op == kBinSub || op == kBinMul) {
    f.lget(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.lget(2);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.i32And();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(1);
    f.lget(3);
    if (op == kBinAdd) {
      f.i64Add();
    } else if (op == kBinSub) {
      f.i64Sub();
    } else {
      f.i64Mul();
    }
    f.ret();
    f.end();

    emitIsNumeric(f, 0);
    emitIsNumeric(f, 2);
    f.i32And();
    f.if_();
    emitMaxRank();
    emitToF64(f, 0, 1);
    f.lset(a);
    emitToF64(f, 2, 3);
    f.lset(b);
    f.lget(a);
    f.lget(b);
    if (op == kBinAdd) {
      f.f64Add();
    } else if (op == kBinSub) {
      f.f64Sub();
    } else {
      f.f64Mul();
    }
    f.lset(r);
    emitRound();
    emitResultTag();
    f.lget(r);
    f.i64ReinterpretF64();
    f.ret();
    f.end();

    if (op == kBinAdd) {
      f.lget(0);
      f.i64c(static_cast<std::int64_t>(kStr));
      f.i64Eq();
      f.lget(2);
      f.i64c(static_cast<std::int64_t>(kStr));
      f.i64Eq();
      f.i32And();
      f.if_();
      f.i64c(static_cast<std::int64_t>(kStr));
      f.lget(1);
      f.i32WrapI64();
      f.lget(3);
      f.i32WrapI64();
      f.call(concat);
      f.i64ExtendI32U();
      f.ret();
      f.end();

      // `a + b` on two arrays concatenates them into a fresh array.
      f.lget(0);
      f.i64c(static_cast<std::int64_t>(kArray));
      f.i64Eq();
      f.lget(2);
      f.i64c(static_cast<std::int64_t>(kArray));
      f.i64Eq();
      f.i32And();
      f.if_();
      f.lget(1);
      f.i32WrapI64();
      f.lset(addrA);
      f.lget(addrA);
      f.i64Load(kArrayLengthOffset);
      f.lset(acc);
      f.lget(addrA);
      f.i64Load(kArrayItemsOffset);
      f.i32WrapI64();
      f.lset(itemsA);
      f.lget(3);
      f.i32WrapI64();
      f.lset(addrB);
      f.lget(addrB);
      f.i64Load(kArrayLengthOffset);
      f.lset(exp);
      f.lget(addrB);
      f.i64Load(kArrayItemsOffset);
      f.i32WrapI64();
      f.lset(itemsB);
      f.lget(acc);
      f.lget(exp);
      f.i64Add();
      f.call(newArray);
      f.lset(resPay);
      f.lset(resTag);
      f.i64c(0);
      f.lset(li);
      f.block();
      f.loop();
      f.lget(li);
      f.lget(acc);
      f.i64GeS();
      f.brIf(1);
      f.lget(itemsA);
      f.lget(li);
      f.i32WrapI64();
      f.i32c(16);
      f.i32Mul();
      f.i32Add();
      f.lset(addrA);
      f.lget(resPay);
      f.i32WrapI64();
      f.lget(addrA);
      f.i64Load(0);
      f.lget(addrA);
      f.i64Load(8);
      f.call(arrayPush);
      f.lget(li);
      f.i64c(1);
      f.i64Add();
      f.lset(li);
      f.br(0);
      f.end();
      f.end();
      f.i64c(0);
      f.lset(li);
      f.block();
      f.loop();
      f.lget(li);
      f.lget(exp);
      f.i64GeS();
      f.brIf(1);
      f.lget(itemsB);
      f.lget(li);
      f.i32WrapI64();
      f.i32c(16);
      f.i32Mul();
      f.i32Add();
      f.lset(addrB);
      f.lget(resPay);
      f.i32WrapI64();
      f.lget(addrB);
      f.i64Load(0);
      f.lget(addrB);
      f.i64Load(8);
      f.call(arrayPush);
      f.lget(li);
      f.i64c(1);
      f.i64Add();
      f.lset(li);
      f.br(0);
      f.end();
      f.end();
      f.lget(resTag);
      f.lget(resPay);
      f.ret();
      f.end();
    }
    emitFailStatic(f, sUnsupportedOperands);
    return index;
  }

  if (op == kBinDiv) {
    emitIsNumeric(f, 0);
    emitIsNumeric(f, 2);
    f.i32And();
    f.if_();
    emitToF64(f, 2, 3);
    f.lset(b);
    f.lget(b);
    f.f64c(0.0);
    f.f64Eq();
    f.if_();
    emitFailStatic(f, sDivZero);
    f.end();
    f.lget(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.lget(2);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.i32And();
    f.if_();
    f.lget(1);
    f.i64c(std::numeric_limits<std::int64_t>::min());
    f.i64Eq();
    f.lget(3);
    f.i64c(-1);
    f.i64Eq();
    f.i32And();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(1);
    f.ret();
    f.end();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(1);
    f.lget(3);
    f.i64DivS();
    f.ret();
    f.end();
    emitMaxRank();
    emitToF64(f, 0, 1);
    f.lset(a);
    f.lget(a);
    f.lget(b);
    f.f64Div();
    f.lset(r);
    emitRound();
    emitResultTag();
    f.lget(r);
    f.i64ReinterpretF64();
    f.ret();
    f.end();
    emitFailStatic(f, sUnsupportedOperands);
    return index;
  }

  if (op == kBinMod) {
    emitIsNumeric(f, 0);
    emitIsNumeric(f, 2);
    f.i32And();
    f.if_();
    f.lget(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.lget(2);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.i32And();
    f.if_();
    f.lget(3);
    f.i64c(0);
    f.i64Eq();
    f.if_();
    emitFailStatic(f, sDivZero);
    f.end();
    f.lget(1);
    f.i64c(std::numeric_limits<std::int64_t>::min());
    f.i64Eq();
    f.lget(3);
    f.i64c(-1);
    f.i64Eq();
    f.i32And();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64c(0);
    f.ret();
    f.end();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(1);
    f.lget(3);
    f.i64RemS();
    f.ret();
    f.end();
    emitToF64(f, 2, 3);
    f.lset(b);
    f.lget(b);
    f.f64c(0.0);
    f.f64Eq();
    f.if_();
    emitFailStatic(f, sDivZero);
    f.end();
    emitMaxRank();
    emitToF64(f, 0, 1);
    f.lset(a);
    f.lget(a);
    f.lget(a);
    f.lget(b);
    f.f64Div();
    f.f64Trunc();
    f.lget(b);
    f.f64Mul();
    f.f64Sub();
    f.lset(r);
    emitRound();
    emitResultTag();
    f.lget(r);
    f.i64ReinterpretF64();
    f.ret();
    f.end();
    emitFailStatic(f, sUnsupportedOperands);
    return index;
  }

  if (op == kBinPow) {
    emitIsNumeric(f, 0);
    emitIsNumeric(f, 2);
    f.i32And();
    f.if_();
    f.lget(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.lget(2);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.i32And();
    f.lget(3);
    f.i64c(0);
    f.i64GeS();
    f.i32And();
    f.if_();
    f.lget(1);
    f.lset(base);
    f.lget(3);
    f.lset(exp);
    f.i64c(1);
    f.lset(acc);
    f.block();
    f.loop();
    f.lget(exp);
    f.i64c(0);
    f.i64Eq();
    f.brIf(1);
    f.lget(exp);
    f.i64c(1);
    f.i64And();
    f.i64c(0);
    f.i64Ne();
    f.if_();
    f.lget(acc);
    f.lget(base);
    f.i64Mul();
    f.lset(acc);
    f.end();
    f.lget(base);
    f.lget(base);
    f.i64Mul();
    f.lset(base);
    f.lget(exp);
    f.i64c(1);
    f.i64ShrU();
    f.lset(exp);
    f.br(0);
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(acc);
    f.ret();
    f.end();
    emitToF64(f, 2, 3);
    f.lset(b);
    f.lget(b);
    f.f64Trunc();
    f.lget(b);
    f.f64Eq();
    f.i32Eqz();
    f.if_();
    emitFailStatic(f, sBadPower);
    f.end();
    f.lget(b);
    f.f64Abs();
    f.f64c(4096.0);
    f.f64Gt();
    f.if_();
    emitFailStatic(f, sBadPower);
    f.end();
    emitMaxRank();
    emitToF64(f, 0, 1);
    f.lset(a);
    f.lget(b);
    f.i64TruncSatF64S();
    f.lset(exp);
    f.lget(exp);
    f.i64c(0);
    f.i64LtS();
    f.lset(neg);
    f.lget(neg);
    f.if_();
    f.i64c(0);
    f.lget(exp);
    f.i64Sub();
    f.lset(exp);
    f.end();
    f.f64c(1.0);
    f.lset(r);
    f.block();
    f.loop();
    f.lget(exp);
    f.i64c(0);
    f.i64Eq();
    f.brIf(1);
    f.lget(exp);
    f.i64c(1);
    f.i64And();
    f.i64c(0);
    f.i64Ne();
    f.if_();
    f.lget(r);
    f.lget(a);
    f.f64Mul();
    f.lset(r);
    f.end();
    f.lget(a);
    f.lget(a);
    f.f64Mul();
    f.lset(a);
    f.lget(exp);
    f.i64c(1);
    f.i64ShrU();
    f.lset(exp);
    f.br(0);
    f.end();
    f.end();
    f.lget(neg);
    f.if_();
    f.f64c(1.0);
    f.lget(r);
    f.f64Div();
    f.lset(r);
    f.end();
    emitRound();
    emitResultTag();
    f.lget(r);
    f.i64ReinterpretF64();
    f.ret();
    f.end();
    emitFailStatic(f, sUnsupportedOperands);
    return index;
  }

  // Bitwise and shift operators: integers only.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.i32And();
  f.if_();
  if (op == kBinShl || op == kBinShr) {
    f.lget(3);
    f.i64c(0);
    f.i64LtS();
    f.lget(3);
    f.i64c(63);
    f.i64GtS();
    f.i32Or();
    f.if_();
    emitFailStatic(f, sBadShift);
    f.end();
  }
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lget(1);
  f.lget(3);
  switch (op) {
    case kBinBand: f.i64And(); break;
    case kBinBor: f.i64Or(); break;
    case kBinBxor: f.i64Xor(); break;
    case kBinShl: f.i64Shl(); break;
    default: f.i64ShrS(); break;
  }
  f.ret();
  f.end();
  emitFailStatic(f, sUnsupportedOperands);
  return index;
}

std::uint32_t Runtime::emitUnary(int op) {
  std::uint32_t index = m.reserveFunction(tUn);
  Func f(m, index, {kI64, kI64});
  if (op == 1) {  // logical not
    f.i64c(static_cast<std::int64_t>(kBool));
    f.lget(0);
    f.lget(1);
    f.call(truthy);
    f.i32Eqz();
    f.i64ExtendI32U();
    return index;
  }
  if (op == 0) {  // negate
    f.lget(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64c(0);
    f.lget(1);
    f.i64Sub();
    f.ret();
    f.end();
    f.lget(0);
    f.i64c(static_cast<std::int64_t>(kFloat));
    f.i64Eq();
    f.lget(0);
    f.i64c(static_cast<std::int64_t>(kDouble));
    f.i64Eq();
    f.i32Or();
    f.if_();
    f.lget(0);
    f.lget(1);
    f.f64ReinterpretI64();
    f.f64Neg();
    f.i64ReinterpretF64();
    f.ret();
    f.end();
    emitFailStatic(f, sUnsupportedOperands);
    return index;
  }
  // bitwise not
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.if_();
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lget(1);
  f.i64c(-1);
  f.i64Xor();
  f.ret();
  f.end();
  emitFailStatic(f, sUnsupportedOperands);
  return index;
}

std::uint32_t Runtime::emitCompare(int op) {
  std::uint32_t index = m.reserveFunction(tBin);
  Func f(m, index, {kI64, kI64, kI64, kI64});
  std::uint32_t addrA = f.addLocal(kI32), addrB = f.addLocal(kI32);

  if (op == 0) {  // equality
    f.lget(0);
    f.lget(1);
    f.lget(2);
    f.lget(3);
    f.i32c(0);
    f.call(equals);
    f.lset(addrA);
    f.i64c(static_cast<std::int64_t>(kBool));
    f.lget(addrA);
    f.i64ExtendI32U();
    return index;
  }

  // Strict ordering.
  emitIsNumeric(f, 0);
  emitIsNumeric(f, 2);
  f.i32And();
  f.if_();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.i32And();
  f.if_();
  f.i64c(static_cast<std::int64_t>(kBool));
  f.lget(1);
  f.lget(3);
  f.i64LtS();
  f.i64ExtendI32U();
  f.ret();
  f.end();
  f.i64c(static_cast<std::int64_t>(kBool));
  emitToF64(f, 0, 1);
  emitToF64(f, 2, 3);
  f.f64Lt();
  f.i64ExtendI32U();
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.i32And();
  f.if_();
  f.i64c(static_cast<std::int64_t>(kBool));
  f.lget(1);
  f.i32WrapI64();
  f.lget(3);
  f.i32WrapI64();
  f.call(strCmp);
  f.i32c(0);
  f.i32LtS();
  f.i64ExtendI32U();
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kBool));
  f.i64Eq();
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kBool));
  f.i64Eq();
  f.i32And();
  f.if_();
  f.i64c(static_cast<std::int64_t>(kBool));
  f.lget(1);
  f.i64c(0);
  f.i64Eq();
  f.lget(3);
  f.i64c(0);
  f.i64Ne();
  f.i32And();
  f.i64ExtendI32U();
  f.ret();
  f.end();
  emitFailStatic(f, sNotComparable);
  return index;
}

std::uint32_t Runtime::emitIn() {
  std::uint32_t index = m.reserveFunction(tBin);
  Func f(m, index, {kI64, kI64, kI64, kI64});
  std::uint32_t ln = f.addLocal(kI64), lh = f.addLocal(kI64), i = f.addLocal(kI64),
                j = f.addLocal(kI64), match = f.addLocal(kI32), b1 = f.addLocal(kI32),
                b2 = f.addLocal(kI32), addr = f.addLocal(kI32), start = f.addLocal(kI64),
                end = f.addLocal(kI64), incl = f.addLocal(kI32), v = f.addLocal(kI64);

  std::uint32_t items = f.addLocal(kI32), entry = f.addLocal(kI32),
                idx = f.addLocal(kI32);

  // Array membership: any item equal to the needle.
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.if_();
  f.lget(3);
  f.i32WrapI64();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(kArrayLengthOffset);
  f.lset(ln);
  f.lget(addr);
  f.i64Load(kArrayItemsOffset);
  f.i32WrapI64();
  f.lset(items);
  f.i64c(0);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(ln);
  f.i64GeS();
  f.brIf(1);
  f.lget(items);
  f.lget(i);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.lset(entry);
  f.lget(entry);
  f.i64Load(0);
  f.lget(entry);
  f.i64Load(8);
  f.lget(0);
  f.lget(1);
  f.i32c(0);
  f.call(equals);
  f.if_();
  f.i64c(static_cast<std::int64_t>(kBool));
  f.i32c(1);
  f.i64ExtendI32U();
  f.ret();
  f.end();
  f.lget(i);
  f.i64c(1);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.i64c(static_cast<std::int64_t>(kBool));
  f.i32c(0);
  f.i64ExtendI32U();
  f.ret();
  f.end();

  // Map membership: a string key.
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.i64Eq();
  f.if_();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sMapMembership);
  f.end();
  f.lget(3);
  f.i32WrapI64();
  f.lget(1);
  f.i32WrapI64();
  f.call(mapFind);
  f.i32c(0);
  f.i32GeS();
  f.lset(idx);
  f.i64c(static_cast<std::int64_t>(kBool));
  f.lget(idx);
  f.i64ExtendI32U();
  f.ret();
  f.end();

  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.if_();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sNotComparable);
  f.end();
  f.lget(1);
  f.i32WrapI64();
  f.i64Load(0);
  f.lset(ln);
  f.lget(3);
  f.i32WrapI64();
  f.i64Load(0);
  f.lset(lh);
  f.lget(ln);
  f.lget(lh);
  f.i64GtU();
  f.if_();
  f.i64c(static_cast<std::int64_t>(kBool));
  f.i32c(0);
  f.i64ExtendI32U();
  f.ret();
  f.end();
  f.i64c(0);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(ln);
  f.i64Add();
  f.lget(lh);
  f.i64GtU();
  f.brIf(1);
  f.i64c(0);
  f.lset(j);
  f.i32c(1);
  f.lset(match);
  f.block();
  f.loop();
  f.lget(j);
  f.lget(ln);
  f.i64GeU();
  f.brIf(1);
  f.lget(1);
  f.i32WrapI64();
  f.i32c(8);
  f.i32Add();
  f.lget(j);
  f.i32WrapI64();
  f.i32Add();
  f.i32Load8u(0);
  f.lset(b1);
  f.lget(3);
  f.i32WrapI64();
  f.i32c(8);
  f.i32Add();
  f.lget(i);
  f.i32WrapI64();
  f.i32Add();
  f.lget(j);
  f.i32WrapI64();
  f.i32Add();
  f.i32Load8u(0);
  f.lset(b2);
  f.lget(b1);
  f.lget(b2);
  f.i32Ne();
  f.if_();
  f.i32c(0);
  f.lset(match);
  f.br(2);
  f.end();
  f.lget(j);
  f.i64c(1);
  f.i64Add();
  f.lset(j);
  f.br(0);
  f.end();
  f.end();
  f.lget(match);
  f.if_();
  f.i64c(static_cast<std::int64_t>(kBool));
  f.i32c(1);
  f.i64ExtendI32U();
  f.ret();
  f.end();
  f.lget(i);
  f.i64c(1);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.i64c(static_cast<std::int64_t>(kBool));
  f.i32c(0);
  f.i64ExtendI32U();
  f.ret();
  f.end();

  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kRange));
  f.i64Eq();
  f.if_();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sNotComparable);
  f.end();
  f.lget(1);
  f.lset(v);
  f.lget(3);
  f.i32WrapI64();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(0);
  f.lset(start);
  f.lget(addr);
  f.i64Load(8);
  f.lset(end);
  f.lget(addr);
  f.i64Load(16);
  f.i32WrapI64();
  f.lset(incl);
  // start <= end ? (incl ? v>=start && v<=end : v>=start && v<end)
  //             : (incl ? v>=end && v<=start : v>end && v<start)
  f.i64c(static_cast<std::int64_t>(kBool));
  f.lget(start);
  f.lget(end);
  f.i64LeS();
  f.ifResult(kI32);
  f.lget(incl);
  f.ifResult(kI32);
  f.lget(v);
  f.lget(start);
  f.i64GeS();
  f.lget(v);
  f.lget(end);
  f.i64LeS();
  f.i32And();
  f.else_();
  f.lget(v);
  f.lget(start);
  f.i64GeS();
  f.lget(v);
  f.lget(end);
  f.i64LtS();
  f.i32And();
  f.end();
  f.else_();
  f.lget(incl);
  f.ifResult(kI32);
  f.lget(v);
  f.lget(end);
  f.i64GeS();
  f.lget(v);
  f.lget(start);
  f.i64LeS();
  f.i32And();
  f.else_();
  f.lget(v);
  f.lget(end);
  f.i64GtS();
  f.lget(v);
  f.lget(start);
  f.i64LtS();
  f.i32And();
  f.end();
  f.end();
  f.i64ExtendI32U();
  f.ret();
  f.end();
  emitFailStatic(f, sUnsupportedOperands);
  return index;
}

std::uint32_t Runtime::emitUtf8ByteOffset() {
  std::uint32_t type = m.typeFor({kI32, kI64}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32, kI64});
  std::uint32_t off = f.addLocal(kI64), len = f.addLocal(kI64), c = f.addLocal(kI32);
  f.lget(0);
  f.i64Load(0);
  f.lset(len);
  f.i64c(0);
  f.lset(off);
  f.block();
  f.loop();
  f.lget(1);
  f.i64c(0);
  f.i64LeS();
  f.brIf(1);
  f.lget(off);
  f.lget(len);
  f.i64GeU();
  f.brIf(1);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(off);
  f.i32WrapI64();
  f.i32Add();
  f.i32Load8u(0);
  f.lset(c);
  f.lget(off);
  f.lget(c);
  f.i32c(0x80);
  f.i32LtU();
  f.ifResult(kI64);
  f.i64c(1);
  f.else_();
  f.lget(c);
  f.i32c(0xE0);
  f.i32And();
  f.i32c(0xC0);
  f.i32Eq();
  f.ifResult(kI64);
  f.i64c(2);
  f.else_();
  f.lget(c);
  f.i32c(0xF0);
  f.i32And();
  f.i32c(0xE0);
  f.i32Eq();
  f.ifResult(kI64);
  f.i64c(3);
  f.else_();
  f.i64c(4);
  f.end();
  f.end();
  f.end();
  f.i64Add();
  f.lset(off);
  f.lget(1);
  f.i64c(1);
  f.i64Sub();
  f.lset(1);
  f.br(0);
  f.end();
  f.end();
  f.lget(off);
  f.i32WrapI64();
  return index;
}

std::uint32_t Runtime::emitMemberGet() {
  std::uint32_t index = m.reserveFunction(tBin);
  Func f(m, index, {kI64, kI64, kI64, kI64});
  std::uint32_t addr = f.addLocal(kI32), idx = f.addLocal(kI32),
                entry = f.addLocal(kI32), start = f.addLocal(kI64),
                end = f.addLocal(kI64), incl = f.addLocal(kI32);

  // An error exposes its `message` and `code` fields.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kError));
  f.i64Eq();
  f.if_();
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sNoMember);
  f.end();
  f.lget(3);
  f.i32WrapI64();
  f.i32c(static_cast<std::int32_t>(sMessage));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  f.i64c(static_cast<std::int64_t>(kStr));
  f.lget(1);
  f.i32WrapI64();
  f.i64Load(0);
  f.ret();
  f.end();
  f.lget(3);
  f.i32WrapI64();
  f.i32c(static_cast<std::int32_t>(sCode));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lget(1);
  f.i32WrapI64();
  f.i64Load(8);
  f.ret();
  f.end();
  emitFailStatic(f, sNoMember);
  f.end();

  // A member name is always a string; a dynamic index goes through
  // `indexGet` instead.
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sNoMember);
  f.end();
  f.lget(3);
  f.i32WrapI64();
  f.lset(addr);

  // A map looks the name up first, so a stored `length` wins, then falls back
  // to `length` and the bound methods; an absent name is nil, not an error.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lget(addr);
  f.call(mapFind);
  f.lset(idx);
  f.lget(idx);
  f.i32c(0);
  f.i32GeS();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.i64Load(kMapEntriesOffset);
  f.i32WrapI64();
  f.lget(idx);
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.i32Add();
  f.lset(entry);
  f.lget(entry);
  f.i64Load(kMapEntryTagOffset);
  f.lget(entry);
  f.i64Load(kMapEntryPayloadOffset);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sLength));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lget(1);
  f.i32WrapI64();
  f.i64Load(kMapLengthOffset);
  f.ret();
  f.end();
  f.i64c(static_cast<std::int64_t>(kNil));
  f.i64c(0);
  f.ret();
  f.end();

  // An array exposes `length` and its bound methods.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.if_();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sLength));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lget(1);
  f.i32WrapI64();
  f.i64Load(kArrayLengthOffset);
  f.ret();
  f.end();
  emitFailStatic(f, sNoMember);
  f.end();

  // A string exposes `length` and its bound methods.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.if_();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sLength));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lget(1);
  f.i32WrapI64();
  f.call(utf8Len);
  f.ret();
  f.end();
  emitFailStatic(f, sNoMember);
  f.end();

  // A range exposes `length` and its bound methods.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kRange));
  f.i64Eq();
  f.if_();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sLength));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(0);
  f.lset(start);
  f.lget(addr);
  f.i64Load(8);
  f.lset(end);
  f.lget(addr);
  f.i64Load(16);
  f.i32WrapI64();
  f.lset(incl);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lget(end);
  f.lget(start);
  f.i64Sub();
  f.lget(start);
  f.lget(end);
  f.i64Sub();
  f.lget(end);
  f.lget(start);
  f.i64GeS();
  f.select();
  f.lget(incl);
  f.i64ExtendI32U();
  f.i64Add();
  f.ret();
  f.end();
  emitFailStatic(f, sNoMember);
  f.end();

  emitFailStatic(f, sCannotIndex);
  return index;
}

std::uint32_t Runtime::emitMemberSet() {
  std::uint32_t type = m.typeFor({kI64, kI64, kI64, kI64, kI64, kI64}, {kI64, kI64});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI64, kI64, kI64, kI64, kI64, kI64});
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.i64Eq();
  f.if_();
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sMapKeyString);
  f.end();
  f.lget(1);
  f.i32WrapI64();
  f.lget(3);
  f.i32WrapI64();
  f.lget(4);
  f.lget(5);
  f.call(mapSet);
  f.i64c(static_cast<std::int64_t>(kNil));
  f.i64c(0);
  f.ret();
  f.end();
  emitFailStatic(f, sCannotSetField);
  return index;
}

// -------------------------------------------------------------- collections
// An array and a map share the on-image layout documented in
// `include/native/value.h`: a three-word header whose first word is the live
// length. `newArray`/`newMap` allocate the header and the items block; the
// accessors mirror `ops::indexGet`/`indexSet` so a program behaves the same
// on the VM and in the module.
std::uint32_t Runtime::emitNewArray() {
  std::uint32_t index = m.reserveFunction(tNewArray);
  Func f(m, index, {kI64});
  std::uint32_t cap = f.addLocal(kI32), header = f.addLocal(kI32),
                items = f.addLocal(kI32);
  f.lget(0);
  f.i32WrapI64();
  f.lset(cap);
  f.lget(cap);
  f.i32c(4);
  f.i32LtS();
  f.if_();
  f.i32c(4);
  f.lset(cap);
  f.end();
  f.i32c(24);
  f.call(alloc);
  f.lset(header);
  f.lget(cap);
  f.i32c(16);
  f.i32Mul();
  f.call(alloc);
  f.lset(items);
  f.lget(header);
  f.i64c(0);
  f.i64Store(kArrayLengthOffset);
  f.lget(header);
  f.lget(cap);
  f.i64ExtendI32U();
  f.i64Store(kArrayCapacityOffset);
  f.lget(header);
  f.lget(items);
  f.i64ExtendI32U();
  f.i64Store(kArrayItemsOffset);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.lget(header);
  f.i64ExtendI32U();
  return index;
}

std::uint32_t Runtime::emitNewMap() {
  std::uint32_t index = m.reserveFunction(tNewMap);
  Func f(m, index, {});
  std::uint32_t header = f.addLocal(kI32), entries = f.addLocal(kI32);
  f.i32c(24);
  f.call(alloc);
  f.lset(header);
  f.i32c(4 * kMapEntrySize);
  f.call(alloc);
  f.lset(entries);
  f.lget(header);
  f.i64c(0);
  f.i64Store(kMapLengthOffset);
  f.lget(header);
  f.i64c(4);
  f.i64Store(kMapCapacityOffset);
  f.lget(header);
  f.lget(entries);
  f.i64ExtendI32U();
  f.i64Store(kMapEntriesOffset);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.lget(header);
  f.i64ExtendI32U();
  return index;
}

std::uint32_t Runtime::emitArrayPush() {
  std::uint32_t index = m.reserveFunction(tArrayPush);
  Func f(m, index, {kI32, kI64, kI64});
  std::uint32_t len = f.addLocal(kI64), cap = f.addLocal(kI64),
                fresh = f.addLocal(kI32), addr = f.addLocal(kI32);
  f.lget(0);
  f.i64Load(kArrayLengthOffset);
  f.lset(len);
  f.lget(0);
  f.i64Load(kArrayCapacityOffset);
  f.lset(cap);
  f.lget(len);
  f.lget(cap);
  f.i64GeU();
  f.if_();
  f.lget(cap);
  f.i64c(2);
  f.i64Mul();
  f.lset(cap);
  f.lget(cap);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.call(alloc);
  f.lset(fresh);
  f.lget(fresh);
  f.lget(0);
  f.i64Load(kArrayItemsOffset);
  f.i32WrapI64();
  f.lget(len);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.memoryCopy();
  f.lget(0);
  f.lget(cap);
  f.i64Store(kArrayCapacityOffset);
  f.lget(0);
  f.lget(fresh);
  f.i64ExtendI32U();
  f.i64Store(kArrayItemsOffset);
  f.end();
  f.lget(0);
  f.i64Load(kArrayItemsOffset);
  f.i32WrapI64();
  f.lget(len);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.lset(addr);
  f.lget(addr);
  f.lget(1);
  f.i64Store(0);
  f.lget(addr);
  f.lget(2);
  f.i64Store(8);
  f.lget(0);
  f.lget(len);
  f.i64c(1);
  f.i64Add();
  f.i64Store(kArrayLengthOffset);
  return index;
}

std::uint32_t Runtime::emitArrayGet() {
  std::uint32_t index = m.reserveFunction(tArrayGet);
  Func f(m, index, {kI32, kI64});
  std::uint32_t len = f.addLocal(kI64), idx = f.addLocal(kI64),
                addr = f.addLocal(kI32);
  f.lget(0);
  f.i64Load(kArrayLengthOffset);
  f.lset(len);
  f.lget(1);
  f.lset(idx);
  f.lget(idx);
  f.i64c(0);
  f.i64LtS();
  f.if_();
  f.lget(idx);
  f.lget(len);
  f.i64Add();
  f.lset(idx);
  f.end();
  f.lget(idx);
  f.i64c(0);
  f.i64LtS();
  f.lget(idx);
  f.lget(len);
  f.i64GeS();
  f.i32Or();
  f.if_();
  emitFailStatic(f, sArrayRange);
  f.end();
  f.lget(0);
  f.i64Load(kArrayItemsOffset);
  f.i32WrapI64();
  f.lget(idx);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(0);
  f.lget(addr);
  f.i64Load(8);
  return index;
}

std::uint32_t Runtime::emitArraySet() {
  std::uint32_t index = m.reserveFunction(tArraySet);
  Func f(m, index, {kI32, kI64, kI64, kI64});
  std::uint32_t len = f.addLocal(kI64), idx = f.addLocal(kI64),
                addr = f.addLocal(kI32);
  f.lget(0);
  f.i64Load(kArrayLengthOffset);
  f.lset(len);
  f.lget(1);
  f.lset(idx);
  f.lget(idx);
  f.i64c(0);
  f.i64LtS();
  f.if_();
  f.lget(idx);
  f.lget(len);
  f.i64Add();
  f.lset(idx);
  f.end();
  f.lget(idx);
  f.lget(len);
  f.i64Eq();
  f.if_();
  f.lget(0);
  f.lget(2);
  f.lget(3);
  f.call(arrayPush);
  f.ret();
  f.end();
  f.lget(idx);
  f.i64c(0);
  f.i64LtS();
  f.lget(idx);
  f.lget(len);
  f.i64GtS();
  f.i32Or();
  f.if_();
  emitFailStatic(f, sArrayRange);
  f.end();
  f.lget(0);
  f.i64Load(kArrayItemsOffset);
  f.i32WrapI64();
  f.lget(idx);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.lset(addr);
  f.lget(addr);
  f.lget(2);
  f.i64Store(0);
  f.lget(addr);
  f.lget(3);
  f.i64Store(8);
  return index;
}

std::uint32_t Runtime::emitMapFind() {
  std::uint32_t index = m.reserveFunction(tMapFind);
  Func f(m, index, {kI32, kI32});
  std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64),
                entries = f.addLocal(kI32), entry = f.addLocal(kI32);
  f.lget(0);
  f.i64Load(kMapLengthOffset);
  f.lset(len);
  f.lget(0);
  f.i64Load(kMapEntriesOffset);
  f.i32WrapI64();
  f.lset(entries);
  f.i64c(0);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(len);
  f.i64GeS();
  f.brIf(1);
  f.lget(entries);
  f.lget(i);
  f.i32WrapI64();
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.i32Add();
  f.lset(entry);
  f.lget(entry);
  f.i64Load(kMapEntryKeyOffset);
  f.i32WrapI64();
  f.lget(1);
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  f.lget(i);
  f.i32WrapI64();
  f.ret();
  f.end();
  f.lget(i);
  f.i64c(1);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.i32c(-1);
  return index;
}

std::uint32_t Runtime::emitMapSet() {
  std::uint32_t index = m.reserveFunction(tMapSet);
  Func f(m, index, {kI32, kI32, kI64, kI64});
  std::uint32_t idx = f.addLocal(kI32), len = f.addLocal(kI64),
                cap = f.addLocal(kI64), entries = f.addLocal(kI32),
                fresh = f.addLocal(kI32), entry = f.addLocal(kI32);
  f.lget(0);
  f.lget(1);
  f.call(mapFind);
  f.lset(idx);
  f.lget(idx);
  f.i32c(0);
  f.i32GeS();
  f.if_();
  f.lget(0);
  f.i64Load(kMapEntriesOffset);
  f.i32WrapI64();
  f.lget(idx);
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.i32Add();
  f.lset(entry);
  f.lget(entry);
  f.lget(2);
  f.i64Store(kMapEntryTagOffset);
  f.lget(entry);
  f.lget(3);
  f.i64Store(kMapEntryPayloadOffset);
  f.ret();
  f.end();
  f.lget(0);
  f.i64Load(kMapLengthOffset);
  f.lset(len);
  f.lget(0);
  f.i64Load(kMapCapacityOffset);
  f.lset(cap);
  f.lget(len);
  f.lget(cap);
  f.i64GeU();
  f.if_();
  f.lget(cap);
  f.i64c(2);
  f.i64Mul();
  f.lset(cap);
  f.lget(cap);
  f.i32WrapI64();
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.call(alloc);
  f.lset(fresh);
  f.lget(fresh);
  f.lget(0);
  f.i64Load(kMapEntriesOffset);
  f.i32WrapI64();
  f.lget(len);
  f.i32WrapI64();
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.memoryCopy();
  f.lget(0);
  f.lget(cap);
  f.i64Store(kMapCapacityOffset);
  f.lget(0);
  f.lget(fresh);
  f.i64ExtendI32U();
  f.i64Store(kMapEntriesOffset);
  f.end();
  f.lget(0);
  f.i64Load(kMapEntriesOffset);
  f.i32WrapI64();
  f.lset(entries);
  f.lget(entries);
  f.lget(len);
  f.i32WrapI64();
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.i32Add();
  f.lset(entry);
  f.lget(entry);
  f.lget(1);
  f.i64ExtendI32U();
  f.i64Store(kMapEntryKeyOffset);
  f.lget(entry);
  f.lget(2);
  f.i64Store(kMapEntryTagOffset);
  f.lget(entry);
  f.lget(3);
  f.i64Store(kMapEntryPayloadOffset);
  f.lget(0);
  f.lget(len);
  f.i64c(1);
  f.i64Add();
  f.i64Store(kMapLengthOffset);
  return index;
}

std::uint32_t Runtime::emitMapGet() {
  std::uint32_t index = m.reserveFunction(tMapGet);
  Func f(m, index, {kI32, kI32});
  std::uint32_t idx = f.addLocal(kI32), entry = f.addLocal(kI32);
  f.lget(0);
  f.lget(1);
  f.call(mapFind);
  f.lset(idx);
  f.lget(idx);
  f.i32c(0);
  f.i32LtS();
  f.if_();
  f.i64c(static_cast<std::int64_t>(kNil));
  f.i64c(0);
  f.ret();
  f.end();
  f.lget(0);
  f.i64Load(kMapEntriesOffset);
  f.i32WrapI64();
  f.lget(idx);
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.i32Add();
  f.lset(entry);
  f.lget(entry);
  f.i64Load(kMapEntryTagOffset);
  f.lget(entry);
  f.i64Load(kMapEntryPayloadOffset);
  return index;
}

std::uint32_t Runtime::emitMapDelete() {
  std::uint32_t index = m.reserveFunction(tMapDelete);
  Func f(m, index, {kI32, kI32});
  std::uint32_t idx = f.addLocal(kI32), len = f.addLocal(kI64),
                entries = f.addLocal(kI32), dst = f.addLocal(kI32),
                src = f.addLocal(kI32), count = f.addLocal(kI32);
  f.lget(0);
  f.lget(1);
  f.call(mapFind);
  f.lset(idx);
  f.lget(idx);
  f.i32c(0);
  f.i32LtS();
  f.if_();
  f.i32c(0);
  f.ret();
  f.end();
  f.lget(0);
  f.i64Load(kMapLengthOffset);
  f.lset(len);
  f.lget(0);
  f.i64Load(kMapEntriesOffset);
  f.i32WrapI64();
  f.lset(entries);
  f.lget(entries);
  f.lget(idx);
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.i32Add();
  f.lset(dst);
  f.lget(dst);
  f.i32c(kMapEntrySize);
  f.i32Add();
  f.lset(src);
  f.lget(len);
  f.i32WrapI64();
  f.lget(idx);
  f.i32Sub();
  f.i32c(1);
  f.i32Sub();
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.lset(count);
  f.lget(dst);
  f.lget(src);
  f.lget(count);
  f.memoryCopy();
  f.lget(0);
  f.lget(len);
  f.i64c(1);
  f.i64Sub();
  f.i64Store(kMapLengthOffset);
  f.i32c(1);
  return index;
}

std::uint32_t Runtime::emitMapClear() {
  std::uint32_t index = m.reserveFunction(m.typeFor({kI32}, {}));
  Func f(m, index, {kI32});
  f.lget(0);
  f.i64c(0);
  f.i64Store(kMapLengthOffset);
  return index;
}

// Structural equality, with the hosted engine's `visited` rule: a pair already
// under comparison is assumed equal, so a cyclic structure terminates. The
// list is a chain of 16-byte nodes {a address, b address, next}.
std::uint32_t Runtime::emitEquals() {
  std::uint32_t index = m.reserveFunction(tEquals);
  Func f(m, index, {kI64, kI64, kI64, kI64, kI32});
  std::uint32_t addrA = f.addLocal(kI32), addrB = f.addLocal(kI32),
                lenA = f.addLocal(kI64), lenB = f.addLocal(kI64),
                i = f.addLocal(kI64), node = f.addLocal(kI32),
                p = f.addLocal(kI32), itemsA = f.addLocal(kI32),
                itemsB = f.addLocal(kI32), entryA = f.addLocal(kI32),
                entryB = f.addLocal(kI32), idx = f.addLocal(kI32),
                a = f.addLocal(kF64), b = f.addLocal(kF64);

  // Numbers compare numerically (`1 == 1.0`), matching `Value::equals`.
  emitIsNumeric(f, 0);
  emitIsNumeric(f, 2);
  f.i32And();
  f.if_();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.i32And();
  f.if_();
  f.lget(1);
  f.lget(3);
  f.i64Eq();
  f.ret();
  f.end();
  emitToF64(f, 0, 1);
  f.lset(a);
  emitToF64(f, 2, 3);
  f.lset(b);
  f.lget(a);
  f.lget(b);
  f.f64Eq();
  f.ret();
  f.end();

  // Different kinds are never equal (the numeric pair was handled above).
  f.lget(0);
  f.lget(2);
  f.i64Ne();
  f.if_();
  f.i32c(0);
  f.ret();
  f.end();

  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kNil));
  f.i64Eq();
  f.if_();
  f.i32c(1);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kBool));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.lget(3);
  f.i64Eq();
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lget(3);
  f.i32WrapI64();
  f.call(strCmp);
  f.i32Eqz();
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kFunc));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.lget(3);
  f.i64Eq();
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(AotTag::Ptr));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.lget(3);
  f.i64Eq();
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kRange));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addrA);
  f.lget(3);
  f.i32WrapI64();
  f.lset(addrB);
  f.lget(addrA);
  f.i64Load(0);
  f.lget(addrB);
  f.i64Load(0);
  f.i64Eq();
  f.lget(addrA);
  f.i64Load(8);
  f.lget(addrB);
  f.i64Load(8);
  f.i64Eq();
  f.i32And();
  f.lget(addrA);
  f.i64Load(16);
  f.lget(addrB);
  f.i64Load(16);
  f.i64Eq();
  f.i32And();
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kError));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addrA);
  f.lget(3);
  f.i32WrapI64();
  f.lset(addrB);
  f.lget(addrA);
  f.i64Load(0);
  f.i32WrapI64();
  f.lget(addrB);
  f.i64Load(0);
  f.i32WrapI64();
  f.call(strCmp);
  f.i32Eqz();
  f.lget(addrA);
  f.i64Load(8);
  f.lget(addrB);
  f.i64Load(8);
  f.i64Eq();
  f.i32And();
  f.ret();
  f.end();

  // Containers: compare lengths, then walk the pair list so a cycle is
  // assumed equal instead of recursing forever.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.i64Eq();
  f.i32Or();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addrA);
  f.lget(3);
  f.i32WrapI64();
  f.lset(addrB);
  f.lget(addrA);
  f.i64Load(0);
  f.lset(lenA);
  f.lget(addrB);
  f.i64Load(0);
  f.lset(lenB);
  f.lget(lenA);
  f.lget(lenB);
  f.i64Ne();
  f.if_();
  f.i32c(0);
  f.ret();
  f.end();
  f.lget(4);
  f.lset(p);
  f.block();
  f.loop();
  f.lget(p);
  f.i32Eqz();
  f.brIf(1);
  f.lget(p);
  f.i32Load(0);
  f.lget(addrA);
  f.i32Eq();
  f.lget(p);
  f.i32Load(4);
  f.lget(addrB);
  f.i32Eq();
  f.i32And();
  f.if_();
  f.i32c(1);
  f.ret();
  f.end();
  f.lget(p);
  f.i32Load(8);
  f.lset(p);
  f.br(0);
  f.end();
  f.end();
  f.i32c(16);
  f.call(alloc);
  f.lset(node);
  f.lget(node);
  f.lget(addrA);
  f.i32Store(0);
  f.lget(node);
  f.lget(addrB);
  f.i32Store(4);
  f.lget(node);
  f.lget(4);
  f.i32Store(8);

  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.if_();
  f.lget(addrA);
  f.i64Load(kArrayItemsOffset);
  f.i32WrapI64();
  f.lset(itemsA);
  f.lget(addrB);
  f.i64Load(kArrayItemsOffset);
  f.i32WrapI64();
  f.lset(itemsB);
  f.i64c(0);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(lenA);
  f.i64GeS();
  f.brIf(1);
  f.lget(itemsA);
  f.lget(i);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.lset(addrA);
  f.lget(itemsB);
  f.lget(i);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.lset(addrB);
  f.lget(addrA);
  f.i64Load(0);
  f.lget(addrA);
  f.i64Load(8);
  f.lget(addrB);
  f.i64Load(0);
  f.lget(addrB);
  f.i64Load(8);
  f.lget(node);
  f.call(index);
  f.i32Eqz();
  f.if_();
  f.i32c(0);
  f.ret();
  f.end();
  f.lget(i);
  f.i64c(1);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.i32c(1);
  f.ret();
  f.end();

  // Map: every key of A must exist in B with an equal value.
  f.lget(addrA);
  f.i64Load(kMapEntriesOffset);
  f.i32WrapI64();
  f.lset(itemsA);
  f.lget(addrB);
  f.i64Load(kMapEntriesOffset);
  f.i32WrapI64();
  f.lset(itemsB);
  f.i64c(0);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(lenA);
  f.i64GeS();
  f.brIf(1);
  f.lget(itemsA);
  f.lget(i);
  f.i32WrapI64();
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.i32Add();
  f.lset(entryA);
  f.lget(addrB);
  f.lget(entryA);
  f.i64Load(kMapEntryKeyOffset);
  f.i32WrapI64();
  f.call(mapFind);
  f.lset(idx);
  f.lget(idx);
  f.i32c(0);
  f.i32LtS();
  f.if_();
  f.i32c(0);
  f.ret();
  f.end();
  f.lget(itemsB);
  f.lget(idx);
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.i32Add();
  f.lset(entryB);
  f.lget(entryA);
  f.i64Load(kMapEntryTagOffset);
  f.lget(entryA);
  f.i64Load(kMapEntryPayloadOffset);
  f.lget(entryB);
  f.i64Load(kMapEntryTagOffset);
  f.lget(entryB);
  f.i64Load(kMapEntryPayloadOffset);
  f.lget(node);
  f.call(index);
  f.i32Eqz();
  f.if_();
  f.i32c(0);
  f.ret();
  f.end();
  f.lget(i);
  f.i64c(1);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.i32c(1);
  f.ret();
  f.end();

  f.i32c(0);
  return index;
}

// The depth-aware renderers. `toStr`/`repr` are thin wrappers that start at
// depth 0; a nested container renders its items with depth + 1 and stops at 9
// with `...`, exactly like `writeValue` in `value.cpp`.
std::uint32_t Runtime::emitToStrDepthBody(std::uint32_t index) {
  Func f(m, index, {kI64, kI64, kI32});
  f.lget(2);
  f.i32c(8);
  f.i32GtS();
  f.if_();
  f.i32c(static_cast<std::int32_t>(sEllipsis));
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.i64Eq();
  f.i32Or();
  f.if_();
  f.lget(0);
  f.lget(1);
  f.lget(2);
  f.call(reprDepth);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kNil));
  f.i64Eq();
  f.if_();
  f.i32c(static_cast<std::int32_t>(sNil));
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kBool));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.if_();
  f.i32c(static_cast<std::int32_t>(sTrue));
  f.ret();
  f.else_();
  f.i32c(static_cast<std::int32_t>(sFalse));
  f.ret();
  f.end();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.call(fmtInt);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kFloat));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.call(fmtDouble);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kDouble));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.call(fmtDouble);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kRange));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.call(fmtRange);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kFunc));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.call(fmtFunc);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kError));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.i64Load(0);
  f.i32WrapI64();
  f.ret();
  f.end();
  emitFailStatic(f, sUnknown);
  return index;
}

std::uint32_t Runtime::emitReprDepthBody(std::uint32_t index) {
  Func f(m, index, {kI64, kI64, kI32});
  std::uint32_t addr = f.addLocal(kI32), len = f.addLocal(kI64),
                i = f.addLocal(kI64), items = f.addLocal(kI32),
                s = f.addLocal(kI32), itemTag = f.addLocal(kI64),
                itemPayload = f.addLocal(kI64), entry = f.addLocal(kI32);
  f.lget(2);
  f.i32c(8);
  f.i32GtS();
  f.if_();
  f.i32c(static_cast<std::int32_t>(sEllipsis));
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.call(quote);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kError));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addr);
  f.i32c(static_cast<std::int32_t>(sErrorPrefix));
  f.lget(addr);
  f.i64Load(0);
  f.i32WrapI64();
  f.call(quote);
  f.call(concat);
  f.lset(s);
  f.lget(addr);
  f.i64Load(8);
  f.i64c(0);
  f.i64Ne();
  f.if_();
  f.lget(s);
  f.i32c(static_cast<std::int32_t>(sComma));
  f.call(concat);
  f.lget(addr);
  f.i64Load(8);
  f.call(fmtInt);
  f.call(concat);
  f.lset(s);
  f.end();
  f.lget(s);
  f.i32c(static_cast<std::int32_t>(sCloseParen));
  f.call(concat);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(kArrayLengthOffset);
  f.lset(len);
  f.lget(addr);
  f.i64Load(kArrayItemsOffset);
  f.i32WrapI64();
  f.lset(items);
  f.i32c(static_cast<std::int32_t>(sOpenBracket));
  f.lset(s);
  f.i64c(0);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(len);
  f.i64GeS();
  f.brIf(1);
  f.lget(i);
  f.i64c(0);
  f.i64GtS();
  f.if_();
  f.lget(s);
  f.i32c(static_cast<std::int32_t>(sComma));
  f.call(concat);
  f.lset(s);
  f.end();
  f.lget(items);
  f.lget(i);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(0);
  f.lset(itemTag);
  f.lget(addr);
  f.i64Load(8);
  f.lset(itemPayload);
  f.lget(s);
  f.lget(itemTag);
  f.lget(itemPayload);
  f.lget(2);
  f.i32c(1);
  f.i32Add();
  f.call(reprDepth);
  f.call(concat);
  f.lset(s);
  f.lget(i);
  f.i64c(1);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.lget(s);
  f.i32c(static_cast<std::int32_t>(sCloseBracket));
  f.call(concat);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(kMapLengthOffset);
  f.lset(len);
  f.lget(addr);
  f.i64Load(kMapEntriesOffset);
  f.i32WrapI64();
  f.lset(items);
  f.i32c(static_cast<std::int32_t>(sOpenBrace));
  f.lset(s);
  f.i64c(0);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(len);
  f.i64GeS();
  f.brIf(1);
  f.lget(i);
  f.i64c(0);
  f.i64GtS();
  f.if_();
  f.lget(s);
  f.i32c(static_cast<std::int32_t>(sComma));
  f.call(concat);
  f.lset(s);
  f.end();
  f.lget(items);
  f.lget(i);
  f.i32WrapI64();
  f.i32c(kMapEntrySize);
  f.i32Mul();
  f.i32Add();
  f.lset(entry);
  f.lget(s);
  f.lget(entry);
  f.i64Load(kMapEntryKeyOffset);
  f.i32WrapI64();
  f.call(quote);
  f.call(concat);
  f.lset(s);
  f.lget(s);
  f.i32c(static_cast<std::int32_t>(sColonSpace));
  f.call(concat);
  f.lset(s);
  f.lget(s);
  f.lget(entry);
  f.i64Load(kMapEntryTagOffset);
  f.lget(entry);
  f.i64Load(kMapEntryPayloadOffset);
  f.lget(2);
  f.i32c(1);
  f.i32Add();
  f.call(reprDepth);
  f.call(concat);
  f.lset(s);
  f.lget(i);
  f.i64c(1);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.lget(s);
  f.i32c(static_cast<std::int32_t>(sCloseBrace));
  f.call(concat);
  f.ret();
  f.end();
  f.lget(0);
  f.lget(1);
  f.lget(2);
  f.call(toStrDepth);
  return index;
}

std::uint32_t Runtime::emitIndexGet() {
  std::uint32_t index = m.reserveFunction(tBin);
  Func f(m, index, {kI64, kI64, kI64, kI64});
  std::uint32_t addr = f.addLocal(kI32), count = f.addLocal(kI64),
                idx = f.addLocal(kI64), off = f.addLocal(kI32),
                byte = f.addLocal(kI32), clen = f.addLocal(kI32),
                out = f.addLocal(kI32);
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.if_();
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sArrayIndexInt);
  f.end();
  f.lget(1);
  f.i32WrapI64();
  f.lget(3);
  f.call(arrayGet);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.i64Eq();
  f.if_();
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sMapKeyString);
  f.end();
  f.lget(1);
  f.i32WrapI64();
  f.lget(3);
  f.i32WrapI64();
  f.call(mapGet);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.if_();
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sStringIndexInt);
  f.end();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addr);
  f.lget(addr);
  f.call(utf8Len);
  f.lset(count);
  f.lget(3);
  f.lset(idx);
  f.lget(idx);
  f.i64c(0);
  f.i64LtS();
  f.if_();
  f.lget(idx);
  f.lget(count);
  f.i64Add();
  f.lset(idx);
  f.end();
  f.lget(idx);
  f.i64c(0);
  f.i64LtS();
  f.lget(idx);
  f.lget(count);
  f.i64GeS();
  f.i32Or();
  f.if_();
  emitFailStatic(f, sIndexRange);
  f.end();
  f.lget(addr);
  f.lget(idx);
  f.call(utf8ByteOffset);
  f.lset(off);
  f.lget(addr);
  f.i32c(8);
  f.i32Add();
  f.lget(off);
  f.i32Add();
  f.i32Load8u(0);
  f.lset(byte);
  f.lget(byte);
  f.i32c(0x80);
  f.i32LtU();
  f.ifResult(kI32);
  f.i32c(1);
  f.else_();
  f.lget(byte);
  f.i32c(0xE0);
  f.i32And();
  f.i32c(0xC0);
  f.i32Eq();
  f.ifResult(kI32);
  f.i32c(2);
  f.else_();
  f.lget(byte);
  f.i32c(0xF0);
  f.i32And();
  f.i32c(0xE0);
  f.i32Eq();
  f.ifResult(kI32);
  f.i32c(3);
  f.else_();
  f.i32c(4);
  f.end();
  f.end();
  f.end();
  f.lset(clen);
  f.lget(clen);
  f.call(newStr);
  f.lset(out);
  f.lget(out);
  f.i32c(8);
  f.i32Add();
  f.lget(addr);
  f.i32c(8);
  f.i32Add();
  f.lget(off);
  f.i32Add();
  f.lget(clen);
  f.memoryCopy();
  f.i64c(static_cast<std::int64_t>(kStr));
  f.lget(out);
  f.i64ExtendI32U();
  f.ret();
  f.end();
  emitFailStatic(f, sCannotIndex);
  return index;
}

std::uint32_t Runtime::emitIndexSet() {
  std::uint32_t type = m.typeFor({kI64, kI64, kI64, kI64, kI64, kI64}, {});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI64, kI64, kI64, kI64, kI64, kI64});
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.if_();
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.i64Eq();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sArrayIndexInt);
  f.end();
  f.lget(1);
  f.i32WrapI64();
  f.lget(3);
  f.lget(4);
  f.lget(5);
  f.call(arraySet);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.if_();
  emitFailStatic(f, sStringImmutable);
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.i64Eq();
  f.if_();
  f.lget(2);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sMapKeyString);
  f.end();
  f.lget(1);
  f.i32WrapI64();
  f.lget(3);
  f.i32WrapI64();
  f.lget(4);
  f.lget(5);
  f.call(mapSet);
  f.ret();
  f.end();
  emitFailStatic(f, sCannotAssignIndex);
  return index;
}


// `$slice(receiver, start, end, step)`: nil bounds are "not given", a
// negative bound counts from the end and the bounds are clamped the way
// Python clamps them, so `a[:]`, `a[-2:]`, `a[::-1]` and `s[1:len(s)-1]`
// share one path. A string slices by UTF-8 character.
std::uint32_t Runtime::emitSlice() {
  std::uint32_t type = m.typeFor(
      {kI64, kI64, kI64, kI64, kI64, kI64, kI64, kI64}, {kI64, kI64});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI64, kI64, kI64, kI64, kI64, kI64, kI64, kI64});
  std::uint32_t len = f.addLocal(kI64), from = f.addLocal(kI64),
                to = f.addLocal(kI64), step = f.addLocal(kI64),
                i = f.addLocal(kI64), lower = f.addLocal(kI64),
                upper = f.addLocal(kI64), hasStart = f.addLocal(kI32),
                hasEnd = f.addLocal(kI32), hasStep = f.addLocal(kI32),
                addr = f.addLocal(kI32), items = f.addLocal(kI32),
                itemAddr = f.addLocal(kI32), resTag = f.addLocal(kI64),
                resPay = f.addLocal(kI64), out = f.addLocal(kI32),
                cpay = f.addLocal(kI64);

  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kStr));
  f.i64Eq();
  f.i32Or();
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sCannotSlice);
  f.end();

  auto parseBound = [&](int tagParam, int payParam, std::uint32_t present,
                        std::uint32_t value, std::uint32_t message) {
    f.lget(tagParam);
    f.i64c(static_cast<std::int64_t>(kNil));
    f.i64Ne();
    f.lset(present);
    f.lget(present);
    f.if_();
    f.lget(tagParam);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.i32Eqz();
    f.if_();
    emitFailStatic(f, message);
    f.end();
    f.lget(payParam);
    f.lset(value);
    f.end();
  };
  parseBound(2, 3, hasStart, from, sSliceStartInt);
  parseBound(4, 5, hasEnd, to, sSliceEndInt);
  parseBound(6, 7, hasStep, step, sSliceStepInt);
  f.lget(hasStep);
  f.i32Eqz();
  f.if_();
  f.i64c(1);
  f.lset(step);
  f.end();
  f.lget(step);
  f.i64c(0);
  f.i64Eq();
  f.if_();
  emitFailStatic(f, sSliceStepZero);
  f.end();

  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.i64Load(kArrayLengthOffset);
  f.lset(len);
  f.else_();
  f.lget(1);
  f.i32WrapI64();
  f.call(utf8Len);
  f.lset(len);
  f.end();

  // lower/upper follow the step's direction; a missing bound is that side's
  // default rather than 0.
  f.lget(step);
  f.i64c(0);
  f.i64GtS();
  f.ifResult(kI64);
  f.i64c(0);
  f.else_();
  f.i64c(-1);
  f.end();
  f.lset(lower);
  f.lget(step);
  f.i64c(0);
  f.i64GtS();
  f.ifResult(kI64);
  f.lget(len);
  f.else_();
  f.lget(len);
  f.i64c(1);
  f.i64Sub();
  f.end();
  f.lset(upper);

  auto clamp = [&](std::uint32_t value, std::uint32_t present,
                   std::uint32_t result, bool isStart) {
    f.lget(present);
    f.i32Eqz();
    f.if_();
    f.lget(step);
    f.i64c(0);
    f.i64GtS();
    f.ifResult(kI64);
    f.lget(isStart ? lower : upper);
    f.else_();
    f.lget(isStart ? upper : lower);
    f.end();
    f.lset(result);
    f.else_();
    f.lget(value);
    f.i64c(0);
    f.i64LtS();
    f.if_();
    f.lget(value);
    f.lget(len);
    f.i64Add();
    f.lset(value);
    f.end();
    f.lget(value);
    f.lget(lower);
    f.i64LtS();
    f.if_();
    f.lget(lower);
    f.lset(value);
    f.end();
    f.lget(value);
    f.lget(upper);
    f.i64GtS();
    f.if_();
    f.lget(upper);
    f.lset(value);
    f.end();
    f.lget(value);
    f.lset(result);
    f.end();
  };
  clamp(from, hasStart, from, true);
  clamp(to, hasEnd, to, false);

  auto emitLoopCondition = [&]() {
    f.lget(step);
    f.i64c(0);
    f.i64GtS();
    f.ifResult(kI32);
    f.lget(i);
    f.lget(to);
    f.i64GeS();
    f.else_();
    f.lget(i);
    f.lget(to);
    f.i64LeS();
    f.end();
    f.brIf(1);
  };

  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kArray));
  f.i64Eq();
  f.if_();
  f.lget(len);
  f.call(newArray);
  f.lset(resPay);
  f.lset(resTag);
  f.lget(1);
  f.i32WrapI64();
  f.i64Load(kArrayItemsOffset);
  f.i32WrapI64();
  f.lset(items);
  f.lget(from);
  f.lset(i);
  f.block();
  f.loop();
  emitLoopCondition();
  f.lget(items);
  f.lget(i);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.lset(itemAddr);
  f.lget(resPay);
  f.i32WrapI64();
  f.lget(itemAddr);
  f.i64Load(0);
  f.lget(itemAddr);
  f.i64Load(8);
  f.call(arrayPush);
  f.lget(i);
  f.lget(step);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.lget(resTag);
  f.lget(resPay);
  f.ret();
  f.end();

  f.i32c(0);
  f.call(newStr);
  f.lset(out);
  f.lget(from);
  f.lset(i);
  f.block();
  f.loop();
  emitLoopCondition();
  f.i64c(static_cast<std::int64_t>(kStr));
  f.lget(1);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lget(i);
  f.call(indexGet);
  f.lset(cpay);
  f.drop();
  f.lget(out);
  f.lget(cpay);
  f.i32WrapI64();
  f.call(concat);
  f.lset(out);
  f.lget(i);
  f.lget(step);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.i64c(static_cast<std::int64_t>(kStr));
  f.lget(out);
  f.i64ExtendI32U();
  return index;
}


// String-to-number parsing for `int()`, `float()` and `double()`. The VM
// trims ASCII whitespace and rejects trailing junk, so these do the same.
std::uint32_t Runtime::emitParseIntStr() {
  std::uint32_t index = m.reserveFunction(m.typeFor({kI32}, {kI64}));
  Func f(m, index, {kI32});
  std::uint32_t p = f.addLocal(kI32), end = f.addLocal(kI32),
                b = f.addLocal(kI32), digits = f.addLocal(kI32),
                base = f.addLocal(kI32), sign = f.addLocal(kI64),
                acc = f.addLocal(kI64), digitVal = f.addLocal(kI64),
                limit = f.addLocal(kI64);

  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lset(p);
  f.lget(p);
  f.lget(0);
  f.i64Load(0);
  f.i32WrapI64();
  f.i32Add();
  f.lset(end);

  auto isSpace = [&]() {
    // b is on the stack; leave an i32 condition.
    f.lget(b);
    f.i32c(32);
    f.i32Eq();
    f.lget(b);
    f.i32c(9);
    f.i32Eq();
    f.i32Or();
    f.lget(b);
    f.i32c(10);
    f.i32Eq();
    f.i32Or();
    f.lget(b);
    f.i32c(13);
    f.i32Eq();
    f.i32Or();
  };
  f.block();
  f.loop();
  f.lget(p);
  f.lget(end);
  f.i32GeS();
  f.brIf(1);
  f.lget(p);
  f.i32Load8u(0);
  f.lset(b);
  isSpace();
  f.i32Eqz();
  f.brIf(1);
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.br(0);
  f.end();
  f.end();
  f.block();
  f.loop();
  f.lget(end);
  f.lget(p);
  f.i32LeS();
  f.brIf(1);
  f.lget(end);
  f.i32c(1);
  f.i32Sub();
  f.i32Load8u(0);
  f.lset(b);
  isSpace();
  f.i32Eqz();
  f.brIf(1);
  f.lget(end);
  f.i32c(1);
  f.i32Sub();
  f.lset(end);
  f.br(0);
  f.end();
  f.end();

  f.i64c(1);
  f.lset(sign);
  f.lget(p);
  f.lget(end);
  f.i32LtS();
  f.if_();
  f.lget(p);
  f.i32Load8u(0);
  f.lset(b);
  f.lget(b);
  f.i32c(43);
  f.i32Eq();
  f.if_();
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.else_();
  f.lget(b);
  f.i32c(45);
  f.i32Eq();
  f.if_();
  f.i64c(-1);
  f.lset(sign);
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.end();
  f.end();
  f.end();

  // `strtoll` accepts `0x`/`0b`/`0o` prefixes; keep the same spellings.
  f.i32c(10);
  f.lset(base);
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lget(end);
  f.i32LtS();
  f.if_();
  f.lget(p);
  f.i32Load8u(0);
  f.i32c(48);
  f.i32Eq();
  f.if_();
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.i32Load8u(0);
  f.lset(b);
  f.lget(b);
  f.i32c(120);
  f.i32Eq();
  f.lget(b);
  f.i32c(88);
  f.i32Eq();
  f.i32Or();
  f.if_();
  f.i32c(16);
  f.lset(base);
  f.lget(p);
  f.i32c(2);
  f.i32Add();
  f.lset(p);
  f.else_();
  f.lget(b);
  f.i32c(98);
  f.i32Eq();
  f.lget(b);
  f.i32c(66);
  f.i32Eq();
  f.i32Or();
  f.if_();
  f.i32c(2);
  f.lset(base);
  f.lget(p);
  f.i32c(2);
  f.i32Add();
  f.lset(p);
  f.else_();
  f.lget(b);
  f.i32c(111);
  f.i32Eq();
  f.lget(b);
  f.i32c(79);
  f.i32Eq();
  f.i32Or();
  f.if_();
  f.i32c(8);
  f.lset(base);
  f.lget(p);
  f.i32c(2);
  f.i32Add();
  f.lset(p);
  f.end();
  f.end();
  f.end();
  f.end();
  f.end();

  // The magnitude limit is INT64_MAX for a positive number and 2^63 for a
  // negative one (whose bits are INT64_MIN).
  f.lget(sign);
  f.i64c(0);
  f.i64GtS();
  f.ifResult(kI64);
  f.i64c(std::numeric_limits<std::int64_t>::max());
  f.else_();
  f.i64c(std::numeric_limits<std::int64_t>::min());
  f.end();
  f.lset(limit);

  f.i64c(0);
  f.lset(acc);
  f.i32c(0);
  f.lset(digits);
  f.block();
  f.loop();
  f.lget(p);
  f.lget(end);
  f.i32GeS();
  f.brIf(1);
  f.lget(p);
  f.i32Load8u(0);
  f.lset(b);
  f.lget(b);
  f.i32c(48);
  f.i32LtU();
  f.lget(b);
  f.i32c(57);
  f.i32GtU();
  f.i32Or();
  f.lget(b);
  f.i32c(97);
  f.i32LtU();
  f.lget(b);
  f.i32c(102);
  f.i32GtU();
  f.i32Or();
  f.i32And();
  f.lget(b);
  f.i32c(65);
  f.i32LtU();
  f.lget(b);
  f.i32c(70);
  f.i32GtU();
  f.i32Or();
  f.i32And();
  f.if_();
  emitFailStatic(f, sIntString);
  f.end();
  f.lget(b);
  f.i32c(58);
  f.i32GeU();
  f.if_();
  f.lget(b);
  f.i32c(97);
  f.i32LtU();
  f.if_();
  f.lget(b);
  f.i32c(65);
  f.i32Sub();
  f.i32c(10);
  f.i32Add();
  f.i64ExtendI32U();
  f.lset(digitVal);
  f.else_();
  f.lget(b);
  f.i32c(97);
  f.i32Sub();
  f.i32c(10);
  f.i32Add();
  f.i64ExtendI32U();
  f.lset(digitVal);
  f.end();
  f.else_();
  f.lget(b);
  f.i32c(48);
  f.i32Sub();
  f.i64ExtendI32U();
  f.lset(digitVal);
  f.end();
  f.lget(digitVal);
  f.lget(base);
  f.i64ExtendI32U();
  f.i64GeU();
  f.if_();
  emitFailStatic(f, sIntString);
  f.end();
  f.lget(acc);
  f.lget(limit);
  f.lget(digitVal);
  f.i64Sub();
  f.lget(base);
  f.i64ExtendI32U();
  f.i64DivU();
  f.i64GtU();
  f.if_();
  emitFailStatic(f, sIntString);
  f.end();
  f.lget(acc);
  f.lget(base);
  f.i64ExtendI32U();
  f.i64Mul();
  f.lget(digitVal);
  f.i64Add();
  f.lset(acc);
  f.lget(digits);
  f.i32c(1);
  f.i32Add();
  f.lset(digits);
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.br(0);
  f.end();
  f.end();
  f.lget(digits);
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sIntString);
  f.end();
  f.lget(acc);
  f.lget(sign);
  f.i64Mul();
  return index;
}

std::uint32_t Runtime::emitParseDoubleStr() {
  std::uint32_t index = m.reserveFunction(m.typeFor({kI32}, {kF64}));
  Func f(m, index, {kI32});
  std::uint32_t p = f.addLocal(kI32), end = f.addLocal(kI32),
                b = f.addLocal(kI32), digits = f.addLocal(kI32),
                expDigits = f.addLocal(kI32), exp = f.addLocal(kI64),
                expSign = f.addLocal(kI64), value = f.addLocal(kF64),
                scale = f.addLocal(kF64), sign = f.addLocal(kF64),
                fracMantissa = f.addLocal(kI64), fracDigits = f.addLocal(kI32),
                extra = f.addLocal(kI64);

  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lset(p);
  f.lget(p);
  f.lget(0);
  f.i64Load(0);
  f.i32WrapI64();
  f.i32Add();
  f.lset(end);
  auto isSpace = [&]() {
    f.lget(b);
    f.i32c(32);
    f.i32Eq();
    f.lget(b);
    f.i32c(9);
    f.i32Eq();
    f.i32Or();
    f.lget(b);
    f.i32c(10);
    f.i32Eq();
    f.i32Or();
    f.lget(b);
    f.i32c(13);
    f.i32Eq();
    f.i32Or();
  };
  f.block();
  f.loop();
  f.lget(p);
  f.lget(end);
  f.i32GeS();
  f.brIf(1);
  f.lget(p);
  f.i32Load8u(0);
  f.lset(b);
  isSpace();
  f.i32Eqz();
  f.brIf(1);
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.br(0);
  f.end();
  f.end();
  f.block();
  f.loop();
  f.lget(end);
  f.lget(p);
  f.i32LeS();
  f.brIf(1);
  f.lget(end);
  f.i32c(1);
  f.i32Sub();
  f.i32Load8u(0);
  f.lset(b);
  isSpace();
  f.i32Eqz();
  f.brIf(1);
  f.lget(end);
  f.i32c(1);
  f.i32Sub();
  f.lset(end);
  f.br(0);
  f.end();
  f.end();

  f.f64c(1.0);
  f.lset(sign);
  f.lget(p);
  f.lget(end);
  f.i32LtS();
  f.if_();
  f.lget(p);
  f.i32Load8u(0);
  f.lset(b);
  f.lget(b);
  f.i32c(43);
  f.i32Eq();
  f.if_();
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.else_();
  f.lget(b);
  f.i32c(45);
  f.i32Eq();
  f.if_();
  f.f64c(-1.0);
  f.lset(sign);
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.end();
  f.end();
  f.end();

  f.f64c(0.0);
  f.lset(value);
  f.i32c(0);
  f.lset(digits);
  f.block();
  f.loop();
  f.lget(p);
  f.lget(end);
  f.i32GeS();
  f.brIf(1);
  f.lget(p);
  f.i32Load8u(0);
  f.lset(b);
  f.lget(b);
  f.i32c(48);
  f.i32LtU();
  f.lget(b);
  f.i32c(57);
  f.i32GtU();
  f.i32Or();
  f.brIf(1);
  f.lget(value);
  f.f64c(10.0);
  f.f64Mul();
  f.lget(b);
  f.i32c(48);
  f.i32Sub();
  f.f64ConvertI32U();
  f.f64Add();
  f.lset(value);
  f.lget(digits);
  f.i32c(1);
  f.i32Add();
  f.lset(digits);
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.br(0);
  f.end();
  f.end();

  f.lget(p);
  f.lget(end);
  f.i32LtS();
  f.if_();
  f.lget(p);
  f.i32Load8u(0);
  f.i32c(46);
  f.i32Eq();
  f.if_();
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.i64c(0);
  f.lset(fracMantissa);
  f.i32c(0);
  f.lset(fracDigits);
  f.block();
  f.loop();
  f.lget(p);
  f.lget(end);
  f.i32GeS();
  f.brIf(1);
  f.lget(p);
  f.i32Load8u(0);
  f.lset(b);
  f.lget(b);
  f.i32c(48);
  f.i32LtU();
  f.lget(b);
  f.i32c(57);
  f.i32GtU();
  f.i32Or();
  f.brIf(1);
  f.lget(fracMantissa);
  f.i64c(10);
  f.i64Mul();
  f.lget(b);
  f.i32c(48);
  f.i32Sub();
  f.i64ExtendI32U();
  f.i64Add();
  f.lset(fracMantissa);
  f.lget(fracDigits);
  f.i32c(1);
  f.i32Add();
  f.lset(fracDigits);
  f.lget(digits);
  f.i32c(1);
  f.i32Add();
  f.lset(digits);
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.br(0);
  f.end();
  f.end();
  f.f64c(1.0);
  f.lset(scale);
  f.i32c(0);
  f.lset(expDigits);
  f.block();
  f.loop();
  f.lget(expDigits);
  f.lget(fracDigits);
  f.i32GeS();
  f.brIf(1);
  f.lget(scale);
  f.f64c(10.0);
  f.f64Mul();
  f.lset(scale);
  f.lget(expDigits);
  f.i32c(1);
  f.i32Add();
  f.lset(expDigits);
  f.br(0);
  f.end();
  f.end();
  f.lget(value);
  f.lget(fracMantissa);
  f.f64ConvertI64S();
  f.lget(scale);
  f.f64Div();
  f.f64Add();
  f.lset(value);
  f.end();
  f.end();

  f.lget(digits);
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sFloatString);
  f.end();

  f.lget(p);
  f.lget(end);
  f.i32LtS();
  f.if_();
  f.lget(p);
  f.i32Load8u(0);
  f.lset(b);
  f.lget(b);
  f.i32c(101);
  f.i32Eq();
  f.lget(b);
  f.i32c(69);
  f.i32Eq();
  f.i32Or();
  f.if_();
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.i64c(1);
  f.lset(expSign);
  f.lget(p);
  f.lget(end);
  f.i32LtS();
  f.if_();
  f.lget(p);
  f.i32Load8u(0);
  f.lset(b);
  f.lget(b);
  f.i32c(43);
  f.i32Eq();
  f.if_();
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.else_();
  f.lget(b);
  f.i32c(45);
  f.i32Eq();
  f.if_();
  f.i64c(-1);
  f.lset(expSign);
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.end();
  f.end();
  f.end();
  f.i64c(0);
  f.lset(exp);
  f.i32c(0);
  f.lset(expDigits);
  f.block();
  f.loop();
  f.lget(p);
  f.lget(end);
  f.i32GeS();
  f.brIf(1);
  f.lget(p);
  f.i32Load8u(0);
  f.lset(b);
  f.lget(b);
  f.i32c(48);
  f.i32LtU();
  f.lget(b);
  f.i32c(57);
  f.i32GtU();
  f.i32Or();
  f.brIf(1);
  f.lget(exp);
  f.i64c(10);
  f.i64Mul();
  f.lget(b);
  f.i32c(48);
  f.i32Sub();
  f.i64ExtendI32U();
  f.i64Add();
  f.lset(exp);
  f.lget(expDigits);
  f.i32c(1);
  f.i32Add();
  f.lset(expDigits);
  f.lget(p);
  f.i32c(1);
  f.i32Add();
  f.lset(p);
  f.br(0);
  f.end();
  f.end();
  f.lget(expDigits);
  f.i32Eqz();
  f.if_();
  emitFailStatic(f, sFloatString);
  f.end();
  f.i64c(0);
  f.lset(extra);
  f.lget(exp);
  f.i64c(308);
  f.i64GtS();
  f.if_();
  f.lget(exp);
  f.i64c(308);
  f.i64Sub();
  f.lset(extra);
  f.i64c(308);
  f.lset(exp);
  f.end();
  f.lget(expSign);
  f.i64c(0);
  f.i64GtS();
  f.if_();
  f.lget(value);
  f.i32c(static_cast<std::int32_t>(pow10Table));
  f.lget(exp);
  f.i32WrapI64();
  f.i32c(324);
  f.i32Add();
  f.i32c(3);
  f.i32Shl();
  f.i32Add();
  f.f64Load(0);
  f.f64Mul();
  f.lset(value);
  f.else_();
  f.lget(value);
  f.i32c(static_cast<std::int32_t>(pow10Table));
  f.lget(exp);
  f.i32WrapI64();
  f.i32c(324);
  f.i32Add();
  f.i32c(3);
  f.i32Shl();
  f.i32Add();
  f.f64Load(0);
  f.f64Div();
  f.lset(value);
  f.end();
  f.block();
  f.loop();
  f.lget(extra);
  f.i64c(0);
  f.i64LeS();
  f.brIf(1);
  f.lget(expSign);
  f.i64c(0);
  f.i64GtS();
  f.if_();
  f.lget(value);
  f.f64c(10.0);
  f.f64Mul();
  f.lset(value);
  f.else_();
  f.lget(value);
  f.f64c(10.0);
  f.f64Div();
  f.lset(value);
  f.end();
  f.lget(extra);
  f.i64c(1);
  f.i64Sub();
  f.lset(extra);
  f.br(0);
  f.end();
  f.end();
  f.end();
  f.end();

  f.lget(p);
  f.lget(end);
  f.i32Ne();
  f.if_();
  emitFailStatic(f, sFloatString);
  f.end();
  f.lget(value);
  f.lget(sign);
  f.f64Mul();
  return index;
}

void Runtime::emitCore() {
  alloc = emitAlloc();
  writeStr = emitWriteStr();
  fail = emitFail();
  utf8Len = emitUtf8Len();
  newStr = emitNewStr();
  concat = emitConcat();
  strCmp = emitStrCmp();
  fmtInt = emitFmtInt();
  parseIntStr = emitParseIntStr();
  parseDoubleStr = emitParseDoubleStr();
  fmtDouble = emitFmtDouble();
  fmtFunc = emitFmtFunc();
  fmtRange = emitFmtRange();
  quote = emitQuote();
  // The two renderers call each other, so both indices are reserved before
  // either body is emitted.
  toStrDepth = m.reserveFunction(tToStrDepth);
  reprDepth = m.reserveFunction(tToStrDepth);
  emitToStrDepthBody(toStrDepth);
  emitReprDepthBody(reprDepth);
  toStr = emitToStr();
  reprStr = emitReprStr();
  truthy = emitTruthy();
  iterStep = emitIterStep();
  typeName = emitTypeName();
  newArray = emitNewArray();
  newMap = emitNewMap();
  arrayPush = emitArrayPush();
  arrayGet = emitArrayGet();
  arraySet = emitArraySet();
  mapFind = emitMapFind();
  mapSet = emitMapSet();
  mapGet = emitMapGet();
  mapDelete = emitMapDelete();
  mapClear = emitMapClear();
  equals = emitEquals();
  opAdd = emitBinary(kBinAdd, 0, "");
  opSub = emitBinary(kBinSub, 0, "");
  opMul = emitBinary(kBinMul, 0, "");
  opDiv = emitBinary(kBinDiv, 0, "");
  opMod = emitBinary(kBinMod, 0, "");
  opPow = emitBinary(kBinPow, 0, "");
  opBand = emitBinary(kBinBand, 0, "");
  opBor = emitBinary(kBinBor, 0, "");
  opBxor = emitBinary(kBinBxor, 0, "");
  opShl = emitBinary(kBinShl, 0, "");
  opShr = emitBinary(kBinShr, 0, "");
  opEq = emitCompare(0);
  opLt = emitCompare(1);
  opIn = emitIn();
  utf8ByteOffset = emitUtf8ByteOffset();
  indexGet = emitIndexGet();
  indexSet = emitIndexSet();
  slice = emitSlice();
  memberGet = emitMemberGet();
  memberSet = emitMemberSet();
  unNeg = emitUnary(0);
  unNot = emitUnary(1);
  unBitNot = emitUnary(2);
  if (randomStateSlot >= 0) {
    randomSeedFn = emitRandomSeed();
    randomTwistFn = emitRandomTwist();
    randomNextFn = emitRandomNext();
    randomState = emitRandomState();
  }
}

// --- mt19937_64 (PHASE 9.3) --------------------------------------------------
//
// The generator is emitted as ordinary WASM functions, exactly like the x86-64
// runtime emits it as machine code: the standard fully specifies the bit
// stream, so `random()`/`seed()` reproduce the VM's seeded sequence. The state
// is a 312-word block plus a cursor, allocated from the arena and cached in the
// hidden `$randomState` global's payload.

std::uint32_t Runtime::emitRandomSeed() {
  std::uint32_t type = m.typeFor({kI32, kI64}, {});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32, kI64});
  std::uint32_t k = f.addLocal(kI32), prev = f.addLocal(kI64),
                val = f.addLocal(kI64);

  f.lget(0);
  f.lget(1);
  f.i64Store(0);  // s[0] = seed
  f.i32c(1);
  f.lset(k);
  f.block();
  f.loop();
  f.lget(k);
  f.i32c(static_cast<std::int32_t>(kMtWords));
  f.i32GeS();
  f.brIf(1);
  // prev = s[k-1]
  f.lget(0);
  f.lget(k);
  f.i32c(1);
  f.i32Sub();
  f.i32c(8);
  f.i32Mul();
  f.i32Add();
  f.i64Load(0);
  f.lset(prev);
  // s[k] = 6364136223846793005 * (prev ^ (prev >> 62)) + k
  f.lget(prev);
  f.lget(prev);
  f.i64c(62);
  f.i64ShrU();
  f.i64Xor();
  f.i64c(static_cast<std::int64_t>(6364136223846793005ULL));
  f.i64Mul();
  f.lget(k);
  f.i64ExtendI32S();
  f.i64Add();
  f.lset(val);
  f.lget(0);
  f.lget(k);
  f.i32c(8);
  f.i32Mul();
  f.i32Add();
  f.lget(val);
  f.i64Store(0);
  f.lget(k);
  f.i32c(1);
  f.i32Add();
  f.lset(k);
  f.br(0);
  f.end();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kMtWords));
  f.i64Store(kMtCursorOffset);  // the first draw twists
  return index;
}

std::uint32_t Runtime::emitRandomTwist() {
  std::uint32_t type = m.typeFor({kI32}, {});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32});
  std::uint32_t k = f.addLocal(kI32), succ = f.addLocal(kI32),
                mid = f.addLocal(kI32), x = f.addLocal(kI64),
                xa = f.addLocal(kI64);

  f.i32c(0);
  f.lset(k);
  f.block();
  f.loop();
  f.lget(k);
  f.i32c(static_cast<std::int32_t>(kMtWords));
  f.i32GeS();
  f.brIf(1);
  // x = (s[k] & UM) | (s[(k+1) % N] & LM)
  f.lget(0);
  f.lget(k);
  f.i32c(8);
  f.i32Mul();
  f.i32Add();
  f.i64Load(0);
  f.i64c(static_cast<std::int64_t>(0xFFFFFFFF80000000ULL));
  f.i64And();
  f.lset(x);
  f.lget(k);
  f.i32c(1);
  f.i32Add();
  f.lset(succ);
  f.lget(succ);
  f.i32c(static_cast<std::int32_t>(kMtWords));
  f.i32GeS();
  f.if_();
  f.i32c(0);
  f.lset(succ);
  f.end();
  f.lget(0);
  f.lget(succ);
  f.i32c(8);
  f.i32Mul();
  f.i32Add();
  f.i64Load(0);
  f.i64c(0x7FFFFFFF);
  f.i64And();
  f.lget(x);
  f.i64Or();
  f.lset(x);
  // xa = x >> 1; if (x & 1) xa ^= MATRIX_A.
  f.lget(x);
  f.i64c(1);
  f.i64ShrU();
  f.lset(xa);
  f.lget(x);
  f.i64c(1);
  f.i64And();
  f.i64Eqz();
  f.i32Eqz();
  f.if_();
  f.lget(xa);
  f.i64c(static_cast<std::int64_t>(0xB5026F5AA96619E9ULL));
  f.i64Xor();
  f.lset(xa);
  f.end();
  // s[k] = s[(k + M) % N] ^ xa.
  f.lget(k);
  f.i32c(static_cast<std::int32_t>(kMtMiddleWord));
  f.i32Add();
  f.lset(mid);
  f.lget(mid);
  f.i32c(static_cast<std::int32_t>(kMtWords));
  f.i32GeS();
  f.if_();
  f.lget(mid);
  f.i32c(static_cast<std::int32_t>(kMtWords));
  f.i32Sub();
  f.lset(mid);
  f.end();
  f.lget(0);
  f.lget(k);
  f.i32c(8);
  f.i32Mul();
  f.i32Add();
  f.lget(0);
  f.lget(mid);
  f.i32c(8);
  f.i32Mul();
  f.i32Add();
  f.i64Load(0);
  f.lget(xa);
  f.i64Xor();
  f.i64Store(0);
  f.lget(k);
  f.i32c(1);
  f.i32Add();
  f.lset(k);
  f.br(0);
  f.end();
  f.end();
  f.lget(0);
  f.i64c(0);
  f.i64Store(kMtCursorOffset);
  return index;
}

std::uint32_t Runtime::emitRandomNext() {
  std::uint32_t type = m.typeFor({kI32}, {kI64});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32});
  std::uint32_t cursor = f.addLocal(kI32), y = f.addLocal(kI64);

  f.lget(0);
  f.i64Load(kMtCursorOffset);
  f.i32WrapI64();
  f.lset(cursor);
  f.lget(cursor);
  f.i32c(static_cast<std::int32_t>(kMtWords));
  f.i32GeS();
  f.if_();
  f.lget(0);
  f.call(randomTwistFn);
  f.end();
  f.lget(0);
  f.i64Load(kMtCursorOffset);
  f.i32WrapI64();
  f.lset(cursor);
  // y = s[cursor]
  f.lget(0);
  f.lget(cursor);
  f.i32c(8);
  f.i32Mul();
  f.i32Add();
  f.i64Load(0);
  f.lset(y);
  f.lget(0);
  f.lget(cursor);
  f.i32c(1);
  f.i32Add();
  f.i64ExtendI32S();
  f.i64Store(kMtCursorOffset);
  // The standard's tempering.
  f.lget(y);
  f.lget(y);
  f.i64c(29);
  f.i64ShrU();
  f.i64c(static_cast<std::int64_t>(0x5555555555555555ULL));
  f.i64And();
  f.i64Xor();
  f.lset(y);
  f.lget(y);
  f.lget(y);
  f.i64c(17);
  f.i64Shl();
  f.i64c(static_cast<std::int64_t>(0x71D67FFFEDA60000ULL));
  f.i64And();
  f.i64Xor();
  f.lset(y);
  f.lget(y);
  f.lget(y);
  f.i64c(37);
  f.i64Shl();
  f.i64c(static_cast<std::int64_t>(0xFFF7EEE000000000ULL));
  f.i64And();
  f.i64Xor();
  f.lset(y);
  f.lget(y);
  f.lget(y);
  f.i64c(43);
  f.i64ShrU();
  f.i64Xor();
  return index;
}

std::uint32_t Runtime::emitRandomState() {
  std::uint32_t type = m.typeFor({}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {});
  std::uint32_t ptr = f.addLocal(kI32);
  const std::uint32_t slot = static_cast<std::uint32_t>(randomStateSlot * 16);

  f.i32c(static_cast<std::int32_t>(slot + 8));
  f.i64Load(0);
  f.i32WrapI64();
  f.lset(ptr);
  f.lget(ptr);
  f.i32Eqz();
  f.if_();
  f.i32c(static_cast<std::int32_t>(kMtStateTotal));
  f.call(alloc);
  f.lset(ptr);
  f.i32c(static_cast<std::int32_t>(slot + 8));
  f.lget(ptr);
  f.i64ExtendI32U();
  f.i64Store(0);
  f.lget(ptr);
  f.i64c(static_cast<std::int64_t>(kMtDefaultSeed));
  f.call(randomSeedFn);
  f.end();
  f.lget(ptr);
  return index;
}

std::uint32_t Runtime::emitTrapBuiltin(const std::string& name) {
  std::uint32_t message = pool.addString(
      "builtin '" + name +
      "' is not supported by the WebAssembly backend yet (PHASE 9)");
  std::uint32_t index = m.reserveFunction(tValue);
  Func f(m, index, {kI32, kI32, kI32});
  emitFailStatic(f, message);
  return index;
}

std::uint32_t Runtime::emitBuiltin(const std::string& name) {
  std::uint32_t index = m.reserveFunction(tValue);
  Func f(m, index, {kI32, kI32, kI32});

  auto argAddr = [&](int i) {
    f.lget(0);
    f.i32c(16 * i);
    f.i32Add();
  };
  auto argTag = [&](int i) {
    argAddr(i);
    f.i64Load(0);
  };
  auto argPayload = [&](int i) {
    argAddr(i);
    f.i64Load(8);
  };
  auto arity = [&](int n) {
    f.lget(1);
    f.i32c(n);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
  };
  auto arityBetween = [&](int lo, int hi) {
    f.lget(1);
    f.i32c(lo);
    f.i32LtS();
    f.lget(1);
    f.i32c(hi);
    f.i32GtS();
    f.i32Or();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
  };
  auto retNil = [&]() {
    f.i64c(static_cast<std::int64_t>(kNil));
    f.i64c(0);
  };
  auto failArg = [&](int i) {
    argAddr(i);
    f.i64Load(8);
    f.i32WrapI64();
    f.i32c(8);
    f.i32Add();
    argAddr(i);
    f.i64Load(8);
    f.i32WrapI64();
    f.i64Load(0);
    f.i32WrapI64();
    f.call(fail);
    f.unreachable();
  };
  auto failMaybeArg = [&](int i, std::uint32_t fallback) {
    f.lget(1);
    f.i32c(i + 1);
    f.i32GeS();
    f.if_();
    argTag(i);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Eq();
    f.if_();
    failArg(i);
    f.else_();
    emitFailStatic(f, fallback);
    f.end();
    f.else_();
    emitFailStatic(f, fallback);
    f.end();
  };
  auto toF64Arg = [&](int i) {
    argTag(i);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.ifResult(kF64);
    argPayload(i);
    f.f64ConvertI64S();
    f.else_();
    argPayload(i);
    f.f64ReinterpretI64();
    f.end();
  };

  if (name == "print" || name == "write" || name == "eprint") {
    const int fd = name == "eprint" ? 2 : 1;
    std::uint32_t i = f.addLocal(kI32);
    f.i32c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(1);
    f.i32GeS();
    f.brIf(1);
    f.lget(i);
    f.i32c(0);
    f.i32GtS();
    f.if_();
    f.i32c(fd);
    f.i32c(static_cast<std::int32_t>(sSpace));
    f.call(writeStr);
    f.end();
    f.i32c(fd);
    f.lget(0);
    f.lget(i);
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lget(0);
    f.lget(i);
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.call(toStr);
    f.call(writeStr);
    f.lget(i);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    if (name != "write") {
      f.i32c(fd);
      f.i32c(static_cast<std::int32_t>(sNewline));
      f.call(writeStr);
    }
    retNil();
    return index;
  }

  if (name == "len") {
    arity(1);
    std::uint32_t addr = f.addLocal(kI32), start = f.addLocal(kI64), end = f.addLocal(kI64),
                  incl = f.addLocal(kI32);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Eq();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kInt));
    argPayload(0);
    f.i32WrapI64();
    f.call(utf8Len);
    f.ret();
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kRange));
    f.i64Eq();
    f.if_();
    argPayload(0);
    f.i32WrapI64();
    f.lset(addr);
    f.lget(addr);
    f.i64Load(0);
    f.lset(start);
    f.lget(addr);
    f.i64Load(8);
    f.lset(end);
    f.lget(addr);
    f.i64Load(16);
    f.i32WrapI64();
    f.lset(incl);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(end);
    f.lget(start);
    f.i64Sub();
    f.lget(start);
    f.lget(end);
    f.i64Sub();
    f.lget(end);
    f.lget(start);
    f.i64GeS();
    f.select();
    f.lget(incl);
    f.i64ExtendI32U();
    f.i64Add();
    f.ret();
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kArray));
    f.i64Eq();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kMap));
    f.i64Eq();
    f.i32Or();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kInt));
    argPayload(0);
    f.i32WrapI64();
    f.i64Load(0);
    f.ret();
    f.end();
    emitFailStatic(f, sLenType);
    return index;
  }

  if (name == "type") {
    arity(1);
    f.i64c(static_cast<std::int64_t>(kStr));
    argTag(0);
    f.call(typeName);
    f.i64ExtendI32U();
    return index;
  }

  if (name == "str" || name == "$str") {
    arity(1);
    f.i64c(static_cast<std::int64_t>(kStr));
    argTag(0);
    argPayload(0);
    f.call(toStr);
    f.i64ExtendI32U();
    return index;
  }

  if (name == "repr") {
    arity(1);
    f.i64c(static_cast<std::int64_t>(kStr));
    argTag(0);
    argPayload(0);
    f.call(reprStr);
    f.i64ExtendI32U();
    return index;
  }

  if (name == "bool") {
    arity(1);
    f.i64c(static_cast<std::int64_t>(kBool));
    argTag(0);
    argPayload(0);
    f.call(truthy);
    f.i64ExtendI32U();
    return index;
  }

  if (name == "int") {
    arity(1);
    std::uint32_t d = f.addLocal(kF64);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.if_();
    argTag(0);
    argPayload(0);
    f.ret();
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kBool));
    f.i64Eq();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kInt));
    argPayload(0);
    f.ret();
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kFloat));
    f.i64Eq();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kDouble));
    f.i64Eq();
    f.i32Or();
    f.if_();
    toF64Arg(0);
    f.lset(d);
    f.lget(d);
    f.lget(d);
    f.f64Ne();
    f.if_();
    emitFailStatic(f, sIntNonFinite);
    f.end();
    f.lget(d);
    f.f64Abs();
    f.f64c(std::numeric_limits<double>::infinity());
    f.f64Eq();
    f.if_();
    emitFailStatic(f, sIntNonFinite);
    f.end();
    f.lget(d);
    f.f64c(-9.2233720368547758e18);
    f.f64Lt();
    f.lget(d);
    f.f64c(9.2233720368547758e18);
    f.f64Ge();
    f.i32Or();
    f.if_();
    emitFailStatic(f, sIntRange);
    f.end();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(d);
    f.i64TruncSatF64S();
    f.ret();
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Eq();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kInt));
    argPayload(0);
    f.i32WrapI64();
    f.call(parseIntStr);
    f.ret();
    f.end();
    emitFailStatic(f, sUnsupportedOperands);
    return index;
  }

  if (name == "float") {
    arity(1);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kFloat));
    f.i64Eq();
    f.if_();
    argTag(0);
    argPayload(0);
    f.ret();
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Eq();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kFloat));
    argPayload(0);
    f.i32WrapI64();
    f.call(parseDoubleStr);
    f.f32DemoteF64();
    f.f64PromoteF32();
    f.i64ReinterpretF64();
    f.ret();
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kBool));
    f.i64Eq();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kFloat));
    argPayload(0);
    f.f64ConvertI64S();
    f.f32DemoteF64();
    f.f64PromoteF32();
    f.i64ReinterpretF64();
    f.ret();
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kDouble));
    f.i64Eq();
    f.i32Or();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kFloat));
    toF64Arg(0);
    f.f32DemoteF64();
    f.f64PromoteF32();
    f.i64ReinterpretF64();
    f.ret();
    f.end();
    emitFailStatic(f, sUnsupportedOperands);
    return index;
  }

  if (name == "double") {
    arity(1);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kDouble));
    f.i64Eq();
    f.if_();
    argTag(0);
    argPayload(0);
    f.ret();
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Eq();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kDouble));
    argPayload(0);
    f.i32WrapI64();
    f.call(parseDoubleStr);
    f.i64ReinterpretF64();
    f.ret();
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kBool));
    f.i64Eq();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kDouble));
    argPayload(0);
    f.f64ConvertI64S();
    f.i64ReinterpretF64();
    f.ret();
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kFloat));
    f.i64Eq();
    f.i32Or();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kDouble));
    toF64Arg(0);
    f.i64ReinterpretF64();
    f.ret();
    f.end();
    emitFailStatic(f, sUnsupportedOperands);
    return index;
  }

  if (name == "chr") {
    arity(1);
    std::uint32_t cp = f.addLocal(kI64), len = f.addLocal(kI32), s = f.addLocal(kI32);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.i32Eqz();
    f.if_();
    emitFailStatic(f, sChrRange);
    f.end();
    argPayload(0);
    f.lset(cp);
    f.lget(cp);
    f.i64c(0);
    f.i64LtS();
    f.lget(cp);
    f.i64c(0x10FFFF);
    f.i64GtS();
    f.i32Or();
    f.if_();
    emitFailStatic(f, sChrRange);
    f.end();
    f.lget(cp);
    f.i64c(0xD800);
    f.i64GeS();
    f.lget(cp);
    f.i64c(0xDFFF);
    f.i64LeS();
    f.i32And();
    f.if_();
    emitFailStatic(f, sChrSurrogate);
    f.end();
    f.lget(cp);
    f.i64c(0x80);
    f.i64LtS();
    f.ifResult(kI32);
    f.i32c(1);
    f.else_();
    f.lget(cp);
    f.i64c(0x800);
    f.i64LtS();
    f.ifResult(kI32);
    f.i32c(2);
    f.else_();
    f.lget(cp);
    f.i64c(0x10000);
    f.i64LtS();
    f.ifResult(kI32);
    f.i32c(3);
    f.else_();
    f.i32c(4);
    f.end();
    f.end();
    f.end();
    f.lset(len);
    f.lget(len);
    f.call(newStr);
    f.lset(s);
    f.lget(len);
    f.i32c(1);
    f.i32Eq();
    f.if_();
    f.lget(s);
    f.i32c(8);
    f.i32Add();
    f.lget(cp);
    f.i32WrapI64();
    f.i32Store8(0);
    f.else_();
    f.lget(len);
    f.i32c(2);
    f.i32Eq();
    f.if_();
    f.lget(s);
    f.i32c(8);
    f.i32Add();
    f.lget(cp);
    f.i64c(6);
    f.i64ShrU();
    f.i32WrapI64();
    f.i32c(0xC0);
    f.i32Or();
    f.i32Store8(0);
    f.lget(s);
    f.i32c(9);
    f.i32Add();
    f.lget(cp);
    f.i32WrapI64();
    f.i32c(0x3F);
    f.i32And();
    f.i32c(0x80);
    f.i32Or();
    f.i32Store8(0);
    f.else_();
    f.lget(len);
    f.i32c(3);
    f.i32Eq();
    f.if_();
    f.lget(s);
    f.i32c(8);
    f.i32Add();
    f.lget(cp);
    f.i64c(12);
    f.i64ShrU();
    f.i32WrapI64();
    f.i32c(0xE0);
    f.i32Or();
    f.i32Store8(0);
    f.lget(s);
    f.i32c(9);
    f.i32Add();
    f.lget(cp);
    f.i64c(6);
    f.i64ShrU();
    f.i32WrapI64();
    f.i32c(0x3F);
    f.i32And();
    f.i32c(0x80);
    f.i32Or();
    f.i32Store8(0);
    f.lget(s);
    f.i32c(10);
    f.i32Add();
    f.lget(cp);
    f.i32WrapI64();
    f.i32c(0x3F);
    f.i32And();
    f.i32c(0x80);
    f.i32Or();
    f.i32Store8(0);
    f.else_();
    f.lget(s);
    f.i32c(8);
    f.i32Add();
    f.lget(cp);
    f.i64c(18);
    f.i64ShrU();
    f.i32WrapI64();
    f.i32c(0xF0);
    f.i32Or();
    f.i32Store8(0);
    f.lget(s);
    f.i32c(9);
    f.i32Add();
    f.lget(cp);
    f.i64c(12);
    f.i64ShrU();
    f.i32WrapI64();
    f.i32c(0x3F);
    f.i32And();
    f.i32c(0x80);
    f.i32Or();
    f.i32Store8(0);
    f.lget(s);
    f.i32c(10);
    f.i32Add();
    f.lget(cp);
    f.i64c(6);
    f.i64ShrU();
    f.i32WrapI64();
    f.i32c(0x3F);
    f.i32And();
    f.i32c(0x80);
    f.i32Or();
    f.i32Store8(0);
    f.lget(s);
    f.i32c(11);
    f.i32Add();
    f.lget(cp);
    f.i32WrapI64();
    f.i32c(0x3F);
    f.i32And();
    f.i32c(0x80);
    f.i32Or();
    f.i32Store8(0);
    f.end();
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(s);
    f.i64ExtendI32U();
    return index;
  }

  if (name == "ord") {
    arity(1);
    std::uint32_t s = f.addLocal(kI32), b0 = f.addLocal(kI64), b1 = f.addLocal(kI64),
                  b2 = f.addLocal(kI64), b3 = f.addLocal(kI64);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Eq();
    f.i32Eqz();
    f.if_();
    emitFailStatic(f, sOrdSingle);
    f.end();
    argPayload(0);
    f.i32WrapI64();
    f.lset(s);
    f.lget(s);
    f.call(utf8Len);
    f.i64c(1);
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sOrdSingle);
    f.end();
    f.lget(s);
    f.i32c(8);
    f.i32Add();
    f.i32Load8u(0);
    f.i64ExtendI32U();
    f.lset(b0);
    f.lget(s);
    f.i32c(9);
    f.i32Add();
    f.i32Load8u(0);
    f.i64ExtendI32U();
    f.lset(b1);
    f.lget(s);
    f.i32c(10);
    f.i32Add();
    f.i32Load8u(0);
    f.i64ExtendI32U();
    f.lset(b2);
    f.lget(s);
    f.i32c(11);
    f.i32Add();
    f.i32Load8u(0);
    f.i64ExtendI32U();
    f.lset(b3);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(b0);
    f.i64c(0x80);
    f.i64LtU();
    f.ifResult(kI64);
    f.lget(b0);
    f.else_();
    f.lget(b0);
    f.i64c(0xE0);
    f.i64LtU();
    f.ifResult(kI64);
    f.lget(b0);
    f.i64c(0x1F);
    f.i64And();
    f.i64c(6);
    f.i64Shl();
    f.lget(b1);
    f.i64c(0x3F);
    f.i64And();
    f.i64Or();
    f.else_();
    f.lget(b0);
    f.i64c(0xF0);
    f.i64LtU();
    f.ifResult(kI64);
    f.lget(b0);
    f.i64c(0x0F);
    f.i64And();
    f.i64c(12);
    f.i64Shl();
    f.lget(b1);
    f.i64c(0x3F);
    f.i64And();
    f.i64c(6);
    f.i64Shl();
    f.i64Or();
    f.lget(b2);
    f.i64c(0x3F);
    f.i64And();
    f.i64Or();
    f.else_();
    f.lget(b0);
    f.i64c(0x07);
    f.i64And();
    f.i64c(18);
    f.i64Shl();
    f.lget(b1);
    f.i64c(0x3F);
    f.i64And();
    f.i64c(12);
    f.i64Shl();
    f.i64Or();
    f.lget(b2);
    f.i64c(0x3F);
    f.i64And();
    f.i64c(6);
    f.i64Shl();
    f.i64Or();
    f.lget(b3);
    f.i64c(0x3F);
    f.i64And();
    f.i64Or();
    f.end();
    f.end();
    f.end();
    return index;
  }

  if (name == "floor" || name == "ceil" || name == "round" || name == "sqrt") {
    arity(1);
    std::uint32_t d = f.addLocal(kF64);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.if_();
    if (name == "sqrt") {
      // sqrt(int) is a double, not the int itself.
      f.i64c(static_cast<std::int64_t>(kDouble));
      argPayload(0);
      f.f64ConvertI64S();
      f.f64Sqrt();
      f.i64ReinterpretF64();
      f.ret();
    } else {
      argTag(0);
      argPayload(0);
      f.ret();
    }
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kFloat));
    f.i64Eq();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kDouble));
    f.i64Eq();
    f.i32Or();
    f.i32Eqz();
    f.if_();
    emitFailStatic(f, sUnsupportedOperands);
    f.end();
    toF64Arg(0);
    f.lset(d);
    if (name == "sqrt") {
      f.lget(d);
      f.f64c(0.0);
      f.f64Lt();
      f.if_();
      emitFailStatic(f, sSqrtNegative);
      f.end();
    }
    f.i64c(static_cast<std::int64_t>(kDouble));
    if (name == "floor") {
      f.lget(d);
      f.f64Floor();
    } else if (name == "ceil") {
      f.lget(d);
      f.f64Ceil();
    } else if (name == "sqrt") {
      f.lget(d);
      f.f64Sqrt();
    } else {
      f.lget(d);
      f.f64c(0.0);
      f.f64Ge();
      f.ifResult(kF64);
      f.lget(d);
      f.f64c(0.5);
      f.f64Add();
      f.f64Floor();
      f.else_();
      f.lget(d);
      f.f64c(0.5);
      f.f64Sub();
      f.f64Ceil();
      f.end();
    }
    f.i64ReinterpretF64();
    return index;
  }

  if (name == "assert") {
    arityBetween(1, 2);
    argTag(0);
    argPayload(0);
    f.call(truthy);
    f.i32Eqz();
    f.if_();
    failMaybeArg(1, sAssertFailed);
    f.end();
    retNil();
    return index;
  }

  if (name == "assertEq") {
    arityBetween(2, 3);
    std::uint32_t tmpTag = f.addLocal(kI64), tmpPayload = f.addLocal(kI64);
    argTag(0);
    argPayload(0);
    argTag(1);
    argPayload(1);
    f.call(opEq);
    f.lset(tmpPayload);
    f.lset(tmpTag);
    f.lget(tmpPayload);
    f.i64Eqz();
    f.if_();
    failMaybeArg(2, sAssertFailed);
    f.end();
    retNil();
    return index;
  }

  if (name == "panic") {
    arityBetween(0, 1);
    failMaybeArg(0, sPanic);
    retNil();
    return index;
  }

  if (name == "error") {
    arityBetween(1, 2);
    std::uint32_t addr = f.addLocal(kI32), msg = f.addLocal(kI32), code = f.addLocal(kI64);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Eq();
    f.if_();
    argPayload(0);
    f.i32WrapI64();
    f.lset(msg);
    f.else_();
    argTag(0);
    argPayload(0);
    f.call(toStr);
    f.lset(msg);
    f.end();
    f.i64c(0);
    f.lset(code);
    f.lget(1);
    f.i32c(2);
    f.i32GeS();
    f.if_();
    argTag(1);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.i32Eqz();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    argPayload(1);
    f.lset(code);
    f.end();
    f.i32c(16);
    f.call(alloc);
    f.lset(addr);
    f.lget(addr);
    f.lget(msg);
    f.i64ExtendI32U();
    f.i64Store(0);
    f.lget(addr);
    f.lget(code);
    f.i64Store(8);
    f.i64c(static_cast<std::int64_t>(kError));
    f.lget(addr);
    f.i64ExtendI32U();
    return index;
  }

  if (name == "isError" || name == "$isError") {
    arity(1);
    f.i64c(static_cast<std::int64_t>(kBool));
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kError));
    f.i64Eq();
    f.i64ExtendI32U();
    return index;
  }

  if (name == "$nonNull") {
    arity(1);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kNil));
    f.i64Eq();
    f.if_();
    emitFailStatic(f, sNonNull);
    f.end();
    argTag(0);
    argPayload(0);
    return index;
  }

  if (name == "$slice") {
    arity(4);
    for (int i = 0; i < 4; ++i) {
      argTag(i);
      argPayload(i);
    }
    f.call(slice);
    return index;
  }

  if (name == "$range" || name == "range") {
    const int count = name == "$range" ? 3 : 2;
    arity(count);
    std::uint32_t addr = f.addLocal(kI32);
    for (int i = 0; i < 2; ++i) {
      argTag(i);
      f.i64c(static_cast<std::int64_t>(kInt));
      f.i64Eq();
      f.i32Eqz();
      f.if_();
      emitFailStatic(f, sRangeBounds);
      f.end();
    }
    f.i32c(24);
    f.call(alloc);
    f.lset(addr);
    f.lget(addr);
    argPayload(0);
    f.i64Store(0);
    f.lget(addr);
    argPayload(1);
    f.i64Store(8);
    f.lget(addr);
    if (count == 3) {
      argTag(2);
      argPayload(2);
      f.call(truthy);
      f.i64ExtendI32U();
    } else {
      f.i64c(0);
    }
    f.i64Store(16);
    f.i64c(static_cast<std::int64_t>(kRange));
    f.lget(addr);
    f.i64ExtendI32U();
    return index;
  }

  if (name == "time") {
    arity(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.call(timeImport);
    return index;
  }

  if (name == "clock") {
    arity(0);
    f.i64c(static_cast<std::int64_t>(kDouble));
    f.call(clockImport);
    f.i64ReinterpretF64();
    return index;
  }

  if (name == "seed") {
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sSeedArgc);
    f.end();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sSeedType);
    f.end();
    f.call(randomState);
    argPayload(0);
    f.call(randomSeedFn);
    retNil();
    return index;
  }

  if (name == "random") {
    f.lget(1);
    f.i32c(2);
    f.i32GtS();
    f.if_();
    emitFailStatic(f, sRandomArgc);
    f.end();
    // No argument: the top 53 bits over 2^53, the VM's formula.
    f.lget(1);
    f.i32Eqz();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kDouble));
    f.call(randomState);
    f.call(randomNextFn);
    f.i64c(11);
    f.i64ShrU();
    f.f64ConvertI64U();
    f.f64c(9007199254740992.0);
    f.f64Div();
    f.i64ReinterpretF64();
    f.ret();
    f.end();
    // One argument: an integer in [0, high).
    f.lget(1);
    f.i32c(1);
    f.i32Eq();
    f.if_();
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sRandomType);
    f.end();
    argPayload(0);
    f.i64c(0);
    f.i64LeS();
    f.if_();
    emitFailStatic(f, sRandomHigh);
    f.end();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.call(randomState);
    f.call(randomNextFn);
    argPayload(0);
    f.i64RemU();
    f.ret();
    f.end();
    // Two arguments: low + next() % (high - low).
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sRandomType);
    f.end();
    argTag(1);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sRandomType);
    f.end();
    argPayload(1);
    argPayload(0);
    f.i64LeS();
    f.if_();
    emitFailStatic(f, sRandomBounds);
    f.end();
    std::uint32_t low = f.addLocal(kI64), span = f.addLocal(kI64);
    argPayload(0);
    f.lset(low);
    argPayload(1);
    f.lget(low);
    f.i64Sub();
    f.lset(span);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.call(randomState);
    f.call(randomNextFn);
    f.lget(span);
    f.i64RemU();
    f.lget(low);
    f.i64Add();
    return index;
  }

  if (name == "exit") {
    arityBetween(0, 1);
    f.lget(1);
    f.i32c(1);
    f.i32Eq();
    f.ifResult(kI32);
    argPayload(0);
    f.i32WrapI64();
    f.else_();
    f.i32c(0);
    f.end();
    f.call(exitImport);
    f.unreachable();
    return index;
  }

  // A name in the supported list that reached here is an emitter bug.
  emitFailStatic(f, sUnknown);
  return index;
}

// --------------------------------------------------------------- the lowering
constexpr std::uint32_t kNoLocal = 0xFFFFFFFFu;

const std::unordered_set<std::string>& supportedBuiltins() {
  static const std::unordered_set<std::string> names = {
      "print", "write", "eprint", "len", "type", "str", "$str", "repr",
      "int", "float", "double", "bool", "chr", "ord", "floor", "ceil",
      "round", "sqrt", "assert", "assertEq", "panic", "error", "isError",
      "$isError", "$nonNull", "$range", "range", "exit", "$slice",
      "time", "clock", "random", "seed",
  };
  return names;
}

// Every name the hosted engine installs. A name here that is not in
// `supportedBuiltins` is seeded as a closure that raises a clear error when
// called, so a module that merely *declares* an unsupported native (the way
// `import std.math` declares `sin`) still builds.
const std::unordered_set<std::string>& vmBuiltinNames() {
  static const std::unordered_set<std::string> names = {
      "print", "write", "eprint", "input", "len", "type", "str", "$str",
      "repr", "int", "float", "double", "bool", "chr", "ord", "floor",
      "ceil", "round", "sqrt", "pow", "sin", "cos", "tan", "log", "exp",
      "random", "seed", "clock", "time", "memSize", "assert", "assertEq",
            "panic", "pcall", "error", "isError", "$isError", "exit", "args",
      "readFile", "writeFile", "fileExists", "range", "$range", "$nonNull",
      "$slice", "cstring", "buffer", "cstr", "byteLen", "loadByte",
      "storeByte", "intToPtr", "ptrToInt", "free",
  };
  return names;
}

std::string wasmNameOf(const Bytecode& proto, int index) {
  const Value& name = proto.constants.at(static_cast<std::size_t>(index));
  if (!name.isStr()) {
    throw LoweringError("lowering: global name is not a string constant");
  }
  return name.asStr();
}

void collectProtos(const Bytecode& proto, std::vector<const Bytecode*>& out) {
  out.push_back(&proto);
  for (const std::shared_ptr<Bytecode>& child : proto.children) {
    if (child) collectProtos(*child, out);
  }
}

std::unordered_set<int> capturedLocalSlots(const Bytecode& proto) {
  std::unordered_set<int> captured;
  for (const std::shared_ptr<Bytecode>& child : proto.children) {
    if (!child) continue;
    for (const UpvalDesc& desc : child->upvalues) {
      if (desc.inParentLocal) captured.insert(desc.index);
    }
  }
  return captured;
}

struct Lowering {
  const Bytecode& root;
  ModuleBuilder m;
  StaticPool pool;
  Runtime rt{m, pool};

  std::vector<const Bytecode*> protos;
  std::unordered_map<const Bytecode*, std::uint32_t> protoFunc;
  std::unordered_map<const Bytecode*, std::uint32_t> protoTable;
  std::unordered_map<const Bytecode*, std::uint32_t> protoNameAddr;
  std::vector<std::vector<std::uint32_t>> stringAddr;

  std::unordered_map<std::string, int> globalSlot;
  int globalCount = 0;

  std::vector<std::string> builtinOrder;
  std::unordered_map<std::string, std::uint32_t> builtinFunc;
  std::unordered_map<std::string, std::uint32_t> builtinTable;

  std::vector<int> instructionBlock;
  int sentinelBlock = 0;

  int blockOfInstruction(int index) const {
    if (index < 0 || index >= static_cast<int>(instructionBlock.size())) return sentinelBlock;
    return instructionBlock[static_cast<std::size_t>(index)];
  }

  explicit Lowering(const Bytecode& proto) : root(proto) {}

  void planGlobals();
  void emitProto(const Bytecode& proto, std::size_t protoIndex);
  void emitInstruction(const Bytecode& proto, const Instruction& instr,
                       const std::vector<std::uint32_t>& regTag,
                       const std::vector<std::uint32_t>& regPayload,
                       const std::vector<std::uint32_t>& localTag,
                       const std::vector<std::uint32_t>& localPayload,
                       const std::vector<std::uint32_t>& localCell,
                       std::uint32_t pcLocal, std::uint32_t windowLocal,
                       std::uint32_t addrLocal, std::uint32_t itTagLocal,
                       std::uint32_t itPayloadLocal, std::uint32_t curTagLocal,
                       std::uint32_t curPayloadLocal, std::uint32_t doneLocal,
                       std::uint32_t vTagLocal, std::uint32_t vPayloadLocal,
                       std::size_t protoIndex, int blockIndex);
  std::vector<std::uint8_t> run();
};

void Lowering::planGlobals() {
  std::unordered_set<std::string> hostBindings;
  std::unordered_set<std::string> externNames;
  for (const Bytecode* proto : protos) {
    for (const ExternSymbol& symbol : proto->externs) {
      hostBindings.insert(symbol.name);
      externNames.insert(symbol.name);
    }
    for (const std::string& name : proto->nativeFns) hostBindings.insert(name);
  }
  const std::unordered_set<std::string>& builtins = vmBuiltinNames();
  auto assign = [this](const std::string& name) {
    if (globalSlot.find(name) != globalSlot.end()) return;
    globalSlot[name] = globalCount++;
  };
  // A builtin is a global the hosted engine installs, so it needs a slot only
  // when the program reads it.
  for (const Bytecode* proto : protos) {
    for (const Instruction& instr : proto->code) {
      if (instr.op != OpCode::GETGLOBAL) continue;
      const std::string& name = wasmNameOf(*proto, instr.b);
      if (builtins.find(name) != builtins.end()) assign(name);
    }
  }
  if (globalSlot.find("random") != globalSlot.end() ||
      globalSlot.find("seed") != globalSlot.end()) {
    assign("$randomState");
  }
  for (const std::string& name : hostBindings) assign(name);
  auto isHostBindingShape = [&](const Bytecode& proto, std::size_t index) {
    if (index == 0) return false;
    const Instruction& set = proto.code[index];
    const Instruction& prev = proto.code[index - 1];
    if (set.op != OpCode::SETGLOBAL || prev.op != OpCode::GETGLOBAL) return false;
    return wasmNameOf(proto, set.b) == wasmNameOf(proto, prev.b);
  };
  for (const Bytecode* proto : protos) {
    for (std::size_t i = 0; i < proto->code.size(); ++i) {
      const Instruction& instr = proto->code[i];
      if (instr.op != OpCode::SETGLOBAL) continue;
      if (isHostBindingShape(*proto, i)) continue;
      assign(wasmNameOf(*proto, instr.b));
    }
  }
  for (const Bytecode* proto : protos) {
    for (const Instruction& instr : proto->code) {
      if (instr.op != OpCode::GETGLOBAL) continue;
      const std::string& name = wasmNameOf(*proto, instr.b);
      if (globalSlot.find(name) == globalSlot.end()) {
        throw LoweringError("lowering: global '" + name + "' is read but never assigned");
      }
    }
  }
  std::unordered_set<std::string> usedNatives;
  for (const Bytecode* proto : protos) {
    for (const std::string& name : proto->nativeUses) usedNatives.insert(name);
  }
  for (const std::string& name : usedNatives) {
    if (builtins.find(name) != builtins.end()) continue;
    if (externNames.find(name) != externNames.end()) {
      throw LoweringError("lowering: extern \"C\" symbol '" + name +
                          "' cannot be resolved by the WebAssembly backend");
    }
    throw LoweringError("lowering: native fn '" + name +
                        "' cannot be resolved by the WebAssembly backend");
  }
}

void Lowering::emitInstruction(
    const Bytecode& proto, const Instruction& instr,
    const std::vector<std::uint32_t>& regTag,
    const std::vector<std::uint32_t>& regPayload,
    const std::vector<std::uint32_t>& localTag,
    const std::vector<std::uint32_t>& localPayload,
    const std::vector<std::uint32_t>& localCell, std::uint32_t pcLocal,
    std::uint32_t windowLocal, std::uint32_t addrLocal, std::uint32_t itTagLocal,
    std::uint32_t itPayloadLocal, std::uint32_t curTagLocal,
    std::uint32_t curPayloadLocal, std::uint32_t doneLocal,
    std::uint32_t vTagLocal, std::uint32_t vPayloadLocal,
    std::size_t protoIndex, int blockIndex) {
  const std::uint32_t functionIndex = protoFunc[&proto];
  Func f(m, functionIndex, {kI32, kI32, kI32});

  auto storeReg = [&](int index) {
    f.lset(regPayload.at(static_cast<std::size_t>(index)));
    f.lset(regTag.at(static_cast<std::size_t>(index)));
  };
  auto loadReg = [&](int index) {
    f.lget(regTag.at(static_cast<std::size_t>(index)));
    f.lget(regPayload.at(static_cast<std::size_t>(index)));
  };
  auto callPair = [&](std::uint32_t helper, int lhs, int rhs) {
    loadReg(lhs);
    loadReg(rhs);
    f.call(helper);
  };
  auto negatePayload = [&]() {
    f.i64c(0);
    f.i64Eq();
    f.i64ExtendI32U();
  };
  auto jumpTo = [&](int targetInstruction) {
    const int target = static_cast<int>(blockOfInstruction(targetInstruction));
    f.i32c(target);
    f.lset(pcLocal);
    f.br(static_cast<std::uint32_t>(blockIndex));
  };

  (void)protoIndex;
  switch (instr.op) {
    case OpCode::LOADK: {
      const Value& value = proto.constants.at(static_cast<std::size_t>(instr.b));
      if (value.isNil()) {
        f.i64c(static_cast<std::int64_t>(kNil));
        f.i64c(0);
      } else if (value.isBool()) {
        f.i64c(static_cast<std::int64_t>(kBool));
        f.i64c(value.asBool() ? 1 : 0);
      } else if (value.isInt()) {
        f.i64c(static_cast<std::int64_t>(kInt));
        f.i64c(value.asInt());
      } else if (value.isFloat()) {
        f.i64c(static_cast<std::int64_t>(kFloat));
        f.i64c(static_cast<std::int64_t>(doubleBits(value.number())));
      } else if (value.isDouble()) {
        f.i64c(static_cast<std::int64_t>(kDouble));
        f.i64c(static_cast<std::int64_t>(doubleBits(value.asDouble())));
      } else if (value.isStr()) {
        f.i64c(static_cast<std::int64_t>(kStr));
        f.i64c(static_cast<std::int64_t>(
            stringAddr[protoIndex][static_cast<std::size_t>(instr.b)]));
      } else {
        throw LoweringError("lowering: unsupported constant in the WebAssembly backend");
      }
      storeReg(instr.a);
      break;
    }
    case OpCode::MOVE:
      loadReg(instr.b);
      storeReg(instr.a);
      break;
    case OpCode::ADD:
    case OpCode::SUB:
    case OpCode::MUL:
    case OpCode::DIV:
    case OpCode::MOD:
    case OpCode::POW:
    case OpCode::BAND:
    case OpCode::BOR:
    case OpCode::BXOR:
    case OpCode::SHL:
    case OpCode::SHR: {
      std::uint32_t helper = rt.opAdd;
      switch (instr.op) {
        case OpCode::ADD: helper = rt.opAdd; break;
        case OpCode::SUB: helper = rt.opSub; break;
        case OpCode::MUL: helper = rt.opMul; break;
        case OpCode::DIV: helper = rt.opDiv; break;
        case OpCode::MOD: helper = rt.opMod; break;
        case OpCode::POW: helper = rt.opPow; break;
        case OpCode::BAND: helper = rt.opBand; break;
        case OpCode::BOR: helper = rt.opBor; break;
        case OpCode::BXOR: helper = rt.opBxor; break;
        case OpCode::SHL: helper = rt.opShl; break;
        default: helper = rt.opShr; break;
      }
      callPair(helper, instr.b, instr.c);
      storeReg(instr.a);
      break;
    }
    case OpCode::UNOP: {
      const std::uint32_t helper =
          instr.c == 0 ? rt.unNeg : (instr.c == 1 ? rt.unNot : rt.unBitNot);
      loadReg(instr.b);
      f.call(helper);
      storeReg(instr.a);
      break;
    }
    case OpCode::EQ:
      callPair(rt.opEq, instr.b, instr.c);
      storeReg(instr.a);
      break;
    case OpCode::NEQ:
      callPair(rt.opEq, instr.b, instr.c);
      negatePayload();
      storeReg(instr.a);
      break;
    case OpCode::LT:
      callPair(rt.opLt, instr.b, instr.c);
      storeReg(instr.a);
      break;
    case OpCode::GT:
      callPair(rt.opLt, instr.c, instr.b);
      storeReg(instr.a);
      break;
    case OpCode::LTE:
      callPair(rt.opLt, instr.c, instr.b);
      negatePayload();
      storeReg(instr.a);
      break;
    case OpCode::GTE:
      callPair(rt.opLt, instr.b, instr.c);
      negatePayload();
      storeReg(instr.a);
      break;
    case OpCode::JMP:
      jumpTo(instr.jump);
      break;
    case OpCode::JMP_IF_FALSE: {
      const int target = static_cast<int>(blockOfInstruction(instr.jump));
      const int next = static_cast<int>(blockOfInstruction(
          static_cast<int>(&instr - proto.code.data()) + 1));
      f.i32c(next);
      f.i32c(target);
      loadReg(instr.a);
      f.call(rt.truthy);
      f.select();
      f.lset(pcLocal);
      f.br(static_cast<std::uint32_t>(blockIndex));
      break;
    }
    case OpCode::JMP_IF_TRUE: {
      const int target = static_cast<int>(blockOfInstruction(instr.jump));
      const int next = static_cast<int>(blockOfInstruction(
          static_cast<int>(&instr - proto.code.data()) + 1));
      f.i32c(target);
      f.i32c(next);
      loadReg(instr.a);
      f.call(rt.truthy);
      f.select();
      f.lset(pcLocal);
      f.br(static_cast<std::uint32_t>(blockIndex));
      break;
    }
    case OpCode::SETGLOBAL: {
      const std::string& name = wasmNameOf(proto, instr.b);
      const std::uint32_t slot =
          static_cast<std::uint32_t>(globalSlot.at(name) * 16);
      f.i32c(static_cast<std::int32_t>(slot));
      f.lget(regTag.at(static_cast<std::size_t>(instr.a)));
      f.i64Store(0);
      f.i32c(static_cast<std::int32_t>(slot + 8));
      f.lget(regPayload.at(static_cast<std::size_t>(instr.a)));
      f.i64Store(0);
      break;
    }
    case OpCode::GETGLOBAL: {
      const std::string& name = wasmNameOf(proto, instr.b);
      const std::uint32_t slot =
          static_cast<std::uint32_t>(globalSlot.at(name) * 16);
      f.i32c(static_cast<std::int32_t>(slot));
      f.i64Load(0);
      f.i32c(static_cast<std::int32_t>(slot + 8));
      f.i64Load(0);
      storeReg(instr.a);
      break;
    }
    case OpCode::DECLLOCAL:
      if (localCell.at(static_cast<std::size_t>(instr.b)) != kNoLocal) {
        const std::uint32_t cell = localCell.at(static_cast<std::size_t>(instr.b));
        f.i32c(16);
        f.call(rt.alloc);
        f.lset(cell);
        f.lget(cell);
        f.lget(regTag.at(static_cast<std::size_t>(instr.a)));
        f.i64Store(0);
        f.lget(cell);
        f.lget(regPayload.at(static_cast<std::size_t>(instr.a)));
        f.i64Store(8);
      } else {
        f.lget(regTag.at(static_cast<std::size_t>(instr.a)));
        f.lset(localTag.at(static_cast<std::size_t>(instr.b)));
        f.lget(regPayload.at(static_cast<std::size_t>(instr.a)));
        f.lset(localPayload.at(static_cast<std::size_t>(instr.b)));
      }
      break;
    case OpCode::SETLOCAL:
      if (localCell.at(static_cast<std::size_t>(instr.b)) != kNoLocal) {
        const std::uint32_t cell = localCell.at(static_cast<std::size_t>(instr.b));
        f.lget(cell);
        f.lget(regTag.at(static_cast<std::size_t>(instr.a)));
        f.i64Store(0);
        f.lget(cell);
        f.lget(regPayload.at(static_cast<std::size_t>(instr.a)));
        f.i64Store(8);
      } else {
        f.lget(regTag.at(static_cast<std::size_t>(instr.a)));
        f.lset(localTag.at(static_cast<std::size_t>(instr.b)));
        f.lget(regPayload.at(static_cast<std::size_t>(instr.a)));
        f.lset(localPayload.at(static_cast<std::size_t>(instr.b)));
      }
      break;
    case OpCode::GETLOCAL:
      if (localCell.at(static_cast<std::size_t>(instr.b)) != kNoLocal) {
        const std::uint32_t cell = localCell.at(static_cast<std::size_t>(instr.b));
        f.lget(cell);
        f.i64Load(0);
        f.lget(cell);
        f.i64Load(8);
        storeReg(instr.a);
      } else {
        f.lget(localTag.at(static_cast<std::size_t>(instr.b)));
        f.lget(localPayload.at(static_cast<std::size_t>(instr.b)));
        storeReg(instr.a);
      }
      break;
    case OpCode::CALL: {
      const int argc = instr.b;
      f.i32c(16 * argc);
      f.call(rt.alloc);
      f.lset(windowLocal);
      for (int j = 0; j < argc; ++j) {
        f.lget(windowLocal);
        f.i32c(16 * j);
        f.i32Add();
        f.lget(regTag.at(static_cast<std::size_t>(instr.a + 1 + j)));
        f.i64Store(0);
        f.lget(windowLocal);
        f.i32c(16 * j);
        f.i32Add();
        f.lget(regPayload.at(static_cast<std::size_t>(instr.a + 1 + j)));
        f.i64Store(8);
      }
      f.lget(regTag.at(static_cast<std::size_t>(instr.a)));
      f.i64c(static_cast<std::int64_t>(kFunc));
      f.i64Eq();
      f.i32Eqz();
      f.if_();
      rt.emitFailStatic(f, rt.sNotCallable);
      f.end();
      f.lget(windowLocal);
      f.i32c(argc);
      f.lget(regPayload.at(static_cast<std::size_t>(instr.a)));
      f.i32WrapI64();
      f.lget(regPayload.at(static_cast<std::size_t>(instr.a)));
      f.i32WrapI64();
      f.i32Load(0);
      f.callIndirect(rt.tValue);
      storeReg(instr.a);
      break;
    }
    case OpCode::RETURN:
      loadReg(instr.a);
      f.ret();
      break;
    case OpCode::CLOSURE: {
      const Bytecode* child = proto.children.at(static_cast<std::size_t>(instr.b)).get();
      const std::uint32_t upvalues = static_cast<std::uint32_t>(child->upvalues.size());
      f.i32c(static_cast<std::int32_t>(40 + 8 * upvalues));
      f.call(rt.alloc);
      f.lset(addrLocal);
      f.lget(addrLocal);
      f.i64c(static_cast<std::int64_t>(protoTable.at(child)));
      f.i64Store(0);
      f.lget(addrLocal);
      f.i64c(child->numParams);
      f.i64Store(8);
      f.lget(addrLocal);
      f.i64c(upvalues);
      f.i64Store(16);
      f.lget(addrLocal);
      f.i64c(static_cast<std::int64_t>(protoNameAddr.at(child)));
      f.i64Store(24);
      f.lget(addrLocal);
      f.i64c(0);
      f.i64Store(32);
      for (std::uint32_t j = 0; j < upvalues; ++j) {
        const UpvalDesc& desc = child->upvalues[j];
        f.lget(addrLocal);
        f.i32c(static_cast<std::int32_t>(40 + 8 * j));
        f.i32Add();
        if (desc.inParentLocal) {
          f.lget(localCell.at(static_cast<std::size_t>(desc.index)));
          f.i64ExtendI32U();
        } else {
          f.lget(2);
          f.i32c(static_cast<std::int32_t>(40 + 8 * desc.index));
          f.i32Add();
          f.i64Load(0);
        }
        f.i64Store(0);
      }
      f.i64c(static_cast<std::int64_t>(kFunc));
      f.lget(addrLocal);
      f.i64ExtendI32U();
      storeReg(instr.a);
      break;
    }
    case OpCode::GETUPVAL:
      f.lget(2);
      f.i32c(static_cast<std::int32_t>(40 + 8 * instr.b));
      f.i32Add();
      f.i64Load(0);
      f.i32WrapI64();
      f.lset(addrLocal);
      f.lget(addrLocal);
      f.i64Load(0);
      f.lget(addrLocal);
      f.i64Load(8);
      storeReg(instr.a);
      break;
    case OpCode::SETUPVAL:
      f.lget(2);
      f.i32c(static_cast<std::int32_t>(40 + 8 * instr.b));
      f.i32Add();
      f.i64Load(0);
      f.i32WrapI64();
      f.lset(addrLocal);
      f.lget(addrLocal);
      f.lget(regTag.at(static_cast<std::size_t>(instr.a)));
      f.i64Store(0);
      f.lget(addrLocal);
      f.lget(regPayload.at(static_cast<std::size_t>(instr.a)));
      f.i64Store(8);
      break;
    case OpCode::ITER: {
      loadReg(instr.b);
      loadReg(instr.c);
      f.i32c(instr.d >= 0 ? 1 : 0);
      f.call(rt.iterStep);
      f.lset(vPayloadLocal);
      f.lset(vTagLocal);
      f.lset(doneLocal);
      f.lset(curPayloadLocal);
      f.lset(curTagLocal);
      f.lset(itPayloadLocal);
      f.lset(itTagLocal);
      f.lget(curTagLocal);
      f.lset(regTag.at(static_cast<std::size_t>(instr.c)));
      f.lget(curPayloadLocal);
      f.lset(regPayload.at(static_cast<std::size_t>(instr.c)));
      f.lget(itTagLocal);
      f.lset(regTag.at(static_cast<std::size_t>(instr.a)));
      f.lget(itPayloadLocal);
      f.lset(regPayload.at(static_cast<std::size_t>(instr.a)));
      if (instr.d >= 0) {
        f.lget(vTagLocal);
        f.lset(regTag.at(static_cast<std::size_t>(instr.d)));
        f.lget(vPayloadLocal);
        f.lset(regPayload.at(static_cast<std::size_t>(instr.d)));
      }
      f.i64c(static_cast<std::int64_t>(kBool));
      f.lget(doneLocal);
      f.i64ExtendI32U();
      f.lset(regPayload.at(static_cast<std::size_t>(instr.e)));
      f.lset(regTag.at(static_cast<std::size_t>(instr.e)));
      break;
    }
    case OpCode::IN:
      callPair(rt.opIn, instr.b, instr.c);
      storeReg(instr.a);
      break;
    case OpCode::HALT:
      loadReg(0);
      f.ret();
      break;
    case OpCode::GETTABLE: {
      loadReg(instr.b);
      if (instr.c >= 0) {
        loadReg(instr.c);
        f.call(rt.indexGet);
      } else {
        f.i64c(static_cast<std::int64_t>(kStr));
        f.i64c(static_cast<std::int64_t>(
            stringAddr[protoIndex][static_cast<std::size_t>(-instr.c - 1)]));
        f.call(rt.memberGet);
      }
      storeReg(instr.a);
      break;
    }
    case OpCode::SETTABLE: {
      loadReg(instr.b);
      if (instr.c >= 0) {
        loadReg(instr.c);
        loadReg(instr.a);
        f.call(rt.indexSet);
      } else {
        f.i64c(static_cast<std::int64_t>(kStr));
        f.i64c(static_cast<std::int64_t>(
            stringAddr[protoIndex][static_cast<std::size_t>(-instr.c - 1)]));
        loadReg(instr.a);
        f.call(rt.memberSet);
        f.drop();
        f.drop();
      }
      break;
    }
    case OpCode::NEWTABLE:
      if (instr.c == 0) {
        f.i64c(instr.b);
        f.call(rt.newArray);
      } else if (instr.c == 1) {
        f.call(rt.newMap);
      } else {
        throw LoweringError("lowering: invalid table kind for NEWTABLE");
      }
      storeReg(instr.a);
      break;
  }
}

void Lowering::emitProto(const Bytecode& proto, std::size_t protoIndex) {
  const std::uint32_t functionIndex = protoFunc[&proto];
  const int fileRegs = std::max(1, proto.numRegisters);
  const int numLocals = std::max(0, proto.numLocals);
  const std::unordered_set<int> captured = capturedLocalSlots(proto);

  Func f(m, functionIndex, {kI32, kI32, kI32});
  std::vector<std::uint32_t> regTag(static_cast<std::size_t>(fileRegs));
  std::vector<std::uint32_t> regPayload(static_cast<std::size_t>(fileRegs));
  for (int i = 0; i < fileRegs; ++i) {
    regTag[static_cast<std::size_t>(i)] = f.addLocal(kI64);
    regPayload[static_cast<std::size_t>(i)] = f.addLocal(kI64);
  }
  std::vector<std::uint32_t> localTag(static_cast<std::size_t>(numLocals), kNoLocal);
  std::vector<std::uint32_t> localPayload(static_cast<std::size_t>(numLocals), kNoLocal);
  std::vector<std::uint32_t> localCell(static_cast<std::size_t>(numLocals), kNoLocal);
  for (int b = 0; b < numLocals; ++b) {
    if (captured.count(b) != 0) {
      localCell[static_cast<std::size_t>(b)] = f.addLocal(kI32);
    } else {
      localTag[static_cast<std::size_t>(b)] = f.addLocal(kI64);
      localPayload[static_cast<std::size_t>(b)] = f.addLocal(kI64);
    }
  }
  const std::uint32_t pcLocal = f.addLocal(kI32);
  const std::uint32_t windowLocal = f.addLocal(kI32);
  const std::uint32_t addrLocal = f.addLocal(kI32);
  const std::uint32_t itTagLocal = f.addLocal(kI64);
  const std::uint32_t itPayloadLocal = f.addLocal(kI64);
  const std::uint32_t curTagLocal = f.addLocal(kI64);
  const std::uint32_t curPayloadLocal = f.addLocal(kI64);
  const std::uint32_t doneLocal = f.addLocal(kI32);
  const std::uint32_t vTagLocal = f.addLocal(kI64);
  const std::uint32_t vPayloadLocal = f.addLocal(kI64);

  const int required = proto.numParams - (proto.variadic ? 1 : 0);
  if (proto.variadic) {
    f.lget(1);
    f.i32c(required);
    f.i32LtS();
  } else {
    f.lget(1);
    f.i32c(required);
    f.i32Ne();
  }
  f.if_();
  rt.emitFailStatic(f, rt.sBadArity);
  f.end();
  for (int i = 0; i < required && i < fileRegs; ++i) {
    f.lget(0);
    f.i32c(16 * i);
    f.i32Add();
    f.i64Load(0);
    f.lset(regTag[static_cast<std::size_t>(i)]);
    f.lget(0);
    f.i32c(16 * i + 8);
    f.i32Add();
    f.i64Load(0);
    f.lset(regPayload[static_cast<std::size_t>(i)]);
  }
  if (proto.variadic && required < fileRegs) {
    // The extras are packed into a fresh array bound to the last parameter,
    // exactly as the VM's call setup does.
    f.lget(1);
    f.i32c(required);
    f.i32Sub();
    f.i64ExtendI32U();
    f.call(rt.newArray);
    f.lset(vPayloadLocal);
    f.lset(vTagLocal);
    f.i32c(required);
    f.lset(addrLocal);
    f.block();
    f.loop();
    f.lget(addrLocal);
    f.lget(1);
    f.i32GeS();
    f.brIf(1);
    f.lget(vPayloadLocal);
    f.i32WrapI64();
    f.lget(0);
    f.lget(addrLocal);
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lget(0);
    f.lget(addrLocal);
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.call(rt.arrayPush);
    f.lget(addrLocal);
    f.i32c(1);
    f.i32Add();
    f.lset(addrLocal);
    f.br(0);
    f.end();
    f.end();
    f.lget(vTagLocal);
    f.lset(regTag[static_cast<std::size_t>(required)]);
    f.lget(vPayloadLocal);
    f.lset(regPayload[static_cast<std::size_t>(required)]);
  }

  const int count = static_cast<int>(proto.code.size());
  std::vector<int> leaders{0};
  for (int i = 0; i < count; ++i) {
    const Instruction& instr = proto.code[static_cast<std::size_t>(i)];
    const bool terminator = instr.op == OpCode::JMP ||
                            instr.op == OpCode::JMP_IF_FALSE ||
                            instr.op == OpCode::JMP_IF_TRUE ||
                            instr.op == OpCode::RETURN || instr.op == OpCode::HALT;
    if (terminator && i + 1 < count) leaders.push_back(i + 1);
    if (instr.op == OpCode::JMP || instr.op == OpCode::JMP_IF_FALSE ||
        instr.op == OpCode::JMP_IF_TRUE) {
      if (instr.jump >= 0 && instr.jump < count) leaders.push_back(instr.jump);
    }
  }
  std::sort(leaders.begin(), leaders.end());
  leaders.erase(std::unique(leaders.begin(), leaders.end()), leaders.end());
  leaders.erase(std::remove_if(leaders.begin(), leaders.end(),
                               [count](int value) { return value < 0 || value >= count; }),
                leaders.end());
  if (leaders.empty()) leaders.push_back(0);
  instructionBlock.assign(static_cast<std::size_t>(count), 0);
  sentinelBlock = static_cast<int>(leaders.size());
  int leader = 0;
  for (int i = 0; i < count; ++i) {
    while (leader + 1 < static_cast<int>(leaders.size()) &&
           leaders[static_cast<std::size_t>(leader + 1)] <= i) {
      ++leader;
    }
    instructionBlock[static_cast<std::size_t>(i)] = leader;
  }
  const int totalBlocks = sentinelBlock + 1;

  f.loop();
  for (int k = 0; k < totalBlocks; ++k) f.block();
  f.lget(pcLocal);
  std::vector<std::uint32_t> targets;
  targets.reserve(static_cast<std::size_t>(totalBlocks));
  for (int k = 0; k < totalBlocks; ++k) {
    targets.push_back(static_cast<std::uint32_t>(totalBlocks - 1 - k));
  }
  f.brTable(targets, 0);
  for (int k = totalBlocks - 1; k >= 0; --k) {
    f.end();
    if (k == sentinelBlock) {
      f.i64c(static_cast<std::int64_t>(kNil));
      f.i64c(0);
      f.ret();
      continue;
    }
    const int start = leaders[static_cast<std::size_t>(k)];
    const int end = (k + 1 < static_cast<int>(leaders.size()))
                        ? leaders[static_cast<std::size_t>(k + 1)]
                        : count;
    for (int i = start; i < end; ++i) {
      emitInstruction(proto, proto.code[static_cast<std::size_t>(i)], regTag,
                      regPayload, localTag, localPayload, localCell, pcLocal,
                      windowLocal, addrLocal, itTagLocal, itPayloadLocal,
                      curTagLocal, curPayloadLocal, doneLocal, vTagLocal,
                      vPayloadLocal, protoIndex, k);
    }
    const OpCode lastOp = proto.code[static_cast<std::size_t>(end - 1)].op;
    const bool terminator = lastOp == OpCode::JMP || lastOp == OpCode::JMP_IF_FALSE ||
                            lastOp == OpCode::JMP_IF_TRUE || lastOp == OpCode::RETURN ||
                            lastOp == OpCode::HALT;
    if (!terminator) {
      f.i32c(blockOfInstruction(end));
      f.lset(pcLocal);
      f.br(static_cast<std::uint32_t>(k));
    }
  }
  f.end();
  f.unreachable();
}

std::vector<std::uint8_t> Lowering::run() {
  collectProtos(root, protos);
  planGlobals();
  rt.randomStateSlot = globalSlot.find("$randomState") != globalSlot.end()
                           ? globalSlot.at("$randomState")
                           : -1;

  pool.base = static_cast<std::uint32_t>(globalCount * 16);
  rt.declareTypes();
  rt.addImports();
  rt.addStaticStrings();
  rt.emitCore();

  // One static string per string constant and per proto name, created before
  // any proto body is emitted so a LOADK/CLOSURE can name its address.
  stringAddr.resize(protos.size());
  for (std::size_t p = 0; p < protos.size(); ++p) {
    stringAddr[p].assign(protos[p]->constants.size(), 0);
    for (std::size_t c = 0; c < protos[p]->constants.size(); ++c) {
      if (protos[p]->constants[c].isStr()) {
        stringAddr[p][c] = pool.addString(protos[p]->constants[c].asStr());
      }
    }
    protoNameAddr[protos[p]] = pool.addString(protos[p]->name);
  }

  // Seed the builtins the program reads. The order is sorted so the module is
  // deterministic; a builtin without a WASM body gets a closure that raises a
  // clear error when it is called.
  std::vector<std::string> seeded;
  for (const auto& entry : globalSlot) {
    if (vmBuiltinNames().find(entry.first) != vmBuiltinNames().end()) {
      seeded.push_back(entry.first);
    }
  }
  std::sort(seeded.begin(), seeded.end());
  for (const std::string& name : seeded) {
    std::uint32_t index;
    if (supportedBuiltins().find(name) != supportedBuiltins().end()) {
      index = rt.emitBuiltin(name);
    } else {
      index = rt.emitTrapBuiltin(name);
    }
    builtinFunc[name] = index;
    builtinTable[name] = static_cast<std::uint32_t>(builtinOrder.size());
    builtinOrder.push_back(name);
  }

  for (const Bytecode* proto : protos) {
    protoFunc[proto] = m.reserveFunction(rt.tValue);
  }
  for (std::size_t i = 0; i < protos.size(); ++i) {
    protoTable[protos[i]] =
        static_cast<std::uint32_t>(builtinOrder.size() + i);
  }

  for (std::size_t i = 0; i < protos.size(); ++i) emitProto(*protos[i], i);

  // Static closure per seeded builtin, plus a data segment that puts the
  // `Func` value in its global slot.
  for (const std::string& name : builtinOrder) {
    const std::uint32_t nameAddress = pool.addString(name);
    const std::uint32_t closure = pool.addClosure(
        builtinTable[name], 0, 0, nameAddress, 1);
    Bytes segment;
    std::uint64_t tag = kFunc;
    for (int i = 0; i < 8; ++i) segment.push_back(static_cast<std::uint8_t>(tag >> (8 * i)));
    for (int i = 0; i < 8; ++i) {
      segment.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(closure) >> (8 * i)));
    }
    m.data.emplace_back(static_cast<std::uint32_t>(globalSlot.at(name) * 16),
                        std::move(segment));
  }

  const std::uint32_t arenaStart = (pool.end() + 15u) & ~15u;
  m.arenaStart = arenaStart;
  m.memoryPages = std::max<std::uint32_t>(1, (arenaStart + 65535u) / 65536u);
  m.data.emplace_back(pool.base, pool.bytes);

  m.tableFuncs.clear();
  for (const std::string& name : builtinOrder) m.tableFuncs.push_back(builtinFunc[name]);
  for (const Bytecode* proto : protos) m.tableFuncs.push_back(protoFunc[proto]);

  // `_start` is reserved last, because `build()` exports the last function.
  const std::uint32_t startIndex = m.reserveFunction(rt.tStart);
  Func start(m, startIndex, {});
  start.i32c(0);
  start.i32c(0);
  start.i32c(0);
  start.call(protoFunc[&root]);
  start.drop();
  start.drop();

  return m.build();
}

}  // namespace

std::vector<std::uint8_t> lowerToWasm(const Target& target, const Bytecode& proto) {
  if (target.arch != Arch::Wasm32 || target.abi != Abi::Wasm ||
      target.os != Os::Freestanding) {
    throw std::invalid_argument("wasm: no backend for target " + targetName(target));
  }
  Lowering lowering(proto);
  return lowering.run();
}
