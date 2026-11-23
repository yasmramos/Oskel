#include "native/lower.h"

#include "native/bare_metal.h"
#include "native/codegen.h"
#include "native/entry.h"
#include "native/runtime.h"
#include "native/value.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// PHASE 7c.1 lowers the runtime-free opcodes over the boxed `AotValue`. A
// bytecode register is now a 16-byte slot in a register file on the stack, not
// a physical CPU register: a boxed value does not fit in one, and the file is
// what makes `print`, collections and calls (7c.2/7c.3) possible without a
// second value representation. The file grows down from rbp:
//
//   [rbp - 16 * (i + 1)] = tag(Ri), [rbp - 16 * (i + 1) + 8] = payload(Ri)
//
// so R0 sits highest, right below rbp. rbp is both the frame pointer and the
// file base, so the epilogue (`mov rsp, rbp; pop rbp; ret`) needs no extra
// bookkeeping.

// The physical registers the lowering uses for scratch. The register file
// lives on the stack, so the CPU registers are free to be temporaries.
constexpr X64Reg kFileBase = X64Reg::Rbp;  // &R0 is [rbp - 16]
constexpr X64Reg kTmpTag = X64Reg::Rax;
constexpr X64Reg kTmpPayload = X64Reg::Rdx;

// Byte offset of a register slot from the file base (`rbp`). Slot i sits
// *below* R0, hence the negative offset.
constexpr std::int64_t slotOffset(int index) { return -16LL * (index + 1); }

// Local slots (PHASE 7c.2b) live in a second region of the same frame, right
// below the register file. Slot `b` sits at `[rbp - 16 * (numRegisters + b + 1)]`.
// The `numRegisters` argument is the *allocated* file size (at least 1), which
// is what the frame actually reserved, not the proto's declared count.
//
// From 7c.2c.2 a slot the proto captures (some child's upvalue points at it)
// holds a *cell*: the first 8 bytes are a pointer to a 16-byte `AotValue` in
// the arena, shared with every closure that captured the slot. A slot no child
// captures stays a plain inline `AotValue` (16 bytes), so the common local
// costs no allocation.
constexpr std::int64_t localSlotOffset(int numRegisters, int index) {
  return -16LL * (numRegisters + index + 1);
}

// The `AotClosure*` the frame was entered with, saved just below the locals so
// `GETUPVAL`/`SETUPVAL` and a nested `CLOSURE` can reach the captured cells.
// Only a bytecode function has one; the root chunk reserves the slot too (the
// frame is a fixed shape) but never reads it.
constexpr std::int64_t selfSlotOffset(int numRegisters, int numLocals) {
  return -16LL * (numRegisters + numLocals + 1);
}

// The frame of the root chunk: six callee-saved registers are pushed (rbp is
// the file base; rbx and r12-r15 hold the arena/allocator state and the global
// table, and the System V ABI requires them preserved at the process
// boundary), then a block of `16 * (fileRegisters + fileLocals)` bytes below
// rbp holds the register file and the local slots.
//
// The chunk is entered through a `call` (from `main`), so rsp is 8 mod 16; the
// six pushes leave it 8 mod 16 and the multiple-of-16 frame keeps it there, so
// one extra 8-byte slot below the file brings it back to 0 mod 16, which every
// `call` the body makes requires. The epilogue drops it with `mov rsp, rbp`.
constexpr std::int64_t frameBytes(int fileRegisters, int fileLocals) {
  return 16LL * (fileRegisters + fileLocals);
}

void emitRootPrologue(CodeGen& cg, int fileRegisters, int fileLocals) {
  cg.push(X64Reg::Rbp);
  cg.push(X64Reg::Rbx);
  cg.push(X64Reg::R12);
  cg.push(X64Reg::R13);
  cg.push(X64Reg::R14);
  cg.push(X64Reg::R15);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.subRegImm32(X64Reg::Rsp, static_cast<std::int32_t>(frameBytes(fileRegisters, fileLocals) + 8));
}

void emitRootEpilogue(CodeGen& cg, int callbackStateSlot = -1) {
  // A stored callback may run after the chunk returns (`atexit`), so publish
  // the final arena cursor/end before the frame restores the process-boundary
  // registers. A program that never registered a callback leaves the slot nil.
  if (callbackStateSlot >= 0) {
    CodeGen::Label noState = cg.newLabel();
    cg.movFromMem(X64Reg::R10, X64Reg::R15, 16 * callbackStateSlot + 8);
    cg.testReg(X64Reg::R10);
    cg.jcc(Jcc::Eq, noState);
    cg.movToMem(X64Reg::R10, 0, X64Reg::R13);
    cg.movToMem(X64Reg::R10, 8, X64Reg::R14);
    cg.bind(noState);
  }
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::R15);
  cg.pop(X64Reg::R14);
  cg.pop(X64Reg::R13);
  cg.pop(X64Reg::R12);
  cg.pop(X64Reg::Rbx);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();
}

// The frame of a bytecode function (a child proto). It pushes only rbp: the
// arena cursor (r13), the arena end (r14) and the global table base (r15) are
// the *program's* state, not this call's, so a function must NOT save and
// restore them -- an allocation inside a function has to advance the shared
// cursor, and restoring it on return would lose the allocation. rbp is the
// caller's register-file base, so it must be preserved. One push keeps rsp
// 16-byte aligned at the body's first `call` (entry rsp % 16 == 8). The frame
// reserves one extra slot past the locals for the saved `AotClosure*` (the
// `self` the call passed in rdx), which a captured local is reached through.
void emitFunctionPrologue(CodeGen& cg, int fileRegisters, int fileLocals) {
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.subRegImm32(X64Reg::Rsp,
                 static_cast<std::int32_t>(frameBytes(fileRegisters, fileLocals + 1)));
}

void emitFunctionEpilogue(CodeGen& cg) {
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();
}

// Loads register `index` into `tag`:`payload`. Two moves, the tagged load the
// lowering table budgets.
void loadValue(CodeGen& cg, int index, X64Reg tag, X64Reg payload) {
  cg.movFromMem(tag, kFileBase, slotOffset(index));
  cg.movFromMem(payload, kFileBase, slotOffset(index) + 8);
}

// Stores `tag`:`payload` into register `index`.
void storeValue(CodeGen& cg, int index, X64Reg tag, X64Reg payload) {
  cg.movToMem(kFileBase, slotOffset(index), tag);
  cg.movToMem(kFileBase, slotOffset(index) + 8, payload);
}

// Loads a 64-bit immediate into a register slot as an integer value.
void loadIntImmediate(CodeGen& cg, int index, std::int64_t value) {
  cg.movImmToMem(kFileBase, slotOffset(index),
                 static_cast<std::int64_t>(AotTag::Int));
  cg.movImm(kTmpTag, value);
  cg.movToMem(kFileBase, slotOffset(index) + 8, kTmpTag);
}

// Loads a boolean immediate into a register slot.
void loadBoolImmediate(CodeGen& cg, int index, bool value) {
  cg.movImmToMem(kFileBase, slotOffset(index),
                 static_cast<std::int64_t>(AotTag::Bool));
  cg.movImmToMem(kFileBase, slotOffset(index) + 8, value ? 1 : 0);
}

// Loads nil into a register slot.
void loadNilImmediate(CodeGen& cg, int index) {
  cg.movImmToMem(kFileBase, slotOffset(index),
                 static_cast<std::int64_t>(AotTag::Nil));
  cg.movImmToMem(kFileBase, slotOffset(index) + 8, 0);
}

// Loads a float/double immediate into a register slot. The payload is the
// IEEE-754 bit pattern of the (already widened) binary64 value, exactly the
// representation the runtime helpers expect.
void loadFloatingImmediate(CodeGen& cg, int index, double value, AotTag tag) {
  cg.movImmToMem(kFileBase, slotOffset(index), static_cast<std::int64_t>(tag));
  cg.movImm(kTmpTag, static_cast<std::int64_t>(doubleBits(value)));
  cg.movToMem(kFileBase, slotOffset(index) + 8, kTmpTag);
}

// Loads a string constant into a register slot. The payload is the address of
// an `AotString` laid out in the image (see `StringConstant`): a `lea` of the
// header, since the bytes live in the code buffer, not in the register file.
void loadStringImmediate(CodeGen& cg, int index, CodeGen::Label dataLabel) {
  cg.movImmToMem(kFileBase, slotOffset(index), static_cast<std::int64_t>(AotTag::Str));
  cg.leaRipLabel(kTmpTag, dataLabel);
  cg.movToMem(kFileBase, slotOffset(index) + 8, kTmpTag);
}

// Emits a call to a runtime helper that takes two `AotValue`s (rdi:rsi,
// rdx:rcx) plus a 64-bit selector in r8, and stores the returned `AotValue`
// (rax:rdx) in register `dst`.
void callBinaryWithSelector(CodeGen& cg, CodeGen::Label helper, int lhs, int rhs,
                            int dst, std::int64_t selector) {
  loadValue(cg, lhs, X64Reg::Rdi, X64Reg::Rsi);
  loadValue(cg, rhs, X64Reg::Rdx, X64Reg::Rcx);
  cg.movImm(X64Reg::R8, selector);
  cg.callLabel(helper);
  storeValue(cg, dst, X64Reg::Rax, X64Reg::Rdx);
}

// PHASE 7c.1 still lowers a subset. Floats and strings need the arena and
// their own tags (7c.1b); calls and collections need frames and the allocator
// (7c.2c/7c.3). Every other opcode raises a `LoweringError`
// instead of emitting wrong code.
[[noreturn]] void unsupported(const char* opName, const char* phase) {
  throw LoweringError(std::string("lowering: opcode '") + opName +
                      "' needs runtime support (" + phase + ")");
}

// Emits a call to a runtime helper that takes one `AotValue` (in rdi:rsi) and
// returns one (in rax:rdx), then stores the result in register `dst`.
void callUnary(CodeGen& cg, CodeGen::Label helper, int src, int dst) {
  loadValue(cg, src, X64Reg::Rdi, X64Reg::Rsi);
  cg.callLabel(helper);
  storeValue(cg, dst, X64Reg::Rax, X64Reg::Rdx);
}

// Same, for a helper taking two `AotValue`s (rdi:rsi, rdx:rcx).
void callBinary(CodeGen& cg, CodeGen::Label helper, int lhs, int rhs, int dst) {
  loadValue(cg, lhs, X64Reg::Rdi, X64Reg::Rsi);
  loadValue(cg, rhs, X64Reg::Rdx, X64Reg::Rcx);
  cg.callLabel(helper);
  storeValue(cg, dst, X64Reg::Rax, X64Reg::Rdx);
}

CodeGen::Label jumpTarget(const Instruction& instr, const Bytecode& proto,
                          const std::vector<CodeGen::Label>& labels) {
  if (instr.jump < 0 || static_cast<std::size_t>(instr.jump) >= proto.code.size()) {
    throw LoweringError("lowering: jump target " + std::to_string(instr.jump) +
                        " is outside the code (size " +
                        std::to_string(proto.code.size()) + ")");
  }
  return labels[static_cast<std::size_t>(instr.jump)];
}

// A string constant laid out in the image: an 8-byte length, then the bytes.
// `label` is bound at the header once the code is complete.
struct StringConstant {
  CodeGen::Label label;
  std::string bytes;
};

void loadMemberName(CodeGen& cg, const Bytecode& proto, int c, X64Reg tag, X64Reg payload,
                    std::vector<StringConstant>& stringConstants) {
  const Value& value = proto.constants.at(static_cast<std::size_t>(-c - 1));
  if (!value.isStr()) {
    throw LoweringError("lowering: member name is not a string constant");
  }
  StringConstant constant{cg.newLabel(), value.asStr()};
  cg.movImm(tag, static_cast<std::int64_t>(AotTag::Str));
  cg.leaRipLabel(payload, constant.label);
  stringConstants.push_back(std::move(constant));
}

