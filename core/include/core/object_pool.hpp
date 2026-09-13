/// \file object_pool.hpp
/// \brief Fixed-capacity slab allocator handing out 32-bit indices.
///
/// The matching engine must never allocate after construction: a `malloc` on
/// the hot path is a lock, a potential page fault, and an unbounded tail
/// latency event all at once. Every resting order therefore lives in one
/// contiguous slab allocated at start-up, and is referred to by a
/// `std::uint32_t` index rather than a pointer (see DD-005).
///
/// The free list is threaded through the *unused* slots, reusing each
/// element's own `next` field. That costs zero extra memory and keeps the
/// free list in the same cache lines the allocator is about to hand out.
///
/// Exhaustion is a normal, testable condition. `allocate()` returns
/// `kInvalidIndex`; it never throws and never grows. The engine maps that to
/// `RejectReason::PoolExhausted` and carries on.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#include "core/cache.hpp"
#include "core/prefault.hpp"
#include "core/types.hpp"

#ifndef NDEBUG
#include <cassert>
#include <vector>
#endif

namespace miniex {

/// An element usable in an ObjectPool exposes a 32-bit `next` field that the
/// pool may scribble on while the element is free.
template <class T>
concept PoolLinkable = std::is_trivially_copyable_v<T> &&
                       std::is_trivially_destructible_v<T> && requires(T t) {
                         { t.next } -> std::same_as<std::uint32_t&>;
                       };

template <PoolLinkable T>
class ObjectPool {
public:
  using value_type = T;

  /// Allocates the whole slab up front. This is the only allocation the pool
  /// ever performs.
  explicit ObjectPool(std::size_t capacity)
      : capacity_(capacity),
        slab_(static_cast<T*>(::operator new[](sizeof(T) * capacity,
                                               std::align_val_t{kCacheLine}))) {
#ifndef NDEBUG
    live_.assign(capacity, false);
#endif
    // reset() threads the free list through every slot, which faults the slab
    // in as a side effect. Doing it explicitly first means the guarantee does
    // not depend on that implementation detail staying true.
    prefault(slab_, sizeof(T) * capacity);
    reset();
  }

  ~ObjectPool() { ::operator delete[](slab_, std::align_val_t{kCacheLine}); }

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;
  ObjectPool(ObjectPool&&) = delete;
  ObjectPool& operator=(ObjectPool&&) = delete;

  /// O(1). Returns `kInvalidIndex` when the pool is exhausted.
  [[nodiscard]] std::uint32_t allocate() noexcept {
    const std::uint32_t idx = free_head_;
    if (idx == kInvalidIndex) return kInvalidIndex;
    free_head_ = slab_[idx].next;
    ++live_count_;
#ifndef NDEBUG
    live_[idx] = true;
#endif
    return idx;
  }

  /// O(1). A double free is caught by assertion in debug builds; in release
  /// it corrupts the free list, which is why the assertion exists.
  void deallocate(std::uint32_t index) noexcept {
#ifndef NDEBUG
    assert(index < capacity_ && "ObjectPool: index out of range");
    assert(live_[index] && "ObjectPool: double free or free of unallocated");
    live_[index] = false;
#endif
    slab_[index].next = free_head_;
    free_head_ = index;
    --live_count_;
  }

  [[nodiscard]] T& operator[](std::uint32_t index) noexcept {
#ifndef NDEBUG
    assert(index < capacity_);
#endif
    return slab_[index];
  }
  [[nodiscard]] const T& operator[](std::uint32_t index) const noexcept {
#ifndef NDEBUG
    assert(index < capacity_);
#endif
    return slab_[index];
  }

  /// Rebuilds the free list over the whole slab. O(capacity); start-up and
  /// test-teardown only, never on the hot path.
  void reset() noexcept {
    const auto n = static_cast<std::uint32_t>(capacity_);
    for (std::uint32_t i = 0; i + 1 < n; ++i) slab_[i].next = i + 1;
    if (n > 0) slab_[n - 1].next = kInvalidIndex;
    free_head_ = n > 0 ? 0U : kInvalidIndex;
    live_count_ = 0;
#ifndef NDEBUG
    // assign() could reallocate and therefore throw; the vector is already
    // sized by the constructor, so fill it in place and keep reset() honest
    // about being noexcept.
    std::fill(live_.begin(), live_.end(), false);
#endif
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t live_count() const noexcept { return live_count_; }
  [[nodiscard]] std::size_t free_count() const noexcept {
    return capacity_ - live_count_;
  }
  [[nodiscard]] bool exhausted() const noexcept {
    return free_head_ == kInvalidIndex;
  }

  /// Debug helper used by the invariant suite: walks the free list and checks
  /// that live + free == capacity and that the list contains no cycle.
  [[nodiscard]] bool verify() const noexcept {
    std::size_t seen = 0;
    std::uint32_t idx = free_head_;
    while (idx != kInvalidIndex) {
      if (idx >= capacity_) return false;
      if (++seen > capacity_) return false; // cycle
      idx = slab_[idx].next;
    }
    return seen == free_count();
  }

private:
  std::size_t capacity_;
  T* slab_;
  std::uint32_t free_head_{kInvalidIndex};
  std::size_t live_count_{0};
#ifndef NDEBUG
  std::vector<bool> live_;
#endif
};

} // namespace miniex
