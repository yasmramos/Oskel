#include "native/runtime.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "native/dtoa.h"
#include "native/mathlib.h"
#include "native/parse_float.h"

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

// Field offsets of the collection headers (see `native/value.h`). Arrays and
// maps share the same three-word header (length, capacity, block), so one set
// of offsets serves both; a map entry is a key pointer followed by a value.
constexpr std::int64_t kTableLengthOffset = 0;
constexpr std::int64_t kTableCapacityOffset = 8;
constexpr std::int64_t kTableBlockOffset = 16;
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
  CodeGen::Label mapFind;      // long oskel_map_find(AotMap* m, AotString* k)
  CodeGen::Label blockEnsure;  // long oskel_block_ensure(void* h, long n, long itemSize)
  CodeGen::Label utf8Len;      // long oskel_utf8_len(AotString* s)
  CodeGen::Label utf8CharAt;   // (AotString*, long) -> char* in rax, length in rdx
  CodeGen::Label compare;      // AotValue oskel_compare(AotValue, AotValue, long)
  CodeGen::Label rawPrint;     // void oskel_print_value_raw(AotValue)
  CodeGen::Label printRepr;    // void oskel_print_repr(AotValue, long depth)
  CodeGen::Label printStrEsc;  // void oskel_print_str_escaped(AotString*)
  CodeGen::Label lengthConst;  // data: the string "length"
  // Bound array methods (PHASE 7c.3c/7c.3e): `oskel_member_get` returns a
  // closure for `arr.push`/`arr.pop`/`arr.shift`/`arr.unshift`/`arr.insert`/
  // `arr.removeAt`/`arr.contains`/`arr.indexOf`/`arr.clear`, built by
  // `bindArrayMethod`; the method bodies are the `*Method` labels below. The
  // member names are the data islands `*Const` (raw `AotString`s, like
  // `lengthConst`).
  CodeGen::Label bindArrayMethod;
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
  // The error member names (`err.message` / `err.code`), raw `AotString`s.
  CodeGen::Label messageConst;   // data: the string "message"
  CodeGen::Label codeConst;      // data: the string "code"
  const DataBlob* msgOom;
  const DataBlob* msgArrayIndexType;
  const DataBlob* msgIndexRange;
  const DataBlob* msgMapKeyType;
  const DataBlob* msgBadIndex;
  const DataBlob* msgNoMember;
  const DataBlob* msgImmutable;
  const DataBlob* msgBadAssign;
  const DataBlob* msgBadField;
  const DataBlob* msgBadIn;
  const DataBlob* msgMapValue;
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

// Emits `call fail(message)`: loads the message address and length, then calls
// the shared fail helper. Never returns.
void emitFailCall(CodeGen& cg, CodeGen::Label fail, const DataBlob& message) {
  cg.leaRipLabel(Rdi, message.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(message.text.size()));
  cg.callLabel(fail);
  cg.ud2();
}

// `void fail(rdi = text, rsi = length)`: writes the text to stderr and exits
// with the runtime-error code. Never returns, so it has no epilogue.
void emitFail(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
              CodeGen::Label exitLabel) {
  cg.bind(label);
  prologue(cg);
  cg.movReg(Rdx, Rsi);
  cg.movReg(Rsi, Rdi);
  cg.movImm(Rdi, 2);  // stderr
  cg.callLabel(writeLabel);
  cg.movImm(Rdi, kRuntimeErrorExit);
  cg.callLabel(exitLabel);
  cg.ud2();
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
                        std::int64_t fd, bool newline, bool hosted) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asNum = cg.newLabel();
  CodeGen::Label asRepr = cg.newLabel();
  CodeGen::Label asError = cg.newLabel();
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
  emitFailCall(cg, fail, badValue);

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
                    bool hosted) {
  emitPrintValueCore(cg, label, writeLabel, fail, writeInt, formatDouble, printRepr,
                     badValue, textTrue, textFalse, textNil, textNewline, /*fd=*/1,
                     /*newline=*/true, hosted);
}

// `void oskel_print_value_raw(AotValue v)`: prints the value with no trailing
// newline, to stdout, for the `print`/`write` builtins.
void emitPrintValueRaw(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                       CodeGen::Label fail, CodeGen::Label writeInt,
                       CodeGen::Label formatDouble, CodeGen::Label printRepr,
                       const DataBlob& badValue,
                       const DataBlob& textTrue, const DataBlob& textFalse,
                       const DataBlob& textNil, const DataBlob& textNewline,
                       bool hosted) {
  emitPrintValueCore(cg, label, writeLabel, fail, writeInt, formatDouble, printRepr,
                     badValue, textTrue, textFalse, textNil, textNewline, /*fd=*/1,
                     /*newline=*/false, hosted);
}

// `void oskel_print_value_err_raw(AotValue v)`: prints the value with no trailing
// newline, to stderr, for the `eprint` builtin.
void emitPrintValueErrRaw(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                          CodeGen::Label fail, CodeGen::Label writeInt,
                          CodeGen::Label formatDouble, CodeGen::Label printRepr,
                          const DataBlob& badValue,
                          const DataBlob& textTrue, const DataBlob& textFalse,
                          const DataBlob& textNil, const DataBlob& textNewline,
                          bool hosted) {
  emitPrintValueCore(cg, label, writeLabel, fail, writeInt, formatDouble, printRepr,
                     badValue, textTrue, textFalse, textNil, textNewline, /*fd=*/2,
                     /*newline=*/false, hosted);
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
               const DataBlob& msgTypes, const DataBlob& msgDivZero,
               CodeGen::Label concatLabel, CodeGen::Label arrayConcatLabel) {
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
  emitFailCall(cg, fail, msgTypes);
}

// `AotValue oskel_pow(AotValue a, AotValue b)`: `**` with the int/float/double
// promotion of `ops::binary`. int/int with a non-negative exponent uses the
// wrapping integer loop (`intPow`); every other numeric pair (a float/double
// operand, or a negative exponent) converts to binary64, calls the embedded
// musl `pow` and re-tags the result, so a float/float result rounds back to
// binary32 exactly like the hosted `makeNumeric`. A non-number operand is the
// unsupported-operand error.
void emitPow(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
             const DataBlob& msgPow, CodeGen::Label mathPow) {
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
  emitFailCall(cg, fail, msgPow);
}

// `AotValue oskel_shift(AotValue a, AotValue b, long op)`: integer `<<`/`>>`.
// The count must be an int in 0..63 (the hosted range check); `>>` is
// arithmetic, so it preserves the sign.
void emitShift(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
               const DataBlob& msgShift) {
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label doShl = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
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
}

// `AotValue oskel_bitwise(AotValue a, AotValue b, long op)`: int-only `&`, `|`
// and `^`, selected by `op` in r8.
void emitBitwise(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                 const DataBlob& message) {
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
  emitFailCall(cg, fail, message);
}

// `AotValue oskel_negate(AotValue a)`: arithmetic negation of an int (wrapping)
// or a float/double (sign-bit flip, exact for every value including NaN).
void emitNegate(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                const DataBlob& message) {
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
  emitFailCall(cg, fail, message);
}

