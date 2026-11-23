#include "bytecode_io.h"

#include <cstring>
#include <limits>
#include <string>

// Implementation of the `.oskc` image (PHASE 6i). The layout is documented in
// the header; this file is the single reader and the single writer, so the two
// cannot drift. Nothing here depends on the host's endianness, word size or
// struct padding: every field is written byte by byte, least significant first.

namespace {

constexpr char kMagic[4] = {'O', 'S', 'K', 'C'};

void appendU8(std::vector<std::uint8_t>& out, std::uint8_t value) {
  out.push_back(value);
}

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void appendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void appendI32(std::vector<std::uint8_t>& out, int value) {
  appendU32(out, static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
}

void appendString(std::vector<std::uint8_t>& out, const std::string& value) {
  appendU32(out, static_cast<std::uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

// A cursor over the image. Every read is bounds-checked; a short read throws
// instead of walking off the end, which is what makes a truncated file a clean
// error rather than a crash.
struct Reader {
  const std::vector<std::uint8_t>& bytes;
  std::size_t offset = 0;

  void require(std::size_t count, const char* what) const {
    if (offset > bytes.size() || bytes.size() - offset < count) {
      throw BytecodeIoError(std::string("truncated bytecode image while reading ") +
                            what);
    }
  }

  std::uint8_t u8(const char* what) {
    require(1, what);
    return bytes[offset++];
  }

  std::uint16_t u16(const char* what) {
    require(2, what);
    std::uint16_t value = static_cast<std::uint16_t>(bytes[offset]) |
                          static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
    offset += 2;
    return value;
  }

  std::uint32_t u32(const char* what) {
    require(4, what);
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    }
    return value;
  }

  std::uint64_t u64(const char* what) {
    require(8, what);
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    }
    return value;
  }

  int i32(const char* what) { return static_cast<std::int32_t>(u32(what)); }

  std::string str(const char* what) {
    const std::uint32_t length = u32(what);
    require(length, what);
    std::string value(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
    offset += length;
    return value;
  }
};

void writeFfiField(std::vector<std::uint8_t>& out, const ffi::FfiField& field) {
  appendString(out, field.name);
  appendString(out, field.type);
}

void writeFieldList(std::vector<std::uint8_t>& out,
                    const std::vector<ffi::FfiField>& fields) {
  appendU32(out, static_cast<std::uint32_t>(fields.size()));
  for (const ffi::FfiField& field : fields) writeFfiField(out, field);
}

void writeStringList(std::vector<std::uint8_t>& out,
                     const std::vector<std::string>& values) {
  appendU32(out, static_cast<std::uint32_t>(values.size()));
  for (const std::string& value : values) appendString(out, value);
}

void writeTypeSpec(std::vector<std::uint8_t>& out, const ffi::TypeSpec& spec) {
  appendString(out, spec.scalarType);
  appendU8(out, spec.isStruct ? 1 : 0);
  writeFieldList(out, spec.fields);
  appendU8(out, spec.isCallback ? 1 : 0);
  writeStringList(out, spec.callbackParams);
  appendString(out, spec.callbackReturn);
  appendU8(out, spec.callbackReturnsStruct ? 1 : 0);
  writeFieldList(out, spec.callbackReturnFields);
}

void writeExtern(std::vector<std::uint8_t>& out, const ExternSymbol& symbol) {
  appendString(out, symbol.name);
  appendU8(out, symbol.variadic ? 1 : 0);
  writeTypeSpec(out, symbol.returnType);
  appendU32(out, static_cast<std::uint32_t>(symbol.paramTypes.size()));
  for (const ffi::TypeSpec& spec : symbol.paramTypes) writeTypeSpec(out, spec);
}

void writeConstant(std::vector<std::uint8_t>& out, const Value& value) {
  // Only the values `LOADK` can load live in the pool. Anything else would be
  // a chunk the compiler never produces, and writing it would create a file the
  // loader could not rebuild.
  switch (value.type()) {
    case VType::Nil:
      appendU8(out, 0);
      return;
    case VType::Bool:
      appendU8(out, 1);
      appendU8(out, value.asBool() ? 1 : 0);
      return;
    case VType::Int:
      appendU8(out, 2);
      appendU64(out, static_cast<std::uint64_t>(value.asInt()));
      return;
    case VType::Float: {
      appendU8(out, 3);
      const float narrowed = static_cast<float>(value.asDouble());
      std::uint32_t bits = 0;
      std::memcpy(&bits, &narrowed, sizeof(bits));
      appendU32(out, bits);
      return;
    }
    case VType::Double: {
      appendU8(out, 4);
      const double number = value.asDouble();
      std::uint64_t bits = 0;
      std::memcpy(&bits, &number, sizeof(bits));
      appendU64(out, bits);
      return;
    }
    case VType::Str:
      appendU8(out, 5);
      appendString(out, value.asStr());
      return;
    default:
      throw BytecodeIoError(
          std::string("cannot serialize a constant of type '") + value.typeName() +
          "': the constant pool holds only nil, bool, int, float, double and string");
  }
}

void writeChunk(std::vector<std::uint8_t>& out, const Bytecode& chunk);

void writeInstruction(std::vector<std::uint8_t>& out, const Instruction& instruction) {
  appendU8(out, static_cast<std::uint8_t>(instruction.op));
  appendI32(out, instruction.a);
  appendI32(out, instruction.b);
  appendI32(out, instruction.c);
  appendI32(out, instruction.jump);
  appendI32(out, instruction.d);
  appendI32(out, instruction.e);
  appendI32(out, instruction.line);
  appendI32(out, instruction.col);
}

void writeChunk(std::vector<std::uint8_t>& out, const Bytecode& chunk) {
  appendString(out, chunk.name);
  appendI32(out, chunk.numParams);
  appendI32(out, chunk.numRegisters);
  appendI32(out, chunk.numLocals);
  appendU8(out, chunk.variadic ? 1 : 0);

  appendU32(out, static_cast<std::uint32_t>(chunk.code.size()));
  for (const Instruction& instruction : chunk.code) writeInstruction(out, instruction);

  appendU32(out, static_cast<std::uint32_t>(chunk.constants.size()));
  for (const Value& value : chunk.constants) writeConstant(out, value);

  appendU32(out, static_cast<std::uint32_t>(chunk.children.size()));
  for (const std::shared_ptr<Bytecode>& child : chunk.children) {
    if (!child) throw BytecodeIoError("cannot serialize a null child proto");
    writeChunk(out, *child);
  }

  appendU32(out, static_cast<std::uint32_t>(chunk.upvalues.size()));
  for (const UpvalDesc& upvalue : chunk.upvalues) {
    appendU8(out, upvalue.inParentLocal ? 1 : 0);
    appendI32(out, upvalue.index);
  }

  appendU32(out, static_cast<std::uint32_t>(chunk.externs.size()));
  for (const ExternSymbol& symbol : chunk.externs) writeExtern(out, symbol);

  writeStringList(out, chunk.nativeFns);
  writeStringList(out, chunk.nativeUses);
  writeStringList(out, chunk.usedMethods);
}

ffi::FfiField readFfiField(Reader& reader) {
  ffi::FfiField field;
  field.name = reader.str("ffi field name");
  field.type = reader.str("ffi field type");
  return field;
}

std::vector<ffi::FfiField> readFieldList(Reader& reader, const char* what) {
  const std::uint32_t count = reader.u32(what);
  std::vector<ffi::FfiField> fields;
  fields.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) fields.push_back(readFfiField(reader));
  return fields;
}

std::vector<std::string> readStringList(Reader& reader, const char* what) {
  const std::uint32_t count = reader.u32(what);
  std::vector<std::string> values;
  values.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) values.push_back(reader.str(what));
  return values;
}

ffi::TypeSpec readTypeSpec(Reader& reader) {
  ffi::TypeSpec spec;
  spec.scalarType = reader.str("ffi scalar type");
  spec.isStruct = reader.u8("ffi struct flag") != 0;
  spec.fields = readFieldList(reader, "ffi struct fields");
  spec.isCallback = reader.u8("ffi callback flag") != 0;
  spec.callbackParams = readStringList(reader, "ffi callback parameters");
  spec.callbackReturn = reader.str("ffi callback return type");
  spec.callbackReturnsStruct = reader.u8("ffi callback return struct flag") != 0;
  spec.callbackReturnFields = readFieldList(reader, "ffi callback return fields");
  return spec;
}

ExternSymbol readExtern(Reader& reader) {
  ExternSymbol symbol;
  symbol.name = reader.str("extern symbol name");
  symbol.variadic = reader.u8("extern variadic flag") != 0;
  symbol.returnType = readTypeSpec(reader);
  const std::uint32_t count = reader.u32("extern parameter count");
  symbol.paramTypes.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) symbol.paramTypes.push_back(readTypeSpec(reader));
  return symbol;
}

