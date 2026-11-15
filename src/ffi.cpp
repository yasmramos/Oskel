#include "ffi_marshal.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "ffi_types.h"

#ifdef OSKEL_HAVE_LIBFFI
// libffi builds the trampoline a C function calls back into. It is only used
// for callbacks, and only in the development engine (DEVELOPMENT.md §5.1/§6.6).
// `ffi.h` is the system header: the project's own header is `ffi_marshal.h`
// precisely so this include cannot be shadowed by `-Iinclude`.
#include <ffi.h>
#endif

// Marshalling across the System V AMD64 ABI, without `libffi` (PHASE 6.6).
//
// The call is a compile-time thunk selected by the signature's ABI shape, not
// code generated at run time. Three facts make this a small, closed set:
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
// Bounding a signature by eightbytes (`kMaxExternArgs` = 6) rather than
// parameters is deliberate: it is at most the number of general-purpose
// registers System V passes arguments in, so every flattened eightbyte still
// lands in a register and none spills to the stack, sidestepping the rule that
// a partly-registered aggregate is passed wholly in memory.
//
// The argument list is walked with a compile-time depth (`Index`), not a
// runtime count: the recursion stops at `kMaxExternArgs`, so the compiler
// instantiates a bounded set of thunks instead of recursing on a runtime
// condition (which would instantiate forever).
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
        return dispatch<Index + 1>(address, rc, shape, layout, returnType, classes, types, values,
                                   rt, built..., marshalFloat(rt, values[Index], types[Index]));
      case ParamClass::Double:
        return dispatch<Index + 1>(address, rc, shape, layout, returnType, classes, types, values,
                                   rt, built..., marshalDouble(rt, values[Index], types[Index]));
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

#ifdef OSKEL_HAVE_LIBFFI
// ------------------------------------------------------------------ callbacks

// Maps an FFI scalar type name to the libffi type the trampoline marshals it
// as. `validateSignature` has already rejected anything unsupported, so the
// fallback (`int`) is only reached for an unannotated scalar.
ffi_type* libffiTypeFor(const std::string& typeName) {
  if (typeName.empty() || typeName == "any" || typeName == "int" || typeName == "int32") {
    return &ffi_type_sint32;
  }
  if (typeName == "bool" || typeName == "uint8") return &ffi_type_uint8;
  if (typeName == "int8") return &ffi_type_sint8;
  if (typeName == "int16") return &ffi_type_sint16;
  if (typeName == "uint16") return &ffi_type_uint16;
  if (typeName == "int64") return &ffi_type_sint64;
  if (typeName == "uint" || typeName == "uint32") return &ffi_type_uint32;
  if (typeName == "uint64" || typeName == "size_t") return &ffi_type_uint64;
  if (typeName == "float") return &ffi_type_float;
  if (typeName == "double") return &ffi_type_double;
  if (typeName == "ptr" || isFfiCStringTypeName(typeName)) return &ffi_type_pointer;
  return &ffi_type_sint32;
}

// Owns every libffi object one signature needs, so a single `ffi_prep_cif` can
// keep them alive for as long as the `cif` lives. A struct type needs its
// element array to outlive it (libffi reads it during preparation), so both the
// `ffi_type` and the element vector are owned here.
struct LibffiSignature {
  std::vector<std::unique_ptr<ffi_type>> types;
  std::vector<std::unique_ptr<std::vector<ffi_type*>>> elementArrays;
};

// Builds the libffi type of a struct from its C layout, appending the owned
// objects to `sig`. A field is always a scalar (a nested struct is rejected by
// `buildStructLayout`), so the element list is one level deep.
ffi_type* libffiStructType(const StructLayout& layout, LibffiSignature& sig) {
  auto elements = std::make_unique<std::vector<ffi_type*>>();
  elements->reserve(layout.fields.size() + 1);
  for (const FfiField& field : layout.fields) {
    elements->push_back(libffiTypeFor(field.type));
  }
  elements->push_back(nullptr);
  auto type = std::make_unique<ffi_type>();
  type->type = FFI_TYPE_STRUCT;
  type->size = 0;
  type->alignment = 0;
  type->elements = elements->data();
  ffi_type* raw = type.get();
  sig.elementArrays.push_back(std::move(elements));
  sig.types.push_back(std::move(type));
  return raw;
}

