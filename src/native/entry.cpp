#include "native/entry.h"

#include "native/pe.h"

namespace {

// Linux x86-64 syscall numbers used by the prelude.
constexpr long kSysWrite = 1;
constexpr long kSysMmap = 9;
constexpr long kSysExitGroup = 231;

// mmap flags: a private, anonymous, read/write mapping (no file).
constexpr long kProtReadWrite = 3;
constexpr long kMapPrivateAnonymous = 0x22;

}  // namespace

std::uint64_t emitFreestandingRuntime(CodeGen& code, const RuntimeHandles& handles,
                                      CodeGen::Label programEntry) {
  // --- long oskel_write(long fd, const char* buf, long len) ---
  // Retries until `len` bytes are out (the Platform contract forbids short
  // writes) or gives up on the first error / zero progress. Returns the number
  // of bytes written.
  code.bind(handles.write);
  {
    CodeGen::Label loop = code.newLabel();
    CodeGen::Label done = code.newLabel();
    // r8 keeps the running total; rsi/rdx advance as bytes are written.
    code.xorReg(X64Reg::R8, X64Reg::R8);
    code.bind(loop);
    code.testReg(X64Reg::Rdx);
    code.jcc(Jcc::Le, done);  // nothing left to write
    code.movImm(X64Reg::Rax, kSysWrite);
    code.syscallInsn();
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Lt, done);  // error: rax < 0
    code.jcc(Jcc::Eq, done);  // no progress
    code.addReg(X64Reg::R8, X64Reg::Rax);
    code.addReg(X64Reg::Rsi, X64Reg::Rax);
    code.subReg(X64Reg::Rdx, X64Reg::Rax);
    code.jmp(loop);
    code.bind(done);
    code.movReg(X64Reg::Rax, X64Reg::R8);
    code.retInsn();
  }

  // --- void oskel_exit(long code) ---
  code.bind(handles.exit);
  code.movImm(X64Reg::Rax, kSysExitGroup);
  code.syscallInsn();
  code.ud2();  // unreachable: exit_group never returns

  // --- void oskel_init_arena(void) ---
  // mmap(NULL, kAotArenaSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS,
  //      -1, 0) and seed the allocator state. The kernel returns the region
  // base in rax (or a negative error, which the allocator later turns into a
  // null allocation). The cursor starts `kAotArenaFirstOffset` in, so no live
  // pointer is ever zero. The chunk entry calls this before running the
  // program, so a program lowered in isolation (a test that calls the chunk
  // directly) still has an arena.
  code.bind(handles.initArena);
  code.xorReg(X64Reg::Rdi, X64Reg::Rdi);            // addr = NULL
  code.movImm(X64Reg::Rsi, kAotArenaSize);          // length
  code.movImm(X64Reg::Rdx, kProtReadWrite);         // prot
  code.movImm(X64Reg::R10, kMapPrivateAnonymous);   // flags (syscall arg 4)
  code.movImm(X64Reg::R8, -1);                      // fd = -1
  code.xorReg(X64Reg::R9, X64Reg::R9);              // offset = 0
  code.movImm(X64Reg::Rax, kSysMmap);
  code.syscallInsn();
  code.movReg(kAotArenaCursor, X64Reg::Rax);
  code.addImm32(kAotArenaCursor, static_cast<std::int32_t>(kAotArenaFirstOffset));
  code.movReg(kAotArenaEnd, X64Reg::Rax);
  code.addImm32(kAotArenaEnd, static_cast<std::int32_t>(kAotArenaSize));
  code.retInsn();

  // --- _start: the ELF entry point ---
  // The kernel leaves argc at [rsp] and argv[] starting at [rsp+8]. Pass them
  // to the program entry as (argc, argv) and exit with its result. The chunk
  // entry maps the arena itself, so `_start` does not.
  std::uint64_t startOffset = code.offset();
  code.movFromMem(X64Reg::Rdi, X64Reg::Rsp, 0);   // argc
  code.movReg(X64Reg::Rsi, X64Reg::Rsp);
  code.addImm32(X64Reg::Rsi, 8);                  // argv
  code.callLabel(programEntry);
  code.movReg(X64Reg::Rdi, X64Reg::Rax);          // exit code = main's result
  code.callLabel(handles.exit);
  code.ud2();  // oskel_exit never returns

  return startOffset;
}

