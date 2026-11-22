#include "native/platform_freestanding.h"

#include <cstdint>

#if defined(__linux__) && defined(__x86_64__)

#include <sys/stat.h>

namespace {

// Raw Linux x86-64 syscall gate. The System V register order is
// rax (number), rdi, rsi, rdx, r10, r8, r9. rcx and r11 are clobbered by
// `syscall` itself, so they are declared as clobbers; "memory" keeps the
// compiler from reordering memory accesses across the gate.
long rawSyscall(long number, long a1, long a2, long a3, long a4, long a5,
                long a6) noexcept {
  long result;
  register long r10 __asm__("r10") = a4;
  register long r8 __asm__("r8") = a5;
  register long r9 __asm__("r9") = a6;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8),
                     "r"(r9)
                   : "rcx", "r11", "memory");
  return result;
}

// Syscall numbers (asm/unistd_64.h).
constexpr long kSysRead = 0;
constexpr long kSysWrite = 1;
constexpr long kSysClose = 3;
constexpr long kSysLseek = 8;
constexpr long kSysMmap = 9;
constexpr long kSysGetcwd = 79;
constexpr long kSysChdir = 80;
constexpr long kSysRename = 82;
constexpr long kSysMkdir = 83;
constexpr long kSysRmdir = 84;
constexpr long kSysUnlink = 87;
constexpr long kSysGetdents64 = 217;
constexpr long kSysExitGroup = 231;
constexpr long kSysOpenAt = 257;
constexpr long kSysNewfstatat = 262;

// openat / lseek constants.
constexpr long kAtFdcwd = -100;
constexpr int kOReadOnly = 0;
constexpr int kOWriteOnly = 1;
constexpr int kOCreate = 64;
constexpr int kOTrunc = 512;
constexpr int kOAppend = 1024;

// mmap constants.
constexpr long kProtReadWrite = 3;          // PROT_READ | PROT_WRITE
constexpr long kMapPrivateAnon = 0x22;      // MAP_PRIVATE | MAP_ANONYMOUS

// A negative return in the range [-4095, -1] is an errno, not a result.
bool isError(long value) noexcept { return value < 0 && value >= -4095; }

// EINTR: the call was interrupted by a signal before it transferred data. The
// correct response is to retry, not to report failure. A freestanding binary
// has no signal handlers of its own, but the process can still receive a
// signal delivered from outside, so a blocking `read`/`write` can be cut short.
constexpr long kEintr = 4;
bool isInterrupted(long value) noexcept { return value == -kEintr; }

}  // namespace

void PlatformFreestanding::write(int fd, const char* data, std::size_t length) {
  // The contract forbids short writes, so retry until the whole buffer is out
  // (a pipe or a signal can interrupt a large write).
  std::size_t written = 0;
  while (written < length) {
    long n = rawSyscall(kSysWrite, fd, reinterpret_cast<long>(data + written),
                        static_cast<long>(length - written), 0, 0, 0);
    if (isInterrupted(n)) continue;  // interrupted before any byte: retry
    if (isError(n) || n == 0) {
      // No error channel is available below libc; dropping the rest is the
      // only option that cannot recurse into a failing `writeErr`.
      return;
    }
    written += static_cast<std::size_t>(n);
  }
}

void PlatformFreestanding::writeErr(const char* data, std::size_t length) {
  write(Platform::kStderr, data, length);
}

void PlatformFreestanding::flush(int /*fd*/) {
  // Direct syscalls are unbuffered; there is nothing to push.
}

std::size_t PlatformFreestanding::read(int fd, char* buffer, std::size_t length) {
  long n;
  do {
    n = rawSyscall(kSysRead, fd, reinterpret_cast<long>(buffer),
                   static_cast<long>(length), 0, 0, 0);
  } while (isInterrupted(n));
  if (isError(n) || n < 0) return 0;
  return static_cast<std::size_t>(n);
}

int PlatformFreestanding::open(const char* path, OpenMode mode) {
  int flags = kOReadOnly;
  if (mode == OpenMode::Write) flags = kOWriteOnly | kOCreate | kOTrunc;
  if (mode == OpenMode::Append) flags = kOWriteOnly | kOCreate | kOAppend;
  long fd;
  do {
    fd = rawSyscall(kSysOpenAt, kAtFdcwd, reinterpret_cast<long>(path), flags, 0644, 0, 0);
  } while (isInterrupted(fd));
  if (isError(fd)) return kInvalidFd;
  return static_cast<int>(fd);
}