// Global variables (PHASE 7c.2a). The compiler names a global by a string
// constant index (`instruction.b`), but the binary has no strings: each
// distinct name gets an index into a flat `AotValue` array. The lowerer
// derives the map from the code itself, in first-assignment order, so the
// `Bytecode` structure and the `.oskc` format stay unchanged.
//
// A `GETGLOBAL` of a name the program never assigns is a lowering error naming
// the variable, the compile-time counterpart of the hosted engine's run-time
// `undefined variable`. Builtins (`print`, ...) are globals the hosted engine
// installs up front; the AOT backend has no builtins until calls land
// (7c.2c), at which point they will be seeded as pre-assigned here.
//
// The key is the *name*, not the constant index: each proto has its own
// constant pool, so index 3 in the root and index 3 in a function are usually
// different names, and a recursion (`fact` reading `fact`) must resolve to one
// slot across protos.
struct GlobalSlots {
  std::unordered_map<std::string, int> slotOfName;
  int count = 0;
  // Names whose slot the chunk fills before the program runs: a builtin with a
  // stub, a resolved `extern "C"` thunk, and the hidden runtime slots. A read
  // of one of these can never observe `Unset`, so it needs no check.
  std::unordered_set<std::string> seeded;
  // The names the chunk declares as host bindings (`native fn`, `extern "C"`).
  // A declaration compiles to `GETGLOBAL N; SETGLOBAL N`; the read is the
  // compiler's shape for "resolve the host binding", not a use of the value, so
  // it must not trip the unset check when the binding has no stub (PHASE 7f.1).
  std::unordered_set<std::string> hostBindings;
};

// The name of the hidden global that holds the `mt19937_64` state for
// `random()`/`seed()` (PHASE 7c.5b.6). The state is 312 words plus a cursor, so
// it lives in the global table (which the arena backs) rather than in a
// register: the image has no writable data segment, and a callee-saved register
// would have to be reserved across every helper. The name cannot collide with a
// program's, because the lexer rejects `$` in an identifier.
constexpr const char* kAotRandomStateGlobal = "$randomState";
// The hidden global that points at the shared block holding the arena cursor and
// end across a C callback. A C caller expects r13/r14/r15 preserved, but those
// are the Oskel arena/global state, so the callback body parks them here while
// C runs and reloads them for the next callback.
constexpr const char* kAotCallbackStateGlobal = "$callbackState";

// The name of the hidden global that holds the address of the innermost active
// `pcall` handler record (BACKLOG §6 item 4). The error sink is reached by an
// ordinary `call`, so it cannot see the `pcall` frame; the record's address
// therefore travels through the global table, which the arena backs. Reserved
// only when the program calls `pcall`, so a program that does not keeps its
// image byte for byte.
constexpr const char* kAotPcallHandlerGlobal = "$pcallHandler";

// The names of the two hidden globals the `clock()` and `args()` builtins read
// (BACKLOG §6 item 25). Both hold a value the chunk can only obtain at entry and
// both are needed from inside an ordinary builtin call, so they travel through
// the global table like the `random` state and the `pcall` handler. Each slot is
// reserved only when the program uses its builtin, so a program that does not
// keeps its image byte for byte.
constexpr const char* kAotClockOriginGlobal = "$clockOrigin";
constexpr const char* kAotArgsGlobal = "$args";

// Narrows the compiler's `extern "C"` declarations to the thunks the hosted
// backend resolves through the system linker (PHASE 7f.5, structs in 7f.7).
//
// The compiler records the full ABI signature on each `ExternSymbol`; a thunk
// needs the flattened register classes, so this walks every proto's externs and
// produces one `AotExternThunk` per symbol whose every parameter and return is
// expressible: a scalar, or a struct that fits in two eightbytes (a class-MEMORY
// struct is stack-passed, and supported too). The first six integer/pointer
// eightbytes use the System V GPRs and any further one spills to the outgoing
// stack area; a register-class struct that does not fit the remaining registers
// is passed wholly on the stack. A callback is left out:
// `collectGlobals` then rejects it exactly as before, so the unsupported shapes
// keep their clean build error instead of compiling to a thunk that would
// marshal the wrong registers. A C variadic is expressible (PHASE 7f.5): its
// declared parameters are the fixed prefix and the thunk walks the tail at run
// time, so the signature is validated as scalars-only by `validateSignature`.
//
// Deduplicated by symbol name: several protos may declare the same C function
// (`std.math` and a user block both naming `abs`), and one thunk serves all of
// them, seeded into the single global slot the name resolves to.
std::vector<AotExternThunk> collectExternThunks(CodeGen& cg,
                                                const std::vector<const Bytecode*>& protos,
                                                std::unordered_set<std::string>& resolved,
                                                std::vector<AotCallbackSignature>& callbacks,
                                                Abi abi) {
  std::vector<AotExternThunk> thunks;
  std::unordered_set<std::string> seen;
  // The Microsoft x64 ABI uses four integer and four SSE argument registers,
  // reserves 32 bytes of shadow space and passes a struct by reference; the
  // thunk supports the scalar/pointer surface there and rejects the aggregate
  // and callback shapes with a dedicated message instead of marshalling the
  // wrong registers.
  const bool win64 = abi == Abi::Win64;
  const int gprLimit = win64 ? 4 : 6;
  const int sseLimit = win64 ? 4 : 8;

  // The C side of a callback: its scalar parameters classified by the System V
  // ABI, and its return class. The body is a shared machine-code function, so
  // two declarations of the same shape share one entry. A struct return is
  // supported too: the body rebuilds the map's fields into the C bytes and
  // loads them into the return registers (or the hidden `sret` buffer).
  auto callbackIndex = [&](const ffi::TypeSpec& spec) -> int {
    AotCallbackSignature signature;
    if (spec.callbackReturnsStruct) {
      std::string reason;
      if (!ffi::buildStructLayout(spec.callbackReturnFields,
                                  signature.returnStruct.layout, reason)) {
        throw LoweringError("lowering: " + reason);
      }
      signature.returnsStruct = true;
      signature.returnMemory = signature.returnStruct.layout.isMemory;
      signature.returns = ffi::ReturnClass::Struct;
      for (std::size_t f = 0; f < signature.returnStruct.layout.fields.size(); ++f) {
        signature.returnStruct.fieldNameLabels.push_back(cg.newLabel());
      }
    } else {
      signature.returnType = spec.callbackReturn;
      if (spec.callbackReturn.empty() || spec.callbackReturn == "void") {
        signature.returns = ffi::ReturnClass::Void;
      } else if (!ffi::classifyReturn(spec.callbackReturn, signature.returns) ||
                 signature.returns == ffi::ReturnClass::Struct) {
        throw LoweringError("lowering: callback return type '" + spec.callbackReturn +
                            "' cannot cross the C boundary");
      }
    }
    // A class-MEMORY return takes `rdi` for the hidden `sret` pointer, so the
    // first declared parameter starts in the next integer register.
    int gpr = signature.returnMemory ? 1 : 0;
    int sse = 0;
    std::int64_t stack = 0;
    for (const std::string& declared : spec.callbackParams) {
      const std::string type = declared.empty() ? "int32" : declared;
      ffi::ParamClass cls = ffi::ParamClass::Int;
      if (!ffi::classifyParam(type, cls)) {
        throw LoweringError("lowering: callback parameter type '" + type +
                            "' cannot cross the C boundary");
      }
      AotCallbackParam param;
      param.cls = cls;
      param.type = type;
      if (cls == ffi::ParamClass::Float || cls == ffi::ParamClass::Double) {
        if (sse < 8) {
          param.sse = sse++;
        } else {
          param.stackOffset = stack;
          stack += 8;
        }
      } else if (gpr < 6) {
        param.gpr = gpr++;
      } else {
        param.stackOffset = stack;
        stack += 8;
      }
      signature.params.push_back(std::move(param));
    }
    for (std::size_t i = 0; i < callbacks.size(); ++i) {
      if (callbacks[i].returnsStruct != signature.returnsStruct) continue;
      if (callbacks[i].returns != signature.returns) continue;
      if (callbacks[i].returnType != signature.returnType) continue;
      if (signature.returnsStruct) {
        const ffi::StructLayout& a = callbacks[i].returnStruct.layout;
        const ffi::StructLayout& b = signature.returnStruct.layout;
        if (a.size != b.size || a.fields.size() != b.fields.size()) continue;
        bool sameLayout = true;
        for (std::size_t k = 0; k < a.fields.size(); ++k) {
          if (a.fields[k].name != b.fields[k].name || a.fields[k].type != b.fields[k].type) {
            sameLayout = false;
            break;
          }
        }
        if (!sameLayout) continue;
      }
      if (callbacks[i].params.size() != signature.params.size()) continue;
      bool same = true;
      for (std::size_t k = 0; k < signature.params.size(); ++k) {
        if (callbacks[i].params[k].type != signature.params[k].type) {
          same = false;
          break;
        }
      }
      if (same) return static_cast<int>(i);
    }
    signature.body = cg.newLabel();
    callbacks.push_back(std::move(signature));
    return static_cast<int>(callbacks.size() - 1);
  };
  for (const Bytecode* proto : protos) {
    for (const ExternSymbol& symbol : proto->externs) {
      if (!seen.insert(symbol.name).second) continue;
      AotExternThunk thunk;
      thunk.symbol = symbol.name;
      thunk.arity = symbol.paramTypes.size();
      thunk.variadic = symbol.variadic;
      if (win64 && symbol.returnType.isStruct) {
        throw LoweringError(
            "lowering: a struct return from '" + symbol.name +
            "' is not supported on the Windows x86-64 hosted backend yet");
      }
      // A class-MEMORY return reserves `rdi` for the hidden `sret` pointer, so
      // the integer argument registers start one later. Compute it before the
      // parameters because their register/stack assignment depends on it.
      bool returnMemory = false;
      if (symbol.returnType.isStruct) {
        ffi::StructLayout layout;
        std::string reason;
        if (ffi::buildStructLayout(symbol.returnType.fields, layout, reason)) {
          returnMemory = layout.isMemory;
        }
      }
      int gpr = returnMemory ? 1 : 0;  // the integer argument registers
      int sse = 0;                     // the SSE argument registers
      int nextBufSlot = 0;             // register-class struct staging buffers
      // The Microsoft ABI reserves 32 bytes of shadow space below the outgoing
      // stack arguments, so a Win64 stack argument starts at offset 32. A
      // variadic Win64 thunk reserves the whole outgoing area at run time
      // (shadow space plus the fixed and variable stack arguments), so its
      // fixed frame carries none of it.
      thunk.stackArgBytes = win64 && !symbol.variadic ? 32 : 0;
      auto addStackStruct = [&](int index, int arg, int size) {
        thunk.stackArgBytes = (thunk.stackArgBytes + 7) / 8 * 8;
        AotStackArg stacked;
        stacked.structIndex = index;
        stacked.argIndex = arg;
        stacked.offset = thunk.stackArgBytes;
        thunk.stackArgBytes += static_cast<std::int64_t>(size);
        thunk.stackArgs.push_back(stacked);
      };
      bool supported = true;
      const int argCount = static_cast<int>(symbol.paramTypes.size());
      for (int arg = 0; arg < argCount; ++arg) {
        const ffi::TypeSpec& param = symbol.paramTypes[static_cast<std::size_t>(arg)];
        if (param.isCallback) {
          if (win64) {
            throw LoweringError(
                "lowering: a callback parameter to '" + symbol.name +
                "' is not supported on the Windows x86-64 hosted backend yet");
          }
          AotThunkParam flat;
          flat.cls = ffi::ParamClass::Pointer;
          flat.argIndex = arg;
          flat.callbackSignature = callbackIndex(param);
          if (gpr < gprLimit) {
            ++gpr;
          } else {
            flat.stackOffset = (thunk.stackArgBytes + 7) / 8 * 8;
            thunk.stackArgBytes = flat.stackOffset + 8;
          }
          thunk.params.push_back(flat);
          continue;
        }
        if (param.isStruct) {
          if (win64) {
            throw LoweringError(
                "lowering: a struct parameter to '" + symbol.name +
                "' is not supported on the Windows x86-64 hosted backend yet");
          }
          ffi::StructLayout layout;
          std::string reason;
          if (!ffi::buildStructLayout(param.fields, layout, reason)) {
            supported = false;
            break;
          }
          const int index = static_cast<int>(thunk.structs.size());
          AotStructLayout entry;
          entry.layout = layout;
          for (std::size_t f = 0; f < layout.fields.size(); ++f) {
            entry.fieldNameLabels.push_back(cg.newLabel());
          }
          if (layout.isMemory) {
            // Class MEMORY: the whole struct travels in the outgoing stack
            // argument area, 8-byte aligned, after every earlier stack argument.
            addStackStruct(index, arg, static_cast<int>(layout.size));
          } else {
            // A register-class struct is passed wholly in registers when its
            // eightbytes fit the remaining GPR/SSE registers, and wholly on the
            // stack otherwise (the System V rule for a partly-registered
            // aggregate). A variadic tail cannot carry a stack-passed fixed
            // prefix, so that shape stays unsupported there.
            int needGpr = 0;
            int needSse = 0;
            for (int e = 0; e < layout.eightbyteCount; ++e) {
              if (layout.eightbyte[e] == 'S') ++needSse; else ++needGpr;
            }
            if (gpr + needGpr <= gprLimit && sse + needSse <= sseLimit) {
              entry.bufSlot = nextBufSlot++;
              for (int e = 0; e < layout.eightbyteCount; ++e) {
                AotThunkParam flat;
                flat.cls = layout.eightbyte[e] == 'S' ? ffi::ParamClass::Double : ffi::ParamClass::Int;
                flat.argIndex = arg;
                flat.eightbyte = e;
                flat.structIndex = index;
                thunk.params.push_back(flat);
              }
              gpr += needGpr;
              sse += needSse;
            } else if (symbol.variadic) {
              supported = false;
              break;
            } else {
              addStackStruct(index, arg, static_cast<int>(layout.size));
            }
          }
          thunk.structs.push_back(std::move(entry));
          continue;
        }
        ffi::ParamClass cls = ffi::ParamClass::Int;
        if (!ffi::classifyParam(param.scalarType, cls)) {
          supported = false;
          break;
        }
        AotThunkParam flat;
        flat.cls = cls;
        flat.argIndex = arg;
        if (win64) {
          // The Microsoft x64 ABI assigns the register by argument position,
          // not by class: argument i uses the i-th integer or SSE register,
          // and every argument from the fifth onward travels on the stack.
          if (arg >= 4) {
            flat.stackOffset = 32 + 8LL * (arg - 4);
            if (!symbol.variadic) thunk.stackArgBytes = flat.stackOffset + 8;
          }
        } else if (cls == ffi::ParamClass::Float || cls == ffi::ParamClass::Double) {
          if (sse < sseLimit) {
            ++sse;
          } else {
            // The eightbyte bound keeps every System V SSE value in xmm0-xmm7,
            // so an SSE spill is unreachable; reject rather than emit an
            // unhandled shape.
            supported = false;
            break;
          }
        } else if (gpr < gprLimit) {
          ++gpr;
        } else if (symbol.variadic) {
          // The variadic tail's stack area is allocated below the fixed frame,
          // so the fixed prefix must stay in registers.
          supported = false;
          break;
        } else {
          // A later GPR eightbyte travels in the outgoing stack argument area,
          // at `[rsp + stackOffset]` before the call.
          flat.stackOffset = (thunk.stackArgBytes + 7) / 8 * 8;
          thunk.stackArgBytes = flat.stackOffset + 8;
        }
        thunk.params.push_back(flat);
      }
      if (!supported) continue;
      if (symbol.returnType.isStruct) {
        ffi::StructLayout layout;
        std::string reason;
        if (!ffi::buildStructLayout(symbol.returnType.fields, layout, reason)) {
          continue;
        }
        const int index = static_cast<int>(thunk.structs.size());
        AotStructLayout entry;
        entry.layout = layout;
        for (std::size_t f = 0; f < layout.fields.size(); ++f) {
          entry.fieldNameLabels.push_back(cg.newLabel());
        }
        thunk.structs.push_back(std::move(entry));
        thunk.returnStructIndex = index;
        thunk.returns = ffi::ReturnClass::Struct;
        thunk.returnType = "";
        // A class-MEMORY return goes through a hidden `sret` buffer instead of
        // the return registers.
        if (layout.isMemory) thunk.returnMemoryBytes = static_cast<std::int64_t>(layout.size);
      } else {
        ffi::ReturnClass ret = ffi::ReturnClass::Int;
        // `Ptr` is supported: the thunk boxes the returned address as a `Void`
        // handle (or nil for NULL). Only a by-value `struct` needs the layout
        // path above, so a bare `Struct` here means the layout was missing.
        if (!ffi::classifyReturn(symbol.returnType.scalarType, ret) ||
            ret == ffi::ReturnClass::Struct) {
          continue;
        }
        thunk.returns = ret;
        thunk.returnType = symbol.returnType.scalarType;
      }
      resolved.insert(symbol.name);
      thunks.push_back(std::move(thunk));
    }
  }
  return thunks;
}

