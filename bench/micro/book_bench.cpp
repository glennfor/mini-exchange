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

void BM_AddToEmptyLevel(benchmark::State& state) {
  NaiveBook book;
  OrderId id = 1;
  Price price = 1000000;
  for (auto _ : state) {
    state.PauseTiming();
    if (book.order_count() > 100000) {
      book.clear();
      price = 1000000;
    }
    state.ResumeTiming();
    bool added = book.add(id++, Side::Buy, price++, 100, 0, 0);
    benchmark::DoNotOptimize(added);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_AddToDeepLevel(benchmark::State& state) {
  NaiveBook book;
  populate(book, 10000, 1);
  OrderId id = 1000000;
  for (auto _ : state) {
    bool added = book.add(id++, Side::Buy, 1001999, 100, 0, 0);
    benchmark::DoNotOptimize(added);
  }
  state.SetItemsProcessed(state.iterations());
}

// --- cancel -----------------------------------------------------------------
//
// Cancel gets three separate benchmarks because where the order sits in the
// level's FIFO is the whole question: an implementation with an intrusive
// doubly-linked list is O(1) wherever it is, and one that searches the level
// is O(position). A single "cancel" number would hide exactly that.

void cancel_at_position(benchmark::State& state, double position_fraction) {
  constexpr std::size_t kOrders = 4096;
  for (auto _ : state) {
    state.PauseTiming();
    NaiveBook book;
    populate(book, kOrders, 1);
    const auto target = static_cast<OrderId>(
        1 + static_cast<std::size_t>(position_fraction *
                                     static_cast<double>(kOrders - 1)));
    state.ResumeTiming();
    bool cancelled = book.cancel(target);
    benchmark::DoNotOptimize(cancelled);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_CancelHead(benchmark::State& state) {
  cancel_at_position(state, 0.0);
}
void BM_CancelMiddle(benchmark::State& state) {
  cancel_at_position(state, 0.5);
}
void BM_CancelTail(benchmark::State& state) {
  cancel_at_position(state, 1.0);
}

// --- matching ---------------------------------------------------------------

void match_levels(benchmark::State& state, std::size_t levels) {
  const auto per_level = static_cast<Qty>(100);
  for (auto _ : state) {
    state.PauseTiming();
    VectorSink sink;
    MatchingEngine<NaiveBook, VectorSink> engine(sink);
    for (std::size_t i = 0; i < levels; ++i) {
      NewOrder n{};
      n.id = static_cast<OrderId>(i + 1);
      n.side = Side::Sell;
      n.price = 1002000 + static_cast<Price>(i);
      n.qty = per_level;
      engine.on_new(n, static_cast<TsNanos>(i));
    }
    NewOrder aggressor{};
    aggressor.id = 999999;
    aggressor.side = Side::Buy;
    aggressor.price = 1002000 + static_cast<Price>(levels);
    aggressor.qty = static_cast<Qty>(per_level * levels);
    aggressor.tif = TimeInForce::IOC;
    state.ResumeTiming();
    engine.on_new(aggressor, 1);
    std::uint64_t trades = engine.stats().trades;
    benchmark::DoNotOptimize(trades);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(levels));
}

void BM_MatchOneLevel(benchmark::State& state) {
  match_levels(state, 1);
}
void BM_MatchTenLevels(benchmark::State& state) {
  match_levels(state, 10);
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
      state.PauseTiming();
      engine.on_replace(ReplaceOrder{1, 1002000, 1000000}, 0);
      qty = 999999;
      state.ResumeTiming();
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
// find the next occupied level. With a wide, sparse book this is where a
// scanning implementation falls over.

void best_after_touch_empties(benchmark::State& state, Price spacing) {
  NaiveBook book;
  for (std::size_t i = 0; i < 64; ++i) {
    (void)book.add(static_cast<OrderId>(i + 1), Side::Buy,
                   1002000 - static_cast<Price>(i) * spacing, 100,
                   static_cast<TsNanos>(i), 0);
  }
  OrderId next = 1;
  for (auto _ : state) {
    state.PauseTiming();
    if (book.order_count() == 0) {
      for (std::size_t i = 0; i < 64; ++i)
        (void)book.add(static_cast<OrderId>(next++), Side::Buy,
                       1002000 - static_cast<Price>(i) * spacing, 100, 0, 0);
    }
    const Price touch = book.best(Side::Buy);
    state.ResumeTiming();
    (void)book.cancel(book.front(Side::Buy)->id);
    Price best = book.best(Side::Buy);
    benchmark::DoNotOptimize(best);
    benchmark::ClobberMemory();
    (void)touch;
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_BestAfterTouchEmpties_Dense(benchmark::State& state) {
  best_after_touch_empties(state, 1);
}
void BM_BestAfterTouchEmpties_Sparse(benchmark::State& state) {
  best_after_touch_empties(state, 1000);
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

/// The operation the hierarchical bitset exists for, on the structure that
/// actually uses it.
void BM_FastBookBestAfterTouchEmpties(benchmark::State& state) {
  FastBook book(bench_ladder());
  OrderId next = 1;
  for (auto _ : state) {
    state.PauseTiming();
    if (book.order_count() == 0) {
      for (std::size_t i = 0; i < 64; ++i)
        (void)book.add(next++, Side::Buy,
                       1002000 - static_cast<Price>(i) * 1000, 100, 0, 0);
    }
    state.ResumeTiming();
    (void)book.cancel(book.front(Side::Buy)->id);
    Price best = book.best(Side::Buy);
    benchmark::DoNotOptimize(best);
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_AddToEmptyLevel);
BENCHMARK(BM_AddToDeepLevel);
BENCHMARK(BM_CancelHead);
BENCHMARK(BM_CancelMiddle);
BENCHMARK(BM_CancelTail);
BENCHMARK(BM_MatchOneLevel);
BENCHMARK(BM_MatchTenLevels);
BENCHMARK(BM_ReplaceSamePrice);
BENCHMARK(BM_ReplaceCrossPrice);
BENCHMARK(BM_BestAfterTouchEmpties_Dense);
BENCHMARK(BM_BestAfterTouchEmpties_Sparse);
BENCHMARK(BM_FastBookBestAfterTouchEmpties);
BENCHMARK(BM_MixedFlow_Naive)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MixedFlow_Fast)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MixedFlow_Fast_LadderResident)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MixedFlow_Fast_LadderOverflow)->Unit(benchmark::kMillisecond);

} // namespace
} // namespace miniex
