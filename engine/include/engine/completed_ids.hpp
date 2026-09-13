/// \file completed_ids.hpp
/// \brief Bounded memory of recently-completed order ids.
///
/// Cancelling an order that no longer exists has two distinct causes, and an
/// exchange must tell them apart: the id never existed (a client bug, reject
/// `UnknownOrder`) or the order filled or was cancelled a moment ago (a race
/// the client could not have avoided, reject `TooLateToCancel`).
///
/// Answering that exactly would mean remembering every id for the whole run,
/// which is unbounded memory. Instead this keeps the last N completed ids in
/// a FIFO ring with a hash set beside it for O(1) membership. The semantics
/// at the boundary are therefore explicit and documented rather than
/// accidental: **an order that completed more than N completions ago reports
/// `UnknownOrder`, not `TooLateToCancel`.** N is `kCompletedRingCapacity`
/// (65536 by default, a CMake cache variable), which at realistic completion
/// rates is several seconds of history -- far longer than any in-flight
/// cancel.
///
/// The Python reference oracle implements the same bounded ring with the same
/// capacity, so the two agree at the boundary and not merely in the middle.
#pragma once

#include <cstddef>
#include <vector>

#include "core/open_hash_map.hpp"
#include "core/types.hpp"

namespace miniex {

class CompletedIdRing {
public:
  explicit CompletedIdRing(std::size_t capacity)
      : capacity_(capacity), ring_(capacity, 0), set_(capacity) {}

  /// Records an id as completed, evicting the oldest if the ring is full.
  void insert(OrderId id) {
    if (capacity_ == 0) return;
    if (set_.contains(id)) return;
    if (count_ == capacity_) {
      (void)set_.erase(ring_[head_]);
      --count_;
    }
    ring_[head_] = id;
    head_ = (head_ + 1) % capacity_;
    ++count_;
    // The marker value is arbitrary; only membership matters.
    (void)set_.insert(id, 1);
  }

  [[nodiscard]] bool contains(OrderId id) const { return set_.contains(id); }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  void clear() {
    set_.clear();
    head_ = 0;
    count_ = 0;
  }

private:
  std::size_t capacity_;
  std::vector<OrderId> ring_;
  OpenHashMap set_;
  std::size_t head_{0};
  std::size_t count_{0};
};

} // namespace miniex
