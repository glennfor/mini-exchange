/// \file shm_ring.hpp
/// \brief The engine-side writer and the attach/create plumbing for the tap.
///
/// ## The hard constraints (spec 19.1)
///
/// * The engine-side write is **exactly one bounded, non-blocking
///   operation**: copy a fixed-size record into the ring and advance the head
///   with a release store. No branches beyond the full check, no syscalls, no
///   locks, no allocation.
/// * A full ring increments `dropped` and returns. It never blocks, never
///   resizes, never waits. **Dropping observability data is correct;
///   stalling matching to feed a dashboard is not.**
/// * The reader is **untrusted from the engine's perspective**. If it dies,
///   stalls, or is never started at all, the engine is unaffected -- it will
///   fill the ring, start dropping, and carry on at exactly the same speed.
///   There is a test that kills the reader mid-run.
///
/// The consequence of the last point is that the engine never reads anything
/// the reader writes except the tail, and it only reads that when the ring
/// *appears* full. A reader that stops advancing the tail costs the engine one
/// extra load per record and nothing else.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/timestamp.hpp"

#include "viz/tap_layout.hpp"

namespace miniex {

/// Maps a POSIX shared-memory segment and lays the tap structures over it.
///
/// Owns the mapping, not the data: several of these may exist over one
/// segment (one per process), and the structures inside are designed for
/// exactly one writer and one reader.
class ShmTapSegment {
public:
  ShmTapSegment() = default;
  ~ShmTapSegment() { close(); }

  ShmTapSegment(const ShmTapSegment&) = delete;
  ShmTapSegment& operator=(const ShmTapSegment&) = delete;

  /// Creates (or truncates) a segment and initialises the header.
  /// \param name a POSIX shm name; a leading '/' is added if absent.
  bool create(const std::string& name, std::size_t capacity) {
    close();
    if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
      error_ = "tap capacity must be a power of two and at least 2";
      return false;
    }
    name_ = normalise(name);
    size_ = tap_segment_size(capacity);

    fd_ = ::shm_open(name_.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
      error_ = "shm_open(" + name_ + "): " + std::strerror(errno);
      return false;
    }
    if (::ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
      error_ = "ftruncate: " + std::string(std::strerror(errno));
      close();
      return false;
    }
    if (!map()) return false;

    // Zero first, then stamp the header last -- a reader that attaches while
    // this is happening must not see a valid magic over uninitialised
    // cursors.
    std::memset(base_, 0, size_);
    TapHeader h{};
    h.magic = kTapMagic;
    h.version = kTapVersion;
    h.record_size = sizeof(TapRecord);
    h.capacity = capacity;
    h.flags = kTapFlagProducerLive;
    h.created_ns = now_steady_ns();
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(base_, &h, sizeof(h));
    capacity_ = capacity;
    owner_ = true;
    return true;
  }

  /// Attaches to an existing segment, refusing on a version or magic
  /// mismatch. Refusing is the point: reinterpreting bytes from a different
  /// layout produces a dashboard that is confidently wrong.
  bool attach(const std::string& name) {
    close();
    name_ = normalise(name);
    fd_ = ::shm_open(name_.c_str(), O_RDWR, 0600);
    if (fd_ < 0) {
      error_ = "shm_open(" + name_ + "): " + std::strerror(errno);
      return false;
    }
    struct stat st{};
    if (::fstat(fd_, &st) != 0 ||
        static_cast<std::size_t>(st.st_size) < kTapDataOffset) {
      error_ = "segment is too small to contain a tap header";
      close();
      return false;
    }
    size_ = static_cast<std::size_t>(st.st_size);
    if (!map()) return false;

    TapHeader h{};
    std::memcpy(&h, base_, sizeof(h));
    if (h.magic != kTapMagic) {
      error_ = "bad magic: not a miniex tap segment";
      close();
      return false;
    }
    if (h.version != kTapVersion) {
      error_ = "tap version mismatch: segment is v" +
               std::to_string(h.version) + ", this build speaks v" +
               std::to_string(kTapVersion);
      close();
      return false;
    }
    if (h.record_size != sizeof(TapRecord)) {
      error_ =
          "record size mismatch: segment says " + std::to_string(h.record_size);
      close();
      return false;
    }
    if (size_ < tap_segment_size(h.capacity)) {
      error_ = "segment is smaller than its header claims";
      close();
      return false;
    }
    capacity_ = h.capacity;
    return true;
  }

