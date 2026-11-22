#include "platform_posix.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#include <sys/stat.h>
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

bool isFileFd(int fd) { return fd > Platform::kStderr; }

int seekWhence(Platform::SeekOrigin origin) {
  switch (origin) {
    case Platform::SeekOrigin::Start:
      return SEEK_SET;
    case Platform::SeekOrigin::Current:
      return SEEK_CUR;
    case Platform::SeekOrigin::End:
      return SEEK_END;
  }
  return SEEK_SET;
}

}  // namespace

void PlatformPosix::write(int fd, const char* data, std::size_t length) {
  if (length == 0) return;
  if (fd == kStdout) {
    std::cout.write(data, static_cast<std::streamsize>(length));
    return;
  }
  if (fd == kStderr) {
    std::cerr.write(data, static_cast<std::streamsize>(length));
    return;
  }
  if (!isFileFd(fd)) return;

  std::size_t written = 0;
  while (written < length) {
#ifdef _WIN32
    int n = ::_write(fd, data + written, static_cast<unsigned int>(length - written));
#else
    ssize_t n = ::write(fd, data + written, length - written);
#endif
    if (n <= 0) break;
    written += static_cast<std::size_t>(n);
  }
}

void PlatformPosix::writeErr(const char* data, std::size_t length) {
  if (length == 0) return;
  std::cerr.write(data, static_cast<std::streamsize>(length));
}

void PlatformPosix::flush(int fd) {
  if (fd == kStdout) {
    std::cout.flush();
  } else if (fd == kStderr) {
    std::cerr.flush();
  }
  // File descriptors are written unbuffered, so there is nothing to flush.
}

std::size_t PlatformPosix::read(int fd, char* buffer, std::size_t length) {
  if (length == 0) return 0;
  if (fd == kStdin) {
    std::cin.read(buffer, static_cast<std::streamsize>(length));
    return static_cast<std::size_t>(std::cin.gcount());
  }
  if (!isFileFd(fd)) return 0;

#ifdef _WIN32
  int n = ::_read(fd, buffer, static_cast<unsigned int>(length));
#else
  ssize_t n = ::read(fd, buffer, length);
#endif
  return n > 0 ? static_cast<std::size_t>(n) : 0;
}

int PlatformPosix::open(const char* path, OpenMode mode) {
#ifdef _WIN32
  int flags = _O_RDONLY | _O_BINARY;
  if (mode == OpenMode::Write) flags = _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY;
  if (mode == OpenMode::Append) flags = _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY;
  // `_open` takes an ANSI path; `_wopen` keeps a UTF-8 path intact.
  const std::wstring wide = fs::u8path(path).wstring();
  return ::_wopen(wide.c_str(), flags, _S_IREAD | _S_IWRITE);
#else
  int flags = O_RDONLY;
  if (mode == OpenMode::Write) flags = O_WRONLY | O_CREAT | O_TRUNC;
  if (mode == OpenMode::Append) flags = O_WRONLY | O_CREAT | O_APPEND;
  return ::open(path, flags, 0644);
#endif
}

void PlatformPosix::close(int fd) {
  // Standard descriptors belong to the host; closing them would break the
  // process for every later write.
  if (!isFileFd(fd)) return;
#ifdef _WIN32
  ::_close(fd);
#else
  ::close(fd);
#endif
}

bool PlatformPosix::seek(int fd, std::int64_t offset, SeekOrigin origin) {
  if (!isFileFd(fd)) return false;
#ifdef _WIN32
  return ::_lseeki64(fd, offset, seekWhence(origin)) >= 0;
#else
  return ::lseek(fd, static_cast<off_t>(offset), seekWhence(origin)) >= 0;
#endif
}

std::int64_t PlatformPosix::tell(int fd) {
  if (!isFileFd(fd)) return -1;
#ifdef _WIN32
  return static_cast<std::int64_t>(::_lseeki64(fd, 0, SEEK_CUR));
#else
  return static_cast<std::int64_t>(::lseek(fd, 0, SEEK_CUR));
#endif
}

bool PlatformPosix::remove(const char* path) {
  std::error_code error;
  const bool removed = fs::remove(fs::u8path(path), error);
  return removed && !error;
}

bool PlatformPosix::rename(const char* from, const char* to) {
  std::error_code error;
  fs::rename(fs::u8path(from), fs::u8path(to), error);
  return !error;
}

bool PlatformPosix::makeDirectory(const char* path) {
  std::error_code error;
  fs::create_directory(fs::u8path(path), error);
  return !error;
}

bool PlatformPosix::removeDirectory(const char* path) {
  // `std::filesystem::remove` also deletes a file, so it would make `fsRmdir`
  // succeed on a regular file. `rmdir` keeps the "directory only" contract.
#ifdef _WIN32
  return ::_wrmdir(fs::u8path(path).c_str()) == 0;
#else
  return ::rmdir(path) == 0;
#endif
}

bool PlatformPosix::fileStat(const char* path, Platform::FileInfo& out) {
#ifdef _WIN32
  // `_stat64` takes an ANSI path; `_wstat64` keeps a UTF-8 path intact.
  struct _stat64 info;
  if (::_wstat64(fs::u8path(path).c_str(), &info) != 0) return false;
  out.size = static_cast<std::uint64_t>(info.st_size);
  out.modified = static_cast<std::int64_t>(info.st_mtime);
  out.isDirectory = (info.st_mode & _S_IFDIR) != 0;
  out.isFile = (info.st_mode & _S_IFREG) != 0;
#else
  struct stat info;
  if (::stat(path, &info) != 0) return false;
  out.size = static_cast<std::uint64_t>(info.st_size);
  out.modified = static_cast<std::int64_t>(info.st_mtime);
  out.isDirectory = S_ISDIR(info.st_mode);
  out.isFile = S_ISREG(info.st_mode);
#endif
  return true;
}

