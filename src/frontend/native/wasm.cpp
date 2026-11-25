#include "native/wasm.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/value.h"
#include "native/lower.h"
#include "native/value.h"
#include "unicode_case.h"

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
constexpr std::uint64_t kPtr = static_cast<std::uint64_t>(AotTag::Ptr);

// On-image layouts, shared with `include/native/value.h`. An array is
// {length, capacity, items}; a map is {length, capacity, entries}, where an
// entry is {key AotString*, value tag, value payload} (24 bytes).
constexpr std::uint32_t kArrayLengthOffset = 0;
constexpr std::uint32_t kArrayCapacityOffset = 8;
constexpr std::uint32_t kArrayItemsOffset = 16;
constexpr std::uint32_t kMapLengthOffset = 0;
constexpr std::uint32_t kMapCapacityOffset = 8;
constexpr std::uint32_t kMapEntriesOffset = 16;
// Class identity (PHASE 12): the class-name string address, or 0 for a plain
// map, and the method-table map address, or 0.
constexpr std::uint32_t kMapClassNameOffset = 24;
constexpr std::uint32_t kMapMethodsOffset = 32;
constexpr std::uint32_t kMapHeaderSize = 40;
constexpr std::uint32_t kMapEntryKeyOffset = 0;
constexpr std::uint32_t kMapEntryTagOffset = 8;
constexpr std::uint32_t kMapEntryPayloadOffset = 16;
constexpr std::uint32_t kMapEntrySize = 24;

// A pointer handle is {kind, data, length, released}, the `AotPtr` layout in
// `include/native/value.h`. `data` owns the bytes for a `cstring`/`buffer` and
// is the opaque token for a `void *`; `free` only marks the handle released,
// because the arena never reclaims.
constexpr std::uint32_t kPtrKindOffset = 0;
constexpr std::uint32_t kPtrDataOffset = 8;
constexpr std::uint32_t kPtrLengthOffset = 16;
constexpr std::uint32_t kPtrReleasedOffset = 24;
constexpr std::uint32_t kPtrHeaderSize = 32;
constexpr std::uint64_t kPtrKindVoid = 0;
constexpr std::uint64_t kPtrKindCString = 1;
constexpr std::uint64_t kPtrKindBuffer = 2;

// A bound method is a closure whose `flags` word holds one of these selectors;
// the shared dispatcher reads it and calls the matching helper.
constexpr std::uint64_t kMethodArrayPush = 0;
constexpr std::uint64_t kMethodArrayPop = 1;
constexpr std::uint64_t kMethodArrayShift = 2;
constexpr std::uint64_t kMethodArrayUnshift = 3;
constexpr std::uint64_t kMethodArrayInsert = 4;
constexpr std::uint64_t kMethodArrayRemoveAt = 5;
constexpr std::uint64_t kMethodArrayContains = 6;
constexpr std::uint64_t kMethodArrayIndexOf = 7;
constexpr std::uint64_t kMethodArrayClear = 8;
constexpr std::uint64_t kMethodArraySlice = 9;
constexpr std::uint64_t kMethodArrayConcat = 10;
constexpr std::uint64_t kMethodArrayReversed = 11;
constexpr std::uint64_t kMethodArrayClone = 12;
constexpr std::uint64_t kMethodArraySum = 13;
constexpr std::uint64_t kMethodArrayJoin = 14;
constexpr std::uint64_t kMethodArrayMap = 15;
constexpr std::uint64_t kMethodArrayFilter = 16;
constexpr std::uint64_t kMethodArrayEach = 17;
constexpr std::uint64_t kMethodArrayFind = 18;
constexpr std::uint64_t kMethodArrayAny = 19;
constexpr std::uint64_t kMethodArrayAll = 20;
constexpr std::uint64_t kMethodArrayReduce = 21;
constexpr std::uint64_t kMethodArraySort = 22;
constexpr std::uint64_t kMethodArraySorted = 23;
constexpr std::uint64_t kMethodMapKeys = 24;
constexpr std::uint64_t kMethodMapValues = 25;
constexpr std::uint64_t kMethodMapEntries = 26;
constexpr std::uint64_t kMethodMapHas = 27;
constexpr std::uint64_t kMethodMapGet = 28;
constexpr std::uint64_t kMethodMapSet = 29;
constexpr std::uint64_t kMethodMapDelete = 30;
constexpr std::uint64_t kMethodMapMerge = 31;
constexpr std::uint64_t kMethodMapClone = 32;
constexpr std::uint64_t kMethodMapClear = 33;
constexpr std::uint64_t kMethodStrUpper = 34;
constexpr std::uint64_t kMethodStrLower = 35;
constexpr std::uint64_t kMethodStrTrim = 36;
constexpr std::uint64_t kMethodStrTrimStart = 37;
constexpr std::uint64_t kMethodStrTrimEnd = 38;
constexpr std::uint64_t kMethodStrSplit = 39;
constexpr std::uint64_t kMethodStrLines = 40;
constexpr std::uint64_t kMethodStrReplace = 41;
constexpr std::uint64_t kMethodStrReplaceFirst = 42;
constexpr std::uint64_t kMethodStrStartsWith = 43;
constexpr std::uint64_t kMethodStrEndsWith = 44;
constexpr std::uint64_t kMethodStrContains = 45;
constexpr std::uint64_t kMethodStrIndexOf = 46;
constexpr std::uint64_t kMethodStrSubstr = 47;
constexpr std::uint64_t kMethodStrChars = 48;
constexpr std::uint64_t kMethodStrRepeat = 49;
constexpr std::uint64_t kMethodStrReversed = 50;
constexpr std::uint64_t kMethodStrPadStart = 51;
constexpr std::uint64_t kMethodStrPadEnd = 52;
constexpr std::uint64_t kMethodRangeToArray = 53;
constexpr std::uint64_t kMethodRangeContains = 54;
constexpr std::size_t kMethodCount = 55;
static_assert(kMethodRangeContains + 1 == kMethodCount,
              "the last method selector must match the helper count");

// The method names a static member read can reach. The same name may belong to
// more than one owner (`contains`, `indexOf`, `clone`, `clear`, `reversed`), so
// every selector with that name is enabled: the receiver type is not known at
// compile time, and a false positive only keeps one extra body.
bool enableMethodsForName(const std::string& name,
                          std::array<bool, kMethodCount>& enabled) {
  struct Entry {
    const char* name;
    std::uint64_t selector;
  };
  static constexpr Entry kEntries[] = {
      {"push", kMethodArrayPush},
      {"pop", kMethodArrayPop},
      {"shift", kMethodArrayShift},
      {"unshift", kMethodArrayUnshift},
      {"insert", kMethodArrayInsert},
      {"removeAt", kMethodArrayRemoveAt},
      {"contains", kMethodArrayContains},
      {"indexOf", kMethodArrayIndexOf},
      {"clear", kMethodArrayClear},
      {"slice", kMethodArraySlice},
      {"concat", kMethodArrayConcat},
      {"reversed", kMethodArrayReversed},
      {"clone", kMethodArrayClone},
      {"sum", kMethodArraySum},
      {"join", kMethodArrayJoin},
      {"map", kMethodArrayMap},
      {"filter", kMethodArrayFilter},
      {"each", kMethodArrayEach},
      {"find", kMethodArrayFind},
      {"any", kMethodArrayAny},
      {"all", kMethodArrayAll},
      {"reduce", kMethodArrayReduce},
      {"sort", kMethodArraySort},
      {"sorted", kMethodArraySorted},
      {"keys", kMethodMapKeys},
      {"values", kMethodMapValues},
      {"entries", kMethodMapEntries},
      {"has", kMethodMapHas},
      {"get", kMethodMapGet},
      {"set", kMethodMapSet},
      {"delete", kMethodMapDelete},
      {"merge", kMethodMapMerge},
      {"clone", kMethodMapClone},
      {"clear", kMethodMapClear},
      {"upper", kMethodStrUpper},
      {"lower", kMethodStrLower},
      {"trim", kMethodStrTrim},
      {"trimStart", kMethodStrTrimStart},
      {"trimEnd", kMethodStrTrimEnd},
      {"split", kMethodStrSplit},
      {"lines", kMethodStrLines},
      {"replace", kMethodStrReplace},
      {"replaceFirst", kMethodStrReplaceFirst},
      {"startsWith", kMethodStrStartsWith},
      {"endsWith", kMethodStrEndsWith},
      {"contains", kMethodStrContains},
      {"indexOf", kMethodStrIndexOf},
      {"substr", kMethodStrSubstr},
      {"chars", kMethodStrChars},
      {"repeat", kMethodStrRepeat},
      {"reversed", kMethodStrReversed},
      {"padStart", kMethodStrPadStart},
      {"padEnd", kMethodStrPadEnd},
      {"toArray", kMethodRangeToArray},
      {"contains", kMethodRangeContains},
  };
  static_assert(std::size(kEntries) == kMethodCount,
                "every method helper needs a name entry");
  bool any = false;
  for (const Entry& entry : kEntries) {
    if (name == entry.name) {
      enabled[static_cast<std::size_t>(entry.selector)] = true;
      any = true;
    }
  }
  return any;
}

// mt19937_64 state (PHASE 9.3): 312 words followed by the cursor, the same
// layout the x86-64 runtime uses, so a seeded stream is identical on both
// backends and in the VM.
constexpr std::uint32_t kMtWords = 312;
constexpr std::uint32_t kMtMiddleWord = 156;
constexpr std::uint32_t kMtStateBytes = kMtWords * 8;
constexpr std::uint32_t kMtCursorOffset = kMtStateBytes;
constexpr std::uint32_t kMtStateTotal = kMtStateBytes + 8;
constexpr std::uint64_t kMtDefaultSeed = 5489ULL;

