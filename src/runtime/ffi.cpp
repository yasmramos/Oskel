#include "ffi_marshal.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "ffi_trampoline.h"
#include "ffi_types.h"

// Marshalling across the C ABI (PHASE 6.6), without a host FFI library.
//
// A fixed scalar signature, or one whose structs fit in registers, is a
// compile-time thunk selected by its ABI shape. Three facts make that a small,
// closed set:
//
//   - Every integer, bool and pointer argument travels in a general-purpose
//     register; every `float`/`double` in an SSE register. So an argument only
//     needs its register class, not its exact width: the callee reads the
//     sub-register it declared, and the value in the full register matches.
//   - The result, by contrast, is read from `rax`/`xmm0` at the width the
//     declaration named. A C `int` leaves the upper half of `rax` undefined, so
//     the caller truncates to the declared width before boxing it. That is why
//     the return type needs the exact width and an argument does not.
//   - An `extern "C"` declaration is checked at compile time
//     (`ffi::validateSignature`), so a signature reaching the marshalling is
//     always one of the shapes below.
//
// A small struct (<= 16 bytes) is passed and returned in registers as up to two
// *eightbytes*, each classified INTEGER (a general-purpose register) or SSE (an
// xmm register). Rather than carry a distinct C++ type per struct shape, a
// struct argument is flattened into the same scalar alphabet the thunks already
// use: an INTEGER eightbyte is passed bit-for-bit as an `int64_t`, an SSE
// eightbyte as a `double`, so the callee sees exactly the bytes of the struct
// in the registers the ABI assigns. A struct return is captured through a fixed
// POD whose members land in `rax`/`rdx`/`xmm0`/`xmm1` exactly as the six class
// shapes (`I`, `S`, `II`, `SS`, `IS`, `SI`) require.
//
// Bounding a signature by eightbytes (`kMaxExternArgs` = 8) rather than
// parameters is deliberate: the template recursion below instantiates one
// sequence per combination of `int64_t`/`double`/`float`, so the bound is what
// keeps it at a few thousand instead of exploding. It is not an ABI limit: the
// typed function-pointer call lets the C++ compiler place the seventh and
// eighth integer arguments on the stack, and the AOT backend emits the same
// spill.
//
// The argument list is walked with a compile-time depth (`Index`), not a
// runtime count: the recursion stops at `kMaxExternArgs`, so the compiler
// instantiates a bounded set of thunks instead of recursing on a runtime
// condition (which would instantiate forever).
//
// Three shapes do not fit a compile-time thunk: a C variadic (the tail length
// is only known at the call site), a MEMORY struct (outgoing stack area and a
// hidden `sret` pointer), and a callback (C has to re-enter the VM). They go
// through the small x86-64 descriptor and closure stubs in
// `ffi_trampoline.cpp`, which reuse the AOT backend's `CodeGen`. No host FFI
// library is involved on either path.
namespace ffi {
namespace {

// ------------------------------------------------------------------ arguments

// The machine address behind a pointer argument. A `void *` carries an opaque
// token; an owned `cstring`/buffer exposes the address of its bytes, which is
// what a C callee dereferences. A Oskel string is NUL-terminated and stays alive
// for the whole call, so its bytes are a valid `const char *`.
std::int64_t pointerAddress(Runtime& rt, const Value& value, const std::string& typeName) {
  if (value.isNil()) return 0;  // NULL
  if (value.isStr()) return reinterpret_cast<std::int64_t>(value.asStr().c_str());
  if (!value.isPtr()) {
    rt.fail("argument of type '" + std::string(value.typeName()) + "' cannot be passed as '" +
            typeName + "'");
  }
  PtrObj& pointer = value.ptr();
  if (pointer.kind == PtrKind::Void) return pointer.address;
  return reinterpret_cast<std::int64_t>(pointer.bytes.data());
}

// A general-purpose-register argument: an integer, a bool or a pointer. All of
// them travel in the same register; the declaration decides how the Oskel value
// is turned into the 64-bit word.
std::int64_t marshalGpr(Runtime& rt, const Value& value, ParamClass cls,
                        const std::string& typeName) {
  if (cls == ParamClass::Pointer) return pointerAddress(rt, value, typeName);
  if (value.isBool()) return value.asBool() ? 1 : 0;
  if (value.isInt()) return value.asInt();
  if (value.isNumber()) return static_cast<std::int64_t>(value.number());
  rt.fail("argument of type '" + std::string(value.typeName()) + "' cannot be passed as '" +
          typeName + "'");
}

double marshalDouble(Runtime& rt, const Value& value, const std::string& typeName) {
  if (value.isNumber()) return value.number();
  rt.fail("argument of type '" + std::string(value.typeName()) + "' cannot be passed as '" +
          typeName + "'");
}

float marshalFloat(Runtime& rt, const Value& value, const std::string& typeName) {
  return static_cast<float>(marshalDouble(rt, value, typeName));
}

// An SSE argument travels in an xmm register: a `double` fills it, a `float`
// occupies its low 32 bits. Returning the float's bit pattern zero-extended into
// a `double` slot is therefore what a callee expecting `float` reads, and it lets
// the dispatch walk carry one branch for the whole SSE class instead of one per
// floating-point width (3^N thunks become 2^N).
double marshalSseBits(Runtime& rt, const Value& value, const std::string& typeName) {
  if (typeName != "float") return marshalDouble(rt, value, typeName);
  const float f = marshalFloat(rt, value, typeName);
  std::uint32_t bits = 0;
  std::memcpy(&bits, &f, sizeof(bits));
  const std::uint64_t wide = bits;
  double slot = 0;
  std::memcpy(&slot, &wide, sizeof(slot));
  return slot;
}

// -------------------------------------------------------------------- results

// Truncates the raw `rax` to the width the declaration named, so a C `int` is
// read as 32 bits and not as the garbage the upper half of `rax` may hold.
Value wrapInteger(Runtime& rt, const std::string& typeName, std::int64_t raw) {
  if (typeName.empty() || typeName == "any" || typeName == "int" || typeName == "int32") {
    return Value(static_cast<std::int64_t>(static_cast<std::int32_t>(raw)));
  }
  if (typeName == "bool") return Value((raw & 0xff) != 0);
  if (typeName == "int8") return Value(static_cast<std::int64_t>(static_cast<std::int8_t>(raw)));
  if (typeName == "int16") return Value(static_cast<std::int64_t>(static_cast<std::int16_t>(raw)));
  if (typeName == "int64") return Value(raw);
  if (typeName == "uint8") return Value(static_cast<std::int64_t>(static_cast<std::uint8_t>(raw)));
  if (typeName == "uint16") return Value(static_cast<std::int64_t>(static_cast<std::uint16_t>(raw)));
  if (typeName == "uint" || typeName == "uint32") {
    return Value(static_cast<std::int64_t>(static_cast<std::uint32_t>(raw)));
  }
  if (typeName == "uint64" || typeName == "size_t") {
    return Value(static_cast<std::int64_t>(static_cast<std::uint64_t>(raw)));
  }
  // `validateSignature` rejects anything else before a call can reach here.
  rt.fail("unsupported C return type '" + typeName + "'");
}

// A `cstring` a C function returns is a `const char *`, and the C contract is
// that it is NUL-terminated. A host that breaks that contract would make an
// unbounded `strlen` walk off the end of the buffer, so the scan is capped: past
// the cap the read stops with a clear runtime error instead of an unbounded
// walk. The cap is generous (a C string that large is almost always a bug).
constexpr std::size_t kMaxCStringBytes = 1u << 24;  // 16 MiB

Value wrapPointerValue(Runtime& rt, const std::string& typeName, std::int64_t raw) {
  if (raw == 0) return Value();  // NULL
  if (typeName == "ptr") return makePtr(PtrKind::Void, {}, raw);
  const char* text = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(raw));
  std::size_t length = 0;
  while (length < kMaxCStringBytes && text[length] != '\0') ++length;
  if (length == kMaxCStringBytes) {
    rt.fail("a C function returned a 'cstring' longer than " + std::to_string(kMaxCStringBytes) +
            " bytes with no NUL terminator; the host broke the `const char *` contract");
  }
  std::vector<char> bytes(text, text + length);
  bytes.push_back('\0');
  return makePtr(PtrKind::CString, std::move(bytes));
}

// -------------------------------------------------------------------- structs

// Writes one scalar field of a struct argument into `bytes` at `offset`, in the
// C representation of its declared type, using exactly the field's width (the
// target is little-endian, so a narrower store is the low-order bytes).
void writeStructField(std::vector<char>& bytes, std::size_t offset, const std::string& type,
                      const Value& value, Runtime& rt) {
  if (type == "float") {
    const float f = marshalFloat(rt, value, type);
    std::memcpy(bytes.data() + offset, &f, sizeof(f));
    return;
  }
  if (type == "double") {
    const double d = marshalDouble(rt, value, type);
    std::memcpy(bytes.data() + offset, &d, sizeof(d));
    return;
  }
  std::size_t size = 8;
  std::size_t alignment = 8;
  ParamClass cls = ParamClass::Int;
  scalarFieldLayout(type, size, alignment, cls);
  std::int64_t raw = (cls == ParamClass::Pointer) ? pointerAddress(rt, value, type)
                                                  : marshalGpr(rt, value, cls, type);
  std::memcpy(bytes.data() + offset, &raw, size);
}

// Serialises a Oskel struct value (a map) into the C bytes of its layout. The
// buffer is sized to the layout and zero-filled, so the padding a C compiler
// leaves between fields is deterministic.
void writeStructBytes(Runtime& rt, const StructLayout& layout, const Value& value,
                      const std::string& name, std::size_t argIndex, std::vector<char>& bytes) {
  if (!value.isMap()) {
    rt.fail("argument " + std::to_string(argIndex + 1) + " of '" + name + "' must be a struct value");
  }
  bytes.assign(layout.size, 0);
  for (std::size_t k = 0; k < layout.fields.size(); ++k) {
    const Value* field = value.map().get(layout.fields[k].name);
    if (field == nullptr) {
      rt.fail("argument " + std::to_string(argIndex + 1) + " of '" + name + "' is missing field '" +
              layout.fields[k].name + "'");
    }
    writeStructField(bytes, layout.offsets[k], layout.fields[k].type, *field, rt);
  }
}

// Reads one scalar field of a returned struct from `bytes` at `offset` and boxes
// it as a Oskel value, reading only the field's declared width.
Value readStructField(Runtime& rt, const char* bytes, std::size_t offset,
                      const std::string& type) {
  if (type == "float") {
    float f = 0;
    std::memcpy(&f, bytes + offset, sizeof(f));
    return Value::fromFloat(f);
  }
  if (type == "double") {
    double d = 0;
    std::memcpy(&d, bytes + offset, sizeof(d));
    return Value(d);
  }
  std::size_t size = 8;
  std::size_t alignment = 8;
  ParamClass cls = ParamClass::Int;
  scalarFieldLayout(type, size, alignment, cls);
  std::int64_t raw = 0;
  std::memcpy(&raw, bytes + offset, size);
  if (type == "ptr" || isFfiCStringTypeName(type)) return wrapPointerValue(rt, type, raw);
  return wrapInteger(rt, type.empty() ? "int" : type, raw);
}

// The POD types a returned struct is captured through. Each matches one System V
// class shape: the two eightbytes come back in `rax`/`rdx` (INTEGER) and/or
// `xmm0`/`xmm1` (SSE), and a trivially copyable aggregate of these members is
// returned exactly there, so the C++ compiler does the register choreography.
struct PodI { std::int64_t a; };
struct PodS { double a; };
struct PodII { std::int64_t a; std::int64_t b; };
struct PodSS { double a; double b; };
struct PodIS { std::int64_t a; double b; };
struct PodSI { double a; std::int64_t b; };

// Rebuilds the Oskel map of a returned struct from its raw bytes.
Value boxStructResult(Runtime& rt, const StructLayout& layout, const char* bytes) {
  MapPtr result = makeMap();
  for (std::size_t k = 0; k < layout.fields.size(); ++k) {
    result->set(layout.fields[k].name,
                readStructField(rt, bytes, layout.offsets[k], layout.fields[k].type));
  }
  return Value(result);
}

// Runs the returned-struct case for a chosen POD shape, then boxes the bytes.
template <typename Pod, typename... Built>
Value invokeStruct(void* address, const StructLayout& layout, Runtime& rt, Built... built) {
  Pod pod = reinterpret_cast<Pod (*)(Built...)>(address)(built...);
  char bytes[16] = {0};
  std::memcpy(bytes, &pod, sizeof(Pod));
  return boxStructResult(rt, layout, bytes);
}

// Invokes the C function with the fully marshalled argument list. The return
// class selects the register and the width: `void` reads nothing, the integer
// family and pointers read `rax`, `float`/`double` read `xmm0`, and a struct
// reads the registers its shape assigns.
template <typename... Built>
Value invoke(void* address, ReturnClass rc, StructShape shape, const StructLayout& layout,
             const std::string& returnType, Runtime& rt, Built... built) {
  switch (rc) {
    case ReturnClass::Void:
      reinterpret_cast<void (*)(Built...)>(address)(built...);
      return Value();
    case ReturnClass::Float:
      return Value::fromFloat(reinterpret_cast<float (*)(Built...)>(address)(built...));
    case ReturnClass::Double:
      return Value(reinterpret_cast<double (*)(Built...)>(address)(built...));
    case ReturnClass::Ptr:
    case ReturnClass::CString: {
      auto result = reinterpret_cast<std::int64_t (*)(Built...)>(address)(built...);
      return wrapPointerValue(rt, returnType, result);
    }
    case ReturnClass::Struct:
      switch (shape) {
        case StructShape::I1: return invokeStruct<PodI>(address, layout, rt, built...);
        case StructShape::S1: return invokeStruct<PodS>(address, layout, rt, built...);
        case StructShape::II: return invokeStruct<PodII>(address, layout, rt, built...);
        case StructShape::SS: return invokeStruct<PodSS>(address, layout, rt, built...);
        case StructShape::IS: return invokeStruct<PodIS>(address, layout, rt, built...);
        case StructShape::SI: return invokeStruct<PodSI>(address, layout, rt, built...);
        case StructShape::None: break;
      }
      rt.fail("internal error: a struct return has no ABI shape");
    default: {  // Int / Bool: read `rax` and truncate to the declared width
      auto result = reinterpret_cast<std::int64_t (*)(Built...)>(address)(built...);
      return wrapInteger(rt, returnType, result);
    }
  }
}

// Walks the flattened eightbyte argument list, marshalling one per level, then
// calls `invoke` with the argument list typed exactly as the declaration named
// it. `Index` is a template parameter so the recursion is bounded by
// `kMaxExternArgs` and the compiler stops instantiating; the runtime check
// `Index >= classes.size()` ends the walk for a shorter signature.
template <std::size_t Index, typename... Built>
Value dispatch(void* address, ReturnClass rc, StructShape shape, const StructLayout& layout,
               const std::string& returnType, const std::vector<ParamClass>& classes,
               const std::vector<std::string>& types, const std::vector<Value>& values, Runtime& rt,
               Built... built) {
  if constexpr (Index < kMaxExternArgs) {
    if (Index >= classes.size()) {
      return invoke(address, rc, shape, layout, returnType, rt, built...);
    }
    switch (classes[Index]) {
      case ParamClass::Int:
      case ParamClass::Pointer:
        return dispatch<Index + 1>(address, rc, shape, layout, returnType, classes, types, values,
                                   rt, built...,
                                   marshalGpr(rt, values[Index], classes[Index], types[Index]));
      case ParamClass::Float:
      case ParamClass::Double:
        return dispatch<Index + 1>(address, rc, shape, layout, returnType, classes, types, values,
                                   rt, built...,
                                   marshalSseBits(rt, values[Index], types[Index]));
      case ParamClass::Callback:
        // A callback is marshalled to an `int64` trampoline address before the
        // dispatch walk, so it never reaches here; fail rather than fall
        // through silently if that ever changes.
        break;
    }
    rt.fail("unsupported C parameter class");
  } else {
    return invoke(address, rc, shape, layout, returnType, rt, built...);
  }
}

#if OSKEL_FFI_TRAMPOLINE
// ------------------------------------------------------------------ callbacks

// Where one callback argument lives in the buffer the closure stub fills: a
// GPR slot, an SSE slot, or the stack area after the 112-byte register block.
struct CallbackArgSlot {
  std::size_t offset = 0;
};

// The state behind one Oskel function passed to C as a callback. The closure
// stub embeds a pointer to this object; the object owns the executable page,
// so the C function pointer stays valid for the process lifetime. C may store
// the pointer and invoke it after the extern call returns (an `atexit`
// handler), so the engine retains the object instead of releasing it with the
// call (BACKLOG §6 item 27).
struct CallbackObj : RefCounted {
  Runtime* runtime = nullptr;
  Value function;
  std::string name;
  std::vector<std::string> paramTypes;
  std::string returnType;  // "void" for no result
  bool returnsStruct = false;
  bool memoryReturn = false;
  StructLayout returnLayout;
  StructShape returnShape = StructShape::None;
  std::vector<CallbackArgSlot> argSlots;
  std::uint32_t gprCount = 0;
  std::uint32_t sseCount = 0;
  std::uint32_t stackBytes = 0;
  ExecutableBuffer code;
  // A RuntimeError raised inside the Oskel callback cannot unwind through the C
  // frame that called the stub, so the body stores it here and the extern call
  // re-raises it once C returns.
  bool hasPendingError = false;
  std::string pendingMessage;
  int pendingLine = 0;
  int pendingCol = 0;
};

// Reads one callback argument from the buffer the closure stub filled and boxes
// it as a Oskel value, using the declared width so a narrow C `int` is not read
// as 64 bits.
Value readCallbackArg(Runtime& rt, const std::string& typeName, const std::uint8_t* slot) {
  if (typeName == "float") {
    float f = 0;
    std::memcpy(&f, slot, sizeof(f));
    return Value::fromFloat(f);
  }
  if (typeName == "double") {
    double d = 0;
    std::memcpy(&d, slot, sizeof(d));
    return Value(d);
  }
  if (typeName == "ptr" || isFfiCStringTypeName(typeName)) {
    void* pointer = nullptr;
    std::memcpy(&pointer, slot, sizeof(pointer));
    return wrapPointerValue(rt, typeName, reinterpret_cast<std::int64_t>(pointer));
  }
  std::int64_t raw = 0;
  std::size_t size = 8;
  std::size_t alignment = 8;
  ParamClass cls = ParamClass::Int;
  scalarFieldLayout(typeName, size, alignment, cls);
  std::memcpy(&raw, slot, size);
  return wrapInteger(rt, typeName, raw);
}

// Writes the Oskel callback's result into the raw return storage in the declared
// C type. `ret` is the 32-byte register buffer; `sret` is the hidden pointer a
// class-MEMORY struct return writes through. `void` writes nothing; the error
// path writes zero of the declared width so C never observes an uninitialised
// return.
void writeCallbackResult(const CallbackObj& callback, const Value& value, std::uint8_t* ret,
                         std::uint8_t* sret) {
  const std::string& typeName = callback.returnType;
  if (callback.returnsStruct) {
    if (!value.isMap()) {
      callback.runtime->fail("a callback returning a struct must return a struct value");
    }
    std::vector<char> bytes(callback.returnLayout.size, 0);
    for (std::size_t k = 0; k < callback.returnLayout.fields.size(); ++k) {
      const Value* field = value.map().get(callback.returnLayout.fields[k].name);
      if (field == nullptr) {
        callback.runtime->fail("the struct returned by a callback is missing field '" +
                               callback.returnLayout.fields[k].name + "'");
      }
      writeStructField(bytes, callback.returnLayout.offsets[k],
                       callback.returnLayout.fields[k].type, *field, *callback.runtime);
    }
    std::uint8_t* dest = callback.memoryReturn ? sret : ret;
    if (dest != nullptr) std::memcpy(dest, bytes.data(), bytes.size());
    return;
  }
  if (typeName.empty() || typeName == "void") return;
  if (typeName == "float") {
    float f = value.isNumber() ? static_cast<float>(value.number()) : 0.0f;
    std::memcpy(ret, &f, sizeof(f));
    return;
  }
  if (typeName == "double") {
    double d = value.isNumber() ? value.number() : 0.0;
    std::memcpy(ret, &d, sizeof(d));
    return;
  }
  if (typeName == "ptr" || isFfiCStringTypeName(typeName)) {
    std::int64_t raw = 0;
    if (value.isStr()) {
      raw = reinterpret_cast<std::int64_t>(value.asStr().c_str());
    } else if (value.isPtr()) {
      const PtrObj& pointer = value.ptr();
      raw = pointer.kind == PtrKind::Void
                ? pointer.address
                : reinterpret_cast<std::int64_t>(pointer.bytes.data());
    }
    std::memcpy(ret, &raw, sizeof(raw));
    return;
  }
  std::int64_t raw = 0;
  if (value.isBool()) {
    raw = value.asBool() ? 1 : 0;
  } else if (value.isInt()) {
    raw = value.asInt();
  } else if (value.isNumber()) {
    raw = static_cast<std::int64_t>(value.number());
  }
  std::size_t size = 8;
  std::size_t alignment = 8;
  ParamClass cls = ParamClass::Int;
  scalarFieldLayout(typeName, size, alignment, cls);
  std::memcpy(ret, &raw, size);
}

// The C entry point every closure stub calls. It converts the C arguments to
// Oskel values, re-enters the engine through `Runtime::call`, and writes the
// result back. It must never let a C++ exception cross the C frame (the caller
// is C code such as `qsort`), so a Oskel error is captured and re-raised by the
// extern call once C returns.
extern "C" void oskelVmCallbackBody(void* context, std::uint8_t* args, std::uint8_t* ret,
                                    std::uint8_t* sret) noexcept {
  CallbackObj* callback = static_cast<CallbackObj*>(context);
  std::memset(ret, 0, 32);
  if (callback->memoryReturn && sret != nullptr) {
    std::memset(sret, 0, callback->returnLayout.size);
  }
  if (callback->hasPendingError) {
    // The extern call already recorded a failure and will re-raise it once C
    // returns. C may invoke the callback again before that, so short-circuit:
    // re-entering the VM after an error is not meaningful. Both buffers are
    // already zeroed above.
    return;
  }
  std::vector<Value> oskelArgs;
  oskelArgs.reserve(callback->paramTypes.size());
  for (std::size_t i = 0; i < callback->paramTypes.size(); ++i) {
    oskelArgs.push_back(
        readCallbackArg(*callback->runtime, callback->paramTypes[i], args + callback->argSlots[i].offset));
  }
  Value result;
  try {
    result = callback->runtime->call(callback->function, std::move(oskelArgs),
                                     callback->runtime->currentLine, callback->runtime->currentCol);
  } catch (const RuntimeError& error) {
    callback->hasPendingError = true;
    callback->pendingMessage = error.message;
    callback->pendingLine = error.line;
    callback->pendingCol = error.col;
    return;
  } catch (const std::exception& error) {
    callback->hasPendingError = true;
    callback->pendingMessage = std::string("the callback raised: ") + error.what();
    return;
  } catch (...) {
    callback->hasPendingError = true;
    callback->pendingMessage = "the callback raised an unknown error";
    return;
  }
  try {
    writeCallbackResult(*callback, result, ret, sret);
  } catch (const RuntimeError& error) {
    callback->hasPendingError = true;
    callback->pendingMessage = error.message;
    callback->pendingLine = error.line;
    callback->pendingCol = error.col;
  }
}

StubReturn stubReturnFor(const CallbackObj& callback) {
  if (callback.returnsStruct) {
    if (callback.memoryReturn) return StubReturn::Memory;
    switch (callback.returnShape) {
      case StructShape::I1: return StubReturn::I1;
      case StructShape::S1: return StubReturn::S1;
      case StructShape::II: return StubReturn::II;
      case StructShape::SS: return StubReturn::SS;
      case StructShape::IS: return StubReturn::IS;
      case StructShape::SI: return StubReturn::SI;
      case StructShape::None: break;
    }
    return StubReturn::Void;
  }
  if (callback.returnType.empty() || callback.returnType == "void") return StubReturn::Void;
  if (callback.returnType == "float") return StubReturn::Float;
  if (callback.returnType == "double") return StubReturn::Double;
  return StubReturn::Int;
}

// Builds the closure for one Oskel function bound to a callback parameter.
Ref<CallbackObj> makeCallback(Runtime& rt, const std::string& name, const TypeSpec& spec,
                              const Value& function) {
  if (!kCallbacksSupported) {
    rt.fail("a callback cannot cross the C boundary on this target");
  }
  if (!function.isFunc()) {
    rt.fail("a callback argument to '" + name + "' must be a function, got '" +
            std::string(function.typeName()) + "'");
  }
  auto callback = makeRef<CallbackObj>();
  callback->runtime = &rt;
  callback->function = function;
  callback->name = name;
  callback->paramTypes = spec.callbackParams;
  callback->returnType = spec.callbackReturn.empty() ? "void" : spec.callbackReturn;
  callback->returnsStruct = spec.callbackReturnsStruct;
  if (callback->returnsStruct) {
    std::string reason;
    if (!buildStructLayout(spec.callbackReturnFields, callback->returnLayout, reason)) {
      rt.fail(reason);
    }
    callback->memoryReturn = callback->returnLayout.isMemory;
    if (!callback->memoryReturn) {
      callback->returnShape = structShapeOf(callback->returnLayout);
    }
  }

  // The System V argument assignment the closure stub and the body share. A
  // MEMORY struct return consumes rdi as the hidden sret pointer, so the first
  // declared parameter starts in rsi.
  std::size_t gpr = callback->memoryReturn ? 1 : 0;
  std::size_t sse = 0;
  std::size_t stack = 0;
  for (const std::string& type : callback->paramTypes) {
    ParamClass cls = ParamClass::Int;
    classifyParam(type, cls);
    CallbackArgSlot slot;
    const bool floating = cls == ParamClass::Float || cls == ParamClass::Double;
    if (floating) {
      if (sse < 8) {
        slot.offset = 48 + 8 * sse;
        ++sse;
      } else {
        slot.offset = 112 + stack;
        stack += 8;
      }
    } else {
      if (gpr < 6) {
        slot.offset = 8 * gpr;
        ++gpr;
      } else {
        slot.offset = 112 + stack;
        stack += 8;
      }
    }
    callback->argSlots.push_back(slot);
  }
  callback->gprCount = static_cast<std::uint32_t>(gpr);
  callback->sseCount = static_cast<std::uint32_t>(sse);
  callback->stackBytes = static_cast<std::uint32_t>(stack);

  CallbackStubLayout layout;
  layout.win64 = false;
  layout.gprCount = callback->gprCount;
  layout.sseCount = callback->sseCount;
  layout.stackBytes = callback->stackBytes;
  layout.argsBytes = 112 + callback->stackBytes;
  layout.frameBytes = ((layout.argsBytes + 32 + 15) / 16) * 16;
  layout.ret = stubReturnFor(*callback);
  layout.memoryReturn = callback->memoryReturn;
  layout.context = callback.get();
  layout.body = reinterpret_cast<const void*>(&oskelVmCallbackBody);

  std::vector<std::uint8_t> bytes;
  if (!emitCallbackStub(layout, bytes) || !callback->code.assign(bytes)) {
    rt.fail("cannot build a callback for '" + name + "'");
  }
  return callback;
}

// ------------------------------------------------------------------ dynamic calls

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
constexpr bool kHostWin64 = true;
#else
constexpr bool kHostWin64 = false;
#endif

struct PromotedArg {
  ParamClass cls = ParamClass::Int;
  std::uint64_t raw = 0;
};

// Applies the C default argument promotions to one trailing variadic value.
PromotedArg promoteVariadic(Runtime& rt, const std::string& name, std::size_t index,
                            const Value& value) {
  if (!(value.isInt() || value.isBool() || value.isFloat() || value.isDouble() ||
        value.isStr() || value.isPtr() || value.isNil())) {
    rt.fail("argument " + std::to_string(index + 1) + " of '" + name + "' has type '" +
            value.typeName() + "', which cannot cross the C variadic boundary");
  }
  if (value.isFloat() || value.isDouble()) {
    double d = value.number();
    std::uint64_t raw = 0;
    std::memcpy(&raw, &d, sizeof(d));
    return {ParamClass::Double, raw};
  }
  if (value.isStr() || value.isPtr() || value.isNil()) {
    const std::int64_t raw = pointerAddress(rt, value, "ptr");
    return {ParamClass::Int, static_cast<std::uint64_t>(raw)};
  }
  if (value.isBool()) {
    return {ParamClass::Int, value.asBool() ? 1u : 0u};
  }
  return {ParamClass::Int, static_cast<std::uint64_t>(value.asInt())};
}

// Reads one scalar argument as the raw 64-bit word the descriptor carries. A
// `float` occupies the low 32 bits of its slot, exactly as the xmm register
// holds it; a `double` fills the slot.
std::uint64_t scalarRaw(Runtime& rt, const Value& value, const std::string& type, ParamClass cls) {
  if (cls == ParamClass::Float) {
    const float f = marshalFloat(rt, value, type);
    std::uint64_t raw = 0;
    std::memcpy(&raw, &f, sizeof(f));
    return raw;
  }
  if (cls == ParamClass::Double) {
    const double d = marshalDouble(rt, value, type);
    std::uint64_t raw = 0;
    std::memcpy(&raw, &d, sizeof(d));
    return raw;
  }
  return static_cast<std::uint64_t>(marshalGpr(rt, value, cls, type));
}

// Calls a signature the compile-time thunks cannot express: a C variadic, a
// class-MEMORY struct, or a call that carries a callback beside one of those.
// The descriptor tells the cached stub which registers and stack slots to fill;
// the stub does the call and stores the raw return registers.
Value callDynamic(Runtime& rt, const std::string& name, void* address,
                  const std::vector<TypeSpec>& paramTypes, const TypeSpec& returnType,
                  bool variadic, std::vector<Value>& args) {
  const std::size_t fixedCount = paramTypes.size();
  if (variadic) {
    if (args.size() < fixedCount) {
      rt.fail("'" + name + "' expects at least " + std::to_string(fixedCount) + " argument" +
              (fixedCount == 1 ? "" : "s") + ", got " + std::to_string(args.size()));
    }
  } else if (args.size() != fixedCount) {
    rt.fail("'" + name + "' expects " + std::to_string(fixedCount) + " argument" +
            (fixedCount == 1 ? "" : "s") + ", got " + std::to_string(args.size()));
  }

  StructLayout retLayout;
  if (returnType.isStruct) {
    std::string reason;
    if (!buildStructLayout(returnType.fields, retLayout, reason)) rt.fail(reason);
  }

  CallDescriptor descriptor;
  descriptor.function = address;
  std::uint8_t ret[32] = {};
  descriptor.ret = ret;
  std::vector<std::uint8_t> stack;
  std::vector<std::uint8_t> sret;
  std::vector<Ref<CallbackObj>> callbacks;

  if (returnType.isStruct && retLayout.isMemory) {
    if (kHostWin64) {
      rt.fail("a struct of class MEMORY cannot cross the C boundary on this target");
    }
    sret.assign(retLayout.size, 0);
    descriptor.gpr[0] = reinterpret_cast<std::uint64_t>(sret.data());
    descriptor.gprCount = 1;
  }

  std::size_t gpr = descriptor.gprCount;
  std::size_t sse = 0;
  std::size_t stackBytes = 0;
  auto pushBytes = [&](const std::uint8_t* bytes, std::size_t size) {
    stackBytes = (stackBytes + 7) & ~std::size_t(7);
    stack.resize(stackBytes + size, 0);
    std::memcpy(stack.data() + stackBytes, bytes, size);
    stackBytes += size;
  };
  auto pushRaw = [&](std::uint64_t raw) {
    stackBytes = (stackBytes + 7) & ~std::size_t(7);
    stack.resize(stackBytes + 8, 0);
    std::memcpy(stack.data() + stackBytes, &raw, sizeof(raw));
    stackBytes += 8;
  };
  auto assign = [&](std::size_t position, ParamClass cls, std::uint64_t raw,
                    bool duplicateFloat) {
    const bool floating = cls == ParamClass::Float || cls == ParamClass::Double;
    if (kHostWin64) {
      if (position < 4) {
        if (floating) {
          descriptor.sse[position] = raw;
          if (duplicateFloat) descriptor.gpr[position] = raw;
        } else {
          descriptor.gpr[position] = raw;
        }
      } else {
        pushRaw(raw);
      }
      return;
    }
    if (floating) {
      if (sse < 8) {
        descriptor.sse[sse++] = raw;
      } else {
        pushRaw(raw);
      }
    } else {
      if (gpr < 6) {
        descriptor.gpr[gpr++] = raw;
      } else {
        pushRaw(raw);
      }
    }
  };

  for (std::size_t i = 0; i < fixedCount; ++i) {
    const TypeSpec& spec = paramTypes[i];
    if (spec.isCallback) {
      std::int64_t code = 0;
      if (!args[i].isNil()) {
        if (!args[i].isFunc()) {
          rt.fail("argument " + std::to_string(i + 1) + " of '" + name +
                  "' must be a function or nil, got '" + args[i].typeName() + "'");
        }
        auto callback = makeCallback(rt, name, spec, args[i]);
        code = reinterpret_cast<std::int64_t>(callback->code.entry());
        callbacks.push_back(std::move(callback));
      }
      assign(i, ParamClass::Int, static_cast<std::uint64_t>(code), false);
      continue;
    }
    if (spec.isStruct) {
      StructLayout layout;
      std::string reason;
      if (!buildStructLayout(spec.fields, layout, reason)) rt.fail(reason);
      std::vector<char> bytes;
      writeStructBytes(rt, layout, args[i], name, i, bytes);
      if (layout.isMemory) {
        pushBytes(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        continue;
      }
      if (kHostWin64) {
        rt.fail("a struct cannot cross the C boundary by value on this target");
      }
      bytes.resize(static_cast<std::size_t>(layout.eightbyteCount) * 8, 0);
      for (int e = 0; e < layout.eightbyteCount; ++e) {
        std::uint64_t raw = 0;
        std::memcpy(&raw, bytes.data() + 8 * e, sizeof(raw));
        assign(i, layout.eightbyte[e] == 'S' ? ParamClass::Double : ParamClass::Int, raw, false);
      }
      continue;
    }
    ParamClass cls = ParamClass::Int;
    classifyParam(spec.scalarType, cls);
    const std::uint64_t raw = scalarRaw(rt, args[i], spec.scalarType, cls);
    assign(i, cls, raw, variadic && (cls == ParamClass::Float || cls == ParamClass::Double));
  }

  for (std::size_t i = fixedCount; i < args.size(); ++i) {
    const PromotedArg promoted = promoteVariadic(rt, name, i, args[i]);
    assign(i, promoted.cls, promoted.raw, true);
  }

  descriptor.stackBytes = static_cast<std::uint32_t>(stack.size());
  descriptor.stack = stack.empty() ? nullptr : stack.data();
  if (kHostWin64) {
    descriptor.gprCount = 4;
    descriptor.sseCount = 4;
    descriptor.vectorCount = 0;
  } else {
    descriptor.gprCount = static_cast<std::uint32_t>(gpr);
    descriptor.sseCount = static_cast<std::uint32_t>(sse);
    descriptor.vectorCount = static_cast<std::uint32_t>(sse);
  }

  CallStub stub = callStub(kHostWin64);
  if (stub == nullptr) {
    rt.fail("cannot allocate the extern \"C\" call trampoline");
  }
  stub(&descriptor);
  for (Ref<CallbackObj>& callback : callbacks) {
    if (callback->hasPendingError) {
      rt.fail(callback->pendingMessage, callback->pendingLine, callback->pendingCol);
    }
    // C may keep the function pointer and call it after this extern call
    // returns, so the stub outlives the call. The engine owns it for the
    // process lifetime, exactly like the AOT backend's stub page
    // (BACKLOG §6 item 27).
    rt.retainHostObject(std::move(callback));
  }

  if (returnType.isStruct) {
    if (retLayout.isMemory) {
      return boxStructResult(rt, retLayout, reinterpret_cast<const char*>(sret.data()));
    }
    char bytes[16] = {0};
    switch (structShapeOf(retLayout)) {
      case StructShape::I1:
        std::memcpy(bytes, ret + 0, 8);
        break;
      case StructShape::S1:
        std::memcpy(bytes, ret + 16, 8);
        break;
      case StructShape::II:
        std::memcpy(bytes, ret + 0, 8);
        std::memcpy(bytes + 8, ret + 8, 8);
        break;
      case StructShape::SS:
        std::memcpy(bytes, ret + 16, 8);
        std::memcpy(bytes + 8, ret + 24, 8);
        break;
      case StructShape::IS:
        std::memcpy(bytes, ret + 0, 8);
        std::memcpy(bytes + 8, ret + 16, 8);
        break;
      case StructShape::SI:
        std::memcpy(bytes, ret + 16, 8);
        std::memcpy(bytes + 8, ret + 8, 8);
        break;
      case StructShape::None:
        rt.fail("internal error: a struct return has no ABI shape");
    }
    return boxStructResult(rt, retLayout, bytes);
  }

  ReturnClass rc = ReturnClass::Int;
  classifyReturn(returnType.scalarType, rc);
  switch (rc) {
    case ReturnClass::Void:
      return Value();
    case ReturnClass::Float: {
      float f = 0;
      std::memcpy(&f, ret + 16, sizeof(f));
      return Value::fromFloat(f);
    }
    case ReturnClass::Double: {
      double d = 0;
      std::memcpy(&d, ret + 16, sizeof(d));
      return Value(d);
    }
    case ReturnClass::Ptr:
    case ReturnClass::CString: {
      std::int64_t addressValue = 0;
      std::memcpy(&addressValue, ret + 0, sizeof(addressValue));
      return wrapPointerValue(rt, returnType.scalarType, addressValue);
    }
    default: {
      std::int64_t value = 0;
      std::memcpy(&value, ret + 0, sizeof(value));
      return wrapInteger(rt, returnType.scalarType, value);
    }
  }
}
#else  // OSKEL_FFI_TRAMPOLINE
// Other hosts keep the fixed-scalar typed thunk. The compiler's capability
// flags reject a variadic, a MEMORY struct and a callback there; these helpers
// keep a hand-built signature from falling through to a wild call.
Value callDynamic(Runtime& rt, const std::string&, void*, const std::vector<TypeSpec>&,
                  const TypeSpec&, bool, std::vector<Value>&) {
  rt.fail("a variadic 'extern \"C\"' call or a class-MEMORY struct needs the x86-64 FFI trampoline");
}
#endif  // OSKEL_FFI_TRAMPOLINE

}  // namespace

Value makeExternNative(Runtime& rt, const std::string& name, void* address,
                       const std::vector<TypeSpec>& paramTypes, const TypeSpec& returnType,
                       bool variadic) {
  if (address == nullptr) {
    // `VM::installExtern` already rejects an unresolved symbol; this guards the
    // other callers (the AOT backend, tests) from binding a thunk to NULL, which
    // would turn a bad declaration into a call to address zero.
    rt.fail("extern \"C\" symbol '" + name + "' has no address to bind");
  }
  // Precompute the return disposition once: its class, its shape (for a struct)
  // and its resolved layout, so the call site only marshals arguments.
  ReturnClass rc = ReturnClass::Int;
  StructShape shape = StructShape::None;
  StructLayout layout;
  std::string returnScalar;
  if (returnType.isStruct) {
    std::string reason;
    if (!buildStructLayout(returnType.fields, layout, reason)) {
      rt.fail("invalid extern return struct: " + reason);
    }
    if (layout.isMemory) {
      // A MEMORY struct comes back through a hidden `sret` pointer: only the
      // descriptor stub expresses it, handled below.
      if (!kMemoryStructsSupported) {
        rt.fail("a struct of class MEMORY cannot cross the C boundary on this target");
      }
      rc = ReturnClass::Struct;
    } else {
      if (!kStructByValueSupported) {
        rt.fail("a struct cannot cross the C boundary by value on this target");
      }
      rc = ReturnClass::Struct;
      shape = structShapeOf(layout);
    }
  } else {
    returnScalar = returnType.scalarType;
    classifyReturn(returnScalar, rc);
  }

  // A C variadic or a class-MEMORY struct needs the descriptor stub: the
  // compile-time thunks cannot express a tail whose length is only known at the
  // call site, or an argument that travels on the outgoing stack. A callback
  // beside one of those travels through the same path.
  bool needsDynamic = variadic;
  if (!needsDynamic && returnType.isStruct && layout.isMemory) needsDynamic = true;
  if (!needsDynamic) {
    for (const TypeSpec& spec : paramTypes) {
      if (!spec.isStruct) continue;
      StructLayout paramLayout;
      std::string reason;
      if (!buildStructLayout(spec.fields, paramLayout, reason)) rt.fail(reason);
      if (paramLayout.isMemory) needsDynamic = true;
    }
  }
  if (needsDynamic) {
    if (variadic && !kCVariadicSupported) {
      rt.fail("a variadic 'extern \"C\"' declaration needs the x86-64 FFI trampoline");
    }
    if (!variadic && !kMemoryStructsSupported) {
      rt.fail("a struct of class MEMORY cannot cross the C boundary on this target");
    }
    return makeNative(name, [name, address, paramTypes, returnType, variadic](
                                Runtime& in, std::vector<Value>& args) -> Value {
      return callDynamic(in, name, address, paramTypes, returnType, variadic, args);
    });
  }

  // The address, the signature and the (flattened) parameter classes are
  // captured by value, so the native is self-contained.
  return makeNative(name, [name, address, rc, shape, layout, returnScalar, paramTypes](
                              Runtime& in, std::vector<Value>& args) -> Value {
    if (args.size() != paramTypes.size()) {
      in.fail("'" + name + "' expects " + std::to_string(paramTypes.size()) + " argument" +
              (paramTypes.size() == 1 ? "" : "s") + ", got " + std::to_string(args.size()));
    }
    // Flatten the declared arguments to eightbytes: a scalar is one entry in the
    // register class the ABI assigns it; a struct is one entry per eightbyte, an
    // INTEGER eightbyte passed bit-for-bit as an `int64_t` and an SSE eightbyte
    // as a `double`, so the callee sees the struct's exact bytes.
    std::vector<ParamClass> classes;
    std::vector<std::string> types;
    std::vector<Value> values;
#if OSKEL_FFI_TRAMPOLINE
    // Callbacks built for this call, kept alive until C returns so the
    // trampoline address stays valid while the callee runs.
    std::vector<Ref<CallbackObj>> callbacks;
#endif
    for (std::size_t i = 0; i < paramTypes.size(); ++i) {
      const TypeSpec& spec = paramTypes[i];
      if (spec.isCallback) {
#if OSKEL_FFI_TRAMPOLINE
        // A callback is one GPR: the trampoline address (or NULL for `nil`).
        std::int64_t code = 0;
        if (!args[i].isNil()) {
          if (!args[i].isFunc()) {
            in.fail("argument " + std::to_string(i + 1) + " of '" + name +
                    "' must be a function or nil, got '" + args[i].typeName() + "'");
          }
          auto callback = makeCallback(in, name, spec, args[i]);
          code = reinterpret_cast<std::int64_t>(callback->code.entry());
          callbacks.push_back(std::move(callback));
        }
        classes.push_back(ParamClass::Int);
        types.push_back("int64");
        values.push_back(Value(code));
#else
        in.fail("a callback argument to '" + name +
                "' needs the x86-64 FFI trampoline, which this target does not have");
#endif
        continue;
      }
      if (!spec.isStruct) {
        ParamClass cls = ParamClass::Int;
        classifyParam(spec.scalarType, cls);
        classes.push_back(cls);
        types.push_back(spec.scalarType);
        values.push_back(args[i]);
        continue;
      }
      StructLayout structLayout;
      std::string reason;
      if (!kStructByValueSupported) {
        in.fail("a struct cannot cross the C boundary by value on this target");
      }
      if (!buildStructLayout(spec.fields, structLayout, reason)) in.fail(reason);
      if (structLayout.isMemory) {
        // A MEMORY struct goes through the descriptor stub; reaching here means a
        // hand-built `ExternSymbol` skipped validation, so fail instead of
        // dropping the argument.
        in.fail("a struct of class MEMORY cannot cross the C boundary on this path");
      }
      if (!args[i].isMap()) {
        in.fail("argument " + std::to_string(i + 1) + " of '" + name +
                "' must be a struct value");
      }
      // The buffer is padded to whole eightbytes so the eightbyte extraction
      // below never reads past the end of a struct smaller than 8 bytes; the
      // padding bytes are zero and a C callee ignores them.
      std::vector<char> bytes(static_cast<std::size_t>(structLayout.eightbyteCount) * 8, 0);
      for (std::size_t k = 0; k < structLayout.fields.size(); ++k) {
        const Value* field = args[i].map().get(structLayout.fields[k].name);
        if (field == nullptr) {
          in.fail("argument " + std::to_string(i + 1) + " of '" + name + "' is missing field '" +
                  structLayout.fields[k].name + "'");
        }
        writeStructField(bytes, structLayout.offsets[k], structLayout.fields[k].type, *field, in);
      }
      for (int e = 0; e < structLayout.eightbyteCount; ++e) {
        const char* chunk = bytes.data() + e * 8;
        if (structLayout.eightbyte[e] == 'I') {
          std::int64_t raw = 0;
          std::memcpy(&raw, chunk, sizeof(raw));
          classes.push_back(ParamClass::Int);
          types.push_back("int64");
          values.push_back(Value(raw));
        } else {
          double bits = 0;
          std::memcpy(&bits, chunk, sizeof(bits));
          classes.push_back(ParamClass::Double);
          types.push_back("double");
          values.push_back(Value(bits));
        }
      }
    }
    if (classes.size() > kMaxExternArgs) {
      // `ffi::validateSignature` rejects this at compile time; the guard keeps a
      // signature that skipped validation (a hand-built `ExternSymbol`) from
      // silently dropping its trailing arguments in `dispatch`.
      in.fail("'" + name + "' passes " + std::to_string(classes.size()) +
              " register-sized values; at most " + std::to_string(kMaxExternArgs) +
              " are supported");
    }
    Value result = dispatch<0>(address, rc, shape, layout, returnScalar, classes, types, values, in);
#if OSKEL_FFI_TRAMPOLINE
    // A Oskel error raised inside a callback could not unwind through the C
    // frame; re-raise it now that C has returned.
    for (Ref<CallbackObj>& callback : callbacks) {
      if (callback->hasPendingError) {
        in.fail(callback->pendingMessage, callback->pendingLine, callback->pendingCol);
      }
      // A callback C stores for a later call must stay valid past this call,
      // matching the AOT backend's process-lifetime stub (BACKLOG §6 item 27).
      in.retainHostObject(std::move(callback));
    }
#endif
    return result;
  });
}

}  // namespace ffi