// The state behind one Oskel function passed to C as a callback: the function
// itself, its prepared `ffi_cif`, and the libffi closure C calls. The object is
// kept alive for the whole extern call (the caller holds a `Ref`), so the
// trampoline address stays valid while C runs. A callback C stores for later is
// out of scope for this phase and would need a longer-lived owner.
struct CallbackObj : RefCounted {
  ~CallbackObj() {
    if (closure != nullptr) ffi_closure_free(closure);
  }

  Runtime* runtime = nullptr;
  Value function;
  std::string name;  // the extern symbol, for diagnostics
  std::vector<std::string> paramTypes;
  std::string returnType;  // "void" for no result
  std::vector<ffi_type*> argTypes;
  // A callback that returns a struct (PHASE 6.6): the return `ffi_type` is built
  // from these, and the trampoline serialises the resulting map into the return
  // storage using this layout. `signature` owns the libffi objects so they
  // outlive the `cif`.
  bool returnsStruct = false;
  StructLayout returnLayout;
  LibffiSignature signature;
  ffi_cif cif{};
  ffi_closure* closure = nullptr;
  void* code = nullptr;
  // A RuntimeError raised inside the Oskel callback cannot unwind through the C
  // frame that called the trampoline, so the trampoline stores it here and the
  // extern call re-raises it once C returns.
  bool hasPendingError = false;
  std::string pendingMessage;
  int pendingLine = 0;
  int pendingCol = 0;
};

// Reads one callback argument from the `void*` libffi hands the trampoline and
// boxes it as a Oskel value, using the declared width so a narrow C `int` is not
// read as 64 bits.
Value readCallbackArg(Runtime& rt, const std::string& typeName, void* slot) {
  if (typeName == "float") return Value::fromFloat(*static_cast<float*>(slot));
  if (typeName == "double") return Value(*static_cast<double*>(slot));
  if (typeName == "ptr" || isFfiCStringTypeName(typeName)) {
    auto raw = reinterpret_cast<std::int64_t>(*static_cast<void**>(slot));
    return wrapPointerValue(rt, typeName, raw);
  }
  std::int64_t raw = 0;
  std::size_t size = 8;
  std::size_t alignment = 8;
  ParamClass cls = ParamClass::Int;
  scalarFieldLayout(typeName, size, alignment, cls);
  std::memcpy(&raw, slot, size);
  return wrapInteger(rt, typeName, raw);
}