// Dragon4 scratch (PHASE 9.3): bignum state plus the digit buffer and the
// input fields. Byte offsets are kept explicit because the emitted helpers
// address the block directly.
constexpr std::uint32_t kDtoaLimbs = 48;
constexpr std::uint32_t kDtoaLimbStride = 8;
constexpr std::uint32_t kDtoaBnW = 8;
constexpr std::uint32_t kDtoaBnBytes = kDtoaBnW + kDtoaLimbs * kDtoaLimbStride;
constexpr std::uint32_t kDtoaR = 0;
constexpr std::uint32_t kDtoaS = kDtoaR + kDtoaBnBytes;
constexpr std::uint32_t kDtoaMp = kDtoaS + kDtoaBnBytes;
constexpr std::uint32_t kDtoaMm = kDtoaMp + kDtoaBnBytes;
constexpr std::uint32_t kDtoaT = kDtoaMm + kDtoaBnBytes;
constexpr std::uint32_t kDtoaDigits = kDtoaT + kDtoaBnBytes;
constexpr std::uint32_t kDtoaNd = kDtoaDigits + 64;
constexpr std::uint32_t kDtoaK = kDtoaNd + 8;
constexpr std::uint32_t kDtoaF = kDtoaK + 8;
constexpr std::uint32_t kDtoaE = kDtoaF + 8;
constexpr std::uint32_t kDtoaP = kDtoaE + 8;
constexpr std::uint32_t kDtoaP2 = kDtoaP + 8;
constexpr std::uint32_t kDtoaMinE = kDtoaP2 + 8;
constexpr std::uint32_t kDtoaSign = kDtoaMinE + 8;
constexpr std::uint32_t kDtoaBoundary = kDtoaSign + 8;
constexpr std::uint32_t kDtoaOddM = kDtoaBoundary + 8;
constexpr std::uint32_t kDtoaLow = kDtoaOddM + 8;
constexpr std::uint32_t kDtoaHigh = kDtoaLow + 8;
constexpr std::uint32_t kDtoaD = kDtoaHigh + 8;
constexpr std::uint32_t kDtoaScratchBytes = kDtoaD + 8;

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

  static bool sameTypes(const std::vector<std::uint8_t>& left,
                        const std::vector<std::uint8_t>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
      if (left[i] != right[i]) return false;
    }
    return true;
  }

  std::uint32_t typeFor(const std::vector<std::uint8_t>& params,
                        const std::vector<std::uint8_t>& results) {
    for (std::size_t i = 0; i < types.size(); ++i) {
      if (sameTypes(types[i].params, params) && sameTypes(types[i].results, results)) {
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
  void memoryFill() {
    op(0xFC);
    uleb(body_.code, 0x0B);
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
  void i64Clz() { op(0x79); }
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
  [[maybe_unused]] ModuleBuilder& module_;
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

  std::uint32_t addZeros(std::uint32_t size) {
    alignTo(8);
    std::uint32_t address = base + static_cast<std::uint32_t>(bytes.size());
    bytes.resize(bytes.size() + size, 0);
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

  // One `{first, last, delta}` run is 12 bytes, little-endian, matching the
  // layout `caseLookup` reads.
  std::uint32_t addCaseRuns(const UnicodeCaseRun* runs, std::size_t count) {
    alignTo(4);
    std::uint32_t address = base + static_cast<std::uint32_t>(bytes.size());
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint32_t first = runs[i].first;
      const std::uint32_t last = runs[i].last;
      const std::uint32_t delta = static_cast<std::uint32_t>(runs[i].delta);
      for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>(first >> (8 * b)));
      for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>(last >> (8 * b)));
      for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>(delta >> (8 * b)));
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
                tStart = 0, tBin = 0, tFmtDouble = 0,
                tUn = 0, tTruthy = 0, tAlloc = 0, tIter = 0, tFmtInt = 0,
                tNewArray = 0, tNewMap = 0, tArrayPush = 0, tArrayGet = 0,
                tArraySet = 0, tMapFind = 0, tMapSet = 0, tMapGet = 0,
                tMapDelete = 0, tEquals = 0, tToStrDepth = 0, tInvoke = 0,
                tBindMethod = 0;

  // Table slot 0 is the shared method dispatcher. A method closure stores that
  // slot and its selector in `flags`; the dispatcher calls the helper.
  std::uint32_t methodDispatch = 0;
  std::vector<std::uint32_t> methodHelpers;

  std::uint32_t writeImport = 0, exitImport = 0, timeImport = 0,
                clockImport = 0;

  // Static strings the helpers and the builtins share.
  std::uint32_t sNil = 0, sTrue = 0, sFalse = 0, sNan = 0, sInf = 0, sNegInf = 0,
                sInfinity = 0,
                sZero = 0, sNegZero = 0, pow10Table = 0,
                sDotDot = 0, sDotDotEq = 0, sPrefix = 0, sNewline = 0,
                sSpace = 0, sSpaceCloseBrace = 0, sQuote = 0, sErrorPrefix = 0;
  std::uint32_t sTypeNil = 0, sTypeBool = 0, sTypeInt = 0, sTypeFloat = 0,
                sTypeDouble = 0, sTypeString = 0, sTypeFunction = 0,
                sTypeRange = 0, sTypeError = 0, sTypeArray = 0, sTypeMap = 0,
                sTypePtr = 0, sTypeCString = 0, sPtrDisplay = 0, sUnknown = 0;
  std::uint32_t sDivZero = 0, sBadShift = 0, sNonIntegral = 0, sLargeDouble = 0,
                sNotIterable = 0, sNotComparable = 0, sUnsupportedOperands = 0,
                sUnsupportedUnary = 0, sBadPower = 0, sNotCallable = 0;
  std::uint32_t sFnOpen = 0, sNativeOpen = 0, sAngleClose = 0, sComma = 0,
                sCloseParen = 0;
  std::uint32_t sBadArity = 0, sRangeBounds = 0, sChrRange = 0, sChrSurrogate = 0,
                sOrdSingle = 0, sSqrtNegative = 0, sIntString = 0, sIntNonFinite = 0,
                sRandomHigh = 0, sRandomBounds = 0, sRandomArgc = 0,
                sRandomType = 0, sSeedArgc = 0, sSeedType = 0,
                sIntRange = 0, sFloatString = 0, sAssertFailed = 0, sPanic = 0,
                sNonNull = 0, sLenType = 0;
  std::uint32_t sToArrayArgc = 0, sContainsArgc = 0, sRangeTooLarge = 0,
                sRangeMembership = 0;
  std::uint32_t sCStringArgc = 0, sCStringType = 0, sBufferArgc = 0,
                sBufferType = 0, sBufferNegative = 0, sCstrArgc = 0,
                sCstrType = 0, sCstrFreed = 0, sByteLenArgc = 0,
                sByteLenType = 0, sByteLenFreed = 0, sLoadArgc = 0,
                sLoadType = 0, sLoadFreed = 0, sLoadNeeds = 0,
                sLoadIndexType = 0, sLoadRangePrefix = 0,
                sLoadRangeSuffix = 0, sStoreArgc = 0, sStoreType = 0,
                sStoreFreed = 0, sStoreNeeds = 0, sStoreIndexType = 0,
                sStoreByteType = 0, sStoreRangePrefix = 0,
                sStoreRangeSuffix = 0, sStoreByteRange = 0,
                sIntToPtrArgc = 0, sIntToPtrType = 0, sPtrToIntArgc = 0,
                sPtrToIntType = 0, sPtrToIntFreed = 0, sFreeArgc = 0,
                sFreeType = 0, sFreeFreed = 0, sBufferTooLarge = 0,
                sOutOfMemory = 0;
  std::uint32_t sMessage = 0, sCode = 0, sNoMember = 0, sCannotIndex = 0,
                sStringImmutable = 0, sIndexRange = 0, sKeyValue = 0;
  std::uint32_t sInsertRange = 0, sRemoveAtRange = 0, sInsertIndex = 0,
                sRemoveAtIndex = 0, sConcatArray = 0, sSumType = 0,
                sReduceEmpty = 0, sComparatorBool = 0, sMapKey = 0,
                sMergeMap = 0;
  std::uint32_t sOpenBracket = 0, sCloseBracket = 0, sOpenBrace = 0,
                sCloseBrace = 0, sColonSpace = 0, sEllipsis = 0, sLength = 0,
                sPush = 0, sPop = 0, sShift = 0, sUnshift = 0, sInsert = 0,
                sRemoveAt = 0, sContains = 0, sToArray = 0, sIndexOf = 0,
                sClear = 0,
                sSlice = 0, sConcat = 0, sReversed = 0, sClone = 0,
                sSum = 0, sJoin = 0, sMap = 0, sFilter = 0, sEach = 0,
                sFind = 0, sAny = 0, sAll = 0, sReduce = 0, sSort = 0,
                sSorted = 0, sKeys = 0, sValues = 0, sEntries = 0,
                sHas = 0, sGet = 0, sSet = 0, sDelete = 0, sMerge = 0,
                sCannotSetField = 0, sNoField = 0, sCannotSlice = 0, sSliceStartInt = 0,
                sSliceEndInt = 0, sSliceStepInt = 0, sSliceStepZero = 0;
  std::uint32_t sArrayIndexInt = 0, sMapKeyString = 0, sStringIndexInt = 0,
                sArrayRange = 0, sMapMembership = 0, sStringMembership = 0,
                sCannotAssignIndex = 0;
  std::uint32_t sUpper = 0, sLower = 0, sTrim = 0, sTrimStart = 0,
                sTrimEnd = 0, sSplit = 0, sLines = 0, sReplace = 0,
                sReplaceFirst = 0, sStartsWith = 0, sEndsWith = 0,
                sSubstr = 0, sChars = 0, sRepeat = 0, sPadStart = 0,
                sPadEnd = 0, sStrArg = 0, sIntArg = 0, sRepeatNegative = 0,
                sRepeatLarge = 0, sReplaceEmpty = 0, sReplaceFirstEmpty = 0,
                sPadStartFill = 0, sPadEndFill = 0, sPadStartLarge = 0,
                sPadEndLarge = 0;
  std::uint32_t upperTable = 0, lowerTable = 0;
  std::uint32_t upperCount = 0, lowerCount = 0;
  // PHASE 9.4: only the bound methods the program can reach keep a body, and
  // the Unicode case tables are only emitted when `upper`/`lower` are used.
  std::array<bool, kMethodCount> methodEnabled{};
  bool useUpper = true, useLower = true;

  std::uint32_t randomState = 0, randomSeedFn = 0, randomTwistFn = 0,
                randomNextFn = 0;
  int randomStateSlot = -1;

  std::uint32_t dtoaScratch = 0, dtoaGenerate = 0;
  std::uint32_t bnSetU64 = 0, bnSetU32 = 0, bnCopy = 0, bnCmp = 0,
                bnMulSmall = 0, bnShl = 0, bnSub = 0, bnAdd = 0,
                bnMulPow10 = 0, bnDivMod = 0;

  std::uint32_t alloc = 0, fail = 0, writeStr = 0, utf8Len = 0, newStr = 0,
                concat = 0, strCmp = 0, fmtInt = 0, fmtDouble = 0, fmtRange = 0,
                toStr = 0, quote = 0, reprStr = 0, truthy = 0, iterStep = 0,
                typeName = 0, fmtFunc = 0, memberGet = 0, memberSet = 0,
                utf8ByteOffset = 0, indexGet = 0, indexSet = 0;
  std::uint32_t utf8SeqLen = 0, decodeCodePoint = 0, encodeUtf8 = 0,
                caseLookup = 0, caseMap = 0, strFind = 0, strSliceBytes = 0,
                strReverse = 0;
  std::uint32_t newArray = 0, newMap = 0, arrayPush = 0, arrayGet = 0,
                arraySet = 0, mapFind = 0, mapSet = 0, mapGet = 0,
                mapDelete = 0, mapClear = 0, equals = 0, toStrDepth = 0,
                reprDepth = 0, slice = 0, parseIntStr = 0,
                parseDoubleStr = 0, wordMatch = 0;
  std::uint32_t newPtr = 0, ptrTextLen = 0, ptrText = 0, ptrTypeName = 0;

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
    tFmtDouble = m.typeFor({kI64, kI64}, {kI32});
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
    sInfinity = pool.addString("infinity");
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
    sSpaceCloseBrace = pool.addString(" }");
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
    sTypeCString = pool.addString("cstring");
    sPtrDisplay = pool.addString("<ptr>");
    sUnknown = pool.addString("unknown");
    sDivZero = pool.addString("division by zero");
    sBadShift = pool.addString("shift count must be between 0 and 63");
    sNonIntegral = pool.addString(
        "the WebAssembly backend cannot format a non-integral double yet (PHASE 9)");
    sLargeDouble = pool.addString(
        "the WebAssembly backend cannot format an out-of-range double yet (PHASE 9)");
    sNotIterable = pool.addString("cannot iterate over a value of type");
    sNotComparable = pool.addString("cannot compare these values");
    sUnsupportedOperands = pool.addString("unsupported operand types for");
    sUnsupportedUnary = pool.addString("unsupported unary operator for type");
    sBadPower = pool.addString("pow: unsupported exponent in the WebAssembly backend");
    sNotCallable = pool.addString("cannot call a value of type");
    sFnOpen = pool.addString("<fn ");
    sNativeOpen = pool.addString("<native fn ");
    sAngleClose = pool.addString(">");
    sComma = pool.addString(", ");
    sCloseParen = pool.addString(")");
    sBadArity = pool.addString("wrong number of arguments to a builtin");
    sRangeBounds = pool.addString("range bounds must be integers");
    sToArrayArgc = pool.addString("toArray() expects 0 arguments");
    sContainsArgc = pool.addString("contains() expects 1 argument");
    sRangeTooLarge = pool.addString("range is too large to convert to an array");
    sRangeMembership = pool.addString("range membership requires an integer");
    sCStringArgc = pool.addString("cstring() expects 1 argument");
    sCStringType = pool.addString("cstring() expects a string argument");
    sBufferArgc = pool.addString("buffer() expects 1 argument");
    sBufferType = pool.addString("buffer() expects an integer size");
    sBufferNegative = pool.addString("buffer() size must not be negative");
    sCstrArgc = pool.addString("cstr() expects 1 argument");
    sCstrType = pool.addString("cstr() expects a pointer argument");
    sCstrFreed = pool.addString("cstr() used a pointer after it was freed");
    sByteLenArgc = pool.addString("byteLen() expects 1 argument");
    sByteLenType = pool.addString("byteLen() expects a pointer argument");
    sByteLenFreed = pool.addString("byteLen() used a pointer after it was freed");
    sLoadArgc = pool.addString("loadByte() expects 2 arguments");
    sLoadType = pool.addString("loadByte() expects a pointer value");
    sLoadFreed = pool.addString("loadByte() used a pointer after it was freed");
    sLoadNeeds = pool.addString("loadByte() needs a cstring or buffer");
    sLoadIndexType = pool.addString("loadByte() expects an integer index");
    sLoadRangePrefix = pool.addString("loadByte() index ");
    sLoadRangeSuffix = pool.addString(" is out of range");
    sStoreArgc = pool.addString("storeByte() expects 3 arguments");
    sStoreType = pool.addString("storeByte() expects a pointer value");
    sStoreFreed = pool.addString("storeByte() used a pointer after it was freed");
    sStoreNeeds = pool.addString("storeByte() needs a buffer (a cstring is read-only)");
    sStoreIndexType = pool.addString("storeByte() expects an integer index");
    sStoreByteType = pool.addString("storeByte() expects an integer byte");
    sStoreRangePrefix = pool.addString("storeByte() index ");
    sStoreRangeSuffix = pool.addString(" is out of range");
    sStoreByteRange = pool.addString("storeByte() byte must be in 0..255");
    sIntToPtrArgc = pool.addString("intToPtr() expects 1 argument");
    sIntToPtrType = pool.addString("intToPtr() expects an integer token");
    sPtrToIntArgc = pool.addString("ptrToInt() expects 1 argument");
    sPtrToIntType = pool.addString("ptrToInt() expects a pointer argument");
    sPtrToIntFreed = pool.addString("ptrToInt() used a pointer after it was freed");
    sFreeArgc = pool.addString("free() expects 1 argument");
    sFreeType = pool.addString("free() expects a pointer");
    sFreeFreed = pool.addString("free() used a pointer after it was freed");
    sBufferTooLarge = pool.addString("buffer() size is too large");
    sOutOfMemory = pool.addString("out of memory");
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
    sLenType = pool.addString("len() is not defined for values of type");
    sMessage = pool.addString("message");
    sCode = pool.addString("code");
    sNoMember = pool.addString("type has no member");
    sCannotIndex = pool.addString("cannot index a value of type");
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
    sPush = pool.addString("push");
    sPop = pool.addString("pop");
    sShift = pool.addString("shift");
    sUnshift = pool.addString("unshift");
    sInsert = pool.addString("insert");
    sRemoveAt = pool.addString("removeAt");
    sContains = pool.addString("contains");
    sToArray = pool.addString("toArray");
    sIndexOf = pool.addString("indexOf");
    sClear = pool.addString("clear");
    sSlice = pool.addString("slice");
    sConcat = pool.addString("concat");
    sReversed = pool.addString("reversed");
    sClone = pool.addString("clone");
    sConcatArray = pool.addString("concat() expects an array");
    sSum = pool.addString("sum");
    sJoin = pool.addString("join");
    sMap = pool.addString("map");
    sFilter = pool.addString("filter");
    sEach = pool.addString("each");
    sFind = pool.addString("find");
    sAny = pool.addString("any");
    sAll = pool.addString("all");
    sReduce = pool.addString("reduce");
    sSort = pool.addString("sort");
    sSorted = pool.addString("sorted");
    sSumType = pool.addString("sum() expects an array of numbers");
    sReduceEmpty = pool.addString("reduce() of an empty array needs an initial value");
    sComparatorBool = pool.addString("sort comparator must return a bool");
    sKeys = pool.addString("keys");
    sValues = pool.addString("values");
    sEntries = pool.addString("entries");
    sHas = pool.addString("has");
    sGet = pool.addString("get");
    sSet = pool.addString("set");
    sDelete = pool.addString("delete");
    sMerge = pool.addString("merge");
    sMapKey = pool.addString("map keys must be strings");
    sMergeMap = pool.addString("merge() expects a map");
    sInsertRange = pool.addString("insert() index out of range");
    sRemoveAtRange = pool.addString("removeAt() index out of range");
    sInsertIndex = pool.addString("insert() index must be an integer");
    sRemoveAtIndex = pool.addString("removeAt() index must be an integer");
    sCannotSetField = pool.addString("cannot set a field on a value of type");
    sNoField = pool.addString("class instance has no such field");
    sCannotSlice = pool.addString("cannot slice a value of type");
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
    sUpper = pool.addString("upper");
    sLower = pool.addString("lower");
    sTrim = pool.addString("trim");
    sTrimStart = pool.addString("trimStart");
    sTrimEnd = pool.addString("trimEnd");
    sSplit = pool.addString("split");
    sLines = pool.addString("lines");
    sReplace = pool.addString("replace");
    sReplaceFirst = pool.addString("replaceFirst");
    sStartsWith = pool.addString("startsWith");
    sEndsWith = pool.addString("endsWith");
    sSubstr = pool.addString("substr");
    sChars = pool.addString("chars");
    sRepeat = pool.addString("repeat");
    sPadStart = pool.addString("padStart");
    sPadEnd = pool.addString("padEnd");
    sStrArg = pool.addString("expects a string argument");
    sIntArg = pool.addString("expects an integer argument");
    sRepeatNegative = pool.addString("repeat() count cannot be negative");
    sRepeatLarge = pool.addString("repeat() result is too large");
    sReplaceEmpty = pool.addString("replace() search text cannot be empty");
    sReplaceFirstEmpty = pool.addString("replaceFirst() search text cannot be empty");
    sPadStartFill = pool.addString("padStart() fill cannot be empty");
    sPadEndFill = pool.addString("padEnd() fill cannot be empty");
    sPadStartLarge = pool.addString("padStart() result is too large");
    sPadEndLarge = pool.addString("padEnd() result is too large");
    // The case tables are 12 bytes per run (16 KB together). A program that
    // never reads `upper`/`lower` does not need either one (PHASE 9.4).
    if (useUpper) {
      upperTable = pool.addCaseRuns(kUnicodeUpperRuns, kUnicodeUpperRunCount);
      upperCount = static_cast<std::uint32_t>(kUnicodeUpperRunCount);
    }
    if (useLower) {
      lowerTable = pool.addCaseRuns(kUnicodeLowerRuns, kUnicodeLowerRunCount);
      lowerCount = static_cast<std::uint32_t>(kUnicodeLowerRunCount);
    }
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
  void emitDtoaHelpers();
  std::uint32_t emitRandomSeed();
  std::uint32_t emitRandomTwist();
  std::uint32_t emitRandomNext();
  std::uint32_t emitRandomState();
  std::uint32_t emitFail();
  std::uint32_t emitWriteStr();
  std::uint32_t emitUtf8Len();
  std::uint32_t emitUtf8SeqLen();
  std::uint32_t emitDecodeCodePoint();
  std::uint32_t emitEncodeUtf8();
  std::uint32_t emitCaseLookup();
  std::uint32_t emitCaseMap();
  std::uint32_t emitStrFind();
  std::uint32_t emitStrSliceBytes();
  std::uint32_t emitStrReverse();
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
  std::uint32_t emitNewPtr();
  std::uint32_t emitPtrTextLen();
  std::uint32_t emitPtrText();
  std::uint32_t emitPtrTypeName();
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
  std::uint32_t emitWordMatch();
  std::uint32_t emitBinary(int op, std::uint32_t failAddress, const char* failText);
  std::uint32_t emitUnary(int op);
  std::uint32_t emitCompare(int op);
  std::uint32_t emitIn();
  std::uint32_t emitBuiltin(const std::string& name);
  std::uint32_t emitMemberGet();
  std::uint32_t emitMemberSet();
  void reserveArrayMethods();
  void emitArrayMethods();
  void emitStringMethods();
  void emitRangeMethods();
  void emitMethodDispatch();
  void emitMethodValue(Func& f, const char* owner, const char* method,
                       std::uint64_t selector);
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
  // `need` wrapped past the 32-bit address space: the allocation cannot fit.
  f.lget(need);
  f.lget(cur);
  f.i32LtU();
  f.if_();
  emitFailStatic(f, sOutOfMemory);
  f.end();
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
  f.i32c(-1);
  f.i32Eq();
  f.if_();
  emitFailStatic(f, sOutOfMemory);
  f.end();
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

void Runtime::emitDtoaHelpers() {
  // bn_set_u64(Bignum* bn, long value)
  bnSetU64 = m.reserveFunction(m.typeFor({kI32, kI64}, {}));
  {
    Func f(m, bnSetU64, {kI32, kI64});
    std::uint32_t hi = f.addLocal(kI64);
    f.lget(0); f.i64c(0); f.i64Store(0);
    f.lget(1); f.i64Eqz(); f.if_(); f.ret(); f.end();
    f.lget(0); f.lget(1); f.i32WrapI64(); f.i64ExtendI32U(); f.i64Store(kDtoaBnW);
    f.lget(1); f.i64c(32); f.i64ShrU(); f.lset(hi);
    f.lget(hi); f.i64Eqz(); f.if_();
    f.lget(0); f.i64c(1); f.i64Store(0);
    f.else_();
    f.lget(0); f.lget(hi); f.i64Store(kDtoaBnW + 8);
    f.lget(0); f.i64c(2); f.i64Store(0);
    f.end();
  }

  // bn_set_u32(Bignum* bn, long value)
  bnSetU32 = m.reserveFunction(m.typeFor({kI32, kI64}, {}));
  {
    Func f(m, bnSetU32, {kI32, kI64});
    f.lget(0); f.i64c(0); f.i64Store(0);
    f.lget(1); f.i64Eqz(); f.if_(); f.ret(); f.end();
    f.lget(0); f.lget(1); f.i32WrapI64(); f.i64ExtendI32U(); f.i64Store(kDtoaBnW);
    f.lget(0); f.i64c(1); f.i64Store(0);
  }

  // bn_copy(Bignum* dst, const Bignum* src)
  bnCopy = m.reserveFunction(m.typeFor({kI32, kI32}, {}));
  {
    Func f(m, bnCopy, {kI32, kI32});
    std::uint32_t n = f.addLocal(kI64), i = f.addLocal(kI32);
    f.lget(1); f.i64Load(0); f.lset(n);
    f.lget(0); f.lget(n); f.i64Store(0);
    f.i32c(0); f.lset(i);
    f.block(); f.loop();
    f.lget(i); f.i64ExtendI32U(); f.lget(n); f.i64GeU(); f.brIf(1);
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add();
    f.lget(1); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add(); f.i64Load(kDtoaBnW);
    f.i64Store(kDtoaBnW);
    f.lget(i); f.i32c(1); f.i32Add(); f.lset(i);
    f.br(0); f.end(); f.end();
  }

  // long bn_cmp(const Bignum* a, const Bignum* b)
  bnCmp = m.reserveFunction(m.typeFor({kI32, kI32}, {kI32}));
  {
    Func f(m, bnCmp, {kI32, kI32});
    std::uint32_t na = f.addLocal(kI64), nb = f.addLocal(kI64),
                  i = f.addLocal(kI32), av = f.addLocal(kI64), bv = f.addLocal(kI64);
    f.lget(0); f.i64Load(0); f.lset(na);
    f.lget(1); f.i64Load(0); f.lset(nb);
    f.lget(na); f.lget(nb); f.i64Ne(); f.if_();
    f.lget(na); f.lget(nb); f.i64GtU(); f.ifResult(kI32);
    f.i32c(1); f.else_(); f.i32c(-1); f.end(); f.ret();
    f.end();
    f.lget(na); f.i32WrapI64(); f.i32c(1); f.i32Sub(); f.lset(i);
    f.block(); f.loop();
    f.lget(i); f.i32c(0); f.i32LtS(); f.brIf(1);
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add(); f.i64Load(kDtoaBnW); f.lset(av);
    f.lget(1); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add(); f.i64Load(kDtoaBnW); f.lset(bv);
    f.lget(av); f.lget(bv); f.i64GtU(); f.if_(); f.i32c(1); f.ret(); f.end();
    f.lget(av); f.lget(bv); f.i64LtU(); f.if_(); f.i32c(-1); f.ret(); f.end();
    f.lget(i); f.i32c(1); f.i32Sub(); f.lset(i);
    f.br(0); f.end(); f.end();
    f.i32c(0);
  }

  // void bn_mul_small(Bignum* bn, long m)
  bnMulSmall = m.reserveFunction(m.typeFor({kI32, kI64}, {}));
  {
    Func f(m, bnMulSmall, {kI32, kI64});
    std::uint32_t n = f.addLocal(kI64), i = f.addLocal(kI32),
                  carry = f.addLocal(kI64), prod = f.addLocal(kI64), mm = f.addLocal(kI64);
    f.lget(1); f.lset(mm);
    f.lget(0); f.i64Load(0); f.lset(n);
    f.i32c(0); f.lset(i);
    f.i64c(0); f.lset(carry);
    f.block(); f.loop();
    f.lget(i); f.i64ExtendI32U(); f.lget(n); f.i64GeU(); f.brIf(1);
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add(); f.i64Load(kDtoaBnW);
    f.lget(mm); f.i64Mul(); f.lget(carry); f.i64Add(); f.lset(prod);
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add();
    f.lget(prod); f.i64c(0xFFFFFFFFLL); f.i64And(); f.i64Store(kDtoaBnW);
    f.lget(prod); f.i64c(32); f.i64ShrU(); f.lset(carry);
    f.lget(i); f.i32c(1); f.i32Add(); f.lset(i);
    f.br(0); f.end(); f.end();
    f.block(); f.loop();
    f.lget(carry); f.i64Eqz(); f.brIf(1);
    f.lget(0); f.lget(n); f.i32WrapI64(); f.i32c(8); f.i32Mul(); f.i32Add();
    f.lget(carry); f.i64c(0xFFFFFFFFLL); f.i64And(); f.i64Store(kDtoaBnW);
    f.lget(carry); f.i64c(32); f.i64ShrU(); f.lset(carry);
    f.lget(n); f.i64c(1); f.i64Add(); f.lset(n);
    f.br(0); f.end(); f.end();
    f.lget(0); f.lget(n); f.i64Store(0);
  }

  // void bn_shl(Bignum* bn, long bits)
  bnShl = m.reserveFunction(m.typeFor({kI32, kI64}, {}));
  {
    Func f(m, bnShl, {kI32, kI64});
    std::uint32_t n = f.addLocal(kI64), limbs = f.addLocal(kI64), rem = f.addLocal(kI64),
                  i = f.addLocal(kI32), limb = f.addLocal(kI64), carry = f.addLocal(kI64);
    f.lget(0); f.i64Load(0); f.lset(n);
    f.lget(n); f.i64Eqz(); f.if_(); f.ret(); f.end();
    f.lget(1); f.i64Eqz(); f.if_(); f.ret(); f.end();
    f.lget(1); f.i64c(5); f.i64ShrU(); f.lset(limbs);
    f.lget(1); f.i64c(31); f.i64And(); f.lset(rem);
    f.lget(n); f.i32WrapI64(); f.i32c(1); f.i32Sub(); f.lset(i);
    f.block(); f.loop();
    f.lget(i); f.i32c(0); f.i32LtS(); f.brIf(1);
    f.lget(0); f.lget(i); f.lget(limbs); f.i32WrapI64(); f.i32Add(); f.i32c(8); f.i32Mul(); f.i32Add();
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add(); f.i64Load(kDtoaBnW);
    f.i64Store(kDtoaBnW);
    f.lget(i); f.i32c(1); f.i32Sub(); f.lset(i);
    f.br(0); f.end(); f.end();
    f.i32c(0); f.lset(i);
    f.block(); f.loop();
    f.lget(i); f.i64ExtendI32U(); f.lget(limbs); f.i64GeU(); f.brIf(1);
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add(); f.i64c(0); f.i64Store(kDtoaBnW);
    f.lget(i); f.i32c(1); f.i32Add(); f.lset(i);
    f.br(0); f.end(); f.end();
    f.lget(n); f.lget(limbs); f.i64Add(); f.lset(n);
    f.lget(rem); f.i64Eqz(); f.if_();
    f.lget(0); f.lget(n); f.i64Store(0); f.ret();
    f.end();
    f.i64c(0); f.lset(carry);
    f.i32c(0); f.lset(i);
    f.block(); f.loop();
    f.lget(i); f.i64ExtendI32U(); f.lget(n); f.i64GeU(); f.brIf(1);
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add(); f.i64Load(kDtoaBnW); f.lset(limb);
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add();
    f.lget(limb); f.lget(rem); f.i64Shl(); f.lget(carry); f.i64Or();
    f.i64c(0xFFFFFFFFLL); f.i64And(); f.i64Store(kDtoaBnW);
    f.lget(limb); f.i64c(32); f.lget(rem); f.i64Sub(); f.i64ShrU(); f.lset(carry);
    f.lget(i); f.i32c(1); f.i32Add(); f.lset(i);
    f.br(0); f.end(); f.end();
    f.lget(carry); f.i64Eqz(); f.if_();
    f.lget(0); f.lget(n); f.i64Store(0); f.ret();
    f.end();
    f.lget(0); f.lget(n); f.i32WrapI64(); f.i32c(8); f.i32Mul(); f.i32Add();
    f.lget(carry); f.i64Store(kDtoaBnW);
    f.lget(n); f.i64c(1); f.i64Add(); f.lset(n);
    f.lget(0); f.lget(n); f.i64Store(0);
  }

  // void bn_sub(Bignum* a, const Bignum* b)
  bnSub = m.reserveFunction(m.typeFor({kI32, kI32}, {}));
  {
    Func f(m, bnSub, {kI32, kI32});
    std::uint32_t na = f.addLocal(kI64), nb = f.addLocal(kI64), i = f.addLocal(kI32),
                  borrow = f.addLocal(kI64), av = f.addLocal(kI64), bv = f.addLocal(kI64),
                  diff = f.addLocal(kI64);
    f.lget(0); f.i64Load(0); f.lset(na);
    f.lget(1); f.i64Load(0); f.lset(nb);
    f.i32c(0); f.lset(i);
    f.i64c(0); f.lset(borrow);
    f.block(); f.loop();
    f.lget(i); f.i64ExtendI32U(); f.lget(na); f.i64GeU(); f.brIf(1);
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add(); f.i64Load(kDtoaBnW); f.lset(av);
    f.i64c(0); f.lset(bv);
    f.lget(i); f.i64ExtendI32U(); f.lget(nb); f.i64LtU(); f.if_();
    f.lget(1); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add(); f.i64Load(kDtoaBnW); f.lset(bv);
    f.end();
    f.lget(av); f.lget(bv); f.i64Sub(); f.lget(borrow); f.i64Sub(); f.lset(diff);
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add();
    f.lget(diff); f.i64c(0xFFFFFFFFLL); f.i64And(); f.i64Store(kDtoaBnW);
    f.lget(diff); f.i64c(63); f.i64ShrS(); f.i64c(1); f.i64And(); f.lset(borrow);
    f.lget(i); f.i32c(1); f.i32Add(); f.lset(i);
    f.br(0); f.end(); f.end();
    f.block(); f.loop();
    f.lget(na); f.i64Eqz(); f.brIf(1);
    f.lget(0); f.lget(na); f.i32WrapI64(); f.i32c(1); f.i32Sub(); f.i32c(8); f.i32Mul(); f.i32Add();
    f.i64Load(kDtoaBnW); f.i64Eqz(); f.i32Eqz(); f.brIf(1);
    f.lget(na); f.i64c(1); f.i64Sub(); f.lset(na);
    f.br(0); f.end(); f.end();
    f.lget(0); f.lget(na); f.i64Store(0);
  }

  // void bn_add(Bignum* a, const Bignum* b)
  bnAdd = m.reserveFunction(m.typeFor({kI32, kI32}, {}));
  {
    Func f(m, bnAdd, {kI32, kI32});
    std::uint32_t na = f.addLocal(kI64), nb = f.addLocal(kI64), n = f.addLocal(kI64),
                  i = f.addLocal(kI32), carry = f.addLocal(kI64),
                  av = f.addLocal(kI64), bv = f.addLocal(kI64), sum = f.addLocal(kI64);
    f.lget(0); f.i64Load(0); f.lset(na);
    f.lget(1); f.i64Load(0); f.lset(nb);
    f.lget(na); f.lset(n);
    f.lget(n); f.lget(nb); f.i64LtU(); f.if_(); f.lget(nb); f.lset(n); f.end();
    f.i32c(0); f.lset(i);
    f.i64c(0); f.lset(carry);
    f.block(); f.loop();
    f.lget(i); f.i64ExtendI32U(); f.lget(n); f.i64GeU(); f.brIf(1);
    f.i64c(0); f.lset(av);
    f.lget(i); f.i64ExtendI32U(); f.lget(na); f.i64LtU(); f.if_();
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add(); f.i64Load(kDtoaBnW); f.lset(av);
    f.end();
    f.i64c(0); f.lset(bv);
    f.lget(i); f.i64ExtendI32U(); f.lget(nb); f.i64LtU(); f.if_();
    f.lget(1); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add(); f.i64Load(kDtoaBnW); f.lset(bv);
    f.end();
    f.lget(av); f.lget(bv); f.i64Add(); f.lget(carry); f.i64Add(); f.lset(sum);
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add();
    f.lget(sum); f.i64c(0xFFFFFFFFLL); f.i64And(); f.i64Store(kDtoaBnW);
    f.lget(sum); f.i64c(32); f.i64ShrU(); f.lset(carry);
    f.lget(i); f.i32c(1); f.i32Add(); f.lset(i);
    f.br(0); f.end(); f.end();
    f.lget(carry); f.i64Eqz(); f.if_(); f.lget(0); f.lget(n); f.i64Store(0); f.ret(); f.end();
    f.lget(0); f.lget(n); f.i32WrapI64(); f.i32c(8); f.i32Mul(); f.i32Add();
    f.lget(carry); f.i64Store(kDtoaBnW);
    f.lget(n); f.i64c(1); f.i64Add(); f.lset(n);
    f.lget(0); f.lget(n); f.i64Store(0);
  }

  // void bn_mul_pow10(Bignum* bn, long k)
  bnMulPow10 = m.reserveFunction(m.typeFor({kI32, kI64}, {}));
  {
    Func f(m, bnMulPow10, {kI32, kI64});
    std::uint32_t k = f.addLocal(kI64);
    f.lget(1); f.lset(k);
    f.block(); f.loop();
    f.lget(k); f.i64c(9); f.i64LtS(); f.brIf(1);
    f.lget(0); f.i64c(1000000000LL); f.call(bnMulSmall);
    f.lget(k); f.i64c(9); f.i64Sub(); f.lset(k);
    f.br(0); f.end(); f.end();
    f.block(); f.loop();
    f.lget(k); f.i64Eqz(); f.brIf(1);
    f.lget(0); f.i64c(10); f.call(bnMulSmall);
    f.lget(k); f.i64c(1); f.i64Sub(); f.lset(k);
    f.br(0); f.end(); f.end();
  }

  // long bn_divmod_small(Bignum* bn, long divisor)
  bnDivMod = m.reserveFunction(m.typeFor({kI32, kI64}, {kI64}));
  {
    Func f(m, bnDivMod, {kI32, kI64});
    std::uint32_t n = f.addLocal(kI64), i = f.addLocal(kI32), rem = f.addLocal(kI64),
                  cur = f.addLocal(kI64), q = f.addLocal(kI64), d = f.addLocal(kI64);
    f.lget(1); f.lset(d);
    f.lget(0); f.i64Load(0); f.lset(n);
    f.lget(n); f.i32WrapI64(); f.i32c(1); f.i32Sub(); f.lset(i);
    f.i64c(0); f.lset(rem);
    f.block(); f.loop();
    f.lget(i); f.i32c(0); f.i32LtS(); f.brIf(1);
    f.lget(rem); f.i64c(32); f.i64Shl();
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add(); f.i64Load(kDtoaBnW);
    f.i64Or(); f.lset(cur);
    f.lget(cur); f.lget(d); f.i64DivU(); f.lset(q);
    f.lget(cur); f.lget(d); f.i64RemU(); f.lset(rem);
    f.lget(0); f.lget(i); f.i32c(8); f.i32Mul(); f.i32Add();
    f.lget(q); f.i64c(0xFFFFFFFFLL); f.i64And(); f.i64Store(kDtoaBnW);
    f.lget(i); f.i32c(1); f.i32Sub(); f.lset(i);
    f.br(0); f.end(); f.end();
    f.block(); f.loop();
    f.lget(n); f.i64Eqz(); f.brIf(1);
    f.lget(0); f.lget(n); f.i32WrapI64(); f.i32c(1); f.i32Sub(); f.i32c(8); f.i32Mul(); f.i32Add();
    f.i64Load(kDtoaBnW); f.i64Eqz(); f.i32Eqz(); f.brIf(1);
    f.lget(n); f.i64c(1); f.i64Sub(); f.lset(n);
    f.br(0); f.end(); f.end();
    f.lget(0); f.lget(n); f.i64Store(0);
    f.lget(rem);
  }

  // void generate(Scratch* s): Dragon4 free-format digits.
  dtoaGenerate = m.reserveFunction(m.typeFor({kI32}, {}));
  {
    Func f(m, dtoaGenerate, {kI32});
    std::uint32_t boundary = f.addLocal(kI32),
                  k = f.addLocal(kI64), nd = f.addLocal(kI64), d = f.addLocal(kI64),
                  low = f.addLocal(kI32), high = f.addLocal(kI32), i = f.addLocal(kI32),
                  mval = f.addLocal(kI64), q = f.addLocal(kI64),
                  cmp = f.addLocal(kI32), shift = f.addLocal(kI64),
                  roundUp = f.addLocal(kI32), carry = f.addLocal(kI64),
                  digit = f.addLocal(kI64);

    auto addr = [&](std::uint32_t off) {
      f.lget(0);
      if (off != 0) { f.i32c(static_cast<std::int32_t>(off)); f.i32Add(); }
    };
    auto load = [&](std::uint32_t off) { addr(off); f.i64Load(0); };

    // boundary = (f == p2) && (e > minE)
    load(kDtoaF); load(kDtoaP2); f.i64Eq(); f.lset(boundary);
    load(kDtoaE); load(kDtoaMinE); f.i64GtS(); f.lget(boundary); f.i32And(); f.lset(boundary);

    load(kDtoaE); f.i64c(0); f.i64GeS(); f.if_();
      // r = f << (boundary ? e+2 : e+1)
      addr(kDtoaR); load(kDtoaF); f.call(bnSetU64);
      load(kDtoaE); f.i64c(1); f.i64Add(); f.lset(shift);
      f.lget(boundary); f.if_(); f.lget(shift); f.i64c(1); f.i64Add(); f.lset(shift); f.end();
      addr(kDtoaR); f.lget(shift); f.call(bnShl);
      f.lget(boundary); f.if_();
        addr(kDtoaS); f.i64c(4); f.call(bnSetU32);
        addr(kDtoaMp); f.i64c(1); f.call(bnSetU32);
        addr(kDtoaMp); load(kDtoaE); f.i64c(1); f.i64Add(); f.call(bnShl);
        addr(kDtoaMm); f.i64c(1); f.call(bnSetU32);
        addr(kDtoaMm); load(kDtoaE); f.call(bnShl);
      f.else_();
        addr(kDtoaS); f.i64c(2); f.call(bnSetU32);
        addr(kDtoaMp); f.i64c(1); f.call(bnSetU32);
        addr(kDtoaMp); load(kDtoaE); f.call(bnShl);
        addr(kDtoaMm); addr(kDtoaMp); f.call(bnCopy);
      f.end();
    f.else_();
      // e < 0: r = f << (boundary ? 2 : 1)
      addr(kDtoaR); load(kDtoaF); f.call(bnSetU64);
      f.lget(boundary); f.i64ExtendI32U(); f.i64c(1); f.i64Add(); f.lset(shift);
      addr(kDtoaR); f.lget(shift); f.call(bnShl);
      f.lget(boundary); f.if_();
        addr(kDtoaS); f.i64c(1); f.call(bnSetU32);
        addr(kDtoaS); load(kDtoaE); f.i64c(-1); f.i64Mul(); f.i64c(2); f.i64Add(); f.call(bnShl);
        addr(kDtoaMp); f.i64c(2); f.call(bnSetU32);
        addr(kDtoaMm); f.i64c(1); f.call(bnSetU32);
      f.else_();
        addr(kDtoaS); f.i64c(1); f.call(bnSetU32);
        addr(kDtoaS); load(kDtoaE); f.i64c(-1); f.i64Mul(); f.i64c(1); f.i64Add(); f.call(bnShl);
        addr(kDtoaMp); f.i64c(1); f.call(bnSetU32);
        addr(kDtoaMm); f.i64c(1); f.call(bnSetU32);
      f.end();
    f.end();

    // m = e + bsr(f); q = floor(m*30103/100000); k = q + 1
    f.i64c(63); load(kDtoaF); f.i64Clz(); f.i64Sub();
    load(kDtoaE); f.i64Add(); f.lset(mval);
    f.lget(mval); f.i64c(30103); f.i64Mul(); f.i64c(100000); f.i64DivS(); f.lset(q);
    f.lget(mval); f.i64c(30103); f.i64Mul(); f.i64c(100000); f.i64RemS(); f.i64Eqz();
    f.if_();
    f.else_();
      f.lget(mval); f.i64c(0); f.i64LtS(); f.if_();
      f.lget(q); f.i64c(1); f.i64Sub(); f.lset(q);
      f.end();
    f.end();
    f.lget(q); f.i64c(1); f.i64Add(); f.lset(k);

    f.lget(k); f.i64c(0); f.i64GeS(); f.if_();
      addr(kDtoaS); f.lget(k); f.call(bnMulPow10);
    f.else_();
      f.lget(k); f.i64c(-1); f.i64Mul(); f.lset(shift);
      addr(kDtoaR); f.lget(shift); f.call(bnMulPow10);
      addr(kDtoaMp); f.lget(shift); f.call(bnMulPow10);
      addr(kDtoaMm); f.lget(shift); f.call(bnMulPow10);
    f.end();

    // while (r >= s) { s *= 10; k++ }
    f.block(); f.loop();
    addr(kDtoaR); addr(kDtoaS); f.call(bnCmp); f.i32c(0); f.i32LtS(); f.brIf(1);
    addr(kDtoaS); f.i64c(10); f.call(bnMulSmall);
    f.lget(k); f.i64c(1); f.i64Add(); f.lset(k);
    f.br(0); f.end(); f.end();

    // while (r*10 < s) { r,mp,mm *= 10; k-- }
    f.block(); f.loop();
    addr(kDtoaT); addr(kDtoaR); f.call(bnCopy);
    addr(kDtoaT); f.i64c(10); f.call(bnMulSmall);
    addr(kDtoaT); addr(kDtoaS); f.call(bnCmp); f.i32c(0); f.i32GeS(); f.brIf(1);
    addr(kDtoaR); f.i64c(10); f.call(bnMulSmall);
    addr(kDtoaMp); f.i64c(10); f.call(bnMulSmall);
    addr(kDtoaMm); f.i64c(10); f.call(bnMulSmall);
    f.lget(k); f.i64c(1); f.i64Sub(); f.lset(k);
    f.br(0); f.end(); f.end();

    f.i64c(0); f.lset(nd);
    f.block(); f.loop();  // outer digit loop
      addr(kDtoaR); f.i64c(10); f.call(bnMulSmall);
      addr(kDtoaMp); f.i64c(10); f.call(bnMulSmall);
      addr(kDtoaMm); f.i64c(10); f.call(bnMulSmall);
      f.i64c(0); f.lset(d);
      f.block(); f.loop();  // inner subtraction loop
        addr(kDtoaR); addr(kDtoaS); f.call(bnCmp); f.lset(cmp);
        f.lget(cmp); f.i32c(0); f.i32LtS(); f.brIf(1);
        addr(kDtoaR); addr(kDtoaS); f.call(bnSub);
        f.lget(d); f.i64c(1); f.i64Add(); f.lset(d);
        f.br(0);
      f.end(); f.end();
      addr(kDtoaDigits); f.lget(nd); f.i32WrapI64(); f.i32Add();
      f.lget(d); f.i32WrapI64(); f.i32Store8(0);
      f.lget(nd); f.i64c(1); f.i64Add(); f.lset(nd);

      // low = oddM ? (r < mm) : (r <= mm)
      addr(kDtoaR); addr(kDtoaMm); f.call(bnCmp); f.lset(cmp);
      load(kDtoaOddM); f.i64Eqz(); f.if_();
        f.lget(cmp); f.i32c(0); f.i32LeS(); f.lset(low);
      f.else_();
        f.lget(cmp); f.i32c(0); f.i32LtS(); f.lset(low);
      f.end();

      // high = oddM ? (r+mp > s) : (r+mp >= s)
      addr(kDtoaT); addr(kDtoaR); f.call(bnCopy);
      addr(kDtoaT); addr(kDtoaMp); f.call(bnAdd);
      addr(kDtoaT); addr(kDtoaS); f.call(bnCmp); f.lset(cmp);
      load(kDtoaOddM); f.i64Eqz(); f.if_();
        f.lget(cmp); f.i32c(0); f.i32GeS(); f.lset(high);
      f.else_();
        f.lget(cmp); f.i32c(0); f.i32GtS(); f.lset(high);
      f.end();

      f.lget(low); f.i32Eqz(); f.lget(high); f.i32Eqz(); f.i32And();
      f.if_(); f.br(1); f.end();

      // roundUp = (low != high) ? high : (2r > s) || (2r == s && last digit odd)
      f.i32c(0); f.lset(roundUp);
      f.lget(low); f.lget(high); f.i32Ne(); f.if_();
        f.lget(high); f.lset(roundUp);
      f.else_();
        addr(kDtoaT); addr(kDtoaR); f.call(bnCopy);
        addr(kDtoaT); addr(kDtoaR); f.call(bnAdd);
        addr(kDtoaT); addr(kDtoaS); f.call(bnCmp); f.lset(cmp);
        f.lget(cmp); f.i32c(0); f.i32GtS(); f.if_();
          f.i32c(1); f.lset(roundUp);
        f.else_();
          f.lget(cmp); f.i32c(0); f.i32Eq(); f.if_();
            f.lget(nd); f.i32WrapI64(); f.i32c(1); f.i32Sub(); f.lset(i);
            addr(kDtoaDigits); f.lget(i); f.i32Add(); f.i32Load8u(0); f.i32c(1); f.i32And(); f.lset(roundUp);
          f.end();
        f.end();
      f.end();

      f.lget(roundUp); f.if_();
        f.lget(nd); f.i32WrapI64(); f.i32c(1); f.i32Sub(); f.lset(i);
        f.i64c(1); f.lset(carry);
        f.block(); f.loop();
          f.lget(i); f.i32c(0); f.i32LtS(); f.brIf(1);
          f.lget(carry); f.i64Eqz(); f.brIf(1);
          addr(kDtoaDigits); f.lget(i); f.i32Add(); f.i32Load8u(0); f.i64ExtendI32U(); f.lset(digit);
          f.lget(digit); f.i64c(9); f.i64Eq(); f.if_();
            addr(kDtoaDigits); f.lget(i); f.i32Add(); f.i32c(0); f.i32Store8(0);
            f.lget(i); f.i32c(1); f.i32Sub(); f.lset(i);
          f.else_();
            addr(kDtoaDigits); f.lget(i); f.i32Add(); f.lget(digit); f.i32WrapI64(); f.i32c(1); f.i32Add(); f.i32Store8(0);
            f.i64c(0); f.lset(carry);
          f.end();
          f.br(0);
        f.end(); f.end();
        f.lget(carry); f.i64Eqz(); f.i32Eqz(); f.if_();
          f.lget(nd); f.i32WrapI64(); f.lset(i);
          f.block(); f.loop();
            f.lget(i); f.i32c(0); f.i32LeS(); f.brIf(1);
            addr(kDtoaDigits); f.lget(i); f.i32Add();
            addr(kDtoaDigits); f.lget(i); f.i32c(1); f.i32Sub(); f.i32Add(); f.i32Load8u(0);
            f.i32Store8(0);
            f.lget(i); f.i32c(1); f.i32Sub(); f.lset(i);
            f.br(0);
          f.end(); f.end();
          addr(kDtoaDigits); f.i32c(1); f.i32Store8(0);
          f.lget(nd); f.i64c(1); f.i64Add(); f.lset(nd);
          f.lget(k); f.i64c(1); f.i64Add(); f.lset(k);
        f.end();
      f.end();
      f.br(1);
    f.end(); f.end();

    // strip trailing zeros
    f.block(); f.loop();
    f.lget(nd); f.i64c(1); f.i64LeS(); f.brIf(1);
    f.lget(nd); f.i32WrapI64(); f.i32c(1); f.i32Sub(); f.lset(i);
    addr(kDtoaDigits); f.lget(i); f.i32Add(); f.i32Load8u(0); f.i32Eqz(); f.i32Eqz(); f.brIf(1);
    f.lget(nd); f.i64c(1); f.i64Sub(); f.lset(nd);
    f.br(0); f.end(); f.end();

    addr(kDtoaNd); f.lget(nd); f.i64Store(0);
    addr(kDtoaK); f.lget(k); f.i64Store(0);
  }

}

std::uint32_t Runtime::emitFmtDouble() {
  std::uint32_t index = m.reserveFunction(tFmtDouble);
  Func f(m, index, {kI64, kI64});
  std::uint32_t bits = f.addLocal(kI64), d = f.addLocal(kF64),
                sign = f.addLocal(kI32), qq = f.addLocal(kI64),
                i = f.addLocal(kI32), bestP = f.addLocal(kI32),
                bestQ = f.addLocal(kI64), bestE = f.addLocal(kI32),
                pos = f.addLocal(kI32), outFixed = f.addLocal(kI32),
                outSci = f.addLocal(kI32), lenFixed = f.addLocal(kI32),
                lenSci = f.addLocal(kI32), exp = f.addLocal(kI32),
                digit = f.addLocal(kI64),
                be = f.addLocal(kI64), frac = f.addLocal(kI64), fv = f.addLocal(kI64),
                ev = f.addLocal(kI64), pv = f.addLocal(kI64), minEv = f.addLocal(kI64),
                p2v = f.addLocal(kI64), oddMv = f.addLocal(kI64);

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

  f.lget(1);
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

  // Dragon4: extract the binary64 significand/exponent, generate the
  // shortest round-trip digits, and hand them to the fixed/scientific builders.
  f.lget(bits); f.i64c(52); f.i64ShrU(); f.i64c(0x7FFLL); f.i64And(); f.lset(be);
  f.lget(bits); f.i64c(0xFFFFFFFFFFFFFLL); f.i64And(); f.lset(frac);
  f.lget(be); f.i64Eqz(); f.if_();
    f.lget(frac); f.lset(fv);
    f.i64c(-1074); f.lset(ev);
  f.else_();
    f.lget(frac); f.i64c(0x10000000000000LL); f.i64Or(); f.lset(fv);
    f.lget(be); f.i64c(1075); f.i64Sub(); f.lset(ev);
  f.end();
  f.i64c(53); f.lset(pv);
  f.i64c(-1074); f.lset(minEv);
  f.i64c(1); f.i64c(52); f.i64Shl(); f.lset(p2v);
  f.lget(fv); f.i64c(1); f.i64And(); f.lset(oddMv);
  f.i32c(dtoaScratch + kDtoaF); f.lget(fv); f.i64Store(0);
  f.i32c(dtoaScratch + kDtoaE); f.lget(ev); f.i64Store(0);
  f.i32c(dtoaScratch + kDtoaP); f.lget(pv); f.i64Store(0);
  f.i32c(dtoaScratch + kDtoaMinE); f.lget(minEv); f.i64Store(0);
  f.i32c(dtoaScratch + kDtoaP2); f.lget(p2v); f.i64Store(0);
  f.i32c(dtoaScratch + kDtoaOddM); f.lget(oddMv); f.i64Store(0);
  f.i32c(dtoaScratch); f.call(dtoaGenerate);

  f.i32c(dtoaScratch + kDtoaNd); f.i64Load(0); f.i32WrapI64(); f.lset(bestP);
  f.i32c(dtoaScratch + kDtoaK); f.i64Load(0); f.i32WrapI64(); f.i32c(1); f.i32Sub(); f.lset(bestE);
  f.i64c(0); f.lset(bestQ);
  f.i32c(0); f.lset(i);
  f.block(); f.loop();
  f.lget(i); f.lget(bestP); f.i32GeS(); f.brIf(1);
  f.lget(bestQ); f.i64c(10); f.i64Mul();
  f.i32c(dtoaScratch + kDtoaDigits); f.lget(i); f.i32Add(); f.i32Load8u(0); f.i64ExtendI32U();
  f.i64Add(); f.lset(bestQ);
  f.lget(i); f.i32c(1); f.i32Add(); f.lset(i);
  f.br(0); f.end(); f.end();

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
  (void)idx;  // declared to reserve the local slot, never read
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
    emitFailStatic(f, sUnsupportedUnary);
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
  emitFailStatic(f, sUnsupportedUnary);
  return index;
}

std::uint32_t Runtime::emitCompare(int op) {
  std::uint32_t index = m.reserveFunction(tBin);
  Func f(m, index, {kI64, kI64, kI64, kI64});
  std::uint32_t addrA = f.addLocal(kI32), addrB = f.addLocal(kI32);
  (void)addrB;  // declared to reserve the local slot, never read

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
  emitFailStatic(f, sRangeMembership);
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

std::uint32_t Runtime::emitUtf8SeqLen() {
  std::uint32_t type = m.typeFor({kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32});
  f.lget(0);
  f.i32c(0x80);
  f.i32LtU();
  f.ifResult(kI32);
  f.i32c(1);
  f.else_();
  f.lget(0);
  f.i32c(0xE0);
  f.i32And();
  f.i32c(0xC0);
  f.i32Eq();
  f.ifResult(kI32);
  f.i32c(2);
  f.else_();
  f.lget(0);
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
  return index;
}

std::uint32_t Runtime::emitDecodeCodePoint() {
  std::uint32_t type = m.typeFor({kI32, kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32, kI32});
  std::uint32_t b0 = f.addLocal(kI32), len = f.addLocal(kI32);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32Load8u(0);
  f.lset(b0);
  f.lget(b0);
  f.call(utf8SeqLen);
  f.lset(len);
  f.lget(len);
  f.i32c(1);
  f.i32Eq();
  f.ifResult(kI32);
  f.lget(b0);
  f.else_();
  f.lget(len);
  f.i32c(2);
  f.i32Eq();
  f.ifResult(kI32);
  f.lget(b0);
  f.i32c(0x1F);
  f.i32And();
  f.i32c(6);
  f.i32Shl();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32Load8u(1);
  f.i32c(0x3F);
  f.i32And();
  f.i32Or();
  f.else_();
  f.lget(len);
  f.i32c(3);
  f.i32Eq();
  f.ifResult(kI32);
  f.lget(b0);
  f.i32c(0x0F);
  f.i32And();
  f.i32c(12);
  f.i32Shl();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32Load8u(1);
  f.i32c(0x3F);
  f.i32And();
  f.i32c(6);
  f.i32Shl();
  f.i32Or();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32Load8u(2);
  f.i32c(0x3F);
  f.i32And();
  f.i32Or();
  f.else_();
  f.lget(b0);
  f.i32c(0x07);
  f.i32And();
  f.i32c(18);
  f.i32Shl();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32Load8u(1);
  f.i32c(0x3F);
  f.i32And();
  f.i32c(12);
  f.i32Shl();
  f.i32Or();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32Load8u(2);
  f.i32c(0x3F);
  f.i32And();
  f.i32c(6);
  f.i32Shl();
  f.i32Or();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32Load8u(3);
  f.i32c(0x3F);
  f.i32And();
  f.i32Or();
  f.end();
  f.end();
  f.end();
  return index;
}

std::uint32_t Runtime::emitEncodeUtf8() {
  std::uint32_t type = m.typeFor({kI32, kI32, kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32, kI32, kI32});
  f.lget(2);
  f.i32c(0x80);
  f.i32LtU();
  f.if_();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.lget(2);
  f.i32Store8(0);
  f.lget(1);
  f.i32c(1);
  f.i32Add();
  f.ret();
  f.end();
  f.lget(2);
  f.i32c(0x800);
  f.i32LtU();
  f.if_();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32c(0xC0);
  f.lget(2);
  f.i32c(6);
  f.i32ShrU();
  f.i32Or();
  f.i32Store8(0);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32c(0x80);
  f.lget(2);
  f.i32c(0x3F);
  f.i32And();
  f.i32Or();
  f.i32Store8(1);
  f.lget(1);
  f.i32c(2);
  f.i32Add();
  f.ret();
  f.end();
  f.lget(2);
  f.i32c(0x10000);
  f.i32LtU();
  f.if_();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32c(0xE0);
  f.lget(2);
  f.i32c(12);
  f.i32ShrU();
  f.i32Or();
  f.i32Store8(0);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32c(0x80);
  f.lget(2);
  f.i32c(6);
  f.i32ShrU();
  f.i32c(0x3F);
  f.i32And();
  f.i32Or();
  f.i32Store8(1);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32c(0x80);
  f.lget(2);
  f.i32c(0x3F);
  f.i32And();
  f.i32Or();
  f.i32Store8(2);
  f.lget(1);
  f.i32c(3);
  f.i32Add();
  f.ret();
  f.end();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32c(0xF0);
  f.lget(2);
  f.i32c(18);
  f.i32ShrU();
  f.i32Or();
  f.i32Store8(0);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32c(0x80);
  f.lget(2);
  f.i32c(12);
  f.i32ShrU();
  f.i32c(0x3F);
  f.i32And();
  f.i32Or();
  f.i32Store8(1);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32c(0x80);
  f.lget(2);
  f.i32c(6);
  f.i32ShrU();
  f.i32c(0x3F);
  f.i32And();
  f.i32Or();
  f.i32Store8(2);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.i32c(0x80);
  f.lget(2);
  f.i32c(0x3F);
  f.i32And();
  f.i32Or();
  f.i32Store8(3);
  f.lget(1);
  f.i32c(4);
  f.i32Add();
  f.ret();
  return index;
}

std::uint32_t Runtime::emitCaseLookup() {
  std::uint32_t type = m.typeFor({kI32, kI32, kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32, kI32, kI32});
  std::uint32_t low = f.addLocal(kI32), high = f.addLocal(kI32),
                mid = f.addLocal(kI32), base = f.addLocal(kI32);
  f.i32c(0);
  f.lset(low);
  f.lget(2);
  f.lset(high);
  f.block();
  f.loop();
  f.lget(low);
  f.lget(high);
  f.i32GeU();
  f.brIf(1);
  f.lget(low);
  f.lget(high);
  f.lget(low);
  f.i32Sub();
  f.i32c(1);
  f.i32ShrU();
  f.i32Add();
  f.lset(mid);
  f.lget(1);
  f.lget(mid);
  f.i32c(12);
  f.i32Mul();
  f.i32Add();
  f.lset(base);
  f.lget(0);
  f.lget(base);
  f.i32Load(0);
  f.i32LtU();
  f.if_();
  f.lget(mid);
  f.lset(high);
  f.br(1);
  f.end();
  f.lget(0);
  f.lget(base);
  f.i32Load(4);
  f.i32GtU();
  f.if_();
  f.lget(mid);
  f.i32c(1);
  f.i32Add();
  f.lset(low);
  f.br(1);
  f.end();
  f.lget(0);
  f.lget(base);
  f.i32Load(8);
  f.i32Add();
  f.ret();
  f.end();
  f.end();
  f.lget(0);
  return index;
}

std::uint32_t Runtime::emitCaseMap() {
  std::uint32_t type = m.typeFor({kI32, kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32, kI32});
  std::uint32_t out = f.addLocal(kI32), src = f.addLocal(kI32),
                dst = f.addLocal(kI32), cp = f.addLocal(kI32),
                len = f.addLocal(kI32), mapped = f.addLocal(kI32);
  f.lget(0);
  f.call(utf8Len);
  f.i64c(4);
  f.i64Mul();
  f.i32WrapI64();
  f.call(newStr);
  f.lset(out);
  f.i32c(0);
  f.lset(src);
  f.i32c(0);
  f.lset(dst);
  f.block();
  f.loop();
  f.lget(src);
  f.lget(0);
  f.i32Load(0);
  f.i32GeU();
  f.brIf(1);
  f.lget(0);
  f.lget(src);
  f.call(decodeCodePoint);
  f.lset(cp);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(src);
  f.i32Add();
  f.i32Load8u(0);
  f.call(utf8SeqLen);
  f.lset(len);
  f.lget(cp);
  f.i32c(0x80);
  f.i32LtU();
  f.if_();
  f.lget(1);
  f.if_();
  f.lget(cp);
  f.i32c('a');
  f.i32GeS();
  f.lget(cp);
  f.i32c('z');
  f.i32LeS();
  f.i32And();
  f.if_();
  f.lget(cp);
  f.i32c(0x20);
  f.i32Sub();
  f.lset(cp);
  f.end();
  f.else_();
  f.lget(cp);
  f.i32c('A');
  f.i32GeS();
  f.lget(cp);
  f.i32c('Z');
  f.i32LeS();
  f.i32And();
  f.if_();
  f.lget(cp);
  f.i32c(0x20);
  f.i32Add();
  f.lset(cp);
  f.end();
  f.end();
  f.lget(cp);
  f.lset(mapped);
  f.else_();
  f.lget(cp);
  f.lget(1);
  f.ifResult(kI32);
  f.i32c(static_cast<std::int32_t>(upperTable));
  f.else_();
  f.i32c(static_cast<std::int32_t>(lowerTable));
  f.end();
  f.lget(1);
  f.ifResult(kI32);
  f.i32c(static_cast<std::int32_t>(upperCount));
  f.else_();
  f.i32c(static_cast<std::int32_t>(lowerCount));
  f.end();
  f.call(caseLookup);
  f.lset(mapped);
  f.end();
  f.lget(out);
  f.lget(dst);
  f.lget(mapped);
  f.call(encodeUtf8);
  f.lset(dst);
  f.lget(src);
  f.lget(len);
  f.i32Add();
  f.lset(src);
  f.br(0);
  f.end();
  f.end();
  f.lget(out);
  f.lget(dst);
  f.i64ExtendI32U();
  f.i64Store(0);
  f.lget(out);
  return index;
}

std::uint32_t Runtime::emitStrFind() {
  std::uint32_t type = m.typeFor({kI32, kI32, kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32, kI32, kI32});
  std::uint32_t hl = f.addLocal(kI32), nl = f.addLocal(kI32),
                i = f.addLocal(kI32), j = f.addLocal(kI32);
  f.lget(0);
  f.i32Load(0);
  f.lset(hl);
  f.lget(1);
  f.i32Load(0);
  f.lset(nl);
  f.lget(nl);
  f.i32Eqz();
  f.if_();
  f.lget(2);
  f.lget(hl);
  f.i32LeU();
  f.if_();
  f.lget(2);
  f.ret();
  f.end();
  f.i32c(-1);
  f.ret();
  f.end();
  f.lget(2);
  f.i32c(0);
  f.i32LtS();
  f.if_();
  f.i32c(0);
  f.lset(2);
  f.end();
  f.lget(2);
  f.lget(nl);
  f.i32Add();
  f.lget(hl);
  f.i32GtU();
  f.if_();
  f.i32c(-1);
  f.ret();
  f.end();
  f.lget(2);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(nl);
  f.i32Add();
  f.lget(hl);
  f.i32GtU();
  f.brIf(1);
  f.i32c(0);
  f.lset(j);
  f.block();
  f.loop();
  f.lget(j);
  f.lget(nl);
  f.i32GeU();
  f.brIf(1);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(i);
  f.i32Add();
  f.lget(j);
  f.i32Add();
  f.i32Load8u(0);
  f.lget(1);
  f.i32c(8);
  f.i32Add();
  f.lget(j);
  f.i32Add();
  f.i32Load8u(0);
  f.i32Ne();
  f.brIf(1);
  f.lget(j);
  f.i32c(1);
  f.i32Add();
  f.lset(j);
  f.br(0);
  f.end();
  f.end();
  f.lget(j);
  f.lget(nl);
  f.i32Eq();
  f.if_();
  f.lget(i);
  f.ret();
  f.end();
  f.lget(i);
  f.i32c(1);
  f.i32Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.i32c(-1);
  return index;
}

std::uint32_t Runtime::emitStrSliceBytes() {
  std::uint32_t type = m.typeFor({kI32, kI32, kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32, kI32, kI32});
  std::uint32_t slen = f.addLocal(kI32), out = f.addLocal(kI32);
  f.lget(0);
  f.i32Load(0);
  f.lset(slen);
  f.lget(1);
  f.i32c(0);
  f.i32LtS();
  f.if_();
  f.i32c(0);
  f.lset(1);
  f.end();
  f.lget(1);
  f.lget(slen);
  f.i32GtU();
  f.if_();
  f.lget(slen);
  f.lset(1);
  f.end();
  f.lget(2);
  f.i32c(0);
  f.i32LtS();
  f.if_();
  f.i32c(0);
  f.lset(2);
  f.end();
  f.lget(1);
  f.lget(2);
  f.i32Add();
  f.lget(slen);
  f.i32GtU();
  f.if_();
  f.lget(slen);
  f.lget(1);
  f.i32Sub();
  f.lset(2);
  f.end();
  f.lget(2);
  f.call(newStr);
  f.lset(out);
  f.lget(out);
  f.i32c(8);
  f.i32Add();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(1);
  f.i32Add();
  f.lget(2);
  f.memoryCopy();
  f.lget(out);
  return index;
}

std::uint32_t Runtime::emitStrReverse() {
  std::uint32_t type = m.typeFor({kI32}, {kI32});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI32});
  std::uint32_t len = f.addLocal(kI32), out = f.addLocal(kI32),
                src = f.addLocal(kI32), dst = f.addLocal(kI32),
                clen = f.addLocal(kI32);
  f.lget(0);
  f.i32Load(0);
  f.lset(len);
  f.lget(len);
  f.call(newStr);
  f.lset(out);
  f.i32c(0);
  f.lset(src);
  f.lget(len);
  f.lset(dst);
  f.block();
  f.loop();
  f.lget(src);
  f.lget(len);
  f.i32GeU();
  f.brIf(1);
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(src);
  f.i32Add();
  f.i32Load8u(0);
  f.call(utf8SeqLen);
  f.lset(clen);
  f.lget(dst);
  f.lget(clen);
  f.i32Sub();
  f.lset(dst);
  f.lget(out);
  f.i32c(8);
  f.i32Add();
  f.lget(dst);
  f.i32Add();
  f.lget(0);
  f.i32c(8);
  f.i32Add();
  f.lget(src);
  f.i32Add();
  f.lget(clen);
  f.memoryCopy();
  f.lget(src);
  f.lget(clen);
  f.i32Add();
  f.lset(src);
  f.br(0);
  f.end();
  f.end();
  f.lget(out);
  return index;
}

// Builds a bound-method closure. The receiver is boxed in a cell so the
// closure keeps it alive when the method is detached, and the selector in
// `flags` tells the dispatcher which helper to run.
void Runtime::emitMethodValue(Func& f, const char* owner, const char* method,
                              std::uint64_t selector) {
  // `fmtFunc` adds the `<native fn ...>` wrapper itself, so the header keeps
  // the bare `owner.method` name and marks the closure native in `flags`.
  const std::uint32_t nameAddress =
      pool.addString(std::string(owner) + "." + method);
  const std::uint32_t cell = f.addLocal(kI32), closure = f.addLocal(kI32);
  f.i32c(16);
  f.call(alloc);
  f.lset(cell);
  f.lget(cell);
  f.lget(0);
  f.i64Store(0);
  f.lget(cell);
  f.lget(1);
  f.i64Store(8);
  f.i32c(48);
  f.call(alloc);
  f.lset(closure);
  f.lget(closure);
  f.i64c(0);
  f.i64Store(0);
  f.lget(closure);
  f.i64c(static_cast<std::int64_t>(selector));
  f.i64Store(8);
  f.lget(closure);
  f.i64c(1);
  f.i64Store(16);
  f.lget(closure);
  f.i64c(static_cast<std::int64_t>(nameAddress));
  f.i64Store(24);
  f.lget(closure);
  f.i64c(1);
  f.i64Store(32);
  f.lget(closure);
  f.i32c(40);
  f.i32Add();
  f.lget(cell);
  f.i64ExtendI32U();
  f.i64Store(0);
  f.i64c(static_cast<std::int64_t>(kFunc));
  f.lget(closure);
  f.i64ExtendI32U();
}

// The range's `.length`: the magnitude of the bound difference, plus one when
// inclusive, saturated at INT64_MAX. The subtraction is unsigned, so a span
// wider than INT64_MAX does not wrap to a negative length.
static void emitRangeCount(Func& f, std::uint32_t start, std::uint32_t end,
                           std::uint32_t incl, std::uint32_t count) {
  f.lget(start);
  f.lget(end);
  f.i64LeS();
  f.ifResult(kI64);
  f.lget(end);
  f.lget(start);
  f.i64Sub();
  f.else_();
  f.lget(start);
  f.lget(end);
  f.i64Sub();
  f.end();
  f.lset(count);
  f.lget(incl);
  f.if_();
  f.lget(count);
  f.i64c(-1);
  f.i64Eq();
  f.if_();
  f.i64c(9223372036854775807LL);
  f.lset(count);
  f.else_();
  f.lget(count);
  f.i64c(1);
  f.i64Add();
  f.lset(count);
  f.end();
  f.end();
  f.lget(count);
  f.i64c(9223372036854775807LL);
  f.i64GtU();
  f.if_();
  f.i64c(9223372036854775807LL);
  f.lset(count);
  f.end();
}

std::uint32_t Runtime::emitMemberGet() {
  std::uint32_t index = m.reserveFunction(tBin);
  Func f(m, index, {kI64, kI64, kI64, kI64});
  std::uint32_t addr = f.addLocal(kI32), idx = f.addLocal(kI32),
                entry = f.addLocal(kI32), methods = f.addLocal(kI32),
                start = f.addLocal(kI64),
                end = f.addLocal(kI64), incl = f.addLocal(kI32),
                count = f.addLocal(kI64), range = f.addLocal(kI32);

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
  // A user method in the class method table wins over the built-in map
  // members; a method named `length` must shadow the map length.
  f.lget(1);
  f.i32WrapI64();
  f.i64Load(kMapMethodsOffset);
  f.i32WrapI64();
  f.lset(methods);
  f.lget(methods);
  f.i32c(0);
  f.i32Ne();
  f.if_();
  f.lget(methods);
  f.lget(addr);
  f.call(mapFind);
  f.lset(idx);
  f.lget(idx);
  f.i32c(0);
  f.i32GeS();
  f.if_();
  f.lget(methods);
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
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sKeys));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "map", "keys", kMethodMapKeys);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sValues));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "map", "values", kMethodMapValues);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sEntries));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "map", "entries", kMethodMapEntries);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sHas));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "map", "has", kMethodMapHas);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sGet));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "map", "get", kMethodMapGet);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sSet));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "map", "set", kMethodMapSet);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sDelete));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "map", "delete", kMethodMapDelete);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sMerge));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "map", "merge", kMethodMapMerge);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sClone));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "map", "clone", kMethodMapClone);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sClear));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "map", "clear", kMethodMapClear);
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
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sPush));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "push", kMethodArrayPush);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sPop));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "pop", kMethodArrayPop);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sShift));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "shift", kMethodArrayShift);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sUnshift));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "unshift", kMethodArrayUnshift);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sInsert));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "insert", kMethodArrayInsert);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sRemoveAt));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "removeAt", kMethodArrayRemoveAt);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sContains));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "contains", kMethodArrayContains);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sIndexOf));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "indexOf", kMethodArrayIndexOf);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sClear));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "clear", kMethodArrayClear);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sSlice));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "slice", kMethodArraySlice);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sConcat));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "concat", kMethodArrayConcat);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sReversed));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "reversed", kMethodArrayReversed);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sClone));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "clone", kMethodArrayClone);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sSum));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "sum", kMethodArraySum);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sJoin));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "join", kMethodArrayJoin);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sMap));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "map", kMethodArrayMap);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sFilter));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "filter", kMethodArrayFilter);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sEach));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "each", kMethodArrayEach);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sFind));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "find", kMethodArrayFind);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sAny));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "any", kMethodArrayAny);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sAll));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "all", kMethodArrayAll);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sReduce));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "reduce", kMethodArrayReduce);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sSort));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "sort", kMethodArraySort);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sSorted));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "array", "sorted", kMethodArraySorted);
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
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sUpper));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "upper", kMethodStrUpper);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sLower));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "lower", kMethodStrLower);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sTrim));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "trim", kMethodStrTrim);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sTrimStart));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "trimStart", kMethodStrTrimStart);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sTrimEnd));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "trimEnd", kMethodStrTrimEnd);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sSplit));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "split", kMethodStrSplit);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sLines));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "lines", kMethodStrLines);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sReplace));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "replace", kMethodStrReplace);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sReplaceFirst));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "replaceFirst", kMethodStrReplaceFirst);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sStartsWith));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "startsWith", kMethodStrStartsWith);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sEndsWith));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "endsWith", kMethodStrEndsWith);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sContains));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "contains", kMethodStrContains);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sIndexOf));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "indexOf", kMethodStrIndexOf);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sSubstr));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "substr", kMethodStrSubstr);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sChars));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "chars", kMethodStrChars);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sRepeat));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "repeat", kMethodStrRepeat);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sReversed));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "reversed", kMethodStrReversed);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sPadStart));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "padStart", kMethodStrPadStart);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sPadEnd));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "string", "padEnd", kMethodStrPadEnd);
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
  f.lset(range);
  f.lget(range);
  f.i64Load(0);
  f.lset(start);
  f.lget(range);
  f.i64Load(8);
  f.lset(end);
  f.lget(range);
  f.i64Load(16);
  f.i32WrapI64();
  f.lset(incl);
  emitRangeCount(f, start, end, incl, count);
  f.i64c(static_cast<std::int64_t>(kInt));
  f.lget(count);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sToArray));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "range", "toArray", kMethodRangeToArray);
  f.ret();
  f.end();
  f.lget(addr);
  f.i32c(static_cast<std::int32_t>(sContains));
  f.call(strCmp);
  f.i32Eqz();
  f.if_();
  emitMethodValue(f, "range", "contains", kMethodRangeContains);
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
  std::uint32_t addr = f.addLocal(kI32), methods = f.addLocal(kI32),
                idx = f.addLocal(kI32), name = f.addLocal(kI32),
                className = f.addLocal(kI32);
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
  f.lset(addr);
  f.lget(3);
  f.i32WrapI64();
  f.lset(name);
  // Reassigning a method updates the method table, not a new field.
  f.lget(addr);
  f.i64Load(kMapMethodsOffset);
  f.i32WrapI64();
  f.lset(methods);
  f.lget(methods);
  f.i32c(0);
  f.i32Ne();
  f.if_();
  f.lget(methods);
  f.lget(name);
  f.call(mapFind);
  f.lset(idx);
  f.lget(idx);
  f.i32c(0);
  f.i32GeS();
  f.if_();
  f.lget(methods);
  f.lget(name);
  f.lget(4);
  f.lget(5);
  f.call(mapSet);
  f.i64c(static_cast<std::int64_t>(kNil));
  f.i64c(0);
  f.ret();
  f.end();
  f.end();
  // A class instance has a fixed field set; an unknown name is an error.
  f.lget(addr);
  f.i64Load(kMapClassNameOffset);
  f.i32WrapI64();
  f.lset(className);
  f.lget(className);
  f.i32c(0);
  f.i32Ne();
  f.if_();
  f.lget(addr);
  f.lget(name);
  f.call(mapFind);
  f.lset(idx);
  f.lget(idx);
  f.i32c(0);
  f.i32LtS();
  f.if_();
  emitFailStatic(f, sNoField);
  f.end();
  f.end();
  f.lget(addr);
  f.lget(name);
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

