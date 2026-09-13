/// \file spsc_ring.hpp
/// \brief Lock-free single-producer / single-consumer ring buffer.
///
/// This is the ingress queue between the load generator (or feed thread) and
/// the single-threaded matching core, and the same structure backs the
/// observability tap. It is written by hand rather than pulled from a
/// library because its memory ordering and its false-sharing layout are the
/// two things worth being able to explain.
///
/// Design notes, in the order an interviewer tends to ask about them:
///
/// **Monotonic counters, masked indices.** `head_` and `tail_` count total
/// items ever pushed/popped and never wrap in practice (2^64 items at
/// 10^8/s is ~5800 years). The slot is `counter & (Capacity - 1)`, which is
/// one AND instead of a division, and it removes the classic "full vs empty
/// are both head == tail" ambiguity without sacrificing a slot.
///
/// **Separate cache lines with cached opposite counters.** The producer
/// writes `head_` and reads `tail_`; the consumer does the reverse. Putting
/// them on one line means every push invalidates the consumer's copy of the
/// line it is spinning on, and throughput collapses to the cost of a cache
/// line ping-pong (~100ns per item). So each side gets its own
/// `alignas(kDestructiveInterference)` block, *and* each side keeps a
/// private, non-atomic cached copy of the other side's counter. The cached
/// value is only refreshed when the ring looks full (producer) or empty
/// (consumer), so in steady state neither core ever reads the other's line.
///
/// **acquire/release, not seq_cst.** The only thing that must be ordered is
/// "the slot write happens before the counter publish" and its mirror on the
/// read side. That is exactly a release store paired with an acquire load.
/// `seq_cst` would additionally impose a single total order across *all*
/// atomics in the program, which this queue does not need, and on x86-64 it
/// costs an `mfence`/`xchg` on every push.
///
/// **No spinning inside the container.** `try_push`/`try_pop` return false
/// rather than blocking. Whether a full ring means "back off", "drop", or
/// "count it as backpressure and carry on" is a policy question, and policy
/// does not belong in a data structure.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "core/cache.hpp"

#ifndef NDEBUG
#include <cassert>
#include <thread>
#endif

namespace miniex {

/// \tparam T        element type; must be trivially copyable
/// \tparam Capacity number of slots; must be a power of two
template <class T, std::size_t Capacity>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>,
                "SpscRing elements are memcpy'd; they must be trivially "
                "copyable");
  static_assert(std::is_trivially_destructible_v<T>);
  static_assert(Capacity >= 2, "a one-slot ring cannot be lock-free useful");
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two so the index is a mask");

