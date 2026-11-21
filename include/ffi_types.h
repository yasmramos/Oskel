#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "frontend/ast.h"

// The FFI type vocabulary, classified for the System V AMD64 ABI (PHASE 6.6).
//
// This header is deliberately free of any runtime dependency: the compiler uses
// it to validate an `extern "C"` signature and reject one the marshalling
// cannot honour, and the runtime uses the same tables to build the call. That
// keeps a single source of truth while respecting the toolchain boundary
// (DEVELOPMENT.md §2: `src/frontend/compiler.*` must not pull in the VM or the interpreter).
//
// The ABI passes every integer and pointer in a general-purpose register and
// every floating-point value in an SSE register, so a declaration only needs
// to distinguish those two register classes; `float` and `double` share the
// SSE class but need distinct C++ types to marshal the value.
namespace ffi {

// The register class a declared parameter maps to.
enum class ParamClass {
  Int,       // integer, bool or an unannotated/`any` parameter (GPR)
  Pointer,   // `ptr`/`cstring` (GPR, but marshalled from a handle)
  Float,     // C `float` (SSE)
  Double,    // C `double` (SSE)
  Callback,  // `fn(...): R` (GPR, the trampoline address of a Oskel function)
};

// The class of a declared return type. `Void` produces no value; `Ptr` and
// `CString` produce a Oskel pointer handle; `Struct` produces a map rebuilt from
// the returned registers.
enum class ReturnClass {
  Void,
  Bool,
  Int,
  Float,
  Double,
  Ptr,
  CString,
  Struct,
};

// Largest number of *eightbytes* the compile-time thunks cover. A scalar
// argument or return value is one eightbyte; a `struct` counts one per 8 bytes
// it spans (so a 16-byte struct is two). Bounding a signature by eightbytes,
// not parameters, is what keeps the thunk's type alphabet at three C++ types
// (a struct eightbyte is marshalled as a plain `int64_t` or `double`), so the
// template recursion stays a few thousand instantiations instead of exploding.
constexpr std::size_t kMaxExternArgs = 6;

// Whether a `struct` may cross the C boundary by value. The marshalling is
// written for the System V AMD64 ABI, where a struct of up to 16 bytes travels
// in registers as up to two eightbytes; the Microsoft x64 ABI (Windows) passes
// a struct larger than 8 bytes through a hidden pointer instead, a shape the
// register thunks cannot express. Scalars stay portable: the call goes through
// a typed function pointer, so the host compiler applies the platform ABI.
//
// A macro, not only a `constexpr`, because the tests select which cases to
// compile with `#if` (the preprocessor does not evaluate `constexpr`).
#ifndef OSKEL_FFI_STRUCT_BY_VALUE
#if defined(__x86_64__) && !defined(_WIN32)
#define OSKEL_FFI_STRUCT_BY_VALUE 1
#else
#define OSKEL_FFI_STRUCT_BY_VALUE 0
#endif
#endif

constexpr bool kStructByValueSupported = OSKEL_FFI_STRUCT_BY_VALUE != 0;

// Whether an `extern "C"` declaration may be variadic (`fn printf(fmt: cstring,
// ...): int`). A C variadic call needs `ffi_prep_cif_var`, so it needs libffi,
// and the default-argument promotions it applies are the System V AMD64 ones
// (the Microsoft x64 ABI passes a variadic floating-point argument in *both* an
// xmm and a general-purpose register, a shape the register thunks cannot
// express). Scalars stay portable, but variadics are a development-engine,
// System V feature, exactly like a class-MEMORY struct. A macro, not only a
// `constexpr`, for the same `#if` reason as `OSKEL_FFI_STRUCT_BY_VALUE`.
#ifndef OSKEL_FFI_VARIADIC
#if defined(OSKEL_HAVE_LIBFFI) && defined(__x86_64__) && !defined(_WIN32)
#define OSKEL_FFI_VARIADIC 1
#else
#define OSKEL_FFI_VARIADIC 0
#endif
#endif

constexpr bool kCVariadicSupported = OSKEL_FFI_VARIADIC != 0;

// Whether a struct larger than 16 bytes (System V class MEMORY) may cross the
// boundary. The register thunks express a struct only as up to two eightbytes,
// so a MEMORY struct — passed on the stack, and returned through a hidden
// `sret` pointer — cannot go through them; it goes through `libffi`, which
// reproduces the stack layout at run time. That makes it available only where
// the dev engine is built with libffi on the System V AMD64 ABI; elsewhere the
// declaration is rejected at compile time with a clear message. The macro
// derives from `OSKEL_HAVE_LIBFFI` so the compiler and the runtime agree
// without the compiler pulling in any libffi header.
#ifndef OSKEL_FFI_MEMORY_STRUCTS
#if defined(OSKEL_HAVE_LIBFFI) && defined(__x86_64__) && !defined(_WIN32)
#define OSKEL_FFI_MEMORY_STRUCTS 1
#else
#define OSKEL_FFI_MEMORY_STRUCTS 0
#endif
#endif

constexpr bool kMemoryStructsSupported = OSKEL_FFI_MEMORY_STRUCTS != 0;

// The optional ABI capabilities of a build, bundled so a caller can ask the
// same validation twice: once for this build, and once with every capability
// on. A rejection by the first and an acceptance by the second means the
// *vocabulary* can express the shape but this build cannot marshal it, which is
// the difference between skipping a declaration and writing it commented out
// (`bindgen` draws exactly that line). The defaults are this build's.
struct AbiCapabilities {
  bool structByValue = kStructByValueSupported;
  bool memoryStructs = kMemoryStructsSupported;
  bool variadic = kCVariadicSupported;
};

// Every optional capability on: what a build with libffi on the System V AMD64
// ABI accepts.
inline constexpr AbiCapabilities kFullAbiCapabilities{true, true, true};

// What the AOT backend marshals, independent of how the development engine was
// built. The backend emits its own thunks (`native/runtime.cpp`), so it never
// calls libffi: a class-MEMORY struct travels in the outgoing stack area and
// comes back through an `sret` pointer, and a register-class struct goes through
// the eightbyte buffers. Its thunks hard-code the System V argument registers,
// which is why the hosted link mode is Linux x86-64 only.
//
// A C variadic is lowered too (PHASE 7f.5): the thunk walks the tail at run
// time, promotes each value by its tag, continues the GPR/SSE allocation after
// the fixed prefix, spills the overflow to the outgoing stack area and sets
// `%al` to the vector-register count. The declaration is accepted for the AOT
// destination even when the development engine has no libffi, because the
// backend does not need it.
//
// The compiler validates `extern "C"` against the *destination*: `oskel build`
// asks with these capabilities, so a shape the backend can emit is not rejected
// because the development engine happened to be built without libffi. The
// interpreter asks with `AbiCapabilities{}`, the build's own.
inline constexpr AbiCapabilities kAotAbiCapabilities{/*structByValue=*/true,
                                                     /*memoryStructs=*/true,
                                                     /*variadic=*/true};

// One field of a struct crossing the C boundary. `type` is an FFI scalar type
// name (the same vocabulary as a parameter annotation); a nested struct is not
// a scalar and is rejected by `buildStructLayout`.
struct FfiField {
  std::string name;
  std::string type;
};

// The C memory layout of a small struct, computed from its field declarations.
// The compiler builds one to validate an `extern "C"` signature and the runtime
// builds the same to marshal the value, so a single function is the source of
// truth for both (the compiler never carries ABI logic of its own).
struct StructLayout {
  std::vector<FfiField> fields;
  std::vector<std::size_t> offsets;  // byte offset of each field
  std::vector<std::size_t> sizes;    // byte size of each field
  std::vector<ParamClass> classes;   // register class of each field
  std::size_t size = 0;              // total size, padded to `alignment`
  std::size_t alignment = 1;
  // System V classification: at most two eightbytes, each `'I'` (INTEGER, a
  // general-purpose register) or `'S'` (SSE, an xmm register). A struct whose
  // size exceeds 16 bytes is class MEMORY: `isMemory` is set, the eightbytes are
  // not classified, and the whole buffer crosses the boundary through libffi.
  bool isMemory = false;
  char eightbyte[2] = {'I', 'I'};
  int eightbyteCount = 0;
};

// A parameter or return type as written in an `extern "C"` declaration: either
// a scalar (the FFI vocabulary) or a struct (its resolved fields). The compiler
// produces it from the AST, the runtime consumes it, and the AOT backend
// (PHASE 7) will read the same shape off the proto.
struct TypeSpec {
  std::string scalarType;  // scalar spelling when `isStruct` is false ("" = int)
  bool isStruct = false;
  std::vector<FfiField> fields;  // struct shape when `isStruct` is true
  // Callback (PHASE 6.6): a C function pointer. `isCallback` marks it; the
  // signature is stored as scalar type names (a callback parameter cannot
  // itself be a callback or a struct, so no recursion is needed). A callback
  // crosses the ABI as one GPR (the trampoline address).
  bool isCallback = false;
  std::vector<std::string> callbackParams;
  std::string callbackReturn;  // "void" for a callback that returns nothing
  // A callback may return a struct (PHASE 6.6): `callbackReturnsStruct` marks
  // it and `callbackReturnFields` carries the resolved shape, exactly as a
  // struct parameter carries `fields`. A struct field is a scalar (a nested
  // struct is rejected by `buildStructLayout`), so one level suffices.
  bool callbackReturnsStruct = false;
  std::vector<FfiField> callbackReturnFields;
};

// A Oskel `string` and a `cstring` are both a NUL-terminated `const char *` at
// the C boundary, so a declaration may spell a parameter either way. A `string`
// argument is a valid `const char *` (it is NUL-terminated and stays alive for
// the whole call), and `cstring` remains the spelling for a returned pointer,
// which the runtime wraps as an owned handle. `string` is not a valid *return*
// type: a C function returns a `cstring` (see `classifyReturn`).
inline bool isFfiCStringTypeName(const std::string& typeName) {
  return typeName == "cstring" || typeName == "string";
}

// Byte size, alignment and register class of a scalar field type. Returns false
// for a type that cannot be a C struct member (a nested struct, a map, ...).
inline bool scalarFieldLayout(const std::string& typeName, std::size_t& size,
                              std::size_t& alignment, ParamClass& cls) {
  if (typeName.empty() || typeName == "any" || typeName == "int" || typeName == "int32" ||
      typeName == "uint" || typeName == "uint32") {
    size = 4; alignment = 4; cls = ParamClass::Int; return true;
  }
  if (typeName == "int8" || typeName == "uint8" || typeName == "bool") {
    size = 1; alignment = 1; cls = ParamClass::Int; return true;
  }
  if (typeName == "int16" || typeName == "uint16") {
    size = 2; alignment = 2; cls = ParamClass::Int; return true;
  }
  if (typeName == "int64" || typeName == "uint64" || typeName == "size_t") {
    size = 8; alignment = 8; cls = ParamClass::Int; return true;
  }
  if (typeName == "ptr" || isFfiCStringTypeName(typeName)) {
    size = 8; alignment = 8; cls = ParamClass::Pointer; return true;
  }
  if (typeName == "float") { size = 4; alignment = 4; cls = ParamClass::Float; return true; }
  if (typeName == "double") { size = 8; alignment = 8; cls = ParamClass::Double; return true; }
  return false;
}

// Computes the C layout of a struct from its fields, following the natural
// alignment of each scalar member. On failure it fills `reason` and returns
// false: a nested/unsupported field type or an empty struct. A struct larger
// than 16 bytes succeeds with `out.isMemory` set (System V class MEMORY); the
// caller decides whether the target can express it.
inline bool buildStructLayout(const std::vector<FfiField>& fields, StructLayout& out,
                              std::string& reason) {
  out = StructLayout{};
  out.fields = fields;
  std::size_t offset = 0;
  std::size_t maxAlignment = 1;
  for (const FfiField& field : fields) {
    std::size_t size = 0;
    std::size_t alignment = 1;
    ParamClass cls = ParamClass::Int;
    if (!scalarFieldLayout(field.type, size, alignment, cls)) {
      reason = "field '" + field.name + "' has type '" +
               (field.type.empty() ? "unannotated" : field.type) +
               "', which cannot cross the C boundary";
      return false;
    }
    offset = (offset + alignment - 1) / alignment * alignment;
    out.offsets.push_back(offset);
    out.sizes.push_back(size);
    out.classes.push_back(cls);
    offset += size;
    if (alignment > maxAlignment) maxAlignment = alignment;
  }
  if (offset == 0) {
    reason = "an empty struct cannot cross the C boundary";
    return false;
  }
  const std::size_t total = (offset + maxAlignment - 1) / maxAlignment * maxAlignment;
  out.size = total;
  out.alignment = maxAlignment;
  if (total > 16) {
    // Class MEMORY: passed on the stack and returned through a hidden pointer.
    // The eightbyte classification below does not apply, so stop here; the
    // caller decides whether the target can express it (see `validateTypeSpec`).
    out.isMemory = true;
    out.eightbyteCount = 0;
    return true;
  }
  out.eightbyteCount = static_cast<int>((total + 7) / 8);
  // An eightbyte is SSE only when every field touching it is floating point;
  // an integer/pointer field anywhere in it makes the whole eightbyte INTEGER.
  bool anyInteger[2] = {false, false};
  for (std::size_t k = 0; k < fields.size(); ++k) {
    const bool integer = out.classes[k] != ParamClass::Float &&
                         out.classes[k] != ParamClass::Double;
    if (!integer) continue;
    for (std::size_t b = out.offsets[k]; b < out.offsets[k] + out.sizes[k]; ++b) {
      anyInteger[b / 8] = true;
    }
  }
  for (int i = 0; i < out.eightbyteCount; ++i) out.eightbyte[i] = anyInteger[i] ? 'I' : 'S';
  return true;
}

// Maps a declared Oskel parameter type to its FFI class. An empty name or `any`
// is treated as C's implicit `int` (the GPR class). Returns false when the type
// cannot cross the C boundary.
inline bool classifyParam(const std::string& typeName, ParamClass& out) {
  if (typeName.empty() || typeName == "any" || typeName == "bool" ||
      isIntegerTypeName(typeName)) {
    out = ParamClass::Int;
    return true;
  }
  if (typeName == "ptr" || isFfiCStringTypeName(typeName)) {
    out = ParamClass::Pointer;
    return true;
  }
  if (typeName == "float") {
    out = ParamClass::Float;
    return true;
  }
  if (typeName == "double") {
    out = ParamClass::Double;
    return true;
  }
  return false;
}

// Maps a declared return type to its FFI class. An empty name is C's implicit
// `int`. Returns false when the type cannot be produced by a C function.
inline bool classifyReturn(const std::string& typeName, ReturnClass& out) {
  if (typeName.empty() || typeName == "any" || isIntegerTypeName(typeName)) {
    out = ReturnClass::Int;
    return true;
  }
  if (typeName == "void") {
    out = ReturnClass::Void;
    return true;
  }
  if (typeName == "bool") {
    out = ReturnClass::Bool;
    return true;
  }
  if (typeName == "float") {
    out = ReturnClass::Float;
    return true;
  }
  if (typeName == "double") {
    out = ReturnClass::Double;
    return true;
  }
  if (typeName == "ptr") {
    out = ReturnClass::Ptr;
    return true;
  }
  if (typeName == "cstring") {
    out = ReturnClass::CString;
    return true;
  }
  return false;
}

// Human-readable reason a type is not marshallable, for a diagnostic. The
// caller prefixes it with the symbol it belongs to.
inline std::string unsupportedTypeMessage(const std::string& typeName, bool isReturn) {
  std::string shown = typeName.empty() ? "unannotated" : "'" + typeName + "'";
  return (isReturn ? "return type " : "parameter type ") + shown +
         " cannot cross the C boundary";
}

// The register shape of a small struct: at most two eightbytes, each INTEGER
// (`I`, a general-purpose register) or SSE (`S`, an xmm register). `None` is a
// scalar, not a struct. The runtime dispatches on this to pick the fixed POD
// type it passes to, or reads the result from, the C function.
enum class StructShape { None, I1, S1, II, SS, IS, SI };

inline StructShape structShapeOf(const StructLayout& layout) {
  if (layout.eightbyteCount == 1) {
    return layout.eightbyte[0] == 'I' ? StructShape::I1 : StructShape::S1;
  }
  const char first = layout.eightbyte[0];
  const char second = layout.eightbyte[1];
  if (first == 'I') return second == 'I' ? StructShape::II : StructShape::IS;
  return second == 'I' ? StructShape::SI : StructShape::SS;
}

// Checks one resolved parameter/return spec against what the marshalling can
// honour. Returns an empty string when the type is fine, or a message naming
// the problem. A scalar goes through `classifyParam`/`classifyReturn`; a struct
// must have a layout `buildStructLayout` accepts.
//
// `caps` is the ABI this check is asked about. The default is this build; pass
// `kFullAbiCapabilities` to ask whether the vocabulary can express the shape at
// all, ignoring what this particular build can marshal.
inline std::string validateTypeSpec(const TypeSpec& spec, bool isReturn,
                                    const AbiCapabilities& caps = AbiCapabilities{}) {
  if (spec.isCallback) {
    // A callback is a C function pointer. It crosses the ABI as one GPR, so it
    // is only meaningful as a parameter: a C function that *returns* a
    // function pointer is not part of the surface this phase supports.
    if (isReturn) {
      return "a callback cannot be a return type";
    }
    for (const std::string& param : spec.callbackParams) {
      ParamClass ignored;
      // A callback parameter is a scalar; a nested callback or a struct by
      // value in a callback signature is out of scope.
      if (!classifyParam(param, ignored)) {
        return "callback parameter type '" + (param.empty() ? "unannotated" : param) +
               "' cannot cross the C boundary";
      }
    }
    if (spec.callbackReturnsStruct) {
      StructLayout layout;
      std::string reason;
      if (!buildStructLayout(spec.callbackReturnFields, layout, reason)) return reason;
      if (layout.isMemory) {
        if (!caps.memoryStructs) {
          return "a callback returning a struct of " + std::to_string(layout.size) +
                 " bytes is class MEMORY (> 16 bytes: passed on the stack, returned through a "
                 "hidden pointer), which this build does not support: it needs the System V AMD64 "
                 "ABI and libffi in the development engine";
        }
      } else if (!caps.structByValue) {
        return "a callback cannot return a struct by value on this target: the marshalling "
               "implements the System V AMD64 ABI only";
      }
      return "";
    }
    if (!spec.callbackReturn.empty() && spec.callbackReturn != "void") {
      ReturnClass ignored;
      if (!classifyReturn(spec.callbackReturn, ignored)) {
        return "callback return type '" + spec.callbackReturn + "' cannot cross the C boundary";
      }
    }
    return "";
  }
  if (!spec.isStruct) {
    if (isReturn) {
      ReturnClass ignored;
      if (!classifyReturn(spec.scalarType, ignored)) {
        return unsupportedTypeMessage(spec.scalarType, true);
      }
    } else {
      ParamClass ignored;
      if (!classifyParam(spec.scalarType, ignored)) {
        return unsupportedTypeMessage(spec.scalarType, false);
      }
    }
    return "";
  }
  StructLayout layout;
  std::string reason;
  if (!buildStructLayout(spec.fields, layout, reason)) return reason;
  if (layout.isMemory) {
    // A struct larger than 16 bytes is class MEMORY (passed on the stack,
    // returned through a hidden `sret` pointer). Only the libffi-backed
    // marshalling expresses that; the register thunks cannot.
    if (!caps.memoryStructs) {
      return "a struct of " + std::to_string(layout.size) +
             " bytes is class MEMORY (> 16 bytes: passed on the stack, returned through a hidden "
             "pointer), which this build does not support: it needs the System V AMD64 ABI and "
             "libffi in the development engine";
    }
    return "";
  }
  if (!caps.structByValue) {
    return "a struct cannot cross the C boundary by value on this target: the register "
           "marshalling implements the System V AMD64 ABI only (the Microsoft x64 ABI passes "
           "a struct larger than 8 bytes through a hidden pointer)";
  }
  return "";
}

// A canonical, comparable spelling of one resolved type spec. Two declarations
// of the same C symbol must resolve to the same spec; this string is the key
// that makes the comparison exact (a scalar by name, a struct by its ordered
// field names and types, a callback by its signature). It is deliberately a
// pure function of the spec: no ABI detail leaks in, so the compiler can detect
// a conflict without knowing the target.
inline std::string canonicalTypeSpec(const TypeSpec& spec) {
  if (spec.isCallback) {
    std::string out = "fn(";
    for (std::size_t i = 0; i < spec.callbackParams.size(); ++i) {
      if (i != 0) out += ",";
      out += spec.callbackParams[i].empty() ? "int" : spec.callbackParams[i];
    }
    out += "):";
    if (spec.callbackReturnsStruct) {
      out += "struct{";
      for (std::size_t i = 0; i < spec.callbackReturnFields.size(); ++i) {
        if (i != 0) out += ",";
        out += spec.callbackReturnFields[i].name;
        out += ":";
        out += spec.callbackReturnFields[i].type.empty() ? "int" : spec.callbackReturnFields[i].type;
      }
      out += "}";
    } else {
      out += spec.callbackReturn.empty() ? "void" : spec.callbackReturn;
    }
    return out;
  }
  if (spec.isStruct) {
    std::string out = "struct{";
    for (std::size_t i = 0; i < spec.fields.size(); ++i) {
      if (i != 0) out += ",";
      out += spec.fields[i].name;
      out += ":";
      out += spec.fields[i].type.empty() ? "int" : spec.fields[i].type;
    }
    out += "}";
    return out;
  }
  return spec.scalarType.empty() ? "int" : spec.scalarType;
}

// The canonical spelling of a whole signature, parameters then return. Two
// `extern "C"` declarations of one symbol agree exactly when these strings are
// equal; the compiler rejects a mismatch instead of letting one silently win.
// The trailing `...` of a C variadic is part of the signature: `printf(a: int)`
// and `printf(a: int, ...)` are different declarations.
inline std::string canonicalSignature(const std::vector<TypeSpec>& params,
                                      const TypeSpec& returnType, bool variadic = false) {
  std::string out = "(";
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i != 0) out += ",";
    out += canonicalTypeSpec(params[i]);
  }
  if (variadic) out += params.empty() ? "..." : ",...";
  out += ")->";
  out += canonicalTypeSpec(returnType);
  return out;
}

