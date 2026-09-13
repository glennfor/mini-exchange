/// \file open_hash_map.hpp
/// \brief Open-addressed OrderId -> pool-index map.
///
/// This is the hottest lookup in the system. Real equity flow is dominated by
/// cancels and replaces (see docs/benchmarking.md for the measured mix), and
/// every one of them starts by turning an order id into a pool index.
///
/// **Open addressing, not chaining.** A chained map costs a pointer chase
/// into a separately-allocated node for every probe, and the nodes drift
/// apart in memory as the day goes on. Linear probing over a flat array of
/// 16-byte buckets means the common case (load factor <= 0.5, first probe
/// hits) touches exactly one cache line, and a collision usually touches the
/// *same* line, because four buckets fit in one.
///
/// **Backward-shift deletion, not tombstones.** Cancels are the dominant
/// operation, so deletions are not rare — with tombstones the table degrades
/// toward a linear scan over a trading day and needs a rehash policy with a
/// latency cliff in it. Backward-shift deletion keeps every probe sequence
/// contiguous with no tombstones at all, at the cost of a short, bounded
/// shift loop inside `erase`. Trading an unbounded periodic rehash for a
/// bounded per-erase cost is the right call for a latency-sensitive system.
///
/// **Fibonacci hashing, not std::hash.** ITCH order reference numbers are
/// dense and sequential. `std::hash<uint64_t>` is the identity on every
/// mainstream implementation, so sequential ids map to sequential buckets;
/// that is actually *ideal* for insertion locality but pathological once
/// arbitrary subsets are erased and reinserted. Multiplying by the 64-bit
/// golden-ratio constant and taking the high bits spreads sequential ids
/// uniformly while costing one multiply.
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>

#include "core/cache.hpp"
#include "core/prefault.hpp"
#include "core/types.hpp"

#ifndef NDEBUG
#include <cassert>
#endif

namespace miniex {

class OpenHashMap {
public:
  /// One probe touches one bucket, and four buckets share a cache line.
  struct Bucket {
    OrderId key;
    std::uint32_t value; ///< kInvalidIndex marks the bucket empty
    std::uint32_t _pad;
  };
  static_assert(sizeof(Bucket) == 16);

  /// The golden-ratio constant, 2^64 / phi, rounded to an odd number.
  static constexpr std::uint64_t kMix = 0x9E3779B97F4A7C15ULL;

  /// Maximum fraction of buckets that may be occupied. Linear probing
  /// degrades sharply past ~0.7; 0.5 keeps the expected probe count under
  /// 1.5 and costs 16 bytes per live order, which is cheap next to the
  /// 64-byte order record it points at.
  static constexpr std::size_t kLoadFactorPercent = 50;

  /// \param max_entries the largest number of live entries that must fit.
  /// The table is sized to the next power of two >= max_entries * 2.
  explicit OpenHashMap(std::size_t max_entries)
      : capacity_(round_up_pow2((max_entries * 100 / kLoadFactorPercent) + 1)),
        mask_(capacity_ - 1),
        shift_(static_cast<unsigned>(64 - std::countr_zero(capacity_))),
        max_entries_(max_entries),
        buckets_(static_cast<Bucket*>(::operator new[](
            sizeof(Bucket) * capacity_, std::align_val_t{kCacheLine}))) {
    // clear() writes every bucket, which faults the table in. Stated rather
    // than relied upon; see core/prefault.hpp for why it matters.
    prefault(buckets_, sizeof(Bucket) * capacity_);
    clear();
  }

  ~OpenHashMap() {
    ::operator delete[](buckets_, std::align_val_t{kCacheLine});
  }

  OpenHashMap(const OpenHashMap&) = delete;
  OpenHashMap& operator=(const OpenHashMap&) = delete;
  OpenHashMap(OpenHashMap&&) = delete;
  OpenHashMap& operator=(OpenHashMap&&) = delete;

  void clear() noexcept {
    for (std::size_t i = 0; i < capacity_; ++i) {
      buckets_[i].key = 0;
      buckets_[i].value = kInvalidIndex;
      buckets_[i]._pad = 0;
    }
    size_ = 0;
  }

  /// Insert a new key. Returns false if the key is already present or the
  /// table is at its configured entry ceiling. Never allocates.
  [[nodiscard]] bool insert(OrderId key, std::uint32_t value) noexcept {
#ifndef NDEBUG
    assert(value != kInvalidIndex && "kInvalidIndex is the empty marker");
#endif
    if (size_ >= max_entries_) return false;
    std::size_t i = home(key);
    // The static analyser reports a garbage read on the line below: it cannot
    // see that the constructor's clear() writes every bucket behind an
    // ::operator new[] that returns void*. Every bucket is initialised before
    // any read; see the constructor.
    // NOLINTNEXTLINE(clang-analyzer-core.UndefinedBinaryOperatorResult)
    while (buckets_[i].value != kInvalidIndex) {
      if (buckets_[i].key == key) return false; // duplicate
      i = (i + 1) & mask_;
    }
    buckets_[i].key = key;
    buckets_[i].value = value;
    ++size_;
    return true;
  }