// Writes the Oskel callback's result into the storage libffi provides, in the
// declared C type. `void` writes nothing; the error path writes zero of the
// declared width so C never observes an uninitialised return. A struct return
// is serialised field by field into the same storage using its layout.
void writeCallbackResult(const CallbackObj& callback, const Value& value, void* ret) {
  const std::string& typeName = callback.returnType;
  if (callback.returnsStruct) {
    // The error path passes an empty `Value`, which is not a map: zero the whole
    // buffer so the caller never reads stale bytes.
    std::vector<char> bytes(callback.returnLayout.size, 0);
    if (value.isMap()) {
      for (std::size_t k = 0; k < callback.returnLayout.fields.size(); ++k) {
        const Value* field = value.map().get(callback.returnLayout.fields[k].name);
        if (field != nullptr) {
          writeStructField(bytes, callback.returnLayout.offsets[k],
                           callback.returnLayout.fields[k].type, *field, *callback.runtime);
        }
      }
    }
    std::memcpy(ret, bytes.data(), callback.returnLayout.size);
    return;
  }
  if (typeName.empty() || typeName == "void") return;
  if (typeName == "float") {
    *static_cast<float*>(ret) = value.isNumber() ? static_cast<float>(value.number()) : 0.0f;
    return;
  }
  if (typeName == "double") {
    *static_cast<double*>(ret) = value.isNumber() ? value.number() : 0.0;
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
    *static_cast<void**>(ret) = reinterpret_cast<void*>(raw);
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

// The C entry point libffi calls for every invocation. It converts the C
// arguments to Oskel values, re-enters the engine through `Runtime::call`, and
// writes the result back. It must never let a C++ exception cross the C frame
// (the caller is C code such as `qsort`), so a Oskel error is captured and
// re-raised by the extern call once C returns.
void oskelCallbackTrampoline(ffi_cif* cif, void* ret, void** args, void* userData) {
  (void)cif;
  CallbackObj* callback = static_cast<CallbackObj*>(userData);
  if (callback->hasPendingError) {
    // The extern call already recorded a failure and will re-raise it once C
    // returns. C may invoke the callback again before that, so short-circuit:
    // re-entering the VM after an error is not meaningful. Leave a zero of the
    // declared width, never a stale value, in the return storage.
    writeCallbackResult(*callback, Value(), ret);
    return;
  }
  std::vector<Value> oskelArgs;
  oskelArgs.reserve(callback->paramTypes.size());
  for (std::size_t i = 0; i < callback->paramTypes.size(); ++i) {
    oskelArgs.push_back(readCallbackArg(*callback->runtime, callback->paramTypes[i], args[i]));
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
    writeCallbackResult(*callback, Value(), ret);
    return;
  } catch (const std::exception& error) {
    // A non-`RuntimeError` (a `std::bad_alloc`, a `std::logic_error`) would
    // otherwise be flattened into a generic message; keep what it says.
    callback->hasPendingError = true;
    callback->pendingMessage = std::string("the callback raised: ") + error.what();
    writeCallbackResult(*callback, Value(), ret);
    return;
  } catch (...) {
    callback->hasPendingError = true;
    callback->pendingMessage = "the callback raised an unknown error";
    writeCallbackResult(*callback, Value(), ret);
    return;
  }
  try {
    writeCallbackResult(*callback, result, ret);
  } catch (const RuntimeError& error) {
    // A struct return that does not match the declared shape (not a map, a
    // missing field, a field of the wrong type) fails here. Record it exactly
    // like an error raised inside the callback: it must not unwind through the
    // C frame that called the trampoline.
    callback->hasPendingError = true;
    callback->pendingMessage = error.message;
    callback->pendingLine = error.line;
    callback->pendingCol = error.col;
    writeCallbackResult(*callback, Value(), ret);
  }
}

// Builds the closure for one Oskel function bound to a callback parameter.
Ref<CallbackObj> makeCallback(Runtime& rt, const std::string& name, const TypeSpec& spec,
                              const Value& function) {
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
  callback->argTypes.reserve(callback->paramTypes.size());
  for (const std::string& type : callback->paramTypes) {
    callback->argTypes.push_back(libffiTypeFor(type));
  }
  ffi_type* returnType = nullptr;
  if (spec.callbackReturnsStruct) {
    std::string reason;
    if (!buildStructLayout(spec.callbackReturnFields, callback->returnLayout, reason)) {
      rt.fail(reason);
    }
    callback->returnsStruct = true;
    returnType = libffiStructType(callback->returnLayout, callback->signature);
  } else {
    returnType =
        callback->returnType == "void" ? &ffi_type_void : libffiTypeFor(callback->returnType);
  }
  if (ffi_prep_cif(&callback->cif, FFI_DEFAULT_ABI,
                   static_cast<unsigned>(callback->argTypes.size()), returnType,
                   callback->argTypes.data()) != FFI_OK) {
    rt.fail("cannot build a callback for '" + name + "'");
  }
  void* code = nullptr;
  callback->closure = static_cast<ffi_closure*>(ffi_closure_alloc(sizeof(ffi_closure), &code));
  if (callback->closure == nullptr) {
    rt.fail("cannot allocate a callback for '" + name + "'");
  }
  if (ffi_prep_closure_loc(callback->closure, &callback->cif, oskelCallbackTrampoline,
                           callback.get(), code) != FFI_OK) {
    ffi_closure_free(callback->closure);
    callback->closure = nullptr;
    rt.fail("cannot prepare a callback for '" + name + "'");
  }
  callback->code = code;
  return callback;
}

// ------------------------------------------------------------------ MEMORY structs

// Eight-byte-aligned storage for one marshalled value, so libffi can read it
// with the alignment the ABI assumes regardless of the exact width.
struct AlignedBytes {
  std::vector<std::uint64_t> words;
  explicit AlignedBytes(std::size_t bytes) : words((bytes + 7) / 8, 0) {}
  void* data() { return words.data(); }
};

// Everything one libffi-backed call needs, prepared once and kept alive for the
// native's lifetime (the `cif` and the struct `ffi_type`s must outlive every
// call).
struct PreparedLibffiCall {
  ffi_cif cif{};
  LibffiSignature signature;
  // `ffi_prep_cif` keeps the pointer to the caller's argument-type array, so the
  // array must outlive the `cif` exactly as the `ffi_type`s do.
  std::vector<ffi_type*> argTypes;
};

// The libffi type of a trailing argument of a C variadic call, after the
// default argument promotions: a `float` widens to `double`, a `bool` becomes
// an `int`, and a string, pointer or nil is a `void*`. A Oskel `int` keeps its
// natural 64-bit width (the register holds the whole value either way, and the
// callee's length modifier decides how many bits to read), so `printf("%d", 5)`
// and `printf("%ld", 5)` both work.
ffi_type* variadicPromotedType(const Value& value) {
  if (value.isFloat() || value.isDouble()) return &ffi_type_double;
  if (value.isStr() || value.isPtr() || value.isNil()) return &ffi_type_pointer;
  if (value.isBool()) return &ffi_type_sint32;
  return &ffi_type_sint64;
}

// Writes one trailing variadic argument into `slot`, matching
// `variadicPromotedType`. The caller has already rejected a value with no C
// representation, so this only sees an int, bool, float, double, string,
// pointer or nil.
void writePromotedArg(Runtime& rt, const Value& value, AlignedBytes& slot) {
  if (value.isFloat()) {
    double d = static_cast<double>(value.number());
    std::memcpy(slot.data(), &d, sizeof(d));
    return;
  }
  if (value.isDouble()) {
    double d = value.number();
    std::memcpy(slot.data(), &d, sizeof(d));
    return;
  }
  if (value.isStr() || value.isPtr() || value.isNil()) {
    std::int64_t raw = pointerAddress(rt, value, "ptr");
    std::memcpy(slot.data(), &raw, sizeof(raw));
    return;
  }
  std::int64_t raw = value.isBool() ? (value.asBool() ? 1 : 0) : value.asInt();
  std::memcpy(slot.data(), &raw, sizeof(raw));
}

// Calls `address` through libffi. Used when the signature carries a struct of
// class MEMORY (> 16 bytes): such a struct travels on the stack and comes back
// through a hidden `sret` pointer, a shape the register thunks cannot express,
// and libffi reproduces the stack layout at run time.
//
// It also drives a C variadic call (`variadic` true): the `ffi_cif` is prepared
// per call with `ffi_prep_cif_var`, because the number of trailing arguments
// changes from one call to the next, and each trailing argument is marshalled
// with the default promotions.
Value callWithLibffi(Runtime& rt, const std::string& name, void* address,
                     const std::vector<TypeSpec>& paramTypes, const TypeSpec& returnType,
                     PreparedLibffiCall* prepared, bool variadic, std::vector<Value>& args) {
  const std::size_t fixedCount = paramTypes.size();
  // A variadic call prepares its own `cif`; a fixed call reuses the one built
  // once in `prepareLibffiCall`. Both `cif` and `argTypes` stay alive for the
  // whole call, which is what libffi requires.
  std::vector<ffi_type*> variadicArgTypes;
  ffi_cif variadicCif{};
  ffi_cif* cif = nullptr;
  if (variadic) {
    variadicArgTypes.reserve(args.size());
    for (std::size_t i = 0; i < fixedCount; ++i) {
      variadicArgTypes.push_back(libffiTypeFor(paramTypes[i].scalarType));
    }
    for (std::size_t i = fixedCount; i < args.size(); ++i) {
      variadicArgTypes.push_back(variadicPromotedType(args[i]));
    }
    ffi_type* returnTypeFfi = returnType.scalarType.empty() || returnType.scalarType == "void"
                                  ? &ffi_type_void
                                  : libffiTypeFor(returnType.scalarType);
    if (ffi_prep_cif_var(&variadicCif, FFI_DEFAULT_ABI, static_cast<unsigned>(fixedCount),
                         static_cast<unsigned>(variadicArgTypes.size()), returnTypeFfi,
                         variadicArgTypes.data()) != FFI_OK) {
      rt.fail("cannot prepare the variadic libffi call for extern \"C\" symbol '" + name + "'");
    }
    cif = &variadicCif;
  } else {
    cif = &prepared->cif;
  }

  std::vector<AlignedBytes> storage;
  storage.reserve(args.size());
  std::vector<void*> avalue;
  avalue.reserve(args.size());
  std::vector<Ref<CallbackObj>> callbacks;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i >= fixedCount) {
      // A trailing argument of a C variadic: promoted, so it is never a struct
      // or a callback. A value with no C representation is rejected here.
      if (!(args[i].isInt() || args[i].isBool() || args[i].isFloat() || args[i].isDouble() ||
            args[i].isStr() || args[i].isPtr() || args[i].isNil())) {
        rt.fail("argument " + std::to_string(i + 1) + " of '" + name + "' has type '" +
                args[i].typeName() + "', which cannot cross the C variadic boundary");
      }
      storage.emplace_back(8);
      writePromotedArg(rt, args[i], storage.back());
      avalue.push_back(storage.back().data());
      continue;
    }
    const TypeSpec& spec = paramTypes[i];
    if (spec.isCallback) {
      std::int64_t code = 0;
      if (!args[i].isNil()) {
        if (!args[i].isFunc()) {
          rt.fail("argument " + std::to_string(i + 1) + " of '" + name +
                  "' must be a function or nil, got '" + args[i].typeName() + "'");
        }
        auto callback = makeCallback(rt, name, spec, args[i]);
        code = reinterpret_cast<std::int64_t>(callback->code);
        callbacks.push_back(std::move(callback));
      }
      storage.emplace_back(sizeof(void*));
      std::memcpy(storage.back().data(), &code, sizeof(void*));
      avalue.push_back(storage.back().data());
      continue;
    }
    if (spec.isStruct) {
      StructLayout layout;
      std::string reason;
      if (!buildStructLayout(spec.fields, layout, reason)) rt.fail(reason);
      std::vector<char> bytes;
      writeStructBytes(rt, layout, args[i], name, i, bytes);
      storage.emplace_back(layout.size);
      std::memcpy(storage.back().data(), bytes.data(), layout.size);
      avalue.push_back(storage.back().data());
      continue;
    }
    // A scalar: write it at its declared width into an aligned slot.
    std::size_t size = 8;
    std::size_t alignment = 8;
    ParamClass cls = ParamClass::Int;
    scalarFieldLayout(spec.scalarType, size, alignment, cls);
    std::vector<char> bytes(size, 0);
    writeStructField(bytes, 0, spec.scalarType, args[i], rt);
    storage.emplace_back(size);
    std::memcpy(storage.back().data(), bytes.data(), size);
    avalue.push_back(storage.back().data());
  }

  // The return storage. A MEMORY struct comes back through the hidden pointer
  // libffi places; a scalar is read from the same slot at its declared width.
  StructLayout retLayout;
  if (returnType.isStruct) {
    std::string reason;
    if (!buildStructLayout(returnType.fields, retLayout, reason)) rt.fail(reason);
  }
  AlignedBytes ret(returnType.isStruct ? retLayout.size : 8);

  ffi_call(cif, FFI_FN(address), ret.data(), avalue.data());

  for (const Ref<CallbackObj>& callback : callbacks) {
    if (callback->hasPendingError) {
      rt.fail(callback->pendingMessage, callback->pendingLine, callback->pendingCol);
    }
  }

  if (returnType.isStruct) {
    return boxStructResult(rt, retLayout, static_cast<const char*>(ret.data()));
  }
  ReturnClass rc = ReturnClass::Int;
  classifyReturn(returnType.scalarType, rc);
  const char* raw = static_cast<const char*>(ret.data());
  switch (rc) {
    case ReturnClass::Void: return Value();
    case ReturnClass::Float: {
      float f = 0;
      std::memcpy(&f, raw, sizeof(f));
      return Value::fromFloat(f);
    }
    case ReturnClass::Double: {
      double d = 0;
      std::memcpy(&d, raw, sizeof(d));
      return Value(d);
    }
    case ReturnClass::Ptr:
    case ReturnClass::CString: {
      std::int64_t addressValue = 0;
      std::memcpy(&addressValue, raw, sizeof(addressValue));
      return wrapPointerValue(rt, returnType.scalarType, addressValue);
    }
    default: {
      std::int64_t value = 0;
      std::memcpy(&value, raw, sizeof(value));
      return wrapInteger(rt, returnType.scalarType, value);
    }
  }
}