Value readConstant(Reader& reader) {
  const std::uint8_t tag = reader.u8("constant tag");
  switch (tag) {
    case 0:
      return Value();
    case 1:
      return Value(reader.u8("bool constant") != 0);
    case 2:
      return Value(static_cast<int64_t>(reader.u64("int constant")));
    case 3: {
      const std::uint32_t bits = reader.u32("float constant");
      float narrowed = 0.0f;
      std::memcpy(&narrowed, &bits, sizeof(narrowed));
      return Value::fromFloat(narrowed);
    }
    case 4: {
      const std::uint64_t bits = reader.u64("double constant");
      double number = 0.0;
      std::memcpy(&number, &bits, sizeof(number));
      return Value(number);
    }
    case 5:
      return Value(reader.str("string constant"));
    default:
      throw BytecodeIoError("unknown constant tag " + std::to_string(tag));
  }
}

Bytecode readChunk(Reader& reader, int depth);

Instruction readInstruction(Reader& reader) {
  Instruction instruction;
  const std::uint8_t op = reader.u8("opcode");
  if (op >= static_cast<std::uint8_t>(kOpcodeCount)) {
    throw BytecodeIoError("unknown opcode " + std::to_string(op));
  }
  instruction.op = static_cast<OpCode>(op);
  instruction.a = reader.i32("operand a");
  instruction.b = reader.i32("operand b");
  instruction.c = reader.i32("operand c");
  instruction.jump = reader.i32("jump target");
  instruction.d = reader.i32("operand d");
  instruction.e = reader.i32("operand e");
  instruction.line = reader.i32("source line");
  instruction.col = reader.i32("source column");
  return instruction;
}

