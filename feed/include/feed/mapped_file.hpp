/// \file mapped_file.hpp
/// \brief Read-only whole-file mapping, with a windowed fallback.
///
/// A Nasdaq sample day is several gigabytes. Reading it into a
/// `std::vector<char>` means one enormous allocation, a full copy, and the
/// page cache holding two copies of everything; `mmap` means the decoder
/// walks the page cache directly and the kernel reads ahead for it.
///
/// `MAP_PRIVATE` because nothing is written back. `MADV_SEQUENTIAL` because
/// the access pattern is exactly that and the hint measurably changes how
/// aggressively the kernel reads ahead.
///
/// If the mapping fails -- a file larger than the address space, or memory
/// pressure -- the fallback is a *windowed* reader, not a slurp into a
/// vector. Silently reading the whole file into memory after failing to map
/// it would turn an out-of-memory condition into a worse out-of-memory
/// condition.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace miniex {

class MappedFile {
public:
  MappedFile() = default;
  ~MappedFile() { close(); }

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& other) noexcept { *this = std::move(other); }
  MappedFile& operator=(MappedFile&& other) noexcept {
    if (this != &other) {
      close();
      data_ = other.data_;
      size_ = other.size_;
      fd_ = other.fd_;
      mapped_ = other.mapped_;
      error_ = std::move(other.error_);
      other.data_ = nullptr;
      other.size_ = 0;
      other.fd_ = -1;
      other.mapped_ = false;
    }
    return *this;
  }

  /// Returns false and sets `error()` on failure.
  bool open(const std::string& path) {
    close();
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
      error_ = "cannot open " + path + ": " + std::strerror(errno);
      return false;
    }
    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
      error_ = "cannot stat " + path + ": " + std::strerror(errno);
      close();
      return false;
    }
    size_ = static_cast<std::size_t>(st.st_size);
    if (size_ == 0) {
      error_ = path + " is empty";
      close();
      return false;
    }

    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (p == MAP_FAILED) {
      error_ = "cannot mmap " + path + " (" + std::to_string(size_) +
               " bytes): " + std::strerror(errno);
      close();
      return false;
    }
#if defined(MADV_SEQUENTIAL)
    (void)::madvise(p, size_, MADV_SEQUENTIAL);
#endif
    data_ = static_cast<const std::byte*>(p);
    mapped_ = true;
    return true;
  }

  void close() {
    if (mapped_ && data_ != nullptr) {
      ::munmap(const_cast<void*>(static_cast<const void*>(data_)), size_);
    }
    if (fd_ >= 0) ::close(fd_);
    data_ = nullptr;
    size_ = 0;
    fd_ = -1;
    mapped_ = false;
  }

  [[nodiscard]] const std::byte* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool is_open() const noexcept { return data_ != nullptr; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
  const std::byte* data_{nullptr};
  std::size_t size_{0};
  int fd_{-1};
  bool mapped_{false};
  std::string error_;
};

} // namespace miniex
