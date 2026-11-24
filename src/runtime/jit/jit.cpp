#include "jit/jit.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include "native/codegen.h"
#include "native/target.h"

#if defined(_WIN32)
// Executable memory is not mapped on Windows in this slice; the JIT reports
// "no backend" and the VM keeps interpreting.
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

// The JIT emits x86-64 for the System V ABI only, like the AOT backend. On any
// other host there is nothing to run, so `jitCompile` bails out immediately
// instead of producing bytes the process cannot execute.
constexpr bool jitHostSupported() {
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
  return true;
#else
  return false;
#endif
}

// Physical registers a register-resident slot can live in: all caller-saved,
// and a supported proto is a leaf (no `CALL`), so the body makes no call that
// could clobber them. r10/r11 stay scratch and rbp is the frame pointer.
constexpr int kJitRegPoolCount = 7;
constexpr X64Reg kJitRegPool[kJitRegPoolCount] = {
    X64Reg::Rax, X64Reg::Rcx, X64Reg::Rdx, X64Reg::Rsi,
    X64Reg::Rdi, X64Reg::R8, X64Reg::R9};

// -------------------------------------------------------------- value analysis
//
// The JIT only compiles a proto whose body provably manipulates `nil`, `bool`
// and `int` alone. Those three value kinds carry no reference count and own no
// memory, so the generated code never has to touch the heap or the cycle
// collector: it reads and writes a self-contained register file and returns a
// value. Everything else (strings, floats, containers, functions, globals) is
// left to the interpreter.
//
// `Ty` is the per-register lattice element. `Undef` means "not yet known" and
// is only meaningful while the fixpoint runs; once it settles, a read of an
// `Undef` or `Top` register means the proto is outside the subset.
enum class Ty : std::uint8_t { Undef, Nil, Bool, Int, Top };

Ty mergeTy(Ty a, Ty b) {
  if (a == Ty::Undef) return b;
  if (b == Ty::Undef) return a;
  if (a == b) return a;
  return Ty::Top;
}

bool isScalar(Ty t) { return t == Ty::Nil || t == Ty::Bool || t == Ty::Int; }

// The lattice element a constant maps to, or `Top` when the constant is a kind
// the JIT does not handle.
Ty constTy(const Value& value) {
  switch (value.type()) {
    case VType::Nil:
      return Ty::Nil;
    case VType::Bool:
      return Ty::Bool;
    case VType::Int:
      return Ty::Int;
    default:
      return Ty::Top;
  }
}

std::uint8_t tagOf(Ty t) {
  switch (t) {
    case Ty::Nil:
      return static_cast<std::uint8_t>(VType::Nil);
    case Ty::Bool:
      return static_cast<std::uint8_t>(VType::Bool);
    default:
      return static_cast<std::uint8_t>(VType::Int);
  }
}

// True when `value` fits in the 32-bit immediate the emitter's `imm32` forms
// take (sign-extended to 64 bits). A value outside this range must go through a
// register, so the caller materializes it first.
bool fitsImm32(std::int64_t value) {
  return value >= -2147483648LL && value <= 2147483647LL;
}

// `n > 0` and a power of two; `log2Pow2` then holds its exponent. Used to turn a
// multiply into a shift.
bool isPow2(std::int64_t n, int& log2Pow2) {
  if (n <= 0) return false;
  const std::uint64_t u = static_cast<std::uint64_t>(n);
  if (u & (u - 1)) return false;
  log2Pow2 = 0;
  while ((u >> log2Pow2) > 1) ++log2Pow2;
  return true;
}

// Folds `a OP b` for two int64 operands, wrapping exactly like the interpreter
// (`ops.cpp::binary`). Only the operators the JIT subset accepts reach here.
bool foldBinaryInt(OpCode op, std::int64_t a, std::int64_t b, std::int64_t& out) {
  const std::uint64_t ua = static_cast<std::uint64_t>(a);
  const std::uint64_t ub = static_cast<std::uint64_t>(b);
  switch (op) {
    case OpCode::ADD: out = static_cast<std::int64_t>(ua + ub); return true;
    case OpCode::SUB: out = static_cast<std::int64_t>(ua - ub); return true;
    case OpCode::MUL: out = static_cast<std::int64_t>(ua * ub); return true;
    case OpCode::BAND: out = a & b; return true;
    case OpCode::BOR: out = a | b; return true;
    case OpCode::BXOR: out = a ^ b; return true;
    default: return false;
  }
}

// Folds an integer comparison. The interpreter compares signed int64 for
// `LT`/`LTE`/`GT`/`GTE` and value equality for `EQ`/`NEQ`; both agree with the
// signed machine compare for the int/bool operands the subset admits.
bool foldCompareInt(OpCode op, std::int64_t a, std::int64_t b, bool& out) {
  switch (op) {
    case OpCode::EQ: out = a == b; return true;
    case OpCode::NEQ: out = a != b; return true;
    case OpCode::LT: out = a < b; return true;
    case OpCode::LTE: out = a <= b; return true;
    case OpCode::GT: out = a > b; return true;
    case OpCode::GTE: out = a >= b; return true;
    default: return false;
  }
}

// Folds `-x` and `~x` (both preserve `Int`). `!x` is handled with the truthiness
// rule, not here.
bool foldUnaryInt(UnaryOp op, std::int64_t a, std::int64_t& out) {
  switch (op) {
    case UnaryOp::Negate:
      out = static_cast<std::int64_t>(0ULL - static_cast<std::uint64_t>(a));
      return true;
    case UnaryOp::BitNot: out = ~a; return true;
    default: return false;
  }
}

// Oskel truthiness for a scalar constant: only `nil` and `false` are falsy.
bool truthyConst(Ty kind, std::int64_t value) {
  if (kind == Ty::Nil) return false;
  if (kind == Ty::Bool) return value != 0;
  return true;
}

// The condition code a comparison opcode tests for, and its mirror (`LT` becomes
// `GT`, and so on) so a constant left operand can be compared from the right.
Jcc compareCc(OpCode op) {
  switch (op) {
    case OpCode::EQ: return Jcc::Eq;
    case OpCode::NEQ: return Jcc::Ne;
    case OpCode::LT: return Jcc::Lt;
    case OpCode::LTE: return Jcc::Le;
    case OpCode::GT: return Jcc::Gt;
    default: return Jcc::Ge;
  }
}

Jcc mirrorCc(Jcc cc) {
  switch (cc) {
    case Jcc::Lt: return Jcc::Gt;
    case Jcc::Le: return Jcc::Ge;
    case Jcc::Gt: return Jcc::Lt;
    case Jcc::Ge: return Jcc::Le;
    default: return cc;  // Eq / Ne are symmetric
  }
}

// The complementary condition code, so a branch on a comparison's negation
// still tests the flags the comparison left (`JMP_IF_FALSE` jumps when the
// comparison is false, i.e. on `negateCc` of the comparison's own code).
Jcc negateCc(Jcc cc) {
  switch (cc) {
    case Jcc::Eq: return Jcc::Ne;
    case Jcc::Ne: return Jcc::Eq;
    case Jcc::Lt: return Jcc::Ge;
    case Jcc::Le: return Jcc::Gt;
    case Jcc::Gt: return Jcc::Le;
    case Jcc::Ge: return Jcc::Lt;
    default: return cc;
  }
}

// True for the operators where `a OP b == b OP a`, so a constant on the left can
// be applied as an immediate on the right.
bool isCommutative(OpCode op) {
  switch (op) {
    case OpCode::ADD:
    case OpCode::MUL:
    case OpCode::BAND:
    case OpCode::BOR:
    case OpCode::BXOR:
      return true;
    default:
      return false;
  }
}

struct Analysis {
  bool supported = false;
  int numRegs = 0;
  int numLocals = 0;
  std::vector<bool> reachable;
  // Type of every register and local on entry to each instruction. Index
  // `numRegs + l` is local `l`.
  std::vector<std::vector<Ty>> stateIn;
  // Per slot (registers then locals), true when the slot can live in a
  // physical CPU register instead of its 16-byte stack slot: every reachable
  // read observes a concrete scalar (`nil`/`bool`/`int`) and every write stores
  // one. In a supported proto that is *every* slot, because the verifier
  // already rejects any read of a non-scalar; the flag stays explicit so a
  // future opcode that produces a reference type cannot silently break the
  // invariant. The register holds the raw payload (int64, or 0/1 for a bool,
  // 0 for nil); the exact kind at each use is recovered from the dataflow
  // state, so a slot may legitimately change scalar kind across the body.
  std::vector<bool> regResident;

  // Compile-time constant of a slot at the entry of each instruction, or a
  // sentinel `hasConst == false` when the slot is not a known constant there.
  // A slot is constant when every reachable path into the instruction assigns
  // it the same `LOADK` value and nothing else writes it in between; the
  // emitter uses this to fold an operation whose operands are both constant,
  // and to pick an immediate form for one that is. `constVal` holds the
  // payload (int64, or 0/1 for a bool, 0 for nil) and `constKind` its scalar
  // kind, so a folded result keeps the same tag the interpreter would produce.
  std::vector<std::vector<bool>> hasConst;
  std::vector<std::vector<std::int64_t>> constVal;
  std::vector<std::vector<Ty>> constKind;

  // Per instruction, whether the *result* it defines in `a` is a known constant
  // (a pure definition whose value the propagation pinned down). When set, the
  // emitter writes the constant directly and skips the opcode entirely: a
  // `2 * 3` folds to `6`, and a chain folds as far as the constants reach.
  std::vector<bool> defConst;
  std::vector<std::int64_t> defVal;
  std::vector<Ty> defKind;

  // Compare-branch fusion. For a comparison whose boolean result is dead right
  // after a conditional branch on it, the emitter skips materializing the bool
  // and turns the pair into `cmp; jcc`. `fusedBranch[i]` is the branch index a
  // comparison at `i` jumps for, or -1 when the comparison emits normally; a
  // branch listed here emits nothing (the comparison already jumped).
  std::vector<int> fusedBranch;

  // Register allocation. `readReg[i][slot]` is the physical register (an index
  // into `kJitRegPool`, or -1 for a stack slot) holding a slot's value when
  // instruction `i` *reads* it; `writeReg[i][slot]` is the register it lands in
  // when `i` *defines* it. The two differ because one instruction can read and
  // write the same slot through different registers. `entryReg[slot]` is the
  // register a parameter's incoming value is lifted into.
  std::vector<std::vector<int>> readReg;
  std::vector<std::vector<int>> writeReg;
  std::vector<int> entryReg;

  // Per instruction, true when it is a pure definition whose value no reachable
  // read observes (a dead web). The emitter skips it: it is the last residue a
  // folded expression leaves behind, e.g. a `LOADK` whose constant the emitter
  // already used as an immediate.
  std::vector<bool> deadDef;
};

// The slots an instruction reads. Shared by the liveness pass, the fusion pass
// and the allocator, so they all agree with the VM's operand use.
void instrReads(const Instruction& in, int numRegs, int total, std::vector<int>& out) {
  auto ok = [&](int s) { return s >= 0 && s < total; };
  switch (in.op) {
    case OpCode::MOVE:
      if (ok(in.b)) out.push_back(in.b);
      break;
    case OpCode::ADD:
    case OpCode::SUB:
    case OpCode::MUL:
    case OpCode::BAND:
    case OpCode::BOR:
    case OpCode::BXOR:
    case OpCode::EQ:
    case OpCode::NEQ:
    case OpCode::LT:
    case OpCode::LTE:
    case OpCode::GT:
    case OpCode::GTE:
      if (ok(in.b)) out.push_back(in.b);
      if (ok(in.c)) out.push_back(in.c);
      break;
    case OpCode::UNOP:
      if (ok(in.b)) out.push_back(in.b);
      break;
    case OpCode::JMP_IF_FALSE:
    case OpCode::JMP_IF_TRUE:
      if (ok(in.a)) out.push_back(in.a);
      break;
    case OpCode::DECLLOCAL:
    case OpCode::SETLOCAL:
      if (ok(in.a)) out.push_back(in.a);
      break;
    case OpCode::GETLOCAL:
      if (ok(numRegs + in.b)) out.push_back(numRegs + in.b);
      break;
    case OpCode::RETURN:
      if (ok(in.a)) out.push_back(in.a);
      break;
    default:
      break;
  }
}

