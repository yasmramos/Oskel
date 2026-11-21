#include "native/entry.h"

#include "native/pe.h"

namespace {

// Linux x86-64 syscall numbers used by the prelude.
constexpr long kSysRead = 0;
constexpr long kSysWrite = 1;
constexpr long kSysClose = 3;
constexpr long kSysMmap = 9;
constexpr long kSysClockGettime = 228;
constexpr long kSysExitGroup = 231;
constexpr long kSysOpenAt = 257;
// `time(NULL)`: the wall clock as seconds since the Unix epoch. It is the one
// value the runtime cannot compute from memory, and the kernel's own clock is
// already in UTC, so no timezone handling is needed.
constexpr long kSysTime = 201;

// mmap flags: a private, anonymous, read/write mapping (no file).
constexpr long kProtReadWrite = 3;
constexpr long kMapPrivateAnonymous = 0x22;

// `clock_gettime` takes one of these; `CLOCK_MONOTONIC` cannot go backwards and
// is unaffected by a wall-clock adjustment, which is what `clock()` reports.
constexpr long kClockMonotonic = 1;

// `openat` flags for the two modes `oskel_open` supports.
constexpr long kAtFdcwd = -100;
constexpr long kOpenReadOnly = 0;
constexpr long kOpenWrite = 1 | 64 | 512;  // O_WRONLY | O_CREAT | O_TRUNC

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

  // --- long oskel_time(void) ---
  // `time(NULL)`: seconds since the Unix epoch, in rax. The kernel returns the
  // current UTC wall clock, so no timezone shift is applied here (a local time
  // is `std.datetime`'s job, and it is a pure function of this value plus an
  // offset).
  code.bind(handles.now);
  code.xorReg(X64Reg::Rdi, X64Reg::Rdi);  // tloc = NULL
  code.movImm(X64Reg::Rax, kSysTime);
  code.syscallInsn();
  code.retInsn();