  void close() {
    if (base_ != nullptr) {
      if (owner_) header()->flags &= ~kTapFlagProducerLive;
      ::munmap(base_, size_);
      base_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    capacity_ = 0;
    size_ = 0;
    owner_ = false;
  }

  /// Removes the name from the filesystem. The mapping survives in any
  /// process that already has it.
  static bool unlink(const std::string& name) {
    return ::shm_unlink(normalise(name).c_str()) == 0;
  }

  [[nodiscard]] bool attached() const { return base_ != nullptr; }
  [[nodiscard]] const std::string& error() const { return error_; }
  [[nodiscard]] std::size_t capacity() const { return capacity_; }
  [[nodiscard]] std::size_t size() const { return size_; }
  [[nodiscard]] const std::string& name() const { return name_; }

  [[nodiscard]] TapHeader* header() {
    return reinterpret_cast<TapHeader*>(at(kTapHeaderOffset));
  }
  [[nodiscard]] std::atomic<std::uint64_t>* head() {
    return reinterpret_cast<std::atomic<std::uint64_t>*>(at(kTapHeadOffset));
  }
  [[nodiscard]] std::atomic<std::uint64_t>* tail() {
    return reinterpret_cast<std::atomic<std::uint64_t>*>(at(kTapTailOffset));
  }
  [[nodiscard]] TapCounters* counters() {
    return reinterpret_cast<TapCounters*>(at(kTapCountersOffset));
  }
  [[nodiscard]] TapStats* stats() {
    return reinterpret_cast<TapStats*>(at(kTapStatsOffset));
  }
  [[nodiscard]] TapRecord* records() {
    return reinterpret_cast<TapRecord*>(at(kTapDataOffset));
  }

private:
  static std::string normalise(const std::string& name) {
    return name.empty() || name[0] == '/' ? name : "/" + name;
  }

  bool map() {
    void* p =
        ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) {
      error_ = "mmap: " + std::string(std::strerror(errno));
      close();
      return false;
    }
    base_ = static_cast<std::byte*>(p);
    return true;
  }

  [[nodiscard]] std::byte* at(std::size_t offset) { return base_ + offset; }

  std::string name_;
  std::string error_;
  std::byte* base_{nullptr};
  std::size_t size_{0};
  std::size_t capacity_{0};
  int fd_{-1};
  bool owner_{false};
};

/// The engine-side writer. Constructed once; `write()` is the hot path.
class ShmTapWriter {
public:
  ShmTapWriter() = default;

  bool create(const std::string& name, std::size_t capacity) {
    if (!segment_.create(name, capacity)) return false;
    bind();
    return true;
  }
  bool attach(const std::string& name) {
    if (!segment_.attach(name)) return false;
    bind();
    return true;
  }

  /// The whole hot path. One bounded store sequence, no syscalls, no locks.
  ///
  /// Returns false when the record was dropped because the ring was full,
  /// having incremented the drop counter. The caller must not react to that
  /// beyond noting it: back-pressuring the matching engine on behalf of a
  /// dashboard would be exactly backwards.
  bool write(const TapRecord& record) noexcept {
    if (records_ == nullptr) return false;
    const std::uint64_t head = head_->load(std::memory_order_relaxed);
    if (head - cached_tail_ >= capacity_) {
      // The only place the engine reads anything the reader wrote, and only
      // when the ring looks full. A dead reader therefore costs one load per
      // record and nothing else.
      cached_tail_ = tail_->load(std::memory_order_acquire);
      if (head - cached_tail_ >= capacity_) {
        ++counters_->dropped;
        return false;
      }
    }
    records_[head & mask_] = record;
    head_->store(head + 1, std::memory_order_release);
    ++counters_->produced;
    return true;
  }

  /// Publishes a statistics snapshot under the seqlock. Called periodically
  /// -- once per 100 ms of engine time or per million events -- never per
  /// event.
  void publish_stats(const TapStats& snapshot) noexcept {
    if (stats_ == nullptr) return;
    auto* seq = reinterpret_cast<std::atomic<std::uint64_t>*>(&stats_->seq);
    const std::uint64_t start = seq->load(std::memory_order_relaxed);
    seq->store(start + 1, std::memory_order_relaxed); // odd: writing
    std::atomic_thread_fence(std::memory_order_release);
    stats_->ops_per_sec = snapshot.ops_per_sec;
    stats_->p50_ns = snapshot.p50_ns;
    stats_->p99_ns = snapshot.p99_ns;
    stats_->p999_ns = snapshot.p999_ns;
    stats_->max_ns = snapshot.max_ns;
    stats_->orders_resting = snapshot.orders_resting;
    stats_->schedule_misses = snapshot.schedule_misses;
    std::atomic_thread_fence(std::memory_order_release);
    seq->store(start + 2, std::memory_order_relaxed); // even: readable
  }

