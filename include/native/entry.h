#pragma once

#include <cstdint>

#include "native/codegen.h"

// Freestanding entry point and runtime prelude (PHASE 7c).
//
// A generated binary has no libc and no `_start`: this module appends both to
// the same machine-code buffer the lowering produced, so every `call` resolves
// through the emitter's own labels -- no relocations, no linker.
//
// `_start` is the ELF entry: it reads argc/argv from the initial stack, calls
// the program's entry, and exits with the returned value. The prelude provides
// the syscall wrappers the runtime calls: a `write` that retries on short
// writes, and `exit`.

// Labels the caller creates up front, so a program emitted *before* the
// prelude can already reference them; `emitFreestandingRuntime` binds them.
struct RuntimeHandles {
  CodeGen::Label write;      // long oskel_write(long fd, const char* buf, long len)
  CodeGen::Label exit;       // void oskel_exit(long code)   (never returns)
  CodeGen::Label initArena;  // void oskel_init_arena(void)  (maps the bump region)
  // `long oskel_time(void)`: whole seconds since the Unix epoch, UTC. It is the
  // wall clock the `time()` builtin reads, and the anchor `std.datetime` builds
  // civil dates on. Each prelude reaches it differently -- a raw syscall on
  // Linux, libc `time` in the hosted mode, kernel32 on Windows, the CMOS RTC on
  // bare metal -- because it is the one value the embedded runtime cannot
  // compute from memory.
  CodeGen::Label now;
  // `long oskel_read(char* buf, long len)`: read one line into `buf` (the
  // terminating newline is consumed, not stored) and return the byte count. It
  // is the input counterpart to `write` and the only prelude-owned channel the
  // `input` builtin reads through. Only the bare-metal prelude binds it -- it
  // drains the IRQ1 keyboard ring buffer, blocking with `hlt` on an empty one --
  // because the bare-metal slice is the one that gives the kernel a keyboard;
  // the other preludes leave the label unbound, and the shared runtime never
  // references it unless `input` was seeded (see `lowerToX64`).
  CodeGen::Label read;
  // `long oskel_mem_size(void)`: the usable RAM in bytes, or 0 when the boot
  // loader gave no memory map. Only the bare-metal prelude binds it (from the
  // multiboot map); the hosted targets leave it unbound and the lowerer rejects
  // `memSize()` there instead of emitting a call to nil.
  CodeGen::Label memSize;
  // `long oskel_monotonic(void)`: a monotonically non-decreasing nanosecond
  // counter with an arbitrary origin. `clock()` subtracts the value captured at
  // program start, so what it reports is the elapsed time and not the origin --
  // which is why every target can use its own source (CLOCK_MONOTONIC, libc
  // `clock_gettime`, `GetTickCount64`, the PIT tick counter). Unlike `now` it is
  // never a date, and unlike the hosted `clock()` it does not depend on libm.
  CodeGen::Label monotonic;
  // The file system: `long oskel_open(const char* path, long mode)` (mode 0
  // reads, 1 truncates and creates, 2 creates if needed and appends; -1 on
  // failure), `long oskel_read_fd(long fd, char* buf, long len)`, `long
  // oskel_write_fd(long fd, const char* buf, long len)` (both return the bytes
  // transferred, or a negative value) and `void oskel_close(long fd)`. The
  // runtime's `readFile`/`writeFile`/`fileExists` and the `std.fs.File`
  // primitives are written against these, so the builtins carry no syscall of
  // their own and the Windows prelude keeps the Microsoft ABI in one place. A
  // bare-metal image has no file system, so it binds none of them and the
  // lowerer rejects those builtins by name.
  CodeGen::Label open;
  CodeGen::Label readFd;
  CodeGen::Label writeFd;
  CodeGen::Label close;
  // `long oskel_seek(long fd, long offset, long origin)` and `long
  // oskel_tell(long fd)`: the streaming position of a file handle. `origin` is
  // 0 (start), 1 (current) or 2 (end), the same values `SeekOrigin` maps to.
  // Both return the resulting byte offset, or -1 on failure.
  CodeGen::Label seek;
  CodeGen::Label tell;
};

inline RuntimeHandles newRuntimeHandles(CodeGen& code) {
  return RuntimeHandles{code.newLabel(), code.newLabel(), code.newLabel(),
                        code.newLabel(), code.newLabel(), code.newLabel(),
                        code.newLabel(), code.newLabel(), code.newLabel(),
                        code.newLabel(), code.newLabel(), code.newLabel(),
                        code.newLabel()};
}

