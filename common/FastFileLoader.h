// FIX (Bug 2): <span> is C++20.  Replaced std::span<const uint8_t> with a
// plain ByteView struct (pointer + size) that is fully C++17 compatible and
// carries the same semantics: a non-owning, read-only view of the mapped bytes.
#pragma once
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tsh {

/// Non-owning read-only view over a contiguous byte range (C++17 replacement
/// for std::span<const uint8_t>).
struct ByteView {
  const uint8_t *ptr = nullptr;
  std::size_t size = 0;

  const uint8_t *begin() const { return ptr; }
  const uint8_t *end() const { return ptr + size; }
  bool empty() const { return size == 0; }
};

class FastFileLoader {
  int fd = -1;
  void *mapped_data = MAP_FAILED;
  size_t file_size = 0;

public:
  explicit FastFileLoader(const std::string &path) {
    fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
      throw std::runtime_error("Could not open file for zero-copy loading.");

    struct stat st{};
    if (fstat(fd, &st) != 0) {
      close(fd);
      fd = -1;
      throw std::runtime_error("fstat failed.");
    }
    file_size = static_cast<size_t>(st.st_size);
    mapped_data = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped_data == MAP_FAILED) {
      close(fd);
      fd = -1;
      throw std::runtime_error("mmap failed.");
    }
  }

  ~FastFileLoader() {
    if (mapped_data != MAP_FAILED)
      munmap(mapped_data, file_size);
    if (fd >= 0)
      close(fd);
  }

  // Non-copyable, non-movable (owns the mapping).
  FastFileLoader(const FastFileLoader &) = delete;
  FastFileLoader &operator=(const FastFileLoader &) = delete;

  /// Returns a C++17-safe non-owning view of the mapped bytes.
  ByteView data() const {
    return {static_cast<const uint8_t *>(mapped_data), file_size};
  }

  std::size_t size() const { return file_size; }
};

} // namespace tsh