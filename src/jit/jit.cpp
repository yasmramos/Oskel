#include "jit/jit.h"

#include <algorithm>
#include <cstring>

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

  // Per slot, true when some reachable instruction reads the slot's *value* at
  // run time (the emitter materializes it in a register or stack slot). A slot
  // that is only ever read through a folded constant -- an operand the emitter
  // replaces by an immediate, or a `defConst` result -- never needs its value,
  // so a `LOADK` that defines such a slot emits nothing. This removes the
  // constants a folded expression leaves behind.
  std::vector<bool> materialized;

  // Compare-branch fusion. For a comparison whose boolean result is dead right
  // after a conditional branch on it, the emitter skips materializing the bool
  // and turns the pair into `cmp; jcc`. `fusedBranch[i]` is the branch index a
  // comparison at `i` jumps for, or -1 when the comparison emits normally; a
  // branch listed here emits nothing (the comparison already jumped).
  std::vector<int> fusedBranch;
};

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

  // Dead-constant analysis (see `Analysis::materialized`). Mirror the emitter's
  // fold decisions: an operand the emitter replaces by an immediate, or a whole
  // instruction `defConst` folds, never needs the operand's value. Every other
  // read materializes it. A `LOADK` whose slot is never materialized then emits
  // nothing -- the constants a folded expression leaves behind disappear.
  result.materialized.assign(total, false);
  {
    auto constAt = [&](int i, int slot, std::int64_t& v) {
      if (slot < 0 || slot >= total) return false;
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
      if (ii >= result.stateIn.size() || slot < 0 ||
          static_cast<std::size_t>(slot) >= result.stateIn[ii].size()) {
        return Ty::Top;
      }
      return result.stateIn[ii][static_cast<std::size_t>(slot)];
    };
    for (int i = 0; i < n; ++i) {
      if (!result.reachable[static_cast<std::size_t>(i)]) continue;
      const Instruction& in = proto.code[static_cast<std::size_t>(i)];
      const std::size_t ii = static_cast<std::size_t>(i);
      std::int64_t bv = 0, dv = 0;
      switch (in.op) {
        case OpCode::MOVE:
          if (indexOk(in.b)) result.materialized[static_cast<std::size_t>(in.b)] = true;
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
        case OpCode::GTE: {
          if (result.defConst[ii]) break;  // fully folded, no operand read
          const bool bc = constAt(i, in.b, bv);
          const bool cc = constAt(i, in.c, dv);
          const bool bUsable = bc && fitsImm32(bv);
          const bool cUsable = cc && fitsImm32(dv);
          if (bUsable && !cc) {
            if (indexOk(in.c)) result.materialized[static_cast<std::size_t>(in.c)] = true;
          } else if (cUsable && !bc) {
            if (indexOk(in.b)) result.materialized[static_cast<std::size_t>(in.b)] = true;
          } else {
            if (indexOk(in.b)) result.materialized[static_cast<std::size_t>(in.b)] = true;
            if (indexOk(in.c)) result.materialized[static_cast<std::size_t>(in.c)] = true;
          }
          break;
        }
        case OpCode::UNOP:
          if (result.defConst[ii]) break;
          if (indexOk(in.b)) result.materialized[static_cast<std::size_t>(in.b)] = true;
          break;
        case OpCode::JMP_IF_FALSE:
        case OpCode::JMP_IF_TRUE: {
          if (constAt(i, in.a, bv)) break;
          const Ty t = tyAt(i, in.a);
          if (t == Ty::Nil || t == Ty::Int) break;  // branch decided by the type
          if (indexOk(in.a)) result.materialized[static_cast<std::size_t>(in.a)] = true;
          break;
        }
        case OpCode::DECLLOCAL:
        case OpCode::SETLOCAL:
          if (constAt(i, in.a, bv)) break;
          if (indexOk(in.a)) result.materialized[static_cast<std::size_t>(in.a)] = true;
          break;
        case OpCode::GETLOCAL:
          if (constAt(i, result.numRegs + in.b, bv)) break;
          if (indexOk(result.numRegs + in.b)) {
            result.materialized[static_cast<std::size_t>(result.numRegs + in.b)] = true;
          }
          break;
        case OpCode::RETURN:
          if (indexOk(in.a)) result.materialized[static_cast<std::size_t>(in.a)] = true;
          break;
        default:
          break;
      }
    }
  }


  // Compare-branch fusion (see `Analysis::fusedBranch`). Backward liveness over
  // the reachable instructions; a comparison's boolean result may be dropped
  // only when it is dead once the branch that consumes it has run, and only when
  // that branch is reached by fall-through alone (never a jump target), so
  // skipping it cannot lose a re-test of the condition.
  result.fusedBranch.assign(n, -1);
  {
    auto reads = [&](const Instruction& in, std::vector<int>& out) {
      switch (in.op) {
        case OpCode::MOVE:
          if (indexOk(in.b)) out.push_back(in.b);
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
          if (indexOk(in.b)) out.push_back(in.b);
          if (indexOk(in.c)) out.push_back(in.c);
          break;
        case OpCode::UNOP:
          if (indexOk(in.b)) out.push_back(in.b);
          break;
        case OpCode::JMP_IF_FALSE:
        case OpCode::JMP_IF_TRUE:
          if (indexOk(in.a)) out.push_back(in.a);
          break;
        case OpCode::DECLLOCAL:
        case OpCode::SETLOCAL:
          if (indexOk(in.a)) out.push_back(in.a);
          break;
        case OpCode::GETLOCAL:
          if (indexOk(result.numRegs + in.b)) out.push_back(result.numRegs + in.b);
          break;
        case OpCode::RETURN:
          if (indexOk(in.a)) out.push_back(in.a);
          break;
        default:
          break;
      }
    };
    auto writes = [&](const Instruction& in, std::vector<int>& out) {
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
          if (indexOk(in.a)) out.push_back(in.a);
          break;
        case OpCode::DECLLOCAL:
        case OpCode::SETLOCAL:
          if (indexOk(result.numRegs + in.b)) out.push_back(result.numRegs + in.b);
          break;
        default:
          break;
      }
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

  // Physical registers a register-resident slot can live in. They are all
  // caller-saved, and a supported proto is a leaf (no `CALL`), so the body
  // makes no call that could clobber them; r10/r11 stay scratch and rbp is the
  // frame pointer. A slot the pool cannot cover (too many slots) keeps its
  // 16-byte stack slot. Keeping the common scalar body entirely in registers is
  // what removes the load/store traffic the stack model paid on every
  // operation.
  const X64Reg kRegPool[] = {X64Reg::Rax, X64Reg::Rcx, X64Reg::Rdx,
                             X64Reg::Rsi, X64Reg::Rdi, X64Reg::R8, X64Reg::R9};
  const int kRegPoolCount = static_cast<int>(sizeof(kRegPool) / sizeof(kRegPool[0]));
  std::vector<int> regOf(static_cast<std::size_t>(total), -1);
  for (int s = 0; s < total; ++s) {
    if (s < kRegPoolCount && static_cast<std::size_t>(s) < analysis.regResident.size() &&
        analysis.regResident[static_cast<std::size_t>(s)]) {
      regOf[static_cast<std::size_t>(s)] = s;
    }
  }
  auto isReg = [&](int slot) {
    return slot >= 0 && slot < total && regOf[static_cast<std::size_t>(slot)] >= 0;
  };
  auto physOf = [&](int slot) { return kRegPool[regOf[static_cast<std::size_t>(slot)]]; };

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
  // Loads the raw scalar payload of a slot into `dst`: from its register when
  // it has one, else from the stack payload word. The tag is not read; the
  // caller knows the slot's scalar kind from the dataflow state.
  auto loadScalar = [&](X64Reg dst, int slot) {
    if (isReg(slot)) {
      if (physOf(slot) != dst) cg.movReg(dst, physOf(slot));
    } else {
      cg.movFromMem(dst, X64Reg::Rbp, slotDisp(slot) + 8);
    }
  };
  // Writes a canonical scalar payload (`src`: a full int64, or 0/1 for a bool)
  // of kind `kind` to a slot: its register, or the stack slot plus the kind's
  // tag.
  auto storeScalar = [&](int slot, Ty kind, X64Reg src) {
    if (isReg(slot)) {
      if (physOf(slot) != src) cg.movReg(physOf(slot), src);
    } else {
      cg.movToMem(X64Reg::Rbp, slotDisp(slot) + 8, src);
      cg.movImmByteToMem(X64Reg::Rbp, slotDisp(slot), tagOf(kind));
    }
  };
  // Copies a whole slot between the two representations. `kind` is the scalar
  // kind of the source at this point, needed when the destination is a stack
  // slot (to pick its tag).
  auto copySlotValue = [&](int dstSlot, Ty kind, int srcSlot) {
    if (isReg(srcSlot) && isReg(dstSlot)) {
      cg.movReg(physOf(dstSlot), physOf(srcSlot));
    } else if (isReg(srcSlot)) {
      storeScalar(dstSlot, kind, physOf(srcSlot));
    } else if (isReg(dstSlot)) {
      loadScalar(physOf(dstSlot), srcSlot);
    } else {
      copySlot(slotDisp(dstSlot), slotDisp(srcSlot));
    }
  };
  auto epilogue = [&]() {
    cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
    cg.pop(X64Reg::Rbp);
    cg.retInsn();
  };
  auto returnValue = [&](int slot, Ty kind) {
    cg.movFromMem(X64Reg::R11, X64Reg::Rbp, outDisp());  // out pointer
    if (isReg(slot)) {
      cg.movToMem(X64Reg::R11, 8, physOf(slot));
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
  // stack slots, then lift the resident ones into their registers. The lift
  // runs after the whole copy loop, so it may freely use rdi/rsi (the argument
  // pointer and count) as destinations.
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
    if (isReg(i)) cg.movFromMem(physOf(i), X64Reg::Rbp, slotDisp(i) + 8);
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
  // Writes a scalar constant to a slot: its register, or its stack slot with
  // the kind's tag.
  auto emitConst = [&](int slot, Ty kind, std::int64_t value) {
    if (isReg(slot)) {
      cg.movImm(physOf(slot), value);
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
  // `dst = lhs OP rhs` with both operands in slots. The destination's register
  // is spent in place when it already holds an operand; otherwise the result is
  // computed in scratch and stored. A register operand is used directly, so no
  // copy stands between the operands and the ALU.
  auto emitBinReg = [&](OpCode op, int dst, int lhs, int rhs) {
    if (isReg(dst)) {
      const X64Reg d = physOf(dst);
      if (dst == lhs) {
        if (isReg(rhs)) applyReg(op, d, physOf(rhs));
        else { loadScalar(X64Reg::R10, rhs); applyReg(op, d, X64Reg::R10); }
        return;
      }
      if (dst == rhs && isCommutative(op)) {
        if (isReg(lhs)) applyReg(op, d, physOf(lhs));
        else { loadScalar(X64Reg::R10, lhs); applyReg(op, d, X64Reg::R10); }
        return;
      }
      if (dst != rhs) {
        // `dst` is a distinct register, free until here: compute into it
        // directly, without a scratch copy.
        loadScalar(d, lhs);
        if (isReg(rhs)) applyReg(op, d, physOf(rhs));
        else { loadScalar(X64Reg::R10, rhs); applyReg(op, d, X64Reg::R10); }
        return;
      }
      // `dst == rhs` with a non-commutative op (only `SUB`): the old right
      // operand lives in `dst`, so the scratch path below reads it before the
      // destination is overwritten.
    }
    loadScalar(X64Reg::R10, lhs);
    loadScalar(X64Reg::R11, rhs);
    applyReg(op, X64Reg::R10, X64Reg::R11);
    storeScalar(dst, Ty::Int, X64Reg::R10);
  };
  // `dst = var OP imm` for an integer immediate: identity and absorbing cases
  // first (no instruction), then the immediate form, a shift for a power of two,
  // or a materialized register when the constant is out of imm32 range. The
  // arithmetic runs in the destination's register when it has one, so `i * 2`
  // becomes a single `shl`.
  auto emitBinImm = [&](OpCode op, int dst, int varSlot, std::int64_t imm) {
    if ((op == OpCode::ADD || op == OpCode::SUB || op == OpCode::BOR ||
         op == OpCode::BXOR) && imm == 0) {
      copySlotValue(dst, Ty::Int, varSlot);
      return;
    }
    if (op == OpCode::MUL && imm == 1) {
      copySlotValue(dst, Ty::Int, varSlot);
      return;
    }
    if ((op == OpCode::MUL || op == OpCode::BAND) && imm == 0) {
      emitConst(dst, Ty::Int, 0);
      return;
    }
    if (op == OpCode::BAND && imm == -1) {
      copySlotValue(dst, Ty::Int, varSlot);
      return;
    }
    // Compute in the destination register when it has one: `dst` is dead until
    // this instruction, so it is free scratch, and the final store becomes a
    // no-op. Otherwise fall back to r10.
    const X64Reg work = isReg(dst) ? physOf(dst) : X64Reg::R10;
    loadScalar(work, varSlot);
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
    storeScalar(dst, Ty::Int, work);
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
      loadScalar(X64Reg::R10, c);
      cg.cmpImm32(X64Reg::R10, static_cast<std::int32_t>(cv));
      cc = mirrorCc(cc);
    } else if (cConst && !bConst && fitsImm32(dv)) {
      loadScalar(X64Reg::R10, b);
      cg.cmpImm32(X64Reg::R10, static_cast<std::int32_t>(dv));
    } else if (isReg(b) && isReg(c)) {
      // Both operands live in registers: compare them directly, so no scratch
      // copy stands before the compare.
      cg.cmpReg(physOf(b), physOf(c));
    } else {
      loadScalar(X64Reg::R10, b);
      loadScalar(X64Reg::R11, c);
      cg.cmpReg(X64Reg::R10, X64Reg::R11);
    }
    return cc;
  };

  for (int i = 0; i < n; ++i) {
    if (!analysis.reachable[static_cast<std::size_t>(i)]) continue;
    cg.bind(labels[static_cast<std::size_t>(i)]);
    const Instruction& in = proto.code[static_cast<std::size_t>(i)];
    const std::vector<Ty>& st = analysis.stateIn[static_cast<std::size_t>(i)];
    auto tyOf = [&](int r) { return st[static_cast<std::size_t>(r)]; };
    auto ltyOf = [&](int l) { return st[static_cast<std::size_t>(numRegs + l)]; };

    switch (in.op) {
      case OpCode::LOADK: {
        // A constant no reachable read ever materializes is dead: a folded
        // expression consumed it as an immediate, so the slot is never loaded.
        if (in.a >= 0 && in.a < total && !analysis.materialized[static_cast<std::size_t>(in.a)]) {
          break;
        }
        const Value& k = proto.constants[static_cast<std::size_t>(in.b)];
        const Ty t = constTy(k);
        const std::int64_t payload =
            (t == Ty::Int) ? k.asInt() : (t == Ty::Bool ? (k.asBool() ? 1 : 0) : 0);
        emitConst(in.a, t, payload);
        break;
      }
      case OpCode::MOVE:
        copySlotValue(in.a, tyOf(in.b), in.b);
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
            loadScalar(X64Reg::R10, in.c);
            cg.subReg(X64Reg::R11, X64Reg::R10);
            storeScalar(in.a, Ty::Int, X64Reg::R11);
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
            loadScalar(X64Reg::R10, in.b);
            cg.testReg(X64Reg::R10);
            cg.setcc(Jcc::Eq, X64Reg::R10);
            cg.movzx(X64Reg::R10, X64Reg::R10);
          }
          storeScalar(in.a, Ty::Bool, X64Reg::R10);
          break;
        }
        loadScalar(X64Reg::R10, in.b);
        if (op == UnaryOp::Negate) cg.negReg(X64Reg::R10);
        else cg.notReg(X64Reg::R10);
        storeScalar(in.a, Ty::Int, X64Reg::R10);
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
        storeScalar(in.a, Ty::Bool, X64Reg::R10);
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
          if (isReg(in.a)) {
            cg.testReg(physOf(in.a));
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
        else copySlotValue(dst, tyOf(in.a), in.a);
        break;
      }
      case OpCode::GETLOCAL: {
        std::int64_t cv = 0;
        Ty ck = Ty::Undef;
        if (constIn(i, numRegs + in.b, cv, ck)) emitConst(in.a, ck, cv);
        else copySlotValue(in.a, ltyOf(in.b), numRegs + in.b);
        break;
      }
      case OpCode::RETURN:
        returnValue(in.a, tyOf(in.a));
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