// Prepares the libffi call for a signature that carries a struct of class
// MEMORY (> 16 bytes). Returns null when no such struct is present, so the
// caller keeps the register-thunk path for every other signature. `ok` is set
// false when libffi cannot prepare the call (a malformed type), so the caller
// can fail cleanly instead of making a wild call.
std::shared_ptr<PreparedLibffiCall> prepareLibffiCall(const std::vector<TypeSpec>& paramTypes,
                                                      const TypeSpec& returnType, bool& ok) {
  ok = true;
  bool usesMemory = false;
  auto prepared = std::make_shared<PreparedLibffiCall>();
  std::vector<ffi_type*>& argTypes = prepared->argTypes;
  argTypes.reserve(paramTypes.size());
  for (const TypeSpec& spec : paramTypes) {
    if (spec.isCallback) {
      argTypes.push_back(&ffi_type_pointer);
      continue;
    }
    if (spec.isStruct) {
      StructLayout layout;
      std::string reason;
      if (!buildStructLayout(spec.fields, layout, reason)) {
        ok = false;
        return nullptr;
      }
      if (layout.isMemory) usesMemory = true;
      argTypes.push_back(libffiStructType(layout, prepared->signature));
      continue;
    }
    argTypes.push_back(libffiTypeFor(spec.scalarType));
  }
  ffi_type* returnTypeFfi = nullptr;
  if (returnType.isStruct) {
    StructLayout layout;
    std::string reason;
    if (!buildStructLayout(returnType.fields, layout, reason)) {
      ok = false;
      return nullptr;
    }
    if (layout.isMemory) usesMemory = true;
    returnTypeFfi = libffiStructType(layout, prepared->signature);
  } else {
    returnTypeFfi = returnType.scalarType.empty() || returnType.scalarType == "void"
                        ? &ffi_type_void
                        : libffiTypeFor(returnType.scalarType);
  }
  if (!usesMemory) return nullptr;
  if (ffi_prep_cif(&prepared->cif, FFI_DEFAULT_ABI, static_cast<unsigned>(argTypes.size()),
                   returnTypeFfi, argTypes.data()) != FFI_OK) {
    ok = false;
    return nullptr;
  }
  return prepared;
}
#endif  // OSKEL_HAVE_LIBFFI

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
      // libffi path expresses it, handled below.
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

  // A struct of class MEMORY (> 16 bytes) is passed on the stack and returned
  // through a hidden pointer, a shape the register thunks cannot express, so the
  // whole call goes through libffi instead. `prepareLibffiCall` returns null for
  // every other signature, which keeps the register path.