// Rejects a register operand outside the frame the chunk reserves. The VM
// indexes its register window directly, so an out-of-range register is
// undefined behaviour there; catching it at load time is what turns a corrupt
// file into a diagnostic.
void requireRegister(int value, const Bytecode& chunk, const char* what) {
  if (value < 0 || value >= chunk.numRegisters) {
    throw BytecodeIoError(std::string(what) + " " + std::to_string(value) +
                          " is outside the " + std::to_string(chunk.numRegisters) +
                          " registers of chunk '" + chunk.name + "'");
  }
}

void requireConstant(int value, const Bytecode& chunk, const char* what) {
  if (value < 0 || static_cast<std::size_t>(value) >= chunk.constants.size()) {
    throw BytecodeIoError(std::string(what) + " " + std::to_string(value) +
                          " is outside the " +
                          std::to_string(chunk.constants.size()) +
                          " constants of chunk '" + chunk.name + "'");
  }
}

// The name a global opcode reads out of the pool. A `GETGLOBAL`/`SETGLOBAL`
// indexes a string constant, so a file that points at an int would reach the VM
// as a bad lookup; the loader says so instead.
void requireGlobalName(int value, const Bytecode& chunk) {
  requireConstant(value, chunk, "global name index");
  if (!chunk.constants[static_cast<std::size_t>(value)].isStr()) {
    throw BytecodeIoError("global name index " + std::to_string(value) +
                          " in chunk '" + chunk.name + "' does not name a string");
  }
}

