#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Runtime trampolines for the development engine's `extern "C"` FFI.
//
// The fixed scalar and register-struct calls use the compile-time thunks in
// `ffi.cpp`; the three shapes those thunks cannot express get a small x86-64
// stub instead of a host FFI library:
//
//   - a C variadic, whose tail length is only known at the call site;
//   - a class-MEMORY struct, passed on the stack and returned through `sret`;
//   - a callback, whose C entry point has to re-enter the VM.
//
// `callStub` emits one descriptor-driven call stub per ABI (System V or
// Microsoft x64) and caches it; `emitCallbackStub` emits one closure stub per
// callback signature. Both use the AOT backend's `CodeGen`, so the emitted
// instructions are the same ones the hosted backend already runs.
// The runtime stubs are x86-64 machine code. Other hosts keep the fixed-scalar
// typed thunk, but cannot marshal a variadic, a MEMORY struct or a callback;
// the compiler's capability flags report that and reject the declaration.
#if defined(__x86_64__) || defined(_M_X64)
#define OSKEL_FFI_TRAMPOLINE 1
#else
#define OSKEL_FFI_TRAMPOLINE 0
#endif

namespace ffi {

// A callback's C return class, in the form the stub has to load into the
// registers the ABI assigns.
enum class StubReturn : std::uint8_t {
  Void,
  Int,
  Float,
  Double,
  I1,
  S1,
  II,
  SS,
  IS,
  SI,
  Memory,
};

// Everything one dynamic call needs. The caller fills the slots the ABI
// assigns, then hands the descriptor to the stub, which loads the registers,
// copies the outgoing stack area, calls `function` and stores the raw return
// registers in `ret` (rax, rdx, xmm0, xmm1 at offsets 0, 8, 16 and 24).
struct CallDescriptor {
  void* function = nullptr;
  std::uint32_t gprCount = 0;
  std::uint32_t sseCount = 0;
  std::uint32_t stackBytes = 0;
  std::uint32_t vectorCount = 0;
  std::uint64_t gpr[6] = {};
  std::uint64_t sse[8] = {};
  const std::uint8_t* stack = nullptr;
  std::uint8_t* ret = nullptr;
};

// The frame one callback stub builds. `argsBytes` covers six GPR slots, eight
// SSE slots and the stack arguments; `frameBytes` is that plus the 32-byte
// return buffer, rounded to 16 bytes.
struct CallbackStubLayout {
  bool win64 = false;
  std::uint32_t gprCount = 0;
  std::uint32_t sseCount = 0;
  std::uint32_t stackBytes = 0;
  std::size_t argsBytes = 0;
  std::size_t frameBytes = 0;
  StubReturn ret = StubReturn::Void;
  bool memoryReturn = false;
  const void* context = nullptr;
  const void* body = nullptr;
};

// A private executable page. The mapping is writable while the bytes are
// copied and executable afterwards, never both at once.
class ExecutableBuffer {
 public:
  ExecutableBuffer() = default;
  ~ExecutableBuffer();

  ExecutableBuffer(const ExecutableBuffer&) = delete;
  ExecutableBuffer& operator=(const ExecutableBuffer&) = delete;
  ExecutableBuffer(ExecutableBuffer&& other) noexcept;
  ExecutableBuffer& operator=(ExecutableBuffer&& other) noexcept;

  bool assign(const std::vector<std::uint8_t>& bytes);
  void release();

  const void* entry() const { return entry_; }

 private:
  void* mapping_ = nullptr;
  std::size_t size_ = 0;
  void* entry_ = nullptr;
};

#if OSKEL_FFI_TRAMPOLINE
using CallStub = void (*)(CallDescriptor*);

// The cached call stub for the host ABI, or nullptr when executable memory is
// unavailable. The stub is immutable and reentrant: every call passes its own
// descriptor.
CallStub callStub(bool win64);

// Emits one callback closure stub into `out`. Returns false for an ABI the
// stub does not implement (Win64 callbacks are rejected by the compiler).
bool emitCallbackStub(const CallbackStubLayout& layout, std::vector<std::uint8_t>& out);
#endif

}  // namespace ffi