#ifdef OSKEL_HAVE_LIBFFI
  if (variadic) {
    // A C variadic always goes through libffi: the number of trailing arguments
    // changes per call, so the `cif` is prepared at the call site with
    // `ffi_prep_cif_var`. `validateSignature` already rejected a variadic build
    // without libffi, so reaching here without support is a programming error.
    if (!kCVariadicSupported) {
      rt.fail("a variadic 'extern \"C\"' declaration needs libffi and the System V AMD64 ABI");
    }
    return makeNative(name, [name, address, paramTypes, returnType](
                                Runtime& in, std::vector<Value>& args) -> Value {
      if (args.size() < paramTypes.size()) {
        in.fail("'" + name + "' expects at least " + std::to_string(paramTypes.size()) +
                " argument" + (paramTypes.size() == 1 ? "" : "s") + ", got " +
                std::to_string(args.size()));
      }
      return callWithLibffi(in, name, address, paramTypes, returnType, /*prepared=*/nullptr,
                            /*variadic=*/true, args);
    });
  }
  bool libffiOk = true;
  std::shared_ptr<PreparedLibffiCall> libffiCall = prepareLibffiCall(paramTypes, returnType, libffiOk);
  if (!libffiOk) {
    rt.fail("cannot prepare the libffi call for extern \"C\" symbol '" + name + "'");
  }
  if (libffiCall != nullptr) {
    return makeNative(name, [name, address, paramTypes, returnType,
                             libffiCall](Runtime& in, std::vector<Value>& args) -> Value {
      if (args.size() != paramTypes.size()) {
        in.fail("'" + name + "' expects " + std::to_string(paramTypes.size()) + " argument" +
                (paramTypes.size() == 1 ? "" : "s") + ", got " + std::to_string(args.size()));
      }
      return callWithLibffi(in, name, address, paramTypes, returnType, libffiCall.get(),
                            /*variadic=*/false, args);
    });
  }