void validateInstruction(const Instruction& instruction, const Bytecode& chunk) {
  switch (instruction.op) {
    case OpCode::LOADK:
      requireRegister(instruction.a, chunk, "register");
      requireConstant(instruction.b, chunk, "constant index");
      break;
    case OpCode::MOVE:
      requireRegister(instruction.a, chunk, "register");
      requireRegister(instruction.b, chunk, "register");
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
    case OpCode::SHR:
    case OpCode::EQ:
    case OpCode::NEQ:
    case OpCode::LT:
    case OpCode::LTE:
    case OpCode::GT:
    case OpCode::GTE:
    case OpCode::IN:
      requireRegister(instruction.a, chunk, "register");
      requireRegister(instruction.b, chunk, "register");
      requireRegister(instruction.c, chunk, "register");
      break;
    case OpCode::UNOP:
      requireRegister(instruction.a, chunk, "register");
      requireRegister(instruction.b, chunk, "register");
      if (instruction.c < 0 || instruction.c > 2) {
        throw BytecodeIoError("unary operator " + std::to_string(instruction.c) +
                              " is not a UnaryOp");
      }
      break;
    case OpCode::JMP:
      break;
    case OpCode::JMP_IF_FALSE:
    case OpCode::JMP_IF_TRUE:
      requireRegister(instruction.a, chunk, "register");
      break;
    case OpCode::SETGLOBAL:
      requireRegister(instruction.a, chunk, "register");
      requireGlobalName(instruction.b, chunk);
      if (instruction.c < 0 || instruction.c > 2) {
        throw BytecodeIoError("global write mode " + std::to_string(instruction.c) +
                              " is not a GlobalWrite");
      }
      break;
    case OpCode::GETGLOBAL:
      requireRegister(instruction.a, chunk, "register");
      requireGlobalName(instruction.b, chunk);
      break;
    case OpCode::DECLLOCAL:
    case OpCode::SETLOCAL:
      requireRegister(instruction.a, chunk, "register");
      if (instruction.b < 0 || instruction.b >= chunk.numLocals) {
        throw BytecodeIoError("local cell " + std::to_string(instruction.b) +
                              " is outside the " + std::to_string(chunk.numLocals) +
                              " locals of chunk '" + chunk.name + "'");
      }
      break;
    case OpCode::GETLOCAL:
      requireRegister(instruction.a, chunk, "register");
      if (instruction.b < 0 || instruction.b >= chunk.numLocals) {
        throw BytecodeIoError("local cell " + std::to_string(instruction.b) +
                              " is outside the " + std::to_string(chunk.numLocals) +
                              " locals of chunk '" + chunk.name + "'");
      }
      break;
    case OpCode::CALL:
      requireRegister(instruction.a, chunk, "register");
      if (instruction.b < 0) {
        throw BytecodeIoError("negative argument count in CALL");
      }
      if (instruction.a + instruction.b >= chunk.numRegisters) {
        throw BytecodeIoError("CALL argument window runs past the " +
                              std::to_string(chunk.numRegisters) + " registers of chunk '" +
                              chunk.name + "'");
      }
      break;
    case OpCode::RETURN:
      requireRegister(instruction.a, chunk, "register");
      break;
    case OpCode::CLOSURE:
      requireRegister(instruction.a, chunk, "register");
      if (instruction.b < 0 ||
          static_cast<std::size_t>(instruction.b) >= chunk.children.size()) {
        throw BytecodeIoError("child proto " + std::to_string(instruction.b) +
                              " is outside the " +
                              std::to_string(chunk.children.size()) + " children of chunk '" +
                              chunk.name + "'");
      }
      break;
    case OpCode::GETUPVAL:
    case OpCode::SETUPVAL:
      requireRegister(instruction.a, chunk, "register");
      if (instruction.b < 0 ||
          static_cast<std::size_t>(instruction.b) >= chunk.upvalues.size()) {
        throw BytecodeIoError("upvalue " + std::to_string(instruction.b) +
                              " is outside the " +
                              std::to_string(chunk.upvalues.size()) + " upvalues of chunk '" +
                              chunk.name + "'");
      }
      break;
    case OpCode::NEWTABLE:
      requireRegister(instruction.a, chunk, "register");
      if (instruction.b < 0) {
        throw BytecodeIoError("negative table size in NEWTABLE");
      }
      if (instruction.c != 0 && instruction.c != 1) {
        throw BytecodeIoError("table kind " + std::to_string(instruction.c) +
                              " is neither an array (0) nor a map (1)");
      }
      break;
    case OpCode::GETTABLE:
    case OpCode::SETTABLE:
      requireRegister(instruction.a, chunk, "register");
      requireRegister(instruction.b, chunk, "register");
      // `c >= 0` is a key register; `c < 0` is the string constant `-(c + 1)`.
      if (instruction.c >= 0) {
        requireRegister(instruction.c, chunk, "register");
      } else {
        requireConstant(-(instruction.c + 1), chunk, "member name index");
      }
      break;
    case OpCode::ITER:
      requireRegister(instruction.a, chunk, "register");
      requireRegister(instruction.b, chunk, "register");
      requireRegister(instruction.c, chunk, "register");
      requireRegister(instruction.e, chunk, "register");
      if (instruction.d != -1) requireRegister(instruction.d, chunk, "register");
      break;
    case OpCode::HALT:
      break;
  }

  // A jump target is an absolute instruction index. `code.size()` is legal: it
  // is the address just past the last instruction, which a loop's exit uses.
  if (instruction.op == OpCode::JMP || instruction.op == OpCode::JMP_IF_FALSE ||
      instruction.op == OpCode::JMP_IF_TRUE) {
    if (instruction.jump < 0 ||
        static_cast<std::size_t>(instruction.jump) > chunk.code.size()) {
      throw BytecodeIoError("jump target " + std::to_string(instruction.jump) +
                            " is outside the " + std::to_string(chunk.code.size()) +
                            " instructions of chunk '" + chunk.name + "'");
    }
  }
}