// The slots an instruction defines. Every supported instruction defines at most
// one slot.
void instrWrites(const Instruction& in, int numRegs, int total, std::vector<int>& out) {
  auto ok = [&](int s) { return s >= 0 && s < total; };
  switch (in.op) {
    case OpCode::LOADK:
    case OpCode::MOVE:
    case OpCode::ADD:
    case OpCode::SUB:
    case OpCode::MUL:
    case OpCode::BAND:
    case OpCode::BOR:
    case OpCode::BXOR:
    case OpCode::UNOP:
    case OpCode::EQ:
    case OpCode::NEQ:
    case OpCode::LT:
    case OpCode::LTE:
    case OpCode::GT:
    case OpCode::GTE:
    case OpCode::GETLOCAL:
      if (ok(in.a)) out.push_back(in.a);
      break;
    case OpCode::DECLLOCAL:
    case OpCode::SETLOCAL:
      if (ok(numRegs + in.b)) out.push_back(numRegs + in.b);
      break;
    default:
      break;
  }
}

// The successor instruction indices of `i` (its fall-through and/or its jump
// target). A `RETURN`/`HALT` has none.
void instrSuccessors(const Instruction& in, int i, std::vector<int>& out) {
  switch (in.op) {
    case OpCode::JMP:
      out.push_back(in.jump);
      break;
    case OpCode::JMP_IF_FALSE:
    case OpCode::JMP_IF_TRUE:
      out.push_back(in.jump);
      out.push_back(i + 1);
      break;
    case OpCode::RETURN:
    case OpCode::HALT:
      break;
    default:
      out.push_back(i + 1);
      break;
  }
}