// The method helpers are reserved before `emitCore` so the dispatcher and the
// method closures can refer to them, and their bodies are emitted after it so
// they can call the runtime helpers (`arrayPush`, `alloc`, ...).
// Reads the receiver a method closure captured. The closure's first upvalue
// is a cell holding the receiver's tag and payload.
static void emitReadReceiver(Func& f, std::uint32_t cell, std::uint32_t header,
                             std::uint32_t recvTag, std::uint32_t recvPayload) {
  f.lget(2);
  f.i32c(40);
  f.i32Add();
  f.i64Load(0);
  f.i32WrapI64();
  f.lset(cell);
  f.lget(cell);
  f.i64Load(0);
  f.lset(recvTag);
  f.lget(cell);
  f.i64Load(8);
  f.lset(recvPayload);
  f.lget(recvPayload);
  f.i32WrapI64();
  f.lset(header);
}

// The hosted `clampIndex`: a negative bound counts from the end, then the
// result is clamped to `[0, length]`.
static void emitClampIndex(Func& f, std::uint32_t value, std::uint32_t len) {
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
  f.i64c(0);
  f.i64LtS();
  f.if_();
  f.i64c(0);
  f.lset(value);
  f.end();
  f.lget(value);
  f.lget(len);
  f.i64GtS();
  f.if_();
  f.lget(len);
  f.lset(value);
  f.end();
}