// `AotValue oskel_bit_not(AotValue a)`: int-only `~`.
void emitBitNot(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                const DataBlob& message) {
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
  emitFailCall(cg, fail, message);
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
void emitCompare(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                 const DataBlob& message, CodeGen::Label mapFind) {
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
  emitFailCall(cg, fail, message);

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
// pointer, or null when the region is exhausted (mirroring `Arena::allocate`).
void emitAlloc(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label exhausted = cg.newLabel();

  cg.bind(label);
  cg.movReg(Rax, kAotArenaCursor);
  cg.addImm32(Rax, 7);
  cg.movImm(R10, -8);
  cg.andReg(Rax, R10);      // rax = cursor aligned up to 8
  cg.movReg(R11, Rax);
  cg.addReg(R11, Rdi);      // r11 = new cursor
  cg.jcc(Jcc::B, exhausted);        // wrapped: size was absurd
  cg.cmpReg(R11, kAotArenaEnd);
  cg.jcc(Jcc::A, exhausted);        // past the end
  cg.movReg(kAotArenaCursor, R11);
  cg.retInsn();
  cg.bind(exhausted);
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
                         const std::vector<CodeGen::Label>& methods) {
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
  cg.jmp(haveCode);
  for (std::size_t i = 0; i + 1 < methods.size(); ++i) {
    cg.bind(use[i]);
    cg.leaRipLabel(Rdi, methods[i]);
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
void emitArrayPopMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
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
  emitFailCall(cg, env.fail, msgArgc);
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
void emitArrayShiftMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
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
  emitFailCall(cg, env.fail, msgArgc);
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
  emitFailCall(cg, env.fail, msgArgc);
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
                           const DataBlob& msgRange) {
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
  emitFailCall(cg, env.fail, msgArgc);
  cg.bind(badType);
  emitFailCall(cg, env.fail, msgIndexType);
  cg.bind(badRange);
  emitFailCall(cg, env.fail, msgRange);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue oskel_builtin_array_remove_at(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.removeAt(index)`. A negative index counts from the end, an
// index outside `[0, len)` is the hosted "index out of range" error; the item
// is removed and returned. The tail items move down one slot with a single
// forward `rep movsb`.
void emitArrayRemoveAtMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                             const DataBlob& msgArgc, const DataBlob& msgIndexType,
                             const DataBlob& msgRange) {
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
  emitFailCall(cg, env.fail, msgArgc);
  cg.bind(badType);
  emitFailCall(cg, env.fail, msgIndexType);
  cg.bind(badRange);
  emitFailCall(cg, env.fail, msgRange);
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
  emitFailCall(cg, env.fail, msgArgc);
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
  emitFailCall(cg, env.fail, msgArgc);
}

// `AotValue oskel_builtin_array_clear(AotValue* args, long argc, AotClosure* self)`:
// the bound `arr.clear()`. It sets the length to zero in place and returns the
// receiver -- the hosted `bindArray` `clear`. The items block is kept (the
// capacity is unchanged), exactly as `std::vector::clear` keeps it.
void emitArrayClearMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
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
  emitFailCall(cg, env.fail, msgArgc);
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
  emitFailCall(cg, env.fail, msgArgc);
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
  emitFailCall(cg, env.fail, msgArgc);
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
  emitFailCall(cg, env.fail, msgArgc);
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
  emitFailCall(cg, env.fail, msgArgc);
  cg.bind(badStart);
  emitFailCall(cg, env.fail, msgStart);
  cg.bind(badEnd);
  emitFailCall(cg, env.fail, msgEnd);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `void oskel_init_globals(long count)`: allocate `count` `AotValue` slots
// (16 bytes each) from the arena, clear them to nil, and keep the base in r15
// (`kAotGlobalBase`). Called once by the chunk entry, before the program runs,
// so every global reads as nil until it is assigned -- the same "unset" state
// the hosted engine's `GETGLOBAL` checks. Reports "out of memory" on a null
// allocation.
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

  // Zero every slot: tag = Nil (0), payload = 0. `rep stosq` would need rdi
  // and rcx; a short explicit loop keeps the register use obvious and the
  // count is small (one slot per distinct global in the program).
  cg.movReg(R10, Rax);
  cg.movReg(R11, Rbx);
  cg.shlImm8(R11, 4);                  // byte count again
  cg.testReg(R11);
  cg.jcc(Jcc::Eq, clearDone);
  cg.bind(clear);
  cg.movImmToMem(R10, 0, 0);
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
// into rax:rdx. The slot is nil until assigned, matching the hosted engine's
// "unset" state; the lowerer rejects a read of a global that is never assigned
// in the chunk, so a program never observes that nil by accident.
void emitGlobalGet(CodeGen& cg, CodeGen::Label label) {
  cg.bind(label);
  cg.shlImm8(Rdi, 4);                  // slot * 16
  cg.addReg(Rdi, kAotGlobalBase);
  cg.movFromMem(Rax, Rdi, 0);          // tag
  cg.movFromMem(Rdx, Rdi, 8);          // payload
  cg.retInsn();
}

// `AotValue oskel_closure(void* code, long arity, long upvalueCount)`: allocate
// an `AotClosure` (code, arity, upvalueCount) followed by `upvalueCount` cell
// pointers in the arena and return it as a `Func` value (rax:rdx). The slot
// area is left for the lowering to fill: it knows which cells the child
// captures and writes each pointer with a plain store, so the helper does not
// need the descriptor table.
void emitClosure(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                 CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);  // scratch slot for the capture count (keeps rsp aligned)
  cg.movReg(Rbx, Rdi);      // code address
  cg.movReg(R12, Rsi);      // arity
  cg.movToMem(Rbp, -24, Rdx);  // stash upvalueCount
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
  cg.movReg(Rdx, Rax);                  // payload
  cg.movImm(Rax, kTagFunc);             // tag
  cg.addImm32(Rsp, 16);
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
void emitCall(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
              const DataBlob& msgNonFunc) {
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
  emitFailCall(cg, fail, msgNonFunc);
}

// `void oskel_arity_error(void)`: report the wrong-number-of-arguments error and
// exit. A function's prologue calls it when `argc` does not match its arity. It
// pushes rbp only to keep the stack 16-byte aligned at the `fail` call, exactly
// like every other helper; it never returns, so it has no epilogue.
void emitArityError(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                    const DataBlob& message) {
  cg.bind(label);
  prologue(cg);
  emitFailCall(cg, fail, message);
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
// array bodies. All ten mirror the hosted `bindMap` (src/builtins.cpp).

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
  emitFailCall(cg, env.fail, msgArgc);
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
  emitFailCall(cg, env.fail, msgArgc);
  cg.bind(badType);
  emitFailCall(cg, env.fail, msgKeyType);
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
  emitFailCall(cg, env.fail, msgArgc);
  cg.bind(badType);
  emitFailCall(cg, env.fail, msgKeyType);
}

// `delete(key)`: true when an entry was removed, false otherwise.
void emitMapDeleteBody(CodeGen& cg, const CollectionEnv& env) {
  (void)env;
  CodeGen::Label no = cg.newLabel();
  CodeGen::Label noMove = cg.newLabel();
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
  cg.bind(store);
  cg.movFromMem(R9, Rbp, -40);          // value tag
  cg.movToMem(R10, 0, R9);
  cg.movFromMem(R9, Rbp, -48);          // value payload
  cg.movToMem(R10, 8, R9);
  cg.movImm(Rax, kTagMap);
  cg.movReg(Rdx, Rbx);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitFailCall(cg, env.fail, msgArgc);
  cg.bind(badType);
  emitFailCall(cg, env.fail, msgKeyType);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `clear()`: drop every entry, then return the receiver.
void emitMapClearMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                        const DataBlob& msgArgc) {
  CodeGen::Label badArgc = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, badArgc);
  emitLoadMapReceiver(cg, Rbx, Rdx);
  cg.movImmToMem(Rbx, kTableLengthOffset, 0);
  cg.movImm(Rax, kTagMap);
  cg.movReg(Rdx, Rbx);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(badArgc);
  emitFailCall(cg, env.fail, msgArgc);
}

// `merge(other)`: copy every entry of `other` over the receiver, then return it.
void emitMapMergeMethod(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                        const DataBlob& msgArgc, const DataBlob& msgType) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label append = cg.newLabel();
  CodeGen::Label store = cg.newLabel();
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
  cg.bind(store);
  cg.movFromMem(R9, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movFromMem(R9, Rbp, -56);
  cg.movToMem(R10, 8, R9);
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
  emitFailCall(cg, env.fail, msgArgc);
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
  emitFailCall(cg, env.fail, msgArgc);
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
// own members; anything else is "value has no such member".
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
  CodeGen::Label rangeMax = cg.newLabel();
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
  CodeGen::Label bind = cg.newLabel();

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
  // integer code). Any other name is "value has no such member", matching the
  // hosted `memberGet`.
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
  cg.jcc(Jcc::Eq, failMember);
  cg.movReg(Rdi, R12);
  cg.callLabel(env.utf8Len);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

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
  cg.jcc(Jcc::Eq, failMember);
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
  cg.jcc(Jcc::Eq, rangeUse);
  cg.movImm(R11, -1);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Eq, rangeMax);       // distance == UINT64_MAX: saturate
  cg.addImm32(R10, 1);
  cg.bind(rangeMax);
  cg.movImm(R11, -1);
  cg.shrImm8(R11, 1);              // INT64_MAX
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Le, rangeUse);
  cg.movReg(R10, R11);             // saturate
  cg.bind(rangeUse);
  cg.movReg(Rdx, R10);
  cg.movImm(Rax, kTagInt);

  cg.bind(done);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(failMember);
  emitFailCall(cg, env.fail, *env.msgNoMember);
  cg.bind(failIndex);
  emitFailCall(cg, env.fail, *env.msgBadIndex);
}


// `long oskel_map_find(AotMap* m, AotString* key)`: the index of `key` in the
// map's entry array, or -1. A linear scan keeps the layout simple; a hash index
// can be added later without changing the contract. The loop state survives the
// `strEq` call on the stack, since that call clobbers r8-r11.
void emitMapFind(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label found = cg.newLabel();
  CodeGen::Label notFound = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(Rbx, Rdi);  // m
  cg.movReg(R12, Rsi);  // key
  cg.movFromMem(Rax, Rbx, kTableLengthOffset);
  cg.movToMem(Rbp, -24, Rax);  // len
  cg.movImmToMem(Rbp, -32, 0); // i
  cg.bind(loop);
  cg.movFromMem(R8, Rbp, -32);  // i
  cg.movFromMem(R9, Rbp, -24);  // len
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, notFound);
  cg.movReg(R10, R8);
  cg.imulImm32(R10, R10, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R11, Rbx, kTableBlockOffset);
  cg.addReg(R10, R11);              // entry = entries + i*24
  cg.movFromMem(Rdi, R10, kMapEntryKeyOffset);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, found);
  cg.movFromMem(R8, Rbp, -32);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -32, R8);  // i++
  cg.jmp(loop);
  cg.bind(found);
  cg.movFromMem(Rax, Rbp, -32);  // index
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(notFound);
  cg.movImm(Rax, -1);
  cg.addImm32(Rsp, 16);
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
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);  // kind
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotArrayHeaderSize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movImmToMem(Rax, kTableLengthOffset, 0);
  cg.movImmToMem(Rax, kTableCapacityOffset, 0);
  cg.movImmToMem(Rax, kTableBlockOffset, 0);
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
  CodeGen::Label failRange = cg.newLabel();
  CodeGen::Label failIndex = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
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
  cg.jcc(Jcc::Lt, failRange);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, failRange);
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
  // index is rebased against the code-point length first; `utf8Len` clobbers
  // r8, so the raw index is reloaded from its stack slot before the add.
  cg.movFromMem(R8, Rbp, -32);
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, strNonNeg);
  cg.movReg(Rdi, R12);
  cg.callLabel(env.utf8Len);
  cg.movFromMem(R8, Rbp, -32);
  cg.addReg(R8, Rax);
  cg.bind(strNonNeg);
  cg.movReg(Rdi, R12);
  cg.movReg(Rsi, R8);
  cg.callLabel(env.utf8CharAt);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, failRange);
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
  emitFailCall(cg, env.fail, *env.msgArrayIndexType);
  cg.bind(failMapType);
  emitFailCall(cg, env.fail, *env.msgMapKeyType);
  cg.bind(failRange);
  emitFailCall(cg, env.fail, *env.msgIndexRange);
  cg.bind(failIndex);
  emitFailCall(cg, env.fail, *env.msgBadIndex);
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
  cg.bind(mapStore);
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movToMem(R10, 8, R11);
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
  emitFailCall(cg, env.fail, *env.msgBadAssign);
  cg.bind(arrRange);
  emitFailCall(cg, env.fail, *env.msgIndexRange);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `obj.name = value`, valid only on a map (mirrors `ops::memberSet`).
void emitMemberSet(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label append = cg.newLabel();
  CodeGen::Label store = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failField = cg.newLabel();

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
  cg.bind(append);
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
  cg.bind(store);
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movToMem(R10, 8, R11);
  cg.bind(done);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(failField);
  emitFailCall(cg, env.fail, *env.msgBadField);
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
  cg.jcc(Jcc::Ne, failType);
  cg.movFromMem(Rdi, Rbp, -32);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, mapFound);
  cg.jmp(falseDone);

  cg.bind(str);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, failType);
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
  cg.jcc(Jcc::Ne, failType);
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
  emitFailCall(cg, env.fail, *env.msgBadIn);
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
  // A `for k, v` over anything but a map is a fatal error, the same one the VM
  // raises; the AOT message is the fixed `msgMapValue` blob (it does not carry
  // the offending type name, which the freestanding runtime has no formatter
  // for on this path).
  cg.movFromMem(R8, Rbp, -56);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, noValueCheck);
  cg.movFromMem(R8, Rbp, -24);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Ne, failKeyValue);
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
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -32);
  cg.callLabel(env.utf8CharAt);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, doneTrue);
  cg.movToMem(Rbp, -40, Rax);       // char pointer
  cg.movToMem(Rbp, -48, Rdx);       // char byte length
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
  cg.jmp(storeCursor);

  emitIterNextRange(cg, range, rangeAsc, rangeAscIncl, rangeDescIncl, rangeEmit,
                    rangeDescNext, rangeStoreCursor, doneTrue, done);

  cg.bind(storeCursor);
  cg.movFromMem(R9, Rbp, -32);
  cg.addImm32(R9, 1);
  cg.movImmToMem(Rbx, 0, kTagInt);
  cg.movToMem(Rbx, 8, R9);
  cg.xorReg(R8, R8);                // done = false
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
  emitFailCall(cg, env.fail, *env.msgMapValue);
  cg.bind(failType);
  emitFailCall(cg, env.fail, *env.msgBadIn);
  cg.bind(failState);
  emitFailCall(cg, env.fail, *env.msgBadIn);
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
void emitIsErrorBuiltin(CodeGen& cg, CodeGen::Label label) {
  cg.bind(label);
  prologue(cg);
  // args[0] is the operand: the argument window grows *down* from `args`, so
  // args[0] sits at [rdi + 0] (tag) and [rdi + 8] (payload).
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagError));
  cg.setcc(Jcc::Eq, Rdx);
  cg.movzx(Rdx, Rdx);
  cg.movImm(Rax, kTagBool);
  epilogue(cg);
}

// `AotValue oskel_builtin_error(AotValue* args, long argc, AotClosure* self)`:
// the public `error(message, code?)`. It mirrors the hosted builtin: the message
// is the first argument rendered to a string (a string is taken verbatim, any
// other value through `toString`), and the optional second argument is the
// integer code (0 by default). The result is an `Error` value.
void emitErrorBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                      CodeGen::Label fail, CodeGen::Label strBuiltin,
                      const DataBlob& msgOom, const DataBlob& msgCodeType,
                      const DataBlob& msgArity) {
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
  emitFailCall(cg, fail, msgArity);
  cg.bind(arityOk);
  cg.cmpImm32(R12, 2);
  cg.jcc(Jcc::Le, arityOk2);
  emitFailCall(cg, fail, msgArity);
  cg.bind(arityOk2);

  // code = 0 unless a second argument is present and an integer.
  cg.xorReg(R8, R8);
  cg.movToMem(Rbp, -24, R8);        // code = 0
  cg.cmpImm32(R12, 2);
  cg.jcc(Jcc::Lt, haveCode);
  cg.movFromMem(Rax, Rbx, -16);     // args[1].tag
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, codeOk);
  emitFailCall(cg, fail, msgCodeType);
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
                    const DataBlob& textNil, const DataBlob& msgStrBad) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label boolFalse = cg.newLabel();
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asNum = cg.newLabel();
  CodeGen::Label asRepr = cg.newLabel();
  CodeGen::Label asError = cg.newLabel();
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
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 64);   // digit buffer (int, or range bounds + separator)
  cg.movReg(Rbx, Rdi);       // rbx = args

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
  emitFailCall(cg, fail, msgStrBad);

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
void emitSliceBuiltin(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env,
                      const DataBlob& msgSliceStart, const DataBlob& msgSliceEnd,
                      const DataBlob& msgSliceStep, const DataBlob& msgSliceType) {
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

  // Bounds: start = args[1], end = args[2], step = args[3] (nil = absent).
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

  cg.movFromMem(Rax, Rbx, 0);
  cg.movToMem(Rbp, kObjTag, Rax);
  cg.movFromMem(R12, Rbx, 8);  // object payload
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, sliceArray);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, sliceStr);
  cg.jmp(failType);

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
  emitFailCall(cg, env.fail, msgSliceStep);
  cg.bind(failStep);
  emitFailCall(cg, env.fail, msgSliceStep);
  cg.bind(failType);
  emitFailCall(cg, env.fail, msgSliceType);
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
                    const DataBlob& msgLenType) {
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asArray = cg.newLabel();
  CodeGen::Label asMap = cg.newLabel();
  CodeGen::Label asRange = cg.newLabel();
  CodeGen::Label rangeAsc = cg.newLabel();
  CodeGen::Label rangeSat = cg.newLabel();
  CodeGen::Label rangeUse = cg.newLabel();
  CodeGen::Label rangeMax = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
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
  emitFailCall(cg, env.fail, msgLenType);

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
  cg.jcc(Jcc::Le, rangeUse);
  cg.movReg(R10, R11);
  cg.bind(rangeUse);
  cg.movReg(Rdx, R10);
  cg.movImm(Rax, kTagInt);

  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `AotValue oskel_builtin_type(AotValue* args, long argc, AotClosure* self)`: the
// public `type`. The tag selects a constant type-name string, allocated into the
// arena, matching the hosted `Value::typeName` names.
void emitTypeBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                     CodeGen::Label fail, const DataBlob& msgOom, const DataBlob& tNil,
                     const DataBlob& tBool, const DataBlob& tInt, const DataBlob& tFloat,
                     const DataBlob& tDouble, const DataBlob& tStr, const DataBlob& tArray,
                     const DataBlob& tMap, const DataBlob& tFunc, const DataBlob& tRange,
                     const DataBlob& tError) {
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asFloat = cg.newLabel();
  CodeGen::Label asDouble = cg.newLabel();
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asArray = cg.newLabel();
  CodeGen::Label asMap = cg.newLabel();
  CodeGen::Label asFunc = cg.newLabel();
  CodeGen::Label asRange = cg.newLabel();
  CodeGen::Label asError = cg.newLabel();
  CodeGen::Label buildConst = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
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
  cg.jmp(asNil);  // every AotTag is covered; default is defensive only

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
  OSKEL_TYPE_CASE(asMap, tMap)
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
void emitIntTextParser(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                       const DataBlob& message) {
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
  cg.jcc(Jcc::Ae, bad);                   // empty text

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
  cg.jcc(Jcc::A, bad);                    // acc > cutoff: overflow
  cg.jcc(Jcc::B, accumulate);
  cg.cmpReg(Rdx, Rax);
  cg.jcc(Jcc::A, bad);                    // acc == cutoff and digit > rem
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
  cg.movReg(Rax, R9);
  cg.pop(Rbx);
  cg.retInsn();

  cg.bind(bad);
  emitFailCall(cg, fail, message);
}

// The three numeric conversions (PHASE 7c.5b.1): the public `int`, `float` and
// `double` for a numeric or boolean operand. They match the hosted conversions
// and are pure SSE -- no libm, no libc -- so they link into the freestanding
// binary. `int` also accepts a string and calls `intTextParser` (7c.5b.2a);
// `float`/`double` reject a string until 7c.5b.2b, with a clear message. `args`
// is in rdi, `argc` in rsi on entry; the helpers make no call before they are
// done with their inputs, so `rsp` stays 16-byte aligned at the `fail` call and
// they save nothing.
void emitNumericConversionBuiltins(CodeGen& cg, CodeGen::Label intLabel,
                                   CodeGen::Label floatLabel, CodeGen::Label doubleLabel,
                                   CodeGen::Label fail, CodeGen::Label intTextParser,
                                   CodeGen::Label floatTextParser,
                                   const DataBlob& msgInt,
                                   const DataBlob& msgFloat, const DataBlob& msgDouble) {
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
    CodeGen::Label badArgc = cg.newLabel();
    CodeGen::Label failFinite = cg.newLabel();
    CodeGen::Label failRange = cg.newLabel();
    CodeGen::Label done = cg.newLabel();

    cg.bind(intLabel);
    prologue(cg);
    cg.cmpImm32(Rsi, 1);
    cg.jcc(Jcc::Ne, badArgc);
    cg.movFromMem(Rax, Rdi, 0);   // tag
    cg.movFromMem(Rcx, Rdi, 8);   // payload
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
    emitFailCall(cg, fail, msgInt);

    cg.bind(asStr);
    cg.movReg(Rdi, Rcx);                          // AotString*
    cg.movFromMem(Rsi, Rcx, 0);                   // length
    cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
    cg.callLabel(intTextParser);
    cg.movReg(Rdx, Rax);
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

    cg.bind(badArgc);
    emitFailCall(cg, fail, msgInt);
    cg.bind(failFinite);
    emitFailCall(cg, fail, msgInt);
    cg.bind(failRange);
    emitFailCall(cg, fail, msgInt);

    cg.bind(done);
    epilogue(cg);
  }

  // --- double ---
  {
    CodeGen::Label asInt = cg.newLabel();
    CodeGen::Label asBool = cg.newLabel();
    CodeGen::Label asFloat = cg.newLabel();
    CodeGen::Label asDouble = cg.newLabel();
    CodeGen::Label asStr = cg.newLabel();
    CodeGen::Label failStr = cg.newLabel();
    CodeGen::Label badArgc = cg.newLabel();
    CodeGen::Label finish = cg.newLabel();
    CodeGen::Label done = cg.newLabel();

    cg.bind(doubleLabel);
    prologue(cg);
    cg.cmpImm32(Rsi, 1);
    cg.jcc(Jcc::Ne, badArgc);
    cg.movFromMem(Rax, Rdi, 0);   // tag
    cg.movFromMem(Rcx, Rdi, 8);   // payload
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
    emitFailCall(cg, fail, msgDouble);

    // A string: parse the text with the freestanding parser, which yields the
    // binary64 pattern or a failure the hosted engine reports as an error.
    cg.bind(asStr);
    cg.movReg(Rdi, Rcx);                          // AotString*
    cg.movFromMem(Rsi, Rcx, 0);                   // length
    cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
    cg.subRegImm32(Rsp, static_cast<std::int32_t>(kParseFrame));
    cg.movReg(Rcx, Rsp);                          // scratch
    cg.movReg(Rdx, Rsp);
    cg.addImm32(Rdx, static_cast<std::int32_t>(kParseScratchBytes));  // outBits
    cg.callLabel(floatTextParser);
    cg.testReg(Rax);
    cg.jcc(Jcc::Ne, failStr);
    cg.movFromMem(Rdx, Rsp, static_cast<std::int32_t>(kParseScratchBytes));
    cg.movReg(R11, Rsp);
    cg.addImm32(R11, static_cast<std::int32_t>(kParseFrame));
    cg.movReg(Rsp, R11);
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

    cg.bind(badArgc);
    emitFailCall(cg, fail, msgDouble);
    cg.bind(failStr);
    emitFailCall(cg, fail, msgDouble);

    cg.bind(done);
    epilogue(cg);
  }

  // --- float ---
  {
    CodeGen::Label asInt = cg.newLabel();
    CodeGen::Label asBool = cg.newLabel();
    CodeGen::Label asFloat = cg.newLabel();
    CodeGen::Label asDouble = cg.newLabel();
    CodeGen::Label asStr = cg.newLabel();
    CodeGen::Label failStr = cg.newLabel();
    CodeGen::Label badArgc = cg.newLabel();
    CodeGen::Label roundToFloat = cg.newLabel();
    CodeGen::Label done = cg.newLabel();

    cg.bind(floatLabel);
    prologue(cg);
    cg.cmpImm32(Rsi, 1);
    cg.jcc(Jcc::Ne, badArgc);
    cg.movFromMem(Rax, Rdi, 0);   // tag
    cg.movFromMem(Rcx, Rdi, 8);   // payload
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
    emitFailCall(cg, fail, msgFloat);

    // A string: parse to binary64, then narrow, matching the hosted `float`
    // (which parses with `strtod` and casts the result to `float`).
    cg.bind(asStr);
    cg.movReg(Rdi, Rcx);                          // AotString*
    cg.movFromMem(Rsi, Rcx, 0);                   // length
    cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
    cg.subRegImm32(Rsp, static_cast<std::int32_t>(kParseFrame));
    cg.movReg(Rcx, Rsp);
    cg.movReg(Rdx, Rsp);
    cg.addImm32(Rdx, static_cast<std::int32_t>(kParseScratchBytes));
    cg.callLabel(floatTextParser);
    cg.testReg(Rax);
    cg.jcc(Jcc::Ne, failStr);
    cg.movFromMem(Rax, Rsp, static_cast<std::int32_t>(kParseScratchBytes));
    cg.movqToXmm(Xmm0, Rax);
    cg.movReg(R11, Rsp);
    cg.addImm32(R11, static_cast<std::int32_t>(kParseFrame));
    cg.movReg(Rsp, R11);
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
    cg.cvtsd2ss(Xmm0, Xmm0);      // round to binary32 ...
    cg.cvtss2sd(Xmm0, Xmm0);      // ... and widen back, as the hosted `Float` holds
    cg.movqFromXmm(Rdx, Xmm0);
    cg.movImm(Rax, kTagFloat);
    cg.jmp(done);

    cg.bind(badArgc);
    emitFailCall(cg, fail, msgFloat);
    cg.bind(failStr);
    emitFailCall(cg, fail, msgFloat);

    cg.bind(done);
    epilogue(cg);
  }
}

// `AotValue oskel_builtin_repr(AotValue* args, long argc, AotClosure* self)`: the
// public `repr`. It renders through the same recursive repr `$str` uses for a
// collection, which is exactly `Value::repr()` for every type (a top-level
// string is quoted).
void emitReprBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label reprToString) {
  cg.bind(label);
  prologue(cg);
  cg.movFromMem(Rsi, Rdi, 8);   // payload
  cg.movFromMem(Rdi, Rdi, 0);   // tag
  cg.callLabel(reprToString);
  epilogue(cg);
}

// `AotValue oskel_builtin_bool(AotValue* args, long argc, AotClosure* self)`: the
// public `bool`, i.e. the operand's truthiness (`0` and `nil` are false, every
// other value true).
void emitBoolBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label truthy) {
  cg.bind(label);
  prologue(cg);
  cg.movFromMem(Rsi, Rdi, 8);   // payload
  cg.movFromMem(Rdi, Rdi, 0);   // tag
  cg.callLabel(truthy);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagBool);
  epilogue(cg);
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
  emitFailCall(cg, fail, msgType);

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
  emitFailCall(cg, fail, msgArgc);

  cg.bind(negative);
  emitFailCall(cg, fail, msgNegative);
}

// `AotValue oskel_builtin_ord(AotValue* args, long argc, AotClosure* self)`: the
// public `ord`. The operand must be a string holding exactly one UTF-8 code
// point; the code point is returned as an int.
void emitOrdBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label utf8Len,
                    CodeGen::Label fail, const DataBlob& msgOrdType,
                    const DataBlob& msgOrdChar) {
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
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, isStr);
  emitFailCall(cg, fail, msgOrdType);

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
}

// `AotValue oskel_builtin_chr(AotValue* args, long argc, AotClosure* self)`: the
// public `chr`. The operand must be an int in `0..0x10FFFF`; the code point is
// UTF-8 encoded into a fresh arena string.
void emitChrBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                    CodeGen::Label fail, const DataBlob& msgChrInt,
                    const DataBlob& msgChrRange, const DataBlob& msgChrSurrogate,
                    const DataBlob& msgOom) {
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
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, isInt);
  emitFailCall(cg, fail, msgChrInt);

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
}

// `void oskel_fail_dynamic(char* text, long length)`: writes "oskel: " + the
// message + a newline to stderr and exits with the runtime-error code, never
// returning. It is the dynamic counterpart of `oskel_fail`, which takes a
// constant blob, and lets `assert`/`panic` report the caller's own message.
void emitFailDynamic(CodeGen& cg, CodeGen::Label label, CodeGen::Label write,
                     CodeGen::Label exit, const DataBlob& prefix, const DataBlob& newline) {
  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);          // message bytes
  cg.movReg(R12, Rsi);          // message length
  cg.leaRipLabel(Rdi, prefix.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(prefix.text.size()));
  emitWriteToFd(cg, write, 2);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R12);
  emitWriteToFd(cg, write, 2);
  cg.leaRipLabel(Rdi, newline.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(newline.text.size()));
  emitWriteToFd(cg, write, 2);
  cg.movImm(Rdi, kRuntimeErrorExit);
  cg.callLabel(exit);
  cg.ud2();
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

void emitRoundingBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                         const DataBlob& message, AotRoundMode mode) {
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
  emitFailCall(cg, fail, message);

  cg.bind(badArgc);
  emitFailCall(cg, fail, message);

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
void emitNumberArg(CodeGen& cg, int byteOffset, int xmm, CodeGen::Label fail,
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
  emitFailCall(cg, fail, message);
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
                               bool positiveOnly, const DataBlob& msgNonPositive) {
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
  emitNumberArg(cg, 0, Xmm0, fail, message);
  cg.jmp(check);

  cg.bind(badArgc);
  emitFailCall(cg, fail, message);

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
void emitPowBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                    CodeGen::Label mathCore, const DataBlob& message) {
  constexpr int Xmm0 = kXmmBase + 0;
  constexpr int Xmm1 = kXmmBase + 1;

  CodeGen::Label badArgc = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rsi, 2);
  cg.jcc(Jcc::Ne, badArgc);
  emitNumberArg(cg, 0, Xmm0, fail, message);
  emitNumberArg(cg, -16, Xmm1, fail, message);
  cg.callLabel(mathCore);
  emitMathReturn(cg);
  epilogue(cg);

  cg.bind(badArgc);
  emitFailCall(cg, fail, message);
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
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badStart);
  cg.movFromMem(R8, Rbx, 8);    // start
  cg.movFromMem(Rax, Rbx, -16);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badEnd);
  cg.movFromMem(R9, Rbx, -8);   // end
  cg.jmp(boundsSet);

  cg.bind(oneArg);
  cg.movFromMem(Rax, Rbx, 0);
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
  emitFailCall(cg, fail, msgArgc);
  cg.bind(badStart);
  emitFailCall(cg, fail, msgStart);
  cg.bind(badEnd);
  emitFailCall(cg, fail, msgEnd);
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
  emitFailCall(cg, fail, msgArgc);
  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `AotValue oskel_builtin_exit(AotValue* args, long argc, AotClosure* self)`: the
// public `exit`. It leaves the process through the entry prelude's `oskel_exit`
// with the integer code (`0` when omitted), so it never returns to the caller.
// The hosted builtin accepts zero or one integer argument.
void emitExitBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label exitLabel,
                     CodeGen::Label fail, const DataBlob& msgArgc,
                     const DataBlob& msgType) {
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
  emitFailCall(cg, fail, msgArgc);
  cg.bind(badType);
  emitFailCall(cg, fail, msgType);
}

// `time()`: the wall clock, in whole seconds since the Unix epoch (UTC), as an
// `int`. It takes no argument. The value comes from the target's `entry.now`
// handle, which each prelude binds its own way -- a raw syscall on Linux, libc
// `time` in the hosted mode, kernel32 on Windows and the CMOS RTC on bare
// metal -- so this emitter is target-independent and the runtime stays free of
// libc. The hosted engine's `time` builtin reads the same wall clock.
void emitTimeBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label nowLabel,
                     CodeGen::Label fail, const DataBlob& msgArgc) {
  CodeGen::Label ok = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, ok);
  emitFailCall(cg, fail, msgArgc);

  cg.bind(ok);
  cg.callLabel(nowLabel);          // rax = seconds since the epoch
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
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
  emitFailCall(cg, fail, msgArgc);

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
                        CodeGen::Label fail, const DataBlob& msgArgc) {
  CodeGen::Label ok = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, ok);
  emitFailCall(cg, fail, msgArgc);

  cg.bind(ok);
  cg.callLabel(memSizeLabel);      // rax = usable bytes
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);
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
void emitSbAppendEscaped(CodeGen& cg, CodeGen::Label label, CodeGen::Label sbAppend,
                         CodeGen::Label sbAppendByte, const DataBlob& textQuote,
                         const DataBlob& textEscQuote, const DataBlob& textEscBackslash,
                         const DataBlob& textEscNL, const DataBlob& textEscTab,
                         const DataBlob& textEscCR) {
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
  cg.movReg(R12, Rsi);  // string
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
  cg.movFromMem(R9, R12, 0);
  cg.cmpReg(R11, R9);
  cg.jcc(Jcc::Ae, loopDone);

  cg.movReg(R10, R11);
  cg.addReg(R10, R12);
  cg.movzxFromMem(R10, R10, kStringDataOffset);
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
                  CodeGen::Label sbAppendEscaped, CodeGen::Label formatDouble,
                  const DataBlob& textTrue, const DataBlob& textFalse,
                  const DataBlob& textNil, const DataBlob& textFunc,
                  const DataBlob& textComma, const DataBlob& textColon,
                  const DataBlob& textOpenBracket, const DataBlob& textCloseBracket,
                  const DataBlob& textOpenBrace, const DataBlob& textCloseBrace,
                  const DataBlob& textEllipsis, const DataBlob& textError,
                  const DataBlob& textCloseParen) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label boolFalse = cg.newLabel();
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asNum = cg.newLabel();
  CodeGen::Label asFunc = cg.newLabel();
  CodeGen::Label asArray = cg.newLabel();
  CodeGen::Label asMap = cg.newLabel();
  CodeGen::Label asRange = cg.newLabel();
  CodeGen::Label asError = cg.newLabel();
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
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textOpenBrace.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textOpenBrace.text.size()));
  cg.callLabel(sbAppend);
  cg.movFromMem(R12, Rbp, -40);        // map header
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
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textCloseBrace.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textCloseBrace.text.size()));
  cg.callLabel(sbAppend);
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

// The System V AMD64 integer argument registers, in order (PHASE 7f.5). A
// scalar `extern "C"` signature has at most `kMaxExternArgs` (6) parameters, so
// at most 6 of these 6 registers and 6 of the 8 SSE registers are used: the two
// counters in `emitExternThunk` can never overrun.
constexpr X64Reg kIntArgRegs[] = {Rdi, Rsi, Rdx, Rcx, R8, R9};
// A scratch xmm for the register-class conversions (a boxed number narrowed to
// an integer, an integer widened to a float). It must not collide with an
// argument xmm: a scalar signature uses at most xmm0-xmm5, so xmm7 is free. It
// is kept in the xmm0-xmm7 range deliberately, so no instruction that uses it
// needs a REX prefix.
constexpr int XmmScratch = kXmmBase + 7;

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
// `argSlot` is the argument's frame offset (`[rbx + argSlot]`). Each field is
// read with `oskel_member_get`, exactly like `obj.field`, so the read matches
// the VM's. A missing field yields nil, which the width stores turn into zero
// bytes, the same zero-fill the hosted `writeStructBytes` starts from.
void emitStructArgBytes(CodeGen& cg, const AotStructLayout& arg, std::int64_t argSlot,
                        std::int64_t bufDisp, CodeGen::Label memberGet) {
  // Zero the buffer first: the padding a C compiler leaves between fields is
  // part of the value the callee may copy.
  for (std::int64_t w = 0; w < 16; w += 8) cg.movImmToMem(Rbp, bufDisp + w, 0);

  const ffi::StructLayout& layout = arg.layout;
  for (std::size_t k = 0; k < layout.fields.size(); ++k) {
    const std::int64_t fieldOffset = static_cast<std::int64_t>(layout.offsets[k]);
    // rdi:rsi = the struct value (the map), rdx:rcx = the field name.
    cg.movFromMem(Rdi, Rbx, argSlot);
    cg.movFromMem(Rsi, Rbx, argSlot + 8);
    cg.movImm(Rdx, static_cast<std::int64_t>(AotTag::Str));
    cg.leaRipLabel(Rcx, arg.fieldNameLabels[k]);
    cg.callLabel(memberGet);
    // The field's boxed value is in rax (tag) / rdx (payload). Store its bytes
    // at the field's offset and width. A `float` field is boxed as binary64, so
    // it is narrowed back to binary32 first; every other class is a bit pattern
    // or an integer whose low bytes are the C representation.
    const std::size_t size = layout.sizes[k];
    if (layout.classes[k] == ffi::ParamClass::Float) {
      cg.movqToXmm(XmmScratch, Rdx);
      cg.cvtsd2ss(XmmScratch, XmmScratch);
      cg.movqFromXmm(Rdx, XmmScratch);
    }
    if (size == 1) {
      cg.movByteToMem(Rbp, bufDisp + fieldOffset, Rdx);
    } else if (size == 2) {
      // No 16-bit store exists (the backend's narrow forms are 8 and 32 bits),
      // so the two bytes go out one at a time; a single 32-bit store could spill
      // into the next field.
      cg.movByteToMem(Rbp, bufDisp + fieldOffset, Rdx);
      cg.shrImm8(Rdx, 8);
      cg.movByteToMem(Rbp, bufDisp + fieldOffset + 1, Rdx);
    } else if (size == 4) {
      cg.movToMem32(Rbp, bufDisp + fieldOffset, Rdx);
    } else {
      cg.movToMem(Rbp, bufDisp + fieldOffset, Rdx);
    }
  }
}

// Rebuilds the `AotMap` of a struct returned by value. The eightbytes come back
// in the registers their class assigns: an INTEGER eightbyte in `rax` then
// `rdx`, an SSE one in `xmm0` then `xmm1`. They are stashed in a stack buffer
// first, then each field is read at its offset, boxed exactly as the hosted
// `readStructField` does, and written into a fresh map.
void emitStructReturn(CodeGen& cg, const AotStructLayout& ret, std::int64_t bufDisp,
                      CodeGen::Label tableNew, CodeGen::Label memberSet) {
  const ffi::StructLayout& layout = ret.layout;
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
                     const DataBlob& msgArity, const DataBlob& msgType,
                     const DataBlob& msgOom, const DataBlob& msgCStringTooLong) {
  CodeGen::Label arityOk = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(thunk.label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);  // a callee-saved scratch: the `alloc`/`rep movsb` calls clobber rdi/rsi/rcx/r10/r11
  // The integer arguments are staged here and loaded into their registers only
  // just before the call, because marshalling a later pointer argument calls
  // `alloc` and `rep movsb`, which clobber the argument registers. Six 8-byte
  // slots keep the frame 16-byte aligned (48 is a multiple of 16).
  constexpr std::int32_t kArgStageBytes = 6 * 8;
  // One 16-byte buffer per by-value struct (at most six, since a signature is
  // bounded by six eightbytes and every struct contributes at least one). They
  // sit above the staging slots and are addressed from `rbp`, since the staging
  // slots are `rsp`-relative. Both regions are multiples of 16, so `rsp` stays
  // 16-byte aligned at the call.
  constexpr std::int32_t kStructBufSlots = 6;
  constexpr std::int32_t kStructBufBytes = 16 * kStructBufSlots;
  constexpr std::int64_t kStructBufBase =
      -static_cast<std::int64_t>(16 + kArgStageBytes + kStructBufBytes) + kArgStageBytes;
  cg.subRegImm32(Rsp, kArgStageBytes + kStructBufBytes);
  cg.movReg(Rbx, Rdi);  // args base; argument i lives at [rbx - 16*i]

  // The thunk is the callee, so it checks its own arity, exactly like a user
  // function's prologue. A direct call is already arity-checked at compile time,
  // so this only fires through a first-class reference (`var f = abs; f(1, 2)`).
  cg.cmpImm32(Rsi, static_cast<std::int32_t>(thunk.arity));
  cg.jcc(Jcc::Eq, arityOk);
  emitFailCall(cg, fail, msgArity);
  cg.bind(arityOk);

  // Build every struct argument's bytes first: each field read calls
  // `oskel_member_get`, which clobbers the argument registers and the xmm
  // registers, so nothing may be live in them yet. The buffers are dead once
  // the loop below has loaded the eightbytes, which is why the return path
  // reuses them.
  for (const AotThunkParam& param : thunk.params) {
    if (param.structIndex >= 0 && param.eightbyte == 0) {
      emitStructArgBytes(cg, thunk.structs[static_cast<std::size_t>(param.structIndex)],
                         -16LL * param.argIndex,
                         kStructBufBase + 16LL * param.structIndex, memberGet);
    }
  }

  int gpr = 0;
  int sse = 0;
  for (std::size_t i = 0; i < thunk.params.size(); ++i) {
    const AotThunkParam& param = thunk.params[i];
    if (param.structIndex >= 0) {
      // One eightbyte of a struct: load it whole out of the buffer the pre-pass
      // filled. An INTEGER eightbyte stages like a scalar GPR argument, an SSE
      // one goes straight to its xmm register.
      const std::int64_t off = kStructBufBase + 16LL * param.structIndex + 8LL * param.eightbyte;
      cg.movFromMem(R11, Rbp, off);
      if (param.cls == ffi::ParamClass::Double) {
        cg.movqToXmm(kXmmBase + sse, R11);
        ++sse;
      } else {
        cg.movToMem(Rsp, 8LL * gpr, R11);
        ++gpr;
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
        cg.movToMem(Rsp, 8LL * gpr, R11);
        ++gpr;
        break;
      }
      case ffi::ParamClass::Double: {
        // A number; an int is widened to binary64. A Float/Double payload is
        // already the binary64 bit pattern, so it moves verbatim.
        CodeGen::Label asIs = cg.newLabel();
        CodeGen::Label asInt = cg.newLabel();
        CodeGen::Label done = cg.newLabel();
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagFloat));
        cg.jcc(Jcc::Eq, asIs);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagDouble));
        cg.jcc(Jcc::Eq, asIs);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagInt));
        cg.jcc(Jcc::Eq, asInt);
        emitFailCall(cg, fail, msgType);
        cg.bind(asInt);
        cg.movFromMem(R11, Rbx, base + 8);
        cg.cvtsi2sd(kXmmBase + sse, R11);
        cg.jmp(done);
        cg.bind(asIs);
        cg.movFromMem(R11, Rbx, base + 8);
        cg.movqToXmm(kXmmBase + sse, R11);
        cg.bind(done);
        ++sse;
        break;
      }
      case ffi::ParamClass::Float: {
        // A number narrowed to binary32 (the ABI passes it in an SSE register).
        CodeGen::Label asIs = cg.newLabel();
        CodeGen::Label asInt = cg.newLabel();
        CodeGen::Label done = cg.newLabel();
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagFloat));
        cg.jcc(Jcc::Eq, asIs);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagDouble));
        cg.jcc(Jcc::Eq, asIs);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagInt));
        cg.jcc(Jcc::Eq, asInt);
        emitFailCall(cg, fail, msgType);
        cg.bind(asInt);
        cg.movFromMem(R11, Rbx, base + 8);
        cg.cvtsi2sd(XmmScratch, R11);
        cg.cvtsd2ss(kXmmBase + sse, XmmScratch);
        cg.jmp(done);
        cg.bind(asIs);
        cg.movFromMem(R11, Rbx, base + 8);
        cg.movqToXmm(kXmmBase + sse, R11);
        cg.cvtsd2ss(kXmmBase + sse, kXmmBase + sse);
        cg.bind(done);
        ++sse;
        break;
      }
      case ffi::ParamClass::Pointer: {
        // A `cstring`/`ptr` crosses as a GPR holding a machine address. `nil`
        // is C's NULL (0). A Oskel string is an `AotString` (a length then the
        // bytes, *not* NUL-terminated), so it cannot be passed as-is: the thunk
        // materialises a NUL-terminated copy in the arena and passes its
        // address, exactly the `const char *` a C callee dereferences. A
        // non-string value is not a pointer the AOT runtime can represent, so
        // it fails like the hosted engine does.
        CodeGen::Label isNil = cg.newLabel();
        CodeGen::Label isStr = cg.newLabel();
        CodeGen::Label staged = cg.newLabel();
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagNil));
        cg.jcc(Jcc::Eq, isNil);
        cg.cmpImm32(R10, static_cast<std::int32_t>(kTagStr));
        cg.jcc(Jcc::Eq, isStr);
        emitFailCall(cg, fail, msgType);
        cg.bind(isNil);
        cg.xorReg(R11, R11);  // NULL
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
        cg.movToMem(Rsp, 8LL * gpr, R11);
        ++gpr;
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
  // `alloc`/`rep movsb` has run. The SSE arguments are already in place.
  for (int k = 0; k < gpr; ++k) cg.movFromMem(kIntArgRegs[k], Rsp, 8LL * k);

  cg.callExternal(thunk.symbol);

  // A struct return comes back in the registers its eightbyte classes assign
  // (`rax`/`rdx` and `xmm0`/`xmm1`), so it is rebuilt into a map and the scalar
  // switch below never sees it.
  if (thunk.returnStructIndex >= 0) {
    emitStructReturn(cg, thunk.structs[static_cast<std::size_t>(thunk.returnStructIndex)],
                     kStructBufBase, tableNew, memberSet);
    cg.addImm32(Rsp, kArgStageBytes + kStructBufBytes);
    cg.pop(R12);
    cg.pop(Rbx);
    epilogue(cg);
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
    case ffi::ReturnClass::CString: {
      // A `const char *` comes back in `rax`. NULL is nil; otherwise the bytes
      // are scanned to the NUL (with the hosted engine's 16 MiB cap) and copied
      // into an `AotString`, which is the AOT representation of text.
      CodeGen::Label null = cg.newLabel();
      CodeGen::Label scanned = cg.newLabel();
      CodeGen::Label scanLoop = cg.newLabel();
      CodeGen::Label returned = cg.newLabel();
      cg.testReg(Rax);
      cg.jcc(Jcc::Eq, null);
      cg.movReg(R12, Rax);      // the C string; r12 survives the `alloc`
      cg.xorReg(Rcx, Rcx);      // the byte count
      cg.bind(scanLoop);
      // `movzxFromMem` takes a displacement, so index through a temporary base.
      cg.movReg(R11, R12);
      cg.addReg(R11, Rcx);
      cg.movzxFromMem(R10, R11, 0);
      cg.testReg(R10);
      cg.jcc(Jcc::Eq, scanned);
      cg.addImm32(Rcx, 1);
      cg.cmpImm32(Rcx, static_cast<std::int32_t>(kMaxCStringBytes));
      cg.jcc(Jcc::B, scanLoop);
      emitFailCall(cg, fail, msgCStringTooLong);
      cg.bind(scanned);
      cg.movReg(Rdi, Rcx);
      cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
      cg.callLabel(allocLabel);
      cg.testReg(Rax);
      cg.jcc(Jcc::Eq, oom);
      cg.movToMem(Rax, 0, Rcx);  // the AotString length
      cg.movReg(Rdi, Rax);
      cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
      cg.movReg(Rsi, R12);
      cg.repMovsb();             // copies rcx bytes (the scan left rcx = length)
      cg.movReg(Rdx, Rax);
      cg.movImm(Rax, kTagStr);
      cg.jmp(returned);
      cg.bind(null);
      cg.movImm(Rax, kTagNil);
      cg.xorReg(Rdx, Rdx);
      cg.bind(returned);
      break;
    }
    default:  // Int
      normalizeIntegerReturn(cg, thunk.returnType);
      cg.movReg(Rdx, Rax);
      cg.movImm(Rax, kTagInt);
      break;
  }

  cg.addImm32(Rsp, kArgStageBytes + kStructBufBytes);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

}  // namespace

void emitAotRuntime(CodeGen& cg, const AotRuntimeHandles& handles,
                    const RuntimeHandles& entry,
                    const std::vector<AotExternThunk>* externThunks, bool hosted,
                    bool hasInput, bool bareMetal) {
  // Constant text, bound after all code so nothing falls through into it.
  std::vector<DataBlob> data;
  auto addText = [&](const char* text) {
    DataBlob blob{cg.newLabel(), std::string(text)};
    data.push_back(std::move(blob));
    return data.back();
  };
  // Copy the blobs out so later `data.push_back` reallocations cannot
  // invalidate a reference held by an emit helper below.
  DataBlob msgTypes = addText("oskel: unsupported operand types\n");
  DataBlob msgPow = addText("oskel: unsupported operand types for '**'\n");
  DataBlob msgDivZero = addText("oskel: division by zero\n");
  DataBlob msgShift = addText("oskel: shift count must be between 0 and 63\n");
  DataBlob msgBit = addText("oskel: unsupported operand types for a bitwise operator\n");
  DataBlob msgNeg = addText("oskel: unsupported operand type for unary '-'\n");
  DataBlob msgBitNot = addText("oskel: unsupported operand type for unary '~'\n");
  DataBlob msgCompare = addText("oskel: cannot compare these values\n");
  DataBlob msgPrint = addText("oskel: cannot print this value\n");
  DataBlob msgOom = addText("oskel: out of memory\n");
  DataBlob msgCallNonFunc = addText("oskel: cannot call a non-function value\n");
  DataBlob msgArity = addText("oskel: wrong number of arguments\n");
  DataBlob msgArrayIndexType = addText("oskel: array index must be an integer\n");
  DataBlob msgIndexRange = addText("oskel: index out of range\n");
  DataBlob msgMapKeyType = addText("oskel: map key must be a string\n");
  DataBlob msgBadIndex = addText("oskel: value is not indexable\n");
  DataBlob msgNoMember = addText("oskel: value has no such member\n");
  DataBlob msgImmutable = addText("oskel: strings are immutable\n");
  DataBlob msgPopArgc = addText("oskel: pop() expects 0 arguments\n");
  DataBlob msgShiftArgc = addText("oskel: shift() expects 0 arguments\n");
  DataBlob msgUnshiftArgc = addText("oskel: unshift() expects 1 argument\n");
  DataBlob msgInsertArgc = addText("oskel: insert() expects 2 arguments\n");
  DataBlob msgRemoveAtArgc = addText("oskel: removeAt() expects 1 argument\n");
  DataBlob msgContainsArgc = addText("oskel: contains() expects 1 argument\n");
  DataBlob msgIndexOfArgc = addText("oskel: indexOf() expects 1 argument\n");
  DataBlob msgClearArgc = addText("oskel: clear() expects 0 arguments\n");
  DataBlob msgArraySliceArgc = addText("oskel: slice() expects between 0 and 2 arguments\n");
  DataBlob msgArraySliceStart = addText("oskel: slice() expects an integer start\n");
  DataBlob msgArraySliceEnd = addText("oskel: slice() expects an integer end\n");
  DataBlob msgConcatArgc = addText("oskel: concat() expects 1 argument\n");
  DataBlob msgConcatType = addText("oskel: concat() expects an array\n");
  DataBlob msgReversedArgc = addText("oskel: reversed() expects 0 arguments\n");
  DataBlob msgCloneArgc = addText("oskel: clone() expects 0 arguments\n");
  // The bound map methods (PHASE 7c.3g). `clear`/`clone` reuse the array
  // messages above, their text being identical.
  DataBlob msgMapKeysArgc = addText("oskel: keys() expects 0 arguments\n");
  DataBlob msgMapValuesArgc = addText("oskel: values() expects 0 arguments\n");
  DataBlob msgMapEntriesArgc = addText("oskel: entries() expects 0 arguments\n");
  DataBlob msgMapHasArgc = addText("oskel: has() expects 1 argument\n");
  DataBlob msgMapHasKeyType = addText("oskel: has() expects a string key\n");
  DataBlob msgMapGetArgc = addText("oskel: get() expects between 1 and 2 arguments\n");
  DataBlob msgMapGetKeyType = addText("oskel: get() expects a string key\n");
  DataBlob msgMapSetArgc = addText("oskel: set() expects 2 arguments\n");
  DataBlob msgMapSetKeyType = addText("oskel: set() expects a string key\n");
  DataBlob msgMapDeleteArgc = addText("oskel: delete() expects 1 argument\n");
  DataBlob msgMapDeleteKeyType = addText("oskel: delete() expects a string key\n");
  DataBlob msgMapMergeArgc = addText("oskel: merge() expects 1 argument\n");
  DataBlob msgMapMergeType = addText("oskel: merge() expects a map\n");
  DataBlob msgInsertRange = addText("oskel: insert() index out of range\n");
  DataBlob msgRemoveAtRange = addText("oskel: removeAt() index out of range\n");
  DataBlob msgBadAssign = addText("oskel: cannot assign to this value\n");
  DataBlob msgBadField = addText("oskel: cannot assign a field on this value\n");
  DataBlob msgBadIn = addText("oskel: unsupported operands for 'in'\n");
  DataBlob msgMapValue = addText("oskel: key/value iteration ('for k, v in ...') requires a map\n");
  DataBlob msgNonNull = addText("oskel: unexpected nil (non-null assertion '!!' failed)\n");
  DataBlob msgStrBad = addText("oskel: cannot render this value to a string\n");
  DataBlob textTrue = addText("true");
  DataBlob textFalse = addText("false");
  DataBlob textNil = addText("nil");
  DataBlob textNewline = addText("\n");
  DataBlob textSpace = addText(" ");
  DataBlob textFunc = addText("<fn>");
  DataBlob textComma = addText(", ");
  DataBlob textColon = addText(": ");
  DataBlob textOpenBracket = addText("[");
  DataBlob textCloseBracket = addText("]");
  DataBlob textOpenBrace = addText("{");
  DataBlob textCloseBrace = addText("}");
  DataBlob textEllipsis = addText("...");
  DataBlob msgRangeBounds = addText("oskel: range bounds must be integers\n");
  DataBlob msgSliceStart = addText("oskel: slice start must be an integer\n");
  DataBlob msgSliceEnd = addText("oskel: slice end must be an integer\n");
  DataBlob msgSliceStep = addText("oskel: slice step must not be zero\n");
  DataBlob msgSliceType = addText("oskel: value is not sliceable\n");
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
  DataBlob msgErrorCodeType = addText("oskel: error() expects an integer code\n");
  DataBlob textError = addText("error(");
  DataBlob textCloseParen = addText(")");
  DataBlob textOskelPrefix = addText("oskel: ");
  DataBlob msgLenType = addText("oskel: len() is not defined for this type\n");
  DataBlob msgOrdType = addText("oskel: ord() expects a string argument\n");
  DataBlob msgOrdChar = addText("oskel: ord() expects a single character\n");
  DataBlob msgChrInt = addText("oskel: chr() expects an integer argument\n");
  DataBlob msgChrRange = addText("oskel: chr() code point out of range\n");
  DataBlob msgChrSurrogate = addText("oskel: chr() code point is a surrogate\n");
  DataBlob msgAssert = addText("oskel: assertion failed\n");
  DataBlob msgPanic = addText("oskel: panic\n");
  DataBlob msgConvertInt = addText("oskel: cannot convert this value to int\n");
  DataBlob msgConvertFloat = addText("oskel: cannot convert this value to float\n");
  DataBlob msgConvertDouble = addText("oskel: cannot convert this value to double\n");
  DataBlob msgFloorType = addText("oskel: floor() expects a number argument\n");
  DataBlob msgCeilType = addText("oskel: ceil() expects a number argument\n");
  DataBlob msgRoundType = addText("oskel: round() expects a number argument\n");
  // The curated `sqrt` stub (PHASE 7f.1). The messages mirror the hosted
  // builtin's, which the CLI prefixes with `file:line:col: runtime error: `.
  DataBlob msgSqrtArgc = addText("oskel: sqrt() expects 1 argument\n");
  DataBlob msgSqrtType = addText("oskel: sqrt() expects a number argument\n");
  DataBlob msgSqrtNegative = addText("oskel: sqrt() of a negative number\n");
  DataBlob msgExpType = addText("oskel: exp() expects a number argument\n");
  DataBlob msgLogType = addText("oskel: log() expects a number argument\n");
  DataBlob msgLogNonPositive = addText("oskel: log() of a non-positive number\n");
  DataBlob msgPowType = addText("oskel: pow() expects a number argument\n");
  DataBlob msgSinType = addText("oskel: sin() expects a number argument\n");
  DataBlob msgCosType = addText("oskel: cos() expects a number argument\n");
  DataBlob msgTanType = addText("oskel: tan() expects a number argument\n");
  // The remaining public builtins (PHASE 7c.5b.5): `range`, `assertEq` and
  // `exit`. `assertEq` reuses the shared `failDynamic` and the recursive repr,
  // so its message is assembled at run time from the two fragments below.
  DataBlob msgRangeFnArgc = addText("oskel: range() expects between 1 and 2 arguments\n");
  DataBlob msgRangeFnStart = addText("oskel: range() expects an integer start\n");
  DataBlob msgRangeFnEnd = addText("oskel: range() expects an integer end\n");
  DataBlob msgAssertEqArgc = addText("oskel: assertEq() expects between 2 and 3 arguments\n");
  DataBlob msgAssertEqDefault = addText("assertion failed");
  DataBlob textExpected = addText(": expected ");
  DataBlob textGot = addText(", got ");
  DataBlob msgExitArgc = addText("oskel: exit() expects between 0 and 1 arguments\n");
  DataBlob msgExitType = addText("oskel: exit() expects an integer argument\n");
  DataBlob msgTimeArgc = addText("oskel: time() expects 0 arguments\n");
  DataBlob msgInputArgc = addText("oskel: input() expects between 0 and 1 arguments\n");
  DataBlob msgMemSizeArgc = addText("oskel: memSize() expects 0 arguments\n");
  // The scalar `extern "C"` thunks (PHASE 7f.5). A direct call is arity- and
  // type-checked at compile time, so these only fire through a first-class
  // reference (`var f = abs; f("x")`). The arity message is the shared one; the
  // type message is added only in the hosted mode, so a freestanding image
  // stays byte for byte what it was.
  DataBlob msgExternType;
  if (externThunks != nullptr) {
    msgExternType = addText("oskel: argument type does not match the extern \"C\" declaration\n");
  }
  // The returned-`cstring` scan cap message, hosted only, for the same
  // byte-identity reason.
  DataBlob msgCStringTooLong;
  if (externThunks != nullptr) {
    msgCStringTooLong = addText(
        "oskel: a C function returned a 'cstring' longer than 16777216 bytes with no NUL "
        "terminator; the host broke the `const char *` contract\n");
  }
  CodeGen::Label fail = cg.newLabel();
  CodeGen::Label failDynamic = cg.newLabel();
  CodeGen::Label writeInt = cg.newLabel();
  CodeGen::Label printRepr = cg.newLabel();
  CodeGen::Label printStrEsc = cg.newLabel();
  CodeGen::Label sbInit = handles.sbInit;
  // Bound at the public handles so a test can drive them directly.
  CodeGen::Label sbAppend = handles.sbAppend;
  CodeGen::Label sbAppendByte = cg.newLabel();
  CodeGen::Label sbNew = handles.sbNew;
  CodeGen::Label sbAppendEscaped = cg.newLabel();
  CodeGen::Label reprInto = cg.newLabel();
  CodeGen::Label reprToString = cg.newLabel();

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
  env.mapFind = cg.newLabel();
  env.blockEnsure = cg.newLabel();
  env.utf8Len = cg.newLabel();
  env.utf8CharAt = cg.newLabel();
  env.compare = handles.compareValues;
  env.rawPrint = handles.printValueRaw;
  env.printRepr = printRepr;
  env.printStrEsc = printStrEsc;
  env.lengthConst = cg.newLabel();
  env.bindArrayMethod = handles.bindArrayMethod;
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
  env.messageConst = cg.newLabel();
  env.codeConst = cg.newLabel();
  env.msgOom = &msgOom;
  env.msgArrayIndexType = &msgArrayIndexType;
  env.msgIndexRange = &msgIndexRange;
  env.msgMapKeyType = &msgMapKeyType;
  env.msgBadIndex = &msgBadIndex;
  env.msgNoMember = &msgNoMember;
  env.msgImmutable = &msgImmutable;
  env.msgBadAssign = &msgBadAssign;
  env.msgBadField = &msgBadField;
  env.msgBadIn = &msgBadIn;
  env.msgMapValue = &msgMapValue;
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

  emitFail(cg, fail, entry.write, entry.exit);
  emitWriteInt(cg, writeInt, entry.write);
  emitAlloc(cg, handles.alloc);
  emitConcat(cg, handles.concat, handles.alloc, fail, msgOom);
  emitArrayConcat(cg, handles.arrayConcat, handles.alloc, env.blockEnsure, fail, msgOom);
  emitBindArrayMethod(cg, handles.bindArrayMethod, handles.cellNew, handles.closure,
                      {handles.arrayPushMethod, handles.arrayPopMethod,
                       handles.arrayShiftMethod, handles.arrayUnshiftMethod,
                       handles.arrayInsertMethod, handles.arrayRemoveAtMethod,
                       handles.arrayContainsMethod, handles.arrayIndexOfMethod,
                       handles.arrayClearMethod, handles.arraySliceMethod,
                       handles.arrayConcatMethod, handles.arrayReversedMethod,
                       handles.arrayCloneMethod});
  emitArrayPushMethod(cg, handles.arrayPushMethod, env);
  emitArrayPopMethod(cg, handles.arrayPopMethod, env, msgPopArgc);
  emitArrayShiftMethod(cg, handles.arrayShiftMethod, env, msgShiftArgc);
  emitArrayUnshiftMethod(cg, handles.arrayUnshiftMethod, env, msgUnshiftArgc);
  emitArrayInsertMethod(cg, handles.arrayInsertMethod, env, msgInsertArgc,
                        msgArrayIndexType, msgInsertRange);
  emitArrayRemoveAtMethod(cg, handles.arrayRemoveAtMethod, env, msgRemoveAtArgc,
                          msgArrayIndexType, msgRemoveAtRange);
  emitArrayContainsMethod(cg, handles.arrayContainsMethod, env, msgContainsArgc);
  emitArrayIndexOfMethod(cg, handles.arrayIndexOfMethod, env, msgIndexOfArgc);
  emitArrayClearMethod(cg, handles.arrayClearMethod, env, msgClearArgc);
  emitArraySliceMethod(cg, handles.arraySliceMethod, env, msgArraySliceArgc,
                       msgArraySliceStart, msgArraySliceEnd);
  emitArrayConcatMethod(cg, handles.arrayConcatMethod, env, msgConcatArgc, msgConcatType);
  emitArrayReversedMethod(cg, handles.arrayReversedMethod, env, msgReversedArgc);
  emitArrayCloneMethod(cg, handles.arrayCloneMethod, env, msgCloneArgc);
  // The bound map methods (PHASE 7c.3g). The binder shares `emitBindArrayMethod`
  // (the mechanism is identical); the ten bodies mirror the hosted `bindMap`.
  emitBindArrayMethod(cg, handles.bindMapMethod, handles.cellNew, handles.closure,
                      {handles.mapKeysMethod, handles.mapValuesMethod,
                       handles.mapEntriesMethod, handles.mapHasMethod,
                       handles.mapGetMethod, handles.mapSetMethod,
                       handles.mapDeleteMethod, handles.mapClearMethod,
                       handles.mapMergeMethod, handles.mapCloneMethod});
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
  emitInitGlobals(cg, handles.initGlobals, handles.alloc, fail, msgOom);
  emitGlobalSet(cg, handles.globalSet);
  emitGlobalGet(cg, handles.globalGet);
  emitClosure(cg, handles.closure, handles.alloc, fail, msgOom);
  emitCellNew(cg, handles.cellNew, handles.alloc, fail, msgOom);
  emitCall(cg, handles.call, fail, msgCallNonFunc);
  emitArityError(cg, handles.arityError, fail, msgArity);
  if (externThunks != nullptr) {
    for (const AotExternThunk& thunk : *externThunks) {
      emitExternThunk(cg, thunk, fail, handles.alloc, handles.memberGet, handles.tableNew,
                      handles.memberSet, msgArity, msgExternType, msgOom, msgCStringTooLong);
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
  emitSbAppendEscaped(cg, sbAppendEscaped, sbAppend, sbAppendByte, textQuote,
                      textEscQuote, textEscBackslash, textEscNL, textEscTab,
                      textEscCR);
  emitReprInto(cg, reprInto, sbAppend, sbAppendEscaped, dtoaHandles.formatDouble,
               textTrue, textFalse, textNil, textFunc, textComma, textColon,
               textOpenBracket, textCloseBracket, textOpenBrace, textCloseBrace,
               textEllipsis, textError, textCloseParen);
  emitReprToString(cg, reprToString, sbNew, reprInto, handles.alloc, fail, msgOom);
  emitPrintRepr(cg, printRepr, entry.write, sbInit, reprInto);
  emitPrintValue(cg, handles.printValue, entry.write, fail, writeInt,
                 dtoaHandles.formatDouble, printRepr, msgPrint, textTrue, textFalse,
                 textNil, textNewline, hosted);
  emitPrintValueRaw(cg, handles.printValueRaw, entry.write, fail, writeInt,
                    dtoaHandles.formatDouble, printRepr, msgPrint, textTrue, textFalse,
                    textNil, textNewline, hosted);
  emitPrintValueErrRaw(cg, handles.printValueErrRaw, entry.write, fail, writeInt,
                       dtoaHandles.formatDouble, printRepr, msgPrint, textTrue,
                       textFalse, textNil, textNewline, hosted);
  emitPrintBuiltin(cg, handles.printBuiltin, handles.printValueRaw, entry.write,
                   textSpace, textNewline, /*fd=*/1, /*newline=*/true);
  emitPrintBuiltin(cg, handles.writeBuiltin, handles.printValueRaw, entry.write,
                   textSpace, textNewline, /*fd=*/1, /*newline=*/false);
  emitPrintBuiltin(cg, handles.eprintBuiltin, handles.printValueErrRaw, entry.write,
                   textSpace, textNewline, /*fd=*/2, /*newline=*/true);
  emitTruthy(cg, handles.truthy);
  emitArith(cg, handles.arithValues, fail, msgTypes, msgDivZero, handles.concat,
            handles.arrayConcat);
  emitShift(cg, handles.shiftValues, fail, msgShift);
  emitBitwise(cg, handles.bitwiseValues, fail, msgBit);
  emitNegate(cg, handles.negateValue, fail, msgNeg);
  emitBitNot(cg, handles.bitNotValue, fail, msgBitNot);
  emitNot(cg, handles.notValue);
  emitCompare(cg, handles.compareValues, fail, msgCompare, env.mapFind);
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
  emitIsErrorBuiltin(cg, handles.isErrorBuiltin);
  emitErrorBuiltin(cg, handles.errorBuiltin, handles.alloc, fail, handles.strBuiltin,
                   msgOom, msgErrorCodeType, msgArity);
  emitNonNullBuiltin(cg, handles.nonNullBuiltin, fail, msgNonNull);
  emitStrBuiltin(cg, handles.strBuiltin, handles.alloc, fail,
                 dtoaHandles.formatDouble, reprToString, msgOom, textTrue, textFalse,
                 textNil, msgStrBad);
  emitRangeBuiltin(cg, handles.rangeBuiltin, handles.alloc, fail, msgOom,
                   msgRangeBounds);
  emitSliceBuiltin(cg, handles.sliceBuiltin, env, msgSliceStart, msgSliceEnd,
                   msgSliceStep, msgSliceType);
  // The public builtins (PHASE 7c.5a). `str` and `isError` reuse the `$str` and
  // `$isError` code: the public names are just two more seeds of the same entry.
  emitFailDynamic(cg, failDynamic, entry.write, entry.exit, textOskelPrefix, textNewline);
  emitLenBuiltin(cg, handles.lenBuiltin, env, msgLenType);
  emitTypeBuiltin(cg, handles.typeBuiltin, handles.alloc, fail, msgOom, typeNameNil,
                  typeNameBool, typeNameInt, typeNameFloat, typeNameDouble, typeNameStr,
                  typeNameArray, typeNameMap, typeNameFunc, typeNameRange, typeNameError);
  emitReprBuiltin(cg, handles.reprBuiltin, reprToString);
  emitBoolBuiltin(cg, handles.boolBuiltin, handles.truthy);
  emitOrdBuiltin(cg, handles.ordBuiltin, env.utf8Len, fail, msgOrdType, msgOrdChar);
  emitChrBuiltin(cg, handles.chrBuiltin, handles.alloc, fail, msgChrInt, msgChrRange,
                 msgChrSurrogate, msgOom);
  emitAssertBuiltin(cg, handles.assertBuiltin, handles.truthy, fail, failDynamic,
                    msgAssert);
  emitPanicBuiltin(cg, handles.panicBuiltin, fail, failDynamic, msgPanic);
  emitNumericConversionBuiltins(cg, handles.intBuiltin, handles.floatBuiltin,
                                handles.doubleBuiltin, fail, handles.intTextParser,
                                parseFloatHandles.parseDouble, msgConvertInt,
                                msgConvertFloat, msgConvertDouble);
  emitRoundingBuiltin(cg, handles.floorBuiltin, fail, msgFloorType, AotRoundMode::Floor);
  emitRoundingBuiltin(cg, handles.ceilBuiltin, fail, msgCeilType, AotRoundMode::Ceil);
  emitRoundingBuiltin(cg, handles.roundBuiltin, fail, msgRoundType, AotRoundMode::Round);
  emitSqrtBuiltin(cg, handles.sqrtBuiltin, fail, msgSqrtArgc, msgSqrtType,
                  msgSqrtNegative);
  // The transcendental builtins (PHASE 7c.5b.4a). The three routines share one
  // set of tables; `mathlib::emitMathLib` emits them plus the wrappers' targets.
  mathlib::MathHandles math = mathlib::newMathHandles(cg);
  mathlib::emitMathLib(cg, math);
  // `**` shares the embedded `pow` with the `pow` builtin.
  emitPow(cg, handles.powValues, fail, msgPow, math.pow);
  emitTranscendentalBuiltin(cg, handles.expBuiltin, fail, math.exp, msgExpType, false,
                            msgLogNonPositive);
  emitTranscendentalBuiltin(cg, handles.logBuiltin, fail, math.log, msgLogType, true,
                            msgLogNonPositive);
  emitPowBuiltin(cg, handles.powBuiltin, fail, math.pow, msgPowType);
  // The trigonometric builtins (PHASE 7c.5b.4b). `sin`/`cos`/`tan` share the
  // `__rem_pio2` reduction and the `__sin`/`__cos` kernels; `emitMathLib` already
  // emitted them above.
  emitTranscendentalBuiltin(cg, handles.sinBuiltin, fail, math.sin, msgSinType, false,
                            msgLogNonPositive);
  emitTranscendentalBuiltin(cg, handles.cosBuiltin, fail, math.cos, msgCosType, false,
                            msgLogNonPositive);
  emitTranscendentalBuiltin(cg, handles.tanBuiltin, fail, math.tan, msgTanType, false,
                            msgLogNonPositive);
  // The remaining public builtins (PHASE 7c.5b.5): `range`, `assertEq` and
  // `exit`. `range` reuses the arena allocator; `assertEq` reuses the shared
  // comparison, the recursive repr and the string builder; `exit` leaves
  // through the entry prelude.
  emitRangeFnBuiltin(cg, handles.rangeFnBuiltin, handles.alloc, fail, msgRangeFnArgc,
                     msgRangeFnStart, msgRangeFnEnd, msgOom);
  emitAssertEqBuiltin(cg, handles.assertEqBuiltin, handles.compareValues, reprToString,
                      sbNew, sbAppend, failDynamic, fail, msgAssertEqArgc,
                      msgAssertEqDefault, textExpected, textGot, msgOom);
  emitExitBuiltin(cg, handles.exitBuiltin, entry.exit, fail, msgExitArgc, msgExitType);
  emitTimeBuiltin(cg, handles.timeBuiltin, entry.now, fail, msgTimeArgc);
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
    emitMemSizeBuiltin(cg, handles.memSizeBuiltin, entry.memSize, fail, msgMemSizeArgc);
  }
  emitIntTextParser(cg, handles.intTextParser, fail, msgConvertInt);
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

  // Data islands last: every code path above ends in `ret` or `ud2`, so
  // control never reaches them, and their addresses are already referenced by
  // the `lea` fixups emitted above.
  for (const DataBlob& blob : data) {
    cg.bind(blob.label);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(blob.text.data()),
                 blob.text.size());
  }
}