  [[nodiscard]] std::uint64_t produced() const {
    return counters_ == nullptr ? 0 : counters_->produced;
  }
  [[nodiscard]] std::uint64_t dropped() const {
    return counters_ == nullptr ? 0 : counters_->dropped;
  }
  [[nodiscard]] bool ready() const { return records_ != nullptr; }
  [[nodiscard]] const std::string& error() const { return segment_.error(); }
  [[nodiscard]] ShmTapSegment& segment() { return segment_; }

private:
  void bind() {
    head_ = segment_.head();
    tail_ = segment_.tail();
    counters_ = segment_.counters();
    stats_ = segment_.stats();
    records_ = segment_.records();
    capacity_ = segment_.capacity();
    mask_ = capacity_ - 1;
    cached_tail_ = tail_->load(std::memory_order_acquire);
  }

  ShmTapSegment segment_;
  std::atomic<std::uint64_t>* head_{nullptr};
  std::atomic<std::uint64_t>* tail_{nullptr};
  TapCounters* counters_{nullptr};
  TapStats* stats_{nullptr};
  TapRecord* records_{nullptr};
  std::uint64_t capacity_{0};
  std::uint64_t mask_{0};
  std::uint64_t cached_tail_{0};
};

/// The reader side, used by the C++ tests. The production reader is Python
/// (`viz/server/shm_reader.py`); this exists so the ring's semantics can be
/// tested without a Python process in the loop.
class ShmTapReader {
public:
  bool attach(const std::string& name) {
    if (!segment_.attach(name)) return false;
    head_ = segment_.head();
    tail_ = segment_.tail();
    counters_ = segment_.counters();
    stats_ = segment_.stats();
    records_ = segment_.records();
    capacity_ = segment_.capacity();
    mask_ = capacity_ - 1;
    return true;
  }

  /// Drains up to \p max records. Batched on purpose: the reader is allowed
  /// to be as slow as it likes, and batching is where its throughput comes
  /// from.
  std::size_t read(TapRecord* out, std::size_t max) {
    if (records_ == nullptr) return 0;
    const std::uint64_t tail = tail_->load(std::memory_order_relaxed);
    const std::uint64_t head = head_->load(std::memory_order_acquire);
    std::size_t available = static_cast<std::size_t>(head - tail);
    if (available > max) available = max;
    for (std::size_t i = 0; i < available; ++i)
      out[i] = records_[(tail + i) & mask_];
    tail_->store(tail + available, std::memory_order_release);
    return available;
  }

  /// Reads the statistics snapshot, retrying until it gets a torn-free one.
  bool read_stats(TapStats& out, int attempts = 16) {
    if (stats_ == nullptr) return false;
    auto* seq = reinterpret_cast<std::atomic<std::uint64_t>*>(&stats_->seq);
    for (int i = 0; i < attempts; ++i) {
      const std::uint64_t before = seq->load(std::memory_order_relaxed);
      if ((before & 1U) != 0) continue; // a write is in progress
      std::atomic_thread_fence(std::memory_order_acquire);
      out = *stats_;
      std::atomic_thread_fence(std::memory_order_acquire);
      if (seq->load(std::memory_order_relaxed) == before) {
        out.seq = before;
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::uint64_t produced() const {
    return counters_ == nullptr ? 0 : counters_->produced;
  }
  [[nodiscard]] std::uint64_t dropped() const {
    return counters_ == nullptr ? 0 : counters_->dropped;
  }
  [[nodiscard]] std::uint64_t lag() const {
    return head_ == nullptr ? 0
                            : head_->load(std::memory_order_acquire) -
                                  tail_->load(std::memory_order_relaxed);
  }
  [[nodiscard]] bool producer_live() const {
    return segment_.attached() &&
           (const_cast<ShmTapSegment&>(segment_).header()->flags &
            kTapFlagProducerLive) != 0;
  }
  [[nodiscard]] const std::string& error() const { return segment_.error(); }

private:
  ShmTapSegment segment_;
  std::atomic<std::uint64_t>* head_{nullptr};
  std::atomic<std::uint64_t>* tail_{nullptr};
  TapCounters* counters_{nullptr};
  TapStats* stats_{nullptr};
  TapRecord* records_{nullptr};
  std::uint64_t capacity_{0};
  std::uint64_t mask_{0};
};

} // namespace miniex
