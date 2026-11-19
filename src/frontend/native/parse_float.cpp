#include "native/parse_float.h"

#include <cstddef>

// Emitter for the freestanding text-to-binary64 parser (PHASE 7c.5b.2b).
//
// Register conventions. The routine keeps the scratch block in rbx (the
// `bn_*` helpers preserve it), the cursor in r12, the end pointer in r13 and
// the caller's output word in r14; r12-r14 are saved on entry because r13/r14
// hold the program's arena state in the surrounding runtime. Every other value
// lives in a `ParseScratch` field and is reloaded after a helper call, because
// the helpers clobber rax, rcx, rdx, rsi, rdi and r8-r11.
namespace parse_float {
namespace {

using X64Reg = ::X64Reg;
using Jcc = ::Jcc;

constexpr X64Reg Rax = X64Reg::Rax;
constexpr X64Reg Rcx = X64Reg::Rcx;
constexpr X64Reg Rdx = X64Reg::Rdx;
constexpr X64Reg Rbx = X64Reg::Rbx;
constexpr X64Reg Rsp = X64Reg::Rsp;
constexpr X64Reg Rbp = X64Reg::Rbp;
constexpr X64Reg Rsi = X64Reg::Rsi;
constexpr X64Reg Rdi = X64Reg::Rdi;
constexpr X64Reg R8 = X64Reg::R8;
constexpr X64Reg R9 = X64Reg::R9;
constexpr X64Reg R10 = X64Reg::R10;
constexpr X64Reg R11 = X64Reg::R11;
constexpr X64Reg R12 = X64Reg::R12;
constexpr X64Reg R13 = X64Reg::R13;
constexpr X64Reg R14 = X64Reg::R14;

constexpr std::int64_t kWO = static_cast<std::int64_t>(offsetof(ParseBignum, w));

#define O(name) static_cast<std::int64_t>(offsetof(ParseScratch, name))
constexpr std::int64_t offDigits = O(digits);
constexpr std::int64_t offOut = O(out);
constexpr std::int64_t offNeg = O(neg);
constexpr std::int64_t offTotal = O(total);
constexpr std::int64_t offFrac = O(frac);
constexpr std::int64_t offExtra = O(extra);
constexpr std::int64_t offSticky = O(sticky);
constexpr std::int64_t offExp10 = O(exp10);
constexpr std::int64_t offPexp = O(pexp);
constexpr std::int64_t offNet = O(net);
constexpr std::int64_t offSExp = O(sExp);
constexpr std::int64_t offBits = O(bits);
constexpr std::int64_t offSeenDot = O(seenDot);
constexpr std::int64_t offSeenDigit = O(seenDigit);
constexpr std::int64_t offStarted = O(started);
constexpr std::int64_t offAnyHex = O(anyHex);
constexpr std::int64_t offFracHex = O(fracHex);
constexpr std::int64_t offTmp = O(tmp);
constexpr std::int64_t offL = O(L);
constexpr std::int64_t offE = O(E);
constexpr std::int64_t offUlp = O(ulp);
constexpr std::int64_t offShift = O(shift);
constexpr std::int64_t offRb = O(rb);
constexpr std::int64_t offSt = O(st);
constexpr std::int64_t offT2 = O(t2);
constexpr std::int64_t offI = O(i);
constexpr std::int64_t offCnt = O(cnt);
#undef O

constexpr std::int64_t kMaxDigits = static_cast<std::int64_t>(parse_float::kMaxDigits);
constexpr std::int64_t kMaxHexDigits = static_cast<std::int64_t>(parse_float::kMaxHexDigits);

// log2(10) in 16.16 fixed point (217706 / 65536 = 3.32193).
constexpr std::int64_t kLog2TenQ16 = 217706;
constexpr std::int64_t kExpClamp = 1000000000;

void setRdi(CodeGen& cg, std::int64_t off) {
  cg.movReg(Rdi, Rbx);
  if (off != 0) cg.addImm32(Rdi, static_cast<std::int32_t>(off));
}

// --- Emitted bignum subroutines -------------------------------------------

// void bn_add_small(Bignum* bn, long v): bn += v, v >= 0 and < 2^32.
void emitBnAddSmall(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label grow = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.bind(label);
  cg.movFromMem(Rax, Rdi, 0);  // n
  cg.movReg(R9, Rdi);
  cg.addImm32(R9, static_cast<std::int32_t>(kWO));  // &w[0]
  cg.xorReg(Rcx, Rcx);                              // i
  cg.movReg(R8, Rsi);                               // carry
  cg.bind(loop);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, done);
  cg.cmpReg(Rcx, Rax);
  cg.jcc(Jcc::Ae, grow);
  cg.movReg(Rdx, Rcx);
  cg.shlImm8(Rdx, 3);
  cg.addReg(Rdx, R9);
  cg.movFromMem32(R10, Rdx, 0);
  cg.addReg(R10, R8);
  cg.movToMem32(Rdx, 0, R10);
  cg.movReg(R8, R10);
  cg.shrImm8(R8, 32);
  cg.addImm32(Rcx, 1);
  cg.jmp(loop);
  cg.bind(grow);
  cg.movReg(Rdx, Rcx);
  cg.shlImm8(Rdx, 3);
  cg.addReg(Rdx, R9);
  cg.movToMem32(Rdx, 0, R8);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rdi, 0, Rax);
  cg.bind(done);
  cg.retInsn();
}

// long bn_bits(const Bignum* bn) -> rax: the bit length (0 for zero).
void emitBnBits(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label done = cg.newLabel();
  cg.bind(label);
  cg.movFromMem(Rax, Rdi, 0);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, done);
  cg.movReg(Rdx, Rax);
  cg.subRegImm32(Rdx, 1);
  cg.shlImm8(Rdx, 3);
  cg.addReg(Rdx, Rdi);
  cg.addImm32(Rdx, static_cast<std::int32_t>(kWO));
  cg.movFromMem32(Rcx, Rdx, 0);  // top limb
  cg.bsrReg(R8, Rcx);
  cg.subRegImm32(Rax, 1);
  cg.shlImm8(Rax, 5);
  cg.addImm32(R8, 1);
  cg.addReg(Rax, R8);
  cg.bind(done);
  cg.retInsn();
}