// Web-based register allocation with copy coalescing (PHASE 8.4).
//
// The older scheme tied every slot to a fixed register by index (`R0` to the
// first pool register, `R1` to the second, ...), so a temporary reused for two
// different values forced a copy at each reuse and every `GETLOCAL`/`SETLOCAL`
// copied between the temporary's register and the local's. This pass instead
// gives each *web* -- a maximal set of definitions a single use can observe --
// its own register. Two temporaries whose live ranges do not overlap then share
// a register, and a copy whose two webs do not interfere disappears. It runs
// after the verifier, so every slot is a scalar; a web the 7-register pool
// cannot color keeps its 16-byte stack slot (a `-1` register), exactly like the
// old overflow path.
void allocateRegisters(const Bytecode& proto, Analysis& result) {
  const int n = static_cast<int>(proto.code.size());
  const int numRegs = result.numRegs;
  const int total = numRegs + result.numLocals;
  const int K = kJitRegPoolCount;

  result.readReg.assign(static_cast<std::size_t>(n), std::vector<int>(total, -1));
  result.writeReg.assign(static_cast<std::size_t>(n), std::vector<int>(total, -1));
  result.entryReg.assign(static_cast<std::size_t>(total), -1);
  result.deadDef.assign(static_cast<std::size_t>(n), false);
  if (n == 0 || total == 0) return;

  auto indexOk = [&](int s) { return s >= 0 && s < total; };

  // The slots an instruction actually reads at run time, mirroring the emitter's
  // fold decisions: an operand the emitter replaces by an immediate (a
  // `constIn` hit) is not read, and neither is a branch on a constant or on a
  // `nil`/`int` condition (decided by the type). Liveness over these reads makes
  // a definition nothing observes -- the constant a folded expression leaves
  // behind -- dead, so the emitter can skip it.
  auto constAt = [&](int i, int slot, std::int64_t& v) -> bool {
    if (!indexOk(slot)) return false;
    const std::size_t ii = static_cast<std::size_t>(i);
    if (ii >= result.hasConst.size() ||
        static_cast<std::size_t>(slot) >= result.hasConst[ii].size()) {
      return false;
    }
    if (!result.hasConst[ii][static_cast<std::size_t>(slot)]) return false;
    v = result.constVal[ii][static_cast<std::size_t>(slot)];
    return true;
  };
  auto tyAt = [&](int i, int slot) -> Ty {
    const std::size_t ii = static_cast<std::size_t>(i);
    if (ii >= result.stateIn.size() || !indexOk(slot) ||
        static_cast<std::size_t>(slot) >= result.stateIn[ii].size()) {
      return Ty::Top;
    }
    return result.stateIn[ii][static_cast<std::size_t>(slot)];
  };
  auto runtimeReads = [&](int i, const Instruction& in, std::vector<int>& out) {
    std::int64_t v = 0;
    switch (in.op) {
      case OpCode::MOVE:
        if (indexOk(in.b)) out.push_back(in.b);
        break;
      case OpCode::ADD:
      case OpCode::SUB:
      case OpCode::MUL:
      case OpCode::BAND:
      case OpCode::BOR:
      case OpCode::BXOR: {
        if (result.defConst[static_cast<std::size_t>(i)]) break;
        const bool bc = constAt(i, in.b, v);
        const bool cc = constAt(i, in.c, v);
        if (bc && !cc) { if (indexOk(in.c)) out.push_back(in.c); }
        else if (cc && !bc) { if (indexOk(in.b)) out.push_back(in.b); }
        else { if (indexOk(in.b)) out.push_back(in.b); if (indexOk(in.c)) out.push_back(in.c); }
        break;
      }
      case OpCode::EQ:
      case OpCode::NEQ:
      case OpCode::LT:
      case OpCode::LTE:
      case OpCode::GT:
      case OpCode::GTE: {
        if (result.defConst[static_cast<std::size_t>(i)]) break;
        const bool bc = constAt(i, in.b, v) && fitsImm32(v);
        const bool cc = constAt(i, in.c, v) && fitsImm32(v);
        if (bc && !cc) { if (indexOk(in.c)) out.push_back(in.c); }
        else if (cc && !bc) { if (indexOk(in.b)) out.push_back(in.b); }
        else { if (indexOk(in.b)) out.push_back(in.b); if (indexOk(in.c)) out.push_back(in.c); }
        break;
      }
      case OpCode::UNOP: {
        if (result.defConst[static_cast<std::size_t>(i)]) break;
        if (static_cast<UnaryOp>(in.c) == UnaryOp::Not) {
          const Ty t = tyAt(i, in.b);
          if (t == Ty::Nil || t == Ty::Int) break;  // result decided by the type
        }
        if (indexOk(in.b)) out.push_back(in.b);
        break;
      }
      case OpCode::JMP_IF_FALSE:
      case OpCode::JMP_IF_TRUE: {
        if (constAt(i, in.a, v)) break;  // branch decided at compile time
        const Ty t = tyAt(i, in.a);
        if (t == Ty::Nil || t == Ty::Int) break;  // decided by the type
        if (indexOk(in.a)) out.push_back(in.a);
        break;
      }
      case OpCode::DECLLOCAL:
      case OpCode::SETLOCAL:
        if (constAt(i, in.a, v)) break;
        if (indexOk(in.a)) out.push_back(in.a);
        break;
      case OpCode::GETLOCAL:
        if (constAt(i, numRegs + in.b, v)) break;
        if (indexOk(numRegs + in.b)) out.push_back(numRegs + in.b);
        break;
      case OpCode::RETURN:
        if (indexOk(in.a)) out.push_back(in.a);
        break;
      default:
        break;
    }
  };

  // Fallback: the pre-8.4 fixed by-index mapping, used when a live slot has no
  // reaching definition (which the verifier should already have ruled out).
  auto identityMapping = [&]() {
    result.readReg.assign(static_cast<std::size_t>(n), std::vector<int>(total, -1));
    result.writeReg.assign(static_cast<std::size_t>(n), std::vector<int>(total, -1));
    result.entryReg.assign(static_cast<std::size_t>(total), -1);
    result.deadDef.assign(static_cast<std::size_t>(n), false);
    for (int s = 0; s < total; ++s) {
      if (s < K && static_cast<std::size_t>(s) < result.regResident.size() &&
          result.regResident[static_cast<std::size_t>(s)]) {
        result.entryReg[static_cast<std::size_t>(s)] = s;
        for (int i = 0; i < n; ++i) {
          result.readReg[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] = s;
          result.writeReg[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] = s;
        }
      }
    }
  };

  // Definitions: one per writing instruction, plus a synthetic entry definition
  // per parameter (its incoming value).
  std::vector<int> genDef(static_cast<std::size_t>(n), -1);
  std::vector<int> defSlot;
  std::vector<int> defInstr;
  std::vector<int> entryDef(static_cast<std::size_t>(total), -1);
  for (int i = 0; i < n; ++i) {
    if (!result.reachable[static_cast<std::size_t>(i)]) continue;
    std::vector<int> wr;
    instrWrites(proto.code[static_cast<std::size_t>(i)], numRegs, total, wr);
    if (!wr.empty()) {
      genDef[static_cast<std::size_t>(i)] = static_cast<int>(defSlot.size());
      defSlot.push_back(wr[0]);
      defInstr.push_back(i);
    }
  }
  for (int p = 0; p < proto.numParams && p < numRegs; ++p) {
    entryDef[static_cast<std::size_t>(p)] = static_cast<int>(defSlot.size());
    defSlot.push_back(p);
    defInstr.push_back(-1);
  }
  const int numDefs = static_cast<int>(defSlot.size());
  if (numDefs == 0) return;

  // Liveness per slot (backward fixpoint).
  std::vector<std::vector<bool>> liveIn(static_cast<std::size_t>(n), std::vector<bool>(total, false));
  std::vector<std::vector<bool>> liveOut(static_cast<std::size_t>(n), std::vector<bool>(total, false));
  {
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 100000) {
      changed = false;
      for (int i = n - 1; i >= 0; --i) {
        if (!result.reachable[static_cast<std::size_t>(i)]) continue;
        std::vector<bool> out(total, false);
        std::vector<int> succ;
        instrSuccessors(proto.code[static_cast<std::size_t>(i)], i, succ);
        for (int s : succ) {
          if (s < 0 || s >= n || !result.reachable[static_cast<std::size_t>(s)]) continue;
          for (int k = 0; k < total; ++k) {
            if (liveIn[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)]) {
              out[static_cast<std::size_t>(k)] = true;
            }
          }
        }
        std::vector<bool> in = out;
        std::vector<int> wr;
        instrWrites(proto.code[static_cast<std::size_t>(i)], numRegs, total, wr);
        for (int w : wr) in[static_cast<std::size_t>(w)] = false;
        std::vector<int> rd;
        runtimeReads(i, proto.code[static_cast<std::size_t>(i)], rd);
        for (int r : rd) in[static_cast<std::size_t>(r)] = true;
        if (out != liveOut[static_cast<std::size_t>(i)] || in != liveIn[static_cast<std::size_t>(i)]) {
          liveOut[static_cast<std::size_t>(i)] = out;
          liveIn[static_cast<std::size_t>(i)] = in;
          changed = true;
        }
      }
    }
  }

  // Reaching definitions (forward fixpoint).
  std::vector<std::vector<char>> reach(static_cast<std::size_t>(n), std::vector<char>(numDefs, 0));
  std::vector<std::vector<char>> reachOut(static_cast<std::size_t>(n), std::vector<char>(numDefs, 0));
  std::vector<std::vector<int>> preds(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    if (!result.reachable[static_cast<std::size_t>(i)]) continue;
    std::vector<int> succ;
    instrSuccessors(proto.code[static_cast<std::size_t>(i)], i, succ);
    for (int s : succ) {
      if (s >= 0 && s < n && result.reachable[static_cast<std::size_t>(s)]) {
        preds[static_cast<std::size_t>(s)].push_back(i);
      }
    }
  }
  {
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 100000) {
      changed = false;
      for (int i = 0; i < n; ++i) {
        if (!result.reachable[static_cast<std::size_t>(i)]) continue;
        std::vector<char> in(numDefs, 0);
        if (i == 0) {
          for (int p = 0; p < proto.numParams && p < numRegs; ++p) {
            if (entryDef[static_cast<std::size_t>(p)] >= 0) in[static_cast<std::size_t>(entryDef[static_cast<std::size_t>(p)])] = 1;
          }
        }
        for (int p : preds[static_cast<std::size_t>(i)]) {
          for (int d = 0; d < numDefs; ++d) {
            if (reachOut[static_cast<std::size_t>(p)][static_cast<std::size_t>(d)]) in[static_cast<std::size_t>(d)] = 1;
          }
        }
        std::vector<char> out = in;
        std::vector<int> wr;
        instrWrites(proto.code[static_cast<std::size_t>(i)], numRegs, total, wr);
        for (int s : wr) {
          for (int d = 0; d < numDefs; ++d) {
            if (defSlot[static_cast<std::size_t>(d)] == s) out[static_cast<std::size_t>(d)] = 0;
          }
        }
        if (genDef[static_cast<std::size_t>(i)] >= 0) out[static_cast<std::size_t>(genDef[static_cast<std::size_t>(i)])] = 1;
        if (in != reach[static_cast<std::size_t>(i)] || out != reachOut[static_cast<std::size_t>(i)]) {
          reach[static_cast<std::size_t>(i)] = in;
          reachOut[static_cast<std::size_t>(i)] = out;
          changed = true;
        }
      }
    }
  }

  // Web construction: definitions a single live use can observe share a web, so
  // at any point where a slot is live all its reaching definitions are one web
  // and a read can name a single register.
  std::vector<int> uf(static_cast<std::size_t>(numDefs));
  for (int d = 0; d < numDefs; ++d) uf[static_cast<std::size_t>(d)] = d;
  auto find = [&](int x) {
    while (uf[static_cast<std::size_t>(x)] != x) {
      uf[static_cast<std::size_t>(x)] = uf[static_cast<std::size_t>(uf[static_cast<std::size_t>(x)])];
      x = uf[static_cast<std::size_t>(x)];
    }
    return x;
  };
  auto unite = [&](int a, int b) {
    a = find(a);
    b = find(b);
    if (a != b) uf[static_cast<std::size_t>(a)] = b;
  };
  bool consistent = true;
  for (int i = 0; i < n; ++i) {
    if (!result.reachable[static_cast<std::size_t>(i)]) continue;
    for (int s = 0; s < total; ++s) {
      const bool lin = liveIn[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)];
      const bool lout = liveOut[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)];
      if (!lin && !lout) continue;
      const std::vector<char>& set = lin ? reach[static_cast<std::size_t>(i)] : reachOut[static_cast<std::size_t>(i)];
      int first = -1;
      for (int d = 0; d < numDefs; ++d) {
        if (set[static_cast<std::size_t>(d)] && defSlot[static_cast<std::size_t>(d)] == s) {
          if (first < 0) first = d;
          else unite(first, d);
        }
      }
      if (first < 0 && s < proto.numParams && entryDef[static_cast<std::size_t>(s)] >= 0) first = entryDef[static_cast<std::size_t>(s)];
      if (first < 0) consistent = false;
    }
  }
  if (!consistent) {
    identityMapping();
    return;
  }

  std::vector<int> webOf(static_cast<std::size_t>(numDefs), -1);
  std::vector<int> rootToWeb(static_cast<std::size_t>(numDefs), -1);
  int numWebs = 0;
  for (int d = 0; d < numDefs; ++d) {
    const int r = find(d);
    if (rootToWeb[static_cast<std::size_t>(r)] < 0) rootToWeb[static_cast<std::size_t>(r)] = numWebs++;
    webOf[static_cast<std::size_t>(d)] = rootToWeb[static_cast<std::size_t>(r)];
  }

  // Per instruction and slot, the web of the reaching definition.
  std::vector<std::vector<int>> webIn(static_cast<std::size_t>(n), std::vector<int>(total, -1));
  std::vector<std::vector<int>> webOut(static_cast<std::size_t>(n), std::vector<int>(total, -1));
  for (int i = 0; i < n; ++i) {
    if (!result.reachable[static_cast<std::size_t>(i)]) continue;
    for (int d = 0; d < numDefs; ++d) {
      if (reach[static_cast<std::size_t>(i)][static_cast<std::size_t>(d)]) {
        webIn[static_cast<std::size_t>(i)][static_cast<std::size_t>(defSlot[static_cast<std::size_t>(d)])] = webOf[static_cast<std::size_t>(d)];
      }
      if (reachOut[static_cast<std::size_t>(i)][static_cast<std::size_t>(d)]) {
        webOut[static_cast<std::size_t>(i)][static_cast<std::size_t>(defSlot[static_cast<std::size_t>(d)])] = webOf[static_cast<std::size_t>(d)];
      }
    }
  }

  // Interference: two webs that are live at the same point need different
  // registers. A *pure copy* web (a `MOVE`/`GETLOCAL`/`DECLLOCAL`/`SETLOCAL`
  // whose destination web has no other definition) holds its source's value
  // wherever both are live, so the edge between the pair is spurious: dropping
  // it lets the copy coalesce away.
  auto webAt = [&](int i, int s) -> int {
    if (!indexOk(s)) return -1;
    return webIn[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)];
  };
  std::vector<std::vector<char>> adj(static_cast<std::size_t>(numWebs), std::vector<char>(numWebs, 0));
  std::vector<std::vector<char>> moveRel(static_cast<std::size_t>(numWebs), std::vector<char>(numWebs, 0));
  {
    std::vector<int> webDefCount(static_cast<std::size_t>(numWebs), 0);
    for (int d = 0; d < numDefs; ++d) webDefCount[static_cast<std::size_t>(webOf[static_cast<std::size_t>(d)])]++;
    for (int i = 0; i < n; ++i) {
      if (!result.reachable[static_cast<std::size_t>(i)]) continue;
      if (genDef[static_cast<std::size_t>(i)] < 0) continue;
      const Instruction& in = proto.code[static_cast<std::size_t>(i)];
      const int dw = webOf[static_cast<std::size_t>(genDef[static_cast<std::size_t>(i)])];
      int sw = -1;
      switch (in.op) {
        case OpCode::MOVE: sw = webAt(i, in.b); break;
        case OpCode::GETLOCAL: sw = webAt(i, numRegs + in.b); break;
        case OpCode::DECLLOCAL:
        case OpCode::SETLOCAL: sw = webAt(i, in.a); break;
        default: break;
      }
      if (sw >= 0 && sw != dw && webDefCount[static_cast<std::size_t>(dw)] == 1) {
        moveRel[static_cast<std::size_t>(sw)][static_cast<std::size_t>(dw)] = 1;
        moveRel[static_cast<std::size_t>(dw)][static_cast<std::size_t>(sw)] = 1;
      }
    }
  }
  auto addEdge = [&](int a, int b) {
    if (a >= 0 && b >= 0 && a != b && !moveRel[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)]) {
      adj[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)] = 1;
      adj[static_cast<std::size_t>(b)][static_cast<std::size_t>(a)] = 1;
    }
  };
  for (int i = 0; i < n; ++i) {
    if (!result.reachable[static_cast<std::size_t>(i)]) continue;
    std::vector<int> inw;
    std::vector<int> outw;
    for (int s = 0; s < total; ++s) {
      if (liveIn[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] && webIn[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] >= 0) {
        inw.push_back(webIn[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)]);
      }
      if (liveOut[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] && webOut[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] >= 0) {
        outw.push_back(webOut[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)]);
      }
    }
    for (std::size_t x = 0; x < inw.size(); ++x) {
      for (std::size_t y = x + 1; y < inw.size(); ++y) addEdge(inw[x], inw[y]);
    }
    for (std::size_t x = 0; x < outw.size(); ++x) {
      for (std::size_t y = x + 1; y < outw.size(); ++y) addEdge(outw[x], outw[y]);
    }
  }

  // Copy coalescing: a copy `dst = src` whose two webs do not interfere can
  // share a register, which removes the move. The candidates are the copies
  // themselves and an arithmetic definition that overwrites one of its operands
  // (`s = s + x`), which becomes an in-place update.
  std::vector<std::pair<int, int>> copies;
  for (int i = 0; i < n; ++i) {
    if (!result.reachable[static_cast<std::size_t>(i)]) continue;
    if (genDef[static_cast<std::size_t>(i)] < 0) continue;
    const Instruction& in = proto.code[static_cast<std::size_t>(i)];
    const int dw = webOf[static_cast<std::size_t>(genDef[static_cast<std::size_t>(i)])];
    auto note = [&](int srcWeb) { if (srcWeb >= 0 && srcWeb != dw) copies.emplace_back(srcWeb, dw); };
    switch (in.op) {
      case OpCode::MOVE:
        note(webAt(i, in.b));
        break;
      case OpCode::GETLOCAL:
        note(webAt(i, numRegs + in.b));
        break;
      case OpCode::DECLLOCAL:
      case OpCode::SETLOCAL:
        note(webAt(i, in.a));
        break;
      case OpCode::ADD:
      case OpCode::SUB:
      case OpCode::MUL:
      case OpCode::BAND:
      case OpCode::BOR:
      case OpCode::BXOR:
        if (in.a == in.b) note(webAt(i, in.b));
        if (in.a == in.c) note(webAt(i, in.c));
        break;
      default:
        break;
    }
  }

  std::vector<int> wuf(static_cast<std::size_t>(numWebs));
  for (int w = 0; w < numWebs; ++w) wuf[static_cast<std::size_t>(w)] = w;
  auto wfind = [&](int x) {
    while (wuf[static_cast<std::size_t>(x)] != x) {
      wuf[static_cast<std::size_t>(x)] = wuf[static_cast<std::size_t>(wuf[static_cast<std::size_t>(x)])];
      x = wuf[static_cast<std::size_t>(x)];
    }
    return x;
  };
  std::vector<std::vector<int>> members(static_cast<std::size_t>(numWebs));
  for (int w = 0; w < numWebs; ++w) members[static_cast<std::size_t>(w)].push_back(w);
  for (const auto& c : copies) {
    const int ra = wfind(c.first);
    const int rb = wfind(c.second);
    if (ra == rb) continue;
    bool clash = false;
    for (int x : members[static_cast<std::size_t>(ra)]) {
      for (int y = 0; y < numWebs && !clash; ++y) {
        if (adj[static_cast<std::size_t>(x)][static_cast<std::size_t>(y)] && wfind(y) == rb) clash = true;
      }
      if (clash) break;
    }
    for (int y : members[static_cast<std::size_t>(rb)]) {
      for (int x = 0; x < numWebs && !clash; ++x) {
        if (adj[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] && wfind(x) == ra) clash = true;
      }
      if (clash) break;
    }
    if (clash) continue;
    // Keep the merged web colorable: its distinct neighbour roots must be fewer
    // than the pool, or the merge only creates a spill.
    std::vector<int> nbrs;
    auto addNbr = [&](int root) {
      for (int z : nbrs) {
        if (z == root) return;
      }
      nbrs.push_back(root);
    };
    for (int x : members[static_cast<std::size_t>(ra)]) {
      for (int y = 0; y < numWebs; ++y) {
        if (adj[static_cast<std::size_t>(x)][static_cast<std::size_t>(y)]) addNbr(wfind(y));
      }
    }
    for (int y : members[static_cast<std::size_t>(rb)]) {
      for (int x = 0; x < numWebs; ++x) {
        if (adj[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)]) addNbr(wfind(x));
      }
    }
    if (static_cast<int>(nbrs.size()) >= K) continue;
    wuf[static_cast<std::size_t>(rb)] = ra;
    for (int y : members[static_cast<std::size_t>(rb)]) members[static_cast<std::size_t>(ra)].push_back(y);
    members[static_cast<std::size_t>(rb)].clear();
  }

  // Greedy colouring on the merged interference graph; a web with no free colour
  // keeps its stack slot.
  std::vector<int> color(static_cast<std::size_t>(numWebs), -1);
  {
    std::vector<int> roots;
    for (int w = 0; w < numWebs; ++w) {
      if (wfind(w) == w) roots.push_back(w);
    }
    std::vector<std::vector<int>> fadj(static_cast<std::size_t>(numWebs));
    for (int w = 0; w < numWebs; ++w) {
      for (int y = 0; y < numWebs; ++y) {
        if (!adj[static_cast<std::size_t>(w)][static_cast<std::size_t>(y)]) continue;
        const int a = wfind(w);
        const int b = wfind(y);
        if (a == b) continue;
        bool seen = false;
        for (int z : fadj[static_cast<std::size_t>(a)]) {
          if (z == b) { seen = true; break; }
        }
        if (!seen) {
          fadj[static_cast<std::size_t>(a)].push_back(b);
          fadj[static_cast<std::size_t>(b)].push_back(a);
        }
      }
    }
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
      return fadj[static_cast<std::size_t>(a)].size() > fadj[static_cast<std::size_t>(b)].size();
    });
    for (int r : roots) {
      std::vector<char> used(K, 0);
      for (int nb : fadj[static_cast<std::size_t>(r)]) {
        if (color[static_cast<std::size_t>(nb)] >= 0) used[static_cast<std::size_t>(color[static_cast<std::size_t>(nb)])] = 1;
      }
      int c = -1;
      for (int k = 0; k < K; ++k) {
        if (!used[static_cast<std::size_t>(k)]) { c = k; break; }
      }
      color[static_cast<std::size_t>(r)] = c;
    }
  }
  std::vector<int> regOf(static_cast<std::size_t>(numWebs), -1);
  for (int w = 0; w < numWebs; ++w) regOf[static_cast<std::size_t>(w)] = color[static_cast<std::size_t>(wfind(w))];

  // A web that never appears in a live set is dead: its definition writes a value
  // nothing reads, so the emitter skips it.
  std::vector<char> webLive(static_cast<std::size_t>(numWebs), 0);
  for (int i = 0; i < n; ++i) {
    if (!result.reachable[static_cast<std::size_t>(i)]) continue;
    for (int s = 0; s < total; ++s) {
      if (liveIn[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] && webIn[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] >= 0) {
        webLive[static_cast<std::size_t>(webIn[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)])] = 1;
      }
      if (liveOut[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] && webOut[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] >= 0) {
        webLive[static_cast<std::size_t>(webOut[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)])] = 1;
      }
    }
  }
  for (int d = 0; d < numDefs; ++d) {
    if (defInstr[static_cast<std::size_t>(d)] >= 0 && !webLive[static_cast<std::size_t>(webOf[static_cast<std::size_t>(d)])]) {
      result.deadDef[static_cast<std::size_t>(defInstr[static_cast<std::size_t>(d)])] = true;
    }
  }
  // A fused comparison is not dead: it emits the `cmp; jcc` for its branch.
  for (int i = 0; i < n; ++i) {
    if (result.fusedBranch[static_cast<std::size_t>(i)] >= 0) result.deadDef[static_cast<std::size_t>(i)] = false;
  }

  // Publish the allocation.
  for (int i = 0; i < n; ++i) {
    if (!result.reachable[static_cast<std::size_t>(i)]) continue;
    for (int s = 0; s < total; ++s) {
      if (liveIn[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] && webIn[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] >= 0) {
        result.readReg[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] = regOf[static_cast<std::size_t>(webIn[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)])];
      }
    }
    if (genDef[static_cast<std::size_t>(i)] >= 0 && !result.deadDef[static_cast<std::size_t>(i)]) {
      const int s = defSlot[static_cast<std::size_t>(genDef[static_cast<std::size_t>(i)])];
      result.writeReg[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)] =
          regOf[static_cast<std::size_t>(webOf[static_cast<std::size_t>(genDef[static_cast<std::size_t>(i)])])];
    }
  }
  for (int p = 0; p < proto.numParams && p < numRegs; ++p) {
    if (entryDef[static_cast<std::size_t>(p)] >= 0) {
      result.entryReg[static_cast<std::size_t>(p)] = regOf[static_cast<std::size_t>(webOf[static_cast<std::size_t>(entryDef[static_cast<std::size_t>(p)])])];
    }
  }
}