public:
  using value_type = T;
  static constexpr std::size_t kCapacity = Capacity;
  static constexpr std::size_t kMask = Capacity - 1;

  SpscRing()
      : slots_(static_cast<T*>(::operator new[](
            sizeof(T) * Capacity, std::align_val_t{kCacheLine}))) {}

  ~SpscRing() { ::operator delete[](slots_, std::align_val_t{kCacheLine}); }

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;

  // -------------------------------------------------------------------------
  // Producer side
  // -------------------------------------------------------------------------

  /// Push one element. Returns false if the ring is full; the ring is
  /// unchanged in that case.
  [[nodiscard]] bool try_push(const T& value) noexcept {
    check_producer();
    const std::uint64_t head = head_.value.load(std::memory_order_relaxed);
    if (head - cached_tail_ >= Capacity) {
      cached_tail_ = tail_.value.load(std::memory_order_acquire);
      if (head - cached_tail_ >= Capacity) return false;
    }
    slots_[head & kMask] = value;
    head_.value.store(head + 1, std::memory_order_release);
    return true;
  }

  /// Push up to \p n elements. Returns how many were actually moved, which
  /// is min(n, free space). Batching is where the throughput is: one release
  /// store per batch instead of one per element.
  [[nodiscard]] std::size_t try_push_n(const T* src, std::size_t n) noexcept {
    check_producer();
    const std::uint64_t head = head_.value.load(std::memory_order_relaxed);
    std::size_t free_slots =
        Capacity - static_cast<std::size_t>(head - cached_tail_);
    if (free_slots < n) {
      cached_tail_ = tail_.value.load(std::memory_order_acquire);
      free_slots = Capacity - static_cast<std::size_t>(head - cached_tail_);
    }
    const std::size_t count = n < free_slots ? n : free_slots;
    if (count == 0) return 0;

    const std::size_t start = static_cast<std::size_t>(head & kMask);
    const std::size_t first =
        (start + count <= Capacity) ? count : Capacity - start;
    std::memcpy(slots_ + start, src, first * sizeof(T));
    if (first != count) {
      std::memcpy(slots_, src + first, (count - first) * sizeof(T));
    }
    head_.value.store(head + count, std::memory_order_release);
    return count;
  }

  // -------------------------------------------------------------------------
  // Consumer side
  // -------------------------------------------------------------------------

  /// Pop one element into \p out. Returns false if the ring is empty.
  [[nodiscard]] bool try_pop(T& out) noexcept {
    check_consumer();
    const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);
    if (tail == cached_head_) {
      cached_head_ = head_.value.load(std::memory_order_acquire);
      if (tail == cached_head_) return false;
    }
    out = slots_[tail & kMask];
    tail_.value.store(tail + 1, std::memory_order_release);
    return true;
  }

  /// Pop up to \p n elements into \p dst. Returns how many were moved.
  [[nodiscard]] std::size_t try_pop_n(T* dst, std::size_t n) noexcept {
    check_consumer();
    const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);
    std::size_t available = static_cast<std::size_t>(cached_head_ - tail);
    if (available < n) {
      cached_head_ = head_.value.load(std::memory_order_acquire);
      available = static_cast<std::size_t>(cached_head_ - tail);
    }
    const std::size_t count = n < available ? n : available;
    if (count == 0) return 0;

    const std::size_t start = static_cast<std::size_t>(tail & kMask);
    const std::size_t first =
        (start + count <= Capacity) ? count : Capacity - start;
    std::memcpy(dst, slots_ + start, first * sizeof(T));
    if (first != count) {
      std::memcpy(dst + first, slots_, (count - first) * sizeof(T));
    }
    tail_.value.store(tail + count, std::memory_order_release);
    return count;
  }

  // -------------------------------------------------------------------------
  // Observation (approximate when called from a third thread)
  // -------------------------------------------------------------------------

  [[nodiscard]] std::size_t size() const noexcept {
    const std::uint64_t head = head_.value.load(std::memory_order_acquire);
    const std::uint64_t tail = tail_.value.load(std::memory_order_acquire);
    return static_cast<std::size_t>(head - tail);
  }

  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] bool full() const noexcept { return size() >= Capacity; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return Capacity;
  }

  /// Total items ever pushed / popped. Used by the harness to report
  /// backpressure without perturbing the hot path.
  [[nodiscard]] std::uint64_t produced() const noexcept {
    return head_.value.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t consumed() const noexcept {
    return tail_.value.load(std::memory_order_acquire);
  }

private:
  /// A counter pinned to its own coherence unit. The padding is explicit
  /// rather than implied by alignas alone so that `sizeof` is assertable.
  struct alignas(kDestructiveInterference) PaddedCounter {
    std::atomic<std::uint64_t> value{0};
    char pad[kDestructiveInterference - sizeof(std::atomic<std::uint64_t>)]{};
  };

  // Producer line: head_ plus the producer's private view of tail_.
  PaddedCounter head_;
  alignas(kDestructiveInterference) std::uint64_t cached_tail_{0};

  // Consumer line: tail_ plus the consumer's private view of head_.
  PaddedCounter tail_;
  alignas(kDestructiveInterference) std::uint64_t cached_head_{0};

  T* slots_;

  // -------------------------------------------------------------------------
  // Debug-only single-producer / single-consumer enforcement.
  //
  // The contract is unenforceable by the type system, so it is enforced by
  // assertion in debug builds and compiled entirely away in release. Without
  // this, a second producer produces silent, intermittent corruption that
  // costs a day to find.
  // -------------------------------------------------------------------------
#ifndef NDEBUG
  mutable std::atomic<std::thread::id> producer_{std::thread::id{}};
  mutable std::atomic<std::thread::id> consumer_{std::thread::id{}};

  void check_producer() const noexcept {
    claim(producer_, "SpscRing: more than one producer thread");
  }
  void check_consumer() const noexcept {
    claim(consumer_, "SpscRing: more than one consumer thread");
  }
  static void claim(std::atomic<std::thread::id>& slot,
                    const char* what) noexcept {
    const std::thread::id me = std::this_thread::get_id();
    std::thread::id expected{};
    if (!slot.compare_exchange_strong(expected, me,
                                      std::memory_order_relaxed)) {
      assert(expected == me && what);
      (void)what;
    }
  }
#else
  static void check_producer() noexcept {}
  static void check_consumer() noexcept {}
#endif
};

} // namespace miniex