GlobalSlots collectGlobals(const std::vector<const Bytecode*>& protos,
                           const std::unordered_map<std::string, CodeGen::Label>& builtins,
                           const std::unordered_set<std::string>& resolvedExterns) {
  GlobalSlots globals;
  // The host bindings the chunk declares: `extern "C"` symbols and `native fn`
  // names. Each declaration compiles to `GETGLOBAL N; SETGLOBAL N` (the read
  // resolves the name the host registered). The AOT backend has no host, so it
  // resolves a binding from a curated stub when one exists, and otherwise
  // reports it only when the program actually uses it (see the last loop).
  std::unordered_set<std::string> hostBindings;
  std::unordered_set<std::string> externNames;
  for (const Bytecode* proto : protos) {
    for (const ExternSymbol& symbol : proto->externs) {
      hostBindings.insert(symbol.name);
      externNames.insert(symbol.name);
    }
    for (const std::string& name : proto->nativeFns) hostBindings.insert(name);
  }
  globals.hostBindings = hostBindings;
  auto nameOf = [](const Bytecode& proto, int index) -> const std::string& {
    const Value& name = proto.constants.at(static_cast<std::size_t>(index));
    if (!name.isStr()) {
      throw LoweringError("lowering: global name is not a string constant");
    }
    return name.asStr();
  };
  // A builtin (`print`, ...) is a global the hosted engine installs up front, so
  // it needs a slot only when the program reads it. It is pre-assigned first so
  // a user binding (`var print = ...`) reuses the same slot and shadows the
  // builtin, exactly like the hosted engine's "install, then run" order.
  for (const Bytecode* proto : protos) {
    for (const Instruction& instr : proto->code) {
      if (instr.op != OpCode::GETGLOBAL) continue;
      const std::string& name = nameOf(*proto, instr.b);
      if (builtins.find(name) == builtins.end()) continue;
      if (globals.slotOfName.find(name) != globals.slotOfName.end()) continue;
      globals.slotOfName[name] = globals.count++;
    }
  }
  // Every host binding gets a slot, so the `SETGLOBAL` its declaration emits has
  // somewhere to land. A binding with a stub (a builtin the AOT runtime
  // provides) is seeded by the caller, exactly like `print`; one without a stub
  // stays nil, which only matters if the program uses it (reported below).
  for (const std::string& name : hostBindings) {
    if (globals.slotOfName.find(name) != globals.slotOfName.end()) continue;
    globals.slotOfName[name] = globals.count++;
  }
  // A `SETGLOBAL N` whose value is the read of the same `N` is not an
  // assignment: it is the compiler's shape for a host binding (`native fn` and
  // `extern "C"`, which read the name the host registered) and for a
  // self-referential declaration (`var x = x;`). The hosted engine resolves a
  // host binding in its registry; the AOT backend seeds a stub instead. A
  // genuine self-assignment (`var x = x;`) is a run-time error on the hosted
  // engine too, so it stays unassigned and the read is reported below.
  auto isHostBindingShape = [&](const Bytecode& proto, std::size_t index) {
    if (index == 0) return false;
    const Instruction& set = proto.code[index];
    const Instruction& prev = proto.code[index - 1];
    if (set.op != OpCode::SETGLOBAL || prev.op != OpCode::GETGLOBAL) return false;
    return nameOf(proto, set.b) == nameOf(proto, prev.b);
  };
  // First-assignment order across every proto, so a function and its caller
  // (recursion) agree on the slot of a shared name.
  for (const Bytecode* proto : protos) {
    for (std::size_t i = 0; i < proto->code.size(); ++i) {
      const Instruction& instr = proto->code[i];
      if (instr.op != OpCode::SETGLOBAL) continue;
      if (isHostBindingShape(*proto, i)) continue;
      const std::string& name = nameOf(*proto, instr.b);
      if (globals.slotOfName.find(name) != globals.slotOfName.end()) continue;
      globals.slotOfName[name] = globals.count++;
    }
  }
  for (const Bytecode* proto : protos) {
    for (const Instruction& instr : proto->code) {
      if (instr.op != OpCode::GETGLOBAL) continue;
      const std::string& name = nameOf(*proto, instr.b);
      if (globals.slotOfName.find(name) != globals.slotOfName.end()) continue;
      throw LoweringError("lowering: global '" + name + "' is read but never assigned");
    }
  }
  // A host binding the program uses but the AOT backend has no stub for is a
  // real limitation, not a typo: name the symbol and the backend instead of
  // leaving a nil that would fault at run time. A binding counts as used when
  // the compiler recorded a use for it (`nativeUses`), or — for a proto built
  // before the metadata existed — when it is read as a global (`GETGLOBAL`) more
  // often than it is declared, because a declaration emits exactly one read of
  // its own name. A binding that is only declared is harmless (the hosted engine
  // installs it, the AOT backend nils it) and must not fail the build, so
  // `import std.math` (which declares `pow`, `sin`, ... that only a libm-backed
  // host can provide) still compiles when the program calls none of them.
  //
  // `nativeUses` is what makes a call through a module namespace
  // (`std.math.pow(...)`, which reads the name with `GETTABLE` off the module
  // table) and a capture (`GETUPVAL`) fail the build too: neither leaves a
  // `GETGLOBAL` behind, so the read count alone cannot see them. The compiler
  // records every such use on the proto that performs it (PHASE 7f.2), so the
  // lowerer reads the use instead of reconstructing it from the code.
  std::unordered_set<std::string> usedNatives;
  for (const Bytecode* proto : protos) {
    for (const std::string& name : proto->nativeUses) usedNatives.insert(name);
  }
  std::unordered_map<std::string, int> declarations;
  std::unordered_map<std::string, int> reads;
  for (const Bytecode* proto : protos) {
    std::unordered_set<std::string> declared;
    for (const std::string& name : proto->nativeFns) declared.insert(name);
    for (const ExternSymbol& symbol : proto->externs) declared.insert(symbol.name);
    for (const std::string& name : declared) declarations[name]++;
    for (const Instruction& instr : proto->code) {
      if (instr.op == OpCode::GETGLOBAL) reads[nameOf(*proto, instr.b)]++;
    }
  }
  for (const std::string& name : hostBindings) {
    if (builtins.find(name) != builtins.end()) continue;  // has a stub
    if (resolvedExterns.find(name) != resolvedExterns.end()) continue;  // a linker-resolved scalar thunk
    const bool used = usedNatives.find(name) != usedNatives.end();
    if (!used && reads[name] <= declarations[name]) continue;  // declared but unused
    if (externNames.find(name) != externNames.end()) {
      throw LoweringError("lowering: extern \"C\" symbol '" + name +
                          "' cannot be resolved by the AOT backend (PHASE 7f)");
    }
    throw LoweringError("lowering: native fn '" + name +
                        "' cannot be resolved by the AOT backend (PHASE 7f)");
  }
  return globals;
}

// Every proto in the tree, root first, then its descendants (pre-order). Each
// one is emitted as its own machine-code function; a `CLOSURE` needs its child
// entry label, so the labels are created for the whole tree before any body is
// emitted.
void collectProtos(const Bytecode& proto, std::vector<const Bytecode*>& out) {
  out.push_back(&proto);
  for (const std::shared_ptr<Bytecode>& child : proto.children) {
    if (child) collectProtos(*child, out);
  }
}