// Forward dataflow to a fixpoint, then a verification pass. The fixpoint is a
// sound over-approximation: a register the fixpoint cannot pin down to a
// concrete scalar fails the verification pass, so an unsupported proto is
// rejected rather than miscompiled.
Analysis analyze(const Bytecode& proto) {
  Analysis result;
  result.numRegs = std::max(proto.numRegisters, proto.numParams);
  result.numLocals = std::max(proto.numLocals, 0);
  const int total = result.numRegs + result.numLocals;
  const int n = static_cast<int>(proto.code.size());

  // A proto with children, captures or externs could instantiate a closure or
  // call out at run time; none of that is expressible in the subset.
  if (!proto.children.empty() || !proto.upvalues.empty() || !proto.externs.empty() ||
      proto.variadic) {
    return result;
  }

  result.reachable.assign(n, false);
  result.stateIn.assign(n, std::vector<Ty>(total, Ty::Undef));
  if (n == 0) {
    result.supported = true;
    return result;
  }

  // Entry state: the VM only enters the JIT with all-integer arguments, so the
  // parameters start as `Int`. Locals start undeclared.
  result.reachable[0] = true;
  for (int i = 0; i < proto.numParams && i < result.numRegs; ++i) {
    result.stateIn[0][i] = Ty::Int;
  }

  auto indexOk = [&](int r) { return r >= 0 && r < total; };

  // Transfer function: writes the out-state of one instruction in place. A
  // violated requirement by a *concrete* input is not fatal here; the
  // verification pass below catches it once the fixpoint has settled.
  auto transfer = [&](const Instruction& in, std::vector<Ty>& st) {
    auto reg = [&](int r) -> Ty { return indexOk(r) ? st[r] : Ty::Top; };
    auto local = [&](int l) -> Ty { return indexOk(result.numRegs + l) ? st[result.numRegs + l] : Ty::Top; };
    auto setLocal = [&](int l, Ty t) { if (indexOk(result.numRegs + l)) st[result.numRegs + l] = t; };

    switch (in.op) {
      case OpCode::LOADK:
        if (indexOk(in.a) && in.b >= 0 && static_cast<std::size_t>(in.b) < proto.constants.size()) {
          st[in.a] = constTy(proto.constants[static_cast<std::size_t>(in.b)]);
        }
        break;
      case OpCode::MOVE:
        if (indexOk(in.a)) st[in.a] = reg(in.b);
        break;
      case OpCode::ADD:
      case OpCode::SUB:
      case OpCode::MUL:
      case OpCode::BAND:
      case OpCode::BOR:
      case OpCode::BXOR: {
        const Ty x = reg(in.b), y = reg(in.c);
        if (indexOk(in.a)) {
          if (x == Ty::Int && y == Ty::Int) st[in.a] = Ty::Int;
          else if (x == Ty::Undef || y == Ty::Undef) st[in.a] = Ty::Undef;
          else st[in.a] = Ty::Top;
        }
        break;
      }
      case OpCode::UNOP: {
        const Ty x = reg(in.b);
        if (!indexOk(in.a)) break;
        if (static_cast<UnaryOp>(in.c) == UnaryOp::Not) {
          st[in.a] = Ty::Bool;  // `!x` is a bool whatever `x` is
        } else if (x == Ty::Int) {
          st[in.a] = Ty::Int;
        } else {
          st[in.a] = x == Ty::Undef ? Ty::Undef : Ty::Top;
        }
        break;
      }
      case OpCode::EQ:
      case OpCode::NEQ:
      case OpCode::LT:
      case OpCode::LTE:
      case OpCode::GT:
      case OpCode::GTE:
        if (indexOk(in.a)) st[in.a] = Ty::Bool;
        break;
      case OpCode::DECLLOCAL:
      case OpCode::SETLOCAL:
        setLocal(in.b, reg(in.a));
        break;
      case OpCode::GETLOCAL:
        if (indexOk(in.a)) st[in.a] = local(in.b);
        break;
      case OpCode::JMP:
      case OpCode::JMP_IF_FALSE:
      case OpCode::JMP_IF_TRUE:
      case OpCode::RETURN:
      case OpCode::HALT:
        break;
      default:
        break;
    }
  };

  auto successors = [&](int i, std::vector<int>& out) {
    const Instruction& in = proto.code[static_cast<std::size_t>(i)];
    switch (in.op) {
      case OpCode::JMP:
        out.push_back(in.jump);
        break;
      case OpCode::JMP_IF_FALSE:
      case OpCode::JMP_IF_TRUE:
        out.push_back(in.jump);
        out.push_back(i + 1);
        break;
      case OpCode::RETURN:
      case OpCode::HALT:
        break;
      default:
        out.push_back(i + 1);
        break;
    }
  };

  bool changed = true;
  int guard = 0;
  while (changed && guard++ < 100000) {
    changed = false;
    for (int i = 0; i < n; ++i) {
      if (!result.reachable[static_cast<std::size_t>(i)]) continue;
      std::vector<Ty> out = result.stateIn[static_cast<std::size_t>(i)];
      transfer(proto.code[static_cast<std::size_t>(i)], out);
      std::vector<int> succ;
      successors(i, succ);
      for (int s : succ) {
        if (s < 0 || s > n) continue;  // validated in the verification pass
        if (s == n) continue;          // falling off the end returns nil
        if (!result.reachable[static_cast<std::size_t>(s)]) {
          result.reachable[static_cast<std::size_t>(s)] = true;
          changed = true;
        }
        std::vector<Ty>& dst = result.stateIn[static_cast<std::size_t>(s)];
        for (int k = 0; k < total; ++k) {
          const Ty merged = mergeTy(dst[static_cast<std::size_t>(k)], out[static_cast<std::size_t>(k)]);
          if (merged != dst[static_cast<std::size_t>(k)]) {
            dst[static_cast<std::size_t>(k)] = merged;
            changed = true;
          }
        }
      }
    }
  }

  // Verification pass against the settled state. Every register read must be a
  // concrete scalar and every opcode must be one the JIT can lower.
  for (int i = 0; i < n; ++i) {
    if (!result.reachable[static_cast<std::size_t>(i)]) continue;
    const Instruction& in = proto.code[static_cast<std::size_t>(i)];
    const std::vector<Ty>& st = result.stateIn[static_cast<std::size_t>(i)];
    auto reg = [&](int r) -> Ty { return indexOk(r) ? st[static_cast<std::size_t>(r)] : Ty::Top; };
    auto local = [&](int l) -> Ty {
      return indexOk(result.numRegs + l) ? st[static_cast<std::size_t>(result.numRegs + l)] : Ty::Top;
    };
    auto scalarReg = [&](int r) { return isScalar(reg(r)); };
    auto intReg = [&](int r) { return reg(r) == Ty::Int; };

    switch (in.op) {
      case OpCode::LOADK:
        if (!indexOk(in.a) || in.b < 0 || static_cast<std::size_t>(in.b) >= proto.constants.size() ||
            constTy(proto.constants[static_cast<std::size_t>(in.b)]) == Ty::Top) {
          return result;
        }
        break;
      case OpCode::MOVE:
        if (!indexOk(in.a) || !scalarReg(in.b)) return result;
        break;
      case OpCode::ADD:
      case OpCode::SUB:
      case OpCode::MUL:
      case OpCode::BAND:
      case OpCode::BOR:
      case OpCode::BXOR:
        if (!indexOk(in.a) || !intReg(in.b) || !intReg(in.c)) return result;
        break;
      case OpCode::UNOP:
        if (!indexOk(in.a) || !indexOk(in.b)) return result;
        if (static_cast<UnaryOp>(in.c) == UnaryOp::Not) {
          if (!scalarReg(in.b)) return result;
        } else if (static_cast<UnaryOp>(in.c) == UnaryOp::Negate ||
                   static_cast<UnaryOp>(in.c) == UnaryOp::BitNot) {
          if (!intReg(in.b)) return result;
        } else {
          return result;  // unknown selector
        }
        break;
      case OpCode::EQ:
      case OpCode::NEQ:
        if (!indexOk(in.a)) return result;
        if (!((intReg(in.b) && intReg(in.c)) || (reg(in.b) == Ty::Bool && reg(in.c) == Ty::Bool))) {
          return result;
        }
        break;
      case OpCode::LT:
      case OpCode::LTE:
      case OpCode::GT:
      case OpCode::GTE:
        if (!indexOk(in.a) || !intReg(in.b) || !intReg(in.c)) return result;
        break;
      case OpCode::JMP:
        if (in.jump < 0 || in.jump > n) return result;
        break;
      case OpCode::JMP_IF_FALSE:
      case OpCode::JMP_IF_TRUE:
        if (!indexOk(in.a) || !scalarReg(in.a) || in.jump < 0 || in.jump > n) return result;
        break;
      case OpCode::DECLLOCAL:
        // The local is being declared here, so only the source must be scalar;
        // the slot itself is Undef until this instruction defines it.
        if (!scalarReg(in.a) || !indexOk(result.numRegs + in.b)) return result;
        break;
      case OpCode::SETLOCAL:
        if (!scalarReg(in.a) || !indexOk(result.numRegs + in.b) || !isScalar(local(in.b))) return result;
        break;
      case OpCode::GETLOCAL:
        if (!indexOk(in.a) || !indexOk(result.numRegs + in.b) || !isScalar(local(in.b))) return result;
        break;
      case OpCode::RETURN:
        if (!indexOk(in.a) || !scalarReg(in.a)) return result;
        break;
      case OpCode::HALT:
        break;
      default:
        return result;  // globals, calls, closures, collections, iteration
    }
  }

  // Register-resident-slot discovery (see `Analysis::regResident`). Runs after
  // verification, so every reachable instruction is already known valid and
  // every operand read is a concrete scalar. A slot is register-resident when
  // every reachable read observes a scalar and every write stores one; the
  // verifier already guarantees reads, so only the writes need a pass. The
  // scalar kind at each use is read back from `stateIn` when the body is
  // emitted, so a slot may change kind (`bool` result feeding an `int` slot,
  // and back) without leaving its register.
  result.regResident.assign(static_cast<std::size_t>(total), false);
  {
    std::vector<bool> everRead(static_cast<std::size_t>(total), false);
    std::vector<bool> scalarWrite(static_cast<std::size_t>(total), true);
    auto noteRead = [&](int slot) {
      if (indexOk(slot)) everRead[static_cast<std::size_t>(slot)] = true;
    };
    auto noteWrite = [&](int slot, Ty t) {
      if (!indexOk(slot)) return;
      if (!isScalar(t)) scalarWrite[static_cast<std::size_t>(slot)] = false;
    };
    // A parameter enters as an `Int` (the VM only enters the JIT with
    // all-integer arguments), a write with no instruction behind it.
    for (int i = 0; i < proto.numParams && i < result.numRegs; ++i) noteWrite(i, Ty::Int);
    for (int i = 0; i < n; ++i) {
      if (!result.reachable[static_cast<std::size_t>(i)]) continue;
      const Instruction& in = proto.code[static_cast<std::size_t>(i)];
      const std::vector<Ty>& st = result.stateIn[static_cast<std::size_t>(i)];
      auto ty = [&](int r) { return indexOk(r) ? st[static_cast<std::size_t>(r)] : Ty::Top; };
      auto lty = [&](int l) {
        const int slot = result.numRegs + l;
        return indexOk(slot) ? st[static_cast<std::size_t>(slot)] : Ty::Top;
      };
      switch (in.op) {
        case OpCode::LOADK:
          noteWrite(in.a, constTy(proto.constants[static_cast<std::size_t>(in.b)]));
          break;
        case OpCode::MOVE:
          noteRead(in.b);
          noteWrite(in.a, ty(in.b));
          break;
        case OpCode::ADD:
        case OpCode::SUB:
        case OpCode::MUL:
        case OpCode::BAND:
        case OpCode::BOR:
        case OpCode::BXOR:
          noteRead(in.b);
          noteRead(in.c);
          noteWrite(in.a, Ty::Int);
          break;
        case OpCode::UNOP:
          noteRead(in.b);
          noteWrite(in.a, static_cast<UnaryOp>(in.c) == UnaryOp::Not ? Ty::Bool : Ty::Int);
          break;
        case OpCode::EQ:
        case OpCode::NEQ:
        case OpCode::LT:
        case OpCode::LTE:
        case OpCode::GT:
        case OpCode::GTE:
          noteRead(in.b);
          noteRead(in.c);
          noteWrite(in.a, Ty::Bool);
          break;
        case OpCode::JMP_IF_FALSE:
        case OpCode::JMP_IF_TRUE:
          noteRead(in.a);
          break;
        case OpCode::DECLLOCAL:
        case OpCode::SETLOCAL:
          noteRead(in.a);
          noteWrite(result.numRegs + in.b, ty(in.a));
          break;
        case OpCode::GETLOCAL:
          noteRead(result.numRegs + in.b);
          noteWrite(in.a, lty(in.b));
          break;
        case OpCode::RETURN:
          noteRead(in.a);
          break;
        default:
          break;
      }
    }
    for (int s = 0; s < total; ++s) {
      const std::size_t slot = static_cast<std::size_t>(s);
      result.regResident[slot] = everRead[slot] && scalarWrite[slot];
    }
  }

  // Constant propagation (see `Analysis::hasConst`). A slot is constant on entry
  // to an instruction when *every* reachable predecessor agrees on the same
  // `LOADK` value and no other instruction wrote the slot on the way in. The
  // lattice is `Undef` (no path yet), `Const(kind, value)`, and `Top` (not
  // constant): two paths with different values meet at `Top`. Only `LOADK` seeds
  // a constant; a write of anything else makes the slot `Top`. The emitter then
  // folds an operation whose operands are both constant and picks an immediate
  // form when one of them is.
  result.hasConst.assign(n, std::vector<bool>(total, false));
  result.constVal.assign(n, std::vector<std::int64_t>(total, 0));
  result.constKind.assign(n, std::vector<Ty>(total, Ty::Undef));
  {
    // -1 = Undef (unseen), 0 = Top (not constant), 1 = constant.
    std::vector<std::vector<std::int8_t>> state(n, std::vector<std::int8_t>(total, -1));
    std::vector<std::vector<std::int64_t>> value(n, std::vector<std::int64_t>(total, 0));
    std::vector<std::vector<Ty>> kind(n, std::vector<Ty>(total, Ty::Undef));
    // Out-state of each instruction (its result), kept for the final pass so a
    // definition's own value can be read back once the fixpoint has settled.
    std::vector<std::vector<std::int8_t>> outStateArr(n, std::vector<std::int8_t>(total, -1));
    std::vector<std::vector<std::int64_t>> outValArr(n, std::vector<std::int64_t>(total, 0));
    std::vector<std::vector<Ty>> outKindArr(n, std::vector<Ty>(total, Ty::Undef));
    // Entry: the parameters are not constants.
    for (int i = 0; i < proto.numParams && i < result.numRegs; ++i) state[0][i] = 0;

    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 100000) {
      changed = false;
      for (int i = 0; i < n; ++i) {
        if (!result.reachable[static_cast<std::size_t>(i)]) continue;
        const Instruction& in = proto.code[static_cast<std::size_t>(i)];
        std::vector<std::int8_t> outState = state[i];
        std::vector<std::int64_t> outValue = value[i];
        std::vector<Ty> outKind = kind[i];
        // Apply the instruction's effect to the out-state (in place). A write
        // overwrites whatever the slot held on entry: a constant write makes it
        // `Const`, any other write makes it `Top`.
        auto writeConst = [&](int slot, bool isConst, std::int64_t v, Ty k) {
          if (!indexOk(slot)) return;
          if (isConst) {
            outState[slot] = 1;
            outValue[slot] = v;
            outKind[slot] = k;
          } else {
            outState[slot] = 0;
          }
        };
        // Read a slot as a constant, or `false` when it is not.
        auto readConst = [&](int slot, std::int64_t& v, Ty& k) {
          if (!indexOk(slot) || outState[slot] != 1) return false;
          v = outValue[slot];
          k = outKind[slot];
          return true;
        };
        // An arithmetic/comparison result is constant only when both operands
        // are; fold it exactly like the interpreter, else mark it Top.
        auto foldInto = [&](int dst, bool isIntOp) {
          std::int64_t av = 0, bv = 0;
          Ty ak = Ty::Undef, bk = Ty::Undef;
          const bool aConst = readConst(in.b, av, ak);
          const bool bConst = readConst(in.c, bv, bk);
          if (!aConst || !bConst) {
            writeConst(dst, false, 0, Ty::Undef);
            return;
          }
          std::int64_t res = 0;
          bool ok = false;
          if (isIntOp) {
            ok = foldBinaryInt(in.op, av, bv, res);
          } else {
            bool b = false;
            ok = foldCompareInt(in.op, av, bv, b);
            res = b ? 1 : 0;
          }
          if (ok) {
            writeConst(dst, true, res, isIntOp ? Ty::Int : Ty::Bool);
          } else {
            writeConst(dst, false, 0, Ty::Undef);
          }
        };
        switch (in.op) {
          case OpCode::LOADK: {
            const Value& k = proto.constants[static_cast<std::size_t>(in.b)];
            const Ty t = constTy(k);
            const std::int64_t payload =
                (t == Ty::Int) ? k.asInt() : (t == Ty::Bool ? (k.asBool() ? 1 : 0) : 0);
            writeConst(in.a, t != Ty::Top, payload, t);
            break;
          }
          case OpCode::MOVE:
            if (outState[in.b] == 1) writeConst(in.a, true, outValue[in.b], outKind[in.b]);
            else writeConst(in.a, false, 0, Ty::Undef);
            break;
          case OpCode::ADD:
          case OpCode::SUB:
          case OpCode::MUL:
          case OpCode::BAND:
          case OpCode::BOR:
          case OpCode::BXOR:
            foldInto(in.a, true);
            break;
          case OpCode::EQ:
          case OpCode::NEQ:
          case OpCode::LT:
          case OpCode::LTE:
          case OpCode::GT:
          case OpCode::GTE:
            foldInto(in.a, false);
            break;
          case OpCode::UNOP: {
            const UnaryOp op = static_cast<UnaryOp>(in.c);
            std::int64_t bv = 0;
            Ty bk = Ty::Undef;
            if (op == UnaryOp::Not) {
              if (readConst(in.b, bv, bk)) writeConst(in.a, true, truthyConst(bk, bv) ? 0 : 1, Ty::Bool);
              else writeConst(in.a, false, 0, Ty::Undef);
            } else {
              std::int64_t res = 0;
              if (readConst(in.b, bv, bk) && foldUnaryInt(op, bv, res)) {
                writeConst(in.a, true, res, Ty::Int);
              } else {
                writeConst(in.a, false, 0, Ty::Undef);
              }
            }
            break;
          }
          case OpCode::DECLLOCAL:
          case OpCode::SETLOCAL:
            if (outState[in.a] == 1) {
              writeConst(result.numRegs + in.b, true, outValue[in.a], outKind[in.a]);
            } else {
              writeConst(result.numRegs + in.b, false, 0, Ty::Undef);
            }
            break;
          case OpCode::GETLOCAL: {
            const int slot = result.numRegs + in.b;
            if (indexOk(slot) && outState[slot] == 1) {
              writeConst(in.a, true, outValue[slot], outKind[slot]);
            } else {
              writeConst(in.a, false, 0, Ty::Undef);
            }
            break;
          }
          default:
            break;
        }
        // Remember this instruction's out-state; the final (no-change) sweep
        // leaves the settled values here for the definition check below.
        outStateArr[i] = outState;
        outValArr[i] = outValue;
        outKindArr[i] = outKind;
        // Propagate to the successors, meeting each slot.
        std::vector<int> succ;
        successors(i, succ);
        for (int s : succ) {
          if (s < 0 || s >= n) continue;
          for (int k = 0; k < total; ++k) {
            const std::int8_t cur = state[s][k];
            const std::int8_t inc = outState[k];
            if (inc == -1) continue;  // nothing known from this path
            if (cur == -1) {
              state[s][k] = inc;
              value[s][k] = outValue[k];
              kind[s][k] = outKind[k];
              changed = true;
            } else if (cur == 1) {
              if (inc == 0 || value[s][k] != outValue[k] || kind[s][k] != outKind[k]) {
                state[s][k] = 0;
                changed = true;
              }
            }
            // cur == 0 stays Top.
          }
        }
      }
    }
    for (int i = 0; i < n; ++i) {
      for (int s = 0; s < total; ++s) {
        if (state[i][s] == 1) {
          result.hasConst[i][s] = true;
          result.constVal[i][s] = value[i][s];
          result.constKind[i][s] = kind[i][s];
        }
      }
    }
    // A definition whose *result* is a pinned constant can skip its opcode. Only
    // a pure opcode qualifies; a `MOVE`/`GETLOCAL` still needs the copy because
    // the destination may not be constant elsewhere, and a `LOADK` already just
    // materializes a constant.
    result.defConst.assign(n, false);
    result.defVal.assign(n, 0);
    result.defKind.assign(n, Ty::Undef);
    for (int i = 0; i < n; ++i) {
      if (!result.reachable[static_cast<std::size_t>(i)]) continue;
      const Instruction& in = proto.code[static_cast<std::size_t>(i)];
      bool pure = false;
      switch (in.op) {
        case OpCode::ADD:
        case OpCode::SUB:
        case OpCode::MUL:
        case OpCode::BAND:
        case OpCode::BOR:
        case OpCode::BXOR:
        case OpCode::EQ:
        case OpCode::NEQ:
        case OpCode::LT:
        case OpCode::LTE:
        case OpCode::GT:
        case OpCode::GTE:
        case OpCode::UNOP:
          pure = true;
          break;
        default:
          break;
      }
      if (pure && indexOk(in.a) && outStateArr[i][in.a] == 1) {
        result.defConst[i] = true;
        result.defVal[i] = outValArr[i][in.a];
        result.defKind[i] = outKindArr[i][in.a];
      }
    }
  }

  // Dead-constant analysis now lives in `allocateRegisters`: a definition whose
  // web no live read observes is a `deadDef` the emitter skips, which covers the
  // constant a folded expression leaves behind. See `Analysis::deadDef`.

  // Compare-branch fusion (see `Analysis::fusedBranch`). Backward liveness over
  // the reachable instructions; a comparison's boolean result may be dropped
  // only when it is dead once the branch that consumes it has run, and only when
  // that branch is reached by fall-through alone (never a jump target), so
  // skipping it cannot lose a re-test of the condition.
  result.fusedBranch.assign(n, -1);
  {
    auto reads = [&](const Instruction& in, std::vector<int>& out) {
      instrReads(in, result.numRegs, total, out);
    };
    auto writes = [&](const Instruction& in, std::vector<int>& out) {
      instrWrites(in, result.numRegs, total, out);
    };

    std::vector<std::vector<bool>> liveIn(n, std::vector<bool>(total, false));
    std::vector<std::vector<bool>> liveOut(n, std::vector<bool>(total, false));
    bool lchanged = true;
    int lguard = 0;
    while (lchanged && lguard++ < 100000) {
      lchanged = false;
      for (int i = n - 1; i >= 0; --i) {
        if (!result.reachable[static_cast<std::size_t>(i)]) continue;
        std::vector<bool> out(total, false);
        std::vector<int> succ;
        successors(i, succ);
        for (int s : succ) {
          if (s < 0 || s >= n || !result.reachable[static_cast<std::size_t>(s)]) continue;
          for (int k = 0; k < total; ++k) {
            if (liveIn[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)]) {
              out[static_cast<std::size_t>(k)] = true;
            }
          }
        }
        std::vector<bool> in = out;
        std::vector<int> wr;
        writes(proto.code[static_cast<std::size_t>(i)], wr);
        for (int w : wr) in[static_cast<std::size_t>(w)] = false;
        std::vector<int> rd;
        reads(proto.code[static_cast<std::size_t>(i)], rd);
        for (int r : rd) in[static_cast<std::size_t>(r)] = true;
        const std::size_t ii = static_cast<std::size_t>(i);
        if (out != liveOut[ii] || in != liveIn[ii]) {
          liveOut[ii] = out;
          liveIn[ii] = in;
          lchanged = true;
        }
      }
    }

    std::vector<bool> isJumpTarget(n, false);
    for (int i = 0; i < n; ++i) {
      if (!result.reachable[static_cast<std::size_t>(i)]) continue;
      const Instruction& in = proto.code[static_cast<std::size_t>(i)];
      if ((in.op == OpCode::JMP || in.op == OpCode::JMP_IF_FALSE ||
           in.op == OpCode::JMP_IF_TRUE) &&
          in.jump >= 0 && in.jump < n) {
        isJumpTarget[static_cast<std::size_t>(in.jump)] = true;
      }
    }

    for (int i = 0; i + 1 < n; ++i) {
      if (!result.reachable[static_cast<std::size_t>(i)]) continue;
      const Instruction& in = proto.code[static_cast<std::size_t>(i)];
      switch (in.op) {
        case OpCode::EQ:
        case OpCode::NEQ:
        case OpCode::LT:
        case OpCode::LTE:
        case OpCode::GT:
        case OpCode::GTE:
          break;
        default:
          continue;
      }
      if (result.defConst[static_cast<std::size_t>(i)]) continue;  // the branch sees a constant
      const int j = i + 1;
      if (!result.reachable[static_cast<std::size_t>(j)]) continue;
      const Instruction& br = proto.code[static_cast<std::size_t>(j)];
      if (br.op != OpCode::JMP_IF_FALSE && br.op != OpCode::JMP_IF_TRUE) continue;
      if (br.a != in.a || !indexOk(in.a)) continue;
      if (isJumpTarget[static_cast<std::size_t>(j)]) continue;
      if (liveOut[static_cast<std::size_t>(j)][static_cast<std::size_t>(in.a)]) continue;
      result.fusedBranch[static_cast<std::size_t>(i)] = j;
    }
  }

  // Register allocation (see `Analysis::readReg`/`writeReg`/`entryReg` and
  // `allocateRegisters`). Runs last: it needs the fusion result to know which
  // definitions the emitter still emits.
  allocateRegisters(proto, result);

  result.supported = true;
  return result;
}