// ---------------------------------------------------------------------------
// Windows x86-64 prelude (PHASE 7e)
//
// Windows has no stable syscall interface, so the prelude reaches the system
// through kernel32.dll with the Microsoft x64 ABI: the first four arguments go
// in rcx/rdx/r8/r9, the caller reserves 32 bytes of shadow space and keeps rsp
// 16-byte aligned, and the return value is in rax. Only these three thunks see
// that ABI; the embedded runtime keeps the lowering's own convention.
//
// Every call goes through the IAT slot the PE emitter publishes, addressed
// RIP-relative with a constant displacement (the .idata section sits at a fixed
// RVA, so `slotRva - (textRva + callSite + 6)` does not depend on the program).
namespace {

constexpr std::int64_t kWinStdOutputHandle = -11;  // GetStdHandle(STD_OUTPUT_HANDLE)
constexpr std::int64_t kWinStdErrorHandle = -12;   // GetStdHandle(STD_ERROR_HANDLE)
constexpr std::int64_t kWinMemCommitReserve = 0x3000;  // MEM_COMMIT | MEM_RESERVE
constexpr std::int64_t kWinPageReadWrite = 0x04;       // PAGE_READWRITE
constexpr std::int64_t kWinShadowSpace = 32;

// `call qword ptr [rip + delta]` to an imported function, with `delta` measured
// from the end of the instruction (six bytes: FF 15 + disp32).
void callImport(CodeGen& code, PeImport import) {
  const std::int64_t callSite = static_cast<std::int64_t>(code.offset());
  const std::int64_t next = static_cast<std::int64_t>(kPeTextRva) + callSite + 6;
  const std::int64_t delta =
      static_cast<std::int64_t>(importIatRva(import)) - next;
  code.callRipRel(static_cast<std::int32_t>(delta));
}

}  // namespace

