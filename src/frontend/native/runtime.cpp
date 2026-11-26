#include "native/runtime.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "native/dtoa.h"
#include "native/mathlib.h"
#include "native/parse_float.h"
#include "unicode_case.h"

namespace {

// Shorthand for the registers the runtime uses, so the emit helpers read like
// assembly instead of `X64Reg::Rax` everywhere. C++17 has no `using enum`, so
// they are plain constants.
constexpr X64Reg Rax = X64Reg::Rax;
constexpr X64Reg Rcx = X64Reg::Rcx;
constexpr X64Reg Rdx = X64Reg::Rdx;
constexpr X64Reg Rbx = X64Reg::Rbx;
constexpr X64Reg Rsi = X64Reg::Rsi;
constexpr X64Reg Rdi = X64Reg::Rdi;
constexpr X64Reg R8 = X64Reg::R8;
constexpr X64Reg R9 = X64Reg::R9;
constexpr X64Reg R10 = X64Reg::R10;
constexpr X64Reg R11 = X64Reg::R11;
constexpr X64Reg R12 = X64Reg::R12;
constexpr X64Reg R13 = X64Reg::R13;
constexpr X64Reg R14 = X64Reg::R14;
constexpr X64Reg R15 = X64Reg::R15;
constexpr X64Reg Rbp = X64Reg::Rbp;
constexpr X64Reg Rsp = X64Reg::Rsp;

constexpr std::int64_t kTagNil = static_cast<std::int64_t>(AotTag::Nil);
constexpr std::int64_t kTagBool = static_cast<std::int64_t>(AotTag::Bool);
constexpr std::int64_t kTagInt = static_cast<std::int64_t>(AotTag::Int);
constexpr std::int64_t kTagFloat = static_cast<std::int64_t>(AotTag::Float);
constexpr std::int64_t kTagDouble = static_cast<std::int64_t>(AotTag::Double);
constexpr std::int64_t kTagStr = static_cast<std::int64_t>(AotTag::Str);
constexpr std::int64_t kTagFunc = static_cast<std::int64_t>(AotTag::Func);
constexpr std::int64_t kTagArray = static_cast<std::int64_t>(AotTag::Array);
constexpr std::int64_t kTagMap = static_cast<std::int64_t>(AotTag::Map);
constexpr std::int64_t kTagRange = static_cast<std::int64_t>(AotTag::Range);
constexpr std::int64_t kTagError = static_cast<std::int64_t>(AotTag::Error);
constexpr std::int64_t kTagPtr = static_cast<std::int64_t>(AotTag::Ptr);
constexpr std::int64_t kTagUnset = static_cast<std::int64_t>(AotTag::Unset);

// Field offsets of a pointer handle (see `native/value.h`).
constexpr std::int64_t kPtrKindOffset = 0;
constexpr std::int64_t kPtrDataOffset = 8;
constexpr std::int64_t kPtrLengthOffset = 16;
constexpr std::int64_t kPtrReleasedOffset = 24;
constexpr std::int64_t kPtrKindVoid = static_cast<std::int64_t>(AotPtrKind::Void);
constexpr std::int64_t kPtrKindCString = static_cast<std::int64_t>(AotPtrKind::CString);
constexpr std::int64_t kPtrKindBuffer = static_cast<std::int64_t>(AotPtrKind::Buffer);

// Field offsets of the collection headers (see `native/value.h`). Arrays use a
// three-word header (length, capacity, block); maps share those three words and
// add the lazy hash index in two more. A map entry is a key pointer followed by
// a value.
constexpr std::int64_t kTableLengthOffset = 0;
constexpr std::int64_t kTableCapacityOffset = 8;
constexpr std::int64_t kTableBlockOffset = 16;
// Map-only header words: the lazy hash index over the entry array.
constexpr std::int64_t kTableHashCapacityOffset = 24;
constexpr std::int64_t kTableHashBlockOffset = 32;
// Class identity, after the hash index (PHASE 12).
constexpr std::int64_t kTableClassNameOffset = 40;
constexpr std::int64_t kTableMethodsOffset = 48;
// Below this many entries a linear scan beats the index's allocation.
constexpr std::int64_t kMapHashThreshold = 8;
constexpr std::int64_t kArrayItemSize = static_cast<std::int64_t>(sizeof(AotValue));
constexpr std::int64_t kMapEntrySize = static_cast<std::int64_t>(kAotMapEntrySize);
constexpr std::int64_t kMapEntryKeyOffset = 0;
constexpr std::int64_t kMapEntryValueOffset = 8;

// Field offsets of a range header (see `native/value.h`).
constexpr std::int64_t kRangeStartOffset = 0;
constexpr std::int64_t kRangeEndOffset = 8;
constexpr std::int64_t kRangeInclusiveOffset = 16;

// Field offsets of an error header (see `native/value.h`): the message
// `AotString` address, then the integer code.
constexpr std::int64_t kErrorMessageOffset = 0;
constexpr std::int64_t kErrorCodeOffset = 8;

// Byte offset of a string's first byte from its `AotString` header (see
// `native/value.h`): the length is an 8-byte word, the bytes follow.
constexpr std::int64_t kStringDataOffset = static_cast<std::int64_t>(kAotStringHeaderSize);

// Field offsets of a string builder header (see `native/value.h`). It shares the
// array/map three-word shape (length, capacity, block), so the array offsets
// above serve it too; these aliases name the intent.
constexpr std::int64_t kSbLengthOffset = 0;
constexpr std::int64_t kSbCapacityOffset = 8;
constexpr std::int64_t kSbDataOffset = 16;
// Initial capacity of a string builder, in bytes. A rendered scalar or a small
// collection fits without a regrow; the arena never frees, so the block is
// sized to cover the common case rather than the worst one.
constexpr std::int64_t kSbInitialCapacity = 64;
// The depth at which the recursive repr prints `...` instead of descending, so
// a self-referential structure terminates. Matches the hosted `writeValue`.
constexpr std::int64_t kReprMaxDepth = 8;

// The sign bit of an IEEE-754 binary64, used to negate a float by flipping it.
constexpr std::int64_t kDoubleSignBit = static_cast<std::int64_t>(0x8000000000000000ULL);
// INT64_MIN as a bit pattern, for the `idiv` overflow special case.
constexpr std::int64_t kInt64Min = static_cast<std::int64_t>(0x8000000000000000ULL);

// SSE registers are numbered from `kXmmBase` in the emitter API (see
// `native/codegen.h`), not from zero, so the runtime names its scratch xmms
// through that base.
constexpr int Xmm0 = kXmmBase + 0;
constexpr int Xmm1 = kXmmBase + 1;
constexpr int Xmm2 = kXmmBase + 2;

// Exit code of an uncaught runtime error, matching the development engine
// (`main.cpp` returns 70 after a `RuntimeError`).
constexpr std::int64_t kRuntimeErrorExit = 70;

// Stack frame `oskel_print_value` builds for a Float/Double: the dtoa `Scratch`
// block followed by a text buffer. Both sizes are rounded so the frame is a
// multiple of 16, keeping rsp 16-byte aligned at the `call` to the formatter
// (the ABI requires it, and the formatter's own `call`s rely on it).
constexpr std::int64_t kDtoaScratchBytes = static_cast<std::int64_t>(dtoa::kScratchBytes);
constexpr std::int64_t kDtoaTextBytes = 400;
constexpr std::int64_t kDtoaFrameBytes = kDtoaScratchBytes + kDtoaTextBytes;

// Every helper keeps the System V stack invariant: it is entered with
// rsp % 16 == 8 (the `call` pushed a return address), it pushes rbp so rsp is
// 16-byte aligned inside, and any `call` it makes therefore leaves its callee
// aligned. rbp is callee-saved, so preserving it also protects the caller's
// register file, whose base lives in rbp. `prologue`/`epilogue` make that
// uniform; a helper that never returns omits the epilogue.
void prologue(CodeGen& cg) {
  cg.push(Rbp);
  cg.movReg(Rbp, Rsp);
}

void epilogue(CodeGen& cg) {
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();
}

// A block of constant text emitted after all the code, addressed through a
// label. The runtime needs the error messages as bytes in the text section; a
// data island with a `lea`-resolved address is enough and keeps the whole
// thing inside the single code buffer (no .rodata section, no relocations).
struct DataBlob {
  CodeGen::Label label;
  std::string text;
};

// Everything the collection helpers (PHASE 7c.3a) need from the surrounding
// runtime: the labels of the helpers they call and the error/format text they
// reference. Bundled so the emitters take one argument instead of twenty.
struct CollectionEnv {
  CodeGen::Label alloc;
  CodeGen::Label fail;
  CodeGen::Label write;
  CodeGen::Label writeInt;
  CodeGen::Label formatDouble;
  CodeGen::Label strEq;        // long oskel_str_eq(AotString* a, AotString* b)
  CodeGen::Label strHash;      // long oskel_str_hash(AotString* s)
  CodeGen::Label mapFind;      // long oskel_map_find(AotMap* m, AotString* k)
  CodeGen::Label mapIndexBuild;   // void oskel_map_index_build(AotMap* m)
  CodeGen::Label mapIndexFill;    // void oskel_map_index_fill(AotMap* m)
  CodeGen::Label mapIndexInsert;  // void oskel_map_index_insert(AotMap* m, long i)
  CodeGen::Label blockEnsure;  // long oskel_block_ensure(void* h, long n, long itemSize)
  CodeGen::Label utf8Len;      // long oskel_utf8_len(AotString* s)
  CodeGen::Label utf8CharAt;   // (AotString*, long) -> char* in rax, length in rdx
  CodeGen::Label compare;      // AotValue oskel_compare(AotValue, AotValue, long)
  CodeGen::Label contains;     // AotValue oskel_contains(AotValue needle, AotValue haystack)
  CodeGen::Label call;         // AotValue oskel_call(AotValue, long, AotValue*)
  CodeGen::Label concat;       // AotValue oskel_concat(AotValue a, AotValue b)
  CodeGen::Label strBuiltin;   // AotValue oskel_builtin_str(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label truthy;       // long oskel_truthy(AotValue)
  CodeGen::Label rawPrint;     // void oskel_print_value_raw(AotValue)
  CodeGen::Label printRepr;    // void oskel_print_repr(AotValue, long depth)
  CodeGen::Label printStrEsc;  // void oskel_print_str_escaped(AotString*)
  CodeGen::Label lengthConst;  // data: the string "length"
  // The table helpers the file/directory builtins use to build an array or a
  // map (`tableNew` returns an `AotValue`, `tableSet` writes one entry). They
  // are the same helpers `NEWTABLE`/`SETTABLE` reach, so a builtin-built map is
  // an ordinary map.
  CodeGen::Label tableNew;
  CodeGen::Label tableSet;
  // The `std.fs.stat` field names, raw `AotString`s.
  CodeGen::Label sizeConst;
  CodeGen::Label modifiedConst;
  CodeGen::Label isFileConst;
  CodeGen::Label isDirConst;
  // Bound array methods (PHASE 7c.3c/7c.3e): `oskel_member_get` returns a
  // closure for `arr.push`/`arr.pop`/`arr.shift`/`arr.unshift`/`arr.insert`/
  // `arr.removeAt`/`arr.contains`/`arr.indexOf`/`arr.clear`, built by
  // `bindArrayMethod`; the method bodies are the `*Method` labels below. The
  // member names are the data islands `*Const` (raw `AotString`s, like
  // `lengthConst`).
  CodeGen::Label bindArrayMethod;
  // The text `repr`/`str` render for each bound method value, in selector
  // order: `"<native fn array.push>"`, `"<native fn array.pop>"`, ... The
  // binder hands the matching label to `oskel_closure` as its `display`.
  std::vector<CodeGen::Label> arrayMethodDisplays;
  std::vector<CodeGen::Label> mapMethodDisplays;
  std::vector<CodeGen::Label> stringMethodDisplays;
  std::vector<CodeGen::Label> rangeMethodDisplays;
  std::vector<std::string> arrayDisplayTexts;
  std::vector<std::string> mapDisplayTexts;
  std::vector<std::string> stringDisplayTexts;
  std::vector<std::string> rangeDisplayTexts;
  CodeGen::Label pushMethod;
  CodeGen::Label popMethod;
  CodeGen::Label shiftMethod;
  CodeGen::Label unshiftMethod;
  CodeGen::Label insertMethod;
  CodeGen::Label removeAtMethod;
  CodeGen::Label containsMethod;
  CodeGen::Label indexOfMethod;
  CodeGen::Label clearMethod;
  CodeGen::Label sliceMethod;
  CodeGen::Label concatMethod;
  CodeGen::Label reversedMethod;
  CodeGen::Label cloneMethod;
  CodeGen::Label mapMethod;
  CodeGen::Label filterMethod;
  CodeGen::Label eachMethod;
  CodeGen::Label findMethod;
  CodeGen::Label anyMethod;
  CodeGen::Label allMethod;
  CodeGen::Label reduceMethod;
  CodeGen::Label sumMethod;
  CodeGen::Label joinMethod;
  CodeGen::Label sortMethod;
  CodeGen::Label sortedMethod;
  CodeGen::Label emptyString;    // data: an empty `AotString` (length 0)
  CodeGen::Label pushConst;      // data: the string "push"
  CodeGen::Label popConst;       // data: the string "pop"
  CodeGen::Label shiftConst;     // data: the string "shift"
  CodeGen::Label unshiftConst;   // data: the string "unshift"
  CodeGen::Label insertConst;    // data: the string "insert"
  CodeGen::Label removeAtConst;  // data: the string "removeAt"
  CodeGen::Label containsConst;  // data: the string "contains"
  CodeGen::Label indexOfConst;   // data: the string "indexOf"
  CodeGen::Label clearConst;     // data: the string "clear"
  CodeGen::Label sliceConst;     // data: the string "slice"
  CodeGen::Label concatConst;    // data: the string "concat"
  CodeGen::Label reversedConst;  // data: the string "reversed"
  CodeGen::Label cloneConst;     // data: the string "clone"
  CodeGen::Label mapConst;       // data: the string "map"
  CodeGen::Label filterConst;    // data: the string "filter"
  CodeGen::Label eachConst;      // data: the string "each"
  CodeGen::Label findConst;      // data: the string "find"
  CodeGen::Label anyConst;       // data: the string "any"
  CodeGen::Label allConst;       // data: the string "all"
  CodeGen::Label reduceConst;    // data: the string "reduce"
  CodeGen::Label sumConst;       // data: the string "sum"
  CodeGen::Label joinConst;      // data: the string "join"
  CodeGen::Label sortConst;      // data: the string "sort"
  CodeGen::Label sortedConst;    // data: the string "sorted"
  // Bound string methods (PHASE 7c.3i): `oskel_member_get` returns a closure
  // for `s.upper`/`s.lower`/... built by `bindStringMethod`; the method bodies
  // are the `string*Method` labels below. Each takes the receiver from its
  // closure's single upvalue cell, exactly like the array methods.
  CodeGen::Label bindStringMethod;
  CodeGen::Label stringUpperMethod;
  CodeGen::Label stringLowerMethod;
  CodeGen::Label stringTrimMethod;
  CodeGen::Label stringTrimStartMethod;
  CodeGen::Label stringTrimEndMethod;
  CodeGen::Label stringContainsMethod;
  CodeGen::Label stringStartsWithMethod;
  CodeGen::Label stringEndsWithMethod;
  CodeGen::Label stringIndexOfMethod;
  CodeGen::Label stringSubstrMethod;
  CodeGen::Label stringReversedMethod;
  CodeGen::Label stringRepeatMethod;
  CodeGen::Label stringSplitMethod;
  CodeGen::Label stringLinesMethod;
  CodeGen::Label stringCharsMethod;
  CodeGen::Label stringReplaceMethod;
  CodeGen::Label stringReplaceFirstMethod;
  CodeGen::Label stringPadStartMethod;
  CodeGen::Label stringPadEndMethod;
  // Helpers the string bodies share: `strNew` builds an `AotString` from
  // bytes and `sbNew`/`sbAppend`/`sbAppendByte` drive a growable builder.
  CodeGen::Label strNew;
  CodeGen::Label sbNew;
  CodeGen::Label sbAppend;
  CodeGen::Label sbAppendByte;
  CodeGen::Label strFind;        // long oskel_str_find(AotString* hay, AotString* needle)
  CodeGen::Label strFindFrom;    // long oskel_str_find_from(AotString* hay, AotString* needle, long start)
  CodeGen::Label strPrefixEq;    // long oskel_str_prefix_eq(AotString* a, AotString* b)
  CodeGen::Label isSpace;        // long oskel_is_space(long byte)
  CodeGen::Label strCpCount;     // long oskel_str_cp_count(AotString* text, long byteLen)
  CodeGen::Label strByteAt;      // long oskel_str_byte_at(AotString* text, long index)
  CodeGen::Label caseMap;        // void oskel_case_map(AotString* text, AotStringBuilder* sb, const void* table, long count, long upper)
  // The Unicode simple case-mapping tables, emitted as data: a sorted array of
  // `{first, last, delta}` runs (three int32 each) and its length. `upperTable`
  // and `lowerTable` are the tables, `upperCount`/`lowerCount` the entry counts.
  CodeGen::Label upperTable;
  CodeGen::Label lowerTable;
  std::int64_t upperCount;
  std::int64_t lowerCount;
  CodeGen::Label upperConst;     // data: the string "upper"
  CodeGen::Label lowerConst;     // data: the string "lower"
  CodeGen::Label trimConst;      // data: the string "trim"
  CodeGen::Label trimStartConst; // data: the string "trimStart"
  CodeGen::Label trimEndConst;   // data: the string "trimEnd"
  CodeGen::Label startsWithConst;// data: the string "startsWith"
  CodeGen::Label endsWithConst;  // data: the string "endsWith"
  CodeGen::Label substrConst;    // data: the string "substr"
  CodeGen::Label repeatConst;    // data: the string "repeat"
  CodeGen::Label containsStrConst; // data: the string "contains" (string method)
  CodeGen::Label indexOfStrConst;  // data: the string "indexOf" (string method)
  CodeGen::Label reversedStrConst; // data: the string "reversed" (string method)
  CodeGen::Label splitConst;       // data: the string "split"
  CodeGen::Label linesConst;       // data: the string "lines"
  CodeGen::Label charsConst;       // data: the string "chars"
  CodeGen::Label replaceConst;     // data: the string "replace"
  CodeGen::Label replaceFirstConst; // data: the string "replaceFirst"
  CodeGen::Label padStartConst;    // data: the string "padStart"
  CodeGen::Label padEndConst;      // data: the string "padEnd"
  CodeGen::Label spaceConst;       // data: the string " " (the default pad fill)
  const DataBlob* msgUpperArgc;
  const DataBlob* msgLowerArgc;
  const DataBlob* msgTrimArgc;
  const DataBlob* msgTrimStartArgc;
  const DataBlob* msgTrimEndArgc;
  const DataBlob* msgStrContainsArgc;
  const DataBlob* msgStartsWithArgc;
  const DataBlob* msgEndsWithArgc;
  const DataBlob* msgStrIndexOfArgc;
  const DataBlob* msgSubstrArgc;
  const DataBlob* msgStrReversedArgc;
  const DataBlob* msgRepeatArgc;
  const DataBlob* msgStrArgType;
  const DataBlob* msgSubstrStartType;
  const DataBlob* msgSubstrLengthType;
  const DataBlob* msgRepeatCountType;
  const DataBlob* msgRepeatNegative;
  const DataBlob* msgRepeatTooLarge;
  const DataBlob* msgSplitArgc;
  const DataBlob* msgSplitType;
  const DataBlob* msgLinesArgc;
  const DataBlob* msgCharsArgc;
  const DataBlob* msgReplaceArgc;
  const DataBlob* msgReplaceSearchType;
  const DataBlob* msgReplaceReplType;
  const DataBlob* msgReplaceEmpty;
  const DataBlob* msgReplaceFirstArgc;
  const DataBlob* msgReplaceFirstSearchType;
  const DataBlob* msgReplaceFirstReplType;
  const DataBlob* msgReplaceFirstEmpty;
  const DataBlob* msgPadStartArgc;
  const DataBlob* msgPadStartWidthType;
  const DataBlob* msgPadStartFillType;
  const DataBlob* msgPadStartEmpty;
  const DataBlob* msgPadStartTooLarge;
  const DataBlob* msgPadEndArgc;
  const DataBlob* msgPadEndWidthType;
  const DataBlob* msgPadEndFillType;
  const DataBlob* msgPadEndEmpty;
  const DataBlob* msgPadEndTooLarge;
  // Bound map methods (PHASE 7c.3g): `oskel_member_get` returns a closure for
  // `m.keys`/`m.values`/`m.entries`/`m.has`/`m.get`/`m.set`/`m.delete`/
  // `m.clear`/`m.merge`/`m.clone`, built by `bindMapMethod` (the same emitter
  // the array methods use); the method bodies are the `*Method` labels below.
  // The member names are the data islands `*Const` (`clear`/`clone` reuse the
  // array ones, the strings being identical).
  CodeGen::Label bindMapMethod;
  CodeGen::Label mapKeysMethod;
  CodeGen::Label mapValuesMethod;
  CodeGen::Label mapEntriesMethod;
  CodeGen::Label mapHasMethod;
  CodeGen::Label mapGetMethod;
  CodeGen::Label mapSetMethod;
  CodeGen::Label mapDeleteMethod;
  CodeGen::Label mapClearMethod;
  CodeGen::Label mapMergeMethod;
  CodeGen::Label mapCloneMethod;
  CodeGen::Label keysConst;      // data: the string "keys"
  CodeGen::Label valuesConst;    // data: the string "values"
  CodeGen::Label entriesConst;   // data: the string "entries"
  CodeGen::Label hasConst;       // data: the string "has"
  CodeGen::Label getConst;       // data: the string "get"
  CodeGen::Label setConst;       // data: the string "set"
  CodeGen::Label deleteConst;    // data: the string "delete"
  CodeGen::Label mergeConst;     // data: the string "merge"
  const DataBlob* msgKeysArgc;
  const DataBlob* msgValuesArgc;
  const DataBlob* msgEntriesArgc;
  const DataBlob* msgHasArgc;
  const DataBlob* msgHasKeyType;
  const DataBlob* msgGetArgc;
  const DataBlob* msgGetKeyType;
  const DataBlob* msgSetArgc;
  const DataBlob* msgSetKeyType;
  const DataBlob* msgDeleteArgc;
  const DataBlob* msgDeleteKeyType;
  const DataBlob* msgMergeArgc;
  const DataBlob* msgMergeType;
  // Bound range methods (PHASE 7c.3l): `r.toArray`/`r.contains`, built by
  // `bindRangeMethod`; `lengthConst`/`containsConst` reuse the islands above.
  CodeGen::Label bindRangeMethod;
  CodeGen::Label rangeToArrayMethod;
  CodeGen::Label rangeContainsMethod;
  CodeGen::Label toArrayConst;   // data: the string "toArray"
  const DataBlob* msgToArrayArgc;
  const DataBlob* msgToArrayTooLarge;
  const DataBlob* msgRangeIntType;
  // The error member names (`err.message` / `err.code`), raw `AotString`s.
  CodeGen::Label messageConst;   // data: the string "message"
  CodeGen::Label codeConst;      // data: the string "code"
  const DataBlob* msgOom;
  const DataBlob* msgArrayIndexType;
  const DataBlob* msgStringIndexType;
  const DataBlob* msgArrayIndexKind;
  const DataBlob* msgStringIndexKind;
  const DataBlob* msgIndexOutOfRange;
  const DataBlob* msgIndexRangeClose;
  const DataBlob* msgMapKeyType;
  const DataBlob* msgBadIndex;
  const DataBlob* msgNoMember;
  const DataBlob* msgImmutable;
  const DataBlob* msgBadAssign;
  const DataBlob* msgBadField;
  const DataBlob* msgNoField;
  const DataBlob* msgBadIn;
  const DataBlob* msgMapMembershipKey;
  const DataBlob* msgStringMembershipOperand;
  const DataBlob* msgMapValue;
  const DataBlob* msgNotIterable;
  const DataBlob* textTrue;
  const DataBlob* textFalse;
  const DataBlob* textNil;
  const DataBlob* textFunc;
  const DataBlob* textComma;
  const DataBlob* textColon;
  const DataBlob* textOpenBracket;
  const DataBlob* textCloseBracket;
  const DataBlob* textOpenBrace;
  const DataBlob* textCloseBrace;
  const DataBlob* textEllipsis;
  const DataBlob* textQuote;
  const DataBlob* textEscQuote;
  const DataBlob* textEscBackslash;
  const DataBlob* textEscNL;
  const DataBlob* textEscTab;
  const DataBlob* textEscCR;
  const DataBlob* textHexPrefix;
};

// Emits `oskel_write(1, rdi, rsi)`: writes `rsi` bytes from `rdi` to stdout.
// Clobbers rdi, rsi, rdx and rax (the syscall's return).
void inlineWriteStdout(CodeGen& cg, CodeGen::Label writeLabel) {
  cg.movReg(Rdx, Rsi);
  cg.movReg(Rsi, Rdi);
  cg.movImm(Rdi, 1);
  cg.callLabel(writeLabel);
}

// Emits `oskel_write(fdReg, rdi, rsi)`: writes `rsi` bytes from `rdi` to `fdReg`.
// `fdReg` must not be rdi, rsi or rdx (they are the syscall's other arguments).
// Clobbers rdi, rsi, rdx and rax.
void inlineWriteFd(CodeGen& cg, CodeGen::Label writeLabel, X64Reg fdReg) {
  cg.movReg(Rdx, Rsi);
  cg.movReg(Rsi, Rdi);
  cg.movReg(Rdi, fdReg);
  cg.callLabel(writeLabel);
}

// Emits a write of `rsi` bytes from `rdi` to a *constant* fd (1 for stdout, 2
// for stderr). fd 1 uses the tight `inlineWriteStdout`; any other fd loads r10
// (a scratch the helpers own) and goes through `inlineWriteFd`. Clobbers rdi,
// rsi, rdx, r10 and rax.
void emitWriteToFd(CodeGen& cg, CodeGen::Label writeLabel, std::int64_t fd) {
  if (fd == 1) {
    inlineWriteStdout(cg, writeLabel);
  } else {
    cg.movImm(R10, fd);
    inlineWriteFd(cg, writeLabel, R10);
  }
}

// Emits a write of the decimal form of `reg` (taken as non-negative) to fd 2.
// It builds the digits downward from `[rbp - 16]`, which the caller must have
// covered by lowering `rsp` past it first. Clobbers rdi, rsi, rdx, r10, rax,
// rcx and r8.
void emitWriteUnsignedDecimal(CodeGen& cg, CodeGen::Label writeLabel, X64Reg reg) {
  constexpr std::int32_t kScratch = -16;  // one past the last digit
  CodeGen::Label loop = cg.newLabel();

  cg.movReg(Rax, reg);
  cg.leaMem(R8, Rbp, kScratch);
  cg.bind(loop);
  cg.movImm(Rcx, 10);
  cg.divUnsigned(Rcx);  // rax /= 10, rdx = digit (rdx pre-zeroed)
  cg.addImm32(Rdx, static_cast<std::int32_t>('0'));
  cg.subRegImm32(R8, 1);
  cg.movByteToMem(R8, 0, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, loop);
  cg.movReg(Rdi, R8);
  cg.leaMem(Rsi, Rbp, kScratch);
  cg.subReg(Rsi, R8);
  emitWriteToFd(cg, writeLabel, 2);
}

// Emits `call fail(message)`: loads the message address and length, then calls
// the shared fail helper. Never returns.
void emitFailCall(CodeGen& cg, CodeGen::Label fail, const DataBlob& message) {
  cg.leaRipLabel(Rdi, message.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(message.text.size()));
  cg.callLabel(fail);
  cg.ud2();
}

// The type-name blobs the `, got '<type>'` guards quote, built from the same
// blobs `type` uses, so the vocabulary cannot drift.
struct TypeNameBlobs {
  const DataBlob* nil;
  const DataBlob* boolValue;
  const DataBlob* intValue;
  const DataBlob* floatValue;
  const DataBlob* doubleValue;
  const DataBlob* str;
  const DataBlob* array;
  const DataBlob* map;
  const DataBlob* func;
  const DataBlob* range;
  const DataBlob* error;
  const DataBlob* ptr;
  const DataBlob* cstring;
};

// The handles and fragments a guard needs to word `..., got '<type>'` or
// `... (length N)` at run time. The builtin and method emitters that own those
// guards do not receive the runtime handles, so `emitAotRuntime` publishes them
// here before emitting any of them -- the same file-scope trick as
// `gArityFailLabel`, and for the same reason.
struct DynamicMessageEnv {
  CodeGen::Label failDynamic;
  CodeGen::Label sbNew;
  CodeGen::Label sbAppend;
  const TypeNameBlobs* names = nullptr;
  const DataBlob* textGot = nullptr;
  const DataBlob* textTypeQuote = nullptr;
  const DataBlob* textTypeOpen = nullptr;
  const DataBlob* textHasNoMember = nullptr;
  const DataBlob* textWith = nullptr;
  const DataBlob* textAnd = nullptr;
  const DataBlob* quoteClose = nullptr;
};
DynamicMessageEnv gMsgText;

// One piece of a message assembled at run time: either a constant blob or the
// decimal form of a register. A message is a list of these, appended to a fresh
// builder and handed to `failDynamic`, which is how the hosted engine words
// every error it builds by concatenation.
struct FailPart {
  const DataBlob* text = nullptr;  // constant bytes; nullptr for a number
  X64Reg reg = Rax;                // the number, when `text` is null
};

// The message builders below are defined with the rest of the runtime bodies,
// but the collection-method emitters that use them are defined earlier, so the
// prototypes live here.
void emitFailParts(CodeGen& cg, CodeGen::Label failDynamic, CodeGen::Label sbNew,
                   CodeGen::Label sbAppend, std::initializer_list<FailPart> parts);
void emitAppendBlob(CodeGen& cg, CodeGen::Label sbAppend, std::int32_t builderSlot,
                    const DataBlob& blob);
void emitAppendInt(CodeGen& cg, CodeGen::Label sbAppend, std::int32_t builderSlot,
                   std::int32_t scratchSlot, X64Reg reg);
void emitFailLength(CodeGen& cg, CodeGen::Label label, CodeGen::Label failDynamic,
                    CodeGen::Label sbNew, CodeGen::Label sbAppend,
                    const DataBlob& prefix, const DataBlob& close, X64Reg lenReg);
void emitFailGotType(CodeGen& cg, CodeGen::Label label, CodeGen::Label failDynamic,
                     CodeGen::Label sbNew, CodeGen::Label sbAppend,
                     const DataBlob& prefix, const DataBlob& textGot,
                     const DataBlob& textQuoteEnd, const TypeNameBlobs& names,
                     X64Reg tagReg, X64Reg payloadReg);
void emitTypeTailFail(CodeGen& cg, const DataBlob& prefix, X64Reg tagReg,
                      X64Reg payloadReg);
void emitTypeQuoteFail(CodeGen& cg, const DataBlob& prefix, X64Reg tagReg,
                       X64Reg payloadReg);
void emitNoMemberFail(CodeGen& cg, X64Reg tagReg, X64Reg payloadReg,
                      X64Reg nameReg);
void emitTypeNameAppend(CodeGen& cg, CodeGen::Label sbAppend, std::int32_t tagSlot,
                        std::int32_t payloadSlot, std::int32_t builderSlot);
void emitTwoTypeFail(CodeGen& cg, const DataBlob& prefix, const DataBlob& separator,
                     X64Reg tag1, X64Reg payload1, X64Reg tag2, X64Reg payload2);

// The one wrong-arity body (`emitArityFail`) is emitted once per program, but
// the ~70 guards that call it are spread over the builtin and method emitters,
// none of which receives the runtime handles. `emitAotRuntime` sets this slot
// from `handles.arityFail` before it emits any of them.
CodeGen::Label gArityFailLabel;

// Emits `call oskel_arity_fail(msg, len, argc)`, where `argc` is the count still
// live at the guard. Loading the message into rdi/rsi would clobber it, so the
// count moves to rdx first. `argcReg` is rsi everywhere except `error()`, which
// keeps its count in r12.
void emitArityFailCall(CodeGen& cg, const DataBlob& msgArgc, X64Reg argcReg = Rsi) {
  cg.movReg(Rdx, argcReg);
  cg.leaRipLabel(Rdi, msgArgc.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(msgArgc.text.size()));
  cg.callLabel(gArityFailLabel);
  cg.ud2();
}

// The `... (length N)` tail: appends the length in `lenReg` in decimal. It
// mirrors `insert() index out of range (length 3)` and `removeAt() ...`, where
// the hosted method names no index. Runs in the caller's frame, so the frame is
// sized for the worst case, a caller at `rsp == rbp`.
void emitFailLength(CodeGen& cg, CodeGen::Label label, CodeGen::Label failDynamic,
                    CodeGen::Label sbNew, CodeGen::Label sbAppend,
                    const DataBlob& prefix, const DataBlob& close, X64Reg lenReg) {
  constexpr std::int32_t kBuilder = -96;
  constexpr std::int32_t kLength = -104;
  constexpr std::int32_t kScratchBytes = 24;  // 20 digits of int64 plus a sign
  constexpr std::int32_t kScratch = -160;  // one past the last digit
  constexpr std::int32_t kFrameBytes = -kScratch + kScratchBytes + 8;

  cg.bind(label);
  cg.subRegImm32(Rsp, kFrameBytes);
  cg.movToMem(Rbp, kLength, lenReg);
  cg.callLabel(sbNew);
  cg.movToMem(Rbp, kBuilder, Rax);
  emitAppendBlob(cg, sbAppend, kBuilder, prefix);
  cg.movFromMem(Rax, Rbp, kLength);
  emitAppendInt(cg, sbAppend, kBuilder, kScratch, Rax);
  emitAppendBlob(cg, sbAppend, kBuilder, close);
  cg.movFromMem(Rax, Rbp, kBuilder);
  cg.movFromMem(Rdi, Rax, kSbDataOffset);
  cg.movFromMem(Rsi, Rax, kSbLengthOffset);
  cg.callLabel(failDynamic);
  cg.ud2();
}

// `void fail(rdi = text, rsi = length)`: reports a runtime error from a fixed
// message blob. It is a plain jump to `failDynamic`, the one sink, so both the
// fixed and the assembled messages share the presentation and the `pcall`
// hand-off. The jump keeps the stack shape a `call` produced (rsp % 16 == 8),
// which is what the sink's own prologue expects.
void emitFail(CodeGen& cg, CodeGen::Label label, CodeGen::Label failDynamic) {
  cg.bind(label);
  cg.jmp(failDynamic);
}

// Emits `rax = 1` if `tag` is a number (int, float or double), else `rax = 0`.
// The three tags are contiguous (Int=2, Float=3, Double=4), so the check is a
// range test. Clobbers rax and the flags.
void emitIsNumberFlag(CodeGen& cg, X64Reg tag) {
  CodeGen::Label notNumber = cg.newLabel();
  cg.xorReg(Rax, Rax);
  cg.cmpImm32(tag, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::B, notNumber);  // tag < Int
  cg.cmpImm32(tag, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::A, notNumber);  // tag > Double
  cg.movImm(Rax, 1);
  cg.bind(notNumber);
}

// Emits `xmm = (double)value`, where `value` is `tag`:`payload`. An int is
// converted with `cvtsi2sd`; a float/double payload already *is* the bit
// pattern of a binary64 value, so it moves with `movq`. A float is stored
// widened, exactly as the hosted `Value` stores it, so the conversion is
// lossless. Clobbers rax.
void emitToDouble(CodeGen& cg, X64Reg tag, X64Reg payload, int xmm) {
  CodeGen::Label isInt = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.cmpImm32(tag, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, isInt);
  cg.movqToXmm(xmm, payload);
  cg.jmp(done);
  cg.bind(isInt);
  cg.cvtsi2sd(xmm, payload);
  cg.bind(done);
}

// `long oskel_truthy(AotValue a)`: 1 when the value is truthy, else 0. Mirrors
// `Value::truthy`: a bool is itself, nil is false, everything else is true.
void emitTruthy(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label isBool = cg.newLabel();
  CodeGen::Label isNil = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Eq, isBool);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, isNil);
  cg.movImm(Rax, 1);  // every other value is truthy
  epilogue(cg);
  cg.bind(isBool);
  cg.movReg(Rax, Rsi);
  epilogue(cg);
  cg.bind(isNil);
  cg.xorReg(Rax, Rax);
  epilogue(cg);
}

// The shared body of the printers: renders one value to file descriptor `fd`.
// `newline` appends a trailing '\n' (the `print` builtin and the standalone
// `oskel_print_value`), which the raw variants omit so they can join arguments.
// int, bool, nil and str render directly; a Float/Double is formatted with the
// embedded dtoa (`oskel_format_double`) into an arena buffer, then written.
void emitPrintValueCore(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                        CodeGen::Label fail, CodeGen::Label writeInt,
                        CodeGen::Label formatDouble, CodeGen::Label printRepr,
                        const DataBlob& badValue,
                        const DataBlob& textTrue, const DataBlob& textFalse,
                        const DataBlob& textNil, const DataBlob& textNewline,
                        const DataBlob& textPtr, std::int64_t fd, bool newline,
                        bool hosted) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asNum = cg.newLabel();
  CodeGen::Label asRepr = cg.newLabel();
  CodeGen::Label asError = cg.newLabel();
  CodeGen::Label asPtr = cg.newLabel();
  CodeGen::Label ptrOpaque = cg.newLabel();
  CodeGen::Label boolFalse = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);

  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, asInt);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Eq, asBool);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, asNil);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, asStr);
  // The remaining tags are Float (3) and Double (4); everything else is not
  // printable.
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, asNum);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, asNum);
  // Functions and collections are rendered by the recursive repr helper, which
  // quotes nested strings, prints arrays as `[a, b]` and maps as `{k: v}`.
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFunc));
  cg.jcc(Jcc::Eq, asRepr);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, asRepr);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, asRepr);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, asRepr);
  // An error prints its message alone (`Value::toString` display form).
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagError));
  cg.jcc(Jcc::Eq, asError);
  // A pointer handle prints the text a `cstring` points at, up to the first
  // NUL, and `<ptr>` for any other kind -- never an address the program must
  // not see (the hosted `writeValue` rule).
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagPtr));
  cg.jcc(Jcc::Eq, asPtr);
  emitFailCall(cg, fail, badValue);

  cg.bind(asPtr);
  cg.movFromMem(Rax, Rsi, kPtrKindOffset);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kPtrKindCString));
  cg.jcc(Jcc::Ne, ptrOpaque);
  // A `cstring` displays up to the first NUL, which is C's view of it: an
  // embedded NUL ends the text even though the handle owns the bytes after it.
  cg.movFromMem(Rdi, Rsi, kPtrDataOffset);   // the bytes
  cg.movFromMem(Rcx, Rsi, kPtrLengthOffset); // the owned length
  cg.xorReg(R8, R8);                         // scanned length
  CodeGen::Label ptrScan = cg.newLabel();
  CodeGen::Label ptrScanEnd = cg.newLabel();
  cg.bind(ptrScan);
  cg.cmpReg(R8, Rcx);
  cg.jcc(Jcc::Ae, ptrScanEnd);
  cg.movReg(R9, Rdi);
  cg.addReg(R9, R8);
  cg.movzxFromMem(Rax, R9, 0);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, ptrScanEnd);
  cg.addImm32(R8, 1);
  cg.jmp(ptrScan);
  cg.bind(ptrScanEnd);
  cg.movReg(Rsi, R8);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, done);                     // an empty handle writes nothing
  emitWriteToFd(cg, writeLabel, fd);
  cg.jmp(done);
  cg.bind(ptrOpaque);
  cg.leaRipLabel(Rdi, textPtr.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textPtr.text.size()));
  emitWriteToFd(cg, writeLabel, fd);
  cg.jmp(done);

  cg.bind(asRepr);
  cg.xorReg(Rdx, Rdx);          // depth = 0
  cg.movImm(Rcx, fd);           // target fd for the nested render
  cg.callLabel(printRepr);
  cg.jmp(done);

  // An error: write the message `AotString`'s bytes verbatim (no quoting),
  // exactly like a string.
  cg.bind(asError);
  cg.movFromMem(Rcx, Rsi, kErrorMessageOffset);  // message AotString*
  cg.movFromMem(Rdi, Rcx, 0);                    // length
  cg.addImm32(Rcx, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rsi, Rdi);                           // rsi = length
  cg.movReg(Rdi, Rcx);                           // rdi = bytes
  emitWriteToFd(cg, writeLabel, fd);
  cg.jmp(done);

  cg.bind(asInt);
  cg.movReg(Rdi, Rsi);
  cg.movImm(R10, fd);  // `oskel_write_int` writes to the fd in r10
  cg.callLabel(writeInt);
  cg.jmp(done);

  // A Float/Double: format into an arena buffer with the embedded dtoa, then
  // write the returned length. The `Scratch` block and the text buffer live on
  // the stack for the call; `kDtoaScratchBytes` is padded to a multiple of 16
  // so rsp stays 16-byte aligned (the ABI requirement at every `call`).
  cg.bind(asNum);
  cg.subRegImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  cg.movReg(Rcx, Rsp);                        // scratch = rsp
  cg.movReg(Rdx, Rsp);
  cg.addImm32(Rdx, static_cast<std::int32_t>(kDtoaScratchBytes));  // out = rsp + scratch
  cg.callLabel(formatDouble);                 // rdi=tag, rsi=payload (unchanged)
  cg.movReg(Rsi, Rax);                        // length
  cg.movReg(Rdi, Rsp);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kDtoaScratchBytes));
  // In the hosted mode the write reaches libc, whose own stack use can reach the
  // dead zone below rsp; the frame must stay allocated so the text buffer stays
  // above rsp. The freestanding syscall stub cannot touch it, so its code is
  // left exactly as it was (the byte-identity contract of that backend).
  if (hosted) {
    emitWriteToFd(cg, writeLabel, fd);
    cg.addImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  } else {
    cg.addImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
    emitWriteToFd(cg, writeLabel, fd);
  }
  cg.jmp(done);

  cg.bind(asBool);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, boolFalse);
  cg.leaRipLabel(Rdi, textTrue.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textTrue.text.size()));
  emitWriteToFd(cg, writeLabel, fd);
  cg.jmp(done);
  cg.bind(boolFalse);
  cg.leaRipLabel(Rdi, textFalse.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textFalse.text.size()));
  emitWriteToFd(cg, writeLabel, fd);
  cg.jmp(done);

  cg.bind(asNil);
  cg.leaRipLabel(Rdi, textNil.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textNil.text.size()));
  emitWriteToFd(cg, writeLabel, fd);
  cg.jmp(done);

  // A string is its bytes verbatim: print writes the payload's `length` bytes
  // starting right after the header. No quoting, matching `Value::toString`.
  // The write helpers take (rdi = buffer, rsi = length).
  cg.bind(asStr);
  cg.movFromMem(Rcx, Rsi, 0);  // length
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rdi, Rsi);         // buffer = payload + 8
  cg.movReg(Rsi, Rcx);         // length
  emitWriteToFd(cg, writeLabel, fd);

  cg.bind(done);
  if (newline) {
    cg.leaRipLabel(Rdi, textNewline.label);
    cg.movImm(Rsi, static_cast<std::int64_t>(textNewline.text.size()));
    emitWriteToFd(cg, writeLabel, fd);
  }
  epilogue(cg);
}

// `void oskel_print_value(AotValue v)`: prints the value followed by a newline.
void emitPrintValue(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                    CodeGen::Label fail, CodeGen::Label writeInt,
                    CodeGen::Label formatDouble, CodeGen::Label printRepr,
                    const DataBlob& badValue,
                    const DataBlob& textTrue, const DataBlob& textFalse,
                    const DataBlob& textNil, const DataBlob& textNewline,
                    const DataBlob& textPtr, bool hosted) {
  emitPrintValueCore(cg, label, writeLabel, fail, writeInt, formatDouble, printRepr,
                     badValue, textTrue, textFalse, textNil, textNewline, textPtr,
                     /*fd=*/1, /*newline=*/true, hosted);
}

// `void oskel_print_value_raw(AotValue v)`: prints the value with no trailing
// newline, to stdout, for the `print`/`write` builtins.
void emitPrintValueRaw(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                       CodeGen::Label fail, CodeGen::Label writeInt,
                       CodeGen::Label formatDouble, CodeGen::Label printRepr,
                       const DataBlob& badValue,
                       const DataBlob& textTrue, const DataBlob& textFalse,
                       const DataBlob& textNil, const DataBlob& textNewline,
                       const DataBlob& textPtr, bool hosted) {
  emitPrintValueCore(cg, label, writeLabel, fail, writeInt, formatDouble, printRepr,
                     badValue, textTrue, textFalse, textNil, textNewline, textPtr,
                     /*fd=*/1, /*newline=*/false, hosted);
}

// `void oskel_print_value_err_raw(AotValue v)`: prints the value with no trailing
// newline, to stderr, for the `eprint` builtin.
void emitPrintValueErrRaw(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                          CodeGen::Label fail, CodeGen::Label writeInt,
                          CodeGen::Label formatDouble, CodeGen::Label printRepr,
                          const DataBlob& badValue,
                          const DataBlob& textTrue, const DataBlob& textFalse,
                          const DataBlob& textNil, const DataBlob& textNewline,
                          const DataBlob& textPtr, bool hosted) {
  emitPrintValueCore(cg, label, writeLabel, fail, writeInt, formatDouble, printRepr,
                     badValue, textTrue, textFalse, textNil, textNewline, textPtr,
                     /*fd=*/2, /*newline=*/false, hosted);
}

// One of the `print`/`write`/`eprint` builtins, as a machine-code function with
// the same convention as a bytecode proto: `AotValue entry(AotValue* args, long
// argc, AotClosure* self)`. `args` points at the caller's `R[a+1]` and the
// caller's register file grows down, so `args[i]` sits 16 bytes *below*
// `args[i-1]` -- the same layout the proto prologue reads. Arguments are joined
// with a single space; `rawPrint` renders each one (no newline) and `writeLabel`
// writes the separators. `newline` appends a trailing '\n' (print/eprint), which
// `write` omits. The result is always nil.
void emitPrintBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label rawPrint,
                      CodeGen::Label writeLabel, const DataBlob& textSpace,
                      const DataBlob& textNewline, std::int64_t fd, bool newline) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label body = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);          // scratch for the index (keeps rsp aligned)
  cg.movReg(Rbx, Rdi);             // rbx = args
  cg.movReg(R12, Rsi);             // r12 = argc
  cg.movImmToMem(Rbp, -24, 0);     // index = 0

  cg.bind(loop);
  cg.movFromMem(Rax, Rbp, -24);    // index
  cg.cmpReg(Rax, R12);
  cg.jcc(Jcc::Ge, done);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, body);           // index == 0: no separator
  cg.leaRipLabel(Rdi, textSpace.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textSpace.text.size()));
  emitWriteToFd(cg, writeLabel, fd);

  cg.bind(body);
  cg.movFromMem(Rax, Rbp, -24);
  cg.shlImm8(Rax, 4);              // index * 16
  cg.movReg(Rdi, Rbx);
  cg.subReg(Rdi, Rax);             // rdi = &args[index]
  cg.movFromMem(Rsi, Rdi, 8);      // payload
  cg.movFromMem(Rdi, Rdi, 0);      // tag
  cg.callLabel(rawPrint);
  cg.movFromMem(Rax, Rbp, -24);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbp, -24, Rax);      // index++
  cg.jmp(loop);

  cg.bind(done);
  if (newline) {
    cg.leaRipLabel(Rdi, textNewline.label);
    cg.movImm(Rsi, static_cast<std::int64_t>(textNewline.text.size()));
    emitWriteToFd(cg, writeLabel, fd);
  }
  cg.xorReg(Rax, Rax);             // nil result
  cg.xorReg(Rdx, Rdx);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();
}

// `void write_int(long value)`: prints a signed 64-bit integer in decimal
// without libc. The digits are produced least-significant first into a stack
// buffer and written in one syscall; `INT64_MIN` is handled by taking the
// magnitude in unsigned arithmetic (`neg` on the two's-complement pattern is
// exact for every value, including the most negative one).
void emitWriteInt(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel) {
  constexpr std::int64_t kBuffer = 48;  // 20 digits + sign, rounded up

  CodeGen::Label positive = cg.newLabel();
  CodeGen::Label convert = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label emit = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.subRegImm32(Rsp, static_cast<std::int32_t>(kBuffer));

  cg.movReg(R8, Rsp);
  cg.addImm32(R8, static_cast<std::int32_t>(kBuffer));  // cursor = end of buffer
  cg.movReg(Rax, Rdi);

  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, positive);
  cg.movImm(R9, 1);  // remember the sign
  cg.negReg(Rax);    // magnitude
  cg.jmp(convert);

  cg.bind(positive);
  cg.xorReg(R9, R9);

  cg.bind(convert);
  cg.movImm(Rcx, 10);
  cg.bind(loop);
  cg.divUnsigned(Rcx);  // rax = rax / 10, rdx = rax % 10 (rdx pre-zeroed)
  cg.addImm32(Rdx, static_cast<std::int32_t>('0'));
  cg.subRegImm32(R8, 1);
  cg.movByteToMem(R8, 0, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, loop);

  cg.testReg(R9);
  cg.jcc(Jcc::Eq, emit);
  cg.subRegImm32(R8, 1);
  cg.movImmByteToMem(R8, 0, static_cast<std::uint8_t>('-'));

  cg.bind(emit);
  cg.movReg(Rsi, Rsp);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kBuffer));
  cg.subReg(Rsi, R8);  // length = end - cursor
  cg.movReg(Rdi, R8);
  inlineWriteFd(cg, writeLabel, R10);  // fd is in r10

  epilogue(cg);
}

// `AotValue oskel_arith(AotValue a, AotValue b, long op)`: `+`, `-`, `*`, `/`
// and `%` with the int/float/double promotion of `ops::binary`. int/int uses
// 64-bit wrapping arithmetic; anything with a float promotes to double, and a
// float/float result rounds back to binary32 so `0.1f + 0.2f == 0.3f` holds,
// exactly like the hosted engine. `Add` on two strings concatenates (7c.1c);
// `Add` on two arrays concatenates (7c.3b); `%` on floats uses x87 `fprem`
// (7c.1d.1).
void emitArith(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
               const DataBlob& msgTypes, const DataBlob& separator,
               const std::vector<const DataBlob*>& opMessages,
               const DataBlob& msgDivZero, CodeGen::Label concatLabel,
               CodeGen::Label arrayConcatLabel) {
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label strConcat = cg.newLabel();
  CodeGen::Label arrConcat = cg.newLabel();
  CodeGen::Label concatEnd = cg.newLabel();
  CodeGen::Label intPath = cg.newLabel();
  CodeGen::Label floatPath = cg.newLabel();
  CodeGen::Label arithNumbers = cg.newLabel();
  CodeGen::Label intDone = cg.newLabel();
  CodeGen::Label intAdd = cg.newLabel();
  CodeGen::Label intSub = cg.newLabel();
  CodeGen::Label intMul = cg.newLabel();
  CodeGen::Label intDiv = cg.newLabel();
  CodeGen::Label intMod = cg.newLabel();
  CodeGen::Label intDivCommon = cg.newLabel();
  CodeGen::Label intDivZero = cg.newLabel();
  CodeGen::Label intNormalDiv = cg.newLabel();
  CodeGen::Label intSpecial = cg.newLabel();
  CodeGen::Label intSpecialMod = cg.newLabel();
  CodeGen::Label floatAdd = cg.newLabel();
  CodeGen::Label floatSub = cg.newLabel();
  CodeGen::Label floatMul = cg.newLabel();
  CodeGen::Label floatDiv = cg.newLabel();
  CodeGen::Label floatMod = cg.newLabel();
  CodeGen::Label floatModGo = cg.newLabel();
  CodeGen::Label floatModLoop = cg.newLabel();
  CodeGen::Label floatDivZero = cg.newLabel();
  CodeGen::Label floatNotZero = cg.newLabel();
  CodeGen::Label floatDone = cg.newLabel();
  CodeGen::Label useFloat = cg.newLabel();
  CodeGen::Label useDouble = cg.newLabel();

  cg.bind(label);
  prologue(cg);

  // Both operands numbers -> numeric arithmetic (below). Both strings with `+`
  // -> string concatenation; both arrays with `+` -> array concatenation.
  // Anything else is an unsupported operand pair.
  emitIsNumberFlag(cg, Rdi);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, arithNumbers);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Add));
  cg.jcc(Jcc::Ne, bad);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, strConcat);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, arrConcat);
  cg.jmp(bad);

  cg.bind(strConcat);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, bad);
  cg.callLabel(concatLabel);  // returns an AotValue in rax:rdx
  cg.jmp(concatEnd);

  cg.bind(arrConcat);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Ne, bad);
  cg.callLabel(arrayConcatLabel);  // returns an AotValue in rax:rdx
  cg.bind(concatEnd);
  epilogue(cg);

  cg.bind(arithNumbers);
  emitIsNumberFlag(cg, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, bad);

  // int/int takes the wrapping integer path; everything else promotes.
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, floatPath);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, floatPath);
  cg.jmp(intPath);

  // --- integer path ---
  cg.bind(intPath);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Add));
  cg.jcc(Jcc::Eq, intAdd);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Sub));
  cg.jcc(Jcc::Eq, intSub);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Mul));
  cg.jcc(Jcc::Eq, intMul);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Div));
  cg.jcc(Jcc::Eq, intDiv);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Mod));
  cg.jcc(Jcc::Eq, intMod);
  cg.jmp(bad);

  cg.bind(intAdd);
  cg.addReg(Rsi, Rcx);
  cg.movReg(Rax, Rsi);
  cg.jmp(intDone);
  cg.bind(intSub);
  cg.subReg(Rsi, Rcx);
  cg.movReg(Rax, Rsi);
  cg.jmp(intDone);
  cg.bind(intMul);
  cg.imulReg(Rsi, Rcx);
  cg.movReg(Rax, Rsi);
  cg.jmp(intDone);

  cg.bind(intDiv);
  cg.movImm(R9, 1);  // want the quotient
  cg.jmp(intDivCommon);
  cg.bind(intMod);
  cg.xorReg(R9, R9);  // want the remainder

  cg.bind(intDivCommon);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, intDivZero);
  // INT64_MIN / -1 overflows in `idiv`; the hosted engine returns `a` for `/`
  // and 0 for `%`, so the runtime special-cases it instead of faulting.
  cg.movImm(R10, kInt64Min);
  cg.cmpReg(Rsi, R10);
  cg.jcc(Jcc::Ne, intNormalDiv);
  cg.cmpImm32(Rcx, -1);
  cg.jcc(Jcc::Ne, intNormalDiv);
  cg.jmp(intSpecial);
  cg.bind(intSpecial);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, intSpecialMod);
  cg.movReg(Rax, Rsi);  // quotient = a (INT64_MIN)
  cg.jmp(intDone);
  cg.bind(intSpecialMod);
  cg.xorReg(Rax, Rax);  // remainder = 0
  cg.jmp(intDone);
  cg.bind(intNormalDiv);
  cg.movReg(Rax, Rsi);
  cg.divReg(Rcx);  // rax = quotient, rdx = remainder
  cg.testReg(R9);
  cg.jcc(Jcc::Ne, intDone);
  cg.movReg(Rax, Rdx);
  cg.jmp(intDone);

  cg.bind(intDone);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);

  cg.bind(intDivZero);
  emitFailCall(cg, fail, msgDivZero);

  // --- float/double path ---
  cg.bind(floatPath);
  emitToDouble(cg, Rdi, Rsi, Xmm0);  // xmm0 = a
  emitToDouble(cg, Rdx, Rcx, Xmm1);  // xmm1 = b
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Add));
  cg.jcc(Jcc::Eq, floatAdd);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Sub));
  cg.jcc(Jcc::Eq, floatSub);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Mul));
  cg.jcc(Jcc::Eq, floatMul);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Div));
  cg.jcc(Jcc::Eq, floatDiv);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Mod));
  cg.jcc(Jcc::Eq, floatMod);
  cg.jmp(bad);  // float `**` needs a correctly rounded pow (7c.1d.2)

  cg.bind(floatAdd);
  cg.addsd(Xmm0, Xmm1);
  cg.jmp(floatDone);
  cg.bind(floatSub);
  cg.subsd(Xmm0, Xmm1);
  cg.jmp(floatDone);
  cg.bind(floatMul);
  cg.mulsd(Xmm0, Xmm1);
  cg.jmp(floatDone);

  cg.bind(floatDiv);
  cg.xorps(Xmm2, Xmm2);  // xmm2 = 0.0
  cg.ucomisd(Xmm1, Xmm2);
  cg.jcc(Jcc::P, floatNotZero);  // unordered (NaN) is not zero
  cg.jcc(Jcc::Eq, floatDivZero);
  cg.bind(floatNotZero);
  cg.divsd(Xmm0, Xmm1);
  cg.jmp(floatDone);
  cg.bind(floatDivZero);
  emitFailCall(cg, fail, msgDivZero);

  // float/double `%`: the truncated remainder, matching `std::fmod` (the sign
  // follows the dividend). SSE has no remainder instruction, so the operands
  // cross to the x87 unit through the stack. `fprem` produces a partial
  // remainder and sets C2 while the result is not yet complete; the loop runs
  // until C2 is clear. Dividing by zero is the same error as `/` (the hosted
  // engine checks `right.number() == 0` before `fmod`). The x87 stack is
  // drained on every path, so the rest of the backend keeps seeing a clean FPU.
  cg.bind(floatMod);
  cg.xorps(Xmm2, Xmm2);  // xmm2 = 0.0
  cg.ucomisd(Xmm1, Xmm2);
  cg.jcc(Jcc::P, floatModGo);  // unordered (NaN) divisor is not zero
  cg.jcc(Jcc::Eq, floatDivZero);
  cg.bind(floatModGo);
  cg.subRegImm32(Rsp, 16);         // scratch for st(1), kept 16-byte aligned
  cg.movsdFromXmm(Rsp, 0, Xmm0);   // [rsp] = a
  cg.movsdFromXmm(Rsp, 8, Xmm1);   // [rsp+8] = b
  cg.fldM64(Rsp, 8);               // st0 = b
  cg.fldM64(Rsp, 0);               // st0 = a, st1 = b
  cg.bind(floatModLoop);
  cg.fprem();                      // st0 = partial remainder
  cg.fnstswAx();
  cg.testAhImm8(0x04);             // C2 -> incomplete, keep reducing
  cg.jcc(Jcc::Ne, floatModLoop);
  cg.fstpM64(Rsp, 0);              // [rsp] = result, pop st0
  cg.fstpSt0();                    // pop st1 (the divisor)
  cg.movsdToXmm(Xmm0, Rsp, 0);
  cg.addImm32(Rsp, 16);
  cg.jmp(floatDone);

  cg.bind(floatDone);
  // The result is a double unless neither operand is one, in which case it
  // rounds back to binary32.
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, useDouble);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, useDouble);
  cg.jmp(useFloat);
  cg.bind(useFloat);
  cg.cvtsd2ss(Xmm0, Xmm0);
  cg.cvtss2sd(Xmm0, Xmm0);  // store the binary32 value widened, as the hosted Value does
  cg.movqFromXmm(Rdx, Xmm0);
  cg.movImm(Rax, kTagFloat);
  epilogue(cg);
  cg.bind(useDouble);
  cg.movqFromXmm(Rdx, Xmm0);
  cg.movImm(Rax, kTagDouble);
  epilogue(cg);

  cg.bind(bad);
  // The VM names the operator, so the selector picks the prefix; the operands
  // are untouched here (the number-flag test only writes rax).
  {
    // The selectors in declaration order, so the message list stays parallel to
    // `AotArithOp` rather than to its numeric values.
    constexpr AotArithOp kOps[] = {AotArithOp::Add, AotArithOp::Sub, AotArithOp::Mul,
                                   AotArithOp::Div, AotArithOp::Mod};
    constexpr std::size_t kOpCount = sizeof kOps / sizeof kOps[0];
    static_assert(kOpCount == 5);
    std::vector<CodeGen::Label> named;
    for (std::size_t i = 0; i < kOpCount && i < opMessages.size(); ++i) {
      named.push_back(cg.newLabel());
      cg.cmpImm32(R8, static_cast<std::int32_t>(kOps[i]));
      cg.jcc(Jcc::Eq, named.back());
    }
    emitFailCall(cg, fail, msgTypes);  // an unknown selector keeps the bare text
    for (std::size_t i = 0; i < named.size(); ++i) {
      cg.bind(named[i]);
      emitTwoTypeFail(cg, *opMessages[i], separator, Rdi, Rsi, Rdx, Rcx);
    }
  }
}

// `AotValue oskel_pow(AotValue a, AotValue b)`: `**` with the int/float/double
// promotion of `ops::binary`. int/int with a non-negative exponent uses the
// wrapping integer loop (`intPow`); every other numeric pair (a float/double
// operand, or a negative exponent) converts to binary64, calls the embedded
// musl `pow` and re-tags the result, so a float/float result rounds back to
// binary32 exactly like the hosted `makeNumeric`. A non-number operand is the
// unsupported-operand error.
void emitPow(CodeGen& cg, CodeGen::Label label, CodeGen::Label /*fail*/, const DataBlob& msgPow,
             const DataBlob& separator, CodeGen::Label mathPow) {
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label floatPath = cg.newLabel();
  CodeGen::Label floatFloat = cg.newLabel();
  CodeGen::Label floatDouble = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label skipMul = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  // Both operands must be numbers; anything else is the unsupported pair.
  emitIsNumberFlag(cg, Rdi);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, bad);
  emitIsNumberFlag(cg, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, bad);
  // int/int with a non-negative exponent stays integer; every other numeric
  // pair goes through `pow`.
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, floatPath);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, floatPath);
  cg.testReg(Rcx);
  cg.jcc(Jcc::S, floatPath);  // negative exponent -> pow (7c.1c/7c.1d.2)

  cg.movReg(R8, Rcx);  // exp = b
  cg.movReg(R9, Rsi);  // factor = a
  cg.movImm(Rax, 1);   // result = 1
  cg.movImm(Rcx, 1);   // cl = 1, the shift amount for the loop
  cg.bind(loop);
  cg.testImm32(R8, 1);
  cg.jcc(Jcc::Eq, skipMul);
  cg.imulReg(Rax, R9);
  cg.bind(skipMul);
  cg.imulReg(R9, R9);
  cg.shrCl(R8);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, loop);

  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);

  // --- float/double path ---
  // `mathPow` clobbers the volatile registers (including the operand tags in
  // rdi/rdx), so the result type is decided *before* the call and each branch
  // re-does the conversion and the call. The type follows `makeNumeric`'s rank
  // rule: binary32 only when at least one operand is a float and none is a
  // double; int/int (a negative exponent) and any double stay binary64.
  cg.bind(floatPath);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, floatDouble);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, floatDouble);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, floatFloat);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Ne, floatDouble);  // int/int (negative exponent) -> double
  cg.bind(floatFloat);
  emitToDouble(cg, Rdi, Rsi, kXmmBase + 0);  // xmm0 = a
  emitToDouble(cg, Rdx, Rcx, kXmmBase + 1);  // xmm1 = b
  cg.callLabel(mathPow);                     // xmm0 = pow(a, b)
  cg.cvtsd2ss(kXmmBase + 0, kXmmBase + 0);
  cg.cvtss2sd(kXmmBase + 0, kXmmBase + 0);  // store the binary32 value widened
  cg.movqFromXmm(Rdx, kXmmBase + 0);
  cg.movImm(Rax, kTagFloat);
  epilogue(cg);
  cg.bind(floatDouble);
  emitToDouble(cg, Rdi, Rsi, kXmmBase + 0);  // xmm0 = a
  emitToDouble(cg, Rdx, Rcx, kXmmBase + 1);  // xmm1 = b
  cg.callLabel(mathPow);                     // xmm0 = pow(a, b)
  cg.movqFromXmm(Rdx, kXmmBase + 0);
  cg.movImm(Rax, kTagDouble);
  epilogue(cg);

  cg.bind(bad);
  emitTwoTypeFail(cg, msgPow, separator, Rdi, Rsi, Rdx, Rcx);
}

// `AotValue oskel_shift(AotValue a, AotValue b, long op)`: integer `<<`/`>>`.
// The count must be an int in 0..63 (the hosted range check); `>>` is
// arithmetic, so it preserves the sign.
void emitShift(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
               const DataBlob& msgShift, const DataBlob& msgLeft,
               const DataBlob& msgRight, const DataBlob& separator) {
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label badOperand = cg.newLabel();
  CodeGen::Label doShl = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  // A non-int operand is the VM's operand message, which names the direction;
  // only an out-of-range integer count reports the count range.
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badOperand);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badOperand);
  cg.testReg(Rcx);
  cg.jcc(Jcc::S, bad);  // negative count
  cg.cmpImm32(Rcx, 63);
  cg.jcc(Jcc::Gt, bad);
  cg.movReg(Rax, Rsi);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotShiftOp::Shl));
  cg.jcc(Jcc::Eq, doShl);
  cg.sarCl(Rax);
  cg.jmp(done);
  cg.bind(doShl);
  cg.shlCl(Rax);
  cg.bind(done);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);

  cg.bind(bad);
  emitFailCall(cg, fail, msgShift);
  cg.bind(badOperand);
  {
    CodeGen::Label leftName = cg.newLabel();
    cg.cmpImm32(R8, static_cast<std::int32_t>(AotShiftOp::Shl));
    cg.jcc(Jcc::Eq, leftName);
    emitTwoTypeFail(cg, msgRight, separator, Rdi, Rsi, Rdx, Rcx);
    cg.bind(leftName);
    emitTwoTypeFail(cg, msgLeft, separator, Rdi, Rsi, Rdx, Rcx);
  }
}

// `AotValue oskel_bitwise(AotValue a, AotValue b, long op)`: int-only `&`, `|`
// and `^`, selected by `op` in r8.
void emitBitwise(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                 const DataBlob& message, const DataBlob& msgAnd, const DataBlob& msgOr,
                 const DataBlob& msgXor, const DataBlob& separator) {
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label isAnd = cg.newLabel();
  CodeGen::Label isOr = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotBitwiseOp::And));
  cg.jcc(Jcc::Eq, isAnd);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotBitwiseOp::Or));
  cg.jcc(Jcc::Eq, isOr);
  cg.xorReg(Rsi, Rcx);  // Xor is the fall-through case
  cg.jmp(done);
  cg.bind(isAnd);
  cg.andReg(Rsi, Rcx);
  cg.jmp(done);
  cg.bind(isOr);
  cg.orReg(Rsi, Rcx);
  cg.bind(done);
  cg.movImm(Rax, kTagInt);
  cg.movReg(Rdx, Rsi);
  epilogue(cg);
  cg.bind(bad);
  {
    CodeGen::Label andName = cg.newLabel();
    CodeGen::Label orName = cg.newLabel();
    CodeGen::Label xorName = cg.newLabel();
    cg.cmpImm32(R8, static_cast<std::int32_t>(AotBitwiseOp::And));
    cg.jcc(Jcc::Eq, andName);
    cg.cmpImm32(R8, static_cast<std::int32_t>(AotBitwiseOp::Or));
    cg.jcc(Jcc::Eq, orName);
    cg.cmpImm32(R8, static_cast<std::int32_t>(AotBitwiseOp::Xor));
    cg.jcc(Jcc::Eq, xorName);
    emitFailCall(cg, fail, message);  // an unknown selector keeps the bare text
    cg.bind(andName);
    emitTwoTypeFail(cg, msgAnd, separator, Rdi, Rsi, Rdx, Rcx);
    cg.bind(orName);
    emitTwoTypeFail(cg, msgOr, separator, Rdi, Rsi, Rdx, Rcx);
    cg.bind(xorName);
    emitTwoTypeFail(cg, msgXor, separator, Rdi, Rsi, Rdx, Rcx);
  }
}

// `AotValue oskel_negate(AotValue a)`: arithmetic negation of an int (wrapping)
// or a float/double (sign-bit flip, exact for every value including NaN).
void emitNegate(CodeGen& cg, CodeGen::Label label, const DataBlob& message) {
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asFloat = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, asInt);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, asFloat);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, asFloat);
  cg.jmp(bad);

  cg.bind(asInt);
  cg.movReg(Rdx, Rsi);
  cg.negReg(Rdx);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);

  cg.bind(asFloat);
  cg.movReg(R10, Rsi);
  cg.movImm(R11, kDoubleSignBit);
  cg.xorReg(R10, R11);  // flip the sign bit
  cg.movReg(Rdx, R10);
  cg.movReg(Rax, Rdi);  // the tag is unchanged (Float or Double)
  epilogue(cg);

  cg.bind(bad);
  emitTypeQuoteFail(cg, message, Rdi, Rsi);
}

// `AotValue oskel_bit_not(AotValue a)`: int-only `~`.
void emitBitNot(CodeGen& cg, CodeGen::Label label, const DataBlob& message) {
  CodeGen::Label bad = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.movReg(Rdx, Rsi);
  cg.notReg(Rdx);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);
  cg.bind(bad);
  emitTypeQuoteFail(cg, message, Rdi, Rsi);
}

// `AotValue oskel_not(AotValue a)`: logical not of the value's truthiness,
// always a bool.
void emitNot(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label isBool = cg.newLabel();
  CodeGen::Label isNil = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Eq, isBool);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, isNil);
  cg.xorReg(Rdx, Rdx);  // any other value is truthy, so not(x) is false
  cg.jmp(done);
  cg.bind(isBool);
  cg.xorReg(Rdx, Rdx);
  cg.testReg(Rsi);
  cg.setcc(Jcc::Eq, Rdx);  // payload == 0 (false) -> not is true
  cg.movzx(Rdx, Rdx);
  cg.jmp(done);
  cg.bind(isNil);
  cg.movImm(Rdx, 1);
  cg.bind(done);
  cg.movImm(Rax, kTagBool);
  epilogue(cg);
}

// `AotValue oskel_compare(AotValue a, AotValue b, long op)`: equality for any
// pair and ordering for int/int, float/double and bool/bool. Equality of two
// numbers is numeric (`1 == 1.0` is true), matching `Value::equals`; equality of
// two arrays or maps is *structural* (same length and equal items/entries), the
// way the hosted engine compares them, not pointer identity. The result is
// always a bool. The op selector is the low nibble of the x86-64 condition code
// of the matching comparison. `mapFind` is needed by the map branch of the
// structural equality.
void emitCompare(CodeGen& cg, CodeGen::Label label, const DataBlob& message,
                 CodeGen::Label mapFind) {
  CodeGen::Label eqBranch = cg.newLabel();
  CodeGen::Label neBranch = cg.newLabel();
  CodeGen::Label order = cg.newLabel();
  CodeGen::Label boolResult = cg.newLabel();
  CodeGen::Label orderInt = cg.newLabel();
  CodeGen::Label orderMixed = cg.newLabel();
  CodeGen::Label orderNonNumeric = cg.newLabel();
  CodeGen::Label orderBool = cg.newLabel();
  CodeGen::Label orderSelect = cg.newLabel();
  CodeGen::Label selLt = cg.newLabel();
  CodeGen::Label selGt = cg.newLabel();
  CodeGen::Label selLe = cg.newLabel();
  CodeGen::Label selGe = cg.newLabel();
  CodeGen::Label orderFinish = cg.newLabel();
  CodeGen::Label badCompare = cg.newLabel();
  CodeGen::Label abDone = cg.newLabel();
  CodeGen::Label baDone = cg.newLabel();
  CodeGen::Label abDoneBool = cg.newLabel();
  CodeGen::Label baDoneBool = cg.newLabel();
  CodeGen::Label eqHelper = cg.newLabel();
  CodeGen::Label eqReturn = cg.newLabel();
  CodeGen::Label eqRetTrue = cg.newLabel();
  CodeGen::Label eqRetFalse = cg.newLabel();
  CodeGen::Label eqNumFloat = cg.newLabel();
  CodeGen::Label eqNonNumeric = cg.newLabel();
  CodeGen::Label eqIdentity = cg.newLabel();
  CodeGen::Label eqStrPair = cg.newLabel();
  CodeGen::Label eqRange = cg.newLabel();
  CodeGen::Label eqError = cg.newLabel();
  CodeGen::Label eqArray = cg.newLabel();
  CodeGen::Label eqMap = cg.newLabel();
  CodeGen::Label eqMapClassB = cg.newLabel();
  CodeGen::Label eqArrayLoop = cg.newLabel();
  CodeGen::Label eqMapLoop = cg.newLabel();
  CodeGen::Label eqArrayScan = cg.newLabel();
  CodeGen::Label eqArrayScanNext = cg.newLabel();
  CodeGen::Label eqArrayScanDone = cg.newLabel();
  CodeGen::Label eqMapScan = cg.newLabel();
  CodeGen::Label eqMapScanNext = cg.newLabel();
  CodeGen::Label eqMapScanDone = cg.newLabel();
  CodeGen::Label strEquals = cg.newLabel();
  CodeGen::Label strEqualsLoop = cg.newLabel();
  CodeGen::Label strEqualsSame = cg.newLabel();
  CodeGen::Label strEqualsDone = cg.newLabel();
  CodeGen::Label strOrder = cg.newLabel();
  CodeGen::Label strOrderLoop = cg.newLabel();
  CodeGen::Label strOrderByteDiffers = cg.newLabel();
  CodeGen::Label strOrderAExhausted = cg.newLabel();
  CodeGen::Label strOrderBExhausted = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Eq));
  cg.jcc(Jcc::Eq, eqBranch);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Ne));
  cg.jcc(Jcc::Eq, neBranch);
  cg.jmp(order);

  cg.bind(eqBranch);
  cg.xorReg(R8, R8);  // depth 0
  cg.callLabel(eqHelper);
  cg.jmp(boolResult);
  cg.bind(neBranch);
  cg.xorReg(R8, R8);  // depth 0
  cg.callLabel(eqHelper);
  cg.testReg(Rax);
  cg.setcc(Jcc::Eq, Rax);
  cg.movzx(Rax, Rax);
  cg.jmp(boolResult);
  cg.bind(boolResult);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagBool);
  epilogue(cg);

  // --- ordering ---
  cg.bind(order);
  emitIsNumberFlag(cg, Rdi);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, orderNonNumeric);
  emitIsNumberFlag(cg, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, orderNonNumeric);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, orderMixed);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, orderMixed);
  cg.jmp(orderInt);

  cg.bind(orderInt);
  cg.xorReg(Rax, Rax);
  cg.cmpReg(Rsi, Rcx);
  cg.setcc(Jcc::Lt, Rax);  // ab = a < b
  cg.movzx(Rax, Rax);
  cg.xorReg(R11, R11);
  cg.cmpReg(Rcx, Rsi);
  cg.setcc(Jcc::Lt, R11);  // ba = b < a
  cg.movzx(R11, R11);
  cg.jmp(orderSelect);

  cg.bind(orderMixed);
  emitToDouble(cg, Rdi, Rsi, Xmm0);
  emitToDouble(cg, Rdx, Rcx, Xmm1);
  cg.xorReg(Rax, Rax);
  cg.ucomisd(Xmm0, Xmm1);
  cg.jcc(Jcc::P, abDone);   // unordered -> ab = 0
  cg.jcc(Jcc::Ae, abDone);  // CF=0 -> not less
  cg.movImm(Rax, 1);
  cg.bind(abDone);
  cg.xorReg(R11, R11);
  cg.ucomisd(1, 0);
  cg.jcc(Jcc::P, baDone);
  cg.jcc(Jcc::Ae, baDone);
  cg.movImm(R11, 1);
  cg.bind(baDone);
  cg.jmp(orderSelect);

  // --- non-numeric ordering: strings compare lexicographically, bools by
  // value; any other pair cannot be ordered. ---
  cg.bind(orderNonNumeric);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, orderBool);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badCompare);
  cg.callLabel(strOrder);
  cg.jmp(orderSelect);

  cg.bind(orderBool);
  cg.cmpReg(Rdi, Rdx);
  cg.jcc(Jcc::Ne, badCompare);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Ne, badCompare);
  cg.xorReg(Rax, Rax);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, abDoneBool);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, abDoneBool);
  cg.movImm(Rax, 1);  // ab = !a && b
  cg.bind(abDoneBool);
  cg.xorReg(R11, R11);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Ne, baDoneBool);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, baDoneBool);
  cg.movImm(R11, 1);  // ba = !b && a
  cg.bind(baDoneBool);
  cg.jmp(orderSelect);

  cg.bind(orderSelect);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Lt));
  cg.jcc(Jcc::Eq, selLt);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Gt));
  cg.jcc(Jcc::Eq, selGt);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Le));
  cg.jcc(Jcc::Eq, selLe);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Ge));
  cg.jcc(Jcc::Eq, selGe);
  cg.jmp(badCompare);
  cg.bind(selLt);
  cg.jmp(orderFinish);  // rax already holds ab
  cg.bind(selGt);
  cg.movReg(Rax, R11);
  cg.jmp(orderFinish);
  cg.bind(selLe);  // !ba
  cg.testReg(R11);
  cg.setcc(Jcc::Eq, Rax);
  cg.movzx(Rax, Rax);
  cg.jmp(orderFinish);
  cg.bind(selGe);  // !ab
  cg.testReg(Rax);
  cg.setcc(Jcc::Eq, Rax);
  cg.movzx(Rax, Rax);
  cg.bind(orderFinish);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagBool);
  epilogue(cg);

  cg.bind(badCompare);
  // The VM lowers `a > b` to `lessThan(b, a)` and `a <= b` to `!lessThan(b, a)`,
  // so its message quotes those two operators' operands reversed. Match it:
  // `error.message` is observable.
  CodeGen::Label badReversed = cg.newLabel();
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Gt));
  cg.jcc(Jcc::Eq, badReversed);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Le));
  cg.jcc(Jcc::Eq, badReversed);
  emitTwoTypeFail(cg, message, *gMsgText.textWith, Rdi, Rsi, Rdx, Rcx);
  cg.bind(badReversed);
  emitTwoTypeFail(cg, message, *gMsgText.textWith, Rdx, Rcx, Rdi, Rsi);

  // --- internal subroutine: rax = (a == b) ---
  // Not reached by fall-through: every branch above ends in `ret` or a call to
  // `fail` that never returns. Called with the operands in rdi:rsi/rdx:rcx and
  // the visited-pair list head in r8 (0 from the top-level `==`). It is
  // recursive: the array and map branches push `(a, b)` as a node on this
  // frame's stack and call back with that node as the list, so a pair already
  // under comparison is assumed equal -- the hosted `Value::equals` `visited`
  // rule, which makes a cyclic structure compare equal to an isomorphic one and
  // terminates instead of recursing forever.
  cg.bind(eqHelper);
  cg.push(Rbp);
  cg.movReg(Rbp, Rsp);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 80);       // locals at rbp-24..-96 (rbx/r12 at rbp-8/-16)
  cg.movToMem(Rbp, -48, R8);     // incoming visited list
  emitIsNumberFlag(cg, Rdi);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, eqNonNumeric);
  emitIsNumberFlag(cg, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, eqNonNumeric);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, eqNumFloat);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, eqNumFloat);
  cg.xorReg(Rax, Rax);
  cg.cmpReg(Rsi, Rcx);
  cg.setcc(Jcc::Eq, Rax);
  cg.movzx(Rax, Rax);
  cg.jmp(eqReturn);
  cg.bind(eqNumFloat);
  emitToDouble(cg, Rdi, Rsi, Xmm0);
  emitToDouble(cg, Rdx, Rcx, Xmm1);
  cg.xorReg(Rax, Rax);
  cg.ucomisd(Xmm0, Xmm1);
  cg.jcc(Jcc::P, eqReturn);   // unordered -> not equal
  cg.jcc(Jcc::Ne, eqReturn);  // ZF=0 -> not equal
  cg.movImm(Rax, 1);
  cg.jmp(eqReturn);

  cg.bind(eqNonNumeric);
  // Two values of different kinds are never equal (a number pair was handled
  // above). Same kind: strings by content, ranges by their three fields, errors
  // by message and code, arrays and maps structurally; everything else (nil,
  // bool, func, ptr) is identity.
  cg.cmpReg(Rdi, Rdx);
  cg.jcc(Jcc::Ne, eqRetFalse);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, eqStrPair);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, eqRange);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagError));
  cg.jcc(Jcc::Eq, eqError);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, eqArray);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, eqMap);
  cg.jmp(eqIdentity);

  cg.bind(eqStrPair);
  cg.callLabel(strEquals);
  cg.jmp(eqReturn);

  // A range is equal when start, end and the inclusive flag all match.
  cg.bind(eqRange);
  cg.movFromMem(R8, Rsi, kRangeStartOffset);
  cg.movFromMem(R9, Rcx, kRangeStartOffset);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ne, eqRetFalse);
  cg.movFromMem(R8, Rsi, kRangeEndOffset);
  cg.movFromMem(R9, Rcx, kRangeEndOffset);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ne, eqRetFalse);
  cg.movFromMem(R8, Rsi, kRangeInclusiveOffset);
  cg.movFromMem(R9, Rcx, kRangeInclusiveOffset);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ne, eqRetFalse);
  cg.jmp(eqRetTrue);

  // An error is equal when the message string and the code both match, the same
  // rule as the hosted `ErrorObj` (`message` and `code` compared by value).
  cg.bind(eqError);
  cg.movFromMem(R8, Rsi, kErrorCodeOffset);
  cg.movFromMem(R9, Rcx, kErrorCodeOffset);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ne, eqRetFalse);
  cg.movFromMem(R8, Rsi, kErrorMessageOffset);
  cg.movFromMem(R9, Rcx, kErrorMessageOffset);
  cg.movReg(Rsi, R8);  // message A
  cg.movReg(Rcx, R9);  // message B
  cg.callLabel(strEquals);
  cg.jmp(eqReturn);

  // --- arrays: equal length, then equal items, in order ---
  cg.bind(eqArray);
  cg.movFromMem(Rbx, Rsi, kTableLengthOffset);
  cg.movFromMem(R9, Rcx, kTableLengthOffset);
  cg.cmpReg(Rbx, R9);
  cg.jcc(Jcc::Ne, eqRetFalse);
  // A pair already under comparison is equal (the cycle case).
  cg.movFromMem(R10, Rbp, -48);
  cg.bind(eqArrayScan);
  cg.testReg(R10);
  cg.jcc(Jcc::Eq, eqArrayScanDone);
  cg.movFromMem(R11, R10, 0);
  cg.cmpReg(R11, Rsi);
  cg.jcc(Jcc::Ne, eqArrayScanNext);
  cg.movFromMem(R11, R10, 8);
  cg.cmpReg(R11, Rcx);
  cg.jcc(Jcc::Eq, eqRetTrue);
  cg.bind(eqArrayScanNext);
  cg.movFromMem(R10, R10, 16);
  cg.jmp(eqArrayScan);
  cg.bind(eqArrayScanDone);
  // Push (A, B) as a node in this frame (A at +0, B at +8, prev at +16);
  // children recurse with &node as the list head.
  cg.movToMem(Rbp, -80, Rsi);
  cg.movToMem(Rbp, -72, Rcx);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(Rbp, -64, R11);
  cg.movToMem(Rbp, -24, Rsi);    // array A header
  cg.movToMem(Rbp, -32, Rcx);    // array B header
  cg.movImmToMem(Rbp, -40, 0);   // index
  cg.bind(eqArrayLoop);
  cg.movFromMem(R9, Rbp, -40);
  cg.cmpReg(R9, Rbx);
  cg.jcc(Jcc::Ae, eqRetTrue);
  cg.movReg(R10, R9);
  cg.shlImm8(R10, 4);
  cg.movFromMem(R11, Rbp, -24);
  cg.movFromMem(R11, R11, kTableBlockOffset);
  cg.addReg(R10, R11);           // &A[i]
  cg.movReg(R11, R9);
  cg.shlImm8(R11, 4);
  cg.movFromMem(R12, Rbp, -32);
  cg.movFromMem(R12, R12, kTableBlockOffset);
  cg.addReg(R11, R12);           // &B[i]
  cg.movFromMem(Rdi, R10, 0);
  cg.movFromMem(Rsi, R10, 8);
  cg.movFromMem(Rdx, R11, 0);
  cg.movFromMem(Rcx, R11, 8);
  cg.leaMem(R8, Rbp, -80);       // visited = &node
  cg.callLabel(eqHelper);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, eqRetFalse);
  cg.movFromMem(R9, Rbp, -40);
  cg.addImm32(R9, 1);
  cg.movToMem(Rbp, -40, R9);
  cg.jmp(eqArrayLoop);

  // --- maps: equal length, then every key of A present in B with an equal
  // value (the same rule as the hosted `Value::equals`, which looks up each of
  // A's keys in B). ---
  cg.bind(eqMap);
  // A class instance compares by identity, never structurally. The class-name
  // islands are shared per class, so a pointer comparison is enough.
  cg.movFromMem(Rax, Rsi, kTableClassNameOffset);
  cg.movFromMem(Rdx, Rcx, kTableClassNameOffset);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, eqMapClassB);
  cg.testReg(Rdx);
  cg.jcc(Jcc::Eq, eqRetFalse);
  cg.cmpReg(Rax, Rdx);
  cg.jcc(Jcc::Ne, eqRetFalse);
  cg.cmpReg(Rsi, Rcx);
  cg.jcc(Jcc::Eq, eqRetTrue);
  cg.jmp(eqRetFalse);
  cg.bind(eqMapClassB);
  cg.testReg(Rdx);
  cg.jcc(Jcc::Ne, eqRetFalse);
  cg.movFromMem(Rbx, Rsi, kTableLengthOffset);
  cg.movFromMem(R9, Rcx, kTableLengthOffset);
  cg.cmpReg(Rbx, R9);
  cg.jcc(Jcc::Ne, eqRetFalse);
  cg.movFromMem(R10, Rbp, -48);
  cg.bind(eqMapScan);
  cg.testReg(R10);
  cg.jcc(Jcc::Eq, eqMapScanDone);
  cg.movFromMem(R11, R10, 0);
  cg.cmpReg(R11, Rsi);
  cg.jcc(Jcc::Ne, eqMapScanNext);
  cg.movFromMem(R11, R10, 8);
  cg.cmpReg(R11, Rcx);
  cg.jcc(Jcc::Eq, eqRetTrue);
  cg.bind(eqMapScanNext);
  cg.movFromMem(R10, R10, 16);
  cg.jmp(eqMapScan);
  cg.bind(eqMapScanDone);
  cg.movToMem(Rbp, -80, Rsi);
  cg.movToMem(Rbp, -72, Rcx);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(Rbp, -64, R11);
  cg.movToMem(Rbp, -24, Rsi);    // map A header
  cg.movToMem(Rbp, -32, Rcx);    // map B header
  cg.movImmToMem(Rbp, -40, 0);   // index
  cg.bind(eqMapLoop);
  cg.movFromMem(R9, Rbp, -40);
  cg.cmpReg(R9, Rbx);
  cg.jcc(Jcc::Ae, eqRetTrue);
  cg.movReg(R10, R9);
  cg.imulImm32(R10, R10, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R11, Rbp, -24);
  cg.movFromMem(R11, R11, kTableBlockOffset);
  cg.addReg(R10, R11);           // entry A[i]
  cg.movFromMem(R12, R10, kMapEntryKeyOffset);  // key A
  // Find the key in B; a missing key means the maps differ.
  cg.movFromMem(Rdi, Rbp, -32);
  cg.movReg(Rsi, R12);
  cg.callLabel(mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, eqRetFalse);
  // B's entry for the key: valueB = B.entries[index].value
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R11, Rbp, -32);
  cg.movFromMem(R11, R11, kTableBlockOffset);
  cg.addReg(Rax, R11);
  // Reload A's entry (the mapFind call clobbered the scratch registers).
  cg.movFromMem(R9, Rbp, -40);
  cg.imulImm32(R9, R9, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R11, Rbp, -24);
  cg.movFromMem(R11, R11, kTableBlockOffset);
  cg.addReg(R9, R11);            // entry A[i]
  cg.movFromMem(Rdi, R9, kMapEntryValueOffset);
  cg.movFromMem(Rsi, R9, kMapEntryValueOffset + 8);
  cg.movFromMem(Rdx, Rax, kMapEntryValueOffset);
  cg.movFromMem(Rcx, Rax, kMapEntryValueOffset + 8);
  cg.leaMem(R8, Rbp, -80);       // visited = &node
  cg.callLabel(eqHelper);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, eqRetFalse);
  cg.movFromMem(R9, Rbp, -40);
  cg.addImm32(R9, 1);
  cg.movToMem(Rbp, -40, R9);
  cg.jmp(eqMapLoop);

  // Everything else of the same kind (nil, bool, func, ptr) is identity: the
  // tags already matched, so equal payloads mean the same value.
  cg.bind(eqIdentity);
  cg.xorReg(Rax, Rax);
  cg.cmpReg(Rsi, Rcx);
  cg.jcc(Jcc::Ne, eqReturn);
  cg.movImm(Rax, 1);
  cg.jmp(eqReturn);

  cg.bind(eqRetTrue);
  cg.movImm(Rax, 1);
  cg.jmp(eqReturn);
  cg.bind(eqRetFalse);
  cg.xorReg(Rax, Rax);
  cg.bind(eqReturn);
  cg.addImm32(Rsp, 80);
  cg.pop(R12);
  cg.pop(Rbx);
  cg.pop(Rbp);
  cg.retInsn();

  // --- internal subroutine: rax = (a == b) for two strings ---
  // rsi/rcx hold the two `AotString` pointers. Equal lengths and equal bytes.
  cg.bind(strEquals);
  cg.movFromMem(R8, Rsi, 0);   // lenA
  cg.movFromMem(R9, Rcx, 0);   // lenB
  cg.xorReg(Rax, Rax);         // result = 0
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ne, strEqualsDone);
  cg.xorReg(R10, R10);         // index
  cg.bind(strEqualsLoop);
  cg.cmpReg(R10, R8);
  cg.jcc(Jcc::Ae, strEqualsSame);  // index >= len -> equal
  cg.movReg(R11, R10);
  cg.addReg(R11, Rsi);
  cg.movzxFromMem(R11, R11, kStringDataOffset);  // a byte
  cg.movReg(R9, R10);
  cg.addReg(R9, Rcx);
  cg.movzxFromMem(R9, R9, kStringDataOffset);    // b byte
  cg.cmpReg(R11, R9);
  cg.jcc(Jcc::Ne, strEqualsDone);  // a byte differs -> not equal
  cg.addImm32(R10, 1);
  cg.jmp(strEqualsLoop);
  cg.bind(strEqualsSame);
  cg.movImm(Rax, 1);
  cg.bind(strEqualsDone);
  cg.retInsn();

  // --- internal subroutine: rax = (a < b), r11 = (b < a) for two strings ---
  // Lexicographic over unsigned bytes; a proper prefix is smaller. rsi/rcx hold
  // the two `AotString` pointers. The selector in r8 must survive (orderSelect
  // reads it), so this uses only r9/rdi/rdx/r10/r11 as scratch.
  cg.bind(strOrder);
  cg.movFromMem(R9, Rsi, 0);   // lenA
  cg.movFromMem(Rdi, Rcx, 0);  // lenB
  cg.xorReg(R10, R10);         // index
  cg.bind(strOrderLoop);
  cg.cmpReg(R10, R9);
  cg.jcc(Jcc::Ae, strOrderAExhausted);  // index >= lenA
  cg.cmpReg(R10, Rdi);
  cg.jcc(Jcc::Ae, strOrderBExhausted);  // index >= lenB
  cg.movReg(Rdx, R10);
  cg.addReg(Rdx, Rcx);
  cg.movzxFromMem(Rdx, Rdx, kStringDataOffset);  // b byte
  cg.movReg(R11, R10);
  cg.addReg(R11, Rsi);
  cg.movzxFromMem(R11, R11, kStringDataOffset);  // a byte
  cg.cmpReg(R11, Rdx);
  cg.jcc(Jcc::Ne, strOrderByteDiffers);
  cg.addImm32(R10, 1);
  cg.jmp(strOrderLoop);
  cg.bind(strOrderByteDiffers);
  // A byte at `index` differs (the loop only reaches here on inequality), so
  // the bytes are ordered one way or the other. Set `ab` from the unsigned
  // comparison, then `ba = !ab`. `setcc` reads the flags, so nothing may
  // clobber them between the `cmp` and the `setcc`.
  cg.xorReg(Rax, Rax);
  cg.cmpReg(R11, Rdx);
  cg.setcc(Jcc::B, Rax);   // ab = aByte < bByte
  cg.movzx(Rax, Rax);
  cg.xorReg(R11, R11);
  cg.testReg(Rax);
  cg.setcc(Jcc::Eq, R11);  // ba = !ab (the bytes differ)
  cg.movzx(R11, R11);
  cg.retInsn();
  cg.bind(strOrderAExhausted);
  // a ran out first: a < b iff b has bytes left, i.e. lenA < lenB.
  cg.xorReg(Rax, Rax);
  cg.cmpReg(R9, Rdi);
  cg.setcc(Jcc::B, Rax);   // ab = lenA < lenB
  cg.movzx(Rax, Rax);
  cg.xorReg(R11, R11);
  cg.retInsn();
  cg.bind(strOrderBExhausted);
  // b ran out first (and a has bytes left): b < a.
  cg.xorReg(Rax, Rax);
  cg.movImm(R11, 1);
  cg.retInsn();
}

// `void* oskel_alloc(long size)`: bump allocation from the arena seeded by
// `oskel_init_arena`. The cursor and end live in r13/r14 (see `native/entry.h`);
// this helper reads and advances the cursor only. Returns an 8-byte-aligned
// pointer, or null when the current chunk is exhausted and `oskel_arena_grow`
// cannot map another one (mirroring `Arena::allocate`).
//
// Contract: it clobbers only rax (the result) and the r10/r11 scratch pair;
// every other register survives. That is *not* System V, but the whole emitted
// runtime is written against it -- `oskel_concat` reads the second operand out
// of rsi after the allocation, for one -- so the growth path has to keep the
// promise the leaf version made by accident. It saves rdi, rsi, rdx, rcx, r8
// and r9 around `oskel_arena_grow`; the cost is one slow path per chunk.
//
// Growth is invisible to the caller: the exhausted chunk is abandoned, the
// cursor/end pair is repointed at a fresh region and the request is retried
// from its base.
void emitAlloc(CodeGen& cg, CodeGen::Label label, CodeGen::Label growLabel) {
  CodeGen::Label retry = cg.newLabel();
  CodeGen::Label overflow = cg.newLabel();
  CodeGen::Label exhausted = cg.newLabel();

  cg.bind(label);
  cg.bind(retry);
  cg.movReg(Rax, kAotArenaCursor);
  cg.addImm32(Rax, 7);
  cg.movImm(R10, -8);
  cg.andReg(Rax, R10);      // rax = cursor aligned up to 8
  cg.movReg(R11, Rax);
  cg.addReg(R11, Rdi);      // r11 = new cursor
  cg.jcc(Jcc::B, overflow);  // wrapped: size was absurd
  cg.cmpReg(R11, kAotArenaEnd);
  cg.jcc(Jcc::A, exhausted);  // past the end of this chunk
  cg.movReg(kAotArenaCursor, R11);
  cg.retInsn();
  cg.bind(exhausted);
  cg.push(Rdi);
  cg.push(Rsi);
  cg.push(Rdx);
  cg.push(Rcx);
  cg.push(R8);
  cg.push(R9);
  cg.subRegImm32(Rsp, 8);   // six pushes leave rsp at 8 mod 16; this lands it on 0
  cg.callLabel(growLabel);  // rax = 1 when a new chunk is in place
  cg.addImm32(Rsp, 8);
  cg.pop(R9);
  cg.pop(R8);
  cg.pop(Rcx);
  cg.pop(Rdx);
  cg.pop(Rsi);
  cg.pop(Rdi);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, retry);
  cg.bind(overflow);
  cg.xorReg(Rax, Rax);
  cg.retInsn();
}

// `AotValue oskel_concat(AotValue a, AotValue b)`: allocate lenA+lenB+8 bytes,
// write the combined length and the two byte runs, and return a Str value
// (payload = the new `AotString`). `a` is rdi:rsi, `b` is rdx:rcx. rbx and r12
// are callee-saved, so they are preserved across the helper.
void emitConcat(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  cg.push(Rbp);
  cg.movReg(Rbp, Rsp);
  cg.push(Rbx);
  cg.push(R12);

  cg.movReg(Rbx, Rcx);            // rbx = b's AotString
  cg.movFromMem(R12, Rsi, 0);     // r12 = lenA
  cg.movFromMem(Rax, Rbx, 0);     // lenB
  cg.addReg(R12, Rax);            // r12 = lenA + lenB
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.callLabel(allocLabel);       // rax = result pointer (or null)
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, R12);       // store the combined length
  // Copy A's bytes: dst = ptr + 8, src = a's bytes, count = lenA.
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.movFromMem(Rcx, Rsi, 0);     // lenA
  cg.addImm32(Rsi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.repMovsb();
  // Copy B's bytes: dst = current rdi, src = b's bytes, count = lenB.
  cg.movReg(Rsi, Rbx);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.movFromMem(Rcx, Rbx, 0);     // lenB
  cg.repMovsb();
  // The result value: tag Str, payload = the pointer (still in rax).
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.pop(R12);
  cg.pop(Rbx);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `AotValue oskel_array_concat(AotValue a, AotValue b)`: the array twin of
// `oskel_concat`. Both operands are arrays (the caller, `oskel_arith`, checks
// the tags), so this allocates a fresh `AotArray` of `lenA + lenB` items,
// copies A's items then B's items into it and returns it as an `Array` value.
// The operands are read only, so `a + b` leaves both unchanged, matching the
// hosted `ops::binary` (which builds a new vector). `a` is rdi:rsi, `b` is
// rdx:rcx. rbx, r12 and r13 are callee-saved (r13 holds the arena cursor, so
// the `blockEnsure` call preserves it), and the items block is grown once to
// the exact item count.
void emitArrayConcat(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                     CodeGen::Label blockEnsure, CodeGen::Label fail,
                     const DataBlob& msgOom) {
  CodeGen::Label copyALoop = cg.newLabel();
  CodeGen::Label copyADone = cg.newLabel();
  CodeGen::Label copyBLoop = cg.newLabel();
  CodeGen::Label copyBDone = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movToMem(Rbp, -24, Rcx);  // b's AotArray
  cg.movToMem(Rbp, -32, Rsi);  // a's AotArray

  // The new header, zeroed (length/capacity/items all 0).
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotArrayHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);  // header
  cg.movImmToMem(Rbx, kTableLengthOffset, 0);
  cg.movImmToMem(Rbx, kTableCapacityOffset, 0);
  cg.movImmToMem(Rbx, kTableBlockOffset, 0);

  // r12 = lenA + lenB, the item count. Both operands were saved to the frame
  // before the call, so read them back rather than relying on the allocator
  // leaving rsi/rcx intact.
  cg.movFromMem(R12, Rbp, -32);               // a's AotArray
  cg.movFromMem(R12, R12, kTableLengthOffset);  // lenA
  cg.movFromMem(Rax, Rbp, -24);               // b's AotArray
  cg.movFromMem(Rax, Rax, kTableLengthOffset);  // lenB
  cg.addReg(R12, Rax);

  // Grow the items block to the exact count (no-op for two empty arrays).
  cg.testReg(R12);
  cg.jcc(Jcc::Eq, copyBDone);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.movImm(Rdx, kArrayItemSize);
  cg.callLabel(blockEnsure);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);

  // Copy A's items: block[i] = a.items[i], for i in 0..lenA.
  cg.movFromMem(R8, Rbp, -32);   // a's AotArray
  cg.movFromMem(R9, R8, kTableBlockOffset);   // src = a.items
  cg.movReg(R10, Rax);                        // dst = block
  cg.movFromMem(R11, R8, kTableLengthOffset); // lenA
  cg.xorReg(Rcx, Rcx);                        // i
  cg.bind(copyALoop);
  cg.cmpReg(Rcx, R11);
  cg.jcc(Jcc::Ae, copyADone);
  cg.movFromMem(Rdx, R9, 0);
  cg.movToMem(R10, 0, Rdx);
  cg.movFromMem(Rdx, R9, 8);
  cg.movToMem(R10, 8, Rdx);
  cg.addImm32(R9, static_cast<std::int32_t>(kArrayItemSize));
  cg.addImm32(R10, static_cast<std::int32_t>(kArrayItemSize));
  cg.addImm32(Rcx, 1);
  cg.jmp(copyALoop);
  cg.bind(copyADone);

  // Copy B's items, continuing at dst and i.
  cg.movFromMem(R8, Rbp, -24);   // b's AotArray
  cg.movFromMem(R9, R8, kTableBlockOffset);   // src = b.items
  cg.movFromMem(R11, R8, kTableLengthOffset); // lenB
  cg.xorReg(Rcx, Rcx);                        // i
  cg.bind(copyBLoop);
  cg.cmpReg(Rcx, R11);
  cg.jcc(Jcc::Ae, copyBDone);
  cg.movFromMem(Rdx, R9, 0);
  cg.movToMem(R10, 0, Rdx);
  cg.movFromMem(Rdx, R9, 8);
  cg.movToMem(R10, 8, Rdx);
  cg.addImm32(R9, static_cast<std::int32_t>(kArrayItemSize));
  cg.addImm32(R10, static_cast<std::int32_t>(kArrayItemSize));
  cg.addImm32(Rcx, 1);
  cg.jmp(copyBLoop);

  cg.bind(copyBDone);
  cg.movToMem(Rbx, kTableLengthOffset, R12);  // length = lenA + lenB
  cg.movReg(Rdx, Rbx);
  cg.movImm(Rax, kTagArray);

  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `AotValue oskel_bind_array_method(AotValue arr, long which)`: the value of a
// bound array method. `which` indexes `methods` (the order `oskel_member_get`
// assigns: push, pop, shift, unshift, insert, removeAt, contains, indexOf,
// clear). It mirrors the hosted `bindArray`: the method owns its receiver, so
// the receiver is stored in a 16-byte cell (shared with the method, exactly
// like a captured local) and the method is an ordinary `AotClosure` whose
// single upvalue is that cell. The returned `Func` reaches its body through
// the ordinary `oskel_call` path, so the bytecode and the compiler are
// unchanged. `arr` is rdi:rsi, the selector rdx; rbx/r12 are callee-saved and
// preserved across the helper calls.
void emitBindArrayMethod(CodeGen& cg, CodeGen::Label label, CodeGen::Label cellNew,
                         CodeGen::Label closure,
                         const std::vector<CodeGen::Label>& methods,
                         const std::vector<CodeGen::Label>& displays) {
  std::vector<CodeGen::Label> use(methods.size());
  for (CodeGen::Label& l : use) l = cg.newLabel();
  CodeGen::Label haveCode = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);              // scratch slot for the selector
  cg.movToMem(Rbp, -24, Rdx);           // which
  cg.callLabel(cellNew);                // cell holding the receiver (rdi:rsi)
  cg.movReg(R12, Rax);                  // r12 = cell (callee-saved)
  // Select the method body: `cmp rdx, i; je use[i]` for every entry, then the
  // last one as the fallthrough so no selector needs a final unconditional
  // jump. `oskel_member_get` only ever passes a valid selector.
  for (std::size_t i = 0; i + 1 < methods.size(); ++i) {
    cg.movFromMem(Rax, Rbp, -24);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(i));
    cg.jcc(Jcc::Eq, use[i]);
  }
  cg.leaRipLabel(Rdi, methods.back());
  cg.leaRipLabel(Rcx, displays.back());  // the text repr/str renders
  cg.jmp(haveCode);
  for (std::size_t i = 0; i + 1 < methods.size(); ++i) {
    cg.bind(use[i]);
    cg.leaRipLabel(Rdi, methods[i]);
    cg.leaRipLabel(Rcx, displays[i]);
    cg.jmp(haveCode);
  }
  cg.bind(haveCode);
  cg.movImm(Rsi, 0);                    // arity: the body checks its own argc
  cg.movImm(Rdx, 1);                    // one upvalue: the receiver's cell
  cg.callLabel(closure);                // rax:rdx = Func value, rdx = AotClosure*
  cg.movToMem(Rdx, static_cast<std::int64_t>(kAotClosureHeaderSize), R12);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `AotValue oskel_builtin_array_push(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.push(...)`. The receiver is the closure's single upvalue cell;
// every argument is appended in order and the receiver is returned, matching
// the hosted `bindArray` `push` (which returns the array, not a count). The
// window grows *down*, so `args[i]` sits at `args - 16*i`. Growth goes through
// `oskel_block_ensure`, so an append is amortised O(1).
void emitArrayPushMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);                  // args
  cg.movReg(R12, Rsi);                  // argc
  cg.movFromMem(Rax, Rdx, static_cast<std::int64_t>(kAotClosureHeaderSize));
  cg.movFromMem(Rax, Rax, 8);           // cell->payload = AotArray*
  cg.movToMem(Rbp, -24, Rax);           // array header
  cg.movImmToMem(Rbp, -32, 0);          // index
  cg.bind(loop);
  cg.movFromMem(R8, Rbp, -32);          // index
  cg.cmpReg(R8, R12);
  cg.jcc(Jcc::Ge, done);
  cg.movFromMem(R9, Rbp, -24);          // array
  cg.movFromMem(R10, R9, kTableLengthOffset);
  cg.movReg(Rdi, R9);
  cg.movReg(Rsi, R10);
  cg.addImm32(Rsi, 1);
  cg.movImm(Rdx, kArrayItemSize);
  cg.callLabel(env.blockEnsure);        // rax = items block
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(R9, Rbp, -24);          // array
  cg.movFromMem(R10, R9, kTableLengthOffset);
  cg.movReg(R11, R10);
  cg.shlImm8(R11, 4);                   // len * 16
  cg.addReg(R11, Rax);                  // &block[len]
  cg.movFromMem(Rcx, Rbp, -32);         // index
  cg.shlImm8(Rcx, 4);
  cg.movReg(Rdx, Rbx);
  cg.subReg(Rdx, Rcx);                  // &args[index]
  cg.movFromMem(Rcx, Rdx, 0);
  cg.movToMem(R11, 0, Rcx);
  cg.movFromMem(Rcx, Rdx, 8);
  cg.movToMem(R11, 8, Rcx);
  cg.movFromMem(R10, R9, kTableLengthOffset);
  cg.addImm32(R10, 1);
  cg.movToMem(R9, kTableLengthOffset, R10);
  cg.movFromMem(R8, Rbp, -32);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -32, R8);
  cg.jmp(loop);
  cg.bind(done);
  cg.movFromMem(Rdx, Rbp, -24);         // return the receiver array
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_builtin_array_pop(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.pop()`. It takes no argument, removes the last item and
// returns it, or returns nil when the array is empty -- the hosted `bindArray`
// `pop`. `self`'s single upvalue is the receiver's cell.
void emitArrayPopMethod(CodeGen& cg, CodeGen::Label label, [[maybe_unused]] const CollectionEnv& env,
                        const DataBlob& msgArgc) {
  CodeGen::Label empty = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdx, static_cast<std::int64_t>(kAotClosureHeaderSize));
  cg.movFromMem(R12, Rax, 8);           // cell->payload = AotArray*
  cg.movFromMem(Rbx, R12, kTableLengthOffset);
  cg.testReg(Rbx);
  cg.jcc(Jcc::Eq, empty);
  cg.subRegImm32(Rbx, 1);
  cg.movToMem(R12, kTableLengthOffset, Rbx);
  cg.movReg(Rax, Rbx);
  cg.shlImm8(Rax, 4);                   // (len - 1) * 16
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.movFromMem(Rax, R10, 0);
  cg.movFromMem(Rdx, R10, 8);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(empty);
  cg.xorReg(Rax, Rax);                  // nil
  cg.xorReg(Rdx, Rdx);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// Loads the bound method's receiver array into `dst`: `self` (rdx) owns one
// upvalue cell (`oskel_cell_new`) whose payload is the `AotArray*`. Clobbers
// rax.
void emitLoadArrayReceiver(CodeGen& cg, X64Reg dst, X64Reg selfReg) {
  cg.movFromMem(Rax, selfReg, static_cast<std::int64_t>(kAotClosureHeaderSize));
  cg.movFromMem(dst, Rax, 8);
}

// The map twin of `emitLoadArrayReceiver` (PHASE 7c.3g): the bound method's
// receiver `AotMap*` lives in `self`'s single upvalue cell. Clobbers rax.
void emitLoadMapReceiver(CodeGen& cg, X64Reg dst, X64Reg selfReg) {
  cg.movFromMem(Rax, selfReg, static_cast<std::int64_t>(kAotClosureHeaderSize));
  cg.movFromMem(dst, Rax, 8);
}

// `AotValue oskel_builtin_array_shift(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.shift()`. It takes no argument, removes the first item and
// returns it, or returns nil when the array is empty -- the hosted `bindArray`
// `shift`. The tail items move down one slot with a single `rep movsb` (the
// destination is below the source, so a forward copy never overwrites an
// unread item).
void emitArrayShiftMethod(CodeGen& cg, CodeGen::Label label, [[maybe_unused]] const CollectionEnv& env,
                          const DataBlob& msgArgc) {
  CodeGen::Label empty = cg.newLabel();
  CodeGen::Label noMove = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movFromMem(R12, Rbx, kTableLengthOffset);
  cg.testReg(R12);
  cg.jcc(Jcc::Eq, empty);
  cg.movFromMem(R10, Rbx, kTableBlockOffset);  // items block
  cg.movFromMem(Rax, R10, 0);
  cg.movToMem(Rbp, -24, Rax);                  // front tag
  cg.movFromMem(Rax, R10, 8);
  cg.movToMem(Rbp, -32, Rax);                  // front payload
  // memmove: block[0..len-2] = block[1..len-1], (len - 1) * 16 bytes.
  cg.movReg(R11, R12);
  cg.subRegImm32(R11, 1);
  cg.testReg(R11);
  cg.jcc(Jcc::Eq, noMove);
  cg.shlImm8(R11, 4);
  cg.movReg(Rdi, R10);
  cg.movReg(Rsi, R10);
  cg.addImm32(Rsi, 16);
  cg.movReg(Rcx, R11);
  cg.repMovsb();
  cg.bind(noMove);
  cg.subRegImm32(R12, 1);
  cg.movToMem(Rbx, kTableLengthOffset, R12);
  cg.movFromMem(Rax, Rbp, -24);
  cg.movFromMem(Rdx, Rbp, -32);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(empty);
  cg.xorReg(Rax, Rax);                          // nil
  cg.xorReg(Rdx, Rdx);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_array_unshift(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.unshift(x)`. It inserts `x` at the front and returns the
// receiver -- the hosted `bindArray` `unshift`. The block grows through
// `oskel_block_ensure`, then the items move up one slot with a backward loop
// (the destination is above the source, so a forward copy would clobber).
void emitArrayUnshiftMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                            const DataBlob& msgArgc) {
  CodeGen::Label shiftLoop = cg.newLabel();
  CodeGen::Label shiftDone = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movToMem(Rbp, -24, Rdi);                   // args
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movFromMem(R12, Rbx, kTableLengthOffset);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.addImm32(Rsi, 1);
  cg.movImm(Rdx, kArrayItemSize);
  cg.callLabel(env.blockEnsure);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(R10, Rax);                          // items block
  // for (j = len; j > 0; --j) block[j] = block[j-1].
  cg.movReg(R8, R12);
  cg.bind(shiftLoop);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, shiftDone);
  cg.movReg(R9, R8);
  cg.shlImm8(R9, 4);
  cg.addReg(R9, R10);                           // &block[j]
  cg.movFromMem(Rcx, R9, -16);
  cg.movToMem(R9, 0, Rcx);
  cg.movFromMem(Rcx, R9, -8);
  cg.movToMem(R9, 8, Rcx);
  cg.subRegImm32(R8, 1);
  cg.jmp(shiftLoop);
  cg.bind(shiftDone);
  cg.movFromMem(Rdi, Rbp, -24);                 // args
  cg.movFromMem(Rcx, Rdi, 0);                   // args[0] tag
  cg.movToMem(R10, 0, Rcx);
  cg.movFromMem(Rcx, Rdi, 8);                   // args[0] payload
  cg.movToMem(R10, 8, Rcx);
  cg.addImm32(R12, 1);
  cg.movToMem(Rbx, kTableLengthOffset, R12);
  cg.movReg(Rdx, Rbx);
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_builtin_array_insert(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.insert(index, x)`. A negative index counts from the end, an
// index outside `[0, len]` is the hosted "index out of range" error; the item
// goes in at `index` and the receiver is returned. The block grows, then the
// items from `index` on move up one slot (backward loop).
void emitArrayInsertMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                           const DataBlob& msgArgc, const DataBlob& msgIndexType,
                           const DataBlob& msgRange, const DataBlob& msgRangeClose) {
  CodeGen::Label nonNegative = cg.newLabel();
  CodeGen::Label shiftLoop = cg.newLabel();
  CodeGen::Label shiftDone = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label badRange = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movToMem(Rbp, -24, Rdi);                   // args
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movFromMem(R12, Rbx, kTableLengthOffset);
  cg.movFromMem(Rdi, Rbp, -24);                 // args
  cg.movFromMem(Rax, Rdi, 0);                   // index tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(R8, Rdi, 8);                    // index
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, nonNegative);
  cg.addReg(R8, R12);                           // negative: count from the end
  cg.bind(nonNegative);
  cg.testReg(R8);
  cg.jcc(Jcc::Lt, badRange);
  cg.cmpReg(R8, R12);
  cg.jcc(Jcc::Gt, badRange);
  cg.movToMem(Rbp, -32, R8);                    // index
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.addImm32(Rsi, 1);
  cg.movImm(Rdx, kArrayItemSize);
  cg.callLabel(env.blockEnsure);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(R10, Rax);                          // items block
  // for (j = len; j > index; --j) block[j] = block[j-1].
  cg.movReg(R8, R12);
  cg.bind(shiftLoop);
  cg.movFromMem(R9, Rbp, -32);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Le, shiftDone);
  cg.movReg(R11, R8);
  cg.shlImm8(R11, 4);
  cg.addReg(R11, R10);                          // &block[j]
  cg.movFromMem(Rcx, R11, -16);
  cg.movToMem(R11, 0, Rcx);
  cg.movFromMem(Rcx, R11, -8);
  cg.movToMem(R11, 8, Rcx);
  cg.subRegImm32(R8, 1);
  cg.jmp(shiftLoop);
  cg.bind(shiftDone);
  cg.movFromMem(R8, Rbp, -32);
  cg.shlImm8(R8, 4);
  cg.addReg(R8, R10);                           // &block[index]
  cg.movFromMem(Rdi, Rbp, -24);                 // args
  cg.movFromMem(Rcx, Rdi, -16);                 // args[1] tag
  cg.movToMem(R8, 0, Rcx);
  cg.movFromMem(Rcx, Rdi, -8);                  // args[1] payload
  cg.movToMem(R8, 8, Rcx);
  cg.addImm32(R12, 1);
  cg.movToMem(Rbx, kTableLengthOffset, R12);
  cg.movReg(Rdx, Rbx);
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  cg.movFromMem(Rdx, Rdi, 8);  // index payload, for the quoted type
  emitTypeTailFail(cg, msgIndexType, Rax, Rdx);
  cg.bind(badRange);
  emitFailLength(cg, cg.newLabel(), gMsgText.failDynamic, gMsgText.sbNew,
                 gMsgText.sbAppend, msgRange, msgRangeClose, R12);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_builtin_array_remove_at(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.removeAt(index)`. A negative index counts from the end, an
// index outside `[0, len)` is the hosted "index out of range" error; the item
// is removed and returned. The tail items move down one slot with a single
// forward `rep movsb`.
void emitArrayRemoveAtMethod(CodeGen& cg, CodeGen::Label label,
                             [[maybe_unused]] const CollectionEnv& env,
                             const DataBlob& msgArgc, const DataBlob& msgIndexType,
                             const DataBlob& msgRange, const DataBlob& msgRangeClose) {
  CodeGen::Label nonNegative = cg.newLabel();
  CodeGen::Label noMove = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label badRange = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movToMem(Rbp, -24, Rdi);                   // args
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movFromMem(R12, Rbx, kTableLengthOffset);
  cg.movFromMem(Rdi, Rbp, -24);                 // args
  cg.movFromMem(Rax, Rdi, 0);                   // index tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(R8, Rdi, 8);                    // index
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, nonNegative);
  cg.addReg(R8, R12);
  cg.bind(nonNegative);
  cg.testReg(R8);
  cg.jcc(Jcc::Lt, badRange);
  cg.cmpReg(R8, R12);
  cg.jcc(Jcc::Ge, badRange);
  cg.movToMem(Rbp, -32, R8);                    // index
  cg.movFromMem(R10, Rbx, kTableBlockOffset);   // items block
  cg.movReg(R9, R8);
  cg.shlImm8(R9, 4);
  cg.addReg(R9, R10);                           // &block[index]
  cg.movFromMem(Rax, R9, 0);
  cg.movToMem(Rbp, -40, Rax);                   // value tag
  cg.movFromMem(Rax, R9, 8);
  cg.movToMem(Rbp, -48, Rax);                   // value payload
  // memmove: block[index..len-2] = block[index+1..len-1], (len-1-index)*16.
  cg.movReg(R11, R12);
  cg.subRegImm32(R11, 1);
  cg.subReg(R11, R8);
  cg.testReg(R11);
  cg.jcc(Jcc::Eq, noMove);
  cg.shlImm8(R11, 4);
  cg.movReg(Rdi, R9);
  cg.movReg(Rsi, R9);
  cg.addImm32(Rsi, 16);
  cg.movReg(Rcx, R11);
  cg.repMovsb();
  cg.bind(noMove);
  cg.subRegImm32(R12, 1);
  cg.movToMem(Rbx, kTableLengthOffset, R12);
  cg.movFromMem(Rax, Rbp, -40);
  cg.movFromMem(Rdx, Rbp, -48);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  cg.movFromMem(Rdx, Rdi, 8);  // index payload, for the quoted type
  emitTypeTailFail(cg, msgIndexType, Rax, Rdx);
  cg.bind(badRange);
  emitFailLength(cg, cg.newLabel(), gMsgText.failDynamic, gMsgText.sbNew,
                 gMsgText.sbAppend, msgRange, msgRangeClose, R12);
}

// `AotValue oskel_builtin_array_contains(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.contains(x)`. It scans the items and returns a Bool, using the
// shared `oskel_compare` with `Eq` so nested arrays/maps compare structurally,
// exactly like the hosted `bindArray` `contains` (which calls `Value::equals`).
void emitArrayContainsMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                             const DataBlob& msgArgc) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label found = cg.newLabel();
  CodeGen::Label miss = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.push(R13);
  cg.push(R14);
  cg.subRegImm32(Rsp, 16);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movFromMem(R13, Rdi, 0);                   // needle tag
  cg.movFromMem(R14, Rdi, 8);                   // needle payload
  cg.movFromMem(Rax, Rbx, kTableLengthOffset);
  cg.movToMem(Rbp, -40, Rax);                   // len
  cg.xorReg(R12, R12);                          // index
  cg.bind(loop);
  cg.movFromMem(R8, Rbp, -40);
  cg.cmpReg(R12, R8);
  cg.jcc(Jcc::Ae, miss);
  cg.movReg(R9, R12);
  cg.shlImm8(R9, 4);
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.addReg(R9, R10);                           // &item
  cg.movFromMem(Rdi, R9, 0);
  cg.movFromMem(Rsi, R9, 8);
  cg.movReg(Rdx, R13);
  cg.movReg(Rcx, R14);
  cg.movImm(R8, static_cast<std::int64_t>(AotCompareOp::Eq));
  cg.callLabel(env.compare);
  cg.testReg(Rdx);
  cg.jcc(Jcc::Ne, found);
  cg.addImm32(R12, 1);
  cg.jmp(loop);
  cg.bind(found);
  cg.movImm(Rax, kTagBool);
  cg.movImm(Rdx, 1);
  cg.jmp(done);
  cg.bind(miss);
  cg.movImm(Rax, kTagBool);
  cg.xorReg(Rdx, Rdx);
  cg.bind(done);
  cg.addImm32(Rsp, 16);
  cg.pop(R14);
  cg.pop(R13);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_array_index_of(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.indexOf(x)`. Same scan as `contains`, but it returns the first
// matching index as an Int, or -1 when there is no match.
void emitArrayIndexOfMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                            const DataBlob& msgArgc) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label found = cg.newLabel();
  CodeGen::Label miss = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.push(R13);
  cg.push(R14);
  cg.subRegImm32(Rsp, 16);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movFromMem(R13, Rdi, 0);                   // needle tag
  cg.movFromMem(R14, Rdi, 8);                   // needle payload
  cg.movFromMem(Rax, Rbx, kTableLengthOffset);
  cg.movToMem(Rbp, -40, Rax);                   // len
  cg.xorReg(R12, R12);                          // index
  cg.bind(loop);
  cg.movFromMem(R8, Rbp, -40);
  cg.cmpReg(R12, R8);
  cg.jcc(Jcc::Ae, miss);
  cg.movReg(R9, R12);
  cg.shlImm8(R9, 4);
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.addReg(R9, R10);
  cg.movFromMem(Rdi, R9, 0);
  cg.movFromMem(Rsi, R9, 8);
  cg.movReg(Rdx, R13);
  cg.movReg(Rcx, R14);
  cg.movImm(R8, static_cast<std::int64_t>(AotCompareOp::Eq));
  cg.callLabel(env.compare);
  cg.testReg(Rdx);
  cg.jcc(Jcc::Ne, found);
  cg.addImm32(R12, 1);
  cg.jmp(loop);
  cg.bind(found);
  cg.movImm(Rax, kTagInt);
  cg.movReg(Rdx, R12);
  cg.jmp(done);
  cg.bind(miss);
  cg.movImm(Rax, kTagInt);
  cg.movImm(Rdx, -1);
  cg.bind(done);
  cg.addImm32(Rsp, 16);
  cg.pop(R14);
  cg.pop(R13);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_array_clear(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.clear()`. It sets the length to zero in place and returns the
// receiver -- the hosted `bindArray` `clear`. The items block is kept (the
// capacity is unchanged), exactly as `std::vector::clear` keeps it.
void emitArrayClearMethod(CodeGen& cg, CodeGen::Label label, [[maybe_unused]] const CollectionEnv& env,
                          const DataBlob& msgArgc) {
  CodeGen::Label badArgc = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movImmToMem(Rbx, kTableLengthOffset, 0);
  cg.movReg(Rdx, Rbx);
  cg.movImm(Rax, kTagArray);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// Allocates a fresh `AotArray` header and its items block for `count` items,
// with length and capacity set to `count` and the block bound. `count` is read
// from the stack slot `countSlot`; the header and the block are stored in
// `headerSlot` and `blockSlot`. Jumps to `oom` when the arena is exhausted.
// Every value the two `oskel_alloc` calls would clobber lives in a slot, so the
// helper needs no callee-saved register of its own.
void emitArrayAlloc(CodeGen& cg, const CollectionEnv& env, std::int64_t countSlot,
                    std::int64_t headerSlot, std::int64_t blockSlot,
                    CodeGen::Label oom) {
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotArrayHeaderSize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbp, headerSlot, Rax);
  cg.movImmToMem(Rax, kTableLengthOffset, 0);
  cg.movImmToMem(Rax, kTableCapacityOffset, 0);
  cg.movImmToMem(Rax, kTableBlockOffset, 0);
  cg.movFromMem(Rdi, Rbp, countSlot);
  cg.shlImm8(Rdi, 4);  // count * sizeof(AotValue)
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbp, blockSlot, Rax);
  cg.movFromMem(R8, Rbp, headerSlot);
  cg.movFromMem(R9, Rbp, countSlot);
  cg.movToMem(R8, kTableLengthOffset, R9);
  cg.movToMem(R8, kTableCapacityOffset, R9);
  cg.movToMem(R8, kTableBlockOffset, Rax);
}

// `AotValue oskel_builtin_range_to_array(AotValue* args, long argc, AotClosure* self)`:
// the bound `r.toArray()`. It takes no argument and materialises the range's
// values into a new array -- the hosted `bindRange` `toArray`, including the
// guard that refuses a span over ten million items before allocating. The
// receiver is unchanged.
void emitRangeToArrayMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                            const DataBlob& msgArgc, const DataBlob& msgTooLarge) {
  CodeGen::Label descending = cg.newLabel();
  CodeGen::Label saturate = cg.newLabel();
  CodeGen::Label countReady = cg.newLabel();
  CodeGen::Label countStored = cg.newLabel();
  CodeGen::Label stepReady = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label tooLarge = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 48);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);       // the range header
  // The item count is the range's `.length`, computed exactly as the hosted
  // `Range::length`: the magnitude of the bound difference in uint64 (a plain
  // 64-bit `sub` is the unsigned difference), plus one when inclusive,
  // saturated at INT64_MAX.
  cg.movFromMem(R8, Rbx, kRangeStartOffset);
  cg.movFromMem(R9, Rbx, kRangeEndOffset);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Lt, descending);
  cg.movReg(R10, R9);
  cg.subReg(R10, R8);                        // ascending: end - start
  cg.jmp(saturate);
  cg.bind(descending);
  cg.movReg(R10, R8);
  cg.subReg(R10, R9);                        // descending: start - end
  cg.bind(saturate);
  cg.movFromMem(R8, Rbx, kRangeInclusiveOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, countReady);
  cg.movImm(R11, -1);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Eq, countReady);               // distance == UINT64_MAX: saturate
  cg.addImm32(R10, 1);
  cg.bind(countReady);
  cg.movImm(R11, -1);
  cg.shrImm8(R11, 1);                        // INT64_MAX
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Be, countStored);
  cg.movReg(R10, R11);                       // saturate
  cg.bind(countStored);
  cg.movToMem(Rbp, -24, R10);                // count
  cg.movImm(R11, 10000000);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Gt, tooLarge);
  emitArrayAlloc(cg, env, -24, -32, -40, oom);
  // Fill the block: `i` walks from the start bound, `step` is +1 or -1 by the
  // range's direction, and the loop stores exactly `count` items.
  cg.movFromMem(R8, Rbx, kRangeStartOffset);
  cg.movToMem(Rbp, -48, R8);                 // i
  cg.movImm(R9, 1);
  cg.movFromMem(R10, Rbx, kRangeEndOffset);
  cg.movFromMem(R11, Rbx, kRangeStartOffset);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Ge, stepReady);
  cg.movImm(R9, -1);                         // descending
  cg.bind(stepReady);
  cg.movToMem(Rbp, -56, R9);                 // step
  cg.movImmToMem(Rbp, -64, 0);               // index
  cg.bind(loop);
  cg.movFromMem(R8, Rbp, -64);               // index
  cg.movFromMem(R9, Rbp, -24);               // count
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, done);
  cg.movFromMem(R10, Rbp, -48);              // i
  cg.movReg(R11, R8);
  cg.shlImm8(R11, 4);
  cg.movFromMem(R12, Rbp, -40);              // dst block
  cg.addReg(R11, R12);
  cg.movImmToMem(R11, 0, kTagInt);
  cg.movToMem(R11, 8, R10);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -64, R8);                 // index++
  cg.movFromMem(R10, Rbp, -48);
  cg.movFromMem(R9, Rbp, -56);               // step
  cg.addReg(R10, R9);
  cg.movToMem(Rbp, -48, R10);                // i += step
  cg.jmp(loop);

  cg.bind(done);
  cg.movFromMem(Rdx, Rbp, -32);              // the new array
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, 48);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(tooLarge);
  emitFailCall(cg, env.fail, msgTooLarge);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_builtin_range_contains(AotValue* args, long argc, AotClosure* self)`:
// the bound `r.contains(x)`, the method form of `x in r`. The receiver is the
// closure's upvalue cell and the single argument is the needle. The needle must
// be an integer (the hosted `bindRange` delegates to the `in` operator, whose
// range rule rejects anything else), and the bounds test is the shared
// `oskel_contains` range branch, so the two forms cannot drift.
void emitRangeContainsMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                             const DataBlob& msgArgc, const DataBlob& msgIntType) {
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rbx, Rdi, 0);                  // needle tag
  cg.movFromMem(R12, Rdi, 8);                  // needle payload
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badType);
  emitLoadArrayReceiver(cg, Rbx, Rdx);         // the range header
  cg.movImm(Rdi, kTagInt);                     // needle: an int
  cg.movReg(Rsi, R12);
  cg.movImm(Rdx, kTagRange);                   // haystack: the range
  cg.movReg(Rcx, Rbx);
  cg.callLabel(env.contains);
  cg.jmp(done);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  emitFailCall(cg, env.fail, msgIntType);
  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `AotValue oskel_builtin_array_clone(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.clone()`. It takes no argument and returns a shallow copy of
// the receiver as a new array -- the hosted `bindArray` `clone` (`makeArray`
// over a copy of the items). The receiver is left untouched, so a later write
// through the copy does not reach it.
void emitArrayCloneMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                          const DataBlob& msgArgc) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 48);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movFromMem(R12, Rbx, kTableLengthOffset);
  cg.movToMem(Rbp, -40, R12);           // count
  emitArrayAlloc(cg, env, -40, -48, -56, oom);
  cg.movFromMem(R9, Rbp, -40);          // count
  cg.movFromMem(R10, Rbx, kTableBlockOffset);   // src
  cg.movFromMem(R11, Rbp, -56);                 // dst
  cg.xorReg(R8, R8);                            // index
  cg.bind(loop);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, done);
  cg.movReg(Rax, R8);
  cg.shlImm8(Rax, 4);
  cg.addReg(Rax, R10);
  cg.movReg(Rdx, R8);
  cg.shlImm8(Rdx, 4);
  cg.addReg(Rdx, R11);
  cg.movFromMem(Rcx, Rax, 0);
  cg.movToMem(Rdx, 0, Rcx);
  cg.movFromMem(Rcx, Rax, 8);
  cg.movToMem(Rdx, 8, Rcx);
  cg.addImm32(R8, 1);
  cg.jmp(loop);
  cg.bind(done);
  cg.movFromMem(Rdx, Rbp, -48);         // the new array
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, 48);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_builtin_array_reversed(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.reversed()`. It takes no argument and returns a new array with
// the receiver's items in reverse order -- the hosted `bindArray` `reversed`
// (`std::reverse` over a copy). The receiver is unchanged.
void emitArrayReversedMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                             const DataBlob& msgArgc) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 48);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movFromMem(R12, Rbx, kTableLengthOffset);
  cg.movToMem(Rbp, -40, R12);           // count
  emitArrayAlloc(cg, env, -40, -48, -56, oom);
  cg.movFromMem(R9, Rbp, -40);          // count
  cg.movFromMem(R10, Rbx, kTableBlockOffset);   // src
  cg.movFromMem(R11, Rbp, -56);                 // dst
  cg.xorReg(R8, R8);                            // index
  cg.bind(loop);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, done);
  cg.movReg(Rax, R9);                   // count - 1 - index
  cg.subRegImm32(Rax, 1);
  cg.subReg(Rax, R8);
  cg.shlImm8(Rax, 4);
  cg.addReg(Rax, R10);
  cg.movReg(Rdx, R8);
  cg.shlImm8(Rdx, 4);
  cg.addReg(Rdx, R11);
  cg.movFromMem(Rcx, Rax, 0);
  cg.movToMem(Rdx, 0, Rcx);
  cg.movFromMem(Rcx, Rax, 8);
  cg.movToMem(Rdx, 8, Rcx);
  cg.addImm32(R8, 1);
  cg.jmp(loop);
  cg.bind(done);
  cg.movFromMem(Rdx, Rbp, -48);         // the new array
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, 48);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// Which callback method a body implements. The six share one skeleton -- walk
// the receiver, call the closure once per item, act on the result -- so they
// share one emitter instead of six copies of the same instructions.
enum class CallbackKind {
  Map,     // collect the results into a new array
  Filter,  // keep the items whose result is truthy
  Each,    // call for effect; return the receiver
  Find,    // return the first item whose result is truthy, else nil
  Any,     // true at the first truthy result, else false
  All      // false at the first falsy result, else true
};

// `AotValue oskel_builtin_array_<kind>(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.map(f)`/`filter`/`each`/`find`/`any`/`all`. Each takes exactly
// one argument, the callback, and mirrors the hosted `bindArray` of the same
// name (`src/runtime/builtins.cpp`), including the return value: `each` returns
// the receiver, not a new array.
//
// This is the first helper that calls *back* into the program, so the register
// discipline differs from every body above. `oskel_call` runs arbitrary Oskel
// code, which allocates, so r13/r14 (the arena cursor and end) must be left
// *advanced* by whatever the callback allocated -- saving and restoring them
// would rewind the cursor and let the next allocation overwrite the array this
// body built. r15 is the global table, written once at program start and never
// after, so it needs no save. Only rbx and r12, this body's own callee-saved
// locals, are pushed. Every piece of loop state lives in a stack slot and is
// reloaded after each call, because a call clobbers every caller-saved
// register.
//
// Frame slots below rbp. The return address, rbp, rbx and r12 occupy -8..-24,
// so every slot here starts below them. An `AotValue` occupies 16 bytes with
// its tag at the slot and its payload 8 bytes above it, so the two-value slots
// are spaced 16 apart: -48 receiver, -64/-56 callback tag/payload, -72 length,
// -80 index, -88/-96 output header/block, -104 output count, -128/-120 the
// one-item argument window, -144/-136 the callback's result. The 128-byte frame
// after the two pushes puts rsp at -144, the lowest slot.
constexpr std::int64_t kReceiverSlot = -48;
constexpr std::int64_t kCallbackTagSlot = -64;
constexpr std::int64_t kCallbackPayloadSlot = -56;
constexpr std::int64_t kLengthSlot = -72;
constexpr std::int64_t kIndexSlot = -80;
constexpr std::int64_t kOutHeaderSlot = -88;
constexpr std::int64_t kOutBlockSlot = -96;
constexpr std::int64_t kOutCountSlot = -104;
constexpr std::int64_t kWindowSlot = -128;
constexpr std::int64_t kResultSlot = -144;

// `AotValue oskel_builtin_array_<kind>(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.map(f)`/`filter`/`each`/`find`/`any`/`all`. Each takes exactly
// one argument, the callback, and mirrors the hosted `bindArray` of the same
// name (`src/runtime/builtins.cpp`), including the return value: `each` returns
// the receiver, not a new array.
//
// This is the first helper that calls *back* into the program, so the register
// discipline needs stating. `oskel_call` runs arbitrary Oskel code, which
// allocates: r13/r14 are the arena cursor and end, and they must be left
// *advanced* by whatever the callback allocated -- saving and restoring them
// would rewind the cursor and let the next allocation overwrite the array this
// body just built. r15 is the global table base, set once at program start and
// never written, so it needs no save either. Only rbx and r12 -- the program's
// own locals in the calling frame -- are pushed and restored. Every piece of
// loop state lives in a stack slot and is reloaded after each call, because a
// call clobbers every caller-saved register.
void emitArrayCallbackMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                             const DataBlob& msgArgc, const DataBlob& msgNotFunction,
                             CallbackKind kind) {
  // The two pushes leave rsp 0 mod 16; a 128-byte frame keeps it there and
  // reaches the lowest slot at -144.
  constexpr std::int32_t kFrameSize = 128;

  const bool buildsArray = kind == CallbackKind::Map || kind == CallbackKind::Filter;
  const bool needsTruthy = kind == CallbackKind::Filter || kind == CallbackKind::Find ||
                           kind == CallbackKind::Any || kind == CallbackKind::All;

  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label next = cg.newLabel();
  CodeGen::Label keep = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label exit = cg.newLabel();
  CodeGen::Label retTrue = cg.newLabel();
  CodeGen::Label retFalse = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, kFrameSize);

  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movToMem(Rbp, kReceiverSlot, Rbx);             // receiver
  cg.movFromMem(Rax, Rdi, 0);                       // callback tag
  cg.movToMem(Rbp, kCallbackTagSlot, Rax);
  cg.movFromMem(Rax, Rdi, 8);                       // callback payload
  cg.movToMem(Rbp, kCallbackPayloadSlot, Rax);
  cg.movFromMem(Rax, Rbx, kTableLengthOffset);      // length
  cg.movToMem(Rbp, kLengthSlot, Rax);
  cg.movImmToMem(Rbp, kIndexSlot, 0);               // index
  cg.movImmToMem(Rbp, kOutCountSlot, 0);            // output count

  if (buildsArray) {
    // Reserve the worst case up front -- map keeps every item and filter at
    // most every item -- so the loop never grows the block. A filter corrects
    // the header's length at the end; the capacity stays as reserved.
    emitArrayAlloc(cg, env, kLengthSlot, kOutHeaderSlot, kOutBlockSlot, oom);
  }

  cg.bind(loop);
  cg.movFromMem(R8, Rbp, kIndexSlot);               // index
  cg.movFromMem(R9, Rbp, kLengthSlot);              // length
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, done);

  // window[0] = receiver[index]
  cg.movFromMem(R10, Rbp, kReceiverSlot);           // receiver
  cg.movFromMem(R11, R10, kTableBlockOffset);       // items
  cg.movReg(Rax, R8);
  cg.shlImm8(Rax, 4);
  cg.addReg(Rax, R11);
  cg.movFromMem(Rcx, Rax, 0);
  cg.movToMem(Rbp, kWindowSlot, Rcx);
  cg.movFromMem(Rcx, Rax, 8);
  cg.movToMem(Rbp, kWindowSlot + 8, Rcx);

  // result = call(callback, 1, &window)
  cg.movFromMem(Rdi, Rbp, kCallbackTagSlot);        // callback tag
  cg.movFromMem(Rsi, Rbp, kCallbackPayloadSlot);    // callback payload
  // The VM names the method when the callback is not a function. The check
  // sits at the call, not at entry, because the VM validates only what it
  // invokes: an empty receiver never reports it.
  CodeGen::Label callable = cg.newLabel();
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFunc));
  cg.jcc(Jcc::Eq, callable);
  emitTypeTailFail(cg, msgNotFunction, Rdi, Rsi);
  cg.bind(callable);
  cg.movImm(Rdx, 1);
  cg.leaMem(Rcx, Rbp, kWindowSlot);
  cg.callLabel(env.call);
  cg.movToMem(Rbp, kResultSlot, Rax);               // result tag
  cg.movToMem(Rbp, kResultSlot + 8, Rdx);           // result payload

  if (needsTruthy) {
    cg.movFromMem(Rdi, Rbp, kResultSlot);
    cg.movFromMem(Rsi, Rbp, kResultSlot + 8);
    cg.callLabel(env.truthy);
    cg.testReg(Rax);
    cg.jcc(Jcc::Ne, keep);
    // The predicate said no: `all` fails, every other form just moves on.
    if (kind == CallbackKind::All) {
      cg.jmp(retFalse);
    } else {
      cg.jmp(next);
    }
  } else {
    cg.jmp(keep);  // map and each always act on the item
  }

  cg.bind(keep);
  switch (kind) {
    case CallbackKind::Map:
    case CallbackKind::Filter: {
      // map stores the callback's result, filter stores the item it was given.
      cg.movFromMem(R8, Rbp, kOutCountSlot);        // output count
      cg.movFromMem(R9, Rbp, kOutBlockSlot);        // block
      cg.movReg(Rax, R8);
      cg.shlImm8(Rax, 4);
      cg.addReg(Rax, R9);
      if (kind == CallbackKind::Map) {
        cg.movFromMem(Rcx, Rbp, kResultSlot);
        cg.movToMem(Rax, 0, Rcx);
        cg.movFromMem(Rcx, Rbp, kResultSlot + 8);
        cg.movToMem(Rax, 8, Rcx);
      } else {
        cg.movFromMem(Rcx, Rbp, kWindowSlot);
        cg.movToMem(Rax, 0, Rcx);
        cg.movFromMem(Rcx, Rbp, kWindowSlot + 8);
        cg.movToMem(Rax, 8, Rcx);
      }
      cg.addImm32(R8, 1);
      cg.movToMem(Rbp, kOutCountSlot, R8);
      cg.jmp(next);
      break;
    }
    case CallbackKind::Each:
      cg.jmp(next);
      break;
    case CallbackKind::Find:
      cg.movFromMem(Rax, Rbp, kWindowSlot);         // the matching item
      cg.movFromMem(Rdx, Rbp, kWindowSlot + 8);
      cg.jmp(exit);
      break;
    case CallbackKind::Any:
      cg.jmp(retTrue);
      break;
    case CallbackKind::All:
      cg.jmp(next);
      break;
  }

  cg.bind(next);
  cg.movFromMem(R8, Rbp, kIndexSlot);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kIndexSlot, R8);
  cg.jmp(loop);

  cg.bind(done);
  if (buildsArray) {
    // A filter kept `count` of `length` slots: publish the real length.
    cg.movFromMem(R8, Rbp, kOutCountSlot);
    cg.movFromMem(R9, Rbp, kOutHeaderSlot);
    cg.movToMem(R9, kTableLengthOffset, R8);
    cg.movReg(Rdx, R9);
    cg.movImm(Rax, kTagArray);
  } else if (kind == CallbackKind::Each) {
    cg.movFromMem(Rdx, Rbp, kReceiverSlot);         // the receiver, unchanged
    cg.movImm(Rax, kTagArray);
  } else if (kind == CallbackKind::Find) {
    cg.movImm(Rax, kTagNil);                        // no item matched
    cg.xorReg(Rdx, Rdx);
  } else if (kind == CallbackKind::Any) {
    cg.movImm(Rax, kTagBool);                       // nothing was truthy
    cg.xorReg(Rdx, Rdx);
  } else {
    cg.movImm(Rax, kTagBool);                       // all() held everywhere
    cg.movImm(Rdx, 1);
  }
  cg.jmp(exit);

  // The early exits, each setting rax:rdx and falling into the shared epilogue.
  cg.bind(retTrue);
  cg.movImm(Rax, kTagBool);
  cg.movImm(Rdx, 1);
  cg.jmp(exit);
  cg.bind(retFalse);
  cg.movImm(Rax, kTagBool);
  cg.xorReg(Rdx, Rdx);

  cg.bind(exit);
  cg.addImm32(Rsp, kFrameSize);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_builtin_array_reduce(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.reduce(f, initial?)` -- the hosted `bindArray` `reduce`. With
// an initial value (argc 2) the accumulator starts there and every item is
// folded; without one (argc 1) the first item seeds the accumulator and the
// fold starts at index 1, so an empty receiver is the hosted "reduce() of an
// empty array needs an initial value" error. The callback takes two arguments
// (accumulator, item) and its result becomes the next accumulator.
//
// The register discipline matches `emitArrayCallbackMethod`: rbx/r12 are the
// only saved registers, r13/r14 are the arena cursor and must be left advanced
// by whatever the callback allocated, and every loop value lives in a slot.
void emitArrayReduceMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                           const DataBlob& msgArgc, const DataBlob& msgEmpty,
                           const DataBlob& msgNotFunction) {
  // Slots below the two saved registers: -48 receiver, -64/-56 accumulator
  // tag/payload, -72 length, -80 index, -96/-88 callback tag/payload, -112 the
  // argument window. A callee reads `args[i]` at `args - 16*i` (the register
  // file grows down), so the window's *first* value sits at its highest
  // address: accumulator at -112, item at -128.
  constexpr std::int64_t kReceiver = -48;
  constexpr std::int64_t kAccTag = -64;
  constexpr std::int64_t kAccPayload = -56;
  constexpr std::int64_t kLength = -72;
  constexpr std::int64_t kIndex = -80;
  constexpr std::int64_t kCbTag = -96;
  constexpr std::int64_t kCbPayload = -88;
  constexpr std::int64_t kWindow = -112;
  constexpr std::int64_t kItemSlot = -128;
  constexpr std::int32_t kFrameSize = 160;

  CodeGen::Label hasInitial = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label empty = cg.newLabel();
  CodeGen::Label exit = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, kFrameSize);

  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::B, badArgc);              // argc < 1
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::A, badArgc);              // argc > 2
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movToMem(Rbp, kReceiver, Rbx);
  cg.movFromMem(Rax, Rbx, kTableLengthOffset);
  cg.movToMem(Rbp, kLength, Rax);
  cg.movFromMem(Rax, Rdi, 0);           // callback tag
  cg.movToMem(Rbp, kCbTag, Rax);
  cg.movFromMem(Rax, Rdi, 8);           // callback payload
  cg.movToMem(Rbp, kCbPayload, Rax);

  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Eq, hasInitial);
  // No initial value: the first item seeds the accumulator, folding from 1.
  cg.movFromMem(R8, Rbp, kLength);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, empty);
  cg.movFromMem(R9, Rbp, kReceiver);
  cg.movFromMem(R10, R9, kTableBlockOffset);
  cg.movFromMem(Rax, R10, 0);           // items[0] tag
  cg.movToMem(Rbp, kAccTag, Rax);
  cg.movFromMem(Rax, R10, 8);           // items[0] payload
  cg.movToMem(Rbp, kAccPayload, Rax);
  cg.movImmToMem(Rbp, kIndex, 1);
  cg.jmp(loop);
  cg.bind(hasInitial);
  cg.movFromMem(Rax, Rdi, -16);         // args[1] tag
  cg.movToMem(Rbp, kAccTag, Rax);
  cg.movFromMem(Rax, Rdi, -8);          // args[1] payload
  cg.movToMem(Rbp, kAccPayload, Rax);
  cg.movImmToMem(Rbp, kIndex, 0);

  cg.bind(loop);
  cg.movFromMem(R8, Rbp, kIndex);
  cg.movFromMem(R9, Rbp, kLength);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, done);

  // window[0] = accumulator, window[1] = receiver[index]
  cg.movFromMem(Rax, Rbp, kAccTag);
  cg.movToMem(Rbp, kWindow, Rax);
  cg.movFromMem(Rax, Rbp, kAccPayload);
  cg.movToMem(Rbp, kWindow + 8, Rax);
  cg.movFromMem(R10, Rbp, kReceiver);
  cg.movFromMem(R11, R10, kTableBlockOffset);
  cg.movReg(Rax, R8);
  cg.shlImm8(Rax, 4);
  cg.addReg(Rax, R11);
  cg.movFromMem(Rcx, Rax, 0);
  cg.movToMem(Rbp, kItemSlot, Rcx);
  cg.movFromMem(Rcx, Rax, 8);
  cg.movToMem(Rbp, kItemSlot + 8, Rcx);

  cg.movFromMem(Rdi, Rbp, kCbTag);
  cg.movFromMem(Rsi, Rbp, kCbPayload);
  // Same guard as the other callback methods: the VM reports the name, and
  // only once it actually invokes.
  CodeGen::Label callable = cg.newLabel();
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFunc));
  cg.jcc(Jcc::Eq, callable);
  emitTypeTailFail(cg, msgNotFunction, Rdi, Rsi);
  cg.bind(callable);
  cg.movImm(Rdx, 2);
  cg.leaMem(Rcx, Rbp, kWindow);
  cg.callLabel(env.call);
  cg.movToMem(Rbp, kAccTag, Rax);       // the next accumulator
  cg.movToMem(Rbp, kAccPayload, Rdx);

  cg.movFromMem(R8, Rbp, kIndex);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kIndex, R8);
  cg.jmp(loop);

  cg.bind(done);
  cg.movFromMem(Rax, Rbp, kAccTag);
  cg.movFromMem(Rdx, Rbp, kAccPayload);
  cg.jmp(exit);

  cg.bind(exit);
  cg.addImm32(Rsp, kFrameSize);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(empty);
  emitFailCall(cg, env.fail, msgEmpty);
}

// `AotValue oskel_builtin_array_sum(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.sum()` -- the hosted `bindArray` `sum`. It accumulates every
// item as an integer while they are all integers and switches to a double at
// the first float/double, so `[1, 2].sum()` is the int 3 and `[1, 2.5].sum()`
// is the double 3.5, matching the VM's all-int fast path. A non-number is the
// hosted "sum() expects an array of numbers" error.
void emitArraySumMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                        const DataBlob& msgArgc, const DataBlob& msgType) {
  // Slots: -48 receiver, -56 length, -64 index, -72 int total, -80 float
  // total, -88 the all-ints flag (0 once a float is seen).
  constexpr std::int64_t kReceiver = -48;
  constexpr std::int64_t kLength = -56;
  constexpr std::int64_t kIndex = -64;
  constexpr std::int64_t kIntTotal = -72;
  constexpr std::int64_t kFloatTotal = -80;
  constexpr std::int64_t kAllInts = -88;
  // The two pushes take -8..-16, so the frame must reach past -88.
  constexpr std::int32_t kFrameSize = 80;

  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label addInt = cg.newLabel();
  CodeGen::Label addFloat = cg.newLabel();
  CodeGen::Label accumulate = cg.newLabel();
  CodeGen::Label intAsDouble = cg.newLabel();
  CodeGen::Label next = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label exitAsDouble = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label exit = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, kFrameSize);

  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movToMem(Rbp, kReceiver, Rbx);
  cg.movFromMem(Rax, Rbx, kTableLengthOffset);
  cg.movToMem(Rbp, kLength, Rax);
  cg.movImmToMem(Rbp, kIndex, 0);
  cg.movImmToMem(Rbp, kIntTotal, 0);
  cg.movImmToMem(Rbp, kFloatTotal, 0);
  cg.movImmToMem(Rbp, kAllInts, 1);

  cg.bind(loop);
  cg.movFromMem(R8, Rbp, kIndex);
  cg.movFromMem(R9, Rbp, kLength);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, done);
  cg.movFromMem(R10, Rbp, kReceiver);
  cg.movFromMem(R11, R10, kTableBlockOffset);
  cg.movReg(Rax, R8);
  cg.shlImm8(Rax, 4);
  cg.addReg(Rax, R11);                  // &items[index]
  cg.movFromMem(Rcx, Rax, 0);           // tag
  cg.movFromMem(Rdx, Rax, 8);           // payload
  cg.cmpImm32(Rcx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, addInt);
  cg.cmpImm32(Rcx, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, addFloat);
  cg.cmpImm32(Rcx, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, addFloat);
  cg.jmp(badType);

  cg.bind(addInt);
  cg.movFromMem(R9, Rbp, kAllInts);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, intAsDouble);         // a float was already seen
  cg.movFromMem(R9, Rbp, kIntTotal);
  cg.addReg(R9, Rdx);                   // wrapping add, like the VM's uint64 cast
  cg.movToMem(Rbp, kIntTotal, R9);
  cg.jmp(next);
  cg.bind(intAsDouble);
  cg.cvtsi2sd(Xmm1, Rdx);               // the payload is an integer, not a bit pattern
  cg.movqFromXmm(Rdx, Xmm1);
  cg.jmp(accumulate);

  cg.bind(addFloat);
  cg.movFromMem(R9, Rbp, kAllInts);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, accumulate);          // the float total is already seeded
  cg.movFromMem(R8, Rbp, kIntTotal);    // seed the float total from the int one
  cg.cvtsi2sd(Xmm0, R8);
  cg.movqFromXmm(R8, Xmm0);
  cg.movToMem(Rbp, kFloatTotal, R8);
  cg.movImmToMem(Rbp, kAllInts, 0);

  cg.bind(accumulate);
  // A float/double payload is already the binary64 bit pattern; an int was
  // converted above.
  cg.movFromMem(R8, Rbp, kFloatTotal);
  cg.movqToXmm(Xmm0, R8);
  cg.movqToXmm(Xmm1, Rdx);
  cg.addsd(Xmm0, Xmm1);
  cg.movqFromXmm(R8, Xmm0);
  cg.movToMem(Rbp, kFloatTotal, R8);
  cg.jmp(next);

  cg.bind(next);
  cg.movFromMem(R8, Rbp, kIndex);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kIndex, R8);
  cg.jmp(loop);

  cg.bind(done);
  cg.movFromMem(R9, Rbp, kAllInts);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, exitAsDouble);
  cg.movImm(Rax, kTagInt);
  cg.movFromMem(Rdx, Rbp, kIntTotal);
  cg.jmp(exit);
  cg.bind(exitAsDouble);
  cg.movImm(Rax, kTagDouble);
  cg.movFromMem(Rdx, Rbp, kFloatTotal);
  cg.jmp(exit);

  cg.bind(exit);
  cg.addImm32(Rsp, kFrameSize);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  emitFailCall(cg, env.fail, msgType);
}

// `AotValue oskel_builtin_array_join(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.join(separator?)` -- the hosted `bindArray` `join`. Each item
// renders with `toString` (a string unquoted, a collection as its repr) and the
// separator, empty when absent, goes between them.
//
// The parts go into one `AotStringBuilder`, whose growth doubles the block, so
// the total work is linear in the result. Appending with `oskel_concat` per item
// allocated and copied the whole accumulator every time, which is quadratic
// (BACKLOG §6 item 36): 5 000 parts took 147 ms that way and 6.7 ms in the VM.
void emitArrayJoinMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                         const DataBlob& msgArgc) {
  // Slots: -48 receiver, -56 length, -64 index, -72 builder, -80 separator
  // payload, -88 rendered item payload, -104/-96 the one-element argument
  // window (`$str` reads a contiguous tag/payload pair).
  constexpr std::int64_t kReceiver = -48;
  constexpr std::int64_t kLength = -56;
  constexpr std::int64_t kIndex = -64;
  constexpr std::int64_t kBuilder = -72;
  constexpr std::int64_t kSepPayload = -80;
  constexpr std::int64_t kItemPayload = -88;
  constexpr std::int64_t kWindowTag = -104;
  constexpr std::int64_t kWindowPayload = -96;
  constexpr std::int32_t kFrameSize = 112;

  CodeGen::Label emptySep = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label iterate = cg.newLabel();
  CodeGen::Label noSeparator = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label exitEmpty = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label exit = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, kFrameSize);

  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::A, badArgc);              // argc > 1
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movToMem(Rbp, kReceiver, Rbx);
  cg.movFromMem(Rax, Rbx, kTableLengthOffset);
  cg.movToMem(Rbp, kLength, Rax);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, exitEmpty);  // an empty receiver joins to ""
  cg.movImmToMem(Rbp, kIndex, 0);
  // The separator: args[0] rendered through `toString` when present (the
  // hosted `join` calls `args[0].toString()`, so any value is accepted), else
  // the empty string.
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, emptySep);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movToMem(Rbp, kWindowTag, Rax);
  cg.movFromMem(Rax, Rdi, 8);
  cg.movToMem(Rbp, kWindowPayload, Rax);
  cg.leaMem(Rdi, Rbp, kWindowTag);
  cg.movImm(Rsi, 1);
  cg.callLabel(env.strBuiltin);
  cg.movToMem(Rbp, kSepPayload, Rdx);
  cg.jmp(loop);
  cg.bind(emptySep);
  cg.leaRipLabel(Rax, env.emptyString);
  cg.movToMem(Rbp, kSepPayload, Rax);

  cg.bind(loop);
  cg.callLabel(env.sbNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbp, kBuilder, Rax);

  cg.bind(iterate);
  cg.movFromMem(R8, Rbp, kIndex);
  cg.movFromMem(R9, Rbp, kLength);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, done);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, noSeparator);  // the first item takes no separator
  cg.movFromMem(Rdi, Rbp, kBuilder);
  cg.movFromMem(Rsi, Rbp, kSepPayload);
  cg.movFromMem(Rdx, Rsi, 0);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(env.sbAppend);

  cg.bind(noSeparator);
  cg.movFromMem(R8, Rbp, kIndex);
  cg.movFromMem(R10, Rbp, kReceiver);
  cg.movFromMem(R11, R10, kTableBlockOffset);
  cg.movReg(Rax, R8);
  cg.shlImm8(Rax, 4);
  cg.addReg(Rax, R11);
  // rendered = $str(item): a one-element argument window, then (args, argc).
  cg.movFromMem(Rcx, Rax, 0);
  cg.movToMem(Rbp, kWindowTag, Rcx);
  cg.movFromMem(Rcx, Rax, 8);
  cg.movToMem(Rbp, kWindowPayload, Rcx);
  cg.leaMem(Rdi, Rbp, kWindowTag);
  cg.movImm(Rsi, 1);
  cg.callLabel(env.strBuiltin);
  cg.movToMem(Rbp, kItemPayload, Rdx);
  cg.movFromMem(Rdi, Rbp, kBuilder);
  cg.movFromMem(Rsi, Rbp, kItemPayload);
  cg.movFromMem(Rdx, Rsi, 0);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(env.sbAppend);
  cg.movFromMem(R8, Rbp, kIndex);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kIndex, R8);
  cg.jmp(iterate);

  cg.bind(done);
  cg.movFromMem(R11, Rbp, kBuilder);
  cg.movFromMem(Rdi, R11, kSbDataOffset);
  cg.movFromMem(Rsi, R11, kSbLengthOffset);
  cg.callLabel(env.strNew);
  cg.jmp(exit);

  cg.bind(exitEmpty);
  cg.leaRipLabel(Rdx, env.emptyString);
  cg.movImm(Rax, kTagStr);

  cg.bind(exit);
  cg.addImm32(Rsp, kFrameSize);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// Stable merge sort over an array's items, shared by `sort` and `sorted`
// (PHASE 7c.3j). The hosted engine sorts with `std::stable_sort`, so a merge
// sort is the one algorithm that reproduces its result exactly: an unstable
// sort would reorder equal items and diverge from the VM whenever a comparator
// treats two distinct items as equal.
//
// Bottom-up rather than recursive: a recursive merge would deepen the program's
// own stack by log2(n) frames. The auxiliary block is one arena allocation of
// `n` values, taken *before* the first comparator call, because a callback
// allocates and the arena cursor (r13) must not be snapshotted across it.
//
// Every comparator call goes through `oskel_call`, which clobbers the caller-
// saved registers, so the whole sort state lives in these slots:
//   -48 receiver, -56 count, -64 source block, -72 auxiliary block,
//   -80/-88 comparator tag/payload, -96 hasComparator, -104 new array header
//   (only for `sorted`), -128/-144 the two-value callback window (window[0] at
//   the higher address because the register file grows down), -152 merge width,
//   -160 run start, -168 middle, -176 run end, -184 left, -192 right,
//   -200 output.
void emitArraySortBody(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                       const DataBlob& msgArgc, const DataBlob& msgCmpType,
                       bool mutateReceiver) {
  constexpr std::int64_t kReceiver = -48;
  constexpr std::int64_t kCount = -56;
  constexpr std::int64_t kSource = -64;
  constexpr std::int64_t kAux = -72;
  constexpr std::int64_t kCbTag = -80;
  constexpr std::int64_t kCbPayload = -88;
  constexpr std::int64_t kHasCmp = -96;
  constexpr std::int64_t kNewHeader = -104;
  // The callback window is two AotValues. `args[i]` sits at `args - 16*i`, so
  // window[0] is the higher address (-128/-120) and window[1] the lower
  // (-144/-136); the merge state starts below them.
  constexpr std::int64_t kWindow = -128;
  constexpr std::int64_t kWidth = -152;
  constexpr std::int64_t kRunStart = -160;
  constexpr std::int64_t kMiddle = -168;
  constexpr std::int64_t kRunEnd = -176;
  constexpr std::int64_t kLeft = -184;
  constexpr std::int64_t kRight = -192;
  constexpr std::int64_t kOut = -200;
  constexpr std::int32_t kFrameSize = 224;

  CodeGen::Label noCmp = cg.newLabel();
  CodeGen::Label cmpReady = cg.newLabel();
  CodeGen::Label widthLoop = cg.newLabel();
  CodeGen::Label runLoop = cg.newLabel();
  CodeGen::Label middleClamped = cg.newLabel();
  CodeGen::Label endClamped = cg.newLabel();
  CodeGen::Label mergeLoop = cg.newLabel();
  CodeGen::Label defaultCompare = cg.newLabel();
  CodeGen::Label takeRight = cg.newLabel();
  CodeGen::Label takeLeft = cg.newLabel();
  CodeGen::Label drainLeft = cg.newLabel();
  CodeGen::Label drainRight = cg.newLabel();
  CodeGen::Label copyBack = cg.newLabel();
  CodeGen::Label cloneLoop = cg.newLabel();
  CodeGen::Label cloneDone = cg.newLabel();
  CodeGen::Label backLoop = cg.newLabel();
  CodeGen::Label backDone = cg.newLabel();
  CodeGen::Label nextWidth = cg.newLabel();
  CodeGen::Label commitLoop = cg.newLabel();
  CodeGen::Label commitDone = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badCmpType = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, kFrameSize);

  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::A, badArgc);              // argc > 1
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movToMem(Rbp, kReceiver, Rbx);

  // The comparator is optional; absent means the default `lessThan` order, which
  // `oskel_compare(Lt)` already implements with the same type rules and the
  // same failure message.
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, noCmp);
  cg.movFromMem(Rax, Rdi, 0);           // comparator tag
  cg.movToMem(Rbp, kCbTag, Rax);
  cg.movFromMem(Rax, Rdi, 8);           // comparator payload
  cg.movToMem(Rbp, kCbPayload, Rax);
  cg.movImmToMem(Rbp, kHasCmp, 1);
  cg.jmp(cmpReady);
  cg.bind(noCmp);
  cg.movImmToMem(Rbp, kHasCmp, 0);
  cg.bind(cmpReady);

  cg.movFromMem(Rax, Rbx, kTableLengthOffset);
  cg.movToMem(Rbp, kCount, Rax);

  // Both forms sort a *copy* and commit only on success. `std::stable_sort` in
  // the hosted engine permutes the range in place, so it sorts a copy and
  // assigns it back afterwards: a comparator that raises leaves the array
  // untouched. Sorting the receiver's own block here would leave it partially
  // ordered, so the merge always runs on this fresh block and `sort` copies it
  // back once the pass is complete.
  emitArrayAlloc(cg, env, kCount, kNewHeader, kSource, oom);
  cg.movFromMem(R9, Rbx, kTableBlockOffset);   // src
  cg.movFromMem(R10, Rbp, kSource);            // dst
  cg.movFromMem(R11, Rbp, kCount);
  cg.xorReg(R8, R8);
  cg.bind(cloneLoop);
  cg.cmpReg(R8, R11);
  cg.jcc(Jcc::Ae, cloneDone);
  cg.movReg(Rax, R8);
  cg.shlImm8(Rax, 4);
  cg.addReg(Rax, R9);
  cg.movFromMem(Rcx, Rax, 0);
  cg.movToMem(R10, 0, Rcx);
  cg.movFromMem(Rcx, Rax, 8);
  cg.movToMem(R10, 8, Rcx);
  cg.addImm32(R10, 16);
  cg.addImm32(R8, 1);
  cg.jmp(cloneLoop);
  cg.bind(cloneDone);

  // One auxiliary block of `n` values, so a merge never allocates.
  cg.movFromMem(Rdi, Rbp, kCount);
  cg.shlImm8(Rdi, 4);
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbp, kAux, Rax);

  cg.movImmToMem(Rbp, kWidth, 1);
  cg.bind(widthLoop);
  cg.movFromMem(R8, Rbp, kWidth);
  cg.movFromMem(R9, Rbp, kCount);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, done);                // width >= count: sorted
  cg.movImmToMem(Rbp, kRunStart, 0);

  cg.bind(runLoop);
  cg.movFromMem(R8, Rbp, kRunStart);
  cg.movFromMem(R9, Rbp, kCount);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, nextWidth);

  // middle = min(runStart + width, count)
  cg.movFromMem(Rax, Rbp, kWidth);
  cg.addReg(Rax, R8);
  cg.movFromMem(Rcx, Rbp, kCount);
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::B, middleClamped);
  cg.movReg(Rax, Rcx);
  cg.bind(middleClamped);
  cg.movToMem(Rbp, kMiddle, Rax);
  // end = min(middle + width, count)
  cg.movFromMem(Rcx, Rbp, kWidth);
  cg.addReg(Rcx, Rax);
  cg.movFromMem(R11, Rbp, kCount);
  cg.cmpReg(Rcx, R11);
  cg.jcc(Jcc::B, endClamped);
  cg.movReg(Rcx, R11);
  cg.bind(endClamped);
  cg.movToMem(Rbp, kRunEnd, Rcx);

  cg.movToMem(Rbp, kLeft, R8);          // left = runStart
  cg.movToMem(Rbp, kRight, Rax);        // right = middle
  cg.movToMem(Rbp, kOut, R8);           // out = runStart

  cg.bind(mergeLoop);
  cg.movFromMem(R8, Rbp, kLeft);
  cg.movFromMem(R9, Rbp, kMiddle);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, drainRight);          // left run exhausted
  cg.movFromMem(R10, Rbp, kRight);
  cg.movFromMem(R11, Rbp, kRunEnd);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Ae, drainLeft);           // right run exhausted

  // Both runs alive: take the right item only when it is strictly before the
  // left one. On a tie the left item wins, which is what makes the sort stable.
  cg.movFromMem(Rax, Rbp, kHasCmp);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, defaultCompare);

  cg.movFromMem(R8, Rbp, kSource);      // src block
  cg.movFromMem(R9, Rbp, kRight);
  cg.shlImm8(R9, 4);
  cg.addReg(R9, R8);                    // &src[right]
  cg.movFromMem(Rax, R9, 0);
  cg.movToMem(Rbp, kWindow, Rax);       // window[0] = src[right]
  cg.movFromMem(Rax, R9, 8);
  cg.movToMem(Rbp, kWindow + 8, Rax);
  cg.movFromMem(R10, Rbp, kLeft);
  cg.shlImm8(R10, 4);
  cg.addReg(R10, R8);                   // &src[left]
  cg.movFromMem(Rax, R10, 0);
  cg.movToMem(Rbp, kWindow - 16, Rax);  // window[1] = src[left]
  cg.movFromMem(Rax, R10, 8);
  cg.movToMem(Rbp, kWindow - 8, Rax);

  cg.movFromMem(Rdi, Rbp, kCbTag);
  cg.movFromMem(Rsi, Rbp, kCbPayload);
  cg.movImm(Rdx, 2);
  cg.leaMem(Rcx, Rbp, kWindow);
  cg.callLabel(env.call);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Ne, badCmpType);
  cg.testReg(Rdx);
  cg.jcc(Jcc::Ne, takeRight);
  cg.jmp(takeLeft);

  cg.bind(defaultCompare);
  cg.movFromMem(R8, Rbp, kSource);
  cg.movFromMem(R9, Rbp, kRight);
  cg.shlImm8(R9, 4);
  cg.addReg(R9, R8);                    // &src[right]
  cg.movFromMem(Rdi, R9, 0);            // right tag
  cg.movFromMem(Rsi, R9, 8);            // right payload
  cg.movFromMem(R10, Rbp, kLeft);
  cg.shlImm8(R10, 4);
  cg.addReg(R10, R8);                   // &src[left]
  cg.movFromMem(Rdx, R10, 0);           // left tag
  cg.movFromMem(Rcx, R10, 8);           // left payload
  cg.movImm(R8, static_cast<std::int64_t>(AotCompareOp::Lt));
  cg.callLabel(env.compare);            // rax:rdx = right < left
  cg.testReg(Rdx);
  cg.jcc(Jcc::Ne, takeRight);

  cg.bind(takeLeft);
  cg.movFromMem(R8, Rbp, kLeft);
  cg.movFromMem(R9, Rbp, kSource);
  cg.movReg(R10, R8);
  cg.shlImm8(R10, 4);
  cg.addReg(R10, R9);                   // &src[left]
  cg.movFromMem(Rax, R10, 0);
  cg.movFromMem(Rdx, R10, 8);
  cg.movFromMem(R11, Rbp, kOut);
  cg.shlImm8(R11, 4);
  cg.movFromMem(R12, Rbp, kAux);
  cg.addReg(R11, R12);                  // &aux[out]
  cg.movToMem(R11, 0, Rax);
  cg.movToMem(R11, 8, Rdx);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kLeft, R8);
  cg.movFromMem(R8, Rbp, kOut);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kOut, R8);
  cg.jmp(mergeLoop);

  cg.bind(takeRight);
  cg.movFromMem(R8, Rbp, kRight);
  cg.movFromMem(R9, Rbp, kSource);
  cg.movReg(R10, R8);
  cg.shlImm8(R10, 4);
  cg.addReg(R10, R9);                   // &src[right]
  cg.movFromMem(Rax, R10, 0);
  cg.movFromMem(Rdx, R10, 8);
  cg.movFromMem(R11, Rbp, kOut);
  cg.shlImm8(R11, 4);
  cg.movFromMem(R12, Rbp, kAux);
  cg.addReg(R11, R12);                  // &aux[out]
  cg.movToMem(R11, 0, Rax);
  cg.movToMem(R11, 8, Rdx);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kRight, R8);
  cg.movFromMem(R8, Rbp, kOut);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kOut, R8);
  cg.jmp(mergeLoop);

  // Drain whichever run still has items, then copy the merged run back.
  cg.bind(drainLeft);
  cg.movFromMem(R8, Rbp, kLeft);
  cg.movFromMem(R9, Rbp, kMiddle);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, copyBack);
  cg.movFromMem(R10, Rbp, kSource);
  cg.movReg(R11, R8);
  cg.shlImm8(R11, 4);
  cg.addReg(R11, R10);
  cg.movFromMem(Rax, R11, 0);
  cg.movFromMem(Rdx, R11, 8);
  cg.movFromMem(R11, Rbp, kOut);
  cg.shlImm8(R11, 4);
  cg.movFromMem(R12, Rbp, kAux);
  cg.addReg(R11, R12);
  cg.movToMem(R11, 0, Rax);
  cg.movToMem(R11, 8, Rdx);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kLeft, R8);
  cg.movFromMem(R8, Rbp, kOut);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kOut, R8);
  cg.jmp(drainLeft);

  cg.bind(drainRight);
  cg.movFromMem(R8, Rbp, kRight);
  cg.movFromMem(R9, Rbp, kRunEnd);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, copyBack);
  cg.movFromMem(R10, Rbp, kSource);
  cg.movReg(R11, R8);
  cg.shlImm8(R11, 4);
  cg.addReg(R11, R10);
  cg.movFromMem(Rax, R11, 0);
  cg.movFromMem(Rdx, R11, 8);
  cg.movFromMem(R11, Rbp, kOut);
  cg.shlImm8(R11, 4);
  cg.movFromMem(R12, Rbp, kAux);
  cg.addReg(R11, R12);
  cg.movToMem(R11, 0, Rax);
  cg.movToMem(R11, 8, Rdx);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kRight, R8);
  cg.movFromMem(R8, Rbp, kOut);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kOut, R8);
  cg.jmp(drainRight);

  // Copy the merged run [runStart, runEnd) from the auxiliary block back into
  // the source block, then advance to the next pair.
  cg.bind(copyBack);
  cg.movFromMem(R8, Rbp, kRunStart);
  cg.movFromMem(R9, Rbp, kRunEnd);
  cg.bind(backLoop);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, backDone);
  cg.movReg(R10, R8);
  cg.shlImm8(R10, 4);
  cg.movFromMem(R11, Rbp, kAux);
  cg.addReg(R10, R11);
  cg.movFromMem(Rax, R10, 0);
  cg.movFromMem(Rdx, R10, 8);
  cg.movReg(R10, R8);
  cg.shlImm8(R10, 4);
  cg.movFromMem(R11, Rbp, kSource);
  cg.addReg(R10, R11);
  cg.movToMem(R10, 0, Rax);
  cg.movToMem(R10, 8, Rdx);
  cg.addImm32(R8, 1);
  cg.jmp(backLoop);
  cg.bind(backDone);
  cg.movFromMem(R8, Rbp, kRunEnd);
  cg.movToMem(Rbp, kRunStart, R8);
  cg.jmp(runLoop);

  cg.bind(nextWidth);
  cg.movFromMem(R8, Rbp, kWidth);
  cg.addReg(R8, R8);                    // width *= 2
  cg.movToMem(Rbp, kWidth, R8);
  cg.jmp(widthLoop);

  cg.bind(done);
  if (mutateReceiver) {
    // Commit: copy the sorted block back over the receiver's items, so the
    // array the caller holds becomes sorted and `sort` returns that same array.
    cg.movFromMem(R8, Rbp, kReceiver);
    cg.movFromMem(R9, R8, kTableBlockOffset);    // receiver block
    cg.movFromMem(R10, Rbp, kSource);            // sorted block
    cg.movFromMem(R11, Rbp, kCount);
    cg.xorReg(Rax, Rax);
    cg.bind(commitLoop);
    cg.cmpReg(Rax, R11);
    cg.jcc(Jcc::Ae, commitDone);
    cg.movReg(Rcx, Rax);
    cg.shlImm8(Rcx, 4);
    cg.movReg(R12, Rcx);
    cg.addReg(R12, R10);
    cg.movFromMem(Rdi, R12, 0);
    cg.movFromMem(Rsi, R12, 8);
    cg.addReg(Rcx, R9);
    cg.movToMem(Rcx, 0, Rdi);
    cg.movToMem(Rcx, 8, Rsi);
    cg.addImm32(Rax, 1);
    cg.jmp(commitLoop);
    cg.bind(commitDone);
    cg.movFromMem(Rdx, Rbp, kReceiver);  // `sort` returns the receiver
  } else {
    cg.movFromMem(Rdx, Rbp, kNewHeader); // `sorted` returns the new array
  }
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, kFrameSize);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badCmpType);
  emitFailCall(cg, env.fail, msgCmpType);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

void emitArraySortMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                         const DataBlob& msgArgc, const DataBlob& msgCmpType) {
  emitArraySortBody(cg, label, env, msgArgc, msgCmpType, true);
}

void emitArraySortedMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                           const DataBlob& msgArgc, const DataBlob& msgCmpType) {
  emitArraySortBody(cg, label, env, msgArgc, msgCmpType, false);
}

// ---------------------------------------------------------- string method helpers
//
// The string methods below share four primitives. Writing them once keeps each
// method body short and, more importantly, keeps the UTF-8 rules in one place:
// a "character" is a lead byte (anything that is not a continuation byte,
// `(b & 0xC0) != 0x80`) plus the continuation bytes that follow it, which is
// exactly how the hosted `utf8Chars` splits a string.

// `long oskel_str_find(AotString* hay, AotString* needle)`: the byte offset of
// the first occurrence of `needle` in `hay`, or -1. An empty `needle` is found
// at 0, matching `std::string::find`. A naive scan: the strings a program
// searches are short and the hosted engine uses the same algorithm.
void emitStrFind(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label outer = cg.newLabel();
  CodeGen::Label inner = cg.newLabel();
  CodeGen::Label next = cg.newLabel();
  CodeGen::Label found = cg.newLabel();
  CodeGen::Label notFound = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  cg.movFromMem(R8, Rdi, 0);    // hayLen
  cg.movFromMem(R9, Rsi, 0);    // needleLen
  cg.xorReg(Rax, Rax);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, done);        // empty needle: offset 0
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::A, notFound);     // needle longer than hay
  cg.xorReg(R10, R10);          // i
  cg.bind(outer);
  cg.movReg(Rax, R8);
  cg.subReg(Rax, R9);           // last start = hayLen - needleLen
  cg.cmpReg(R10, Rax);
  cg.jcc(Jcc::A, notFound);
  cg.xorReg(R11, R11);          // j
  cg.bind(inner);
  cg.cmpReg(R11, R9);
  cg.jcc(Jcc::Ae, found);
  cg.movReg(Rax, R10);
  cg.addReg(Rax, R11);
  cg.addReg(Rax, Rdi);
  cg.movzxFromMem(Rax, Rax, kStringDataOffset);   // hay[i + j]
  cg.movReg(Rcx, R11);
  cg.addReg(Rcx, Rsi);
  cg.movzxFromMem(Rcx, Rcx, kStringDataOffset);   // needle[j]
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::Ne, next);
  cg.addImm32(R11, 1);
  cg.jmp(inner);
  cg.bind(next);
  cg.addImm32(R10, 1);
  cg.jmp(outer);
  cg.bind(found);
  cg.movReg(Rax, R10);
  cg.jmp(done);
  cg.bind(notFound);
  cg.movImm(Rax, -1);
  cg.bind(done);
  cg.retInsn();
}

// `long oskel_str_find_from(AotString* hay, AotString* needle, long start)`:
// the byte offset of the first `needle` at or after `start`, or -1. The same
// scan as `emitStrFind`, with the outer index seeded from `start`, so `split`
// and `replace` can walk a separator without building a substring per step.
void emitStrFindFrom(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label outer = cg.newLabel();
  CodeGen::Label inner = cg.newLabel();
  CodeGen::Label next = cg.newLabel();
  CodeGen::Label found = cg.newLabel();
  CodeGen::Label notFound = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  cg.movFromMem(R8, Rdi, 0);    // hayLen
  cg.movFromMem(R9, Rsi, 0);    // needleLen
  cg.movReg(R10, Rdx);          // i = start
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::A, notFound);     // needle longer than hay
  cg.bind(outer);
  cg.movReg(Rax, R8);
  cg.subReg(Rax, R9);           // last start = hayLen - needleLen
  cg.cmpReg(R10, Rax);
  cg.jcc(Jcc::A, notFound);
  cg.xorReg(R11, R11);          // j
  cg.bind(inner);
  cg.cmpReg(R11, R9);
  cg.jcc(Jcc::Ae, found);
  cg.movReg(Rax, R10);
  cg.addReg(Rax, R11);
  cg.addReg(Rax, Rdi);
  cg.movzxFromMem(Rax, Rax, kStringDataOffset);   // hay[i + j]
  cg.movReg(Rcx, R11);
  cg.addReg(Rcx, Rsi);
  cg.movzxFromMem(Rcx, Rcx, kStringDataOffset);   // needle[j]
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::Ne, next);
  cg.addImm32(R11, 1);
  cg.jmp(inner);
  cg.bind(next);
  cg.addImm32(R10, 1);
  cg.jmp(outer);
  cg.bind(found);
  cg.movReg(Rax, R10);
  cg.jmp(done);
  cg.bind(notFound);
  cg.movImm(Rax, -1);
  cg.bind(done);
  cg.retInsn();
}

// `long oskel_str_prefix_eq(AotString* a, AotString* b)`: 1 when every byte of
// `b` equals the byte at the same offset in `a` and `b` is no longer than `a`,
// i.e. `b` is a prefix of `a`. The caller checks the length itself for
// `startsWith`; `endsWith` reuses it with the tail of `a` passed as `a`.
void emitStrPrefixEq(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label same = cg.newLabel();
  CodeGen::Label differs = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  cg.movFromMem(R8, Rsi, 0);   // bLen
  cg.xorReg(R10, R10);         // index
  cg.bind(loop);
  cg.cmpReg(R10, R8);
  cg.jcc(Jcc::Ae, same);
  cg.movReg(Rax, R10);
  cg.addReg(Rax, Rdi);
  cg.movzxFromMem(Rax, Rax, kStringDataOffset);
  cg.movReg(Rcx, R10);
  cg.addReg(Rcx, Rsi);
  cg.movzxFromMem(Rcx, Rcx, kStringDataOffset);
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::Ne, differs);
  cg.addImm32(R10, 1);
  cg.jmp(loop);
  cg.bind(same);
  cg.movImm(Rax, 1);
  cg.jmp(done);
  cg.bind(differs);
  cg.xorReg(Rax, Rax);         // the compared byte is not the answer
  cg.bind(done);
  cg.retInsn();
}

// Append the code point in R12 to the builder in Rbx, encoded as UTF-8 the way
// `appendUtf8` does in the hosted engine. `sbAppendByte` takes the builder and
// the byte. A code point past U+10FFFF is dropped, exactly like the hosted
// routine, which has no branch for it.
void emitAppendCodePoint(CodeGen& cg, CodeGen::Label sbAppendByte) {
  CodeGen::Label two = cg.newLabel();
  CodeGen::Label three = cg.newLabel();
  CodeGen::Label four = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.movReg(Rdi, Rbx);
  cg.cmpImm32(R12, 0x7F);
  cg.jcc(Jcc::A, two);
  cg.movReg(Rsi, R12);
  cg.callLabel(sbAppendByte);
  cg.jmp(done);

  cg.bind(two);
  cg.cmpImm32(R12, 0x7FF);
  cg.jcc(Jcc::A, three);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.shrImm8(Rsi, 6);
  cg.addImm32(Rsi, 0xC0);
  cg.callLabel(sbAppendByte);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.andImm32(Rsi, 0x3F);
  cg.addImm32(Rsi, 0x80);
  cg.callLabel(sbAppendByte);
  cg.jmp(done);

  cg.bind(three);
  cg.cmpImm32(R12, 0xFFFF);
  cg.jcc(Jcc::A, four);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.shrImm8(Rsi, 12);
  cg.addImm32(Rsi, 0xE0);
  cg.callLabel(sbAppendByte);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.shrImm8(Rsi, 6);
  cg.andImm32(Rsi, 0x3F);
  cg.addImm32(Rsi, 0x80);
  cg.callLabel(sbAppendByte);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.andImm32(Rsi, 0x3F);
  cg.addImm32(Rsi, 0x80);
  cg.callLabel(sbAppendByte);
  cg.jmp(done);

  cg.bind(four);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.shrImm8(Rsi, 18);
  cg.addImm32(Rsi, 0xF0);
  cg.callLabel(sbAppendByte);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.shrImm8(Rsi, 12);
  cg.andImm32(Rsi, 0x3F);
  cg.addImm32(Rsi, 0x80);
  cg.callLabel(sbAppendByte);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.shrImm8(Rsi, 6);
  cg.andImm32(Rsi, 0x3F);
  cg.addImm32(Rsi, 0x80);
  cg.callLabel(sbAppendByte);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.andImm32(Rsi, 0x3F);
  cg.addImm32(Rsi, 0x80);
  cg.callLabel(sbAppendByte);
  cg.bind(done);
}

// Decode the code point whose lead byte is at `text + R9` and leave it in R12
// with its byte length in R13, following `decodeCodePointAt`: a truncated
// sequence reads zero for the missing bytes, and an invalid lead byte is a
// one-byte code point equal to the byte itself. R14 is the caller's byte length
// and is left alone, so the address lives in R11.
void emitDecodeCodePoint(CodeGen& cg, X64Reg text) {
  CodeGen::Label single = cg.newLabel();
  CodeGen::Label two = cg.newLabel();
  CodeGen::Label three = cg.newLabel();
  CodeGen::Label four = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.movReg(R11, text);
  cg.addReg(R11, R9);
  cg.movzxFromMem(R12, R11, kStringDataOffset);
  cg.cmpImm32(R12, 0x80);
  cg.jcc(Jcc::Lt, single);
  cg.movReg(Rax, R12);
  cg.andImm32(Rax, 0xE0);
  cg.cmpImm32(Rax, 0xC0);
  cg.jcc(Jcc::Eq, two);
  cg.movReg(Rax, R12);
  cg.andImm32(Rax, 0xF0);
  cg.cmpImm32(Rax, 0xE0);
  cg.jcc(Jcc::Eq, three);
  cg.movReg(Rax, R12);
  cg.andImm32(Rax, 0xF8);
  cg.cmpImm32(Rax, 0xF0);
  cg.jcc(Jcc::Eq, four);
  cg.jmp(single);

  cg.bind(single);
  cg.movImm(R13, 1);
  cg.jmp(done);

  cg.bind(two);
  cg.andImm32(R12, 0x1F);
  cg.shlImm8(R12, 6);
  cg.movzxFromMem(Rax, R11, kStringDataOffset + 1);
  cg.andImm32(Rax, 0x3F);
  cg.orReg(R12, Rax);
  cg.movImm(R13, 2);
  cg.jmp(done);

  cg.bind(three);
  cg.andImm32(R12, 0x0F);
  cg.shlImm8(R12, 12);
  cg.movzxFromMem(Rax, R11, kStringDataOffset + 1);
  cg.andImm32(Rax, 0x3F);
  cg.shlImm8(Rax, 6);
  cg.orReg(R12, Rax);
  cg.movzxFromMem(Rax, R11, kStringDataOffset + 2);
  cg.andImm32(Rax, 0x3F);
  cg.orReg(R12, Rax);
  cg.movImm(R13, 3);
  cg.jmp(done);

  cg.bind(four);
  cg.andImm32(R12, 0x07);
  cg.shlImm8(R12, 18);
  cg.movzxFromMem(Rax, R11, kStringDataOffset + 1);
  cg.andImm32(Rax, 0x3F);
  cg.shlImm8(Rax, 12);
  cg.orReg(R12, Rax);
  cg.movzxFromMem(Rax, R11, kStringDataOffset + 2);
  cg.andImm32(Rax, 0x3F);
  cg.shlImm8(Rax, 6);
  cg.orReg(R12, Rax);
  cg.movzxFromMem(Rax, R11, kStringDataOffset + 3);
  cg.andImm32(Rax, 0x3F);
  cg.orReg(R12, Rax);
  cg.movImm(R13, 4);
  cg.bind(done);
}

// `void oskel_case_map(AotString* text, AotStringBuilder* sb, const void* table,
// long count, long upper)`: append `text` to `sb` with every code point mapped
// through `table`, the generated `{first, last, delta}` run array, searched by
// binary search exactly like the hosted `mapThrough`. ASCII takes the classic
// +/-0x20 shift, matching `toUpperCodePoint`/`toLowerCodePoint`.
//
// Stack slots (rbp-relative, below the 32 bytes the four pushes use):
//   -48 index, -56 table, -64 count, -72 upper, -80 the current code point,
//   -88 the text, -96 its byte length.
void emitCaseMap(CodeGen& cg, CodeGen::Label label, CodeGen::Label sbAppendByte) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label searchLoop = cg.newLabel();
  CodeGen::Label searchMid = cg.newLabel();
  CodeGen::Label searchHigh = cg.newLabel();
  CodeGen::Label searchLow = cg.newLabel();
  CodeGen::Label keep = cg.newLabel();
  CodeGen::Label append = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label asciiLower = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.push(R13);
  cg.push(R14);
  cg.subRegImm32(Rsp, 64);
  cg.movToMem(Rbp, -88, Rdi);  // text
  cg.movReg(Rbx, Rsi);         // sb
  cg.movToMem(Rbp, -56, Rdx);  // table
  cg.movToMem(Rbp, -64, Rcx);  // count
  cg.movToMem(Rbp, -72, R8);   // upper
  cg.movFromMem(Rax, Rdi, 0);  // byte length
  cg.movToMem(Rbp, -96, Rax);
  cg.movImmToMem(Rbp, -48, 0);

  cg.bind(loop);
  cg.movFromMem(R9, Rbp, -48);
  cg.movFromMem(R8, Rbp, -96);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ae, done);
  // The text base is reloaded every iteration: `emitDecodeCodePoint` writes the
  // code point into R12, which is why the text cannot live there.
  cg.movFromMem(R14, Rbp, -88);
  emitDecodeCodePoint(cg, R14);

  cg.cmpImm32(R12, 0x80);
  cg.jcc(Jcc::Ae, searchLoop);
  cg.movFromMem(Rax, Rbp, -72);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, asciiLower);
  cg.cmpImm32(R12, 'a');
  cg.jcc(Jcc::Lt, append);
  cg.cmpImm32(R12, 'z');
  cg.jcc(Jcc::Gt, append);
  cg.subRegImm32(R12, 0x20);
  cg.jmp(append);
  cg.bind(asciiLower);
  cg.cmpImm32(R12, 'A');
  cg.jcc(Jcc::Lt, append);
  cg.cmpImm32(R12, 'Z');
  cg.jcc(Jcc::Gt, append);
  cg.addImm32(R12, 0x20);
  cg.jmp(append);

  // Binary search: low = R8, high = R9, mid = R10, entry address = R11.
  cg.bind(searchLoop);
  cg.xorReg(R8, R8);
  cg.movFromMem(R9, Rbp, -64);
  cg.bind(searchMid);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, keep);
  cg.movReg(R10, R9);
  cg.subReg(R10, R8);
  cg.shrImm8(R10, 1);
  cg.addReg(R10, R8);
  cg.movReg(R11, R10);
  cg.imulImm32(R11, R11, 12);      // sizeof(UnicodeCaseRun)
  cg.movFromMem(Rax, Rbp, -56);    // table base (Rdx is not preserved here)
  cg.addReg(R11, Rax);
  cg.movFromMem32(Rax, R11, 0);    // first
  cg.cmpReg(R12, Rax);
  cg.jcc(Jcc::Lt, searchHigh);     // codePoint < first -> high = mid
  cg.movFromMem32(Rax, R11, 4);    // last
  cg.cmpReg(R12, Rax);
  cg.jcc(Jcc::A, searchLow);       // codePoint > last -> low = mid + 1
  cg.movFromMem32(Rax, R11, 8);    // delta
  // `movFromMem32` zero-extends; the delta is a signed int32, so sign-extend it
  // by shifting the 32-bit value into the high half and back arithmetically.
  cg.shlImm8(Rax, 32);
  cg.sarImm8(Rax, 32);
  cg.addReg(R12, Rax);
  cg.jmp(append);

  cg.bind(searchHigh);
  cg.movReg(R9, R10);
  cg.jmp(searchMid);
  cg.bind(searchLow);
  cg.movReg(R8, R10);
  cg.addImm32(R8, 1);
  cg.jmp(searchMid);

  cg.bind(keep);
  cg.bind(append);
  cg.movToMem(Rbp, -80, R12);
  emitAppendCodePoint(cg, sbAppendByte);
  cg.movFromMem(R12, Rbp, -80);
  cg.movFromMem(R9, Rbp, -48);
  cg.addReg(R9, R13);
  cg.movToMem(Rbp, -48, R9);
  cg.jmp(loop);
  cg.bind(done);
  cg.addImm32(Rsp, 64);
  cg.pop(R14);
  cg.pop(R13);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}


// `AotValue oskel_builtin_string_upper(AotValue* args, long argc, AotClosure* self)`:
// the bound `s.upper()`. It takes no argument and returns a new string with
// every code point's simple uppercase mapping applied -- the hosted
// `mapCase(text, true)`. The receiver is read only.
void emitStringUpperMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                           const DataBlob& msgArgc) {
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, R12, Rdx);   // the receiver AotString*
  cg.callLabel(env.sbNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);
  cg.movReg(Rdi, R12);
  cg.movReg(Rsi, Rbx);
  cg.leaRipLabel(Rdx, env.upperTable);
  cg.movImm(Rcx, env.upperCount);
  cg.movImm(R8, 1);
  cg.callLabel(env.caseMap);
  cg.movFromMem(Rdi, Rbx, kSbDataOffset);
  cg.movFromMem(Rsi, Rbx, kSbLengthOffset);
  cg.callLabel(env.strNew);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_builtin_string_lower(...)`: the bound `s.lower()`, the same
// body with the lowercase table and the `upper` flag clear.
void emitStringLowerMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                           const DataBlob& msgArgc) {
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, R12, Rdx);
  cg.callLabel(env.sbNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);
  cg.movReg(Rdi, R12);
  cg.movReg(Rsi, Rbx);
  cg.leaRipLabel(Rdx, env.lowerTable);
  cg.movImm(Rcx, env.lowerCount);
  cg.xorReg(R8, R8);
  cg.callLabel(env.caseMap);
  cg.movFromMem(Rdi, Rbx, kSbDataOffset);
  cg.movFromMem(Rsi, Rbx, kSbLengthOffset);
  cg.callLabel(env.strNew);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}



// `long oskel_str_cp_count(AotString* text, long byteLen)`: the number of code
// points in the first `byteLen` bytes, i.e. the hosted `utf8Length` of the
// prefix. `utf8Len` walks a whole `AotString`; this takes a length so the
// caller never has to shrink the receiver it borrowed.
void emitStrCpCount(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label skip = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  cg.xorReg(Rax, Rax);         // code-point count
  cg.xorReg(R8, R8);           // byte index
  cg.bind(loop);
  cg.cmpReg(R8, Rsi);
  cg.jcc(Jcc::Ae, done);
  cg.movReg(R9, R8);
  cg.addReg(R9, Rdi);
  cg.movzxFromMem(R9, R9, kStringDataOffset);
  cg.andImm32(R9, 0xC0);
  cg.cmpImm32(R9, 0x80);
  cg.jcc(Jcc::Eq, skip);
  cg.addImm32(Rax, 1);
  cg.bind(skip);
  cg.addImm32(R8, 1);
  cg.jmp(loop);
  cg.bind(done);
  cg.retInsn();
}

// `int oskel_is_space(long byte)`: 1 when `byte` is ASCII whitespace, the set
// `std::isspace` covers for the C locale the hosted engine runs in (space, \t,
// \n, \v, \f, \r). Bytes above 0x7F are never whitespace here, which is what
// `isspace` with a `char` cast to `unsigned char` also says in the C locale.
void emitIsSpace(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label yes = cg.newLabel();
  CodeGen::Label no = cg.newLabel();

  cg.bind(label);
  cg.xorReg(Rax, Rax);
  cg.cmpImm32(Rdi, 0x20);
  cg.jcc(Jcc::Eq, yes);
  cg.cmpImm32(Rdi, 0x09);
  cg.jcc(Jcc::Lt, no);
  cg.cmpImm32(Rdi, 0x0D);
  cg.jcc(Jcc::Gt, no);
  cg.bind(yes);
  cg.movImm(Rax, 1);
  cg.bind(no);
  cg.retInsn();
}

// `AotValue oskel_builtin_string_contains(...)`: the bound `s.contains(text)`,
// the hosted `text.find(needle) != npos`.
void emitStringContainsMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                              const DataBlob& msgArgc, const DataBlob& msgType) {
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label no = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(R12, Rdi, 8);                // needle
  emitLoadArrayReceiver(cg, Rbx, Rdx);       // text
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.strFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, no);
  cg.movImm(Rdx, 1);
  cg.jmp(done);
  cg.bind(no);
  cg.xorReg(Rdx, Rdx);
  cg.bind(done);
  cg.movImm(Rax, kTagBool);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  cg.movFromMem(Rdx, Rdi, 8);  // argument payload, for the quoted type
  emitTypeTailFail(cg, msgType, Rax, Rdx);
}

// `AotValue oskel_builtin_string_starts_with(...)`: the bound
// `s.startsWith(prefix)`, the hosted `startsWith`. The prefix must not be
// longer than the receiver, then the bytes must match from offset 0.
void emitStringStartsWithMethod(CodeGen& cg, CodeGen::Label label,
                                const CollectionEnv& env, const DataBlob& msgArgc,
                                const DataBlob& msgType) {
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label no = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(R12, Rdi, 8);                // prefix
  emitLoadArrayReceiver(cg, Rbx, Rdx);       // text
  cg.movFromMem(Rax, R12, 0);
  cg.movFromMem(Rcx, Rbx, 0);
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::A, no);                        // prefix longer than the text
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.strPrefixEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, no);
  cg.movImm(Rdx, 1);
  cg.jmp(done);
  cg.bind(no);
  cg.xorReg(Rdx, Rdx);
  cg.bind(done);
  cg.movImm(Rax, kTagBool);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  cg.movFromMem(Rdx, Rdi, 8);  // argument payload, for the quoted type
  emitTypeTailFail(cg, msgType, Rax, Rdx);
}

// `AotValue oskel_builtin_string_ends_with(...)`: the bound
// `s.endsWith(suffix)`, the hosted length check plus a compare of the tail.
void emitStringEndsWithMethod(CodeGen& cg, CodeGen::Label label,
                              [[maybe_unused]] const CollectionEnv& env,
                              const DataBlob& msgArgc, const DataBlob& msgType) {
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label no = cg.newLabel();
  CodeGen::Label yes = cg.newLabel();
  CodeGen::Label compareLoop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(R12, Rdi, 8);                // suffix
  emitLoadArrayReceiver(cg, Rbx, Rdx);       // text
  cg.movFromMem(Rax, R12, 0);                // suffixLen
  cg.movFromMem(Rcx, Rbx, 0);                // textLen
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::A, no);                        // suffix longer than the text
  cg.movReg(R8, Rcx);
  cg.subReg(R8, Rax);                        // textLen - suffixLen: tail offset
  // `strPrefixEq` compares from the data start of both strings, so the tail is
  // compared here byte by byte instead of handing it a raw pointer.
  cg.xorReg(R9, R9);                         // byte index
  cg.bind(compareLoop);
  cg.cmpReg(R9, Rax);
  cg.jcc(Jcc::Ae, yes);
  cg.movReg(R10, Rbx);
  cg.addReg(R10, R8);
  cg.addReg(R10, R9);
  cg.movzxFromMem(R10, R10, kStringDataOffset);
  cg.movReg(R11, R12);
  cg.addReg(R11, R9);
  cg.movzxFromMem(R11, R11, kStringDataOffset);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Ne, no);
  cg.addImm32(R9, 1);
  cg.jmp(compareLoop);
  cg.bind(yes);
  cg.movImm(Rdx, 1);
  cg.jmp(done);
  cg.bind(no);
  cg.xorReg(Rdx, Rdx);
  cg.bind(done);
  cg.movImm(Rax, kTagBool);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  cg.movFromMem(Rdx, Rdi, 8);  // argument payload, for the quoted type
  emitTypeTailFail(cg, msgType, Rax, Rdx);
}

// `AotValue oskel_builtin_string_index_of(...)`: the bound `s.indexOf(needle)`,
// which reports a *code-point* index (the hosted `utf8Length` of the prefix
// before the byte match) or -1.
void emitStringIndexOfMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                             const DataBlob& msgArgc, const DataBlob& msgType) {
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label miss = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(R12, Rdi, 8);                // needle
  emitLoadArrayReceiver(cg, Rbx, Rdx);       // text
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.strFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, miss);
  // The byte offset is in rax; count the code points before it.
  cg.movReg(Rsi, Rax);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(env.strCpCount);
  cg.movReg(Rdx, Rax);
  cg.jmp(done);
  cg.bind(miss);
  cg.movImm(Rdx, -1);
  cg.bind(done);
  cg.movImm(Rax, kTagInt);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  cg.movFromMem(Rdx, Rdi, 8);  // argument payload, for the quoted type
  emitTypeTailFail(cg, msgType, Rax, Rdx);
}

// `AotValue oskel_builtin_string_trim(AotValue* args, long argc, AotClosure* self)`:
// the bound `s.trim()`, which drops the ASCII whitespace at both ends -- the
// hosted `trim`. `trimStart` and `trimEnd` are the same body with one side
// disabled, selected by `keepStart`/`keepEnd`.
void emitStringTrimMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                          const DataBlob& msgArgc, bool trimStart, bool trimEnd) {
  // Slots: -24 length, -32 start, -40 end. r13/r14 (arena) and r15 (globals)
  // are live across `isSpace`, so the bounds stay on the stack.
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label startLoop = cg.newLabel();
  CodeGen::Label startDone = cg.newLabel();
  CodeGen::Label endLoop = cg.newLabel();
  CodeGen::Label endDone = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, R12, Rdx);       // text
  cg.movFromMem(Rax, R12, 0);                // length
  cg.movToMem(Rbp, -24, Rax);
  cg.movImmToMem(Rbp, -32, 0);               // start
  cg.movToMem(Rbp, -40, Rax);                // end

  if (trimStart) {
    cg.bind(startLoop);
    cg.movFromMem(Rax, Rbp, -32);
    cg.movFromMem(Rcx, Rbp, -24);
    cg.cmpReg(Rax, Rcx);
    cg.jcc(Jcc::Ae, startDone);
    cg.movReg(Rdi, R12);
    cg.addReg(Rdi, Rax);
    cg.movzxFromMem(Rdi, Rdi, kStringDataOffset);
    cg.callLabel(env.isSpace);
    cg.testReg(Rax);
    cg.jcc(Jcc::Eq, startDone);
    cg.movFromMem(Rax, Rbp, -32);
    cg.addImm32(Rax, 1);
    cg.movToMem(Rbp, -32, Rax);
    cg.jmp(startLoop);
  }
  cg.bind(startDone);

  if (trimEnd) {
    cg.bind(endLoop);
    cg.movFromMem(Rax, Rbp, -40);
    cg.movFromMem(Rcx, Rbp, -32);
    cg.cmpReg(Rax, Rcx);
    cg.jcc(Jcc::Be, endDone);                // end <= start: nothing left
    cg.movReg(Rdi, R12);
    cg.movReg(Rdx, Rax);
    cg.subRegImm32(Rdx, 1);
    cg.addReg(Rdi, Rdx);
    cg.movzxFromMem(Rdi, Rdi, kStringDataOffset);
    cg.callLabel(env.isSpace);
    cg.testReg(Rax);
    cg.jcc(Jcc::Eq, endDone);
    cg.movFromMem(Rax, Rbp, -40);
    cg.subRegImm32(Rax, 1);
    cg.movToMem(Rbp, -40, Rax);
    cg.jmp(endLoop);
  }
  cg.bind(endDone);

  // `strNew(data + start, end - start)`.
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rax, Rbp, -32);
  cg.addReg(Rdi, Rax);
  cg.movFromMem(Rsi, Rbp, -40);
  cg.subReg(Rsi, Rax);
  cg.callLabel(env.strNew);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_string_repeat(...)`: the bound `s.repeat(count)`, the
// hosted body: a negative count is an error, an empty text (or a zero count)
// is the empty string without entering the loop, and a result past 64 MiB is
// rejected rather than allocated.
void emitStringRepeatMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                            const DataBlob& msgArgc, const DataBlob& msgType,
                            const DataBlob& msgNegative, const DataBlob& msgTooLarge) {
  // Slots: -24 count, -32 byte length. r13/r14 (arena cursor/end) and r15
  // (globals) must survive the `sbNew`/`sbAppend` calls, so the count lives on
  // the stack rather than in a callee-saved register the arena owns.
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label negative = cg.newLabel();
  CodeGen::Label empty = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label tooLarge = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(Rax, Rdi, 8);                // count
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, negative);
  cg.movToMem(Rbp, -24, Rax);
  emitLoadArrayReceiver(cg, R12, Rdx);       // text
  cg.movFromMem(Rax, R12, 0);                // byte length
  cg.movToMem(Rbp, -32, Rax);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, empty);                    // "" repeats to ""
  cg.movFromMem(Rax, Rbp, -24);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, empty);                    // a zero count is ""
  // 64 MiB / byteLength: reject when count exceeds that, as the hosted body
  // does, before allocating anything.
  cg.movImm(Rax, 64 * 1024 * 1024);
  cg.movFromMem(Rsi, Rbp, -32);
  cg.divUnsigned(Rsi);                       // rdx:rax / rsi -> rax
  cg.movFromMem(Rcx, Rbp, -24);
  cg.cmpReg(Rcx, Rax);
  cg.jcc(Jcc::A, tooLarge);
  cg.callLabel(env.sbNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);
  cg.bind(loop);
  cg.movFromMem(Rax, Rbp, -24);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, done);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rdx, Rbp, -32);
  cg.callLabel(env.sbAppend);
  cg.movFromMem(Rax, Rbp, -24);
  cg.subRegImm32(Rax, 1);
  cg.movToMem(Rbp, -24, Rax);
  cg.jmp(loop);
  cg.bind(done);
  cg.movFromMem(Rdi, Rbx, kSbDataOffset);
  cg.movFromMem(Rsi, Rbx, kSbLengthOffset);
  cg.callLabel(env.strNew);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(empty);
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.xorReg(Rsi, Rsi);
  cg.callLabel(env.strNew);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  cg.movFromMem(Rdx, Rdi, 8);  // count payload, for the quoted type
  emitTypeTailFail(cg, msgType, Rax, Rdx);
  cg.bind(negative);
  emitFailCall(cg, env.fail, msgNegative);
  cg.bind(tooLarge);
  emitFailCall(cg, env.fail, msgTooLarge);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `long oskel_str_byte_at(AotString* text, long index)`: the byte offset of the
// `index`-th code point in `text`, or the byte length when `index` is at or past
// the end. The receiver is read only.
void emitStrByteAt(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label advance = cg.newLabel();
  CodeGen::Label advanceDone = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  cg.xorReg(Rax, Rax);         // byte index
  cg.xorReg(R8, R8);           // code-point index
  cg.bind(loop);
  cg.movFromMem(R9, Rdi, 0);
  cg.cmpReg(R8, Rsi);
  cg.jcc(Jcc::Ae, done);       // index reached
  cg.cmpReg(Rax, R9);
  cg.jcc(Jcc::Ae, done);       // past the last byte
  // Skip one whole character: the lead byte, then every continuation byte.
  cg.addImm32(Rax, 1);
  cg.bind(advance);
  cg.cmpReg(Rax, R9);
  cg.jcc(Jcc::Ae, advanceDone);
  cg.movReg(R10, Rdi);
  cg.addReg(R10, Rax);
  cg.movzxFromMem(R10, R10, kStringDataOffset);
  cg.movReg(R11, R10);
  cg.andImm32(R11, 0xC0);
  cg.cmpImm32(R11, 0x80);
  cg.jcc(Jcc::Ne, advanceDone);
  cg.addImm32(Rax, 1);
  cg.jmp(advance);
  cg.bind(advanceDone);
  cg.addImm32(R8, 1);
  cg.jmp(loop);
  cg.bind(done);
  cg.retInsn();
}

// `AotValue oskel_builtin_string_substr(...)`: the bound `s.substr(start?,
// length?)`, the hosted body. `start` counts code points, a negative start
// counts from the end, both ends clamp to the code-point length, a negative
// length is zero, and the slice stops at the end.
void emitStringSubstrMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                            const DataBlob& msgArgc, const DataBlob& msgStartType,
                            const DataBlob& msgLengthType) {
  // Slots: -24 argc, -32 start, -40 count, -48 explicit-length flag,
  // -56 code-point length, -64 byte start, -72 byte end.
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badStart = cg.newLabel();
  CodeGen::Label badLength = cg.newLabel();
  CodeGen::Label toEnd = cg.newLabel();
  CodeGen::Label haveCount = cg.newLabel();
  CodeGen::Label startNotNeg = cg.newLabel();
  CodeGen::Label startClamped = cg.newLabel();
  CodeGen::Label startStored = cg.newLabel();
  CodeGen::Label endIsLength = cg.newLabel();
  CodeGen::Label endComputed = cg.newLabel();
  CodeGen::Label endClamped = cg.newLabel();
  CodeGen::Label copy = cg.newLabel();
  CodeGen::Label ready = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 64);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Lt, badArgc);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Gt, badArgc);
  cg.movToMem(Rbp, -24, Rsi);                // argc
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badStart);
  cg.movFromMem(Rax, Rdi, 8);
  cg.movToMem(Rbp, -32, Rax);                // start
  // The optional length.
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Lt, toEnd);
  // The argument array is addressed from its last element, so args[1] sits at
  // -16 (tag) and -8 (payload) from the `rdi` the body receives.
  cg.movFromMem(Rax, Rdi, -16);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badLength);
  cg.movFromMem(Rax, Rdi, -8);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, haveCount);
  cg.xorReg(Rax, Rax);                       // a negative length is zero
  cg.bind(haveCount);
  cg.movToMem(Rbp, -40, Rax);                // count
  cg.movImmToMem(Rbp, -48, 1);               // explicit length present
  cg.jmp(ready);
  cg.bind(toEnd);
  cg.movImmToMem(Rbp, -48, 0);

  cg.bind(ready);
  emitLoadArrayReceiver(cg, R12, Rdx);       // text
  // The code-point length.
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, R12, 0);
  cg.callLabel(env.strCpCount);
  cg.movToMem(Rbp, -56, Rax);
  // start: a negative value counts from the end, then clamps to [0, length].
  cg.movFromMem(R8, Rbp, -32);
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, startNotNeg);
  cg.addReg(R8, Rax);
  cg.bind(startNotNeg);
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, startClamped);
  cg.xorReg(R8, R8);
  cg.bind(startClamped);
  cg.cmpReg(R8, Rax);
  cg.jcc(Jcc::Le, startStored);
  cg.movReg(R8, Rax);
  cg.bind(startStored);
  cg.movToMem(Rbp, -32, R8);
  // The end in code points: the explicit length when present, else to the end,
  // clamped to the length.
  cg.movFromMem(Rcx, Rbp, -48);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, endIsLength);
  cg.movFromMem(Rcx, Rbp, -40);
  cg.addReg(Rcx, R8);
  cg.jmp(endComputed);
  cg.bind(endIsLength);
  cg.movReg(Rcx, Rax);                       // no explicit length: to the end
  cg.bind(endComputed);
  cg.cmpReg(Rcx, Rax);
  cg.jcc(Jcc::Le, endClamped);
  cg.movReg(Rcx, Rax);
  cg.bind(endClamped);
  cg.movToMem(Rbp, -72, Rcx);                // end (code points)
  // Byte offsets for both ends, then copy `[start, end)`.
  cg.movReg(Rdi, R12);
  cg.movReg(Rsi, R8);
  cg.callLabel(env.strByteAt);
  cg.movToMem(Rbp, -64, Rax);                // byte start
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -72);
  cg.callLabel(env.strByteAt);
  cg.movReg(Rdx, Rax);
  cg.movFromMem(Rcx, Rbp, -64);
  cg.cmpReg(Rdx, Rcx);
  cg.jcc(Jcc::Ae, copy);
  cg.movReg(Rdx, Rcx);                       // an empty slice
  cg.bind(copy);
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.addReg(Rdi, Rcx);
  cg.movReg(Rsi, Rdx);
  cg.subReg(Rsi, Rcx);
  cg.callLabel(env.strNew);
  cg.addImm32(Rsp, 64);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badStart);
  cg.movFromMem(Rdx, Rdi, 8);   // start payload
  emitTypeTailFail(cg, msgStartType, Rax, Rdx);
  cg.bind(badLength);
  cg.movFromMem(Rdx, Rdi, -8);  // length payload
  emitTypeTailFail(cg, msgLengthType, Rax, Rdx);
}

// Appends the bytes `[startSlot, endSlot)` of `r12` to the array in
// `headerSlot` as a new `Str`, then bumps `outSlot`. Clobbers
// rax/rdx/rcx/rdi/rsi/r8/r9; `r12` (the receiver) is preserved.
void emitPushField(CodeGen& cg, const CollectionEnv& env, std::int64_t headerSlot,
                   std::int64_t startSlot, std::int64_t endSlot, std::int64_t outSlot,
                   std::int64_t scratchSlot) {
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rax, Rbp, startSlot);
  cg.addReg(Rdi, Rax);
  cg.movFromMem(Rsi, Rbp, endSlot);
  cg.movFromMem(Rax, Rbp, startSlot);
  cg.subReg(Rsi, Rax);
  cg.callLabel(env.strNew);                  // rax = tag, rdx = AotString*
  cg.movToMem(Rbp, scratchSlot, Rdx);
  cg.movFromMem(R8, Rbp, headerSlot);
  cg.movFromMem(R9, R8, kTableBlockOffset);
  cg.movFromMem(Rcx, Rbp, outSlot);
  cg.shlImm8(Rcx, 4);
  cg.addReg(R9, Rcx);
  cg.movToMem(R9, 0, Rax);
  cg.movFromMem(Rax, Rbp, scratchSlot);
  cg.movToMem(R9, 8, Rax);
  cg.movFromMem(Rcx, Rbp, outSlot);
  cg.addImm32(Rcx, 1);
  cg.movToMem(Rbp, outSlot, Rcx);
}

// Builds the array of single-code-point strings for the text in `r12`, leaving
// the header in `rax`. Shared by `chars` and by `split("")`, which the hosted
// `bindString` routes through `utf8Chars` too.
void emitCharsArray(CodeGen& cg, const CollectionEnv& env, std::int64_t countSlot,
                    std::int64_t headerSlot, std::int64_t blockSlot,
                    std::int64_t indexSlot, std::int64_t startSlot,
                    std::int64_t endSlot, std::int64_t scratchSlot,
                    CodeGen::Label oom) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, R12, 0);
  cg.callLabel(env.strCpCount);
  cg.movToMem(Rbp, countSlot, Rax);
  emitArrayAlloc(cg, env, countSlot, headerSlot, blockSlot, oom);
  cg.movImmToMem(Rbp, indexSlot, 0);
  cg.bind(loop);
  cg.movFromMem(R8, Rbp, indexSlot);
  cg.movFromMem(R9, Rbp, countSlot);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, done);
  cg.movReg(Rdi, R12);
  cg.movReg(Rsi, R8);
  cg.callLabel(env.strByteAt);
  cg.movToMem(Rbp, startSlot, Rax);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, indexSlot);
  cg.addImm32(Rsi, 1);
  cg.callLabel(env.strByteAt);
  cg.movToMem(Rbp, endSlot, Rax);
  emitPushField(cg, env, headerSlot, startSlot, endSlot, indexSlot, scratchSlot);
  cg.jmp(loop);
  cg.bind(done);
  cg.movFromMem(Rax, Rbp, headerSlot);
}

// `AotValue oskel_builtin_string_chars(...)`: the bound `s.chars()`, one
// single-code-point string per character -- the hosted `utf8Chars`.
void emitStringCharsMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                           const DataBlob& msgArgc) {
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  constexpr std::int32_t kFrameSize = 96;

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, kFrameSize);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, R12, Rdx);       // text
  emitCharsArray(cg, env, -24, -56, -64, -32, -40, -48, -72, oom);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, kFrameSize);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// Drops one trailing '\r' from the segment `[startSlot, endSlot)` of `r12`.
// Clobbers rax/rcx/r10/r11; `r12` is preserved.
void emitStripCr(CodeGen& cg, std::int64_t startSlot, std::int64_t endSlot) {
  CodeGen::Label done = cg.newLabel();

  cg.movFromMem(R10, Rbp, endSlot);
  cg.movFromMem(R11, Rbp, startSlot);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Le, done);                     // empty segment
  cg.movReg(Rcx, R12);
  cg.addReg(Rcx, R10);
  cg.subRegImm32(Rcx, 1);
  cg.movzxFromMem(Rcx, Rcx, kStringDataOffset);
  cg.cmpImm32(Rcx, 0x0D);
  cg.jcc(Jcc::Ne, done);
  cg.subRegImm32(R10, 1);
  cg.movToMem(Rbp, endSlot, R10);
  cg.bind(done);
}

// `AotValue oskel_builtin_string_lines(...)`: the bound `s.lines()`. The text
// is split on '\n' and one trailing '\r' is dropped from each segment; an
// intermediate empty segment is kept, while the segment after the last '\n' is
// emitted only when it has bytes -- the hosted body, edge for edge.
void emitStringLinesMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                           const DataBlob& msgArgc) {
  // Slots: -24 byte length, -32 scan index, -40 count, -48 header, -56 block,
  // -64 segment start, -72 segment end, -80 out index, -88 scratch.
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label countLoop = cg.newLabel();
  CodeGen::Label countNext = cg.newLabel();
  CodeGen::Label countDone = cg.newLabel();
  CodeGen::Label fillLoop = cg.newLabel();
  CodeGen::Label fillNext = cg.newLabel();
  CodeGen::Label fillDone = cg.newLabel();
  CodeGen::Label tail = cg.newLabel();
  CodeGen::Label result = cg.newLabel();
  constexpr std::int32_t kFrameSize = 96;

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, kFrameSize);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, R12, Rdx);       // text
  cg.movFromMem(Rax, R12, 0);
  cg.movToMem(Rbp, -24, Rax);                // byte length

  // Pass 1: count the newlines, then add one for a non-empty tail. The tail is
  // non-empty exactly when the text ends on something other than '\n'.
  cg.movImmToMem(Rbp, -32, 0);               // i
  cg.movImmToMem(Rbp, -40, 0);               // count
  cg.bind(countLoop);
  cg.movFromMem(R8, Rbp, -32);
  cg.movFromMem(R9, Rbp, -24);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, countDone);
  cg.movReg(R10, R12);
  cg.addReg(R10, R8);
  cg.movzxFromMem(R10, R10, kStringDataOffset);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -32, R8);
  cg.cmpImm32(R10, 0x0A);
  cg.jcc(Jcc::Ne, countNext);
  cg.movFromMem(R10, Rbp, -40);
  cg.addImm32(R10, 1);
  cg.movToMem(Rbp, -40, R10);
  cg.bind(countNext);
  cg.jmp(countLoop);
  cg.bind(countDone);
  cg.movFromMem(R9, Rbp, -24);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, tail);                     // empty text: no lines
  cg.movReg(R10, R12);
  cg.addReg(R10, R9);
  cg.subRegImm32(R10, 1);
  cg.movzxFromMem(R10, R10, kStringDataOffset);
  cg.cmpImm32(R10, 0x0A);
  cg.jcc(Jcc::Eq, tail);                     // ends on '\n': no tail
  cg.movFromMem(R10, Rbp, -40);
  cg.addImm32(R10, 1);
  cg.movToMem(Rbp, -40, R10);
  cg.bind(tail);
  emitArrayAlloc(cg, env, -40, -48, -56, oom);

  // Pass 2: emit each segment.
  cg.movImmToMem(Rbp, -32, 0);               // i
  cg.movImmToMem(Rbp, -64, 0);               // segment start
  cg.movImmToMem(Rbp, -80, 0);               // out index
  cg.bind(fillLoop);
  cg.movFromMem(R8, Rbp, -32);
  cg.movFromMem(R9, Rbp, -24);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, fillDone);
  cg.movReg(R10, R12);
  cg.addReg(R10, R8);
  cg.movzxFromMem(R10, R10, kStringDataOffset);
  cg.cmpImm32(R10, 0x0A);
  cg.jcc(Jcc::Ne, fillNext);
  // A '\n' at `i` closes the segment `[segStart, i)`.
  cg.movToMem(Rbp, -72, R8);
  emitStripCr(cg, -64, -72);
  emitPushField(cg, env, -48, -64, -72, -80, -88);
  cg.movFromMem(R8, Rbp, -32);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -32, R8);
  cg.movToMem(Rbp, -64, R8);
  cg.jmp(fillLoop);
  cg.bind(fillNext);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -32, R8);
  cg.jmp(fillLoop);
  cg.bind(fillDone);
  // The tail `[segStart, len)`, only when non-empty.
  cg.movFromMem(R8, Rbp, -64);
  cg.movFromMem(R9, Rbp, -24);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, result);
  cg.movToMem(Rbp, -72, R9);
  emitStripCr(cg, -64, -72);
  emitPushField(cg, env, -48, -64, -72, -80, -88);
  cg.bind(result);
  cg.movFromMem(Rax, Rbp, -48);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, kFrameSize);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_string_split(...)`: the bound `s.split(separator?)`.
// With no argument it splits on runs of ASCII whitespace and emits no empty
// field; with an empty separator it yields one string per code point (the
// `chars` array); otherwise every field is emitted, the empty ones a leading,
// trailing or doubled separator produces included -- the hosted body.
void emitStringSplitMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                           const DataBlob& msgArgc, const DataBlob& msgType) {
  // Slots: -24 text byte length, -32 header, -40 count, -48 out index,
  // -56 field start, -64 field end, -72 separator byte length, -80 cursor,
  // -88 separator AotString*, -96 items block, -104 scratch.
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label wsMode = cg.newLabel();
  CodeGen::Label charsMode = cg.newLabel();
  CodeGen::Label sepMode = cg.newLabel();
  CodeGen::Label wsCountLoop = cg.newLabel();
  CodeGen::Label wsCountField = cg.newLabel();
  CodeGen::Label wsCountFieldLoop = cg.newLabel();
  CodeGen::Label wsCountDone = cg.newLabel();
  CodeGen::Label wsFillLoop = cg.newLabel();
  CodeGen::Label wsFillStart = cg.newLabel();
  CodeGen::Label wsFillFieldLoop = cg.newLabel();
  CodeGen::Label wsFillEnd = cg.newLabel();
  CodeGen::Label wsFillDone = cg.newLabel();
  CodeGen::Label sepCountLoop = cg.newLabel();
  CodeGen::Label sepCountDone = cg.newLabel();
  CodeGen::Label sepFillLoop = cg.newLabel();
  CodeGen::Label sepFillLast = cg.newLabel();
  CodeGen::Label result = cg.newLabel();
  constexpr std::int32_t kFrameSize = 112;

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, kFrameSize);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Gt, badArgc);
  emitLoadArrayReceiver(cg, R12, Rdx);       // text
  cg.movFromMem(Rax, R12, 0);
  cg.movToMem(Rbp, -24, Rax);                // text byte length
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, wsMode);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(Rax, Rdi, 8);
  cg.movToMem(Rbp, -88, Rax);                // separator AotString*
  cg.movFromMem(Rax, Rax, 0);
  cg.movToMem(Rbp, -72, Rax);                // separator byte length
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, charsMode);
  cg.jmp(sepMode);

  // --- no argument: runs of ASCII whitespace, no empty fields ---
  cg.bind(wsMode);
  cg.movImmToMem(Rbp, -80, 0);               // cursor
  cg.movImmToMem(Rbp, -40, 0);               // count
  cg.bind(wsCountLoop);
  cg.movFromMem(R8, Rbp, -80);
  cg.movFromMem(R9, Rbp, -24);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, wsCountDone);
  cg.movReg(Rdi, R12);
  cg.addReg(Rdi, R8);
  cg.movzxFromMem(Rdi, Rdi, kStringDataOffset);
  cg.callLabel(env.isSpace);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, wsCountField);             // not a space: a field starts
  cg.movFromMem(R8, Rbp, -80);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -80, R8);
  cg.jmp(wsCountLoop);
  cg.bind(wsCountField);
  cg.movFromMem(R10, Rbp, -40);
  cg.addImm32(R10, 1);
  cg.movToMem(Rbp, -40, R10);
  cg.bind(wsCountFieldLoop);
  cg.movFromMem(R8, Rbp, -80);
  cg.movFromMem(R9, Rbp, -24);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, wsCountDone);
  cg.movReg(Rdi, R12);
  cg.addReg(Rdi, R8);
  cg.movzxFromMem(Rdi, Rdi, kStringDataOffset);
  cg.callLabel(env.isSpace);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, wsCountLoop);              // the field ended
  cg.movFromMem(R8, Rbp, -80);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -80, R8);
  cg.jmp(wsCountFieldLoop);
  cg.bind(wsCountDone);
  emitArrayAlloc(cg, env, -40, -32, -96, oom);
  cg.movImmToMem(Rbp, -80, 0);
  cg.movImmToMem(Rbp, -48, 0);
  cg.bind(wsFillLoop);
  cg.movFromMem(R8, Rbp, -80);
  cg.movFromMem(R9, Rbp, -24);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, wsFillDone);
  cg.movReg(Rdi, R12);
  cg.addReg(Rdi, R8);
  cg.movzxFromMem(Rdi, Rdi, kStringDataOffset);
  cg.callLabel(env.isSpace);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, wsFillStart);              // not a space: a field starts
  cg.movFromMem(R8, Rbp, -80);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -80, R8);
  cg.jmp(wsFillLoop);
  cg.bind(wsFillStart);
  cg.movToMem(Rbp, -56, R8);                 // field start
  cg.bind(wsFillFieldLoop);
  cg.movFromMem(R8, Rbp, -80);
  cg.movFromMem(R9, Rbp, -24);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, wsFillEnd);
  cg.movReg(Rdi, R12);
  cg.addReg(Rdi, R8);
  cg.movzxFromMem(Rdi, Rdi, kStringDataOffset);
  cg.callLabel(env.isSpace);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, wsFillEnd);
  cg.movFromMem(R8, Rbp, -80);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -80, R8);
  cg.jmp(wsFillFieldLoop);
  cg.bind(wsFillEnd);
  cg.movFromMem(R8, Rbp, -80);
  cg.movToMem(Rbp, -64, R8);                 // field end
  emitPushField(cg, env, -32, -56, -64, -48, -104);
  cg.jmp(wsFillLoop);
  cg.bind(wsFillDone);
  cg.movFromMem(Rax, Rbp, -32);
  cg.jmp(result);

  // --- empty separator: one string per code point ---
  cg.bind(charsMode);
  emitCharsArray(cg, env, -40, -32, -96, -48, -56, -64, -104, oom);
  cg.jmp(result);

  // --- non-empty separator: every field, empty ones included ---
  cg.bind(sepMode);
  cg.movImmToMem(Rbp, -40, 1);               // one field even with no hit
  cg.movImmToMem(Rbp, -80, 0);               // cursor
  cg.bind(sepCountLoop);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -88);
  cg.movFromMem(Rdx, Rbp, -80);
  cg.callLabel(env.strFindFrom);
  cg.cmpImm32(Rax, -1);
  cg.jcc(Jcc::Eq, sepCountDone);
  cg.movFromMem(R10, Rbp, -40);
  cg.addImm32(R10, 1);
  cg.movToMem(Rbp, -40, R10);
  cg.movFromMem(R10, Rbp, -72);
  cg.addReg(Rax, R10);
  cg.movToMem(Rbp, -80, Rax);
  cg.jmp(sepCountLoop);
  cg.bind(sepCountDone);
  emitArrayAlloc(cg, env, -40, -32, -96, oom);
  cg.movImmToMem(Rbp, -56, 0);               // field start
  cg.movImmToMem(Rbp, -48, 0);               // out index
  cg.bind(sepFillLoop);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -88);
  cg.movFromMem(Rdx, Rbp, -56);
  cg.callLabel(env.strFindFrom);
  cg.cmpImm32(Rax, -1);
  cg.jcc(Jcc::Eq, sepFillLast);
  cg.movToMem(Rbp, -64, Rax);                // field end
  emitPushField(cg, env, -32, -56, -64, -48, -104);
  // `emitPushField` clobbers rax, so reload the match offset to resume after
  // the separator rather than from whatever the push left behind.
  cg.movFromMem(Rax, Rbp, -64);
  cg.movFromMem(R10, Rbp, -72);
  cg.addReg(Rax, R10);
  cg.movToMem(Rbp, -56, Rax);
  cg.jmp(sepFillLoop);
  cg.bind(sepFillLast);
  cg.movFromMem(R8, Rbp, -24);
  cg.movToMem(Rbp, -64, R8);                 // the tail runs to the end
  emitPushField(cg, env, -32, -56, -64, -48, -104);
  cg.movFromMem(Rax, Rbp, -32);
  cg.jmp(result);

  cg.bind(result);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, kFrameSize);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  cg.movFromMem(Rdx, Rdi, 8);  // argument payload, for the quoted type
  emitTypeTailFail(cg, msgType, Rax, Rdx);
}

// `AotValue oskel_builtin_string_replace(...)`: the bound `s.replace(from, to)`.
// Every non-overlapping occurrence is replaced, resuming after the inserted
// text -- the hosted body's `position += to.size()` -- so `"aaa".replace("a",
// "aa")` yields `"aaaaaa"`. The same body serves `replaceFirst` through
// `firstOnly`, which stops after the first hit.
void emitStringReplaceMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                             const DataBlob& msgArgc, const DataBlob& msgSearchType,
                             const DataBlob& msgReplType, const DataBlob& msgEmpty,
                             bool firstOnly) {
  // Slots: -24 cursor, -32 fromLen, -40 toLen, -48 match offset, -56 from
  // AotString*, -64 to AotString*.
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badSearch = cg.newLabel();
  CodeGen::Label badRepl = cg.newLabel();
  CodeGen::Label empty = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label tail = cg.newLabel();
  constexpr std::int32_t kFrameSize = 64;

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, kFrameSize);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badSearch);
  cg.movFromMem(Rax, Rdi, 8);
  cg.movToMem(Rbp, -56, Rax);
  cg.movFromMem(Rcx, Rax, 0);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, empty);
  cg.movToMem(Rbp, -32, Rcx);
  cg.movFromMem(Rax, Rdi, -16);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badRepl);
  cg.movFromMem(Rax, Rdi, -8);
  cg.movToMem(Rbp, -64, Rax);
  cg.movFromMem(Rcx, Rax, 0);
  cg.movToMem(Rbp, -40, Rcx);
  emitLoadArrayReceiver(cg, R12, Rdx);
  cg.movImmToMem(Rbp, -24, 0);
  cg.callLabel(env.sbNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);
  cg.bind(loop);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -56);
  cg.movFromMem(Rdx, Rbp, -24);
  cg.callLabel(env.strFindFrom);
  cg.movToMem(Rbp, -48, Rax);
  cg.cmpImm32(Rax, -1);
  cg.jcc(Jcc::Eq, tail);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rax, Rbp, -24);
  cg.addReg(Rsi, Rax);
  cg.movFromMem(Rdx, Rbp, -48);
  cg.subReg(Rdx, Rax);
  cg.callLabel(env.sbAppend);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, Rbp, -64);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rdx, Rbp, -40);
  cg.callLabel(env.sbAppend);
  cg.movFromMem(Rax, Rbp, -48);
  cg.movFromMem(Rcx, Rbp, -32);
  cg.addReg(Rax, Rcx);
  cg.movToMem(Rbp, -24, Rax);
  if (firstOnly) cg.jmp(tail); else cg.jmp(loop);
  cg.bind(tail);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rax, Rbp, -24);
  cg.addReg(Rsi, Rax);
  cg.movFromMem(Rdx, R12, 0);
  cg.subReg(Rdx, Rax);
  cg.callLabel(env.sbAppend);
  cg.movFromMem(Rdi, Rbx, kSbDataOffset);
  cg.movFromMem(Rsi, Rbx, kSbLengthOffset);
  cg.callLabel(env.strNew);
  cg.addImm32(Rsp, kFrameSize);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(empty);
  emitFailCall(cg, env.fail, msgEmpty);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badSearch);
  cg.movFromMem(Rdx, Rdi, 8);   // search payload
  emitTypeTailFail(cg, msgSearchType, Rax, Rdx);
  cg.bind(badRepl);
  cg.movFromMem(Rdx, Rdi, -8);  // replacement payload
  emitTypeTailFail(cg, msgReplType, Rax, Rdx);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

void emitStringReplaceFirstMethod(CodeGen& cg, CodeGen::Label label,
                                  const CollectionEnv& env, const DataBlob& msgArgc,
                                  const DataBlob& msgSearchType,
                                  const DataBlob& msgReplType, const DataBlob& msgEmpty) {
  emitStringReplaceMethod(cg, label, env, msgArgc, msgSearchType, msgReplType, msgEmpty,
                          true);
}
// `AotValue oskel_builtin_string_pad_start(...)` / `..._pad_end(...)`: the
// bound `s.padStart(width, fill?)` and `s.padEnd(width, fill?)`, one shared
// body selected by `atStart`. `width` counts code points; a receiver already
// that wide comes back unchanged. The padding is `fill` repeated while a whole
// copy fits, then clipped to the first code points of one last copy -- the
// hosted body, so `"ab".padStart(5, "xy")` is `"xyxab"`.
void emitStringPadMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                         const DataBlob& msgArgc, const DataBlob& msgWidthType,
                         const DataBlob& msgFillType, const DataBlob& msgEmpty,
                         const DataBlob& msgTooLarge, bool atStart) {
  // Slots: -24 width, -32 current code-point length, -40 fill AotString*,
  // -48 fill code-point length, -56 remaining, -64 byte offset,
  // -72 code-point index.
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badWidth = cg.newLabel();
  CodeGen::Label badFill = cg.newLabel();
  CodeGen::Label emptyFill = cg.newLabel();
  CodeGen::Label tooLarge = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label fillReady = cg.newLabel();
  CodeGen::Label haveFill = cg.newLabel();
  CodeGen::Label wholeLoop = cg.newLabel();
  CodeGen::Label clipCheck = cg.newLabel();
  CodeGen::Label clipLoop = cg.newLabel();
  CodeGen::Label afterClip = cg.newLabel();
  CodeGen::Label same = cg.newLabel();
  constexpr std::int32_t kFrameSize = 96;

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, kFrameSize);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Lt, badArgc);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Gt, badArgc);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badWidth);
  cg.movFromMem(Rax, Rdi, 8);
  cg.movToMem(Rbp, -24, Rax);                // width
  emitLoadArrayReceiver(cg, R12, Rdx);       // text
  // An explicit fill must be a non-empty string.
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Lt, fillReady);
  cg.movFromMem(Rax, Rdi, -16);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badFill);
  cg.movFromMem(Rax, Rdi, -8);
  cg.movToMem(Rbp, -40, Rax);                // fill
  cg.movFromMem(Rcx, Rax, 0);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, emptyFill);                // an empty fill cannot pad

  cg.bind(fillReady);
  // With no explicit fill, the fill is the space constant.
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Ge, haveFill);
  cg.leaRipLabel(Rdi, env.spaceConst);
  cg.movToMem(Rbp, -40, Rdi);
  cg.bind(haveFill);
  // current = the receiver's code-point count; already wide enough is a copy.
  cg.movReg(Rdi, R12);
  cg.callLabel(env.utf8Len);
  cg.movToMem(Rbp, -32, Rax);
  cg.movFromMem(Rcx, Rbp, -24);
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::Ge, same);
  cg.movReg(Rax, Rcx);
  cg.movFromMem(Rcx, Rbp, -32);
  cg.subReg(Rax, Rcx);
  cg.movToMem(Rbp, -56, Rax);                // remaining
  cg.movImm(R10, 64 * 1024 * 1024);
  cg.cmpReg(Rax, R10);
  cg.jcc(Jcc::A, tooLarge);
  cg.movFromMem(Rdi, Rbp, -40);
  cg.callLabel(env.utf8Len);
  cg.movToMem(Rbp, -48, Rax);                // fill code-point length
  cg.callLabel(env.sbNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);
  // A trailing pad puts the text first; a leading pad puts it last.
  if (!atStart) {
    cg.movReg(Rdi, Rbx);
    cg.movReg(Rsi, R12);
    cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
    cg.movFromMem(Rdx, R12, 0);
    cg.callLabel(env.sbAppend);
  }
  // Whole copies of the fill while a whole copy still fits.
  cg.bind(wholeLoop);
  cg.movFromMem(Rax, Rbp, -56);
  cg.movFromMem(Rcx, Rbp, -48);
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::Lt, clipCheck);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, Rbp, -40);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rdx, Rbp, -40);
  cg.movFromMem(Rdx, Rdx, 0);
  cg.callLabel(env.sbAppend);
  cg.movFromMem(Rax, Rbp, -56);
  cg.movFromMem(Rcx, Rbp, -48);
  cg.subReg(Rax, Rcx);
  cg.movToMem(Rbp, -56, Rax);
  cg.jmp(wholeLoop);
  // The first `remaining` code points of one last copy.
  cg.bind(clipCheck);
  cg.movImmToMem(Rbp, -72, 0);
  cg.bind(clipLoop);
  cg.movFromMem(Rax, Rbp, -72);
  cg.movFromMem(Rcx, Rbp, -56);
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::Ge, afterClip);
  cg.movFromMem(Rdi, Rbp, -40);
  cg.movReg(Rsi, Rax);
  cg.callLabel(env.strByteAt);
  cg.movToMem(Rbp, -64, Rax);                // byte start
  cg.movFromMem(Rdi, Rbp, -40);
  cg.movFromMem(Rsi, Rbp, -72);
  cg.addImm32(Rsi, 1);
  cg.callLabel(env.strByteAt);               // byte end
  cg.movReg(Rdx, Rax);
  cg.movFromMem(Rcx, Rbp, -64);
  cg.subReg(Rdx, Rcx);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, Rbp, -40);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.addReg(Rsi, Rcx);
  cg.callLabel(env.sbAppend);
  cg.movFromMem(Rax, Rbp, -72);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbp, -72, Rax);
  cg.jmp(clipLoop);
  cg.bind(afterClip);
  if (atStart) {
    cg.movReg(Rdi, Rbx);
    cg.movReg(Rsi, R12);
    cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
    cg.movFromMem(Rdx, R12, 0);
    cg.callLabel(env.sbAppend);
  }
  cg.movFromMem(Rdi, Rbx, kSbDataOffset);
  cg.movFromMem(Rsi, Rbx, kSbLengthOffset);
  cg.callLabel(env.strNew);
  cg.addImm32(Rsp, kFrameSize);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  // Already wide enough: a copy of the receiver.
  cg.bind(same);
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rsi, R12, 0);
  cg.callLabel(env.strNew);
  cg.addImm32(Rsp, kFrameSize);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badWidth);
  cg.movFromMem(Rdx, Rdi, 8);   // width payload
  emitTypeTailFail(cg, msgWidthType, Rax, Rdx);
  cg.bind(badFill);
  cg.movFromMem(Rdx, Rdi, -8);  // fill payload
  emitTypeTailFail(cg, msgFillType, Rax, Rdx);
  cg.bind(emptyFill);
  emitFailCall(cg, env.fail, msgEmpty);
  cg.bind(tooLarge);
  emitFailCall(cg, env.fail, msgTooLarge);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}


// `AotValue oskel_builtin_string_reversed(...)`: the bound `s.reversed()`, the
// code points in reverse order -- the hosted `utf8Chars` + `std::reverse` +
// `utf8Join`. The builder appends each code point's bytes as it walks the
// receiver backwards.
void emitStringReversedMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                              const DataBlob& msgArgc) {
  // Slots: -24 cursor (just past the last byte), -32 the start of the character
  // being copied. r13/r14 (arena) and r15 (globals) are live across `sbAppend`,
  // so the walk keeps its state on the stack.
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label advance = cg.newLabel();
  CodeGen::Label copyBytes = cg.newLabel();
  CodeGen::Label copyDone = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, R12, Rdx);       // text
  cg.callLabel(env.sbNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);                       // builder
  cg.movFromMem(Rax, R12, 0);                // byte length
  cg.movToMem(Rbp, -24, Rax);                // cursor = length
  cg.bind(loop);
  cg.movFromMem(Rax, Rbp, -24);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, copyDone);
  // Step back over the continuation bytes; the lead byte starts the character.
  cg.movReg(R8, Rax);
  cg.subRegImm32(R8, 1);
  cg.bind(advance);
  cg.movReg(R9, R12);
  cg.addReg(R9, R8);
  cg.movzxFromMem(R9, R9, kStringDataOffset);
  cg.movReg(R10, R9);
  cg.andImm32(R10, 0xC0);
  cg.cmpImm32(R10, 0x80);
  cg.jcc(Jcc::Ne, copyBytes);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, copyBytes);                // a leading continuation byte
  cg.subRegImm32(R8, 1);
  cg.jmp(advance);
  cg.bind(copyBytes);
  cg.movToMem(Rbp, -32, R8);                 // character start
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.addReg(Rsi, R8);
  cg.movFromMem(Rdx, Rbp, -24);
  cg.subReg(Rdx, R8);                        // cursor - start
  cg.callLabel(env.sbAppend);
  cg.movFromMem(Rax, Rbp, -32);
  cg.movToMem(Rbp, -24, Rax);                // cursor = start
  cg.jmp(loop);
  cg.bind(copyDone);
  cg.movFromMem(Rdi, Rbx, kSbDataOffset);
  cg.movFromMem(Rsi, Rbx, kSbLengthOffset);
  cg.callLabel(env.strNew);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_builtin_array_concat(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.concat(other)`. It takes one array and returns a new array of
// the receiver's items followed by `other`'s -- the hosted `bindArray` `concat`
// (`items = self->items; items.insert(end, other...)`). A non-array argument is
// the hosted "concat() expects an array" error; both operands are read only.
void emitArrayConcatMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                           const DataBlob& msgArgc, const DataBlob& msgType) {
  CodeGen::Label copyA = cg.newLabel();
  CodeGen::Label copyADone = cg.newLabel();
  CodeGen::Label copyB = cg.newLabel();
  CodeGen::Label copyBDone = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 48);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadArrayReceiver(cg, Rbx, Rdx);   // receiver array (A)
  cg.movFromMem(Rax, Rdi, 0);            // args[0] tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(R12, Rdi, 8);            // other array (B)
  cg.movFromMem(R8, Rbx, kTableLengthOffset);
  cg.movFromMem(R9, R12, kTableLengthOffset);
  cg.addReg(R8, R9);                     // lenA + lenB
  cg.movToMem(Rbp, -40, R8);
  emitArrayAlloc(cg, env, -40, -48, -56, oom);
  cg.movFromMem(R11, Rbp, -56);          // dst cursor
  // Copy A's items.
  cg.movFromMem(R9, Rbx, kTableLengthOffset);
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.xorReg(R8, R8);
  cg.bind(copyA);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, copyADone);
  cg.movReg(Rax, R8);
  cg.shlImm8(Rax, 4);
  cg.addReg(Rax, R10);
  cg.movFromMem(Rcx, Rax, 0);
  cg.movToMem(R11, 0, Rcx);
  cg.movFromMem(Rcx, Rax, 8);
  cg.movToMem(R11, 8, Rcx);
  cg.addImm32(R11, 16);
  cg.addImm32(R8, 1);
  cg.jmp(copyA);
  cg.bind(copyADone);
  // Copy B's items.
  cg.movFromMem(R9, R12, kTableLengthOffset);
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.xorReg(R8, R8);
  cg.bind(copyB);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, copyBDone);
  cg.movReg(Rax, R8);
  cg.shlImm8(Rax, 4);
  cg.addReg(Rax, R10);
  cg.movFromMem(Rcx, Rax, 0);
  cg.movToMem(R11, 0, Rcx);
  cg.movFromMem(Rcx, Rax, 8);
  cg.movToMem(R11, 8, Rcx);
  cg.addImm32(R11, 16);
  cg.addImm32(R8, 1);
  cg.jmp(copyB);
  cg.bind(copyBDone);
  cg.movFromMem(Rdx, Rbp, -48);          // the new array
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, 48);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  emitFailCall(cg, env.fail, msgType);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_builtin_array_slice(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.slice(start?, end?)`. Both bounds are optional and default to
// `0` and the length; a negative bound counts from the end and every bound is
// clamped to `[0, length]` (the hosted `clampIndex`), then `end < start` is
// raised to `start`. The selected `[start, end)` is copied into a new array --
// the hosted `bindArray` `slice`. The receiver is unchanged.
void emitArraySliceMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                          const DataBlob& msgArgc, const DataBlob& msgStart,
                          const DataBlob& msgEnd) {
  // Stack slots: start (-24), end (-32), count (-40), header (-48), block (-56).
  // r13/r14 are the allocator's cursor/end, so nothing here may use them.
  CodeGen::Label noStart = cg.newLabel();
  CodeGen::Label haveStart = cg.newLabel();
  CodeGen::Label noEnd = cg.newLabel();
  CodeGen::Label haveEnd = cg.newLabel();
  CodeGen::Label startClamped = cg.newLabel();
  CodeGen::Label startHigh = cg.newLabel();
  CodeGen::Label endClamped = cg.newLabel();
  CodeGen::Label endHigh = cg.newLabel();
  CodeGen::Label count = cg.newLabel();
  CodeGen::Label copyLoop = cg.newLabel();
  CodeGen::Label copyDone = cg.newLabel();
  CodeGen::Label badArity = cg.newLabel();
  CodeGen::Label badStart = cg.newLabel();
  CodeGen::Label badEnd = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 48);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::A, badArity);             // argc > 2
  emitLoadArrayReceiver(cg, Rbx, Rdx);
  cg.movFromMem(R12, Rbx, kTableLengthOffset);  // length
  // start: args[0] when present, else 0.
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, noStart);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badStart);
  cg.movFromMem(Rax, Rdi, 8);
  cg.movToMem(Rbp, -24, Rax);
  cg.jmp(haveStart);
  cg.bind(noStart);
  cg.movImmToMem(Rbp, -24, 0);
  cg.bind(haveStart);
  // end: args[1] when present, else the length.
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Ne, noEnd);
  cg.movFromMem(Rax, Rdi, -16);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badEnd);
  cg.movFromMem(Rax, Rdi, -8);
  cg.movToMem(Rbp, -32, Rax);
  cg.jmp(haveEnd);
  cg.bind(noEnd);
  cg.movToMem(Rbp, -32, R12);
  cg.bind(haveEnd);
  // Clamp start into [0, length]: a negative bound counts from the end.
  cg.movFromMem(R8, Rbp, -24);
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, startClamped);
  cg.addReg(R8, R12);
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, startClamped);
  cg.xorReg(R8, R8);
  cg.bind(startClamped);
  cg.cmpReg(R8, R12);
  cg.jcc(Jcc::Le, startHigh);
  cg.movReg(R8, R12);
  cg.bind(startHigh);
  cg.movToMem(Rbp, -24, R8);
  // Clamp end into [0, length].
  cg.movFromMem(R9, Rbp, -32);
  cg.testReg(R9);
  cg.jcc(Jcc::Ge, endClamped);
  cg.addReg(R9, R12);
  cg.testReg(R9);
  cg.jcc(Jcc::Ge, endClamped);
  cg.xorReg(R9, R9);
  cg.bind(endClamped);
  cg.cmpReg(R9, R12);
  cg.jcc(Jcc::Le, endHigh);
  cg.movReg(R9, R12);
  cg.bind(endHigh);
  // end < start is raised to start.
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ge, count);
  cg.movReg(R9, R8);
  // count = end - start.
  cg.bind(count);
  cg.movReg(R10, R9);
  cg.subReg(R10, R8);
  cg.movToMem(Rbp, -40, R10);
  emitArrayAlloc(cg, env, -40, -48, -56, oom);
  cg.movFromMem(R9, Rbp, -40);          // count
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.movFromMem(R11, Rbp, -24);         // start
  cg.shlImm8(R11, 4);
  cg.addReg(R10, R11);                  // src = &block[start]
  cg.movFromMem(R11, Rbp, -56);         // dst
  cg.xorReg(R8, R8);                    // index
  cg.bind(copyLoop);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, copyDone);
  cg.movReg(Rax, R8);
  cg.shlImm8(Rax, 4);
  cg.addReg(Rax, R10);
  cg.movReg(Rdx, R8);
  cg.shlImm8(Rdx, 4);
  cg.addReg(Rdx, R11);
  cg.movFromMem(Rcx, Rax, 0);
  cg.movToMem(Rdx, 0, Rcx);
  cg.movFromMem(Rcx, Rax, 8);
  cg.movToMem(Rdx, 8, Rcx);
  cg.addImm32(R8, 1);
  cg.jmp(copyLoop);
  cg.bind(copyDone);
  cg.movFromMem(Rdx, Rbp, -48);         // the new array
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, 48);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArity);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badStart);
  cg.movFromMem(Rdx, Rdi, 8);   // start payload
  emitTypeTailFail(cg, msgStart, Rax, Rdx);
  cg.bind(badEnd);
  cg.movFromMem(Rdx, Rdi, -8);  // end payload
  emitTypeTailFail(cg, msgEnd, Rax, Rdx);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `void oskel_init_globals(long count)`: allocate `count` `AotValue` slots
// (16 bytes each) from the arena, seed them with the `Unset` tag, and keep the
// base in r15 (`kAotGlobalBase`). Called once by the chunk entry, before the
// program runs, so every global reads as unset until it is assigned -- the same
// "unset" state the hosted engine's `GETGLOBAL` checks.
//
// The tag is `Unset` and not `Nil` because `nil` is a value the program can
// assign (`var g = nil`), so a slot cleared to `Nil` could not tell "never
// assigned" from "assigned nil". `oskel_global_get` still returns the slot
// as-is; the check lives in the caller (`emitGlobalGetChecked`), which only
// emits it where a read can actually observe an unset slot.
//
// Reports "out of memory" on a null allocation.
void emitInitGlobals(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                     CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label clear = cg.newLabel();
  CodeGen::Label clearDone = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  cg.push(Rbp);
  cg.movReg(Rbp, Rsp);
  cg.push(Rbx);

  cg.movReg(Rbx, Rdi);                 // rbx = count
  cg.shlImm8(Rdi, 4);                  // count * 16 bytes
  cg.callLabel(allocLabel);            // rax = base (or null)
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(kAotGlobalBase, Rax);

  // Seed every slot with the `Unset` tag and a zero payload. `rep stosq` would
  // need rdi and rcx; a short explicit loop keeps the register use obvious and
  // the count is small (one slot per distinct global in the program).
  cg.movReg(R10, Rax);
  cg.movReg(R11, Rbx);
  cg.shlImm8(R11, 4);                  // byte count again
  cg.testReg(R11);
  cg.jcc(Jcc::Eq, clearDone);
  cg.bind(clear);
  cg.movImmToMem(R10, 0, kTagUnset);
  cg.movImmToMem(R10, 8, 0);
  cg.addImm32(R10, 16);
  cg.subRegImm32(R11, 16);
  cg.jcc(Jcc::Ne, clear);
  cg.bind(clearDone);

  cg.pop(Rbx);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `void oskel_global_set(long slot, AotValue v)`: store `v` (rsi:rdx) into
// slot `rdi` of the global table at r15. One multiply and two stores; no
// hashing, no strings.
void emitGlobalSet(CodeGen& cg, CodeGen::Label label) {
  cg.bind(label);
  cg.shlImm8(Rdi, 4);                  // slot * 16
  cg.addReg(Rdi, kAotGlobalBase);
  cg.movToMem(Rdi, 0, Rsi);            // tag
  cg.movToMem(Rdi, 8, Rdx);            // payload
  cg.retInsn();
}

// `AotValue oskel_global_get(long slot)`: load slot `rdi` of the global table
// into rax:rdx. The slot carries the `Unset` tag until assigned; the caller
// checks it with `oskel_global_get_checked` when a read can observe that state.
void emitGlobalGet(CodeGen& cg, CodeGen::Label label) {
  cg.bind(label);
  cg.shlImm8(Rdi, 4);                  // slot * 16
  cg.addReg(Rdi, kAotGlobalBase);
  cg.movFromMem(Rax, Rdi, 0);          // tag
  cg.movFromMem(Rdx, Rdi, 8);          // payload
  cg.retInsn();
}

// `AotValue oskel_global_get_checked(long slot, const AotString* msg)`:
// `oskel_global_get` plus the hosted engine's "undefined variable" check. A slot
// that `oskel_init_globals` seeded and nothing has assigned still carries the
// `Unset` tag, so reading it fails with `msg` -- the wording is built at
// lowering time, where the name is known -- and exits 70 like the hosted
// `GETGLOBAL`. The message is a plain `AotString` (an 8-byte length then the
// bytes); the failure sink takes the text and its length separately.
void emitGlobalGetChecked(CodeGen& cg, CodeGen::Label label, CodeGen::Label failDynamic) {
  CodeGen::Label unset = cg.newLabel();
  cg.bind(label);
  cg.movReg(R10, Rsi);                 // r10 = msg (survives the slot math)
  cg.shlImm8(Rdi, 4);                  // slot * 16
  cg.addReg(Rdi, kAotGlobalBase);
  cg.movFromMem(Rax, Rdi, 0);          // tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagUnset));
  cg.jcc(Jcc::Eq, unset);
  cg.movFromMem(Rdx, Rdi, 8);          // payload
  cg.retInsn();

  cg.bind(unset);
  cg.movFromMem(Rsi, R10, 0);          // length
  cg.movReg(Rdi, R10);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));  // data
  cg.jmp(failDynamic);
}

// `AotValue oskel_closure(void* code, long arity, long upvalueCount,
// void* display)`: allocate an `AotClosure` (code, arity, upvalueCount,
// display) followed by `upvalueCount` cell pointers in the arena and return it
// as a `Func` value (rax:rdx). `display` is the address of a static `AotString`
// holding the text `repr`/`str` render for the closure, or 0 to fall back to
// the literal `<fn>`. The slot area is left for the lowering to fill: it knows
// which cells the child captures and writes each pointer with a plain store,
// so the helper does not need the descriptor table.
void emitClosure(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                 CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);  // scratch slots (rsp stays 16-byte aligned)
  cg.movReg(Rbx, Rdi);      // code address
  cg.movReg(R12, Rsi);      // arity
  cg.movToMem(Rbp, -24, Rdx);  // stash upvalueCount
  cg.movToMem(Rbp, -32, Rcx);  // stash display
  cg.movReg(Rdi, Rdx);
  cg.shlImm8(Rdi, 3);       // count * 8 bytes of cell pointers
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotClosureHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);

  cg.movToMem(Rax, 0, Rbx);             // code
  cg.movToMem(Rax, 8, R12);             // arity
  cg.movFromMem(R11, Rbp, -24);
  cg.movToMem(Rax, 16, R11);            // upvalueCount
  cg.movFromMem(R11, Rbp, -32);
  cg.movToMem(Rax, static_cast<std::int64_t>(kAotClosureDisplayOffset), R11);
  cg.movReg(Rdx, Rax);                  // payload
  cg.movImm(Rax, kTagFunc);             // tag
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `AotValue* oskel_cell_new(AotValue v)`: allocate a 16-byte cell in the arena
// holding `v` (rdi:rsi) and return its address (rax). A captured local lives in
// such a cell, shared with every closure that captures it, so a write through
// the closure and a read in the defining frame observe the same value -- the
// `Cell` the hosted engine uses (PHASE 7c.2c.2).
void emitCellNew(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                 CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);   // tag
  cg.movReg(R12, Rsi);   // payload
  cg.movImm(Rdi, static_cast<std::int64_t>(sizeof(AotValue)));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, Rbx);
  cg.movToMem(Rax, 8, R12);
  cg.pop(R12);
  cg.pop(Rbx);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `AotValue oskel_call(AotValue callee, long argc, AotValue* args)`: the callee
// arrives in rdi:rsi, the argument count in rdx and the argument window in rcx.
// If the callee is a function, call its code with the lowering's function
// convention -- rdi = args, rsi = argc, rdx = the `AotClosure*` -- and return
// the result (rax:rdx) untouched. A non-function callee is a fatal error, the
// runtime counterpart of the hosted engine's "cannot call a value of type".
void emitCall(CodeGen& cg, CodeGen::Label label, const DataBlob& msgNonFunc) {
  CodeGen::Label bad = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFunc));
  cg.jcc(Jcc::Ne, bad);
  // Load the code address before clobbering rsi, then reorder the arguments for
  // the function convention (args, argc, self) through r11 as scratch:
  //   in:  rdi = tag, rsi = closure, rdx = argc, rcx = args
  //   out: rdi = args, rsi = argc, rdx = closure, r10 = code
  cg.movFromMem(R10, Rsi, 0);   // code = closure->code
  cg.movReg(R11, Rsi);          // r11 = closure
  cg.movReg(Rsi, Rdx);          // rsi = argc
  cg.movReg(Rdx, R11);          // rdx = closure
  cg.movReg(Rdi, Rcx);          // rdi = args
  cg.callReg(R10);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();

  cg.bind(bad);
  emitTypeQuoteFail(cg, msgNonFunc, Rdi, Rsi);
}

// `long oskel_str_eq(AotString* a, AotString* b)`: 1 when the two strings have
// equal length and equal bytes. Map keys are compared by content, so this is
// the primitive the map helpers build on. A leaf: it calls nothing and uses no
// callee-saved register.
void emitStrEq(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label same = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  cg.movFromMem(R8, Rdi, 0);  // lenA
  cg.movFromMem(R9, Rsi, 0);  // lenB
  cg.xorReg(Rax, Rax);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ne, done);
  cg.xorReg(R10, R10);  // index
  cg.bind(loop);
  cg.cmpReg(R10, R8);
  cg.jcc(Jcc::Ae, same);
  cg.movReg(R11, R10);
  cg.addReg(R11, Rdi);
  cg.movzxFromMem(R11, R11, kStringDataOffset);  // a byte
  cg.movReg(Rcx, R10);
  cg.addReg(Rcx, Rsi);
  cg.movzxFromMem(Rcx, Rcx, kStringDataOffset);  // b byte
  cg.cmpReg(R11, Rcx);
  cg.jcc(Jcc::Ne, done);
  cg.addImm32(R10, 1);
  cg.jmp(loop);
  cg.bind(same);
  cg.movImm(Rax, 1);
  cg.bind(done);
  cg.retInsn();
}

// --------------------------------------------------------------- map methods
//
// The bound map methods (PHASE 7c.3g), the map twin of the array methods above.
// `m.<name>` on a map builds a closure over the receiver (`bindMapMethod`); the
// call reaches one of these bodies through `oskel_call`. Every body follows the
// proto convention `AotValue entry(AotValue* args, long argc, AotClosure* self)`
// and reads its receiver from the captured cell in `self`, exactly like the
// array bodies. All ten mirror the hosted `bindMap` (src/runtime/builtins.cpp).

// A `keys`/`values`/`entries` body that materialises a new array of `count`
// items. `emitFill` writes item `i` from the map's entry block; it may read the
// receiver through `Rbx`, the index through `[Rbp-64]` and its own scratch
// through the slots below -64, and must leave rax:rdx holding the value to
// store. The loop state lives in stack slots (-40 count, -48 header, -56 block,
// -64 index) and is reloaded after every `emitFill` because the filler may
// allocate, and an allocation call clobbers every caller-saved register. `R13`
// and `R14` are the allocator's cursor/end, so nothing here may use them.
void emitMapListMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                       const DataBlob& msgArgc,
                       void (*emitFill)(CodeGen&, const CollectionEnv&)) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 128);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadMapReceiver(cg, Rbx, Rdx);
  cg.movFromMem(Rax, Rbx, kTableLengthOffset);  // count
  cg.movToMem(Rbp, -40, Rax);
  emitArrayAlloc(cg, env, -40, -48, -56, oom);
  cg.movImmToMem(Rbp, -64, 0);                  // index
  cg.bind(loop);
  cg.movFromMem(R8, Rbp, -64);
  cg.movFromMem(R9, Rbp, -40);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, done);
  emitFill(cg, env);                            // reads index from [Rbp-64]
  cg.movFromMem(R8, Rbp, -64);                  // reload: emitFill clobbers
  cg.movFromMem(R10, Rbp, -56);                 // dst block
  cg.movReg(Rcx, R8);
  cg.shlImm8(Rcx, 4);
  cg.addReg(Rcx, R10);
  cg.movToMem(Rcx, 0, Rax);
  cg.movToMem(Rcx, 8, Rdx);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -64, R8);
  cg.jmp(loop);
  cg.bind(done);
  cg.movFromMem(Rdx, Rbp, -48);         // the new array
  cg.movImm(Rax, kTagArray);
  cg.addImm32(Rsp, 128);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// Item `i` of `keys()`: the entry's key string. Reached through the `Str` tag.
void emitMapFillKeys(CodeGen& cg, const CollectionEnv& env) {
  (void)env;
  cg.movFromMem(R9, Rbp, -64);                  // index
  cg.imulImm32(R9, R9, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.addReg(R9, R10);
  cg.movFromMem(Rdx, R9, kMapEntryKeyOffset);
  cg.movImm(Rax, kTagStr);
}

// Item `i` of `values()`: the entry's value, tag and payload copied verbatim.
void emitMapFillValues(CodeGen& cg, const CollectionEnv& env) {
  (void)env;
  cg.movFromMem(R9, Rbp, -64);                  // index
  cg.imulImm32(R9, R9, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.addReg(R9, R10);
  cg.movFromMem(Rax, R9, kMapEntryValueOffset);
  cg.movFromMem(Rdx, R9, kMapEntryValueOffset + 8);
}

// Item `i` of `entries()`: a fresh two-element array `[key, value]`. Allocating
// a 2-item array for every entry is the hosted `bindMap` `entries` shape.
void emitMapFillEntries(CodeGen& cg, const CollectionEnv& env) {
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label have = cg.newLabel();
  cg.movFromMem(R9, Rbp, -64);                  // index
  cg.imulImm32(R9, R9, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.addReg(R9, R10);                           // entry
  cg.movFromMem(Rax, R9, kMapEntryKeyOffset);
  cg.movToMem(Rbp, -72, Rax);                   // key
  cg.movFromMem(Rax, R9, kMapEntryValueOffset);
  cg.movToMem(Rbp, -80, Rax);                   // value tag
  cg.movFromMem(Rax, R9, kMapEntryValueOffset + 8);
  cg.movToMem(Rbp, -88, Rax);                   // value payload
  cg.movImmToMem(Rbp, -96, 2);                  // count for emitArrayAlloc
  emitArrayAlloc(cg, env, -96, -104, -112, oom);
  cg.movFromMem(R10, Rbp, -112);                // dst block
  cg.movFromMem(Rax, Rbp, -72);                 // key payload
  cg.movImmToMem(R10, 0, kTagStr);
  cg.movToMem(R10, 8, Rax);
  cg.movFromMem(Rax, Rbp, -80);
  cg.movToMem(R10, 16, Rax);
  cg.movFromMem(Rax, Rbp, -88);
  cg.movToMem(R10, 24, Rax);
  cg.movFromMem(Rdx, Rbp, -104);
  cg.movImm(Rax, kTagArray);
  cg.jmp(have);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
  cg.bind(have);
}

void emitMapKeysMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                       const DataBlob& msgArgc) {
  emitMapListMethod(cg, label, env, msgArgc, emitMapFillKeys);
}

void emitMapValuesMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                         const DataBlob& msgArgc) {
  emitMapListMethod(cg, label, env, msgArgc, emitMapFillValues);
}

void emitMapEntriesMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                          const DataBlob& msgArgc) {
  emitMapListMethod(cg, label, env, msgArgc, emitMapFillEntries);
}

// A `has`/`get`/`delete` body: one string key argument, then a shared
// `mapFind`. `emitBody` reads the found index from `[Rbp-40]` (-1 for a miss)
// and the key from `[Rbp-24]`, and returns its result in rax:rdx.
void emitMapKeyQueryMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                           const DataBlob& msgArgc, const DataBlob& msgKeyType,
                           void (*emitBody)(CodeGen&, const CollectionEnv&)) {
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movToMem(Rbp, -24, Rdi);           // args
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadMapReceiver(cg, Rbx, Rdx);
  cg.movFromMem(Rax, Rdi, 0);           // key tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(Rax, Rdi, 8);           // key payload
  cg.movToMem(Rbp, -32, Rax);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, Rax);
  cg.callLabel(env.mapFind);
  cg.movToMem(Rbp, -40, Rax);           // index (-1 for a miss)
  emitBody(cg, env);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  cg.movFromMem(Rdx, Rdi, 8);  // key payload, for the quoted type
  emitTypeTailFail(cg, msgKeyType, Rax, Rdx);
}

// `has(key)`: true when the key is present.
void emitMapHasBody(CodeGen& cg, const CollectionEnv& env) {
  (void)env;
  CodeGen::Label no = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.movFromMem(R8, Rbp, -40);
  cg.testReg(R8);
  cg.jcc(Jcc::Lt, no);
  cg.movImm(Rdx, 1);
  cg.jmp(done);
  cg.bind(no);
  cg.xorReg(Rdx, Rdx);
  cg.bind(done);
  cg.movImm(Rax, kTagBool);
}

// `get(key, default?)`: the value, or the default (`nil` when omitted).
void emitMapGetMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                      const DataBlob& msgArgc, const DataBlob& msgKeyType) {
  CodeGen::Label noDefault = cg.newLabel();
  CodeGen::Label miss = cg.newLabel();
  CodeGen::Label nilResult = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 48);
  cg.movToMem(Rbp, -24, Rdi);           // args
  cg.movToMem(Rbp, -64, Rsi);           // argc
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Lt, badArgc);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Gt, badArgc);
  emitLoadMapReceiver(cg, Rbx, Rdx);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.movFromMem(Rax, Rdi, 0);           // key tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(Rax, Rdi, 8);           // key payload
  cg.movToMem(Rbp, -32, Rax);
  cg.movFromMem(R8, Rbp, -64);
  cg.cmpImm32(R8, 2);
  cg.jcc(Jcc::Ne, noDefault);
  cg.movFromMem(Rax, Rdi, -16);         // default tag
  cg.movToMem(Rbp, -48, Rax);
  cg.movFromMem(Rax, Rdi, -8);          // default payload
  cg.movToMem(Rbp, -56, Rax);
  cg.bind(noDefault);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, Rbp, -32);
  cg.callLabel(env.mapFind);
  cg.movToMem(Rbp, -40, Rax);           // index (-1 for a miss)
  cg.movFromMem(R8, Rbp, -40);
  cg.testReg(R8);
  cg.jcc(Jcc::Lt, miss);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R9, Rbx, kTableBlockOffset);
  cg.addReg(R8, R9);
  cg.movFromMem(Rax, R8, kMapEntryValueOffset);
  cg.movFromMem(Rdx, R8, kMapEntryValueOffset + 8);
  cg.jmp(done);
  cg.bind(miss);
  cg.movFromMem(R8, Rbp, -64);
  cg.cmpImm32(R8, 2);
  cg.jcc(Jcc::Ne, nilResult);
  cg.movFromMem(Rax, Rbp, -48);         // the default
  cg.movFromMem(Rdx, Rbp, -56);
  cg.jmp(done);
  cg.bind(nilResult);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.bind(done);
  cg.addImm32(Rsp, 48);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  cg.movFromMem(Rdx, Rdi, 8);  // key payload, for the quoted type
  emitTypeTailFail(cg, msgKeyType, Rax, Rdx);
}

// `delete(key)`: true when an entry was removed, false otherwise.
void emitMapDeleteBody(CodeGen& cg, const CollectionEnv& env) {
  CodeGen::Label no = cg.newLabel();
  CodeGen::Label noMove = cg.newLabel();
  CodeGen::Label noIndex = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.movFromMem(R8, Rbp, -40);
  cg.testReg(R8);
  cg.jcc(Jcc::Lt, no);
  // memmove: entries[i..len-2] = entries[i+1..len-1], (len-1-i)*kMapEntrySize.
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.movReg(R9, R8);
  cg.imulImm32(R9, R9, static_cast<std::int32_t>(kMapEntrySize));
  cg.addReg(R9, R10);                   // &entries[i]
  cg.movFromMem(R11, Rbx, kTableLengthOffset);
  cg.subRegImm32(R11, 1);
  cg.subReg(R11, R8);                   // len - 1 - i
  cg.testReg(R11);
  cg.jcc(Jcc::Eq, noMove);
  cg.imulImm32(R11, R11, static_cast<std::int32_t>(kMapEntrySize));
  cg.movReg(Rdi, R9);
  cg.movReg(Rsi, R9);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kMapEntrySize));
  cg.movReg(Rcx, R11);
  cg.repMovsb();
  cg.bind(noMove);
  cg.movFromMem(R11, Rbx, kTableLengthOffset);
  cg.subRegImm32(R11, 1);
  cg.movToMem(Rbx, kTableLengthOffset, R11);
  // The entries after the deleted one shifted down, so every stored index is
  // stale; refill the existing index in place when the map has one.
  cg.movFromMem(R8, Rbx, kTableHashCapacityOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, noIndex);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(env.mapIndexFill);
  cg.bind(noIndex);
  cg.movImm(Rdx, 1);
  cg.jmp(done);
  cg.bind(no);
  cg.xorReg(Rdx, Rdx);
  cg.bind(done);
  cg.movImm(Rax, kTagBool);
}

void emitMapHasMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                      const DataBlob& msgArgc, const DataBlob& msgKeyType) {
  emitMapKeyQueryMethod(cg, label, env, msgArgc, msgKeyType, emitMapHasBody);
}

void emitMapDeleteMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                         const DataBlob& msgArgc, const DataBlob& msgKeyType) {
  emitMapKeyQueryMethod(cg, label, env, msgArgc, msgKeyType, emitMapDeleteBody);
}

// `set(key, value)`: insert or overwrite, then return the receiver.
void emitMapSetMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                      const DataBlob& msgArgc, const DataBlob& msgKeyType) {
  CodeGen::Label append = cg.newLabel();
  CodeGen::Label store = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movToMem(Rbp, -24, Rdi);           // args
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadMapReceiver(cg, Rbx, Rdx);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.movFromMem(Rax, Rdi, 0);           // key tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(Rax, Rdi, 8);           // key payload
  cg.movToMem(Rbp, -32, Rax);
  cg.movFromMem(Rax, Rdi, -16);         // value tag
  cg.movToMem(Rbp, -40, Rax);
  cg.movFromMem(Rax, Rdi, -8);          // value payload
  cg.movToMem(Rbp, -48, Rax);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, Rbp, -32);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, append);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.addImm32(R10, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.jmp(store);
  cg.bind(append);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, Rbx, kTableLengthOffset);
  cg.addImm32(Rsi, 1);
  cg.movImm(Rdx, kMapEntrySize);
  cg.callLabel(env.blockEnsure);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(R8, Rbx, kTableLengthOffset);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.addReg(R8, Rax);
  cg.movFromMem(R9, Rbp, -32);          // key
  cg.movToMem(R8, kMapEntryKeyOffset, R9);
  cg.addImm32(R8, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.movFromMem(R9, Rbx, kTableLengthOffset);
  cg.addImm32(R9, 1);
  cg.movToMem(Rbx, kTableLengthOffset, R9);
  cg.movReg(R10, R8);
  // Store the new value, then index the entry. The update path jumps straight
  // to `store`, where the key already has an index slot.
  cg.movFromMem(R9, Rbp, -40);          // value tag
  cg.movToMem(R10, 0, R9);
  cg.movFromMem(R9, Rbp, -48);          // value payload
  cg.movToMem(R10, 8, R9);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, Rbx, kTableLengthOffset);
  cg.subRegImm32(Rsi, 1);
  cg.callLabel(env.mapIndexInsert);
  cg.jmp(done);
  cg.bind(store);
  cg.movFromMem(R9, Rbp, -40);          // value tag
  cg.movToMem(R10, 0, R9);
  cg.movFromMem(R9, Rbp, -48);          // value payload
  cg.movToMem(R10, 8, R9);
  cg.bind(done);
  cg.movImm(Rax, kTagMap);
  cg.movReg(Rdx, Rbx);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  cg.movFromMem(Rdx, Rdi, 8);  // key payload, for the quoted type
  emitTypeTailFail(cg, msgKeyType, Rax, Rdx);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `clear()`: drop every entry, then return the receiver.
void emitMapClearMethod(CodeGen& cg, CodeGen::Label label, [[maybe_unused]] const CollectionEnv& env,
                        const DataBlob& msgArgc) {
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label noIndex = cg.newLabel();
  CodeGen::Label clearLoop = cg.newLabel();
  CodeGen::Label clearDone = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadMapReceiver(cg, Rbx, Rdx);
  cg.movImmToMem(Rbx, kTableLengthOffset, 0);
  cg.movFromMem(R8, Rbx, kTableHashCapacityOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, noIndex);
  cg.movFromMem(R9, Rbx, kTableHashBlockOffset);
  cg.xorReg(R10, R10);
  cg.bind(clearLoop);
  cg.cmpReg(R10, R8);
  cg.jcc(Jcc::Ae, clearDone);
  cg.movReg(R11, R10);
  cg.shlImm8(R11, 3);
  cg.addReg(R11, R9);
  cg.movImmToMem(R11, 0, -1);
  cg.addImm32(R10, 1);
  cg.jmp(clearLoop);
  cg.bind(clearDone);
  cg.bind(noIndex);
  cg.movImm(Rax, kTagMap);
  cg.movReg(Rdx, Rbx);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `merge(other)`: copy every entry of `other` over the receiver, then return it.
void emitMapMergeMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                        const DataBlob& msgArgc, const DataBlob& msgType) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label append = cg.newLabel();
  CodeGen::Label store = cg.newLabel();
  CodeGen::Label next = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 64);
  cg.movToMem(Rbp, -24, Rdi);           // args
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadMapReceiver(cg, Rbx, Rdx);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.movFromMem(Rax, Rdi, 0);           // other tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(R12, Rdi, 8);           // other map
  cg.movImmToMem(Rbp, -64, 0);          // index
  cg.movFromMem(Rax, R12, kTableLengthOffset);
  cg.movToMem(Rbp, -72, Rax);           // count
  cg.bind(loop);
  cg.movFromMem(R8, Rbp, -64);
  cg.movFromMem(R9, Rbp, -72);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, done);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R8, R10);                   // &other.entries[i]
  cg.movFromMem(Rax, R8, kMapEntryKeyOffset);
  cg.movToMem(Rbp, -40, Rax);           // key
  cg.movFromMem(Rax, R8, kMapEntryValueOffset);
  cg.movToMem(Rbp, -48, Rax);           // value tag
  cg.movFromMem(Rax, R8, kMapEntryValueOffset + 8);
  cg.movToMem(Rbp, -56, Rax);           // value payload
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, Rbp, -40);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, append);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.addImm32(R10, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.jmp(store);
  cg.bind(append);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, Rbx, kTableLengthOffset);
  cg.addImm32(Rsi, 1);
  cg.movImm(Rdx, kMapEntrySize);
  cg.callLabel(env.blockEnsure);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(R8, Rbx, kTableLengthOffset);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.addReg(R8, Rax);
  cg.movFromMem(R9, Rbp, -40);
  cg.movToMem(R8, kMapEntryKeyOffset, R9);
  cg.addImm32(R8, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.movFromMem(R9, Rbx, kTableLengthOffset);
  cg.addImm32(R9, 1);
  cg.movToMem(Rbx, kTableLengthOffset, R9);
  cg.movReg(R10, R8);
  // Store the new value, then index the entry. The update path jumps straight
  // to `store`, where the key already has an index slot.
  cg.movFromMem(R9, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movFromMem(R9, Rbp, -56);
  cg.movToMem(R10, 8, R9);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, Rbx, kTableLengthOffset);
  cg.subRegImm32(Rsi, 1);
  cg.callLabel(env.mapIndexInsert);
  cg.jmp(next);
  cg.bind(store);
  cg.movFromMem(R9, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movFromMem(R9, Rbp, -56);
  cg.movToMem(R10, 8, R9);
  cg.bind(next);
  cg.movFromMem(R8, Rbp, -64);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -64, R8);
  cg.jmp(loop);
  cg.bind(done);
  cg.movImm(Rax, kTagMap);
  cg.movReg(Rdx, Rbx);
  cg.addImm32(Rsp, 64);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  emitFailCall(cg, env.fail, msgType);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `clone()`: a fresh map with the receiver's entries, as a new value.
void emitMapCloneMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                        const DataBlob& msgArgc) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 48);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadMapReceiver(cg, Rbx, Rdx);
  // Allocate a fresh map header with the receiver's capacity, then copy every
  // 24-byte entry.
  cg.movFromMem(R12, Rbx, kTableLengthOffset);
  cg.movToMem(Rbp, -40, R12);           // count
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotMapHeaderSize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbp, -48, Rax);           // new header
  cg.movImmToMem(Rax, kTableLengthOffset, 0);
  cg.movImmToMem(Rax, kTableCapacityOffset, 0);
  cg.movImmToMem(Rax, kTableBlockOffset, 0);
  cg.movImmToMem(Rax, kTableHashCapacityOffset, 0);
  cg.movImmToMem(Rax, kTableHashBlockOffset, 0);
  // A clone of a class instance keeps its class identity and method table.
  cg.movFromMem(R8, Rbx, kTableClassNameOffset);
  cg.movToMem(Rax, kTableClassNameOffset, R8);
  cg.movFromMem(R8, Rbx, kTableMethodsOffset);
  cg.movToMem(Rax, kTableMethodsOffset, R8);
  cg.movFromMem(Rdi, Rbp, -40);
  cg.imulImm32(Rdi, Rdi, static_cast<std::int32_t>(kMapEntrySize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbp, -56, Rax);           // new block
  cg.movFromMem(R8, Rbp, -48);
  cg.movFromMem(R9, Rbp, -40);
  cg.movToMem(R8, kTableLengthOffset, R9);
  cg.movToMem(R8, kTableCapacityOffset, R9);
  cg.movToMem(R8, kTableBlockOffset, Rax);
  cg.movFromMem(R9, Rbp, -40);          // count
  cg.movFromMem(R10, Rbx, kTableBlockOffset);   // src
  cg.movFromMem(R11, Rbp, -56);                 // dst
  cg.movImmToMem(Rbp, -64, 0);                  // index
  cg.bind(loop);
  cg.movFromMem(R8, Rbp, -64);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, done);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.movReg(Rdx, R8);
  cg.movReg(Rax, R8);
  cg.addReg(Rax, R10);
  cg.addReg(Rdx, R11);
  cg.movFromMem(Rcx, Rax, 0);
  cg.movToMem(Rdx, 0, Rcx);
  cg.movFromMem(Rcx, Rax, 8);
  cg.movToMem(Rdx, 8, Rcx);
  cg.movFromMem(Rcx, Rax, 16);
  cg.movToMem(Rdx, 16, Rcx);
  cg.movFromMem(R8, Rbp, -64);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -64, R8);
  cg.jmp(loop);
  cg.bind(done);
  cg.movFromMem(Rdx, Rbp, -48);
  cg.movImm(Rax, kTagMap);
  cg.addImm32(Rsp, 48);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `oskel_member_get`'s map branch: the constant name has missed the entry
// lookup, so it is `.length`, one of the ten bound methods, or no member.
void emitMapMemberGet(CodeGen& cg, const CollectionEnv& env, CodeGen::Label memberDone) {
  CodeGen::Label bind = cg.newLabel();
  CodeGen::Label b0 = cg.newLabel();
  CodeGen::Label b1 = cg.newLabel();
  CodeGen::Label b2 = cg.newLabel();
  CodeGen::Label b3 = cg.newLabel();
  CodeGen::Label b4 = cg.newLabel();
  CodeGen::Label b5 = cg.newLabel();
  CodeGen::Label b6 = cg.newLabel();
  CodeGen::Label b7 = cg.newLabel();
  CodeGen::Label b8 = cg.newLabel();
  CodeGen::Label b9 = cg.newLabel();
  CodeGen::Label len = cg.newLabel();
  CodeGen::Label fail = cg.newLabel();

  // A class instance may have a user method under this name. The method table
  // is checked before the built-in map members, so a method named `length`
  // wins over the map length, exactly like the hosted `ops::memberGet`.
  CodeGen::Label noMethods = cg.newLabel();
  cg.movFromMem(Rdi, R12, kTableMethodsOffset);
  cg.testReg(Rdi);
  cg.jcc(Jcc::Eq, noMethods);
  cg.movFromMem(Rsi, Rbp, -24);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, noMethods);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableMethodsOffset);
  cg.movFromMem(R10, R10, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.movFromMem(Rax, R10, kMapEntryValueOffset);
  cg.movFromMem(Rdx, R10, kMapEntryValueOffset + 8);
  cg.jmp(memberDone);
  cg.bind(noMethods);

  auto nameIs = [&](CodeGen::Label constLabel, CodeGen::Label target) {
    cg.movFromMem(Rdi, Rbp, -24);
    cg.leaRipLabel(Rsi, constLabel);
    cg.callLabel(env.strEq);
    cg.testReg(Rax);
    cg.jcc(Jcc::Ne, target);
  };

  nameIs(env.keysConst, b0);
  nameIs(env.valuesConst, b1);
  nameIs(env.entriesConst, b2);
  nameIs(env.hasConst, b3);
  nameIs(env.getConst, b4);
  nameIs(env.setConst, b5);
  nameIs(env.deleteConst, b6);
  nameIs(env.clearConst, b7);
  nameIs(env.mergeConst, b8);
  nameIs(env.cloneConst, b9);
  nameIs(env.lengthConst, len);
  cg.jmp(fail);
  cg.bind(b0); cg.movImm(Rdx, 0); cg.jmp(bind);
  cg.bind(b1); cg.movImm(Rdx, 1); cg.jmp(bind);
  cg.bind(b2); cg.movImm(Rdx, 2); cg.jmp(bind);
  cg.bind(b3); cg.movImm(Rdx, 3); cg.jmp(bind);
  cg.bind(b4); cg.movImm(Rdx, 4); cg.jmp(bind);
  cg.bind(b5); cg.movImm(Rdx, 5); cg.jmp(bind);
  cg.bind(b6); cg.movImm(Rdx, 6); cg.jmp(bind);
  cg.bind(b7); cg.movImm(Rdx, 7); cg.jmp(bind);
  cg.bind(b8); cg.movImm(Rdx, 8); cg.jmp(bind);
  cg.bind(b9); cg.movImm(Rdx, 9);
  cg.bind(bind);
  cg.movImm(Rdi, kTagMap);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.bindMapMethod);
  cg.jmp(memberDone);
  cg.bind(len);
  cg.movFromMem(Rdx, R12, kTableLengthOffset);
  cg.movImm(Rax, kTagInt);
  cg.jmp(memberDone);
  // An unknown map member is nil, matching the hosted `ops::memberGet` (maps
  // differ from arrays here: the map branch returns `Value()` rather than
  // failing).
  cg.bind(fail);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
}

// `AotValue oskel_member_get(AotValue obj, AotValue name)`: the constant-name
// form `obj.name`. Arrays, maps, strings, ranges and errors each answer their
// own members; anything else is "type has no member".
void emitMemberGet(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label arr = cg.newLabel();
  CodeGen::Label str = cg.newLabel();
  CodeGen::Label range = cg.newLabel();
  CodeGen::Label error = cg.newLabel();
  CodeGen::Label errorMessage = cg.newLabel();
  CodeGen::Label errorCode = cg.newLabel();
  CodeGen::Label rangeAsc = cg.newLabel();
  CodeGen::Label rangeSat = cg.newLabel();
  CodeGen::Label rangeUse = cg.newLabel();
  CodeGen::Label rangeSatCheck = cg.newLabel();
  CodeGen::Label rangeNotLength = cg.newLabel();
  CodeGen::Label bindToArray = cg.newLabel();
  CodeGen::Label bindRangeContains = cg.newLabel();
  CodeGen::Label bindRange = cg.newLabel();
  CodeGen::Label mapMiss = cg.newLabel();
  CodeGen::Label isLength = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failMember = cg.newLabel();
  CodeGen::Label failIndex = cg.newLabel();
  CodeGen::Label bindPush = cg.newLabel();
  CodeGen::Label bindPop = cg.newLabel();
  CodeGen::Label bindShift = cg.newLabel();
  CodeGen::Label bindUnshift = cg.newLabel();
  CodeGen::Label bindInsert = cg.newLabel();
  CodeGen::Label bindRemoveAt = cg.newLabel();
  CodeGen::Label bindContains = cg.newLabel();
  CodeGen::Label bindIndexOf = cg.newLabel();
  CodeGen::Label bindClear = cg.newLabel();
  CodeGen::Label bindSlice = cg.newLabel();
  CodeGen::Label bindConcat = cg.newLabel();
  CodeGen::Label bindReversed = cg.newLabel();
  CodeGen::Label bindClone = cg.newLabel();
  CodeGen::Label bindMap = cg.newLabel();
  CodeGen::Label bindFilter = cg.newLabel();
  CodeGen::Label bindEach = cg.newLabel();
  CodeGen::Label bindFind = cg.newLabel();
  CodeGen::Label bindAny = cg.newLabel();
  CodeGen::Label bindAll = cg.newLabel();
  CodeGen::Label bindReduce = cg.newLabel();
  CodeGen::Label bindSum = cg.newLabel();
  CodeGen::Label bindJoin = cg.newLabel();
  CodeGen::Label bindSort = cg.newLabel();
  CodeGen::Label bindSorted = cg.newLabel();
  CodeGen::Label bind = cg.newLabel();
  // The string methods, in selector order: the names are what `member_get`
  // compares against and the labels are where each match binds its closure.
  // The order is the order `bindStringMethod`'s method table uses, so the
  // selector and the name list cannot drift apart.
  const std::array<CodeGen::Label, 19> kStringMethodConsts = {
      env.upperConst, env.lowerConst, env.trimConst, env.trimStartConst,
      env.trimEndConst, env.containsStrConst, env.startsWithConst,
      env.endsWithConst, env.indexOfStrConst, env.substrConst,
      env.reversedStrConst, env.repeatConst, env.splitConst, env.linesConst,
      env.charsConst, env.replaceConst, env.replaceFirstConst,
      env.padStartConst, env.padEndConst};
  std::array<CodeGen::Label, 19> strBind;
  for (auto& l : strBind) l = cg.newLabel();
  CodeGen::Label strMethods = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(Rbx, Rdi);          // obj tag
  cg.movReg(R12, Rsi);          // obj payload
  cg.movToMem(Rbp, -24, Rcx);   // name payload (AotString*)
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, arr);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, str);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, range);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagError));
  cg.jcc(Jcc::Eq, error);
  cg.jmp(failMember);

  // An error has two members: `message` (the message string) and `code` (the
  // integer code). Any other name is "type has no member", the fixed prefix of
  // the hosted `memberGet`.
  cg.bind(error);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.messageConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, errorMessage);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.codeConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, errorCode);
  cg.jmp(failMember);
  cg.bind(errorMessage);
  cg.movFromMem(Rdx, R12, kErrorMessageOffset);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);
  cg.bind(errorCode);
  cg.movFromMem(Rdx, R12, kErrorCodeOffset);
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  cg.bind(map);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -24);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, mapMiss);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.movFromMem(Rax, R10, kMapEntryValueOffset);
  cg.movFromMem(Rdx, R10, kMapEntryValueOffset + 8);
  cg.jmp(done);
  cg.bind(mapMiss);
  emitMapMemberGet(cg, env, done);
  cg.jmp(done);

  cg.bind(arr);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.lengthConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, isLength);
  // Not `.length`: each other name that matches a bound array method builds a
  // closure over the receiver, exactly like the hosted `bindArray`. The
  // selector is the method's index in `bindArrayMethod`'s table; any other
  // name is "no such member".
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.pushConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindPush);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.popConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindPop);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.shiftConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindShift);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.unshiftConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindUnshift);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.insertConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindInsert);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.removeAtConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindRemoveAt);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.containsConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindContains);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.indexOfConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindIndexOf);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.clearConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindClear);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.sliceConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindSlice);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.concatConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindConcat);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.reversedConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindReversed);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.cloneConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindClone);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.mapConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindMap);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.filterConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindFilter);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.eachConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindEach);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.findConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindFind);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.anyConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindAny);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.allConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindAll);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.reduceConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindReduce);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.sumConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindSum);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.joinConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindJoin);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.sortConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindSort);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.sortedConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindSorted);
  cg.jmp(failMember);

  // Every `bind*` block loads the receiver (r12), sets the selector in rdx and
  // calls the shared binder.
  cg.bind(bindPush);
  cg.movImm(Rdx, 0);            // push
  cg.jmp(bind);
  cg.bind(bindPop);
  cg.movImm(Rdx, 1);            // pop
  cg.jmp(bind);
  cg.bind(bindShift);
  cg.movImm(Rdx, 2);            // shift
  cg.jmp(bind);
  cg.bind(bindUnshift);
  cg.movImm(Rdx, 3);            // unshift
  cg.jmp(bind);
  cg.bind(bindInsert);
  cg.movImm(Rdx, 4);            // insert
  cg.jmp(bind);
  cg.bind(bindRemoveAt);
  cg.movImm(Rdx, 5);            // removeAt
  cg.jmp(bind);
  cg.bind(bindContains);
  cg.movImm(Rdx, 6);            // contains
  cg.jmp(bind);
  cg.bind(bindIndexOf);
  cg.movImm(Rdx, 7);            // indexOf
  cg.jmp(bind);
  cg.bind(bindClear);
  cg.movImm(Rdx, 8);            // clear
  cg.jmp(bind);
  cg.bind(bindSlice);
  cg.movImm(Rdx, 9);            // slice
  cg.jmp(bind);
  cg.bind(bindConcat);
  cg.movImm(Rdx, 10);           // concat
  cg.jmp(bind);
  cg.bind(bindReversed);
  cg.movImm(Rdx, 11);           // reversed
  cg.jmp(bind);
  cg.bind(bindClone);
  cg.movImm(Rdx, 12);           // clone
  cg.jmp(bind);
  cg.bind(bindMap);
  cg.movImm(Rdx, 13);           // map
  cg.jmp(bind);
  cg.bind(bindFilter);
  cg.movImm(Rdx, 14);           // filter
  cg.jmp(bind);
  cg.bind(bindEach);
  cg.movImm(Rdx, 15);           // each
  cg.jmp(bind);
  cg.bind(bindFind);
  cg.movImm(Rdx, 16);           // find
  cg.jmp(bind);
  cg.bind(bindAny);
  cg.movImm(Rdx, 17);           // any
  cg.jmp(bind);
  cg.bind(bindAll);
  cg.movImm(Rdx, 18);           // all
  cg.jmp(bind);
  cg.bind(bindReduce);
  cg.movImm(Rdx, 19);           // reduce
  cg.jmp(bind);
  cg.bind(bindSum);
  cg.movImm(Rdx, 20);           // sum
  cg.jmp(bind);
  cg.bind(bindJoin);
  cg.movImm(Rdx, 21);           // join
  cg.jmp(bind);
  cg.bind(bindSort);
  cg.movImm(Rdx, 22);           // sort
  cg.jmp(bind);
  cg.bind(bindSorted);
  cg.movImm(Rdx, 23);           // sorted
  cg.bind(bind);
  cg.movImm(Rdi, kTagArray);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.bindArrayMethod);
  cg.jmp(done);

  cg.bind(str);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.lengthConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, strMethods);
  cg.movReg(Rdi, R12);
  cg.callLabel(env.utf8Len);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  // A string method: the twelve bound names, tried in selector order. Each
  // comparison goes through `strEq`; a match binds the closure and the call
  // site invokes it. An unknown name still reports "type has no member".
  cg.bind(strMethods);
  for (std::size_t i = 0; i < kStringMethodConsts.size(); ++i) {
    cg.movFromMem(Rdi, Rbp, -24);
    cg.leaRipLabel(Rsi, kStringMethodConsts[i]);
    cg.callLabel(env.strEq);
    cg.testReg(Rax);
    cg.jcc(Jcc::Ne, strBind[i]);
  }
  cg.jmp(failMember);
  for (std::size_t i = 0; i < kStringMethodConsts.size(); ++i) {
    cg.bind(strBind[i]);
    cg.movImm(Rdi, kTagStr);
    cg.movReg(Rsi, R12);
    cg.movImm(Rdx, static_cast<std::int64_t>(i));
    cg.callLabel(env.bindStringMethod);
    cg.jmp(done);
  }

  cg.bind(isLength);
  cg.movFromMem(Rdx, R12, kTableLengthOffset);
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  // A range's `.length`: the distance between the bounds, plus one when
  // inclusive, saturated at INT64_MAX so an overflowing span stays defined.
  cg.bind(range);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.lengthConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, rangeNotLength);
  cg.movFromMem(R8, R12, kRangeStartOffset);
  cg.movFromMem(R9, R12, kRangeEndOffset);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ge, rangeAsc);
  cg.movReg(R10, R8);
  cg.subReg(R10, R9);              // descending: start - end
  cg.jmp(rangeSat);
  cg.bind(rangeAsc);
  cg.movReg(R10, R9);
  cg.subReg(R10, R8);              // ascending: end - start
  cg.bind(rangeSat);
  cg.movFromMem(R8, R12, kRangeInclusiveOffset);
  cg.testReg(R8);
  // An exclusive range keeps the distance as-is, but it still has to pass the
  // saturation check: a span wider than INT64_MAX overflows either way.
  cg.jcc(Jcc::Eq, rangeSatCheck);
  cg.movImm(R11, -1);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Eq, rangeSatCheck);  // distance == UINT64_MAX: saturate
  cg.addImm32(R10, 1);
  cg.bind(rangeSatCheck);
  cg.movImm(R11, -1);
  cg.shrImm8(R11, 1);              // INT64_MAX
  cg.cmpReg(R10, R11);
  // The magnitude is unsigned: a span wider than INT64_MAX sets the sign bit,
  // so a signed `le` would leave it unsaturated (and a huge span reads as -2).
  cg.jcc(Jcc::Be, rangeUse);
  cg.movReg(R10, R11);             // saturate
  cg.bind(rangeUse);
  cg.movReg(Rdx, R10);
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  // The two bound range methods. The receiver travels in the closure's single
  // upvalue cell, exactly like the array and string methods, so the binder is
  // the shared `bindArrayMethod` emitter with its own body table.
  cg.bind(rangeNotLength);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.toArrayConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindToArray);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.containsConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bindRangeContains);
  cg.jmp(failMember);
  cg.bind(bindToArray);
  cg.movImm(Rdx, 0);               // toArray
  cg.jmp(bindRange);
  cg.bind(bindRangeContains);
  cg.movImm(Rdx, 1);               // contains
  cg.bind(bindRange);
  cg.movImm(Rdi, kTagRange);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.bindRangeMethod);
  cg.jmp(done);

  cg.bind(done);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(failMember);
  cg.movFromMem(R8, Rbp, -24);  // the member name AotString*
  emitNoMemberFail(cg, Rbx, R12, R8);
  cg.bind(failIndex);
  emitTypeQuoteFail(cg, *env.msgBadIndex, Rbx, R12);
}


// `long oskel_str_hash(AotString* s)`: FNV-1a over the bytes. A leaf, and the
// exact hash is private to the AOT runtime: `strEq` still decides equality.
void emitStrHash(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  cg.movFromMem(R8, Rdi, 0);           // len
  cg.movReg(R9, Rdi);
  cg.addImm32(R9, static_cast<std::int32_t>(kStringDataOffset));
  cg.movImm(Rax, 2166136261);          // FNV-1a 32-bit offset basis
  cg.xorReg(R10, R10);                 // i
  cg.bind(loop);
  cg.cmpReg(R10, R8);
  cg.jcc(Jcc::Ae, done);
  cg.movReg(R11, R9);
  cg.addReg(R11, R10);
  cg.movzxFromMem(R11, R11, 0);        // byte
  cg.xorReg(Rax, R11);
  cg.movImm(Rcx, 16777619);            // FNV prime
  cg.imulReg(Rax, Rcx);
  cg.addImm32(R10, 1);
  cg.jmp(loop);
  cg.bind(done);
  cg.retInsn();
}

// `void oskel_map_index_build(AotMap* m)`: allocate an open-addressing index
// sized for the current length and fill it. The index stores entry indices, so
// the insertion-ordered entry array stays the iteration order.
void emitMapIndexBuild(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label grow = cg.newLabel();
  CodeGen::Label growDone = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(Rbx, Rdi);  // m
  // cap = 16; while (length * 10 >= cap * 7) cap *= 2.
  cg.movImm(R8, 16);
  cg.bind(grow);
  cg.movFromMem(R9, Rbx, kTableLengthOffset);
  cg.imulImm32(R9, R9, 10);
  cg.movReg(R10, R8);
  cg.imulImm32(R10, R10, 7);
  cg.cmpReg(R9, R10);
  cg.jcc(Jcc::Lt, growDone);
  cg.addReg(R8, R8);
  cg.jmp(grow);
  cg.bind(growDone);
  cg.movToMem(Rbp, -24, R8);  // cap
  cg.movReg(Rdi, R8);
  cg.shlImm8(Rdi, 3);
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(R8, Rbp, -24);
  cg.movToMem(Rbx, kTableHashCapacityOffset, R8);
  cg.movToMem(Rbx, kTableHashBlockOffset, Rax);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(env.mapIndexFill);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `void oskel_map_index_fill(AotMap* m)`: clear the existing index and insert
// every entry. Used by `map_index_build` after it allocates, and by `delete`,
// where reusing the table keeps a delete loop from allocating on every step.
void emitMapIndexFill(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label fill = cg.newLabel();
  CodeGen::Label fillDone = cg.newLabel();
  CodeGen::Label build = cg.newLabel();
  CodeGen::Label probe = cg.newLabel();
  CodeGen::Label empty = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);  // m
  cg.movFromMem(R8, Rbx, kTableHashCapacityOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, done);
  cg.movFromMem(R9, Rbx, kTableHashBlockOffset);
  cg.xorReg(R11, R11);
  cg.bind(fill);
  cg.cmpReg(R11, R8);
  cg.jcc(Jcc::Ae, fillDone);
  cg.movReg(R12, R11);
  cg.shlImm8(R12, 3);
  cg.addReg(R12, R9);
  cg.movImmToMem(R12, 0, -1);
  cg.addImm32(R11, 1);
  cg.jmp(fill);
  cg.bind(fillDone);
  cg.movImmToMem(Rbp, -24, 0);  // i
  cg.bind(build);
  cg.movFromMem(R8, Rbp, -24);
  cg.movFromMem(R9, Rbx, kTableLengthOffset);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, done);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R9, Rbx, kTableBlockOffset);
  cg.addReg(R8, R9);
  cg.movFromMem(Rdi, R8, kMapEntryKeyOffset);
  cg.callLabel(env.strHash);
  cg.movFromMem(R9, Rbx, kTableHashCapacityOffset);
  cg.subRegImm32(R9, 1);
  cg.andReg(R9, Rax);
  cg.movFromMem(R10, Rbx, kTableHashBlockOffset);
  cg.bind(probe);
  cg.movReg(R11, R9);
  cg.shlImm8(R11, 3);
  cg.addReg(R11, R10);
  cg.movFromMem(R11, R11, 0);
  cg.cmpImm32(R11, -1);
  cg.jcc(Jcc::Eq, empty);
  cg.addImm32(R9, 1);
  cg.movFromMem(R11, Rbx, kTableHashCapacityOffset);
  cg.subRegImm32(R11, 1);
  cg.andReg(R9, R11);
  cg.jmp(probe);
  cg.bind(empty);
  cg.movReg(R11, R9);
  cg.shlImm8(R11, 3);
  cg.addReg(R11, R10);
  cg.movFromMem(R8, Rbp, -24);
  cg.movToMem(R11, 0, R8);
  cg.movFromMem(R8, Rbp, -24);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -24, R8);
  cg.jmp(build);
  cg.bind(done);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `void oskel_map_index_insert(AotMap* m, long index)`: add one entry to an
// existing index, growing and rebuilding first when the load factor would
// exceed 7/10. A map with no index yet is left alone; `map_find` builds it.
void emitMapIndexInsert(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label put = cg.newLabel();
  CodeGen::Label probe = cg.newLabel();
  CodeGen::Label empty = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);  // m
  cg.movReg(R12, Rsi);  // index
  cg.movFromMem(R8, Rbx, kTableHashCapacityOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, done);
  cg.movFromMem(R9, Rbx, kTableLengthOffset);
  cg.imulImm32(R9, R9, 10);
  cg.movReg(R10, R8);
  cg.imulImm32(R10, R10, 7);
  cg.cmpReg(R9, R10);
  cg.jcc(Jcc::Lt, put);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(env.mapIndexBuild);
  cg.jmp(done);
  cg.bind(put);
  cg.movReg(R9, R12);
  cg.imulImm32(R9, R9, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.addReg(R9, R10);
  cg.movFromMem(Rdi, R9, kMapEntryKeyOffset);
  cg.callLabel(env.strHash);
  cg.movFromMem(R9, Rbx, kTableHashCapacityOffset);
  cg.subRegImm32(R9, 1);
  cg.andReg(R9, Rax);
  cg.movFromMem(R10, Rbx, kTableHashBlockOffset);
  cg.bind(probe);
  cg.movReg(R11, R9);
  cg.shlImm8(R11, 3);
  cg.addReg(R11, R10);
  cg.movFromMem(R11, R11, 0);
  cg.cmpImm32(R11, -1);
  cg.jcc(Jcc::Eq, empty);
  cg.addImm32(R9, 1);
  cg.movFromMem(R11, Rbx, kTableHashCapacityOffset);
  cg.subRegImm32(R11, 1);
  cg.andReg(R9, R11);
  cg.jmp(probe);
  cg.bind(empty);
  cg.movReg(R11, R9);
  cg.shlImm8(R11, 3);
  cg.addReg(R11, R10);
  cg.movToMem(R11, 0, R12);
  cg.bind(done);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `long oskel_map_find(AotMap* m, AotString* key)`: the index of `key` in the
// map's entry array, or -1. A map with at least eight entries gets a lazy hash
// index; smaller maps keep the linear scan, which is cheaper than allocating
// one. The loop state survives the `strEq`/`strHash` calls on the stack, since
// those calls clobber r8-r11.
void emitMapFind(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label linear = cg.newLabel();
  CodeGen::Label linearLoop = cg.newLabel();
  CodeGen::Label linearFound = cg.newLabel();
  CodeGen::Label hashLookup = cg.newLabel();
  CodeGen::Label hashLoop = cg.newLabel();
  CodeGen::Label hashFound = cg.newLabel();
  CodeGen::Label notFound = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 64);
  cg.movReg(Rbx, Rdi);  // m
  cg.movReg(R12, Rsi);  // key
  cg.movFromMem(R8, Rbx, kTableHashCapacityOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, hashLookup);
  cg.movFromMem(R8, Rbx, kTableLengthOffset);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kMapHashThreshold));
  cg.jcc(Jcc::Lt, linear);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(env.mapIndexBuild);
  // fall through to the hash path
  cg.bind(hashLookup);
  cg.movFromMem(R8, Rbx, kTableHashCapacityOffset);
  cg.movToMem(Rbp, -24, R8);  // cap
  cg.movReg(Rdi, R12);
  cg.callLabel(env.strHash);
  cg.movFromMem(R9, Rbp, -24);
  cg.subRegImm32(R9, 1);
  cg.andReg(R9, Rax);
  cg.movToMem(Rbp, -32, R9);  // slot
  cg.bind(hashLoop);
  cg.movFromMem(R9, Rbp, -32);
  cg.movFromMem(R10, Rbx, kTableHashBlockOffset);
  cg.movReg(R11, R9);
  cg.shlImm8(R11, 3);
  cg.addReg(R11, R10);
  cg.movFromMem(R11, R11, 0);  // entry index
  cg.cmpImm32(R11, -1);
  cg.jcc(Jcc::Eq, notFound);
  cg.movToMem(Rbp, -40, R11);  // index
  cg.imulImm32(R11, R11, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.addReg(R10, R11);
  cg.movFromMem(Rdi, R10, kMapEntryKeyOffset);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, hashFound);
  cg.movFromMem(R9, Rbp, -32);
  cg.addImm32(R9, 1);
  cg.movFromMem(R10, Rbp, -24);
  cg.subRegImm32(R10, 1);
  cg.andReg(R9, R10);
  cg.movToMem(Rbp, -32, R9);
  cg.jmp(hashLoop);
  cg.bind(hashFound);
  cg.movFromMem(Rax, Rbp, -40);
  cg.jmp(done);
  cg.bind(linear);
  cg.movFromMem(R8, Rbx, kTableLengthOffset);
  cg.movToMem(Rbp, -48, R8);  // len
  cg.movImmToMem(Rbp, -56, 0);  // i
  cg.bind(linearLoop);
  cg.movFromMem(R8, Rbp, -56);
  cg.movFromMem(R9, Rbp, -48);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, notFound);
  cg.movToMem(Rbp, -40, R8);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, Rbx, kTableBlockOffset);
  cg.addReg(R10, R8);
  cg.movFromMem(Rdi, R10, kMapEntryKeyOffset);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, linearFound);
  cg.movFromMem(R8, Rbp, -56);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -56, R8);
  cg.jmp(linearLoop);
  cg.bind(linearFound);
  cg.movFromMem(Rax, Rbp, -40);
  cg.jmp(done);
  cg.bind(notFound);
  cg.movImm(Rax, -1);
  cg.bind(done);
  cg.addImm32(Rsp, 64);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `long oskel_block_ensure(void* header, long needed, long itemSize)`: returns a
// block with room for `needed` items of `itemSize` bytes. The existing block is
// returned when its capacity already suffices; otherwise a fresh block of the
// next power-of-two capacity is allocated from the arena, the live items are
// copied, and the header's block/capacity are updated. Returns null on
// exhaustion. `header` is the 3-word array/map header (length, capacity, block).
void emitBlockEnsure(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label grow = cg.newLabel();
  CodeGen::Label growLoop = cg.newLabel();
  CodeGen::Label growLoopDone = cg.newLabel();
  CodeGen::Label haveCapacity = cg.newLabel();
  CodeGen::Label noCopy = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(Rbx, Rdi);  // header
  cg.movReg(R12, Rsi);  // needed
  cg.movToMem(Rbp, -24, Rdx);  // itemSize
  cg.movFromMem(R8, Rbx, kTableCapacityOffset);
  cg.cmpReg(R8, R12);
  cg.jcc(Jcc::Ae, haveCapacity);
  // newCap = cap == 0 ? 8 : cap * 2; while (newCap < needed) newCap *= 2.
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, grow);
  cg.addReg(R8, R8);
  cg.jmp(growLoop);
  cg.bind(grow);
  cg.movImm(R8, 8);
  cg.bind(growLoop);
  cg.cmpReg(R8, R12);
  cg.jcc(Jcc::Ae, growLoopDone);
  cg.addReg(R8, R8);
  cg.jmp(growLoop);
  cg.bind(growLoopDone);
  cg.movToMem(Rbp, -32, R8);  // newCap
  // size = newCap * itemSize
  cg.movReg(Rdi, R8);
  cg.movFromMem(Rax, Rbp, -24);
  cg.imulReg(Rdi, Rax);
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(R10, Rax);  // new block
  // bytes = length * itemSize
  cg.movFromMem(Rcx, Rbx, kTableLengthOffset);
  cg.movFromMem(Rax, Rbp, -24);
  cg.imulReg(Rcx, Rax);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, noCopy);
  cg.movReg(Rdi, R10);
  cg.movFromMem(Rsi, Rbx, kTableBlockOffset);
  cg.repMovsb();
  cg.bind(noCopy);
  cg.movToMem(Rbx, kTableBlockOffset, R10);
  cg.movFromMem(Rax, Rbp, -32);
  cg.movToMem(Rbx, kTableCapacityOffset, Rax);
  cg.movReg(Rax, R10);
  cg.jmp(done);
  cg.bind(haveCapacity);
  cg.movFromMem(Rax, Rbx, kTableBlockOffset);
  cg.jmp(done);
  cg.bind(oom);
  cg.xorReg(Rax, Rax);
  cg.bind(done);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `long oskel_utf8_len(AotString* s)`: the number of UTF-8 code points, i.e. the
// count of bytes that are not continuation bytes (0b10xxxxxx). A leaf.
void emitUtf8Len(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label skip = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  cg.movFromMem(R8, Rdi, 0);  // length in bytes
  cg.xorReg(Rax, Rax);        // code-point count
  cg.xorReg(R9, R9);          // index
  cg.bind(loop);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ae, done);
  cg.movReg(R10, R9);
  cg.addReg(R10, Rdi);
  cg.movzxFromMem(R10, R10, kStringDataOffset);
  cg.andImm32(R10, 0xC0);
  cg.cmpImm32(R10, 0x80);
  cg.jcc(Jcc::Eq, skip);
  cg.addImm32(Rax, 1);
  cg.bind(skip);
  cg.addImm32(R9, 1);
  cg.jmp(loop);
  cg.bind(done);
  cg.retInsn();
}

// Emits the length of the UTF-8 character whose lead byte is in `lead` (r8) into
// `len` (r9). 1/2/3/4 from the high bits of the lead byte. Clobbers r10.
void emitUtf8LeadLen(CodeGen& cg, X64Reg lead, X64Reg len, X64Reg scratch) {
  CodeGen::Label one = cg.newLabel();
  CodeGen::Label two = cg.newLabel();
  CodeGen::Label three = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.movReg(scratch, lead);
  cg.andImm32(scratch, 0x80);
  cg.testReg(scratch);
  cg.jcc(Jcc::Eq, one);  // < 0x80
  cg.movReg(scratch, lead);
  cg.andImm32(scratch, 0xE0);
  cg.cmpImm32(scratch, 0xC0);
  cg.jcc(Jcc::Eq, two);
  cg.movReg(scratch, lead);
  cg.andImm32(scratch, 0xF0);
  cg.cmpImm32(scratch, 0xE0);
  cg.jcc(Jcc::Eq, three);
  cg.movImm(len, 4);
  cg.jmp(done);
  cg.bind(one);
  cg.movImm(len, 1);
  cg.jmp(done);
  cg.bind(two);
  cg.movImm(len, 2);
  cg.jmp(done);
  cg.bind(three);
  cg.movImm(len, 3);
  cg.bind(done);
}

// `(AotString*, long index) -> (char* in rax, byte length in rdx)`: the bytes of
// the `index`-th UTF-8 code point, or rax = 0 when the index is past the end.
// A leaf used by string indexing and string iteration.
void emitUtf8CharAt(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label found = cg.newLabel();
  CodeGen::Label notFound = cg.newLabel();

  cg.bind(label);
  cg.movFromMem(R8, Rdi, 0);           // total bytes
  cg.movReg(R9, Rdi);
  cg.addImm32(R9, static_cast<std::int32_t>(kStringDataOffset));  // cursor
  cg.movReg(R10, R9);
  cg.addReg(R10, R8);                  // end
  cg.xorReg(R11, R11);                 // code-point index
  cg.bind(loop);
  cg.cmpReg(R9, R10);
  cg.jcc(Jcc::Ae, notFound);
  cg.movzxFromMem(Rcx, R9, 0);         // lead byte
  emitUtf8LeadLen(cg, Rcx, Rdx, R8);
  cg.cmpReg(R11, Rsi);
  cg.jcc(Jcc::Eq, found);
  cg.addReg(R9, Rdx);
  cg.addImm32(R11, 1);
  cg.jmp(loop);
  cg.bind(found);
  cg.movReg(Rax, R9);
  cg.retInsn();
  cg.bind(notFound);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.retInsn();
}

// `AotValue oskel_table_new(long kind)`: allocate a zeroed array (kind 0) or map
// (kind 1) header in the arena. The items/entries block is allocated lazily by
// `oskel_block_ensure` on the first store, so an empty collection costs one
// allocation.
void emitTableNew(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label arrayHeader = cg.newLabel();
  CodeGen::Label allocHeader = cg.newLabel();
  CodeGen::Label arrayDone = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);  // kind
  cg.testReg(Rbx);
  cg.jcc(Jcc::Eq, arrayHeader);
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotMapHeaderSize));
  cg.jmp(allocHeader);
  cg.bind(arrayHeader);
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotArrayHeaderSize));
  cg.bind(allocHeader);
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movImmToMem(Rax, kTableLengthOffset, 0);
  cg.movImmToMem(Rax, kTableCapacityOffset, 0);
  cg.movImmToMem(Rax, kTableBlockOffset, 0);
  cg.testReg(Rbx);
  cg.jcc(Jcc::Eq, arrayDone);
  cg.movImmToMem(Rax, kTableHashCapacityOffset, 0);
  cg.movImmToMem(Rax, kTableHashBlockOffset, 0);
  cg.movImmToMem(Rax, kTableClassNameOffset, 0);
  cg.movImmToMem(Rax, kTableMethodsOffset, 0);
  cg.bind(arrayDone);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagArray);
  cg.testReg(Rbx);
  cg.jcc(Jcc::Eq, done);
  cg.movImm(Rax, kTagMap);
  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_pack_rest(AotValue* first, long count)`: build the array a
// variadic function binds to its trailing `rest...` parameter (PHASE 7c.3). The
// caller's argument window grows *down* (consecutive arguments sit 16 bytes
// apart, the first at the highest address), so item `i` is at `first - 16 * i`.
// The helper allocates the array header, grows the items block once to `count`
// and copies the extras in order. `count == 0` yields an empty array, matching
// the VM's packed `rest` binding for a call with no extras.
void emitPackRest(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label empty = cg.newLabel();
  CodeGen::Label copyLoop = cg.newLabel();
  CodeGen::Label copyDone = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movToMem(Rbp, -24, Rdi);  // first extra
  cg.movReg(R12, Rsi);         // count

  // The array header, zeroed.
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotArrayHeaderSize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);  // header
  cg.movImmToMem(Rbx, kTableLengthOffset, 0);
  cg.movImmToMem(Rbx, kTableCapacityOffset, 0);
  cg.movImmToMem(Rbx, kTableBlockOffset, 0);

  cg.testReg(R12);
  cg.jcc(Jcc::Eq, empty);

  // Grow the items block to `count` items.
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.movImm(Rdx, kArrayItemSize);
  cg.callLabel(env.blockEnsure);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);

  // Copy the extras: block[i] = first[-16 * i].
  cg.xorReg(R8, R8);                     // i
  cg.movFromMem(R9, Rbp, -24);           // src cursor = first
  cg.movReg(R10, Rax);                   // dst cursor = block
  cg.bind(copyLoop);
  cg.cmpReg(R8, R12);
  cg.jcc(Jcc::Ae, copyDone);
  cg.movFromMem(R11, R9, 0);             // tag
  cg.movToMem(R10, 0, R11);
  cg.movFromMem(R11, R9, 8);             // payload
  cg.movToMem(R10, 8, R11);
  cg.subRegImm32(R9, 16);                // next extra (window grows down)
  cg.addImm32(R10, static_cast<std::int32_t>(kArrayItemSize));
  cg.addImm32(R8, 1);
  cg.jmp(copyLoop);
  cg.bind(copyDone);
  cg.movToMem(Rbx, kTableLengthOffset, R12);

  cg.bind(empty);
  cg.movReg(Rdx, Rbx);
  cg.movImm(Rax, kTagArray);
  cg.jmp(done);

  cg.bind(done);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_table_get(AotValue obj, AotValue index)`: `obj[index]` for an
// array (int index, negative from the end), a string (int code-point index) or
// a map (string key, missing key yields nil), mirroring `ops::indexGet`.
void emitTableGet(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label arr = cg.newLabel();
  CodeGen::Label str = cg.newLabel();
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label arrNonNeg = cg.newLabel();
  CodeGen::Label mapMiss = cg.newLabel();
  CodeGen::Label strNonNeg = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failArrayType = cg.newLabel();
  CodeGen::Label failStrType = cg.newLabel();
  CodeGen::Label failMapType = cg.newLabel();
  CodeGen::Label failArrRange = cg.newLabel();
  CodeGen::Label failStrRange = cg.newLabel();
  CodeGen::Label failIndex = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 48);
  cg.movReg(Rbx, Rdi);              // obj tag
  cg.movReg(R12, Rsi);              // obj payload
  cg.movToMem(Rbp, -24, Rdx);       // index tag
  cg.movToMem(Rbp, -32, Rcx);       // index payload
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, arr);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, str);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  cg.jmp(failIndex);

  // --- array[index] ---
  cg.bind(arr);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, failArrayType);
  cg.movFromMem(R8, Rbp, -32);      // i
  cg.movFromMem(R9, R12, kTableLengthOffset);  // len
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, arrNonNeg);
  cg.addReg(R8, R9);                // negative index from the end
  cg.bind(arrNonNeg);
  cg.testReg(R8);
  cg.jcc(Jcc::Lt, failArrRange);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, failArrRange);
  cg.shlImm8(R8, 4);                // i * 16
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, R8);
  cg.movFromMem(Rax, R10, 0);
  cg.movFromMem(Rdx, R10, 8);
  cg.jmp(done);

  // --- string[index] ---
  cg.bind(str);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, failStrType);
  // A negative index counts code points from the end, exactly like an array
  // index (`ops::normalizeIndex`). `utf8CharAt` only walks forward, so the
  // index is rebased against the code-point length first. The length is kept in
  // a slot because the out-of-range message quotes it, as `normalizeIndex` does.
  cg.movReg(Rdi, R12);
  cg.callLabel(env.utf8Len);
  cg.movToMem(Rbp, -56, Rax);
  cg.movFromMem(R8, Rbp, -32);
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, strNonNeg);
  cg.addReg(R8, Rax);
  cg.bind(strNonNeg);
  cg.movReg(Rdi, R12);
  cg.movReg(Rsi, R8);
  cg.callLabel(env.utf8CharAt);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, failStrRange);
  cg.movToMem(Rbp, -40, Rax);       // char pointer
  cg.movToMem(Rbp, -48, Rdx);       // char byte length
  cg.movReg(Rdi, Rdx);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(Rcx, Rbp, -48);
  cg.movToMem(Rax, 0, Rcx);         // length word
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.movFromMem(Rsi, Rbp, -40);
  cg.movFromMem(Rcx, Rbp, -48);
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);

  // --- map[index] ---
  cg.bind(map);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, failMapType);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -32);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, mapMiss);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.movFromMem(Rax, R10, kMapEntryValueOffset);
  cg.movFromMem(Rdx, R10, kMapEntryValueOffset + 8);
  cg.jmp(done);
  cg.bind(mapMiss);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.jmp(done);

  cg.bind(done);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(failArrayType);
  emitFailCall(cg, env.fail, *env.msgArrayIndexType);
  cg.bind(failStrType);
  emitFailCall(cg, env.fail, *env.msgStringIndexType);
  cg.bind(failMapType);
  emitFailCall(cg, env.fail, *env.msgMapKeyType);
  cg.bind(failArrRange);
  cg.movFromMem(Rax, Rbp, -32);     // the index as written, like normalizeIndex
  emitFailParts(cg, gMsgText.failDynamic, gMsgText.sbNew, gMsgText.sbAppend,
                {{env.msgArrayIndexKind, Rax},
                 {nullptr, Rax},
                 {env.msgIndexOutOfRange, Rax},
                 {nullptr, R9},
                 {env.msgIndexRangeClose, Rax}});
  cg.bind(failStrRange);
  cg.movFromMem(Rax, Rbp, -32);
  cg.movFromMem(R9, Rbp, -56);      // the code-point length
  emitFailParts(cg, gMsgText.failDynamic, gMsgText.sbNew, gMsgText.sbAppend,
                {{env.msgStringIndexKind, Rax},
                 {nullptr, Rax},
                 {env.msgIndexOutOfRange, Rax},
                 {nullptr, R9},
                 {env.msgIndexRangeClose, Rax}});
  cg.bind(failIndex);
  emitTypeQuoteFail(cg, *env.msgBadIndex, Rbx, R12);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `void oskel_table_set(AotValue obj, AotValue index, AotValue value)`:
// `obj[index] = value`. An array index equal to the length appends; a map key
// that exists is overwritten, otherwise appended. A string is immutable,
// mirroring `ops::indexSet`.
void emitTableSet(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label arr = cg.newLabel();
  CodeGen::Label arrAppend = cg.newLabel();
  CodeGen::Label arrStore = cg.newLabel();
  CodeGen::Label arrNonNeg = cg.newLabel();
  CodeGen::Label arrRange = cg.newLabel();
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label mapAppend = cg.newLabel();
  CodeGen::Label mapStore = cg.newLabel();
  CodeGen::Label mapNext = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failArrType = cg.newLabel();
  CodeGen::Label failMapType = cg.newLabel();
  CodeGen::Label failImmutable = cg.newLabel();
  CodeGen::Label failAssign = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);              // obj tag
  cg.movReg(R12, Rsi);              // obj payload
  cg.movToMem(Rbp, -24, Rdx);       // index tag
  cg.movToMem(Rbp, -32, Rcx);       // index payload
  cg.movToMem(Rbp, -40, R8);        // value tag
  cg.movToMem(Rbp, -48, R9);        // value payload
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, arr);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, failImmutable);
  cg.jmp(failAssign);

  cg.bind(arr);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, failArrType);
  cg.movFromMem(R8, Rbp, -32);                 // i
  cg.movFromMem(R9, R12, kTableLengthOffset);  // len
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, arrNonNeg);
  cg.addReg(R8, R9);
  cg.bind(arrNonNeg);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Eq, arrAppend);
  cg.testReg(R8);
  cg.jcc(Jcc::Lt, arrRange);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, arrRange);
  cg.jmp(arrStore);
  cg.bind(arrAppend);
  // Grow to len + 1, then store at index len and bump the length.
  cg.movReg(Rdi, R12);
  cg.movReg(Rsi, R9);
  cg.addImm32(Rsi, 1);
  cg.movImm(Rdx, kArrayItemSize);
  cg.callLabel(env.blockEnsure);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(R8, R12, kTableLengthOffset);
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R10, Rbp, -48);
  cg.movReg(R11, R8);
  cg.shlImm8(R11, 4);
  cg.addReg(R11, Rax);
  cg.movToMem(R11, 0, R9);
  cg.movToMem(R11, 8, R10);
  cg.addImm32(R8, 1);
  cg.movToMem(R12, kTableLengthOffset, R8);
  cg.jmp(done);
  cg.bind(arrStore);
  cg.shlImm8(R8, 4);
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, R8);
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movToMem(R10, 8, R11);
  cg.jmp(done);

  cg.bind(map);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, failMapType);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -32);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, mapAppend);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.addImm32(R10, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.jmp(mapStore);
  cg.bind(mapAppend);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, R12, kTableLengthOffset);
  cg.addImm32(Rsi, 1);
  cg.movImm(Rdx, kMapEntrySize);
  cg.callLabel(env.blockEnsure);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(R8, R12, kTableLengthOffset);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.addReg(R8, Rax);
  cg.movFromMem(R9, Rbp, -32);
  cg.movToMem(R8, kMapEntryKeyOffset, R9);
  cg.addImm32(R8, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.movFromMem(R9, R12, kTableLengthOffset);
  cg.addImm32(R9, 1);
  cg.movToMem(R12, kTableLengthOffset, R9);
  cg.movReg(R10, R8);
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movToMem(R10, 8, R11);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, R12, kTableLengthOffset);
  cg.subRegImm32(Rsi, 1);
  cg.callLabel(env.mapIndexInsert);
  cg.jmp(mapNext);
  cg.bind(mapStore);
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movToMem(R10, 8, R11);
  cg.bind(mapNext);
  cg.jmp(done);

  cg.bind(done);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(failArrType);
  emitFailCall(cg, env.fail, *env.msgArrayIndexType);
  cg.bind(failMapType);
  emitFailCall(cg, env.fail, *env.msgMapKeyType);
  cg.bind(failImmutable);
  emitFailCall(cg, env.fail, *env.msgImmutable);
  cg.bind(failAssign);
  emitTypeQuoteFail(cg, *env.msgBadAssign, Rbx, R12);
  cg.bind(arrRange);
  // `ops::indexSet` quotes the index as written and the array's length.
  cg.movFromMem(Rax, Rbp, -32);   // index payload
  cg.movFromMem(R9, R12, kTableLengthOffset);
  emitFailParts(cg, gMsgText.failDynamic, gMsgText.sbNew, gMsgText.sbAppend,
                {{env.msgArrayIndexKind, Rax},
                 {nullptr, Rax},
                 {env.msgIndexOutOfRange, Rax},
                 {nullptr, R9},
                 {env.msgIndexRangeClose, Rax}});
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `obj.name = value`, valid only on a map (mirrors `ops::memberSet`).
void emitMemberSet(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label noMethod = cg.newLabel();
  CodeGen::Label methodStore = cg.newLabel();
  CodeGen::Label append = cg.newLabel();
  CodeGen::Label store = cg.newLabel();
  CodeGen::Label next = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failField = cg.newLabel();
  CodeGen::Label failClassField = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);              // obj tag
  cg.movReg(R12, Rsi);              // obj payload
  cg.movToMem(Rbp, -24, Rcx);       // name payload (AotString*)
  cg.movToMem(Rbp, -40, R8);        // value tag
  cg.movToMem(Rbp, -48, R9);        // value payload
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  cg.jmp(failField);

  cg.bind(map);
  // Reassigning a method updates the method table, not a new field.
  cg.movFromMem(Rdi, R12, kTableMethodsOffset);
  cg.testReg(Rdi);
  cg.jcc(Jcc::Eq, noMethod);
  cg.movFromMem(Rsi, Rbp, -24);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, methodStore);
  cg.bind(noMethod);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -24);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, append);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.addImm32(R10, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.jmp(store);
  cg.bind(methodStore);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableMethodsOffset);
  cg.movFromMem(R10, R10, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.addImm32(R10, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movToMem(R10, 8, R11);
  cg.jmp(done);
  cg.bind(append);
  // A class instance has a fixed field set; an unknown name is an error.
  cg.movFromMem(R8, R12, kTableClassNameOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, failClassField);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, R12, kTableLengthOffset);
  cg.addImm32(Rsi, 1);
  cg.movImm(Rdx, kMapEntrySize);
  cg.callLabel(env.blockEnsure);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(R8, R12, kTableLengthOffset);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.addReg(R8, Rax);
  cg.movFromMem(R9, Rbp, -24);
  cg.movToMem(R8, kMapEntryKeyOffset, R9);
  cg.addImm32(R8, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.movFromMem(R9, R12, kTableLengthOffset);
  cg.addImm32(R9, 1);
  cg.movToMem(R12, kTableLengthOffset, R9);
  cg.movReg(R10, R8);
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movToMem(R10, 8, R11);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, R12, kTableLengthOffset);
  cg.subRegImm32(Rsi, 1);
  cg.callLabel(env.mapIndexInsert);
  cg.jmp(next);
  cg.bind(store);
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movToMem(R10, 8, R11);
  cg.bind(next);
  cg.bind(done);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(failField);
  emitTypeQuoteFail(cg, *env.msgBadField, Rbx, R12);
  cg.bind(failClassField);
  emitFailCall(cg, env.fail, *env.msgNoField);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_contains(AotValue needle, AotValue haystack)`: the `in`
// operator for arrays (value equality via `oskel_compare`), maps (string key)
// and strings (substring). Ranges are not lowered yet.
void emitContains(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label arr = cg.newLabel();
  CodeGen::Label arrLoop = cg.newLabel();
  CodeGen::Label arrFound = cg.newLabel();
  CodeGen::Label arrMiss = cg.newLabel();
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label mapFound = cg.newLabel();
  CodeGen::Label str = cg.newLabel();
  CodeGen::Label strLoop = cg.newLabel();
  CodeGen::Label strOuter = cg.newLabel();
  CodeGen::Label strNext = cg.newLabel();
  CodeGen::Label strFound = cg.newLabel();
  CodeGen::Label strMiss = cg.newLabel();
  CodeGen::Label range = cg.newLabel();
  CodeGen::Label rangeAsc = cg.newLabel();
  CodeGen::Label rangeAscIncl = cg.newLabel();
  CodeGen::Label rangeDescIncl = cg.newLabel();
  CodeGen::Label trueDone = cg.newLabel();
  CodeGen::Label falseDone = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failType = cg.newLabel();
  CodeGen::Label failRangeType = cg.newLabel();
  CodeGen::Label failMapKey = cg.newLabel();
  CodeGen::Label failStrOperand = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 48);
  cg.movReg(Rbx, Rdi);              // needle tag
  cg.movReg(R12, Rsi);              // needle payload
  cg.movToMem(Rbp, -24, Rdx);       // haystack tag
  cg.movToMem(Rbp, -32, Rcx);       // haystack payload
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, arr);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, str);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, range);
  cg.jmp(failType);

  cg.bind(arr);
  cg.movFromMem(R8, Rbp, -32);      // array ptr
  cg.movImmToMem(Rbp, -40, 0);      // index
  cg.movFromMem(R9, R8, kTableLengthOffset);
  cg.movToMem(Rbp, -48, R9);        // len
  cg.bind(arrLoop);
  cg.movFromMem(R10, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Ae, arrMiss);
  cg.movReg(R11, R10);
  cg.shlImm8(R11, 4);
  cg.movFromMem(R9, R8, kTableBlockOffset);
  cg.addReg(R11, R9);
  cg.movFromMem(Rdi, R11, 0);       // item tag
  cg.movFromMem(Rsi, R11, 8);       // item payload
  cg.movReg(Rdx, Rbx);
  cg.movReg(Rcx, R12);
  cg.movImm(R8, static_cast<std::int64_t>(AotCompareOp::Eq));
  cg.callLabel(env.compare);
  cg.testReg(Rdx);
  cg.jcc(Jcc::Ne, arrFound);
  cg.movFromMem(R10, Rbp, -40);
  cg.addImm32(R10, 1);
  cg.movToMem(Rbp, -40, R10);
  cg.movFromMem(R8, Rbp, -32);
  cg.jmp(arrLoop);

  cg.bind(map);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, failMapKey);
  cg.movFromMem(Rdi, Rbp, -32);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, mapFound);
  cg.jmp(falseDone);

  cg.bind(str);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, failStrOperand);
  // Substring search: for each start in the haystack where the needle's first
  // byte matches, compare the following bytes.
  cg.movFromMem(R8, Rbp, -32);       // haystack AotString*
  cg.movFromMem(R9, R12, 0);         // needle byte length
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, strFound);         // an empty needle is always contained
  cg.movImmToMem(Rbp, -40, 0);       // outer index
  cg.bind(strOuter);
  cg.movFromMem(R10, Rbp, -40);      // i
  cg.movFromMem(R11, R8, 0);         // haystack length
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Ae, strMiss);
  // Compare needle[0] with haystack[i]; if equal, compare the rest.
  cg.movReg(Rcx, R12);
  cg.movzxFromMem(Rcx, Rcx, kStringDataOffset);
  cg.movReg(Rdx, R10);
  cg.addReg(Rdx, R8);
  cg.movzxFromMem(Rdx, Rdx, kStringDataOffset);
  cg.cmpReg(Rcx, Rdx);
  cg.jcc(Jcc::Ne, strNext);
  // Check that needle fits and every byte matches.
  cg.movReg(R11, R10);
  cg.addReg(R11, R9);
  cg.movFromMem(Rcx, R8, 0);
  cg.cmpReg(R11, Rcx);
  cg.jcc(Jcc::A, strMiss);           // does not fit
  cg.movImmToMem(Rbp, -48, 0);       // j
  cg.bind(strLoop);
  cg.movFromMem(R11, Rbp, -48);
  cg.cmpReg(R11, R9);
  cg.jcc(Jcc::Ae, strFound);
  cg.movReg(Rcx, R11);
  cg.addReg(Rcx, R12);
  cg.movzxFromMem(Rcx, Rcx, kStringDataOffset);
  cg.movReg(Rdx, R11);
  cg.addReg(Rdx, R10);
  cg.addReg(Rdx, R8);
  cg.movzxFromMem(Rdx, Rdx, kStringDataOffset);
  cg.cmpReg(Rcx, Rdx);
  cg.jcc(Jcc::Ne, strNext);
  cg.movFromMem(R11, Rbp, -48);
  cg.addImm32(R11, 1);
  cg.movToMem(Rbp, -48, R11);
  cg.jmp(strLoop);
  cg.bind(strNext);
  cg.movFromMem(R10, Rbp, -40);
  cg.addImm32(R10, 1);
  cg.movToMem(Rbp, -40, R10);
  cg.jmp(strOuter);

  cg.bind(arrFound);
  cg.bind(mapFound);
  cg.bind(strFound);
  cg.jmp(trueDone);
  cg.bind(arrMiss);
  cg.bind(strMiss);
  cg.jmp(falseDone);

  // A range contains an integer when it falls inside the bounds. An ascending
  // range (start <= end) checks `start <= value < end` (or `<= end` inclusive);
  // a descending one checks the mirror, and a descending exclusive range
  // matches neither bound (`5 in 5..0` is false), matching `ops::inRange`.
  cg.bind(range);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, failRangeType);
  cg.movFromMem(R11, Rbp, -32);        // range header
  cg.movFromMem(R10, R11, kRangeStartOffset);
  cg.movFromMem(R9, R11, kRangeEndOffset);
  cg.cmpReg(R10, R9);
  cg.jcc(Jcc::Le, rangeAsc);
  cg.movFromMem(R8, R11, kRangeInclusiveOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, rangeDescIncl);
  cg.cmpReg(R12, R9);
  cg.jcc(Jcc::Le, falseDone);          // value <= end: outside
  cg.cmpReg(R12, R10);
  cg.jcc(Jcc::Ge, falseDone);          // value >= start: outside
  cg.jmp(trueDone);
  cg.bind(rangeDescIncl);
  cg.cmpReg(R12, R9);
  cg.jcc(Jcc::Lt, falseDone);          // value < end: outside
  cg.cmpReg(R12, R10);
  cg.jcc(Jcc::Gt, falseDone);          // value > start: outside
  cg.jmp(trueDone);
  cg.bind(rangeAsc);
  cg.movFromMem(R8, R11, kRangeInclusiveOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, rangeAscIncl);
  cg.cmpReg(R12, R10);
  cg.jcc(Jcc::Lt, falseDone);          // value < start: outside
  cg.cmpReg(R12, R9);
  cg.jcc(Jcc::Ge, falseDone);          // value >= end: outside
  cg.jmp(trueDone);
  cg.bind(rangeAscIncl);
  cg.cmpReg(R12, R10);
  cg.jcc(Jcc::Lt, falseDone);          // value < start: outside
  cg.cmpReg(R12, R9);
  cg.jcc(Jcc::Gt, falseDone);          // value > end: outside
  cg.jmp(trueDone);

  cg.bind(trueDone);
  cg.movImm(Rax, kTagBool);
  cg.movImm(Rdx, 1);
  cg.jmp(done);
  cg.bind(falseDone);
  cg.movImm(Rax, kTagBool);
  cg.xorReg(Rdx, Rdx);
  cg.bind(done);
  cg.addImm32(Rsp, 48);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(failType);
  emitTwoTypeFail(cg, *env.msgBadIn, *gMsgText.textAnd, Rbx, R12, Rdx, Rcx);
  cg.bind(failRangeType);
  emitFailCall(cg, env.fail, *env.msgRangeIntType);
  cg.bind(failMapKey);
  emitFailCall(cg, env.fail, *env.msgMapMembershipKey);
  cg.bind(failStrOperand);
  emitFailCall(cg, env.fail, *env.msgStringMembershipOperand);
}

// `AotValue oskel_iter_next(AotValue* cursor, AotValue iterable, AotValue* valueOut)`:
// advance the `for-in` cursor and return the item (rax:rdx) with the done flag
// in r8. `valueOut` (rcx) is the slot that receives the paired value of a map
// entry, or null when the loop has no value variable (`for x in ...`). The
// cursor starts nil; it holds an int index afterwards. Arrays yield items, maps
// yield keys (and write the paired value to `*valueOut`), strings yield one
// code point, mirroring the VM's `ITER`.
void emitIterNextRange(CodeGen& cg, CodeGen::Label range, CodeGen::Label rangeAsc,
                       CodeGen::Label rangeAscIncl, CodeGen::Label rangeDescIncl,
                       CodeGen::Label rangeEmit, CodeGen::Label rangeDescNext,
                       CodeGen::Label rangeStoreCursor, CodeGen::Label doneTrue,
                       CodeGen::Label done);

// `AotValue oskel_iter_next(AotValue* cursor, long tag, long payload, AotValue* valueOut)`:
// advance the `for-in` cursor and return the item (rax:rdx) with the done flag
// in r8, writing a map entry's value to `*valueOut` (when non-null).
void emitIterNext(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label zero = cg.newLabel();
  CodeGen::Label arr = cg.newLabel();
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label mapNoValue = cg.newLabel();
  CodeGen::Label str = cg.newLabel();
  CodeGen::Label strSkip = cg.newLabel();
  CodeGen::Label strSkipDone = cg.newLabel();
  CodeGen::Label strStoreCursor = cg.newLabel();
  CodeGen::Label range = cg.newLabel();
  CodeGen::Label rangeAsc = cg.newLabel();
  CodeGen::Label rangeAscIncl = cg.newLabel();
  CodeGen::Label rangeDescIncl = cg.newLabel();
  CodeGen::Label rangeEmit = cg.newLabel();
  CodeGen::Label rangeDescNext = cg.newLabel();
  CodeGen::Label rangeStoreCursor = cg.newLabel();
  CodeGen::Label dispatch = cg.newLabel();
  CodeGen::Label storeCursor = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label doneTrue = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failType = cg.newLabel();
  CodeGen::Label failState = cg.newLabel();
  CodeGen::Label failKeyValue = cg.newLabel();
  CodeGen::Label noValueCheck = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 48);
  cg.movReg(Rbx, Rdi);              // cursor pointer
  cg.movReg(R12, Rdx);              // iterable payload
  cg.movToMem(Rbp, -24, Rsi);       // iterable tag
  cg.movToMem(Rbp, -56, Rcx);       // value-out slot (null when no value variable)
  // A `for k, v` over an iterable that is not a map is a fatal error, the same
  // one the VM raises. An unsupported tag is *not* this error: the VM checks
  // the shape first and reports "cannot iterate", so only array, string and
  // range reach the key/value message here.
  cg.movFromMem(R8, Rbp, -56);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, noValueCheck);
  cg.movFromMem(R8, Rbp, -24);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, noValueCheck);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, failKeyValue);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, failKeyValue);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, failKeyValue);
  cg.bind(noValueCheck);
  cg.movFromMem(R8, Rbx, 0);        // cursor tag
  cg.movFromMem(R9, Rbx, 8);        // cursor payload
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, zero);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, failState);
  cg.jmp(dispatch);
  cg.bind(zero);
  // A range's cursor is the current value, which starts at `start`; every
  // other iterable uses a 0-based index.
  cg.movFromMem(R8, Rbp, -24);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Ne, dispatch);
  cg.movFromMem(R9, R12, kRangeStartOffset);
  cg.bind(dispatch);
  cg.movToMem(Rbp, -32, R9);        // index / range value
  cg.movFromMem(R8, Rbp, -24);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, arr);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, str);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, range);
  cg.jmp(failType);

  cg.bind(arr);
  cg.movFromMem(R9, Rbp, -32);
  cg.movFromMem(R10, R12, kTableLengthOffset);
  cg.cmpReg(R9, R10);
  cg.jcc(Jcc::Ae, doneTrue);
  cg.movReg(R11, R9);
  cg.shlImm8(R11, 4);
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R11, R10);
  cg.movFromMem(Rax, R11, 0);
  cg.movFromMem(Rdx, R11, 8);
  cg.jmp(storeCursor);

  cg.bind(map);
  cg.movFromMem(R9, Rbp, -32);
  cg.movFromMem(R10, R12, kTableLengthOffset);
  cg.cmpReg(R9, R10);
  cg.jcc(Jcc::Ae, doneTrue);
  cg.imulImm32(R9, R9, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R9, R10);
  // Write the paired value straight into the loop variable's slot when the
  // loop has one (`for k, v in map`); `valueOut` is null otherwise.
  cg.movFromMem(R10, Rbp, -56);
  cg.testReg(R10);
  cg.jcc(Jcc::Eq, mapNoValue);
  cg.movFromMem(R11, R9, kMapEntryValueOffset);
  cg.movToMem(R10, 0, R11);
  cg.movFromMem(R11, R9, kMapEntryValueOffset + 8);
  cg.movToMem(R10, 8, R11);
  cg.bind(mapNoValue);
  cg.movImm(Rax, kTagStr);
  cg.movFromMem(Rdx, R9, kMapEntryKeyOffset);
  cg.jmp(storeCursor);

  cg.bind(str);
  // The cursor is a byte offset, not a character index: the step reads the lead
  // byte at the cursor and skips its continuation bytes, so a loop over an
  // n-character string costs O(n) instead of rescanning from the start on every
  // turn. `utf8CharAt` walks from byte 0 each call, which made the loop O(n^2)
  // -- 200 000 characters took 24s. The WASM backend already steps this way.
  cg.movFromMem(R9, R12, 0);     // string byte length
  cg.movFromMem(R10, Rbp, -32);  // offset (cursor)
  cg.cmpReg(R10, R9);
  cg.jcc(Jcc::Ae, doneTrue);  // offset >= length: past the last byte
  cg.movReg(R11, R10);
  cg.addImm32(R11, 1);  // end = offset + 1, then the continuations
  cg.bind(strSkip);
  cg.cmpReg(R11, R9);
  cg.jcc(Jcc::Ae, strSkipDone);
  cg.movReg(Rax, R12);
  cg.addReg(Rax, R11);
  cg.movzxFromMem(Rax, Rax, kStringDataOffset);  // text[end]
  cg.movReg(Rcx, Rax);
  cg.andImm32(Rcx, 0xC0);
  cg.cmpImm32(Rcx, 0x80);
  cg.jcc(Jcc::Ne, strSkipDone);  // not a continuation byte: the char ends
  cg.addImm32(R11, 1);
  cg.jmp(strSkip);
  cg.bind(strSkipDone);
  cg.movReg(Rax, R12);
  cg.addReg(Rax, R10);
  cg.addImm32(Rax, static_cast<std::int32_t>(kStringDataOffset));  // char pointer
  cg.movReg(Rdx, R11);
  cg.subReg(Rdx, R10);              // char byte length
  cg.movToMem(Rbp, -40, Rax);       // char pointer
  cg.movToMem(Rbp, -48, Rdx);       // char byte length
  cg.movToMem(Rbp, -32, R11);       // next cursor = end (stored by strStoreCursor)
  cg.movReg(Rdi, Rdx);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(Rcx, Rbp, -48);
  cg.movToMem(Rax, 0, Rcx);
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.movFromMem(Rsi, Rbp, -40);
  cg.movFromMem(Rcx, Rbp, -48);
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  // Without this jump the char just built would fall through into the range
  // code below, which overwrites rax:rdx with the cursor index.
  cg.jmp(strStoreCursor);

  emitIterNextRange(cg, range, rangeAsc, rangeAscIncl, rangeDescIncl, rangeEmit,
                    rangeDescNext, rangeStoreCursor, doneTrue, done);

  cg.bind(storeCursor);
  cg.movFromMem(R9, Rbp, -32);
  cg.addImm32(R9, 1);
  cg.movImmToMem(Rbx, 0, kTagInt);
  cg.movToMem(Rbx, 8, R9);
  cg.xorReg(R8, R8);                // done = false
  cg.jmp(done);

  // A string's cursor is already the next byte offset (`end`), not an index to
  // advance, so it is stored as it stands.
  cg.bind(strStoreCursor);
  cg.movFromMem(R9, Rbp, -32);
  cg.movImmToMem(Rbx, 0, kTagInt);
  cg.movToMem(Rbx, 8, R9);
  cg.xorReg(R8, R8);  // done = false
  cg.jmp(done);

  cg.bind(doneTrue);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.movImm(R8, 1);

  cg.bind(done);
  cg.addImm32(Rsp, 48);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(failKeyValue);
  emitTypeTailFail(cg, *env.msgMapValue, R8, R12);
  cg.bind(failType);
  emitTypeQuoteFail(cg, *env.msgNotIterable, R8, R12);
  cg.bind(failState);
  emitFailCall(cg, env.fail, *env.msgNotIterable);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// The `for-in` body for a range: `R9` holds the current value, `R12` the range
// header. Ascending ranges step +1, descending ones step -1 (unsigned, so
// INT64_MIN/MAX bounds stay defined). The next value goes to `rangeStoreCursor`.
void emitIterNextRange(CodeGen& cg, CodeGen::Label range, CodeGen::Label rangeAsc,
                       CodeGen::Label rangeAscIncl, CodeGen::Label rangeDescIncl,
                       CodeGen::Label rangeEmit, CodeGen::Label rangeDescNext,
                       CodeGen::Label rangeStoreCursor, CodeGen::Label doneTrue,
                       CodeGen::Label done) {
  cg.bind(range);
  cg.movFromMem(R9, Rbp, -32);          // current value
  cg.movFromMem(R10, R12, kRangeStartOffset);
  cg.movFromMem(R11, R12, kRangeEndOffset);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Le, rangeAsc);

  // Descending: start > end. Stop when the current value passes `end`.
  cg.movFromMem(R8, R12, kRangeInclusiveOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, rangeDescIncl);
  cg.cmpReg(R9, R11);
  cg.jcc(Jcc::Le, doneTrue);            // exclusive: value > end required
  cg.jmp(rangeEmit);
  cg.bind(rangeDescIncl);
  cg.cmpReg(R9, R11);
  cg.jcc(Jcc::Lt, doneTrue);            // inclusive: value >= end required
  cg.jmp(rangeEmit);

  // Ascending: start <= end. Stop when the current value passes `end`.
  cg.bind(rangeAsc);
  cg.movFromMem(R8, R12, kRangeInclusiveOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, rangeAscIncl);
  cg.cmpReg(R9, R11);
  cg.jcc(Jcc::Ge, doneTrue);            // exclusive: value < end required
  cg.jmp(rangeEmit);
  cg.bind(rangeAscIncl);
  cg.cmpReg(R9, R11);
  cg.jcc(Jcc::Gt, doneTrue);            // inclusive: value <= end required

  cg.bind(rangeEmit);
  cg.movImm(Rax, kTagInt);
  cg.movReg(Rdx, R9);
  // Decide the direction once more to step the cursor the right way.
  cg.movFromMem(R10, R12, kRangeStartOffset);
  cg.movFromMem(R11, R12, kRangeEndOffset);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Gt, rangeDescNext);
  cg.addImm32(R9, 1);                   // ascending: next = current + 1
  cg.jmp(rangeStoreCursor);
  cg.bind(rangeDescNext);
  cg.subRegImm32(R9, 1);                // descending: next = current - 1

  cg.bind(rangeStoreCursor);
  cg.movImmToMem(Rbx, 0, kTagInt);
  cg.movToMem(Rbx, 8, R9);
  cg.xorReg(R8, R8);                    // done = false
  cg.jmp(done);
}

// `AotValue oskel_builtin_is_error(AotValue* args, long argc, AotClosure* self)`:
// the hidden `$isError` helper that `try` and `x catch y` compile to. It is the
// exact test the hosted engine uses: a value is an error only when its tag is
// `Error`, so a user map with an `ok` field is never mistaken for one. With
// error values now a real tag, `try`/`catch` observes the propagation the same
// way the VM does.
void emitIsErrorBuiltin(CodeGen& cg, CodeGen::Label label,
                        const DataBlob& msgArgc) {
  CodeGen::Label badArgc = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  // args[0] is the operand: the argument window grows *down* from `args`, so
  // args[0] sits at [rdi + 0] (tag) and [rdi + 8] (payload).
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagError));
  cg.setcc(Jcc::Eq, Rdx);
  cg.movzx(Rdx, Rdx);
  cg.movImm(Rax, kTagBool);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_error(AotValue* args, long argc, AotClosure* self)`:
// the public `error(message, code?)`. It mirrors the hosted builtin: the message
// is the first argument rendered to a string (a string is taken verbatim, any
// other value through `toString`), and the optional second argument is the
// integer code (0 by default). The result is an `Error` value.
void emitErrorBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                      CodeGen::Label fail, CodeGen::Label strBuiltin,
                      const DataBlob& msgOom, const DataBlob& msgCodeType,
                      const DataBlob& msgErrorArgc) {
  CodeGen::Label arityOk = cg.newLabel();
  CodeGen::Label arityOk2 = cg.newLabel();
  CodeGen::Label haveCode = cg.newLabel();
  CodeGen::Label codeOk = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);              // args
  cg.movReg(R12, Rsi);             // argc

  // `error` takes one or two arguments; anything else is the wrong-arity error,
  // matching the hosted `needBetween(in, "error", args, 1, 2)`.
  cg.cmpImm32(R12, 1);
  cg.jcc(Jcc::Ge, arityOk);
  emitArityFailCall(cg, msgErrorArgc, R12);
  cg.bind(arityOk);
  cg.cmpImm32(R12, 2);
  cg.jcc(Jcc::Le, arityOk2);
  emitArityFailCall(cg, msgErrorArgc, R12);
  cg.bind(arityOk2);

  // code = 0 unless a second argument is present and an integer.
  cg.xorReg(R8, R8);
  cg.movToMem(Rbp, -24, R8);        // code = 0
  cg.cmpImm32(R12, 2);
  cg.jcc(Jcc::Lt, haveCode);
  cg.movFromMem(Rax, Rbx, -16);     // args[1].tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, codeOk);
  cg.movFromMem(Rdx, Rbx, -8);      // args[1].payload
  emitTypeTailFail(cg, msgCodeType, Rax, Rdx);
  cg.bind(codeOk);
  cg.movFromMem(Rax, Rbx, -8);      // args[1].payload
  cg.movToMem(Rbp, -24, Rax);

  cg.bind(haveCode);
  // message = str(args[0]): a string is returned as-is, anything else is
  // rendered. `oskel_builtin_str` follows the hosted `toString`; it takes a
  // pointer to a single-element argument window, so copy args[0] into a 16-byte
  // stack slot (tag at rbp-48, payload at rbp-40) and pass its address.
  cg.movFromMem(Rax, Rbx, 0);
  cg.movToMem(Rbp, -48, Rax);
  cg.movFromMem(Rax, Rbx, 8);
  cg.movToMem(Rbp, -40, Rax);
  cg.leaMem(Rdi, Rbp, -48);         // args = &slot
  cg.movImm(Rsi, 1);                // argc
  cg.callLabel(strBuiltin);         // rax:rdx = Str value
  cg.movReg(R8, Rdx);               // message AotString*

  // Allocate the error header.
  cg.movToMem(Rbp, -32, R8);
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotErrorHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(R12, Rax);
  cg.movFromMem(R8, Rbp, -32);
  cg.movToMem(R12, kErrorMessageOffset, R8);
  cg.movFromMem(R8, Rbp, -24);
  cg.movToMem(R12, kErrorCodeOffset, R8);
  cg.movReg(Rdx, R12);
  cg.movImm(Rax, kTagError);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);

  cg.bind(done);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `AotValue oskel_builtin_non_null(AotValue* args, long argc, AotClosure* self)`:
// the hidden `$nonNull` helper for `a!!`. It returns the operand unchanged, or
// fails when it is nil, matching the hosted helper.
void emitNonNullBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                        const DataBlob& msgNonNull) {
  CodeGen::Label ok = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  // args[0] is the operand: the argument window grows *down* from `args`, so
  // args[0] sits at [rdi + 0] (tag) and [rdi + 8] (payload).
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Ne, ok);
  emitFailCall(cg, fail, msgNonNull);

  cg.bind(ok);
  cg.movFromMem(Rax, Rdi, 0);   // tag
  cg.movFromMem(Rdx, Rdi, 8);   // payload
  epilogue(cg);
}

// Renders the integer in `rax` as decimal digits, written *backwards* from the
// cursor in `cursor` (r8), which the helper advances downward. A leading '-'
// is written for a negative value, matching `std::to_string`. Clobbers rax,
// rcx, rdx and r9.
void emitIntDigitsBackwards(CodeGen& cg, X64Reg cursor, CodeGen::Label positive,
                            CodeGen::Label convert, CodeGen::Label loop,
                            CodeGen::Label emit) {
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, positive);
  cg.movImm(R9, 1);          // negative
  cg.negReg(Rax);
  cg.jmp(convert);
  cg.bind(positive);
  cg.xorReg(R9, R9);
  cg.bind(convert);
  cg.movImm(Rcx, 10);
  cg.bind(loop);
  cg.divUnsigned(Rcx);       // rax /= 10, rdx = rax % 10
  cg.addImm32(Rdx, static_cast<std::int32_t>('0'));
  cg.subRegImm32(cursor, 1);
  cg.movByteToMem(cursor, 0, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, loop);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, emit);
  cg.subRegImm32(cursor, 1);
  cg.movImmByteToMem(cursor, 0, static_cast<std::uint8_t>('-'));
  cg.bind(emit);
}

// `AotValue oskel_builtin_str(AotValue* args, long argc, AotClosure* self)`: the
// hidden `$str` helper the compiler emits for string interpolation (`"${x}"`)
// and computed map keys. It renders the operand to an `AotString` in the arena,
// byte-identical to the hosted `Value::toString` for nil, booleans, integers,
// floats, doubles and strings (a string is returned unchanged, so a string
// interpolates without quotes). A collection or function operand renders with
// the recursive repr (`oskel_repr_to_string`), so `"${[1, 2]}"` reads exactly
// like `print([1, 2])` (7c.4b.2).
void emitStrBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                    CodeGen::Label fail, CodeGen::Label formatDouble,
                    CodeGen::Label reprToString, const DataBlob& msgOom,
                    const DataBlob& textTrue, const DataBlob& textFalse,
                    const DataBlob& textNil, const DataBlob& textPtr,
                    const DataBlob& msgStrBad, const DataBlob& msgArgc) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label boolFalse = cg.newLabel();
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asNum = cg.newLabel();
  CodeGen::Label asRepr = cg.newLabel();
  CodeGen::Label asError = cg.newLabel();
  CodeGen::Label asPtr = cg.newLabel();
  CodeGen::Label ptrOpaque = cg.newLabel();
  CodeGen::Label ptrScan = cg.newLabel();
  CodeGen::Label ptrScanEnd = cg.newLabel();
  CodeGen::Label asRange = cg.newLabel();
  CodeGen::Label rangeIncl = cg.newLabel();
  CodeGen::Label rangeAfterSep = cg.newLabel();
  CodeGen::Label intPos = cg.newLabel();
  CodeGen::Label intConv = cg.newLabel();
  CodeGen::Label intLoop = cg.newLabel();
  CodeGen::Label intEmit = cg.newLabel();
  CodeGen::Label rangeEndPos = cg.newLabel();
  CodeGen::Label rangeEndConv = cg.newLabel();
  CodeGen::Label rangeEndLoop = cg.newLabel();
  CodeGen::Label rangeEndEmit = cg.newLabel();
  CodeGen::Label rangeStartPos = cg.newLabel();
  CodeGen::Label rangeStartConv = cg.newLabel();
  CodeGen::Label rangeStartLoop = cg.newLabel();
  CodeGen::Label rangeStartEmit = cg.newLabel();
  CodeGen::Label buildConst = cg.newLabel();
  CodeGen::Label numOom = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 64);   // digit buffer (int, or range bounds + separator)
  cg.movReg(Rbx, Rdi);       // rbx = args
  // Exactly one argument. `$str` (interpolation, computed map keys) goes
  // through the same entry and always passes one, so the check is safe there
  // too; a second argument used to be silently ignored.
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);

  // args[0] is the operand: tag at [rbx + 0], payload at [rbx + 8].
  cg.movFromMem(Rax, Rbx, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, asStr);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, asNil);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Eq, asBool);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, asInt);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, asNum);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, asNum);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, asRange);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, asRepr);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, asRepr);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFunc));
  cg.jcc(Jcc::Eq, asRepr);
  // An error renders as its message alone (the hosted display form).
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagError));
  cg.jcc(Jcc::Eq, asError);
  // A pointer handle renders like it prints: a `cstring` as its text (a fresh
  // arena copy, since a `Str` payload must be an `AotString`), any other handle
  // as `<ptr>`.
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagPtr));
  cg.jcc(Jcc::Eq, asPtr);
  emitFailCall(cg, fail, msgStrBad);

  cg.bind(asPtr);
  cg.movFromMem(R9, Rbx, 8);                  // the handle
  cg.movFromMem(Rax, R9, kPtrKindOffset);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kPtrKindCString));
  cg.jcc(Jcc::Ne, ptrOpaque);
  // Copy the text (up to the first NUL) into a fresh `AotString`.
  cg.movFromMem(R8, R9, kPtrDataOffset);      // the bytes
  cg.movFromMem(R10, R9, kPtrLengthOffset);   // the owned length
  cg.xorReg(R11, R11);                        // the scanned length
  cg.bind(ptrScan);
  cg.cmpReg(R11, R10);
  cg.jcc(Jcc::Ae, ptrScanEnd);
  cg.movReg(Rax, R8);
  cg.addReg(Rax, R11);
  cg.movzxFromMem(Rax, Rax, 0);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, ptrScanEnd);
  cg.addImm32(R11, 1);
  cg.jmp(ptrScan);
  cg.bind(ptrScanEnd);
  cg.movReg(R12, R11);                        // the text length
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movToMem(Rbp, -24, R8);                  // the source bytes
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, R12);                   // the length
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rsi, Rbp, -24);
  cg.movReg(Rcx, R12);
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);
  cg.bind(ptrOpaque);
  cg.leaRipLabel(R8, textPtr.label);
  cg.movImm(R12, static_cast<std::int64_t>(textPtr.text.size()));
  cg.jmp(buildConst);

  // A collection or function renders with the recursive repr, so `"${[1, 2]}"`
  // reads exactly like `print([1, 2])` (nested strings quoted, depth-capped).
  cg.bind(asRepr);
  cg.movFromMem(Rdi, Rbx, 0);
  cg.movFromMem(Rsi, Rbx, 8);
  cg.callLabel(reprToString);
  cg.jmp(done);

  // An error renders as its message string (the hosted display form).
  cg.bind(asError);
  cg.movFromMem(Rdx, Rbx, 8);            // error header
  cg.movFromMem(Rdx, Rdx, kErrorMessageOffset);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);

  // A string renders as itself.
  cg.bind(asStr);
  cg.movFromMem(Rax, Rbx, 0);
  cg.movFromMem(Rdx, Rbx, 8);
  cg.jmp(done);

  cg.bind(asNil);
  cg.leaRipLabel(R8, textNil.label);
  cg.movImm(R12, static_cast<std::int64_t>(textNil.text.size()));
  cg.jmp(buildConst);

  cg.bind(asBool);
  cg.movFromMem(R9, Rbx, 8);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, boolFalse);
  cg.leaRipLabel(R8, textTrue.label);
  cg.movImm(R12, static_cast<std::int64_t>(textTrue.text.size()));
  cg.jmp(buildConst);
  cg.bind(boolFalse);
  cg.leaRipLabel(R8, textFalse.label);
  cg.movImm(R12, static_cast<std::int64_t>(textFalse.text.size()));
  cg.jmp(buildConst);

  // An integer: decimal digits built backwards into the stack buffer, then a
  // leading '-' when negative, matching `std::to_string`.
  cg.bind(asInt);
  cg.movFromMem(Rax, Rbx, 8);
  cg.movReg(R8, Rsp);
  cg.addImm32(R8, 64);       // cursor = end of buffer
  emitIntDigitsBackwards(cg, R8, intPos, intConv, intLoop, intEmit);
  cg.movReg(R12, Rsp);
  cg.addImm32(R12, 64);
  cg.subReg(R12, R8);        // length = end - start
  cg.jmp(buildConst);

  // A range: `<start>..<end>` (exclusive) or `<start>..=<end>` (inclusive),
  // built backwards into the buffer (end, separator, then start).
  cg.bind(asRange);
  cg.movFromMem(R12, Rbx, 8);            // range header
  cg.movReg(R8, Rsp);
  cg.addImm32(R8, 64);                   // cursor = end of buffer
  cg.movFromMem(Rax, R12, kRangeEndOffset);
  emitIntDigitsBackwards(cg, R8, rangeEndPos, rangeEndConv, rangeEndLoop, rangeEndEmit);
  cg.movFromMem(R9, R12, kRangeInclusiveOffset);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, rangeIncl);
  cg.subRegImm32(R8, 3);
  cg.movImmByteToMem(R8, 0, static_cast<std::uint8_t>('.'));
  cg.movImmByteToMem(R8, 1, static_cast<std::uint8_t>('.'));
  cg.movImmByteToMem(R8, 2, static_cast<std::uint8_t>('='));
  cg.jmp(rangeAfterSep);
  cg.bind(rangeIncl);
  cg.subRegImm32(R8, 2);
  cg.movImmByteToMem(R8, 0, static_cast<std::uint8_t>('.'));
  cg.movImmByteToMem(R8, 1, static_cast<std::uint8_t>('.'));
  cg.bind(rangeAfterSep);
  cg.movFromMem(Rax, R12, kRangeStartOffset);
  emitIntDigitsBackwards(cg, R8, rangeStartPos, rangeStartConv, rangeStartLoop,
                         rangeStartEmit);
  cg.movReg(R12, Rsp);
  cg.addImm32(R12, 64);
  cg.subReg(R12, R8);                    // length = end - start
  cg.jmp(buildConst);

  // A Float/Double: format with the embedded dtoa into a stack frame, then copy
  // the text into the arena string. The frame keeps rsp 16-byte aligned at the
  // `call`, as the ABI and the formatter's own calls require.
  cg.bind(asNum);
  cg.subRegImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  cg.movFromMem(Rdi, Rbx, 0);                 // tag
  cg.movFromMem(Rsi, Rbx, 8);                 // payload
  cg.movReg(Rcx, Rsp);                        // scratch = rsp
  cg.movReg(Rdx, Rsp);
  cg.addImm32(Rdx, static_cast<std::int32_t>(kDtoaScratchBytes));  // out
  cg.callLabel(formatDouble);
  cg.movReg(R12, Rax);                        // length
  cg.movReg(R8, Rsp);
  cg.addImm32(R8, static_cast<std::int32_t>(kDtoaScratchBytes));   // text start
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, numOom);
  cg.movToMem(Rax, 0, R12);                   // store the length
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rsi, R8);
  cg.movReg(Rcx, R12);
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.addImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  cg.jmp(done);

  cg.bind(numOom);
  cg.addImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  cg.jmp(oom);

  // Build an `AotString` from a constant/data region: r8 = address, r12 = length.
  cg.bind(buildConst);
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, R12);
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rsi, R8);
  cg.movReg(Rcx, R12);
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);

  cg.bind(oom);
  cg.addImm32(Rsp, 64);
  cg.leaRipLabel(Rdi, msgOom.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(msgOom.text.size()));
  cg.callLabel(fail);
  cg.ud2();

  cg.bind(done);
  cg.addImm32(Rsp, 64);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  cg.addImm32(Rsp, 64);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_range(AotValue* args, long argc, AotClosure* self)`:
// the hidden `$range` helper for the range literals `a..b` (inclusive = false)
// and `a..=b` (inclusive = true). Both bounds must be integers, matching the
// hosted helper. The range header lives in the arena.
void emitRangeBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                      CodeGen::Label fail, const DataBlob& msgOom,
                      const DataBlob& msgRangeBounds) {
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label badBounds = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);  // args
  // args[0] and args[1] are the bounds (16 bytes apart, growing down); args[2]
  // is the inclusive flag. Both bounds must be Int.
  cg.movFromMem(Rax, Rbx, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badBounds);
  cg.movFromMem(Rax, Rbx, -16);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badBounds);

  cg.movImm(Rdi, static_cast<std::int64_t>(kAotRangeHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(R12, Rax);
  cg.movFromMem(R8, Rbx, 8);        // start
  cg.movToMem(R12, kRangeStartOffset, R8);
  cg.movFromMem(R8, Rbx, -8);       // end
  cg.movToMem(R12, kRangeEndOffset, R8);
  cg.movFromMem(R8, Rbx, -24);      // inclusive flag (bool payload)
  cg.movToMem(R12, kRangeInclusiveOffset, R8);
  cg.movReg(Rdx, R12);
  cg.movImm(Rax, kTagRange);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badBounds);
  emitFailCall(cg, fail, msgRangeBounds);
  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `AotValue oskel_builtin_slice(AotValue* args, long argc, AotClosure* self)`: the
// hidden `$slice` helper the compiler emits for `obj[start:end:step]` (PHASE
// 7c.4d). It follows the hosted `$slice`: a bound is nil ("not given") or an
// integer (negative counts from the end), and the selected indices follow
// Python's rule -- a positive step walks `[from, to)` with the defaults
// `0`/`length`, a negative step walks backwards with the defaults `length-1`/
// `-1`, and both clamp to `[-1, length-1]` so a reverse slice never wraps. An
// array slices by element; a string slices by UTF-8 code point. A start or end
// that is neither nil nor an integer, a zero step and any other operand type
// fail, each with its own message.
// `$setClass(obj, name, methods)`: writes the class-name `AotString` and the
// method-table `AotMap` into the instance header. The class desugaring emits
// exactly three arguments; a wrong count fails like every other fixed-arity
// builtin.
void emitSetClassBuiltin(CodeGen& cg, CodeGen::Label label,
                         const DataBlob& msgArgc) {
  CodeGen::Label argcOk = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rsi, 3);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);
  cg.bind(argcOk);
  cg.movFromMem(R8, Rdi, 8);        // args[0] payload: the instance map
  cg.movFromMem(R9, Rdi, -8);       // args[1] payload: the class name
  cg.movFromMem(R10, Rdi, -24);     // args[2] payload: the method map
  cg.movToMem(R8, kTableClassNameOffset, R9);
  cg.movToMem(R8, kTableMethodsOffset, R10);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  epilogue(cg);
}

void emitSliceBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                      const DataBlob& msgSliceStart, const DataBlob& msgSliceEnd,
                      const DataBlob& msgSliceStepType, const DataBlob& msgSliceStep,
                      const DataBlob& msgSliceType) {
  CodeGen::Label stepSet = cg.newLabel();
  CodeGen::Label sliceArray = cg.newLabel();
  CodeGen::Label sliceStr = cg.newLabel();
  CodeGen::Label computeBounds = cg.newLabel();
  CodeGen::Label negativeSetup = cg.newLabel();
  CodeGen::Label afterSetup = cg.newLabel();
  CodeGen::Label arrayBody = cg.newLabel();
  CodeGen::Label strBody = cg.newLabel();
  CodeGen::Label arrayCountLoop = cg.newLabel();
  CodeGen::Label arrayCountDone = cg.newLabel();
  CodeGen::Label arrayCopyLoop = cg.newLabel();
  CodeGen::Label arrayDone = cg.newLabel();
  CodeGen::Label strCountLoop = cg.newLabel();
  CodeGen::Label strCountDone = cg.newLabel();
  CodeGen::Label strCopyLoop = cg.newLabel();
  CodeGen::Label strDone = cg.newLabel();
  CodeGen::Label failStart = cg.newLabel();
  CodeGen::Label failEnd = cg.newLabel();
  CodeGen::Label failStepType = cg.newLabel();
  CodeGen::Label failStep = cg.newLabel();
  CodeGen::Label failType = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label objectOk = cg.newLabel();

  // Stack slots (rbp-relative). r13/r14/r15 are the allocator's own state (the
  // arena cursor/end and the global base), so this helper keeps every value it
  // carries across a call in a slot instead of a register; only rbx and r12 are
  // saved, as in the other helpers.
  constexpr std::int64_t kObjTag = -24;
  constexpr std::int64_t kStart = -32;
  constexpr std::int64_t kStartPresent = -48;
  constexpr std::int64_t kEnd = -56;
  constexpr std::int64_t kEndPresent = -64;
  constexpr std::int64_t kStep = -72;
  constexpr std::int64_t kStepPresent = -80;
  constexpr std::int64_t kLower = -88;
  constexpr std::int64_t kUpper = -96;
  constexpr std::int64_t kLength = -104;
  constexpr std::int64_t kFrom = -112;
  constexpr std::int64_t kTo = -120;
  constexpr std::int64_t kIndex = -128;
  constexpr std::int64_t kOutPtr = -136;  // array block, or the string write cursor
  constexpr std::int64_t kCount = -144;   // selected count
  constexpr std::int64_t kTotal = -152;   // string bytes, or the array copy index
  constexpr std::int64_t kResult = -160;  // array header, or the string header

  // Reads one bound into `valueSlot`/`presentSlot`; nil means "not given". A
  // non-integer bound jumps to `failWhat`, so the message names the bound.
  auto parseBound = [&](std::int64_t tagDisp, std::int64_t valueSlot,
                        std::int64_t presentSlot, CodeGen::Label failWhat) {
    CodeGen::Label absent = cg.newLabel();
    CodeGen::Label boundDone = cg.newLabel();
    cg.movFromMem(Rax, Rbx, tagDisp);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagNil));
    cg.jcc(Jcc::Eq, absent);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
    cg.jcc(Jcc::Ne, failWhat);
    cg.movFromMem(Rax, Rbx, tagDisp + 8);
    cg.movToMem(Rbp, valueSlot, Rax);
    cg.movImmToMem(Rbp, presentSlot, 1);
    cg.jmp(boundDone);
    cg.bind(absent);
    cg.movImmToMem(Rbp, presentSlot, 0);  // the stack slot is not zeroed for us
    cg.bind(boundDone);
  };

  // Jumps to `loopDone` once the cursor has walked past `[from, to)` (positive
  // step) or `(to, from]` (negative step).
  auto selectGoto = [&](CodeGen::Label loopDone) {
    CodeGen::Label negative = cg.newLabel();
    CodeGen::Label body = cg.newLabel();
    cg.movFromMem(R8, Rbp, kStep);
    cg.testReg(R8);
    cg.jcc(Jcc::S, negative);
    cg.movFromMem(R9, Rbp, kIndex);
    cg.movFromMem(R10, Rbp, kTo);
    cg.cmpReg(R9, R10);
    cg.jcc(Jcc::Ge, loopDone);
    cg.jmp(body);
    cg.bind(negative);
    cg.movFromMem(R9, Rbp, kIndex);
    cg.movFromMem(R10, Rbp, kTo);
    cg.cmpReg(R9, R10);
    cg.jcc(Jcc::Le, loopDone);
    cg.bind(body);
  };

  // Clamps `valueSlot` (or the step-dependent default when absent) into
  // `outSlot`, following the hosted rule.
  auto computeBound = [&](std::int64_t valueSlot, std::int64_t presentSlot, bool isStart,
                          std::int64_t outSlot) {
    CodeGen::Label absent = cg.newLabel();
    CodeGen::Label nonNegative = cg.newLabel();
    CodeGen::Label clampLow = cg.newLabel();
    CodeGen::Label clampHigh = cg.newLabel();
    CodeGen::Label store = cg.newLabel();
    CodeGen::Label negative = cg.newLabel();

    cg.movFromMem(Rax, Rbp, presentSlot);
    cg.testReg(Rax);
    cg.jcc(Jcc::Eq, absent);
    cg.movFromMem(Rax, Rbp, valueSlot);
    cg.testReg(Rax);
    cg.jcc(Jcc::Ge, nonNegative);
    cg.movFromMem(R8, Rbp, kLength);
    cg.addReg(Rax, R8);  // a negative bound counts from the end
    cg.bind(nonNegative);
    cg.movFromMem(R8, Rbp, kLower);
    cg.cmpReg(Rax, R8);
    cg.jcc(Jcc::Lt, clampLow);
    cg.movFromMem(R9, Rbp, kUpper);
    cg.cmpReg(Rax, R9);
    cg.jcc(Jcc::Gt, clampHigh);
    cg.jmp(store);
    cg.bind(clampLow);
    cg.movReg(Rax, R8);
    cg.jmp(store);
    cg.bind(clampHigh);
    cg.movReg(Rax, R9);
    cg.jmp(store);

    cg.bind(absent);
    cg.movFromMem(R8, Rbp, kLower);
    cg.movFromMem(R9, Rbp, kUpper);
    cg.movFromMem(R10, Rbp, kStep);
    cg.testReg(R10);
    cg.jcc(Jcc::S, negative);
    if (isStart) cg.movReg(Rax, R8); else cg.movReg(Rax, R9);
    cg.jmp(store);
    cg.bind(negative);
    if (isStart) cg.movReg(Rax, R9); else cg.movReg(Rax, R8);

    cg.bind(store);
    cg.movToMem(Rbp, outSlot, Rax);
  };

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 176);
  cg.movReg(Rbx, Rdi);  // args

  // The receiver type is checked first, as in the hosted `$slice`: a value that
  // cannot be sliced is the primary error, so `n["a":"b"]` on an int names the
  // receiver rather than the bound.
  cg.movFromMem(Rax, Rbx, 0);
  cg.movToMem(Rbp, kObjTag, Rax);
  cg.movFromMem(R12, Rbx, 8);  // object payload
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, objectOk);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, objectOk);
  cg.jmp(failType);

  // Bounds: start = args[1], end = args[2], step = args[3] (nil = absent).
  cg.bind(objectOk);
  parseBound(-16, kStart, kStartPresent, failStart);
  parseBound(-32, kEnd, kEndPresent, failEnd);
  parseBound(-48, kStep, kStepPresent, failStepType);
  cg.movFromMem(Rax, Rbp, kStepPresent);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, stepSet);
  cg.movImmToMem(Rbp, kStep, 1);
  cg.bind(stepSet);
  cg.movFromMem(Rax, Rbp, kStep);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, failStep);

  cg.movFromMem(Rax, Rbp, kObjTag);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, sliceArray);
  cg.jmp(sliceStr);

  cg.bind(sliceArray);
  cg.movFromMem(Rax, R12, kTableLengthOffset);
  cg.movToMem(Rbp, kLength, Rax);
  cg.jmp(computeBounds);

  cg.bind(sliceStr);
  cg.movReg(Rdi, R12);
  cg.callLabel(env.utf8Len);
  cg.movToMem(Rbp, kLength, Rax);

  cg.bind(computeBounds);
  cg.movFromMem(R10, Rbp, kStep);
  cg.testReg(R10);
  cg.jcc(Jcc::S, negativeSetup);
  cg.movImmToMem(Rbp, kLower, 0);
  cg.movFromMem(Rax, Rbp, kLength);
  cg.movToMem(Rbp, kUpper, Rax);
  cg.jmp(afterSetup);
  cg.bind(negativeSetup);
  cg.movImmToMem(Rbp, kLower, -1);
  cg.movFromMem(Rax, Rbp, kLength);
  cg.subRegImm32(Rax, 1);
  cg.movToMem(Rbp, kUpper, Rax);
  cg.bind(afterSetup);
  computeBound(kStart, kStartPresent, true, kFrom);
  computeBound(kEnd, kEndPresent, false, kTo);
  cg.movFromMem(Rax, Rbp, kObjTag);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, arrayBody);
  cg.jmp(strBody);

  // ---- array: count the selected indices, then copy the elements.
  cg.bind(arrayBody);
  cg.movFromMem(Rax, Rbp, kFrom);
  cg.movToMem(Rbp, kIndex, Rax);
  cg.movImmToMem(Rbp, kCount, 0);
  cg.bind(arrayCountLoop);
  selectGoto(arrayCountDone);
  cg.movFromMem(R8, Rbp, kIndex);
  cg.movFromMem(R9, Rbp, kStep);
  cg.addReg(R8, R9);
  cg.movToMem(Rbp, kIndex, R8);
  cg.movFromMem(R8, Rbp, kCount);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kCount, R8);
  cg.jmp(arrayCountLoop);
  cg.bind(arrayCountDone);
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotArrayHeaderSize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbp, kResult, Rax);
  cg.movFromMem(Rdi, Rbp, kCount);
  cg.shlImm8(Rdi, 4);
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbp, kOutPtr, Rax);
  cg.movFromMem(R8, Rbp, kCount);
  cg.movFromMem(R10, Rbp, kResult);
  cg.movToMem(R10, kTableLengthOffset, R8);
  cg.movToMem(R10, kTableCapacityOffset, R8);
  cg.movToMem(R10, kTableBlockOffset, Rax);
  cg.movFromMem(Rax, Rbp, kFrom);
  cg.movToMem(Rbp, kIndex, Rax);
  cg.movImmToMem(Rbp, kTotal, 0);
  cg.bind(arrayCopyLoop);
  selectGoto(arrayDone);
  cg.movFromMem(R8, R12, kTableBlockOffset);
  cg.movFromMem(R9, Rbp, kIndex);
  cg.shlImm8(R9, 4);
  cg.addReg(R8, R9);  // &source[i]
  cg.movFromMem(R10, Rbp, kOutPtr);
  cg.movFromMem(R11, Rbp, kTotal);
  cg.shlImm8(R11, 4);
  cg.addReg(R10, R11);  // &out[k]
  cg.movFromMem(Rax, R8, 0);
  cg.movFromMem(Rcx, R8, 8);
  cg.movToMem(R10, 0, Rax);
  cg.movToMem(R10, 8, Rcx);
  cg.movFromMem(R8, Rbp, kIndex);
  cg.movFromMem(R9, Rbp, kStep);
  cg.addReg(R8, R9);
  cg.movToMem(Rbp, kIndex, R8);
  cg.movFromMem(R8, Rbp, kTotal);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kTotal, R8);
  cg.jmp(arrayCopyLoop);
  cg.bind(arrayDone);
  cg.movImm(Rax, kTagArray);
  cg.movFromMem(Rdx, Rbp, kResult);
  cg.jmp(done);

  // ---- string: count the selected code points and their bytes, then copy.
  cg.bind(strBody);
  cg.movFromMem(Rax, Rbp, kFrom);
  cg.movToMem(Rbp, kIndex, Rax);
  cg.movImmToMem(Rbp, kTotal, 0);
  cg.bind(strCountLoop);
  selectGoto(strCountDone);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, kIndex);
  cg.callLabel(env.utf8CharAt);  // rax = bytes, rdx = byte length
  cg.movFromMem(R8, Rbp, kTotal);
  cg.addReg(R8, Rdx);
  cg.movToMem(Rbp, kTotal, R8);
  cg.movFromMem(R8, Rbp, kIndex);
  cg.movFromMem(R9, Rbp, kStep);
  cg.addReg(R8, R9);
  cg.movToMem(Rbp, kIndex, R8);
  cg.jmp(strCountLoop);
  cg.bind(strCountDone);
  cg.movFromMem(Rdi, Rbp, kTotal);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbp, kResult, Rax);
  cg.movFromMem(R8, Rbp, kTotal);
  cg.movToMem(Rax, 0, R8);
  cg.addImm32(Rax, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.movToMem(Rbp, kOutPtr, Rax);  // the write cursor
  cg.movFromMem(Rax, Rbp, kFrom);
  cg.movToMem(Rbp, kIndex, Rax);
  cg.bind(strCopyLoop);
  selectGoto(strDone);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, kIndex);
  cg.callLabel(env.utf8CharAt);  // rax = bytes, rdx = byte length
  cg.movReg(Rsi, Rax);
  cg.movReg(Rcx, Rdx);
  cg.movFromMem(Rdi, Rbp, kOutPtr);
  cg.repMovsb();
  cg.movToMem(Rbp, kOutPtr, Rdi);  // the cursor advanced past the copied bytes
  cg.movFromMem(R8, Rbp, kIndex);
  cg.movFromMem(R9, Rbp, kStep);
  cg.addReg(R8, R9);
  cg.movToMem(Rbp, kIndex, R8);
  cg.jmp(strCopyLoop);
  cg.bind(strDone);
  cg.movImm(Rax, kTagStr);
  cg.movFromMem(Rdx, Rbp, kResult);
  cg.jmp(done);

  cg.bind(failStart);
  emitFailCall(cg, env.fail, msgSliceStart);
  cg.bind(failEnd);
  emitFailCall(cg, env.fail, msgSliceEnd);
  cg.bind(failStepType);
  emitFailCall(cg, env.fail, msgSliceStepType);
  cg.bind(failStep);
  emitFailCall(cg, env.fail, msgSliceStep);
  cg.bind(failType);
  emitTypeQuoteFail(cg, msgSliceType, Rax, R12);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);

  cg.bind(done);
  cg.addImm32(Rsp, 176);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// ---------------------------------------------- public builtins (PHASE 7c.5a)
//
// Each is a machine-code function with the proto convention
// (`AotValue entry(AotValue* args, long argc, AotClosure* self)`), seeded into
// its global slot as an `AotClosure` by the chunk, so a `CALL` reaches it
// through the ordinary `oskel_call` path. The argument window grows *down* from
// `args`, so args[0] is at [rdi + 0] (tag) and [rdi + 8] (payload) and args[1]
// at [rdi - 16] / [rdi - 8]. Every one is a pure value transformation -- no
// libm, no libc -- so it links into the freestanding binary.

// `AotValue oskel_builtin_len(AotValue* args, long argc, AotClosure* self)`: the
// public `len`. Strings count UTF-8 code points, arrays and maps their entries,
// ranges the distance between the bounds (inclusive-aware, saturated at
// INT64_MAX), matching the hosted `len`. Any other type is a runtime error.
void emitLenBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                    const DataBlob& msgLenType, const DataBlob& msgArgc) {
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asArray = cg.newLabel();
  CodeGen::Label asMap = cg.newLabel();
  CodeGen::Label asRange = cg.newLabel();
  CodeGen::Label rangeAsc = cg.newLabel();
  CodeGen::Label rangeSat = cg.newLabel();
  CodeGen::Label rangeUse = cg.newLabel();
  CodeGen::Label rangeMax = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  // Exactly one argument. Without this the operand below is read from a window
  // that was never written (`len()` with no arguments), and a second argument
  // is silently ignored -- the VM raises "expects 1 argument" for both.
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);   // tag
  cg.movFromMem(R12, Rdi, 8);   // payload
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, asStr);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, asArray);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, asMap);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, asRange);
  emitTypeQuoteFail(cg, msgLenType, Rax, R12);

  cg.bind(asStr);
  cg.movReg(Rdi, R12);
  cg.callLabel(env.utf8Len);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  cg.bind(asArray);
  cg.movFromMem(Rdx, R12, kTableLengthOffset);
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  cg.bind(asMap);
  cg.movFromMem(Rdx, R12, kTableLengthOffset);
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  cg.bind(asRange);
  cg.movFromMem(R8, R12, kRangeStartOffset);
  cg.movFromMem(R9, R12, kRangeEndOffset);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ge, rangeAsc);
  cg.movReg(R10, R8);
  cg.subReg(R10, R9);           // descending: start - end
  cg.jmp(rangeSat);
  cg.bind(rangeAsc);
  cg.movReg(R10, R9);
  cg.subReg(R10, R8);           // ascending: end - start
  cg.bind(rangeSat);
  cg.movFromMem(R8, R12, kRangeInclusiveOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, rangeUse);
  cg.movImm(R11, -1);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Eq, rangeMax);    // distance == UINT64_MAX: saturate
  cg.addImm32(R10, 1);
  cg.bind(rangeMax);
  cg.movImm(R11, -1);
  cg.shrImm8(R11, 1);           // INT64_MAX
  cg.cmpReg(R10, R11);
  // Unsigned compare, matching the hosted `Range::length` (the span magnitude
  // is unsigned, so a signed `le` misses the overflow case).
  cg.jcc(Jcc::Be, rangeUse);
  cg.movReg(R10, R11);
  cg.bind(rangeUse);
  cg.movReg(Rdx, R10);
  cg.movImm(Rax, kTagInt);

  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_type(AotValue* args, long argc, AotClosure* self)`: the
// public `type`. The tag selects a constant type-name string, allocated into the
// arena, matching the hosted `Value::typeName` names.
void emitTypeBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                     CodeGen::Label fail, const DataBlob& msgOom, const DataBlob& tNil,
                     const DataBlob& tBool, const DataBlob& tInt, const DataBlob& tFloat,
                     const DataBlob& tDouble, const DataBlob& tStr, const DataBlob& tArray,
                     const DataBlob& tMap, const DataBlob& tFunc, const DataBlob& tRange,
                     const DataBlob& tError, const DataBlob& tPtr,
                     const DataBlob& tCString, const DataBlob& msgArgc) {
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asFloat = cg.newLabel();
  CodeGen::Label asDouble = cg.newLabel();
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asArray = cg.newLabel();
  CodeGen::Label asMap = cg.newLabel();
  CodeGen::Label asMapPlain = cg.newLabel();
  CodeGen::Label asFunc = cg.newLabel();
  CodeGen::Label asRange = cg.newLabel();
  CodeGen::Label asError = cg.newLabel();
  CodeGen::Label asPtr = cg.newLabel();
  CodeGen::Label asCString = cg.newLabel();
  CodeGen::Label buildConst = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  // Exactly one argument: with none, the tag read below comes from a window
  // nobody wrote, so `type()` returned a garbage name instead of failing.
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);   // tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, asNil);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Eq, asBool);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, asInt);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, asFloat);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, asDouble);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, asStr);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, asArray);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, asMap);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFunc));
  cg.jcc(Jcc::Eq, asFunc);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, asRange);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagError));
  cg.jcc(Jcc::Eq, asError);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagPtr));
  cg.jcc(Jcc::Eq, asPtr);
  cg.jmp(asNil);  // every AotTag is covered; default is defensive only

  // A pointer handle reports the FFI type it was built as, exactly like the
  // hosted `typeName`: `cstring` for a text handle, `ptr` for everything else.
  cg.bind(asPtr);
  cg.movFromMem(Rax, Rdi, 8);   // the handle
  cg.movFromMem(Rax, Rax, kPtrKindOffset);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kPtrKindCString));
  cg.jcc(Jcc::Eq, asCString);
  cg.leaRipLabel(R8, tPtr.label);
  cg.movImm(R12, static_cast<std::int64_t>(tPtr.text.size()));
  cg.jmp(buildConst);
  cg.bind(asCString);
  cg.leaRipLabel(R8, tCString.label);
  cg.movImm(R12, static_cast<std::int64_t>(tCString.text.size()));
  cg.jmp(buildConst);

#define OSKEL_TYPE_CASE(name, blob)          \
  cg.bind(name);                            \
  cg.leaRipLabel(R8, (blob).label);         \
  cg.movImm(R12, static_cast<std::int64_t>((blob).text.size())); \
  cg.jmp(buildConst);
  OSKEL_TYPE_CASE(asNil, tNil)
  OSKEL_TYPE_CASE(asBool, tBool)
  OSKEL_TYPE_CASE(asInt, tInt)
  OSKEL_TYPE_CASE(asFloat, tFloat)
  OSKEL_TYPE_CASE(asDouble, tDouble)
  OSKEL_TYPE_CASE(asStr, tStr)
  OSKEL_TYPE_CASE(asArray, tArray)
  // A class instance reports its class name; a plain map reports "map".
  cg.bind(asMap);
  cg.movFromMem(Rax, Rdi, 8);   // the map payload
  cg.movFromMem(Rax, Rax, kTableClassNameOffset);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, asMapPlain);
  cg.movFromMem(R12, Rax, 0);   // the class-name length
  cg.movReg(R8, Rax);
  cg.addImm32(R8, static_cast<std::int32_t>(kStringDataOffset));
  cg.jmp(buildConst);
  cg.bind(asMapPlain);
  cg.leaRipLabel(R8, tMap.label);
  cg.movImm(R12, static_cast<std::int64_t>(tMap.text.size()));
  cg.jmp(buildConst);
  OSKEL_TYPE_CASE(asFunc, tFunc)
  OSKEL_TYPE_CASE(asRange, tRange)
  OSKEL_TYPE_CASE(asError, tError)
#undef OSKEL_TYPE_CASE

  cg.bind(buildConst);
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, R12);     // store the length
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rsi, R8);
  cg.movReg(Rcx, R12);
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);

  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `int64_t oskel_parse_int_text(char* data, long length)`: parses the text of an
// `int(string)` conversion (PHASE 7c.5b.2a). The hosted `int` trims the text and
// then calls `strtoll` with a base taken from a leading `0x`/`0b`/`0o` prefix (a
// leading zero alone keeps base 10); the whole text must be consumed and the
// value must fit in an int64, or the conversion fails. `strtoll` re-detects a
// `0x`/`0b` prefix once more inside the body (so `"0x0x10"` is 16), which is
// reproduced here so the two engines agree byte for byte. `data` is in rdi and
// `length` in rsi on entry, the result is in rax. Pure integer code -- no libm,
// no libc -- so it links into the freestanding binary.
void emitIntTextParser(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label trimLeft = cg.newLabel();
  CodeGen::Label trimLeftSkip = cg.newLabel();
  CodeGen::Label trimRight = cg.newLabel();
  CodeGen::Label trimRightSkip = cg.newLabel();
  CodeGen::Label trimRightDone = cg.newLabel();
  CodeGen::Label consumeSign = cg.newLabel();
  CodeGen::Label afterSign = cg.newLabel();
  CodeGen::Label prefix16 = cg.newLabel();
  CodeGen::Label prefix2 = cg.newLabel();
  CodeGen::Label prefix8 = cg.newLabel();
  CodeGen::Label noPrefix = cg.newLabel();
  CodeGen::Label recheckX = cg.newLabel();
  CodeGen::Label recheckB = cg.newLabel();
  CodeGen::Label skipRecheckX = cg.newLabel();
  CodeGen::Label skipRecheckB = cg.newLabel();
  CodeGen::Label afterRecheck = cg.newLabel();
  CodeGen::Label haveLimit = cg.newLabel();
  CodeGen::Label digitLoop = cg.newLabel();
  CodeGen::Label notDec = cg.newLabel();
  CodeGen::Label tryUpper = cg.newLabel();
  CodeGen::Label haveDigit = cg.newLabel();
  CodeGen::Label accumulate = cg.newLabel();
  CodeGen::Label digitsDone = cg.newLabel();
  CodeGen::Label bytesDone = cg.newLabel();
  CodeGen::Label finish = cg.newLabel();
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label badOverflow = cg.newLabel();
  CodeGen::Label badEmpty = cg.newLabel();

  constexpr std::int64_t kInt64Max = 0x7FFFFFFFFFFFFFFFLL;
  constexpr std::int64_t kInt64Min = static_cast<std::int64_t>(0x8000000000000000ULL);

  cg.bind(label);
  cg.push(Rbx);
  cg.addReg(Rsi, Rdi);                    // rsi = end = data + length

  // Trim leading whitespace, as the hosted `trim` does.
  cg.bind(trimLeft);
  cg.cmpReg(Rdi, Rsi);
  cg.jcc(Jcc::Ae, trimRight);
  cg.movzxFromMem(Rcx, Rdi, 0);
  cg.cmpImm32(Rcx, 32); cg.jcc(Jcc::Eq, trimLeftSkip);
  cg.cmpImm32(Rcx, 9);  cg.jcc(Jcc::Eq, trimLeftSkip);
  cg.cmpImm32(Rcx, 10); cg.jcc(Jcc::Eq, trimLeftSkip);
  cg.cmpImm32(Rcx, 11); cg.jcc(Jcc::Eq, trimLeftSkip);
  cg.cmpImm32(Rcx, 12); cg.jcc(Jcc::Eq, trimLeftSkip);
  cg.cmpImm32(Rcx, 13); cg.jcc(Jcc::Eq, trimLeftSkip);
  cg.jmp(trimRight);
  cg.bind(trimLeftSkip);
  cg.addImm32(Rdi, 1);
  cg.jmp(trimLeft);

  // Trim trailing whitespace.
  cg.bind(trimRight);
  cg.cmpReg(Rdi, Rsi);
  cg.jcc(Jcc::Ae, trimRightDone);
  cg.movzxFromMem(Rcx, Rsi, -1);
  cg.cmpImm32(Rcx, 32); cg.jcc(Jcc::Eq, trimRightSkip);
  cg.cmpImm32(Rcx, 9);  cg.jcc(Jcc::Eq, trimRightSkip);
  cg.cmpImm32(Rcx, 10); cg.jcc(Jcc::Eq, trimRightSkip);
  cg.cmpImm32(Rcx, 11); cg.jcc(Jcc::Eq, trimRightSkip);
  cg.cmpImm32(Rcx, 12); cg.jcc(Jcc::Eq, trimRightSkip);
  cg.cmpImm32(Rcx, 13); cg.jcc(Jcc::Eq, trimRightSkip);
  cg.jmp(trimRightDone);
  cg.bind(trimRightSkip);
  cg.addImm32(Rsi, -1);
  cg.jmp(trimRight);

  cg.bind(trimRightDone);
  cg.cmpReg(Rdi, Rsi);
  cg.jcc(Jcc::Ae, badEmpty);              // empty text

  // Optional sign. r10 holds the negative flag.
  cg.xorReg(R10, R10);
  cg.movzxFromMem(Rcx, Rdi, 0);
  cg.cmpImm32(Rcx, '+');
  cg.jcc(Jcc::Eq, consumeSign);
  cg.cmpImm32(Rcx, '-');
  cg.jcc(Jcc::Ne, afterSign);
  cg.movImm(R10, 1);
  cg.bind(consumeSign);
  cg.addImm32(Rdi, 1);
  cg.bind(afterSign);

  // Base prefix: `0x`/`0X`, `0b`/`0B`, `0o`/`0O`, or base 10.
  cg.movImm(R8, 10);
  cg.leaMem(Rcx, Rdi, 2);
  cg.cmpReg(Rcx, Rsi);
  cg.jcc(Jcc::A, noPrefix);               // fewer than two characters left
  cg.movzxFromMem(Rcx, Rdi, 0);
  cg.cmpImm32(Rcx, '0');
  cg.jcc(Jcc::Ne, noPrefix);
  cg.movzxFromMem(Rcx, Rdi, 1);
  cg.cmpImm32(Rcx, 'x'); cg.jcc(Jcc::Eq, prefix16);
  cg.cmpImm32(Rcx, 'X'); cg.jcc(Jcc::Eq, prefix16);
  cg.cmpImm32(Rcx, 'b'); cg.jcc(Jcc::Eq, prefix2);
  cg.cmpImm32(Rcx, 'B'); cg.jcc(Jcc::Eq, prefix2);
  cg.cmpImm32(Rcx, 'o'); cg.jcc(Jcc::Eq, prefix8);
  cg.cmpImm32(Rcx, 'O'); cg.jcc(Jcc::Eq, prefix8);
  cg.jmp(noPrefix);
  cg.bind(prefix16);
  cg.movImm(R8, 16);
  cg.addImm32(Rdi, 2);
  cg.jmp(noPrefix);
  cg.bind(prefix2);
  cg.movImm(R8, 2);
  cg.addImm32(Rdi, 2);
  cg.jmp(noPrefix);
  cg.bind(prefix8);
  cg.movImm(R8, 8);
  cg.addImm32(Rdi, 2);

  // `strtoll` re-detects a `0x`/`0b` prefix once more in the body; base 8 does
  // not, so `"0o0o17"` is an error while `"0x0x10"` is 16.
  cg.bind(noPrefix);
  cg.cmpImm32(R8, 16);
  cg.jcc(Jcc::Eq, recheckX);
  cg.cmpImm32(R8, 2);
  cg.jcc(Jcc::Eq, recheckB);
  cg.jmp(afterRecheck);
  cg.bind(recheckX);
  cg.leaMem(Rcx, Rdi, 2);
  cg.cmpReg(Rcx, Rsi);
  cg.jcc(Jcc::A, afterRecheck);
  cg.movzxFromMem(Rcx, Rdi, 0);
  cg.cmpImm32(Rcx, '0');
  cg.jcc(Jcc::Ne, afterRecheck);
  cg.movzxFromMem(Rcx, Rdi, 1);
  cg.cmpImm32(Rcx, 'x'); cg.jcc(Jcc::Eq, skipRecheckX);
  cg.cmpImm32(Rcx, 'X'); cg.jcc(Jcc::Ne, afterRecheck);
  cg.bind(skipRecheckX);
  cg.addImm32(Rdi, 2);
  cg.jmp(afterRecheck);
  cg.bind(recheckB);
  cg.leaMem(Rcx, Rdi, 2);
  cg.cmpReg(Rcx, Rsi);
  cg.jcc(Jcc::A, afterRecheck);
  cg.movzxFromMem(Rcx, Rdi, 0);
  cg.cmpImm32(Rcx, '0');
  cg.jcc(Jcc::Ne, afterRecheck);
  cg.movzxFromMem(Rcx, Rdi, 1);
  cg.cmpImm32(Rcx, 'b'); cg.jcc(Jcc::Eq, skipRecheckB);
  cg.cmpImm32(Rcx, 'B'); cg.jcc(Jcc::Ne, afterRecheck);
  cg.bind(skipRecheckB);
  cg.addImm32(Rdi, 2);

  // The largest magnitude the result may reach: 2^63-1 for a positive value and
  // 2^63 for a negative one. From it the per-digit cutoff and remainder make the
  // overflow test `acc < cutoff || (acc == cutoff && digit <= rem)`.
  cg.bind(afterRecheck);
  cg.movImm(Rax, kInt64Max);
  cg.testReg(R10);
  cg.jcc(Jcc::Eq, haveLimit);
  cg.movImm(Rax, kInt64Min);
  cg.bind(haveLimit);
  cg.divUnsigned(R8);                     // rax = cutoff, rdx = rem
  cg.movReg(R11, Rax);
  cg.movReg(Rax, Rdx);

  cg.movReg(Rbx, Rdi);                    // position of the first digit
  cg.xorReg(R9, R9);                      // acc = 0
  cg.bind(digitLoop);
  cg.cmpReg(Rdi, Rsi);
  cg.jcc(Jcc::Ae, digitsDone);
  cg.movzxFromMem(Rcx, Rdi, 0);
  cg.movImm(Rdx, -1);                     // digit, -1 means "not a digit"
  cg.cmpImm32(Rcx, '0'); cg.jcc(Jcc::B, notDec);
  cg.cmpImm32(Rcx, '9'); cg.jcc(Jcc::A, notDec);
  cg.movReg(Rdx, Rcx);
  cg.subRegImm32(Rdx, '0');
  cg.jmp(haveDigit);
  cg.bind(notDec);
  cg.cmpImm32(R8, 16);
  cg.jcc(Jcc::Ne, haveDigit);
  cg.cmpImm32(Rcx, 'a'); cg.jcc(Jcc::B, tryUpper);
  cg.cmpImm32(Rcx, 'f'); cg.jcc(Jcc::A, tryUpper);
  cg.movReg(Rdx, Rcx);
  cg.subRegImm32(Rdx, 'a');
  cg.addImm32(Rdx, 10);
  cg.jmp(haveDigit);
  cg.bind(tryUpper);
  cg.cmpImm32(Rcx, 'A'); cg.jcc(Jcc::B, haveDigit);
  cg.cmpImm32(Rcx, 'F'); cg.jcc(Jcc::A, haveDigit);
  cg.movReg(Rdx, Rcx);
  cg.subRegImm32(Rdx, 'A');
  cg.addImm32(Rdx, 10);
  cg.bind(haveDigit);
  cg.cmpImm32(Rdx, 0);
  cg.jcc(Jcc::Lt, digitsDone);            // not a digit
  cg.cmpReg(Rdx, R8);
  cg.jcc(Jcc::Ae, digitsDone);            // digit out of the base's range
  cg.cmpReg(R9, R11);
  cg.jcc(Jcc::A, badOverflow);            // acc > cutoff: overflow
  cg.jcc(Jcc::B, accumulate);
  cg.cmpReg(Rdx, Rax);
  cg.jcc(Jcc::A, badOverflow);            // acc == cutoff and digit > rem
  cg.bind(accumulate);
  cg.imulReg(R9, R8);
  cg.addReg(R9, Rdx);
  cg.addImm32(Rdi, 1);
  cg.jmp(digitLoop);

  cg.bind(digitsDone);
  cg.cmpReg(Rdi, Rbx);
  cg.jcc(Jcc::Eq, bad);                   // no digits
  cg.cmpReg(Rdi, Rsi);
  cg.jcc(Jcc::Eq, bytesDone);             // consumed every byte
  cg.movzxFromMem(Rcx, Rdi, 0);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Ne, bad);                   // trailing characters
  // A NUL ends the text, exactly as it ends a C string for the hosted
  // `strtoll`: `int("4\u00002")` is 4 in both engines.
  cg.bind(bytesDone);
  cg.testReg(R10);
  cg.jcc(Jcc::Eq, finish);
  cg.negReg(R9);
  cg.bind(finish);
  cg.movReg(Rdx, R9);                     // rdx = the value
  cg.xorReg(Rax, Rax);                    // rax = 0, success
  cg.pop(Rbx);
  cg.retInsn();

  cg.bind(bad);
  cg.movImm(Rax, 1);                      // 1 = malformed text
  cg.pop(Rbx);
  cg.retInsn();

  cg.bind(badOverflow);
  cg.movImm(Rax, 2);                      // 2 = out of the int64 range
  cg.pop(Rbx);
  cg.retInsn();

  cg.bind(badEmpty);
  cg.movImm(Rax, 3);                      // 3 = empty after trimming
  cg.pop(Rbx);
  cg.retInsn();
}

// The constant type names a conversion quotes in its message, in the order
// `Value::typeName` reports them (PHASE 7c.5b.2c). Built at the call site from

// The fragments and fixed messages the numeric conversions need to reproduce
// the hosted engine's error strings exactly (PHASE 7c.5b.2c). A message that
// quotes the operand is `prefix + <operand> + midQuote + <target>` plus an
// optional suffix; the rest are whole strings the hosted side also emits.
struct ConvertMessages {
  DataBlob prefix;        // "cannot convert '"
  DataBlob midQuote;      // "' to "
  DataBlob targetInt;     // "int"
  DataBlob targetFloat;   // "float"
  DataBlob targetDouble;  // "double"
  DataBlob outOfRange;    // " (out of range)"
  DataBlob useInf;        // " (use inf, -inf or nan)"
  DataBlob emptyInt;      // "cannot convert an empty string to int"
  DataBlob emptyFloat;    // "cannot convert an empty string to float"
  DataBlob emptyDouble;   // "cannot convert an empty string to double"
  DataBlob nonFiniteInt;  // "cannot convert a non-finite float to int"
  DataBlob rangeInt;      // "float is out of the integer range"
  DataBlob argcInt;       // "int() expects 1 argument"
  DataBlob argcFloat;     // "float() expects 1 argument"
  DataBlob argcDouble;    // "double() expects 1 argument"
};

// Frame slots the conversion bodies share. r13-r15 belong to the arena and the
// global table (`kAotArenaCursor`/`kAotArenaEnd`/`kAotGlobalBase`), so the
// builder and the operand bytes live on the stack instead.
constexpr std::int32_t kConvOperandPtr = -24;
constexpr std::int32_t kConvOperandLen = -32;
constexpr std::int32_t kConvBuilder = -40;
constexpr std::int32_t kConvParseCode = -48;
// The slots above span rbp-24..rbp-56 after the two saved registers at
// rbp-8/rbp-16; 48 bytes keeps rsp 16-byte aligned at every `call`.
constexpr std::int32_t kConvFrameBytes = 48;

void saveConvertFrame(CodeGen& cg) {
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, kConvFrameBytes);
}

void restoreConvertFrame(CodeGen& cg) {
  cg.addImm32(Rsp, kConvFrameBytes);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// Appends a constant blob to the builder whose address is in `[rbp + slot]`.
// An empty blob appends nothing, which is how an optional suffix is spelled.
void emitAppendBlob(CodeGen& cg, CodeGen::Label sbAppend, std::int32_t builderSlot,
                    const DataBlob& blob) {
  if (blob.text.empty()) return;
  cg.movFromMem(Rdi, Rbp, builderSlot);
  cg.leaRipLabel(Rsi, blob.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(blob.text.size()));
  cg.callLabel(sbAppend);
}

// Stores the constant type-name bytes for the tag in `rax` into the two shared
// slots -- the same names `Value::typeName` returns. A pointer handle reports
// `cstring` or `ptr` by its kind; an unknown tag falls back to `nil`. It reads
// the argument window from rbx.
void emitTypeNameOfTag(CodeGen& cg, const TypeNameBlobs& names) {
  struct Case {
    std::int64_t tag;
    const DataBlob* blob;
  };
  const Case cases[] = {
      {kTagNil, names.nil},
      {kTagBool, names.boolValue},
      {kTagInt, names.intValue},
      {kTagFloat, names.floatValue},
      {kTagDouble, names.doubleValue},
      {kTagStr, names.str},
      {kTagArray, names.array},
      {kTagMap, names.map},
      {kTagFunc, names.func},
      {kTagRange, names.range},
      {kTagError, names.error},
  };
  CodeGen::Label asPtr = cg.newLabel();
  CodeGen::Label asCString = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

#define OSKEL_CONV_NAME(blob)                                        \
  cg.leaRipLabel(Rax, (blob)->label);                                \
  cg.movToMem(Rbp, kConvOperandPtr, Rax);                            \
  cg.movImm(Rax, static_cast<std::int64_t>((blob)->text.size()));    \
  cg.movToMem(Rbp, kConvOperandLen, Rax);                            \
  cg.jmp(done)

  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagPtr));
  cg.jcc(Jcc::Eq, asPtr);
  for (const Case& c : cases) {
    CodeGen::Label next = cg.newLabel();
    cg.cmpImm32(Rax, static_cast<std::int32_t>(c.tag));
    cg.jcc(Jcc::Ne, next);
    OSKEL_CONV_NAME(c.blob);
    cg.bind(next);
  }
  OSKEL_CONV_NAME(names.nil);

  cg.bind(asPtr);
  cg.movFromMem(Rax, Rbx, 8);   // the handle
  cg.movFromMem(Rax, Rax, kPtrKindOffset);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kPtrKindCString));
  cg.jcc(Jcc::Eq, asCString);
  OSKEL_CONV_NAME(names.ptr);

  cg.bind(asCString);
  OSKEL_CONV_NAME(names.cstring);

#undef OSKEL_CONV_NAME

  cg.bind(done);
}

// Emits the failure path bound to `label`: it builds
// `prefix + <operand> + midQuote + target + suffix` in the arena and hands it to
// `failDynamic`, which is how the hosted engine reports these. It never returns,
// so the caller jumps here instead of falling through. It runs in the caller's
// frame, so the operand bytes must already sit in the shared slots.
void emitQuotedConvertFail(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                           CodeGen::Label failDynamic, CodeGen::Label sbNew,
                           CodeGen::Label sbAppend, const DataBlob& msgOom,
                           const ConvertMessages& msg, const DataBlob& target,
                           const DataBlob* suffix) {
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  cg.callLabel(sbNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbp, kConvBuilder, Rax);
  emitAppendBlob(cg, sbAppend, kConvBuilder, msg.prefix);
  cg.movFromMem(Rdi, Rbp, kConvBuilder);
  cg.movFromMem(Rsi, Rbp, kConvOperandPtr);
  cg.movFromMem(Rdx, Rbp, kConvOperandLen);
  cg.callLabel(sbAppend);
  emitAppendBlob(cg, sbAppend, kConvBuilder, msg.midQuote);
  emitAppendBlob(cg, sbAppend, kConvBuilder, target);
  if (suffix != nullptr) emitAppendBlob(cg, sbAppend, kConvBuilder, *suffix);
  cg.movFromMem(Rax, Rbp, kConvBuilder);
  cg.movFromMem(Rdi, Rax, kSbDataOffset);
  cg.movFromMem(Rsi, Rax, kSbLengthOffset);
  cg.callLabel(failDynamic);
  cg.ud2();

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// Appends the signed integer in `reg` to the builder whose address is in
// `[rbp + builderSlot]`. `reg` is preserved, so one message may format the same
// value twice; rax, rcx, rdx, r8, r9 and the 24-byte scratch block below
// `rbp + scratchSlot` are clobbered. The digits are produced
// least-significant first into the scratch block and appended in one call.
//
// The helper is shared by the arity body and the message builders so there is
// exactly one decimal formatter.
void emitAppendInt(CodeGen& cg, CodeGen::Label sbAppend, std::int32_t builderSlot,
                   std::int32_t scratchSlot, X64Reg reg) {
  CodeGen::Label positive = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label emit = cg.newLabel();

  cg.leaMem(R8, Rbp, scratchSlot);   // one past the last digit
  cg.movReg(Rax, reg);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, positive);
  cg.movImm(R9, 1);                  // remember the sign
  cg.negReg(Rax);
  cg.jmp(loop);
  cg.bind(positive);
  cg.xorReg(R9, R9);

  cg.bind(loop);
  cg.movImm(Rcx, 10);
  cg.divUnsigned(Rcx);               // rax /= 10, rdx = digit (rdx pre-zeroed)
  cg.addImm32(Rdx, static_cast<std::int32_t>('0'));
  cg.subRegImm32(R8, 1);
  cg.movByteToMem(R8, 0, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, loop);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, emit);
  cg.subRegImm32(R8, 1);
  cg.movImmByteToMem(R8, 0, static_cast<std::uint8_t>('-'));

  cg.bind(emit);
  cg.movFromMem(Rdi, Rbp, builderSlot);
  cg.movReg(Rsi, R8);
  cg.leaMem(Rdx, Rbp, scratchSlot);
  cg.subReg(Rdx, R8);
  cg.callLabel(sbAppend);
}

// Emits `sbNew` + one append per part + `failDynamic(data, length)`, then a
// trap. Never returns.
//
// The numbers are parked in the frame before the first append: an append
// clobbers rdi/rsi/rdx (and the decimal formatter also rcx/r8/r9), so a part
// that reads a register after another part has been appended would read
// garbage.
//
// This body does not call `prologue`, so it works against the caller's rbp and
// its `sub rsp` is relative to whatever the caller already pushed. The frame is
// therefore sized for the worst case, a caller at `rsp == rbp`: the lowest byte
// touched is the last digit, at `kScratch - kScratchBytes`. The caller's own
// slots (rbp-24..rbp-56 today) sit above `kBuilder`, so the two never overlap.
void emitFailParts(CodeGen& cg, CodeGen::Label failDynamic, CodeGen::Label sbNew,
                   CodeGen::Label sbAppend, std::initializer_list<FailPart> parts) {
  constexpr std::int32_t kBuilder = -96;
  constexpr std::int32_t kNumBase = -104;  // 8 bytes per number part
  constexpr std::int32_t kScratchBytes = 24;  // 20 digits of int64 plus a sign
  constexpr std::int32_t kScratch = -160;  // one past the last digit
  constexpr std::int32_t kMaxNumbers = 7;  // before the slots reach kScratch
  // Derived, so moving a slot cannot leave the frame too small: the last digit
  // lands at `kScratch - kScratchBytes`, and rsp may start at rbp.
  constexpr std::int32_t kFrameBytes = -kScratch + kScratchBytes + 8;

  std::int32_t numberCount = 0;
  for (const FailPart& part : parts) {
    if (part.text == nullptr) {
      ++numberCount;
    }
  }
  if (numberCount > kMaxNumbers) {
    throw std::logic_error("native/runtime: too many numbers in one message");
  }

  cg.subRegImm32(Rsp, kFrameBytes);
  std::int32_t numSlot = kNumBase;
  for (const FailPart& part : parts) {
    if (part.text == nullptr) {
      cg.movToMem(Rbp, numSlot, part.reg);
      numSlot -= 8;
    }
  }

  cg.callLabel(sbNew);
  cg.movToMem(Rbp, kBuilder, Rax);
  numSlot = kNumBase;
  for (const FailPart& part : parts) {
    if (part.text != nullptr) {
      emitAppendBlob(cg, sbAppend, kBuilder, *part.text);
    } else {
      cg.movFromMem(Rax, Rbp, numSlot);
      numSlot -= 8;
      emitAppendInt(cg, sbAppend, kBuilder, kScratch, Rax);
    }
  }
  cg.movFromMem(Rax, Rbp, kBuilder);
  cg.movFromMem(Rdi, Rax, kSbDataOffset);
  cg.movFromMem(Rsi, Rax, kSbLengthOffset);
  cg.callLabel(failDynamic);
  cg.ud2();
}

void emitArityFail(CodeGen& cg, CodeGen::Label label, CodeGen::Label failDynamic,
                   CodeGen::Label sbNew, CodeGen::Label sbAppend,
                   const DataBlob& textGot) {
  constexpr std::int32_t kArgc = -16;
  constexpr std::int32_t kBuilder = -24;
  constexpr std::int32_t kMsgPtr = -32;
  constexpr std::int32_t kMsgLen = -40;
  constexpr std::int32_t kScratch = -72;

  cg.bind(label);
  prologue(cg);
  cg.subRegImm32(Rsp, 96);
  cg.movToMem(Rbp, kArgc, Rdx);
  cg.movToMem(Rbp, kMsgPtr, Rdi);
  cg.movToMem(Rbp, kMsgLen, Rsi);

  cg.callLabel(sbNew);
  cg.movToMem(Rbp, kBuilder, Rax);

  // The fixed message, then ", got ", then the count in decimal.
  cg.movReg(Rdi, Rax);
  cg.movFromMem(Rsi, Rbp, kMsgPtr);
  cg.movFromMem(Rdx, Rbp, kMsgLen);
  cg.callLabel(sbAppend);
  emitAppendBlob(cg, sbAppend, kBuilder, textGot);
  cg.movFromMem(Rax, Rbp, kArgc);
  emitAppendInt(cg, sbAppend, kBuilder, kScratch, Rax);

  cg.movFromMem(Rax, Rbp, kBuilder);
  cg.movFromMem(Rdi, Rax, kSbDataOffset);
  cg.movFromMem(Rsi, Rax, kSbLengthOffset);
  cg.callLabel(failDynamic);
  cg.ud2();
}

// Appends the `typeName()` of the value parked at `[rbp + tagSlot]` /
// `[rbp + payloadSlot]` to the builder at `[rbp + builderSlot]`. A tag the
// vocabulary does not know reports `nil`, which is what `Value::typeName`
// does; a pointer handle reports its kind (`cstring` or `ptr`). The slots are
// the caller's, so the caller keeps them disjoint from its own frame.
void emitTypeNameAppend(CodeGen& cg, CodeGen::Label sbAppend, std::int32_t tagSlot,
                        std::int32_t payloadSlot, std::int32_t builderSlot) {
  CodeGen::Label asPtr = cg.newLabel();
  CodeGen::Label asCString = cg.newLabel();
  CodeGen::Label emit = cg.newLabel();

  struct Case {
    std::int64_t tag;
    const DataBlob* blob;
  };
  const TypeNameBlobs& names = *gMsgText.names;
  const Case cases[] = {
      {kTagNil, names.nil},         {kTagBool, names.boolValue},
      {kTagInt, names.intValue},    {kTagFloat, names.floatValue},
      {kTagDouble, names.doubleValue}, {kTagStr, names.str},
      {kTagArray, names.array},     {kTagMap, names.map},
      {kTagFunc, names.func},       {kTagRange, names.range},
      {kTagError, names.error},
  };

#define OSKEL_TYPE_NAME(blob)                                  \
  emitAppendBlob(cg, sbAppend, builderSlot, *(blob));          \
  cg.jmp(emit)

  cg.movFromMem(Rax, Rbp, tagSlot);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagPtr));
  cg.jcc(Jcc::Eq, asPtr);
  for (const Case& c : cases) {
    CodeGen::Label next = cg.newLabel();
    cg.cmpImm32(Rax, static_cast<std::int32_t>(c.tag));
    cg.jcc(Jcc::Ne, next);
    OSKEL_TYPE_NAME(c.blob);
    cg.bind(next);
  }
  OSKEL_TYPE_NAME(names.nil);

  cg.bind(asPtr);
  cg.movFromMem(Rax, Rbp, payloadSlot);
  cg.movFromMem(Rax, Rax, kPtrKindOffset);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kPtrKindCString));
  cg.jcc(Jcc::Eq, asCString);
  OSKEL_TYPE_NAME(names.ptr);

  cg.bind(asCString);
  OSKEL_TYPE_NAME(names.cstring);

#undef OSKEL_TYPE_NAME

  cg.bind(emit);
}

// `prefix + " '" + type1 + "'" + separator + "'" + type2 + "'"`: the two
// operator messages (`cannot compare 'int' with 'string'`,
// `operator 'in' is not supported for 'int' and 'int'`).
void emitTwoTypeFail(CodeGen& cg, const DataBlob& prefix, const DataBlob& separator,
                     X64Reg tag1, X64Reg payload1, X64Reg tag2, X64Reg payload2) {
  constexpr std::int32_t kBuilder = -24;
  constexpr std::int32_t kTag1 = -32;
  constexpr std::int32_t kPayload1 = -40;
  constexpr std::int32_t kTag2 = -48;
  constexpr std::int32_t kPayload2 = -56;

  cg.subRegImm32(Rsp, 80);
  cg.movToMem(Rbp, kTag1, tag1);
  cg.movToMem(Rbp, kPayload1, payload1);
  cg.movToMem(Rbp, kTag2, tag2);
  cg.movToMem(Rbp, kPayload2, payload2);
  cg.callLabel(gMsgText.sbNew);
  cg.movToMem(Rbp, kBuilder, Rax);
  emitAppendBlob(cg, gMsgText.sbAppend, kBuilder, prefix);
  emitAppendBlob(cg, gMsgText.sbAppend, kBuilder, *gMsgText.textTypeQuote);
  emitTypeNameAppend(cg, gMsgText.sbAppend, kTag1, kPayload1, kBuilder);
  emitAppendBlob(cg, gMsgText.sbAppend, kBuilder, *gMsgText.quoteClose);
  emitAppendBlob(cg, gMsgText.sbAppend, kBuilder, separator);
  emitAppendBlob(cg, gMsgText.sbAppend, kBuilder, *gMsgText.textTypeQuote);
  emitTypeNameAppend(cg, gMsgText.sbAppend, kTag2, kPayload2, kBuilder);
  emitAppendBlob(cg, gMsgText.sbAppend, kBuilder, *gMsgText.quoteClose);
  cg.movFromMem(Rax, Rbp, kBuilder);
  cg.movFromMem(Rdi, Rax, kSbDataOffset);
  cg.movFromMem(Rsi, Rax, kSbLengthOffset);
  cg.callLabel(gMsgText.failDynamic);
  cg.ud2();
}

// The failure path bound to `label`, for the `, got '<type>'` tail: it quotes
// the type name of the value whose tag is in `tagReg` and whose payload is in
// `payloadReg`, reproducing the hosted `"..., got '" + typeName + "'"`. A tag
// the vocabulary does not know reports `nil`, which is what `Value::typeName`
// does; a pointer handle reports its kind (`cstring` or `ptr`).
void emitFailGotType(CodeGen& cg, CodeGen::Label label, CodeGen::Label failDynamic,
                     CodeGen::Label sbNew, CodeGen::Label sbAppend,
                     const DataBlob& prefix, const DataBlob& textGot,
                     const DataBlob& textQuoteEnd,
                     [[maybe_unused]] const TypeNameBlobs& names,
                     X64Reg tagReg, X64Reg payloadReg) {
  // The appends clobber rdi/rsi/rdx, so the tag and payload are parked first.
  constexpr std::int32_t kBuilder = -24;
  constexpr std::int32_t kTag = -32;
  constexpr std::int32_t kPayload = -40;

  cg.bind(label);
  cg.subRegImm32(Rsp, 48);
  cg.movToMem(Rbp, kTag, tagReg);
  cg.movToMem(Rbp, kPayload, payloadReg);
  cg.callLabel(sbNew);
  cg.movToMem(Rbp, kBuilder, Rax);
  emitAppendBlob(cg, sbAppend, kBuilder, prefix);
  emitAppendBlob(cg, sbAppend, kBuilder, textGot);
  emitTypeNameAppend(cg, sbAppend, kTag, kPayload, kBuilder);
  emitAppendBlob(cg, sbAppend, kBuilder, textQuoteEnd);
  cg.movFromMem(Rax, Rbp, kBuilder);
  cg.movFromMem(Rdi, Rax, kSbDataOffset);
  cg.movFromMem(Rsi, Rax, kSbLengthOffset);
  cg.callLabel(failDynamic);
  cg.ud2();
}

// `prefix + ", got '" + type + "'"`: the builtin argument-type tail.
void emitTypeTailFail(CodeGen& cg, const DataBlob& prefix, X64Reg tagReg,
                      X64Reg payloadReg) {
  emitFailGotType(cg, cg.newLabel(), gMsgText.failDynamic, gMsgText.sbNew,
                  gMsgText.sbAppend, prefix, *gMsgText.textGot,
                  *gMsgText.quoteClose, *gMsgText.names, tagReg, payloadReg);
}

// `prefix + " '" + type + "'"`: the value-type tail (`cannot slice a value of
// type 'int'`), where the prefix already names the condition.
void emitTypeQuoteFail(CodeGen& cg, const DataBlob& prefix, X64Reg tagReg,
                       X64Reg payloadReg) {
  emitFailGotType(cg, cg.newLabel(), gMsgText.failDynamic, gMsgText.sbNew,
                  gMsgText.sbAppend, prefix, *gMsgText.textTypeQuote,
                  *gMsgText.quoteClose, *gMsgText.names, tagReg, payloadReg);
}

// `type '<t>' has no member '<name>'`: the one message that quotes both the
// receiver's type and the member name (`name` is an `AotString*`). The hosted
// `memberGet` words both, so a bare "type has no member" is a divergence.
void emitNoMemberFail(CodeGen& cg, X64Reg tagReg, X64Reg payloadReg,
                      X64Reg nameReg) {
  constexpr std::int32_t kBuilder = -24;
  constexpr std::int32_t kTag = -32;
  constexpr std::int32_t kPayload = -40;
  constexpr std::int32_t kName = -48;

  cg.subRegImm32(Rsp, 64);
  cg.movToMem(Rbp, kTag, tagReg);
  cg.movToMem(Rbp, kPayload, payloadReg);
  cg.movToMem(Rbp, kName, nameReg);
  cg.callLabel(gMsgText.sbNew);
  cg.movToMem(Rbp, kBuilder, Rax);
  emitAppendBlob(cg, gMsgText.sbAppend, kBuilder, *gMsgText.textTypeOpen);
  emitTypeNameAppend(cg, gMsgText.sbAppend, kTag, kPayload, kBuilder);
  emitAppendBlob(cg, gMsgText.sbAppend, kBuilder, *gMsgText.textHasNoMember);
  cg.movFromMem(Rax, Rbp, kName);
  cg.movFromMem(Rdx, Rax, 0);  // the name's byte length
  cg.movReg(Rsi, Rax);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rdi, Rbp, kBuilder);
  cg.callLabel(gMsgText.sbAppend);
  emitAppendBlob(cg, gMsgText.sbAppend, kBuilder, *gMsgText.quoteClose);
  cg.movFromMem(Rax, Rbp, kBuilder);
  cg.movFromMem(Rdi, Rax, kSbDataOffset);
  cg.movFromMem(Rsi, Rax, kSbLengthOffset);
  cg.callLabel(gMsgText.failDynamic);
  cg.ud2();
}

// The three numeric conversions (PHASE 7c.5b.1): the public `int`, `float` and
// `double`. A numeric or boolean operand converts with pure SSE -- no libm, no
// libc -- so it links into the freestanding binary. A string operand goes
// through the text parsers, which report *why* they failed so the error can
// name the operand, the target and the cause exactly as the hosted engine does
// (PHASE 7c.5b.2c). `args` is in rdi, `argc` in rsi on entry.
void emitNumericConversionBuiltins(CodeGen& cg, CodeGen::Label intLabel,
                                   CodeGen::Label floatLabel, CodeGen::Label doubleLabel,
                                   CodeGen::Label fail, CodeGen::Label failDynamic,
                                   CodeGen::Label intTextParser,
                                   CodeGen::Label floatTextParser, CodeGen::Label sbNew,
                                   CodeGen::Label sbAppend, const DataBlob& msgOom,
                                   const ConvertMessages& msg, const TypeNameBlobs& names) {
  constexpr int Xmm3 = kXmmBase + 3;
  constexpr int Xmm4 = kXmmBase + 4;
  // 2^63 and -2^63 as binary64 bit patterns: the exclusive/inclusive bounds the
  // hosted `int` uses (`-2^63 <= x < 2^63`).
  constexpr std::int64_t kTwoPow63 = 0x43E0000000000000ULL;
  constexpr std::int64_t kNegTwoPow63 = 0xC3E0000000000000ULL;
  // Stack frame for `oskel_parse_float_text`: the scratch block followed by the
  // 8-byte output word, rounded up so rsp stays 16-byte aligned at the call.
  constexpr std::int64_t kParseScratchBytes =
      static_cast<std::int64_t>(parse_float::kParseScratchBytes);
  constexpr std::int64_t kParseFrame =
      (kParseScratchBytes + 8 + 15) & ~static_cast<std::int64_t>(15);

  // --- int ---
  {
    CodeGen::Label asInt = cg.newLabel();
    CodeGen::Label asBool = cg.newLabel();
    CodeGen::Label asNum = cg.newLabel();
    CodeGen::Label asStr = cg.newLabel();
    CodeGen::Label strOk = cg.newLabel();
    CodeGen::Label badArgc = cg.newLabel();
    CodeGen::Label badType = cg.newLabel();
    CodeGen::Label badEmpty = cg.newLabel();
    CodeGen::Label failFinite = cg.newLabel();
    CodeGen::Label failRange = cg.newLabel();
    CodeGen::Label quoted = cg.newLabel();
    CodeGen::Label done = cg.newLabel();

    cg.bind(intLabel);
    prologue(cg);
    saveConvertFrame(cg);
    cg.movReg(Rbx, Rdi);
    cg.movReg(R12, Rsi);
    cg.cmpImm32(R12, 1);
    cg.jcc(Jcc::Ne, badArgc);
    cg.movFromMem(Rax, Rbx, 0);   // tag
    cg.movFromMem(Rcx, Rbx, 8);   // payload
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
    cg.jcc(Jcc::Eq, asInt);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagBool));
    cg.jcc(Jcc::Eq, asBool);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFloat));
    cg.jcc(Jcc::Eq, asNum);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagDouble));
    cg.jcc(Jcc::Eq, asNum);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
    cg.jcc(Jcc::Eq, asStr);
    cg.jmp(badType);

    cg.bind(asStr);
    // The operand's bytes go into the shared slots for the message; the parser
    // takes them in rdi/rsi and returns the status in rax, the value in rdx.
    cg.movReg(Rax, Rcx);
    cg.addImm32(Rax, static_cast<std::int32_t>(kStringDataOffset));
    cg.movToMem(Rbp, kConvOperandPtr, Rax);
    cg.movFromMem(Rax, Rcx, 0);
    cg.movToMem(Rbp, kConvOperandLen, Rax);
    cg.movFromMem(Rdi, Rbp, kConvOperandPtr);
    cg.movFromMem(Rsi, Rbp, kConvOperandLen);
    cg.callLabel(intTextParser);
    cg.testReg(Rax);
    cg.jcc(Jcc::Eq, strOk);
    // 3 is "empty after trimming"; 1 and 2 (malformed, out of range) both quote
    // the operand, exactly as `parseIntText` does.
    cg.cmpImm32(Rax, 3);
    cg.jcc(Jcc::Eq, badEmpty);
    cg.jmp(quoted);

    cg.bind(strOk);
    cg.movImm(Rax, kTagInt);
    cg.jmp(done);

    cg.bind(asInt);
    cg.movReg(Rdx, Rcx);
    cg.movImm(Rax, kTagInt);
    cg.jmp(done);

    cg.bind(asBool);
    cg.movReg(Rdx, Rcx);
    cg.movImm(Rax, kTagInt);
    cg.jmp(done);

    cg.bind(asNum);
    cg.movqToXmm(Xmm0, Rcx);
    // Reject NaN/Inf: `x - x == 0` holds for every finite number and is
    // unordered (PF=1) for NaN and both infinities.
    cg.movsdReg(Xmm1, Xmm0);
    cg.subsd(Xmm1, Xmm0);
    cg.xorps(Xmm2, Xmm2);
    cg.ucomisd(Xmm1, Xmm2);
    cg.jcc(Jcc::P, failFinite);
    cg.jcc(Jcc::Ne, failFinite);
    // `ucomisd` sets the flags as an unsigned compare, so `jae` is `x >= 2^63`
    // and `ja` on `-2^63 ?= x` is `x < -2^63`. NaN is already rejected.
    cg.movImm(R9, kTwoPow63);
    cg.movqToXmm(Xmm3, R9);
    cg.ucomisd(Xmm0, Xmm3);
    cg.jcc(Jcc::Ae, failRange);
    cg.movImm(R9, kNegTwoPow63);
    cg.movqToXmm(Xmm4, R9);
    cg.ucomisd(Xmm4, Xmm0);
    cg.jcc(Jcc::A, failRange);
    cg.cvttsd2si(Rdx, Xmm0);
    cg.movImm(Rax, kTagInt);
    cg.jmp(done);

    cg.bind(done);
    restoreConvertFrame(cg);

    cg.bind(badArgc);
    emitArityFailCall(cg, msg.argcInt);
    cg.bind(badEmpty);
    emitFailCall(cg, fail, msg.emptyInt);
    cg.bind(failFinite);
    emitFailCall(cg, fail, msg.nonFiniteInt);
    cg.bind(failRange);
    emitFailCall(cg, fail, msg.rangeInt);
    cg.bind(badType);
    emitTypeNameOfTag(cg, names);
    cg.jmp(quoted);
    emitQuotedConvertFail(cg, quoted, fail, failDynamic, sbNew, sbAppend, msgOom, msg,
                          msg.targetInt, nullptr);
  }

  // --- double ---
  {
    CodeGen::Label asInt = cg.newLabel();
    CodeGen::Label asBool = cg.newLabel();
    CodeGen::Label asFloat = cg.newLabel();
    CodeGen::Label asDouble = cg.newLabel();
    CodeGen::Label asStr = cg.newLabel();
    CodeGen::Label strOk = cg.newLabel();
    CodeGen::Label badArgc = cg.newLabel();
    CodeGen::Label badType = cg.newLabel();
    CodeGen::Label badEmpty = cg.newLabel();
    CodeGen::Label quoted = cg.newLabel();
    CodeGen::Label useInf = cg.newLabel();
    CodeGen::Label finish = cg.newLabel();
    CodeGen::Label done = cg.newLabel();

    cg.bind(doubleLabel);
    prologue(cg);
    saveConvertFrame(cg);
    cg.movReg(Rbx, Rdi);
    cg.movReg(R12, Rsi);
    cg.cmpImm32(R12, 1);
    cg.jcc(Jcc::Ne, badArgc);
    cg.movFromMem(Rax, Rbx, 0);   // tag
    cg.movFromMem(Rcx, Rbx, 8);   // payload
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
    cg.jcc(Jcc::Eq, asInt);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagBool));
    cg.jcc(Jcc::Eq, asBool);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFloat));
    cg.jcc(Jcc::Eq, asFloat);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagDouble));
    cg.jcc(Jcc::Eq, asDouble);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
    cg.jcc(Jcc::Eq, asStr);
    cg.jmp(badType);

    cg.bind(asStr);
    cg.movReg(Rax, Rcx);
    cg.addImm32(Rax, static_cast<std::int32_t>(kStringDataOffset));
    cg.movToMem(Rbp, kConvOperandPtr, Rax);
    cg.movFromMem(Rax, Rcx, 0);
    cg.movToMem(Rbp, kConvOperandLen, Rax);
    cg.subRegImm32(Rsp, static_cast<std::int32_t>(kParseFrame));
    cg.movFromMem(Rdi, Rbp, kConvOperandPtr);
    cg.movFromMem(Rsi, Rbp, kConvOperandLen);
    cg.movReg(Rcx, Rsp);                                              // scratch
    cg.movReg(Rdx, Rsp);
    cg.addImm32(Rdx, static_cast<std::int32_t>(kParseScratchBytes));  // outBits
    cg.callLabel(floatTextParser);
    cg.movToMem(Rbp, kConvParseCode, Rax);
    cg.movFromMem(Rdx, Rsp, static_cast<std::int32_t>(kParseScratchBytes));
    cg.movReg(Rcx, Rsp);
    cg.addImm32(Rcx, static_cast<std::int32_t>(kParseFrame));
    cg.movReg(Rsp, Rcx);
    cg.movFromMem(Rax, Rbp, kConvParseCode);
    cg.testReg(Rax);
    cg.jcc(Jcc::Eq, strOk);
    cg.cmpImm32(Rax, 3);
    cg.jcc(Jcc::Eq, badEmpty);
    cg.cmpImm32(Rax, 2);
    cg.jcc(Jcc::Eq, useInf);
    cg.jmp(quoted);

    cg.bind(strOk);
    cg.movImm(Rax, kTagDouble);
    cg.jmp(done);

    cg.bind(asInt);
    cg.cvtsi2sd(Xmm0, Rcx);
    cg.jmp(finish);

    cg.bind(asBool);
    cg.xorps(Xmm0, Xmm0);
    cg.testReg(Rcx);
    cg.jcc(Jcc::Eq, finish);
    cg.movImm(R8, 1);
    cg.cvtsi2sd(Xmm0, R8);
    cg.jmp(finish);

    cg.bind(asFloat);
    cg.movqToXmm(Xmm0, Rcx);      // a float payload is already widened binary64
    cg.jmp(finish);

    cg.bind(asDouble);
    cg.movqToXmm(Xmm0, Rcx);      // a double payload is already a binary64 value

    cg.bind(finish);
    cg.movqFromXmm(Rdx, Xmm0);
    cg.movImm(Rax, kTagDouble);
    cg.jmp(done);

    cg.bind(done);
    restoreConvertFrame(cg);

    cg.bind(badArgc);
    emitArityFailCall(cg, msg.argcDouble);
    cg.bind(badEmpty);
    emitFailCall(cg, fail, msg.emptyDouble);
    cg.bind(badType);
    emitTypeNameOfTag(cg, names);
    cg.jmp(quoted);
    emitQuotedConvertFail(cg, quoted, fail, failDynamic, sbNew, sbAppend, msgOom, msg,
                          msg.targetDouble, nullptr);
    emitQuotedConvertFail(cg, useInf, fail, failDynamic, sbNew, sbAppend, msgOom, msg,
                          msg.targetDouble, &msg.useInf);
  }

  // --- float ---
  {
    CodeGen::Label asInt = cg.newLabel();
    CodeGen::Label asBool = cg.newLabel();
    CodeGen::Label asFloat = cg.newLabel();
    CodeGen::Label asDouble = cg.newLabel();
    CodeGen::Label asStr = cg.newLabel();
    CodeGen::Label strFailed = cg.newLabel();
    CodeGen::Label badArgc = cg.newLabel();
    CodeGen::Label badType = cg.newLabel();
    CodeGen::Label badEmpty = cg.newLabel();
    CodeGen::Label quoted = cg.newLabel();
    CodeGen::Label useInf = cg.newLabel();
    CodeGen::Label outOfRange = cg.newLabel();
    CodeGen::Label checkOriginal = cg.newLabel();
    CodeGen::Label roundToFloat = cg.newLabel();
    CodeGen::Label storeFloat = cg.newLabel();
    CodeGen::Label done = cg.newLabel();

    cg.bind(floatLabel);
    prologue(cg);
    saveConvertFrame(cg);
    cg.movReg(Rbx, Rdi);
    cg.movReg(R12, Rsi);
    cg.cmpImm32(R12, 1);
    cg.jcc(Jcc::Ne, badArgc);
    cg.movFromMem(Rax, Rbx, 0);   // tag
    cg.movFromMem(Rcx, Rbx, 8);   // payload
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
    cg.jcc(Jcc::Eq, asInt);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagBool));
    cg.jcc(Jcc::Eq, asBool);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFloat));
    cg.jcc(Jcc::Eq, asFloat);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagDouble));
    cg.jcc(Jcc::Eq, asDouble);
    cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
    cg.jcc(Jcc::Eq, asStr);
    cg.jmp(badType);

    // A string parses to binary64 and then narrows, matching the hosted `float`
    // (which parses with `strtod` and casts the result to `float`).
    cg.bind(asStr);
    cg.movReg(Rax, Rcx);
    cg.addImm32(Rax, static_cast<std::int32_t>(kStringDataOffset));
    cg.movToMem(Rbp, kConvOperandPtr, Rax);
    cg.movFromMem(Rax, Rcx, 0);
    cg.movToMem(Rbp, kConvOperandLen, Rax);
    cg.subRegImm32(Rsp, static_cast<std::int32_t>(kParseFrame));
    cg.movFromMem(Rdi, Rbp, kConvOperandPtr);
    cg.movFromMem(Rsi, Rbp, kConvOperandLen);
    cg.movReg(Rcx, Rsp);
    cg.movReg(Rdx, Rsp);
    cg.addImm32(Rdx, static_cast<std::int32_t>(kParseScratchBytes));
    cg.callLabel(floatTextParser);
    cg.movToMem(Rbp, kConvParseCode, Rax);
    cg.movFromMem(Rax, Rsp, static_cast<std::int32_t>(kParseScratchBytes));
    cg.movReg(Rcx, Rsp);
    cg.addImm32(Rcx, static_cast<std::int32_t>(kParseFrame));
    cg.movReg(Rsp, Rcx);
    cg.movFromMem(R11, Rbp, kConvParseCode);
    cg.testReg(R11);
    cg.jcc(Jcc::Ne, strFailed);
    cg.movqToXmm(Xmm0, Rax);
    cg.jmp(roundToFloat);

    cg.bind(asInt);
    cg.cvtsi2sd(Xmm0, Rcx);
    cg.jmp(roundToFloat);

    cg.bind(asBool);
    cg.xorps(Xmm0, Xmm0);
    cg.testReg(Rcx);
    cg.jcc(Jcc::Eq, roundToFloat);
    cg.movImm(R8, 1);
    cg.cvtsi2sd(Xmm0, R8);
    cg.jmp(roundToFloat);

    cg.bind(asFloat);
    cg.movqToXmm(Xmm0, Rcx);
    cg.jmp(roundToFloat);

    cg.bind(asDouble);
    cg.movqToXmm(Xmm0, Rcx);

    cg.bind(roundToFloat);
    cg.cvtsd2ss(Xmm3, Xmm0);      // round to binary32 ...
    cg.cvtss2sd(Xmm4, Xmm3);      // ... and widen back, as the hosted `Float` holds
    // A finite binary64 that narrows to an infinity is a range error the hosted
    // `float` reports separately; a value that was already infinite or NaN is
    // passed through, because the hosted side only checks the narrowed result.
    cg.movsdReg(Xmm1, Xmm4);
    cg.subsd(Xmm1, Xmm4);
    cg.xorps(Xmm2, Xmm2);
    cg.ucomisd(Xmm1, Xmm2);       // PF=1 when the narrowed value is inf or NaN
    cg.jcc(Jcc::P, checkOriginal);
    cg.jmp(storeFloat);
    cg.bind(checkOriginal);
    cg.movsdReg(Xmm1, Xmm0);
    cg.subsd(Xmm1, Xmm0);
    cg.ucomisd(Xmm1, Xmm2);       // PF=1 when the original was inf or NaN
    cg.jcc(Jcc::P, storeFloat);
    cg.jmp(outOfRange);

    cg.bind(storeFloat);
    cg.movqFromXmm(Rdx, Xmm4);
    cg.movImm(Rax, kTagFloat);
    cg.jmp(done);

    cg.bind(strFailed);
    cg.cmpImm32(R11, 3);
    cg.jcc(Jcc::Eq, badEmpty);
    cg.cmpImm32(R11, 2);
    cg.jcc(Jcc::Eq, useInf);
    cg.jmp(quoted);

    cg.bind(done);
    restoreConvertFrame(cg);

    cg.bind(badArgc);
    emitArityFailCall(cg, msg.argcFloat);
    cg.bind(badEmpty);
    emitFailCall(cg, fail, msg.emptyFloat);
    cg.bind(badType);
    emitTypeNameOfTag(cg, names);
    cg.jmp(quoted);
    emitQuotedConvertFail(cg, quoted, fail, failDynamic, sbNew, sbAppend, msgOom, msg,
                          msg.targetFloat, nullptr);
    emitQuotedConvertFail(cg, useInf, fail, failDynamic, sbNew, sbAppend, msgOom, msg,
                          msg.targetFloat, &msg.useInf);
    emitQuotedConvertFail(cg, outOfRange, fail, failDynamic, sbNew, sbAppend, msgOom, msg,
                          msg.targetFloat, &msg.outOfRange);
  }
}

// `AotValue oskel_builtin_repr(AotValue* args, long argc, AotClosure* self)`: the
// public `repr`. It renders through the same recursive repr `$str` uses for a
// collection, which is exactly `Value::repr()` for every type (a top-level
// string is quoted).
void emitReprBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label reprToString,
                     const DataBlob& msgArgc) {
  CodeGen::Label badArgc = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rsi, Rdi, 8);   // payload
  cg.movFromMem(Rdi, Rdi, 0);   // tag
  cg.callLabel(reprToString);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_bool(AotValue* args, long argc, AotClosure* self)`: the
// public `bool`, i.e. the operand's truthiness (`0` and `nil` are false, every
// other value true).
void emitBoolBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label truthy,
                     const DataBlob& msgArgc) {
  CodeGen::Label badArgc = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rsi, Rdi, 8);   // payload
  cg.movFromMem(Rdi, Rdi, 0);   // tag
  cg.callLabel(truthy);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagBool);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_sqrt(AotValue* args, long argc, AotClosure* self)`:
// the standard library's `sqrt` (PHASE 7c.5b.3), which the AOT backend provides
// from `sqrtsd` instead of a host binding. `sqrtsd` is defined to be the
// correctly-rounded square root, so the hardware result and the hosted
// `std::sqrt` agree bit for bit (transcendentals like `pow`/`sin` do not, which
// is why they are not stubbed and a program that uses one is rejected instead).
//
// The stub follows the hosted semantics: the operand must be a number (int,
// float or double), a negative argument is a run-time error, and the result is
// always a `double` (`sqrt(9)` is `3.0`, like the hosted `Value(std::sqrt(...))`).
// A float operand arrives already widened to binary64 (the boxed `Float` holds
// the widened value), so the payload loads directly; widening it again would
// read the binary32 bits as binary64 (PHASE 7f.1).
void emitSqrtBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                     const DataBlob& msgArgc, const DataBlob& msgType,
                     const DataBlob& msgNegative) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asFloat = cg.newLabel();
  CodeGen::Label asDouble = cg.newLabel();
  CodeGen::Label compute = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label negative = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);   // tag
  cg.movFromMem(Rcx, Rdi, 8);   // payload
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, asInt);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, asFloat);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, asDouble);
  emitTypeTailFail(cg, msgType, Rax, Rcx);

  cg.bind(asInt);
  cg.cvtsi2sd(Xmm0, Rcx);
  cg.jmp(compute);

  cg.bind(asFloat);
  cg.movqToXmm(Xmm0, Rcx);      // a float payload is already widened binary64
  cg.jmp(compute);

  cg.bind(asDouble);
  cg.movqToXmm(Xmm0, Rcx);      // a double payload is already binary64

  cg.bind(compute);
  // Reject `value < 0`, the hosted check. `ucomisd` sets CF for "below", but a
  // NaN (unordered) also sets CF (with PF=1), so the parity branch comes first:
  // a NaN is not below zero and reaches `sqrtsd`, which returns NaN, exactly
  // like `std::sqrt(NaN)`.
  CodeGen::Label sqrtNow = cg.newLabel();
  cg.xorps(Xmm1, Xmm1);
  cg.ucomisd(Xmm0, Xmm1);
  cg.jcc(Jcc::P, sqrtNow);      // unordered (NaN): not negative
  cg.jcc(Jcc::B, negative);     // CF=1 and ordered: value < 0

  cg.bind(sqrtNow);
  cg.sqrtsd(Xmm0, Xmm0);
  cg.movqFromXmm(Rdx, Xmm0);
  cg.movImm(Rax, kTagDouble);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);

  cg.bind(negative);
  emitFailCall(cg, fail, msgNegative);
}

// `AotValue oskel_builtin_ord(AotValue* args, long argc, AotClosure* self)`: the
// public `ord`. The operand must be a string holding exactly one UTF-8 code
// point; the code point is returned as an int.
void emitOrdBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label utf8Len,
                    CodeGen::Label fail, const DataBlob& msgOrdType,
                    const DataBlob& msgOrdChar, const DataBlob& msgArgc) {
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label isStr = cg.newLabel();
  CodeGen::Label okLen = cg.newLabel();
  CodeGen::Label single = cg.newLabel();
  CodeGen::Label two = cg.newLabel();
  CodeGen::Label three = cg.newLabel();
  CodeGen::Label four = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isStr);
  cg.movFromMem(Rdx, Rdi, 8);
  emitTypeTailFail(cg, msgOrdType, Rax, Rdx);

  cg.bind(isStr);
  cg.movFromMem(R12, Rdi, 8);   // AotString*
  cg.movReg(Rdi, R12);
  cg.callLabel(utf8Len);
  cg.cmpImm32(Rax, 1);
  cg.jcc(Jcc::Eq, okLen);
  emitFailCall(cg, fail, msgOrdChar);

  cg.bind(okLen);
  cg.movzxFromMem(R8, R12, kStringDataOffset);  // lead byte
  cg.cmpImm32(R8, 0x80);
  cg.jcc(Jcc::Lt, single);
  cg.movReg(R9, R8);
  cg.andImm32(R9, 0xE0);
  cg.cmpImm32(R9, 0xC0);
  cg.jcc(Jcc::Eq, two);
  cg.movReg(R9, R8);
  cg.andImm32(R9, 0xF0);
  cg.cmpImm32(R9, 0xE0);
  cg.jcc(Jcc::Eq, three);
  cg.movReg(R9, R8);
  cg.andImm32(R9, 0xF8);
  cg.cmpImm32(R9, 0xF0);
  cg.jcc(Jcc::Eq, four);
  cg.jmp(single);               // invalid lead: fall back to the byte value

  cg.bind(single);
  cg.movReg(Rdx, R8);
  cg.jmp(done);

  cg.bind(two);
  cg.andImm32(R8, 0x1F);
  cg.shlImm8(R8, 6);
  cg.movzxFromMem(R10, R12, kStringDataOffset + 1);
  cg.andImm32(R10, 0x3F);
  cg.orReg(R8, R10);
  cg.movReg(Rdx, R8);
  cg.jmp(done);

  cg.bind(three);
  cg.andImm32(R8, 0x0F);
  cg.shlImm8(R8, 12);
  cg.movzxFromMem(R10, R12, kStringDataOffset + 1);
  cg.andImm32(R10, 0x3F);
  cg.shlImm8(R10, 6);
  cg.orReg(R8, R10);
  cg.movzxFromMem(R11, R12, kStringDataOffset + 2);
  cg.andImm32(R11, 0x3F);
  cg.orReg(R8, R11);
  cg.movReg(Rdx, R8);
  cg.jmp(done);

  cg.bind(four);
  cg.andImm32(R8, 0x07);
  cg.shlImm8(R8, 18);
  cg.movzxFromMem(R10, R12, kStringDataOffset + 1);
  cg.andImm32(R10, 0x3F);
  cg.shlImm8(R10, 12);
  cg.orReg(R8, R10);
  cg.movzxFromMem(R11, R12, kStringDataOffset + 2);
  cg.andImm32(R11, 0x3F);
  cg.shlImm8(R11, 6);
  cg.orReg(R8, R11);
  cg.movzxFromMem(Rcx, R12, kStringDataOffset + 3);
  cg.andImm32(Rcx, 0x3F);
  cg.orReg(R8, Rcx);
  cg.movReg(Rdx, R8);

  cg.bind(done);
  cg.movImm(Rax, kTagInt);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_chr(AotValue* args, long argc, AotClosure* self)`: the
// public `chr`. The operand must be an int in `0..0x10FFFF`; the code point is
// UTF-8 encoded into a fresh arena string.
void emitChrBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                    CodeGen::Label fail, const DataBlob& msgChrInt,
                    const DataBlob& msgChrRange, const DataBlob& msgChrSurrogate,
                    const DataBlob& msgOom, const DataBlob& msgArgc) {
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label isInt = cg.newLabel();
  CodeGen::Label len1 = cg.newLabel();
  CodeGen::Label len2 = cg.newLabel();
  CodeGen::Label len3 = cg.newLabel();
  CodeGen::Label alloc = cg.newLabel();
  CodeGen::Label enc1 = cg.newLabel();
  CodeGen::Label enc2 = cg.newLabel();
  CodeGen::Label enc3 = cg.newLabel();
  CodeGen::Label enc4 = cg.newLabel();
  CodeGen::Label failRange = cg.newLabel();
  CodeGen::Label failSurrogate = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, isInt);
  cg.movFromMem(Rdx, Rdi, 8);
  emitTypeTailFail(cg, msgChrInt, Rax, Rdx);

  cg.bind(isInt);
  cg.movFromMem(R8, Rdi, 8);    // code point
  cg.testReg(R8);
  cg.jcc(Jcc::S, failRange);    // negative
  cg.cmpImm32(R8, 0x10FFFF);
  cg.jcc(Jcc::Gt, failRange);
  // A surrogate (0xD800..0xDFFF) is not a Unicode scalar value; encoding it
  // would produce CESU-8, which is not valid UTF-8. `R8 - 0xD800 <= 0x7FF`
  // unsigned matches the whole range in one compare.
  cg.movReg(R9, R8);
  cg.subRegImm32(R9, 0xD800);
  cg.cmpImm32(R9, 0x7FF);
  cg.jcc(Jcc::Be, failSurrogate);
  cg.cmpImm32(R8, 0x7F);
  cg.jcc(Jcc::Le, len1);
  cg.cmpImm32(R8, 0x7FF);
  cg.jcc(Jcc::Le, len2);
  cg.cmpImm32(R8, 0xFFFF);
  cg.jcc(Jcc::Le, len3);
  cg.movImm(R12, 4);
  cg.jmp(alloc);
  cg.bind(len1);
  cg.movImm(R12, 1);
  cg.jmp(alloc);
  cg.bind(len2);
  cg.movImm(R12, 2);
  cg.jmp(alloc);
  cg.bind(len3);
  cg.movImm(R12, 3);

  cg.bind(alloc);
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, R12);     // store the length
  cg.movReg(Rbx, Rax);          // base
  cg.cmpImm32(R12, 1);
  cg.jcc(Jcc::Eq, enc1);
  cg.cmpImm32(R12, 2);
  cg.jcc(Jcc::Eq, enc2);
  cg.cmpImm32(R12, 3);
  cg.jcc(Jcc::Eq, enc3);
  cg.jmp(enc4);

  cg.bind(enc1);
  cg.movByteToMem(Rbx, kStringDataOffset, R8);
  cg.jmp(done);

  cg.bind(enc2);
  cg.movReg(R9, R8);
  cg.shrImm8(R9, 6);
  cg.movImm(R10, 0xC0);
  cg.orReg(R9, R10);
  cg.movByteToMem(Rbx, kStringDataOffset, R9);
  cg.movReg(R9, R8);
  cg.movImm(R10, 0x3F);
  cg.andReg(R9, R10);
  cg.movImm(R10, 0x80);
  cg.orReg(R9, R10);
  cg.movByteToMem(Rbx, kStringDataOffset + 1, R9);
  cg.jmp(done);

  cg.bind(enc3);
  cg.movReg(R9, R8);
  cg.shrImm8(R9, 12);
  cg.movImm(R10, 0xE0);
  cg.orReg(R9, R10);
  cg.movByteToMem(Rbx, kStringDataOffset, R9);
  cg.movReg(R9, R8);
  cg.shrImm8(R9, 6);
  cg.movImm(R10, 0x3F);
  cg.andReg(R9, R10);
  cg.movImm(R10, 0x80);
  cg.orReg(R9, R10);
  cg.movByteToMem(Rbx, kStringDataOffset + 1, R9);
  cg.movReg(R9, R8);
  cg.movImm(R10, 0x3F);
  cg.andReg(R9, R10);
  cg.movImm(R10, 0x80);
  cg.orReg(R9, R10);
  cg.movByteToMem(Rbx, kStringDataOffset + 2, R9);
  cg.jmp(done);

  cg.bind(enc4);
  cg.movReg(R9, R8);
  cg.shrImm8(R9, 18);
  cg.movImm(R10, 0xF0);
  cg.orReg(R9, R10);
  cg.movByteToMem(Rbx, kStringDataOffset, R9);
  cg.movReg(R9, R8);
  cg.shrImm8(R9, 12);
  cg.movImm(R10, 0x3F);
  cg.andReg(R9, R10);
  cg.movImm(R10, 0x80);
  cg.orReg(R9, R10);
  cg.movByteToMem(Rbx, kStringDataOffset + 1, R9);
  cg.movReg(R9, R8);
  cg.shrImm8(R9, 6);
  cg.movImm(R10, 0x3F);
  cg.andReg(R9, R10);
  cg.movImm(R10, 0x80);
  cg.orReg(R9, R10);
  cg.movByteToMem(Rbx, kStringDataOffset + 2, R9);
  cg.movReg(R9, R8);
  cg.movImm(R10, 0x3F);
  cg.andReg(R9, R10);
  cg.movImm(R10, 0x80);
  cg.orReg(R9, R10);
  cg.movByteToMem(Rbx, kStringDataOffset + 3, R9);

  cg.bind(done);
  cg.movReg(Rdx, Rbx);
  cg.movImm(Rax, kTagStr);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(failRange);
  emitFailCall(cg, fail, msgChrRange);
  cg.bind(failSurrogate);
  emitFailCall(cg, fail, msgChrSurrogate);
  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// The blobs and the hidden global a diagnostic-enabled sink needs to print
// `oskel: FILE:LINE: ` before the message (PHASE 7g.3). The line is read from
// `lineSlot` at the moment of the failure: the lowering stores the source line
// of the instruction it is about to run there before it runs it, so the value
// the sink sees is the line of the failing instruction. All four blobs live in
// the image and are addressed by label, like every other message.
struct DebugSink {
  int lineSlot = -1;   // hidden global holding the current source line
  DataBlob path;       // the source path as the command line spelled it
  DataBlob separator;  // `:` before the number and `: ` after it, split
  DataBlob tail;       // so the digits can go in between
};

// `void oskel_fail_dynamic(char* text, long length)`: the one runtime-error
// sink. It writes `oskel: ` + text + newline to stderr and exits 70 -- unless a
// `pcall` is active, in which case it hands the text to the innermost one and
// returns there instead (see `emitPcallBuiltin`). It is the dynamic counterpart
// of `oskel_fail`, which takes a constant blob; both end up here.
//
// `handlerSlot` is the hidden `$pcallHandler` global's index, or -1 when the
// program never calls `pcall`. With a handler installed the sink does not print
// and does not exit: it stores the message in that handler's record, pops the
// record, restores the `pcall` frame's `rsp`/`rbp` and jumps to its resume
// address. That is the one non-local return in the AOT runtime (BACKLOG §6 item
// 4). A program that never calls `pcall` keeps a sink with no handler check.
void emitFailDynamic(CodeGen& cg, CodeGen::Label label, CodeGen::Label write, CodeGen::Label exit,
                     const DataBlob& prefix, const DataBlob& newline, int handlerSlot,
                     const DebugSink* debug = nullptr) {
  CodeGen::Label toHandler = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);          // message bytes
  cg.movReg(R12, Rsi);          // message length

  if (handlerSlot >= 0) {
    cg.movFromMem(Rax, kAotGlobalBase, 16LL * handlerSlot + 8);
    cg.testReg(Rax);
    cg.jcc(Jcc::Ne, toHandler);
  }

  if (debug != nullptr) {
    // `oskel: ` first, so the line still starts with the word every existing
    // fixture greps for, then the position: `FILE:` `LINE` `: `.
    cg.leaRipLabel(Rdi, prefix.label);
    cg.movImm(Rsi, static_cast<std::int64_t>(prefix.text.size()));
    emitWriteToFd(cg, write, 2);
    cg.leaRipLabel(Rdi, debug->path.label);
    cg.movImm(Rsi, static_cast<std::int64_t>(debug->path.text.size()));
    emitWriteToFd(cg, write, 2);
    cg.leaRipLabel(Rdi, debug->separator.label);
    cg.movImm(Rsi, static_cast<std::int64_t>(debug->separator.text.size()));
    emitWriteToFd(cg, write, 2);
    // The formatter builds its digits below rbp, so the space has to be
    // reserved: a `call` would otherwise push a return address over them. The
    // line is loaded into rax immediately before the formatter, which moves it
    // to rax itself, so no register has to hold it across a call (r13 is the
    // arena cursor and must not be reused here).
    cg.subRegImm32(Rsp, 32);
    cg.movFromMem(Rax, kAotGlobalBase, 16LL * debug->lineSlot + 8);
    emitWriteUnsignedDecimal(cg, write, Rax);
    cg.addImm32(Rsp, 32);
    cg.leaRipLabel(Rdi, debug->tail.label);
    cg.movImm(Rsi, static_cast<std::int64_t>(debug->tail.text.size()));
    emitWriteToFd(cg, write, 2);
  } else {
    cg.leaRipLabel(Rdi, prefix.label);
    cg.movImm(Rsi, static_cast<std::int64_t>(prefix.text.size()));
    emitWriteToFd(cg, write, 2);
  }

  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  emitWriteToFd(cg, write, 2);
  cg.leaRipLabel(Rdi, newline.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(newline.text.size()));
  emitWriteToFd(cg, write, 2);
  cg.movImm(Rdi, kRuntimeErrorExit);
  cg.callLabel(exit);
  cg.ud2();

  if (handlerSlot < 0) return;

  // A `pcall` is active: hand it the message and leave. The record is the one
  // `oskel_builtin_pcall` built in its own frame:
  //   +0 previous handler, +8 saved rsp, +16 saved rbp, +24 resume address,
  //   +32 message bytes, +40 message length.
  cg.bind(toHandler);
  cg.movFromMem(Rcx, Rax, 0);   // previous handler
  cg.movFromMem(Rdx, Rax, 8);   // saved rsp
  cg.movFromMem(R8, Rax, 16);   // saved rbp
  cg.movFromMem(R9, Rax, 24);   // resume address
  cg.movToMem(Rax, 32, Rbx);    // message bytes
  cg.movToMem(Rax, 40, R12);    // message length
  cg.movToMem(kAotGlobalBase, 16LL * handlerSlot + 8, Rcx);
  cg.movReg(Rsp, Rdx);
  cg.movReg(Rbp, R8);
  cg.jmpReg(R9);
}

// `AotValue oskel_builtin_assert(AotValue* args, long argc, AotClosure* self)`: the
// public `assert`. `args[0]` must be truthy; a second string argument is the
// failure message. On success it returns nil.
void emitAssertBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label truthy,
                       CodeGen::Label fail, CodeGen::Label failDynamic,
                       const DataBlob& msgAssert) {
  CodeGen::Label pass = cg.newLabel();
  CodeGen::Label noMsg = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);          // args
  cg.movReg(R12, Rsi);          // argc
  cg.movFromMem(Rsi, Rbx, 8);
  cg.movFromMem(Rdi, Rbx, 0);
  cg.callLabel(truthy);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, pass);
  cg.cmpImm32(R12, 2);
  cg.jcc(Jcc::Lt, noMsg);
  cg.movFromMem(Rax, Rbx, -16); // args[1] tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, noMsg);
  cg.movFromMem(Rdi, Rbx, -8);  // args[1] payload (AotString*)
  cg.movFromMem(Rsi, Rdi, 0);   // length
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(failDynamic);
  cg.ud2();

  cg.bind(noMsg);
  emitFailCall(cg, fail, msgAssert);

  cg.bind(pass);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `AotValue oskel_builtin_panic(AotValue* args, long argc, AotClosure* self)`: the
// public `panic`. It always fails; a string first argument is the message.
void emitPanicBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                      CodeGen::Label failDynamic, const DataBlob& msgPanic) {
  CodeGen::Label noMsg = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);          // args
  cg.movReg(R12, Rsi);          // argc
  cg.testReg(R12);
  cg.jcc(Jcc::Eq, noMsg);
  cg.movFromMem(Rax, Rbx, 0);   // args[0] tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, noMsg);
  cg.movFromMem(Rdi, Rbx, 8);   // args[0] payload (AotString*)
  cg.movFromMem(Rsi, Rdi, 0);   // length
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(failDynamic);
  cg.ud2();

  cg.bind(noMsg);
  emitFailCall(cg, fail, msgPanic);
}

// `AotValue oskel_builtin_pcall(AotValue* args, long argc, AotClosure* self)`:
// the public `pcall(fn, args...)` (BACKLOG §6 item 4). It calls `fn` with the
// remaining arguments and turns a runtime error into a value: the result is a
// map `{ok, value, error}` -- `ok` true with the return value, or `ok` false,
// `value` nil and `error` an error value -- exactly the hosted builtin's shape.
//
// This is the one place the AOT runtime leaves a frame non-locally. A raised
// error normally reaches `oskel_fail_dynamic`, which prints and exits; while a
// `pcall` is active, that sink instead stores the message in the handler record
// this builtin leaves on its own stack and jumps back to the `resume` label
// below, after restoring this frame's `rsp`/`rbp`. The record also carries the
// previous handler, so a nested `pcall` and a normal return both restore the
// chain; a frame the error abandoned saved rbx/r12 in its own frame, so the two
// this builtin pushed are reloaded on the way back.
//
// The record is the first 48 bytes of the frame, because the sink walks it with
// constant offsets:
//   +0 previous handler, +8 saved rsp, +16 saved rbp, +24 resume address,
//   +32 message bytes, +40 message length.
// The rest of the 144-byte frame holds the call's inputs and results. 144 is a
// multiple of 16, so the two saved callee registers plus the frame keep `rsp`
// 16-byte aligned at the `call` below.
//
// `handlerSlot` is the hidden `$pcallHandler` global's index; the caller emits
// this body only when the program actually calls `pcall`.
void emitPcallBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label callLabel,
                      CodeGen::Label tableNew, CodeGen::Label memberSet,
                      CodeGen::Label allocLabel, CodeGen::Label fail,
                      CodeGen::Label failDynamic, const DataBlob& msgNotFunc,
                      const DataBlob& msgOom, CodeGen::Label okConst,
                      CodeGen::Label valueConst, CodeGen::Label errorConst,
                      int handlerSlot) {
  constexpr std::int64_t kPrev = -144;
  constexpr std::int64_t kSavedRsp = -136;
  constexpr std::int64_t kSavedRbp = -128;
  constexpr std::int64_t kResume = -120;
  constexpr std::int64_t kMsgBytes = -112;
  constexpr std::int64_t kMsgLength = -104;
  constexpr std::int64_t kArgs = -96;
  constexpr std::int64_t kArgc = -88;
  constexpr std::int64_t kCallee = -80;
  constexpr std::int64_t kStatus = -72;
  constexpr std::int64_t kValueTag = -64;
  constexpr std::int64_t kValuePayload = -56;
  constexpr std::int64_t kErrorTag = -48;
  constexpr std::int64_t kErrorPayload = -40;
  constexpr std::int64_t kFrameBytes = 144;

  CodeGen::Label arityOk = cg.newLabel();
  CodeGen::Label isFunc = cg.newLabel();
  CodeGen::Label resume = cg.newLabel();
  CodeGen::Label buildResult = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, static_cast<std::int32_t>(kFrameBytes));
  cg.movToMem(Rbp, kArgs, Rdi);
  cg.movToMem(Rbp, kArgc, Rsi);

  // `pcall` needs a function as its first argument: anything else is a caller
  // bug, so it fails loudly instead of returning `{ok: false}`, with the hosted
  // builtin's message. No handler is installed yet, so this failure reaches an
  // *outer* `pcall` or the process sink, exactly like the hosted builtin's.
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ge, arityOk);
  emitFailCall(cg, failDynamic, msgNotFunc);
  cg.bind(arityOk);
  cg.movFromMem(Rax, Rdi, 0);   // args[0].tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFunc));
  cg.jcc(Jcc::Eq, isFunc);
  emitFailCall(cg, failDynamic, msgNotFunc);
  cg.bind(isFunc);
  cg.movFromMem(Rax, Rdi, 8);   // args[0].payload (AotClosure*)
  cg.movToMem(Rbp, kCallee, Rax);

  // Install the handler record and publish it in the hidden global. The sink
  // only reads the payload word, so the slot's tag stays nil.
  cg.movFromMem(Rax, kAotGlobalBase, 16LL * handlerSlot + 8);
  cg.movToMem(Rbp, kPrev, Rax);
  cg.movToMem(Rbp, kSavedRsp, Rsp);
  cg.movToMem(Rbp, kSavedRbp, Rbp);
  cg.leaRipLabel(Rax, resume);
  cg.movToMem(Rbp, kResume, Rax);
  cg.movImmToMem(Rbp, kMsgBytes, 0);
  cg.movImmToMem(Rbp, kMsgLength, 0);
  cg.leaMem(Rax, Rbp, kPrev);
  cg.movToMem(kAotGlobalBase, 16LL * handlerSlot + 8, Rax);

  // Call the callee with the remaining arguments. The argument window grows
  // down, so `args[1]` sits 16 bytes below `args[0]`.
  cg.movImm(Rdi, static_cast<std::int64_t>(kTagFunc));
  cg.movFromMem(Rsi, Rbp, kCallee);
  cg.movFromMem(Rdx, Rbp, kArgc);
  cg.subRegImm32(Rdx, 1);
  cg.movFromMem(Rcx, Rbp, kArgs);
  cg.subRegImm32(Rcx, 16);
  cg.callLabel(callLabel);

  // Normal return: restore the previous handler and keep the result.
  cg.movFromMem(Rcx, Rbp, kPrev);
  cg.movToMem(kAotGlobalBase, 16LL * handlerSlot + 8, Rcx);
  cg.movToMem(Rbp, kValueTag, Rax);
  cg.movToMem(Rbp, kValuePayload, Rdx);
  cg.movImmToMem(Rbp, kStatus, 0);
  cg.movImmToMem(Rbp, kErrorTag, static_cast<std::int64_t>(kTagNil));
  cg.movImmToMem(Rbp, kErrorPayload, 0);
  cg.jmp(buildResult);

  // The sink jumped back here with the message in the record: rebuild the
  // pushed callee registers (the abandoned frames restored their own), mark the
  // call as failed and clear the value.
  cg.bind(resume);
  cg.movFromMem(Rbx, Rbp, -8);
  cg.movFromMem(R12, Rbp, -16);
  cg.movImmToMem(Rbp, kStatus, 1);
  cg.movImmToMem(Rbp, kValueTag, static_cast<std::int64_t>(kTagNil));
  cg.movImmToMem(Rbp, kValuePayload, 0);

  // Build the error value: an `AotString` from the record's bytes, then the
  // error header around it. The message is a copy, so it outlives the record.
  cg.movFromMem(R8, Rbp, kMsgLength);
  cg.movReg(Rdi, R8);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);          // message AotString*
  cg.movFromMem(R8, Rbp, kMsgLength);
  cg.movToMem(Rbx, 0, R8);
  cg.movReg(Rdi, Rbx);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rsi, Rbp, kMsgBytes);
  cg.movFromMem(Rcx, Rbp, kMsgLength);
  cg.cld();
  cg.repMovsb();

  cg.movImm(Rdi, static_cast<std::int64_t>(kAotErrorHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, kErrorMessageOffset, Rbx);
  cg.movImmToMem(Rax, kErrorCodeOffset, 0);
  cg.movImmToMem(Rbp, kErrorTag, static_cast<std::int64_t>(kTagError));
  cg.movToMem(Rbp, kErrorPayload, Rax);

  cg.bind(buildResult);
  cg.movImm(Rdi, 1);            // kind 1 = map
  cg.callLabel(tableNew);
  cg.movReg(R12, Rdx);          // r12 = the result map

  // ok = status == 0
  cg.movImm(Rdi, static_cast<std::int64_t>(kTagMap));
  cg.movReg(Rsi, R12);
  cg.movImm(Rdx, static_cast<std::int64_t>(kTagStr));
  cg.leaRipLabel(Rcx, okConst);
  cg.movImm(R8, static_cast<std::int64_t>(kTagBool));
  cg.movFromMem(R9, Rbp, kStatus);
  cg.testReg(R9);
  cg.setcc(Jcc::Eq, R9);
  cg.movzx(R9, R9);
  cg.callLabel(memberSet);

  // value = the return value, or nil after a caught error.
  cg.movImm(Rdi, static_cast<std::int64_t>(kTagMap));
  cg.movReg(Rsi, R12);
  cg.movImm(Rdx, static_cast<std::int64_t>(kTagStr));
  cg.leaRipLabel(Rcx, valueConst);
  cg.movFromMem(R8, Rbp, kValueTag);
  cg.movFromMem(R9, Rbp, kValuePayload);
  cg.callLabel(memberSet);

  // error = nil, or the error value the failure path built.
  cg.movImm(Rdi, static_cast<std::int64_t>(kTagMap));
  cg.movReg(Rsi, R12);
  cg.movImm(Rdx, static_cast<std::int64_t>(kTagStr));
  cg.leaRipLabel(Rcx, errorConst);
  cg.movFromMem(R8, Rbp, kErrorTag);
  cg.movFromMem(R9, Rbp, kErrorPayload);
  cg.callLabel(memberSet);

  cg.movImm(Rax, kTagMap);
  cg.movReg(Rdx, R12);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);

  cg.bind(done);
  cg.addImm32(Rsp, static_cast<std::int32_t>(kFrameBytes));
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// The exact math builtins (PHASE 7c.5b.3): `floor`, `ceil`, `round` and `sqrt`.
// Every one is correctly rounded -- no libm, no approximation -- so it
// reproduces the hosted engine (glibc) bit for bit, negative zero included:
//
//   floor(x) = the largest integer <= x   (round toward -inf)
//   ceil(x)  = the smallest integer >= x  (round toward +inf)
//   round(x) = the nearest integer, halves away from zero (C `round`)
//   sqrt(x)  = the IEEE-754 square root   (`sqrtsd`)
//
// `floor`/`ceil`/`round` are built from `cvttsd2si` (truncate) plus a
// correction, because the SSE4.1 `roundsd` instruction is not part of the
// x86-64 baseline the backend targets. `t = trunc(x)` is exact; `floor`
// subtracts one when `t > x`, `ceil` adds one when `t < x`, and `round` moves
// one away from zero when the exact remainder `|x - t| >= 0.5` (comparing the
// remainder rather than computing `trunc(x + copysign(0.5, x))` avoids the
// double rounding that turns `0.49999999999999994` into 1). Every result is
// converted back to binary64, which is exact for the integers produced, and the
// sign of a zero result is preserved (floor/ceil/round of a negative value in
// (-1, 0] yield -0.0, as C does). A value too large for int64 (`|x| >= 2^63`,
// inf, nan) is already an integer or non-finite, so the `cvttsd2si` result is
// discarded and the operand is returned unchanged -- which is exactly what C
// `floor`, `ceil` and `round` return for those inputs.
//
// An int operand is returned unchanged, matching the hosted builtins'
// `if (args[0].isInt()) return args[0]`; any other type fails with the message
// the caller passes.

// Emits the shared tail that turns `xmm0` (the binary64 result) into a `Double`
// `AotValue` in rax:rdx and returns.
void emitMathReturn(CodeGen& cg) {
  cg.movqFromXmm(Rdx, kXmmBase + 0);
  cg.movImm(Rax, kTagDouble);
}

// The three rounding builtins share one shape: check arity, branch on the tag
// (int passes through, float/double compute, anything else fails), then convert.
// `mode` selects the correction applied after the truncation.
enum class AotRoundMode { Floor, Ceil, Round };

void emitRoundingBuiltin(CodeGen& cg, CodeGen::Label label,
                         const DataBlob& message, const DataBlob& msgArgc,
                         AotRoundMode mode) {
  constexpr int Xmm0 = kXmmBase + 0;
  constexpr int Xmm1 = kXmmBase + 1;
  constexpr int Xmm2 = kXmmBase + 2;
  constexpr int Xmm3 = kXmmBase + 3;
  constexpr int Xmm4 = kXmmBase + 4;
  // The sign mask (only the sign bit set), the abs mask and the constant 0.5,
  // as binary64 bit patterns.
  constexpr std::int64_t kSignMask = static_cast<std::int64_t>(0x8000000000000000ULL);
  constexpr std::int64_t kAbsMask = 0x7FFFFFFFFFFFFFFFLL;
  constexpr std::int64_t kHalf = 0x3FE0000000000000LL;
  constexpr std::int64_t kOne = 0x3FF0000000000000LL;
  // 2^63: `|x| >= 2^63` cannot be represented as int64, and such a value is
  // already an integer, so it is returned unchanged (`cvttsd2si` would return
  // INT64_MIN and the correction would corrupt it).
  constexpr std::int64_t kTwoPow63 = 0x43E0000000000000LL;

  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asNumber = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label compute = cg.newLabel();
  CodeGen::Label keep = cg.newLabel();
  CodeGen::Label roundAdd = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);   // tag
  cg.movFromMem(Rcx, Rdi, 8);   // payload
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, asInt);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, asNumber);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, asNumber);
  emitTypeTailFail(cg, message, Rax, Rcx);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);

  // An int is already an integer: return it unchanged (its own tag).
  cg.bind(asInt);
  cg.movReg(Rdx, Rcx);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);

  cg.bind(asNumber);
  cg.movqToXmm(Xmm0, Rcx);      // x
  cg.movsdReg(Xmm4, Xmm0);      // keep x for the negative-zero sign fix
  // `x - x` is 0 for a finite value and NaN for NaN or either infinity, so an
  // unordered result returns the operand unchanged -- which is what C floor,
  // ceil and round do for NaN and the infinities.
  cg.movsdReg(Xmm1, Xmm0);
  cg.subsd(Xmm1, Xmm0);
  cg.xorps(Xmm2, Xmm2);
  cg.ucomisd(Xmm1, Xmm2);
  cg.jcc(Jcc::P, done);         // NaN / +-inf: return x unchanged

  // `|x| >= 2^63` is already an integer: return it unchanged.
  cg.movqFromXmm(R9, Xmm0);
  cg.movImm(R10, kAbsMask);
  cg.andReg(R9, R10);
  cg.movqToXmm(Xmm2, R9);       // |x|
  cg.movImm(R9, kTwoPow63);
  cg.movqToXmm(Xmm1, R9);
  cg.ucomisd(Xmm2, Xmm1);
  cg.jcc(Jcc::Ae, done);        // |x| >= 2^63

  cg.bind(compute);
  cg.cvttsd2si(Rdx, Xmm0);    // trunc(x): toward zero, exact
  cg.cvtsi2sd(Xmm1, Rdx);     // t = (double) trunc(x), exact
  cg.ucomisd(Xmm1, Xmm0);     // t ?= x
  if (mode == AotRoundMode::Round) {
    // round(x): move t away from zero when |x - t| >= 0.5. Comparing the exact
    // remainder |x - t| against 0.5 avoids the double rounding that
    // `trunc(x + copysign(0.5, x))` suffers for values like
    // 0.49999999999999994 (whose sum rounds up to 0.5).
    cg.movsdReg(Xmm2, Xmm0);
    cg.subsd(Xmm2, Xmm1);     // x - t
    cg.movqFromXmm(R9, Xmm2);
    cg.movImm(R10, kAbsMask);
    cg.andReg(R9, R10);       // |x - t|
    cg.movqToXmm(Xmm2, R9);
    cg.movImm(R9, kHalf);
    cg.movqToXmm(Xmm3, R9);
    cg.ucomisd(Xmm2, Xmm3);   // |x - t| ?= 0.5
    cg.jcc(Jcc::B, keep);     // < 0.5: t is the answer
    // >= 0.5: move one away from zero; the sign of x picks the direction.
    cg.movImm(R9, kOne);
    cg.movqToXmm(Xmm3, R9);
    cg.ucomisd(Xmm1, Xmm0);   // t ?= x
    cg.jcc(Jcc::B, roundAdd); // t < x (x positive): add one
    cg.subsd(Xmm1, Xmm3);     // t > x (x negative): subtract one
    cg.jmp(keep);
    cg.bind(roundAdd);
    cg.addsd(Xmm1, Xmm3);
    cg.jmp(keep);
  } else if (mode == AotRoundMode::Floor) {
    // floor subtracts one when trunc(x) > x; otherwise trunc(x) is the answer.
    cg.jcc(Jcc::Be, keep);
    cg.movImm(R9, kOne);
    cg.movqToXmm(Xmm3, R9);
    cg.subsd(Xmm1, Xmm3);
  } else {
    // ceil adds one when trunc(x) < x; otherwise trunc(x) is the answer.
    cg.jcc(Jcc::Ae, keep);
    cg.movImm(R9, kOne);
    cg.movqToXmm(Xmm3, R9);
    cg.addsd(Xmm1, Xmm3);
  }
  cg.bind(keep);
  cg.movsdReg(Xmm0, Xmm1);

  // Preserve the sign of a zero result: floor/ceil/round of a negative value in
  // (-1, 0] is -0.0, as C returns. A +0.0 result has all bits clear, so OR in
  // the sign bit of the operand when it is negative.
  cg.movqFromXmm(R9, Xmm0);
  cg.testReg(R9);
  cg.jcc(Jcc::Ne, done);        // nonzero: nothing to fix
  cg.movqFromXmm(R8, Xmm4);     // bits of the original x
  cg.movImm(R10, kSignMask);
  cg.andReg(R8, R10);
  cg.orReg(R9, R8);
  cg.movqToXmm(Xmm0, R9);

  cg.bind(done);
  emitMathReturn(cg);
  epilogue(cg);
}

// Load the `AotValue` argument at `byteOffset` in the window in rdi and convert
// it to binary64 in `xmm`. The window grows down, so `args[0]` is at offset 0
// and `args[1]` at -16. An int is converted, a float/double is reinterpreted,
// anything else fails with `message`. Only rax/rcx and `xmm` are touched, so rdi
// survives and a second argument can be loaded afterwards.
void emitNumberArg(CodeGen& cg, int byteOffset, int xmm,
                   const DataBlob& message) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asNumber = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.movFromMem(Rax, Rdi, byteOffset);      // tag
  cg.movFromMem(Rcx, Rdi, byteOffset + 8);  // payload
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, asInt);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, asNumber);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, asNumber);
  emitTypeTailFail(cg, message, Rax, Rcx);
  cg.bind(asInt);
  cg.cvtsi2sd(xmm, Rcx);
  cg.jmp(done);
  cg.bind(asNumber);
  cg.movqToXmm(xmm, Rcx);
  cg.bind(done);
}

// `oskel_builtin_exp` / `oskel_builtin_log`: one numeric argument converted to
// binary64 and handed to the embedded routine (`mathCore`), the result tagged
// Double. `log` additionally rejects a non-positive operand, matching the hosted
// `if (value <= 0) fail`; a NaN is not "<= 0" and passes through.
void emitTranscendentalBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                               CodeGen::Label mathCore, const DataBlob& message,
                               const DataBlob& msgArgc, bool positiveOnly,
                               const DataBlob& msgNonPositive) {
  constexpr int Xmm0 = kXmmBase + 0;
  constexpr int Xmm1 = kXmmBase + 1;

  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label check = cg.newLabel();
  CodeGen::Label nonPositive = cg.newLabel();
  CodeGen::Label compute = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  emitNumberArg(cg, 0, Xmm0, message);
  cg.jmp(check);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);

  cg.bind(check);
  if (positiveOnly) {
    cg.xorps(Xmm1, Xmm1);
    cg.ucomisd(Xmm0, Xmm1);
    cg.jcc(Jcc::P, compute);         // NaN: not <= 0
    cg.jcc(Jcc::Be, nonPositive);    // value <= 0
  }
  cg.jmp(compute);

  cg.bind(nonPositive);
  emitFailCall(cg, fail, msgNonPositive);

  cg.bind(compute);
  cg.callLabel(mathCore);
  emitMathReturn(cg);
  epilogue(cg);
}

// `oskel_builtin_pow`: two numeric arguments, both converted to binary64 in
// xmm0/xmm1, then the embedded `pow`. A non-number operand fails.
void emitPowBuiltin(CodeGen& cg, CodeGen::Label label,
                    CodeGen::Label mathCore, const DataBlob& message,
                    const DataBlob& msgArgc) {
  constexpr int Xmm0 = kXmmBase + 0;
  constexpr int Xmm1 = kXmmBase + 1;

  CodeGen::Label badArgc = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Ne, badArgc);
  emitNumberArg(cg, 0, Xmm0, message);
  emitNumberArg(cg, -16, Xmm1, message);
  cg.callLabel(mathCore);
  emitMathReturn(cg);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_range_fn(AotValue* args, long argc, AotClosure* self)`:
// the public `range` builtin. One integer bound means `0..end`, two mean
// `start..end`; the result is the same exclusive `Range` value the hidden
// `$range` helper builds. The hosted builtin accepts one or two integer
// arguments, so a wrong count or a non-integer bound fails with the hosted
// message.
void emitRangeFnBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                        CodeGen::Label fail, const DataBlob& msgArgc,
                        const DataBlob& msgStart, const DataBlob& msgEnd,
                        const DataBlob& msgOom) {
  CodeGen::Label oneArg = cg.newLabel();
  CodeGen::Label boundsSet = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badStart = cg.newLabel();
  CodeGen::Label badEnd = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);          // args
  // argc must be 1 or 2.
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, oneArg);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Ne, badArgc);
  // Two arguments: args[0] is start, args[1] is end.
  cg.movFromMem(Rax, Rbx, 0);
  cg.movFromMem(Rdx, Rbx, 8);   // start payload, for the quoted type
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badStart);
  cg.movFromMem(R8, Rbx, 8);    // start
  cg.movFromMem(Rax, Rbx, -16);
  cg.movFromMem(Rdx, Rbx, -8);  // end payload, for the quoted type
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badEnd);
  cg.movFromMem(R9, Rbx, -8);   // end
  cg.jmp(boundsSet);

  cg.bind(oneArg);
  cg.movFromMem(Rax, Rbx, 0);
  cg.movFromMem(Rdx, Rbx, 8);   // end payload, for the quoted type
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badEnd);
  cg.xorReg(R8, R8);            // start = 0
  cg.movFromMem(R9, Rbx, 8);    // end

  cg.bind(boundsSet);
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotRangeHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(R12, Rax);
  cg.movToMem(R12, kRangeStartOffset, R8);
  cg.movToMem(R12, kRangeEndOffset, R9);
  cg.movImmToMem(R12, kRangeInclusiveOffset, 0);
  cg.movReg(Rdx, R12);
  cg.movImm(Rax, kTagRange);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badStart);
  emitTypeTailFail(cg, msgStart, Rax, Rdx);
  cg.bind(badEnd);
  emitTypeTailFail(cg, msgEnd, Rax, Rdx);
  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `AotValue oskel_builtin_assert_eq(AotValue* args, long argc, AotClosure* self)`:
// the public `assertEq`. It compares `args[0]` and `args[1]` with the shared
// `oskel_compare` (the same structural equality `==` uses) and returns nil on a
// match. On a mismatch it builds the hosted message -- the optional `args[2]`
// string (default `assertion failed`) followed by `: expected ` + repr(a) +
// `, got ` + repr(b) -- and fails through `oskel_fail_dynamic`, which prefixes
// `oskel: ` and exits 70.
void emitAssertEqBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label compare,
                         CodeGen::Label reprToString, CodeGen::Label sbNew,
                         CodeGen::Label sbAppend, CodeGen::Label failDynamic,
                         CodeGen::Label fail, const DataBlob& msgArgc,
                         const DataBlob& msgDefault, const DataBlob& textExpected,
                         const DataBlob& textGot, const DataBlob& msgOom) {
  CodeGen::Label noMessage = cg.newLabel();
  CodeGen::Label haveMessage = cg.newLabel();
  CodeGen::Label messageBuilt = cg.newLabel();
  CodeGen::Label matched = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  // Stack slots (rbp-relative): the two repr strings, then the builder.
  constexpr std::int64_t kReprA = -24;
  constexpr std::int64_t kReprB = -32;
  constexpr std::int64_t kSb = -40;

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);          // args
  cg.movReg(R12, Rsi);          // argc

  // argc must be 2 or 3.
  cg.cmpImm32(R12, 2);
  cg.jcc(Jcc::Lt, badArgc);
  cg.cmpImm32(R12, 3);
  cg.jcc(Jcc::Gt, badArgc);

  // The equality test: `oskel_compare(a, b, Eq)` returns a Bool value whose
  // payload (rdx) is 0 or 1.
  cg.movFromMem(Rdi, Rbx, 0);
  cg.movFromMem(Rsi, Rbx, 8);
  cg.movFromMem(Rdx, Rbx, -16);
  cg.movFromMem(Rcx, Rbx, -8);
  cg.movImm(R8, static_cast<std::int64_t>(AotCompareOp::Eq));
  cg.callLabel(compare);
  cg.testReg(Rdx);
  cg.jcc(Jcc::Ne, matched);

  // --- mismatch: build the message ---
  // repr(a) and repr(b) are rendered first, before the builder allocates, so a
  // failure to render reports the same way it would on its own. `reprToString`
  // takes the value as (rdi = tag, rsi = payload) and returns the string as
  // (rax = Str tag, rdx = AotString*).
  cg.movFromMem(Rsi, Rbx, 8);
  cg.movFromMem(Rdi, Rbx, 0);
  cg.callLabel(reprToString);
  cg.movToMem(Rbp, kReprA, Rdx);
  cg.movFromMem(Rsi, Rbx, -8);
  cg.movFromMem(Rdi, Rbx, -16);
  cg.callLabel(reprToString);
  cg.movToMem(Rbp, kReprB, Rdx);

  cg.callLabel(sbNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbp, kSb, Rax);

  // The prefix: args[2] when present (argc == 3), else "assertion failed".
  // `oskel_sb_append` takes (rdi = builder, rsi = ptr, rdx = len).
  cg.cmpImm32(R12, 3);
  cg.jcc(Jcc::Lt, noMessage);
  cg.movFromMem(Rax, Rbx, -32);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, noMessage);
  cg.movFromMem(Rsi, Rbx, -24);       // args[2] payload (AotString*)
  cg.movFromMem(Rdx, Rsi, 0);         // length
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.jmp(haveMessage);
  cg.bind(noMessage);
  cg.leaRipLabel(Rsi, msgDefault.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(msgDefault.text.size()));
  cg.bind(haveMessage);
  cg.movFromMem(Rdi, Rbp, kSb);
  cg.callLabel(sbAppend);
  cg.jmp(messageBuilt);

  // Append `: expected <repr(a)>, got <repr(b)>`.
  cg.bind(messageBuilt);
  cg.movFromMem(Rdi, Rbp, kSb);
  cg.leaRipLabel(Rsi, textExpected.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textExpected.text.size()));
  cg.callLabel(sbAppend);
  cg.movFromMem(Rdi, Rbp, kSb);
  cg.movFromMem(Rsi, Rbp, kReprA);
  cg.movFromMem(Rdx, Rsi, 0);          // repr(a) length
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(sbAppend);
  cg.movFromMem(Rdi, Rbp, kSb);
  cg.leaRipLabel(Rsi, textGot.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textGot.text.size()));
  cg.callLabel(sbAppend);
  cg.movFromMem(Rdi, Rbp, kSb);
  cg.movFromMem(Rsi, Rbp, kReprB);
  cg.movFromMem(Rdx, Rsi, 0);          // repr(b) length
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(sbAppend);

  cg.movFromMem(Rax, Rbp, kSb);
  cg.movFromMem(Rdi, Rax, kSbDataOffset);
  cg.movFromMem(Rsi, Rax, kSbLengthOffset);
  cg.callLabel(failDynamic);
  cg.ud2();

  // --- match: return nil ---
  cg.bind(matched);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `AotValue oskel_builtin_exit(AotValue* args, long argc, AotClosure* self)`: the
// public `exit`. It leaves the process through the entry prelude's `oskel_exit`
// with the integer code (`0` when omitted), so it never returns to the caller.
// The hosted builtin accepts zero or one integer argument.
void emitExitBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label exitLabel,
                     const DataBlob& msgArgc, const DataBlob& msgType) {
  CodeGen::Label haveCode = cg.newLabel();
  CodeGen::Label exitNow = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rsi, 0);
  cg.jcc(Jcc::Eq, haveCode);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(Rdi, Rdi, 8);   // code
  cg.jmp(exitNow);
  cg.bind(haveCode);
  cg.xorReg(Rdi, Rdi);          // no argument: exit 0
  cg.bind(exitNow);
  cg.callLabel(exitLabel);
  cg.ud2();

  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  cg.movFromMem(Rdx, Rdi, 8);
  emitTypeTailFail(cg, msgType, Rax, Rdx);
}

// `time()`: the wall clock, in whole seconds since the Unix epoch (UTC), as an
// `int`. It takes no argument. The value comes from the target's `entry.now`
// handle, which each prelude binds its own way -- a raw syscall on Linux, libc
// `time` in the hosted mode, kernel32 on Windows and the CMOS RTC on bare
// metal -- so this emitter is target-independent and the runtime stays free of
// libc. The hosted engine's `time` builtin reads the same wall clock.
void emitTimeBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label nowLabel,
                     const DataBlob& msgArgc) {
  CodeGen::Label ok = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, ok);
  emitArityFailCall(cg, msgArgc);

  cg.bind(ok);
  cg.callLabel(nowLabel);          // rax = seconds since the epoch
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);
}

// `AotValue oskel_builtin_platform(...)` and `oskel_builtin_arch(...)`: the
// target names, as static `AotString`s emitted with the other data islands.
// They take no argument; a wrong count goes through the shared arity failure.
void emitConstStringBuiltin(CodeGen& cg, CodeGen::Label label,
                            CodeGen::Label text, const DataBlob& msgArgc) {
  CodeGen::Label ok = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, ok);
  emitArityFailCall(cg, msgArgc);

  cg.bind(ok);
  cg.movImm(Rax, kTagStr);
  cg.leaRipLabel(Rdx, text);
  epilogue(cg);
}

// `AotValue oskel_builtin_input(AotValue* args, long argc, AotClosure* self)`:
// the public `input`. It takes an optional prompt, writes it to stdout and then
// reads one line through the `entry.read` handle, returning the line as a
// string (nil when the read reports end of input). The read handle is bound by
// the bare-metal prelude (the keyboard slice); the builtin is only seeded for
// that target, so the other preludes never reference it.
//
// The line is read into a fixed 256-byte stack buffer -- the same size as the
// kernel's keyboard ring buffer -- and copied into a fresh arena string, so the
// result outlives the call. `read` consumes the newline and returns the byte
// count, which may be zero for an empty line.
void emitInputBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label readLabel,
                      CodeGen::Label rawPrint, CodeGen::Label allocLabel,
                      CodeGen::Label fail, const DataBlob& msgArgc,
                      const DataBlob& msgOom) {
  constexpr std::int64_t kBufferBytes = 256;
  constexpr std::int64_t kBufferOffset = -272;  // rbp-relative, below rbx/r12
  CodeGen::Label noPrompt = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, static_cast<std::int32_t>(kBufferBytes + 16));

  // `input` takes at most one argument (the prompt).
  CodeGen::Label arityOk = cg.newLabel();
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Le, arityOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(arityOk);
  // Prompt: render args[0] with the raw printer (no newline), matching the
  // hosted `input`'s `args[0].toString()`.
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, noPrompt);
  cg.movFromMem(Rsi, Rdi, 8);  // payload
  cg.movFromMem(Rdi, Rdi, 0);  // tag
  cg.callLabel(rawPrint);

  cg.bind(noPrompt);
  // read(buf, 256): rdi = &buf, rsi = 256. The keyboard has no end of input, so
  // the count is the line's byte length and may be zero for an empty line --
  // which becomes an empty string, exactly like the hosted `input`.
  cg.leaMem(Rdi, Rbp, kBufferOffset);
  cg.movImm(Rsi, kBufferBytes);
  cg.callLabel(readLabel);

  cg.movReg(R12, Rax);  // byte count
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, R12);  // length word
  cg.movReg(Rbx, Rax);
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.leaMem(Rsi, Rbp, kBufferOffset);
  cg.movReg(Rcx, R12);
  cg.cld();
  cg.repMovsb();
  cg.movReg(Rdx, Rbx);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);

  cg.bind(done);
  cg.addImm32(Rsp, static_cast<std::int32_t>(kBufferBytes + 16));
  cg.pop(R12);
  cg.pop(Rbx);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();
}

// `AotValue oskel_builtin_mem_size(AotValue* args, long argc, AotClosure* self)`:
// `memSize()`, the usable physical RAM in bytes as an `int`. It reads the
// `memSizeLabel` handle the bare-metal prelude binds (the multiboot map) and
// tags the result as an integer. A boot loader that supplied no map leaves the
// handle at 0, so `memSize()` is 0 rather than a wrong number.
void emitMemSizeBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label memSizeLabel,
                        const DataBlob& msgArgc) {
  CodeGen::Label ok = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, ok);
  emitArityFailCall(cg, msgArgc);

  cg.bind(ok);
  cg.callLabel(memSizeLabel);      // rax = usable bytes
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);
}

// --- mt19937_64 in machine code (PHASE 7c.5b.6) -----------------------------
//
// `random()`/`seed()` need a generator a native binary can reproduce bit for
// bit, because `tests/build_binary.cmake` compares the AOT output with the VM's.
// `std::mt19937_64` is fully specified by the standard, so the hosted engine's
// sequence is portable; the *distributions* are not, which is why the hosted
// side no longer uses them (see `src/runtime/builtins.cpp`). This emits the
// generator itself, then the same reduction formulas.
//
// The state is a flat `uint64_t[312]` plus a cursor, allocated from the arena
// and addressed through the hidden `$randomState` global's payload. The image
// has no writable data segment, so the arena is the only place it can live.
//
// Register discipline: r13/r14/r15 are the arena cursor, the arena end and the
// global table base (entry.h), so a helper must never write them. Only rbx and
// r12 are pushed as scratch; r8-r11 and rax/rcx/rdx are caller-saved.

constexpr std::int64_t kMtWords = 312;                   // the standard's N
constexpr std::int64_t kMtMiddleWord = 156;              // M
constexpr std::int64_t kMtStateBytes = kMtWords * 8;
constexpr std::int64_t kMtCursorOffset = kMtStateBytes;  // cursor follows the words
constexpr std::int64_t kMtStateTotal = kMtStateBytes + 8;

// A native binary has no `std::random_device`, and it must not: two runs of the
// same program would then differ, and the AOT fixture compares against the VM's
// output. The VM seeds from `std::random_device`; under `oskel build` the
// default seed is the standard's, and a program that wants a reproducible stream
// calls `seed(n)`. BACKLOG §6 item 25 records the divergence.
constexpr std::uint64_t kAotDefaultSeed = 5489ULL;  // the standard's default seed

// `void* oskel_random_state(long slot)`: return the generator state, allocating
// and seeding it on first use. The slot is the hidden `$randomState` global's
// index, passed in rdi; the state pointer is cached in that slot's payload, so a
// second call returns the same block. Returns the pointer in rax.
void emitRandomState(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                     CodeGen::Label seedLabel, CodeGen::Label fail,
                     const DataBlob& msgOom) {
  CodeGen::Label allocate = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.shlImm8(Rdi, 4);          // slot * 16
  cg.addReg(Rdi, kAotGlobalBase);
  cg.movReg(Rbx, Rdi);         // &slot
  cg.movFromMem(Rax, Rbx, 8);  // cached pointer (nil until seeded)
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, allocate);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(allocate);
  cg.movImm(Rdi, kMtStateTotal);
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbx, 8, Rax);  // cache it in the slot's payload
  cg.movReg(R12, Rax);
  cg.movReg(Rdi, R12);
  cg.movImm(Rsi, static_cast<std::int64_t>(kAotDefaultSeed));
  cg.callLabel(seedLabel);
  cg.movReg(Rax, R12);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `uint64_t oskel_random_next(void* state)`: one tempered draw, twisting first
// when the cursor has run out. Returns the value in rax.
void emitRandomNext(CodeGen& cg, CodeGen::Label label, CodeGen::Label twistLabel) {
  CodeGen::Label loaded = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);  // scratch: keeps rsp 16-byte aligned at the `call` below
  cg.movReg(Rbx, Rdi);
  cg.movFromMem(Rax, Rbx, kMtCursorOffset);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kMtWords));
  cg.jcc(Jcc::Lt, loaded);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(twistLabel);  // resets the cursor to 0
  cg.bind(loaded);
  cg.movFromMem(R8, Rbx, kMtCursorOffset);
  cg.movReg(R9, R8);
  cg.shlImm8(R9, 3);
  cg.addReg(R9, Rbx);
  cg.movFromMem(Rax, R9, 0);  // y = s[cursor]
  cg.addImm32(R8, 1);
  cg.movToMem(Rbx, kMtCursorOffset, R8);
  // Tempering, exactly as the standard states it.
  cg.movReg(R10, Rax);
  cg.shrImm8(R10, 29);
  cg.movImm(R11, static_cast<std::int64_t>(0x5555555555555555ULL));
  cg.andReg(R10, R11);
  cg.xorReg(Rax, R10);
  cg.movReg(R10, Rax);
  cg.shlImm8(R10, 17);
  cg.movImm(R11, static_cast<std::int64_t>(0x71D67FFFEDA60000ULL));
  cg.andReg(R10, R11);
  cg.xorReg(Rax, R10);
  cg.movReg(R10, Rax);
  cg.shlImm8(R10, 37);
  cg.movImm(R11, static_cast<std::int64_t>(0xFFF7EEE000000000ULL));
  cg.andReg(R10, R11);
  cg.xorReg(Rax, R10);
  cg.movReg(R10, Rax);
  cg.shrImm8(R10, 43);
  cg.xorReg(Rax, R10);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `void oskel_random_twist(void* state)`: the standard's twist, in place.
void emitRandomTwist(CodeGen& cg, CodeGen::Label label) {
  constexpr std::int64_t kUpperMask = static_cast<std::int64_t>(0xFFFFFFFF80000000ULL);
  constexpr std::int64_t kLowerMask = 0x7FFFFFFF;
  constexpr std::int64_t kMatrixA = static_cast<std::int64_t>(0xB5026F5AA96619E9ULL);

  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label noMatrix = cg.newLabel();
  CodeGen::Label successorReady = cg.newLabel();
  CodeGen::Label middleReady = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);   // state
  cg.push(R12);   // k
  cg.movReg(Rbx, Rdi);
  cg.xorReg(R12, R12);

  cg.bind(loop);
  // x = (s[k] & UM) | (s[(k+1) % N] & LM)
  cg.movReg(R11, R12);
  cg.shlImm8(R11, 3);
  cg.addReg(R11, Rbx);
  cg.movFromMem(Rax, R11, 0);  // s[k]
  cg.movImm(R8, kUpperMask);
  cg.andReg(Rax, R8);
  cg.movReg(R9, R12);
  cg.addImm32(R9, 1);
  cg.cmpImm32(R9, static_cast<std::int32_t>(kMtWords));
  cg.jcc(Jcc::Lt, successorReady);
  cg.xorReg(R9, R9);
  cg.bind(successorReady);
  cg.movReg(R10, R9);
  cg.shlImm8(R10, 3);
  cg.addReg(R10, Rbx);
  cg.movFromMem(R8, R10, 0);
  cg.movImm(R9, kLowerMask);
  cg.andReg(R8, R9);
  cg.orReg(Rax, R8);  // rax = x
  // xa = x >> 1; if (x & 1) xa ^= MATRIX_A.
  cg.movReg(R8, Rax);
  cg.shrImm8(Rax, 1);
  cg.movImm(R9, 1);
  cg.andReg(R8, R9);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, noMatrix);
  cg.movImm(R9, kMatrixA);
  cg.xorReg(Rax, R9);
  cg.bind(noMatrix);
  // s[k] = s[(k+M) % N] ^ xa. k < N so k+M < N+M; one subtraction suffices.
  cg.movReg(R9, R12);
  cg.addImm32(R9, static_cast<std::int32_t>(kMtMiddleWord));
  cg.cmpImm32(R9, static_cast<std::int32_t>(kMtWords));
  cg.jcc(Jcc::Lt, middleReady);
  cg.subRegImm32(R9, static_cast<std::int32_t>(kMtWords));
  cg.bind(middleReady);
  cg.movReg(R10, R9);
  cg.shlImm8(R10, 3);
  cg.addReg(R10, Rbx);
  cg.movFromMem(R8, R10, 0);
  cg.xorReg(R8, Rax);
  cg.movReg(R11, R12);
  cg.shlImm8(R11, 3);
  cg.addReg(R11, Rbx);
  cg.movToMem(R11, 0, R8);
  cg.addImm32(R12, 1);
  cg.cmpImm32(R12, static_cast<std::int32_t>(kMtWords));
  cg.jcc(Jcc::Lt, loop);
  cg.movImmToMem(Rbx, kMtCursorOffset, 0);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `void oskel_random_seed(void* state, uint64_t seed)`: the standard's seeding,
// shared by the one-time init (fixed default seed) and `seed(n)`.
void emitRandomSeed(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);   // state
  cg.push(R12);   // k
  cg.movReg(Rbx, Rdi);
  cg.movToMem(Rbx, 0, Rsi);  // s[0] = seed
  cg.movImm(R12, 1);
  cg.bind(loop);
  // s[k] = 6364136223846793005 * (s[k-1] ^ (s[k-1] >> 62)) + k
  cg.movReg(R9, R12);
  cg.shlImm8(R9, 3);
  cg.addReg(R9, Rbx);         // &s[k]
  cg.movFromMem(R10, R9, -8); // s[k-1]
  cg.movReg(R11, R10);
  cg.shrImm8(R11, 62);
  cg.xorReg(R10, R11);
  cg.movImm(R11, static_cast<std::int64_t>(6364136223846793005ULL));
  cg.imulReg(R10, R11);
  cg.addReg(R10, R12);
  cg.movToMem(R9, 0, R10);
  cg.addImm32(R12, 1);
  cg.cmpImm32(R12, static_cast<std::int32_t>(kMtWords));
  cg.jcc(Jcc::Lt, loop);
  cg.movImmToMem(Rbx, kMtCursorOffset, kMtWords);  // first draw twists
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `void oskel_builtin_seed(AotValue* args, long argc, AotClosure* self)`:
// reseed from one integer, exactly as the hosted `seed(n)`.
void emitSeedBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label stateLabel,
                     CodeGen::Label seedLabel, const DataBlob& msgArgc,
                     const DataBlob& msgInt) {
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);  // scratch: keeps rsp 16-byte aligned at the `call`s below
  cg.movReg(Rbx, Rdi);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Ne, badArgc);
  cg.movFromMem(Rax, Rbx, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badType);
  cg.callLabel(stateLabel);
  cg.movReg(Rdi, Rax);
  cg.movFromMem(Rsi, Rbx, 8);
  cg.callLabel(seedLabel);
  // `seed` returns nil in the VM. The reseed helper leaves its own value in
  // rax:rdx, so the nil has to be written explicitly -- without this a
  // `print(seed(n))` fails with "cannot print this value" while the VM prints
  // `nil`.
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  // Out of line, so the happy path stays straight.
  cg.bind(badType);
  cg.movFromMem(Rdx, Rbx, 8);
  emitTypeTailFail(cg, msgInt, Rax, Rdx);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
}

// `AotValue oskel_builtin_random(AotValue* args, long argc, AotClosure* self)`.
// Zero arguments: `(next() >> 11) / 2^53`, the hosted formula. One or two: an
// integer via `next() % span`, offset by the lower bound.
//
// rbx and r12 are the only scratch registers that survive the calls below (both
// are pushed in the prologue and restored by every helper), so the argument
// window goes in rbx and the argument count in r12. The state pointer is not
// kept in a register across a call at all: each draw reloads it through
// `stateLabel`, which is cheap and removes a class of clobber bug.
void emitRandomBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label nextLabel,
                       CodeGen::Label stateLabel, CodeGen::Label fail,
                       const DataBlob& msgHigh, const DataBlob& msgBounds,
                       const DataBlob& msgArgc, const DataBlob& msgType) {
  constexpr int Xmm0 = kXmmBase + 0;
  constexpr int Xmm1 = kXmmBase + 1;
  constexpr std::int64_t kTwoPow53 = 0x4340000000000000LL;

  CodeGen::Label zeroArgs = cg.newLabel();
  CodeGen::Label oneArg = cg.newLabel();
  CodeGen::Label twoArgs = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label badHigh = cg.newLabel();
  CodeGen::Label badBounds = cg.newLabel();
  CodeGen::Label badArgc = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);  // args, then a surviving operand
  cg.push(R12);  // argc, then a surviving operand
  cg.movReg(Rbx, Rdi);
  cg.movReg(R12, Rsi);
  cg.cmpImm32(R12, 2);
  cg.jcc(Jcc::A, badArgc);  // more than two
  cg.testReg(R12);
  cg.jcc(Jcc::Eq, zeroArgs);
  cg.cmpImm32(R12, 1);
  cg.jcc(Jcc::Eq, oneArg);
  cg.jmp(twoArgs);

  // --- two arguments: low = args[0], high = args[1] ---
  cg.bind(twoArgs);
  cg.movFromMem(Rax, Rbx, 0);  // args[0].tag
  cg.movFromMem(Rdx, Rbx, 8);  // args[0].payload, for the quoted type
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badType);
  // Argument slots grow *downward* from `args` (slotOffset is -16*(index+1)),
  // so args[1] sits 16 bytes below args[0], not above it.
  cg.movFromMem(Rax, Rbx, -16);  // args[1].tag
  cg.movFromMem(Rdx, Rbx, -8);   // args[1].payload, for the quoted type
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(R8, Rbx, 8);    // low  = args[0].payload
  cg.movFromMem(R11, Rbx, -8);  // high = args[1].payload
  cg.cmpReg(R11, R8);
  cg.jcc(Jcc::Le, badBounds);
  cg.subReg(R11, R8);   // r11 = span
  cg.movReg(Rbx, R8);   // rbx = low; the argument window is dead from here
  cg.movReg(R12, R11);  // r12 = span
  cg.callLabel(stateLabel);
  cg.movReg(Rdi, Rax);
  cg.callLabel(nextLabel);
  cg.movReg(R9, R12);
  cg.divUnsigned(R9);   // rdx = next() % span
  cg.addReg(Rdx, Rbx);  // low + remainder
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  // --- one argument: high = args[0], 0 <= r < high ---
  cg.bind(oneArg);
  cg.movFromMem(Rax, Rbx, 0);  // args[0].tag
  cg.movFromMem(Rdx, Rbx, 8);  // args[0].payload, for the quoted type
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badType);
  cg.movFromMem(R11, Rbx, 8);  // high
  cg.testReg(R11);
  cg.jcc(Jcc::Le, badHigh);    // high <= 0
  cg.movReg(R12, R11);         // r12 = high
  cg.callLabel(stateLabel);
  cg.movReg(Rdi, Rax);
  cg.callLabel(nextLabel);
  cg.movReg(R9, R12);
  cg.divUnsigned(R9);   // rdx = next() % high
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  // --- no argument: a double in [0, 1) ---
  cg.bind(zeroArgs);
  cg.callLabel(stateLabel);
  cg.movReg(Rdi, Rax);
  cg.callLabel(nextLabel);
  cg.shrImm8(Rax, 11);  // the top 53 bits: every value is exact in a double
  cg.cvtsi2sd(Xmm0, Rax);
  cg.movImm(R9, kTwoPow53);
  cg.movqToXmm(Xmm1, R9);
  cg.divsd(Xmm0, Xmm1);
  cg.movqFromXmm(Rdx, Xmm0);  // payload = the double's bit pattern
  cg.movImm(Rax, kTagDouble);

  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badHigh);
  emitFailCall(cg, fail, msgHigh);
  cg.bind(badBounds);
  emitFailCall(cg, fail, msgBounds);
  cg.bind(badArgc);
  emitArityFailCall(cg, msgArgc);
  cg.bind(badType);
  emitTypeTailFail(cg, msgType, Rax, Rdx);
}

// `void oskel_sb_init(AotStringBuilder* sb, char* data, long capacity)`: start a
// builder over a caller-owned buffer (`data`/`capacity`) with `length` zero. A
// caller that renders a `print` value passes a stack buffer, so a small render
// never touches the arena; `oskel_sb_new` passes an arena block. Growth copies
// the bytes to a fresh arena block, so the initial buffer may be on the stack.
void emitSbInit(CodeGen& cg, CodeGen::Label label) {
  cg.bind(label);
  cg.movImmToMem(Rdi, kSbLengthOffset, 0);
  cg.movToMem(Rdi, kSbCapacityOffset, Rdx);
  cg.movToMem(Rdi, kSbDataOffset, Rsi);
  cg.retInsn();
}
// `void oskel_sb_append(AotStringBuilder* sb, const char* ptr, long len)`:
// append `len` bytes from `ptr`, growing the block when the capacity is short.
// Growth doubles the capacity until it fits, allocates a fresh arena block and
// copies the bytes already written. A capacity of zero cannot double into a
// positive value, so the first grow starts from the exact size needed, and the
// doubling stops (falling back to the needed size) before `capacity * 2` can
// overflow. Preserves rbx/r12 (the builder and the source pointer), which are
// callee-saved.
void emitSbAppend(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                  CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label grow = cg.newLabel();
  CodeGen::Label growLoop = cg.newLabel();
  CodeGen::Label growDone = cg.newLabel();
  CodeGen::Label growUseNeeded = cg.newLabel();
  CodeGen::Label noCopy = cg.newLabel();
  CodeGen::Label copy = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);            // sb
  cg.movReg(R12, Rsi);            // ptr
  cg.movToMem(Rbp, -24, Rdx);     // len
  cg.movFromMem(Rax, Rbx, kSbLengthOffset);
  cg.addReg(Rax, Rdx);            // needed = length + len
  cg.movToMem(Rbp, -32, Rax);
  cg.movFromMem(R8, Rbx, kSbCapacityOffset);
  cg.cmpReg(Rax, R8);
  cg.jcc(Jcc::A, grow);
  cg.jmp(copy);

  cg.bind(grow);
  cg.movFromMem(R8, Rbx, kSbCapacityOffset);
  cg.movFromMem(R9, Rbp, -32);
  // A zero capacity never doubles to a positive value, so it would loop
  // forever; start it from the exact size needed instead.
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, growUseNeeded);
  cg.movImm(R11, std::numeric_limits<std::int64_t>::max() / 2);  // doubling limit
  cg.bind(growLoop);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, growDone);
  // Stop doubling before `capacity * 2` overflows int64: a capacity past
  // INT64_MAX/2 falls back to the exact size needed.
  cg.cmpReg(R8, R11);
  cg.jcc(Jcc::A, growUseNeeded);
  cg.addReg(R8, R8);
  cg.jmp(growLoop);
  cg.bind(growUseNeeded);
  cg.movReg(R8, R9);
  cg.bind(growDone);
  cg.movToMem(Rbp, -32, R8);      // newCap
  cg.movReg(Rdi, R8);
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(R10, Rax);            // new block
  cg.movFromMem(Rcx, Rbx, kSbLengthOffset);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, noCopy);
  cg.movReg(Rdi, R10);
  cg.movFromMem(Rsi, Rbx, kSbDataOffset);
  cg.repMovsb();
  cg.bind(noCopy);
  cg.movToMem(Rbx, kSbDataOffset, R10);
  cg.movFromMem(R8, Rbp, -32);
  cg.movToMem(Rbx, kSbCapacityOffset, R8);

  cg.bind(copy);
  cg.movFromMem(Rdi, Rbx, kSbDataOffset);
  cg.movFromMem(Rax, Rbx, kSbLengthOffset);
  cg.addReg(Rdi, Rax);            // dst = data + length
  cg.movReg(Rsi, R12);
  cg.movFromMem(Rcx, Rbp, -24);
  cg.repMovsb();
  cg.movFromMem(Rax, Rbx, kSbLengthOffset);
  cg.movFromMem(R9, Rbp, -24);
  cg.addReg(Rax, R9);
  cg.movToMem(Rbx, kSbLengthOffset, Rax);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `void oskel_sb_append_byte(AotStringBuilder* sb, long byte)`: append one byte.
// A thin wrapper over `oskel_sb_append` through a one-byte stack slot.
void emitSbAppendByte(CodeGen& cg, CodeGen::Label label, CodeGen::Label sbAppend) {
  cg.bind(label);
  prologue(cg);
  cg.subRegImm32(Rsp, 16);
  cg.movByteToMem(Rsp, 0, Rsi);
  cg.movReg(Rsi, Rsp);
  cg.movImm(Rdx, 1);
  cg.callLabel(sbAppend);
  cg.addImm32(Rsp, 16);
  epilogue(cg);
}

// `AotStringBuilder* oskel_sb_new(void)`: allocate a builder header and its
// initial block from the arena, and return the header address. Reports "out of
// memory" on a null allocation.
void emitSbNew(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
               CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.subRegImm32(Rsp, 16);
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotSbHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);
  cg.movImm(Rdi, kSbInitialCapacity);
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbx, kSbDataOffset, Rax);
  cg.movImmToMem(Rbx, kSbLengthOffset, 0);
  cg.movImmToMem(Rbx, kSbCapacityOffset, kSbInitialCapacity);
  cg.movReg(Rax, Rbx);
  cg.addImm32(Rsp, 16);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `void oskel_sb_append_escaped(AotStringBuilder* sb, AotString* s)`: append `s`
// between double quotes with the same escapes as `quoteString`: `\"`, `\\`,
// `\n`, `\t`, `\r`, and `\xHH` for any other control byte. This is the
// builder-sink counterpart of `oskel_print_str_escaped`, so a quoted string in a
// rendered collection reads identically on `print` and `$str`.
// `void sbAppendEscaped(sb, source[, length])`: appends `source` to the builder
// with the repr escaping rule, surrounded by double quotes. `fromString` selects
// the shape of `source`: an `AotString*` (length at [rsi], bytes at rsi+8) when
// true, or raw bytes with the length in rdx when false -- the form a pointer
// handle needs, since its bytes are not preceded by an `AotString` header.
void emitSbAppendEscaped(CodeGen& cg, CodeGen::Label label, CodeGen::Label sbAppend,
                         CodeGen::Label sbAppendByte, const DataBlob& textQuote,
                         const DataBlob& textEscQuote, const DataBlob& textEscBackslash,
                         const DataBlob& textEscNL, const DataBlob& textEscTab,
                         const DataBlob& textEscCR, bool fromString) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label plain = cg.newLabel();
  CodeGen::Label hex = cg.newLabel();
  CodeGen::Label next = cg.newLabel();
  CodeGen::Label loopDone = cg.newLabel();
  CodeGen::Label escQuote = cg.newLabel();
  CodeGen::Label escBack = cg.newLabel();
  CodeGen::Label escNL = cg.newLabel();
  CodeGen::Label escTab = cg.newLabel();
  CodeGen::Label escCR = cg.newLabel();
  CodeGen::Label hexLow = cg.newLabel();
  CodeGen::Label hexHigh = cg.newLabel();
  CodeGen::Label hexLowDone = cg.newLabel();
  CodeGen::Label hexHighDone = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 40);
  cg.movReg(Rbx, Rdi);  // sb
  cg.movReg(R12, Rsi);  // the bytes
  if (fromString) {
    cg.movFromMem(R8, R12, 0);  // the AotString length
    cg.addImm32(R12, static_cast<std::int32_t>(kStringDataOffset));
  } else {
    cg.movReg(R8, Rdx);  // the raw length
  }
  cg.movToMem(Rbp, -56, R8);    // the byte count
  cg.movImmToMem(Rbp, -40, 0);  // index

  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textQuote.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textQuote.text.size()));
  cg.callLabel(sbAppend);

  // The loop index lives in r11, a caller-saved scratch, and is reloaded from
  // its stack slot after every call. It must not live in a callee-saved
  // register that carries global state: r13 is `kAotArenaCursor` and r14 is
  // `kAotArenaEnd` (entry.h), and `sbAppend` allocates (growing the builder) as
  // it goes, so both clobbering r13 and saving/restoring it around the loop
  // would leave the arena cursor stale and hand the same bytes out twice.
  cg.bind(loop);
  cg.movFromMem(R11, Rbp, -40);
  cg.movFromMem(R9, Rbp, -56);  // the byte length
  cg.cmpReg(R11, R9);
  cg.jcc(Jcc::Ae, loopDone);

  cg.movReg(R10, R11);
  cg.addReg(R10, R12);
  cg.movzxFromMem(R10, R10, 0);
  cg.cmpImm32(R10, '"');
  cg.jcc(Jcc::Eq, escQuote);
  cg.cmpImm32(R10, '\\');
  cg.jcc(Jcc::Eq, escBack);
  cg.cmpImm32(R10, '\n');
  cg.jcc(Jcc::Eq, escNL);
  cg.cmpImm32(R10, '\t');
  cg.jcc(Jcc::Eq, escTab);
  cg.cmpImm32(R10, '\r');
  cg.jcc(Jcc::Eq, escCR);
  cg.cmpImm32(R10, 0x20);
  cg.jcc(Jcc::B, hex);
  cg.jmp(plain);

  cg.bind(escQuote);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textEscQuote.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textEscQuote.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(next);
  cg.bind(escBack);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textEscBackslash.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textEscBackslash.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(next);
  cg.bind(escNL);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textEscNL.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textEscNL.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(next);
  cg.bind(escTab);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textEscTab.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textEscTab.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(next);
  cg.bind(escCR);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textEscCR.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textEscCR.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(next);

  cg.bind(hex);
  cg.movImmByteToMem(Rbp, -48, '\\');
  cg.movImmByteToMem(Rbp, -47, 'x');
  cg.movFromMem(R10, Rbp, -40);
  cg.addReg(R10, R12);
  cg.movzxFromMem(R10, R10, 0);
  cg.movReg(R9, R10);
  cg.shrImm8(R9, 4);
  cg.andImm32(R9, 0x0F);
  cg.movReg(R8, R9);
  cg.cmpImm32(R8, 10);
  cg.jcc(Jcc::B, hexHigh);
  cg.addImm32(R8, 'a' - 10);
  cg.jmp(hexHighDone);
  cg.bind(hexHigh);
  cg.addImm32(R8, '0');
  cg.bind(hexHighDone);
  cg.movByteToMem(Rbp, -46, R8);
  cg.andImm32(R10, 0x0F);
  cg.cmpImm32(R10, 10);
  cg.jcc(Jcc::B, hexLow);
  cg.addImm32(R10, 'a' - 10);
  cg.jmp(hexLowDone);
  cg.bind(hexLow);
  cg.addImm32(R10, '0');
  cg.bind(hexLowDone);
  cg.movByteToMem(Rbp, -45, R10);
  cg.movReg(Rdi, Rbx);
  cg.leaMem(Rsi, Rbp, -48);
  cg.movImm(Rdx, 4);
  cg.callLabel(sbAppend);
  cg.jmp(next);

  cg.bind(plain);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R10);
  cg.callLabel(sbAppendByte);

  cg.bind(next);
  cg.movFromMem(R11, Rbp, -40);
  cg.addImm32(R11, 1);
  cg.movToMem(Rbp, -40, R11);
  cg.jmp(loop);

  cg.bind(loopDone);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textQuote.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textQuote.text.size()));
  cg.callLabel(sbAppend);
  cg.addImm32(Rsp, 40);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `void oskel_repr_into(AotValue v, long depth, AotStringBuilder* sb)`: append
// the representation of `v` to `sb`, the builder-sink counterpart of the hosted
// `Value::repr` (`[1, 2]`, `{"k": 1}`, quoted strings, `<fn>`). `depth` caps the
// recursion at 8, where `...` is appended instead, so a self-referential
// structure terminates. Every `print` and every `$str` of a collection goes
// through here, so the two agree byte for byte.
void emitReprInto(CodeGen& cg, CodeGen::Label label, CodeGen::Label sbAppend,
                  CodeGen::Label sbAppendEscaped, CodeGen::Label sbAppendEscapedBytes,
                  CodeGen::Label formatDouble,
                  const DataBlob& textTrue, const DataBlob& textFalse,
                  const DataBlob& textNil, const DataBlob& textFunc,
                  const DataBlob& textComma, const DataBlob& textColon,
                  const DataBlob& textOpenBracket, const DataBlob& textCloseBracket,
                  const DataBlob& textOpenBrace, const DataBlob& textCloseBrace,
                  const DataBlob& textClassOpen, const DataBlob& textClassClose,
                  const DataBlob& textEllipsis, const DataBlob& textError,
                  const DataBlob& textCloseParen, const DataBlob& textPtr) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label boolFalse = cg.newLabel();
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asNum = cg.newLabel();
  CodeGen::Label asFunc = cg.newLabel();
  CodeGen::Label funcPlain = cg.newLabel();
  CodeGen::Label asArray = cg.newLabel();
  CodeGen::Label asMap = cg.newLabel();
  CodeGen::Label asRange = cg.newLabel();
  CodeGen::Label asError = cg.newLabel();
  CodeGen::Label asPtr = cg.newLabel();
  CodeGen::Label ptrOpaque = cg.newLabel();
  CodeGen::Label ptrScan = cg.newLabel();
  CodeGen::Label ptrScanEnd = cg.newLabel();
  CodeGen::Label errorNoCode = cg.newLabel();
  CodeGen::Label errorCodePos = cg.newLabel();
  CodeGen::Label errorCodeConv = cg.newLabel();
  CodeGen::Label errorCodeLoop = cg.newLabel();
  CodeGen::Label errorCodeEmit = cg.newLabel();
  CodeGen::Label rangeIncl = cg.newLabel();
  CodeGen::Label rangeAfterSep = cg.newLabel();
  CodeGen::Label arrayLoop = cg.newLabel();
  CodeGen::Label arrayBody = cg.newLabel();
  CodeGen::Label arrayEnd = cg.newLabel();
  CodeGen::Label mapLoop = cg.newLabel();
  CodeGen::Label mapBody = cg.newLabel();
  CodeGen::Label mapEnd = cg.newLabel();
  CodeGen::Label mapPlainOpen = cg.newLabel();
  CodeGen::Label mapOpenDone = cg.newLabel();
  CodeGen::Label mapPlainClose = cg.newLabel();
  CodeGen::Label mapCloseDone = cg.newLabel();
  CodeGen::Label tooDeep = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label intPos = cg.newLabel();
  CodeGen::Label intConv = cg.newLabel();
  CodeGen::Label intLoop = cg.newLabel();
  CodeGen::Label intEmit = cg.newLabel();
  CodeGen::Label rangeEndPos = cg.newLabel();
  CodeGen::Label rangeEndConv = cg.newLabel();
  CodeGen::Label rangeEndLoop = cg.newLabel();
  CodeGen::Label rangeEndEmit = cg.newLabel();
  CodeGen::Label rangeStartPos = cg.newLabel();
  CodeGen::Label rangeStartConv = cg.newLabel();
  CodeGen::Label rangeStartLoop = cg.newLabel();
  CodeGen::Label rangeStartEmit = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 128);   // digit buffer (rbp-144..rbp-80) + locals
  cg.movReg(Rbx, Rcx);        // sb
  cg.movToMem(Rbp, -24, Rdx); // depth
  cg.movToMem(Rbp, -32, Rdi); // tag
  cg.movToMem(Rbp, -40, Rsi); // payload

  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kReprMaxDepth));
  cg.jcc(Jcc::Gt, tooDeep);

  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, asInt);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Eq, asBool);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, asNil);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, asStr);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, asNum);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, asNum);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFunc));
  cg.jcc(Jcc::Eq, asFunc);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, asArray);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, asMap);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, asRange);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagError));
  cg.jcc(Jcc::Eq, asError);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagPtr));
  cg.jcc(Jcc::Eq, asPtr);
  cg.jmp(done);

  // A pointer handle: a `cstring` renders its text quoted, like a string (that
  // is what the user holds); any other handle renders as the bare `<ptr>`, so
  // an address never leaks into a rendered collection.
  cg.bind(asPtr);
  cg.movFromMem(Rax, Rbp, -40);        // payload
  cg.movFromMem(Rax, Rax, kPtrKindOffset);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kPtrKindCString));
  cg.jcc(Jcc::Ne, ptrOpaque);
  // Build a temporary `AotString` view over the handle's bytes, cut at the
  // first NUL, and hand it to the escaped appender: the escaping rule is the
  // string one, so `repr(cstring("a\nb"))` quotes like `repr("a\nb")`.
  cg.movFromMem(R8, Rbp, -40);         // the handle
  cg.movFromMem(R9, R8, kPtrDataOffset);    // bytes
  cg.movFromMem(R10, R8, kPtrLengthOffset);  // owned length
  cg.xorReg(R11, R11);                 // scanned length
  cg.bind(ptrScan);
  cg.cmpReg(R11, R10);
  cg.jcc(Jcc::Ae, ptrScanEnd);
  cg.movReg(Rax, R9);
  cg.addReg(Rax, R11);
  cg.movzxFromMem(Rax, Rax, 0);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, ptrScanEnd);
  cg.addImm32(R11, 1);
  cg.jmp(ptrScan);
  cg.bind(ptrScanEnd);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R9);
  cg.movReg(Rdx, R11);
  cg.callLabel(sbAppendEscapedBytes);
  cg.jmp(done);
  cg.bind(ptrOpaque);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textPtr.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textPtr.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(asInt);
  cg.movFromMem(Rax, Rbp, -40);
  cg.movReg(R8, Rsp);
  cg.addImm32(R8, 64);        // cursor = end of digit buffer
  emitIntDigitsBackwards(cg, R8, intPos, intConv, intLoop, intEmit);
  cg.movReg(Rdx, Rsp);
  cg.addImm32(Rdx, 64);
  cg.subReg(Rdx, R8);         // len = end - cursor
  cg.movReg(Rsi, R8);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(asBool);
  cg.movFromMem(R8, Rbp, -40);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, boolFalse);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textTrue.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textTrue.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);
  cg.bind(boolFalse);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textFalse.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textFalse.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(asNil);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textNil.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textNil.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(asStr);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, Rbp, -40);
  cg.callLabel(sbAppendEscaped);
  cg.jmp(done);

  cg.bind(asNum);
  cg.subRegImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  cg.movFromMem(Rdi, Rbp, -32);               // tag
  cg.movFromMem(Rsi, Rbp, -40);               // payload
  cg.movReg(Rcx, Rsp);                        // scratch
  cg.movReg(Rdx, Rsp);
  cg.addImm32(Rdx, static_cast<std::int32_t>(kDtoaScratchBytes));  // out
  cg.callLabel(formatDouble);
  cg.movReg(Rdx, Rax);                        // len
  cg.movReg(Rsi, Rsp);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kDtoaScratchBytes));
  cg.movReg(Rdi, Rbx);
  cg.callLabel(sbAppend);
  cg.addImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  cg.jmp(done);

  cg.bind(asFunc);
  // A closure carries the text to render in its `display` field (a static
  // `AotString`, PHASE 7c.2c.1); a null field falls back to the literal `<fn>`.
  cg.movFromMem(Rax, Rbp, -40);        // closure
  cg.movFromMem(Rax, Rax, static_cast<std::int64_t>(kAotClosureDisplayOffset));
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, funcPlain);
  cg.movFromMem(Rdx, Rax, 0);          // AotString length
  cg.addImm32(Rax, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rsi, Rax);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(funcPlain);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textFunc.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textFunc.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(asArray);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textOpenBracket.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textOpenBracket.text.size()));
  cg.callLabel(sbAppend);
  cg.movFromMem(R12, Rbp, -40);        // array header
  cg.movImmToMem(Rbp, -48, 0);         // index
  cg.bind(arrayLoop);
  cg.movFromMem(R8, Rbp, -48);
  cg.movFromMem(R9, R12, kTableLengthOffset);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, arrayEnd);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, arrayBody);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textComma.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textComma.text.size()));
  cg.callLabel(sbAppend);
  cg.bind(arrayBody);
  cg.movFromMem(R8, Rbp, -48);
  cg.shlImm8(R8, 4);
  cg.movFromMem(R9, R12, kTableBlockOffset);
  cg.addReg(R9, R8);
  cg.movFromMem(Rdi, R9, 0);
  cg.movFromMem(Rsi, R9, 8);
  cg.movFromMem(Rdx, Rbp, -24);
  cg.addImm32(Rdx, 1);
  cg.movReg(Rcx, Rbx);
  cg.callLabel(label);
  cg.movFromMem(R8, Rbp, -48);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -48, R8);
  cg.jmp(arrayLoop);
  cg.bind(arrayEnd);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textCloseBracket.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textCloseBracket.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(asMap);
  cg.movFromMem(R12, Rbp, -40);        // map header
  cg.movFromMem(R8, R12, kTableClassNameOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, mapPlainOpen);
  // `Persona { "name": "Ana" }`: the class name replaces the bare `{`.
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R8);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rdx, R8, 0);
  cg.callLabel(sbAppend);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textClassOpen.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textClassOpen.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(mapOpenDone);
  cg.bind(mapPlainOpen);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textOpenBrace.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textOpenBrace.text.size()));
  cg.callLabel(sbAppend);
  cg.bind(mapOpenDone);
  cg.movImmToMem(Rbp, -48, 0);         // index
  cg.bind(mapLoop);
  cg.movFromMem(R8, Rbp, -48);
  cg.movFromMem(R9, R12, kTableLengthOffset);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, mapEnd);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, mapBody);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textComma.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textComma.text.size()));
  cg.callLabel(sbAppend);
  cg.bind(mapBody);
  cg.movFromMem(R8, Rbp, -48);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R9, R12, kTableBlockOffset);
  cg.addReg(R9, R8);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, R9, kMapEntryKeyOffset);
  cg.callLabel(sbAppendEscaped);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textColon.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textColon.text.size()));
  cg.callLabel(sbAppend);
  cg.movFromMem(R8, Rbp, -48);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R9, R12, kTableBlockOffset);
  cg.addReg(R9, R8);
  cg.movFromMem(Rdi, R9, kMapEntryValueOffset);
  cg.movFromMem(Rsi, R9, kMapEntryValueOffset + 8);
  cg.movFromMem(Rdx, Rbp, -24);
  cg.addImm32(Rdx, 1);
  cg.movReg(Rcx, Rbx);
  cg.callLabel(label);
  cg.movFromMem(R8, Rbp, -48);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -48, R8);
  cg.jmp(mapLoop);
  cg.bind(mapEnd);
  cg.movFromMem(R8, R12, kTableClassNameOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, mapPlainClose);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textClassClose.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textClassClose.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(mapCloseDone);
  cg.bind(mapPlainClose);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textCloseBrace.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textCloseBrace.text.size()));
  cg.callLabel(sbAppend);
  cg.bind(mapCloseDone);
  cg.jmp(done);

  cg.bind(asRange);
  cg.movFromMem(R12, Rbp, -40);        // range header
  cg.movReg(R8, Rsp);
  cg.addImm32(R8, 64);                 // cursor = end of digit buffer
  cg.movFromMem(Rax, R12, kRangeEndOffset);
  emitIntDigitsBackwards(cg, R8, rangeEndPos, rangeEndConv, rangeEndLoop, rangeEndEmit);
  cg.movFromMem(R9, R12, kRangeInclusiveOffset);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, rangeIncl);
  cg.subRegImm32(R8, 3);
  cg.movImmByteToMem(R8, 0, static_cast<std::uint8_t>('.'));
  cg.movImmByteToMem(R8, 1, static_cast<std::uint8_t>('.'));
  cg.movImmByteToMem(R8, 2, static_cast<std::uint8_t>('='));
  cg.jmp(rangeAfterSep);
  cg.bind(rangeIncl);
  cg.subRegImm32(R8, 2);
  cg.movImmByteToMem(R8, 0, static_cast<std::uint8_t>('.'));
  cg.movImmByteToMem(R8, 1, static_cast<std::uint8_t>('.'));
  cg.bind(rangeAfterSep);
  cg.movFromMem(Rax, R12, kRangeStartOffset);
  emitIntDigitsBackwards(cg, R8, rangeStartPos, rangeStartConv, rangeStartLoop,
                         rangeStartEmit);
  cg.movReg(Rdx, Rsp);
  cg.addImm32(Rdx, 64);
  cg.subReg(Rdx, R8);
  cg.movReg(Rsi, R8);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(sbAppend);
  cg.jmp(done);

  // An error: `error("message")`, or `error("message", code)` when the code is
  // non-zero, matching the hosted debug form.
  cg.bind(asError);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textError.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textError.text.size()));
  cg.callLabel(sbAppend);
  cg.movFromMem(R12, Rbp, -40);          // error header
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, R12, kErrorMessageOffset);
  cg.callLabel(sbAppendEscaped);
  cg.movFromMem(R9, R12, kErrorCodeOffset);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, errorNoCode);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textComma.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textComma.text.size()));
  cg.callLabel(sbAppend);
  cg.movFromMem(Rax, R12, kErrorCodeOffset);
  cg.movReg(R8, Rsp);
  cg.addImm32(R8, 64);
  emitIntDigitsBackwards(cg, R8, errorCodePos, errorCodeConv, errorCodeLoop,
                         errorCodeEmit);
  cg.movReg(Rdx, Rsp);
  cg.addImm32(Rdx, 64);
  cg.subReg(Rdx, R8);
  cg.movReg(Rsi, R8);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(sbAppend);
  cg.bind(errorNoCode);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textCloseParen.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textCloseParen.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(tooDeep);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textEllipsis.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textEllipsis.text.size()));
  cg.callLabel(sbAppend);

  cg.bind(done);
  cg.addImm32(Rsp, 128);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `AotValue oskel_repr_to_string(AotValue v)`: render `v` with the recursive repr
// (`oskel_repr_into`) into a fresh arena `AotString` and return it as a `Str`
// value. The `$str` helper uses it for a collection, function or range operand,
// so `"${[1, 2]}"` reads exactly like `print([1, 2])`.
void emitReprToString(CodeGen& cg, CodeGen::Label label, CodeGen::Label sbNew,
                      CodeGen::Label reprInto, CodeGen::Label allocLabel,
                      CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(R12, Rdi);          // tag
  cg.movToMem(Rbp, -24, Rsi);   // payload
  cg.callLabel(sbNew);          // rax = builder
  cg.movReg(Rbx, Rax);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -24);
  cg.xorReg(Rdx, Rdx);
  cg.movReg(Rcx, Rbx);
  cg.callLabel(reprInto);
  cg.movFromMem(R12, Rbx, kSbLengthOffset);   // length
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, R12);
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rsi, Rbx, kSbDataOffset);
  cg.movReg(Rcx, R12);
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);

  cg.bind(done);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `void oskel_print_str_escaped(AotString* s, long fd)`: writes `s` between
// double quotes with the same escapes as `quoteString`: `\"`, `\\`, `\n`,
// `\t`, `\r`, and `\xHH` for any other control byte.
void emitPrintStrEsc(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                     const DataBlob& textQuote, const DataBlob& textEscQuote,
                     const DataBlob& textEscBackslash, const DataBlob& textEscNL,
                     const DataBlob& textEscTab, const DataBlob& textEscCR,
                     const DataBlob& textHexPrefix) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label plain = cg.newLabel();
  CodeGen::Label hex = cg.newLabel();
  CodeGen::Label next = cg.newLabel();
  CodeGen::Label loopDone = cg.newLabel();
  CodeGen::Label escQuote = cg.newLabel();
  CodeGen::Label escBack = cg.newLabel();
  CodeGen::Label escNL = cg.newLabel();
  CodeGen::Label escTab = cg.newLabel();
  CodeGen::Label escCR = cg.newLabel();
  CodeGen::Label hexLow = cg.newLabel();
  CodeGen::Label hexHigh = cg.newLabel();
  CodeGen::Label hexLowDone = cg.newLabel();
  CodeGen::Label hexHighDone = cg.newLabel();
  (void)textHexPrefix;

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);  // string
  cg.movReg(R12, Rsi);  // fd
  cg.movImmToMem(Rbp, -24, 0);  // index

  cg.leaRipLabel(Rdi, textQuote.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textQuote.text.size()));
  inlineWriteFd(cg, writeLabel, R12);

  cg.bind(loop);
  cg.movFromMem(R8, Rbp, -24);
  cg.movFromMem(R9, Rbx, 0);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, loopDone);

  cg.movReg(R10, R8);
  cg.addReg(R10, Rbx);
  cg.movzxFromMem(R10, R10, kStringDataOffset);  // byte
  cg.cmpImm32(R10, '"');
  cg.jcc(Jcc::Eq, escQuote);
  cg.cmpImm32(R10, '\\');
  cg.jcc(Jcc::Eq, escBack);
  cg.cmpImm32(R10, '\n');
  cg.jcc(Jcc::Eq, escNL);
  cg.cmpImm32(R10, '\t');
  cg.jcc(Jcc::Eq, escTab);
  cg.cmpImm32(R10, '\r');
  cg.jcc(Jcc::Eq, escCR);
  cg.cmpImm32(R10, 0x20);
  cg.jcc(Jcc::B, hex);
  cg.jmp(plain);

  cg.bind(escQuote);
  cg.leaRipLabel(Rdi, textEscQuote.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textEscQuote.text.size()));
  inlineWriteFd(cg, writeLabel, R12);
  cg.jmp(next);
  cg.bind(escBack);
  cg.leaRipLabel(Rdi, textEscBackslash.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textEscBackslash.text.size()));
  inlineWriteFd(cg, writeLabel, R12);
  cg.jmp(next);
  cg.bind(escNL);
  cg.leaRipLabel(Rdi, textEscNL.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textEscNL.text.size()));
  inlineWriteFd(cg, writeLabel, R12);
  cg.jmp(next);
  cg.bind(escTab);
  cg.leaRipLabel(Rdi, textEscTab.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textEscTab.text.size()));
  inlineWriteFd(cg, writeLabel, R12);
  cg.jmp(next);
  cg.bind(escCR);
  cg.leaRipLabel(Rdi, textEscCR.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textEscCR.text.size()));
  inlineWriteFd(cg, writeLabel, R12);
  cg.jmp(next);

  cg.bind(hex);
  // Build "\xHH" in a 4-byte stack buffer and write it in one syscall.
  cg.movImmByteToMem(Rbp, -40, '\\');
  cg.movImmByteToMem(Rbp, -39, 'x');
  cg.movFromMem(R10, Rbp, -24);
  cg.addReg(R10, Rbx);
  cg.movzxFromMem(R10, R10, kStringDataOffset);
  cg.movReg(R9, R10);
  cg.shrImm8(R9, 4);
  cg.andImm32(R9, 0x0F);
  cg.movReg(R8, R9);
  cg.cmpImm32(R8, 10);
  cg.jcc(Jcc::B, hexHigh);
  cg.addImm32(R8, 'a' - 10);
  cg.jmp(hexHighDone);
  cg.bind(hexHigh);
  cg.addImm32(R8, '0');
  cg.bind(hexHighDone);
  cg.movByteToMem(Rbp, -38, R8);
  cg.andImm32(R10, 0x0F);
  cg.cmpImm32(R10, 10);
  cg.jcc(Jcc::B, hexLow);
  cg.addImm32(R10, 'a' - 10);
  cg.jmp(hexLowDone);
  cg.bind(hexLow);
  cg.addImm32(R10, '0');
  cg.bind(hexLowDone);
  cg.movByteToMem(Rbp, -37, R10);
  cg.leaMem(Rdi, Rbp, -40);
  cg.movImm(Rsi, 4);
  inlineWriteFd(cg, writeLabel, R12);
  cg.jmp(next);

  cg.bind(plain);
  cg.movFromMem(R8, Rbp, -24);
  cg.movReg(R9, R8);
  cg.addReg(R9, Rbx);
  cg.addImm32(R9, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rdi, R9);
  cg.movImm(Rsi, 1);
  inlineWriteFd(cg, writeLabel, R12);

  cg.bind(next);
  cg.movFromMem(R8, Rbp, -24);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -24, R8);
  cg.jmp(loop);

  cg.bind(loopDone);
  cg.leaRipLabel(Rdi, textQuote.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textQuote.text.size()));
  inlineWriteFd(cg, writeLabel, R12);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `void oskel_print_repr(AotValue v, long depth, long fd)`: the recursive
// container renderer, matching `Value::repr` (`[1, 2]`, `{"k": 1}`, quoted
// strings, `<fn>`). It renders into a stack string builder and writes the bytes
// in one syscall; `oskel_repr_into` does the traversal, so `print` and `$str`
// share one renderer.
void emitPrintRepr(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                   CodeGen::Label sbInit, CodeGen::Label reprInto) {
  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);                  // saved at [rbp-8]
  cg.push(R12);                  // saved at [rbp-16]
  cg.subRegImm32(Rsp, 640);      // builder header + 512-byte initial buffer
  // The builder header sits below the saved registers: [rbp-40] length,
  // [rbp-32] capacity, [rbp-24] data. The buffer runs [rbp-552, rbp-40).
  cg.movToMem(Rbp, -560, Rdi);   // value tag
  cg.movToMem(Rbp, -568, Rsi);   // value payload
  cg.movToMem(Rbp, -576, Rdx);   // depth
  cg.movReg(R12, Rcx);           // fd
  cg.leaMem(Rdi, Rbp, -40);      // builder header
  cg.leaMem(Rsi, Rbp, -552);     // initial buffer
  cg.movImm(Rdx, 512);
  cg.callLabel(sbInit);
  cg.leaMem(Rcx, Rbp, -40);      // sb = header
  cg.movFromMem(Rdi, Rbp, -560);
  cg.movFromMem(Rsi, Rbp, -568);
  cg.movFromMem(Rdx, Rbp, -576);
  cg.callLabel(reprInto);
  cg.movFromMem(Rsi, Rbp, -40);  // length
  cg.movFromMem(Rdi, Rbp, -24);  // data
  cg.movReg(R10, R12);
  inlineWriteFd(cg, writeLabel, R10);
  cg.addImm32(Rsp, 640);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// ------------------------------------------------------------------- extern thunks

// The integer argument registers of the two C ABIs the hosted backend can
// target. System V uses six GPRs and eight SSE registers; the Microsoft x64
// ABI uses four of each and reserves 32 bytes of shadow space, which the
// lowerer accounts for by starting `stackOffset` at 32. The thunk is emitted
// once per signature, so the ABI is a parameter, not a global mode.
constexpr X64Reg kSysVIntArgRegs[] = {Rdi, Rsi, Rdx, Rcx, R8, R9};
constexpr X64Reg kWin64IntArgRegs[] = {Rcx, Rdx, R8, R9};

const X64Reg* intArgRegsFor(Abi abi) {
  return abi == Abi::Win64 ? kWin64IntArgRegs : kSysVIntArgRegs;
}

int intArgCountFor(Abi abi) { return abi == Abi::Win64 ? 4 : 6; }
int sseArgCountFor(Abi abi) { return abi == Abi::Win64 ? 4 : 8; }
// A scratch xmm for the register-class conversions (a boxed number narrowed to
// an integer, an integer widened to a float). It sits outside xmm0-xmm7, the
// eight SSE argument registers, so an SSE argument is never clobbered by a
// later integer argument's conversion; the REX prefix is the price.
constexpr int XmmScratch = kXmmBase + 15;

// The hosted engine caps a returned `cstring` scan at 16 MiB (`src/ffi.cpp`):
// past the cap the C `const char *` contract is broken and the read stops with
// a clear error instead of walking off the end of the buffer. The thunk matches
// that bound so a hosted binary and the VM agree.
constexpr std::int32_t kMaxCStringBytes = 1 << 24;

// Normalizes the integer returned in `rax` to its declared width, exactly like
// the hosted `wrapInteger`: a C `int` leaves the upper half of `rax` undefined,
// so a `shl`/`sar` pair re-extends a narrow signed value and a mask zero-extends
// an unsigned one.
void normalizeIntegerReturn(CodeGen& cg, const std::string& type) {
  if (type == "int8") {
    cg.shlImm8(Rax, 56);
    cg.sarImm8(Rax, 56);
  } else if (type == "int16") {
    cg.shlImm8(Rax, 48);
    cg.sarImm8(Rax, 48);
  } else if (type == "uint8") {
    cg.andImm32(Rax, 0xff);
  } else if (type == "uint16") {
    cg.andImm32(Rax, 0xffff);
  } else if (type == "uint" || type == "uint32") {
    cg.movReg32(Rax, Rax);  // zero-extends the low 32 bits
  } else if (type == "int64" || type == "uint64" || type == "size_t") {
    // Full width: `rax` already holds the whole value.
  } else {
    // "", "any", "int", "int32": a C `int` is 32 bits.
    cg.shlImm8(Rax, 32);
    cg.sarImm8(Rax, 32);
  }
}

// Builds the C bytes of one by-value struct argument in a stack buffer at
// `[rbp + bufDisp]`, reading each field out of the `AotMap` the argument holds.
// The buffer is then reloaded as whole eightbytes, which is why it exists: the
// fields are narrower than an eightbyte and overlap, so staging each at its own
// width and reloading is simpler -- and correct for a mixed INTEGER/SSE struct
// -- than assembling every eightbyte from its fields in registers.
//
// `argSlot` is the argument's frame offset (`[rbx + argSlot]`) and `bufDisp` the
// displacement of the destination, relative to `rbp` for a register-class
// struct's staging buffer or to `rsp` for a class-MEMORY struct's place in the
// outgoing argument area. Each field is read with `oskel_member_get`, exactly
// like `obj.field`, so the read matches the VM's. A missing field yields nil,
// which the width stores turn into zero bytes, the same zero-fill the hosted
// `writeStructBytes` starts from.
void emitStructArgBytes(CodeGen& cg, const AotStructLayout& arg, std::int64_t argSlot,
                        X64Reg bufBase, std::int64_t bufDisp, CodeGen::Label memberGet,
                        CodeGen::Label oom, CodeGen::Label allocLabel, CodeGen::Label fail,
                        const DataBlob& msgType) {
  // Zero the buffer first: the padding a C compiler leaves between fields is
  // part of the value the callee may copy. A class-MEMORY struct is larger than
  // the 16-byte register-class buffers, so the span comes from the layout.
  const std::int64_t span = (static_cast<std::int64_t>(arg.layout.size) + 7) / 8 * 8;
  for (std::int64_t w = 0; w < span; w += 8) cg.movImmToMem(bufBase, bufDisp + w, 0);

  const ffi::StructLayout& layout = arg.layout;
  for (std::size_t k = 0; k < layout.fields.size(); ++k) {
    const std::int64_t fieldOffset = static_cast<std::int64_t>(layout.offsets[k]);
    // rdi:rsi = the struct value (the map), rdx:rcx = the field name.
    cg.movFromMem(Rdi, Rbx, argSlot);
    cg.movFromMem(Rsi, Rbx, argSlot + 8);
    cg.movImm(Rdx, static_cast<std::int64_t>(AotTag::Str));
    cg.leaRipLabel(Rcx, arg.fieldNameLabels[k]);
    cg.callLabel(memberGet);
    // A missing field is a nil, and the hosted engine rejects it by name; a
    // zero-filled field would be a silently wrong call, so fail here too. A
    // pointer or `cstring` field may legitimately be nil (C's NULL), which its
    // own branch below handles.
    if (layout.classes[k] != ffi::ParamClass::Pointer &&
        !ffi::isFfiCStringTypeName(layout.fields[k].type)) {
      CodeGen::Label present = cg.newLabel();
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagNil));
      cg.jcc(Jcc::Ne, present);
      emitFailCall(cg, fail, msgType);
      cg.bind(present);
    }
    // The field's boxed value is in rax (tag) / rdx (payload). Store its bytes
    // at the field's offset and width. A `float` field is boxed as binary64, so
    // it is narrowed back to binary32 first; every other class is a bit pattern
    // or an integer whose low bytes are the C representation.
    const std::size_t size = layout.sizes[k];
    if (ffi::isFfiCStringTypeName(layout.fields[k].type)) {
      // An `AotString` is a length then the bytes, with no NUL, so its address
      // is not a `const char *`. Materialise a terminated copy in the arena,
      // exactly like a scalar `cstring` argument; `nil` stays NULL.
      CodeGen::Label isNil = cg.newLabel();
      CodeGen::Label isStr = cg.newLabel();
      CodeGen::Label isPtr = cg.newLabel();
      CodeGen::Label staged = cg.newLabel();
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagNil));
      cg.jcc(Jcc::Eq, isNil);
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
      cg.jcc(Jcc::Eq, isStr);
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagPtr));
      cg.jcc(Jcc::Eq, isPtr);
      emitFailCall(cg, fail, msgType);
      cg.bind(isNil);
      cg.xorReg(Rdx, Rdx);
      cg.jmp(staged);
      cg.bind(isPtr);
      // A pointer handle already holds NUL-terminated bytes -- the shape a
      // returned `cstring` boxes as -- so its address is the `const char *`
      // with no copy. Without this a struct returned with a `cstring` field
      // could not be passed back to a C function.
      cg.movFromMem(Rdx, Rdx, kPtrDataOffset);
      cg.jmp(staged);
      cg.bind(isStr);
      cg.movReg(R12, Rdx);            // the AotString*
      cg.movFromMem(Rdi, R12, 0);     // the byte length
      cg.addImm32(Rdi, 1);            // plus the terminating NUL
      cg.callLabel(allocLabel);
      cg.testReg(Rax);
      cg.jcc(Jcc::Eq, oom);
      cg.movReg(R11, Rax);            // the block, kept across `rep movsb`
      cg.movReg(Rdi, Rax);
      cg.movReg(Rsi, R12);
      cg.addImm32(Rsi, static_cast<std::int32_t>(kAotStringHeaderSize));
      cg.movFromMem(Rcx, R12, 0);
      cg.repMovsb();
      cg.movImmByteToMem(Rdi, 0, 0);  // the terminating NUL
      cg.movReg(Rdx, R11);
      cg.bind(staged);
    }
    if (layout.classes[k] == ffi::ParamClass::Float) {
      cg.movqToXmm(XmmScratch, Rdx);
      cg.cvtsd2ss(XmmScratch, XmmScratch);
      cg.movqFromXmm(Rdx, XmmScratch);
    }
    if (size == 1) {
      cg.movByteToMem(bufBase, bufDisp + fieldOffset, Rdx);
    } else if (size == 2) {
      // No 16-bit store exists (the backend's narrow forms are 8 and 32 bits),
      // so the two bytes go out one at a time; a single 32-bit store could spill
      // into the next field.
      cg.movByteToMem(bufBase, bufDisp + fieldOffset, Rdx);
      cg.shrImm8(Rdx, 8);
      cg.movByteToMem(bufBase, bufDisp + fieldOffset + 1, Rdx);
    } else if (size == 4) {
      cg.movToMem32(bufBase, bufDisp + fieldOffset, Rdx);
    } else {
      cg.movToMem(bufBase, bufDisp + fieldOffset, Rdx);
    }
  }
}

// Boxes a C string address held in `addrReg` as a `CString` pointer handle, or
// nil for NULL. This is what the hosted engine returns for a `const char *`
// return or field (PHASE 7f.6): a `cstring`, not a string, so `type()` reports
// "cstring", equality is handle identity and `+` rejects it. Boxing as `Str`
// instead would make `type()` lie and silently accept string operations the VM
// rejects (BACKLOG §6 item 23).
//
// The bytes are scanned to the NUL with the hosted engine's 16 MiB cap and
// copied into an arena block that also holds the `AotPtr` header and the
// terminator, exactly as `cstring(text)` builds one. Leaves the boxed value in
// rax:rdx. `oom` is the thunk's out-of-memory label.
void emitCStringBox(CodeGen& cg, X64Reg addrReg, CodeGen::Label oom,
                    CodeGen::Label allocLabel, CodeGen::Label fail,
                    const DataBlob& msgCStringTooLong) {
  CodeGen::Label null = cg.newLabel();
  CodeGen::Label scanned = cg.newLabel();
  CodeGen::Label scanLoop = cg.newLabel();
  CodeGen::Label returned = cg.newLabel();
  cg.testReg(addrReg);
  cg.jcc(Jcc::Eq, null);
  // `rbx` is dead once the C call has returned (the caller reloads the map from
  // `r12`), so it holds the C string across the `alloc`; `r12` must survive
  // because a struct return keeps the map there.
  cg.movReg(Rbx, addrReg);
  cg.xorReg(Rcx, Rcx);      // the byte count, the NUL excluded
  cg.bind(scanLoop);
  // `movzxFromMem` takes a displacement, so index through a temporary base.
  cg.movReg(R11, Rbx);
  cg.addReg(R11, Rcx);
  cg.movzxFromMem(R10, R11, 0);
  cg.testReg(R10);
  cg.jcc(Jcc::Eq, scanned);
  cg.addImm32(Rcx, 1);
  cg.cmpImm32(Rcx, static_cast<std::int32_t>(kMaxCStringBytes));
  cg.jcc(Jcc::B, scanLoop);
  emitFailCall(cg, fail, msgCStringTooLong);
  cg.bind(scanned);
  // The block holds the header, the bytes and the terminator; `length` counts
  // the bytes plus that NUL, matching `cstring(text)` and the hosted `PtrObj`.
  // The length lives in `rsi` -- not `r12`, which a struct return keeps the
  // map in -- and `alloc` preserves it (it only clobbers rax/r10/r11).
  cg.movReg(Rsi, Rcx);
  cg.addImm32(Rsi, 1);  // the terminator
  cg.movReg(Rdi, Rsi);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotPtrHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movImmToMem(Rax, kPtrKindOffset, kPtrKindCString);
  cg.movToMem(Rax, kPtrLengthOffset, Rsi);
  cg.movImmToMem(Rax, kPtrReleasedOffset, 0);
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotPtrHeaderSize));
  cg.movToMem(Rax, kPtrDataOffset, Rdi);  // data points past the header
  // `rbx` still holds the C address; `rcx` is the byte count the scan left.
  cg.movReg(Rsi, Rbx);
  cg.cld();
  cg.repMovsb();
  cg.movImmByteToMem(Rdi, 0, 0);  // the terminator, at data + length
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagPtr);
  cg.jmp(returned);
  cg.bind(null);
  cg.movImm(Rax, kTagNil);
  cg.xorReg(Rdx, Rdx);
  cg.bind(returned);
}

// `AotValue emitVoidPtrBox(std::int64_t address)`: boxes a `ptr` returned by a C
// function. `ptr` is opaque -- the AOT runtime never dereferences it -- so the
// machine address is the whole value: it goes into an `AotPtr` of kind `Void`,
// exactly what `intToPtr` builds, and a later `ptrToInt` reads it back. NULL is
// `nil`, matching `wrapPointerValue` in the hosted engine, which returns a nil
// `Value` for a zero address before it looks at the type name.
void emitVoidPtrBox(CodeGen& cg, CodeGen::Label label, CodeGen::Label ptrNew,
                    CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label null = cg.newLabel();
  CodeGen::Label allocated = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);  // the address
  cg.testReg(Rbx);
  cg.jcc(Jcc::Eq, null);
  cg.movImm(Rdi, kPtrKindVoid);
  cg.xorReg(Rsi, Rsi);  // no owned bytes: a Void handle carries the token only
  cg.callLabel(ptrNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, allocated);
  emitFailCall(cg, fail, msgOom);

  cg.bind(allocated);
  cg.movToMem(Rax, kPtrDataOffset, Rbx);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagPtr);
  cg.jmp(done);

  cg.bind(null);
  cg.movImm(Rax, kTagNil);
  cg.xorReg(Rdx, Rdx);

  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// Rebuilds the `AotMap` of a struct returned by value. The eightbytes come back
// in the registers their class assigns: an INTEGER eightbyte in `rax` then
// `rdx`, an SSE one in `xmm0` then `xmm1`. They are stashed in a stack buffer
// first, then each field is read at its offset, boxed exactly as the hosted
// `readStructField` does, and written into a fresh map.
void emitStructReturn(CodeGen& cg, const AotStructLayout& ret, std::int64_t bufDisp,
                      CodeGen::Label tableNew, CodeGen::Label memberSet,
                      CodeGen::Label oom, CodeGen::Label allocLabel, CodeGen::Label fail,
                      const DataBlob& msgCStringTooLong, bool win64Inline = false,
                      bool fromMemory = false) {
  const ffi::StructLayout& layout = ret.layout;
  if (win64Inline) {
    // A Win64 1/2/4/8-byte aggregate comes back in `rax`; its low bytes are the
    // struct. Park it in the buffer the field loop below reads.
    cg.movToMem(Rbp, bufDisp, Rax);
  } else if (fromMemory) {
    // A class-MEMORY or Win64 by-reference return was written by the callee into
    // the `sret` buffer; the return registers carry only its address.
  } else {
    int intReg = 0;
    int sseReg = 0;
    for (int e = 0; e < layout.eightbyteCount; ++e) {
      const std::int64_t off = bufDisp + 8LL * e;
      if (layout.eightbyte[e] == 'S') {
        cg.movqFromXmm(R11, kXmmBase + sseReg);
        cg.movToMem(Rbp, off, R11);
        ++sseReg;
      } else {
        cg.movToMem(Rbp, off, intReg == 0 ? Rax : Rdx);
        ++intReg;
      }
    }
  }

  cg.movImm(Rdi, 1);  // a map, not an array
  cg.callLabel(tableNew);
  cg.movReg(R12, Rdx);  // the AotMap*, kept across the memberSet calls

  for (std::size_t k = 0; k < layout.fields.size(); ++k) {
    const std::string& type = layout.fields[k].type;
    const std::int64_t fieldOffset = bufDisp + static_cast<std::int64_t>(layout.offsets[k]);
    const std::size_t size = layout.sizes[k];
    const ffi::ParamClass cls = layout.classes[k];

    if (cls == ffi::ParamClass::Float) {
      cg.movFromMem32(Rax, Rbp, fieldOffset);
      cg.movqToXmm(XmmScratch, Rax);
      cg.cvtss2sd(XmmScratch, XmmScratch);  // binary32 -> binary64
      cg.movqFromXmm(Rdx, XmmScratch);
      cg.movImm(Rax, kTagFloat);
    } else if (cls == ffi::ParamClass::Double) {
      cg.movFromMem(Rdx, Rbp, fieldOffset);
      cg.movImm(Rax, kTagDouble);
    } else if (ffi::isFfiCStringTypeName(type)) {
      // A `cstring` field is a `const char *`: the hosted engine copies it into
      // a Oskel string (NULL becomes nil), so the AOT backend must too. A raw
      // address here would be a silently wrong value, not a representation.
      cg.movFromMem(Rax, Rbp, fieldOffset);
      emitCStringBox(cg, Rax, oom, allocLabel, fail, msgCStringTooLong);
    } else if (cls == ffi::ParamClass::Pointer) {
      // A pointer field has no AOT representation of its own, so a NULL (the
      // only value a program can construct, since the backend lowers no `ptr`
      // return) boxes as nil, exactly like the hosted engine's NULL. A non-null
      // address is surfaced as the integer it is, rather than invented into a
      // handle the runtime could not dereference.
      CodeGen::Label isNull = cg.newLabel();
      CodeGen::Label boxed = cg.newLabel();
      cg.movFromMem(Rdx, Rbp, fieldOffset);
      cg.testReg(Rdx);
      cg.jcc(Jcc::Eq, isNull);
      cg.movImm(Rax, kTagInt);
      cg.jmp(boxed);
      cg.bind(isNull);
      cg.xorReg(Rdx, Rdx);
      cg.movImm(Rax, kTagNil);
      cg.bind(boxed);
    } else {
      // An integer family field (or a pointer, which the AOT runtime has no
      // handle for and reports as its address). Load exactly the field's bytes,
      // then normalize to the declared width like a scalar return.
      if (size == 1) {
        cg.movzxFromMem(Rax, Rbp, fieldOffset);
      } else if (size == 2) {
        cg.movFromMem32(Rax, Rbp, fieldOffset);
        cg.andImm32(Rax, 0xffff);
      } else if (size == 4) {
        cg.movFromMem32(Rax, Rbp, fieldOffset);
      } else {
        cg.movFromMem(Rax, Rbp, fieldOffset);
      }
      if (type == "bool") {
        cg.andImm32(Rax, 0xff);
        cg.testReg(Rax);
        cg.setcc(Jcc::Ne, Rdx);
        cg.movzx(Rdx, Rdx);
        cg.movImm(Rax, kTagBool);
      } else {
        normalizeIntegerReturn(cg, type);
        cg.movReg(Rdx, Rax);
        cg.movImm(Rax, kTagInt);
      }
    }

    // oskel_member_set(map, name, value): rdi:rsi the map, rdx:rcx the name,
    // r8:r9 the boxed field. The value is in rax:rdx, so it moves to r8:r9
    // before the name overwrites rdx:rcx.
    cg.movReg(R8, Rax);
    cg.movReg(R9, Rdx);
    cg.movImm(Rdi, kTagMap);
    cg.movReg(Rsi, R12);
    cg.movImm(Rdx, static_cast<std::int64_t>(AotTag::Str));
    cg.leaRipLabel(Rcx, ret.fieldNameLabels[k]);
    cg.callLabel(memberSet);
  }

  cg.movImm(Rax, kTagMap);
  cg.movReg(Rdx, R12);
}

// Emits the marshalling thunk for one `extern "C"` symbol (PHASE 7f.5, 7f.7).
// It has the lowering's own function shape `AotValue thunk(AotValue* args, long
// argc, AotClosure* self)`, so it drops into the `oskel_call` dispatch exactly
// like a user function or a seeded builtin: the chunk seeds it as a `Func` in
// the symbol's global slot.
//
// The body walks the caller's argument window -- `args[i]` is at
// `[rbx - 16*i]`, tag then payload, because the register file grows down --
// converts each boxed value to the register its `ParamClass` assigns (a
// general-purpose register for the integer/pointer class, an SSE register for
// the floating class), calls the real C symbol through an external relocation,
// then boxes the result. It reproduces the hosted `ffi_marshal.h` conversions:
// a bool is 0/1, an integer is the payload, a number is truncated toward zero
// for an integer parameter and widened for a `double`; the return is normalized
// to the declared width.
void emitExternThunk(CodeGen& cg, const AotExternThunk& thunk, CodeGen::Label fail,
                     CodeGen::Label allocLabel, CodeGen::Label memberGet,
                     CodeGen::Label tableNew, CodeGen::Label memberSet,
                     CodeGen::Label voidPtrBox, const DataBlob& msgArity,
                     const DataBlob& msgType, const DataBlob& msgOom,
                     const DataBlob& msgCStringTooLong,
                     const std::vector<AotCallbackSignature>* callbacks,
                     CodeGen::Label callbackCreate, int callbackStateSlot, Abi abi) {
  CodeGen::Label arityOk = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  const X64Reg* intArgRegs = intArgRegsFor(abi);
  const int intArgCount = intArgCountFor(abi);
  const int sseArgCount = sseArgCountFor(abi);
  const bool win64 = abi == Abi::Win64;

  cg.bind(thunk.label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);  // a callee-saved scratch: the `alloc`/`rep movsb` calls clobber rdi/rsi/rcx/r10/r11
  // The integer arguments that fit the six GPRs are staged here and loaded into
  // their registers only just before the call, because marshalling a later
  // pointer argument calls `alloc` and `rep movsb`, which clobber the argument
  // registers. Six 8-byte slots keep the frame 16-byte aligned (48 is a
  // multiple of 16). A spilled GPR is written straight to the outgoing area.
  constexpr std::int32_t kArgStageBytes = 6 * 8;
  // One buffer per by-value struct. System V uses a 16-byte register-class
  // buffer; Win64 uses 16 bytes for an inline 1/2/4/8-byte value and the
  // padded layout size for a by-reference copy. A struct that does not fit a
  // System V register pair is passed wholly on the stack and needs no buffer.
  std::int32_t kStructBufBytes = 0;
  for (const AotStructLayout& layout : thunk.structs) {
    if (layout.bufOffset < 0) continue;
    const std::int64_t span = std::max<std::int64_t>(
        16, (static_cast<std::int64_t>(layout.layout.size) + 15) / 16 * 16);
    kStructBufBytes = std::max<std::int32_t>(
        kStructBufBytes, static_cast<std::int32_t>(layout.bufOffset + span));
  }
  // The frame below `rbp` is laid out top to bottom as: the staging slots, the
  // register-class struct buffers, the `sret` buffer, and finally the outgoing
  // stack-argument area. Only the last is addressed from `rsp` (it must be the
  // lowest part, where the callee reads its stack arguments); everything else is
  // `rbp`-relative, so the two never overlap.
  constexpr std::int64_t kStageBase = -static_cast<std::int64_t>(16 + kArgStageBytes);
  const std::int64_t kStructBufBase = kStageBase - kStructBufBytes;
  const std::int64_t sretBytes =
      thunk.returnMemoryBytes > 0 ? (thunk.returnMemoryBytes + 15) / 16 * 16 : 0;
  const std::int64_t sretBase = kStructBufBase - sretBytes;
  // A Win64 variadic thunk reserves the outgoing area itself: the 32-byte
  // shadow space, the fixed stack arguments (positions four and later) and the
  // variable tail. The fixed stack arguments are already in `thunk.params`.
  int fixedStackCount = 0;
  for (const AotThunkParam& param : thunk.params) {
    if (param.stackOffset >= 0) ++fixedStackCount;
  }
  const std::int64_t varOutBase = win64 ? 32 + 8LL * fixedStackCount : 0;
  // A C variadic carries its walk state in eight 8-byte slots, because the only
  // operation that clobbers the argument registers (`alloc`, while a tail string
  // is materialised) happens inside the walk. A fixed signature reserves none,
  // so its frame -- and the freestanding image -- is unchanged.
  constexpr std::int64_t kVarSlotBytes = 64;
  const std::int64_t varBase = sretBase - (thunk.variadic ? kVarSlotBytes : 0);
  const std::int64_t kVarRemaining = varBase + 0;    // tail arguments left
  const std::int64_t kVarP = varBase + 8;            // current tail arg pointer
  const std::int64_t kVarStageCur = varBase + 16;    // staging cursor
  const std::int64_t kVarStageBase = varBase + 24;   // staging base
  const std::int64_t kVarStackSlots = varBase + 32;  // tail args that spill
  const std::int64_t kVarTailCount = varBase + 40;   // argc - arity
  const std::int64_t kVarGpr = varBase + 48;         // next free GPR index
  const std::int64_t kVarSse = varBase + 56;         // next free SSE index
  const std::int64_t stackArgsBytes = (thunk.stackArgBytes + 15) / 16 * 16;
  // A multiple of 16 in every term, so `rsp` stays 16-byte aligned at the call.
  const std::int64_t frameBytes = kArgStageBytes + kStructBufBytes + sretBytes +
                                  (thunk.variadic ? kVarSlotBytes : 0) + stackArgsBytes;
  cg.subRegImm32(Rsp, static_cast<std::int32_t>(frameBytes));
  cg.movReg(Rbx, Rdi);  // args base; argument i lives at [rbx - 16*i]

  // Undoes the frame and returns. A variadic thunk also reserved a
  // run-time-sized outgoing area below the frame, so it restores `rsp` from
  // `rbp` (a multiple of 16 in every term keeps the call aligned) instead of
  // adding a constant.
  auto restoreFrame = [&]() {
    if (thunk.variadic) {
      cg.movReg(Rsp, Rbp);
      cg.subRegImm32(Rsp, static_cast<std::int32_t>(16 + frameBytes));
    } else {
      cg.addImm32(Rsp, static_cast<std::int32_t>(frameBytes));
    }
    cg.pop(R12);
    cg.pop(Rbx);
    epilogue(cg);
  };

  // The thunk is the callee, so it checks its own arity, exactly like a user
  // function's prologue. A direct call is already arity-checked at compile time,
  // so this only fires through a first-class reference (`var f = abs; f(1, 2)`).
  // A C variadic accepts the fixed prefix or more.
  cg.cmpImm32(Rsi, static_cast<std::int32_t>(thunk.arity));
  cg.jcc(thunk.variadic ? Jcc::Ge : Jcc::Eq, arityOk);
  emitFailCall(cg, fail, msgArity);
  cg.bind(arityOk);

  // --- C variadic: reserve the tail and materialise its strings ---------------
  //
  // The fixed prefix is marshalled below by the ordinary path (a variadic
  // declaration is validated as scalars-only, so there are no structs and no
  // `sret`). The tail is walked here, before any argument register is loaded,
  // for two reasons: the number of tail arguments is a run-time value, so the
  // outgoing stack area must be reserved dynamically, and a tail string calls
  // `alloc`, which clobbers the argument registers.
  //
  // Pass 1 counts the arguments that do not fit the remaining GPR/SSE registers;
  // they are the ones that spill to the outgoing stack area, whose size therefore
  // depends on the call. The area also holds one staging slot per tail argument,
  // so pass A can park a materialised string and pass B can read it back.
  if (thunk.variadic) {
    const std::int32_t fixedArity = static_cast<std::int32_t>(thunk.arity);
    // The GPR/SSE allocation continues after the fixed prefix. Win64 assigns
    // by argument position instead: the tail starts at position `arity`, and
    // only the vector registers actually used are counted for `%al`.
    int fixedGpr = 0;
    int fixedSse = 0;
    if (win64) {
      fixedGpr = fixedArity;
      for (const AotThunkParam& param : thunk.params) {
        if ((param.cls == ffi::ParamClass::Double ||
             param.cls == ffi::ParamClass::Float) &&
            (param.win64Position >= 0 ? param.win64Position : param.argIndex) < 4) {
          ++fixedSse;
        }
      }
    } else {
      for (const AotThunkParam& param : thunk.params) {
        if (param.cls == ffi::ParamClass::Double || param.cls == ffi::ParamClass::Float) {
          ++fixedSse;
        } else {
          ++fixedGpr;
        }
      }
    }

    // tailCount = argc - arity; the arity check above guaranteed it is >= 0.
    cg.movReg(R11, Rsi);
    cg.subRegImm32(R11, fixedArity);
    cg.movToMem(Rbp, kVarTailCount, R11);
    cg.movToMem(Rbp, kVarRemaining, R11);
    // P walks the tail downward: `args[i]` is at `[rbx - 16*i]`.
    cg.movReg(R12, Rbx);
    if (fixedArity > 0) cg.subRegImm32(R12, 16 * fixedArity);
    cg.movToMem(Rbp, kVarP, R12);
    cg.movImm(R11, fixedGpr);
    cg.movToMem(Rbp, kVarGpr, R11);
    cg.movImm(R11, fixedSse);
    cg.movToMem(Rbp, kVarSse, R11);
    cg.movImm(R11, 0);
    cg.movToMem(Rbp, kVarStackSlots, R11);

    CodeGen::Label countLoop = cg.newLabel();
    CodeGen::Label countDone = cg.newLabel();
    CodeGen::Label countSse = cg.newLabel();
    CodeGen::Label countSseFit = cg.newLabel();
    CodeGen::Label countGpr = cg.newLabel();
    CodeGen::Label countGprFit = cg.newLabel();
    CodeGen::Label countSpill = cg.newLabel();
    CodeGen::Label countNext = cg.newLabel();
    cg.bind(countLoop);
    cg.movFromMem(R11, Rbp, kVarRemaining);
    cg.testReg(R11);
    cg.jcc(Jcc::Eq, countDone);
    cg.movFromMem(R10, Rbp, kVarP);
    cg.movFromMem(R11, R10, 0);  // the value's tag
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagFloat));
    cg.jcc(Jcc::Eq, countSse);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagDouble));
    cg.jcc(Jcc::Eq, countSse);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagInt));
    cg.jcc(Jcc::Eq, countGpr);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagBool));
    cg.jcc(Jcc::Eq, countGpr);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagStr));
    cg.jcc(Jcc::Eq, countGpr);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagPtr));
    cg.jcc(Jcc::Eq, countGpr);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagNil));
    cg.jcc(Jcc::Eq, countGpr);
    emitFailCall(cg, fail, msgType);
    cg.bind(countSse);
    if (win64) {
      // Position four and later always spill; a register-position float only
      // consumes one vector register.
      cg.movFromMem(R11, Rbp, kVarGpr);
      cg.cmpImm32(R11, 4);
      cg.jcc(Jcc::Lt, countSseFit);
      cg.jmp(countSpill);
      cg.bind(countSseFit);
      cg.movFromMem(R11, Rbp, kVarSse);
      cg.addImm32(R11, 1);
      cg.movToMem(Rbp, kVarSse, R11);
      cg.jmp(countNext);
    } else {
      cg.movFromMem(R11, Rbp, kVarSse);
      cg.cmpImm32(R11, sseArgCount);
      cg.jcc(Jcc::Lt, countSseFit);
      cg.jmp(countSpill);
      cg.bind(countSseFit);
      cg.addImm32(R11, 1);
      cg.movToMem(Rbp, kVarSse, R11);
      cg.jmp(countNext);
    }
    cg.bind(countGpr);
    if (win64) {
      cg.movFromMem(R11, Rbp, kVarGpr);
      cg.cmpImm32(R11, 4);
      cg.jcc(Jcc::Lt, countNext);
      cg.jmp(countSpill);
    } else {
      cg.movFromMem(R11, Rbp, kVarGpr);
      cg.cmpImm32(R11, intArgCount);
      cg.jcc(Jcc::Lt, countGprFit);
      cg.jmp(countSpill);
      cg.bind(countGprFit);
      cg.addImm32(R11, 1);
      cg.movToMem(Rbp, kVarGpr, R11);
      cg.jmp(countNext);
    }
    cg.bind(countSpill);
    cg.movFromMem(R11, Rbp, kVarStackSlots);
    cg.addImm32(R11, 1);
    cg.movToMem(Rbp, kVarStackSlots, R11);
    cg.bind(countNext);
    cg.movFromMem(R10, Rbp, kVarP);
    cg.subRegImm32(R10, 16);
    cg.movToMem(Rbp, kVarP, R10);
    cg.movFromMem(R11, Rbp, kVarRemaining);
    cg.subRegImm32(R11, 1);
    cg.movToMem(Rbp, kVarRemaining, R11);
    if (win64) {
      // The position advances for every tail argument, including a spill.
      cg.movFromMem(R11, Rbp, kVarGpr);
      cg.addImm32(R11, 1);
      cg.movToMem(Rbp, kVarGpr, R11);
    }
    cg.jmp(countLoop);
    cg.bind(countDone);

    // dynamicBytes = align16(varOutBase + (stackSlots + tailCount) * 8), so
    // `rsp` stays 16-byte aligned at the call. On Win64 `varOutBase` is the
    // shadow space plus the fixed stack arguments, which the callee reads from
    // the bottom of this area.
    cg.movFromMem(R11, Rbp, kVarStackSlots);
    cg.shlImm8(R11, 3);
    cg.movFromMem(R10, Rbp, kVarTailCount);
    cg.shlImm8(R10, 3);
    cg.addReg(R11, R10);
    if (varOutBase > 0) cg.addImm32(R11, static_cast<std::int32_t>(varOutBase));
    cg.addImm32(R11, 15);
    cg.andImm32(R11, -16);
    cg.subReg(Rsp, R11);
    // The staging array sits above the outgoing stack area.
    cg.movFromMem(R10, Rbp, kVarStackSlots);
    cg.shlImm8(R10, 3);
    cg.movReg(R12, Rsp);
    cg.addReg(R12, R10);
    if (varOutBase > 0) cg.addImm32(R12, static_cast<std::int32_t>(varOutBase));
    cg.movToMem(Rbp, kVarStageBase, R12);

    // Pass A: every tail string becomes a NUL-terminated arena copy, parked in
    // the staging slot for its position. `r12` holds the `AotString*` across the
    // `alloc` call because it is callee-saved; the walk state lives in the frame.
    cg.movFromMem(R11, Rbp, kVarTailCount);
    cg.movToMem(Rbp, kVarRemaining, R11);
    cg.movReg(R12, Rbx);
    if (fixedArity > 0) cg.subRegImm32(R12, 16 * fixedArity);
    cg.movToMem(Rbp, kVarP, R12);
    cg.movFromMem(R11, Rbp, kVarStageBase);
    cg.movToMem(Rbp, kVarStageCur, R11);

    CodeGen::Label strLoop = cg.newLabel();
    CodeGen::Label strDone = cg.newLabel();
    CodeGen::Label strNext = cg.newLabel();
    cg.bind(strLoop);
    cg.movFromMem(R11, Rbp, kVarRemaining);
    cg.testReg(R11);
    cg.jcc(Jcc::Eq, strDone);
    cg.movFromMem(R10, Rbp, kVarP);
    cg.movFromMem(R11, R10, 0);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagStr));
    cg.jcc(Jcc::Ne, strNext);
    cg.movFromMem(R12, R10, 8);  // the AotString*
    cg.movFromMem(Rdi, R12, 0);  // the byte length
    cg.addImm32(Rdi, 1);         // plus the terminating NUL
    cg.callLabel(allocLabel);    // rax = the block (or null)
    cg.testReg(Rax);
    cg.jcc(Jcc::Eq, oom);
    cg.movReg(R11, Rax);
    cg.movReg(Rdi, Rax);
    cg.movReg(Rsi, R12);
    cg.addImm32(Rsi, static_cast<std::int32_t>(kAotStringHeaderSize));
    cg.movFromMem(Rcx, R12, 0);
    cg.repMovsb();
    cg.movImmByteToMem(Rdi, 0, 0);  // the terminating NUL
    cg.movFromMem(R10, Rbp, kVarStageCur);
    cg.movToMem(R10, 0, R11);
    cg.bind(strNext);
    cg.movFromMem(R11, Rbp, kVarStageCur);
    cg.addImm32(R11, 8);
    cg.movToMem(Rbp, kVarStageCur, R11);
    cg.movFromMem(R10, Rbp, kVarP);
    cg.subRegImm32(R10, 16);
    cg.movToMem(Rbp, kVarP, R10);
    cg.movFromMem(R11, Rbp, kVarRemaining);
    cg.subRegImm32(R11, 1);
    cg.movToMem(Rbp, kVarRemaining, R11);
    cg.jmp(strLoop);
    cg.bind(strDone);
  }

  // Build every struct argument's bytes first: each field read calls
  // `oskel_member_get`, which clobbers the argument registers and the xmm
  // registers, so nothing may be live in them yet. The buffers are dead once
  // the loop below has loaded the eightbytes, which is why the return path
  // reuses them.
  for (const AotThunkParam& param : thunk.params) {
    const bool firstEightbyte = param.structIndex >= 0 && param.eightbyte == 0;
    const bool win64Struct = param.win64StructBytes > 0 || param.structByRef;
    if (firstEightbyte || win64Struct) {
      const AotStructLayout& arg = thunk.structs[static_cast<std::size_t>(param.structIndex)];
      emitStructArgBytes(cg, arg, -16LL * param.argIndex, Rbp,
                         kStructBufBase + arg.bufOffset, memberGet, oom, allocLabel,
                         fail, msgType);
    }
  }
  // A class-MEMORY argument is written straight into the outgoing stack area,
  // which is `rsp`-relative and already reserved by the frame.
  for (const AotStackArg& stacked : thunk.stackArgs) {
    emitStructArgBytes(cg, thunk.structs[static_cast<std::size_t>(stacked.structIndex)],
                       -16LL * stacked.argIndex, Rsp, stacked.offset, memberGet, oom, allocLabel,
                       fail, msgType);
  }

  // A class-MEMORY return consumes `rdi` for the hidden buffer pointer, so the
  // integer arguments start one register later. The pointer is staged with the
  // other integer arguments and loaded just before the call, like them.
  int gpr = 0;
  bool gprUsed[6] = {};
  auto stageGpr = [&](const AotThunkParam& param, X64Reg value) {
    if (win64) {
      const int position = param.win64Position >= 0 ? param.win64Position : param.argIndex;
      cg.movToMem(Rbp, kStageBase + 8LL * position, value);
      gprUsed[position] = true;
    } else {
      cg.movToMem(Rbp, kStageBase + 8LL * gpr, value);
      gprUsed[gpr] = true;
      ++gpr;
    }
  };
  if (thunk.returnMemoryBytes > 0) {
    cg.leaMem(R11, Rbp, sretBase);
    cg.movToMem(Rbp, kStageBase + 8LL * gpr, R11);
    gprUsed[gpr] = true;
    ++gpr;
  }
  int sse = 0;
  for (std::size_t i = 0; i < thunk.params.size(); ++i) {
    const AotThunkParam& param = thunk.params[i];
    if (param.win64StructBytes > 0) {
      // Win64 inline aggregate: its 1/2/4/8 bytes are one integer argument.
      const AotStructLayout& arg =
          thunk.structs[static_cast<std::size_t>(param.structIndex)];
      const std::int64_t off = kStructBufBase + arg.bufOffset;
      if (param.win64StructBytes == 1) {
        cg.movzxFromMem(R11, Rbp, off);
      } else if (param.win64StructBytes == 2) {
        cg.movFromMem32(R11, Rbp, off);
        cg.andImm32(R11, 0xffff);
      } else if (param.win64StructBytes == 4) {
        cg.movFromMem32(R11, Rbp, off);
      } else {
        cg.movFromMem(R11, Rbp, off);
      }
      if (param.stackOffset >= 0) {
        cg.movToMem(Rsp, param.stackOffset, R11);
      } else {
        stageGpr(param, R11);
      }
      continue;
    }
    if (param.structByRef) {
      // Win64 by-reference aggregate: pass the address of the caller-owned copy.
      const AotStructLayout& arg =
          thunk.structs[static_cast<std::size_t>(param.structIndex)];
      cg.leaMem(R11, Rbp, kStructBufBase + arg.bufOffset);
      if (param.stackOffset >= 0) {
        cg.movToMem(Rsp, param.stackOffset, R11);
      } else {
        stageGpr(param, R11);
      }
      continue;
    }
    if (param.structIndex >= 0) {
      // One eightbyte of a System V struct: load it whole out of the buffer the
      // pre-pass filled. An INTEGER eightbyte stages like a scalar GPR argument,
      // an SSE one goes straight to its xmm register.
      const AotStructLayout& arg =
          thunk.structs[static_cast<std::size_t>(param.structIndex)];
      const std::int64_t off =
          kStructBufBase + arg.bufOffset + 8LL * param.eightbyte;
      cg.movFromMem(R11, Rbp, off);
      if (param.cls == ffi::ParamClass::Double) {
        const int xmmIndex =
            win64 ? (param.win64Position >= 0 ? param.win64Position : param.argIndex) : sse;
        cg.movqToXmm(kXmmBase + xmmIndex, R11);
        if (!win64) ++sse;
      } else {
        stageGpr(param, R11);
      }
      continue;
    }
    const std::int64_t base = -16LL * param.argIndex;
    cg.movFromMem(R10, Rbx, base);  // the value's tag
    switch (param.cls) {
      case ffi::ParamClass::Int: {
        // A bool (payload 0/1), an int, or a number (truncated toward zero).
        CodeGen::Label asIs = cg.newLabel();
        CodeGen::Label asNumber = cg.newLabel();
        CodeGen::Label done = cg.newLabel();
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagBool));
        cg.jcc(Jcc::Eq, asIs);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagInt));
        cg.jcc(Jcc::Eq, asIs);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagFloat));
        cg.jcc(Jcc::Eq, asNumber);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagDouble));
        cg.jcc(Jcc::Eq, asNumber);
        emitFailCall(cg, fail, msgType);
        cg.bind(asNumber);
        cg.movFromMem(R11, Rbx, base + 8);
        cg.movqToXmm(XmmScratch, R11);
        cg.cvttsd2si(R11, XmmScratch);
        cg.jmp(done);
        cg.bind(asIs);
        cg.movFromMem(R11, Rbx, base + 8);
        cg.bind(done);
        if (param.stackOffset >= 0) {
          cg.movToMem(Rsp, param.stackOffset, R11);
        } else {
          stageGpr(param, R11);
        }
        break;
      }
      case ffi::ParamClass::Double: {
        // A number; an int is widened to binary64. A Float/Double payload is
        // already the binary64 bit pattern, so it moves verbatim. A Win64
        // argument past the fourth SSE register travels on the stack as an
        // eight-byte slot.
        CodeGen::Label asIs = cg.newLabel();
        CodeGen::Label asInt = cg.newLabel();
        CodeGen::Label done = cg.newLabel();
        const bool onStack = param.stackOffset >= 0;
        const int sseIndex = win64 ? (param.win64Position >= 0 ? param.win64Position : param.argIndex) : sse;
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagFloat));
        cg.jcc(Jcc::Eq, asIs);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagDouble));
        cg.jcc(Jcc::Eq, asIs);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagInt));
        cg.jcc(Jcc::Eq, asInt);
        emitFailCall(cg, fail, msgType);
        cg.bind(asInt);
        cg.movFromMem(R11, Rbx, base + 8);
        if (onStack) {
          cg.cvtsi2sd(XmmScratch, R11);
          cg.movqFromXmm(R11, XmmScratch);
          cg.movToMem(Rsp, param.stackOffset, R11);
        } else {
          cg.cvtsi2sd(kXmmBase + sseIndex, R11);
          if (win64 && thunk.variadic) {
            cg.movqFromXmm(R11, kXmmBase + sseIndex);
            stageGpr(param, R11);
          }
        }
        cg.jmp(done);
        cg.bind(asIs);
        cg.movFromMem(R11, Rbx, base + 8);
        if (onStack) {
          cg.movToMem(Rsp, param.stackOffset, R11);
        } else {
          cg.movqToXmm(kXmmBase + sseIndex, R11);
          if (win64 && thunk.variadic) stageGpr(param, R11);
        }
        cg.bind(done);
        if (!onStack && !win64) ++sse;
        break;
      }
      case ffi::ParamClass::Float: {
        // A number narrowed to binary32. An SSE register holds the low 32 bits
        // of the xmm; a Win64 stack slot holds the four-byte value.
        CodeGen::Label asIs = cg.newLabel();
        CodeGen::Label asInt = cg.newLabel();
        CodeGen::Label done = cg.newLabel();
        const bool onStack = param.stackOffset >= 0;
        const int sseIndex = win64 ? (param.win64Position >= 0 ? param.win64Position : param.argIndex) : sse;
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagFloat));
        cg.jcc(Jcc::Eq, asIs);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagDouble));
        cg.jcc(Jcc::Eq, asIs);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagInt));
        cg.jcc(Jcc::Eq, asInt);
        emitFailCall(cg, fail, msgType);
        cg.bind(asInt);
        cg.movFromMem(R11, Rbx, base + 8);
        if (onStack) {
          cg.cvtsi2sd(XmmScratch, R11);
          cg.cvtsd2ss(XmmScratch, XmmScratch);
          cg.movqFromXmm(R11, XmmScratch);
          cg.movToMem32(Rsp, param.stackOffset, R11);
        } else {
          cg.cvtsi2sd(XmmScratch, R11);
          cg.cvtsd2ss(kXmmBase + sseIndex, XmmScratch);
          if (win64 && thunk.variadic) {
            cg.movqFromXmm(R11, kXmmBase + sseIndex);
            stageGpr(param, R11);
          }
        }
        cg.jmp(done);
        cg.bind(asIs);
        cg.movFromMem(R11, Rbx, base + 8);
        if (onStack) {
          cg.movqToXmm(XmmScratch, R11);
          cg.cvtsd2ss(XmmScratch, XmmScratch);
          cg.movqFromXmm(R11, XmmScratch);
          cg.movToMem32(Rsp, param.stackOffset, R11);
        } else {
          cg.movqToXmm(kXmmBase + sseIndex, R11);
          cg.cvtsd2ss(kXmmBase + sseIndex, kXmmBase + sseIndex);
          if (win64 && thunk.variadic) {
            cg.movqFromXmm(R11, kXmmBase + sseIndex);
            stageGpr(param, R11);
          }
        }
        cg.bind(done);
        if (!onStack && !win64) ++sse;
        break;
      }
      case ffi::ParamClass::Pointer: {
        if (param.callbackSignature >= 0) {
          // The argument is an Oskel closure. A C function pointer carries no
          // context, so a runtime stub binds the closure and the thunk passes
          // the stub's address.
          CodeGen::Label notFunc = cg.newLabel();
          CodeGen::Label staged = cg.newLabel();
          cg.movFromMem(R10, Rbx, base);  // the value's tag
          cg.cmpImm32(R10, static_cast<std::int32_t>(kTagFunc));
          cg.jcc(Jcc::Ne, notFunc);
          if (callbacks == nullptr ||
              param.callbackSignature >= static_cast<int>(callbacks->size())) {
            emitFailCall(cg, fail, msgType);
          } else {
            cg.leaRipLabel(
                Rdi, (*callbacks)[static_cast<std::size_t>(param.callbackSignature)].body);
            cg.movReg(Rsi, R10);                // the closure tag
            cg.movFromMem(Rdx, Rbx, base + 8);  // the closure payload
            cg.callLabel(callbackCreate);
            if (param.stackOffset >= 0) {
              cg.movToMem(Rsp, param.stackOffset, Rax);
            } else {
              stageGpr(param, Rax);
            }
          }
          cg.jmp(staged);
          cg.bind(notFunc);
          emitFailCall(cg, fail, msgType);
          cg.bind(staged);
          break;
        }
        // A `cstring`/`ptr` crosses as a GPR holding a machine address. `nil`
        // is C's NULL (0). A Oskel string is an `AotString` (a length then the
        // bytes, *not* NUL-terminated), so it cannot be passed as-is: the thunk
        // materialises a NUL-terminated copy in the arena and passes its
        // address, exactly the `const char *` a C callee dereferences. A
        // non-string value is not a pointer the AOT runtime can represent, so
        // it fails like the hosted engine does.
        CodeGen::Label isNil = cg.newLabel();
        CodeGen::Label isStr = cg.newLabel();
        CodeGen::Label isPtr = cg.newLabel();
        CodeGen::Label staged = cg.newLabel();
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagNil));
        cg.jcc(Jcc::Eq, isNil);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagStr));
        cg.jcc(Jcc::Eq, isStr);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagPtr));
        cg.jcc(Jcc::Eq, isPtr);
        emitFailCall(cg, fail, msgType);
        cg.bind(isNil);
        cg.xorReg(R11, R11);  // NULL
        cg.jmp(staged);
        cg.bind(isPtr);
        // A pointer handle already owns NUL-terminated bytes (a `CString` keeps
        // its terminator; a `Void` carries the opaque address), so it crosses
        // as-is -- no copy. This is what the hosted engine passes for a `cstring`
        // argument, and it is how a `cstring` returned by one C function can be
        // fed back into another.
        cg.movFromMem(R11, Rbx, base + 8);            // the AotPtr*
        cg.movFromMem(R11, R11, kPtrDataOffset);      // its bytes
        cg.jmp(staged);
        cg.bind(isStr);
        cg.movFromMem(R12, Rbx, base + 8);  // the AotString* (a length word)
        cg.movFromMem(Rdi, R12, 0);         // the byte length
        cg.addImm32(Rdi, 1);                // the bytes plus the terminating NUL
        cg.callLabel(allocLabel);           // rax = the block (or null)
        cg.testReg(Rax);
        cg.jcc(Jcc::Eq, oom);
        // Copy the bytes: dst = rax, src = the AotString's bytes, count = the
        // length (reloaded from the header, which r12 still points at). `rep
        // movsb` leaves rax (the block) untouched and advances rdi to its end,
        // so the NUL goes right after the bytes.
        cg.movReg(Rdi, Rax);
        cg.movReg(Rsi, R12);
        cg.addImm32(Rsi, static_cast<std::int32_t>(kAotStringHeaderSize));
        cg.movFromMem(Rcx, R12, 0);
        cg.repMovsb();
        cg.movImmByteToMem(Rdi, 0, 0);  // the terminating NUL
        cg.movReg(R11, Rax);
        cg.bind(staged);
        if (param.stackOffset >= 0) {
          cg.movToMem(Rsp, param.stackOffset, R11);
        } else {
          stageGpr(param, R11);
        }
        break;
      }
      default:
        // A callback or struct parameter is not part of this thunk; the lowerer
        // rejects such a declaration before it reaches here.
        emitFailCall(cg, fail, msgType);
        break;
    }
  }

  // Load the staged integer arguments into their registers now that every
  // `alloc`/`rep movsb` has run. The SSE arguments are already in place. A
  // Win64 argument uses the register that matches its position, so an unused
  // slot is left alone instead of shifting the later ones.
  for (int k = 0; k < intArgCount; ++k) {
    if (gprUsed[k]) cg.movFromMem(intArgRegs[k], Rbp, kStageBase + 8LL * k);
  }

  // --- C variadic: assign the tail --------------------------------------------
  //
  // Pass B runs after every `alloc` and after the fixed prefix is in its
  // registers, and makes no calls, so it can write each tail argument straight
  // to its destination: the next free GPR, the next free SSE register, or the
  // outgoing stack area. Each value is promoted to its C type: an `int`/`bool`
  // to 64 bits, a `float`/`double` to binary64 (the AOT payload already is), a
  // string to the NUL-terminated copy pass A made, and a `ptr` to its address.
  if (thunk.variadic) {
    const std::int32_t fixedArity = static_cast<std::int32_t>(thunk.arity);
    int fixedGpr = 0;
    int fixedSse = 0;
    if (win64) {
      fixedGpr = fixedArity;
      for (const AotThunkParam& param : thunk.params) {
        if ((param.cls == ffi::ParamClass::Double ||
             param.cls == ffi::ParamClass::Float) &&
            (param.win64Position >= 0 ? param.win64Position : param.argIndex) < 4) {
          ++fixedSse;
        }
      }
    } else {
      for (const AotThunkParam& param : thunk.params) {
        if (param.cls == ffi::ParamClass::Double || param.cls == ffi::ParamClass::Float) {
          ++fixedSse;
        } else {
          ++fixedGpr;
        }
      }
    }
    cg.movFromMem(R11, Rbp, kVarTailCount);
    cg.movToMem(Rbp, kVarRemaining, R11);
    cg.movReg(R10, Rbx);
    if (fixedArity > 0) cg.subRegImm32(R10, 16 * fixedArity);
    cg.movToMem(Rbp, kVarP, R10);
    cg.movFromMem(R11, Rbp, kVarStageBase);
    cg.movToMem(Rbp, kVarStageCur, R11);
    cg.movImm(R11, fixedGpr);
    cg.movToMem(Rbp, kVarGpr, R11);
    cg.movImm(R11, fixedSse);
    cg.movToMem(Rbp, kVarSse, R11);
    cg.movReg(R12, Rsp);  // the outgoing stack cursor
    if (varOutBase > 0) cg.addImm32(R12, static_cast<std::int32_t>(varOutBase));

    CodeGen::Label tailLoop = cg.newLabel();
    CodeGen::Label tailDone = cg.newLabel();
    CodeGen::Label tailNext = cg.newLabel();
    CodeGen::Label tailInt = cg.newLabel();
    CodeGen::Label tailFloat = cg.newLabel();
    CodeGen::Label tailStr = cg.newLabel();
    CodeGen::Label tailPtr = cg.newLabel();
    CodeGen::Label tailNil = cg.newLabel();
    CodeGen::Label assignGpr = cg.newLabel();
    CodeGen::Label assignSse = cg.newLabel();
    CodeGen::Label gprAssigned = cg.newLabel();
    CodeGen::Label gprSpill = cg.newLabel();
    CodeGen::Label sseAssigned = cg.newLabel();
    CodeGen::Label sseSpill = cg.newLabel();

    cg.bind(tailLoop);
    cg.movFromMem(R11, Rbp, kVarRemaining);
    cg.testReg(R11);
    cg.jcc(Jcc::Eq, tailDone);
    cg.movFromMem(R10, Rbp, kVarP);
    cg.movFromMem(R11, R10, 0);  // the value's tag
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagInt));
    cg.jcc(Jcc::Eq, tailInt);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagBool));
    cg.jcc(Jcc::Eq, tailInt);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagFloat));
    cg.jcc(Jcc::Eq, tailFloat);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagDouble));
    cg.jcc(Jcc::Eq, tailFloat);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagStr));
    cg.jcc(Jcc::Eq, tailStr);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagPtr));
    cg.jcc(Jcc::Eq, tailPtr);
    cg.cmpImm32(R11, static_cast<std::int32_t>(kTagNil));
    cg.jcc(Jcc::Eq, tailNil);
    emitFailCall(cg, fail, msgType);

    cg.bind(tailInt);
    cg.movFromMem(R11, R10, 8);  // an int/bool payload is the 64-bit value
    cg.jmp(assignGpr);
    cg.bind(tailFloat);
    cg.movFromMem(R11, R10, 8);  // a Float/Double payload is binary64 already
    cg.jmp(assignSse);
    cg.bind(tailStr);
    cg.movFromMem(R10, Rbp, kVarStageCur);
    cg.movFromMem(R11, R10, 0);  // the NUL-terminated copy pass A made
    cg.jmp(assignGpr);
    cg.bind(tailPtr);
    cg.movFromMem(R11, R10, 8);              // the AotPtr*
    cg.movFromMem(R11, R11, kPtrDataOffset); // its address
    cg.jmp(assignGpr);
    cg.bind(tailNil);
    cg.xorReg(R11, R11);  // C's NULL
    cg.jmp(assignGpr);

    // The register index is a run-time value, so the destination is a small
    // compare chain. The counters live in the frame and are read into `rax`,
    // which is not an argument register, so no loaded argument is disturbed.
    cg.bind(assignGpr);
    cg.movFromMem(Rax, Rbp, kVarGpr);
    for (int k = 0; k < intArgCount; ++k) {
      CodeGen::Label skip = cg.newLabel();
      cg.cmpImm32(Rax, k);
      cg.jcc(Jcc::Ne, skip);
      cg.movReg(intArgRegs[k], R11);
      cg.jmp(gprAssigned);
      cg.bind(skip);
    }
    cg.jmp(gprSpill);
    cg.bind(gprAssigned);
    if (!win64) {
      cg.addImm32(Rax, 1);
      cg.movToMem(Rbp, kVarGpr, Rax);
    }
    cg.jmp(tailNext);

    cg.bind(assignSse);
    if (win64) {
      // Win64 picks the xmm register by argument position and duplicates the
      // value in the integer register of the same position.
      cg.movFromMem(Rax, Rbp, kVarGpr);
      for (int k = 0; k < sseArgCount; ++k) {
        CodeGen::Label skip = cg.newLabel();
        cg.cmpImm32(Rax, k);
        cg.jcc(Jcc::Ne, skip);
        cg.movqToXmm(kXmmBase + k, R11);
        cg.movReg(intArgRegs[k], R11);
        cg.jmp(sseAssigned);
        cg.bind(skip);
      }
      cg.jmp(sseSpill);
      cg.bind(sseAssigned);
      cg.movFromMem(Rax, Rbp, kVarSse);
      cg.addImm32(Rax, 1);
      cg.movToMem(Rbp, kVarSse, Rax);
    } else {
      cg.movFromMem(Rax, Rbp, kVarSse);
      for (int k = 0; k < sseArgCount; ++k) {
        CodeGen::Label skip = cg.newLabel();
        cg.cmpImm32(Rax, k);
        cg.jcc(Jcc::Ne, skip);
        cg.movqToXmm(kXmmBase + k, R11);
        cg.jmp(sseAssigned);
        cg.bind(skip);
      }
      cg.jmp(sseSpill);
      cg.bind(sseAssigned);
      cg.addImm32(Rax, 1);
      cg.movToMem(Rbp, kVarSse, Rax);
    }
    cg.jmp(tailNext);

    // No register left: the value goes to the outgoing stack area, in order.
    cg.bind(gprSpill);
    cg.movToMem(R12, 0, R11);
    cg.addImm32(R12, 8);
    cg.jmp(tailNext);
    cg.bind(sseSpill);
    cg.movToMem(R12, 0, R11);
    cg.addImm32(R12, 8);

    cg.bind(tailNext);
    cg.movFromMem(R10, Rbp, kVarStageCur);
    cg.addImm32(R10, 8);
    cg.movToMem(Rbp, kVarStageCur, R10);
    cg.movFromMem(R10, Rbp, kVarP);
    cg.subRegImm32(R10, 16);
    cg.movToMem(Rbp, kVarP, R10);
    cg.movFromMem(R11, Rbp, kVarRemaining);
    cg.subRegImm32(R11, 1);
    cg.movToMem(Rbp, kVarRemaining, R11);
    if (win64) {
      // Win64 consumes a position for every tail argument, register or stack.
      cg.movFromMem(R11, Rbp, kVarGpr);
      cg.addImm32(R11, 1);
      cg.movToMem(Rbp, kVarGpr, R11);
    }
    cg.jmp(tailLoop);
    cg.bind(tailDone);

    // `%al` is the number of vector registers a variadic callee may inspect.
    cg.movFromMem(Rax, Rbp, kVarSse);
  }

  // Park the current arena cursor/end where a stored callback will find it.
  // The callback may be invoked by this C function or by a later one that has
  // no callback argument (`set_cb` now, `call_cb` later), so every extern call
  // in a program that has a callback signature publishes the state first.
  if (callbackStateSlot >= 0) {
    CodeGen::Label noState = cg.newLabel();
    cg.movFromMem(R10, R15, 16 * callbackStateSlot + 8);
    cg.testReg(R10);
    cg.jcc(Jcc::Eq, noState);
    cg.movToMem(R10, 0, R13);
    cg.movToMem(R10, 8, R14);
    cg.bind(noState);
  }

  cg.callExternal(thunk.symbol);

  // A callback the C function invoked may have allocated (the `ptr` argument
  // boxes and a string return do), so the arena cursor/end the C caller
  // preserved are stale: the callback body parked the advanced values in the
  // shared state, and the program must resume from there. Reloading after the
  // call is what lets a C caller that uses r13/r14 itself stay correct while the
  // Oskel arena keeps growing.
  if (callbackStateSlot >= 0) {
    CodeGen::Label noState = cg.newLabel();
    cg.movFromMem(R10, R15, 16 * callbackStateSlot + 8);
    cg.testReg(R10);
    cg.jcc(Jcc::Eq, noState);
    cg.movFromMem(R13, R10, 0);
    cg.movFromMem(R14, R10, 8);
    cg.bind(noState);
  }

  // A struct return comes back in the registers its eightbyte classes assign
  // (`rax`/`rdx` and `xmm0`/`xmm1`), so it is rebuilt into a map and the scalar
  // switch below never sees it.
  if (thunk.returnStructIndex >= 0) {
    // A register-class struct is in the return registers; a class-MEMORY one was
    // written by the callee into the `sret` buffer.
    emitStructReturn(cg, thunk.structs[static_cast<std::size_t>(thunk.returnStructIndex)],
                     thunk.returnMemoryBytes > 0 ? sretBase : kStructBufBase, tableNew,
                     memberSet, oom, allocLabel, fail, msgCStringTooLong,
                     win64 && thunk.returnMemoryBytes == 0,
                     thunk.returnMemoryBytes > 0);
    restoreFrame();
    cg.bind(oom);
    emitFailCall(cg, fail, msgOom);
    return;
  }

  // Box the C result: the integer family comes back in `rax` and is normalized
  // to its declared width, a float/double in `xmm0`. A `void` returns nil.
  switch (thunk.returns) {
    case ffi::ReturnClass::Void:
      cg.movImm(Rax, kTagNil);
      cg.xorReg(Rdx, Rdx);
      break;
    case ffi::ReturnClass::Float:
      cg.cvtss2sd(Xmm0, Xmm0);  // a `float` return is binary32; box it as binary64
      cg.movqFromXmm(Rdx, Xmm0);
      cg.movImm(Rax, kTagFloat);
      break;
    case ffi::ReturnClass::Double:
      cg.movqFromXmm(Rdx, Xmm0);
      cg.movImm(Rax, kTagDouble);
      break;
    case ffi::ReturnClass::Bool:
      cg.andImm32(Rax, 0xff);
      cg.testReg(Rax);
      cg.setcc(Jcc::Ne, Rdx);
      cg.movzx(Rdx, Rdx);
      cg.movImm(Rax, kTagBool);
      break;
    case ffi::ReturnClass::CString:
      // A `const char *` comes back in `rax`.
      emitCStringBox(cg, Rax, oom, allocLabel, fail, msgCStringTooLong);
      break;
    case ffi::ReturnClass::Ptr:
      // An opaque `void *` comes back in `rax`; the box maps NULL to nil and
      // keeps every other address as a `Void` handle.
      cg.movReg(Rdi, Rax);
      cg.callLabel(voidPtrBox);
      break;
    default:  // Int
      normalizeIntegerReturn(cg, thunk.returnType);
      cg.movReg(Rdx, Rax);
      cg.movImm(Rax, kTagInt);
      break;
  }

  restoreFrame();

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `void* oskel_callback_create(void* body, AotValue closure)`: builds the C
// function pointer the thunk passes for an Oskel callback. A C function pointer
// carries no context, so each registration gets a tiny executable stub that
// loads the closure and jumps to the shared body:
//
//   49 BA <context>   movabs r10, context
//   49 BB <body>      movabs r11, body
//   41 FF E3          jmp r11
//
// The context is 32 bytes in the arena: the closure tag and payload, the Oskel
// global base, and a pointer to the shared arena state. The body reads it from
// r10. The stub lives in a private `mmap` page, because the image's own text is
// not writable and the compiler cannot know a runtime closure's address. The
// page is mapped read+write, filled, and then `mprotect`ed to read+execute:
// it is never writable and executable at the same time (W^X), exactly like the
// JIT's `JitCode::assign`. This helper exists only in the hosted mode, where
// libc provides `mmap` and `mprotect`.
void emitCallbackCreate(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                        CodeGen::Label fail, const DataBlob& msgOom,
                        const DataBlob& msgCallback, int stateSlot) {
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label mmapFailed = cg.newLabel();
  CodeGen::Label protectFailed = cg.newLabel();
  CodeGen::Label haveState = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  // [rbp-24] the closure tag, [rbp-32] its payload, [rbp-40] the body address.
  cg.movToMem(Rbp, -24, Rsi);
  cg.movToMem(Rbp, -32, Rdx);
  cg.movToMem(Rbp, -40, Rdi);

  cg.movImm(Rdi, 32);
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(R12, Rax);  // the context
  cg.movFromMem(R10, Rbp, -24);
  cg.movToMem(R12, 0, R10);
  cg.movFromMem(R10, Rbp, -32);
  cg.movToMem(R12, 8, R10);
  cg.movToMem(R12, 16, R15);  // the Oskel global base

  // The shared arena state is created once and pointed at by every registration,
  // so a callback that allocates advances the cursor the next callback sees.
  // The global table is an array of 16-byte `AotValue` slots; the pointer is
  // the slot's payload (the tag stays nil, since no Oskel code reads it).
  cg.movFromMem(R10, R15, 16 * stateSlot + 8);
  cg.testReg(R10);
  cg.jcc(Jcc::Ne, haveState);
  cg.movImm(Rdi, 16);
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(R10, Rax);
  cg.movToMem(R15, 16 * stateSlot + 8, R10);
  cg.bind(haveState);
  cg.movToMem(R12, 24, R10);
  // Every allocation above advanced r13/r14; the state starts from the current
  // arena, not from a snapshot taken before the context existed.
  cg.movToMem(R10, 0, R13);
  cg.movToMem(R10, 8, R14);

  // mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0).
  // The page is writable only while the stub is copied in; `mprotect` below
  // turns it read+execute before the function returns, so it is never W+X. A
  // callback is a System V shape, emitted on the Linux and macOS hosted
  // targets; Windows rejects the parameter.
  cg.xorReg(Rdi, Rdi);
  cg.movImm(Rsi, 4096);
  cg.movImm(Rdx, 3);
  cg.movImm(Rcx, 0x22);
  cg.movImm(R8, -1);
  cg.xorReg(R9, R9);
  cg.callExternal("mmap");
  cg.cmpImm32(Rax, -1);
  cg.jcc(Jcc::Eq, mmapFailed);
  cg.movReg(Rbx, Rax);  // the stub page

  cg.movImmByteToMem(Rbx, 0, 0x49);   // movabs r10, ...
  cg.movImmByteToMem(Rbx, 1, 0xBA);
  cg.movToMem(Rbx, 2, R12);           // the context
  cg.movImmByteToMem(Rbx, 10, 0x49);  // movabs r11, ...
  cg.movImmByteToMem(Rbx, 11, 0xBB);
  cg.movFromMem(R10, Rbp, -40);
  cg.movToMem(Rbx, 12, R10);          // the body
  cg.movImmByteToMem(Rbx, 20, 0x41);  // jmp r11
  cg.movImmByteToMem(Rbx, 21, 0xFF);
  cg.movImmByteToMem(Rbx, 22, 0xE3);

  // mprotect(page, 4096, PROT_READ|PROT_EXEC): drop write permission now that
  // the stub is complete. A failure unmaps the page and reports a clean build
  // error; a stub that is still writable must never reach C.
  cg.movReg(Rdi, Rbx);
  cg.movImm(Rsi, 4096);
  cg.movImm(Rdx, 5);
  cg.callExternal("mprotect");
  cg.cmpImm32(Rax, -1);
  cg.jcc(Jcc::Eq, protectFailed);

  // The page is intentionally process-lifetime. C may store the function
  // pointer for a later call (a stored callback, including an `atexit`
  // handler), and Oskel has no way to learn when C is done with it; freeing
  // it here would break that contract. The OS reclaims the mapping at exit.
  // BACKLOG §6 item 29 tracks pooling or an explicit release contract.
  cg.movReg(Rax, Rbx);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
  cg.bind(mmapFailed);
  emitFailCall(cg, fail, msgCallback);
  cg.bind(protectFailed);
  // The stub was written but could not be made executable; unmap it before
  // reporting so a failed registration does not leave a writable page behind.
  cg.movReg(Rdi, Rbx);
  cg.movImm(Rsi, 4096);
  cg.callExternal("munmap");
  emitFailCall(cg, fail, msgCallback);
}

// The shared body of one callback signature. The C side calls a runtime stub,
// which leaves the closure context in r10 and jumps here. The body saves the C
// arguments, boxes them into the Oskel argument window (`args[i]` at
// `args - 16*i`), calls `oskel_call`, and converts the result to the callback's
// C return class.
//
// The C ABI is the System V AMD64 one: six GPRs, eight SSE registers, then the
// stack. The frame keeps the saved arguments, the window and the two callee-
// saved registers disjoint; `r12` holds the closure context across the boxing
// helpers (they may clobber `rbx`), and `r13`/`r14` are the arena cursor/end, so
// they are never touched.
void emitCallbackBody(CodeGen& cg, const AotCallbackSignature& sig,
                      CodeGen::Label call, CodeGen::Label allocLabel,
                      CodeGen::Label memberGet, CodeGen::Label voidPtrBox,
                      CodeGen::Label fail, const DataBlob& msgOom,
                      const DataBlob& msgType, const DataBlob& msgCStringTooLong,
                      const DataBlob& msgStructReturn, const DataBlob& msgStructField) {
  constexpr std::int32_t kStageBytes = (6 + 8) * 8;  // six GPRs and eight SSE slots
  // Five callee-saved registers are pushed (rbx, r12-r15), then the stage and
  // the window live below them. The stage base must clear the saved r15 at
  // -40, or the SSE slots overwrite it and the body returns a wrong r15.
  constexpr std::int64_t kSavedBytes = 5 * 8;
  constexpr std::int64_t kStageBase = -(kSavedBytes + 8 + kStageBytes);
  constexpr std::int64_t kArgs = kStageBase - 16;    // window[0]
  const std::int32_t arity = static_cast<std::int32_t>(sig.params.size());
  // A struct return needs room below the argument window for the returned map
  // value, the C bytes staged from its fields, the hidden `sret` pointer and
  // the closure context (the field staging helper uses r12 as a scratch).
  const std::int64_t structSpan =
      sig.returnsStruct
          ? (static_cast<std::int64_t>(sig.returnStruct.layout.size) + 7) / 8 * 8
          : 0;
  const std::int64_t kStructValue = kStageBase - 16LL * arity - 16;
  const std::int64_t kSretSlot = kStructValue - 8;
  const std::int64_t kStructBuf = kSretSlot - structSpan;
  const std::int64_t kContextSlot = kStructBuf - 8;
  const std::int64_t extra =
      sig.returnsStruct ? ((32 + structSpan + 15) / 16) * 16 : 0;
  const std::int32_t frameBytes =
      kStageBytes + 16 * arity + static_cast<std::int32_t>(extra);
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label bad = cg.newLabel();

  cg.bind(sig.body);
  prologue(cg);
  // The C caller expects every callee-saved register preserved, so the body
  // saves r13/r14/r15 -- the Oskel arena cursor/end and global base -- around
  // the call. They are restored from the shared state, not from a snapshot: a
  // callback that allocates advances the cursor the next callback must see.
  cg.push(Rbx);
  cg.push(R12);
  cg.push(R13);
  cg.push(R14);
  cg.push(R15);
  cg.subRegImm32(Rsp, frameBytes + 8);
  cg.movReg(R12, R10);          // the closure context from the stub
  cg.movFromMem(R15, R12, 16);  // the Oskel global base
  cg.movFromMem(R11, R12, 24);  // the shared arena state
  cg.movFromMem(R13, R11, 0);   // the arena cursor
  cg.movFromMem(R14, R11, 8);   // the arena end

  // Save the C arguments before any helper call. The SSE slots keep the raw
  // bits: a `float` is narrowed back to binary32 below, a `double` is verbatim.
  for (int i = 0; i < 6; ++i) cg.movToMem(Rbp, kStageBase + 8 * i, kSysVIntArgRegs[i]);
  for (int i = 0; i < 8; ++i) {
    cg.movqFromXmm(R11, kXmmBase + i);
    cg.movToMem(Rbp, kStageBase + 8 * (6 + i), R11);
  }

  // Box each C argument into the window. `args[i]` sits at `args - 16*i`; the
  // window is written through `rbp`, so a helper clobbering `rbx` is harmless.
  for (std::int32_t i = 0; i < arity; ++i) {
    const AotCallbackParam& param = sig.params[static_cast<std::size_t>(i)];
    const std::int64_t slot = kArgs - 16LL * i;
    if (param.gpr >= 0) {
      cg.movFromMem(R11, Rbp, kStageBase + 8 * param.gpr);
    } else if (param.sse >= 0) {
      cg.movFromMem(R11, Rbp, kStageBase + 8 * (6 + param.sse));
    } else if (param.cls == ffi::ParamClass::Float) {
      cg.movFromMem32(R11, Rbp, 16 + param.stackOffset);
    } else {
      cg.movFromMem(R11, Rbp, 16 + param.stackOffset);
    }

    if (param.cls == ffi::ParamClass::Float) {
      cg.movqToXmm(XmmScratch, R11);
      cg.cvtss2sd(XmmScratch, XmmScratch);
      cg.movqFromXmm(Rdx, XmmScratch);
      cg.movImm(Rax, kTagFloat);
    } else if (param.cls == ffi::ParamClass::Double) {
      cg.movReg(Rdx, R11);
      cg.movImm(Rax, kTagDouble);
    } else if (param.cls == ffi::ParamClass::Pointer) {
      if (param.type == "cstring") {
        emitCStringBox(cg, R11, oom, allocLabel, fail, msgCStringTooLong);
      } else {
        cg.movReg(Rdi, R11);
        cg.callLabel(voidPtrBox);
      }
    } else if (param.type == "bool") {
      cg.movReg(Rax, R11);
      cg.andImm32(Rax, 0xff);
      cg.testReg(Rax);
      cg.setcc(Jcc::Ne, Rdx);
      cg.movzx(Rdx, Rdx);
      cg.movImm(Rax, kTagBool);
    } else {
      cg.movReg(Rax, R11);
      normalizeIntegerReturn(cg, param.type);
      cg.movReg(Rdx, Rax);
      cg.movImm(Rax, kTagInt);
    }
    cg.movToMem(Rbp, slot, Rax);
    cg.movToMem(Rbp, slot + 8, Rdx);
  }

  // Call the Oskel closure. The context is an `AotValue` (Func, AotClosure*).
  cg.movFromMem(Rdi, R12, 0);
  cg.movFromMem(Rsi, R12, 8);
  cg.movImm(Rdx, arity);
  cg.leaMem(Rcx, Rbp, kArgs);
  cg.callLabel(call);

  // Convert the returned `AotValue` (rax:rdx) to the callback's C return class.
  switch (sig.returns) {
    case ffi::ReturnClass::Void:
      break;
    case ffi::ReturnClass::Bool: {
      CodeGen::Label asBool = cg.newLabel();
      CodeGen::Label asInt = cg.newLabel();
      CodeGen::Label done = cg.newLabel();
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagBool));
      cg.jcc(Jcc::Eq, asBool);
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
      cg.jcc(Jcc::Eq, asInt);
      cg.jmp(bad);
      cg.bind(asBool);
      cg.movReg(Rax, Rdx);
      cg.andImm32(Rax, 1);
      cg.jmp(done);
      cg.bind(asInt);
      cg.testReg(Rdx);
      cg.setcc(Jcc::Ne, Rax);
      cg.movzx(Rax, Rax);
      cg.bind(done);
      break;
    }
    case ffi::ReturnClass::Int: {
      CodeGen::Label asInt = cg.newLabel();
      CodeGen::Label asBool = cg.newLabel();
      CodeGen::Label done = cg.newLabel();
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
      cg.jcc(Jcc::Eq, asInt);
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagBool));
      cg.jcc(Jcc::Eq, asBool);
      cg.jmp(bad);
      cg.bind(asInt);
      cg.movReg(Rax, Rdx);
      normalizeIntegerReturn(cg, sig.returnType);
      cg.jmp(done);
      cg.bind(asBool);
      cg.movReg(Rax, Rdx);
      cg.andImm32(Rax, 1);
      cg.bind(done);
      break;
    }
    case ffi::ReturnClass::Float: {
      CodeGen::Label asNumber = cg.newLabel();
      CodeGen::Label asInt = cg.newLabel();
      CodeGen::Label done = cg.newLabel();
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFloat));
      cg.jcc(Jcc::Eq, asNumber);
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagDouble));
      cg.jcc(Jcc::Eq, asNumber);
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
      cg.jcc(Jcc::Eq, asInt);
      cg.jmp(bad);
      cg.bind(asNumber);
      cg.movqToXmm(Xmm0, Rdx);
      cg.cvtsd2ss(Xmm0, Xmm0);
      cg.jmp(done);
      cg.bind(asInt);
      cg.cvtsi2sd(XmmScratch, Rdx);
      cg.cvtsd2ss(Xmm0, XmmScratch);
      cg.bind(done);
      break;
    }
    case ffi::ReturnClass::Double: {
      CodeGen::Label asNumber = cg.newLabel();
      CodeGen::Label asInt = cg.newLabel();
      CodeGen::Label done = cg.newLabel();
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFloat));
      cg.jcc(Jcc::Eq, asNumber);
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagDouble));
      cg.jcc(Jcc::Eq, asNumber);
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
      cg.jcc(Jcc::Eq, asInt);
      cg.jmp(bad);
      cg.bind(asNumber);
      cg.movqToXmm(Xmm0, Rdx);
      cg.jmp(done);
      cg.bind(asInt);
      cg.cvtsi2sd(Xmm0, Rdx);
      cg.bind(done);
      break;
    }
    case ffi::ReturnClass::CString: {
      CodeGen::Label isNil = cg.newLabel();
      CodeGen::Label isStr = cg.newLabel();
      CodeGen::Label isPtr = cg.newLabel();
      CodeGen::Label done = cg.newLabel();
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagNil));
      cg.jcc(Jcc::Eq, isNil);
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
      cg.jcc(Jcc::Eq, isStr);
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagPtr));
      cg.jcc(Jcc::Eq, isPtr);
      cg.jmp(bad);
      cg.bind(isNil);
      cg.xorReg(Rax, Rax);
      cg.jmp(done);
      cg.bind(isPtr);
      cg.movFromMem(Rax, Rdx, kPtrDataOffset);
      cg.jmp(done);
      cg.bind(isStr);
      // An AotString is not NUL-terminated; copy it into an arena block with
      // the terminator, exactly like a `cstring` argument.
      cg.movReg(Rbx, Rdx);              // the AotString*
      cg.movFromMem(Rdi, Rbx, 0);       // the byte length
      cg.addImm32(Rdi, 1);              // plus the NUL
      cg.callLabel(allocLabel);
      cg.testReg(Rax);
      cg.jcc(Jcc::Eq, oom);
      cg.movReg(Rdi, Rax);
      cg.movReg(Rsi, Rbx);
      cg.addImm32(Rsi, static_cast<std::int32_t>(kAotStringHeaderSize));
      cg.movFromMem(Rcx, Rbx, 0);
      cg.repMovsb();
      cg.movImmByteToMem(Rdi, 0, 0);
      cg.bind(done);
      break;
    }
    case ffi::ReturnClass::Ptr: {
      CodeGen::Label isNil = cg.newLabel();
      CodeGen::Label isPtr = cg.newLabel();
      CodeGen::Label done = cg.newLabel();
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagNil));
      cg.jcc(Jcc::Eq, isNil);
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagPtr));
      cg.jcc(Jcc::Eq, isPtr);
      cg.jmp(bad);
      cg.bind(isNil);
      cg.xorReg(Rax, Rax);
      cg.jmp(done);
      cg.bind(isPtr);
      cg.movFromMem(Rax, Rdx, kPtrDataOffset);
      cg.bind(done);
      break;
    }
    case ffi::ReturnClass::Struct: {
      // The closure returns the struct as a map. Stage its C bytes from the
      // fields, then load the eightbytes into the return registers or copy the
      // buffer through the hidden `sret` pointer. A non-map result is the VM's
      // error, worded the same.
      CodeGen::Label isMap = cg.newLabel();
      cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagMap));
      cg.jcc(Jcc::Eq, isMap);
      emitFailCall(cg, fail, msgStructReturn);
      cg.bind(isMap);
      cg.movToMem(Rbp, kStructValue, Rax);
      cg.movToMem(Rbp, kStructValue + 8, Rdx);
      if (sig.returnMemory) {
        // rdi held the hidden `sret` pointer on entry; the argument stage kept
        // it at the first GPR slot.
        cg.movFromMem(R11, Rbp, kStageBase);
        cg.movToMem(Rbp, kSretSlot, R11);
      }
      // The field staging helper uses r12 as a scratch for a `cstring` field,
      // so park the closure context and reload it afterwards.
      cg.movToMem(Rbp, kContextSlot, R12);
      cg.movReg(Rbx, Rbp);
      emitStructArgBytes(cg, sig.returnStruct, kStructValue, Rbp, kStructBuf,
                         memberGet, oom, allocLabel, fail, msgStructField);
      cg.movFromMem(R12, Rbp, kContextSlot);
      if (sig.returnMemory) {
        cg.movFromMem(Rdi, Rbp, kSretSlot);
        cg.leaMem(Rsi, Rbp, kStructBuf);
        cg.movImm(Rcx, static_cast<std::int64_t>(sig.returnStruct.layout.size));
        cg.cld();
        cg.repMovsb();
        cg.movFromMem(Rax, Rbp, kSretSlot);  // System V also returns the buffer
      } else {
        int intReg = 0;
        int sseReg = 0;
        for (int e = 0; e < sig.returnStruct.layout.eightbyteCount; ++e) {
          cg.movFromMem(R11, Rbp, kStructBuf + 8LL * e);
          if (sig.returnStruct.layout.eightbyte[e] == 'S') {
            cg.movqToXmm(kXmmBase + sseReg, R11);
            ++sseReg;
          } else {
            if (intReg == 0) {
              cg.movReg(Rax, R11);
            } else {
              cg.movReg(Rdx, R11);
            }
            ++intReg;
          }
        }
      }
      break;
    }
    default:
      cg.jmp(bad);
      break;
  }

  // Park the advanced arena cursor/end for the next callback, then restore the
  // C caller's callee-saved registers.
  cg.movFromMem(R11, R12, 24);
  cg.movToMem(R11, 0, R13);
  cg.movToMem(R11, 8, R14);
  cg.addImm32(Rsp, frameBytes + 8);
  cg.pop(R15);
  cg.pop(R14);
  cg.pop(R13);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
  cg.bind(bad);
  emitFailCall(cg, fail, msgType);
}

}  // namespace

// ---------------------------------------------------------------------------
// The pointer, file, clock and argument builtins (BACKLOG §6 item 25)
//
// These are the builtins that used to exist only in the development engine, so a
// program that called one failed `oskel build` with "global 'X' is read but
// never assigned". They now land in both engines:
//
//   * the pointer ones (`cstring`, `cstr`, `buffer`, `byteLen`, `loadByte`,
//     `storeByte`, `intToPtr`, `ptrToInt`, `free`) work on every target, because
//     a handle is an `AotPtr` in the arena and nothing reaches a host;
//   * the file ones (`readFile`, `writeFile`, `fileExists`) are written against
//     the `entry.open`/`readFd`/`writeFd`/`close` handles, so the syscall/API
//     difference stays in the prelude and a target with no file system simply
//     does not seed them;
//   * `clock()` reads `entry.monotonic` minus an origin the chunk captured at
//     entry, and `args()` reads a hidden global the chunk filled there.
//
// A failure reports the hosted message's fixed part. The hosted helpers append a
// dynamic tail (`, got 'int'`, the offending index); the AOT runtime keeps fixed
// text, the convention every other builtin already follows (`ord`, `len`,
// `chr`, `sqrt`, ...), so a failing binary exits 70 with the same wording up to
// that tail.
// ---------------------------------------------------------------------------

// `AotValue oskel_str_new(const char* bytes, long length)`: allocate an
// `AotString` for `length` bytes, copy them in and return the `Str` value (tag
// in rax, payload in rdx). The bytes need no terminator, so a caller holding a
// NUL-terminated buffer passes the text length rather than the buffer size.
void emitStrNew(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(R12, Rdi);         // bytes
  cg.movToMem(Rbp, -24, Rsi);  // length
  cg.movReg(Rdi, Rsi);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);  // the AotString*
  cg.movFromMem(R8, Rbp, -24);
  cg.movToMem(Rbx, 0, R8);  // the length word
  cg.movReg(Rdi, Rbx);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rsi, R12);
  cg.movReg(Rcx, R8);
  cg.cld();
  cg.repMovsb();  // a zero length is a no-op
  cg.movReg(Rdx, Rbx);
  cg.movImm(Rax, kTagStr);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `AotPtr* oskel_ptr_new(long kind, long length)`: allocate a handle and the
// `length` bytes it owns, in one block, and return the header -- or null when
// the arena is exhausted, which the caller reports. The bytes are zeroed, which
// is what `buffer()` promises and what gives a `cstring` its terminator for
// free. A `Void` handle owns no bytes: its `data` word is the opaque token the
// caller stores.
void emitPtrNew(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel) {
  CodeGen::Label zeroLoop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);  // kind
  cg.movReg(R12, Rsi);  // length
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotPtrHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, done);  // rax = 0: the caller reports out of memory
  cg.movToMem(Rax, kPtrKindOffset, Rbx);
  cg.movToMem(Rax, kPtrLengthOffset, R12);
  cg.movImmToMem(Rax, kPtrReleasedOffset, 0);
  cg.movReg(R8, Rax);
  cg.addImm32(R8, static_cast<std::int32_t>(kAotPtrHeaderSize));
  cg.movToMem(Rax, kPtrDataOffset, R8);
  cg.xorReg(R9, R9);    // index
  cg.xorReg(R11, R11);  // the zero byte
  cg.bind(zeroLoop);
  cg.cmpReg(R9, R12);
  cg.jcc(Jcc::Ae, done);
  cg.movReg(R10, R8);
  cg.addReg(R10, R9);
  cg.movByteToMem(R10, 0, R11);
  cg.addImm32(R9, 1);
  cg.jmp(zeroLoop);

  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `long oskel_ptr_text_len(AotPtr* handle)`: how many bytes a `cstring` exposes
// as text -- it stops at its first NUL, C's view of it. Every other kind keeps
// all its bytes. A leaf: it calls nothing and touches only rax and r8-r11.
void emitPtrTextLen(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label opaque = cg.newLabel();
  CodeGen::Label scan = cg.newLabel();
  CodeGen::Label scanned = cg.newLabel();

  cg.bind(label);
  cg.movFromMem(R8, Rdi, kPtrKindOffset);
  cg.movFromMem(R9, Rdi, kPtrLengthOffset);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kPtrKindCString));
  cg.jcc(Jcc::Ne, opaque);
  cg.movFromMem(R10, Rdi, kPtrDataOffset);
  cg.xorReg(Rax, Rax);  // the scanned length
  cg.bind(scan);
  cg.cmpReg(Rax, R9);
  cg.jcc(Jcc::Ae, scanned);
  cg.movReg(R11, R10);
  cg.addReg(R11, Rax);
  cg.movzxFromMem(R11, R11, 0);
  cg.testReg(R11);
  cg.jcc(Jcc::Eq, scanned);
  cg.addImm32(Rax, 1);
  cg.jmp(scan);
  cg.bind(scanned);
  cg.retInsn();

  cg.bind(opaque);
  cg.movReg(Rax, R9);
  cg.retInsn();
}

// Loads `args[index]` as a live pointer handle into `dst`, reporting the type
// error or the use-after-`free` the hosted `wantPtr` reports. The argument
// window grows downward (slot 0 at +0, slot 1 at -16, ...). A wrong type reports
// `msgType + ", got '" + <typeName> + "'"`, exactly as the hosted `needPtr`
// does; the type path is emitted after the live one and never falls through.
void emitPtrArgCheck(CodeGen& cg, X64Reg args, std::int64_t index, X64Reg dst,
                     CodeGen::Label fail, const DataBlob& msgType,
                     const DataBlob& msgFreed) {
  CodeGen::Label isPtr = cg.newLabel();
  CodeGen::Label live = cg.newLabel();
  CodeGen::Label badType = cg.newLabel();
  CodeGen::Label afterBad = cg.newLabel();
  const std::int64_t offset = -16 * index;

  cg.movFromMem(Rax, args, offset);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagPtr));
  cg.jcc(Jcc::Eq, isPtr);
  cg.jmp(badType);

  cg.bind(isPtr);
  cg.movFromMem(dst, args, offset + 8);
  cg.movFromMem(Rax, dst, kPtrReleasedOffset);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, live);
  emitFailCall(cg, fail, msgFreed);

  cg.bind(live);
  cg.jmp(afterBad);

  cg.bind(badType);
  cg.movFromMem(Rdx, args, offset + 8);  // the payload, for a pointer kind
  emitFailGotType(cg, cg.newLabel(), gMsgText.failDynamic, gMsgText.sbNew,
                  gMsgText.sbAppend, msgType, *gMsgText.textGot,
                  *gMsgText.quoteClose, *gMsgText.names, Rax, Rdx);
  cg.bind(afterBad);
}

// `cstring(text)`: a `CString` handle owning the text plus its NUL terminator.
void emitCStringBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label ptrNew,
                        CodeGen::Label fail, const DataBlob& msgArgc,
                        const DataBlob& msgType, const DataBlob& msgOom) {
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isText = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movFromMem(Rdx, Rdi, 8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isText);
  emitTypeTailFail(cg, msgType, Rax, Rdx);

  cg.bind(isText);
  cg.movFromMem(R12, Rdi, 8);  // the AotString*
  cg.movImm(Rdi, kPtrKindCString);
  cg.movFromMem(Rsi, R12, 0);
  cg.addImm32(Rsi, 1);  // the text plus the terminator
  cg.callLabel(ptrNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);
  cg.movFromMem(Rdi, Rbx, kPtrDataOffset);
  cg.movReg(Rsi, R12);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rcx, R12, 0);
  cg.cld();
  cg.repMovsb();
  cg.movReg(Rdx, Rbx);
  cg.movImm(Rax, kTagPtr);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);

  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `buffer(size)`: a `Buffer` handle owning `size` zeroed, writable bytes.
void emitBufferBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label ptrNew,
                       CodeGen::Label fail, const DataBlob& msgArgc,
                       const DataBlob& msgType, const DataBlob& msgNegative,
                       const DataBlob& msgOom) {
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isInt = cg.newLabel();
  CodeGen::Label nonNegative = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movFromMem(Rdx, Rdi, 8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, isInt);
  emitTypeTailFail(cg, msgType, Rax, Rdx);

  cg.bind(isInt);
  cg.movFromMem(R12, Rdi, 8);  // size
  cg.testReg(R12);
  cg.jcc(Jcc::Ge, nonNegative);
  emitFailCall(cg, fail, msgNegative);

  cg.bind(nonNegative);
  cg.movImm(Rdi, kPtrKindBuffer);
  cg.movReg(Rsi, R12);
  cg.callLabel(ptrNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagPtr);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);

  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `cstr(handle)`: the handle's bytes as an Oskel string.
void emitCstrBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label strNew,
                     CodeGen::Label ptrTextLen, CodeGen::Label fail,
                     const DataBlob& msgArgc, const DataBlob& msgType,
                     const DataBlob& msgFreed) {
  CodeGen::Label argcOk = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  emitPtrArgCheck(cg, Rdi, 0, Rbx, fail, msgType, msgFreed);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(ptrTextLen);
  cg.movReg(R12, Rax);
  cg.movFromMem(Rdi, Rbx, kPtrDataOffset);
  cg.movReg(Rsi, R12);
  cg.callLabel(strNew);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `byteLen(handle)`: how many bytes the handle owns, as an `int`. A `cstring`
// counts its text and not its terminator (the hosted builtin drops the trailing
// NUL), a buffer counts every byte, and a `void *` owns none.
void emitByteLenBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                        const DataBlob& msgArgc, const DataBlob& msgType,
                        const DataBlob& msgFreed) {
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  emitPtrArgCheck(cg, Rdi, 0, Rbx, fail, msgType, msgFreed);
  cg.movFromMem(Rdx, Rbx, kPtrLengthOffset);
  cg.movFromMem(R8, Rbx, kPtrKindOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, done);  // a `void *` was built with a zero length
  cg.cmpImm32(R8, static_cast<std::int32_t>(kPtrKindCString));
  cg.jcc(Jcc::Ne, done);  // a buffer keeps every byte
  cg.testReg(Rdx);
  cg.jcc(Jcc::Eq, done);
  cg.subRegImm32(Rdx, 1);  // drop the terminator

  cg.bind(done);
  cg.movImm(Rax, kTagInt);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `loadByte(handle, index)`: one byte (0-255) of a `cstring`/`buffer`.
void emitLoadByteBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                         const DataBlob& msgArgc, const DataBlob& msgType,
                         const DataBlob& msgFreed, const DataBlob& msgNeeds,
                         const DataBlob& msgIndexType, const DataBlob& msgRangePrefix,
                         const DataBlob& msgRangeSuffix) {
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label hasBytes = cg.newLabel();
  CodeGen::Label indexOk = cg.newLabel();
  CodeGen::Label indexNonNeg = cg.newLabel();
  CodeGen::Label inRange = cg.newLabel();
  CodeGen::Label loadRangeFail = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  emitPtrArgCheck(cg, Rdi, 0, Rbx, fail, msgType, msgFreed);
  // The hosted `loadByte` checks the index's *type* before it looks at the
  // handle's kind, so `loadByte(cstring("a"), "x")` reports the index, not the
  // buffer. The order is observable through `pcall` and must match.
  cg.movFromMem(Rax, Rdi, -16);  // args[1].tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, indexOk);
  cg.movFromMem(Rdx, Rdi, -8);   // args[1].payload, for the quoted type
  emitFailGotType(cg, cg.newLabel(), gMsgText.failDynamic, gMsgText.sbNew,
                  gMsgText.sbAppend, msgIndexType, *gMsgText.textGot,
                  *gMsgText.quoteClose, *gMsgText.names, Rax, Rdx);

  cg.bind(indexOk);
  cg.movFromMem(R12, Rdi, -8);  // args[1].payload
  cg.movFromMem(Rax, Rbx, kPtrKindOffset);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, hasBytes);  // a `void *` is opaque
  emitFailCall(cg, fail, msgNeeds);

  cg.bind(hasBytes);
  cg.testReg(R12);
  cg.jcc(Jcc::Ge, indexNonNeg);
  cg.jmp(loadRangeFail);

  cg.bind(indexNonNeg);
  cg.movFromMem(Rax, Rbx, kPtrLengthOffset);
  cg.cmpReg(R12, Rax);
  cg.jcc(Jcc::B, inRange);

  cg.bind(loadRangeFail);
  emitFailParts(cg, gMsgText.failDynamic, gMsgText.sbNew, gMsgText.sbAppend,
                {{&msgRangePrefix, Rax}, {nullptr, R12}, {&msgRangeSuffix, Rax}});

  cg.bind(inRange);
  cg.movFromMem(R8, Rbx, kPtrDataOffset);
  cg.addReg(R8, R12);
  cg.movzxFromMem(Rdx, R8, 0);
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `storeByte(handle, index, byte)`: write one byte into a buffer. A `cstring` is
// read-only, like C's `const char *`.
void emitStoreByteBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                          const DataBlob& msgArgc, const DataBlob& msgType,
                          const DataBlob& msgFreed, const DataBlob& msgNeeds,
                          const DataBlob& msgIndexType, const DataBlob& msgByteType,
                          const DataBlob& msgRangePrefix, const DataBlob& msgRangeSuffix,
                          const DataBlob& msgByteRange) {
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label writable = cg.newLabel();
  CodeGen::Label indexOk = cg.newLabel();
  CodeGen::Label indexNonNeg = cg.newLabel();
  CodeGen::Label storeRangeFail = cg.newLabel();
  CodeGen::Label inRange = cg.newLabel();
  CodeGen::Label byteOk = cg.newLabel();
  CodeGen::Label byteNonNeg = cg.newLabel();
  CodeGen::Label byteInRange = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 3);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  emitPtrArgCheck(cg, Rdi, 0, Rbx, fail, msgType, msgFreed);
  // As in `loadByte`, the hosted `storeByte` validates both index and byte
  // before it decides the handle is writable, so the message order matches.
  cg.movFromMem(Rax, Rdi, -16);  // args[1].tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, indexOk);
  cg.movFromMem(Rdx, Rdi, -8);   // args[1].payload, for the quoted type
  emitFailGotType(cg, cg.newLabel(), gMsgText.failDynamic, gMsgText.sbNew,
                  gMsgText.sbAppend, msgIndexType, *gMsgText.textGot,
                  *gMsgText.quoteClose, *gMsgText.names, Rax, Rdx);

  cg.bind(indexOk);
  cg.movFromMem(R12, Rdi, -8);   // args[1].payload: the index
  cg.movFromMem(Rax, Rdi, -32);  // args[2].tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, byteOk);
  cg.movFromMem(Rdx, Rdi, -24);  // args[2].payload, for the quoted type
  emitFailGotType(cg, cg.newLabel(), gMsgText.failDynamic, gMsgText.sbNew,
                  gMsgText.sbAppend, msgByteType, *gMsgText.textGot,
                  *gMsgText.quoteClose, *gMsgText.names, Rax, Rdx);

  cg.bind(byteOk);
  cg.movFromMem(Rax, Rbx, kPtrKindOffset);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kPtrKindBuffer));
  cg.jcc(Jcc::Eq, writable);
  emitFailCall(cg, fail, msgNeeds);

  cg.bind(writable);
  cg.testReg(R12);
  cg.jcc(Jcc::Ge, indexNonNeg);
  cg.jmp(storeRangeFail);

  cg.bind(indexNonNeg);
  cg.movFromMem(Rax, Rbx, kPtrLengthOffset);
  cg.cmpReg(R12, Rax);
  cg.jcc(Jcc::B, inRange);

  cg.bind(storeRangeFail);
  emitFailParts(cg, gMsgText.failDynamic, gMsgText.sbNew, gMsgText.sbAppend,
                {{&msgRangePrefix, Rax}, {nullptr, R12}, {&msgRangeSuffix, Rax}});

  cg.bind(inRange);
  cg.movFromMem(R8, Rdi, -24);  // args[2].payload: the byte
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, byteNonNeg);
  emitFailCall(cg, fail, msgByteRange);

  cg.bind(byteNonNeg);
  cg.cmpImm32(R8, 255);
  cg.jcc(Jcc::Le, byteInRange);
  emitFailCall(cg, fail, msgByteRange);

  cg.bind(byteInRange);
  cg.movFromMem(R9, Rbx, kPtrDataOffset);
  cg.addReg(R9, R12);
  cg.movByteToMem(R9, 0, R8);
  cg.xorReg(Rax, Rax);  // nil
  cg.xorReg(Rdx, Rdx);

  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `intToPtr(token)`: wrap an opaque integer in a `Void` handle.
void emitIntToPtrBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label ptrNew,
                         CodeGen::Label fail, const DataBlob& msgArgc,
                         const DataBlob& msgType, const DataBlob& msgOom) {
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isInt = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movFromMem(Rdx, Rdi, 8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, isInt);
  emitTypeTailFail(cg, msgType, Rax, Rdx);

  cg.bind(isInt);
  cg.movFromMem(R12, Rdi, 8);  // the token
  cg.movImm(Rdi, kPtrKindVoid);
  cg.xorReg(Rsi, Rsi);
  cg.callLabel(ptrNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, kPtrDataOffset, R12);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagPtr);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);

  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `ptrToInt(handle)`: read the opaque token back out.
void emitPtrToIntBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                         const DataBlob& msgArgc, const DataBlob& msgType,
                         const DataBlob& msgFreed) {
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label haveToken = cg.newLabel();
  CodeGen::Label notVoid = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  emitPtrArgCheck(cg, Rdi, 0, Rbx, fail, msgType, msgFreed);
  // Only a `Void` handle carries a token (`intToPtr`); a cstring or buffer has
  // no address of its own, and the hosted `ptrToInt` returns 0 for both.
  cg.movFromMem(Rax, Rbx, kPtrKindOffset);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, notVoid);
  cg.movFromMem(Rdx, Rbx, kPtrDataOffset);
  cg.jmp(haveToken);
  cg.bind(notVoid);
  cg.xorReg(Rdx, Rdx);
  cg.bind(haveToken);
  cg.movImm(Rax, kTagInt);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `free(handle)`: release the handle's storage. The arena never reclaims, so
// this only marks the handle released -- a later use is a reported error
// instead of a read of bytes the program believed were gone.
void emitFreeBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                     const DataBlob& msgArgc, const DataBlob& msgType,
                     const DataBlob& msgFreed) {
  CodeGen::Label argcOk = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  emitPtrArgCheck(cg, Rdi, 0, Rbx, fail, msgType, msgFreed);
  cg.movImmToMem(Rbx, kPtrReleasedOffset, 1);
  cg.movImmToMem(Rbx, kPtrLengthOffset, 0);
  cg.xorReg(Rax, Rax);  // nil
  cg.xorReg(Rdx, Rdx);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// Copies the text of the `AotString*` in `src` into the arena and leaves a
// NUL-terminated `const char*` in rax -- the path a file handle takes. Reports
// out of memory through `fail`, and a path with an embedded NUL through the
// same sink: the OS would see only the prefix, and the hosted engine rejects
// such a path for exactly that reason, so truncating here would be a silent
// divergence between the two engines. Inline, because each file builtin needs
// it once and the copy is not hot.
void emitPathCopy(CodeGen& cg, X64Reg src, CodeGen::Label allocLabel,
                  CodeGen::Label fail, const DataBlob& msgOom,
                  const DataBlob& msgNul) {
  CodeGen::Label copied = cg.newLabel();
  CodeGen::Label scan = cg.newLabel();
  CodeGen::Label scanned = cg.newLabel();
  CodeGen::Label nulFound = cg.newLabel();
  CodeGen::Label copiedDone = cg.newLabel();

  cg.movFromMem(R8, src, 0);  // the text length
  cg.movReg(R9, src);
  cg.addImm32(R9, static_cast<std::int32_t>(kStringDataOffset));  // the first byte
  cg.movReg(R10, R9);
  cg.addReg(R10, R8);  // one past the last byte
  cg.bind(scan);
  cg.cmpReg(R9, R10);
  cg.jcc(Jcc::Ae, scanned);
  cg.movzxFromMem(Rax, R9, 0);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, nulFound);  // a NUL: report it and stop before copying
  cg.addImm32(R9, 1);
  cg.jmp(scan);

  cg.bind(scanned);
  cg.movReg(Rdi, R8);   // the text length
  cg.addImm32(Rdi, 1);  // plus the terminator
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, copied);
  emitFailCall(cg, fail, msgOom);  // never returns

  cg.bind(copied);
  cg.movReg(R8, Rax);  // the buffer, kept across the copy
  cg.movReg(Rdi, R8);
  cg.movReg(Rsi, src);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rcx, src, 0);
  cg.cld();
  cg.repMovsb();  // rdi now sits at the terminator slot
  cg.movImmByteToMem(Rdi, 0, 0);
  cg.movReg(Rax, R8);
  cg.jmp(copiedDone);

  cg.bind(nulFound);
  emitFailCall(cg, fail, msgNul);

  cg.bind(copiedDone);
}

// `void oskel_fail_path(AotString* path)`: reports `prefix` + the path + `'`, the
// text the hosted `readFile`/`writeFile` produce ("cannot read file 'x'"). The
// bytes go through the one error sink, so a `pcall` catches them like any other
// message. `prefix` selects which of the two messages is reported.
void emitFailPath(CodeGen& cg, CodeGen::Label label, CodeGen::Label failDynamic,
                  CodeGen::Label sbNew, CodeGen::Label sbAppend,
                  const DataBlob& prefix, const DataBlob& textQuote) {
  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movToMem(Rbp, -24, Rdi);  // the path
  cg.callLabel(sbNew);
  cg.movReg(Rbx, Rax);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, prefix.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(prefix.text.size()));
  cg.callLabel(sbAppend);
  cg.movFromMem(R12, Rbp, -24);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rdx, R12, 0);
  cg.callLabel(sbAppend);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textQuote.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textQuote.text.size()));
  cg.callLabel(sbAppend);
  cg.movFromMem(Rdi, Rbx, kSbDataOffset);
  cg.movFromMem(Rsi, Rbx, kSbLengthOffset);
  cg.callLabel(failDynamic);
  cg.ud2();
}

// `readFile(path)`: the whole file as a string. It is read in 4 KiB chunks into
// a string builder, so a file of any size lands in one arena string and no size
// has to be known up front.
void emitReadFileBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                         const RuntimeHandles& entry, CodeGen::Label sbNew,
                         CodeGen::Label sbAppend, CodeGen::Label failPath,
                         const DataBlob& msgArgc, const DataBlob& msgPathType,
                         const DataBlob& msgNul) {
  constexpr std::int64_t kChunk = 4096;
  constexpr std::int64_t kBuffer = -4144;  // 4096 bytes, below the two locals
  constexpr std::int64_t kFd = -24;
  constexpr std::int64_t kSb = -32;
  constexpr std::int64_t kPath = -40;
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isPath = cg.newLabel();
  CodeGen::Label opened = cg.newLabel();
  CodeGen::Label readLoop = cg.newLabel();
  CodeGen::Label buildString = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 4144);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movFromMem(Rdx, Rdi, 8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isPath);
  emitTypeTailFail(cg, msgPathType, Rax, Rdx);

  cg.bind(isPath);
  cg.movFromMem(R12, Rdi, 8);  // the path AotString*
  cg.movToMem(Rbp, kPath, R12);
  cg.callLabel(sbNew);  // the builder first: its own allocation may fail
  cg.movToMem(Rbp, kSb, Rax);
  emitPathCopy(cg, R12, env.alloc, env.fail, *env.msgOom, msgNul);
  cg.movReg(Rdi, Rax);
  cg.xorReg(Rsi, Rsi);  // mode 0: read
  cg.callLabel(entry.open);
  cg.movToMem(Rbp, kFd, Rax);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, opened);
  cg.movFromMem(Rdi, Rbp, kPath);
  cg.callLabel(failPath);
  cg.ud2();

  cg.bind(opened);
  cg.bind(readLoop);
  cg.movFromMem(Rdi, Rbp, kFd);
  cg.leaMem(Rsi, Rbp, kBuffer);
  cg.movImm(Rdx, kChunk);
  cg.callLabel(entry.readFd);
  cg.testReg(Rax);
  cg.jcc(Jcc::Le, buildString);  // end of file, or a read error: stop
  cg.movReg(R12, Rax);
  cg.movFromMem(Rdi, Rbp, kSb);
  cg.leaMem(Rsi, Rbp, kBuffer);
  cg.movReg(Rdx, R12);
  cg.callLabel(sbAppend);
  cg.jmp(readLoop);

  cg.bind(buildString);
  cg.movFromMem(Rdi, Rbp, kFd);
  cg.callLabel(entry.close);
  cg.movFromMem(Rbx, Rbp, kSb);
  cg.movFromMem(R12, Rbx, kSbLengthOffset);
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, R12);
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rsi, Rbx, kSbDataOffset);
  cg.movReg(Rcx, R12);
  cg.cld();
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);

  cg.bind(done);
  cg.addImm32(Rsp, 4144);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `writeFile(path, contents)`: write the string's bytes to the path, creating or
// truncating it, and return nil.
void emitWriteFileBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                          const RuntimeHandles& entry, CodeGen::Label failPath,
                          const DataBlob& msgArgc, const DataBlob& msgPathType,
                          const DataBlob& msgContentsType, const DataBlob& msgNul) {
  constexpr std::int64_t kFd = -24;
  constexpr std::int64_t kPath = -32;
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isPath = cg.newLabel();
  CodeGen::Label isText = cg.newLabel();
  CodeGen::Label opened = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movFromMem(Rdx, Rdi, 8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isPath);
  emitTypeTailFail(cg, msgPathType, Rax, Rdx);

  cg.bind(isPath);
  cg.movFromMem(R12, Rdi, 8);    // the path AotString*
  cg.movToMem(Rbp, kPath, R12);
  cg.movFromMem(Rax, Rdi, -16);  // args[1].tag
  cg.movFromMem(Rdx, Rdi, -8);   // args[1].payload, for the quoted type
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isText);
  emitTypeTailFail(cg, msgContentsType, Rax, Rdx);

  cg.bind(isText);
  cg.movFromMem(Rbx, Rdi, -8);  // args[1].payload: the contents AotString*
  emitPathCopy(cg, R12, env.alloc, env.fail, *env.msgOom, msgNul);
  cg.movReg(Rdi, Rax);
  cg.movImm(Rsi, 1);  // mode 1: write (create or truncate)
  cg.callLabel(entry.open);
  cg.movToMem(Rbp, kFd, Rax);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, opened);
  cg.movFromMem(Rdi, Rbp, kPath);
  cg.callLabel(failPath);
  cg.ud2();

  cg.bind(opened);
  cg.movFromMem(Rdi, Rbp, kFd);
  cg.movReg(Rsi, Rbx);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rdx, Rbx, 0);
  cg.callLabel(entry.writeFd);
  cg.movFromMem(Rdi, Rbp, kFd);
  cg.callLabel(entry.close);
  cg.xorReg(Rax, Rax);  // nil
  cg.xorReg(Rdx, Rdx);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `fileExists(path)`: true when the path opens for reading.
void emitFileExistsBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                           const RuntimeHandles& entry, const DataBlob& msgArgc,
                           const DataBlob& msgPathType, const DataBlob& msgNul) {
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isPath = cg.newLabel();
  CodeGen::Label missing = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movFromMem(Rdx, Rdi, 8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isPath);
  emitTypeTailFail(cg, msgPathType, Rax, Rdx);

  cg.bind(isPath);
  cg.movFromMem(R12, Rdi, 8);
  emitPathCopy(cg, R12, env.alloc, env.fail, *env.msgOom, msgNul);
  cg.movReg(Rdi, Rax);
  cg.xorReg(Rsi, Rsi);  // mode 0: read
  cg.callLabel(entry.open);
  cg.movReg(Rbx, Rax);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, missing);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(entry.close);
  cg.movImm(Rdx, 1);  // true
  CodeGen::Label done = cg.newLabel();
  cg.jmp(done);

  cg.bind(missing);
  cg.xorReg(Rdx, Rdx);  // false

  cg.bind(done);
  cg.movImm(Rax, kTagBool);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// Validates `args[0]` as a live `Void` handle -- the token `fsOpen` stores --
// and leaves the `AotPtr*` in `dst`. A `cstring`/`buffer` is a handle too, but
// not a file handle, so the kind check is part of the contract the hosted
// `wantFileHandle` enforces.
void emitFsHandleCheck(CodeGen& cg, X64Reg args, X64Reg dst, CodeGen::Label fail,
                       const DataBlob& msgType, const DataBlob& msgFreed) {
  CodeGen::Label kindOk = cg.newLabel();
  emitPtrArgCheck(cg, args, 0, dst, fail, msgType, msgFreed);
  cg.movFromMem(Rax, dst, kPtrKindOffset);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, kindOk);  // kPtrKindVoid == 0
  cg.movImm(Rax, kTagPtr);
  cg.movReg(Rdx, dst);
  emitTypeTailFail(cg, msgType, Rax, Rdx);
  cg.bind(kindOk);
}

// `fsOpen(path, mode)`: open a stream and return a `Void` handle whose data
// word is the host descriptor. The mode string is one character: 'r' -> 0,
// 'w' -> 1, 'a' -> 2, matching the prelude's `oskel_open`.
void emitFsOpenBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label ptrNew,
                       const CollectionEnv& env, const RuntimeHandles& entry,
                       const DataBlob& msgArgc, const DataBlob& msgPathType,
                       const DataBlob& msgModeType, const DataBlob& msgModeBad,
                       const DataBlob& msgOpenFailed, const DataBlob& msgNul) {
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isPath = cg.newLabel();
  CodeGen::Label isMode = cg.newLabel();
  CodeGen::Label modeR = cg.newLabel();
  CodeGen::Label modeW = cg.newLabel();
  CodeGen::Label modeA = cg.newLabel();
  CodeGen::Label modeBad = cg.newLabel();
  CodeGen::Label modeChosen = cg.newLabel();
  CodeGen::Label opened = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movFromMem(Rdx, Rdi, 8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isPath);
  emitTypeTailFail(cg, msgPathType, Rax, Rdx);

  cg.bind(isPath);
  cg.movFromMem(R12, Rdi, 8);  // the path AotString*
  cg.movFromMem(Rax, Rdi, -16);
  cg.movFromMem(Rdx, Rdi, -8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isMode);
  emitTypeTailFail(cg, msgModeType, Rax, Rdx);

  cg.bind(isMode);
  cg.movFromMem(Rbx, Rdi, -8);  // the mode AotString*
  cg.movFromMem(Rax, Rbx, 0);
  cg.cmpImm32(Rax, 1);
  cg.jcc(Jcc::Ne, modeBad);
  cg.movzxFromMem(Rax, Rbx, static_cast<std::int32_t>(kStringDataOffset));
  cg.cmpImm32(Rax, 'r');
  cg.jcc(Jcc::Eq, modeR);
  cg.cmpImm32(Rax, 'w');
  cg.jcc(Jcc::Eq, modeW);
  cg.cmpImm32(Rax, 'a');
  cg.jcc(Jcc::Eq, modeA);
  cg.jmp(modeBad);

  cg.bind(modeR);
  cg.xorReg(Rbx, Rbx);
  cg.jmp(modeChosen);
  cg.bind(modeW);
  cg.movImm(Rbx, 1);
  cg.jmp(modeChosen);
  cg.bind(modeA);
  cg.movImm(Rbx, 2);
  cg.jmp(modeChosen);
  cg.bind(modeBad);
  emitFailCall(cg, env.fail, msgModeBad);
  cg.bind(modeChosen);

  emitPathCopy(cg, R12, env.alloc, env.fail, *env.msgOom, msgNul);
  cg.movReg(Rdi, Rax);
  cg.movReg(Rsi, Rbx);
  cg.callLabel(entry.open);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, opened);
  emitFailCall(cg, env.fail, msgOpenFailed);

  cg.bind(opened);
  cg.movReg(R12, Rax);  // the host descriptor
  cg.movImm(Rdi, kPtrKindVoid);
  cg.xorReg(Rsi, Rsi);
  cg.callLabel(ptrNew);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, kPtrDataOffset, R12);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagPtr);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);

  cg.bind(done);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `fsRead(handle, count)`: up to `count` bytes, or the rest of the file when
// `count` is negative. The bytes go through the shared string builder, so a
// short host read is handled the same way `readFile` handles one.
void emitFsReadBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                       const RuntimeHandles& entry, CodeGen::Label sbNew,
                       CodeGen::Label sbAppend, const DataBlob& msgArgc,
                       const DataBlob& msgHandleType, const DataBlob& msgFreed,
                       const DataBlob& msgCountType) {
  constexpr std::int64_t kSb = -24;
  constexpr std::int64_t kRemaining = -32;
  constexpr std::int64_t kBuffer = -4144;  // 4096 bytes, below the locals
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isCount = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label loopRead = cg.newLabel();
  CodeGen::Label buildString = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 4144);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  emitFsHandleCheck(cg, Rdi, Rbx, env.fail, msgHandleType, msgFreed);
  cg.movFromMem(Rax, Rdi, -16);
  cg.movFromMem(Rdx, Rdi, -8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, isCount);
  emitTypeTailFail(cg, msgCountType, Rax, Rdx);

  cg.bind(isCount);
  cg.movFromMem(R12, Rdi, -8);  // the count
  cg.movToMem(Rbp, kRemaining, R12);
  cg.callLabel(sbNew);
  cg.movToMem(Rbp, kSb, Rax);

  cg.bind(loop);
  cg.movFromMem(R12, Rbp, kRemaining);
  cg.testReg(R12);
  cg.jcc(Jcc::Eq, buildString);  // a zero remaining means the caller is done
  cg.movImm(Rdx, 4096);
  cg.testReg(R12);
  cg.jcc(Jcc::Le, loopRead);  // negative: read until EOF
  cg.cmpReg(R12, Rdx);
  cg.jcc(Jcc::Ae, loopRead);
  cg.movReg(Rdx, R12);  // fewer than a full chunk left
  cg.bind(loopRead);
  // The handle lives in rbx; the buffer and count are the other two arguments.
  cg.movFromMem(Rdi, Rbx, kPtrDataOffset);
  cg.leaMem(Rsi, Rbp, kBuffer);
  cg.callLabel(entry.readFd);
  cg.testReg(Rax);
  cg.jcc(Jcc::Le, buildString);
  cg.movReg(R12, Rax);
  cg.movFromMem(Rdi, Rbp, kSb);
  cg.leaMem(Rsi, Rbp, kBuffer);
  cg.movReg(Rdx, R12);
  cg.callLabel(sbAppend);
  cg.movFromMem(Rax, Rbp, kRemaining);
  cg.testReg(Rax);
  cg.jcc(Jcc::Le, loop);  // negative: no limit to subtract from
  cg.subReg(Rax, R12);
  cg.movToMem(Rbp, kRemaining, Rax);
  cg.jmp(loop);

  cg.bind(buildString);
  cg.movFromMem(Rbx, Rbp, kSb);
  cg.movFromMem(R12, Rbx, kSbLengthOffset);
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, R12);
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rsi, Rbx, kSbDataOffset);
  cg.movReg(Rcx, R12);
  cg.cld();
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);

  cg.bind(done);
  cg.addImm32(Rsp, 4144);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `fsWrite(handle, text)`: write every byte, retrying on a short host write.
// The count returned is the original length, matching the hosted builtin.
void emitFsWriteBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                        const RuntimeHandles& entry, const DataBlob& msgArgc,
                        const DataBlob& msgHandleType, const DataBlob& msgFreed,
                        const DataBlob& msgTextType, const DataBlob& msgWriteFailed) {
  constexpr std::int64_t kTotal = -24;
  constexpr std::int64_t kRemaining = -32;
  constexpr std::int64_t kPtr = -40;
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isText = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  emitFsHandleCheck(cg, Rdi, Rbx, env.fail, msgHandleType, msgFreed);
  cg.movFromMem(Rax, Rdi, -16);
  cg.movFromMem(Rdx, Rdi, -8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isText);
  emitTypeTailFail(cg, msgTextType, Rax, Rdx);

  cg.bind(isText);
  cg.movFromMem(R12, Rdi, -8);  // the text AotString*
  cg.movFromMem(Rax, R12, 0);
  cg.movToMem(Rbp, kTotal, Rax);
  cg.movToMem(Rbp, kRemaining, Rax);
  cg.movReg(Rax, R12);
  cg.addImm32(Rax, static_cast<std::int32_t>(kStringDataOffset));
  cg.movToMem(Rbp, kPtr, Rax);

  cg.bind(loop);
  cg.movFromMem(Rdx, Rbp, kRemaining);
  cg.testReg(Rdx);
  cg.jcc(Jcc::Le, done);
  cg.movFromMem(Rdi, Rbx, kPtrDataOffset);
  cg.movFromMem(Rsi, Rbp, kPtr);
  cg.callLabel(entry.writeFd);
  cg.testReg(Rax);
  cg.jcc(Jcc::Le, done);
  cg.movFromMem(R12, Rbp, kPtr);
  cg.addReg(R12, Rax);
  cg.movToMem(Rbp, kPtr, R12);
  cg.movFromMem(R12, Rbp, kRemaining);
  cg.subReg(R12, Rax);
  cg.movToMem(Rbp, kRemaining, R12);
  cg.jmp(loop);

  cg.bind(done);
  // A short write is a failure: the loop only reaches here after a complete
  // write or after the host reported no progress.
  cg.movFromMem(Rdx, Rbp, kTotal);
  cg.movFromMem(Rax, Rbp, kRemaining);
  CodeGen::Label complete = cg.newLabel();
  cg.testReg(Rax);
  cg.jcc(Jcc::Le, complete);
  emitFailCall(cg, env.fail, msgWriteFailed);
  cg.bind(complete);
  cg.movImm(Rax, kTagInt);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `fsSeek(handle, offset, origin)`: move the position and return it. `origin`
// is the one-letter form `'s'`/`'c'`/`'e'` that `std.fs.File.seek` emits; it
// maps to 0/1/2.
void emitFsSeekBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                       const RuntimeHandles& entry, const DataBlob& msgArgc,
                       const DataBlob& msgHandleType, const DataBlob& msgFreed,
                       const DataBlob& msgOffsetType, const DataBlob& msgOriginType,
                       const DataBlob& msgOriginBad, const DataBlob& msgSeekFailed) {
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isOffset = cg.newLabel();
  CodeGen::Label isOrigin = cg.newLabel();
  CodeGen::Label originStart = cg.newLabel();
  CodeGen::Label originCurrent = cg.newLabel();
  CodeGen::Label originEnd = cg.newLabel();
  CodeGen::Label originBad = cg.newLabel();
  CodeGen::Label originChosen = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.cmpImm32(Rsi, 3);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  emitFsHandleCheck(cg, Rdi, Rbx, env.fail, msgHandleType, msgFreed);
  cg.movFromMem(Rax, Rdi, -16);
  cg.movFromMem(Rdx, Rdi, -8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, isOffset);
  emitTypeTailFail(cg, msgOffsetType, Rax, Rdx);

  cg.bind(isOffset);
  cg.movFromMem(R12, Rdi, -8);  // the offset
  cg.movFromMem(Rax, Rdi, -32);
  cg.movFromMem(Rdx, Rdi, -24);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isOrigin);
  emitTypeTailFail(cg, msgOriginType, Rax, Rdx);

  cg.bind(isOrigin);
  cg.movFromMem(Rax, Rdi, -24);  // the origin AotString*
  cg.movFromMem(Rdx, Rax, 0);
  cg.cmpImm32(Rdx, 1);
  cg.jcc(Jcc::Ne, originBad);
  cg.movzxFromMem(Rdx, Rax, static_cast<std::int32_t>(kStringDataOffset));
  cg.cmpImm32(Rdx, 's');
  cg.jcc(Jcc::Eq, originStart);
  cg.cmpImm32(Rdx, 'c');
  cg.jcc(Jcc::Eq, originCurrent);
  cg.cmpImm32(Rdx, 'e');
  cg.jcc(Jcc::Eq, originEnd);
  cg.jmp(originBad);

  cg.bind(originStart);
  cg.xorReg(R8, R8);
  cg.jmp(originChosen);
  cg.bind(originCurrent);
  cg.movImm(R8, 1);
  cg.jmp(originChosen);
  cg.bind(originEnd);
  cg.movImm(R8, 2);
  cg.jmp(originChosen);
  cg.bind(originBad);
  emitFailCall(cg, env.fail, msgOriginBad);
  cg.bind(originChosen);

  cg.movFromMem(Rdi, Rbx, kPtrDataOffset);
  cg.movReg(Rsi, R12);
  cg.movReg(Rdx, R8);
  cg.callLabel(entry.seek);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, done);
  emitFailCall(cg, env.fail, msgSeekFailed);

  cg.bind(done);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `fsClose(handle)`: close the descriptor and mark the handle released, so a
// later read/write/seek reports the same use-after-close the VM does.
void emitFsCloseBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                        const RuntimeHandles& entry, const DataBlob& msgArgc,
                        const DataBlob& msgHandleType, const DataBlob& msgFreed) {
  CodeGen::Label argcOk = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  // One saved register leaves rsp 8 mod 16; the close thunk calls a C API on
  // Windows, which requires a 16-byte aligned stack at the call.
  cg.subRegImm32(Rsp, 8);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  emitFsHandleCheck(cg, Rdi, Rbx, env.fail, msgHandleType, msgFreed);
  cg.movFromMem(Rdi, Rbx, kPtrDataOffset);
  cg.callLabel(entry.close);
  cg.movImmToMem(Rbx, kPtrReleasedOffset, 1);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.addImm32(Rsp, 8);
  cg.pop(Rbx);
  epilogue(cg);
}

// `fsRemove(path)`, `fsMkdir(path)`, `fsRmdir(path)` and `fsChdir(path)`: one
// string argument, one prelude handle, nil on success. The failure message is a
// fixed prefix, the AOT convention for a tail that would need a per-message
// builder.
void emitFsPathOpBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                         CodeGen::Label handle, const DataBlob& msgArgc,
                         const DataBlob& msgPathType, const DataBlob& msgNul,
                         const DataBlob& msgFail) {
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isPath = cg.newLabel();
  CodeGen::Label ok = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movFromMem(Rdx, Rdi, 8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isPath);
  emitTypeTailFail(cg, msgPathType, Rax, Rdx);

  cg.bind(isPath);
  cg.movFromMem(R12, Rdi, 8);
  emitPathCopy(cg, R12, env.alloc, env.fail, *env.msgOom, msgNul);
  cg.movReg(Rdi, Rax);
  cg.callLabel(handle);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, ok);
  emitFailCall(cg, env.fail, msgFail);

  cg.bind(ok);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `fsRename(from, to)`: two string paths, copied into NUL-terminated buffers
// before the prelude call. The failure text stays a fixed prefix.
void emitFsRenameBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                         CodeGen::Label handle, const DataBlob& msgArgc,
                         const DataBlob& msgSourcePathType,
                         const DataBlob& msgDestinationPathType, const DataBlob& msgNul,
                         const DataBlob& msgFail) {
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label firstOk = cg.newLabel();
  CodeGen::Label secondOk = cg.newLabel();
  CodeGen::Label ok = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movFromMem(Rdx, Rdi, 8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, firstOk);
  emitTypeTailFail(cg, msgSourcePathType, Rax, Rdx);

  cg.bind(firstOk);
  cg.movFromMem(R12, Rdi, 8);
  cg.movFromMem(Rax, Rdi, -16);
  cg.movFromMem(Rdx, Rdi, -8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, secondOk);
  emitTypeTailFail(cg, msgDestinationPathType, Rax, Rdx);

  cg.bind(secondOk);
  cg.movFromMem(Rbx, Rdi, -8);
  emitPathCopy(cg, R12, env.alloc, env.fail, *env.msgOom, msgNul);
  cg.movToMem(Rbp, -24, Rax);
  emitPathCopy(cg, Rbx, env.alloc, env.fail, *env.msgOom, msgNul);
  cg.movReg(Rsi, Rax);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.callLabel(handle);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, ok);
  emitFailCall(cg, env.fail, msgFail);

  cg.bind(ok);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `fsList(path)`: an array of the basenames. The prelude returns one name per
// index (`listEntry`), so the runtime owns no directory handle and the order
// is the host's; `std.fs.dir.listDir` sorts the result.
void emitFsListBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                       const RuntimeHandles& entry, const DataBlob& msgArgc,
                       const DataBlob& msgPathType, const DataBlob& msgNul,
                       const DataBlob& msgFail) {
  constexpr std::int32_t kPath = -24;
  constexpr std::int32_t kArray = -32;
  constexpr std::int32_t kIndex = -40;
  constexpr std::int32_t kString = -56;
  constexpr std::int32_t kBuffer = -4208;
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isPath = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label end = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label listDone = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 4208);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movFromMem(Rdx, Rdi, 8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isPath);
  emitTypeTailFail(cg, msgPathType, Rax, Rdx);

  cg.bind(isPath);
  cg.movFromMem(R12, Rdi, 8);
  emitPathCopy(cg, R12, env.alloc, env.fail, *env.msgOom, msgNul);
  cg.movToMem(Rbp, kPath, Rax);
  cg.xorReg(Rdi, Rdi);  // kind 0: array
  cg.callLabel(env.tableNew);
  cg.movToMem(Rbp, kArray, Rdx);
  cg.movImmToMem(Rbp, kIndex, 0);

  cg.bind(loop);
  cg.movFromMem(Rdi, Rbp, kPath);
  cg.movFromMem(Rsi, Rbp, kIndex);
  cg.leaMem(Rdx, Rbp, kBuffer);
  cg.movImm(Rcx, 4096);
  cg.callLabel(entry.listEntry);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, end);   // -1: failure
  cg.jcc(Jcc::Eq, done);  // 0: end of directory
  cg.leaMem(Rdi, Rbp, kBuffer);
  cg.movReg(Rsi, Rax);
  cg.callLabel(env.strNew);
  cg.movToMem(Rbp, kString, Rdx);
  cg.movImm(Rdi, kTagArray);
  cg.movFromMem(Rsi, Rbp, kArray);
  cg.movImm(Rdx, kTagInt);
  cg.movFromMem(Rcx, Rbp, kIndex);
  cg.movImm(R8, kTagStr);
  cg.movFromMem(R9, Rbp, kString);
  cg.callLabel(env.tableSet);
  cg.movFromMem(Rax, Rbp, kIndex);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbp, kIndex, Rax);
  cg.jmp(loop);

  cg.bind(done);
  cg.movImm(Rax, kTagArray);
  cg.movFromMem(Rdx, Rbp, kArray);
  cg.jmp(listDone);
  cg.bind(end);
  emitFailCall(cg, env.fail, msgFail);
  cg.bind(listDone);
  cg.addImm32(Rsp, 4208);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `fsStat(path)`: a map with `size`, `modified`, `isFile` and `isDir`. The
// prelude fills four 64-bit words; the runtime builds an ordinary map with the
// shared table helpers.
void emitFsStatBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                       const RuntimeHandles& entry, const DataBlob& msgArgc,
                       const DataBlob& msgPathType, const DataBlob& msgNul,
                       const DataBlob& msgFail) {
  constexpr std::int32_t kPath = -24;
  constexpr std::int32_t kOut = -64;   // 32 bytes, -64..-33
  constexpr std::int32_t kMap = -72;
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label isPath = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 96);
  cg.cmpImm32(Rsi, 1);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movFromMem(Rdx, Rdi, 8);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isPath);
  emitTypeTailFail(cg, msgPathType, Rax, Rdx);

  cg.bind(isPath);
  cg.movFromMem(R12, Rdi, 8);
  emitPathCopy(cg, R12, env.alloc, env.fail, *env.msgOom, msgNul);
  cg.movToMem(Rbp, kPath, Rax);
  cg.movReg(Rdi, Rax);
  cg.leaMem(Rsi, Rbp, kOut);
  cg.callLabel(entry.statPath);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, done);
  emitFailCall(cg, env.fail, msgFail);

  cg.bind(done);
  cg.movImm(Rdi, 1);  // kind 1: map
  cg.callLabel(env.tableNew);
  cg.movToMem(Rbp, kMap, Rdx);
  const auto setField = [&](CodeGen::Label key, std::int64_t tag, std::int32_t value) {
    cg.movImm(Rdi, kTagMap);
    cg.movFromMem(Rsi, Rbp, kMap);
    cg.movImm(Rdx, kTagStr);
    cg.leaRipLabel(Rcx, key);
    cg.movImm(R8, tag);
    cg.movFromMem(R9, Rbp, value);
    cg.callLabel(env.tableSet);
  };
  setField(env.sizeConst, kTagInt, kOut);
  setField(env.modifiedConst, kTagInt, kOut + 8);
  setField(env.isFileConst, kTagBool, kOut + 16);
  setField(env.isDirConst, kTagBool, kOut + 24);
  cg.movImm(Rax, kTagMap);
  cg.movFromMem(Rdx, Rbp, kMap);
  cg.addImm32(Rsp, 96);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `fsCwd()`: the current working directory as a string. A 4 KiB buffer matches
// the platform contract's maximum path.
void emitFsCwdBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                      const RuntimeHandles& entry, const DataBlob& msgArgc,
                      const DataBlob& msgFail) {
  constexpr std::int32_t kBuffer = -4160;
  CodeGen::Label argcOk = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 4160);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.leaMem(Rdi, Rbp, kBuffer);
  cg.movImm(Rsi, 4096);
  cg.callLabel(entry.cwd);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, done);
  emitFailCall(cg, env.fail, msgFail);

  cg.bind(done);
  cg.movReg(Rsi, Rax);
  cg.leaMem(Rdi, Rbp, kBuffer);
  cg.callLabel(env.strNew);
  cg.addImm32(Rsp, 4160);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `clock()`: the seconds since the process started, as a `double`. The origin is
// the value the chunk captured from `entry.monotonic` at entry, so the first
// call is not the reference; the difference is scaled from nanoseconds to
// seconds, the unit the hosted `clock()` reports.
void emitClockBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label monotonic,
                      int originSlot, const DataBlob& msgArgc) {
  CodeGen::Label argcOk = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.callLabel(monotonic);  // rax = nanoseconds
  cg.movFromMem(R8, kAotGlobalBase, 16LL * originSlot + 8);
  cg.subReg(Rax, R8);
  cg.cvtsi2sd(Xmm0, Rax);
  cg.movImm(R9, 1000000000);
  // `movqToXmm` copies the *bit pattern*, so it would put the integer
  // 1000000000 in as a denormal (~4.9e-315) and the division would overflow to
  // +inf. The divisor is a value, so it has to be converted, not moved.
  cg.cvtsi2sd(Xmm1, R9);
  cg.divsd(Xmm0, Xmm1);
  cg.movqFromXmm(Rdx, Xmm0);
  cg.movImm(Rax, kTagDouble);
  epilogue(cg);
}

// `args()`: the script arguments as an array of strings, read from the hidden
// global the chunk filled at entry. A program with no arguments gets an empty
// array, never nil.
void emitArgsBuiltin(CodeGen& cg, CodeGen::Label label, int argsSlot,
                     const DataBlob& msgArgc) {
  CodeGen::Label argcOk = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, argcOk);
  emitArityFailCall(cg, msgArgc);

  cg.bind(argcOk);
  cg.movFromMem(Rax, kAotGlobalBase, 16LL * argsSlot);
  cg.movFromMem(Rdx, kAotGlobalBase, 16LL * argsSlot + 8);
  epilogue(cg);
}

// `void oskel_build_args(long argc, char** argv, long slot)`: build the array
// `args()` returns from the process argument vector, skipping `argv[0]` (the
// program itself, which the hosted engine does not report either) and storing it
// in the hidden global `slot`. A NULL slot is skipped rather than fatal, so a
// truncated vector still yields the arguments before it.
void emitBuildArgs(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  constexpr std::int64_t kCount = -24;
  constexpr std::int64_t kArgv = -32;
  constexpr std::int64_t kSlot = -40;
  constexpr std::int64_t kIndex = -48;
  constexpr std::int64_t kBlock = -56;
  constexpr std::int64_t kLength = -64;
  CodeGen::Label empty = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label scan = cg.newLabel();
  CodeGen::Label scanned = cg.newLabel();
  CodeGen::Label skip = cg.newLabel();
  CodeGen::Label store = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 48);
  cg.subRegImm32(Rdi, 1);  // the arguments after argv[0]
  cg.movToMem(Rbp, kCount, Rdi);
  cg.movToMem(Rbp, kArgv, Rsi);
  cg.movToMem(Rbp, kSlot, Rdx);

  cg.movImm(Rdi, static_cast<std::int64_t>(kAotArrayHeaderSize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);  // the header
  cg.movImmToMem(Rbx, kTableLengthOffset, 0);
  cg.movImmToMem(Rbx, kTableCapacityOffset, 0);
  cg.movImmToMem(Rbx, kTableBlockOffset, 0);

  cg.movFromMem(R8, Rbp, kCount);
  cg.testReg(R8);
  cg.jcc(Jcc::Le, empty);  // no arguments: an empty array
  cg.movReg(Rdi, R8);
  cg.shlImm8(Rdi, 4);  // count * sizeof(AotValue)
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbp, kBlock, Rax);
  cg.movFromMem(R8, Rbp, kCount);
  cg.movToMem(Rbx, kTableLengthOffset, R8);
  cg.movToMem(Rbx, kTableCapacityOffset, R8);
  cg.movToMem(Rbx, kTableBlockOffset, Rax);
  cg.movImmToMem(Rbp, kIndex, 0);

  cg.bind(loop);
  cg.movFromMem(R8, Rbp, kIndex);
  cg.movFromMem(R9, Rbp, kCount);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, store);

  // The `char*` of this argument: argv[index + 1].
  cg.movFromMem(R9, Rbp, kArgv);
  cg.addImm32(R8, 1);
  cg.shlImm8(R8, 3);  // (index + 1) * sizeof(char*)
  cg.addReg(R9, R8);
  cg.movFromMem(R12, R9, 0);  // the text
  cg.testReg(R12);
  cg.jcc(Jcc::Eq, skip);

  cg.xorReg(R10, R10);  // strlen, inline
  cg.bind(scan);
  cg.movReg(R11, R12);
  cg.addReg(R11, R10);
  cg.movzxFromMem(R11, R11, 0);
  cg.testReg(R11);
  cg.jcc(Jcc::Eq, scanned);
  cg.addImm32(R10, 1);
  cg.jmp(scan);

  cg.bind(scanned);
  cg.movToMem(Rbp, kLength, R10);
  cg.movReg(Rdi, R10);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(R8, Rbp, kLength);
  cg.movToMem(Rax, 0, R8);
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rsi, R12);
  cg.movReg(Rcx, R8);
  cg.cld();
  cg.repMovsb();
  // block[index] = { Str, the AotString* }
  cg.movFromMem(R9, Rbp, kIndex);
  cg.shlImm8(R9, 4);
  cg.movFromMem(R10, Rbp, kBlock);
  cg.addReg(R10, R9);
  cg.movImmToMem(R10, 0, kTagStr);
  cg.movToMem(R10, 8, Rax);

  cg.bind(skip);
  cg.movFromMem(R8, Rbp, kIndex);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, kIndex, R8);
  cg.jmp(loop);

  cg.bind(empty);
  cg.bind(store);
  cg.movFromMem(R9, Rbp, kSlot);
  cg.shlImm8(R9, 4);
  cg.addReg(R9, kAotGlobalBase);
  cg.movImmToMem(R9, 0, kTagArray);
  cg.movToMem(R9, 8, Rbx);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);

  cg.bind(done);
  cg.addImm32(Rsp, 48);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

void emitAotRuntime(CodeGen& cg, const AotRuntimeHandles& handles,
                    const RuntimeHandles& entry,
                    const std::vector<AotExternThunk>* externThunks, bool hosted,
                    bool hasInput, bool bareMetal, int randomStateSlot,
                    int pcallHandlerSlot, const AotRuntimeExtras& extras,
                    const std::vector<AotCallbackSignature>* callbacks) {
  // Constant text, bound after all code so nothing falls through into it.
  std::vector<DataBlob> data;
  auto addText = [&](const char* text) {
    DataBlob blob{cg.newLabel(), std::string(text)};
    data.push_back(std::move(blob));
    return data.back();
  };
  // A static `AotString`: an 8-byte length followed by the bytes, the layout a
  // `Str` payload points at.
  auto addAotString = [&](const char* text) {
    std::string bytes(8, '\0');
    const std::uint64_t length = std::strlen(text);
    for (int i = 0; i < 8; ++i) {
      bytes[i] = static_cast<char>((length >> (8 * i)) & 0xFF);
    }
    bytes.append(text);
    DataBlob blob{cg.newLabel(), std::move(bytes)};
    data.push_back(std::move(blob));
    return data.back();
  };
  // Every builtin guard below calls the one shared arity-fail body. The handle
  // is created with the rest of the runtime labels so a caller can reference it
  // before this function runs; the guards reach it through this file-scope copy
  // because none of them receives the handles.
  gArityFailLabel = handles.arityFail;
  // Copy the blobs out so later `data.push_back` reallocations cannot
  // invalidate a reference held by an emit helper below.
  DataBlob msgTypes = addText("unsupported operand types");
  DataBlob msgPow = addText("unsupported operand types for '**':");
  DataBlob msgDivZero = addText("division by zero");
  DataBlob msgShift = addText("shift count must be between 0 and 63");
  DataBlob msgBit = addText("unsupported operand types for");
  DataBlob msgAnd = addText(" and");
  DataBlob msgArithAdd = addText("unsupported operand types for '+':");
  DataBlob msgArithSub = addText("unsupported operand types for '-':");
  DataBlob msgArithMul = addText("unsupported operand types for '*':");
  DataBlob msgArithDiv = addText("unsupported operand types for '/':");
  DataBlob msgArithMod = addText("unsupported operand types for '%':");
  DataBlob msgBitAnd = addText("unsupported operand types for '&':");
  DataBlob msgBitOr = addText("unsupported operand types for '|':");
  DataBlob msgBitXor = addText("unsupported operand types for '^':");
  DataBlob msgShiftLeft = addText("unsupported operand types for '<<':");
  DataBlob msgShiftRight = addText("unsupported operand types for '>>':");
  DataBlob msgNeg = addText("unsupported unary operator for type");
  DataBlob msgBitNot = addText("unsupported unary operator for type");
  DataBlob msgCompare = addText("cannot compare");
  DataBlob msgPrint = addText("cannot print this value");
  DataBlob msgOom = addText("out of memory");
  DataBlob msgCallNonFunc = addText("cannot call a value of type");
  DataBlob msgArity = addText("wrong number of arguments");
  DataBlob msgArrayIndexType = addText("array index must be an integer");
  DataBlob msgInsertIndexType = addText("insert() expects an integer index");
  DataBlob msgRemoveAtIndexType = addText("removeAt() expects an integer index");
  DataBlob msgStringIndexType = addText("string index must be an integer");
  DataBlob msgMapKeyType = addText("map keys must be strings");
  DataBlob msgBadIndex = addText("cannot index a value of type");
  DataBlob msgNoMember = addText("type has no member");
  DataBlob msgImmutable = addText("strings are immutable");
  DataBlob msgPopArgc = addText("pop() expects 0 arguments");
  DataBlob msgShiftArgc = addText("shift() expects 0 arguments");
  DataBlob msgUnshiftArgc = addText("unshift() expects 1 argument");
  DataBlob msgInsertArgc = addText("insert() expects 2 arguments");
  DataBlob msgRemoveAtArgc = addText("removeAt() expects 1 argument");
  DataBlob msgContainsArgc = addText("contains() expects 1 argument");
  DataBlob msgIndexOfArgc = addText("indexOf() expects 1 argument");
  DataBlob msgClearArgc = addText("clear() expects 0 arguments");
  DataBlob msgArraySliceArgc = addText("slice() expects between 0 and 2 arguments");
  DataBlob msgArraySliceStart = addText("slice() expects an integer start");
  DataBlob msgArraySliceEnd = addText("slice() expects an integer end");
  DataBlob msgConcatArgc = addText("concat() expects 1 argument");
  DataBlob msgConcatType = addText("concat() expects an array");
  DataBlob msgReversedArgc = addText("reversed() expects 0 arguments");
  DataBlob msgCloneArgc = addText("clone() expects 0 arguments");
  DataBlob msgMapArgc = addText("map() expects 1 argument");
  DataBlob msgFilterArgc = addText("filter() expects 1 argument");
  DataBlob msgEachArgc = addText("each() expects 1 argument");
  DataBlob msgFindArgc = addText("find() expects 1 argument");
  DataBlob msgAnyArgc = addText("any() expects 1 argument");
  DataBlob msgAllArgc = addText("all() expects 1 argument");
  DataBlob msgReduceArgc = addText("reduce() expects between 1 and 2 arguments");
  DataBlob msgMapNotFunction = addText("map() must be a function");
  DataBlob msgFilterNotFunction = addText("filter() must be a function");
  DataBlob msgEachNotFunction = addText("each() must be a function");
  DataBlob msgFindNotFunction = addText("find() must be a function");
  DataBlob msgAnyNotFunction = addText("any() must be a function");
  DataBlob msgAllNotFunction = addText("all() must be a function");
  DataBlob msgReduceNotFunction = addText("reduce() must be a function");
  DataBlob msgReduceEmpty =
      addText("reduce() of an empty array needs an initial value");
  DataBlob msgSumArgc = addText("sum() expects 0 arguments");
  DataBlob msgSumType = addText("sum() expects an array of numbers");
  DataBlob msgJoinArgc = addText("join() expects between 0 and 1 arguments");
  DataBlob msgSortArgc = addText("sort() expects between 0 and 1 arguments");
  DataBlob msgSortedArgc = addText("sorted() expects between 0 and 1 arguments");
  DataBlob msgSortCmp = addText("sort comparator must return a bool");
  DataBlob msgSortedCmp = addText("sorted comparator must return a bool");
  DataBlob msgUpperArgc = addText("upper() expects 0 arguments");
  DataBlob msgLowerArgc = addText("lower() expects 0 arguments");
  DataBlob msgTrimArgc = addText("trim() expects 0 arguments");
  DataBlob msgTrimStartArgc = addText("trimStart() expects 0 arguments");
  DataBlob msgTrimEndArgc = addText("trimEnd() expects 0 arguments");
  DataBlob msgStrContainsArgc = addText("contains() expects 1 argument");
  DataBlob msgStrContainsType = addText("contains() expects a string text");
  DataBlob msgStartsWithArgc = addText("startsWith() expects 1 argument");
  DataBlob msgStartsWithType = addText("startsWith() expects a string prefix");
  DataBlob msgEndsWithArgc = addText("endsWith() expects 1 argument");
  DataBlob msgEndsWithType = addText("endsWith() expects a string suffix");
  DataBlob msgStrIndexOfArgc = addText("indexOf() expects 1 argument");
  DataBlob msgStrIndexOfType = addText("indexOf() expects a string text");
  DataBlob msgSubstrArgc = addText("substr() expects between 1 and 2 arguments");
  DataBlob msgSubstrStartType = addText("substr() expects an integer start");
  DataBlob msgSubstrLengthType = addText("substr() expects an integer length");
  DataBlob msgStrReversedArgc = addText("reversed() expects 0 arguments");
  DataBlob msgRepeatArgc = addText("repeat() expects 1 argument");
  DataBlob msgRepeatType = addText("repeat() expects an integer count");
  DataBlob msgRepeatNegative = addText("repeat() count cannot be negative");
  DataBlob msgRepeatTooLarge = addText("repeat() result is too large");
  DataBlob msgSplitArgc = addText("split() expects between 0 and 1 arguments");
  DataBlob msgSplitType = addText("split() expects a string separator");
  DataBlob msgLinesArgc = addText("lines() expects 0 arguments");
  DataBlob msgCharsArgc = addText("chars() expects 0 arguments");
  DataBlob msgReplaceArgc = addText("replace() expects 2 arguments");
  DataBlob msgReplaceSearchType = addText("replace() expects a string search text");
  DataBlob msgReplaceReplType = addText("replace() expects a string replacement text");
  DataBlob msgReplaceEmpty = addText("replace() search text cannot be empty");
  DataBlob msgReplaceFirstArgc = addText("replaceFirst() expects 2 arguments");
  DataBlob msgReplaceFirstSearchType =
      addText("replaceFirst() expects a string search text");
  DataBlob msgReplaceFirstReplType =
      addText("replaceFirst() expects a string replacement text");
  DataBlob msgReplaceFirstEmpty = addText("replaceFirst() search text cannot be empty");
  DataBlob msgPadStartArgc = addText("padStart() expects between 1 and 2 arguments");
  DataBlob msgPadStartWidthType = addText("padStart() expects an integer width");
  DataBlob msgPadStartFillType = addText("padStart() expects a string fill");
  DataBlob msgPadStartEmpty = addText("padStart() fill cannot be empty");
  DataBlob msgPadStartTooLarge = addText("padStart() result is too large");
  DataBlob msgPadEndArgc = addText("padEnd() expects between 1 and 2 arguments");
  DataBlob msgPadEndWidthType = addText("padEnd() expects an integer width");
  DataBlob msgPadEndFillType = addText("padEnd() expects a string fill");
  DataBlob msgPadEndEmpty = addText("padEnd() fill cannot be empty");
  DataBlob msgPadEndTooLarge = addText("padEnd() result is too large");
  // The bound map methods (PHASE 7c.3g). `clear`/`clone` reuse the array
  // messages above, their text being identical.
  DataBlob msgMapKeysArgc = addText("keys() expects 0 arguments");
  DataBlob msgMapValuesArgc = addText("values() expects 0 arguments");
  DataBlob msgMapEntriesArgc = addText("entries() expects 0 arguments");
  DataBlob msgMapHasArgc = addText("has() expects 1 argument");
  DataBlob msgMapHasKeyType = addText("has() expects a string key");
  DataBlob msgMapGetArgc = addText("get() expects between 1 and 2 arguments");
  DataBlob msgMapGetKeyType = addText("get() expects a string key");
  DataBlob msgMapSetArgc = addText("set() expects 2 arguments");
  DataBlob msgMapSetKeyType = addText("set() expects a string key");
  DataBlob msgMapDeleteArgc = addText("delete() expects 1 argument");
  DataBlob msgMapDeleteKeyType = addText("delete() expects a string key");
  DataBlob msgMapMergeArgc = addText("merge() expects 1 argument");
  DataBlob msgMapMergeType = addText("merge() expects a map");
  DataBlob msgToArrayArgc = addText("toArray() expects 0 arguments");
  DataBlob msgToArrayTooLarge = addText("range is too large to convert to an array");
  DataBlob msgRangeIntType = addText("range membership requires an integer");
  DataBlob msgBadAssign = addText("cannot assign to an index of a value of type");
  DataBlob msgBadField = addText("cannot set a field on a value of type");
  DataBlob msgNoField = addText("class instance has no such field");
  DataBlob msgBadIn = addText("operator 'in' is not supported for");
  DataBlob msgMapMembershipKey = addText("map membership requires a string key");
  DataBlob msgStringMembershipOperand = addText("string membership requires a string operand");
  DataBlob msgMapValue = addText("key/value iteration ('for k, v in ...') requires a map");
  DataBlob msgNotIterable = addText("cannot iterate over a value of type");
  DataBlob msgNonNull = addText("unexpected nil (non-null assertion '!!' failed)");
  DataBlob msgStrBad = addText("cannot render this value to a string");
  DataBlob textTrue = addText("true");
  DataBlob textFalse = addText("false");
  DataBlob textNil = addText("nil");
  DataBlob textNewline = addText("\n");
  DataBlob textSpace = addText(" ");
  DataBlob textFunc = addText("<fn>");
  // A pointer handle with no text (`ptr`, or a buffer) renders as `<ptr>`,
  // exactly like the hosted `writeValue`; a `cstring` renders its bytes.
  DataBlob textPtr = addText("<ptr>");
  DataBlob textComma = addText(", ");
  DataBlob textColon = addText(": ");
  DataBlob textOpenBracket = addText("[");
  DataBlob textCloseBracket = addText("]");
  DataBlob textOpenBrace = addText("{");
  DataBlob textClassOpen = addText(" { ");
  DataBlob textClassClose = addText(" }");
  DataBlob textCloseBrace = addText("}");
  DataBlob textEllipsis = addText("...");
  DataBlob msgRangeBounds = addText("range bounds must be integers");
  DataBlob msgSliceStart = addText("slice start must be an integer");
  DataBlob msgSliceEnd = addText("slice end must be an integer");
  DataBlob msgSliceStepType = addText("slice step must be an integer");
  DataBlob msgSliceStep = addText("slice step must not be zero");
  DataBlob msgSliceType = addText("cannot slice a value of type");
  DataBlob msgSetClassArgc = addText("$setClass() expects 3 arguments");
  DataBlob textQuote = addText("\"");
  DataBlob textEscQuote = addText("\\\"");
  DataBlob textEscBackslash = addText("\\\\");
  DataBlob textEscNL = addText("\\n");
  DataBlob textEscTab = addText("\\t");
  DataBlob textEscCR = addText("\\r");
  DataBlob textHexPrefix = addText("\\x");
  // The public builtins (PHASE 7c.5a): the type-name constants, the two
  // dynamic-failure fragments and the fixed messages.
  DataBlob typeNameNil = addText("nil");
  DataBlob typeNameBool = addText("bool");
  DataBlob typeNameInt = addText("int");
  DataBlob typeNameFloat = addText("float");
  DataBlob typeNameDouble = addText("double");
  DataBlob typeNameStr = addText("string");
  DataBlob typeNameArray = addText("array");
  DataBlob typeNameMap = addText("map");
  DataBlob typeNameFunc = addText("function");
  DataBlob typeNameRange = addText("range");
  DataBlob typeNameError = addText("error");
  // A pointer handle reports `ptr` or `cstring` by its kind (PHASE 7g.2), the
  // same two names the hosted `Value::typeName` returns.
  DataBlob typeNamePtr = addText("ptr");
  DataBlob typeNameCString = addText("cstring");
  DataBlob msgErrorCodeType = addText("error() expects an integer code");
  DataBlob msgErrorArgc = addText("error() expects between 1 and 2 arguments");
  DataBlob textError = addText("error(");
  DataBlob textCloseParen = addText(")");
  DataBlob textOskelPrefix = addText("oskel: ");
  DataBlob msgLenType = addText("len() is not defined for values of type");
  // The arity guard every fixed-arity builtin now has: without it a call with
  // the wrong count read a window that was never written (or ignored the extra
  // argument) while the VM raised "expects 1 argument". The count is appended by
  // `oskel_arity_fail`, so both engines word it identically (PHASE 7c.5b.6).
  DataBlob msgLenArgc = addText("len() expects 1 argument");
  DataBlob msgTypeArgc = addText("type() expects 1 argument");
  DataBlob msgStrArgc = addText("str() expects 1 argument");
  DataBlob msgReprArgc = addText("repr() expects 1 argument");
  DataBlob msgBoolArgc = addText("bool() expects 1 argument");
  DataBlob msgIsErrorArgc = addText("isError() expects 1 argument");
  DataBlob msgOrdArgc = addText("ord() expects 1 argument");
  DataBlob msgChrArgc = addText("chr() expects 1 argument");
  DataBlob msgOrdType = addText("ord() expects a string argument");
  DataBlob msgOrdChar = addText("ord() expects a single character");
  DataBlob msgChrInt = addText("chr() expects an integer argument");
  DataBlob msgChrRange = addText("chr() code point out of range");
  DataBlob msgChrSurrogate = addText("chr() code point is a surrogate");
  DataBlob msgAssert = addText("assertion failed");
  DataBlob msgPanic = addText("panic");
  // The numeric conversions quote the operand, its type and the cause, so their
  // messages are assembled at run time from these fragments (PHASE 7c.5b.2c).
  ConvertMessages convMsg;
  convMsg.prefix = addText("cannot convert '");
  convMsg.midQuote = addText("' to ");
  convMsg.targetInt = addText("int");
  convMsg.targetFloat = addText("float");
  convMsg.targetDouble = addText("double");
  convMsg.outOfRange = addText(" (out of range)");
  convMsg.useInf = addText(" (use inf, -inf or nan)");
  convMsg.emptyInt = addText("cannot convert an empty string to int");
  convMsg.emptyFloat = addText("cannot convert an empty string to float");
  convMsg.emptyDouble = addText("cannot convert an empty string to double");
  convMsg.nonFiniteInt = addText("cannot convert a non-finite float to int");
  convMsg.rangeInt = addText("float is out of the integer range");
  convMsg.argcInt = addText("int() expects 1 argument");
  convMsg.argcFloat = addText("float() expects 1 argument");
  convMsg.argcDouble = addText("double() expects 1 argument");
  TypeNameBlobs convNames;
  convNames.nil = &typeNameNil;
  convNames.boolValue = &typeNameBool;
  convNames.intValue = &typeNameInt;
  convNames.floatValue = &typeNameFloat;
  convNames.doubleValue = &typeNameDouble;
  convNames.str = &typeNameStr;
  convNames.array = &typeNameArray;
  convNames.map = &typeNameMap;
  convNames.func = &typeNameFunc;
  convNames.range = &typeNameRange;
  convNames.error = &typeNameError;
  convNames.ptr = &typeNamePtr;
  convNames.cstring = &typeNameCString;
  DataBlob msgFloorType = addText("floor() expects a number argument");
  DataBlob msgCeilType = addText("ceil() expects a number argument");
  DataBlob msgRoundType = addText("round() expects a number argument");
  // A wrong argument *count* is a different failure from a wrong argument
  // *type*, and the hosted engine says so. The guard used to reuse the type
  // blob, reporting "expects a number argument" for `floor()` (BACKLOG §6).
  DataBlob msgFloorArgc = addText("floor() expects 1 argument");
  DataBlob msgCeilArgc = addText("ceil() expects 1 argument");
  DataBlob msgRoundArgc = addText("round() expects 1 argument");
  // The curated `sqrt` stub (PHASE 7f.1). The messages mirror the hosted
  // builtin's, which the CLI prefixes with `file:line:col: runtime error: `.
  DataBlob msgSqrtArgc = addText("sqrt() expects 1 argument");
  DataBlob msgSqrtType = addText("sqrt() expects a number argument");
  DataBlob msgSqrtNegative = addText("sqrt() of a negative number");
  DataBlob msgExpType = addText("exp() expects a number argument");
  DataBlob msgLogType = addText("log() expects a number argument");
  DataBlob msgLogNonPositive = addText("log() of a non-positive number");
  DataBlob msgPowType = addText("pow() expects a number argument");
  DataBlob msgExpArgc = addText("exp() expects 1 argument");
  DataBlob msgLogArgc = addText("log() expects 1 argument");
  DataBlob msgPowArgc = addText("pow() expects 2 arguments");
  DataBlob msgSinArgc = addText("sin() expects 1 argument");
  DataBlob msgCosArgc = addText("cos() expects 1 argument");
  DataBlob msgTanArgc = addText("tan() expects 1 argument");
  DataBlob msgSinType = addText("sin() expects a number argument");
  DataBlob msgCosType = addText("cos() expects a number argument");
  DataBlob msgTanType = addText("tan() expects a number argument");
  // The remaining public builtins (PHASE 7c.5b.5): `range`, `assertEq` and
  // `exit`. `assertEq` reuses the shared `failDynamic` and the recursive repr,
  // so its message is assembled at run time from the two fragments below.
  DataBlob msgRangeFnArgc = addText("range() expects between 1 and 2 arguments");
  DataBlob msgRangeFnStart = addText("range() expects an integer start");
  DataBlob msgRangeFnEnd = addText("range() expects an integer end");
  DataBlob msgAssertEqArgc = addText("assertEq() expects between 2 and 3 arguments");
  DataBlob msgAssertEqDefault = addText("assertion failed");
  DataBlob textExpected = addText(": expected ");
  DataBlob textGot = addText(", got ");
  DataBlob msgExitArgc = addText("exit() expects between 0 and 1 arguments");
  DataBlob msgExitType = addText("exit() expects an integer argument");
  DataBlob msgTimeArgc = addText("time() expects 0 arguments");
  DataBlob msgInputArgc = addText("input() expects between 0 and 1 arguments");
  DataBlob msgMemSizeArgc = addText("memSize() expects 0 arguments");
  // The pointer/file/clock/argument builtins (BACKLOG §6 item 25). A wrong
  // operand type reports `..., got '<type>'` on both engines: the guard appends
  // the quoted type name at run time (`emitFailGotType`), so the blob below is
  // only the constant prefix the hosted helper also has.
  DataBlob msgCStringArgc = addText("cstring() expects 1 argument");
  DataBlob msgCStringType = addText("cstring() expects a string argument");
  DataBlob msgBufferArgc = addText("buffer() expects 1 argument");
  DataBlob msgBufferType = addText("buffer() expects an integer size");
  DataBlob msgBufferNegative = addText("buffer() size must not be negative");
  DataBlob msgCstrArgc = addText("cstr() expects 1 argument");
  DataBlob msgCstrType = addText("cstr() expects a pointer argument");
  DataBlob msgCstrFreed = addText("cstr() used a pointer after it was freed");
  DataBlob msgByteLenArgc = addText("byteLen() expects 1 argument");
  DataBlob msgByteLenType = addText("byteLen() expects a pointer argument");
  DataBlob msgByteLenFreed = addText("byteLen() used a pointer after it was freed");
  DataBlob msgLoadArgc = addText("loadByte() expects 2 arguments");
  DataBlob msgLoadType = addText("loadByte() expects a pointer value");
  DataBlob msgLoadFreed = addText("loadByte() used a pointer after it was freed");
  DataBlob msgLoadNeeds = addText("loadByte() needs a cstring or buffer");
  DataBlob msgLoadIndexType = addText("loadByte() expects an integer index");
  DataBlob msgLoadRangePrefix = addText("loadByte() index ");
  DataBlob msgLoadRangeSuffix = addText(" is out of range");
  DataBlob msgStoreArgc = addText("storeByte() expects 3 arguments");
  DataBlob msgStoreType = addText("storeByte() expects a pointer value");
  DataBlob msgStoreFreed = addText("storeByte() used a pointer after it was freed");
  DataBlob msgStoreNeeds = addText("storeByte() needs a buffer (a cstring is read-only)");
  DataBlob msgStoreIndexType = addText("storeByte() expects an integer index");
  DataBlob msgStoreByteType = addText("storeByte() expects an integer byte");
  DataBlob msgStoreRangePrefix = addText("storeByte() index ");
  DataBlob msgStoreRangeSuffix = addText(" is out of range");
  DataBlob msgStoreByteRange = addText("storeByte() byte must be in 0..255");
  DataBlob msgIntToPtrArgc = addText("intToPtr() expects 1 argument");
  DataBlob msgIntToPtrType = addText("intToPtr() expects an integer token");
  DataBlob msgPtrToIntArgc = addText("ptrToInt() expects 1 argument");
  DataBlob msgPtrToIntType = addText("ptrToInt() expects a pointer argument");
  DataBlob msgPtrToIntFreed = addText("ptrToInt() used a pointer after it was freed");
  DataBlob msgFreeArgc = addText("free() expects 1 argument");
  DataBlob msgFreeType = addText("free() expects a pointer");
  DataBlob msgFreeFreed = addText("free() used a pointer after it was freed");
  // The tails a message assembled at run time needs: the quoted-type one and
  // the `(length N)` one the collection errors carry.
  DataBlob textGotQuote = addText(", got '");
  DataBlob textTypeQuote = addText(" '");
  DataBlob textTypeOpen = addText("type '");
  DataBlob textHasNoMember = addText("' has no member '");
  DataBlob textWith = addText(" with");
  DataBlob textAnd = addText(" and");
  DataBlob textQuoteClose = addText("'");
  DataBlob textLengthOpen = addText(" (length ");
  DataBlob textLengthClose = addText(")");
  DataBlob msgArrayIndexKind = addText("array index ");
  DataBlob msgStringIndexKind = addText("string index ");
  DataBlob msgIndexOutOfRange = addText(" out of range (length ");
  DataBlob msgInsertRangePrefix = addText("insert() index out of range (length ");
  DataBlob msgRemoveAtRangePrefix = addText("removeAt() index out of range (length ");
  DataBlob msgReadFileArgc = addText("readFile() expects 1 argument");
  DataBlob msgWriteFileArgc = addText("writeFile() expects 2 arguments");
  DataBlob msgFileExistsArgc = addText("fileExists() expects 1 argument");
  DataBlob msgFsOpenArgc = addText("fsOpen() expects 2 arguments");
  DataBlob msgFsOpenPathType = addText("fsOpen() expects a string path");
  DataBlob msgFsOpenModeType = addText("fsOpen() expects a string mode");
  DataBlob msgFsOpenModeBad = addText("fsOpen() mode must be 'r', 'w' or 'a'");
  DataBlob msgFsOpenFailed = addText("cannot open file");
  DataBlob msgFsReadHandleType = addText("fsRead() expects a file handle");
  DataBlob msgFsReadHandleFreed = addText("fsRead() used a closed file handle");
  DataBlob msgFsWriteHandleType = addText("fsWrite() expects a file handle");
  DataBlob msgFsWriteHandleFreed = addText("fsWrite() used a closed file handle");
  DataBlob msgFsSeekHandleType = addText("fsSeek() expects a file handle");
  DataBlob msgFsSeekHandleFreed = addText("fsSeek() used a closed file handle");
  DataBlob msgFsCloseHandleType = addText("fsClose() expects a file handle");
  DataBlob msgFsCloseHandleFreed = addText("fsClose() used a closed file handle");
  DataBlob msgFsReadArgc = addText("fsRead() expects 2 arguments");
  DataBlob msgFsReadCountType = addText("fsRead() expects an integer count");
  DataBlob msgFsWriteArgc = addText("fsWrite() expects 2 arguments");
  DataBlob msgFsWriteTextType = addText("fsWrite() expects a string contents");
  DataBlob msgFsWriteFailed = addText("cannot write file");
  DataBlob msgFsSeekArgc = addText("fsSeek() expects 3 arguments");
  DataBlob msgFsSeekOffsetType = addText("fsSeek() expects an integer offset");
  DataBlob msgFsSeekOriginType = addText("fsSeek() expects a string origin");
  DataBlob msgFsSeekOriginBad =
      addText("fsSeek() origin must be 's', 'c' or 'e'");
  DataBlob msgFsSeekFailed = addText("cannot seek in file");
  DataBlob msgFsCloseArgc = addText("fsClose() expects 1 argument");
  DataBlob msgFsRemoveArgc = addText("fsRemove() expects 1 argument");
  DataBlob msgFsRenameArgc = addText("fsRename() expects 2 arguments");
  DataBlob msgFsMkdirArgc = addText("fsMkdir() expects 1 argument");
  DataBlob msgFsRmdirArgc = addText("fsRmdir() expects 1 argument");
  DataBlob msgFsListArgc = addText("fsList() expects 1 argument");
  DataBlob msgFsStatArgc = addText("fsStat() expects 1 argument");
  DataBlob msgFsCwdArgc = addText("fsCwd() expects 0 arguments");
  DataBlob msgFsChdirArgc = addText("fsChdir() expects 1 argument");
  DataBlob msgFsRemovePathType = addText("fsRemove() expects a string path");
  DataBlob msgFsRenameSourcePathType = addText("fsRename() expects a string source path");
  DataBlob msgFsRenameDestinationPathType =
      addText("fsRename() expects a string destination path");
  DataBlob msgFsMkdirPathType = addText("fsMkdir() expects a string path");
  DataBlob msgFsRmdirPathType = addText("fsRmdir() expects a string path");
  DataBlob msgFsListPathType = addText("fsList() expects a string path");
  DataBlob msgFsStatPathType = addText("fsStat() expects a string path");
  DataBlob msgFsChdirPathType = addText("fsChdir() expects a string path");
  DataBlob msgFsRemoveFailed = addText("cannot remove path");
  DataBlob msgFsRenameFailed = addText("cannot rename path");
  DataBlob msgFsMkdirFailed = addText("cannot create directory");
  DataBlob msgFsRmdirFailed = addText("cannot remove directory");
  DataBlob msgFsListFailed = addText("cannot list directory");
  DataBlob msgFsStatFailed = addText("cannot stat path");
  DataBlob msgFsCwdFailed = addText("cannot read the current directory");
  DataBlob msgFsChdirFailed = addText("cannot change directory");
  DataBlob msgFsNul = addText("path must not contain a NUL byte");
  DataBlob msgClockArgc = addText("clock() expects 0 arguments");
  DataBlob msgArgsArgc = addText("args() expects 0 arguments");
  DataBlob msgReadFileType = addText("readFile() expects a string path");
  DataBlob msgReadFileNul = addText("readFile() path must not contain a NUL byte");
  DataBlob msgWriteFilePathType = addText("writeFile() expects a string path");
  DataBlob msgWriteFileNul = addText("writeFile() path must not contain a NUL byte");
  DataBlob msgWriteFileType = addText("writeFile() expects a string contents");
  DataBlob msgFileExistsType = addText("fileExists() expects a string path");
  DataBlob msgFileExistsNul = addText("fileExists() path must not contain a NUL byte");
  DataBlob textReadPrefix = addText("cannot read file '");
  DataBlob textWritePrefix = addText("cannot write file '");
  DataBlob textApostrophe = addText("'");
  // The `random`/`seed` messages are added only when the program draws, so a
  // program that never calls them keeps a byte-for-byte identical image.
  // The `pcall` argument message is added only when the program calls `pcall`,
  // for the same byte-identity reason as the `random` ones.
  DataBlob msgPcallNotFunc;
  DataBlob msgRandomHigh;
  DataBlob msgRandomBounds;
  DataBlob msgRandomArgc;
  DataBlob msgRandomType;
  DataBlob msgSeedArgc;
  DataBlob msgSeedType;
  if (randomStateSlot >= 0) {
    msgRandomHigh = addText("random() bound must be positive");
    msgRandomArgc = addText("random() expects between 0 and 2 arguments");
    msgRandomType = addText("random() expects an integer argument");
    msgRandomBounds =
        addText("random() needs an upper bound greater than the lower bound");
    msgSeedArgc = addText("seed() expects 1 argument");
    msgSeedType = addText("seed() expects an integer argument");
  }
  if (pcallHandlerSlot >= 0) {
    msgPcallNotFunc = addText("pcall() expects a function as its first argument");
  }
  // The scalar `extern "C"` thunks (PHASE 7f.5). A direct call is arity- and
  // type-checked at compile time, so these only fire through a first-class
  // reference (`var f = abs; f("x")`). The arity message is the shared one; the
  // type message is added only in the hosted mode, so a freestanding image
  // stays byte for byte what it was.
  DataBlob msgExternType;
  if (externThunks != nullptr) {
    msgExternType = addText("argument type does not match the extern \"C\" declaration");
  }
  // The returned-`cstring` scan cap message, hosted only, for the same
  // byte-identity reason.
  DataBlob msgCStringTooLong;
  if (externThunks != nullptr) {
    msgCStringTooLong = addText(
        "a C function returned a 'cstring' longer than 16777216 bytes with no NUL "
        "terminator; the host broke the `const char *` contract");
  }
  // A callback registration that cannot get its executable stub. Hosted only:
  // the freestanding image has no `extern "C"` surface and no `mmap`.
  DataBlob msgCallback;
  if (externThunks != nullptr) {
    msgCallback = addText("could not allocate a callback trampoline");
  }
  // A callback that returns a struct by value must produce a struct (a map).
  // The hosted engine words the two failures with the field name appended; the
  // AOT sink keeps the fixed prefix, like every other dynamic tail.
  DataBlob msgCallbackStruct;
  DataBlob msgCallbackField;
  if (externThunks != nullptr) {
    msgCallbackStruct = addText("a callback returning a struct must return a struct value");
    msgCallbackField = addText("the struct returned by a callback is missing field");
  }
  CodeGen::Label fail = cg.newLabel();
  CodeGen::Label failDynamic = cg.newLabel();
  CodeGen::Label writeInt = cg.newLabel();
  CodeGen::Label printRepr = cg.newLabel();
  CodeGen::Label printStrEsc = cg.newLabel();
  CodeGen::Label sbInit = handles.sbInit;
  // The pointer/file/clock/argument helper labels (BACKLOG §6 item 25). They
  // are only emitted when the target can back them, so an unseeded builtin
  // never references an unbound label.
  CodeGen::Label strNew = cg.newLabel();
  CodeGen::Label ptrNew = cg.newLabel();
  CodeGen::Label voidPtrBox = cg.newLabel();
  CodeGen::Label ptrTextLen = cg.newLabel();
  CodeGen::Label failPath = cg.newLabel();
  CodeGen::Label failPathWrite = cg.newLabel();
  // Bound at the public handles so a test can drive them directly.
  CodeGen::Label sbAppend = handles.sbAppend;
  CodeGen::Label sbAppendByte = cg.newLabel();
  CodeGen::Label sbNew = handles.sbNew;
  // Publish the dynamic-message handles for the guards that assemble their
  // text (`..., got '<type>'`, `... (length N)`). The type vocabulary is the
  // same `convNames` table `type()` reads, so a quoted type cannot drift from
  // what `type()` reports.
  gMsgText.failDynamic = failDynamic;
  gMsgText.sbNew = sbNew;
  gMsgText.sbAppend = sbAppend;
  gMsgText.names = &convNames;
  gMsgText.textGot = &textGotQuote;
  gMsgText.textTypeQuote = &textTypeQuote;
  gMsgText.textTypeOpen = &textTypeOpen;
  gMsgText.textHasNoMember = &textHasNoMember;
  gMsgText.textWith = &textWith;
  gMsgText.textAnd = &textAnd;
  gMsgText.quoteClose = &textQuoteClose;
  CodeGen::Label sbAppendEscaped = cg.newLabel();
  CodeGen::Label sbAppendEscapedBytes = cg.newLabel();
  CodeGen::Label reprInto = cg.newLabel();
  CodeGen::Label reprToString = cg.newLabel();
  // The `pcall` result keys, raw `AotString`s laid out with the other data
  // islands; only referenced when the program calls `pcall`.
  CodeGen::Label okConst = cg.newLabel();
  CodeGen::Label valueConst = cg.newLabel();
  CodeGen::Label errorConst = cg.newLabel();

  // The float/double formatter is part of the embedded runtime: bind its
  // handles and emit the whole dtoa routine so `oskel_print_value` can call it.
  dtoa::DtoaHandles dtoaHandles = dtoa::newDtoaHandles(cg);
  parse_float::ParseFloatHandles parseFloatHandles = parse_float::newParseFloatHandles(cg);

  // Collection helpers reference one another and the printers, so the env is
  // built from the handles and the labels above before any of them is emitted.
  CollectionEnv env{};
  env.alloc = handles.alloc;
  env.fail = fail;
  env.write = entry.write;
  env.writeInt = writeInt;
  env.formatDouble = dtoaHandles.formatDouble;
  env.strEq = handles.strEq;
  env.strHash = cg.newLabel();
  env.mapFind = cg.newLabel();
  env.mapIndexBuild = cg.newLabel();
  env.mapIndexFill = cg.newLabel();
  env.mapIndexInsert = cg.newLabel();
  env.blockEnsure = cg.newLabel();
  env.utf8Len = cg.newLabel();
  env.utf8CharAt = cg.newLabel();
  env.compare = handles.compareValues;
  env.contains = handles.contains;
  env.call = handles.call;
  env.truthy = handles.truthy;
  env.rawPrint = handles.printValueRaw;
  env.printRepr = printRepr;
  env.printStrEsc = printStrEsc;
  env.lengthConst = cg.newLabel();
  env.tableNew = handles.tableNew;
  env.tableSet = handles.tableSet;
  env.sizeConst = cg.newLabel();
  env.modifiedConst = cg.newLabel();
  env.isFileConst = cg.newLabel();
  env.isDirConst = cg.newLabel();
  env.bindArrayMethod = handles.bindArrayMethod;
  // One display island per bound method, in the same order `member_get`
  // assigns selectors, so `repr(a.push)` reads `"<native fn array.push>"`.
  const auto nameDisplay = [](const char* owner, const char* method) {
    return std::string("<native fn ") + owner + "." + method + ">";
  };
  for (const char* method : {"push", "pop", "shift", "unshift", "insert",
                             "removeAt", "contains", "indexOf", "clear",
                             "slice", "concat", "reversed", "clone", "map",
                             "filter", "each", "find", "any", "all", "reduce",
                             "sum", "join", "sort", "sorted"}) {
    env.arrayMethodDisplays.push_back(cg.newLabel());
    env.arrayDisplayTexts.emplace_back(nameDisplay("array", method));
  }
  for (const char* method : {"keys", "values", "entries", "has", "get", "set",
                             "delete", "clear", "merge", "clone"}) {
    env.mapMethodDisplays.push_back(cg.newLabel());
    env.mapDisplayTexts.emplace_back(nameDisplay("map", method));
  }
  for (const char* method : {"upper", "lower", "trim", "trimStart", "trimEnd",
                             "contains", "startsWith", "endsWith", "indexOf",
                             "substr", "reversed", "repeat", "split", "lines",
                             "chars", "replace", "replaceFirst", "padStart",
                             "padEnd"}) {
    env.stringMethodDisplays.push_back(cg.newLabel());
    env.stringDisplayTexts.emplace_back(nameDisplay("string", method));
  }
  for (const char* method : {"toArray", "contains"}) {
    env.rangeMethodDisplays.push_back(cg.newLabel());
    env.rangeDisplayTexts.emplace_back(nameDisplay("range", method));
  }
  env.pushMethod = handles.arrayPushMethod;
  env.popMethod = handles.arrayPopMethod;
  env.shiftMethod = handles.arrayShiftMethod;
  env.unshiftMethod = handles.arrayUnshiftMethod;
  env.insertMethod = handles.arrayInsertMethod;
  env.removeAtMethod = handles.arrayRemoveAtMethod;
  env.containsMethod = handles.arrayContainsMethod;
  env.indexOfMethod = handles.arrayIndexOfMethod;
  env.clearMethod = handles.arrayClearMethod;
  env.sliceMethod = handles.arraySliceMethod;
  env.concatMethod = handles.arrayConcatMethod;
  env.reversedMethod = handles.arrayReversedMethod;
  env.cloneMethod = handles.arrayCloneMethod;
  env.pushConst = cg.newLabel();
  env.popConst = cg.newLabel();
  env.shiftConst = cg.newLabel();
  env.unshiftConst = cg.newLabel();
  env.insertConst = cg.newLabel();
  env.removeAtConst = cg.newLabel();
  env.containsConst = cg.newLabel();
  env.indexOfConst = cg.newLabel();
  env.clearConst = cg.newLabel();
  env.sliceConst = cg.newLabel();
  env.concatConst = cg.newLabel();
  env.reversedConst = cg.newLabel();
  env.cloneConst = cg.newLabel();
  env.mapMethod = handles.arrayMapMethod;
  env.filterMethod = handles.arrayFilterMethod;
  env.eachMethod = handles.arrayEachMethod;
  env.findMethod = handles.arrayFindMethod;
  env.anyMethod = handles.arrayAnyMethod;
  env.allMethod = handles.arrayAllMethod;
  env.reduceMethod = handles.arrayReduceMethod;
  env.sumMethod = handles.arraySumMethod;
  env.joinMethod = handles.arrayJoinMethod;
  env.sortMethod = handles.arraySortMethod;
  env.sortedMethod = handles.arraySortedMethod;
  env.concat = handles.concat;
  env.strBuiltin = handles.strBuiltin;
  env.emptyString = cg.newLabel();
  env.mapConst = cg.newLabel();
  env.filterConst = cg.newLabel();
  env.eachConst = cg.newLabel();
  env.findConst = cg.newLabel();
  env.anyConst = cg.newLabel();
  env.allConst = cg.newLabel();
  env.reduceConst = cg.newLabel();
  env.sumConst = cg.newLabel();
  env.joinConst = cg.newLabel();
  env.sortConst = cg.newLabel();
  env.sortedConst = cg.newLabel();
  // The builder and `str_new` labels are local to this function; the handles
  // for them are separate labels nothing emits, so the string methods must
  // reference the ones actually bound below.
  env.strNew = strNew;
  env.sbNew = sbNew;
  env.sbAppend = sbAppend;
  env.sbAppendByte = sbAppendByte;
  env.strFind = cg.newLabel();
  env.strFindFrom = cg.newLabel();
  env.strPrefixEq = cg.newLabel();
  env.strCpCount = cg.newLabel();
  env.strByteAt = cg.newLabel();
  env.isSpace = cg.newLabel();
  env.caseMap = cg.newLabel();
  env.bindStringMethod = handles.bindStringMethod;
  env.stringUpperMethod = handles.stringUpperMethod;
  env.stringLowerMethod = handles.stringLowerMethod;
  env.stringTrimMethod = handles.stringTrimMethod;
  env.stringTrimStartMethod = handles.stringTrimStartMethod;
  env.stringTrimEndMethod = handles.stringTrimEndMethod;
  env.stringContainsMethod = handles.stringContainsMethod;
  env.stringStartsWithMethod = handles.stringStartsWithMethod;
  env.stringEndsWithMethod = handles.stringEndsWithMethod;
  env.stringIndexOfMethod = handles.stringIndexOfMethod;
  env.stringSubstrMethod = handles.stringSubstrMethod;
  env.stringReversedMethod = handles.stringReversedMethod;
  env.stringRepeatMethod = handles.stringRepeatMethod;
  env.stringSplitMethod = handles.stringSplitMethod;
  env.stringLinesMethod = handles.stringLinesMethod;
  env.stringCharsMethod = handles.stringCharsMethod;
  env.stringReplaceMethod = handles.stringReplaceMethod;
  env.stringReplaceFirstMethod = handles.stringReplaceFirstMethod;
  env.stringPadStartMethod = handles.stringPadStartMethod;
  env.stringPadEndMethod = handles.stringPadEndMethod;
  env.upperTable = handles.upperTable;
  env.lowerTable = handles.lowerTable;
  env.upperCount = static_cast<std::int64_t>(kUnicodeUpperRunCount);
  env.lowerCount = static_cast<std::int64_t>(kUnicodeLowerRunCount);
  env.upperConst = cg.newLabel();
  env.lowerConst = cg.newLabel();
  env.trimConst = cg.newLabel();
  env.trimStartConst = cg.newLabel();
  env.trimEndConst = cg.newLabel();
  env.containsStrConst = cg.newLabel();
  env.startsWithConst = cg.newLabel();
  env.endsWithConst = cg.newLabel();
  env.indexOfStrConst = cg.newLabel();
  env.substrConst = cg.newLabel();
  env.reversedStrConst = cg.newLabel();
  env.repeatConst = cg.newLabel();
  env.splitConst = cg.newLabel();
  env.linesConst = cg.newLabel();
  env.charsConst = cg.newLabel();
  env.replaceConst = cg.newLabel();
  env.replaceFirstConst = cg.newLabel();
  env.padStartConst = cg.newLabel();
  env.padEndConst = cg.newLabel();
  env.spaceConst = cg.newLabel();
  env.msgUpperArgc = &msgUpperArgc;
  env.msgLowerArgc = &msgLowerArgc;
  env.msgTrimArgc = &msgTrimArgc;
  env.msgTrimStartArgc = &msgTrimStartArgc;
  env.msgTrimEndArgc = &msgTrimEndArgc;
  env.msgStrContainsArgc = &msgStrContainsArgc;
  env.msgStartsWithArgc = &msgStartsWithArgc;
  env.msgEndsWithArgc = &msgEndsWithArgc;
  env.msgStrIndexOfArgc = &msgStrIndexOfArgc;
  env.msgSubstrArgc = &msgSubstrArgc;
  env.msgStrReversedArgc = &msgStrReversedArgc;
  env.msgRepeatArgc = &msgRepeatArgc;
  env.msgStrArgType = &msgStrContainsType;
  env.msgSubstrStartType = &msgSubstrStartType;
  env.msgSubstrLengthType = &msgSubstrLengthType;
  env.msgRepeatCountType = &msgRepeatType;
  env.msgRepeatNegative = &msgRepeatNegative;
  env.msgRepeatTooLarge = &msgRepeatTooLarge;
  env.msgSplitArgc = &msgSplitArgc;
  env.msgSplitType = &msgSplitType;
  env.msgLinesArgc = &msgLinesArgc;
  env.msgCharsArgc = &msgCharsArgc;
  env.msgReplaceArgc = &msgReplaceArgc;
  env.msgReplaceSearchType = &msgReplaceSearchType;
  env.msgReplaceReplType = &msgReplaceReplType;
  env.msgReplaceEmpty = &msgReplaceEmpty;
  env.msgReplaceFirstArgc = &msgReplaceFirstArgc;
  env.msgReplaceFirstSearchType = &msgReplaceFirstSearchType;
  env.msgReplaceFirstReplType = &msgReplaceFirstReplType;
  env.msgReplaceFirstEmpty = &msgReplaceFirstEmpty;
  env.msgPadStartArgc = &msgPadStartArgc;
  env.msgPadStartWidthType = &msgPadStartWidthType;
  env.msgPadStartFillType = &msgPadStartFillType;
  env.msgPadStartEmpty = &msgPadStartEmpty;
  env.msgPadStartTooLarge = &msgPadStartTooLarge;
  env.msgPadEndArgc = &msgPadEndArgc;
  env.msgPadEndWidthType = &msgPadEndWidthType;
  env.msgPadEndFillType = &msgPadEndFillType;
  env.msgPadEndEmpty = &msgPadEndEmpty;
  env.msgPadEndTooLarge = &msgPadEndTooLarge;
  env.bindMapMethod = handles.bindMapMethod;
  env.mapKeysMethod = handles.mapKeysMethod;
  env.mapValuesMethod = handles.mapValuesMethod;
  env.mapEntriesMethod = handles.mapEntriesMethod;
  env.mapHasMethod = handles.mapHasMethod;
  env.mapGetMethod = handles.mapGetMethod;
  env.mapSetMethod = handles.mapSetMethod;
  env.mapDeleteMethod = handles.mapDeleteMethod;
  env.mapClearMethod = handles.mapClearMethod;
  env.mapMergeMethod = handles.mapMergeMethod;
  env.mapCloneMethod = handles.mapCloneMethod;
  env.keysConst = cg.newLabel();
  env.valuesConst = cg.newLabel();
  env.entriesConst = cg.newLabel();
  env.hasConst = cg.newLabel();
  env.getConst = cg.newLabel();
  env.setConst = cg.newLabel();
  env.deleteConst = cg.newLabel();
  env.mergeConst = cg.newLabel();
  env.msgKeysArgc = &msgMapKeysArgc;
  env.msgValuesArgc = &msgMapValuesArgc;
  env.msgEntriesArgc = &msgMapEntriesArgc;
  env.msgHasArgc = &msgMapHasArgc;
  env.msgHasKeyType = &msgMapHasKeyType;
  env.msgGetArgc = &msgMapGetArgc;
  env.msgGetKeyType = &msgMapGetKeyType;
  env.msgSetArgc = &msgMapSetArgc;
  env.msgSetKeyType = &msgMapSetKeyType;
  env.msgDeleteArgc = &msgMapDeleteArgc;
  env.msgDeleteKeyType = &msgMapDeleteKeyType;
  env.msgMergeArgc = &msgMapMergeArgc;
  env.msgMergeType = &msgMapMergeType;
  env.bindRangeMethod = handles.bindRangeMethod;
  env.rangeToArrayMethod = handles.rangeToArrayMethod;
  env.rangeContainsMethod = handles.rangeContainsMethod;
  env.toArrayConst = cg.newLabel();
  env.msgToArrayArgc = &msgToArrayArgc;
  env.msgToArrayTooLarge = &msgToArrayTooLarge;
  env.msgRangeIntType = &msgRangeIntType;
  env.messageConst = cg.newLabel();
  env.codeConst = cg.newLabel();
  env.msgOom = &msgOom;
  env.msgArrayIndexType = &msgArrayIndexType;
  env.msgStringIndexType = &msgStringIndexType;
  env.msgArrayIndexKind = &msgArrayIndexKind;
  env.msgStringIndexKind = &msgStringIndexKind;
  env.msgIndexOutOfRange = &msgIndexOutOfRange;
  env.msgIndexRangeClose = &textLengthClose;
  env.msgMapKeyType = &msgMapKeyType;
  env.msgBadIndex = &msgBadIndex;
  env.msgNoMember = &msgNoMember;
  env.msgImmutable = &msgImmutable;
  env.msgBadAssign = &msgBadAssign;
  env.msgBadField = &msgBadField;
  env.msgNoField = &msgNoField;
  env.msgBadIn = &msgBadIn;
  env.msgMapMembershipKey = &msgMapMembershipKey;
  env.msgStringMembershipOperand = &msgStringMembershipOperand;
  env.msgMapValue = &msgMapValue;
  env.msgNotIterable = &msgNotIterable;
  env.textTrue = &textTrue;
  env.textFalse = &textFalse;
  env.textNil = &textNil;
  env.textFunc = &textFunc;
  env.textComma = &textComma;
  env.textColon = &textColon;
  env.textOpenBracket = &textOpenBracket;
  env.textCloseBracket = &textCloseBracket;
  env.textOpenBrace = &textOpenBrace;
  env.textCloseBrace = &textCloseBrace;
  env.textEllipsis = &textEllipsis;
  env.textQuote = &textQuote;
  env.textEscQuote = &textEscQuote;
  env.textEscBackslash = &textEscBackslash;
  env.textEscNL = &textEscNL;
  env.textEscTab = &textEscTab;
  env.textEscCR = &textEscCR;
  env.textHexPrefix = &textHexPrefix;

  emitFail(cg, fail, failDynamic);
  emitWriteInt(cg, writeInt, entry.write);
  emitAlloc(cg, handles.alloc, entry.arenaGrow);
  emitConcat(cg, handles.concat, handles.alloc, fail, msgOom);
  emitArrayConcat(cg, handles.arrayConcat, handles.alloc, env.blockEnsure, fail, msgOom);
  emitBindArrayMethod(cg, handles.bindArrayMethod, handles.cellNew, handles.closure,
                      {handles.arrayPushMethod, handles.arrayPopMethod,
                       handles.arrayShiftMethod, handles.arrayUnshiftMethod,
                       handles.arrayInsertMethod, handles.arrayRemoveAtMethod,
                       handles.arrayContainsMethod, handles.arrayIndexOfMethod,
                       handles.arrayClearMethod, handles.arraySliceMethod,
                       handles.arrayConcatMethod, handles.arrayReversedMethod,
                       handles.arrayCloneMethod, handles.arrayMapMethod,
                       handles.arrayFilterMethod, handles.arrayEachMethod,
                       handles.arrayFindMethod, handles.arrayAnyMethod,
                       handles.arrayAllMethod, handles.arrayReduceMethod,
                       handles.arraySumMethod, handles.arrayJoinMethod,
                       handles.arraySortMethod, handles.arraySortedMethod},
                      env.arrayMethodDisplays);
  emitArrayPushMethod(cg, handles.arrayPushMethod, env);
  emitArrayPopMethod(cg, handles.arrayPopMethod, env, msgPopArgc);
  emitArrayShiftMethod(cg, handles.arrayShiftMethod, env, msgShiftArgc);
  emitArrayUnshiftMethod(cg, handles.arrayUnshiftMethod, env, msgUnshiftArgc);
  emitArrayInsertMethod(cg, handles.arrayInsertMethod, env, msgInsertArgc,
                        msgInsertIndexType, msgInsertRangePrefix, textLengthClose);
  emitArrayRemoveAtMethod(cg, handles.arrayRemoveAtMethod, env, msgRemoveAtArgc,
                          msgRemoveAtIndexType, msgRemoveAtRangePrefix, textLengthClose);
  emitArrayContainsMethod(cg, handles.arrayContainsMethod, env, msgContainsArgc);
  emitArrayIndexOfMethod(cg, handles.arrayIndexOfMethod, env, msgIndexOfArgc);
  emitArrayClearMethod(cg, handles.arrayClearMethod, env, msgClearArgc);
  emitArraySliceMethod(cg, handles.arraySliceMethod, env, msgArraySliceArgc,
                       msgArraySliceStart, msgArraySliceEnd);
  emitArrayConcatMethod(cg, handles.arrayConcatMethod, env, msgConcatArgc, msgConcatType);
  emitArrayReversedMethod(cg, handles.arrayReversedMethod, env, msgReversedArgc);
  emitArrayCloneMethod(cg, handles.arrayCloneMethod, env, msgCloneArgc);
  emitArrayCallbackMethod(cg, handles.arrayMapMethod, env, msgMapArgc, msgMapNotFunction,
                          CallbackKind::Map);
  emitArrayCallbackMethod(cg, handles.arrayFilterMethod, env, msgFilterArgc,
                          msgFilterNotFunction, CallbackKind::Filter);
  emitArrayCallbackMethod(cg, handles.arrayEachMethod, env, msgEachArgc, msgEachNotFunction,
                          CallbackKind::Each);
  emitArrayCallbackMethod(cg, handles.arrayFindMethod, env, msgFindArgc, msgFindNotFunction,
                          CallbackKind::Find);
  emitArrayCallbackMethod(cg, handles.arrayAnyMethod, env, msgAnyArgc, msgAnyNotFunction,
                          CallbackKind::Any);
  emitArrayCallbackMethod(cg, handles.arrayAllMethod, env, msgAllArgc, msgAllNotFunction,
                          CallbackKind::All);
  emitArrayReduceMethod(cg, handles.arrayReduceMethod, env, msgReduceArgc, msgReduceEmpty,
                        msgReduceNotFunction);
  emitArraySumMethod(cg, handles.arraySumMethod, env, msgSumArgc, msgSumType);
  emitArrayJoinMethod(cg, handles.arrayJoinMethod, env, msgJoinArgc);
  emitArraySortMethod(cg, handles.arraySortMethod, env, msgSortArgc, msgSortCmp);
  emitArraySortedMethod(cg, handles.arraySortedMethod, env, msgSortedArgc, msgSortedCmp);
  // The bound map methods (PHASE 7c.3g). The binder shares `emitBindArrayMethod`
  // (the mechanism is identical); the ten bodies mirror the hosted `bindMap`.
  emitBindArrayMethod(cg, handles.bindMapMethod, handles.cellNew, handles.closure,
                      {handles.mapKeysMethod, handles.mapValuesMethod,
                       handles.mapEntriesMethod, handles.mapHasMethod,
                       handles.mapGetMethod, handles.mapSetMethod,
                       handles.mapDeleteMethod, handles.mapClearMethod,
                       handles.mapMergeMethod, handles.mapCloneMethod},
                      env.mapMethodDisplays);
  emitMapKeysMethod(cg, handles.mapKeysMethod, env, msgMapKeysArgc);
  emitMapValuesMethod(cg, handles.mapValuesMethod, env, msgMapValuesArgc);
  emitMapEntriesMethod(cg, handles.mapEntriesMethod, env, msgMapEntriesArgc);
  emitMapHasMethod(cg, handles.mapHasMethod, env, msgMapHasArgc, msgMapHasKeyType);
  emitMapGetMethod(cg, handles.mapGetMethod, env, msgMapGetArgc, msgMapGetKeyType);
  emitMapSetMethod(cg, handles.mapSetMethod, env, msgMapSetArgc, msgMapSetKeyType);
  emitMapDeleteMethod(cg, handles.mapDeleteMethod, env, msgMapDeleteArgc,
                      msgMapDeleteKeyType);
  emitMapClearMethod(cg, handles.mapClearMethod, env, msgClearArgc);
  emitMapMergeMethod(cg, handles.mapMergeMethod, env, msgMapMergeArgc, msgMapMergeType);
  emitMapCloneMethod(cg, handles.mapCloneMethod, env, msgCloneArgc);
  // The bound string methods (PHASE 7c.3i). Same binder as the array methods;
  // the bodies mirror the hosted `bindString`.
  emitStrFind(cg, env.strFind);
  emitStrFindFrom(cg, env.strFindFrom);
  emitStrPrefixEq(cg, env.strPrefixEq);
  emitStrCpCount(cg, env.strCpCount);
  emitStrByteAt(cg, env.strByteAt);
  emitIsSpace(cg, env.isSpace);
  emitCaseMap(cg, env.caseMap, env.sbAppendByte);
  emitBindArrayMethod(cg, handles.bindStringMethod, handles.cellNew, handles.closure,
                      {handles.stringUpperMethod, handles.stringLowerMethod,
                       handles.stringTrimMethod, handles.stringTrimStartMethod,
                       handles.stringTrimEndMethod, handles.stringContainsMethod,
                       handles.stringStartsWithMethod, handles.stringEndsWithMethod,
                       handles.stringIndexOfMethod, handles.stringSubstrMethod,
                       handles.stringReversedMethod, handles.stringRepeatMethod,
                       handles.stringSplitMethod, handles.stringLinesMethod,
                       handles.stringCharsMethod, handles.stringReplaceMethod,
                       handles.stringReplaceFirstMethod, handles.stringPadStartMethod,
                       handles.stringPadEndMethod},
                      env.stringMethodDisplays);
  emitStringUpperMethod(cg, handles.stringUpperMethod, env, msgUpperArgc);
  emitStringLowerMethod(cg, handles.stringLowerMethod, env, msgLowerArgc);
  emitStringTrimMethod(cg, handles.stringTrimMethod, env, msgTrimArgc, true, true);
  emitStringTrimMethod(cg, handles.stringTrimStartMethod, env, msgTrimStartArgc, true, false);
  emitStringTrimMethod(cg, handles.stringTrimEndMethod, env, msgTrimEndArgc, false, true);
  emitStringContainsMethod(cg, handles.stringContainsMethod, env, msgStrContainsArgc,
                           msgStrContainsType);
  emitStringStartsWithMethod(cg, handles.stringStartsWithMethod, env, msgStartsWithArgc,
                             msgStartsWithType);
  emitStringEndsWithMethod(cg, handles.stringEndsWithMethod, env, msgEndsWithArgc,
                           msgEndsWithType);
  emitStringIndexOfMethod(cg, handles.stringIndexOfMethod, env, msgStrIndexOfArgc,
                          msgStrIndexOfType);
  emitStringSubstrMethod(cg, handles.stringSubstrMethod, env, msgSubstrArgc,
                         msgSubstrStartType, msgSubstrLengthType);
  emitStringReversedMethod(cg, handles.stringReversedMethod, env, msgStrReversedArgc);
  emitStringRepeatMethod(cg, handles.stringRepeatMethod, env, msgRepeatArgc,
                         msgRepeatType, msgRepeatNegative, msgRepeatTooLarge);
  emitStringSplitMethod(cg, handles.stringSplitMethod, env, msgSplitArgc,
                        msgSplitType);
  emitStringLinesMethod(cg, handles.stringLinesMethod, env, msgLinesArgc);
  emitStringCharsMethod(cg, handles.stringCharsMethod, env, msgCharsArgc);
  emitStringReplaceMethod(cg, handles.stringReplaceMethod, env, msgReplaceArgc,
                          msgReplaceSearchType, msgReplaceReplType, msgReplaceEmpty,
                          false);
  emitStringReplaceFirstMethod(cg, handles.stringReplaceFirstMethod, env,
                               msgReplaceFirstArgc, msgReplaceFirstSearchType,
                               msgReplaceFirstReplType, msgReplaceFirstEmpty);
  emitStringPadMethod(cg, handles.stringPadStartMethod, env, msgPadStartArgc,
                      msgPadStartWidthType, msgPadStartFillType, msgPadStartEmpty,
                      msgPadStartTooLarge, true);
  emitStringPadMethod(cg, handles.stringPadEndMethod, env, msgPadEndArgc,
                      msgPadEndWidthType, msgPadEndFillType, msgPadEndEmpty,
                      msgPadEndTooLarge, false);
  // The bound range methods (PHASE 7c.3l). Same binder as the array and string
  // methods; the bodies mirror the hosted `bindRange`.
  emitBindArrayMethod(cg, handles.bindRangeMethod, handles.cellNew, handles.closure,
                      {handles.rangeToArrayMethod, handles.rangeContainsMethod},
                      env.rangeMethodDisplays);
  emitRangeToArrayMethod(cg, handles.rangeToArrayMethod, env, msgToArrayArgc,
                         msgToArrayTooLarge);
  emitRangeContainsMethod(cg, handles.rangeContainsMethod, env, msgContainsArgc,
                          msgRangeIntType);
  emitInitGlobals(cg, handles.initGlobals, handles.alloc, fail, msgOom);
  emitGlobalSet(cg, handles.globalSet);
  emitGlobalGet(cg, handles.globalGet);
  if (extras.needsGlobalGetChecked) {
    emitGlobalGetChecked(cg, handles.globalGetChecked, failDynamic);
  }
  emitClosure(cg, handles.closure, handles.alloc, fail, msgOom);
  emitCellNew(cg, handles.cellNew, handles.alloc, fail, msgOom);
  emitCall(cg, handles.call, msgCallNonFunc);
  if (externThunks != nullptr) {
    // The box is emitted only for a signature that needs it, so a program with
    // no `ptr`-returning binding and no `ptr` callback parameter keeps its
    // binary byte for byte.
    bool needsVoidPtrBox = false;
    for (const AotExternThunk& thunk : *externThunks) {
      if (thunk.returns == ffi::ReturnClass::Ptr) {
        needsVoidPtrBox = true;
        break;
      }
    }
    if (!needsVoidPtrBox && callbacks != nullptr) {
      for (const AotCallbackSignature& signature : *callbacks) {
        for (const AotCallbackParam& param : signature.params) {
          if (param.cls == ffi::ParamClass::Pointer && param.type != "cstring") {
            needsVoidPtrBox = true;
            break;
          }
        }
        if (needsVoidPtrBox) break;
      }
    }
    if (needsVoidPtrBox) emitVoidPtrBox(cg, voidPtrBox, ptrNew, fail, msgOom);

    // A callback's C function pointer is a runtime stub (the image text is not
    // writable), so the helper and the shared bodies are emitted before the
    // thunks that reference them.
    CodeGen::Label callbackCreate = cg.newLabel();
    if (callbacks != nullptr && !callbacks->empty()) {
      emitCallbackCreate(cg, callbackCreate, handles.alloc, fail, msgOom, msgCallback,
                         extras.callbackStateSlot);
      for (const AotCallbackSignature& signature : *callbacks) {
        emitCallbackBody(cg, signature, handles.call, handles.alloc, handles.memberGet,
                         voidPtrBox, fail, msgOom, msgExternType, msgCStringTooLong,
                         msgCallbackStruct, msgCallbackField);
      }
    }
    for (const AotExternThunk& thunk : *externThunks) {
      emitExternThunk(cg, thunk, fail, handles.alloc, handles.memberGet, handles.tableNew,
                      handles.memberSet, voidPtrBox, msgArity, msgExternType, msgOom,
                      msgCStringTooLong, callbacks, callbackCreate,
                      extras.callbackStateSlot, extras.abi);
    }
  }
  emitPrintStrEsc(cg, printStrEsc, entry.write, textQuote, textEscQuote,
                  textEscBackslash, textEscNL, textEscTab, textEscCR, textHexPrefix);
  // The string builder and the recursive repr are shared by `print` and `$str`:
  // `oskel_repr_into` traverses, `oskel_print_repr` renders into a stack builder
  // and writes, `oskel_repr_to_string` renders into an arena string.
  emitSbInit(cg, sbInit);
  emitSbAppend(cg, sbAppend, handles.alloc, fail, msgOom);
  emitSbAppendByte(cg, sbAppendByte, sbAppend);
  emitSbNew(cg, sbNew, handles.alloc, fail, msgOom);
  // The one wrong-arity body every guard calls. It needs the builder, so it
  // follows `sbNew`/`sbAppend` and precedes the builtins that reference it.
  emitArityFail(cg, gArityFailLabel, failDynamic, sbNew, sbAppend, textGot);
  // The conversions assemble their messages with the builder, so their bodies
  // must follow the helpers they call.
  emitNumericConversionBuiltins(cg, handles.intBuiltin, handles.floatBuiltin,
                                handles.doubleBuiltin, fail, failDynamic,
                                handles.intTextParser, parseFloatHandles.parseDouble,
                                sbNew, sbAppend, msgOom, convMsg, convNames);
  emitSbAppendEscaped(cg, sbAppendEscapedBytes, sbAppend, sbAppendByte, textQuote,
                      textEscQuote, textEscBackslash, textEscNL, textEscTab, textEscCR,
                      /*fromString=*/false);
  emitSbAppendEscaped(cg, sbAppendEscaped, sbAppend, sbAppendByte, textQuote,
                      textEscQuote, textEscBackslash, textEscNL, textEscTab,
                      textEscCR, /*fromString=*/true);
  emitReprInto(cg, reprInto, sbAppend, sbAppendEscaped, sbAppendEscapedBytes,
               dtoaHandles.formatDouble, textTrue, textFalse, textNil, textFunc,
               textComma, textColon, textOpenBracket, textCloseBracket, textOpenBrace,
               textCloseBrace, textClassOpen, textClassClose, textEllipsis, textError,
               textCloseParen, textPtr);
  emitReprToString(cg, reprToString, sbNew, reprInto, handles.alloc, fail, msgOom);
  emitPrintRepr(cg, printRepr, entry.write, sbInit, reprInto);
  emitPrintValue(cg, handles.printValue, entry.write, fail, writeInt,
                 dtoaHandles.formatDouble, printRepr, msgPrint, textTrue, textFalse,
                 textNil, textNewline, textPtr, hosted);
  emitPrintValueRaw(cg, handles.printValueRaw, entry.write, fail, writeInt,
                    dtoaHandles.formatDouble, printRepr, msgPrint, textTrue, textFalse,
                    textNil, textNewline, textPtr, hosted);
  emitPrintValueErrRaw(cg, handles.printValueErrRaw, entry.write, fail, writeInt,
                       dtoaHandles.formatDouble, printRepr, msgPrint, textTrue,
                       textFalse, textNil, textNewline, textPtr, hosted);
  emitPrintBuiltin(cg, handles.printBuiltin, handles.printValueRaw, entry.write,
                   textSpace, textNewline, /*fd=*/1, /*newline=*/true);
  emitPrintBuiltin(cg, handles.writeBuiltin, handles.printValueRaw, entry.write,
                   textSpace, textNewline, /*fd=*/1, /*newline=*/false);
  emitPrintBuiltin(cg, handles.eprintBuiltin, handles.printValueErrRaw, entry.write,
                   textSpace, textNewline, /*fd=*/2, /*newline=*/true);
  emitTruthy(cg, handles.truthy);
  emitArith(cg, handles.arithValues, fail, msgTypes, msgAnd,
            {&msgArithAdd, &msgArithSub, &msgArithMul, &msgArithDiv, &msgArithMod}, msgDivZero,
            handles.concat,
            handles.arrayConcat);
  emitShift(cg, handles.shiftValues, fail, msgShift, msgShiftLeft, msgShiftRight, msgAnd);
  emitBitwise(cg, handles.bitwiseValues, fail, msgBit, msgBitAnd, msgBitOr, msgBitXor, msgAnd);
  emitNegate(cg, handles.negateValue, msgNeg);
  emitBitNot(cg, handles.bitNotValue, msgBitNot);
  emitNot(cg, handles.notValue);
  emitStrHash(cg, env.strHash);
  emitMapIndexBuild(cg, env.mapIndexBuild, env);
  emitMapIndexFill(cg, env.mapIndexFill, env);
  emitMapIndexInsert(cg, env.mapIndexInsert, env);
  emitCompare(cg, handles.compareValues, msgCompare, env.mapFind);
  emitStrEq(cg, handles.strEq);
  emitMapFind(cg, env.mapFind, env);
  emitBlockEnsure(cg, env.blockEnsure, env);
  emitUtf8Len(cg, env.utf8Len);
  emitUtf8CharAt(cg, env.utf8CharAt);
  emitTableNew(cg, handles.tableNew, env);
  emitPackRest(cg, handles.packRest, env);
  emitTableGet(cg, handles.tableGet, env);
  emitMemberGet(cg, handles.memberGet, env);
  emitTableSet(cg, handles.tableSet, env);
  emitMemberSet(cg, handles.memberSet, env);
  emitContains(cg, handles.contains, env);
  emitIterNext(cg, handles.iterNext, env);
  emitIsErrorBuiltin(cg, handles.isErrorBuiltin, msgIsErrorArgc);
  emitErrorBuiltin(cg, handles.errorBuiltin, handles.alloc, fail, handles.strBuiltin,
                   msgOom, msgErrorCodeType, msgErrorArgc);
  emitNonNullBuiltin(cg, handles.nonNullBuiltin, fail, msgNonNull);
  emitStrBuiltin(cg, handles.strBuiltin, handles.alloc, fail,
                 dtoaHandles.formatDouble, reprToString, msgOom, textTrue, textFalse,
                 textNil, textPtr, msgStrBad, msgStrArgc);
  emitRangeBuiltin(cg, handles.rangeBuiltin, handles.alloc, fail, msgOom,
                   msgRangeBounds);
  emitSliceBuiltin(cg, handles.sliceBuiltin, env, msgSliceStart, msgSliceEnd,
                   msgSliceStepType, msgSliceStep, msgSliceType);
  emitSetClassBuiltin(cg, handles.setClassBuiltin, msgSetClassArgc);
  // The pointer/file/clock/argument builtins (BACKLOG §6 item 25). `strNew`,
  // `ptrNew` and `ptrTextLen` are the shared leaves; the builtins below are
  // emitted unconditionally for the pointer ones (every target can back them)
  // and under the target's capability flag for the rest, so a target that does
  // not seed one never emits a body that references an unbound prelude label.
  emitStrNew(cg, strNew, handles.alloc, fail, msgOom);
  emitPtrNew(cg, ptrNew, handles.alloc);
  emitPtrTextLen(cg, ptrTextLen);
  emitCStringBuiltin(cg, handles.cstringBuiltin, ptrNew, fail, msgCStringArgc,
                     msgCStringType, msgOom);
  emitBufferBuiltin(cg, handles.bufferBuiltin, ptrNew, fail, msgBufferArgc,
                    msgBufferType, msgBufferNegative, msgOom);
  emitCstrBuiltin(cg, handles.cstrBuiltin, strNew, ptrTextLen, fail, msgCstrArgc,
                  msgCstrType, msgCstrFreed);
  emitByteLenBuiltin(cg, handles.byteLenBuiltin, fail, msgByteLenArgc, msgByteLenType,
                     msgByteLenFreed);
  emitLoadByteBuiltin(cg, handles.loadByteBuiltin, fail, msgLoadArgc, msgLoadType,
                      msgLoadFreed, msgLoadNeeds, msgLoadIndexType,
                      msgLoadRangePrefix, msgLoadRangeSuffix);
  emitStoreByteBuiltin(cg, handles.storeByteBuiltin, fail, msgStoreArgc, msgStoreType,
                       msgStoreFreed, msgStoreNeeds, msgStoreIndexType, msgStoreByteType,
                       msgStoreRangePrefix, msgStoreRangeSuffix, msgStoreByteRange);
  emitIntToPtrBuiltin(cg, handles.intToPtrBuiltin, ptrNew, fail, msgIntToPtrArgc,
                      msgIntToPtrType, msgOom);
  emitPtrToIntBuiltin(cg, handles.ptrToIntBuiltin, fail, msgPtrToIntArgc,
                      msgPtrToIntType, msgPtrToIntFreed);
  emitFreeBuiltin(cg, handles.freeBuiltin, fail, msgFreeArgc, msgFreeType, msgFreeFreed);
  // `clock()` needs `entry.monotonic`, which every prelude binds; the origin
  // slot is the hidden `$clockOrigin` global the chunk captured at entry.
  if (extras.clockOriginSlot >= 0) {
    emitClockBuiltin(cg, handles.clockBuiltin, entry.monotonic, extras.clockOriginSlot,
                     msgClockArgc);
  }
  // `args()` reads the hidden `$args` global; the chunk filled it at entry from
  // the `(argc, argv)` the prelude handed `main`.
  if (extras.argsSlot >= 0) {
    emitArgsBuiltin(cg, handles.argsBuiltin, extras.argsSlot, msgArgsArgc);
    emitBuildArgs(cg, handles.buildArgs, env);
  }
  // The file builtins are written against the prelude's file handles, so only a
  // target that binds them emits the bodies.
  if (extras.hasFiles) {
    emitFailPath(cg, failPath, failDynamic, sbNew, sbAppend, textReadPrefix,
                 textApostrophe);
    emitFailPath(cg, failPathWrite, failDynamic, sbNew, sbAppend, textWritePrefix,
                 textApostrophe);
    emitReadFileBuiltin(cg, handles.readFileBuiltin, env, entry, sbNew, sbAppend,
                        failPath, msgReadFileArgc, msgReadFileType, msgReadFileNul);
    emitWriteFileBuiltin(cg, handles.writeFileBuiltin, env, entry, failPathWrite,
                         msgWriteFileArgc, msgWriteFilePathType, msgWriteFileType,
                         msgWriteFileNul);
    emitFileExistsBuiltin(cg, handles.fileExistsBuiltin, env, entry, msgFileExistsArgc,
                          msgFileExistsType, msgFileExistsNul);
    emitFsOpenBuiltin(cg, handles.fsOpenBuiltin, ptrNew, env, entry, msgFsOpenArgc,
                      msgFsOpenPathType, msgFsOpenModeType, msgFsOpenModeBad,
                      msgFsOpenFailed, msgFsNul);
    emitFsReadBuiltin(cg, handles.fsReadBuiltin, env, entry, sbNew, sbAppend,
                      msgFsReadArgc, msgFsReadHandleType, msgFsReadHandleFreed,
                      msgFsReadCountType);
    emitFsWriteBuiltin(cg, handles.fsWriteBuiltin, env, entry, msgFsWriteArgc,
                       msgFsWriteHandleType, msgFsWriteHandleFreed,
                       msgFsWriteTextType, msgFsWriteFailed);
    emitFsSeekBuiltin(cg, handles.fsSeekBuiltin, env, entry, msgFsSeekArgc,
                      msgFsSeekHandleType, msgFsSeekHandleFreed,
                      msgFsSeekOffsetType, msgFsSeekOriginType, msgFsSeekOriginBad,
                      msgFsSeekFailed);
    emitFsCloseBuiltin(cg, handles.fsCloseBuiltin, env, entry, msgFsCloseArgc,
                       msgFsCloseHandleType, msgFsCloseHandleFreed);
    emitFsPathOpBuiltin(cg, handles.fsRemoveBuiltin, env, entry.removePath,
                        msgFsRemoveArgc, msgFsRemovePathType, msgFsNul,
                        msgFsRemoveFailed);
    emitFsRenameBuiltin(cg, handles.fsRenameBuiltin, env, entry.renamePath,
                        msgFsRenameArgc, msgFsRenameSourcePathType,
                        msgFsRenameDestinationPathType, msgFsNul,
                        msgFsRenameFailed);
    emitFsPathOpBuiltin(cg, handles.fsMkdirBuiltin, env, entry.mkdirPath,
                        msgFsMkdirArgc, msgFsMkdirPathType, msgFsNul,
                        msgFsMkdirFailed);
    emitFsPathOpBuiltin(cg, handles.fsRmdirBuiltin, env, entry.rmdirPath,
                        msgFsRmdirArgc, msgFsRmdirPathType, msgFsNul,
                        msgFsRmdirFailed);
    emitFsListBuiltin(cg, handles.fsListBuiltin, env, entry, msgFsListArgc,
                      msgFsListPathType, msgFsNul, msgFsListFailed);
    emitFsStatBuiltin(cg, handles.fsStatBuiltin, env, entry, msgFsStatArgc,
                      msgFsStatPathType, msgFsNul, msgFsStatFailed);
    emitFsCwdBuiltin(cg, handles.fsCwdBuiltin, env, entry, msgFsCwdArgc,
                     msgFsCwdFailed);
    emitFsPathOpBuiltin(cg, handles.fsChdirBuiltin, env, entry.chdirPath,
                        msgFsChdirArgc, msgFsChdirPathType, msgFsNul,
                        msgFsChdirFailed);
  }

  // The public builtins (PHASE 7c.5a). `str` and `isError` reuse the `$str` and
  // `$isError` code: the public names are just two more seeds of the same entry.
  DebugSink debugSink;
  const bool wantsDebug =
      extras.sourcePath != nullptr && extras.sourcePath[0] != '\0' && extras.sourceLineSlot >= 0;
  if (wantsDebug) {
    debugSink.lineSlot = extras.sourceLineSlot;
    debugSink.path = addText(extras.sourcePath);
    debugSink.separator = addText(":");
    debugSink.tail = addText(": ");
  }
  emitFailDynamic(cg, failDynamic, entry.write, entry.exit, textOskelPrefix, textNewline,
                  pcallHandlerSlot, wantsDebug ? &debugSink : nullptr);
  emitLenBuiltin(cg, handles.lenBuiltin, env, msgLenType, msgLenArgc);
  emitTypeBuiltin(cg, handles.typeBuiltin, handles.alloc, fail, msgOom, typeNameNil,
                  typeNameBool, typeNameInt, typeNameFloat, typeNameDouble, typeNameStr,
                  typeNameArray, typeNameMap, typeNameFunc, typeNameRange, typeNameError,
                  typeNamePtr, typeNameCString, msgTypeArgc);
  emitReprBuiltin(cg, handles.reprBuiltin, reprToString, msgReprArgc);
  emitBoolBuiltin(cg, handles.boolBuiltin, handles.truthy, msgBoolArgc);
  emitOrdBuiltin(cg, handles.ordBuiltin, env.utf8Len, fail, msgOrdType, msgOrdChar,
                 msgOrdArgc);
  emitChrBuiltin(cg, handles.chrBuiltin, handles.alloc, fail, msgChrInt, msgChrRange,
                 msgChrSurrogate, msgOom, msgChrArgc);
  emitAssertBuiltin(cg, handles.assertBuiltin, handles.truthy, fail, failDynamic,
                    msgAssert);
  emitPanicBuiltin(cg, handles.panicBuiltin, fail, failDynamic, msgPanic);
  emitRoundingBuiltin(cg, handles.floorBuiltin, msgFloorType, msgFloorArgc,
                      AotRoundMode::Floor);
  emitRoundingBuiltin(cg, handles.ceilBuiltin, msgCeilType, msgCeilArgc,
                      AotRoundMode::Ceil);
  emitRoundingBuiltin(cg, handles.roundBuiltin, msgRoundType, msgRoundArgc,
                      AotRoundMode::Round);
  emitSqrtBuiltin(cg, handles.sqrtBuiltin, fail, msgSqrtArgc, msgSqrtType,
                  msgSqrtNegative);
  // The transcendental builtins (PHASE 7c.5b.4a). The three routines share one
  // set of tables; `mathlib::emitMathLib` emits them plus the wrappers' targets.
  mathlib::MathHandles math = mathlib::newMathHandles(cg);
  mathlib::emitMathLib(cg, math);
  // `**` shares the embedded `pow` with the `pow` builtin.
  emitPow(cg, handles.powValues, fail, msgPow, msgAnd, math.pow);
  emitTranscendentalBuiltin(cg, handles.expBuiltin, fail, math.exp, msgExpType,
                            msgExpArgc, false, msgLogNonPositive);
  emitTranscendentalBuiltin(cg, handles.logBuiltin, fail, math.log, msgLogType,
                            msgLogArgc, true, msgLogNonPositive);
  emitPowBuiltin(cg, handles.powBuiltin, math.pow, msgPowType, msgPowArgc);
  // The trigonometric builtins (PHASE 7c.5b.4b). `sin`/`cos`/`tan` share the
  // `__rem_pio2` reduction and the `__sin`/`__cos` kernels; `emitMathLib` already
  // emitted them above.
  emitTranscendentalBuiltin(cg, handles.sinBuiltin, fail, math.sin, msgSinType,
                            msgSinArgc, false, msgLogNonPositive);
  emitTranscendentalBuiltin(cg, handles.cosBuiltin, fail, math.cos, msgCosType,
                            msgCosArgc, false, msgLogNonPositive);
  emitTranscendentalBuiltin(cg, handles.tanBuiltin, fail, math.tan, msgTanType,
                            msgTanArgc, false, msgLogNonPositive);
  // The remaining public builtins (PHASE 7c.5b.5): `range`, `assertEq` and
  // `exit`. `range` reuses the arena allocator; `assertEq` reuses the shared
  // comparison, the recursive repr and the string builder; `exit` leaves
  // through the entry prelude.
  emitRangeFnBuiltin(cg, handles.rangeFnBuiltin, handles.alloc, fail, msgRangeFnArgc,
                     msgRangeFnStart, msgRangeFnEnd, msgOom);
  emitAssertEqBuiltin(cg, handles.assertEqBuiltin, handles.compareValues, reprToString,
                      sbNew, sbAppend, failDynamic, fail, msgAssertEqArgc,
                      msgAssertEqDefault, textExpected, textGot, msgOom);
  emitExitBuiltin(cg, handles.exitBuiltin, entry.exit, msgExitArgc, msgExitType);
  emitTimeBuiltin(cg, handles.timeBuiltin, entry.now, msgTimeArgc);
  // `platform()`/`arch()`: the target names are compile-time constants, so the
  // body only has to return a static `AotString`. They are seeded on every
  // target, like `time()`.
  {
    DataBlob msgPlatformArgc = addText("platform() expects 0 arguments");
    DataBlob msgArchArgc = addText("arch() expects 0 arguments");
    DataBlob platformText = addAotString(extras.platformName != nullptr
                                             ? extras.platformName
                                             : "unknown");
    DataBlob archText = addAotString(extras.archName != nullptr ? extras.archName
                                                                : "unknown");
    emitConstStringBuiltin(cg, handles.platformBuiltin, platformText.label,
                           msgPlatformArgc);
    emitConstStringBuiltin(cg, handles.archBuiltin, archText.label, msgArchArgc);
  }
  // `input` is the bare-metal keyboard builtin: only the prelude that binds
  // `entry.read` may reference it (see `emitAotRuntime`'s `hasInput`).
  if (hasInput) {
    emitInputBuiltin(cg, handles.inputBuiltin, entry.read, handles.printValueRaw,
                     handles.alloc, fail, msgInputArgc, msgOom);
  }
  // `memSize()` is bare metal only: the handle comes from the multiboot map, and
  // `lower.cpp` seeds the builtin only for `Os::Freestanding`, so no other target
  // references the label.
  if (bareMetal) {
    emitMemSizeBuiltin(cg, handles.memSizeBuiltin, entry.memSize, msgMemSizeArgc);
  }
  // `random()`/`seed()` (PHASE 7c.5b.6): the mt19937_64 generator and the two
  // reduction formulas, so a native binary draws the VM's sequence. `stateSlot`
  // is the hidden `$randomState` global's index, resolved by `lower.cpp`.
  if (randomStateSlot >= 0) {
    CodeGen::Label twistLabel = cg.newLabel();
    CodeGen::Label nextLabel = cg.newLabel();
    CodeGen::Label seedLabel = cg.newLabel();
    emitRandomSeed(cg, seedLabel);
    emitRandomTwist(cg, twistLabel);
    emitRandomNext(cg, nextLabel, twistLabel);
    emitRandomState(cg, handles.randomStateInit, handles.alloc, seedLabel, fail, msgOom);
    // The state helper takes the slot in rdi, so wrap it in a thunk that the
    // builtins can `call` with no arguments: `random`/`seed` call this label to
    // obtain the pointer. It reads the slot from a constant.
    CodeGen::Label stateThunk = cg.newLabel();
    cg.bind(stateThunk);
    // A real frame: at a call site rsp is 16-byte aligned, so the thunk starts
    // at 8 (mod 16) and must re-align before it calls out.
    cg.push(Rbp);
    cg.movReg(Rbp, Rsp);
    cg.movImm(X64Reg::Rdi, randomStateSlot);
    cg.callLabel(handles.randomStateInit);
    cg.movReg(Rsp, Rbp);
    cg.pop(Rbp);
    cg.retInsn();
    emitRandomBuiltin(cg, handles.randomBuiltin, nextLabel, stateThunk, fail,
                      msgRandomHigh, msgRandomBounds, msgRandomArgc, msgRandomType);
    emitSeedBuiltin(cg, handles.seedBuiltin, stateThunk, seedLabel, msgSeedArgc,
                    msgSeedType);
  }
  // `pcall(fn, args...)` (BACKLOG §6 item 4). Emitted only when the program
  // calls it: the body installs a handler the error sink jumps back into, and
  // the sink only carries that check when `pcallHandlerSlot` is set.
  if (pcallHandlerSlot >= 0) {
    emitPcallBuiltin(cg, handles.pcallBuiltin, handles.call, handles.tableNew,
                     handles.memberSet, handles.alloc, fail, failDynamic, msgPcallNotFunc,
                     msgOom, okConst, valueConst, errorConst, pcallHandlerSlot);
  }
  emitIntTextParser(cg, handles.intTextParser);
  dtoa::emitDtoa(cg, dtoaHandles);
  parse_float::emitParseFloat(cg, parseFloatHandles, dtoaHandles);

  // The "length" member name is a raw `AotString` (8-byte length + bytes), laid
  // out with the other data islands so no code falls through into it.
  cg.bind(env.lengthConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x06\x00\x00\x00\x00\x00\x00\x00length"), 14);
  // The method member names, the same raw `AotString` layout: an 8-byte length
  // then the bytes.
  cg.bind(env.pushConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x04\x00\x00\x00\x00\x00\x00\x00push"), 12);
  cg.bind(env.popConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x03\x00\x00\x00\x00\x00\x00\x00pop"), 11);
  cg.bind(env.shiftConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x05\x00\x00\x00\x00\x00\x00\x00shift"), 13);
  cg.bind(env.unshiftConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x07\x00\x00\x00\x00\x00\x00\x00unshift"), 15);
  cg.bind(env.insertConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x06\x00\x00\x00\x00\x00\x00\x00insert"), 14);
  cg.bind(env.removeAtConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x08\x00\x00\x00\x00\x00\x00\x00removeAt"), 16);
  cg.bind(env.containsConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x08\x00\x00\x00\x00\x00\x00\x00" "contains"), 16);
  cg.bind(env.indexOfConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x07\x00\x00\x00\x00\x00\x00\x00indexOf"), 15);
  cg.bind(env.clearConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x05\x00\x00\x00\x00\x00\x00\x00" "clear"), 13);
  cg.bind(env.sliceConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x05\x00\x00\x00\x00\x00\x00\x00" "slice"), 13);
  cg.bind(env.concatConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x06\x00\x00\x00\x00\x00\x00\x00" "concat"), 14);
  cg.bind(env.reversedConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x08\x00\x00\x00\x00\x00\x00\x00" "reversed"), 16);
  cg.bind(env.cloneConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x05\x00\x00\x00\x00\x00\x00\x00" "clone"), 13);
  cg.bind(env.mapConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x03\x00\x00\x00\x00\x00\x00\x00" "map"), 11);
  cg.bind(env.filterConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x06\x00\x00\x00\x00\x00\x00\x00" "filter"), 14);
  cg.bind(env.eachConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x04\x00\x00\x00\x00\x00\x00\x00" "each"), 12);
  cg.bind(env.findConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x04\x00\x00\x00\x00\x00\x00\x00" "find"), 12);
  cg.bind(env.anyConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x03\x00\x00\x00\x00\x00\x00\x00" "any"), 11);
  cg.bind(env.allConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x03\x00\x00\x00\x00\x00\x00\x00" "all"), 11);
  cg.bind(env.reduceConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x06\x00\x00\x00\x00\x00\x00\x00" "reduce"), 14);
  cg.bind(env.sumConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x03\x00\x00\x00\x00\x00\x00\x00" "sum"), 11);
  cg.bind(env.joinConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x04\x00\x00\x00\x00\x00\x00\x00" "join"), 12);
  cg.bind(env.sortConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x04\x00\x00\x00\x00\x00\x00\x00" "sort"), 12);
  cg.bind(env.sortedConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x06\x00\x00\x00\x00\x00\x00\x00" "sorted"), 14);
  cg.bind(env.sizeConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x04\x00\x00\x00\x00\x00\x00\x00" "size"), 12);
  cg.bind(env.modifiedConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x08\x00\x00\x00\x00\x00\x00\x00" "modified"), 16);
  cg.bind(env.isFileConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x06\x00\x00\x00\x00\x00\x00\x00" "isFile"), 14);
  cg.bind(env.isDirConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x05\x00\x00\x00\x00\x00\x00\x00" "isDir"), 13);
  cg.bind(env.emptyString);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x00\x00\x00\x00\x00\x00\x00\x00"), 8);
  // The string method names (PHASE 7c.3i), compared by `member_get`.
  auto emitNameConst = [&](CodeGen::Label lbl, const char* name) {
    std::string bytes(8, '\0');
    const std::uint64_t len = std::strlen(name);
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<char>((len >> (8 * i)) & 0xFF);
    cg.bind(lbl);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(bytes.data()), 8);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(name), len);
  };
  emitNameConst(env.upperConst, "upper");
  emitNameConst(env.lowerConst, "lower");
  emitNameConst(env.trimConst, "trim");
  emitNameConst(env.trimStartConst, "trimStart");
  emitNameConst(env.trimEndConst, "trimEnd");
  emitNameConst(env.containsStrConst, "contains");
  emitNameConst(env.startsWithConst, "startsWith");
  emitNameConst(env.endsWithConst, "endsWith");
  emitNameConst(env.indexOfStrConst, "indexOf");
  emitNameConst(env.substrConst, "substr");
  emitNameConst(env.reversedStrConst, "reversed");
  emitNameConst(env.repeatConst, "repeat");
  emitNameConst(env.splitConst, "split");
  emitNameConst(env.linesConst, "lines");
  emitNameConst(env.charsConst, "chars");
  emitNameConst(env.replaceConst, "replace");
  emitNameConst(env.replaceFirstConst, "replaceFirst");
  emitNameConst(env.padStartConst, "padStart");
  emitNameConst(env.padEndConst, "padEnd");
  emitNameConst(env.toArrayConst, "toArray");
  emitNameConst(env.spaceConst, " ");
  // The `repr`/`str` text of each bound method value, the same raw `AotString`
  // layout. A `Func` value renders the closure's `display` field verbatim.
  for (std::size_t i = 0; i < env.arrayMethodDisplays.size(); ++i) {
    emitNameConst(env.arrayMethodDisplays[i], env.arrayDisplayTexts[i].c_str());
  }
  for (std::size_t i = 0; i < env.mapMethodDisplays.size(); ++i) {
    emitNameConst(env.mapMethodDisplays[i], env.mapDisplayTexts[i].c_str());
  }
  for (std::size_t i = 0; i < env.stringMethodDisplays.size(); ++i) {
    emitNameConst(env.stringMethodDisplays[i], env.stringDisplayTexts[i].c_str());
  }
  for (std::size_t i = 0; i < env.rangeMethodDisplays.size(); ++i) {
    emitNameConst(env.rangeMethodDisplays[i], env.rangeDisplayTexts[i].c_str());
  }
  // The Unicode case tables, as `{first, last, delta}` int32 triples, the same
  // layout `UnicodeCaseRun` has on the host.
  auto emitCaseTable = [&](CodeGen::Label lbl, const UnicodeCaseRun* runs,
                           std::size_t count) {
    cg.bind(lbl);
    for (std::size_t i = 0; i < count; ++i) {
      cg.emitBytes(reinterpret_cast<const std::uint8_t*>(&runs[i].first), 4);
      cg.emitBytes(reinterpret_cast<const std::uint8_t*>(&runs[i].last), 4);
      cg.emitBytes(reinterpret_cast<const std::uint8_t*>(&runs[i].delta), 4);
    }
  };
  emitCaseTable(env.upperTable, kUnicodeUpperRuns, kUnicodeUpperRunCount);
  emitCaseTable(env.lowerTable, kUnicodeLowerRuns, kUnicodeLowerRunCount);
  // The map method member names (PHASE 7c.3g). `clear`/`clone` reuse the array
  // islands above.
  cg.bind(env.keysConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x04\x00\x00\x00\x00\x00\x00\x00" "keys"), 12);
  cg.bind(env.valuesConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x06\x00\x00\x00\x00\x00\x00\x00" "values"), 14);
  cg.bind(env.entriesConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x07\x00\x00\x00\x00\x00\x00\x00" "entries"), 15);
  cg.bind(env.hasConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x03\x00\x00\x00\x00\x00\x00\x00" "has"), 11);
  cg.bind(env.getConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x03\x00\x00\x00\x00\x00\x00\x00" "get"), 11);
  cg.bind(env.setConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x03\x00\x00\x00\x00\x00\x00\x00" "set"), 11);
  cg.bind(env.deleteConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x06\x00\x00\x00\x00\x00\x00\x00" "delete"), 14);
  cg.bind(env.mergeConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x05\x00\x00\x00\x00\x00\x00\x00" "merge"), 13);
  cg.bind(env.messageConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x07\x00\x00\x00\x00\x00\x00\x00" "message"), 15);
  cg.bind(env.codeConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x04\x00\x00\x00\x00\x00\x00\x00" "code"), 12);
  if (pcallHandlerSlot >= 0) {
    cg.bind(okConst);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x02\x00\x00\x00\x00\x00\x00\x00" "ok"), 10);
    cg.bind(valueConst);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x05\x00\x00\x00\x00\x00\x00\x00" "value"), 13);
    cg.bind(errorConst);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x05\x00\x00\x00\x00\x00\x00\x00" "error"), 13);
  }

  // The field names of every by-value struct in an `extern "C"` signature
  // (PHASE 7f.7), each a raw `AotString` the thunk uses as a map key. They are
  // emitted here, with the other data islands, because the thunk's `lea` fixups
  // were emitted long before.
  if (externThunks != nullptr) {
    for (const AotExternThunk& thunk : *externThunks) {
      for (const AotStructLayout& layout : thunk.structs) {
        for (std::size_t k = 0; k < layout.fieldNameLabels.size(); ++k) {
          cg.bind(layout.fieldNameLabels[k]);
          const std::uint64_t length = layout.layout.fields[k].name.size();
          cg.emitBytes(reinterpret_cast<const std::uint8_t*>(&length), sizeof length);
          cg.emitBytes(reinterpret_cast<const std::uint8_t*>(layout.layout.fields[k].name.data()),
                       layout.layout.fields[k].name.size());
        }
      }
    }
  }
  // The same name islands for a struct returned by a callback: the body reads
  // each field out of the returned map with `oskel_member_get`.
  if (callbacks != nullptr) {
    for (const AotCallbackSignature& signature : *callbacks) {
      if (!signature.returnsStruct) continue;
      const AotStructLayout& layout = signature.returnStruct;
      for (std::size_t k = 0; k < layout.fieldNameLabels.size(); ++k) {
        cg.bind(layout.fieldNameLabels[k]);
        const std::uint64_t length = layout.layout.fields[k].name.size();
        cg.emitBytes(reinterpret_cast<const std::uint8_t*>(&length), sizeof length);
        cg.emitBytes(reinterpret_cast<const std::uint8_t*>(layout.layout.fields[k].name.data()),
                     layout.layout.fields[k].name.size());
      }
    }
  }

  // Data islands last: every code path above ends in `ret` or `ud2`, so
  // control never reaches them, and their addresses are already referenced by
  // the `lea` fixups emitted above.
  for (const DataBlob& blob : data) {
    cg.bind(blob.label);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(blob.text.data()),
                 blob.text.size());
  }
}
