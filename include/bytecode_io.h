#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "bytecode.h"
#include "ffi_types.h"
#include "native/target.h"

// The `.oskc` bytecode image (PHASE 6i).
//
// A `.oskc` file is the compiled form of a program, independent of the host
// that wrote it: the same file runs on the VM and lowers to a native image on
// every target the AOT backend supports. Portability is an invariant of the
// format, not a goal, and it shows in three rules:
//
//  1. Every field is written with an explicit little-endian encoding. No
//     `memcpy` of a C++ struct and no `sizeof`: a file written on x86-64 reads
//     on arm64 and on any future big-endian target.
//  2. No host ABI leaks into the file. The FFI surface the chunk *needs* is
//     recorded as a capability bitset (`AbiCapabilities`), derived from its
//     `extern "C"` signatures, and the loader checks it against the
//     destination instead of trusting the host that compiled the file. A
//     `.oskc` whose signatures never cross the C boundary writes zero and loads
//     anywhere; one that marshals a struct by value writes that bit, and a
//     Windows destination can refuse exactly the part it cannot marshal, at
//     load time, with a message -- never a mis-compiled binary.
//  3. The loader validates every index it reads (opcode, constant, child,
//     upvalue, jump target) before the VM or the backend sees it, so a
//     truncated or hand-edited file is a clean error rather than a crash.
//
// Layout (all integers little-endian):
//
//   magic    "OSKC"                       4 bytes
//   version  u32                          format version (kOskcVersion)
//   flags    u32                          reserved, must be 0
//   abi      u32                          AbiCapabilities bitset
//   needs    u32                          Requirements bitset
//   chunk    <chunk>                      the root proto
//
//   <chunk>  name          string
//            numParams     i32
//            numRegisters  i32
//            numLocals     i32
//            variadic      u8
//            code          u32 count, then count * <instruction>
//            constants     u32 count, then count * <constant>
//            children      u32 count, then count * <chunk>
//            upvalues      u32 count, then count * <upvalue>
//            externs       u32 count, then count * <extern>
//            nativeFns     u32 count, then count * string
//            nativeUses    u32 count, then count * string
//            usedMethods   u32 count, then count * string
//
//   string        u32 length, then length bytes (no terminator)
//   instruction   op u8, a/b/c/jump/d/e i32, line/col i32
//   constant      tag u8, then the payload the tag names
//                   Nil: none
//                   Bool: u8
//                   Int: i64
//                   Float: binary32 (4 bytes)
//                   Double: binary64 (8 bytes)
//                   Str: string
//   upvalue       inParentLocal u8, index i32
//   extern        name string, variadic u8, returnType <typespec>,
//                 paramTypes u32 count then count * <typespec>
//   typespec      scalarType string, isStruct u8, fields <ffiField list>,
//                 isCallback u8, callbackParams <string list>,
//                 callbackReturn string, callbackReturnsStruct u8,
//                 callbackReturnFields <ffiField list>
//   ffiField      name string, type string
//
// The constant pool only ever holds nil/bool/int/float/double/string: those are
// the values `LOADK` can load, and the serializer rejects anything else instead
// of writing a file the loader could not rebuild.

constexpr std::uint32_t kOskcVersion = 1;

// The header is fixed width: magic, version, reserved flags, ABI bits and
// requirement bits. The tests and any external reader use this rather than a
// literal, so adding a header field cannot silently shift their offsets.
constexpr std::size_t kOskcHeaderSize = 20;

// What the chunk needs from its destination beyond the C ABI. These are
// recorded for the same reason as the ABI bits: the loader must be able to tell
// a program that needs the UI backend from one that does not, and the modules
// that pull it in cannot be recovered from the chunk (the Skia bindings use the
// ambiguous `oskel_` prefix, so scanning `externs` would miss them). The writer
// derives the bits from the modules the compiler loaded.
constexpr std::uint32_t kOskcNeedsSdl = 1u << 0;
constexpr std::uint32_t kOskcNeedsSkia = 1u << 1;

// The ABI capability bits, so the file states what it needs without depending
// on how the writing engine was built or which host wrote it.
constexpr std::uint32_t kOskcAbiStructByValue = 1u << 0;
constexpr std::uint32_t kOskcAbiMemoryStructs = 1u << 1;
constexpr std::uint32_t kOskcAbiVariadic = 1u << 2;
constexpr std::uint32_t kOskcAbiCallbacks = 1u << 3;

// Guards against a malicious or corrupt file: a chunk nested deeper than this
// is rejected instead of exhausting the stack during the recursive load.
constexpr int kOskcMaxDepth = 200;