std::uint64_t emitWindowsRuntime(CodeGen& code, const RuntimeHandles& handles,
                                 CodeGen::Label programEntry) {
  // --- long oskel_write(long fd, const char* buf, long len) ---
  // Writes to the fd's standard handle in a retry loop, so the Platform
  // contract's "no short writes" holds here too. The loop state lives in the
  // frame (WriteFile clobbers every volatile register) and the handle in rbx
  // (callee-saved), so nothing the program owns is disturbed: r13/r14/r15 carry
  // the arena and the global table and must survive this call.
  //
  // The frame is laid out for the Microsoft x64 ABI. With `push rbp; push rbx`
  // and an 88-byte frame, rsp is rbp-96 and 16-byte aligned:
  //   [rbp-96 .. rbp-65]  shadow space (32 bytes, mandatory before a call)
  //   [rbp-64]            the 5th argument slot: lpOverlapped = NULL
  //   [rbp-56] fd, [rbp-48] buf, [rbp-40] len, [rbp-32] total,
  //   [rbp-24] bytes written (WriteFile's out-parameter)
  code.bind(handles.write);
  {
    CodeGen::Label loop = code.newLabel();
    CodeGen::Label done = code.newLabel();

    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.push(X64Reg::Rbx);
    code.subRegImm32(X64Reg::Rsp, 88);
    code.movToMem(X64Reg::Rbp, -56, X64Reg::Rdi);  // fd
    code.movToMem(X64Reg::Rbp, -48, X64Reg::Rsi);  // buf
    code.movToMem(X64Reg::Rbp, -40, X64Reg::Rdx);  // len
    code.movImmToMem(X64Reg::Rbp, -32, 0);         // total written
    code.movImmToMem(X64Reg::Rbp, -64, 0);         // lpOverlapped = NULL

    // rbx = GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE).
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, -56);
    code.cmpImm32(X64Reg::Rax, 1);
    CodeGen::Label isStdout = code.newLabel();
    code.jcc(Jcc::Eq, isStdout);
    code.movImm(X64Reg::Rcx, kWinStdErrorHandle);
    CodeGen::Label haveHandleArg = code.newLabel();
    code.jmp(haveHandleArg);
    code.bind(isStdout);
    code.movImm(X64Reg::Rcx, kWinStdOutputHandle);
    code.bind(haveHandleArg);
    callImport(code, PeImport::GetStdHandle);
    code.movReg(X64Reg::Rbx, X64Reg::Rax);

    code.bind(loop);
    code.movFromMem(X64Reg::Rdx, X64Reg::Rbp, -40);  // remaining
    code.testReg(X64Reg::Rdx);
    code.jcc(Jcc::Le, done);

    code.movReg(X64Reg::Rcx, X64Reg::Rbx);           // hFile
    code.movFromMem(X64Reg::Rdx, X64Reg::Rbp, -48);  // lpBuffer
    code.movFromMem(X64Reg::R8, X64Reg::Rbp, -40);   // nNumberOfBytesToWrite
    code.leaMem(X64Reg::R9, X64Reg::Rbp, -24);       // lpNumberOfBytesWritten
    code.movImmToMem(X64Reg::Rbp, -24, 0);
    callImport(code, PeImport::WriteFile);

    // WriteFile returns non-zero on success. On failure, stop; the bytes that
    // did go out are already counted.
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, done);

    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, -24);  // bytes written
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, done);  // no progress

    code.movFromMem(X64Reg::Rdx, X64Reg::Rbp, -32);
    code.addReg(X64Reg::Rdx, X64Reg::Rax);
    code.movToMem(X64Reg::Rbp, -32, X64Reg::Rdx);  // total += written
    code.movFromMem(X64Reg::Rdx, X64Reg::Rbp, -48);
    code.addReg(X64Reg::Rdx, X64Reg::Rax);
    code.movToMem(X64Reg::Rbp, -48, X64Reg::Rdx);  // buf += written
    code.movFromMem(X64Reg::Rdx, X64Reg::Rbp, -40);
    code.subReg(X64Reg::Rdx, X64Reg::Rax);
    code.movToMem(X64Reg::Rbp, -40, X64Reg::Rdx);  // len -= written
    code.jmp(loop);

    code.bind(done);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, -32);
    code.addImm32(X64Reg::Rsp, 88);
    code.pop(X64Reg::Rbx);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  // --- void oskel_exit(long code) ---
  // ExitProcess(rcx) never returns; the ud2 is unreachable.
  code.bind(handles.exit);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  callImport(code, PeImport::ExitProcess);
  code.ud2();

  // --- void oskel_init_arena(void) ---
  // VirtualAlloc(NULL, kAotArenaSize, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE).
  // Returns the region base in rax (or NULL, which the allocator later turns
  // into a null allocation). The cursor starts `kAotArenaFirstOffset` in, so no
  // live pointer is ever zero.
  code.bind(handles.initArena);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.xorReg(X64Reg::Rcx, X64Reg::Rcx);                    // lpAddress = NULL
  code.movImm(X64Reg::Rdx, kAotArenaSize);                  // dwSize
  code.movImm(X64Reg::R8, kWinMemCommitReserve);            // flAllocationType
  code.movImm(X64Reg::R9, kWinPageReadWrite);               // flProtect
  callImport(code, PeImport::VirtualAlloc);
  code.movReg(kAotArenaCursor, X64Reg::Rax);
  code.addImm32(kAotArenaCursor, static_cast<std::int32_t>(kAotArenaFirstOffset));
  code.movReg(kAotArenaEnd, X64Reg::Rax);
  code.addImm32(kAotArenaEnd, static_cast<std::int32_t>(kAotArenaSize));
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // --- the entry point ---
  // The loader calls it with no arguments (and rsp 8 mod 16). Reserve the
  // shadow space the calls below need, run the program entry and exit with its
  // result. The chunk entry maps the arena itself, so the entry point does not.
  std::uint64_t startOffset = code.offset();
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.callLabel(programEntry);
  code.movReg(X64Reg::Rdi, X64Reg::Rax);  // exit code = main's result
  code.callLabel(handles.exit);
  code.ud2();  // oskel_exit never returns

  return startOffset;
}