  /// Returns the stored index, or kInvalidIndex if the key is absent.
  [[nodiscard]] std::uint32_t find(OrderId key) const noexcept {
    std::size_t i = home(key);
    while (buckets_[i].value != kInvalidIndex) {
      if (buckets_[i].key == key) return buckets_[i].value;
      i = (i + 1) & mask_;
    }
    return kInvalidIndex;
  }

  [[nodiscard]] bool contains(OrderId key) const noexcept {
    return find(key) != kInvalidIndex;
  }

  /// Overwrite the value of an existing key. Returns false if absent.
  [[nodiscard]] bool assign(OrderId key, std::uint32_t value) noexcept {
    std::size_t i = home(key);
    while (buckets_[i].value != kInvalidIndex) {
      if (buckets_[i].key == key) {
        buckets_[i].value = value;
        return true;
      }
      i = (i + 1) & mask_;
    }
    return false;
  }

  /// Backward-shift deletion. Returns false if the key was absent.
  ///
  /// After removing the entry at slot `hole`, every following element of the
  /// probe run is examined: an element may stay where it is only if its home
  /// slot lies cyclically inside (hole, current]; otherwise the hole is
  /// "in front of" it and it must slide back, or a later lookup would stop
  /// at the hole and miss it.
  [[nodiscard]] bool erase(OrderId key) noexcept {
    std::size_t hole = home(key);
    while (buckets_[hole].value != kInvalidIndex) {
      if (buckets_[hole].key == key) break;
      hole = (hole + 1) & mask_;
    }
    if (buckets_[hole].value == kInvalidIndex) return false;

    std::size_t scan = hole;
    for (;;) {
      scan = (scan + 1) & mask_;
      if (buckets_[scan].value == kInvalidIndex) break;
      const std::size_t h = home(buckets_[scan].key);
      const bool wrapped = scan < hole;
      const bool must_move =
          wrapped ? (h <= hole && h > scan) : (h <= hole || h > scan);
      if (must_move) {
        buckets_[hole] = buckets_[scan];
        hole = scan;
      }
    }
    buckets_[hole].value = kInvalidIndex;
    buckets_[hole].key = 0;
    --size_;
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t bucket_count() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t max_entries() const noexcept {
    return max_entries_;
  }

  /// Total probes a lookup of every live key would cost. Used by the hash
  /// distribution test, which feeds it real dense sequential order reference
  /// numbers -- the exact input a bad hash clusters on.
  [[nodiscard]] std::size_t total_probe_distance() const noexcept {
    std::size_t total = 0;
    for (std::size_t i = 0; i < capacity_; ++i) {
      if (buckets_[i].value == kInvalidIndex) continue;
      const std::size_t h = home(buckets_[i].key);
      total += (i >= h) ? (i - h) : (capacity_ - h + i);
    }
    return total;
  }

  /// The longest probe run any single lookup would walk.
  [[nodiscard]] std::size_t max_probe_distance() const noexcept {
    std::size_t worst = 0;
    for (std::size_t i = 0; i < capacity_; ++i) {
      if (buckets_[i].value == kInvalidIndex) continue;
      const std::size_t h = home(buckets_[i].key);
      const std::size_t d = (i >= h) ? (i - h) : (capacity_ - h + i);
      if (d > worst) worst = d;
    }
    return worst;
  }

  [[nodiscard]] std::size_t home(OrderId key) const noexcept {
    return static_cast<std::size_t>((key * kMix) >> shift_);
  }

  /// Visit every live entry. Iteration order is bucket order, which is a
  /// function of the hash -- never rely on it for anything the engine emits,
  /// or determinism dies quietly.
  template <class Fn>
  void for_each(Fn&& fn) const {
    for (std::size_t i = 0; i < capacity_; ++i) {
      if (buckets_[i].value != kInvalidIndex) {
        fn(buckets_[i].key, buckets_[i].value);
      }
    }
  }

private:
  static std::size_t round_up_pow2(std::size_t n) noexcept {
    if (n < 2) return 2;
    return std::size_t{1} << (64 -
                              static_cast<unsigned>(std::countl_zero(n - 1)));
  }

  std::size_t capacity_;
  std::size_t mask_;
  unsigned shift_;
  std::size_t max_entries_;
  Bucket* buckets_;
  std::size_t size_{0};
};

} // namespace miniex