// Checks one `extern "C"` signature against what the marshalling can honour.
// Returns an empty string when the signature is fine, or a message naming the
// first problem. The compiler calls this so an unsupported declaration fails
// at compile time with a clear error instead of at the first call.
//
// `variadic` is true for a C variadic (`fn printf(fmt: cstring, ...): int`).
// Such a declaration always goes through `libffi` (the register thunks cannot
// express a variable argument list), so it needs `caps.variadic` and is
// not bound by the register budget below; its return must be a scalar.
//
// `caps` is the ABI this check is asked about; the default is this build. Pass
// `kFullAbiCapabilities` to ask whether the vocabulary can express the
// signature at all.
inline std::string validateSignature(const std::vector<TypeSpec>& params,
                                     const TypeSpec& returnType, bool variadic = false,
                                     const AbiCapabilities& caps = AbiCapabilities{}) {
  // The bound is on flattened eightbytes, not parameters: a struct contributes
  // one per 8 bytes it spans. Keeping the total at or below the number of
  // general-purpose argument registers guarantees every eightbyte lands in a
  // register, so none spills to the stack (a partly-registered aggregate is
  // passed wholly in memory, which the register thunks cannot express). A
  // variadic call goes through libffi, which reproduces the stack layout, so
  // the bound does not apply to it.
  std::size_t eightbytes = 0;
  for (const TypeSpec& spec : params) {
    if (spec.isStruct) {
      StructLayout layout;
      std::string reason;
      if (!buildStructLayout(spec.fields, layout, reason)) return reason;
      // A MEMORY struct travels on the stack, not in a register, so it does not
      // consume the register budget the bound below enforces.
      if (!layout.isMemory) eightbytes += static_cast<std::size_t>(layout.eightbyteCount);
    } else {
      eightbytes += 1;
    }
    std::string problem = validateTypeSpec(spec, /*isReturn=*/false, caps);
    if (!problem.empty()) return problem;
  }
  if (variadic) {
    if (!caps.variadic) {
      return "a variadic 'extern \"C\"' declaration needs a marshalling that builds the variable "
             "argument list per call (libffi in the development engine), on the System V AMD64 "
             "ABI, which this destination does not have";
    }
    for (const TypeSpec& spec : params) {
      if (spec.isStruct || spec.isCallback) {
        return "a variadic 'extern \"C\"' function takes only scalar parameters before the '...'";
      }
    }
    if (returnType.isStruct) {
      return "a variadic 'extern \"C\"' function must return a scalar";
    }
    return validateTypeSpec(returnType, /*isReturn=*/true, caps);
  }
  if (eightbytes > kMaxExternArgs) {
    return "an 'extern' function may pass at most " + std::to_string(kMaxExternArgs) +
           " register-sized values (a struct counts one per 8 bytes)";
  }
  return validateTypeSpec(returnType, /*isReturn=*/true, caps);
}

}  // namespace ffi
