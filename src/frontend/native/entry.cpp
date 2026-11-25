#include "native/entry.h"

#include "native/pe.h"

namespace {

// Linux x86-64 syscall numbers used by the prelude.
constexpr long kSysRead = 0;
constexpr long kSysWrite = 1;
constexpr long kSysClose = 3;
constexpr long kSysLseek = 8;
constexpr long kSysMmap = 9;
constexpr long kSysClockGettime = 228;
constexpr long kSysExitGroup = 231;
constexpr long kSysOpenAt = 257;
constexpr long kSysGetdents64 = 217;
constexpr long kSysNewfstatat = 262;
constexpr long kSysGetcwd = 79;
constexpr long kSysChdir = 80;
constexpr long kSysRename = 82;
constexpr long kSysMkdir = 83;
constexpr long kSysRmdir = 84;
constexpr long kSysUnlink = 87;
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
constexpr long kOpenWrite = 1 | 64 | 512;   // O_WRONLY | O_CREAT | O_TRUNC
constexpr long kOpenAppend = 1 | 64 | 1024;  // O_WRONLY | O_CREAT | O_APPEND

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

  // --- long oskel_arena_grow(long size) ---
  // mmap(NULL, max(size + kAotArenaFirstOffset, kAotArenaChunkSize),
  //      PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) and repoint the
  // allocator state at the new region, so `oskel_alloc` can retry. Returns 1 on
  // success and 0 when the kernel refuses (the allocator then reports "out of
  // memory"). The exhausted chunk is abandoned: a bump region has no free, and
  // that is the trade for a ceiling that no longer exists.
  code.bind(handles.arenaGrow);
  {
    CodeGen::Label haveLength = code.newLabel();
    CodeGen::Label failed = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.movReg(X64Reg::Rsi, X64Reg::Rdi);  // length = size + first offset
    code.addImm32(X64Reg::Rsi, static_cast<std::int32_t>(kAotArenaFirstOffset));
    code.cmpImm32(X64Reg::Rsi, static_cast<std::int32_t>(kAotArenaChunkSize));
    code.jcc(Jcc::Ae, haveLength);
    code.movImm(X64Reg::Rsi, kAotArenaChunkSize);
    code.bind(haveLength);
    code.xorReg(X64Reg::Rdi, X64Reg::Rdi);           // addr = NULL
    code.movImm(X64Reg::Rdx, kProtReadWrite);        // prot
    code.movImm(X64Reg::R10, kMapPrivateAnonymous);  // flags (syscall arg 4)
    code.movImm(X64Reg::R8, -1);                     // fd = -1
    code.xorReg(X64Reg::R9, X64Reg::R9);             // offset = 0
    code.movImm(X64Reg::Rax, kSysMmap);
    code.syscallInsn();
    // A failure is a negative errno in [-4095, -1], and a null map is not a
    // usable region either. `rsi` (the length) survives the syscall.
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::S, failed);
    code.jcc(Jcc::Eq, failed);
    code.movReg(kAotArenaCursor, X64Reg::Rax);
    code.addImm32(kAotArenaCursor, static_cast<std::int32_t>(kAotArenaFirstOffset));
    code.movReg(kAotArenaEnd, X64Reg::Rax);
    code.addReg(kAotArenaEnd, X64Reg::Rsi);
    code.movImm(X64Reg::Rax, 1);
    code.jmp(done);
    code.bind(failed);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.bind(done);
    code.retInsn();
  }

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
  // `openat(AT_FDCWD, path, flags, 0644)`. The mode selects the access flags:
  // 0 reads, 1 truncates and creates, 2 creates if needed and appends. A
  // negative return is the errno and the caller reports the failure; -1 is
  // never a valid fd.
  code.bind(handles.open);
  code.movReg(X64Reg::R8, X64Reg::Rdi);   // path
  CodeGen::Label reading = code.newLabel();
  CodeGen::Label appending = code.newLabel();
  CodeGen::Label opened = code.newLabel();
  code.cmpImm32(X64Reg::Rsi, 2);          // mode: 0 reads, 1 writes, 2 appends
  code.jcc(Jcc::Eq, appending);
  code.testReg(X64Reg::Rsi);
  code.jcc(Jcc::Eq, reading);
  code.movImm(X64Reg::Rdx, kOpenWrite);
  code.jmp(opened);
  code.bind(appending);
  code.movImm(X64Reg::Rdx, kOpenAppend);
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

  // --- long oskel_seek(long fd, long offset, long origin) ---
  // `lseek(fd, offset, origin)`: the kernel returns the resulting offset, or a
  // negative errno, so the caller can report a failed seek. The origin values
  // are the POSIX ones (0/1/2), which is also what `Platform::SeekOrigin`
  // maps to.
  code.bind(handles.seek);
  code.movImm(X64Reg::Rax, kSysLseek);
  code.syscallInsn();
  code.retInsn();

  // --- long oskel_tell(long fd) ---
  // `lseek(fd, 0, SEEK_CUR)`, the position query.
  code.bind(handles.tell);
  code.xorReg(X64Reg::Rsi, X64Reg::Rsi);  // offset = 0
  code.movImm(X64Reg::Rdx, 1);            // SEEK_CUR
  code.movImm(X64Reg::Rax, kSysLseek);
  code.syscallInsn();
  code.retInsn();

  // --- the directory and metadata handles ---
  // These are the Linux syscalls behind `std.fs.dir`. Each helper returns 1/0
  // (or a length), so the runtime's builtins carry no target-specific code.
  code.bind(handles.removePath);
  {
    CodeGen::Label ok = code.newLabel();
    code.movImm(X64Reg::Rax, kSysUnlink);
    code.syscallInsn();
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Ge, ok);
    // `remove` also accepts an empty directory, like std::filesystem::remove.
    code.movImm(X64Reg::Rax, kSysRmdir);
    code.syscallInsn();
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Ge, ok);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.retInsn();
    code.bind(ok);
    code.movImm(X64Reg::Rax, 1);
    code.retInsn();
  }

  code.bind(handles.renamePath);
  code.movImm(X64Reg::Rax, kSysRename);
  code.syscallInsn();
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Ge, X64Reg::Rax);
  code.retInsn();

  code.bind(handles.mkdirPath);
  code.movImm(X64Reg::Rsi, 0777);
  code.movImm(X64Reg::Rax, kSysMkdir);
  code.syscallInsn();
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Ge, X64Reg::Rax);
  code.retInsn();

  code.bind(handles.rmdirPath);
  code.movImm(X64Reg::Rax, kSysRmdir);
  code.syscallInsn();
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Ge, X64Reg::Rax);
  code.retInsn();

  code.bind(handles.chdirPath);
  code.movImm(X64Reg::Rax, kSysChdir);
  code.syscallInsn();
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Ge, X64Reg::Rax);
  code.retInsn();

  // `long oskel_cwd(char* buf, long cap)`: the getcwd syscall writes the
  // NUL-terminated path and returns its length *including* the terminator, so
  // subtract one before `strNew`.
  code.bind(handles.cwd);
  code.movImm(X64Reg::Rax, kSysGetcwd);
  code.syscallInsn();
  code.testReg(X64Reg::Rax);
  CodeGen::Label cwdOk = code.newLabel();
  code.jcc(Jcc::Ge, cwdOk);
  code.movImm(X64Reg::Rax, -1);
  code.retInsn();
  code.bind(cwdOk);
  code.subRegImm32(X64Reg::Rax, 1);
  code.retInsn();

  // `long oskel_stat(const char* path, long* out)`: newfstatat with a 144-byte
  // `struct stat`; the x86-64 Linux offsets are stable (size 48, mtime 88,
  // mode 24). The four output words are size, mtime, isFile and isDir.
  code.bind(handles.statPath);
  {
    CodeGen::Label failed = code.newLabel();
    CodeGen::Label failedDone = code.newLabel();
    CodeGen::Label notDir = code.newLabel();
    CodeGen::Label notFile = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.push(X64Reg::R12);
    code.subRegImm32(X64Reg::Rsp, 160);
    code.movReg(X64Reg::R12, X64Reg::Rsi);  // out
    code.movReg(X64Reg::R8, X64Reg::Rdi);   // path
    code.movImm(X64Reg::Rdi, kAtFdcwd);
    code.movReg(X64Reg::Rsi, X64Reg::R8);
    code.movReg(X64Reg::Rdx, X64Reg::Rsp);
    code.xorReg(X64Reg::R10, X64Reg::R10);
    code.movImm(X64Reg::Rax, kSysNewfstatat);
    code.syscallInsn();
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Lt, failed);
    code.movFromMem(X64Reg::Rax, X64Reg::Rsp, 48);  // st_size
    code.movToMem(X64Reg::R12, 0, X64Reg::Rax);
    code.movFromMem(X64Reg::Rax, X64Reg::Rsp, 88);  // st_mtime
    code.movToMem(X64Reg::R12, 8, X64Reg::Rax);
    code.movFromMem(X64Reg::Rax, X64Reg::Rsp, 24);  // st_mode
    code.andImm32(X64Reg::Rax, 0170000);
    code.cmpImm32(X64Reg::Rax, 0040000);            // S_IFDIR
    code.jcc(Jcc::Ne, notDir);
    code.movImm(X64Reg::Rax, 1);
    code.movToMem(X64Reg::R12, 24, X64Reg::Rax);
    code.jmp(notFile);
    code.bind(notDir);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.movToMem(X64Reg::R12, 24, X64Reg::Rax);
    code.bind(notFile);
    code.movFromMem(X64Reg::Rax, X64Reg::Rsp, 24);
    code.andImm32(X64Reg::Rax, 0170000);
    code.cmpImm32(X64Reg::Rax, 0100000);            // S_IFREG
    code.movImm(X64Reg::Rax, 0);
    code.setcc(Jcc::Eq, X64Reg::Rax);
    code.movToMem(X64Reg::R12, 16, X64Reg::Rax);
    code.movImm(X64Reg::Rax, 1);
    code.jmp(failed);  // shared epilogue
    code.bind(failed);
    // `failed` is reached either by a syscall error (rax < 0) or by the
    // success path above (rax == 1); distinguish by the sign.
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Ge, failedDone);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.bind(failedDone);
    code.addImm32(X64Reg::Rsp, 160);
    code.pop(X64Reg::R12);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  // `long oskel_list_entry(const char* path, long index, char* buf, long cap)`:
  // open the directory and walk getdents64 entries until the requested index,
  // skipping `.` and `..`. It returns the basename length, 0 at the end of the
  // directory and -1 on failure. The runtime calls it once per index and sorts
  // the names, so the host's directory order never reaches the program.
  code.bind(handles.listEntry);
  {
    constexpr std::int32_t kPath = -24;
    constexpr std::int32_t kIndex = -32;
    constexpr std::int32_t kBuf = -40;
    constexpr std::int32_t kCap = -48;
    constexpr std::int32_t kFd = -56;
    constexpr std::int32_t kCount = -64;
    constexpr std::int32_t kOffset = -72;
    constexpr std::int32_t kBuffer = -4208;
    CodeGen::Label readMore = code.newLabel();
    CodeGen::Label entryLoop = code.newLabel();
    CodeGen::Label nextEntry = code.newLabel();
    CodeGen::Label notDot = code.newLabel();
    CodeGen::Label copyName = code.newLabel();
    CodeGen::Label scanName = code.newLabel();
    CodeGen::Label scanned = code.newLabel();
    CodeGen::Label closeEof = code.newLabel();
    CodeGen::Label closeFail = code.newLabel();
    CodeGen::Label closeReturn = code.newLabel();
    CodeGen::Label cleanup = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.push(X64Reg::Rbx);
    code.push(X64Reg::R12);
    code.subRegImm32(X64Reg::Rsp, 4208);
    code.movToMem(X64Reg::Rbp, kPath, X64Reg::Rdi);
    code.movToMem(X64Reg::Rbp, kIndex, X64Reg::Rsi);
    code.movToMem(X64Reg::Rbp, kBuf, X64Reg::Rdx);
    code.movToMem(X64Reg::Rbp, kCap, X64Reg::Rcx);
    code.xorReg(X64Reg::Rsi, X64Reg::Rsi);  // read mode
    code.callLabel(handles.open);
    code.movToMem(X64Reg::Rbp, kFd, X64Reg::Rax);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Lt, closeFail);

    code.bind(readMore);
    code.movFromMem(X64Reg::Rdi, X64Reg::Rbp, kFd);
    code.leaMem(X64Reg::Rsi, X64Reg::Rbp, kBuffer);
    code.movImm(X64Reg::Rdx, 4096);
    code.movImm(X64Reg::Rax, kSysGetdents64);
    code.syscallInsn();
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Lt, closeFail);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, closeEof);
    code.movToMem(X64Reg::Rbp, kCount, X64Reg::Rax);
    code.movImmToMem(X64Reg::Rbp, kOffset, 0);

    code.bind(entryLoop);
    code.movFromMem(X64Reg::R8, X64Reg::Rbp, kOffset);
    code.movFromMem(X64Reg::R9, X64Reg::Rbp, kCount);
    code.cmpReg(X64Reg::R8, X64Reg::R9);
    code.jcc(Jcc::Ae, readMore);
    code.leaMem(X64Reg::R9, X64Reg::Rbp, kBuffer);
    code.addReg(X64Reg::R9, X64Reg::R8);          // &dirent
    code.movFromMem(X64Reg::R11, X64Reg::R9, 16); // reclen + type + name
    code.andImm32(X64Reg::R11, 0xFFFF);           // d_reclen
    code.testReg(X64Reg::R11);
    code.jcc(Jcc::Eq, closeFail);
    code.addImm32(X64Reg::R9, 19);                // d_name
    code.movzxFromMem(X64Reg::Rax, X64Reg::R9, 0);
    code.cmpImm32(X64Reg::Rax, '.');
    code.jcc(Jcc::Ne, notDot);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R9, 1);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, nextEntry);
    code.cmpImm32(X64Reg::Rax, '.');
    code.jcc(Jcc::Ne, notDot);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R9, 2);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, nextEntry);

    code.bind(notDot);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kIndex);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, copyName);
    code.subRegImm32(X64Reg::Rax, 1);
    code.movToMem(X64Reg::Rbp, kIndex, X64Reg::Rax);
    code.jmp(nextEntry);

    code.bind(copyName);
    code.movReg(X64Reg::R12, X64Reg::R9);         // name
    code.xorReg(X64Reg::Rbx, X64Reg::Rbx);        // length
    code.bind(scanName);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R12, 0);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, scanned);
    code.addImm32(X64Reg::R12, 1);
    code.addImm32(X64Reg::Rbx, 1);
    code.jmp(scanName);
    code.bind(scanned);
    code.movReg(X64Reg::Rax, X64Reg::Rbx);
    code.addImm32(X64Reg::Rax, 1);
    code.movFromMem(X64Reg::R8, X64Reg::Rbp, kCap);
    code.cmpReg(X64Reg::Rax, X64Reg::R8);
    code.jcc(Jcc::A, closeFail);
    code.movFromMem(X64Reg::Rdi, X64Reg::Rbp, kBuf);
    code.movReg(X64Reg::Rsi, X64Reg::R9);
    code.movReg(X64Reg::Rcx, X64Reg::Rbx);
    code.cld();
    code.repMovsb();
    code.movImmByteToMem(X64Reg::Rdi, 0, 0);
    code.movReg(X64Reg::R12, X64Reg::Rbx);        // return length
    code.jmp(closeReturn);

    code.bind(nextEntry);
    code.movFromMem(X64Reg::R8, X64Reg::Rbp, kOffset);
    code.addReg(X64Reg::R8, X64Reg::R11);
    code.movToMem(X64Reg::Rbp, kOffset, X64Reg::R8);
    code.jmp(entryLoop);

    code.bind(closeEof);
    code.xorReg(X64Reg::R12, X64Reg::R12);
    code.jmp(closeReturn);
    code.bind(closeFail);
    code.movImm(X64Reg::R12, -1);
    code.bind(closeReturn);
    code.movFromMem(X64Reg::Rdi, X64Reg::Rbp, kFd);
    code.testReg(X64Reg::Rdi);
    code.jcc(Jcc::Lt, cleanup);
    code.callLabel(handles.close);
    code.bind(cleanup);
    code.movReg(X64Reg::Rax, X64Reg::R12);
    code.addImm32(X64Reg::Rsp, 4208);
    code.pop(X64Reg::R12);
    code.pop(X64Reg::Rbx);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

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

  // --- long oskel_arena_grow(long size) ---
  // VirtualAlloc(NULL, max(size + first offset, kAotArenaChunkSize),
  //              MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE) and repoint the
  // allocator state at the new region. Returns 1 on success and 0 when Windows
  // refuses, which `oskel_alloc` reports as "out of memory". The exhausted
  // chunk is abandoned, exactly like the Linux prelude's.
  code.bind(handles.arenaGrow);
  {
    CodeGen::Label haveLength = code.newLabel();
    CodeGen::Label failed = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.subRegImm32(X64Reg::Rsp, kWinShadowSpace + 16);
    code.movReg(X64Reg::Rdx, X64Reg::Rdi);
    code.addImm32(X64Reg::Rdx, static_cast<std::int32_t>(kAotArenaFirstOffset));
    code.cmpImm32(X64Reg::Rdx, static_cast<std::int32_t>(kAotArenaChunkSize));
    code.jcc(Jcc::Ae, haveLength);
    code.movImm(X64Reg::Rdx, kAotArenaChunkSize);
    code.bind(haveLength);
    // The length must survive the call, and rdx is caller-saved.
    code.movToMem(X64Reg::Rbp, -(kWinShadowSpace + 8), X64Reg::Rdx);
    code.xorReg(X64Reg::Rcx, X64Reg::Rcx);          // lpAddress = NULL
    code.movImm(X64Reg::R8, kWinMemCommitReserve);  // flAllocationType
    code.movImm(X64Reg::R9, kWinPageReadWrite);     // flProtect
    callImport(code, PeImport::VirtualAlloc);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, failed);
    code.movReg(kAotArenaCursor, X64Reg::Rax);
    code.addImm32(kAotArenaCursor, static_cast<std::int32_t>(kAotArenaFirstOffset));
    code.movReg(kAotArenaEnd, X64Reg::Rax);
    code.movFromMem(X64Reg::Rdx, X64Reg::Rbp, -(kWinShadowSpace + 8));
    code.addReg(kAotArenaEnd, X64Reg::Rdx);
    code.movImm(X64Reg::Rax, 1);
    code.jmp(done);
    code.bind(failed);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.bind(done);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

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
    CodeGen::Label appending = code.newLabel();
    CodeGen::Label disposed = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.push(X64Reg::Rbx);
    code.subRegImm32(X64Reg::Rsp, 72);
    code.movToMem(X64Reg::Rbp, -16, X64Reg::Rdi);  // path
    code.cmpImm32(X64Reg::Rsi, 2);                 // mode: 0 read, 1 write, 2 append
    code.jcc(Jcc::Eq, appending);
    code.testReg(X64Reg::Rsi);
    code.jcc(Jcc::Eq, reading);
    code.movImm(X64Reg::Rbx, 0x40000000LL);        // GENERIC_WRITE
    code.movImm(X64Reg::R10, 2);                   // CREATE_ALWAYS
    code.jmp(disposed);
    code.bind(appending);
    code.movImm(X64Reg::Rbx, 4);                   // FILE_APPEND_DATA
    code.movImm(X64Reg::R10, 4);                   // OPEN_ALWAYS
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

  // --- long oskel_seek(long handle, long offset, long origin) ---
  // SetFilePointerEx(hFile, liDistanceToMove, &newPosition, dwMoveMethod). The
  // 64-bit distance travels by value in rdx; the origin (0/1/2) is the fourth
  // argument in r9, so it is moved out of rdx before rdx is reused. The frame
  // is 48 bytes so the out-slot at [rbp-8] sits above the 32-byte shadow space
  // and the saved rbp, and rsp stays 16-byte aligned at the call.
  code.bind(handles.seek);
  {
    CodeGen::Label failed = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.subRegImm32(X64Reg::Rsp, 48);
    code.movToMem(X64Reg::Rbp, -8, X64Reg::Rsi);  // liDistanceToMove
    code.movReg(X64Reg::R9, X64Reg::Rdx);         // dwMoveMethod
    code.movReg(X64Reg::Rcx, X64Reg::Rdi);        // hFile
    code.movFromMem(X64Reg::Rdx, X64Reg::Rbp, -8);
    code.leaMem(X64Reg::R8, X64Reg::Rbp, -8);     // lpNewFilePointer
    code.movImmToMem(X64Reg::Rbp, -8, 0);
    callImport(code, PeImport::SetFilePointerEx);
    code.movReg32(X64Reg::Rax, X64Reg::Rax);
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

  // --- long oskel_tell(long handle) ---
  code.bind(handles.tell);
  code.xorReg(X64Reg::Rsi, X64Reg::Rsi);  // offset = 0
  code.movImm(X64Reg::Rdx, 1);            // FILE_CURRENT
  code.jmp(handles.seek);

  // --- the directory and metadata handles ---
  // The same operations as the Linux prelude, reached through kernel32 with the
  // Microsoft ABI. Each thunk reserves the 32-byte shadow space and returns 1/0
  // (or a length), so the runtime builtins stay target-independent.
  code.bind(handles.removePath);
  {
    CodeGen::Label ok = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
    code.movReg(X64Reg::Rcx, X64Reg::Rdi);
    callImport(code, PeImport::DeleteFileA);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Ne, ok);
    code.movReg(X64Reg::Rcx, X64Reg::Rdi);
    callImport(code, PeImport::RemoveDirectoryA);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Ne, ok);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.jmp(done);
    code.bind(ok);
    code.movImm(X64Reg::Rax, 1);
    code.bind(done);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  code.bind(handles.renamePath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  code.movReg(X64Reg::Rdx, X64Reg::Rsi);
  callImport(code, PeImport::MoveFileA);
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Ne, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.mkdirPath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  code.xorReg(X64Reg::Rdx, X64Reg::Rdx);
  callImport(code, PeImport::CreateDirectoryA);
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Ne, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.rmdirPath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  callImport(code, PeImport::RemoveDirectoryA);
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Ne, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.chdirPath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  callImport(code, PeImport::SetCurrentDirectoryA);
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Ne, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.cwd);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.movReg(X64Reg::Rcx, X64Reg::Rsi);  // capacity
  code.movReg(X64Reg::Rdx, X64Reg::Rdi);  // buffer
  callImport(code, PeImport::GetCurrentDirectoryA);
  CodeGen::Label cwdFits = code.newLabel();
  code.cmpReg(X64Reg::Rax, X64Reg::Rsi);
  code.jcc(Jcc::Le, cwdFits);
  code.movImm(X64Reg::Rax, -1);
  code.retInsn();
  code.bind(cwdFits);
  code.testReg(X64Reg::Rax);
  CodeGen::Label cwdOk = code.newLabel();
  code.jcc(Jcc::Ne, cwdOk);
  code.movImm(X64Reg::Rax, -1);
  code.bind(cwdOk);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // `long oskel_stat(const char* path, long* out)`: GetFileAttributesExA fills
  // a 36-byte WIN32_FILE_ATTRIBUTE_DATA; the size is split high/low and the
  // last-write FILETIME is converted from 100-ns units to Unix seconds.
  code.bind(handles.statPath);
  {
    CodeGen::Label failed = code.newLabel();
    CodeGen::Label notDir = code.newLabel();
    CodeGen::Label storeFile = code.newLabel();
    CodeGen::Label statDone = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.push(X64Reg::R12);
    code.subRegImm32(X64Reg::Rsp, 96);
    code.movReg(X64Reg::R12, X64Reg::Rsi);  // out
    code.movReg(X64Reg::Rcx, X64Reg::Rdi);
    code.xorReg(X64Reg::Rdx, X64Reg::Rdx);  // GetFileExInfoStandard
    code.leaMem(X64Reg::R8, X64Reg::Rsp, 32);  // above the shadow space
    callImport(code, PeImport::GetFileAttributesExA);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, failed);
    // size = high << 32 | low
    code.movFromMem32(X64Reg::Rax, X64Reg::Rsp, 60);
    code.shlImm8(X64Reg::Rax, 32);
    code.movFromMem32(X64Reg::R8, X64Reg::Rsp, 64);
    code.orReg(X64Reg::Rax, X64Reg::R8);
    code.movToMem(X64Reg::R12, 0, X64Reg::Rax);
    // modified = filetime / 10000000 - epoch delta
    code.movFromMem32(X64Reg::Rax, X64Reg::Rsp, 52);
    code.shlImm8(X64Reg::Rax, 32);
    code.movFromMem32(X64Reg::R8, X64Reg::Rsp, 56);
    code.orReg(X64Reg::Rax, X64Reg::R8);
    code.movImm(X64Reg::R8, 10000000);
    code.divUnsigned(X64Reg::R8);
    code.movImm(X64Reg::R8, kWinFileTimeUnixEpochDelta);
    code.subReg(X64Reg::Rax, X64Reg::R8);
    code.movToMem(X64Reg::R12, 8, X64Reg::Rax);
    // isDir from FILE_ATTRIBUTE_DIRECTORY (0x10), isFile = !isDir.
    code.movFromMem32(X64Reg::Rax, X64Reg::Rsp, 32);
    code.testImm32(X64Reg::Rax, 0x10);
    code.jcc(Jcc::Eq, notDir);
    code.movImm(X64Reg::Rax, 1);
    code.movToMem(X64Reg::R12, 24, X64Reg::Rax);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.jmp(storeFile);
    code.bind(notDir);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.movToMem(X64Reg::R12, 24, X64Reg::Rax);
    code.movImm(X64Reg::Rax, 1);
    code.bind(storeFile);
    code.movToMem(X64Reg::R12, 16, X64Reg::Rax);
    code.movImm(X64Reg::Rax, 1);
    code.jmp(statDone);
    code.bind(failed);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.bind(statDone);
    code.addImm32(X64Reg::Rsp, 96);
    code.pop(X64Reg::R12);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  // `long oskel_list_entry(const char* path, long index, char* buf, long cap)`:
  // build `path\*`, walk FindFirstFileA/FindNextFileA and return the indexed
  // basename. `std.fs.dir` sorts the names, so the host order does not leak.
  code.bind(handles.listEntry);
  {
    constexpr std::int32_t kPath = -24;
    constexpr std::int32_t kIndex = -32;
    constexpr std::int32_t kBuf = -40;
    constexpr std::int32_t kCap = -48;
    constexpr std::int32_t kHandle = -56;
    constexpr std::int32_t kPattern = -4176;
    constexpr std::int32_t kFindData = -4592;
    CodeGen::Label scanPath = code.newLabel();
    CodeGen::Label pathScanned = code.newLabel();
    CodeGen::Label appendSeparator = code.newLabel();
    CodeGen::Label appendStar = code.newLabel();
    CodeGen::Label opened = code.newLabel();
    CodeGen::Label loop = code.newLabel();
    CodeGen::Label next = code.newLabel();
    CodeGen::Label copyOrNext = code.newLabel();
    CodeGen::Label copyName = code.newLabel();
    CodeGen::Label scanName = code.newLabel();
    CodeGen::Label scanned = code.newLabel();
    CodeGen::Label closeEof = code.newLabel();
    CodeGen::Label closeFail = code.newLabel();
    CodeGen::Label closeReturn = code.newLabel();
    CodeGen::Label cleanup = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.push(X64Reg::Rbx);
    code.push(X64Reg::R12);
    code.subRegImm32(X64Reg::Rsp, 4800);
    code.movToMem(X64Reg::Rbp, kPath, X64Reg::Rdi);
    code.movToMem(X64Reg::Rbp, kIndex, X64Reg::Rsi);
    code.movToMem(X64Reg::Rbp, kBuf, X64Reg::Rdx);
    code.movToMem(X64Reg::Rbp, kCap, X64Reg::Rcx);
    code.movImmToMem(X64Reg::Rbp, kHandle, -1);
    // pattern = path, with a separator if needed, then `*`.
    code.movFromMem(X64Reg::Rsi, X64Reg::Rbp, kPath);
    code.leaMem(X64Reg::Rdi, X64Reg::Rbp, kPattern);
    code.xorReg(X64Reg::Rcx, X64Reg::Rcx);
    code.bind(scanPath);
    code.movzxFromMem(X64Reg::Rax, X64Reg::Rsi, 0);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, pathScanned);
    code.addImm32(X64Reg::Rcx, 1);
    code.addImm32(X64Reg::Rsi, 1);
    code.jmp(scanPath);
    code.bind(pathScanned);
    code.cmpImm32(X64Reg::Rcx, 4088);
    code.jcc(Jcc::A, closeFail);
    code.movFromMem(X64Reg::Rsi, X64Reg::Rbp, kPath);
    code.leaMem(X64Reg::Rdi, X64Reg::Rbp, kPattern);
    code.cld();
    code.repMovsb();
    code.leaMem(X64Reg::R8, X64Reg::Rbp, kPattern);
    code.cmpReg(X64Reg::Rdi, X64Reg::R8);
    code.jcc(Jcc::Eq, appendSeparator);
    code.movzxFromMem(X64Reg::Rax, X64Reg::Rdi, -1);
    code.cmpImm32(X64Reg::Rax, '\\');
    code.jcc(Jcc::Eq, appendStar);
    code.cmpImm32(X64Reg::Rax, '/');
    code.jcc(Jcc::Eq, appendStar);
    code.bind(appendSeparator);
    code.movImm(X64Reg::Rax, '\\');
    code.movByteToMem(X64Reg::Rdi, 0, X64Reg::Rax);
    code.addImm32(X64Reg::Rdi, 1);
    code.bind(appendStar);
    code.movImm(X64Reg::Rax, '*');
    code.movByteToMem(X64Reg::Rdi, 0, X64Reg::Rax);
    code.movImmByteToMem(X64Reg::Rdi, 1, 0);
    code.leaMem(X64Reg::Rcx, X64Reg::Rbp, kPattern);
    code.leaMem(X64Reg::Rdx, X64Reg::Rbp, kFindData);
    callImport(code, PeImport::FindFirstFileA);
    code.movToMem(X64Reg::Rbp, kHandle, X64Reg::Rax);
    code.cmpImm32(X64Reg::Rax, -1);
    code.jcc(Jcc::Eq, closeFail);
    code.bind(opened);
    code.bind(loop);
    code.leaMem(X64Reg::R9, X64Reg::Rbp, kFindData);
    code.addImm32(X64Reg::R9, 44);  // cFileName
    code.movzxFromMem(X64Reg::Rax, X64Reg::R9, 0);
    code.cmpImm32(X64Reg::Rax, '.');
    code.jcc(Jcc::Ne, copyOrNext);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R9, 1);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, next);
    code.cmpImm32(X64Reg::Rax, '.');
    code.jcc(Jcc::Ne, copyOrNext);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R9, 2);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, next);
    code.bind(copyOrNext);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kIndex);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, copyName);
    code.subRegImm32(X64Reg::Rax, 1);
    code.movToMem(X64Reg::Rbp, kIndex, X64Reg::Rax);
    code.jmp(next);
    code.bind(copyName);
    code.movReg(X64Reg::R12, X64Reg::R9);
    code.xorReg(X64Reg::Rbx, X64Reg::Rbx);
    code.bind(scanName);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R12, 0);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, scanned);
    code.addImm32(X64Reg::R12, 1);
    code.addImm32(X64Reg::Rbx, 1);
    code.jmp(scanName);
    code.bind(scanned);
    code.movReg(X64Reg::Rax, X64Reg::Rbx);
    code.addImm32(X64Reg::Rax, 1);
    code.movFromMem(X64Reg::R8, X64Reg::Rbp, kCap);
    code.cmpReg(X64Reg::Rax, X64Reg::R8);
    code.jcc(Jcc::A, closeFail);
    code.movFromMem(X64Reg::Rdi, X64Reg::Rbp, kBuf);
    code.movReg(X64Reg::Rsi, X64Reg::R9);
    code.movReg(X64Reg::Rcx, X64Reg::Rbx);
    code.cld();
    code.repMovsb();
    code.movImmByteToMem(X64Reg::Rdi, 0, 0);
    code.movReg(X64Reg::R12, X64Reg::Rbx);
    code.jmp(closeReturn);
    code.bind(next);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, kHandle);
    code.leaMem(X64Reg::Rdx, X64Reg::Rbp, kFindData);
    callImport(code, PeImport::FindNextFileA);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Ne, loop);
    code.bind(closeEof);
    code.xorReg(X64Reg::R12, X64Reg::R12);
    code.jmp(closeReturn);
    code.bind(closeFail);
    code.movImm(X64Reg::R12, -1);
    code.bind(closeReturn);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, kHandle);
    code.cmpImm32(X64Reg::Rcx, -1);
    code.jcc(Jcc::Eq, cleanup);
    callImport(code, PeImport::FindClose);
    code.bind(cleanup);
    code.movReg(X64Reg::Rax, X64Reg::R12);
    code.addImm32(X64Reg::Rsp, 4800);
    code.pop(X64Reg::R12);
    code.pop(X64Reg::Rbx);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

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
                                CodeGen::Label programEntry, bool macos) {
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

  // --- long oskel_arena_grow(long size) ---
  // `malloc(max(size + first offset, kAotArenaChunkSize))` and repoint the
  // allocator state at the new block. libc owns the block and frees it at exit;
  // the previous chunk stays allocated (a bump region has no free), which is the
  // price of having no ceiling. Returns 1 on success and 0 on a failed malloc.
  code.bind(handles.arenaGrow);
  {
    CodeGen::Label haveLength = code.newLabel();
    CodeGen::Label failed = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.subRegImm32(X64Reg::Rsp, 16);
    code.addImm32(X64Reg::Rdi, static_cast<std::int32_t>(kAotArenaFirstOffset));
    code.cmpImm32(X64Reg::Rdi, static_cast<std::int32_t>(kAotArenaChunkSize));
    code.jcc(Jcc::Ae, haveLength);
    code.movImm(X64Reg::Rdi, kAotArenaChunkSize);
    code.bind(haveLength);
    code.movToMem(X64Reg::Rbp, -8, X64Reg::Rdi);  // the length, across the call
    code.callExternal("malloc");
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, failed);
    code.movReg(kAotArenaCursor, X64Reg::Rax);
    code.addImm32(kAotArenaCursor, static_cast<std::int32_t>(kAotArenaFirstOffset));
    code.movReg(kAotArenaEnd, X64Reg::Rax);
    code.movFromMem(X64Reg::Rdi, X64Reg::Rbp, -8);
    code.addReg(kAotArenaEnd, X64Reg::Rdi);
    code.movImm(X64Reg::Rax, 1);
    code.jmp(done);
    code.bind(failed);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.bind(done);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

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
  CodeGen::Label reading = code.newLabel();
  CodeGen::Label appending = code.newLabel();
  CodeGen::Label opened = code.newLabel();
  code.cmpImm32(X64Reg::Rsi, 2);          // mode: 0 reads, 1 writes, 2 appends
  code.jcc(Jcc::Eq, appending);
  code.testReg(X64Reg::Rsi);
  code.jcc(Jcc::Eq, reading);
  // Linux and macOS disagree on O_CREAT/O_TRUNC/O_APPEND, so the hosted
  // prelude takes the flags from the target rather than hard-coding Linux.
  code.movImm(X64Reg::Rsi, macos ? (1 | 512 | 1024) : (1 | 64 | 512));
  code.jmp(opened);
  code.bind(appending);
  code.movImm(X64Reg::Rsi, macos ? (1 | 512 | 8) : (1 | 64 | 1024));
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

  code.bind(handles.seek);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.callExternal("lseek");
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.tell);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.xorReg(X64Reg::Rsi, X64Reg::Rsi);  // offset = 0
  code.movImm(X64Reg::Rdx, 1);            // SEEK_CUR
  code.callExternal("lseek");
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // --- the directory and metadata handles, through libc ---
  // `remove` accepts a file or an empty directory, matching the hosted
  // Platform's std::filesystem::remove. `stat`/`readdir` layouts differ between
  // Linux and macOS, so the offsets are selected by `macos`.
  code.bind(handles.removePath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.callExternal("remove");
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Eq, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.renamePath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.callExternal("rename");
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Eq, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.mkdirPath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.movImm(X64Reg::Rsi, 0777);
  code.callExternal("mkdir");
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Eq, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.rmdirPath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.callExternal("rmdir");
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Eq, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.chdirPath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.callExternal("chdir");
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Eq, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.cwd);
  {
    CodeGen::Label failed = code.newLabel();
    CodeGen::Label scan = code.newLabel();
    CodeGen::Label done = code.newLabel();
    CodeGen::Label cwdDone = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.callExternal("getcwd");
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, failed);
    code.movReg(X64Reg::R8, X64Reg::Rax);
    code.bind(scan);
    code.movzxFromMem(X64Reg::R9, X64Reg::R8, 0);
    code.testReg(X64Reg::R9);
    code.jcc(Jcc::Eq, done);
    code.addImm32(X64Reg::R8, 1);
    code.jmp(scan);
    code.bind(done);
    code.movReg(X64Reg::Rax, X64Reg::R8);
    code.subReg(X64Reg::Rax, X64Reg::Rdi);
    code.jmp(cwdDone);
    code.bind(failed);
    code.movImm(X64Reg::Rax, -1);
    code.bind(cwdDone);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  code.bind(handles.statPath);
  {
    constexpr std::int32_t kSize = 48;
    constexpr std::int32_t kMtime = 88;
    constexpr std::int32_t kMode = 24;
    const std::int32_t sizeOffset = macos ? 96 : kSize;
    const std::int32_t mtimeOffset = macos ? 48 : kMtime;
    const std::int32_t modeOffset = macos ? 4 : kMode;
    CodeGen::Label failed = code.newLabel();
    CodeGen::Label notDir = code.newLabel();
    CodeGen::Label storeFile = code.newLabel();
    CodeGen::Label statDone = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.push(X64Reg::R12);
    code.subRegImm32(X64Reg::Rsp, 160);
    code.movReg(X64Reg::R12, X64Reg::Rsi);  // out
    code.movReg(X64Reg::Rsi, X64Reg::Rsp);
    code.callExternal(macos ? "stat$INODE64" : "stat");
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Ne, failed);
    code.movFromMem(X64Reg::Rax, X64Reg::Rsp, sizeOffset);
    code.movToMem(X64Reg::R12, 0, X64Reg::Rax);
    code.movFromMem(X64Reg::Rax, X64Reg::Rsp, mtimeOffset);
    code.movToMem(X64Reg::R12, 8, X64Reg::Rax);
    code.movFromMem(X64Reg::Rax, X64Reg::Rsp, modeOffset);
    code.andImm32(X64Reg::Rax, 0xFFFF);
    code.andImm32(X64Reg::Rax, 0170000);
    code.cmpImm32(X64Reg::Rax, 0040000);
    code.jcc(Jcc::Ne, notDir);
    code.movImm(X64Reg::Rax, 1);
    code.movToMem(X64Reg::R12, 24, X64Reg::Rax);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.jmp(storeFile);
    code.bind(notDir);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.movToMem(X64Reg::R12, 24, X64Reg::Rax);
    code.movImm(X64Reg::Rax, 1);
    code.bind(storeFile);
    code.movToMem(X64Reg::R12, 16, X64Reg::Rax);
    code.movImm(X64Reg::Rax, 1);
    code.jmp(statDone);
    code.bind(failed);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.bind(statDone);
    code.addImm32(X64Reg::Rsp, 160);
    code.pop(X64Reg::R12);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  code.bind(handles.listEntry);
  {
    constexpr std::int32_t kPath = -24;
    constexpr std::int32_t kIndex = -32;
    constexpr std::int32_t kBuf = -40;
    constexpr std::int32_t kCap = -48;
    constexpr std::int32_t kDir = -56;
    const std::int32_t nameOffset = macos ? 21 : 19;
    CodeGen::Label loop = code.newLabel();
    CodeGen::Label next = code.newLabel();
    CodeGen::Label copyOrNext = code.newLabel();
    CodeGen::Label copyName = code.newLabel();
    CodeGen::Label scanName = code.newLabel();
    CodeGen::Label scanned = code.newLabel();
    CodeGen::Label closeEof = code.newLabel();
    CodeGen::Label closeFail = code.newLabel();
    CodeGen::Label closeReturn = code.newLabel();
    CodeGen::Label cleanup = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.push(X64Reg::Rbx);
    code.push(X64Reg::R12);
    code.subRegImm32(X64Reg::Rsp, 64);
    code.movToMem(X64Reg::Rbp, kPath, X64Reg::Rdi);
    code.movToMem(X64Reg::Rbp, kIndex, X64Reg::Rsi);
    code.movToMem(X64Reg::Rbp, kBuf, X64Reg::Rdx);
    code.movToMem(X64Reg::Rbp, kCap, X64Reg::Rcx);
    code.callExternal("opendir");
    code.movToMem(X64Reg::Rbp, kDir, X64Reg::Rax);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, closeFail);
    code.bind(loop);
    code.movFromMem(X64Reg::Rdi, X64Reg::Rbp, kDir);
    code.callExternal(macos ? "readdir$INODE64" : "readdir");
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, closeEof);
    code.movReg(X64Reg::R9, X64Reg::Rax);
    code.addImm32(X64Reg::R9, nameOffset);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R9, 0);
    code.cmpImm32(X64Reg::Rax, '.');
    code.jcc(Jcc::Ne, copyOrNext);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R9, 1);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, next);
    code.cmpImm32(X64Reg::Rax, '.');
    code.jcc(Jcc::Ne, copyOrNext);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R9, 2);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, next);
    code.bind(copyOrNext);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kIndex);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, copyName);
    code.subRegImm32(X64Reg::Rax, 1);
    code.movToMem(X64Reg::Rbp, kIndex, X64Reg::Rax);
    code.jmp(next);
    code.bind(copyName);
    code.movReg(X64Reg::R12, X64Reg::R9);
    code.xorReg(X64Reg::Rbx, X64Reg::Rbx);
    code.bind(scanName);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R12, 0);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, scanned);
    code.addImm32(X64Reg::R12, 1);
    code.addImm32(X64Reg::Rbx, 1);
    code.jmp(scanName);
    code.bind(scanned);
    code.movReg(X64Reg::Rax, X64Reg::Rbx);
    code.addImm32(X64Reg::Rax, 1);
    code.movFromMem(X64Reg::R8, X64Reg::Rbp, kCap);
    code.cmpReg(X64Reg::Rax, X64Reg::R8);
    code.jcc(Jcc::A, closeFail);
    code.movFromMem(X64Reg::Rdi, X64Reg::Rbp, kBuf);
    code.movReg(X64Reg::Rsi, X64Reg::R9);
    code.movReg(X64Reg::Rcx, X64Reg::Rbx);
    code.cld();
    code.repMovsb();
    code.movImmByteToMem(X64Reg::Rdi, 0, 0);
    code.movReg(X64Reg::R12, X64Reg::Rbx);
    code.jmp(closeReturn);
    code.bind(next);
    code.jmp(loop);
    code.bind(closeEof);
    code.xorReg(X64Reg::R12, X64Reg::R12);
    code.jmp(closeReturn);
    code.bind(closeFail);
    code.movImm(X64Reg::R12, -1);
    code.bind(closeReturn);
    code.movFromMem(X64Reg::Rdi, X64Reg::Rbp, kDir);
    code.testReg(X64Reg::Rdi);
    code.jcc(Jcc::Eq, cleanup);
    code.callExternal("closedir");
    code.bind(cleanup);
    code.movReg(X64Reg::Rax, X64Reg::R12);
    code.addImm32(X64Reg::Rsp, 64);
    code.pop(X64Reg::R12);
    code.pop(X64Reg::Rbx);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

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

  // --- long oskel_arena_grow(long size) ---
  // The UCRT's `malloc`, reached with the Microsoft x64 ABI: the size goes in
  // rcx and the call gets 32 bytes of shadow space. Same contract as the other
  // preludes -- 1 when the new block is in place, 0 when malloc fails.
  code.bind(handles.arenaGrow);
  {
    CodeGen::Label haveLength = code.newLabel();
    CodeGen::Label failed = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.subRegImm32(X64Reg::Rsp, kWinShadowSpace + 16);
    code.movReg(X64Reg::Rcx, X64Reg::Rdi);
    code.addImm32(X64Reg::Rcx, static_cast<std::int32_t>(kAotArenaFirstOffset));
    code.cmpImm32(X64Reg::Rcx, static_cast<std::int32_t>(kAotArenaChunkSize));
    code.jcc(Jcc::Ae, haveLength);
    code.movImm(X64Reg::Rcx, kAotArenaChunkSize);
    code.bind(haveLength);
    code.movToMem(X64Reg::Rbp, -(kWinShadowSpace + 8), X64Reg::Rcx);
    code.callExternal("malloc");
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, failed);
    code.movReg(kAotArenaCursor, X64Reg::Rax);
    code.addImm32(kAotArenaCursor, static_cast<std::int32_t>(kAotArenaFirstOffset));
    code.movReg(kAotArenaEnd, X64Reg::Rax);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, -(kWinShadowSpace + 8));
    code.addReg(kAotArenaEnd, X64Reg::Rcx);
    code.movImm(X64Reg::Rax, 1);
    code.jmp(done);
    code.bind(failed);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.bind(done);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  // --- long oskel_time(void) ---
  // UCRT `_time64(NULL)`: seconds since the Unix epoch. `time` is an inline
  // wrapper over `_time64` in the UCRT headers and is not an export of
  // ucrtbase.dll, so the own linker's import table must name `_time64`.
  code.bind(handles.now);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 32);
  code.xorReg(X64Reg::Rcx, X64Reg::Rcx);  // tloc = NULL
  code.callExternal("_time64");
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
  CodeGen::Label reading = code.newLabel();
  CodeGen::Label appending = code.newLabel();
  CodeGen::Label opened = code.newLabel();
  code.cmpImm32(X64Reg::Rsi, 2);          // mode: 0 reads, 1 writes, 2 appends
  code.jcc(Jcc::Eq, appending);
  code.testReg(X64Reg::Rsi);
  code.jcc(Jcc::Eq, reading);
  code.movImm(X64Reg::Rdx, 0x8301);  // _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY
  code.jmp(opened);
  code.bind(appending);
  code.movImm(X64Reg::Rdx, 0x8109);  // _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY
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

  code.bind(handles.seek);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);  // fd
  code.movReg(X64Reg::R8, X64Reg::Rdx);   // origin (before rdx is overwritten)
  code.movReg(X64Reg::Rdx, X64Reg::Rsi);  // offset
  code.callExternal("_lseeki64");
  code.addImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.tell);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);  // fd
  code.xorReg(X64Reg::Rdx, X64Reg::Rdx);  // offset = 0
  code.movImm(X64Reg::R8, 1);             // SEEK_CUR
  code.callExternal("_lseeki64");
  code.addImm32(X64Reg::Rsp, 32);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // --- the directory and metadata handles ---
  // The same operations as the Linux prelude, reached through kernel32 with the
  // Microsoft ABI. Each thunk reserves the 32-byte shadow space and returns 1/0
  // (or a length), so the runtime builtins stay target-independent.
  code.bind(handles.removePath);
  {
    CodeGen::Label ok = code.newLabel();
    CodeGen::Label done = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
    code.movReg(X64Reg::Rcx, X64Reg::Rdi);
    code.callExternal("DeleteFileA");
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Ne, ok);
    code.movReg(X64Reg::Rcx, X64Reg::Rdi);
    code.callExternal("RemoveDirectoryA");
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Ne, ok);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.jmp(done);
    code.bind(ok);
    code.movImm(X64Reg::Rax, 1);
    code.bind(done);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  code.bind(handles.renamePath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  code.movReg(X64Reg::Rdx, X64Reg::Rsi);
  code.callExternal("MoveFileA");
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Ne, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.mkdirPath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  code.xorReg(X64Reg::Rdx, X64Reg::Rdx);
  code.callExternal("CreateDirectoryA");
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Ne, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.rmdirPath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  code.callExternal("RemoveDirectoryA");
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Ne, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.chdirPath);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.movReg(X64Reg::Rcx, X64Reg::Rdi);
  code.callExternal("SetCurrentDirectoryA");
  code.testReg(X64Reg::Rax);
  code.movImm(X64Reg::Rax, 0);
  code.setcc(Jcc::Ne, X64Reg::Rax);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  code.bind(handles.cwd);
  code.push(X64Reg::Rbp);
  code.movReg(X64Reg::Rbp, X64Reg::Rsp);
  code.subRegImm32(X64Reg::Rsp, kWinShadowSpace);
  code.movReg(X64Reg::Rcx, X64Reg::Rsi);  // capacity
  code.movReg(X64Reg::Rdx, X64Reg::Rdi);  // buffer
  code.callExternal("GetCurrentDirectoryA");
  CodeGen::Label cwdFits = code.newLabel();
  code.cmpReg(X64Reg::Rax, X64Reg::Rsi);
  code.jcc(Jcc::Le, cwdFits);
  code.movImm(X64Reg::Rax, -1);
  code.retInsn();
  code.bind(cwdFits);
  code.testReg(X64Reg::Rax);
  CodeGen::Label cwdOk = code.newLabel();
  code.jcc(Jcc::Ne, cwdOk);
  code.movImm(X64Reg::Rax, -1);
  code.bind(cwdOk);
  code.movReg(X64Reg::Rsp, X64Reg::Rbp);
  code.pop(X64Reg::Rbp);
  code.retInsn();

  // `long oskel_stat(const char* path, long* out)`: GetFileAttributesExA fills
  // a 36-byte WIN32_FILE_ATTRIBUTE_DATA; the size is split high/low and the
  // last-write FILETIME is converted from 100-ns units to Unix seconds.
  code.bind(handles.statPath);
  {
    CodeGen::Label failed = code.newLabel();
    CodeGen::Label notDir = code.newLabel();
    CodeGen::Label storeFile = code.newLabel();
    CodeGen::Label statDone = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.push(X64Reg::R12);
    code.subRegImm32(X64Reg::Rsp, 96);
    code.movReg(X64Reg::R12, X64Reg::Rsi);  // out
    code.movReg(X64Reg::Rcx, X64Reg::Rdi);
    code.xorReg(X64Reg::Rdx, X64Reg::Rdx);  // GetFileExInfoStandard
    code.leaMem(X64Reg::R8, X64Reg::Rsp, 32);  // above the shadow space
    code.callExternal("GetFileAttributesExA");
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, failed);
    // size = high << 32 | low
    code.movFromMem32(X64Reg::Rax, X64Reg::Rsp, 60);
    code.shlImm8(X64Reg::Rax, 32);
    code.movFromMem32(X64Reg::R8, X64Reg::Rsp, 64);
    code.orReg(X64Reg::Rax, X64Reg::R8);
    code.movToMem(X64Reg::R12, 0, X64Reg::Rax);
    // modified = filetime / 10000000 - epoch delta
    code.movFromMem32(X64Reg::Rax, X64Reg::Rsp, 52);
    code.shlImm8(X64Reg::Rax, 32);
    code.movFromMem32(X64Reg::R8, X64Reg::Rsp, 56);
    code.orReg(X64Reg::Rax, X64Reg::R8);
    code.movImm(X64Reg::R8, 10000000);
    code.divUnsigned(X64Reg::R8);
    code.movImm(X64Reg::R8, kWinFileTimeUnixEpochDelta);
    code.subReg(X64Reg::Rax, X64Reg::R8);
    code.movToMem(X64Reg::R12, 8, X64Reg::Rax);
    // isDir from FILE_ATTRIBUTE_DIRECTORY (0x10), isFile = !isDir.
    code.movFromMem32(X64Reg::Rax, X64Reg::Rsp, 32);
    code.testImm32(X64Reg::Rax, 0x10);
    code.jcc(Jcc::Eq, notDir);
    code.movImm(X64Reg::Rax, 1);
    code.movToMem(X64Reg::R12, 24, X64Reg::Rax);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.jmp(storeFile);
    code.bind(notDir);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.movToMem(X64Reg::R12, 24, X64Reg::Rax);
    code.movImm(X64Reg::Rax, 1);
    code.bind(storeFile);
    code.movToMem(X64Reg::R12, 16, X64Reg::Rax);
    code.movImm(X64Reg::Rax, 1);
    code.jmp(statDone);
    code.bind(failed);
    code.xorReg(X64Reg::Rax, X64Reg::Rax);
    code.bind(statDone);
    code.addImm32(X64Reg::Rsp, 96);
    code.pop(X64Reg::R12);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }

  // `long oskel_list_entry(const char* path, long index, char* buf, long cap)`:
  // build `path\*`, walk FindFirstFileA/FindNextFileA and return the indexed
  // basename. `std.fs.dir` sorts the names, so the host order does not leak.
  code.bind(handles.listEntry);
  {
    constexpr std::int32_t kPath = -24;
    constexpr std::int32_t kIndex = -32;
    constexpr std::int32_t kBuf = -40;
    constexpr std::int32_t kCap = -48;
    constexpr std::int32_t kHandle = -56;
    constexpr std::int32_t kPattern = -4176;
    constexpr std::int32_t kFindData = -4592;
    CodeGen::Label scanPath = code.newLabel();
    CodeGen::Label pathScanned = code.newLabel();
    CodeGen::Label appendSeparator = code.newLabel();
    CodeGen::Label appendStar = code.newLabel();
    CodeGen::Label opened = code.newLabel();
    CodeGen::Label loop = code.newLabel();
    CodeGen::Label next = code.newLabel();
    CodeGen::Label copyOrNext = code.newLabel();
    CodeGen::Label copyName = code.newLabel();
    CodeGen::Label scanName = code.newLabel();
    CodeGen::Label scanned = code.newLabel();
    CodeGen::Label closeEof = code.newLabel();
    CodeGen::Label closeFail = code.newLabel();
    CodeGen::Label closeReturn = code.newLabel();
    CodeGen::Label cleanup = code.newLabel();
    code.push(X64Reg::Rbp);
    code.movReg(X64Reg::Rbp, X64Reg::Rsp);
    code.push(X64Reg::Rbx);
    code.push(X64Reg::R12);
    code.subRegImm32(X64Reg::Rsp, 4800);
    code.movToMem(X64Reg::Rbp, kPath, X64Reg::Rdi);
    code.movToMem(X64Reg::Rbp, kIndex, X64Reg::Rsi);
    code.movToMem(X64Reg::Rbp, kBuf, X64Reg::Rdx);
    code.movToMem(X64Reg::Rbp, kCap, X64Reg::Rcx);
    code.movImmToMem(X64Reg::Rbp, kHandle, -1);
    // pattern = path, with a separator if needed, then `*`.
    code.movFromMem(X64Reg::Rsi, X64Reg::Rbp, kPath);
    code.leaMem(X64Reg::Rdi, X64Reg::Rbp, kPattern);
    code.xorReg(X64Reg::Rcx, X64Reg::Rcx);
    code.bind(scanPath);
    code.movzxFromMem(X64Reg::Rax, X64Reg::Rsi, 0);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, pathScanned);
    code.addImm32(X64Reg::Rcx, 1);
    code.addImm32(X64Reg::Rsi, 1);
    code.jmp(scanPath);
    code.bind(pathScanned);
    code.cmpImm32(X64Reg::Rcx, 4088);
    code.jcc(Jcc::A, closeFail);
    code.movFromMem(X64Reg::Rsi, X64Reg::Rbp, kPath);
    code.leaMem(X64Reg::Rdi, X64Reg::Rbp, kPattern);
    code.cld();
    code.repMovsb();
    code.leaMem(X64Reg::R8, X64Reg::Rbp, kPattern);
    code.cmpReg(X64Reg::Rdi, X64Reg::R8);
    code.jcc(Jcc::Eq, appendSeparator);
    code.movzxFromMem(X64Reg::Rax, X64Reg::Rdi, -1);
    code.cmpImm32(X64Reg::Rax, '\\');
    code.jcc(Jcc::Eq, appendStar);
    code.cmpImm32(X64Reg::Rax, '/');
    code.jcc(Jcc::Eq, appendStar);
    code.bind(appendSeparator);
    code.movImm(X64Reg::Rax, '\\');
    code.movByteToMem(X64Reg::Rdi, 0, X64Reg::Rax);
    code.addImm32(X64Reg::Rdi, 1);
    code.bind(appendStar);
    code.movImm(X64Reg::Rax, '*');
    code.movByteToMem(X64Reg::Rdi, 0, X64Reg::Rax);
    code.movImmByteToMem(X64Reg::Rdi, 1, 0);
    code.leaMem(X64Reg::Rcx, X64Reg::Rbp, kPattern);
    code.leaMem(X64Reg::Rdx, X64Reg::Rbp, kFindData);
    code.callExternal("FindFirstFileA");
    code.movToMem(X64Reg::Rbp, kHandle, X64Reg::Rax);
    code.cmpImm32(X64Reg::Rax, -1);
    code.jcc(Jcc::Eq, closeFail);
    code.bind(opened);
    code.bind(loop);
    code.leaMem(X64Reg::R9, X64Reg::Rbp, kFindData);
    code.addImm32(X64Reg::R9, 44);  // cFileName
    code.movzxFromMem(X64Reg::Rax, X64Reg::R9, 0);
    code.cmpImm32(X64Reg::Rax, '.');
    code.jcc(Jcc::Ne, copyOrNext);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R9, 1);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, next);
    code.cmpImm32(X64Reg::Rax, '.');
    code.jcc(Jcc::Ne, copyOrNext);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R9, 2);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, next);
    code.bind(copyOrNext);
    code.movFromMem(X64Reg::Rax, X64Reg::Rbp, kIndex);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, copyName);
    code.subRegImm32(X64Reg::Rax, 1);
    code.movToMem(X64Reg::Rbp, kIndex, X64Reg::Rax);
    code.jmp(next);
    code.bind(copyName);
    code.movReg(X64Reg::R12, X64Reg::R9);
    code.xorReg(X64Reg::Rbx, X64Reg::Rbx);
    code.bind(scanName);
    code.movzxFromMem(X64Reg::Rax, X64Reg::R12, 0);
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Eq, scanned);
    code.addImm32(X64Reg::R12, 1);
    code.addImm32(X64Reg::Rbx, 1);
    code.jmp(scanName);
    code.bind(scanned);
    code.movReg(X64Reg::Rax, X64Reg::Rbx);
    code.addImm32(X64Reg::Rax, 1);
    code.movFromMem(X64Reg::R8, X64Reg::Rbp, kCap);
    code.cmpReg(X64Reg::Rax, X64Reg::R8);
    code.jcc(Jcc::A, closeFail);
    code.movFromMem(X64Reg::Rdi, X64Reg::Rbp, kBuf);
    code.movReg(X64Reg::Rsi, X64Reg::R9);
    code.movReg(X64Reg::Rcx, X64Reg::Rbx);
    code.cld();
    code.repMovsb();
    code.movImmByteToMem(X64Reg::Rdi, 0, 0);
    code.movReg(X64Reg::R12, X64Reg::Rbx);
    code.jmp(closeReturn);
    code.bind(next);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, kHandle);
    code.leaMem(X64Reg::Rdx, X64Reg::Rbp, kFindData);
    code.callExternal("FindNextFileA");
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Ne, loop);
    code.bind(closeEof);
    code.xorReg(X64Reg::R12, X64Reg::R12);
    code.jmp(closeReturn);
    code.bind(closeFail);
    code.movImm(X64Reg::R12, -1);
    code.bind(closeReturn);
    code.movFromMem(X64Reg::Rcx, X64Reg::Rbp, kHandle);
    code.cmpImm32(X64Reg::Rcx, -1);
    code.jcc(Jcc::Eq, cleanup);
    code.callExternal("FindClose");
    code.bind(cleanup);
    code.movReg(X64Reg::Rax, X64Reg::R12);
    code.addImm32(X64Reg::Rsp, 4800);
    code.pop(X64Reg::R12);
    code.pop(X64Reg::Rbx);
    code.movReg(X64Reg::Rsp, X64Reg::Rbp);
    code.pop(X64Reg::Rbp);
    code.retInsn();
  }


  // The C runtime and the own linker's `_start` call `main` with the Microsoft
  // x64 convention: argc in rcx, argv in rdx. The Oskel chunk reads rdi/rsi,
  // so a two-instruction wrapper moves them before the chunk. The `jmp` keeps
  // the caller's return address, so the chunk returns straight to the CRT or
  // `_start`.
  CodeGen::Label main = code.newLabel();
  code.bind(main);
  code.movReg(X64Reg::Rdi, X64Reg::Rcx);
  code.movReg(X64Reg::Rsi, X64Reg::Rdx);
  code.jmp(programEntry);
  const std::size_t mainOffset = code.labelOffset(main);
  code.exportSymbol("main", mainOffset);
  return mainOffset;
}