// Emits the string-constant data islands at the end of the code buffer. Called
// after every code path has ended in `ret`/`ud2`, so control never falls into
// them; the `lea` fixups emitted by `loadStringImmediate` resolve against these
// labels.
void emitStringConstants(CodeGen& cg, const std::vector<StringConstant>& constants) {
  // `AotString` begins with a `uint64_t`, so each header must land on an
  // 8-byte boundary: the layout is the contract every AOT target shares and
  // the runtime's arena allocator already honors it. x86-64 happens to
  // tolerate the misaligned load, but the pool must not rely on that.
  constexpr std::uint8_t kPad = 0x00;
  for (const StringConstant& constant : constants) {
    while (cg.offset() % alignof(std::uint64_t) != 0) cg.emitBytes(&kPad, 1);
    cg.bind(constant.label);
    const std::uint64_t length = constant.bytes.size();
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(&length), sizeof length);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(constant.bytes.data()),
                 constant.bytes.size());
  }
}

// The shape of the frame a proto's body runs in, and the set of its local
// slots some child captures. `fileRegisters` is the allocated register-file
// size and `fileLocals` the allocated local region; both are at least the
// proto's declared counts. A slot in `capturedSlots` holds a cell pointer
// instead of an inline `AotValue`, and `hasSelf` says whether the frame has an
// `AotClosure*` in its `self` slot (a bytecode function does, the root chunk
// does not).
struct FrameInfo {
  int fileRegisters = 1;
  int fileLocals = 0;
  bool hasSelf = false;
  // The root chunk's shared callback arena-state slot, or -1. A child proto
  // never runs the root epilogue, so it stays -1 there.
  int callbackStateSlot = -1;
  std::unordered_set<int> capturedSlots;
};

// The local slots of `proto` that at least one of its children captures: each
// is a `UpvalDesc` with `inParentLocal` pointing at the slot. Such a slot must
// live in a shared cell so a closure and the frame observe the same variable
// (PHASE 7c.2c.2).
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

// The `GETGLOBAL` indices of `proto` whose read can observe a slot nothing has
// assigned, so the lowering must route them through `oskel_global_get_checked`.
//
// A read is safe when every path from the proto's entry to it has already
// executed a `SETGLOBAL` of the same name, or when the chunk seeds the slot
// before the program runs (`globals.seeded`). Anything else -- a read before the
// first assignment, a read reachable through a branch that skips the assignment,
// or a read of a name assigned only in another proto -- can see `Unset` at run
// time and must be checked, because the hosted engine fails it with "undefined
// variable '<name>'" and exit 70.
//
// The analysis is a forward "may" dataflow over the instruction graph: a name is
// in `mayUnset[i]` when *some* path from the entry to `i` leaves it unassigned.
// Joins therefore union (the conservative direction for a "may" property), and
// the entry starts with every candidate. A `SETGLOBAL` removes its name from the
// state only when it really assigns; the compiler spells a host binding as
// `GETGLOBAL N; SETGLOBAL N`, which stores a value it just read, so that shape
// leaves the name alone and a later read of an unresolvable binding still gets
// the check (a used one fails the build earlier, in `collectGlobals`).
//
// `candidates` is computed across the whole program, not per proto: a name
// assigned only inside a never-called function is still unset at the top-level
// read, and a per-proto candidate set would miss it.
std::unordered_set<std::size_t> checkedGlobalReads(
    const Bytecode& proto, const GlobalSlots& globals,
    const std::unordered_set<std::string>& candidates) {
  std::unordered_set<std::size_t> checked;
  const std::size_t size = proto.code.size();
  if (size == 0) return checked;
  auto nameOf = [&](int index) -> const std::string& {
    const Value& name = proto.constants.at(static_cast<std::size_t>(index));
    if (!name.isStr()) {
      throw LoweringError("lowering: global name is not a string constant");
    }
    return name.asStr();
  };
  // `GETGLOBAL N; SETGLOBAL N` -- the compiler's shape for "resolve the host
  // binding" (`native fn`, `extern "C"`) and for a self-referential declaration
  // (`var x = x;`). Only a host binding resolves to something; for `var x = x;`
  // the read is a genuine read of an unset global, so it keeps the check.
  auto isHostBindingDeclaration = [&](std::size_t i) {
    if (i + 1 >= size) return false;
    if (proto.code[i].op != OpCode::GETGLOBAL) return false;
    if (proto.code[i + 1].op != OpCode::SETGLOBAL) return false;
    const std::string& name = nameOf(proto.code[i].b);
    return nameOf(proto.code[i + 1].b) == name &&
           globals.hostBindings.count(name) != 0;
  };

  std::vector<std::unordered_set<std::string>> mayUnset(size);
  std::vector<bool> reachable(size, false);
  std::vector<bool> queued(size, false);
  std::vector<std::size_t> work;

  reachable[0] = true;
  mayUnset[0] = candidates;
  work.push_back(0);
  queued[0] = true;
  auto pushSuccessor = [&](std::size_t target, const std::unordered_set<std::string>& out) {
    if (target >= size) return;
    if (!reachable[target]) {
      reachable[target] = true;
      mayUnset[target] = out;
      if (!queued[target]) { queued[target] = true; work.push_back(target); }
      return;
    }
    // Union: a name stays "may be unset" when any incoming path leaves it so.
    bool grew = false;
    for (const std::string& name : out) {
      if (mayUnset[target].insert(name).second) grew = true;
    }
    if (grew && !queued[target]) { queued[target] = true; work.push_back(target); }
  };

  while (!work.empty()) {
    const std::size_t i = work.back();
    work.pop_back();
    queued[i] = false;
    const Instruction& instr = proto.code[i];
    std::unordered_set<std::string> out = mayUnset[i];
    // A `SETGLOBAL` assigns, except when it stores the value its own preceding
    // `GETGLOBAL` just read: that is the compiler's host-binding resolution,
    // which assigns nothing the program can rely on.
    const bool resolvesHostBinding =
        i > 0 && isHostBindingDeclaration(i - 1);
    if (instr.op == OpCode::SETGLOBAL && !resolvesHostBinding) {
      out.erase(nameOf(instr.b));
    }
    switch (instr.op) {
      case OpCode::JMP:
        pushSuccessor(static_cast<std::size_t>(instr.jump), out);
        break;
      case OpCode::JMP_IF_FALSE:
      case OpCode::JMP_IF_TRUE:
        pushSuccessor(static_cast<std::size_t>(instr.jump), out);
        if (i + 1 < size) pushSuccessor(i + 1, out);
        break;
      case OpCode::HALT:
      case OpCode::RETURN:
        break;
      default:
        if (i + 1 < size) pushSuccessor(i + 1, out);
        break;
    }
  }

  for (std::size_t i = 0; i < size; ++i) {
    if (!reachable[i]) continue;
    if (proto.code[i].op != OpCode::GETGLOBAL) continue;
    if (isHostBindingDeclaration(i)) continue;
    if (mayUnset[i].count(nameOf(proto.code[i].b)) != 0) checked.insert(i);
  }
  return checked;
}