// ------------------------------------------------------------------ lowering

// Emits the native body of a supported proto. The convention matches
// `JitFunction::Entry`: args in rdi, argc in rsi, out pointer in rdx.
std::vector<std::uint8_t> emitProto(const Bytecode& proto, const Analysis& analysis) {
  CodeGen cg(Target::linuxX64());

  const int numRegs = analysis.numRegs;
  const int numLocals = analysis.numLocals;
  const int n = static_cast<int>(proto.code.size());
  const int total = numRegs + numLocals;
  const std::int64_t frameSize = 16 * (static_cast<std::int64_t>(total) + 1);

  // A slot is numbered 0..numRegs-1 for registers and numRegs..total-1 for
  // locals, so one formula places every slot in the frame.
  auto slotDisp = [](int s) { return -16 * (static_cast<std::int64_t>(s) + 1); };
  auto outDisp = [&]() { return slotDisp(total); };

  // Register allocation lookup. `curI` is the instruction being emitted, set at
  // the top of the loop below; these helpers resolve a slot's register through
  // the allocation tables. A slot is read through `analysis.readReg` and defined
  // through `analysis.writeReg` (or `entryReg` for a parameter's incoming
  // value), so one instruction can read and write the same slot through
  // different registers. An entry is an index into `kJitRegPool`, or -1 for a
  // slot that keeps its 16-byte stack slot.
  int curI = -1;
  auto readRegOf = [&](int i, int slot) -> int {
    if (slot < 0 || slot >= total) return -1;
    const std::size_t ii = static_cast<std::size_t>(i);
    if (ii >= analysis.readReg.size()) return -1;
    return analysis.readReg[ii][static_cast<std::size_t>(slot)];
  };
  auto writeRegOf = [&](int i, int slot) -> int {
    if (slot < 0 || slot >= total) return -1;
    const std::size_t ii = static_cast<std::size_t>(i);
    if (ii >= analysis.writeReg.size()) return -1;
    return analysis.writeReg[ii][static_cast<std::size_t>(slot)];
  };
  auto poolReg = [&](int idx) { return kJitRegPool[idx]; };
  auto entryRegOf = [&](int slot) -> int {
    if (slot < 0 || slot >= total) return -1;
    return analysis.entryReg[static_cast<std::size_t>(slot)];
  };

  // One label per instruction plus one for the implicit end.
  std::vector<CodeGen::Label> labels(static_cast<std::size_t>(n) + 1);
  for (auto& label : labels) label = cg.newLabel();
  const CodeGen::Label endLabel = labels[static_cast<std::size_t>(n)];

  // Writes a full tagged stack slot (payload qword + tag byte).
  auto storeSlot = [&](std::int64_t disp, std::uint8_t tag, std::int64_t payload) {
    cg.movImm(X64Reg::R11, payload);
    cg.movToMem(X64Reg::Rbp, disp + 8, X64Reg::R11);
    cg.movImmByteToMem(X64Reg::Rbp, disp, tag);
  };
  // Copies a tagged stack slot (payload qword + tag byte) without touching
  // refcounts: only nil/bool/int reach here, and none of them owns a reference.
  auto copySlot = [&](std::int64_t dstDisp, std::int64_t srcDisp) {
    cg.movFromMem(X64Reg::R10, X64Reg::Rbp, srcDisp + 8);
    cg.movToMem(X64Reg::Rbp, dstDisp + 8, X64Reg::R10);
    cg.movzxFromMem(X64Reg::R11, X64Reg::Rbp, srcDisp);
    cg.movByteToMem(X64Reg::Rbp, dstDisp, X64Reg::R11);
  };
  // Loads the raw scalar payload of a slot into `dst`, reading the slot at
  // instruction `i`: from its allocated register when it has one, else from the
  // stack payload word. The tag is not read; the caller knows the slot's scalar
  // kind from the dataflow state.
  auto loadScalarAt = [&](int i, X64Reg dst, int slot) {
    const int r = readRegOf(i, slot);
    if (r >= 0) {
      if (poolReg(r) != dst) cg.movReg(dst, poolReg(r));
    } else {
      cg.movFromMem(dst, X64Reg::Rbp, slotDisp(slot) + 8);
    }
  };
  // Writes a canonical scalar payload (`src`: a full int64, or 0/1 for a bool)
  // of kind `kind` to a slot's register allocated at instruction `i`, else to
  // the stack slot plus the kind's tag.
  auto storeScalarAt = [&](int i, int slot, Ty kind, X64Reg src) {
    const int r = writeRegOf(i, slot);
    if (r >= 0) {
      if (poolReg(r) != src) cg.movReg(poolReg(r), src);
    } else {
      cg.movToMem(X64Reg::Rbp, slotDisp(slot) + 8, src);
      cg.movImmByteToMem(X64Reg::Rbp, slotDisp(slot), tagOf(kind));
    }
  };
  // Copies a whole slot between the two representations at instruction `i`.
  // `kind` is the scalar kind of the source at this point, needed when the
  // destination is a stack slot (to pick its tag).
  auto copySlotValueAt = [&](int i, int dstSlot, Ty kind, int srcSlot) {
    const int sr = readRegOf(i, srcSlot);
    const int dr = writeRegOf(i, dstSlot);
    if (sr >= 0 && dr >= 0) {
      if (poolReg(dr) != poolReg(sr)) cg.movReg(poolReg(dr), poolReg(sr));
    } else if (sr >= 0) {
      storeScalarAt(i, dstSlot, kind, poolReg(sr));
    } else if (dr >= 0) {
      loadScalarAt(i, poolReg(dr), srcSlot);
    } else {
      copySlot(slotDisp(dstSlot), slotDisp(srcSlot));
    }
  };
  auto epilogue = [&]() {
    cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
    cg.pop(X64Reg::Rbp);
    cg.retInsn();
  };
  auto returnValue = [&](int i, int slot, Ty kind) {
    cg.movFromMem(X64Reg::R11, X64Reg::Rbp, outDisp());  // out pointer
    const int r = readRegOf(i, slot);
    if (r >= 0) {
      cg.movToMem(X64Reg::R11, 8, poolReg(r));
      cg.movImmByteToMem(X64Reg::R11, 0, tagOf(kind));
    } else {
      cg.movFromMem(X64Reg::R10, X64Reg::Rbp, slotDisp(slot) + 8);  // payload
      cg.movToMem(X64Reg::R11, 8, X64Reg::R10);
      cg.movzxFromMem(X64Reg::R10, X64Reg::Rbp, slotDisp(slot));  // tag
      cg.movByteToMem(X64Reg::R11, 0, X64Reg::R10);
    }
    epilogue();
  };
  auto returnNil = [&]() {
    cg.movFromMem(X64Reg::R11, X64Reg::Rbp, outDisp());
    cg.movImm(X64Reg::R10, 0);
    cg.movToMem(X64Reg::R11, 8, X64Reg::R10);
    cg.movByteToMem(X64Reg::R11, 0, X64Reg::R10);
    epilogue();
  };

  // Prologue: frame, save the out pointer, copy the incoming arguments to their
  // stack slots, then lift the ones with an entry register. The lift runs after
  // the whole copy loop, so it may freely use rdi/rsi (the argument pointer and
  // count) as destinations.
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.subRegImm32(X64Reg::Rsp, static_cast<std::int32_t>(frameSize));
  cg.movToMem(X64Reg::Rbp, outDisp(), X64Reg::Rdx);
  for (int i = 0; i < proto.numParams && i < numRegs; ++i) {
    cg.movFromMem(X64Reg::R10, X64Reg::Rdi, 16 * i + 8);
    cg.movToMem(X64Reg::Rbp, slotDisp(i) + 8, X64Reg::R10);
    cg.movzxFromMem(X64Reg::R10, X64Reg::Rdi, 16 * i);
    cg.movByteToMem(X64Reg::Rbp, slotDisp(i), X64Reg::R10);
  }
  for (int i = 0; i < proto.numParams && i < numRegs; ++i) {
    if (entryRegOf(i) >= 0) cg.movFromMem(poolReg(entryRegOf(i)), X64Reg::Rbp, slotDisp(i) + 8);
  }

  // The compile-time constant of `slot` on entry to instruction `i`, if the
  // propagation pinned one down. Operands are read before the instruction
  // writes, so the entry state is exactly the value an operand holds.
  auto constIn = [&](int i, int slot, std::int64_t& value, Ty& kind) {
    if (slot < 0 || slot >= total) return false;
    const std::size_t ii = static_cast<std::size_t>(i);
    if (ii >= analysis.hasConst.size() ||
        static_cast<std::size_t>(slot) >= analysis.hasConst[ii].size()) {
      return false;
    }
    if (!analysis.hasConst[ii][static_cast<std::size_t>(slot)]) return false;
    value = analysis.constVal[ii][static_cast<std::size_t>(slot)];
    kind = analysis.constKind[ii][static_cast<std::size_t>(slot)];
    return true;
  };
  // Writes a scalar constant to a slot at the current instruction: its
  // allocated register, or its stack slot with the kind's tag.
  auto emitConst = [&](int slot, Ty kind, std::int64_t value) {
    const int r = writeRegOf(curI, slot);
    if (r >= 0) {
      cg.movImm(poolReg(r), value);
    } else {
      storeSlot(slotDisp(slot), tagOf(kind), value);
    }
  };
  auto applyReg = [&](OpCode op, X64Reg dst, X64Reg src) {
    switch (op) {
      case OpCode::ADD: cg.addReg(dst, src); break;
      case OpCode::SUB: cg.subReg(dst, src); break;
      case OpCode::MUL: cg.imulReg(dst, src); break;
      case OpCode::BAND: cg.andReg(dst, src); break;
      case OpCode::BOR: cg.orReg(dst, src); break;
      default: cg.xorReg(dst, src); break;  // BXOR
    }
  };
  // `dst = lhs OP rhs` with both operands in slots, at the current instruction.
  // The destination's register is spent in place when it already holds an
  // operand; otherwise the result is computed in scratch and stored. A register
  // operand is used directly, so no copy stands between the operands and the
  // ALU. All three registers are resolved at the current instruction, so the
  // same slot may be read and written through different registers.
  auto emitBinReg = [&](OpCode op, int dst, int lhs, int rhs) {
    const int dr = writeRegOf(curI, dst);
    const int lr = readRegOf(curI, lhs);
    const int rr = readRegOf(curI, rhs);
    if (dr >= 0) {
      const X64Reg d = poolReg(dr);
      if (dr == lr) {
        // `d` already holds the left operand (the right one too, when it is the
        // same register, so `d OP d` is still right).
        if (rr >= 0) applyReg(op, d, poolReg(rr));
        else { loadScalarAt(curI, X64Reg::R10, rhs); applyReg(op, d, X64Reg::R10); }
        return;
      }
      if (dr == rr && isCommutative(op)) {
        if (lr >= 0) applyReg(op, d, poolReg(lr));
        else { loadScalarAt(curI, X64Reg::R10, lhs); applyReg(op, d, X64Reg::R10); }
        return;
      }
      if (dr != rr) {
        // `d` is a distinct register, free until here: compute into it
        // directly, without a scratch copy.
        loadScalarAt(curI, d, lhs);
        if (rr >= 0) applyReg(op, d, poolReg(rr));
        else { loadScalarAt(curI, X64Reg::R10, rhs); applyReg(op, d, X64Reg::R10); }
        return;
      }
      // `dr == rr` with a non-commutative op (only `SUB`): the old right operand
      // lives in `dst`, so the scratch path below reads it before the
      // destination is overwritten.
    }
    loadScalarAt(curI, X64Reg::R10, lhs);
    loadScalarAt(curI, X64Reg::R11, rhs);
    applyReg(op, X64Reg::R10, X64Reg::R11);
    storeScalarAt(curI, dst, Ty::Int, X64Reg::R10);
  };
  // `dst = var OP imm` for an integer immediate: identity and absorbing cases
  // first (no instruction), then the immediate form, a shift for a power of two,
  // or a materialized register when the constant is out of imm32 range. The
  // arithmetic runs in the destination's register when it has one, so `i * 2`
  // becomes a single `shl`.
  auto emitBinImm = [&](OpCode op, int dst, int varSlot, std::int64_t imm) {
    if ((op == OpCode::ADD || op == OpCode::SUB || op == OpCode::BOR ||
         op == OpCode::BXOR) && imm == 0) {
      copySlotValueAt(curI, dst, Ty::Int, varSlot);
      return;
    }
    if (op == OpCode::MUL && imm == 1) {
      copySlotValueAt(curI, dst, Ty::Int, varSlot);
      return;
    }
    if ((op == OpCode::MUL || op == OpCode::BAND) && imm == 0) {
      emitConst(dst, Ty::Int, 0);
      return;
    }
    if (op == OpCode::BAND && imm == -1) {
      copySlotValueAt(curI, dst, Ty::Int, varSlot);
      return;
    }
    // Compute in the destination register when it has one: `dst` is dead until
    // this instruction, so it is free scratch, and the final store becomes a
    // no-op. Otherwise fall back to r10.
    const int dr = writeRegOf(curI, dst);
    const X64Reg work = dr >= 0 ? poolReg(dr) : X64Reg::R10;
    loadScalarAt(curI, work, varSlot);
    const bool immOk = fitsImm32(imm);
    switch (op) {
      case OpCode::ADD:
        if (immOk) cg.addImm32(work, static_cast<std::int32_t>(imm));
        else { cg.movImm(X64Reg::R11, imm); cg.addReg(work, X64Reg::R11); }
        break;
      case OpCode::SUB:
        if (immOk) cg.subRegImm32(work, static_cast<std::int32_t>(imm));
        else { cg.movImm(X64Reg::R11, imm); cg.subReg(work, X64Reg::R11); }
        break;
      case OpCode::MUL: {
        int log2 = 0;
        if (isPow2(imm, log2) && log2 >= 1 && log2 <= 63) {
          cg.shlImm8(work, static_cast<std::uint8_t>(log2));
        } else if (immOk) {
          cg.imulImm32(work, work, static_cast<std::int32_t>(imm));
        } else {
          cg.movImm(X64Reg::R11, imm);
          cg.imulReg(work, X64Reg::R11);
        }
        break;
      }
      case OpCode::BAND:
        if (immOk) cg.andImm32(work, static_cast<std::int32_t>(imm));
        else { cg.movImm(X64Reg::R11, imm); cg.andReg(work, X64Reg::R11); }
        break;
      case OpCode::BOR:
        cg.movImm(X64Reg::R11, imm);
        cg.orReg(work, X64Reg::R11);
        break;
      default:
        cg.movImm(X64Reg::R11, imm);
        cg.xorReg(work, X64Reg::R11);
        break;
    }
    storeScalarAt(curI, dst, Ty::Int, work);
  };

  // Emits the `cmp` (or `test`) that leaves the flags for a comparison at `i`,
  // and returns the condition code those flags encode. The caller either
  // materializes a bool (`setcc`) or, when the comparison is fused with the
  // branch that follows, jumps on the code directly. A constant operand becomes
  // an immediate, and a `const OP var` mirrors the condition.
  auto emitCompareFlags = [&](OpCode op, int b, int c, int i) -> Jcc {
    std::int64_t cv = 0;
    Ty ck = Ty::Undef;
    const bool bConst = constIn(i, b, cv, ck);
    std::int64_t dv = 0;
    Ty dk = Ty::Undef;
    const bool cConst = constIn(i, c, dv, dk);
    Jcc cc = compareCc(op);
    if (bConst && !cConst && fitsImm32(cv)) {
      // const OP var  ==  var OP' const (mirror the condition).
      loadScalarAt(i, X64Reg::R10, c);
      cg.cmpImm32(X64Reg::R10, static_cast<std::int32_t>(cv));
      cc = mirrorCc(cc);
    } else if (cConst && !bConst && fitsImm32(dv)) {
      loadScalarAt(i, X64Reg::R10, b);
      cg.cmpImm32(X64Reg::R10, static_cast<std::int32_t>(dv));
    } else if (readRegOf(i, b) >= 0 && readRegOf(i, c) >= 0) {
      // Both operands live in registers: compare them directly, so no scratch
      // copy stands before the compare.
      cg.cmpReg(poolReg(readRegOf(i, b)), poolReg(readRegOf(i, c)));
    } else {
      loadScalarAt(i, X64Reg::R10, b);
      loadScalarAt(i, X64Reg::R11, c);
      cg.cmpReg(X64Reg::R10, X64Reg::R11);
    }
    return cc;
  };

  for (int i = 0; i < n; ++i) {
    if (!analysis.reachable[static_cast<std::size_t>(i)]) continue;
    cg.bind(labels[static_cast<std::size_t>(i)]);
    curI = i;
    // A dead definition has no observable effect: skip it.
    if (analysis.deadDef[static_cast<std::size_t>(i)]) continue;
    const Instruction& in = proto.code[static_cast<std::size_t>(i)];
    const std::vector<Ty>& st = analysis.stateIn[static_cast<std::size_t>(i)];
    auto tyOf = [&](int r) { return st[static_cast<std::size_t>(r)]; };
    auto ltyOf = [&](int l) { return st[static_cast<std::size_t>(numRegs + l)]; };

    switch (in.op) {
      case OpCode::LOADK: {
        const Value& k = proto.constants[static_cast<std::size_t>(in.b)];
        const Ty t = constTy(k);
        const std::int64_t payload =
            (t == Ty::Int) ? k.asInt() : (t == Ty::Bool ? (k.asBool() ? 1 : 0) : 0);
        emitConst(in.a, t, payload);
        break;
      }
      case OpCode::MOVE:
        copySlotValueAt(i, in.a, tyOf(in.b), in.b);
        break;
      case OpCode::ADD:
      case OpCode::SUB:
      case OpCode::MUL:
      case OpCode::BAND:
      case OpCode::BOR:
      case OpCode::BXOR: {
        // A definition the propagation pinned to a constant skips its opcode:
        // `2 * 3` is just `6`, and the emitter writes the folded value.
        if (analysis.defConst[static_cast<std::size_t>(i)]) {
          emitConst(in.a, analysis.defKind[static_cast<std::size_t>(i)],
                    analysis.defVal[static_cast<std::size_t>(i)]);
          break;
        }
        std::int64_t cv = 0;
        Ty ck = Ty::Undef;
        const bool bConst = constIn(i, in.b, cv, ck);
        std::int64_t dv = 0;
        Ty dk = Ty::Undef;
        const bool cConst = constIn(i, in.c, dv, dk);
        if (bConst && !cConst) {
          if (isCommutative(in.op)) {
            emitBinImm(in.op, in.a, in.c, cv);
          } else {
            // `const - var`: compute `const` in scratch, load `var`, subtract.
            cg.movImm(X64Reg::R11, cv);
            loadScalarAt(i, X64Reg::R10, in.c);
            cg.subReg(X64Reg::R11, X64Reg::R10);
            storeScalarAt(i, in.a, Ty::Int, X64Reg::R11);
          }
        } else if (cConst && !bConst) {
          emitBinImm(in.op, in.a, in.b, dv);
        } else {
          emitBinReg(in.op, in.a, in.b, in.c);
        }
        break;
      }
      case OpCode::UNOP: {
        if (analysis.defConst[static_cast<std::size_t>(i)]) {
          emitConst(in.a, analysis.defKind[static_cast<std::size_t>(i)],
                    analysis.defVal[static_cast<std::size_t>(i)]);
          break;
        }
        const UnaryOp op = static_cast<UnaryOp>(in.c);
        if (op == UnaryOp::Not) {
          // Oskel truthiness: only `nil` and `false` are falsy, so `!int` is
          // always false and `!nil` always true; only a bool operand needs a
          // real test.
          const Ty operand = tyOf(in.b);
          if (operand == Ty::Nil) {
            cg.movImm(X64Reg::R10, 1);
          } else if (operand == Ty::Int) {
            cg.movImm(X64Reg::R10, 0);
          } else {
            loadScalarAt(i, X64Reg::R10, in.b);
            cg.testReg(X64Reg::R10);
            cg.setcc(Jcc::Eq, X64Reg::R10);
            cg.movzx(X64Reg::R10, X64Reg::R10);
          }
          storeScalarAt(i, in.a, Ty::Bool, X64Reg::R10);
          break;
        }
        loadScalarAt(i, X64Reg::R10, in.b);
        if (op == UnaryOp::Negate) cg.negReg(X64Reg::R10);
        else cg.notReg(X64Reg::R10);
        storeScalarAt(i, in.a, Ty::Int, X64Reg::R10);
        break;
      }
      case OpCode::EQ:
      case OpCode::NEQ:
      case OpCode::LT:
      case OpCode::LTE:
      case OpCode::GT:
      case OpCode::GTE: {
        if (analysis.defConst[static_cast<std::size_t>(i)]) {
          emitConst(in.a, Ty::Bool, analysis.defVal[static_cast<std::size_t>(i)]);
          break;
        }
        // A fused comparison leaves its condition in the flags for the branch
        // that follows (see below), so it emits only the `cmp` and no bool.
        const int fused = analysis.fusedBranch[static_cast<std::size_t>(i)];
        const Jcc cc = emitCompareFlags(in.op, in.b, in.c, i);
        if (fused >= 0) {
          const Instruction& br = proto.code[static_cast<std::size_t>(fused)];
          const bool jumpWhenTrue = br.op == OpCode::JMP_IF_TRUE;
          cg.jcc(jumpWhenTrue ? cc : negateCc(cc),
                 labels[static_cast<std::size_t>(br.jump)]);
          break;
        }
        cg.setcc(cc, X64Reg::R10);
        cg.movzx(X64Reg::R10, X64Reg::R10);
        storeScalarAt(i, in.a, Ty::Bool, X64Reg::R10);
        break;
      }
      case OpCode::JMP:
        cg.jmp(labels[static_cast<std::size_t>(in.jump)]);
        break;
      case OpCode::JMP_IF_FALSE:
      case OpCode::JMP_IF_TRUE: {
        // A branch fused into the comparison before it already jumped.
        if (i > 0 && analysis.fusedBranch[static_cast<std::size_t>(i - 1)] == i) {
          break;
        }
        const bool jumpWhenTrue = in.op == OpCode::JMP_IF_TRUE;
        std::int64_t cv = 0;
        Ty ck = Ty::Undef;
        if (constIn(i, in.a, cv, ck)) {
          // The condition is a compile-time constant: the branch is decided
          // here, so a taken edge becomes an unconditional jump and a
          // not-taken edge emits nothing.
          const bool taken = truthyConst(ck, cv) == jumpWhenTrue;
          if (taken) cg.jmp(labels[static_cast<std::size_t>(in.jump)]);
          break;
        }
        // `nil` is always falsy, a non-nil int is always truthy, and only a
        // bool needs the payload test.
        if (tyOf(in.a) == Ty::Nil) {
          if (!jumpWhenTrue) cg.jmp(labels[static_cast<std::size_t>(in.jump)]);
        } else if (tyOf(in.a) == Ty::Int) {
          if (jumpWhenTrue) cg.jmp(labels[static_cast<std::size_t>(in.jump)]);
        } else if (tyOf(in.a) == Ty::Bool) {
          const int ar = readRegOf(i, in.a);
          if (ar >= 0) {
            cg.testReg(poolReg(ar));
          } else {
            cg.movFromMem(X64Reg::R10, X64Reg::Rbp, slotDisp(in.a) + 8);
            cg.testReg(X64Reg::R10);
          }
          cg.jcc(jumpWhenTrue ? Jcc::Ne : Jcc::Eq, labels[static_cast<std::size_t>(in.jump)]);
        }
        break;
      }
      case OpCode::DECLLOCAL:
      case OpCode::SETLOCAL: {
        const int dst = numRegs + in.b;
        std::int64_t cv = 0;
        Ty ck = Ty::Undef;
        if (constIn(i, in.a, cv, ck)) emitConst(dst, ck, cv);
        else copySlotValueAt(i, dst, tyOf(in.a), in.a);
        break;
      }
      case OpCode::GETLOCAL: {
        std::int64_t cv = 0;
        Ty ck = Ty::Undef;
        if (constIn(i, numRegs + in.b, cv, ck)) emitConst(in.a, ck, cv);
        else copySlotValueAt(i, in.a, ltyOf(in.b), numRegs + in.b);
        break;
      }
      case OpCode::RETURN:
        returnValue(i, in.a, tyOf(in.a));
        break;
      case OpCode::HALT:
        returnNil();
        break;
      default:
        break;  // unreachable: the analysis rejected every other opcode
    }
  }

  // Falling off the end is an implicit `return nil`, exactly like the VM.
  cg.bind(endLabel);
  returnNil();
  cg.finalize();
  return cg.code();
}

}  // namespace