Bytecode readChunk(Reader& reader, int depth) {
  if (depth > kOskcMaxDepth) {
    throw BytecodeIoError("chunk nesting deeper than " +
                          std::to_string(kOskcMaxDepth) + " levels");
  }

  Bytecode chunk;
  chunk.name = reader.str("chunk name");
  chunk.numParams = reader.i32("parameter count");
  chunk.numRegisters = reader.i32("register count");
  chunk.numLocals = reader.i32("local count");
  chunk.variadic = reader.u8("variadic flag") != 0;

  if (chunk.numParams < 0 || chunk.numRegisters < 0 || chunk.numLocals < 0) {
    throw BytecodeIoError("negative frame size in chunk '" + chunk.name + "'");
  }

  const std::uint32_t codeCount = reader.u32("instruction count");
  chunk.code.reserve(codeCount);
  for (std::uint32_t i = 0; i < codeCount; ++i) {
    chunk.code.push_back(readInstruction(reader));
  }

  const std::uint32_t constantCount = reader.u32("constant count");
  chunk.constants.reserve(constantCount);
  for (std::uint32_t i = 0; i < constantCount; ++i) {
    chunk.constants.push_back(readConstant(reader));
  }

  // The children are read before the instructions are validated, because a
  // `CLOSURE` names a child index and the check needs the count.
  const std::uint32_t childCount = reader.u32("child proto count");
  chunk.children.reserve(childCount);
  for (std::uint32_t i = 0; i < childCount; ++i) {
    chunk.children.push_back(std::make_shared<Bytecode>(readChunk(reader, depth + 1)));
  }

  const std::uint32_t upvalueCount = reader.u32("upvalue count");
  chunk.upvalues.reserve(upvalueCount);
  for (std::uint32_t i = 0; i < upvalueCount; ++i) {
    UpvalDesc upvalue;
    upvalue.inParentLocal = reader.u8("upvalue locality flag") != 0;
    upvalue.index = reader.i32("upvalue index");
    if (upvalue.index < 0) {
      throw BytecodeIoError("negative upvalue index in chunk '" + chunk.name + "'");
    }
    chunk.upvalues.push_back(upvalue);
  }

  const std::uint32_t externCount = reader.u32("extern count");
  chunk.externs.reserve(externCount);
  for (std::uint32_t i = 0; i < externCount; ++i) chunk.externs.push_back(readExtern(reader));

  chunk.nativeFns = readStringList(reader, "native fn name");
  chunk.nativeUses = readStringList(reader, "native use name");
  chunk.usedMethods = readStringList(reader, "used method name");

  for (const Instruction& instruction : chunk.code) {
    validateInstruction(instruction, chunk);
  }

  return chunk;
}