  // --- long oskel_monotonic(void) ---
  // `clock_gettime(CLOCK_MONOTONIC, &ts)` in nanoseconds. The kernel's
  // `timespec` is two 64-bit fields on x86-64, so the 16-byte frame below `rsp`
  // is enough and no alignment fix-up is needed (the syscall does not require
  // one). `clock()` subtracts the origin, so only the differences matter.
  code.bind(handles.monotonic);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 16);
  code.movImm(X64Reg::Rdi, kClockMonotonic);
  code.movReg(X64Reg::Rsi, X64Reg::Rsp);
  code.movImm(X64Reg::Rax, kSysClockGettime);
  code.syscallInsn();
  // A failed call (rax < 0) leaves the frame zeroed, which reports "no time
  // passed" instead of a wild value.
  code.movFromMem(X64Reg::R8, X64Reg::Rsp, 0);   // tv_sec
  code.movFromMem(X64Reg::R9, X64Reg::Rsp, 8);   // tv_nsec
  code.movImm(X64Reg::Rax, 1000000000);
  code.imulReg(X64Reg::Rax, X64Reg::R8);
  code.addReg(X64Reg::Rax, X64Reg::R9);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // --- long oskel_open(const char* path, long mode) ---
  // `openat(AT_FDCWD, path, flags, 0644)`. The mode only selects the access
  // flags: 0 reads, anything else truncates and creates. A negative return is
  // the errno and the caller reports the failure; -1 is never a valid fd.
  code.bind(handles.open);
  code.movReg(X64Reg::R8, X64Reg::Rdi);   // path
  code.testReg(X64Reg::Rsi);              // mode: 0 reads, else writes
  CodeGen::Label reading = code.newLabel();
  code.jcc(Jcc::Eq, reading);
  code.movImm(X64Reg::Rdx, kOpenWrite);
  CodeGen::Label opened = code.newLabel();
  code.jmp(opened);
  code.bind(reading);
  code.movImm(X64Reg::Rdx, kOpenReadOnly);
  code.bind(opened);
  code.movReg(X64Reg::Rsi, X64Reg::R8);   // path
  code.movImm(X64Reg::Rdi, kAtFdcwd);     // dirfd
  code.movImm(X64Reg::R10, 0644);         // mode for O_CREAT
  code.movImm(X64Reg::Rax, kSysOpenAt);
  code.syscallInsn();
  code.retInsn();

  // --- long oskel_read_fd(long fd, char* buf, long len) ---
  code.bind(handles.readFd);
  code.movImm(X64Reg::Rax, kSysRead);
  code.syscallInsn();
  code.retInsn();

  // --- long oskel_write_fd(long fd, const char* buf, long len) ---
  code.bind(handles.writeFd);
  code.movImm(X64Reg::Rax, kSysWrite);
  code.syscallInsn();
  code.retInsn();

  // --- void oskel_close(long fd) ---
  code.bind(handles.close);
  code.movImm(X64Reg::Rax, kSysClose);
  code.syscallInsn();
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
// Seconds between the Windows FILETIME epoch (1601-01-01 UTC) and the Unix
// epoch (1970-01-01 UTC): 134774 days. A FILETIME counts 100-ns intervals, so
// `filetime / 10000000 - this` is the Unix time in seconds.
constexpr std::int64_t kWinFileTimeUnixEpochDelta = 11644473600;

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

  // --- long oskel_time(void) ---
  // GetSystemTimeAsFileTime writes a FILETIME: 100-nanosecond intervals since
  // 1601-01-01 UTC. The Unix epoch is 11644473600 seconds later, so the value
  // is `filetime / 10000000 - 11644473600`. The division is unsigned (a
  // FILETIME is a ULARGE_INTEGER) and the constant is the exact gap between the
  // two epochs, so the result matches the Linux syscall and libc `time`.
  code.bind(handles.now);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace + 16);
  code.leaMem(X64Reg::Rcx, X64Reg::Rbp, -(kWinShadowSpace + 16));
  code.movImmToMem(X64Reg::Rbp, -(kWinShadowSpace + 16), 0);
  callImport(code, PeImport::GetSystemTimeAsFileTime);
  code.movFromMem(X64Reg::Rax, X64Reg::Rbp, -(kWinShadowSpace + 16));
  // `divUnsigned` zeroes rdx (the high half of the dividend), so the divisor
  // must not live there; rcx is free once the call returns.
  code.movImm(X64Reg::Rcx, 10000000);
  code.divUnsigned(X64Reg::Rcx);
  code.movImm(X64Reg::Rdx, kWinFileTimeUnixEpochDelta);
  code.subReg(X64Reg::Rax, X64Reg::Rdx);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // --- long oskel_monotonic(void) ---
  // GetTickCount64: milliseconds since the system started, monotonic and
  // unaffected by a wall-clock change. The value is scaled to nanoseconds so
  // every target hands `clock()` the same unit; the multiplication cannot
  // overflow a 64-bit register for any uptime below 584 years.
  code.bind(handles.monotonic);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  callImport(code, PeImport::GetTickCount64);
  code.movImm(X64Reg::Rcx, 1000000);
  code.imulReg(X64Reg::Rax, X64Reg::Rcx);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // --- the file system, through kernel32 ---
  // `CreateFileA(path, access, FILE_SHARE_READ, NULL, disposition, flags,
  // NULL)`: the fifth and sixth arguments are stack-passed above the 32-byte
  // shadow space. The mode picks the access rights and the disposition -- mode
  // 0 is `GENERIC_READ` + `OPEN_EXISTING`, anything else is `GENERIC_WRITE` +
  // `CREATE_ALWAYS` (which truncates, the POSIX `O_TRUNC` the runtime's writer
  // relies on). A failure is `INVALID_HANDLE_VALUE` (-1), which the runtime
  // treats exactly like a negative POSIX return.
  //
  // The frame is rbp-relative and keeps rsp 16-byte aligned at the call: with
  // `push rbp; push rbx` and a 72-byte frame, rsp is rbp-80, so the outgoing
  // slots at [rsp+32] and [rsp+40] sit above the shadow space. The saved path
  // goes at [rbp-16] = [rsp+64], above every outgoing slot and below the saved
  // rbx at [rbp-8]: storing it at [rbp-48] would alias the fifth argument, so
  // the disposition would overwrite the file name.
  code.bind(handles.open);
  {
    CodeGen::Label reading = code.newLabel();
    CodeGen::Label disposed = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.push(X64Reg::Rbx);
    code.subRegImm32(X64Reg::Rsp, 72);
    code.movToMem(X64Reg::Rbp, -16, X64Reg::Rdi);  // path
    code.testReg(X64Reg::Rsi);                     // mode
    code.jcc(Jcc::Eq, reading);
    code.movImm(X64Reg::Rbx, 0x40000000LL);        // GENERIC_WRITE
    code.movImm(X64Reg::R10, 2);                   // CREATE_ALWAYS
    code.jmp(disposed);
    code.bind(reading);
    code.movImm(X64Reg::Rbx, 0x80000000LL);        // GENERIC_READ
    code.movImm(X64Reg::R10, 3);                   // OPEN_EXISTING
    code.bind(disposed);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, -16);  // lpFileName
    code.movReg(X64Reg::Rdx, X64Reg::Rbx);           // dwDesiredAccess
    code.movImm(X64Reg::R8, 1);                      // FILE_SHARE_READ
    code.xorReg(X64Reg::R9, X64Reg::R9);             // lpSecurityAttributes = NULL
    code.movToMem(X64Reg::Rsp, 32, X64Reg::R10);     // dwCreationDisposition
    code.movImm(X64Reg::R11, 0x80);                  // FILE_ATTRIBUTE_NORMAL
    code.movToMem(X64Reg::Rsp, 40, X64Reg::R11);     // dwFlagsAndAttributes
    code.movImmToMem(X64Reg::Rsp, 48, 0);            // hTemplateFile = NULL
    callImport(code, PeImport::CreateFileA);
    code.addImm32(X64Reg::Rsp, 72);
    code.pop(X64Reg::Rbx);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  // --- long oskel_read_fd(long handle, char* buf, long len) ---
  // ReadFile(hFile, lpBuffer, nBytes, &read, NULL) reports success in rax and
  // the byte count through the out-parameter, so a failed call returns -1 and
  // the runtime's loop stops; a zero count is end of file.
  //
  // The frame is 64 bytes below `rbp`, so [rbp-8] is [rsp+56] -- above the
  // shadow space and the outgoing `lpOverlapped` slot at [rsp+32]. Writing the
  // count to [rbp-32] would alias that slot: the kernel reads `lpOverlapped`
  // and writes `lpNumberOfBytesRead` through the same address.
  code.bind(handles.readFd);
  {
    CodeGen::Label failed = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.subRegImm32(X64Reg::Rsp, 64);
    code.movReg(X64Reg::Rcx, X64Reg::Rdi);           // hFile
    code.movReg(X64Reg::R8, X64Reg::Rdx);            // nNumberOfBytesToRead
    code.movReg(X64Reg::Rdx, X64Reg::Rsi);           // lpBuffer
    code.movImmToMem(X64Reg::Rbp, -8, 0);            // lpNumberOfBytesRead = 0
    code.leaMem(X64Reg::R9, X64Reg::Rbp, -8);
    code.movImmToMem(X64Reg::Rsp, 32, 0);            // lpOverlapped = NULL
    callImport(code, PeImport::ReadFile);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, failed);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, -8);
    code.jmp(done);
    code.bind(failed);
    code.movImm(X64Reg::Rax, -1);
    code.bind(done);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  // --- long oskel_write_fd(long handle, const char* buf, long len) ---
  // The length arrives in rdx and the buffer in rsi, so the count is moved out
  // of rdx *before* rdx is reused for lpBuffer: reading it afterwards would pass
  // the buffer's address as the byte count.
  code.bind(handles.writeFd);
  {
    CodeGen::Label failed = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.subRegImm32(X64Reg::Rsp, 64);
    code.movReg(X64Reg::Rcx, X64Reg::Rdi);           // hFile
    code.movReg(X64Reg::R8, X64Reg::Rdx);            // nNumberOfBytesToWrite
    code.movReg(X64Reg::Rdx, X64Reg::Rsi);           // lpBuffer
    code.movImmToMem(X64Reg::Rbp, -8, 0);            // lpNumberOfBytesWritten = 0
    code.leaMem(X64Reg::R9, X64Reg::Rbp, -8);
    code.movImmToMem(X64Reg::Rsp, 32, 0);            // lpOverlapped = NULL
    callImport(code, PeImport::WriteFile);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, failed);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, -8);
    code.jmp(done);
    code.bind(failed);
    code.movImm(X64Reg::Rax, -1);
    code.bind(done);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  // --- void oskel_close(long handle) ---
  code.bind(handles.close);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  callImport(code, PeImport::CloseHandle);
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