#endif

  if (variadic) {
    // Unreachable in practice: the compiler rejects a variadic declaration when
    // the build cannot express it (`validateSignature`). The guard keeps a
    // hand-built `ExternSymbol` from falling through to the fixed-arity path,
    // where the trailing arguments would be reported as an arity error.
    rt.fail("a variadic 'extern \"C\"' declaration needs libffi and the System V AMD64 ABI");
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
#ifdef OSKEL_HAVE_LIBFFI
    // Callbacks built for this call, kept alive until C returns so the
    // trampoline address stays valid while the callee runs.
    std::vector<Ref<CallbackObj>> callbacks;
#endif
    for (std::size_t i = 0; i < paramTypes.size(); ++i) {
      const TypeSpec& spec = paramTypes[i];
      if (spec.isCallback) {
#ifdef OSKEL_HAVE_LIBFFI
        // A callback is one GPR: the trampoline address (or NULL for `nil`).
        std::int64_t code = 0;
        if (!args[i].isNil()) {
          if (!args[i].isFunc()) {
            in.fail("argument " + std::to_string(i + 1) + " of '" + name +
                    "' must be a function or nil, got '" + args[i].typeName() + "'");
          }
          auto callback = makeCallback(in, name, spec, args[i]);
          code = reinterpret_cast<std::int64_t>(callback->code);
          callbacks.push_back(std::move(callback));
        }
        classes.push_back(ParamClass::Int);
        types.push_back("int64");
        values.push_back(Value(code));
#else
        in.fail("a callback argument to '" + name +
                "' needs libffi, which this build does not have");
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
        // A MEMORY struct goes through the libffi path; reaching here means a
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
#ifdef OSKEL_HAVE_LIBFFI
    // A Oskel error raised inside a callback could not unwind through the C
    // frame; re-raise it now that C has returned.
    for (const Ref<CallbackObj>& callback : callbacks) {
      if (callback->hasPendingError) {
        in.fail(callback->pendingMessage, callback->pendingLine, callback->pendingCol);
      }
    }
#endif
    return result;
  });
}

}  // namespace ffi