void readHeader(Reader& reader, std::uint32_t& version, std::uint32_t& abi,
                std::uint32_t& needs) {
  reader.require(4, "magic");
  if (std::memcmp(reader.bytes.data() + reader.offset, kMagic, 4) != 0) {
    throw BytecodeIoError("not a bytecode image: the magic is not 'OSKC'");
  }
  reader.offset += 4;

  version = reader.u32("format version");
  if (version != kOskcVersion) {
    throw BytecodeIoError("unsupported bytecode image version " +
                          std::to_string(version) + " (this engine reads " +
                          std::to_string(kOskcVersion) + ")");
  }
  const std::uint32_t flags = reader.u32("flags");
  if (flags != 0) {
    throw BytecodeIoError("unsupported bytecode image flags " + std::to_string(flags));
  }
  abi = reader.u32("abi capabilities");
  needs = reader.u32("requirements");
}

} // namespace

std::uint32_t encodeAbiCapabilities(const ffi::AbiCapabilities& capabilities) {
  std::uint32_t bits = 0;
  if (capabilities.structByValue) bits |= kOskcAbiStructByValue;
  if (capabilities.memoryStructs) bits |= kOskcAbiMemoryStructs;
  if (capabilities.variadic) bits |= kOskcAbiVariadic;
  if (capabilities.callbacks) bits |= kOskcAbiCallbacks;
  return bits;
}

ffi::AbiCapabilities decodeAbiCapabilities(std::uint32_t bits) {
  ffi::AbiCapabilities capabilities;
  capabilities.structByValue = (bits & kOskcAbiStructByValue) != 0;
  capabilities.memoryStructs = (bits & kOskcAbiMemoryStructs) != 0;
  capabilities.variadic = (bits & kOskcAbiVariadic) != 0;
  capabilities.callbacks = (bits & kOskcAbiCallbacks) != 0;
  return capabilities;
}

bool abiSatisfies(const ffi::AbiCapabilities& destination,
                  const ffi::AbiCapabilities& required) {
  if (required.structByValue && !destination.structByValue) return false;
  if (required.memoryStructs && !destination.memoryStructs) return false;
  if (required.variadic && !destination.variadic) return false;
  if (required.callbacks && !destination.callbacks) return false;
  return true;
}

namespace {

// The capabilities one resolved signature needs. A scalar crosses every ABI; a
// struct is class MEMORY (> 16 bytes) or class register, and the two need
// different marshalling; a variadic needs the tail walker; a callback needs the
// trampoline.
void requireSpec(const ffi::TypeSpec& spec, ffi::AbiCapabilities& needs) {
  if (spec.isCallback) {
    needs.callbacks = true;
    if (spec.callbackReturnsStruct) {
      ffi::StructLayout layout;
      std::string reason;
      if (ffi::buildStructLayout(spec.callbackReturnFields, layout, reason)) {
        if (layout.isMemory) {
          needs.memoryStructs = true;
        } else {
          needs.structByValue = true;
        }
      }
    }
    return;
  }
  if (!spec.isStruct) return;
  ffi::StructLayout layout;
  std::string reason;
  if (!ffi::buildStructLayout(spec.fields, layout, reason)) return;
  if (layout.isMemory) {
    needs.memoryStructs = true;
  } else {
    needs.structByValue = true;
  }
}

void collectCapabilities(const Bytecode& chunk, ffi::AbiCapabilities& needs) {
  for (const ExternSymbol& symbol : chunk.externs) {
    if (symbol.variadic) needs.variadic = true;
    requireSpec(symbol.returnType, needs);
    for (const ffi::TypeSpec& spec : symbol.paramTypes) requireSpec(spec, needs);
  }
  for (const std::shared_ptr<Bytecode>& child : chunk.children) {
    if (child) collectCapabilities(*child, needs);
  }
}

} // namespace