// The generated binary's bump arena (PHASE 7c.1c). `_start` maps a region and
// keeps the allocator state in two registers the runtime owns:
//
//   kAotArenaCursor -- the next free address (grows upward)
//   kAotArenaEnd    -- one past the last usable byte
//
// They are callee-saved registers no helper touches, so a value survives from
// `_start` down through `main` and the chunk to the allocator without a global
// memory word (the generated image has no writable data segment). A helper
// that allocates updates the cursor in place: `r13`/`r14` are the allocator's
// state, not a preserved register, and only `oskel_alloc` and its callers read
// them. The runtime helpers use none of r12-r15, so the arena and the register
// file on the stack never collide.
constexpr X64Reg kAotArenaCursor = X64Reg::R13;
constexpr X64Reg kAotArenaEnd = X64Reg::R14;

// Base of the global table (PHASE 7c.2a): a contiguous array of `AotValue`
// slots the chunk allocates from the arena once at entry. Like the arena
// cursor, it is a callee-saved register no helper touches, so the chunk and
// every frame reach the globals without a writable data segment. The runtime
// helpers use none of r12-r15, so the arena, the global table and the register
// file never collide.
constexpr X64Reg kAotGlobalBase = X64Reg::R15;

// Region `_start` maps for the arena, and the offset of the first allocation
// (so a live pointer is never null, which is the allocator's failure value).
constexpr std::int64_t kAotArenaSize = 1 << 20;  // 1 MiB
constexpr std::int64_t kAotArenaFirstOffset = 16;

// Appends `oskel_write`, `oskel_exit` and `_start` to `code`, binding the
// handles. `programEntry` must be bound at the program's entry; `_start` calls
// it with (argc, argv) and hands its return value to `oskel_exit`. Returns the
// offset of `_start`, which the caller passes to `buildStaticElf` as the entry
// point.
std::uint64_t emitFreestandingRuntime(CodeGen& code, const RuntimeHandles& handles,
                                      CodeGen::Label programEntry);

// Windows x86-64 entry prelude (PHASE 7e). Same contract as
// `emitFreestandingRuntime` -- it binds the same three handles and returns the
// offset of the entry point -- but it reaches the system through the Windows
// API instead of Linux syscalls: `oskel_write` marshals to
// `GetStdHandle`+`WriteFile`, `oskel_exit` to `ExitProcess` and
// `oskel_init_arena` to `VirtualAlloc`, all imported from kernel32.dll and
// called with the Microsoft x64 ABI (rcx/rdx/r8/r9 + 32 bytes of shadow space).
// The embedded runtime itself is unchanged: the ABI boundary lives only here.
//
// The entry point has no argc/argv (the loader calls it directly), so the
// program entry is invoked with no arguments -- the lowering's `main` ignores
// them anyway -- and its return value becomes the process exit code.
std::uint64_t emitWindowsRuntime(CodeGen& code, const RuntimeHandles& handles,
                                 CodeGen::Label programEntry);

// Hosted entry prelude (PHASE 7f.4). It provides no `_start`: the C runtime
// does, and it calls the `main` symbol this prelude exports (the lowering's
// program entry, whose shape -- no arguments, an `int` result -- is already a
// valid `main`). The three handles reach the system through libc -- `write`,
// `exit` and `malloc` -- instead of a raw syscall, so the object it belongs to
// must be linked against the host C library. That link is the whole point of
// the hosted mode: it is what lets an `extern "C"` symbol resolve.
//
// Returns the offset of the exported `main`, which the caller may record as the
// program entry; the object's real entry point is the linker's `_start`.
std::uint64_t emitHostedRuntime(CodeGen& code, const RuntimeHandles& handles,
                                CodeGen::Label programEntry, bool macos);

// Windows hosted entry prelude. Same contract as `emitHostedRuntime` -- it
// exports the program entry as the C `main` and returns its offset -- but the
// host C library is reached with the Microsoft x64 ABI (rcx/rdx/r8/r9 plus 32
// bytes of shadow space) and the UCRT's underscore-prefixed file functions.
// `_write`/`_read`/`_open`/`_close` carry the file system, `malloc` the arena,
// `exit` the process, `time` the wall clock and `GetTickCount64` the monotonic
// counter (the CRT has no `clock_gettime`). The embedded runtime itself keeps
// the lowering's System V convention; only these thunks cross the ABI.
std::uint64_t emitWindowsHostedRuntime(CodeGen& code, const RuntimeHandles& handles,
                                       CodeGen::Label programEntry);