// Calls a callback closure with the given argument locals. The window is built
// upward (`window + 16*j`), matching the `CALL` lowering.
static void emitCallbackCall(
    Func& f, std::uint32_t alloc, std::uint32_t tValue,
    std::uint32_t closure, std::uint32_t slot,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& args,
    std::uint32_t resultTag, std::uint32_t resultPayload,
    std::uint32_t window) {
  f.i32c(static_cast<std::int32_t>(16 * args.size()));
  f.call(alloc);
  f.lset(window);
  for (std::size_t j = 0; j < args.size(); ++j) {
    f.lget(window);
    f.i32c(static_cast<std::int32_t>(16 * j));
    f.i32Add();
    f.lget(args[j].first);
    f.i64Store(0);
    f.lget(window);
    f.i32c(static_cast<std::int32_t>(16 * j));
    f.i32Add();
    f.lget(args[j].second);
    f.i64Store(8);
  }
  f.lget(window);
  f.i32c(static_cast<std::int32_t>(args.size()));
  f.lget(closure);
  f.lget(slot);
  f.callIndirect(tValue);
  f.lset(resultPayload);
  f.lset(resultTag);
}

// Stable insertion sort over an array header. `hasCmp` selects the callback
// comparator; otherwise the default `lessThan` is the `<` operator. Only items
// strictly less than their predecessor move, so equal keys keep their order.
static void emitInsertionSort(
    Runtime& rt, Func& f, std::uint32_t alloc, std::uint32_t tValue, std::uint32_t opLt,
    std::uint32_t truthy, std::uint32_t header, std::uint32_t len,
    std::uint32_t hasCmp, std::uint32_t cbClosure, std::uint32_t cbSlot,
    std::uint32_t window, std::uint32_t items, std::uint32_t i,
    std::uint32_t j, std::uint32_t keyTag, std::uint32_t keyPayload,
    std::uint32_t prevTag, std::uint32_t prevPayload,
    std::uint32_t resTag, std::uint32_t resPayload, std::uint32_t less,
    std::uint32_t sComparatorBool) {
  f.lget(header);
  f.i64Load(kArrayItemsOffset);
  f.i32WrapI64();
  f.lset(items);
  f.i64c(1);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(len);
  f.i64GeU();
  f.brIf(1);
  f.lget(items);
  f.lget(i);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.i64Load(0);
  f.lset(keyTag);
  f.lget(items);
  f.lget(i);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.i64Load(8);
  f.lset(keyPayload);
  f.lget(i);
  f.lset(j);
  f.block();
  f.loop();
  f.lget(j);
  f.i64c(0);
  f.i64LeS();
  f.brIf(1);
  f.lget(items);
  f.lget(j);
  f.i64c(1);
  f.i64Sub();
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.i64Load(0);
  f.lset(prevTag);
  f.lget(items);
  f.lget(j);
  f.i64c(1);
  f.i64Sub();
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.i64Load(8);
  f.lset(prevPayload);
  f.lget(hasCmp);
  f.if_();
  emitCallbackCall(f, alloc, tValue, cbClosure, cbSlot,
                   {{keyTag, keyPayload}, {prevTag, prevPayload}}, resTag,
                   resPayload, window);
  f.lget(resTag);
  f.i64c(static_cast<std::int64_t>(kBool));
  f.i64Ne();
  f.if_();
  rt.emitFailStatic(f, sComparatorBool);
  f.end();
  f.lget(resTag);
  f.lget(resPayload);
  f.call(truthy);
  f.lset(less);
  f.else_();
  f.lget(keyTag);
  f.lget(keyPayload);
  f.lget(prevTag);
  f.lget(prevPayload);
  f.call(opLt);
  f.lset(resPayload);
  f.lset(resTag);
  f.lget(resTag);
  f.lget(resPayload);
  f.call(truthy);
  f.lset(less);
  f.end();
  f.lget(less);
  f.i32Eqz();
  f.brIf(1);
  f.lget(items);
  f.lget(j);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.lget(items);
  f.lget(j);
  f.i64c(1);
  f.i64Sub();
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.i32c(16);
  f.memoryCopy();
  f.lget(j);
  f.i64c(1);
  f.i64Sub();
  f.lset(j);
  f.br(0);
  f.end();
  f.end();
  f.lget(items);
  f.lget(j);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.lget(keyTag);
  f.i64Store(0);
  f.lget(items);
  f.lget(j);
  f.i32WrapI64();
  f.i32c(16);
  f.i32Mul();
  f.i32Add();
  f.lget(keyPayload);
  f.i64Store(8);
  f.lget(i);
  f.i64c(1);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
}