// Encodes a capability set to its bits, and back. Unknown bits are ignored on
// decode so a newer file's extra capabilities do not break an older loader.
std::uint32_t encodeAbiCapabilities(const ffi::AbiCapabilities& capabilities);
ffi::AbiCapabilities decodeAbiCapabilities(std::uint32_t bits);

// The ABI capabilities `root` actually needs, computed from the `extern "C"`
// declarations it carries (recursively, through the children).
//
// This is what the writer records, and it is deliberately *not*
// `kAotAbiCapabilities`: writing the superset would make every file claim it
// needs structs by value, variadics and callbacks, and the load-time check
// would then be unable to distinguish a program that uses them from one that
// does not -- the portability guarantee would be decorative. A chunk with no
// FFI needs nothing and loads everywhere; a chunk whose signature marshals a
// class-MEMORY struct needs `memoryStructs`, and a target without it refuses
// exactly that file.
ffi::AbiCapabilities requiredAbiCapabilities(const Bytecode& root);

// True when `destination` can run a chunk validated against `required`: every
// capability the file needs is one the destination has. A `.oskc` that uses
// only scalars runs everywhere; one that marshals a struct by value is refused
// on a target whose ABI cannot carry it.
bool abiSatisfies(const ffi::AbiCapabilities& destination,
                  const ffi::AbiCapabilities& required);

// The FFI capabilities a build target marshals. The AOT backend's thunks follow
// the target's C ABI, so this is a property of the destination, not of the host
// that compiled the file:
//
//  - System V x86-64 (Linux, macOS, bare metal): the full surface. Register and
//    class-MEMORY structs, C variadics and callbacks all have a thunk.
//  - Microsoft x64 (Windows): scalars and pointers, including a C variadic. A
//    struct crosses by reference there and the thunk does not build it, and a
//    callback needs the System V trampoline, so both are refused -- exactly the
//    shapes `collectExternThunks` rejects.
//  - WebAssembly: the module has no C FFI at all, so it needs nothing and
//    marshals nothing.
//
// A `.oskc` is checked against this before lowering, so a file written on Linux
// that uses a struct by value is refused by the Windows build with a message
// instead of reaching the thunk builder.
ffi::AbiCapabilities targetAbiCapabilities(const Target& target);

// Raised for any malformed or incompatible `.oskc`. Carries a message naming
// the offending field, so a bad file is diagnosable without a debugger.
class BytecodeIoError : public std::runtime_error {
 public:
  explicit BytecodeIoError(const std::string& message)
      : std::runtime_error(message) {}
};

// Serializes `root` to a `.oskc` image. Throws `BytecodeIoError` when the
// chunk holds a value the format cannot represent (a constant that is not a
// scalar or a string) -- which the compiler never produces, so it is a guard
// against a hand-built proto, not a user-facing path.
//
// `needs` is the requirement bitset (`kOskcNeedsSdl`, `kOskcNeedsSkia`). It is
// an argument rather than something derived from `root` because the modules a
// program imported are the compiler's knowledge, not the chunk's: the Skia
// bindings use the ambiguous `oskel_` prefix, so the chunk alone cannot say
// whether it needs the shim. The ABI bitset, by contrast, is derived here
// because it is a property of the `extern "C"` signatures the chunk carries.
std::vector<std::uint8_t> writeBytecodeImage(const Bytecode& root,
                                             std::uint32_t needs = 0);

// Parses a `.oskc` image, validating the header and every index. Throws
// `BytecodeIoError` on any defect. The returned root is a fully rebuilt
// `Bytecode`, ready for `VM::run` or for the AOT lowerer.
Bytecode readBytecodeImage(const std::vector<std::uint8_t>& bytes);

// The ABI bitset recorded in an image, without rebuilding the chunk. Lets a
// destination refuse an incompatible file before doing any work, and lets the
// tests read the header cheaply.
std::uint32_t readBytecodeImageAbi(const std::vector<std::uint8_t>& bytes);

// The requirement bitset recorded in an image (`kOskcNeedsSdl`,
// `kOskcNeedsSkia`), read from the header without rebuilding the chunk.
std::uint32_t readBytecodeImageNeeds(const std::vector<std::uint8_t>& bytes);

// The format version recorded in an image.
std::uint32_t readBytecodeImageVersion(const std::vector<std::uint8_t>& bytes);

// True when `bytes` starts with the `.oskc` magic. Used by the CLI to route a
// path to the loader instead of the parser.
bool isBytecodeImage(const std::vector<std::uint8_t>& bytes);

// Whether `path` names a bytecode image by extension. The CLI uses this to
// decide between "compile this source" and "load this chunk" before reading it,
// so a missing file reports the right error.
bool isBytecodeImagePath(const std::string& path);