// --------------------------------------------------------------- executable memory

JitCode::~JitCode() { release(); }

JitCode::JitCode(JitCode&& other) noexcept
    : mapping_(other.mapping_), mappingSize_(other.mappingSize_), entry_(other.entry_) {
  other.mapping_ = nullptr;
  other.mappingSize_ = 0;
  other.entry_ = nullptr;
}

JitCode& JitCode::operator=(JitCode&& other) noexcept {
  if (this != &other) {
    release();
    mapping_ = other.mapping_;
    mappingSize_ = other.mappingSize_;
    entry_ = other.entry_;
    other.mapping_ = nullptr;
    other.mappingSize_ = 0;
    other.entry_ = nullptr;
  }
  return *this;
}

void JitCode::release() {
#if !defined(_WIN32)
  if (mapping_ != nullptr) {
    munmap(mapping_, mappingSize_);
  }
#endif
  mapping_ = nullptr;
  mappingSize_ = 0;
  entry_ = nullptr;
}

bool JitCode::assign(const std::vector<std::uint8_t>& bytes) {
  release();
  if (!jitHostSupported() || bytes.empty()) return false;
#if defined(_WIN32)
  return false;
#else
  const long pageSize = sysconf(_SC_PAGESIZE);
  if (pageSize <= 0) return false;
  std::size_t span = static_cast<std::size_t>(pageSize);
  while (span < bytes.size()) span += static_cast<std::size_t>(pageSize);
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return false;
  std::memcpy(mem, bytes.data(), bytes.size());
  // The mapping is never writable and executable at the same time.
  if (mprotect(mem, span, PROT_READ | PROT_EXEC) != 0) {
    munmap(mem, span);
    return false;
  }
  mapping_ = mem;
  mappingSize_ = span;
  entry_ = mem;
  return true;
#endif
}