void PlatformFreestanding::close(int fd) {
  rawSyscall(kSysClose, fd, 0, 0, 0, 0, 0);
}

bool PlatformFreestanding::seek(int fd, std::int64_t offset, SeekOrigin origin) {
  long whence = 0;
  switch (origin) {
    case SeekOrigin::Start: whence = 0; break;
    case SeekOrigin::Current: whence = 1; break;
    case SeekOrigin::End: whence = 2; break;
  }
  long result;
  do {
    result = rawSyscall(kSysLseek, fd, static_cast<long>(offset), whence, 0, 0, 0);
  } while (isInterrupted(result));
  return !isError(result);
}

std::int64_t PlatformFreestanding::tell(int fd) {
  long result;
  do {
    result = rawSyscall(kSysLseek, fd, 0, 1, 0, 0, 0);
  } while (isInterrupted(result));
  return isError(result) ? -1 : static_cast<std::int64_t>(result);
}

namespace {
// The prefix of `struct linux_dirent64`: the name starts at offset 19.
struct LinuxDirent64 {
  std::uint64_t ino;
  std::int64_t off;
  std::uint16_t reclen;
  std::uint8_t type;
  char name[1];
};
constexpr std::size_t kLinuxDirentNameOffset = 19;
constexpr std::uint8_t kDirentDirectory = 4;
}  // namespace

bool PlatformFreestanding::remove(const char* path) {
  const long result = rawSyscall(kSysUnlink, reinterpret_cast<long>(path), 0, 0, 0, 0, 0);
  if (!isError(result)) return true;
  // `remove` also accepts an empty directory, like `std::filesystem::remove`.
  const long directory = rawSyscall(kSysRmdir, reinterpret_cast<long>(path), 0, 0, 0, 0, 0);
  return !isError(directory);
}

bool PlatformFreestanding::rename(const char* from, const char* to) {
  const long result =
      rawSyscall(kSysRename, reinterpret_cast<long>(from), reinterpret_cast<long>(to), 0, 0, 0, 0);
  return !isError(result);
}

bool PlatformFreestanding::makeDirectory(const char* path) {
  const long result = rawSyscall(kSysMkdir, reinterpret_cast<long>(path), 0755, 0, 0, 0, 0);
  return !isError(result);
}

bool PlatformFreestanding::removeDirectory(const char* path) {
  const long result = rawSyscall(kSysRmdir, reinterpret_cast<long>(path), 0, 0, 0, 0, 0);
  return !isError(result);
}

bool PlatformFreestanding::fileStat(const char* path, Platform::FileInfo& out) {
  struct stat info;
  const long result = rawSyscall(kSysNewfstatat, kAtFdcwd, reinterpret_cast<long>(path),
                                 reinterpret_cast<long>(&info), 0, 0, 0);
  if (isError(result)) return false;
  out.size = static_cast<std::uint64_t>(info.st_size);
  out.modified = static_cast<std::int64_t>(info.st_mtime);
  out.isDirectory = S_ISDIR(info.st_mode);
  out.isFile = S_ISREG(info.st_mode);
  return true;
}

bool PlatformFreestanding::listDirectory(const char* path, Platform::DirectoryVisitor visit,
                                         void* user) {
  const int fd = open(path, OpenMode::Read);
  if (fd == kInvalidFd) return false;
  char buffer[4096];
  bool ok = true;
  for (;;) {
    long count;
    do {
      count = rawSyscall(kSysGetdents64, fd, reinterpret_cast<long>(buffer), sizeof buffer, 0, 0, 0);
    } while (isInterrupted(count));
    if (isError(count)) {
      ok = false;
      break;
    }
    if (count == 0) break;
    long offset = 0;
    while (offset < count) {
      const auto* entry = reinterpret_cast<const LinuxDirent64*>(buffer + offset);
      if (entry->reclen == 0 || offset + entry->reclen > count) {
        ok = false;
        break;
      }
      const char* name = buffer + offset + kLinuxDirentNameOffset;
      const bool dot = name[0] == '.' && name[1] == '\0';
      const bool dotdot = name[0] == '.' && name[1] == '.' && name[2] == '\0';
      if (!dot && !dotdot) visit(user, name, entry->type == kDirentDirectory);
      offset += entry->reclen;
    }
    if (!ok) break;
  }
  close(fd);
  return ok;
}