bool PlatformPosix::listDirectory(const char* path, Platform::DirectoryVisitor visit,
                                  void* user) {
  std::error_code error;
  fs::directory_iterator entry(fs::u8path(path), error);
  if (error) return false;
  const fs::directory_iterator end;
  for (; entry != end; entry.increment(error)) {
    if (error) return false;
    std::error_code typeError;
    const bool isDirectory = entry->is_directory(typeError);
    const std::string name = entry->path().filename().u8string();
    visit(user, name.c_str(), isDirectory);
  }
  return true;
}

bool PlatformPosix::currentDirectory(char* buffer, std::size_t size) {
  std::error_code error;
  const fs::path path = fs::current_path(error);
  if (error) return false;
  const std::string text = path.u8string();
  if (text.size() + 1 > size) return false;
  std::memcpy(buffer, text.c_str(), text.size() + 1);
  return true;
}

bool PlatformPosix::changeDirectory(const char* path) {
  std::error_code error;
  fs::current_path(fs::u8path(path), error);
  return !error;
}

void PlatformPosix::exit(int code) { std::exit(code); }

void* PlatformPosix::alloc(std::size_t size, std::size_t alignment) {
  if (size == 0) size = 1;
  // Round up to a power of two no smaller than a pointer, which is what
  // posix_memalign requires and what _aligned_malloc expects in practice.
  std::size_t a = sizeof(void*);
  while (a < alignment) a <<= 1;
#ifdef _WIN32
  return _aligned_malloc(size, a);
#else
  void* pointer = nullptr;
  if (posix_memalign(&pointer, a, size) != 0) return nullptr;
  return pointer;
#endif
}

void PlatformPosix::dealloc(void* pointer) {
#ifdef _WIN32
  _aligned_free(pointer);
#else
  std::free(pointer);
#endif
}

void* PlatformPosix::lookupSymbol(const char* name) {
  // `RTLD_DEFAULT` searches the global symbol scope: the executable and every
  // shared object already loaded, which is where libc, libm and the host
  // program's own C symbols live. That is exactly the set an `extern "C"`
  // declaration can name, and it needs no `-rdynamic` on the host.
#ifdef _WIN32
  // Windows resolves through a module handle; `GetProcAddress` needs one, so
  // the search walks the modules a declared symbol can plausibly live in:
  //   - the process image (`GetModuleHandleA(nullptr)`), for a symbol the host
  //     itself exports,
  //   - kernel32.dll, the Win32 base API (`GetTickCount`, `Sleep`,
  //     `GetCurrentProcessId`, ...), which has no CRT equivalent,
  //   - the C runtime, which carries the libc-style names (`strlen`, `abs`,
  //     `sqrt`, ...) the POSIX path finds through `dlsym(RTLD_DEFAULT)`.
  // Each module is tried in turn; the first hit wins. A symbol in none of them
  // returns nullptr, which the caller turns into a clear "not found" error.
  HMODULE module = GetModuleHandleA(nullptr);
  if (module != nullptr) {
    if (FARPROC symbol = GetProcAddress(module, name)) {
      return reinterpret_cast<void*>(symbol);
    }
  }
  HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
  if (kernel32 != nullptr) {
    if (FARPROC symbol = GetProcAddress(kernel32, name)) {
      return reinterpret_cast<void*>(symbol);
    }
  }
  HMODULE runtime = GetModuleHandleA("ucrtbase.dll");
  if (runtime == nullptr) runtime = GetModuleHandleA("msvcrt.dll");
  if (runtime != nullptr) {
    if (FARPROC symbol = GetProcAddress(runtime, name)) {
      return reinterpret_cast<void*>(symbol);
    }
  }
  // A third-party library the host loaded (`SDL3.dll`, a plugin, ...) is not
  // one of the modules above. Walk every module the process has loaded; the
  // snapshot is kernel32's own, so no extra library is needed. The fast paths
  // above stay first because they cover the common case without the walk.
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snapshot != INVALID_HANDLE_VALUE) {
    MODULEENTRY32 entry;
    entry.dwSize = sizeof(entry);
    if (Module32First(snapshot, &entry)) {
      do {
        if (FARPROC symbol = GetProcAddress(entry.hModule, name)) {
          CloseHandle(snapshot);
          return reinterpret_cast<void*>(symbol);
        }
      } while (Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
  }
  return nullptr;
#else
  return dlsym(RTLD_DEFAULT, name);
#endif
}

Platform& platformPosix() {
  static PlatformPosix instance;
  return instance;
}

// Total physical memory of the machine, in bytes, or 0 when it cannot be
// determined. Windows reports it directly; on POSIX it is the number of
// physical pages times the page size. Both are process-independent machine
// properties, which is what `memSize()` means.
std::uint64_t PlatformPosix::physicalMemory() const {
#ifdef _WIN32
  MEMORYSTATUSEX status;
  status.dwLength = sizeof status;
  if (GlobalMemoryStatusEx(&status)) {
    return static_cast<std::uint64_t>(status.ullTotalPhys);
  }
  return 0;
#else
  long pages = ::sysconf(_SC_PHYS_PAGES);
  long pageSize = ::sysconf(_SC_PAGE_SIZE);
  if (pages <= 0 || pageSize <= 0) return 0;
  return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageSize);
#endif
}
