#pragma once

#include <cstddef>
#include <cstdint>

#include "platform.h"

// Development-engine Platform implementation.
//
// This is the only implementation allowed to use libc and the C++ standard
// library; the freestanding AOT runtime (PHASE 7c) provides its own. It keeps
// no state of its own: standard descriptors 0/1/2 map to the C++ streams and
// every other descriptor is the host's real file descriptor, so `open` and
// `close` behave exactly like the C runtime's.
class PlatformPosix final : public Platform {
 public:
  void write(int fd, const char* data, std::size_t length) override;
  void writeErr(const char* data, std::size_t length) override;
  void flush(int fd) override;
  std::size_t read(int fd, char* buffer, std::size_t length) override;

  int open(const char* path, OpenMode mode) override;
  void close(int fd) override;
  bool seek(int fd, std::int64_t offset, SeekOrigin origin) override;
  std::int64_t tell(int fd) override;

  bool remove(const char* path) override;
  bool rename(const char* from, const char* to) override;
  bool makeDirectory(const char* path) override;
  bool removeDirectory(const char* path) override;
  bool fileStat(const char* path, FileInfo& out) override;
  bool listDirectory(const char* path, DirectoryVisitor visit, void* user) override;
  bool currentDirectory(char* buffer, std::size_t size) override;
  bool changeDirectory(const char* path) override;

  [[noreturn]] void exit(int code) override;

  void* alloc(std::size_t size, std::size_t alignment) override;
  void dealloc(void* pointer) override;

  void* lookupSymbol(const char* name) override;

  std::uint64_t physicalMemory() const override;
};
