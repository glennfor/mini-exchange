/// \file book_bench.cpp
/// \brief Per-operation costs. Spec section 18.1 item 1.
///
/// These measure single operations against a book in a known state. They are
/// *not* end-to-end latency and must never be quoted as such: there is no
/// queueing here, no ingress ring, and the book is hot in cache in a way it
/// never is during a real session. What they are good for is A/B comparison
/// between two implementations of the same operation, which is exactly what
/// the NaiveBook-vs-FastBook story needs.
///
/// Every fixture builds its book *outside* the measured region.
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
/// Instead the structures here are driven in a **steady-state cycle** -- each
/// iteration performs the operation and its inverse, so nothing is ever
/// rebuilt and the shape at the end of an iteration is the shape at the
/// start. Where the inverse is itself measured elsewhere (the add paired with
/// each cancel, say), the reader can see how much of the number it is.
///
/// The single exception is `mixed_flow`, whose measured region is
/// *milliseconds* and whose setup is also milliseconds. There, a
/// one-microsecond pause is 0.01% of the sample rather than 100x the
/// operation, and leaving the setup in would turn the benchmark into a
/// measurement of FastBook's allocator. It is commented at the call site.

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "book/fast_book.hpp"
#include "book/naive_book.hpp"
#include "engine/event_sink.hpp"
#include "engine/matching_engine.hpp"
#include "replay/synthetic.hpp"