ffi::AbiCapabilities requiredAbiCapabilities(const Bytecode& root) {
  // Start from nothing: a program that never declares an `extern "C"` symbol
  // needs no optional capability, so its file loads on any target.
  ffi::AbiCapabilities needs;
  needs.structByValue = false;
  needs.memoryStructs = false;
  needs.variadic = false;
  needs.callbacks = false;
  collectCapabilities(root, needs);
  return needs;
}

ffi::AbiCapabilities targetAbiCapabilities(const Target& target) {
  ffi::AbiCapabilities capabilities;
  capabilities.structByValue = false;
  capabilities.memoryStructs = false;
  capabilities.variadic = false;
  capabilities.callbacks = false;
  switch (target.abi) {
    case Abi::SysV:
      capabilities.structByValue = true;
      capabilities.memoryStructs = true;
      capabilities.variadic = true;
      capabilities.callbacks = true;
      break;
    case Abi::Win64:
      // Scalar and pointer parameters, plus a C variadic over the same surface.
      // `collectExternThunks` rejects a struct return, a struct by value and a
      // callback on this ABI, so the file must not claim them.
      capabilities.variadic = true;
      break;
    case Abi::Aapcs64:
    case Abi::Wasm:
      // No AOT FFI thunk exists for these, so nothing crosses the boundary.
      break;
  }
  return capabilities;
}

std::vector<std::uint8_t> writeBytecodeImage(const Bytecode& root,
                                             std::uint32_t needs) {
  std::vector<std::uint8_t> out;
  out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
  appendU32(out, kOskcVersion);
  appendU32(out, 0);  // reserved flags
  appendU32(out, encodeAbiCapabilities(requiredAbiCapabilities(root)));
  appendU32(out, needs);
  writeChunk(out, root);
  return out;
}

Bytecode readBytecodeImage(const std::vector<std::uint8_t>& bytes) {
  Reader reader{bytes, 0};
  std::uint32_t version = 0;
  std::uint32_t abi = 0;
  std::uint32_t needs = 0;
  readHeader(reader, version, abi, needs);
  Bytecode root = readChunk(reader, 0);
  if (reader.offset != bytes.size()) {
    throw BytecodeIoError("bytecode image has " +
                          std::to_string(bytes.size() - reader.offset) +
                          " trailing bytes");
  }
  return root;
}

std::uint32_t readBytecodeImageAbi(const std::vector<std::uint8_t>& bytes) {
  Reader reader{bytes, 0};
  std::uint32_t version = 0;
  std::uint32_t abi = 0;
  std::uint32_t needs = 0;
  readHeader(reader, version, abi, needs);
  return abi;
}

std::uint32_t readBytecodeImageNeeds(const std::vector<std::uint8_t>& bytes) {
  Reader reader{bytes, 0};
  std::uint32_t version = 0;
  std::uint32_t abi = 0;
  std::uint32_t needs = 0;
  readHeader(reader, version, abi, needs);
  return needs;
}

std::uint32_t readBytecodeImageVersion(const std::vector<std::uint8_t>& bytes) {
  Reader reader{bytes, 0};
  std::uint32_t version = 0;
  std::uint32_t abi = 0;
  std::uint32_t needs = 0;
  readHeader(reader, version, abi, needs);
  return version;
}

bool isBytecodeImage(const std::vector<std::uint8_t>& bytes) {
  return bytes.size() >= 4 && std::memcmp(bytes.data(), kMagic, 4) == 0;
}

bool isBytecodeImagePath(const std::string& path) {
  constexpr const char* kExtension = ".oskc";
  constexpr std::size_t kLength = 5;
  // A bare extension is not a file name: `.oskc` has no stem to build an output
  // name from, so it is treated as "not an image" and reaches the source path,
  // which reports it as unreadable.
  if (path.size() <= kLength) return false;
  for (std::size_t i = 0; i < kLength; ++i) {
    const char actual = path[path.size() - kLength + i];
    const char expected = kExtension[i];
    const char lower = (actual >= 'A' && actual <= 'Z')
                           ? static_cast<char>(actual - 'A' + 'a')
                           : actual;
    if (lower != expected) return false;
  }
  return true;
}