void lowerInstruction(std::size_t index, const Instruction& instr, const Bytecode& proto,
                      CodeGen& cg,
                      const std::vector<CodeGen::Label>& labels,
                      const AotRuntimeHandles& rt, const GlobalSlots& globals,
                      const FrameInfo& frame,
                      const std::unordered_map<const Bytecode*, CodeGen::Label>& protoEntry,
                      const std::unordered_map<const Bytecode*, CodeGen::Label>& displayOf,
                      std::vector<StringConstant>& stringConstants,
                      const std::unordered_set<std::size_t>& checkedReads) {
  const int fileRegisters = frame.fileRegisters;
  switch (instr.op) {
    case OpCode::LOADK: {
      const Value& value = proto.constants.at(static_cast<std::size_t>(instr.b));
      if (value.isInt()) {
        loadIntImmediate(cg, instr.a, value.asInt());
      } else if (value.isBool()) {
        loadBoolImmediate(cg, instr.a, value.asBool());
      } else if (value.isNil()) {
        loadNilImmediate(cg, instr.a);
      } else if (value.isFloat()) {
        loadFloatingImmediate(cg, instr.a, value.asFloat(), AotTag::Float);
      } else if (value.isDouble()) {
        loadFloatingImmediate(cg, instr.a, value.asDouble(), AotTag::Double);
      } else if (value.isStr()) {
        StringConstant constant{cg.newLabel(), value.asStr()};
        loadStringImmediate(cg, instr.a, constant.label);
        stringConstants.push_back(std::move(constant));
      } else {
        unsupported("LOADK (unsupported constant)", "PHASE 7c.2");
      }
      break;
    }

    case OpCode::MOVE:
      loadValue(cg, instr.b, kTmpTag, kTmpPayload);
      storeValue(cg, instr.a, kTmpTag, kTmpPayload);
      break;

    case OpCode::ADD:
      callBinaryWithSelector(cg, rt.arithValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotArithOp::Add));
      break;
    case OpCode::SUB:
      callBinaryWithSelector(cg, rt.arithValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotArithOp::Sub));
      break;
    case OpCode::MUL:
      callBinaryWithSelector(cg, rt.arithValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotArithOp::Mul));
      break;
    case OpCode::DIV:
      callBinaryWithSelector(cg, rt.arithValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotArithOp::Div));
      break;
    case OpCode::MOD:
      callBinaryWithSelector(cg, rt.arithValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotArithOp::Mod));
      break;
    case OpCode::POW:
      callBinary(cg, rt.powValues, instr.b, instr.c, instr.a);
      break;

    case OpCode::SHL:
      callBinaryWithSelector(cg, rt.shiftValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotShiftOp::Shl));
      break;
    case OpCode::SHR:
      callBinaryWithSelector(cg, rt.shiftValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotShiftOp::Shr));
      break;

    case OpCode::BAND:
    case OpCode::BOR:
    case OpCode::BXOR: {
      // The selector is the third argument, so it goes in r8 (after the two
      // `AotValue`s occupy rdi:rsi and rdx:rcx). AotBitwiseOp matches the
      // hosted operator one-to-one.
      const AotBitwiseOp op = instr.op == OpCode::BAND
                                  ? AotBitwiseOp::And
                                  : (instr.op == OpCode::BOR ? AotBitwiseOp::Or
                                                             : AotBitwiseOp::Xor);
      callBinaryWithSelector(cg, rt.bitwiseValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(op));
      break;
    }

    case OpCode::UNOP: {
      switch (static_cast<UnaryOp>(instr.c)) {
        case UnaryOp::Negate:
          callUnary(cg, rt.negateValue, instr.b, instr.a);
          break;
        case UnaryOp::Not:
          callUnary(cg, rt.notValue, instr.b, instr.a);
          break;
        case UnaryOp::BitNot:
          callUnary(cg, rt.bitNotValue, instr.b, instr.a);
          break;
        default:
          throw LoweringError("lowering: invalid UNOP selector " +
                              std::to_string(instr.c));
      }
      break;
    }

    case OpCode::EQ:
    case OpCode::NEQ:
    case OpCode::LT:
    case OpCode::LTE:
    case OpCode::GT:
    case OpCode::GTE: {
      // The op selector is the low nibble of the x86-64 condition code, so the
      // runtime dispatches with a `cmp` + `je` chain. Map each bytecode
      // comparison to its `AotCompareOp`.
      AotCompareOp op;
      switch (instr.op) {
        case OpCode::EQ: op = AotCompareOp::Eq; break;
        case OpCode::NEQ: op = AotCompareOp::Ne; break;
        case OpCode::LT: op = AotCompareOp::Lt; break;
        case OpCode::LTE: op = AotCompareOp::Le; break;
        case OpCode::GT: op = AotCompareOp::Gt; break;
        default: op = AotCompareOp::Ge; break;
      }
      loadValue(cg, instr.b, X64Reg::Rdi, X64Reg::Rsi);
      loadValue(cg, instr.c, X64Reg::Rdx, X64Reg::Rcx);
      cg.movImm(X64Reg::R8, static_cast<std::int64_t>(op));
      cg.callLabel(rt.compareValues);
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }

    case OpCode::JMP:
      cg.jmp(jumpTarget(instr, proto, labels));
      break;
    case OpCode::JMP_IF_FALSE:
    case OpCode::JMP_IF_TRUE: {
      // The condition is the value's truthiness, not its raw payload: `0` is
      // truthy in Oskel, so `oskel_truthy` decides. Branch on its 0/1 result.
      loadValue(cg, instr.a, X64Reg::Rdi, X64Reg::Rsi);
      cg.callLabel(rt.truthy);
      cg.testReg(X64Reg::Rax);
      cg.jcc(instr.op == OpCode::JMP_IF_FALSE ? Jcc::Eq : Jcc::Ne,
             jumpTarget(instr, proto, labels));
      break;
    }

    case OpCode::HALT:
      // `HALT` ends the program by publishing R0 and running the root
      // epilogue. A child proto has its own frame and ends in `RETURN`, so a
      // `HALT` there would unwind the wrong frame (and read the root's R0);
      // reject it instead of emitting undefined behavior.
      if (frame.hasSelf) {
        throw LoweringError("lowering: HALT inside a function proto");
      }
      // The chunk's result is R0, returned as an `AotValue` in rax:rdx (the
      // System V ABI's two-register return), so the test harness can read both
      // the tag and the payload. The process entry discards it and exits 0.
      loadValue(cg, 0, X64Reg::Rax, X64Reg::Rdx);
      emitRootEpilogue(cg, frame.callbackStateSlot);
      break;

    // Globals (7c.2a), locals (7c.2b) and calls/closures (7c.2c.1) are lowered.
    // Upvalues need capturable cells (7c.2c.2); collections and iteration need
    // the arena (7c.3).
    case OpCode::GETGLOBAL: {
      const std::string& name = proto.constants.at(static_cast<std::size_t>(instr.b)).asStr();
      const int slot = globals.slotOfName.at(name);
      cg.movImm(X64Reg::Rdi, slot);
      if (checkedReads.count(index) == 0) {
        cg.callLabel(rt.globalGet);
      } else {
        // The hosted engine fails this read with `undefined variable '<name>'`
        // (exit 70); the check reproduces it. The message is built here, where
        // the name is known, so the image carries the exact wording.
        StringConstant message{cg.newLabel(),
                               "undefined variable '" + name + "'"};
        cg.leaRipLabel(X64Reg::Rsi, message.label);
        stringConstants.push_back(std::move(message));
        cg.callLabel(rt.globalGetChecked);
      }
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }
    case OpCode::SETGLOBAL: {
      const std::string& name = proto.constants.at(static_cast<std::size_t>(instr.b)).asStr();
      const int slot = globals.slotOfName.at(name);
      cg.movImm(X64Reg::Rdi, slot);
      loadValue(cg, instr.a, X64Reg::Rsi, X64Reg::Rdx);
      cg.callLabel(rt.globalSet);
      break;
    }
    // Locals (PHASE 7c.2b, cells in 7c.2c.2). `b` is the cell index in the
    // current frame. A slot no child captures is a plain 16-byte slot in the
    // second region of the frame; a slot some child captures holds a pointer to
    // a shared `AotValue` cell, so a closure and the frame mutate the same
    // variable. `DECLLOCAL` allocates the cell (once) and stores the value,
    // `SETLOCAL` stores through it, `GETLOCAL` loads through it. The hosted
    // engine's "undefined local" check needs a null cell, which a stack slot
    // cannot express, so a well-formed chunk (the compiler always emits
    // `DECLLOCAL` first) is assumed.
    case OpCode::DECLLOCAL:
    case OpCode::SETLOCAL: {
      loadValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      const std::int64_t off = localSlotOffset(fileRegisters, instr.b);
      if (frame.capturedSlots.count(instr.b) != 0) {
        if (instr.op == OpCode::DECLLOCAL) {
          // oskel_cell_new(value) -> cell pointer; keep it in the slot. A fresh
          // cell per declaration, so a closure created earlier keeps the cell
          // it captured (a loop body re-declaring a variable shadows it).
          cg.movReg(X64Reg::Rdi, X64Reg::Rax);
          cg.movReg(X64Reg::Rsi, X64Reg::Rdx);
          cg.callLabel(rt.cellNew);
          cg.movToMem(kFileBase, off, X64Reg::Rax);
        } else {
          cg.movFromMem(X64Reg::Rcx, kFileBase, off);  // rcx = cell
          cg.movToMem(X64Reg::Rcx, 0, X64Reg::Rax);
          cg.movToMem(X64Reg::Rcx, 8, X64Reg::Rdx);
        }
      } else {
        cg.movToMem(kFileBase, off, X64Reg::Rax);
        cg.movToMem(kFileBase, off + 8, X64Reg::Rdx);
      }
      break;
    }
    case OpCode::GETLOCAL: {
      const std::int64_t off = localSlotOffset(fileRegisters, instr.b);
      if (frame.capturedSlots.count(instr.b) != 0) {
        cg.movFromMem(X64Reg::Rcx, kFileBase, off);  // rcx = cell
        cg.movFromMem(X64Reg::Rax, X64Reg::Rcx, 0);
        cg.movFromMem(X64Reg::Rdx, X64Reg::Rcx, 8);
      } else {
        cg.movFromMem(X64Reg::Rax, kFileBase, off);
        cg.movFromMem(X64Reg::Rdx, kFileBase, off + 8);
      }
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }
    case OpCode::CALL: {
      // R[a] = callee, R[a+1 .. a+b] = arguments, contiguous in this frame.
      // Pass the callee (rdi:rsi), the count (rdx) and the argument window's
      // address (rcx) to `oskel_call`, which checks the callee is a function and
      // dispatches it. The result comes back in rax:rdx, straight into R[a].
      loadValue(cg, instr.a, X64Reg::Rdi, X64Reg::Rsi);
      cg.movImm(X64Reg::Rdx, instr.b);
      cg.leaMem(X64Reg::Rcx, kFileBase, slotOffset(instr.a + 1));
      cg.callLabel(rt.call);
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }
    case OpCode::RETURN:
      // Publish R[a] as the frame's result (rax:rdx) and leave the frame. A
      // `RETURN` in the root chunk has no caller: the VM treats it exactly like
      // `HALT` (the REPL's last expression and a top-level `return` compile to
      // it), so publish R[a] and run the root epilogue instead of the function
      // one. The process entry discards the chunk result and exits 0.
      loadValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      if (frame.hasSelf) {
        emitFunctionEpilogue(cg);
      } else {
        emitRootEpilogue(cg, frame.callbackStateSlot);
      }
      break;
    case OpCode::CLOSURE: {
      if (instr.b < 0 || static_cast<std::size_t>(instr.b) >= proto.children.size()) {
        throw LoweringError("lowering: CLOSURE child index " + std::to_string(instr.b) +
                            " is out of range");
      }
      const Bytecode* child = proto.children[static_cast<std::size_t>(instr.b)].get();
      // oskel_closure(code, arity, upvalueCount, display) -> Func in rax:rdx.
      // `display` is the text `repr`/`str` render for the closure, matching the
      // hosted engine's `<fn name>` for the same proto.
      cg.leaRipLabel(X64Reg::Rdi, protoEntry.at(child));
      cg.movImm(X64Reg::Rsi, child->numParams);
      cg.movImm(X64Reg::Rdx, static_cast<std::int64_t>(child->upvalues.size()));
      cg.leaRipLabel(X64Reg::Rcx, displayOf.at(child));
      cg.callLabel(rt.closure);
      // The capture slots are the words after the header; the lowering fills
      // each with the cell pointer it resolved. The child's upvalue `i` points
      // either at a local slot of this frame or at one of this frame's own
      // upvalues (re-exported), exactly as `resolveFromFrame` decided.
      for (std::size_t i = 0; i < child->upvalues.size(); ++i) {
        const UpvalDesc& desc = child->upvalues[i];
        const std::int64_t cellSlot = static_cast<std::int64_t>(kAotClosureHeaderSize) +
                                      static_cast<std::int64_t>(i) * 8;
        if (desc.inParentLocal) {
          // This frame's local slot holds the cell pointer (it is captured, so
          // it is a cell slot by construction).
          cg.movFromMem(X64Reg::Rcx, kFileBase, localSlotOffset(fileRegisters, desc.index));
        } else {
          if (!frame.hasSelf) {
            throw LoweringError("lowering: CLOSURE re-exports an upvalue but the frame has no self");
          }
          cg.movFromMem(X64Reg::Rcx, kFileBase, selfSlotOffset(fileRegisters, frame.fileLocals));
          cg.movFromMem(X64Reg::Rcx, X64Reg::Rcx,
                        static_cast<std::int64_t>(kAotClosureHeaderSize) +
                            static_cast<std::int64_t>(desc.index) * 8);
        }
        // The closure object is in rdx (the payload); store the cell pointer
        // into its slot `i`.
        cg.movToMem(X64Reg::Rdx, cellSlot, X64Reg::Rcx);
      }
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }
    case OpCode::GETUPVAL: {
      // Load the captured cell from self, then the value it holds.
      cg.movFromMem(X64Reg::Rcx, kFileBase, selfSlotOffset(fileRegisters, frame.fileLocals));
      cg.movFromMem(X64Reg::Rcx, X64Reg::Rcx,
                    static_cast<std::int64_t>(kAotClosureHeaderSize) +
                        static_cast<std::int64_t>(instr.b) * 8);
      cg.movFromMem(X64Reg::Rax, X64Reg::Rcx, 0);
      cg.movFromMem(X64Reg::Rdx, X64Reg::Rcx, 8);
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }
    case OpCode::SETUPVAL: {
      loadValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      cg.movFromMem(X64Reg::Rcx, kFileBase, selfSlotOffset(fileRegisters, frame.fileLocals));
      cg.movFromMem(X64Reg::Rcx, X64Reg::Rcx,
                    static_cast<std::int64_t>(kAotClosureHeaderSize) +
                        static_cast<std::int64_t>(instr.b) * 8);
      cg.movToMem(X64Reg::Rcx, 0, X64Reg::Rax);
      cg.movToMem(X64Reg::Rcx, 8, X64Reg::Rdx);
      break;
    }
    case OpCode::NEWTABLE:
      // R[a] = a fresh array (`c == 0`) or map (`c == 1`); `b` is a capacity
      // hint the runtime ignores, exactly like the VM (PHASE 6c).
      cg.movImm(X64Reg::Rdi, instr.c);
      cg.callLabel(rt.tableNew);
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    case OpCode::GETTABLE:
      // R[a] = R[b][R[c]] (dynamic index) or R[b].<name> (constant member name,
      // encoded as `-c - 1`). The name form loads the string constant into
      // rdx:rcx as a value, so both paths share one helper.
      loadValue(cg, instr.b, X64Reg::Rdi, X64Reg::Rsi);
      if (instr.c >= 0) {
        loadValue(cg, instr.c, X64Reg::Rdx, X64Reg::Rcx);
        cg.callLabel(rt.tableGet);
      } else {
        loadMemberName(cg, proto, instr.c, X64Reg::Rdx, X64Reg::Rcx, stringConstants);
        cg.callLabel(rt.memberGet);
      }
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    case OpCode::SETTABLE:
      // R[b][R[c]] = R[a] or R[b].<name> = R[a]. The value goes in r8:r9 (the
      // third `AotValue`), after the object (rdi:rsi) and the index (rdx:rcx).
      loadValue(cg, instr.b, X64Reg::Rdi, X64Reg::Rsi);
      if (instr.c >= 0) {
        loadValue(cg, instr.c, X64Reg::Rdx, X64Reg::Rcx);
        loadValue(cg, instr.a, X64Reg::R8, X64Reg::R9);
        cg.callLabel(rt.tableSet);
      } else {
        loadMemberName(cg, proto, instr.c, X64Reg::Rdx, X64Reg::Rcx, stringConstants);
        loadValue(cg, instr.a, X64Reg::R8, X64Reg::R9);
        cg.callLabel(rt.memberSet);
      }
      break;
    case OpCode::IN: {
      // R[a] = R[b] in R[c]: the needle in rdi:rsi, the haystack in rdx:rcx.
      loadValue(cg, instr.b, X64Reg::Rdi, X64Reg::Rsi);
      loadValue(cg, instr.c, X64Reg::Rdx, X64Reg::Rcx);
      cg.callLabel(rt.contains);
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }
    case OpCode::ITER: {
      // R[a] = the next item; R[d] (when d >= 0) receives the paired value of a
      // map entry and R[e] the done flag. The iterable is R[b], the cursor R[c].
      // `oskel_iter_next` takes the cursor's address (rdi), the iterable by
      // value (rsi:rdx, the second `AotValue`) and the value slot's address
      // (rcx, or null when the loop has no value variable); it returns the item
      // in rax:rdx, the flag in r8 and writes the value straight into the slot.
      loadValue(cg, instr.b, X64Reg::Rsi, X64Reg::Rdx);
      cg.leaMem(X64Reg::Rdi, kFileBase, slotOffset(instr.c));
      if (instr.d >= 0) {
        cg.leaMem(X64Reg::Rcx, kFileBase, slotOffset(instr.d));
      } else {
        cg.xorReg(X64Reg::Rcx, X64Reg::Rcx);
      }
      cg.callLabel(rt.iterNext);
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      cg.movImmToMem(kFileBase, slotOffset(instr.e), static_cast<std::int64_t>(AotTag::Bool));
      cg.movToMem(kFileBase, slotOffset(instr.e) + 8, X64Reg::R8);
      break;
    }
  }
}

}  // namespace

