/// \file core_bench.cpp
/// \brief Per-operation costs for the core primitives.
///
/// These exist to justify the design decisions in `docs/design-decisions.md`
/// with numbers rather than assertions: the cached-counter SPSC ring
/// (DD-007), backward-shift deletion (DD-008), and the hierarchical bitset
/// against a linear scan (the reason `bitset_ladder.hpp` exists at all).
///
/// ## A note on the harness, which cost real time to learn
///
/// None of these benchmarks use `state.PauseTiming()` / `ResumeTiming()`.
///
/// The obvious way to benchmark an operation that consumes its input -- a
/// cancel, an add that creates a level -- is to rebuild the structure inside
/// a paused region. That is wrong twice over. Pause/resume costs microseconds
/// against operations that cost nanoseconds, so a naive version of the cancel
/// benchmark reported **98 us per cancel** when the real figure is 28 ns. And
/// because Google Benchmark excludes paused time from the measurement while
/// still spending it, it keeps raising the iteration count to reach its
/// minimum *measured* time -- one benchmark here ran for over two minutes
/// without finishing.
///
/// Instead the structure is rebuilt only when it is exhausted, in the timed
/// region, so the rebuild is amortised over thousands of operations and
/// contributes a fraction of a percent to each sample. That is a small,
/// honest bias in a known direction, which is a far better trade than a large
/// dishonest one.

#include <cstdint>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

#include "core/bitset_ladder.hpp"
#include "core/config.hpp"
#include "core/object_pool.hpp"
#include "core/open_hash_map.hpp"
#include "core/spsc_ring.hpp"

#include "replay/random.hpp"

namespace miniex {
namespace {

// --- object pool ------------------------------------------------------------

struct Node {
  std::uint32_t next;
  std::uint32_t payload;
};

void BM_PoolAllocFree(benchmark::State& state) {
  ObjectPool<Node> pool(1 << 16);
  for (auto _ : state) {
    std::uint32_t h = pool.allocate();
    benchmark::DoNotOptimize(h);
    pool.deallocate(h);
  }
  state.SetItemsProcessed(state.iterations());
}

// --- open hash map ----------------------------------------------------------
//
// The keys are dense and sequential on purpose: that is what ITCH order
// reference numbers look like, and it is the input an identity hash
// clusters on.

void BM_HashInsert(benchmark::State& state) {
  constexpr std::size_t kEntries = 1 << 16;
  OpenHashMap map(kEntries);
  OrderId id = 4'294'967'296ULL;
  for (auto _ : state) {
    if (map.size() >= kEntries) {
      map.clear();
    }
    bool inserted = map.insert(id++, 1);
    benchmark::DoNotOptimize(inserted);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_HashFindHit(benchmark::State& state) {
  constexpr std::size_t kEntries = 1 << 16;
  OpenHashMap map(kEntries);
  const OrderId base = 4'294'967'296ULL;
  for (std::size_t i = 0; i < kEntries; ++i)
    (void)map.insert(base + i, static_cast<std::uint32_t>(i));
  Rng rng(1);
  for (auto _ : state) {
    std::uint32_t found = map.find(base + rng.below(kEntries));
    benchmark::DoNotOptimize(found);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_HashFindMiss(benchmark::State& state) {
  constexpr std::size_t kEntries = 1 << 16;
  OpenHashMap map(kEntries);
  const OrderId base = 4'294'967'296ULL;
  for (std::size_t i = 0; i < kEntries; ++i)
    (void)map.insert(base + i, static_cast<std::uint32_t>(i));
  OrderId probe = 1;
  for (auto _ : state) {
    std::uint32_t found = map.find(probe++);
    benchmark::DoNotOptimize(found);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_HashErase(benchmark::State& state) {
  constexpr std::size_t kEntries = 1 << 14;
  const OrderId base = 4'294'967'296ULL;
  OpenHashMap map(kEntries);
  std::size_t next = 0;
  for (auto _ : state) {
    if (map.empty()) {
      for (std::size_t i = 0; i < kEntries; ++i)
        (void)map.insert(base + i, static_cast<std::uint32_t>(i));
      next = 0;
    }
    bool erased = map.erase(base + next++);
    benchmark::DoNotOptimize(erased);
  }
  state.SetItemsProcessed(state.iterations());
}

// --- bitset ladder ----------------------------------------------------------
//
// The comparison that justifies the structure: hierarchical lookup against a
// linear scan of the same bits, on a sparse ladder where the answer is far
// away. If these two are close, bitset_ladder.hpp should be deleted.

template <std::size_t N>
struct SparseLadder {
  BitsetLadder<N> bits;
  std::vector<bool> flat = std::vector<bool>(N, false);
  SparseLadder() {
    for (std::size_t i = 0; i < N; i += N / 8) {
      bits.set(i);
      flat[i] = true;
    }
  }
};

void BM_LadderNextSet_Hierarchical(benchmark::State& state) {
  static SparseLadder<kLadderTicks> ladder;
  Rng rng(1);
  for (auto _ : state) {
    const std::size_t from = rng.below(kLadderTicks);
    std::size_t found = ladder.bits.find_next_set_at_or_after(from);
    benchmark::DoNotOptimize(found);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_LadderNextSet_LinearScan(benchmark::State& state) {
  static SparseLadder<kLadderTicks> ladder;
  Rng rng(1);
  for (auto _ : state) {
    const std::size_t from = rng.below(kLadderTicks);
    std::size_t found = kLadderTicks;
    for (std::size_t i = from; i < kLadderTicks; ++i) {
      if (ladder.flat[i]) {
        found = i;
        break;
      }
    }
    benchmark::DoNotOptimize(found);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_LadderSetClear(benchmark::State& state) {
  static BitsetLadder<kLadderTicks> bits;
  Rng rng(2);
  for (auto _ : state) {
    const std::size_t i = rng.below(kLadderTicks);
    bits.set(i);
    bits.clear(i);
  }
  state.SetItemsProcessed(state.iterations());
}

// --- SPSC ring --------------------------------------------------------------

void BM_RingPushPopSingleThreaded(benchmark::State& state) {
  auto ring = std::make_unique<SpscRing<std::uint64_t, 1024>>();
  std::uint64_t value = 0;
  for (auto _ : state) {
    bool pushed = ring->try_push(value);
    benchmark::DoNotOptimize(pushed);
    bool popped = ring->try_pop(value);
    benchmark::DoNotOptimize(popped);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_RingBatch64(benchmark::State& state) {
  auto ring = std::make_unique<SpscRing<std::uint64_t, 1024>>();
  std::uint64_t batch[64] = {};
  for (auto _ : state) {
    std::size_t pushed = ring->try_push_n(batch, 64);
    benchmark::DoNotOptimize(pushed);
    std::size_t popped = ring->try_pop_n(batch, 64);
    benchmark::DoNotOptimize(popped);
  }
  state.SetItemsProcessed(state.iterations() * 64);
}

BENCHMARK(BM_PoolAllocFree);
BENCHMARK(BM_HashInsert);
BENCHMARK(BM_HashFindHit);
BENCHMARK(BM_HashFindMiss);
BENCHMARK(BM_HashErase);
BENCHMARK(BM_LadderNextSet_Hierarchical);
BENCHMARK(BM_LadderNextSet_LinearScan);
BENCHMARK(BM_LadderSetClear);
BENCHMARK(BM_RingPushPopSingleThreaded);
BENCHMARK(BM_RingBatch64);

} // namespace
} // namespace miniex