// The method helpers are reserved before `emitCore` so the dispatcher and the
// method closures can refer to them, and their bodies are emitted after it so
// they can call the runtime helpers (`arrayPush`, `equals`, `alloc`, ...).
void Runtime::reserveArrayMethods() {
  methodHelpers.clear();
  methodHelpers.push_back(m.reserveFunction(tValue));  // push
  methodHelpers.push_back(m.reserveFunction(tValue));  // pop
  methodHelpers.push_back(m.reserveFunction(tValue));  // shift
  methodHelpers.push_back(m.reserveFunction(tValue));  // unshift
  methodHelpers.push_back(m.reserveFunction(tValue));  // insert
  methodHelpers.push_back(m.reserveFunction(tValue));  // removeAt
  methodHelpers.push_back(m.reserveFunction(tValue));  // contains
  methodHelpers.push_back(m.reserveFunction(tValue));  // indexOf
  methodHelpers.push_back(m.reserveFunction(tValue));  // clear
  methodHelpers.push_back(m.reserveFunction(tValue));  // slice
  methodHelpers.push_back(m.reserveFunction(tValue));  // concat
  methodHelpers.push_back(m.reserveFunction(tValue));  // reversed
  methodHelpers.push_back(m.reserveFunction(tValue));  // clone
  methodHelpers.push_back(m.reserveFunction(tValue));  // sum
  methodHelpers.push_back(m.reserveFunction(tValue));  // join
  methodHelpers.push_back(m.reserveFunction(tValue));  // map
  methodHelpers.push_back(m.reserveFunction(tValue));  // filter
  methodHelpers.push_back(m.reserveFunction(tValue));  // each
  methodHelpers.push_back(m.reserveFunction(tValue));  // find
  methodHelpers.push_back(m.reserveFunction(tValue));  // any
  methodHelpers.push_back(m.reserveFunction(tValue));  // all
  methodHelpers.push_back(m.reserveFunction(tValue));  // reduce
  methodHelpers.push_back(m.reserveFunction(tValue));  // sort
  methodHelpers.push_back(m.reserveFunction(tValue));  // sorted
  methodHelpers.push_back(m.reserveFunction(tValue));  // keys
  methodHelpers.push_back(m.reserveFunction(tValue));  // values
  methodHelpers.push_back(m.reserveFunction(tValue));  // entries
  methodHelpers.push_back(m.reserveFunction(tValue));  // has
  methodHelpers.push_back(m.reserveFunction(tValue));  // get
  methodHelpers.push_back(m.reserveFunction(tValue));  // set
  methodHelpers.push_back(m.reserveFunction(tValue));  // delete
  methodHelpers.push_back(m.reserveFunction(tValue));  // merge
  methodHelpers.push_back(m.reserveFunction(tValue));  // clone
  methodHelpers.push_back(m.reserveFunction(tValue));  // clear
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.upper
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.lower
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.trim
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.trimStart
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.trimEnd
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.split
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.lines
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.replace
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.replaceFirst
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.startsWith
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.endsWith
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.contains
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.indexOf
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.substr
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.chars
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.repeat
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.reversed
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.padStart
  methodHelpers.push_back(m.reserveFunction(tValue));  // string.padEnd
  methodHelpers.push_back(m.reserveFunction(tValue));  // range.toArray
  methodHelpers.push_back(m.reserveFunction(tValue));  // range.contains
}

void Runtime::emitArrayMethods() {
  {
    Func f(m, methodHelpers[kMethodArrayPush], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  i = f.addLocal(kI32);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.i32c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(1);
    f.i32GeS();
    f.brIf(1);
    f.lget(header);
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
    f.call(arrayPush);
    f.lget(i);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(recvPayload);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayPop], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  addr = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64);
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(len);
    f.i64c(0);
    f.i64Eq();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kNil));
    f.i64c(0);
    f.ret();
    f.end();
    f.lget(len);
    f.i64c(1);
    f.i64Sub();
    f.lset(len);
    f.lget(header);
    f.lget(len);
    f.i64Store(kArrayLengthOffset);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lget(len);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.lset(addr);
    f.lget(addr);
    f.i64Load(0);
    f.lget(addr);
    f.i64Load(8);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayShift], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  addr = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64), tag = f.addLocal(kI64),
                  payload = f.addLocal(kI64);
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(len);
    f.i64c(0);
    f.i64Eq();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kNil));
    f.i64c(0);
    f.ret();
    f.end();
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(addr);
    f.lget(addr);
    f.i64Load(0);
    f.lset(tag);
    f.lget(addr);
    f.i64Load(8);
    f.lset(payload);
    f.lget(addr);
    f.lget(addr);
    f.i32c(16);
    f.i32Add();
    f.lget(len);
    f.i64c(1);
    f.i64Sub();
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.memoryCopy();
    f.lget(header);
    f.lget(len);
    f.i64c(1);
    f.i64Sub();
    f.i64Store(kArrayLengthOffset);
    f.lget(tag);
    f.lget(payload);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayUnshift], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  i = f.addLocal(kI32), addr = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64), lastTag = f.addLocal(kI64),
                  lastPayload = f.addLocal(kI64);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(1);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.i32Eqz();
    f.brIf(1);
    f.lget(i);
    f.i32c(1);
    f.i32Sub();
    f.lset(i);
    f.lget(header);
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
    f.call(arrayPush);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(addr);
    f.lget(addr);
    f.lget(len);
    f.i64c(1);
    f.i64Sub();
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lset(lastTag);
    f.lget(addr);
    f.lget(len);
    f.i64c(1);
    f.i64Sub();
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lset(lastPayload);
    f.lget(addr);
    f.i32c(16);
    f.i32Add();
    f.lget(addr);
    f.lget(len);
    f.i64c(1);
    f.i64Sub();
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.memoryCopy();
    f.lget(addr);
    f.lget(lastTag);
    f.i64Store(0);
    f.lget(addr);
    f.lget(lastPayload);
    f.i64Store(8);
    f.br(0);
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(recvPayload);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayInsert], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  addr = f.addLocal(kI32);
    std::uint32_t idx = f.addLocal(kI64), len = f.addLocal(kI64),
                  oldLen = f.addLocal(kI64), valueTag = f.addLocal(kI64),
                  valuePayload = f.addLocal(kI64);
    f.lget(1);
    f.i32c(2);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sInsertIndex);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.lset(idx);
    f.lget(0);
    f.i32c(16);
    f.i32Add();
    f.i64Load(0);
    f.lset(valueTag);
    f.lget(0);
    f.i32c(16);
    f.i32Add();
    f.i64Load(8);
    f.lset(valuePayload);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(len);
    f.lset(oldLen);
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
    f.i64GtS();
    f.i32Or();
    f.if_();
    emitFailStatic(f, sInsertRange);
    f.end();
    f.lget(header);
    f.lget(valueTag);
    f.lget(valuePayload);
    f.call(arrayPush);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(addr);
    f.lget(addr);
    f.lget(idx);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i32c(16);
    f.i32Add();
    f.lget(addr);
    f.lget(idx);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.lget(oldLen);
    f.lget(idx);
    f.i64Sub();
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.memoryCopy();
    f.lget(addr);
    f.lget(idx);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.lget(valueTag);
    f.i64Store(0);
    f.lget(addr);
    f.lget(idx);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.lget(valuePayload);
    f.i64Store(8);
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(recvPayload);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayRemoveAt], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  addr = f.addLocal(kI32);
    std::uint32_t idx = f.addLocal(kI64), len = f.addLocal(kI64),
                  tag = f.addLocal(kI64), payload = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sRemoveAtIndex);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.lset(idx);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
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
    emitFailStatic(f, sRemoveAtRange);
    f.end();
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(addr);
    f.lget(addr);
    f.lget(idx);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lset(tag);
    f.lget(addr);
    f.lget(idx);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lset(payload);
    f.lget(addr);
    f.lget(idx);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.lget(addr);
    f.lget(idx);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i32c(16);
    f.i32Add();
    f.lget(len);
    f.lget(idx);
    f.i64Sub();
    f.i64c(1);
    f.i64Sub();
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.memoryCopy();
    f.lget(header);
    f.lget(len);
    f.i64c(1);
    f.i64Sub();
    f.i64Store(kArrayLengthOffset);
    f.lget(tag);
    f.lget(payload);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayContains], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  addr = f.addLocal(kI32);
    std::uint32_t i = f.addLocal(kI64), len = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(addr);
    f.i64c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i64GeU();
    f.brIf(1);
    f.lget(addr);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lget(addr);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lget(0);
    f.i64Load(0);
    f.lget(0);
    f.i64Load(8);
    f.i32c(0);
    f.call(equals);
    f.if_();
    f.i64c(static_cast<std::int64_t>(kBool));
    f.i64c(1);
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
    f.i64c(0);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayIndexOf], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  addr = f.addLocal(kI32);
    std::uint32_t i = f.addLocal(kI64), len = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(addr);
    f.i64c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i64GeU();
    f.brIf(1);
    f.lget(addr);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lget(addr);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lget(0);
    f.i64Load(0);
    f.lget(0);
    f.i64Load(8);
    f.i32c(0);
    f.call(equals);
    f.if_();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(i);
    f.ret();
    f.end();
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64c(-1);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayClear], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64);
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64c(0);
    f.i64Store(kArrayLengthOffset);
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(recvPayload);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArraySlice], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  newHeader = f.addLocal(kI32), newItems = f.addLocal(kI32),
                  oldItems = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64), start = f.addLocal(kI64),
                  end = f.addLocal(kI64), count = f.addLocal(kI64);
    f.lget(1);
    f.i32c(2);
    f.i32GtS();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.i64c(0);
    f.lset(start);
    f.lget(len);
    f.lset(end);
    f.lget(1);
    f.i32c(1);
    f.i32GeS();
    f.if_();
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sSliceStartInt);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.lset(start);
    f.end();
    f.lget(1);
    f.i32c(2);
    f.i32GeS();
    f.if_();
    f.lget(0);
    f.i32c(16);
    f.i32Add();
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sSliceEndInt);
    f.end();
    f.lget(0);
    f.i32c(16);
    f.i32Add();
    f.i64Load(8);
    f.lset(end);
    f.end();
    emitClampIndex(f, start, len);
    emitClampIndex(f, end, len);
    f.lget(end);
    f.lget(start);
    f.i64LtS();
    f.if_();
    f.lget(start);
    f.lset(end);
    f.end();
    f.lget(end);
    f.lget(start);
    f.i64Sub();
    f.lset(count);
    f.lget(count);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(newHeader);
    f.drop();
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(oldItems);
    f.lget(newHeader);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(newItems);
    f.lget(newItems);
    f.lget(oldItems);
    f.lget(start);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.lget(count);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.memoryCopy();
    f.lget(newHeader);
    f.lget(count);
    f.i64Store(kArrayLengthOffset);
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(newHeader);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayConcat], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  newHeader = f.addLocal(kI32), newItems = f.addLocal(kI32),
                  selfItems = f.addLocal(kI32), otherItems = f.addLocal(kI32),
                  otherHeader = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64), otherLen = f.addLocal(kI64),
                  total = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kArray));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sConcatArray);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(otherHeader);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(otherHeader);
    f.i64Load(kArrayLengthOffset);
    f.lset(otherLen);
    f.lget(len);
    f.lget(otherLen);
    f.i64Add();
    f.lset(total);
    f.lget(total);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(newHeader);
    f.drop();
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(selfItems);
    f.lget(otherHeader);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(otherItems);
    f.lget(newHeader);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(newItems);
    f.lget(newItems);
    f.lget(selfItems);
    f.lget(len);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.memoryCopy();
    f.lget(newItems);
    f.lget(len);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.lget(otherItems);
    f.lget(otherLen);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.memoryCopy();
    f.lget(newHeader);
    f.lget(total);
    f.i64Store(kArrayLengthOffset);
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(newHeader);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayReversed], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  newHeader = f.addLocal(kI32), newItems = f.addLocal(kI32),
                  oldItems = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64);
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(len);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(newHeader);
    f.drop();
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(oldItems);
    f.lget(newHeader);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(newItems);
    f.i64c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i64GeU();
    f.brIf(1);
    f.lget(newItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.lget(oldItems);
    f.lget(len);
    f.i64c(1);
    f.i64Sub();
    f.lget(i);
    f.i64Sub();
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i32c(16);
    f.memoryCopy();
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.lget(newHeader);
    f.lget(len);
    f.i64Store(kArrayLengthOffset);
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(newHeader);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayClone], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  newHeader = f.addLocal(kI32), newItems = f.addLocal(kI32),
                  oldItems = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64);
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(len);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(newHeader);
    f.drop();
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(oldItems);
    f.lget(newHeader);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(newItems);
    f.lget(newItems);
    f.lget(oldItems);
    f.lget(len);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.memoryCopy();
    f.lget(newHeader);
    f.lget(len);
    f.i64Store(kArrayLengthOffset);
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(newHeader);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArraySum], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  addr = f.addLocal(kI32), allInts = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64),
                  itemTag = f.addLocal(kI64), itemPayload = f.addLocal(kI64),
                  intTotal = f.addLocal(kI64), floatTotal = f.addLocal(kF64);
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(addr);
    f.i32c(1);
    f.lset(allInts);
    f.i64c(0);
    f.lset(intTotal);
    f.f64c(0.0);
    f.lset(floatTotal);
    f.i64c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i64GeU();
    f.brIf(1);
    f.lget(addr);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lset(itemTag);
    f.lget(addr);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lset(itemPayload);
    f.lget(itemTag);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.if_();
    f.lget(intTotal);
    f.lget(itemPayload);
    f.i64Add();
    f.lset(intTotal);
    f.lget(allInts);
    f.i32Eqz();
    f.if_();
    f.lget(floatTotal);
    f.lget(itemPayload);
    f.f64ConvertI64S();
    f.f64Add();
    f.lset(floatTotal);
    f.end();
    f.else_();
    f.lget(itemTag);
    f.i64c(static_cast<std::int64_t>(kFloat));
    f.i64Eq();
    f.lget(itemTag);
    f.i64c(static_cast<std::int64_t>(kDouble));
    f.i64Eq();
    f.i32Or();
    f.if_();
    f.lget(allInts);
    f.if_();
    f.lget(intTotal);
    f.f64ConvertI64S();
    f.lset(floatTotal);
    f.i32c(0);
    f.lset(allInts);
    f.end();
    f.lget(floatTotal);
    emitToF64(f, itemTag, itemPayload);
    f.f64Add();
    f.lset(floatTotal);
    f.else_();
    emitFailStatic(f, sSumType);
    f.end();
    f.end();
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.lget(allInts);
    f.if_();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(intTotal);
    f.ret();
    f.end();
    f.i64c(static_cast<std::int64_t>(kDouble));
    f.lget(floatTotal);
    f.i64ReinterpretF64();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayJoin], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  addr = f.addLocal(kI32), sep = f.addLocal(kI32),
                  out = f.addLocal(kI32), itemStr = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64),
                  itemTag = f.addLocal(kI64), itemPayload = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32GtS();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(1);
    f.i32c(1);
    f.i32Eq();
    f.if_();
    f.lget(0);
    f.i64Load(0);
    f.lget(0);
    f.i64Load(8);
    f.call(toStr);
    f.lset(sep);
    f.else_();
    f.i32c(0);
    f.call(newStr);
    f.lset(sep);
    f.end();
    f.i32c(0);
    f.call(newStr);
    f.lset(out);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(addr);
    f.i64c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i64GeU();
    f.brIf(1);
    f.lget(i);
    f.i64c(0);
    f.i64GtS();
    f.if_();
    f.lget(out);
    f.lget(sep);
    f.call(concat);
    f.lset(out);
    f.end();
    f.lget(addr);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lset(itemTag);
    f.lget(addr);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lset(itemPayload);
    f.lget(itemTag);
    f.lget(itemPayload);
    f.call(toStr);
    f.lset(itemStr);
    f.lget(out);
    f.lget(itemStr);
    f.call(concat);
    f.lset(out);
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(out);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayMap], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  cbClosure = f.addLocal(kI32), cbSlot = f.addLocal(kI32),
                  window = f.addLocal(kI32), newHeader = f.addLocal(kI32),
                  newItems = f.addLocal(kI32), oldItems = f.addLocal(kI32),
                  itemTag = f.addLocal(kI64), itemPayload = f.addLocal(kI64),
                  resTag = f.addLocal(kI64), resPayload = f.addLocal(kI64);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kFunc));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sNotCallable);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(cbClosure);
    f.lget(cbClosure);
    f.i32Load(0);
    f.lset(cbSlot);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(len);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(newHeader);
    f.drop();
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(oldItems);
    f.lget(newHeader);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(newItems);
    f.i64c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i64GeU();
    f.brIf(1);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lset(itemTag);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lset(itemPayload);
    emitCallbackCall(f, alloc, tValue, cbClosure, cbSlot,
                     {{itemTag, itemPayload}}, resTag, resPayload, window);
    f.lget(newItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.lget(resTag);
    f.i64Store(0);
    f.lget(newItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.lget(resPayload);
    f.i64Store(8);
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.lget(newHeader);
    f.lget(len);
    f.i64Store(kArrayLengthOffset);
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(newHeader);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayFilter], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  cbClosure = f.addLocal(kI32), cbSlot = f.addLocal(kI32),
                  window = f.addLocal(kI32), newHeader = f.addLocal(kI32),
                  newItems = f.addLocal(kI32), oldItems = f.addLocal(kI32),
                  itemTag = f.addLocal(kI64), itemPayload = f.addLocal(kI64),
                  resTag = f.addLocal(kI64), resPayload = f.addLocal(kI64);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64),
                  count = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kFunc));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sNotCallable);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(cbClosure);
    f.lget(cbClosure);
    f.i32Load(0);
    f.lset(cbSlot);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(len);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(newHeader);
    f.drop();
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(oldItems);
    f.lget(newHeader);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(newItems);
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
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lset(itemTag);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lset(itemPayload);
    emitCallbackCall(f, alloc, tValue, cbClosure, cbSlot,
                     {{itemTag, itemPayload}}, resTag, resPayload, window);
    f.lget(resTag);
    f.lget(resPayload);
    f.call(truthy);
    f.if_();
    f.lget(newItems);
    f.lget(count);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.lget(itemTag);
    f.i64Store(0);
    f.lget(newItems);
    f.lget(count);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.lget(itemPayload);
    f.i64Store(8);
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
    f.lget(newHeader);
    f.lget(count);
    f.i64Store(kArrayLengthOffset);
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(newHeader);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayEach], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  cbClosure = f.addLocal(kI32), cbSlot = f.addLocal(kI32),
                  window = f.addLocal(kI32), oldItems = f.addLocal(kI32),
                  itemTag = f.addLocal(kI64), itemPayload = f.addLocal(kI64),
                  resTag = f.addLocal(kI64), resPayload = f.addLocal(kI64);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kFunc));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sNotCallable);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(cbClosure);
    f.lget(cbClosure);
    f.i32Load(0);
    f.lset(cbSlot);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(oldItems);
    f.i64c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i64GeU();
    f.brIf(1);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lset(itemTag);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lset(itemPayload);
    emitCallbackCall(f, alloc, tValue, cbClosure, cbSlot,
                     {{itemTag, itemPayload}}, resTag, resPayload, window);
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(recvPayload);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayFind], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  cbClosure = f.addLocal(kI32), cbSlot = f.addLocal(kI32),
                  window = f.addLocal(kI32), oldItems = f.addLocal(kI32),
                  itemTag = f.addLocal(kI64), itemPayload = f.addLocal(kI64),
                  resTag = f.addLocal(kI64), resPayload = f.addLocal(kI64);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kFunc));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sNotCallable);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(cbClosure);
    f.lget(cbClosure);
    f.i32Load(0);
    f.lset(cbSlot);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(oldItems);
    f.i64c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i64GeU();
    f.brIf(1);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lset(itemTag);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lset(itemPayload);
    emitCallbackCall(f, alloc, tValue, cbClosure, cbSlot,
                     {{itemTag, itemPayload}}, resTag, resPayload, window);
    f.lget(resTag);
    f.lget(resPayload);
    f.call(truthy);
    f.if_();
    f.lget(itemTag);
    f.lget(itemPayload);
    f.ret();
    f.end();
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kNil));
    f.i64c(0);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayAny], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  cbClosure = f.addLocal(kI32), cbSlot = f.addLocal(kI32),
                  window = f.addLocal(kI32), oldItems = f.addLocal(kI32),
                  itemTag = f.addLocal(kI64), itemPayload = f.addLocal(kI64),
                  resTag = f.addLocal(kI64), resPayload = f.addLocal(kI64);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kFunc));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sNotCallable);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(cbClosure);
    f.lget(cbClosure);
    f.i32Load(0);
    f.lset(cbSlot);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(oldItems);
    f.i64c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i64GeU();
    f.brIf(1);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lset(itemTag);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lset(itemPayload);
    emitCallbackCall(f, alloc, tValue, cbClosure, cbSlot,
                     {{itemTag, itemPayload}}, resTag, resPayload, window);
    f.lget(resTag);
    f.lget(resPayload);
    f.call(truthy);
    f.if_();
    f.i64c(static_cast<std::int64_t>(kBool));
    f.i64c(1);
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
    f.i64c(0);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayAll], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  cbClosure = f.addLocal(kI32), cbSlot = f.addLocal(kI32),
                  window = f.addLocal(kI32), oldItems = f.addLocal(kI32),
                  itemTag = f.addLocal(kI64), itemPayload = f.addLocal(kI64),
                  resTag = f.addLocal(kI64), resPayload = f.addLocal(kI64);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kFunc));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sNotCallable);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(cbClosure);
    f.lget(cbClosure);
    f.i32Load(0);
    f.lset(cbSlot);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(oldItems);
    f.i64c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i64GeU();
    f.brIf(1);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lset(itemTag);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lset(itemPayload);
    emitCallbackCall(f, alloc, tValue, cbClosure, cbSlot,
                     {{itemTag, itemPayload}}, resTag, resPayload, window);
    f.lget(resTag);
    f.lget(resPayload);
    f.call(truthy);
    f.i32Eqz();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kBool));
    f.i64c(0);
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
    f.i64c(1);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArrayReduce], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  cbClosure = f.addLocal(kI32), cbSlot = f.addLocal(kI32),
                  window = f.addLocal(kI32), oldItems = f.addLocal(kI32),
                  itemTag = f.addLocal(kI64), itemPayload = f.addLocal(kI64),
                  accTag = f.addLocal(kI64), accPayload = f.addLocal(kI64),
                  resTag = f.addLocal(kI64), resPayload = f.addLocal(kI64);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32LtS();
    f.lget(1);
    f.i32c(2);
    f.i32GtS();
    f.i32Or();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kFunc));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sNotCallable);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(cbClosure);
    f.lget(cbClosure);
    f.i32Load(0);
    f.lset(cbSlot);
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(oldItems);
    f.lget(1);
    f.i32c(2);
    f.i32Eq();
    f.if_();
    f.lget(0);
    f.i32c(16);
    f.i32Add();
    f.i64Load(0);
    f.lset(accTag);
    f.lget(0);
    f.i32c(16);
    f.i32Add();
    f.i64Load(8);
    f.lset(accPayload);
    f.i64c(0);
    f.lset(i);
    f.else_();
    f.lget(len);
    f.i64c(0);
    f.i64Eq();
    f.if_();
    emitFailStatic(f, sReduceEmpty);
    f.end();
    f.lget(oldItems);
    f.i64Load(0);
    f.lset(accTag);
    f.lget(oldItems);
    f.i64Load(8);
    f.lset(accPayload);
    f.i64c(1);
    f.lset(i);
    f.end();
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i64GeU();
    f.brIf(1);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(0);
    f.lset(itemTag);
    f.lget(oldItems);
    f.lget(i);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.i32Add();
    f.i64Load(8);
    f.lset(itemPayload);
    emitCallbackCall(f, alloc, tValue, cbClosure, cbSlot,
                     {{accTag, accPayload}, {itemTag, itemPayload}}, resTag,
                     resPayload, window);
    f.lget(resTag);
    f.lset(accTag);
    f.lget(resPayload);
    f.lset(accPayload);
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.lget(accTag);
    f.lget(accPayload);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArraySort], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  cbClosure = f.addLocal(kI32), cbSlot = f.addLocal(kI32),
                  window = f.addLocal(kI32), hasCmp = f.addLocal(kI32),
                  copyHeader = f.addLocal(kI32), copyItems = f.addLocal(kI32),
                  recvItems = f.addLocal(kI32), items = f.addLocal(kI32),
                  i = f.addLocal(kI64), j = f.addLocal(kI64),
                  keyTag = f.addLocal(kI64), keyPayload = f.addLocal(kI64),
                  prevTag = f.addLocal(kI64), prevPayload = f.addLocal(kI64),
                  resTag = f.addLocal(kI64), resPayload = f.addLocal(kI64),
                  less = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32GtS();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.i32c(0);
    f.lset(hasCmp);
    f.i32c(0);
    f.lset(cbClosure);
    f.i32c(0);
    f.lset(cbSlot);
    f.lget(1);
    f.i32c(1);
    f.i32Eq();
    f.if_();
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kFunc));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sNotCallable);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(cbClosure);
    f.lget(cbClosure);
    f.i32Load(0);
    f.lset(cbSlot);
    f.i32c(1);
    f.lset(hasCmp);
    f.end();
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(len);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(copyHeader);
    f.drop();
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(recvItems);
    f.lget(copyHeader);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(copyItems);
    f.lget(copyItems);
    f.lget(recvItems);
    f.lget(len);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.memoryCopy();
    f.lget(copyHeader);
    f.lget(len);
    f.i64Store(kArrayLengthOffset);
    emitInsertionSort(*this, f, alloc, tValue, opLt, truthy, copyHeader, len, hasCmp,
                      cbClosure, cbSlot, window, items, i, j, keyTag,
                      keyPayload, prevTag, prevPayload, resTag, resPayload, less,
                      sComparatorBool);
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(recvItems);
    f.lget(copyHeader);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(copyItems);
    f.lget(recvItems);
    f.lget(copyItems);
    f.lget(len);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.memoryCopy();
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(recvPayload);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodArraySorted], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  cbClosure = f.addLocal(kI32), cbSlot = f.addLocal(kI32),
                  window = f.addLocal(kI32), hasCmp = f.addLocal(kI32),
                  copyHeader = f.addLocal(kI32), copyItems = f.addLocal(kI32),
                  recvItems = f.addLocal(kI32), items = f.addLocal(kI32),
                  i = f.addLocal(kI64), j = f.addLocal(kI64),
                  keyTag = f.addLocal(kI64), keyPayload = f.addLocal(kI64),
                  prevTag = f.addLocal(kI64), prevPayload = f.addLocal(kI64),
                  resTag = f.addLocal(kI64), resPayload = f.addLocal(kI64),
                  less = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32GtS();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.i32c(0);
    f.lset(hasCmp);
    f.i32c(0);
    f.lset(cbClosure);
    f.i32c(0);
    f.lset(cbSlot);
    f.lget(1);
    f.i32c(1);
    f.i32Eq();
    f.if_();
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kFunc));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sNotCallable);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(cbClosure);
    f.lget(cbClosure);
    f.i32Load(0);
    f.lset(cbSlot);
    f.i32c(1);
    f.lset(hasCmp);
    f.end();
    f.lget(header);
    f.i64Load(kArrayLengthOffset);
    f.lset(len);
    f.lget(len);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(copyHeader);
    f.drop();
    f.lget(header);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(recvItems);
    f.lget(copyHeader);
    f.i64Load(kArrayItemsOffset);
    f.i32WrapI64();
    f.lset(copyItems);
    f.lget(copyItems);
    f.lget(recvItems);
    f.lget(len);
    f.i32WrapI64();
    f.i32c(16);
    f.i32Mul();
    f.memoryCopy();
    f.lget(copyHeader);
    f.lget(len);
    f.i64Store(kArrayLengthOffset);
    emitInsertionSort(*this, f, alloc, tValue, opLt, truthy, copyHeader, len, hasCmp,
                      cbClosure, cbSlot, window, items, i, j, keyTag,
                      keyPayload, prevTag, prevPayload, resTag, resPayload, less,
                      sComparatorBool);
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(copyHeader);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodMapKeys], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  entries = f.addLocal(kI32), entry = f.addLocal(kI32),
                  newHeader = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64);
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64Load(kMapLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kMapEntriesOffset);
    f.i32WrapI64();
    f.lset(entries);
    f.lget(len);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(newHeader);
    f.drop();
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
    f.lget(newHeader);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(entry);
    f.i64Load(kMapEntryKeyOffset);
    f.call(arrayPush);
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(newHeader);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodMapValues], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  entries = f.addLocal(kI32), entry = f.addLocal(kI32),
                  newHeader = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64);
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64Load(kMapLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kMapEntriesOffset);
    f.i32WrapI64();
    f.lset(entries);
    f.lget(len);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(newHeader);
    f.drop();
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
    f.lget(newHeader);
    f.lget(entry);
    f.i64Load(kMapEntryTagOffset);
    f.lget(entry);
    f.i64Load(kMapEntryPayloadOffset);
    f.call(arrayPush);
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(newHeader);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodMapEntries], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  entries = f.addLocal(kI32), entry = f.addLocal(kI32),
                  newHeader = f.addLocal(kI32), pair = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64);
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64Load(kMapLengthOffset);
    f.lset(len);
    f.lget(header);
    f.i64Load(kMapEntriesOffset);
    f.i32WrapI64();
    f.lset(entries);
    f.lget(len);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(newHeader);
    f.drop();
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
    f.i64c(2);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(pair);
    f.drop();
    f.lget(pair);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(entry);
    f.i64Load(kMapEntryKeyOffset);
    f.call(arrayPush);
    f.lget(pair);
    f.lget(entry);
    f.i64Load(kMapEntryTagOffset);
    f.lget(entry);
    f.i64Load(kMapEntryPayloadOffset);
    f.call(arrayPush);
    f.lget(newHeader);
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(pair);
    f.i64ExtendI32U();
    f.call(arrayPush);
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(newHeader);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodMapHas], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  keyPtr = f.addLocal(kI32), idx = f.addLocal(kI32);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sMapKey);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(keyPtr);
    f.lget(header);
    f.lget(keyPtr);
    f.call(mapFind);
    f.lset(idx);
    f.i64c(static_cast<std::int64_t>(kBool));
    f.lget(idx);
    f.i32c(0);
    f.i32GeS();
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodMapGet], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  keyPtr = f.addLocal(kI32), idx = f.addLocal(kI32),
                  entries = f.addLocal(kI32), entry = f.addLocal(kI32);
    f.lget(1);
    f.i32c(1);
    f.i32LtS();
    f.lget(1);
    f.i32c(2);
    f.i32GtS();
    f.i32Or();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sMapKey);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(keyPtr);
    f.lget(header);
    f.lget(keyPtr);
    f.call(mapFind);
    f.lset(idx);
    f.lget(idx);
    f.i32c(0);
    f.i32GeS();
    f.if_();
    f.lget(header);
    f.i64Load(kMapEntriesOffset);
    f.i32WrapI64();
    f.lset(entries);
    f.lget(entries);
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
    f.lget(1);
    f.i32c(2);
    f.i32Eq();
    f.if_();
    f.lget(0);
    f.i32c(16);
    f.i32Add();
    f.i64Load(0);
    f.lget(0);
    f.i32c(16);
    f.i32Add();
    f.i64Load(8);
    f.ret();
    f.end();
    f.i64c(static_cast<std::int64_t>(kNil));
    f.i64c(0);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodMapSet], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  keyPtr = f.addLocal(kI32);
    f.lget(1);
    f.i32c(2);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sMapKey);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(keyPtr);
    f.lget(header);
    f.lget(keyPtr);
    f.lget(0);
    f.i32c(16);
    f.i32Add();
    f.i64Load(0);
    f.lget(0);
    f.i32c(16);
    f.i32Add();
    f.i64Load(8);
    f.call(mapSet);
    f.i64c(static_cast<std::int64_t>(kMap));
    f.lget(recvPayload);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodMapDelete], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  keyPtr = f.addLocal(kI32);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sMapKey);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(keyPtr);
    f.i64c(static_cast<std::int64_t>(kBool));
    f.lget(header);
    f.lget(keyPtr);
    f.call(mapDelete);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodMapMerge], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  otherHeader = f.addLocal(kI32), entries = f.addLocal(kI32),
                  entry = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kMap));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sMergeMap);
    f.end();
    f.lget(0);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(otherHeader);
    f.lget(otherHeader);
    f.i64Load(kMapLengthOffset);
    f.lset(len);
    f.lget(otherHeader);
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
    f.lget(header);
    f.lget(entry);
    f.i64Load(kMapEntryKeyOffset);
    f.i32WrapI64();
    f.lget(entry);
    f.i64Load(kMapEntryTagOffset);
    f.lget(entry);
    f.i64Load(kMapEntryPayloadOffset);
    f.call(mapSet);
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kMap));
    f.lget(recvPayload);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodMapClone], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  entries = f.addLocal(kI32), entry = f.addLocal(kI32),
                  newHeader = f.addLocal(kI32);
    std::uint32_t len = f.addLocal(kI64), i = f.addLocal(kI64);
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.call(newMap);
    f.i32WrapI64();
    f.lset(newHeader);
    f.drop();
    f.lget(header);
    f.i64Load(kMapLengthOffset);
    f.lset(len);
    f.lget(header);
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
    f.lget(newHeader);
    f.lget(entry);
    f.i64Load(kMapEntryKeyOffset);
    f.i32WrapI64();
    f.lget(entry);
    f.i64Load(kMapEntryTagOffset);
    f.lget(entry);
    f.i64Load(kMapEntryPayloadOffset);
    f.call(mapSet);
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kMap));
    f.lget(newHeader);
    f.i64ExtendI32U();
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodMapClear], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64);
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.call(mapClear);
    f.i64c(static_cast<std::int64_t>(kMap));
    f.lget(recvPayload);
    f.ret();
  }
}