LoweredChunk lowerToX64(const Target& target, const Bytecode& proto,
                        const CodeGen& /*codegen*/, LinkMode mode) {
  requireSupportedTarget(target);
  requireSupportedLinkMode(target, mode);

  CodeGen cg(target);

  // The entry prelude and the runtime share the program's code buffer, so
  // every `call` resolves through the emitter's labels: no linker, no
  // relocations.
  RuntimeHandles entry = newRuntimeHandles(cg);
  AotRuntimeHandles rt = newAotRuntimeHandles(cg);
  CodeGen::Label chunkEntry = cg.newLabel();
  CodeGen::Label mainEntry = cg.newLabel();
  std::vector<StringConstant> stringConstants;

  // The bare-metal prelude is a multiboot kernel header followed by a 32-bit
  // long-mode trampoline, and multiboot requires the header in the first 8 KiB
  // of the file, so it must be the very first thing in the code buffer. Emit it
  // now, before the chunk; its `call`s to `mainEntry` and the exit thunk are
  // forward references the emitter patches at `finalize()`. The other targets
  // append their prelude at the end, after the program.
  std::uint64_t bareMetalEntry = 0;
  if (target.os == Os::Freestanding) {
    bareMetalEntry = emitBareMetalPreamble(cg, entry, mainEntry);
  }

  // Every proto in the tree, root first. Each becomes its own machine function;
  // `protoEntry` maps a proto to its entry label so a `CLOSURE` can address a
  // child's code before that child's body is emitted.
  std::vector<const Bytecode*> protos;
  collectProtos(proto, protos);
  std::unordered_map<const Bytecode*, CodeGen::Label> protoEntry;
  for (const Bytecode* p : protos) {
    protoEntry[p] = (p == &proto) ? chunkEntry : cg.newLabel();
  }

  // The text `repr`/`str` render for each function value, one data island per
  // proto: `<fn name>`, matching the hosted engine's `Value::writeValue`. A
  // closure built from the proto points its `display` field here, so the render
  // is a plain append and the closure keeps its fixed-size header.
  std::unordered_map<const Bytecode*, CodeGen::Label> displayOf;
  for (const Bytecode* p : protos) {
    // The root chunk is never a value: only a `CLOSURE` names a proto.
    if (p != &proto) displayOf[p] = cg.newLabel();
  }

  // The builtins the AOT runtime provides, keyed by the global name the hosted
  // engine installs. The chunk seeds each one into its global slot as an
  // `AotClosure`, so a `CALL` reaches it through the ordinary `oskel_call` path.
  std::unordered_map<std::string, CodeGen::Label> builtins = {
      {"print", rt.printBuiltin},
      {"write", rt.writeBuiltin},
      {"eprint", rt.eprintBuiltin},
      // The hidden helpers the compiler emits for PHASE 6.5 features.
      // `$isError` (try/catch) and `$nonNull` (`a!!`) are seeded the same way,
      // so those constructs lower to ordinary calls. `$str`, `$range` and
      // `$slice` are seeded too.
      {"$isError", rt.isErrorBuiltin},
      {"$nonNull", rt.nonNullBuiltin},
      {"$str", rt.strBuiltin},
      {"$range", rt.rangeBuiltin},
      {"$slice", rt.sliceBuiltin},
      // The public standard-library builtins (PHASE 7c.5a). Seeded the same way,
      // so `len(x)` and `print(x)` are both ordinary calls. `str` and `isError`
      // reuse the `$str` and `$isError` code; a user binding of any of these
      // names shadows the builtin, exactly like the hosted engine's
      // "install builtins, then run the chunk" order.
      {"len", rt.lenBuiltin},
      {"type", rt.typeBuiltin},
      {"str", rt.strBuiltin},
      {"repr", rt.reprBuiltin},
      {"bool", rt.boolBuiltin},
      {"ord", rt.ordBuiltin},
      {"chr", rt.chrBuiltin},
      {"assert", rt.assertBuiltin},
      {"panic", rt.panicBuiltin},
      {"isError", rt.isErrorBuiltin},
      {"error", rt.errorBuiltin},
      // The numeric conversions (PHASE 7c.5b.1): `int`/`float`/`double` for a
      // numeric or boolean operand, matching the hosted conversion.
      {"int", rt.intBuiltin},
      {"float", rt.floatBuiltin},
      {"double", rt.doubleBuiltin},
      // The exact math builtins (PHASE 7c.5b.3): `floor`, `ceil`, `round` and
      // `sqrt`. Correctly rounded, so they match the hosted engine bit for bit
      // without an embedded libm. Seeding `sqrt` here also resolves the
      // `native fn sqrt` binding `std.math` declares (PHASE 7f.1), and makes a
      // bare `sqrt(x)` (the hosted builtin, no declaration) work like
      // `len`/`type`.
      {"floor", rt.floorBuiltin},
      {"ceil", rt.ceilBuiltin},
      {"round", rt.roundBuiltin},
      {"sqrt", rt.sqrtBuiltin},
      // The transcendental builtins (PHASE 7c.5b.4a): `exp`, `log` and `pow`.
      // They need libm in the hosted engine; the freestanding runtime embeds the
      // ported musl routines and reproduces the hosted results to 1 ulp.
      {"exp", rt.expBuiltin},
      {"log", rt.logBuiltin},
      {"pow", rt.powBuiltin},
      // The trigonometric builtins (PHASE 7c.5b.4b): `sin`, `cos` and `tan`.
      {"sin", rt.sinBuiltin},
      {"cos", rt.cosBuiltin},
      {"tan", rt.tanBuiltin},
      // The remaining public builtins (PHASE 7c.5b.5): `range`, `assertEq` and
      // `exit`. `range` matches the hosted `range`; `assertEq` reproduces the
      // hosted comparison message; `exit` leaves through the entry prelude.
      {"range", rt.rangeFnBuiltin},
      {"assertEq", rt.assertEqBuiltin},
      {"exit", rt.exitBuiltin},
      // `time()` (wall clock): the seconds since the Unix epoch, read from the
      // `entry.now` handle the target's prelude binds. It matches the hosted
      // builtin, so `time()` is one more builtin that lowers to an ordinary
      // call.
      {"time", rt.timeBuiltin},
      // The target names (PHASE 10): compile-time constants, so every target
      // seeds them just like `time()`.
      {"platform", rt.platformBuiltin},
      {"arch", rt.archBuiltin},
      // The pointer builtins (BACKLOG §6 item 25). A handle is an `AotPtr` in the
      // arena, so every target backs them and they are seeded unconditionally.
      {"cstring", rt.cstringBuiltin},
      {"cstr", rt.cstrBuiltin},
      {"buffer", rt.bufferBuiltin},
      {"byteLen", rt.byteLenBuiltin},
      {"loadByte", rt.loadByteBuiltin},
      {"storeByte", rt.storeByteBuiltin},
      {"intToPtr", rt.intToPtrBuiltin},
      {"ptrToInt", rt.ptrToIntBuiltin},
      {"free", rt.freeBuiltin},
      // `clock()`: elapsed seconds since the program started, from the
      // `entry.monotonic` handle every prelude binds.
      {"clock", rt.clockBuiltin},
  };

  // `input` reads the keyboard, so only the bare-metal prelude -- the one that
  // binds `entry.read` -- seeds it. The other targets reject it by name below,
  // like the file builtins. This mirrors `emitAotRuntime`'s `hasInput`.
  const bool hasInput = target.os == Os::Freestanding;
  if (hasInput) builtins.emplace("input", rt.inputBuiltin);

  // The file builtins and `args()` (BACKLOG §6 item 25) need something from the
  // prelude, so they are seeded only where it exists:
  //
  //   * `readFile`/`writeFile`/`fileExists` are written against
  //     `entry.open`/`readFd`/`writeFd`/`close`, which every prelude with a file
  //     system binds. A bare-metal image has none, so there the three are
  //     rejected by name below rather than emitted against an unbound label.
  //   * `args()` reads the `(argc, argv)` the prelude hands `main`. The hosted
  //     runtime passes one (the Windows `main` wrapper moves the CRT's rcx/rdx
  //     to the chunk's rdi/rsi, and the own linker's `_start` obtains it with
  //     `__getmainargs`); freestanding Linux passes one. The freestanding
  //     Windows image and bare metal have no command line, so `args` stays
  //     unseeded there.
  const bool hasFiles = target.os != Os::Freestanding || mode == LinkMode::Hosted;
  const bool hasArgv = mode == LinkMode::Hosted ||
                       (target.os == Os::Linux && target.arch == Arch::X86_64);
  if (hasFiles) {
    builtins.emplace("readFile", rt.readFileBuiltin);
    builtins.emplace("writeFile", rt.writeFileBuiltin);
    builtins.emplace("fileExists", rt.fileExistsBuiltin);
    builtins.emplace("fsOpen", rt.fsOpenBuiltin);
    builtins.emplace("fsRead", rt.fsReadBuiltin);
    builtins.emplace("fsWrite", rt.fsWriteBuiltin);
    builtins.emplace("fsSeek", rt.fsSeekBuiltin);
    builtins.emplace("fsClose", rt.fsCloseBuiltin);
    builtins.emplace("fsRemove", rt.fsRemoveBuiltin);
    builtins.emplace("fsRename", rt.fsRenameBuiltin);
    builtins.emplace("fsMkdir", rt.fsMkdirBuiltin);
    builtins.emplace("fsRmdir", rt.fsRmdirBuiltin);
    builtins.emplace("fsList", rt.fsListBuiltin);
    builtins.emplace("fsStat", rt.fsStatBuiltin);
    builtins.emplace("fsCwd", rt.fsCwdBuiltin);
    builtins.emplace("fsChdir", rt.fsChdirBuiltin);
  }
  if (hasArgv) builtins.emplace("args", rt.argsBuiltin);
  // `random()`/`seed()` (PHASE 7c.5b.6) work on every target: the generator is
  // emitted in machine code and its state lives in the arena, so no host and no
  // syscall is involved. Both are seeded unconditionally; `emitAotRuntime` emits
  // the generator only when `randomStateSlot` is set below, so a program that
  // never draws carries none of it.
  builtins.emplace("random", rt.randomBuiltin);
  builtins.emplace("seed", rt.seedBuiltin);
  // `pcall(fn, args...)` (BACKLOG §6 item 4) works on every target: it is the
  // only way to recover from a `panic` raised inside a function chosen at run
  // time, and the AOT runtime reports a failure by exiting, so the builtin
  // installs a handler the error sink jumps back into (a non-local return in the
  // call ABI). Seeded unconditionally; `emitAotRuntime` emits the body and the
  // sink's handler check only when the program actually calls it, so a program
  // that does not keeps its image byte for byte.
  builtins.emplace("pcall", rt.pcallBuiltin);
  // A builtin whose backing exists on only some targets is seeded where it does
  // and *named* where it does not, so the failure points at the target instead
  // of the generic "global 'X' is read but never assigned" from `collectGlobals`
  // -- which is what a typo produces, and the two are not the same problem. A
  // user binding (`var memSize = 5`) shadows the builtin exactly as it shadows
  // `print`, so only a read with no assignment counts; a shadowed name is left
  // to the ordinary global path.
  auto unassignedRead = [&](const char* builtin) {
    bool assigned = false;
    bool read = false;
    for (const Bytecode* p : protos) {
      for (const Instruction& instr : p->code) {
        if (instr.op != OpCode::GETGLOBAL && instr.op != OpCode::SETGLOBAL) continue;
        const Value& name = p->constants.at(static_cast<std::size_t>(instr.b));
        if (!name.isStr() || name.asStr() != builtin) continue;
        if (instr.op == OpCode::GETGLOBAL) {
          read = true;
        } else {
          assigned = true;
        }
      }
    }
    return read && !assigned;
  };

  // `memSize()` is bare metal only: its value comes from the multiboot map, and
  // no hosted prelude binds the handle.
  if (target.os == Os::Freestanding) {
    builtins["memSize"] = rt.memSizeBuiltin;
  } else if (unassignedRead("memSize")) {
    throw LoweringError(
        "lowering: memSize() is only available on the bare-metal target "
        "(x86_64-freestanding-sysv); it reads the multiboot memory map");
  }
  // `args()` needs the `(argc, argv)` the process entry hands `main`, which a
  // freestanding Windows image and a bare-metal boot do not provide.
  if (!hasArgv && unassignedRead("args")) {
    throw LoweringError(
        "lowering: args() needs a process argument vector, which this target "
        "does not provide (the freestanding Windows image has no command line "
        "and bare metal has none)");
  }
  // `input()` reads the keyboard through `entry.read`, which only the bare-metal
  // prelude binds.
  if (!hasInput && unassignedRead("input")) {
    throw LoweringError(
        "lowering: input() needs a keyboard, which only the bare-metal target "
        "(x86_64-freestanding-sysv) provides; a hosted image reads standard "
        "input instead");
  }
  // The file and directory builtins need the prelude's file-system handles,
  // which a bare-metal image has no file system for.
  if (!hasFiles) {
    for (const char* name : {"readFile", "writeFile", "fileExists", "fsOpen",
                             "fsRead", "fsWrite", "fsSeek", "fsClose",
                             "fsRemove", "fsRename", "fsMkdir", "fsRmdir",
                             "fsList", "fsStat", "fsCwd", "fsChdir"}) {
      if (!unassignedRead(name)) continue;
      throw LoweringError(std::string("lowering: ") + name +
                          "() needs a file system, which the bare-metal target "
                          "does not have");
    }
  }

  // The scalar `extern "C"` surface, hosted only (PHASE 7f.5): each symbol gets
  // a marshalling thunk the chunk seeds as a `Func` into its global slot, so a
  // `CALL` reaches the real C function through the ordinary `oskel_call` path.
  // The thunk's label is created here (before the body is emitted) and bound by
  // `emitAotRuntime` at the end, exactly like a builtin's code label. In the
  // freestanding mode the list stays empty: the image carries no external
  // reference and an `extern "C"` use keeps the build rejection.
  std::vector<AotExternThunk> externThunks;
  std::vector<AotCallbackSignature> callbackSignatures;
  std::unordered_set<std::string> resolvedExterns;
  if (mode == LinkMode::Hosted) {
    externThunks =
        collectExternThunks(cg, protos, resolvedExterns, callbackSignatures, target.abi);
    for (AotExternThunk& thunk : externThunks) thunk.label = cg.newLabel();
  }

  GlobalSlots globals = collectGlobals(protos, builtins, resolvedExterns);

  // The slots the chunk fills before the program runs: a builtin with a stub, a
  // resolved `extern "C"` thunk, and (below) the hidden runtime slots. A read of
  // one of these never observes `Unset`, so the unset check skips it.
  for (const auto& builtin : builtins) globals.seeded.insert(builtin.first);
  for (const std::string& name : resolvedExterns) globals.seeded.insert(name);
  for (const AotExternThunk& thunk : externThunks) globals.seeded.insert(thunk.symbol);
  // A host binding with no stub is never seeded, so a read of one stays a
  // candidate: `collectGlobals` already rejected the used ones, and a later read
  // of a *declared but unused* one still fails cleanly at run time rather than
  // reading a zeroed slot. Names seeded here are the only ones the chunk writes
  // before `main` runs; everything else is the program's own assignment.

  // `random()`/`seed()` keep their generator state in a hidden global (PHASE
  // 7c.5b.6): the image has no writable data segment, and the state is 2504
  // bytes, too large to thread through registers. `collectGlobals` pre-assigns a
  // builtin's slot only when the program reads it, so a name here means the
  // program actually draws. Reserving the slot only in that case keeps a program
  // that never calls `random`/`seed` byte for byte what it was.
  int randomStateSlot = -1;
  if (globals.slotOfName.count("random") != 0 || globals.slotOfName.count("seed") != 0) {
    randomStateSlot = globals.count++;
    globals.slotOfName[kAotRandomStateGlobal] = randomStateSlot;
  }

  // `pcall` needs the same treatment for a different reason: the error sink has
  // to find the innermost handler from a plain call, so the handler's address
  // lives in a hidden global the sink reads. `collectGlobals` pre-assigns
  // `pcall`'s slot only when the program reads it, so a name here means the
  // program catches errors. Reserving the slot only then keeps a program that
  // never calls `pcall` byte for byte what it was -- the sink carries no
  // handler check and the builtin body is not emitted.
  int pcallHandlerSlot = -1;
  if (globals.slotOfName.count("pcall") != 0) {
    pcallHandlerSlot = globals.count++;
    globals.slotOfName[kAotPcallHandlerGlobal] = pcallHandlerSlot;
  }

  // `clock()` subtracts the origin captured at entry (BACKLOG §6 item 25), so
  // the value is elapsed time. The slot exists only when the program reads
  // `clock`, so a program that does not keeps its image byte for byte.
  int clockOriginSlot = -1;
  if (globals.slotOfName.count("clock") != 0) {
    clockOriginSlot = globals.count++;
    globals.slotOfName[kAotClockOriginGlobal] = clockOriginSlot;
  }

  // `args()` reads the array the chunk builds at entry from the process
  // arguments. Same rule: the slot is reserved only when the builtin is read.
  int argsSlot = -1;
  if (globals.slotOfName.count("args") != 0) {
    argsSlot = globals.count++;
    globals.slotOfName[kAotArgsGlobal] = argsSlot;
  }

  // A callback crosses a C boundary, where the C caller expects r13/r14/r15 to
  // be preserved. Those are the Oskel arena cursor/end and global base, so the
  // callback body parks them in a shared state block. The slot exists only when
  // the program passes a callback, keeping every other image byte for byte.
  int callbackStateSlot = -1;
  if (!callbackSignatures.empty()) {
    callbackStateSlot = globals.count++;
    globals.slotOfName[kAotCallbackStateGlobal] = callbackStateSlot;
  }

  // The hidden runtime slots are seeded by the chunk entry too.
  for (const char* name : {kAotRandomStateGlobal, kAotPcallHandlerGlobal,
                           kAotClockOriginGlobal, kAotArgsGlobal,
                           kAotCallbackStateGlobal}) {
    if (globals.slotOfName.count(name) != 0) globals.seeded.insert(name);
  }

  // Every global the program assigns somewhere, minus the seeded ones: the
  // names a read can find unset. Computed across the whole program because a
  // name assigned only inside a never-called function is still unset at a
  // top-level read.
  std::unordered_set<std::string> unsetCandidates;
  for (const Bytecode* p : protos) {
    for (const Instruction& instr : p->code) {
      if (instr.op != OpCode::SETGLOBAL) continue;
      const Value& name = p->constants.at(static_cast<std::size_t>(instr.b));
      if (!name.isStr()) continue;
      if (globals.seeded.count(name.asStr()) != 0) continue;
      unsetCandidates.insert(name.asStr());
    }
  }
  // Set while emitting: whether any proto needs `oskel_global_get_checked`. A
  // program with no such read keeps the smaller runtime and its image byte for
  // byte.
  bool needsGlobalGetChecked = false;

  const std::uint64_t chunkOffset = cg.offset();

  // The root chunk: build the register file, map the arena and the global
  // table, then run the body. It is the only frame that saves rbx/r12-r15
  // (they carry the program's arena/global state down to every call) and the
  // only one that maps the arena.
  cg.bind(chunkEntry);
  const int rootRegisters = proto.numRegisters > 0 ? proto.numRegisters : 1;
  const int rootLocals = proto.numLocals > 0 ? proto.numLocals : 0;
  // `args()` needs the `(argc, argv)` the chunk was entered with, but the
  // allocation of the global table clobbers rdi/rsi before the builder can run.
  // Two scratch slots at the end of the local region hold them across that call;
  // a program that does not call `args` reserves neither.
  const int argSlots = argsSlot >= 0 ? 2 : 0;
  emitRootPrologue(cg, rootRegisters, rootLocals + argSlots);
  if (argsSlot >= 0) {
    cg.movToMem(kFileBase, localSlotOffset(rootRegisters, rootLocals), X64Reg::Rdi);
    cg.movToMem(kFileBase, localSlotOffset(rootRegisters, rootLocals + 1), X64Reg::Rsi);
  }
  cg.callLabel(entry.initArena);
  cg.movImm(X64Reg::Rdi, globals.count);
  cg.callLabel(rt.initGlobals);

  // `clock()` reads the monotonic counter and subtracts this origin, so what it
  // reports is elapsed time rather than an absolute counter (BACKLOG §6 item
  // 25). Captured here, before the program's first statement.
  if (clockOriginSlot >= 0) {
    cg.callLabel(entry.monotonic);
    cg.movImm(X64Reg::Rdi, clockOriginSlot);
    cg.movImm(X64Reg::Rsi, static_cast<std::int64_t>(AotTag::Int));
    cg.movReg(X64Reg::Rdx, X64Reg::Rax);
    cg.callLabel(rt.globalSet);
  }
  // `args()` reads the array built from the process arguments the prelude
  // handed `main`: `rdi` = argc, `rsi` = argv. `main` receives them from
  // `_start` (freestanding) or the C runtime (hosted), so the chunk reads its
  // own incoming registers before the first statement clobbers them.
  if (argsSlot >= 0) {
    cg.movFromMem(X64Reg::Rdi, kFileBase, localSlotOffset(rootRegisters, rootLocals));
    cg.movFromMem(X64Reg::Rsi, kFileBase, localSlotOffset(rootRegisters, rootLocals + 1));
    cg.movImm(X64Reg::Rdx, argsSlot);
    cg.callLabel(rt.buildArgs);
  }

  // Seed each builtin the program reads: build an `AotClosure` for its code and
  // store it into the global slot. A builtin has no captured cells, so the
  // closure is a plain code pointer with arity 0 (the runtime entry checks no
  // arity itself; `print`/`write`/`eprint` accept any count).
  std::vector<std::string> builtinOrder;
  builtinOrder.reserve(builtins.size());
  for (const auto& entry : builtins) builtinOrder.push_back(entry.first);
  std::sort(builtinOrder.begin(), builtinOrder.end());
  for (const std::string& name : builtinOrder) {
    auto slot = globals.slotOfName.find(name);
    if (slot == globals.slotOfName.end()) continue;
    const CodeGen::Label code = builtins.at(name);
    // A builtin is a native function in the hosted engine, so `repr` reads
    // `<native fn name>`; the display island carries exactly that text.
    StringConstant display{cg.newLabel(), "<native fn " + name + ">"};
    cg.leaRipLabel(X64Reg::Rdi, code);
    cg.movImm(X64Reg::Rsi, 0);  // arity
    cg.movImm(X64Reg::Rdx, 0);  // upvalueCount
    cg.leaRipLabel(X64Reg::Rcx, display.label);
    stringConstants.push_back(std::move(display));
    cg.callLabel(rt.closure);
    cg.movImm(X64Reg::Rdi, slot->second);
    cg.movReg(X64Reg::Rsi, X64Reg::Rax);
    // `oskel_closure` returns the Func value in rax:rdx, so rdx already holds the
    // payload that `oskel_global_set` expects.
    cg.callLabel(rt.globalSet);
  }
  // The scalar `extern "C"` thunks (PHASE 7f.5), seeded exactly like a builtin:
  // the thunk's code becomes an `AotClosure` in the symbol's global slot, so the
  // `SETGLOBAL` the declaration emits stores a real function over it and a call
  // dispatches through `oskel_call` into the marshalling body.
  for (const AotExternThunk& thunk : externThunks) {
    auto slot = globals.slotOfName.find(thunk.symbol);
    if (slot == globals.slotOfName.end()) continue;
    // An `extern "C"` binding is a native function too: the hosted engine
    // installs it through `makeNative`, so `repr` reads `<native fn symbol>`.
    StringConstant display{cg.newLabel(), "<native fn " + thunk.symbol + ">"};
    cg.leaRipLabel(X64Reg::Rdi, thunk.label);
    cg.movImm(X64Reg::Rsi, 0);  // arity: the thunk checks its own argc
    cg.movImm(X64Reg::Rdx, 0);  // upvalueCount
    cg.leaRipLabel(X64Reg::Rcx, display.label);
    stringConstants.push_back(std::move(display));
    cg.callLabel(rt.closure);
    cg.movImm(X64Reg::Rdi, slot->second);
    cg.movReg(X64Reg::Rsi, X64Reg::Rax);
    cg.callLabel(rt.globalSet);
  }
  {
    FrameInfo frame;
    frame.fileRegisters = rootRegisters;
    frame.fileLocals = rootLocals;
    frame.hasSelf = false;
    frame.callbackStateSlot = callbackStateSlot;
    frame.capturedSlots = capturedLocalSlots(proto);
    std::vector<CodeGen::Label> labels;
    labels.reserve(proto.code.size());
    for (std::size_t i = 0; i < proto.code.size(); ++i) labels.push_back(cg.newLabel());
    const std::unordered_set<std::size_t> checked =
        checkedGlobalReads(proto, globals, unsetCandidates);
    if (!checked.empty()) needsGlobalGetChecked = true;
    for (std::size_t i = 0; i < proto.code.size(); ++i) {
      cg.bind(labels[i]);
      lowerInstruction(i, proto.code[i], proto, cg, labels, rt, globals, frame,
                       protoEntry, displayOf, stringConstants, checked);
    }
  }

  // Every child proto: a function with the lowering's own convention
  // `AotValue entry(AotValue* args, long argc, AotClosure* self)`. It binds its
  // arguments from the caller's window into R0..R(n-1) and, past the last
  // statement, returns R0 (the compiler appends a `return nil`-style fallback,
  // so a body always ends in RETURN).
  for (const Bytecode* p : protos) {
    if (p == &proto) continue;
    const Bytecode& fn = *p;
    cg.bind(protoEntry[p]);
    const int fnRegisters = fn.numRegisters > 0 ? fn.numRegisters : 1;
    const int fnLocals = fn.numLocals > 0 ? fn.numLocals : 0;
    emitFunctionPrologue(cg, fnRegisters, fnLocals);

    // Save the closure this frame was entered with (`self`, in rdx) just below
    // the locals. `GETUPVAL`/`SETUPVAL` and a nested `CLOSURE` read the captured
    // cells through it. `self` is the callee's payload, so it is never null for
    // a well-formed call.
    cg.movToMem(kFileBase, selfSlotOffset(fnRegisters, fnLocals), X64Reg::Rdx);

    // The compiler does not check a user function's arity -- the hosted engine
    // does at run time -- so the prologue checks `argc` (rsi) against the
    // declared parameter count and reports the mismatch. A non-variadic
    // function needs an exact match; a variadic one accepts `numParams - 1` or
    // more (the last slot takes the packed extras, PHASE 7c.6).
    const int fixedParams = fn.numParams - (fn.variadic ? 1 : 0);
    {
      CodeGen::Label arityOk = cg.newLabel();
      if (fn.variadic) {
        cg.cmpImm32(X64Reg::Rsi, fixedParams);
        cg.jcc(Jcc::Ge, arityOk);
      } else {
        cg.cmpImm32(X64Reg::Rsi, fn.numParams);
        cg.jcc(Jcc::Eq, arityOk);
      }
      // The message names the function and its expected count, both known here;
      // only the actual count is dynamic, and the runtime helper appends it
      // (`, got N`) exactly as the hosted engine words it.
      const std::string expected = std::to_string(fixedParams) +
                                   (fixedParams == 1 ? " argument" : " arguments") +
                                   (fn.variadic ? " or more" : "");
      StringConstant message{cg.newLabel(), fn.name + "() expects " + expected};
      cg.movReg(X64Reg::Rdx, X64Reg::Rsi);   // argc, before rsi is reloaded
      cg.leaRipLabel(X64Reg::Rdi, message.label);
      // The constant's label points at its `AotString` header; the bytes start
      // after the 8-byte length.
      cg.addImm32(X64Reg::Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
      cg.movImm(X64Reg::Rsi, static_cast<std::int64_t>(message.bytes.size()));
      stringConstants.push_back(std::move(message));
      cg.callLabel(rt.arityFail);
      cg.bind(arityOk);
    }

    FrameInfo frame;
    frame.fileRegisters = fnRegisters;
    frame.fileLocals = fnLocals;
    frame.hasSelf = true;
    frame.capturedSlots = capturedLocalSlots(fn);

    // Copy the fixed arguments into the register file. `args` points at the
    // caller's R[a+1]; the file grows *down*, so consecutive registers sit 16
    // bytes *below* each other, and args[i] is at `args - 16*i`. Copying rather
    // than aliasing keeps the callee's writes out of the caller's registers.
    for (int i = 0; i < fixedParams; ++i) {
      const std::int64_t off = -16LL * i;
      cg.movFromMem(X64Reg::Rax, X64Reg::Rdi, off);
      cg.movFromMem(X64Reg::Rdx, X64Reg::Rdi, off + 8);
      storeValue(cg, i, X64Reg::Rax, X64Reg::Rdx);
    }

    // A variadic function packs its extras into an array and binds it to the
    // last parameter slot. `rdi`/`rsi` still hold `args`/`argc` (the copy loop
    // only read them), so the first extra is `args - 16 * fixedParams` and there
    // are `argc - fixedParams` of them.
    if (fn.variadic) {
      cg.subRegImm32(X64Reg::Rdi, static_cast<std::int32_t>(16 * fixedParams));
      cg.subRegImm32(X64Reg::Rsi, fixedParams);
      cg.callLabel(rt.packRest);
      storeValue(cg, fixedParams, X64Reg::Rax, X64Reg::Rdx);
    }

    std::vector<CodeGen::Label> labels;
    labels.reserve(fn.code.size());
    for (std::size_t i = 0; i < fn.code.size(); ++i) labels.push_back(cg.newLabel());
    const std::unordered_set<std::size_t> checked =
        checkedGlobalReads(fn, globals, unsetCandidates);
    if (!checked.empty()) needsGlobalGetChecked = true;
    for (std::size_t i = 0; i < fn.code.size(); ++i) {
      cg.bind(labels[i]);
      lowerInstruction(i, fn.code[i], fn, cg, labels, rt, globals, frame,
                       protoEntry, displayOf, stringConstants, checked);
    }
  }

  // `main`: run the chunk and return 0, so the process exit code is a clean
  // "success" and does not leak whatever value R0 happens to hold. `_start`
  // (emitted by `emitFreestandingRuntime`) calls this and exits with its
  // result. The frame keeps rsp 16-byte aligned at the `call` into the chunk,
  // which the System V ABI requires (a callee sees rsp % 16 == 8 at entry).
  cg.bind(mainEntry);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  // `rdi`/`rsi` are still the `(argc, argv)` the process entry handed this
  // function -- `_start` on Linux, the C runtime in the hosted mode -- and the
  // chunk reads them to build `args()`. `push rbp`/`mov rbp, rsp` touch neither,
  // so they reach the chunk intact. A Windows hosted `main` receives them in
  // the Microsoft x64 registers instead, so they are moved to the lowering's
  // own pair before the chunk sees them. On freestanding Windows and bare metal
  // the entry passes nothing and `args` is not seeded there.
  if (mode == LinkMode::Hosted && target.os == Os::Windows) {
    cg.movReg(X64Reg::Rdi, X64Reg::Rcx);
    cg.movReg(X64Reg::Rsi, X64Reg::Rdx);
  }
  cg.callLabel(chunkEntry);
  cg.xorReg(X64Reg::Rax, X64Reg::Rax);
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();

  // The entry prelude reaches the system, and that is the one place the
  // targets and the link mode differ. Freestanding Linux goes through syscalls,
  // Windows through kernel32 with the Microsoft x64 ABI, and bare metal through
  // the serial port and `hlt`; the hosted mode (PHASE 7f.4) instead exports the
  // program entry as the C `main` and reaches the system through libc, leaving
  // its undefined symbols for the object emitter and the linker. Everything
  // else -- the lowering and the embedded runtime -- is the same machine code
  // for every combination. The prelude binds the `entry` handles the runtime
  // already references (write/exit/init_arena).
  std::uint64_t startOffset = bareMetalEntry;
  if (mode == LinkMode::Hosted && target.os == Os::Windows) {
    startOffset = emitWindowsHostedRuntime(cg, entry, mainEntry);
  } else if (mode == LinkMode::Hosted) {
    startOffset = emitHostedRuntime(cg, entry, mainEntry, target.os == Os::MacOs);
  } else if (target.os == Os::Windows) {
    startOffset = emitWindowsRuntime(cg, entry, mainEntry);
  } else if (target.os == Os::Freestanding) {
    emitBareMetalRuntime(cg, entry);
  } else {
    startOffset = emitFreestandingRuntime(cg, entry, mainEntry);
  }
  AotRuntimeExtras extras;
  extras.clockOriginSlot = clockOriginSlot;
  extras.argsSlot = argsSlot;
  extras.hasArgv = hasArgv;
  extras.hasFiles = hasFiles;
  extras.callbackStateSlot = callbackStateSlot;
  extras.abi = target.abi;
  extras.platformName = osName(target.os);
  extras.archName = archName(target.arch);
  extras.needsGlobalGetChecked = needsGlobalGetChecked;
  emitAotRuntime(cg, rt, entry, externThunks.empty() ? nullptr : &externThunks,
                 mode == LinkMode::Hosted, hasInput,
                 target.os == Os::Freestanding, randomStateSlot, pcallHandlerSlot,
                 extras, &callbackSignatures);
  // String constants last: their bytes are addressed by `lea` from the code
  // above, and no path falls through into them.
  for (const Bytecode* child : protos) {
    if (child == &proto) continue;
    StringConstant display{displayOf.at(child), "<fn " + child->name + ">"};
    stringConstants.push_back(std::move(display));
  }
  emitStringConstants(cg, stringConstants);

  cg.finalize();

  LoweredChunk result;
  result.code = cg.code();
  result.programEntry = chunkOffset;
  result.processEntry = startOffset;
  // The hosted mode carries the references the linker must resolve; the
  // freestanding modes never create one, so both vectors stay empty and the
  // image is closed without a linker.
  result.externals = cg.externalRefs();
  result.exports = cg.exportedSymbols();
  return result;
}