// --------------------------------------------------------------- public API

Value JitFunction::call(const std::vector<Value>& args) const {
  // The native body reads `args[0 .. numParams_ - 1]` with no bounds check, so
  // a shorter vector would be an out-of-bounds read. `VM::jitFor` already
  // guarantees the count; this keeps a direct caller from breaking the contract.
  if (args.size() != static_cast<std::size_t>(numParams_)) {
    throw std::logic_error("jit: JitFunction::call expects " + std::to_string(numParams_) +
                           " argument(s), got " + std::to_string(args.size()));
  }
  Value out;
  auto entry = reinterpret_cast<Entry>(code_.entry());
  entry(args.data(), static_cast<std::int64_t>(args.size()), &out);
  return out;
}

std::unique_ptr<JitFunction> jitCompile(const Bytecode& proto) {
  if (!jitHostSupported()) return nullptr;
  const Analysis analysis = analyze(proto);
  if (!analysis.supported) return nullptr;
  std::vector<std::uint8_t> bytes = emitProto(proto, analysis);
  if (bytes.empty()) return nullptr;
  JitCode code;
  if (!code.assign(bytes)) return nullptr;
  return std::unique_ptr<JitFunction>(new JitFunction(std::move(code), proto.numParams));
}

JitFunction* Jit::get(const std::shared_ptr<const Bytecode>& proto) {
  if (!proto) return nullptr;
  const Bytecode* key = proto.get();
  auto it = functions_.find(key);
  if (it != functions_.end()) return it->second.function.get();

  std::unique_ptr<JitFunction> compiled = jitCompile(*proto);
  Entry entry;
  entry.proto = proto;
  entry.function = std::move(compiled);
  JitFunction* result = entry.function.get();
  // Cache even a failed compilation (nullptr function) so a proto outside the
  // subset is analyzed once, not on every call.
  functions_.emplace(key, std::move(entry));
  return result;
}