void Runtime::emitStringMethods() {
  auto argAt = [](Func& f, int index) {
    f.lget(0);
    if (index != 0) {
      f.i32c(16 * index);
      f.i32Add();
    }
  };
  auto needStr = [&](Func& f, int index, std::uint32_t local) {
    argAt(f, index);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sStrArg);
    f.end();
    argAt(f, index);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(local);
  };
  auto needInt = [&](Func& f, int index, std::uint32_t local) {
    argAt(f, index);
    f.i64Load(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sIntArg);
    f.end();
    argAt(f, index);
    f.i64Load(8);
    f.i32WrapI64();
    f.lset(local);
  };
  auto retStr = [](Func& f, std::uint32_t value) {
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(value);
    f.i64ExtendI32U();
    f.ret();
  };
  auto retArray = [](Func& f, std::uint32_t value) {
    f.i64c(static_cast<std::int64_t>(kArray));
    f.lget(value);
    f.i64ExtendI32U();
    f.ret();
  };
  auto retInt = [](Func& f, std::uint32_t value) {
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(value);
    f.i64ExtendI32U();
    f.ret();
  };
  auto retBool = [](Func& f, std::uint32_t value) {
    f.i64c(static_cast<std::int64_t>(kBool));
    f.lget(value);
    f.i64ExtendI32U();
    f.ret();
  };
  auto isSpace = [](Func& f, std::uint32_t b) {
    f.lget(b);
    f.i32c(0x20);
    f.i32Eq();
    f.lget(b);
    f.i32c(0x09);
    f.i32GeU();
    f.lget(b);
    f.i32c(0x0D);
    f.i32LeU();
    f.i32And();
    f.i32Or();
  };
  auto arity0 = [&](Func& f) {
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
  };
  auto arity1 = [&](Func& f) {
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
  };
  auto arity2 = [&](Func& f) {
    f.lget(1);
    f.i32c(2);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
  };
  auto arity1to2 = [&](Func& f) {
    f.lget(1);
    f.i32c(1);
    f.i32LtS();
    f.lget(1);
    f.i32c(2);
    f.i32GtS();
    f.i32Or();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
  };
  auto arity0to1 = [&](Func& f) {
    f.lget(1);
    f.i32c(0);
    f.i32LtS();
    f.lget(1);
    f.i32c(1);
    f.i32GtS();
    f.i32Or();
    f.if_();
    emitFailStatic(f, sBadArity);
    f.end();
  };

  {
    Func f(m, methodHelpers[kMethodStrUpper], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  result = f.addLocal(kI32);
    arity0(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i32c(1);
    f.call(caseMap);
    f.lset(result);
    retStr(f, result);
  }
  {
    Func f(m, methodHelpers[kMethodStrLower], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  result = f.addLocal(kI32);
    arity0(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i32c(0);
    f.call(caseMap);
    f.lset(result);
    retStr(f, result);
  }
  {
    Func f(m, methodHelpers[kMethodStrTrimStart], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  start = f.addLocal(kI32), len = f.addLocal(kI32),
                  b = f.addLocal(kI32), result = f.addLocal(kI32);
    arity0(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i32Load(0);
    f.lset(len);
    f.i32c(0);
    f.lset(start);
    f.block();
    f.loop();
    f.lget(start);
    f.lget(len);
    f.i32GeU();
    f.brIf(1);
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(start);
    f.i32Add();
    f.i32Load8u(0);
    f.lset(b);
    isSpace(f, b);
    f.i32Eqz();
    f.brIf(1);
    f.lget(start);
    f.i32c(1);
    f.i32Add();
    f.lset(start);
    f.br(0);
    f.end();
    f.end();
    f.lget(header);
    f.lget(start);
    f.lget(len);
    f.lget(start);
    f.i32Sub();
    f.call(strSliceBytes);
    f.lset(result);
    retStr(f, result);
  }
  {
    Func f(m, methodHelpers[kMethodStrTrimEnd], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  end = f.addLocal(kI32), b = f.addLocal(kI32),
                  result = f.addLocal(kI32);
    arity0(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i32Load(0);
    f.lset(end);
    f.block();
    f.loop();
    f.lget(end);
    f.i32c(0);
    f.i32LeS();
    f.brIf(1);
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(end);
    f.i32Add();
    f.i32c(1);
    f.i32Sub();
    f.i32Load8u(0);
    f.lset(b);
    isSpace(f, b);
    f.i32Eqz();
    f.brIf(1);
    f.lget(end);
    f.i32c(1);
    f.i32Sub();
    f.lset(end);
    f.br(0);
    f.end();
    f.end();
    f.lget(header);
    f.i32c(0);
    f.lget(end);
    f.call(strSliceBytes);
    f.lset(result);
    retStr(f, result);
  }
  {
    Func f(m, methodHelpers[kMethodStrTrim], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  start = f.addLocal(kI32), end = f.addLocal(kI32),
                  b = f.addLocal(kI32), result = f.addLocal(kI32);
    arity0(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i32Load(0);
    f.lset(end);
    f.i32c(0);
    f.lset(start);
    f.block();
    f.loop();
    f.lget(start);
    f.lget(end);
    f.i32GeU();
    f.brIf(1);
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(start);
    f.i32Add();
    f.i32Load8u(0);
    f.lset(b);
    isSpace(f, b);
    f.i32Eqz();
    f.brIf(1);
    f.lget(start);
    f.i32c(1);
    f.i32Add();
    f.lset(start);
    f.br(0);
    f.end();
    f.end();
    f.block();
    f.loop();
    f.lget(end);
    f.lget(start);
    f.i32LeS();
    f.brIf(1);
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(end);
    f.i32Add();
    f.i32c(1);
    f.i32Sub();
    f.i32Load8u(0);
    f.lset(b);
    isSpace(f, b);
    f.i32Eqz();
    f.brIf(1);
    f.lget(end);
    f.i32c(1);
    f.i32Sub();
    f.lset(end);
    f.br(0);
    f.end();
    f.end();
    f.lget(header);
    f.lget(start);
    f.lget(end);
    f.lget(start);
    f.i32Sub();
    f.call(strSliceBytes);
    f.lset(result);
    retStr(f, result);
  }
  {
    Func f(m, methodHelpers[kMethodStrContains], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  needle = f.addLocal(kI32), cond = f.addLocal(kI32);
    arity1(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    needStr(f, 0, needle);
    f.lget(header);
    f.lget(needle);
    f.i32c(0);
    f.call(strFind);
    f.i32c(0);
    f.i32GeS();
    f.lset(cond);
    retBool(f, cond);
  }
  {
    Func f(m, methodHelpers[kMethodStrStartsWith], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  needle = f.addLocal(kI32), cond = f.addLocal(kI32);
    arity1(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    needStr(f, 0, needle);
    f.lget(header);
    f.lget(needle);
    f.i32c(0);
    f.call(strFind);
    f.i32c(0);
    f.i32Eq();
    f.lset(cond);
    retBool(f, cond);
  }
  {
    Func f(m, methodHelpers[kMethodStrEndsWith], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  needle = f.addLocal(kI32), start = f.addLocal(kI32),
                  cond = f.addLocal(kI32);
    arity1(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    needStr(f, 0, needle);
    f.lget(needle);
    f.i32Load(0);
    f.lget(header);
    f.i32Load(0);
    f.i32GtU();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kBool));
    f.i64c(0);
    f.ret();
    f.end();
    f.lget(header);
    f.i32Load(0);
    f.lget(needle);
    f.i32Load(0);
    f.i32Sub();
    f.lset(start);
    f.lget(header);
    f.lget(needle);
    f.lget(start);
    f.call(strFind);
    f.lget(start);
    f.i32Eq();
    f.lset(cond);
    retBool(f, cond);
  }
  {
    Func f(m, methodHelpers[kMethodStrIndexOf], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  needle = f.addLocal(kI32), found = f.addLocal(kI32),
                  i = f.addLocal(kI32), count = f.addLocal(kI32),
                  b = f.addLocal(kI32);
    arity1(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    needStr(f, 0, needle);
    f.lget(header);
    f.lget(needle);
    f.i32c(0);
    f.call(strFind);
    f.lset(found);
    f.lget(found);
    f.i32c(0);
    f.i32LtS();
    f.if_();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64c(-1);
    f.ret();
    f.end();
    f.i32c(0);
    f.lset(i);
    f.i32c(0);
    f.lset(count);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(found);
    f.i32GeS();
    f.brIf(1);
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.i32Load8u(0);
    f.lset(b);
    f.lget(b);
    f.i32c(0xC0);
    f.i32And();
    f.i32c(0x80);
    f.i32Ne();
    f.if_();
    f.lget(count);
    f.i32c(1);
    f.i32Add();
    f.lset(count);
    f.end();
    f.lget(i);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    retInt(f, count);
  }
  {
    Func f(m, methodHelpers[kMethodStrSubstr], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  start = f.addLocal(kI32), count = f.addLocal(kI32),
                  len = f.addLocal(kI32), byteStart = f.addLocal(kI32),
                  byteEnd = f.addLocal(kI32), result = f.addLocal(kI32);
    arity1to2(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    needInt(f, 0, start);
    f.lget(header);
    f.call(utf8Len);
    f.i32WrapI64();
    f.lset(len);
    f.lget(start);
    f.i32c(0);
    f.i32LtS();
    f.if_();
    f.lget(start);
    f.lget(len);
    f.i32Add();
    f.lset(start);
    f.end();
    f.lget(start);
    f.i32c(0);
    f.i32LtS();
    f.if_();
    f.i32c(0);
    f.lset(start);
    f.end();
    f.lget(start);
    f.lget(len);
    f.i32GtU();
    f.if_();
    f.lget(len);
    f.lset(start);
    f.end();
    f.lget(len);
    f.lget(start);
    f.i32Sub();
    f.lset(count);
    f.lget(1);
    f.i32c(2);
    f.i32Eq();
    f.if_();
    needInt(f, 1, count);
    f.lget(count);
    f.i32c(0);
    f.i32LtS();
    f.if_();
    f.i32c(0);
    f.lset(count);
    f.end();
    f.end();
    f.lget(start);
    f.lget(count);
    f.i32Add();
    f.lget(len);
    f.i32GtU();
    f.if_();
    f.lget(len);
    f.lget(start);
    f.i32Sub();
    f.lset(count);
    f.end();
    f.lget(header);
    f.lget(start);
    f.i64ExtendI32S();
    f.call(utf8ByteOffset);
    f.lset(byteStart);
    f.lget(header);
    f.lget(start);
    f.lget(count);
    f.i32Add();
    f.i64ExtendI32S();
    f.call(utf8ByteOffset);
    f.lset(byteEnd);
    f.lget(header);
    f.lget(byteStart);
    f.lget(byteEnd);
    f.lget(byteStart);
    f.i32Sub();
    f.call(strSliceBytes);
    f.lset(result);
    retStr(f, result);
  }
  {
    Func f(m, methodHelpers[kMethodStrChars], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  result = f.addLocal(kI32), off = f.addLocal(kI32),
                  len = f.addLocal(kI32), clen = f.addLocal(kI32);
    arity0(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.i64c(0);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(result);
    f.drop();
    f.lget(header);
    f.i32Load(0);
    f.lset(len);
    f.i32c(0);
    f.lset(off);
    f.block();
    f.loop();
    f.lget(off);
    f.lget(len);
    f.i32GeU();
    f.brIf(1);
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(off);
    f.i32Add();
    f.i32Load8u(0);
    f.call(utf8SeqLen);
    f.lset(clen);
    f.lget(result);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(header);
    f.lget(off);
    f.lget(clen);
    f.call(strSliceBytes);
    f.i64ExtendI32U();
    f.call(arrayPush);
    f.lget(off);
    f.lget(clen);
    f.i32Add();
    f.lset(off);
    f.br(0);
    f.end();
    f.end();
    retArray(f, result);
  }
  {
    Func f(m, methodHelpers[kMethodStrReversed], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  result = f.addLocal(kI32);
    arity0(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.call(strReverse);
    f.lset(result);
    retStr(f, result);
  }
  {
    Func f(m, methodHelpers[kMethodStrRepeat], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  n = f.addLocal(kI32), slen = f.addLocal(kI32),
                  out = f.addLocal(kI32), i = f.addLocal(kI32);
    arity1(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    needInt(f, 0, n);
    f.lget(n);
    f.i32c(0);
    f.i32LtS();
    f.if_();
    emitFailStatic(f, sRepeatNegative);
    f.end();
    f.lget(header);
    f.i32Load(0);
    f.lset(slen);
    f.lget(slen);
    f.i32Eqz();
    f.lget(n);
    f.i32Eqz();
    f.i32Or();
    f.if_();
    f.i32c(0);
    f.call(newStr);
    f.lset(out);
    retStr(f, out);
    f.end();
    f.i32c(67108864);
    f.lget(slen);
    f.i32DivU();
    f.lget(n);
    f.i32LtU();
    f.if_();
    emitFailStatic(f, sRepeatLarge);
    f.end();
    f.lget(slen);
    f.lget(n);
    f.i32Mul();
    f.call(newStr);
    f.lset(out);
    f.i32c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(n);
    f.i32GeS();
    f.brIf(1);
    f.lget(out);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.lget(slen);
    f.i32Mul();
    f.i32Add();
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(slen);
    f.memoryCopy();
    f.lget(i);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    retStr(f, out);
  }
  {
    Func f(m, methodHelpers[kMethodStrSplit], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  result = f.addLocal(kI32), sep = f.addLocal(kI32),
                  i = f.addLocal(kI32), start = f.addLocal(kI32),
                  b = f.addLocal(kI32), found = f.addLocal(kI32),
                  off = f.addLocal(kI32), clen = f.addLocal(kI32),
                  len = f.addLocal(kI32);
    arity0to1(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.i64c(0);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(result);
    f.drop();
    f.lget(header);
    f.i32Load(0);
    f.lset(len);
    f.lget(1);
    f.i32c(0);
    f.i32Eq();
    f.if_();
    f.i32c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i32GeU();
    f.brIf(1);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i32GeU();
    f.brIf(1);
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.i32Load8u(0);
    f.lset(b);
    isSpace(f, b);
    f.i32Eqz();
    f.brIf(1);
    f.lget(i);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.lget(i);
    f.lget(len);
    f.i32GeU();
    f.brIf(1);
    f.lget(i);
    f.lset(start);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i32GeU();
    f.brIf(1);
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.i32Load8u(0);
    f.lset(b);
    isSpace(f, b);
    f.brIf(1);
    f.lget(i);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.lget(result);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(header);
    f.lget(start);
    f.lget(i);
    f.lget(start);
    f.i32Sub();
    f.call(strSliceBytes);
    f.i64ExtendI32U();
    f.call(arrayPush);
    f.br(0);
    f.end();
    f.end();
    f.else_();
    needStr(f, 0, sep);
    f.lget(sep);
    f.i32Load(0);
    f.i32Eqz();
    f.if_();
    f.i32c(0);
    f.lset(off);
    f.block();
    f.loop();
    f.lget(off);
    f.lget(len);
    f.i32GeU();
    f.brIf(1);
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(off);
    f.i32Add();
    f.i32Load8u(0);
    f.call(utf8SeqLen);
    f.lset(clen);
    f.lget(result);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(header);
    f.lget(off);
    f.lget(clen);
    f.call(strSliceBytes);
    f.i64ExtendI32U();
    f.call(arrayPush);
    f.lget(off);
    f.lget(clen);
    f.i32Add();
    f.lset(off);
    f.br(0);
    f.end();
    f.end();
    f.else_();
    f.i32c(0);
    f.lset(start);
    f.block();
    f.loop();
    f.lget(header);
    f.lget(sep);
    f.lget(start);
    f.call(strFind);
    f.lset(found);
    f.lget(found);
    f.i32c(0);
    f.i32LtS();
    f.if_();
    f.lget(result);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(header);
    f.lget(start);
    f.lget(len);
    f.lget(start);
    f.i32Sub();
    f.call(strSliceBytes);
    f.i64ExtendI32U();
    f.call(arrayPush);
    f.br(2);
    f.end();
    f.lget(result);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(header);
    f.lget(start);
    f.lget(found);
    f.lget(start);
    f.i32Sub();
    f.call(strSliceBytes);
    f.i64ExtendI32U();
    f.call(arrayPush);
    f.lget(found);
    f.lget(sep);
    f.i32Load(0);
    f.i32Add();
    f.lset(start);
    f.br(0);
    f.end();
    f.end();
    f.end();
    f.end();
    retArray(f, result);
  }
  {
    Func f(m, methodHelpers[kMethodStrLines], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  result = f.addLocal(kI32), len = f.addLocal(kI32),
                  start = f.addLocal(kI32), i = f.addLocal(kI32),
                  end = f.addLocal(kI32), b = f.addLocal(kI32);
    arity0(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.i64c(0);
    f.call(newArray);
    f.i32WrapI64();
    f.lset(result);
    f.drop();
    f.lget(header);
    f.i32Load(0);
    f.lset(len);
    f.i32c(0);
    f.lset(start);
    f.i32c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(len);
    f.i32GeU();
    f.brIf(1);
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(i);
    f.i32Add();
    f.i32Load8u(0);
    f.lset(b);
    f.lget(b);
    f.i32c(10);
    f.i32Eq();
    f.if_();
    f.lget(i);
    f.lset(end);
    f.lget(end);
    f.lget(start);
    f.i32GtS();
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(end);
    f.i32Add();
    f.i32c(1);
    f.i32Sub();
    f.i32Load8u(0);
    f.i32c(13);
    f.i32Eq();
    f.i32And();
    f.if_();
    f.lget(end);
    f.i32c(1);
    f.i32Sub();
    f.lset(end);
    f.end();
    f.lget(result);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(header);
    f.lget(start);
    f.lget(end);
    f.lget(start);
    f.i32Sub();
    f.call(strSliceBytes);
    f.i64ExtendI32U();
    f.call(arrayPush);
    f.lget(i);
    f.i32c(1);
    f.i32Add();
    f.lset(start);
    f.end();
    f.lget(i);
    f.i32c(1);
    f.i32Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.lget(start);
    f.lget(len);
    f.i32LtS();
    f.if_();
    f.lget(len);
    f.lset(end);
    f.lget(end);
    f.lget(start);
    f.i32GtS();
    f.lget(header);
    f.i32c(8);
    f.i32Add();
    f.lget(end);
    f.i32Add();
    f.i32c(1);
    f.i32Sub();
    f.i32Load8u(0);
    f.i32c(13);
    f.i32Eq();
    f.i32And();
    f.if_();
    f.lget(end);
    f.i32c(1);
    f.i32Sub();
    f.lset(end);
    f.end();
    f.lget(result);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(header);
    f.lget(start);
    f.lget(end);
    f.lget(start);
    f.i32Sub();
    f.call(strSliceBytes);
    f.i64ExtendI32U();
    f.call(arrayPush);
    f.end();
    retArray(f, result);
  }
  {
    Func f(m, methodHelpers[kMethodStrReplaceFirst], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  from = f.addLocal(kI32), to = f.addLocal(kI32),
                  found = f.addLocal(kI32), len = f.addLocal(kI32),
                  prefix = f.addLocal(kI32), suffix = f.addLocal(kI32),
                  tmp = f.addLocal(kI32), result = f.addLocal(kI32);
    arity2(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    needStr(f, 0, from);
    needStr(f, 1, to);
    f.lget(from);
    f.i32Load(0);
    f.i32Eqz();
    f.if_();
    emitFailStatic(f, sReplaceFirstEmpty);
    f.end();
    f.lget(header);
    f.i32Load(0);
    f.lset(len);
    f.lget(header);
    f.lget(from);
    f.i32c(0);
    f.call(strFind);
    f.lset(found);
    f.lget(found);
    f.i32c(0);
    f.i32LtS();
    f.if_();
    f.lget(header);
    f.i32c(0);
    f.lget(len);
    f.call(strSliceBytes);
    f.lset(result);
    retStr(f, result);
    f.end();
    f.lget(header);
    f.i32c(0);
    f.lget(found);
    f.call(strSliceBytes);
    f.lset(prefix);
    f.lget(header);
    f.lget(found);
    f.lget(from);
    f.i32Load(0);
    f.i32Add();
    f.lget(len);
    f.lget(found);
    f.i32Sub();
    f.lget(from);
    f.i32Load(0);
    f.i32Sub();
    f.call(strSliceBytes);
    f.lset(suffix);
    f.lget(prefix);
    f.lget(to);
    f.call(concat);
    f.lset(tmp);
    f.lget(tmp);
    f.lget(suffix);
    f.call(concat);
    f.lset(result);
    retStr(f, result);
  }
  {
    Func f(m, methodHelpers[kMethodStrReplace], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  from = f.addLocal(kI32), to = f.addLocal(kI32),
                  found = f.addLocal(kI32), len = f.addLocal(kI32),
                  pos = f.addLocal(kI32), result = f.addLocal(kI32);
    arity2(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    needStr(f, 0, from);
    needStr(f, 1, to);
    f.lget(from);
    f.i32Load(0);
    f.i32Eqz();
    f.if_();
    emitFailStatic(f, sReplaceEmpty);
    f.end();
    f.lget(header);
    f.i32Load(0);
    f.lset(len);
    f.i32c(0);
    f.call(newStr);
    f.lset(result);
    f.i32c(0);
    f.lset(pos);
    f.block();
    f.loop();
    f.lget(header);
    f.lget(from);
    f.lget(pos);
    f.call(strFind);
    f.lset(found);
    f.lget(found);
    f.i32c(0);
    f.i32LtS();
    f.if_();
    f.lget(result);
    f.lget(header);
    f.lget(pos);
    f.lget(len);
    f.lget(pos);
    f.i32Sub();
    f.call(strSliceBytes);
    f.call(concat);
    f.lset(result);
    f.br(2);
    f.end();
    f.lget(result);
    f.lget(header);
    f.lget(pos);
    f.lget(found);
    f.lget(pos);
    f.i32Sub();
    f.call(strSliceBytes);
    f.call(concat);
    f.lset(result);
    f.lget(result);
    f.lget(to);
    f.call(concat);
    f.lset(result);
    f.lget(found);
    f.lget(from);
    f.i32Load(0);
    f.i32Add();
    f.lset(pos);
    f.br(0);
    f.end();
    f.end();
    retStr(f, result);
  }

  auto emitPad = [&](std::uint32_t helper, bool atStart, std::uint32_t fillMsg,
                     std::uint32_t largeMsg) {
    Func f(m, methodHelpers[helper], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  width = f.addLocal(kI32), fill = f.addLocal(kI32),
                  current = f.addLocal(kI32), keep = f.addLocal(kI32),
                  fillChars = f.addLocal(kI32), padding = f.addLocal(kI32),
                  dst = f.addLocal(kI32), remaining = f.addLocal(kI32),
                  off = f.addLocal(kI32), copied = f.addLocal(kI32),
                  clen = f.addLocal(kI32), result = f.addLocal(kI32);
    arity1to2(f);
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    needInt(f, 0, width);
    f.lget(1);
    f.i32c(2);
    f.i32Eq();
    f.if_();
    needStr(f, 1, fill);
    f.else_();
    f.i32c(static_cast<std::int32_t>(sSpace));
    f.lset(fill);
    f.end();
    f.lget(fill);
    f.i32Load(0);
    f.i32Eqz();
    f.if_();
    emitFailStatic(f, fillMsg);
    f.end();
    f.lget(header);
    f.call(utf8Len);
    f.i32WrapI64();
    f.lset(current);
    f.lget(current);
    f.lget(width);
    f.i32GeS();
    f.if_();
    f.lget(header);
    f.i32c(0);
    f.lget(header);
    f.i32Load(0);
    f.call(strSliceBytes);
    f.lset(result);
    retStr(f, result);
    f.end();
    f.lget(width);
    f.lget(current);
    f.i32Sub();
    f.lset(keep);
    f.lget(keep);
    f.i32c(67108864);
    f.i32GtU();
    f.if_();
    emitFailStatic(f, largeMsg);
    f.end();
    f.lget(fill);
    f.call(utf8Len);
    f.i32WrapI64();
    f.lset(fillChars);
    f.lget(keep);
    f.i32c(4);
    f.i32Mul();
    f.call(newStr);
    f.lset(padding);
    f.i32c(0);
    f.lset(dst);
    f.lget(keep);
    f.lset(remaining);
    f.block();
    f.loop();
    f.lget(remaining);
    f.lget(fillChars);
    f.i32LtS();
    f.brIf(1);
    f.lget(padding);
    f.i32c(8);
    f.i32Add();
    f.lget(dst);
    f.i32Add();
    f.lget(fill);
    f.i32c(8);
    f.i32Add();
    f.lget(fill);
    f.i32Load(0);
    f.memoryCopy();
    f.lget(dst);
    f.lget(fill);
    f.i32Load(0);
    f.i32Add();
    f.lset(dst);
    f.lget(remaining);
    f.lget(fillChars);
    f.i32Sub();
    f.lset(remaining);
    f.br(0);
    f.end();
    f.end();
    f.lget(remaining);
    f.i32c(0);
    f.i32GtS();
    f.if_();
    f.i32c(0);
    f.lset(off);
    f.i32c(0);
    f.lset(copied);
    f.block();
    f.loop();
    f.lget(copied);
    f.lget(remaining);
    f.i32GeS();
    f.brIf(1);
    f.lget(fill);
    f.i32c(8);
    f.i32Add();
    f.lget(off);
    f.i32Add();
    f.i32Load8u(0);
    f.call(utf8SeqLen);
    f.lset(clen);
    f.lget(padding);
    f.i32c(8);
    f.i32Add();
    f.lget(dst);
    f.i32Add();
    f.lget(fill);
    f.i32c(8);
    f.i32Add();
    f.lget(off);
    f.i32Add();
    f.lget(clen);
    f.memoryCopy();
    f.lget(off);
    f.lget(clen);
    f.i32Add();
    f.lset(off);
    f.lget(dst);
    f.lget(clen);
    f.i32Add();
    f.lset(dst);
    f.lget(copied);
    f.i32c(1);
    f.i32Add();
    f.lset(copied);
    f.br(0);
    f.end();
    f.end();
    f.end();
    f.lget(padding);
    f.lget(dst);
    f.i64ExtendI32U();
    f.i64Store(0);
    if (atStart) {
      f.lget(padding);
      f.lget(header);
      f.call(concat);
    } else {
      f.lget(header);
      f.lget(padding);
      f.call(concat);
    }
    f.lset(result);
    retStr(f, result);
  };
  emitPad(kMethodStrPadStart, true, sPadStartFill, sPadStartLarge);
  emitPad(kMethodStrPadEnd, false, sPadEndFill, sPadEndLarge);
}

void Runtime::emitRangeMethods() {
  {
    Func f(m, methodHelpers[kMethodRangeToArray], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64),
                  start = f.addLocal(kI64), end = f.addLocal(kI64),
                  incl = f.addLocal(kI32), count = f.addLocal(kI64),
                  i = f.addLocal(kI64), step = f.addLocal(kI64),
                  arrTag = f.addLocal(kI64), arrPayload = f.addLocal(kI64);
    f.lget(1);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sToArrayArgc);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(header);
    f.i64Load(0);
    f.lset(start);
    f.lget(header);
    f.i64Load(8);
    f.lset(end);
    f.lget(header);
    f.i64Load(16);
    f.i32WrapI64();
    f.lset(incl);
    emitRangeCount(f, start, end, incl, count);
    f.lget(count);
    f.i64c(10000000);
    f.i64GtS();
    f.if_();
    emitFailStatic(f, sRangeTooLarge);
    f.end();
    f.lget(count);
    f.call(newArray);
    f.lset(arrPayload);
    f.lset(arrTag);
    f.lget(start);
    f.lget(end);
    f.i64LeS();
    f.ifResult(kI64);
    f.i64c(1);
    f.else_();
    f.i64c(-1);
    f.end();
    f.lset(step);
    f.i64c(0);
    f.lset(i);
    f.block();
    f.loop();
    f.lget(i);
    f.lget(count);
    f.i64GeS();
    f.brIf(1);
    f.lget(arrPayload);
    f.i32WrapI64();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(start);
    f.lget(i);
    f.lget(step);
    f.i64Mul();
    f.i64Add();
    f.call(arrayPush);
    f.lget(i);
    f.i64c(1);
    f.i64Add();
    f.lset(i);
    f.br(0);
    f.end();
    f.end();
    f.lget(arrTag);
    f.lget(arrPayload);
    f.ret();
  }
  {
    Func f(m, methodHelpers[kMethodRangeContains], {kI32, kI32, kI32});
    std::uint32_t cell = f.addLocal(kI32), header = f.addLocal(kI32),
                  recvTag = f.addLocal(kI64), recvPayload = f.addLocal(kI64);
    f.lget(1);
    f.i32c(1);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, sContainsArgc);
    f.end();
    emitReadReceiver(f, cell, header, recvTag, recvPayload);
    f.lget(0);
    f.i64Load(0);
    f.lget(0);
    f.i64Load(8);
    f.i64c(static_cast<std::int64_t>(kRange));
    f.lget(header);
    f.i64ExtendI32U();
    f.call(opIn);
    f.ret();
  }
}

void Runtime::emitMethodDispatch() {
  Func f(m, methodDispatch, {kI32, kI32, kI32});
  std::uint32_t selector = f.addLocal(kI32);
  f.lget(2);
  f.i64Load(8);
  f.i32WrapI64();
  f.lset(selector);
  for (std::size_t i = 0; i < methodHelpers.size(); ++i) {
    f.lget(selector);
    f.i32c(static_cast<std::int32_t>(i));
    f.i32Eq();
    f.if_();
    f.lget(0);
    f.lget(1);
    f.lget(2);
    f.call(methodHelpers[i]);
    f.ret();
    f.end();
  }
  emitFailStatic(f, sNoMember);
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
  f.i32c(static_cast<std::int32_t>(kMapHeaderSize));
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
  f.lget(header);
  f.i64c(0);
  f.i64Store(kMapClassNameOffset);
  f.lget(header);
  f.i64c(0);
  f.i64Store(kMapMethodsOffset);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.lget(header);
  f.i64ExtendI32U();
  return index;
}

std::uint32_t Runtime::emitNewPtr() {
  std::uint32_t type = m.typeFor({kI64, kI64, kI64}, {kI64, kI64});
  std::uint32_t index = m.reserveFunction(type);
  Func f(m, index, {kI64, kI64, kI64});
  std::uint32_t header = f.addLocal(kI32);
  f.i32c(static_cast<std::int32_t>(kPtrHeaderSize));
  f.call(alloc);
  f.lset(header);
  f.lget(header);
  f.lget(0);
  f.i64Store(kPtrKindOffset);
  f.lget(header);
  f.lget(1);
  f.i64Store(kPtrDataOffset);
  f.lget(header);
  f.lget(2);
  f.i64Store(kPtrLengthOffset);
  f.lget(header);
  f.i64c(0);
  f.i64Store(kPtrReleasedOffset);
  f.i64c(static_cast<std::int64_t>(kPtr));
  f.lget(header);
  f.i64ExtendI32U();
  return index;
}

// The text a `cstring` owns, up to its first NUL. A `buffer` yields every byte
// and a `void *` owns none, matching the hosted `PtrObj`.
std::uint32_t Runtime::emitPtrTextLen() {
  std::uint32_t index = m.reserveFunction(m.typeFor({kI32}, {kI64}));
  Func f(m, index, {kI32});
  std::uint32_t kind = f.addLocal(kI64), data = f.addLocal(kI32),
                length = f.addLocal(kI64), i = f.addLocal(kI64);
  f.lget(0);
  f.i64Load(kPtrKindOffset);
  f.lset(kind);
  f.lget(kind);
  f.i64c(static_cast<std::int64_t>(kPtrKindBuffer));
  f.i64Eq();
  f.if_();
  f.lget(0);
  f.i64Load(kPtrLengthOffset);
  f.ret();
  f.end();
  f.lget(kind);
  f.i64c(static_cast<std::int64_t>(kPtrKindCString));
  f.i64Ne();
  f.if_();
  f.i64c(0);
  f.ret();
  f.end();
  f.lget(0);
  f.i64Load(kPtrDataOffset);
  f.i32WrapI64();
  f.lset(data);
  f.lget(0);
  f.i64Load(kPtrLengthOffset);
  f.lset(length);
  f.i64c(0);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(length);
  f.i64GeU();
  f.brIf(1);
  f.lget(data);
  f.lget(i);
  f.i32WrapI64();
  f.i32Add();
  f.i32Load8u(0);
  f.i32Eqz();
  f.if_();
  f.br(2);
  f.end();
  f.lget(i);
  f.i64c(1);
  f.i64Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();
  f.lget(i);
  return index;
}

// The rendered form of a handle: a `cstring` as its text, anything else as
// `<ptr>` (the VM never leaks the opaque token).
std::uint32_t Runtime::emitPtrText() {
  std::uint32_t index = m.reserveFunction(m.typeFor({kI32}, {kI32}));
  Func f(m, index, {kI32});
  std::uint32_t length = f.addLocal(kI64), out = f.addLocal(kI32);
  f.lget(0);
  f.i64Load(kPtrKindOffset);
  f.i64c(static_cast<std::int64_t>(kPtrKindCString));
  f.i64Ne();
  f.if_();
  f.i32c(static_cast<std::int32_t>(sPtrDisplay));
  f.ret();
  f.end();
  f.lget(0);
  f.call(ptrTextLen);
  f.lset(length);
  f.lget(length);
  f.i32WrapI64();
  f.call(newStr);
  f.lset(out);
  f.lget(out);
  f.i32c(8);
  f.i32Add();
  f.lget(0);
  f.i64Load(kPtrDataOffset);
  f.i32WrapI64();
  f.lget(length);
  f.i32WrapI64();
  f.memoryCopy();
  f.lget(out);
  return index;
}

std::uint32_t Runtime::emitPtrTypeName() {
  std::uint32_t index = m.reserveFunction(m.typeFor({kI32}, {kI32}));
  Func f(m, index, {kI32});
  f.lget(0);
  f.i64Load(kPtrKindOffset);
  f.i64c(static_cast<std::int64_t>(kPtrKindCString));
  f.i64Eq();
  f.if_();
  f.i32c(static_cast<std::int32_t>(sTypeCString));
  f.ret();
  f.end();
  f.i32c(static_cast<std::int32_t>(sTypePtr));
  f.ret();
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
                classA = f.addLocal(kI32), classB = f.addLocal(kI32),
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

  // A class instance compares by identity, never structurally. The class-name
  // islands are shared per class, so a pointer comparison is enough.
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kMap));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addrA);
  f.lget(3);
  f.i32WrapI64();
  f.lset(addrB);
  f.lget(addrA);
  f.i64Load(kMapClassNameOffset);
  f.i32WrapI64();
  f.lset(classA);
  f.lget(addrB);
  f.i64Load(kMapClassNameOffset);
  f.i32WrapI64();
  f.lset(classB);
  f.lget(classA);
  f.i32c(0);
  f.i32Ne();
  f.lget(classB);
  f.i32c(0);
  f.i32Ne();
  f.i32Or();
  f.if_();
  f.lget(classA);
  f.lget(classB);
  f.i32Ne();
  f.if_();
  f.i32c(0);
  f.ret();
  f.end();
  f.lget(addrA);
  f.lget(addrB);
  f.i32Eq();
  f.ret();
  f.end();
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
  f.lget(0);
  f.lget(1);
  f.call(fmtDouble);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kDouble));
  f.i64Eq();
  f.if_();
  f.lget(0);
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
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kPtr));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.call(ptrText);
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
                itemPayload = f.addLocal(kI64), entry = f.addLocal(kI32),
                className = f.addLocal(kI32);
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
  // A class instance prints its class name before the field map.
  f.lget(addr);
  f.i64Load(kMapClassNameOffset);
  f.i32WrapI64();
  f.lset(className);
  f.lget(className);
  f.i32c(0);
  f.i32Ne();
  f.if_();
  f.lget(className);
  f.i32c(static_cast<std::int32_t>(sSpace));
  f.call(concat);
  f.i32c(static_cast<std::int32_t>(sOpenBrace));
  f.call(concat);
  f.i32c(static_cast<std::int32_t>(sSpace));
  f.call(concat);
  f.lset(s);
  f.else_();
  f.i32c(static_cast<std::int32_t>(sOpenBrace));
  f.lset(s);
  f.end();
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
  f.lget(className);
  f.i32c(0);
  f.i32Ne();
  f.if_();
  f.lget(s);
  f.i32c(static_cast<std::int32_t>(sSpaceCloseBrace));
  f.call(concat);
  f.lset(s);
  f.else_();
  f.lget(s);
  f.i32c(static_cast<std::int32_t>(sCloseBrace));
  f.call(concat);
  f.lset(s);
  f.end();
  f.lget(s);
  f.ret();
  f.end();
  f.lget(0);
  f.i64c(static_cast<std::int64_t>(kPtr));
  f.i64Eq();
  f.if_();
  f.lget(1);
  f.i32WrapI64();
  f.lset(addr);
  f.lget(addr);
  f.i64Load(kPtrKindOffset);
  f.i64c(static_cast<std::int64_t>(kPtrKindCString));
  f.i64Eq();
  f.if_();
  f.lget(addr);
  f.call(ptrText);
  f.call(quote);
  f.ret();
  f.end();
  f.i32c(static_cast<std::int32_t>(sPtrDisplay));
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
  (void)addr;  // declared to reserve the local slot, never read

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

// `(p, end, literal, length) -> i32`: case-insensitive comparison of the
// byte range `[p, end)` with a raw literal (the address points at the bytes,
// not at the AotString header). Used for the `inf`/`infinity`/`nan` spellings.
std::uint32_t Runtime::emitWordMatch() {
  std::uint32_t index = m.reserveFunction(m.typeFor({kI32, kI32, kI32, kI32}, {kI32}));
  Func f(m, index, {kI32, kI32, kI32, kI32});
  std::uint32_t i = f.addLocal(kI32), a = f.addLocal(kI32), b = f.addLocal(kI32);

  f.lget(1);
  f.lget(0);
  f.i32Sub();
  f.lget(3);
  f.i32Ne();
  f.if_();
  f.i32c(0);
  f.ret();
  f.end();

  f.i32c(0);
  f.lset(i);
  f.block();
  f.loop();
  f.lget(i);
  f.lget(3);
  f.i32GeS();
  f.brIf(1);

  f.lget(0);
  f.lget(i);
  f.i32Add();
  f.i32Load8u(0);
  f.lset(a);
  f.lget(a);
  f.i32c(65);
  f.i32GeS();
  f.lget(a);
  f.i32c(90);
  f.i32LeS();
  f.i32And();
  f.if_();
  f.lget(a);
  f.i32c(32);
  f.i32Add();
  f.lset(a);
  f.end();

  f.lget(2);
  f.lget(i);
  f.i32Add();
  f.i32Load8u(0);
  f.lset(b);
  f.lget(b);
  f.i32c(65);
  f.i32GeS();
  f.lget(b);
  f.i32c(90);
  f.i32LeS();
  f.i32And();
  f.if_();
  f.lget(b);
  f.i32c(32);
  f.i32Add();
  f.lset(b);
  f.end();

  f.lget(a);
  f.lget(b);
  f.i32Ne();
  f.if_();
  f.i32c(0);
  f.ret();
  f.end();

  f.lget(i);
  f.i32c(1);
  f.i32Add();
  f.lset(i);
  f.br(0);
  f.end();
  f.end();

  f.i32c(1);
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

  // The VM accepts the non-finite spellings `inf`, `infinity` and `nan`
  // (case-insensitive, optional sign) and rejects a silent overflow such as
  // `1e400`. The sign was parsed above, so `-inf` reaches here as `inf` with
  // `sign == -1`.
  f.lget(p);
  f.lget(end);
  f.i32c(static_cast<std::int32_t>(sInf + 8));
  f.i32c(3);
  f.call(wordMatch);
  f.if_();
  f.f64c(std::numeric_limits<double>::infinity());
  f.lget(sign);
  f.f64Mul();
  f.ret();
  f.end();
  f.lget(p);
  f.lget(end);
  f.i32c(static_cast<std::int32_t>(sInfinity + 8));
  f.i32c(8);
  f.call(wordMatch);
  f.if_();
  f.f64c(std::numeric_limits<double>::infinity());
  f.lget(sign);
  f.f64Mul();
  f.ret();
  f.end();
  f.lget(p);
  f.lget(end);
  f.i32c(static_cast<std::int32_t>(sNan + 8));
  f.i32c(3);
  f.call(wordMatch);
  f.if_();
  f.f64c(std::numeric_limits<double>::quiet_NaN());
  f.ret();
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

  // Only the explicit `inf`/`-inf` spellings may produce an infinity; a
  // numeric literal that overflows (`1e400`) is rejected like the VM.
  f.lget(value);
  f.f64c(std::numeric_limits<double>::infinity());
  f.f64Eq();
  f.lget(value);
  f.f64c(-std::numeric_limits<double>::infinity());
  f.f64Eq();
  f.i32Or();
  f.if_();
  emitFailStatic(f, sFloatString);
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
  writeStr = emitWriteStr();
  fail = emitFail();
  alloc = emitAlloc();
  utf8Len = emitUtf8Len();
  newStr = emitNewStr();
  concat = emitConcat();
  strCmp = emitStrCmp();
  fmtInt = emitFmtInt();
  parseIntStr = emitParseIntStr();
  wordMatch = emitWordMatch();
  parseDoubleStr = emitParseDoubleStr();
  emitDtoaHelpers();
  fmtDouble = emitFmtDouble();
  fmtFunc = emitFmtFunc();
  fmtRange = emitFmtRange();
  quote = emitQuote();
  newPtr = emitNewPtr();
  ptrTextLen = emitPtrTextLen();
  ptrText = emitPtrText();
  ptrTypeName = emitPtrTypeName();
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
  utf8SeqLen = emitUtf8SeqLen();
  decodeCodePoint = emitDecodeCodePoint();
  encodeUtf8 = emitEncodeUtf8();
  caseLookup = emitCaseLookup();
  caseMap = emitCaseMap();
  strFind = emitStrFind();
  strSliceBytes = emitStrSliceBytes();
  strReverse = emitStrReverse();
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
  auto arityMsg = [&](int n, std::uint32_t message) {
    f.lget(1);
    f.i32c(n);
    f.i32Ne();
    f.if_();
    emitFailStatic(f, message);
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
    std::uint32_t typeResult = f.addLocal(kI32), className = f.addLocal(kI32);
    f.i64c(static_cast<std::int64_t>(kStr));
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kPtr));
    f.i64Eq();
    f.if_();
    argPayload(0);
    f.i32WrapI64();
    f.call(ptrTypeName);
    f.lset(typeResult);
    f.else_();
    argTag(0);
    f.call(typeName);
    f.lset(typeResult);
    // A class instance reports its class name instead of "map".
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kMap));
    f.i64Eq();
    f.if_();
    argPayload(0);
    f.i32WrapI64();
    f.i64Load(kMapClassNameOffset);
    f.i32WrapI64();
    f.lset(className);
    f.lget(className);
    f.i32c(0);
    f.i32Ne();
    f.if_();
    f.lget(className);
    f.lset(typeResult);
    f.end();
    f.end();
    f.end();
    f.lget(typeResult);
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

  if (name == "$setClass") {
    arity(3);
    std::uint32_t instance = f.addLocal(kI32), className = f.addLocal(kI32),
                  methods = f.addLocal(kI32);
    argPayload(0);
    f.i32WrapI64();
    f.lset(instance);
    argPayload(1);
    f.i32WrapI64();
    f.lset(className);
    argPayload(2);
    f.i32WrapI64();
    f.lset(methods);
    f.lget(instance);
    f.lget(className);
    f.i64ExtendI32U();
    f.i64Store(kMapClassNameOffset);
    f.lget(instance);
    f.lget(methods);
    f.i64ExtendI32U();
    f.i64Store(kMapMethodsOffset);
    f.i64c(static_cast<std::int64_t>(kNil));
    f.i64c(0);
    f.ret();
    return index;
  }

  if (name == "$range" || name == "range") {
    const bool inclusive = name == "$range";
    if (inclusive) {
      arity(3);
    } else {
      // The public form takes 1 or 2 bounds: `range(5)` starts at 0.
      arityBetween(1, 2);
    }
    std::uint32_t addr = f.addLocal(kI32);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.i32Eqz();
    f.if_();
    emitFailStatic(f, sRangeBounds);
    f.end();
    if (inclusive) {
      argTag(1);
      f.i64c(static_cast<std::int64_t>(kInt));
      f.i64Eq();
      f.i32Eqz();
      f.if_();
      emitFailStatic(f, sRangeBounds);
      f.end();
    } else {
      f.lget(1);
      f.i32c(2);
      f.i32Eq();
      f.if_();
      argTag(1);
      f.i64c(static_cast<std::int64_t>(kInt));
      f.i64Eq();
      f.i32Eqz();
      f.if_();
      emitFailStatic(f, sRangeBounds);
      f.end();
      f.end();
    }
    f.i32c(24);
    f.call(alloc);
    f.lset(addr);
    f.lget(addr);
    if (inclusive) {
      argPayload(0);
    } else {
      f.lget(1);
      f.i32c(1);
      f.i32Eq();
      f.ifResult(kI64);
      f.i64c(0);
      f.else_();
      argPayload(0);
      f.end();
    }
    f.i64Store(0);
    f.lget(addr);
    if (inclusive) {
      argPayload(1);
    } else {
      f.lget(1);
      f.i32c(1);
      f.i32Eq();
      f.ifResult(kI64);
      argPayload(0);
      f.else_();
      argPayload(1);
      f.end();
    }
    f.i64Store(8);
    f.lget(addr);
    if (inclusive) {
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

  if (name == "platform") {
    arity(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64c(static_cast<std::int64_t>(pool.addString("freestanding")));
    return index;
  }

  if (name == "arch") {
    arity(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64c(static_cast<std::int64_t>(pool.addString("wasm32")));
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

  if (name == "cstring") {
    arityMsg(1, sCStringArgc);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kStr));
    f.i64Eq();
    f.if_();
    f.else_();
    emitFailStatic(f, sCStringType);
    f.end();
    std::uint32_t text = f.addLocal(kI32), length = f.addLocal(kI64),
                  data = f.addLocal(kI32);
    argPayload(0);
    f.i32WrapI64();
    f.lset(text);
    f.lget(text);
    f.i64Load(0);
    f.lset(length);
    f.lget(length);
    f.i32WrapI64();
    f.i32c(1);
    f.i32Add();
    f.call(alloc);
    f.lset(data);
    f.lget(data);
    f.lget(text);
    f.i32c(8);
    f.i32Add();
    f.lget(length);
    f.i32WrapI64();
    f.memoryCopy();
    f.lget(data);
    f.lget(length);
    f.i32WrapI64();
    f.i32Add();
    f.i32c(0);
    f.i32Store8(0);
    f.i64c(static_cast<std::int64_t>(kPtrKindCString));
    f.lget(data);
    f.i64ExtendI32U();
    f.lget(length);
    f.i64c(1);
    f.i64Add();
    f.call(newPtr);
    return index;
  }

  if (name == "buffer") {
    arityMsg(1, sBufferArgc);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.if_();
    f.else_();
    emitFailStatic(f, sBufferType);
    f.end();
    std::uint32_t size = f.addLocal(kI64), data = f.addLocal(kI32);
    argPayload(0);
    f.lset(size);
    f.lget(size);
    f.i64c(0);
    f.i64LtS();
    f.if_();
    emitFailStatic(f, sBufferNegative);
    f.end();
    // WASM addresses are 32-bit: a larger request would truncate the
    // allocation while the header kept the original length.
    f.lget(size);
    f.i64c(0xFFFFFFFF);
    f.i64GtU();
    f.if_();
    emitFailStatic(f, sBufferTooLarge);
    f.end();
    f.lget(size);
    f.i32WrapI64();
    f.call(alloc);
    f.lset(data);
    f.lget(data);
    f.i32c(0);
    f.lget(size);
    f.i32WrapI64();
    f.memoryFill();
    f.i64c(static_cast<std::int64_t>(kPtrKindBuffer));
    f.lget(data);
    f.i64ExtendI32U();
    f.lget(size);
    f.call(newPtr);
    return index;
  }

  if (name == "cstr") {
    arityMsg(1, sCstrArgc);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kPtr));
    f.i64Eq();
    f.if_();
    f.else_();
    emitFailStatic(f, sCstrType);
    f.end();
    std::uint32_t header = f.addLocal(kI32), length = f.addLocal(kI64),
                  out = f.addLocal(kI32);
    argPayload(0);
    f.i32WrapI64();
    f.lset(header);
    f.lget(header);
    f.i64Load(kPtrReleasedOffset);
    f.i64c(0);
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sCstrFreed);
    f.end();
    f.lget(header);
    f.call(ptrTextLen);
    f.lset(length);
    f.lget(length);
    f.i32WrapI64();
    f.call(newStr);
    f.lset(out);
    // A `void *` owns no bytes, so its token may point past linear memory.
    // `memory.copy` validates the source even for a zero length, so skip it.
    f.lget(length);
    f.i64c(0);
    f.i64GtS();
    f.if_();
    f.lget(out);
    f.i32c(8);
    f.i32Add();
    f.lget(header);
    f.i64Load(kPtrDataOffset);
    f.i32WrapI64();
    f.lget(length);
    f.i32WrapI64();
    f.memoryCopy();
    f.end();
    f.i64c(static_cast<std::int64_t>(kStr));
    f.lget(out);
    f.i64ExtendI32U();
    return index;
  }

  if (name == "byteLen") {
    arityMsg(1, sByteLenArgc);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kPtr));
    f.i64Eq();
    f.if_();
    f.else_();
    emitFailStatic(f, sByteLenType);
    f.end();
    std::uint32_t header = f.addLocal(kI32), kind = f.addLocal(kI64),
                  length = f.addLocal(kI64);
    argPayload(0);
    f.i32WrapI64();
    f.lset(header);
    f.lget(header);
    f.i64Load(kPtrReleasedOffset);
    f.i64c(0);
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sByteLenFreed);
    f.end();
    f.lget(header);
    f.i64Load(kPtrKindOffset);
    f.lset(kind);
    f.lget(header);
    f.i64Load(kPtrLengthOffset);
    f.lset(length);
    f.lget(kind);
    f.i64c(static_cast<std::int64_t>(kPtrKindVoid));
    f.i64Eq();
    f.if_();
    f.i64c(0);
    f.lset(length);
    f.else_();
    f.lget(kind);
    f.i64c(static_cast<std::int64_t>(kPtrKindCString));
    f.i64Eq();
    f.if_();
    f.lget(length);
    f.i64c(0);
    f.i64GtS();
    f.if_();
    f.lget(header);
    f.i64Load(kPtrDataOffset);
    f.i32WrapI64();
    f.lget(length);
    f.i64c(1);
    f.i64Sub();
    f.i32WrapI64();
    f.i32Add();
    f.i32Load8u(0);
    f.i32Eqz();
    f.if_();
    f.lget(length);
    f.i64c(1);
    f.i64Sub();
    f.lset(length);
    f.end();
    f.end();
    f.end();
    f.end();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(length);
    return index;
  }

  if (name == "loadByte") {
    arityMsg(2, sLoadArgc);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kPtr));
    f.i64Eq();
    f.if_();
    f.else_();
    emitFailStatic(f, sLoadType);
    f.end();
    std::uint32_t header = f.addLocal(kI32), kind = f.addLocal(kI64),
                  at = f.addLocal(kI64), length = f.addLocal(kI64),
                  msg = f.addLocal(kI32);
    argPayload(0);
    f.i32WrapI64();
    f.lset(header);
    f.lget(header);
    f.i64Load(kPtrReleasedOffset);
    f.i64c(0);
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sLoadFreed);
    f.end();
    argTag(1);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.if_();
    f.else_();
    emitFailStatic(f, sLoadIndexType);
    f.end();
    argPayload(1);
    f.lset(at);
    f.lget(header);
    f.i64Load(kPtrKindOffset);
    f.lset(kind);
    f.lget(kind);
    f.i64c(static_cast<std::int64_t>(kPtrKindVoid));
    f.i64Eq();
    f.if_();
    emitFailStatic(f, sLoadNeeds);
    f.end();
    f.lget(header);
    f.i64Load(kPtrLengthOffset);
    f.lset(length);
    f.lget(at);
    f.i64c(0);
    f.i64LtS();
    f.lget(at);
    f.lget(length);
    f.i64GeS();
    f.i32Or();
    f.if_();
    f.i32c(static_cast<std::int32_t>(sLoadRangePrefix));
    f.lget(at);
    f.call(fmtInt);
    f.call(concat);
    f.lset(msg);
    f.lget(msg);
    f.i32c(static_cast<std::int32_t>(sLoadRangeSuffix));
    f.call(concat);
    f.lset(msg);
    f.lget(msg);
    f.i32c(8);
    f.i32Add();
    f.lget(msg);
    f.i64Load(0);
    f.i32WrapI64();
    f.call(fail);
    f.unreachable();
    f.end();
    f.i64c(static_cast<std::int64_t>(kInt));
    f.lget(header);
    f.i64Load(kPtrDataOffset);
    f.i32WrapI64();
    f.lget(at);
    f.i32WrapI64();
    f.i32Add();
    f.i32Load8u(0);
    f.i64ExtendI32U();
    return index;
  }

  if (name == "storeByte") {
    arityMsg(3, sStoreArgc);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kPtr));
    f.i64Eq();
    f.if_();
    f.else_();
    emitFailStatic(f, sStoreType);
    f.end();
    std::uint32_t header = f.addLocal(kI32), kind = f.addLocal(kI64),
                  at = f.addLocal(kI64), length = f.addLocal(kI64),
                  byte = f.addLocal(kI64), msg = f.addLocal(kI32);
    argPayload(0);
    f.i32WrapI64();
    f.lset(header);
    f.lget(header);
    f.i64Load(kPtrReleasedOffset);
    f.i64c(0);
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sStoreFreed);
    f.end();
    argTag(1);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.if_();
    f.else_();
    emitFailStatic(f, sStoreIndexType);
    f.end();
    argTag(2);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.if_();
    f.else_();
    emitFailStatic(f, sStoreByteType);
    f.end();
    argPayload(1);
    f.lset(at);
    argPayload(2);
    f.lset(byte);
    f.lget(header);
    f.i64Load(kPtrKindOffset);
    f.lset(kind);
    f.lget(kind);
    f.i64c(static_cast<std::int64_t>(kPtrKindBuffer));
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sStoreNeeds);
    f.end();
    f.lget(header);
    f.i64Load(kPtrLengthOffset);
    f.lset(length);
    f.lget(at);
    f.i64c(0);
    f.i64LtS();
    f.lget(at);
    f.lget(length);
    f.i64GeS();
    f.i32Or();
    f.if_();
    f.i32c(static_cast<std::int32_t>(sStoreRangePrefix));
    f.lget(at);
    f.call(fmtInt);
    f.call(concat);
    f.lset(msg);
    f.lget(msg);
    f.i32c(static_cast<std::int32_t>(sStoreRangeSuffix));
    f.call(concat);
    f.lset(msg);
    f.lget(msg);
    f.i32c(8);
    f.i32Add();
    f.lget(msg);
    f.i64Load(0);
    f.i32WrapI64();
    f.call(fail);
    f.unreachable();
    f.end();
    f.lget(byte);
    f.i64c(0);
    f.i64LtS();
    f.lget(byte);
    f.i64c(255);
    f.i64GtS();
    f.i32Or();
    f.if_();
    emitFailStatic(f, sStoreByteRange);
    f.end();
    f.lget(header);
    f.i64Load(kPtrDataOffset);
    f.i32WrapI64();
    f.lget(at);
    f.i32WrapI64();
    f.i32Add();
    f.lget(byte);
    f.i32WrapI64();
    f.i32Store8(0);
    retNil();
    return index;
  }

  if (name == "intToPtr") {
    arityMsg(1, sIntToPtrArgc);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kInt));
    f.i64Eq();
    f.if_();
    f.else_();
    emitFailStatic(f, sIntToPtrType);
    f.end();
    f.i64c(static_cast<std::int64_t>(kPtrKindVoid));
    argPayload(0);
    f.i64c(0);
    f.call(newPtr);
    return index;
  }

  if (name == "ptrToInt") {
    arityMsg(1, sPtrToIntArgc);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kPtr));
    f.i64Eq();
    f.if_();
    f.else_();
    emitFailStatic(f, sPtrToIntType);
    f.end();
    std::uint32_t header = f.addLocal(kI32);
    argPayload(0);
    f.i32WrapI64();
    f.lset(header);
    f.lget(header);
    f.i64Load(kPtrReleasedOffset);
    f.i64c(0);
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sPtrToIntFreed);
    f.end();
    f.i64c(static_cast<std::int64_t>(kInt));
    // Only a `void *` carries a token. A `cstring`/`buffer` owns its bytes,
    // so its address is not a token the VM would hand back.
    f.lget(header);
    f.i64Load(kPtrKindOffset);
    f.i64c(static_cast<std::int64_t>(kPtrKindVoid));
    f.i64Eq();
    f.ifResult(kI64);
    f.lget(header);
    f.i64Load(kPtrDataOffset);
    f.else_();
    f.i64c(0);
    f.end();
    return index;
  }

  if (name == "free") {
    arityMsg(1, sFreeArgc);
    argTag(0);
    f.i64c(static_cast<std::int64_t>(kPtr));
    f.i64Eq();
    f.if_();
    f.else_();
    emitFailStatic(f, sFreeType);
    f.end();
    std::uint32_t header = f.addLocal(kI32);
    argPayload(0);
    f.i32WrapI64();
    f.lset(header);
    f.lget(header);
    f.i64Load(kPtrReleasedOffset);
    f.i64c(0);
    f.i64Ne();
    f.if_();
    emitFailStatic(f, sFreeFreed);
    f.end();
    f.lget(header);
    f.i64c(1);
    f.i64Store(kPtrReleasedOffset);
    f.lget(header);
    f.i64c(0);
    f.i64Store(kPtrLengthOffset);
    retNil();
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
      "$isError", "$nonNull", "$range", "range", "exit", "$slice", "$setClass",
      "time", "clock", "platform", "arch", "random", "seed",
      "cstring", "buffer", "cstr", "byteLen", "loadByte", "storeByte",
      "intToPtr", "ptrToInt", "free",
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
      "random", "seed", "clock", "time", "platform", "arch", "memSize",
      "assert", "assertEq", "panic", "pcall", "error", "isError", "$isError",
      "exit", "args",
      "readFile", "writeFile", "fileExists", "fsOpen", "fsRead", "fsWrite",
      "fsSeek", "fsClose", "fsRemove", "fsRename", "fsMkdir", "fsRmdir",
      "fsList", "fsStat", "fsCwd", "fsChdir", "range", "$range", "$nonNull",
      "$slice", "$setClass", "cstring", "buffer", "cstr", "byteLen", "loadByte",
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

  // PHASE 9.4: a bound method is only reachable through a static member read
  // (`a.push`, `a?.push`); indexing (`a[k]`) is an index, never a method. The
  // compiler records every member name, so only the reachable helpers keep a
  // body and the Unicode case tables are emitted only for `upper`/`lower`.
  std::unordered_set<std::string> usedMethodNames;
  for (const Bytecode* proto : protos) {
    usedMethodNames.insert(proto->usedMethods.begin(), proto->usedMethods.end());
  }
  rt.useUpper = usedMethodNames.count("upper") != 0;
  rt.useLower = usedMethodNames.count("lower") != 0;
  for (const std::string& name : usedMethodNames) {
    enableMethodsForName(name, rt.methodEnabled);
  }

  pool.base = static_cast<std::uint32_t>(globalCount * 16);
  rt.declareTypes();
  rt.addImports();
  rt.addStaticStrings();
  rt.dtoaScratch = pool.addZeros(kDtoaScratchBytes);
  // Table slot 0 is the dispatcher; the method helpers are ordinary functions
  // called by it, so only the dispatcher needs a table entry.
  rt.methodDispatch = m.reserveFunction(rt.tValue);
  rt.reserveArrayMethods();
  rt.emitCore();
  rt.emitArrayMethods();
  rt.emitStringMethods();
  rt.emitRangeMethods();
  rt.emitMethodDispatch();

  // Discard the bodies of methods the program cannot reach. The dispatcher
  // still names every helper, so the function index and type stay valid; an
  // unreachable body is the smallest legal one and traps if a future dynamic
  // path ever reaches it (PHASE 9.4).
  for (std::size_t i = 0; i < rt.methodHelpers.size() && i < kMethodCount; ++i) {
    if (rt.methodEnabled[i]) continue;
    Body& body = m.funcs[rt.methodHelpers[i] - m.importCount()];
    body.code = {0x00};
    body.localTypes.clear();
  }

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
    builtinTable[name] = 1 + static_cast<std::uint32_t>(builtinOrder.size());
    builtinOrder.push_back(name);
  }

  for (const Bytecode* proto : protos) {
    protoFunc[proto] = m.reserveFunction(rt.tValue);
  }
  for (std::size_t i = 0; i < protos.size(); ++i) {
    protoTable[protos[i]] =
        1 + static_cast<std::uint32_t>(builtinOrder.size() + i);
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
  m.tableFuncs.push_back(rt.methodDispatch);
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