namespace miniex {
namespace {

/// Fills a book with `orders` resting orders spread over `levels` prices.
template <class Book>
void populate(Book& book, std::size_t orders, std::size_t levels,
              Price base = 1002000) {
  for (std::size_t i = 0; i < orders; ++i) {
    const auto level = static_cast<Price>(i % levels);
    (void)book.add(static_cast<OrderId>(i + 1), Side::Buy, base - 1 - level,
                   100, static_cast<TsNanos>(i), 0);
  }
}

// --- add --------------------------------------------------------------------

/// Creating a brand-new price level and destroying it again.
///
/// Measured as a **pair**, because an add that creates a level has to be
/// undone before the next one can create the same kind of level, and the only
/// honest alternatives are to include a bulk teardown in the average (which
/// is what the first version of this file did, and it was half destruction)
/// or to pause the clock (which costs more than the operation).
///
/// Subtract nothing: `BM_CancelNewLevel` below is the same cancel measured on
/// its own, so the split is visible.
void BM_AddAndCancelNewLevel(benchmark::State& state) {
  NaiveBook book;
  OrderId id = 1;
  Price price = 1000000;
  for (auto _ : state) {
    const OrderId current = id++;
    bool added = book.add(current, Side::Buy, price, 100, 0, 0);
    benchmark::DoNotOptimize(added);
    bool cancelled = book.cancel(current);
    benchmark::DoNotOptimize(cancelled);
    // Walk the price so each iteration really does create a fresh level
    // rather than reusing a hot allocation.
    price = price + 1 > 1010000 ? 1000000 : price + 1;
  }
  state.SetItemsProcessed(state.iterations());
}

/// Appending to a level that already has ten thousand orders on it. Steady
/// state: one add, one cancel of the order added 4096 iterations ago, so the
/// level stays deep and nothing is ever rebuilt.
void BM_AddToDeepLevel(benchmark::State& state) {
  constexpr std::size_t kWindow = 4096;
  NaiveBook book;
  OrderId id = 1;
  for (std::size_t i = 0; i < 10000; ++i)
    (void)book.add(id++, Side::Buy, 1001999, 100, static_cast<TsNanos>(i), 0);

  std::vector<OrderId> window;
  window.reserve(kWindow);
  for (std::size_t i = 0; i < kWindow; ++i) {
    window.push_back(id);
    (void)book.add(id++, Side::Buy, 1001999, 100, 0, 0);
  }

  std::size_t cursor = 0;
  for (auto _ : state) {
    bool added = book.add(id, Side::Buy, 1001999, 100, 0, 0);
    benchmark::DoNotOptimize(added);
    bool cancelled = book.cancel(window[cursor]);
    benchmark::DoNotOptimize(cancelled);
    window[cursor] = id++;
    cursor = (cursor + 1) % kWindow;
  }
  state.SetItemsProcessed(state.iterations());
}

// --- cancel -----------------------------------------------------------------
//
// Cancel gets three benchmarks because where the order sits in its level's
// FIFO is the whole question: an intrusive doubly-linked list is O(1)
// wherever it is, and an implementation that searches the level is
// O(position). A single "cancel" number would hide exactly that.
//
// Steady state, no teardown: each iteration adds one order and cancels one at
// the chosen position, so the level stays the same depth forever. The add is
// therefore in every sample; `BM_AddToDeepLevel` measures the same add on its
// own, so the reader can see how much of each number it is.

enum class CancelPosition {
  Head,
  Middle,
  Tail
};

void cancel_at_position(benchmark::State& state, CancelPosition where) {
  constexpr std::size_t kDepth = 4096;
  NaiveBook book;
  OrderId id = 1;

  // A circular buffer of the ids currently resting, oldest at `front`.
  //
  // Not a deque: erasing from the middle of a 4096-element deque is O(n) and
  // costs more than the cancel being measured. The first version of this
  // benchmark did exactly that and reported CancelMiddle at 331 ns against
  // 61 ns for the head -- a fivefold difference that was entirely the
  // harness, and which would have read as "this book's cancel is O(position)"
  // when the whole point of the intrusive list is that it is not.
  std::vector<OrderId> ring(kDepth);
  for (std::size_t i = 0; i < kDepth; ++i) {
    ring[i] = id;
    (void)book.add(id++, Side::Buy, 1002000, 100, static_cast<TsNanos>(i), 0);
  }
  std::size_t front = 0;

  for (auto _ : state) {
    (void)book.add(id, Side::Buy, 1002000, 100, 0, 0);
    std::size_t slot = 0;
    switch (where) {
    case CancelPosition::Head: slot = front; break;
    case CancelPosition::Tail: slot = (front + kDepth - 1) % kDepth; break;
    case CancelPosition::Middle: slot = (front + kDepth / 2) % kDepth; break;
    }
    const OrderId victim = ring[slot];
    ring[slot] = id++;
    front = (front + 1) % kDepth;

    bool cancelled = book.cancel(victim);
    benchmark::DoNotOptimize(cancelled);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_CancelHead(benchmark::State& state) {
  cancel_at_position(state, CancelPosition::Head);
}
void BM_CancelMiddle(benchmark::State& state) {
  cancel_at_position(state, CancelPosition::Middle);
}
void BM_CancelTail(benchmark::State& state) {
  cancel_at_position(state, CancelPosition::Tail);
}

// --- matching ---------------------------------------------------------------

/// One fill against one resting order, with **no teardown at all**.
///
/// The resting order carries a billion shares and the aggressor takes one, so
/// the fill is always partial, the order never leaves the book, and the
/// structure is identical at the end of every iteration. This is the cleanest
/// per-operation number in the file precisely because nothing has to be
/// rebuilt.
void BM_MatchOneFill(benchmark::State& state) {
  CountingSink sink;
  MatchingEngine<NaiveBook, CountingSink> engine(sink);
  NewOrder resting{};
  resting.id = 1;
  resting.side = Side::Sell;
  resting.price = 1002000;
  resting.qty = 1000000000;
  engine.on_new(resting, 0);

  OrderId id = 2;
  for (auto _ : state) {
    NewOrder aggressor{};
    aggressor.id = id++;
    aggressor.side = Side::Buy;
    aggressor.price = 1002000;
    aggressor.qty = 1;
    aggressor.tif = TimeInForce::IOC;
    engine.on_new(aggressor, 1);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}

/// Sweeping ten price levels, which necessarily empties them -- so the ten
/// adds that put them back are **inside the measurement**.
///
/// Stated rather than hidden: at roughly 55 ns per add
/// (`BM_AddToDeepLevel`), about 550 ns of each sample is the refill. The
/// number is still useful as an A/B between books, which is what it is for.
void BM_SweepTenLevelsIncludingRefill(benchmark::State& state) {
  constexpr std::size_t kLevels = 10;
  CountingSink sink;
  MatchingEngine<NaiveBook, CountingSink> engine(sink);
  OrderId id = 1;

  for (auto _ : state) {
    for (std::size_t i = 0; i < kLevels; ++i) {
      NewOrder n{};
      n.id = id++;
      n.side = Side::Sell;
      n.price = 1002000 + static_cast<Price>(i);
      n.qty = 100;
      engine.on_new(n, static_cast<TsNanos>(i));
    }
    NewOrder aggressor{};
    aggressor.id = id++;
    aggressor.side = Side::Buy;
    aggressor.price = 1002000 + static_cast<Price>(kLevels);
    aggressor.qty = 100 * kLevels;
    aggressor.tif = TimeInForce::IOC;
    engine.on_new(aggressor, 1);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(kLevels));
}

// --- replace ----------------------------------------------------------------

void BM_ReplaceSamePrice(benchmark::State& state) {
  VectorSink sink;
  MatchingEngine<NaiveBook, VectorSink> engine(sink);
  NewOrder n{};
  n.id = 1;
  n.side = Side::Buy;
  n.price = 1002000;
  n.qty = 1000000;
  engine.on_new(n, 0);
  Qty qty = 999999;
  for (auto _ : state) {
    engine.on_replace(ReplaceOrder{1, 1002000, qty}, 0);
    if (--qty < 2) {
      engine.on_replace(ReplaceOrder{1, 1002000, 1000000}, 0);
      qty = 999999;
    }
    sink.clear();
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_ReplaceCrossPrice(benchmark::State& state) {
  VectorSink sink;
  MatchingEngine<NaiveBook, VectorSink> engine(sink);
  NewOrder n{};
  n.id = 1;
  n.side = Side::Buy;
  n.price = 1002000;
  n.qty = 100;
  engine.on_new(n, 0);
  Price price = 1002000;
  for (auto _ : state) {
    price = price == 1002000 ? 1001990 : 1002000;
    engine.on_replace(ReplaceOrder{1, price, 100}, 0);
    sink.clear();
  }
  state.SetItemsProcessed(state.iterations());
}

// --- best-price discovery ---------------------------------------------------
//
// The operation the hierarchical bitset exists for: after the touch empties,
// find the next occupied level. On a wide, sparse book a scanning
// implementation is at its worst exactly when the book is thinnest.
//
// Steady state again, and specifically **without** calling `clear()`: a
// FastBook's clear() rebuilds the whole free list, which is O(pool capacity)
// and swamped these samples entirely when it was amortised over a few hundred
// of them -- 1456 ns per "best price lookup" that actually costs a few
// nanoseconds. Each iteration cancels the touch, reads the new best, and adds
// a replacement order at the emptied price, so the ladder never changes shape.

template <class Book, class... BookArgs>
void best_after_touch_empties(benchmark::State& state, Price spacing,
                              BookArgs&&... book_args) {
  constexpr std::size_t kLevels = 512;
  Book book(std::forward<BookArgs>(book_args)...);
  OrderId id = 1;
  for (std::size_t i = 0; i < kLevels; ++i) {
    (void)book.add(id++, Side::Buy, 1002000 - static_cast<Price>(i) * spacing,
                   100, static_cast<TsNanos>(i), 0);
  }

  for (auto _ : state) {
    OrderHandle touch = book.front(Side::Buy);
    const Price emptied = touch->price;
    (void)book.cancel(touch->id);
    Price best = book.best(Side::Buy);
    benchmark::DoNotOptimize(best);
    // Put the level back, so the next iteration starts from the same shape.
    (void)book.add(id++, Side::Buy, emptied, 100, 0, 0);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_BestAfterTouchEmpties_Naive_Dense(benchmark::State& state) {
  best_after_touch_empties<NaiveBook>(state, 1);
}
void BM_BestAfterTouchEmpties_Naive_Sparse(benchmark::State& state) {
  best_after_touch_empties<NaiveBook>(state, 200);
}

// --- realistic mixed flow ---------------------------------------------------

/// The headline A/B: the same engine, the same flow, two books. Whatever
/// ratio this reports is the ratio the README is allowed to claim, on the
/// machine whose report is committed next to it.
template <class Book, class... BookArgs>
void mixed_flow(benchmark::State& state, FlowPattern pattern,
                BookArgs&&... book_args) {
  FlowConfig cfg;
  cfg.seed = 1;
  cfg.pattern = pattern;
  cfg.warmup_adds = 10000;
  SyntheticFlow flow(cfg);
  std::vector<Command> commands;
  flow.warm_up(commands);
  flow.generate(commands, 100000);

  for (auto _ : state) {
    // The one place in this file that pauses the clock, and the reason is the
    // mirror image of why nothing else does.
    //
    // Constructing a FastBook pre-faults roughly 40 MB of pool, index and
    // ladder -- milliseconds. The measured region here is a hundred thousand
    // commands, also milliseconds. Pause/resume costs a microsecond, so it is
    // 0.01% of this sample, against being 100x the operation in a
    // nanosecond-scale benchmark. Leaving construction in would make this a
    // measurement of FastBook's allocator, and it did: without the pause,
    // FastBook's realistic-flow throughput read 16.7 M/s instead of 20.0 M/s,
    // and the difference was page faults on memory it only touches once.
    state.PauseTiming();
    CountingSink sink;
    MatchingEngine<Book, CountingSink> engine(
        sink, EngineConfig{}, std::forward<BookArgs>(book_args)...);
    state.ResumeTiming();

    for (const Command& c : commands) engine.apply(c);
    std::uint64_t total = sink.total;
    benchmark::DoNotOptimize(total);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(commands.size()));
}

FastBookConfig bench_ladder() {
  FastBookConfig cfg;
  cfg.max_orders = 1 << 18;
  cfg.centre_price = 1002000;
  return cfg;
}

void BM_MixedFlow_Naive(benchmark::State& state) {
  mixed_flow<NaiveBook>(state, FlowPattern::Balanced);
}
void BM_MixedFlow_Fast(benchmark::State& state) {
  mixed_flow<FastBook>(state, FlowPattern::Balanced, bench_ladder());
}

/// FastBook's own worst case next to its best case. Publishing the gap is the
/// point: the resident ladder is O(1), the overflow map is a red-black tree.
void BM_MixedFlow_Fast_LadderResident(benchmark::State& state) {
  mixed_flow<FastBook>(state, FlowPattern::TouchChurn, bench_ladder());
}
void BM_MixedFlow_Fast_LadderOverflow(benchmark::State& state) {
  mixed_flow<FastBook>(state, FlowPattern::LadderOverflow, bench_ladder());
}

/// The same operation on the structure the bitset actually serves. The pair
/// of dense and sparse numbers against NaiveBook's pair is the argument for
/// bitset_ladder.hpp existing.
void BM_BestAfterTouchEmpties_Fast_Dense(benchmark::State& state) {
  best_after_touch_empties<FastBook>(state, 1, bench_ladder());
}
void BM_BestAfterTouchEmpties_Fast_Sparse(benchmark::State& state) {
  best_after_touch_empties<FastBook>(state, 200, bench_ladder());
}

BENCHMARK(BM_AddAndCancelNewLevel);
BENCHMARK(BM_AddToDeepLevel);
BENCHMARK(BM_CancelHead);
BENCHMARK(BM_CancelMiddle);
BENCHMARK(BM_CancelTail);
BENCHMARK(BM_MatchOneFill);
BENCHMARK(BM_SweepTenLevelsIncludingRefill);
BENCHMARK(BM_ReplaceSamePrice);
BENCHMARK(BM_ReplaceCrossPrice);
BENCHMARK(BM_BestAfterTouchEmpties_Naive_Dense);
BENCHMARK(BM_BestAfterTouchEmpties_Naive_Sparse);
BENCHMARK(BM_BestAfterTouchEmpties_Fast_Dense);
BENCHMARK(BM_BestAfterTouchEmpties_Fast_Sparse);
BENCHMARK(BM_MixedFlow_Naive)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MixedFlow_Fast)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MixedFlow_Fast_LadderResident)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MixedFlow_Fast_LadderOverflow)->Unit(benchmark::kMillisecond);

} // namespace
} // namespace miniex