std::uint64_t emitHostedRuntime(CodeGen& code, const RuntimeHandles& handles,
                                CodeGen::Label programEntry) {
  // --- long oskel_write(long fd, const char* buf, long len) ---
  // libc's `write` may write fewer bytes than asked (a pipe, a signal), so the
  // Platform contract's "no short writes" is honoured here, exactly like the
  // freestanding prelude. The loop state lives in callee-saved registers the
  // libc call preserves: the runtime's arena cursor (r13), arena end (r14) and
  // global table (r15) must survive, and so must rbx/r12 (the lowering's own
  // scratch), so all four are saved and restored around the body.
  code.bind(handles.write);
  {
    CodeGen::Label loop = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.push(X64Reg::Rbx);
    code.push(X64Reg::R12);
    code.push(X64Reg::R13);
    code.push(X64Reg::R14);
    code.movReg(X64Reg::Rbx, X64Reg::Rdi);  // fd
    code.movReg(X64Reg::R12, X64Reg::Rsi);  // buf
    code.movReg(X64Reg::R13, X64Reg::Rdx);  // remaining
    code.xorReg(X64Reg::R14, X64Reg::R14);  // running total
    code.bind(loop);
    code.testReg(X64Reg::R13);
    code.jcc(Jcc::Le, done);  // nothing left to write
    code.movReg(X64Reg::Rdi, X64Reg::Rbx);
    code.movReg(X64Reg::Rsi, X64Reg::R12);
    code.movReg(X64Reg::Rdx, X64Reg::R13);
    code.callExternal("write");
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Le, done);  // error (rax < 0) or no progress
    code.addReg(X64Reg::R12, X64Reg::Rax);
    code.subReg(X64Reg::R13, X64Reg::Rax);
    code.addReg(X64Reg::R14, X64Reg::Rax);
    code.jmp(loop);
    code.bind(done);
    code.movReg(X64Reg::Rax, X64Reg::R14);  // return the total, like the syscall prelude
    code.pop(X64Reg::R14);
    code.pop(X64Reg::R13);
    code.pop(X64Reg::R12);
    code.pop(X64Reg::Rbx);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  // --- void oskel_exit(long code) ---
  // libc `exit` runs the atexit handlers and flushes stdio before the process
  // ends, so the hosted image never loses buffered output. It never returns.
  code.bind(handles.exit);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.callExternal("exit");
  code.ud2();  // unreachable: exit never returns

  // --- void oskel_init_arena(void) ---
  // `malloc(kAotArenaSize)` and seed the allocator state. libc owns the region
  // and frees it at `exit`; the runtime's own bump allocator then serves every
  // allocation from it, so the value helpers stay libc-free. The cursor starts
  // `kAotArenaFirstOffset` in, so no live pointer is ever zero.
  code.bind(handles.initArena);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.movImm(X64Reg::Rdi, kAotArenaSize);
  code.callExternal("malloc");
  code.movReg(kAotArenaCursor, X64Reg::Rax);
  code.addImm32(kAotArenaCursor, static_cast<std::int32_t>(kAotArenaFirstOffset));
  code.movReg(kAotArenaEnd, X64Reg::Rax);
  code.addImm32(kAotArenaEnd, static_cast<std::int32_t>(kAotArenaSize));
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // --- long oskel_time(void) ---
  // libc `time(NULL)`: seconds since the Unix epoch. The hosted mode links
  // against libc by construction (that is the point of the mode), so the wall
  // clock comes from the C library rather than a syscall, and the value is the
  // same one the freestanding syscall returns.
  code.bind(handles.now);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.xorReg(X64Reg::Rdi, X64Reg::Rdi);  // tloc = NULL
  code.callExternal("time");
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // --- long oskel_monotonic(void) ---
  // libc `clock_gettime(CLOCK_MONOTONIC, &ts)` in nanoseconds, the same source
  // the freestanding syscall reads. `timespec` is two 64-bit fields, so a
  // 16-byte frame serves it; the second argument is the frame's address.
  code.bind(handles.monotonic);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 16);
  code.movImm(X64Reg::Rdi, 1);            // CLOCK_MONOTONIC
  code.movReg(X64Reg::Rsi, X64Reg::Rsp);
  code.callExternal("clock_gettime");
  code.movFromMem(X64Reg::R8, X64Reg::Rsp, 0);   // tv_sec
  code.movFromMem(X64Reg::R9, X64Reg::Rsp, 8);   // tv_nsec
  code.movImm(X64Reg::Rax, 1000000000);
  code.imulReg(X64Reg::Rax, X64Reg::R8);
  code.addReg(X64Reg::Rax, X64Reg::R9);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // --- the file system, through libc ---
  // `open` is variadic, so it is called as `open(path, flags, 0644)`: the mode
  // is ignored unless O_CREAT is set, and always passing it keeps one call
  // site. `read`/`write`/`close` are the plain POSIX names, so the runtime's
  // file builtins need no per-mode branch of their own.
  code.bind(handles.open);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.movReg(X64Reg::R8, X64Reg::Rdi);   // path
  code.testReg(X64Reg::Rsi);              // mode: 0 reads, else writes
  CodeGen::Label reading = code.newLabel();
  code.jcc(Jcc::Eq, reading);
  code.movImm(X64Reg::Rsi, 1 | 64 | 512);  // O_WRONLY | O_CREAT | O_TRUNC
  CodeGen::Label opened = code.newLabel();
  code.jmp(opened);
  code.bind(reading);
  code.xorReg(X64Reg::Rsi, X64Reg::Rsi);  // O_RDONLY
  code.bind(opened);
  code.movReg(X64Reg::Rdi, X64Reg::R8);
  code.movImm(X64Reg::Rdx, 0644);
  code.xorReg(X64Reg::Rax, X64Reg::Rax);  // variadic: no vector arguments
  code.callExternal("open");
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.readFd);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.callExternal("read");
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.writeFd);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.callExternal("write");
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.close);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.callExternal("close");
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // The program's entry becomes the C `main` the C runtime calls: the lowering
  // already gives it the `int main(void)` shape (no arguments, an `int`
  // result), so no thunk is needed. `_start` -- which crt1.o defines -- reaches
  // it through `__libc_start_main`, so the object exports the symbol rather than
  // naming an entry offset.
  const std::size_t mainOffset = code.labelOffset(programEntry);
  code.exportSymbol("main", mainOffset);
  return mainOffset;
}

