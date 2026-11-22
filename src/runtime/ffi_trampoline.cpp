#include "ffi_trampoline.h"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <utility>

#include "native/codegen.h"
#include "native/target.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace ffi {
namespace {

#if OSKEL_FFI_TRAMPOLINE
constexpr X64Reg kGprArg[6] = {X64Reg::Rdi, X64Reg::Rsi, X64Reg::Rdx,
                               X64Reg::Rcx, X64Reg::R8,  X64Reg::R9};
constexpr X64Reg kWin64GprArg[4] = {X64Reg::Rcx, X64Reg::Rdx, X64Reg::R8, X64Reg::R9};

// The args buffer the callback stub hands the C++ body: six GPR slots followed
// by eight SSE slots, each eight bytes wide. The stack arguments follow.
constexpr std::int64_t kCallbackGprBytes = 6 * 8;
constexpr std::int64_t kCallbackSseBytes = 8 * 8;
constexpr std::int64_t kCallbackArgsBytes = kCallbackGprBytes + kCallbackSseBytes;

std::size_t pageSize() {
#if defined(_WIN32)
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  return info.dwPageSize == 0 ? 4096 : static_cast<std::size_t>(info.dwPageSize);
#else
  const long size = sysconf(_SC_PAGESIZE);
  return size > 0 ? static_cast<std::size_t>(size) : 4096;
#endif
}

std::size_t roundToPage(std::size_t bytes) {
  const std::size_t page = pageSize();
  std::size_t span = page;
  while (span < bytes) span += page;
  return span;
}

std::vector<std::uint8_t> emitCallStub(bool win64) {
  CodeGen cg(win64 ? Target::winX64() : Target::linuxX64());

  // rsp is 8 mod 16 on entry. Three pushes make it 0 mod 16; the outgoing
  // area is a multiple of 16, so the call sees the alignment the ABI wants.
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.push(X64Reg::Rbx);
  cg.push(X64Reg::R12);
  cg.movReg(X64Reg::Rbx, win64 ? X64Reg::Rcx : X64Reg::Rdi);

  // The outgoing area is the stack arguments (plus the Win64 shadow space),
  // rounded up to 16. R12 holds the aligned size while rsp is adjusted.
  cg.movFromMem32(X64Reg::R12, X64Reg::Rbx, offsetof(CallDescriptor, stackBytes));
  if (win64) cg.addImm32(X64Reg::R12, 32);
  cg.addImm32(X64Reg::R12, 15);
  cg.andImm32(X64Reg::R12, -16);
  cg.subReg(X64Reg::Rsp, X64Reg::R12);

  if (win64) {
    // The callee is allowed to spill its four register arguments into this
    // shadow space; zero it so a register the caller did not set is not a
    // stale value from an earlier call.
    for (int i = 0; i < 4; ++i) cg.movImmToMem(X64Reg::Rsp, i * 8, 0);
  }

  // Copy the stack arguments into the outgoing area. The ABI reads them from
  // the caller's rsp (Win64: rsp + 32, past the shadow space).
  cg.movFromMem32(X64Reg::R12, X64Reg::Rbx, offsetof(CallDescriptor, stackBytes));
  cg.testReg(X64Reg::R12);
  CodeGen::Label noStack = cg.newLabel();
  cg.jcc(Jcc::Eq, noStack);
  cg.movFromMem(X64Reg::Rsi, X64Reg::Rbx, offsetof(CallDescriptor, stack));
  cg.movReg(X64Reg::Rdi, X64Reg::Rsp);
  if (win64) cg.addImm32(X64Reg::Rdi, 32);
  cg.movFromMem32(X64Reg::Rcx, X64Reg::Rbx, offsetof(CallDescriptor, stackBytes));
  cg.cld();
  cg.repMovsb();
  cg.bind(noStack);

  const int gprCount = win64 ? 4 : 6;
  for (int i = 0; i < gprCount; ++i) {
    cg.movFromMem(win64 ? kWin64GprArg[i] : kGprArg[i], X64Reg::Rbx,
                  offsetof(CallDescriptor, gpr) + 8 * i);
  }
  const int sseCount = win64 ? 4 : 8;
  for (int i = 0; i < sseCount; ++i) {
    cg.movsdToXmm(kXmmBase + i, X64Reg::Rbx, offsetof(CallDescriptor, sse) + 8 * i);
  }

  // System V variadics read %al to know how many xmm registers to spill. A
  // non-variadic callee ignores it; Win64 has no such rule.
  cg.movFromMem32(X64Reg::Rax, X64Reg::Rbx, offsetof(CallDescriptor, vectorCount));
  cg.movFromMem(X64Reg::R11, X64Reg::Rbx, offsetof(CallDescriptor, function));
  cg.callReg(X64Reg::R11);

  // Store the raw return registers; the caller knows which one its declared
  // return class reads.
  cg.movFromMem(X64Reg::R12, X64Reg::Rbx, offsetof(CallDescriptor, ret));
  cg.movToMem(X64Reg::R12, 0, X64Reg::Rax);
  cg.movToMem(X64Reg::R12, 8, X64Reg::Rdx);
  cg.movsdFromXmm(X64Reg::R12, 16, kXmmBase + 0);
  cg.movsdFromXmm(X64Reg::R12, 24, kXmmBase + 1);

  cg.leaMem(X64Reg::Rsp, X64Reg::Rbp, -16);
  cg.pop(X64Reg::R12);
  cg.pop(X64Reg::Rbx);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();
  cg.finalize();
  return cg.code();
}

std::vector<std::uint8_t> emitCallbackStubBytes(const CallbackStubLayout& layout) {
  CodeGen cg(Target::linuxX64());

  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.push(X64Reg::Rbx);
  cg.push(X64Reg::R12);
  cg.subRegImm32(X64Reg::Rsp, static_cast<std::int32_t>(layout.frameBytes));

  // A MEMORY struct return uses the hidden sret pointer in rdi. Save it before
  // rdi is reused for the body's first argument.
  if (layout.memoryReturn) {
    cg.movReg(X64Reg::R12, X64Reg::Rdi);
  } else {
    cg.xorReg(X64Reg::R12, X64Reg::R12);
  }

  for (int i = 0; i < 6; ++i) {
    cg.movToMem(X64Reg::Rsp, 8 * i, kGprArg[i]);
  }
  for (int i = 0; i < 8; ++i) {
    cg.movsdFromXmm(X64Reg::Rsp, kCallbackGprBytes + 8 * i, kXmmBase + i);
  }

  // Copy the stack arguments the C caller placed after the return address.
  if (layout.stackBytes != 0) {
    cg.movFromMem(X64Reg::Rsi, X64Reg::Rbp, 16);
    cg.leaMem(X64Reg::Rdi, X64Reg::Rsp, kCallbackArgsBytes);
    cg.movImm(X64Reg::Rcx, layout.stackBytes);
    cg.cld();
    cg.repMovsb();
  }

  cg.movImm(X64Reg::Rdi, reinterpret_cast<std::int64_t>(layout.context));
  cg.movReg(X64Reg::Rsi, X64Reg::Rsp);
  cg.leaMem(X64Reg::Rdx, X64Reg::Rsp, static_cast<std::int64_t>(layout.argsBytes));
  cg.movReg(X64Reg::Rcx, X64Reg::R12);
  cg.movImm(X64Reg::Rax, reinterpret_cast<std::int64_t>(layout.body));
  cg.callReg(X64Reg::Rax);

  cg.leaMem(X64Reg::R10, X64Reg::Rsp, static_cast<std::int64_t>(layout.argsBytes));
  switch (layout.ret) {
    case StubReturn::Void:
      break;
    case StubReturn::Int:
      cg.movFromMem(X64Reg::Rax, X64Reg::R10, 0);
      break;
    case StubReturn::Float:
    case StubReturn::Double:
      cg.movsdToXmm(kXmmBase + 0, X64Reg::R10, 0);
      break;
    case StubReturn::I1:
      cg.movFromMem(X64Reg::Rax, X64Reg::R10, 0);
      break;
    case StubReturn::S1:
      cg.movsdToXmm(kXmmBase + 0, X64Reg::R10, 0);
      break;
    case StubReturn::II:
      cg.movFromMem(X64Reg::Rax, X64Reg::R10, 0);
      cg.movFromMem(X64Reg::Rdx, X64Reg::R10, 8);
      break;
    case StubReturn::SS:
      cg.movsdToXmm(kXmmBase + 0, X64Reg::R10, 0);
      cg.movsdToXmm(kXmmBase + 1, X64Reg::R10, 8);
      break;
    case StubReturn::IS:
      cg.movFromMem(X64Reg::Rax, X64Reg::R10, 0);
      cg.movsdToXmm(kXmmBase + 0, X64Reg::R10, 8);
      break;
    case StubReturn::SI:
      cg.movsdToXmm(kXmmBase + 0, X64Reg::R10, 0);
      cg.movFromMem(X64Reg::Rax, X64Reg::R10, 8);
      break;
    case StubReturn::Memory:
      cg.movReg(X64Reg::Rax, X64Reg::R12);
      break;
  }

  cg.leaMem(X64Reg::Rsp, X64Reg::Rbp, -16);
  cg.pop(X64Reg::R12);
  cg.pop(X64Reg::Rbx);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();
  cg.finalize();
  return cg.code();
}
#endif

}  // namespace