// long bn_bit(const Bignum* bn, long i) -> rax: bit i (0 or 1).
void emitBnBit(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label zero = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.bind(label);
  cg.movReg(Rax, Rsi);
  cg.shrImm8(Rax, 5);  // limb index
  cg.movFromMem(Rdx, Rdi, 0);
  cg.cmpReg(Rax, Rdx);
  cg.jcc(Jcc::Ae, zero);
  cg.shlImm8(Rax, 3);
  cg.addReg(Rax, Rdi);
  cg.addImm32(Rax, static_cast<std::int32_t>(kWO));
  cg.movFromMem32(Rax, Rax, 0);
  cg.movReg(Rcx, Rsi);
  cg.andImm32(Rcx, 31);
  cg.shrCl(Rax);
  cg.andImm32(Rax, 1);
  cg.jmp(done);
  cg.bind(zero);
  cg.xorReg(Rax, Rax);
  cg.bind(done);
  cg.retInsn();
}

// --- The parser ------------------------------------------------------------

void emitParse(CodeGen& cg, CodeGen::Label label, CodeGen::Label addSmall,
               CodeGen::Label bitsFn, CodeGen::Label bitFn,
               const dtoa::DtoaHandles& h) {
  CodeGen::Label trimL = cg.newLabel();
  CodeGen::Label trimLSkip = cg.newLabel();
  CodeGen::Label trimR = cg.newLabel();
  CodeGen::Label trimRSkip = cg.newLabel();
  CodeGen::Label trimRDone = cg.newLabel();
  CodeGen::Label nulLoop = cg.newLabel();
  CodeGen::Label nulDone = cg.newLabel();
  CodeGen::Label afterSign = cg.newLabel();
  CodeGen::Label consumeSign = cg.newLabel();
  CodeGen::Label len3 = cg.newLabel();
  CodeGen::Label len8 = cg.newLabel();
  CodeGen::Label maybeInf3 = cg.newLabel();
  CodeGen::Label maybeNan3 = cg.newLabel();
  CodeGen::Label isInf = cg.newLabel();
  CodeGen::Label isNan = cg.newLabel();
  CodeGen::Label noSpecial = cg.newLabel();
  CodeGen::Label notHex = cg.newLabel();
  CodeGen::Label hexLoop = cg.newLabel();
  CodeGen::Label hexEntry = cg.newLabel();
  CodeGen::Label hexStore = cg.newLabel();
  CodeGen::Label hexStoreExtra = cg.newLabel();
  CodeGen::Label hexStoreNoStart = cg.newLabel();
  CodeGen::Label hexDigitReady = cg.newLabel();
  CodeGen::Label hexAfterDot = cg.newLabel();
  CodeGen::Label hexNoFrac = cg.newLabel();
  CodeGen::Label hexBuildStart = cg.newLabel();
  CodeGen::Label decStore = cg.newLabel();
  CodeGen::Label decStoreExtra = cg.newLabel();
  CodeGen::Label decStoreNoStart = cg.newLabel();
  CodeGen::Label roundNegSkip = cg.newLabel();
  CodeGen::Label hexDot = cg.newLabel();
  CodeGen::Label hexDigit = cg.newLabel();
  CodeGen::Label hexNotDigit = cg.newLabel();
  CodeGen::Label hexMantDone = cg.newLabel();
  CodeGen::Label hexExp = cg.newLabel();
  CodeGen::Label hexExpSign = cg.newLabel();
  CodeGen::Label hexExpDigits = cg.newLabel();
  CodeGen::Label hexExpLoop = cg.newLabel();
  CodeGen::Label hexExpCont = cg.newLabel();
  CodeGen::Label hexExpDone = cg.newLabel();
  CodeGen::Label hexExpStore = cg.newLabel();
  CodeGen::Label hexBuild = cg.newLabel();
  CodeGen::Label hexZero = cg.newLabel();
  CodeGen::Label hexBuildLoop = cg.newLabel();
  CodeGen::Label hexBuildDone = cg.newLabel();
  CodeGen::Label decLoop = cg.newLabel();
  CodeGen::Label decDot = cg.newLabel();
  CodeGen::Label decDigit = cg.newLabel();
  CodeGen::Label decAfterDot = cg.newLabel();
  CodeGen::Label decNoFrac = cg.newLabel();
  CodeGen::Label decMantDone = cg.newLabel();
  CodeGen::Label decExp = cg.newLabel();
  CodeGen::Label decExpSign = cg.newLabel();
  CodeGen::Label decExpDigits = cg.newLabel();
  CodeGen::Label decExpLoop = cg.newLabel();
  CodeGen::Label decExpCont = cg.newLabel();
  CodeGen::Label decExpDone = cg.newLabel();
  CodeGen::Label decNoExp = cg.newLabel();
  CodeGen::Label decExpStore = cg.newLabel();
  CodeGen::Label decBuild = cg.newLabel();
  CodeGen::Label decTrim = cg.newLabel();
  CodeGen::Label decTrimDone = cg.newLabel();
  CodeGen::Label decDigitsLoop = cg.newLabel();
  CodeGen::Label decDigitsDone = cg.newLabel();
  CodeGen::Label decNeg = cg.newLabel();
  CodeGen::Label decKOk = cg.newLabel();
  CodeGen::Label decMul5Loop = cg.newLabel();
  CodeGen::Label decMul5Done = cg.newLabel();
  CodeGen::Label decDiv10Loop = cg.newLabel();
  CodeGen::Label decDiv10Done = cg.newLabel();
  CodeGen::Label round = cg.newLabel();
  CodeGen::Label roundZero = cg.newLabel();
  CodeGen::Label roundUlpSub = cg.newLabel();
  CodeGen::Label roundUlpDone = cg.newLabel();
  CodeGen::Label roundPosLoop = cg.newLabel();
  CodeGen::Label roundPosNext = cg.newLabel();
  CodeGen::Label roundPosDone = cg.newLabel();
  CodeGen::Label roundNegLoop = cg.newLabel();
  CodeGen::Label roundNegNext = cg.newLabel();
  CodeGen::Label roundNegDone = cg.newLabel();
  CodeGen::Label roundStLoop = cg.newLabel();
  CodeGen::Label roundStSet = cg.newLabel();
  CodeGen::Label roundStDone = cg.newLabel();
  CodeGen::Label roundDecide = cg.newLabel();
  CodeGen::Label roundInc = cg.newLabel();
  CodeGen::Label roundNoInc = cg.newLabel();
  CodeGen::Label pack = cg.newLabel();
  CodeGen::Label packNoBump = cg.newLabel();
  CodeGen::Label packNormal = cg.newLabel();
  CodeGen::Label packSub = cg.newLabel();
  CodeGen::Label emitSign = cg.newLabel();
  CodeGen::Label emitDone = cg.newLabel();
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label epilogue = cg.newLabel();

  constexpr std::int64_t kTwoPow52 = 0x10000000000000LL;
  constexpr std::int64_t kTwoPow53 = 0x20000000000000LL;

  cg.bind(label);
  cg.push(Rbp);
  cg.movReg(Rbp, Rsp);
  cg.push(Rbx);
  cg.push(R12);
  cg.push(R13);
  cg.push(R14);
  cg.movReg(Rbx, Rcx);  // caller's scratch block
  cg.movToMem(Rbx, offOut, Rdx);
  cg.addReg(Rsi, Rdi);   // rsi = end
  cg.movReg(R12, Rdi);   // cursor
  cg.movReg(R13, Rsi);

  // Trim leading whitespace (space, \t, \n, \v, \f, \r), like the hosted trim.
  cg.bind(trimL);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, trimRDone);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, 32); cg.jcc(Jcc::Eq, trimLSkip);
  cg.cmpImm32(Rcx, 9);  cg.jcc(Jcc::Eq, trimLSkip);
  cg.cmpImm32(Rcx, 10); cg.jcc(Jcc::Eq, trimLSkip);
  cg.cmpImm32(Rcx, 11); cg.jcc(Jcc::Eq, trimLSkip);
  cg.cmpImm32(Rcx, 12); cg.jcc(Jcc::Eq, trimLSkip);
  cg.cmpImm32(Rcx, 13); cg.jcc(Jcc::Eq, trimLSkip);
  cg.jmp(trimR);
  cg.bind(trimLSkip);
  cg.addImm32(R12, 1);
  cg.jmp(trimL);

  // Trim trailing whitespace.
  cg.bind(trimR);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, trimRDone);
  cg.movzxFromMem(Rcx, R13, -1);
  cg.cmpImm32(Rcx, 32); cg.jcc(Jcc::Eq, trimRSkip);
  cg.cmpImm32(Rcx, 9);  cg.jcc(Jcc::Eq, trimRSkip);
  cg.cmpImm32(Rcx, 10); cg.jcc(Jcc::Eq, trimRSkip);
  cg.cmpImm32(Rcx, 11); cg.jcc(Jcc::Eq, trimRSkip);
  cg.cmpImm32(Rcx, 12); cg.jcc(Jcc::Eq, trimRSkip);
  cg.cmpImm32(Rcx, 13); cg.jcc(Jcc::Eq, trimRSkip);
  cg.jmp(trimRDone);
  cg.bind(trimRSkip);
  cg.addImm32(R13, -1);
  cg.jmp(trimR);

  // Cut at the first embedded NUL: `strtod` stops there and the hosted engine
  // ignores the trailing bytes (`double("1\u0000abc")` is 1).
  cg.bind(trimRDone);
  cg.movReg(Rcx, R12);
  cg.bind(nulLoop);
  cg.cmpReg(Rcx, R13);
  cg.jcc(Jcc::Ae, nulDone);
  cg.movzxFromMem(Rax, Rcx, 0);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, nulDone);
  cg.addImm32(Rcx, 1);
  cg.jmp(nulLoop);
  cg.bind(nulDone);
  cg.movReg(R13, Rcx);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, bad);  // empty text

  // Optional sign.
  cg.movImmToMem(Rbx, offNeg, 0);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, '+');
  cg.jcc(Jcc::Eq, consumeSign);
  cg.cmpImm32(Rcx, '-');
  cg.jcc(Jcc::Ne, afterSign);
  cg.movImmToMem(Rbx, offNeg, 1);
  cg.bind(consumeSign);
  cg.addImm32(R12, 1);
  cg.bind(afterSign);

  // `inf`/`infinity`/`nan`, case-insensitive, on the remaining bytes.
  cg.movReg(Rax, R13);
  cg.subReg(Rax, R12);
  cg.cmpImm32(Rax, 3);
  cg.jcc(Jcc::Eq, len3);
  cg.cmpImm32(Rax, 8);
  cg.jcc(Jcc::Eq, len8);
  cg.jmp(noSpecial);
  cg.bind(len3);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.movImm(R8, 0x20);
  cg.orReg(Rcx, R8);
  cg.cmpImm32(Rcx, 'i');
  cg.jcc(Jcc::Eq, maybeInf3);
  cg.cmpImm32(Rcx, 'n');
  cg.jcc(Jcc::Eq, maybeNan3);
  cg.jmp(noSpecial);
  cg.bind(maybeInf3);
  cg.movzxFromMem(Rcx, R12, 1);
  cg.orReg(Rcx, R8);
  cg.cmpImm32(Rcx, 'n');
  cg.jcc(Jcc::Ne, noSpecial);
  cg.movzxFromMem(Rcx, R12, 2);
  cg.orReg(Rcx, R8);
  cg.cmpImm32(Rcx, 'f');
  cg.jcc(Jcc::Eq, isInf);
  cg.jmp(noSpecial);
  cg.bind(maybeNan3);
  cg.movzxFromMem(Rcx, R12, 1);
  cg.orReg(Rcx, R8);
  cg.cmpImm32(Rcx, 'a');
  cg.jcc(Jcc::Ne, noSpecial);
  cg.movzxFromMem(Rcx, R12, 2);
  cg.orReg(Rcx, R8);
  cg.cmpImm32(Rcx, 'n');
  cg.jcc(Jcc::Eq, isNan);
  cg.jmp(noSpecial);
  cg.bind(len8);
  {
    static const char kInf[8] = {'i', 'n', 'f', 'i', 'n', 'i', 't', 'y'};
    for (int i = 0; i < 8; i++) {
      cg.movzxFromMem(Rcx, R12, i);
      cg.movImm(R8, 0x20);
      cg.orReg(Rcx, R8);
      cg.cmpImm32(Rcx, kInf[i]);
      cg.jcc(Jcc::Ne, noSpecial);
    }
  }
  cg.bind(isInf);
  cg.movImm(R9, 0x7FF0000000000000LL);
  cg.movToMem(Rbx, offBits, R9);
  cg.jmp(emitSign);
  cg.bind(isNan);
  cg.movImm(R9, 0x7FF8000000000000LL);
  cg.movToMem(Rbx, offBits, R9);
  cg.jmp(emitSign);

  // Hex or decimal path?
  cg.bind(noSpecial);
  cg.leaMem(Rcx, R12, 2);
  cg.cmpReg(Rcx, R13);
  cg.jcc(Jcc::A, notHex);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, '0');
  cg.jcc(Jcc::Ne, notHex);
  cg.movzxFromMem(Rcx, R12, 1);
  cg.cmpImm32(Rcx, 'x');
  cg.jcc(Jcc::Eq, hexEntry);
  cg.cmpImm32(Rcx, 'X');
  cg.jcc(Jcc::Eq, hexEntry);
  cg.jmp(notHex);

  // --- Hex: 0x<hexdigits>[.<hexdigits>][p<exp>] ---
  cg.bind(hexEntry);
  cg.movImmToMem(Rbx, offAnyHex, 0);
  cg.movImmToMem(Rbx, offFracHex, 0);
  cg.movImmToMem(Rbx, offSeenDot, 0);
  cg.movImmToMem(Rbx, offStarted, 0);
  cg.movImmToMem(Rbx, offTotal, 0);
  cg.movImmToMem(Rbx, offExtra, 0);
  cg.movImmToMem(Rbx, offSticky, 0);
  cg.movImmToMem(Rbx, offPexp, 0);
  cg.addImm32(R12, 2);  // skip "0x"
  cg.bind(hexLoop);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, hexMantDone);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, '.');
  cg.jcc(Jcc::Eq, hexDot);
  // Hex digit? Compute its value in r11 (a-f/A-F -> 10..15).
  cg.movReg(R11, Rcx);
  cg.cmpImm32(Rcx, '0'); cg.jcc(Jcc::Lt, hexNotDigit);
  cg.cmpImm32(Rcx, '9'); cg.jcc(Jcc::Le, hexDigit);
  cg.movReg(R9, Rcx);
  cg.movImm(R10, 0x20);
  cg.orReg(R9, R10);
  cg.cmpImm32(R9, 'a'); cg.jcc(Jcc::Lt, hexNotDigit);
  cg.cmpImm32(R9, 'f'); cg.jcc(Jcc::Gt, hexNotDigit);
  cg.movReg(R11, R9);
  cg.subRegImm32(R11, 'a');
  cg.addImm32(R11, 10);
  cg.jmp(hexDigit);
  cg.bind(hexDot);
  cg.movFromMem(Rax, Rbx, offSeenDot);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, hexMantDone);
  cg.movImmToMem(Rbx, offSeenDot, 1);
  cg.addImm32(R12, 1);
  cg.jmp(hexLoop);
  cg.bind(hexDigit);
  cg.movImmToMem(Rbx, offAnyHex, 1);
  // Digit value: rcx <= '9' -> rcx-'0', else r11 is already 10..15.
  cg.cmpImm32(Rcx, '9');
  cg.jcc(Jcc::Gt, hexDigitReady);
  cg.movReg(R11, Rcx);
  cg.subRegImm32(R11, '0');
  cg.bind(hexDigitReady);
  cg.movFromMem(Rax, Rbx, offStarted);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, hexStore);
  cg.testReg(R11);
  cg.jcc(Jcc::Ne, hexStore);
  cg.jmp(hexAfterDot);  // leading zero
  cg.bind(hexStore);
  cg.testReg(R11);
  cg.jcc(Jcc::Eq, hexStoreNoStart);
  cg.movImmToMem(Rbx, offStarted, 1);
  cg.bind(hexStoreNoStart);
  cg.movFromMem(Rax, Rbx, offTotal);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kMaxHexDigits));
  cg.jcc(Jcc::Ae, hexStoreExtra);
  cg.movReg(Rdx, Rax);
  cg.addReg(Rdx, Rbx);
  cg.addImm32(Rdx, static_cast<std::int32_t>(offDigits));
  cg.movByteToMem(Rdx, 0, R11);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offTotal, Rax);
  cg.jmp(hexAfterDot);
  cg.bind(hexStoreExtra);
  cg.movFromMem(Rax, Rbx, offExtra);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offExtra, Rax);
  cg.testReg(R11);
  cg.jcc(Jcc::Eq, hexAfterDot);
  cg.movImmToMem(Rbx, offSticky, 1);
  cg.bind(hexAfterDot);
  cg.movFromMem(Rax, Rbx, offSeenDot);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, hexNoFrac);
  cg.movFromMem(Rax, Rbx, offFracHex);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offFracHex, Rax);
  cg.bind(hexNoFrac);
  cg.addImm32(R12, 1);
  cg.jmp(hexLoop);
  cg.bind(hexNotDigit);
  cg.jmp(hexMantDone);

  cg.bind(hexMantDone);
  cg.movFromMem(Rax, Rbx, offAnyHex);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, bad);  // no hex digits
  cg.movFromMem(Rax, Rbx, offStarted);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, hexZero);  // mantissa is zero
  // Optional binary exponent.
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, hexBuild);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, 'p');
  cg.jcc(Jcc::Eq, hexExp);
  cg.cmpImm32(Rcx, 'P');
  cg.jcc(Jcc::Eq, hexExp);
  cg.jmp(hexBuild);
  cg.bind(hexExp);
  cg.addImm32(R12, 1);
  cg.movImmToMem(Rbx, offTmp, 0);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, bad);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, '+');
  cg.jcc(Jcc::Eq, hexExpSign);
  cg.cmpImm32(Rcx, '-');
  cg.jcc(Jcc::Ne, hexExpDigits);
  cg.movImmToMem(Rbx, offTmp, 1);
  cg.bind(hexExpSign);
  cg.addImm32(R12, 1);
  cg.bind(hexExpDigits);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, bad);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, '0'); cg.jcc(Jcc::Lt, bad);
  cg.cmpImm32(Rcx, '9'); cg.jcc(Jcc::Gt, bad);
  cg.xorReg(R9, R9);
  cg.bind(hexExpLoop);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, hexExpDone);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, '0'); cg.jcc(Jcc::Lt, hexExpDone);
  cg.cmpImm32(Rcx, '9'); cg.jcc(Jcc::Gt, hexExpDone);
  cg.subRegImm32(Rcx, '0');
  cg.imulImm32(R9, R9, 10);
  cg.addReg(R9, Rcx);
  cg.movImm(R10, kExpClamp);
  cg.cmpReg(R9, R10);
  cg.jcc(Jcc::Le, hexExpCont);
  cg.movReg(R9, R10);
  cg.bind(hexExpCont);
  cg.addImm32(R12, 1);
  cg.jmp(hexExpLoop);
  cg.bind(hexExpDone);
  cg.movFromMem(Rax, Rbx, offTmp);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, hexExpStore);
  cg.negReg(R9);
  cg.bind(hexExpStore);
  cg.movToMem(Rbx, offPexp, R9);

  cg.bind(hexBuild);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, hexBuildStart);
  cg.jmp(bad);  // trailing characters
  cg.bind(hexBuildStart);
  // sExp = pexp - 4*fracHex + 4*extra
  cg.movFromMem(R9, Rbx, offFracHex);
  cg.shlImm8(R9, 2);
  cg.movFromMem(R10, Rbx, offPexp);
  cg.subReg(R10, R9);
  cg.movFromMem(R9, Rbx, offExtra);
  cg.shlImm8(R9, 2);
  cg.addReg(R10, R9);
  cg.movToMem(Rbx, offSExp, R10);
  // Build d from digits[0..total).
  setRdi(cg, 0);
  cg.movImm(Rsi, 0);
  cg.callLabel(h.bnSetU64);
  cg.movImmToMem(Rbx, offI, 0);
  cg.bind(hexBuildLoop);
  cg.movFromMem(Rax, Rbx, offI);
  cg.movFromMem(R8, Rbx, offTotal);
  cg.cmpReg(Rax, R8);
  cg.jcc(Jcc::Ae, hexBuildDone);
  cg.movReg(Rdx, Rax);
  cg.addReg(Rdx, Rbx);
  cg.addImm32(Rdx, static_cast<std::int32_t>(offDigits));
  cg.movzxFromMem(R9, Rdx, 0);
  cg.movToMem(Rbx, offTmp, R9);  // bnMulSmall clobbers r9
  setRdi(cg, 0);
  cg.movImm(Rsi, 16);
  cg.callLabel(h.bnMulSmall);
  setRdi(cg, 0);
  cg.movFromMem(Rsi, Rbx, offTmp);
  cg.callLabel(addSmall);
  cg.movFromMem(Rax, Rbx, offI);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offI, Rax);
  cg.jmp(hexBuildLoop);
  cg.bind(hexBuildDone);
  // Range check before the (bounded) rounding shift.
  setRdi(cg, 0);
  cg.callLabel(bitsFn);
  cg.subRegImm32(Rax, 1);
  cg.movFromMem(R8, Rbx, offSExp);
  cg.addReg(Rax, R8);  // E
  cg.movToMem(Rbx, offE, Rax);
  cg.movImm(R9, 1080);
  cg.cmpReg(Rax, R9);
  cg.jcc(Jcc::Gt, bad);
  cg.movImm(R9, -1200);
  cg.cmpReg(Rax, R9);
  cg.jcc(Jcc::Lt, roundZero);
  cg.jmp(round);
  cg.bind(hexZero);
  cg.jmp(roundZero);

  // --- Decimal ---
  cg.bind(notHex);
  cg.movImmToMem(Rbx, offSeenDigit, 0);
  cg.movImmToMem(Rbx, offSeenDot, 0);
  cg.movImmToMem(Rbx, offStarted, 0);
  cg.movImmToMem(Rbx, offTotal, 0);
  cg.movImmToMem(Rbx, offFrac, 0);
  cg.movImmToMem(Rbx, offExtra, 0);
  cg.movImmToMem(Rbx, offSticky, 0);
  cg.movImmToMem(Rbx, offExp10, 0);
  cg.bind(decLoop);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, decMantDone);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, '.');
  cg.jcc(Jcc::Eq, decDot);
  cg.cmpImm32(Rcx, '0');
  cg.jcc(Jcc::Lt, decMantDone);
  cg.cmpImm32(Rcx, '9');
  cg.jcc(Jcc::Le, decDigit);
  cg.jmp(decMantDone);
  cg.bind(decDot);
  cg.movFromMem(Rax, Rbx, offSeenDot);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, decMantDone);
  cg.movImmToMem(Rbx, offSeenDot, 1);
  cg.addImm32(R12, 1);
  cg.jmp(decLoop);
  cg.bind(decDigit);
  cg.movImmToMem(Rbx, offSeenDigit, 1);
  cg.subRegImm32(Rcx, '0');
  cg.movFromMem(Rax, Rbx, offStarted);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, decStore);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Ne, decStore);
  cg.jmp(decAfterDot);  // leading zero
  cg.bind(decStore);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, decStoreNoStart);
  cg.movImmToMem(Rbx, offStarted, 1);
  cg.bind(decStoreNoStart);
  cg.movFromMem(Rax, Rbx, offTotal);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kMaxDigits));
  cg.jcc(Jcc::Ae, decStoreExtra);
  cg.movReg(Rdx, Rax);
  cg.addReg(Rdx, Rbx);
  cg.addImm32(Rdx, static_cast<std::int32_t>(offDigits));
  cg.movByteToMem(Rdx, 0, Rcx);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offTotal, Rax);
  cg.jmp(decAfterDot);
  cg.bind(decStoreExtra);
  cg.movFromMem(Rax, Rbx, offExtra);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offExtra, Rax);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, decAfterDot);
  cg.movImmToMem(Rbx, offSticky, 1);
  cg.bind(decAfterDot);
  cg.movFromMem(Rax, Rbx, offSeenDot);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, decNoFrac);
  cg.movFromMem(Rax, Rbx, offFrac);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offFrac, Rax);
  cg.bind(decNoFrac);
  cg.addImm32(R12, 1);
  cg.jmp(decLoop);

  cg.bind(decMantDone);
  cg.movFromMem(Rax, Rbx, offSeenDigit);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, bad);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, decNoExp);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, 'e');
  cg.jcc(Jcc::Eq, decExp);
  cg.cmpImm32(Rcx, 'E');
  cg.jcc(Jcc::Eq, decExp);
  cg.jmp(decNoExp);
  cg.bind(decExp);
  cg.addImm32(R12, 1);
  cg.movImmToMem(Rbx, offTmp, 0);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, bad);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, '+');
  cg.jcc(Jcc::Eq, decExpSign);
  cg.cmpImm32(Rcx, '-');
  cg.jcc(Jcc::Ne, decExpDigits);
  cg.movImmToMem(Rbx, offTmp, 1);
  cg.bind(decExpSign);
  cg.addImm32(R12, 1);
  cg.bind(decExpDigits);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, bad);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, '0'); cg.jcc(Jcc::Lt, bad);
  cg.cmpImm32(Rcx, '9'); cg.jcc(Jcc::Gt, bad);
  cg.xorReg(R9, R9);
  cg.bind(decExpLoop);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, decExpDone);
  cg.movzxFromMem(Rcx, R12, 0);
  cg.cmpImm32(Rcx, '0'); cg.jcc(Jcc::Lt, decExpDone);
  cg.cmpImm32(Rcx, '9'); cg.jcc(Jcc::Gt, decExpDone);
  cg.subRegImm32(Rcx, '0');
  cg.imulImm32(R9, R9, 10);
  cg.addReg(R9, Rcx);
  cg.movImm(R10, kExpClamp);
  cg.cmpReg(R9, R10);
  cg.jcc(Jcc::Le, decExpCont);
  cg.movReg(R9, R10);
  cg.bind(decExpCont);
  cg.addImm32(R12, 1);
  cg.jmp(decExpLoop);
  cg.bind(decExpDone);
  cg.movFromMem(Rax, Rbx, offTmp);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, decExpStore);
  cg.negReg(R9);
  cg.bind(decExpStore);
  cg.movToMem(Rbx, offExp10, R9);
  cg.bind(decNoExp);
  cg.cmpReg(R12, R13);
  cg.jcc(Jcc::Ae, decBuild);
  cg.jmp(bad);  // trailing characters

  cg.bind(decBuild);
  cg.movFromMem(Rax, Rbx, offStarted);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, roundZero);
  // Trim trailing zeros: net = exp10 - frac + extra + tz.
  cg.movFromMem(Rax, Rbx, offTotal);
  cg.xorReg(R9, R9);
  cg.bind(decTrim);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, decTrimDone);
  cg.movReg(Rdx, Rax);
  cg.subRegImm32(Rdx, 1);
  cg.addReg(Rdx, Rbx);
  cg.addImm32(Rdx, static_cast<std::int32_t>(offDigits));
  cg.movzxFromMem(R10, Rdx, 0);
  cg.testReg(R10);
  cg.jcc(Jcc::Ne, decTrimDone);
  cg.subRegImm32(Rax, 1);
  cg.addImm32(R9, 1);
  cg.jmp(decTrim);
  cg.bind(decTrimDone);
  cg.movToMem(Rbx, offTotal, Rax);
  cg.movFromMem(R10, Rbx, offExp10);
  cg.movFromMem(R11, Rbx, offFrac);
  cg.subReg(R10, R11);
  cg.movFromMem(R11, Rbx, offExtra);
  cg.addReg(R10, R11);
  cg.addReg(R10, R9);
  cg.movToMem(Rbx, offNet, R10);
  // Build d from digits[0..total).
  setRdi(cg, 0);
  cg.movImm(Rsi, 0);
  cg.callLabel(h.bnSetU64);
  cg.movImmToMem(Rbx, offI, 0);
  cg.bind(decDigitsLoop);
  cg.movFromMem(Rax, Rbx, offI);
  cg.movFromMem(R8, Rbx, offTotal);
  cg.cmpReg(Rax, R8);
  cg.jcc(Jcc::Ae, decDigitsDone);
  cg.movReg(Rdx, Rax);
  cg.addReg(Rdx, Rbx);
  cg.addImm32(Rdx, static_cast<std::int32_t>(offDigits));
  cg.movzxFromMem(R9, Rdx, 0);
  cg.movToMem(Rbx, offTmp, R9);  // bnMulSmall clobbers r9
  setRdi(cg, 0);
  cg.movImm(Rsi, 10);
  cg.callLabel(h.bnMulSmall);
  setRdi(cg, 0);
  cg.movFromMem(Rsi, Rbx, offTmp);
  cg.callLabel(addSmall);
  cg.movFromMem(Rax, Rbx, offI);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offI, Rax);
  cg.jmp(decDigitsLoop);
  cg.bind(decDigitsDone);

  // net >= 0: d *= 5^net, sExp = net. net < 0: d <<= K, d /= 10^g, sExp = -K.
  cg.movFromMem(R10, Rbx, offNet);
  cg.testReg(R10);
  cg.jcc(Jcc::S, decNeg);
  // Eest = (bd-1) + floor(net*log2(10)); overflow above 1080 -> error.
  cg.movReg(R11, R10);
  cg.imulImm32(R11, R11, static_cast<std::int32_t>(kLog2TenQ16));
  cg.shrImm8(R11, 16);
  setRdi(cg, 0);
  cg.callLabel(bitsFn);
  cg.subRegImm32(Rax, 1);
  cg.addReg(R11, Rax);
  cg.movImm(R8, 1080);
  cg.cmpReg(R11, R8);
  cg.jcc(Jcc::Gt, bad);
  cg.movToMem(Rbx, offSExp, R10);
  cg.movToMem(Rbx, offCnt, R10);
  cg.bind(decMul5Loop);
  cg.movFromMem(R9, Rbx, offCnt);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, decMul5Done);
  setRdi(cg, 0);
  cg.movImm(Rsi, 5);
  cg.callLabel(h.bnMulSmall);
  cg.movFromMem(R9, Rbx, offCnt);
  cg.subRegImm32(R9, 1);
  cg.movToMem(Rbx, offCnt, R9);
  cg.jmp(decMul5Loop);
  cg.bind(decMul5Done);
  cg.jmp(round);

  cg.bind(decNeg);
  cg.negReg(R10);  // g
  cg.movToMem(Rbx, offTmp, R10);
  cg.movReg(R11, R10);
  cg.imulImm32(R11, R11, static_cast<std::int32_t>(kLog2TenQ16));
  cg.shrImm8(R11, 16);
  setRdi(cg, 0);
  cg.callLabel(bitsFn);
  cg.subRegImm32(Rax, 1);
  cg.movReg(R8, Rax);
  cg.subReg(R8, R11);  // Eest = (bd-1) - floor(g*log2(10))
  cg.movImm(R9, -1080);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Lt, roundZero);
  cg.movImm(R9, 1200);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Gt, bad);
  // K = 64 - Eest, clamped to >= 0.
  cg.movImm(R9, 64);
  cg.subReg(R9, R8);
  cg.testReg(R9);
  cg.jcc(Jcc::Ns, decKOk);
  cg.xorReg(R9, R9);
  cg.bind(decKOk);
  cg.movToMem(Rbx, offCnt, R9);
  setRdi(cg, 0);
  cg.movReg(Rsi, R9);
  cg.callLabel(h.bnShl);
  cg.movFromMem(R9, Rbx, offCnt);
  cg.negReg(R9);
  cg.movToMem(Rbx, offSExp, R9);
  cg.movImmToMem(Rbx, offCnt, 0);  // remainder accumulator
  cg.bind(decDiv10Loop);
  cg.movFromMem(R9, Rbx, offTmp);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, decDiv10Done);
  setRdi(cg, 0);
  cg.movImm(Rsi, 10);
  cg.callLabel(h.bnDivMod);  // rax = remainder
  cg.movFromMem(R8, Rbx, offCnt);
  cg.orReg(R8, Rax);
  cg.movToMem(Rbx, offCnt, R8);
  cg.movFromMem(R9, Rbx, offTmp);
  cg.subRegImm32(R9, 1);
  cg.movToMem(Rbx, offTmp, R9);
  cg.jmp(decDiv10Loop);
  cg.bind(decDiv10Done);
  cg.movFromMem(R8, Rbx, offCnt);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, round);
  cg.movImmToMem(Rbx, offSticky, 1);
  cg.jmp(round);

  // --- Rounding ---
  cg.bind(roundZero);
  cg.movImmToMem(Rbx, offBits, 0);
  cg.jmp(emitSign);

  cg.bind(round);
  setRdi(cg, 0);
  cg.callLabel(bitsFn);
  cg.movToMem(Rbx, offL, Rax);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, roundZero);
  cg.subRegImm32(Rax, 1);
  cg.movFromMem(R8, Rbx, offSExp);
  cg.addReg(Rax, R8);
  cg.movToMem(Rbx, offE, Rax);  // E = L-1+sExp
  cg.movImm(R9, -1022);
  cg.cmpReg(Rax, R9);
  cg.jcc(Jcc::Lt, roundUlpSub);
  cg.subRegImm32(Rax, 52);
  cg.jmp(roundUlpDone);
  cg.bind(roundUlpSub);
  cg.movImm(Rax, -1074);
  cg.bind(roundUlpDone);
  cg.movToMem(Rbx, offUlp, Rax);
  cg.movFromMem(R10, Rbx, offSExp);
  cg.subReg(R10, Rax);
  cg.movToMem(Rbx, offShift, R10);
  cg.movImmToMem(Rbx, offT2, 0);
  cg.movImmToMem(Rbx, offRb, 0);
  cg.movImmToMem(Rbx, offSt, 0);
  cg.testReg(R10);
  cg.jcc(Jcc::S, roundNegLoop);

  // shift >= 0: T = low 64 bits of (d << shift); st = sticky.
  cg.movFromMem(R9, Rbx, offL);
  cg.addReg(R9, R10);  // n = L + shift
  cg.movImmToMem(Rbx, offI, 0);
  cg.bind(roundPosLoop);
  cg.movFromMem(Rax, Rbx, offI);
  cg.cmpImm32(Rax, 64);
  cg.jcc(Jcc::Ae, roundPosDone);
  cg.cmpReg(Rax, R9);
  cg.jcc(Jcc::Ae, roundPosDone);
  cg.movReg(Rdx, Rax);
  cg.subReg(Rdx, R10);  // bi = i - shift
  cg.testReg(Rdx);
  cg.jcc(Jcc::S, roundPosNext);
  setRdi(cg, 0);
  cg.movReg(Rsi, Rdx);
  cg.callLabel(bitFn);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, roundPosNext);
  cg.movFromMem(R8, Rbx, offI);
  cg.movImm(R11, 1);
  cg.movReg(Rcx, R8);
  cg.shlCl(R11);
  cg.movFromMem(R8, Rbx, offT2);
  cg.orReg(R8, R11);
  cg.movToMem(Rbx, offT2, R8);
  cg.bind(roundPosNext);
  cg.movFromMem(Rax, Rbx, offI);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offI, Rax);
  cg.jmp(roundPosLoop);
  cg.bind(roundPosDone);
  cg.movFromMem(R8, Rbx, offSticky);
  cg.movToMem(Rbx, offSt, R8);
  cg.jmp(roundDecide);

  // shift < 0: drop = -shift.
  cg.bind(roundNegLoop);
  cg.negReg(R10);  // drop
  cg.movImmToMem(Rbx, offI, 0);
  cg.bind(roundNegNext);
  cg.movFromMem(Rax, Rbx, offI);
  cg.cmpImm32(Rax, 64);
  cg.jcc(Jcc::Ae, roundNegDone);
  cg.movReg(Rdx, R10);
  cg.addReg(Rdx, Rax);
  setRdi(cg, 0);
  cg.movReg(Rsi, Rdx);
  cg.callLabel(bitFn);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, roundNegSkip);
  cg.movFromMem(R8, Rbx, offI);
  cg.movImm(R11, 1);
  cg.movReg(Rcx, R8);
  cg.shlCl(R11);
  cg.movFromMem(R8, Rbx, offT2);
  cg.orReg(R8, R11);
  cg.movToMem(Rbx, offT2, R8);
  cg.bind(roundNegSkip);
  cg.movFromMem(Rax, Rbx, offI);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offI, Rax);
  cg.jmp(roundNegNext);
  cg.bind(roundNegDone);
  // rb = bit(drop-1)
  cg.movReg(Rdx, R10);
  cg.subRegImm32(Rdx, 1);
  setRdi(cg, 0);
  cg.movReg(Rsi, Rdx);
  cg.callLabel(bitFn);
  cg.movToMem(Rbx, offRb, Rax);
  // st = any bit in [0, drop-1) or sticky
  cg.movImmToMem(Rbx, offI, 0);
  cg.bind(roundStLoop);
  cg.movFromMem(Rax, Rbx, offI);
  cg.movReg(Rdx, R10);
  cg.subRegImm32(Rdx, 1);
  cg.cmpReg(Rax, Rdx);
  cg.jcc(Jcc::Ae, roundStDone);
  setRdi(cg, 0);
  cg.movReg(Rsi, Rax);
  cg.callLabel(bitFn);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, roundStSet);
  cg.movFromMem(Rax, Rbx, offI);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offI, Rax);
  cg.jmp(roundStLoop);
  cg.bind(roundStSet);
  cg.movImmToMem(Rbx, offSt, 1);
  cg.bind(roundStDone);
  cg.movFromMem(R8, Rbx, offSticky);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, roundDecide);
  cg.movImmToMem(Rbx, offSt, 1);

  cg.bind(roundDecide);
  cg.movFromMem(R8, Rbx, offRb);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, roundNoInc);
  cg.movFromMem(R9, Rbx, offSt);
  cg.testReg(R9);
  cg.jcc(Jcc::Ne, roundInc);
  cg.movFromMem(R9, Rbx, offT2);
  cg.testImm32(R9, 1);
  cg.jcc(Jcc::Eq, roundNoInc);
  cg.bind(roundInc);
  cg.movFromMem(R9, Rbx, offT2);
  cg.addImm32(R9, 1);
  cg.movToMem(Rbx, offT2, R9);
  cg.bind(roundNoInc);

  cg.bind(pack);
  cg.movFromMem(R9, Rbx, offT2);
  cg.movFromMem(R10, Rbx, offE);
  cg.movImm(R8, kTwoPow53);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Lt, packNoBump);
  cg.shrImm8(R9, 1);
  cg.addImm32(R10, 1);
  cg.bind(packNoBump);
  cg.movToMem(Rbx, offT2, R9);
  cg.movToMem(Rbx, offE, R10);
  cg.movImm(R8, 1023);
  cg.cmpReg(R10, R8);
  cg.jcc(Jcc::Gt, bad);  // overflow
  cg.movImm(R8, -1022);
  cg.cmpReg(R10, R8);
  cg.jcc(Jcc::Ge, packNormal);
  // subnormal
  cg.movImm(R8, kTwoPow52);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ge, packSub);
  cg.movToMem(Rbx, offBits, R9);
  cg.jmp(emitSign);
  cg.bind(packSub);
  cg.movToMem(Rbx, offBits, R8);  // smallest normal
  cg.jmp(emitSign);
  cg.bind(packNormal);
  cg.movImm(R8, 1023);
  cg.addReg(R10, R8);
  cg.movReg(R8, R10);
  cg.shlImm8(R8, 52);
  cg.movImm(R11, kTwoPow52);
  cg.subRegImm32(R11, 1);
  cg.andReg(R9, R11);
  cg.orReg(R8, R9);
  cg.movToMem(Rbx, offBits, R8);

  cg.bind(emitSign);
  cg.movFromMem(Rax, Rbx, offNeg);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, emitDone);
  cg.movFromMem(R9, Rbx, offBits);
  cg.movImm(R8, 1);
  cg.movReg(Rcx, R8);
  cg.shlImm8(Rcx, 63);
  cg.orReg(R9, Rcx);
  cg.movToMem(Rbx, offBits, R9);
  cg.bind(emitDone);
  cg.movFromMem(Rdx, Rbx, offOut);
  cg.movFromMem(Rax, Rbx, offBits);
  cg.movToMem(Rdx, 0, Rax);
  cg.xorReg(Rax, Rax);  // 0 = success
  cg.jmp(epilogue);

  cg.bind(bad);
  cg.movImm(Rax, 1);  // 1 = failure

  cg.bind(epilogue);
  cg.pop(R14);
  cg.pop(R13);
  cg.pop(R12);
  cg.pop(Rbx);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();
}

}  // namespace

void emitParseFloat(CodeGen& code, const ParseFloatHandles& handles,
                    const dtoa::DtoaHandles& dtoaHandles) {
  CodeGen::Label addSmall = code.newLabel();
  CodeGen::Label bitsFn = code.newLabel();
  CodeGen::Label bitFn = code.newLabel();
  emitBnAddSmall(code, addSmall);
  emitBnBits(code, bitsFn);
  emitBnBit(code, bitFn);
  emitParse(code, handles.parseDouble, addSmall, bitsFn, bitFn, dtoaHandles);
}

}  // namespace parse_float