std::uint64_t emitWindowsHostedRuntime(CodeGen& code, const RuntimeHandles& handles,
                                       CodeGen::Label programEntry) {
  // Every call into the UCRT is a Microsoft x64 call: the first four integer
  // arguments travel in rcx/rdx/r8/r9 and the caller reserves 32 bytes of
  // shadow space. `sub rsp, 32` after the frame leaves rsp 16-byte aligned at
  // the call, which is what the ABI requires. The embedded runtime keeps the
  // lowering's System V convention; only these thunks cross the boundary.
  //
  // `_write` returns an `int`, and the upper half of rax is undefined. The
  // shl/sar pair re-extends the low 32 bits before the loop compares it, or a
  // -1 would look like a large positive count.
  code.bind(handles.write);
  {
    CodeGen::Label loop = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.subRegImm32(X64Reg::Rsp, 96);  // shadow space plus four frame slots
    code.movToMem(X64Reg::Rbp, -64, X64Reg::Rdi);  // fd
    code.movToMem(X64Reg::Rbp, -56, X64Reg::Rsi);  // buf
    code.movToMem(X64Reg::Rbp, -48, X64Reg::Rdx);  // remaining
    code.movImmToMem(X64Reg::Rbp, -40, 0);         // total written
    code.bind(loop);
    code.movFromMem(X64Reg::R11, X64Reg::Rbp, -48);
    code.testReg(X64Reg::R11);
    code.jcc(Jcc::Le, done);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, -64);
    code.movFromMem(X64Reg::Rdx, X64Reg::Rbp, -56);
    code.movFromMem(X64Reg::R8, X64Reg::Rbp, -48);
    code.callExternal("_write");
    code.shlImm8(X64Reg::Rax, 32);
    code.sarImm8(X64Reg::Rax, 32);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Le, done);  // error or no progress
    code.movFromMem(X64Reg::R11, X64Reg::Rbp, -40);
    code.addReg(X64Reg::R11, X64Reg::Rax);
    code.movToMem(X64Reg::Rbp, -40, X64Reg::R11);
    code.movFromMem(X64Reg::R11, X64Reg::Rbp, -56);
    code.addReg(X64Reg::R11, X64Reg::Rax);
    code.movToMem(X64Reg::Rbp, -56, X64Reg::R11);
    code.movFromMem(X64Reg::R11, X64Reg::Rbp, -48);
    code.subReg(X64Reg::R11, X64Reg::Rax);
    code.movToMem(X64Reg::Rbp, -48, X64Reg::R11);
    code.jmp(loop);
    code.bind(done);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, -40);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  // --- void oskel_exit(long code) ---
  code.bind(handles.exit);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  code.callExternal("exit");
  code.ud2();  // unreachable: exit never returns

  // --- void oskel_init_arena(void) ---
  // `malloc` owns the region; the runtime's bump allocator serves every
  // allocation from it and the CRT frees it at exit.
  code.bind(handles.initArena);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 32);
  // The CRT opens stdout/stderr in text mode, which turns every `\n` into
  // `\r\n`; the VM writes the bytes it built, so the binary would disagree with
  // it. Switch both descriptors to binary before the first write.
  code.movImm(X64Reg::Rcx, 1);
  code.movImm(X64Reg::Rdx, 0x8000);  // _O_BINARY
  code.callExternal("_setmode");
  code.movImm(X64Reg::Rcx, 2);
  code.movImm(X64Reg::Rdx, 0x8000);
  code.callExternal("_setmode");
  code.movImm(X64Reg::Rcx, kAotArenaSize);
  code.callExternal("malloc");
  code.addImm32(X64Reg::Rsp, 32);
  code.movReg(kAotArenaCursor, X64Reg::Rax);
  code.addImm32(kAotArenaCursor, static_cast<std::int32_t>(kAotArenaFirstOffset));
  code.movReg(kAotArenaEnd, X64Reg::Rax);
  code.addImm32(kAotArenaEnd, static_cast<std::int32_t>(kAotArenaSize));
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // --- long oskel_time(void) ---
  code.bind(handles.now);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 32);
  code.xorReg(X64Reg::Rcx, X64Reg::Rcx);  // tloc = NULL
  code.callExternal("time");
  code.addImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // --- long oskel_monotonic(void) ---
  // `GetTickCount64` is milliseconds; multiply to the nanosecond unit the
  // `clock()` builtin subtracts, so only differences matter.
  code.bind(handles.monotonic);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 32);
  code.callExternal("GetTickCount64");
  code.addImm32(X64Reg::Rsp, 32);
  code.imulImm32(X64Reg::Rax, X64Reg::Rax, 1000000);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // --- the file system, through the UCRT ---
  // `_open(path, flags, pmode)`: `_O_BINARY` keeps byte counts exact, `_O_WRONLY
  // | _O_CREAT | _O_TRUNC` is the write mode and `_S_IREAD | _S_IWRITE` is the
  // pmode. The other three are the plain `_read`/`_write`/`_close` calls.
  code.bind(handles.open);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);  // path
  code.testReg(X64Reg::Rsi);              // mode: 0 reads, else writes
  CodeGen::Label reading = code.newLabel();
  CodeGen::Label opened = code.newLabel();
  code.jcc(Jcc::Eq, reading);
  code.movImm(X64Reg::Rdx, 0x8301);  // _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY
  code.jmp(opened);
  code.bind(reading);
  code.movImm(X64Reg::Rdx, 0x8000);  // _O_RDONLY | _O_BINARY
  code.bind(opened);
  code.movImm(X64Reg::R8, 0x0180);   // _S_IREAD | _S_IWRITE
  // `_open` is declared variadic, so the Microsoft ABI requires `al` to name
  // the number of vector registers used; zero here.
  code.xorReg(X64Reg::Rax, X64Reg::Rax);
  code.callExternal("_open");
  code.shlImm8(X64Reg::Rax, 32);
  code.sarImm8(X64Reg::Rax, 32);
  code.addImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.readFd);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  code.movReg(X64Reg::R8, X64Reg::Rdx);
  code.movReg(X64Reg::Rdx, X64Reg::Rsi);
  code.callExternal("_read");
  code.shlImm8(X64Reg::Rax, 32);
  code.sarImm8(X64Reg::Rax, 32);
  code.addImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.writeFd);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  code.movReg(X64Reg::R8, X64Reg::Rdx);
  code.movReg(X64Reg::Rdx, X64Reg::Rsi);
  code.callExternal("_write");
  code.shlImm8(X64Reg::Rax, 32);
  code.sarImm8(X64Reg::Rax, 32);
  code.addImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.close);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  code.callExternal("_close");
  code.shlImm8(X64Reg::Rax, 32);
  code.sarImm8(X64Reg::Rax, 32);
  code.addImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  const std::size_t mainOffset = code.labelOffset(programEntry);
  code.exportSymbol("main", mainOffset);
  return mainOffset;
}