ExecutableBuffer::~ExecutableBuffer() { release(); }

ExecutableBuffer::ExecutableBuffer(ExecutableBuffer&& other) noexcept
    : mapping_(other.mapping_), size_(other.size_), entry_(other.entry_) {
  other.mapping_ = nullptr;
  other.size_ = 0;
  other.entry_ = nullptr;
}

ExecutableBuffer& ExecutableBuffer::operator=(ExecutableBuffer&& other) noexcept {
  if (this != &other) {
    release();
    mapping_ = other.mapping_;
    size_ = other.size_;
    entry_ = other.entry_;
    other.mapping_ = nullptr;
    other.size_ = 0;
    other.entry_ = nullptr;
  }
  return *this;
}

void ExecutableBuffer::release() {
#if defined(_WIN32)
  if (mapping_ != nullptr) VirtualFree(mapping_, 0, MEM_RELEASE);
#else
  if (mapping_ != nullptr) munmap(mapping_, size_);
#endif
  mapping_ = nullptr;
  size_ = 0;
  entry_ = nullptr;
}

bool ExecutableBuffer::assign(const std::vector<std::uint8_t>& bytes) {
  release();
  if (bytes.empty()) return false;
  const std::size_t span = roundToPage(bytes.size());
#if defined(_WIN32)
  void* mem = VirtualAlloc(nullptr, span, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (mem == nullptr) return false;
  std::memcpy(mem, bytes.data(), bytes.size());
  DWORD previous = 0;
  if (VirtualProtect(mem, span, PAGE_EXECUTE_READ, &previous) == 0) {
    VirtualFree(mem, 0, MEM_RELEASE);
    return false;
  }
  FlushInstructionCache(GetCurrentProcess(), mem, bytes.size());
#else
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return false;
  std::memcpy(mem, bytes.data(), bytes.size());
  if (mprotect(mem, span, PROT_READ | PROT_EXEC) != 0) {
    munmap(mem, span);
    return false;
  }
#endif
  mapping_ = mem;
  size_ = span;
  entry_ = mem;
  return true;
}

#if OSKEL_FFI_TRAMPOLINE
CallStub callStub(bool win64) {
  static std::mutex mutex;
  static ExecutableBuffer sysv;
  static ExecutableBuffer win64Buffer;
  static CallStub sysvStub = nullptr;
  static CallStub win64Stub = nullptr;

  std::lock_guard<std::mutex> lock(mutex);
  CallStub& cached = win64 ? win64Stub : sysvStub;
  if (cached != nullptr) return cached;
  try {
    ExecutableBuffer& buffer = win64 ? win64Buffer : sysv;
    if (!buffer.assign(emitCallStub(win64))) return nullptr;
    cached = reinterpret_cast<CallStub>(const_cast<void*>(buffer.entry()));
    return cached;
  } catch (...) {
    return nullptr;
  }
}

bool emitCallbackStub(const CallbackStubLayout& layout, std::vector<std::uint8_t>& out) {
  // The Microsoft x64 callback body is not part of the hosted surface yet, so
  // the compiler rejects a callback there; refuse to emit one rather than
  // producing a stub that would mis-read the stack arguments.
  if (layout.win64) return false;
  try {
    out = emitCallbackStubBytes(layout);
    return !out.empty();
  } catch (...) {
    return false;
  }
}
#endif

}  // namespace ffi