bool PlatformFreestanding::currentDirectory(char* buffer, std::size_t size) {
  const long result = rawSyscall(kSysGetcwd, reinterpret_cast<long>(buffer), static_cast<long>(size),
                                 0, 0, 0, 0);
  return !isError(result);
}

bool PlatformFreestanding::changeDirectory(const char* path) {
  const long result = rawSyscall(kSysChdir, reinterpret_cast<long>(path), 0, 0, 0, 0, 0);
  return !isError(result);
}

void PlatformFreestanding::exit(int code) {
  rawSyscall(kSysExitGroup, code, 0, 0, 0, 0, 0);
  // `exit_group` never returns; the builtin keeps the compiler happy about the
  // [[noreturn]] contract on every path.
  __builtin_unreachable();
}

void* PlatformFreestanding::alloc(std::size_t size, std::size_t alignment) {
  if (cursor_ == nullptr) {
    long region = rawSyscall(kSysMmap, 0, static_cast<long>(regionSize_),
                             kProtReadWrite, kMapPrivateAnon, -1, 0);
    if (isError(region) || region == 0) return nullptr;
    cursor_ = reinterpret_cast<std::uint8_t*>(region);
    end_ = cursor_ + regionSize_;
  }
  const std::uintptr_t current = reinterpret_cast<std::uintptr_t>(cursor_);
  const std::uintptr_t mask = static_cast<std::uintptr_t>(alignment) - 1;
  const std::uintptr_t aligned = (current + mask) & ~mask;
  const std::size_t actual = size == 0 ? 1 : size;
  if (aligned > reinterpret_cast<std::uintptr_t>(end_) ||
      actual > reinterpret_cast<std::uintptr_t>(end_) - aligned) {
    return nullptr;
  }
  cursor_ = reinterpret_cast<std::uint8_t*>(aligned) + actual;
  return reinterpret_cast<void*>(aligned);
}

void PlatformFreestanding::dealloc(void* /*pointer*/) {
  // Region allocation: individual frees are not part of the model.
}

void* PlatformFreestanding::lookupSymbol(const char* /*name*/) {
  // No dynamic linker in a freestanding binary: `extern "C"` symbols are
  // link-time relocations. Reporting "not found" is the honest answer, and the
  // caller turns it into a clear error rather than a wild call.
  return nullptr;
}

#else  // not Linux x86-64

// The class must compile everywhere the dev library does (Windows and every
// CI compiler), but there is no syscall gate to reach for. Every operation
// fails loudly rather than silently pretending to work.
#include <stdexcept>

namespace {
[[noreturn]] void unsupported() {
  throw std::runtime_error("native/platform_freestanding: only linux-x86_64 is implemented");
}
}  // namespace

void PlatformFreestanding::write(int, const char*, std::size_t) { unsupported(); }
void PlatformFreestanding::writeErr(const char*, std::size_t) { unsupported(); }
void PlatformFreestanding::flush(int) { unsupported(); }
std::size_t PlatformFreestanding::read(int, char*, std::size_t) { unsupported(); }
int PlatformFreestanding::open(const char*, OpenMode) { unsupported(); }
void PlatformFreestanding::close(int) { unsupported(); }
bool PlatformFreestanding::seek(int, std::int64_t, SeekOrigin) { unsupported(); }
std::int64_t PlatformFreestanding::tell(int) { unsupported(); }
bool PlatformFreestanding::remove(const char*) { unsupported(); }
bool PlatformFreestanding::rename(const char*, const char*) { unsupported(); }
bool PlatformFreestanding::makeDirectory(const char*) { unsupported(); }
bool PlatformFreestanding::removeDirectory(const char*) { unsupported(); }
bool PlatformFreestanding::fileStat(const char*, Platform::FileInfo&) { unsupported(); }
bool PlatformFreestanding::listDirectory(const char*, Platform::DirectoryVisitor, void*) { unsupported(); }
bool PlatformFreestanding::currentDirectory(char*, std::size_t) { unsupported(); }
bool PlatformFreestanding::changeDirectory(const char*) { unsupported(); }
void PlatformFreestanding::exit(int) { unsupported(); }
void* PlatformFreestanding::alloc(std::size_t, std::size_t) { unsupported(); }
void PlatformFreestanding::dealloc(void*) { unsupported(); }
void* PlatformFreestanding::lookupSymbol(const char*) { unsupported(); }

#endif
