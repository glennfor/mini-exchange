// Property tests over the invariants of spec section 8.8. These are the tests
// that run randomised command sequences; the directed tests elsewhere name
// individual rules.

#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "book/naive_book.hpp"
#include "engine/event_sink.hpp"
#include "engine/invariants.hpp"
#include "engine/matching_engine.hpp"

namespace miniex {
namespace {

using Engine = MatchingEngine<NaiveBook, VectorSink>;

/// Seeded, reproducible random command flow. Deliberately weighted towards
/// adds and cancels, because real equity flow is; a 50%-aggressive mix would
/// exercise the matching loop harder and the book's bookkeeping less, which
/// is the opposite of where bugs live.
std::vector<Command> generate(std::uint64_t seed, std::size_t count) {
  std::mt19937_64 rng(seed);
  std::vector<Command> out;
  out.reserve(count);
  std::vector<OrderId> live;
  OrderId next_id = 1;
  TsNanos ts = 0;

  for (std::size_t i = 0; i < count; ++i) {
    const int roll = static_cast<int>(rng() % 100);
    ++ts;
    if (roll < 45 || live.empty()) {
      NewOrder n{};
      n.id = next_id++;
      n.side = (rng() % 2) ? Side::Buy : Side::Sell;
      n.price = 1002000 + static_cast<Price>(rng() % 41);
      n.qty = static_cast<Qty>(1 + rng() % 500);
      n.participant = static_cast<ParticipantId>(rng() % 4);
      switch (rng() % 10) {
      case 0: n.tif = TimeInForce::IOC; break;
      case 1: n.tif = TimeInForce::FOK; break;
      case 2: n.tif = TimeInForce::PostOnly; break;
      default: n.tif = TimeInForce::Day; break;
      }
      if (n.tif != TimeInForce::Day && n.tif != TimeInForce::PostOnly &&
          rng() % 8 == 0) {
        n.type = OrderType::Market;
      }
      out.push_back(Command::make_new(ts, n));
      live.push_back(n.id);
    } else if (roll < 80) {
      const std::size_t k = rng() % live.size();
      out.push_back(Command::make_cancel(ts, live[k]));
      live[k] = live.back();
      live.pop_back();
    } else {
      const std::size_t k = rng() % live.size();
      out.push_back(Command::make_replace(
          ts, live[k], 1002000 + static_cast<Price>(rng() % 41),
          static_cast<Qty>(1 + rng() % 500)));
    }
  }
  return out;
}

TEST(Invariants, RandomFlowNeverViolatesThem) {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    VectorSink sink;
    Engine engine(sink);
    ShareLedger ledger;
    std::size_t applied = 0;

    for (const Command& c : generate(seed, 2000)) {
      const std::size_t before = sink.size();
      engine.apply(c);
      ++applied;
      for (std::size_t i = before; i < sink.size(); ++i)
        ledger.on_event(sink[i]);

      const auto book_ok = check_book(engine.book());
      ASSERT_TRUE(book_ok.ok())
          << "seed " << seed << " command " << applied << ": " << book_ok.what;
      const auto ledger_ok = ledger.check();
      ASSERT_TRUE(ledger_ok.ok()) << "seed " << seed << " command " << applied
                                  << ": " << ledger_ok.what;
    }
    EXPECT_GT(engine.stats().trades, 0U) << "seed " << seed << " never traded";
  }
}

TEST(Invariants, RandomFlowWithSelfTradePreventionOn) {
  EngineConfig cfg;
  cfg.stp = SelfTradePolicy::CancelResting;
  for (std::uint64_t seed = 100; seed <= 110; ++seed) {
    VectorSink sink;
    Engine engine(sink, cfg);
    ShareLedger ledger;
    for (const Command& c : generate(seed, 2000)) {
      const std::size_t before = sink.size();
      engine.apply(c);
      for (std::size_t i = before; i < sink.size(); ++i)
        ledger.on_event(sink[i]);
      ASSERT_TRUE(check_book(engine.book()).ok())
          << check_book(engine.book()).what;
      ASSERT_TRUE(ledger.check().ok()) << ledger.check().what;
    }
    EXPECT_GT(engine.stats().stp_cancels, 0U) << "seed " << seed;
  }
}

TEST(Invariants, CancellingEveryOrderLeavesAnEmptyBook) {
  VectorSink sink;
  Engine engine(sink);
  TsNanos ts = 0;
  std::vector<OrderId> ids;
  for (OrderId id = 1; id <= 500; ++id) {
    NewOrder n{};
    n.id = id;
    // Two non-overlapping ranges, so nothing ever trades and every order
    // must still be resting when we cancel it.
    n.side = (id % 2) ? Side::Buy : Side::Sell;
    n.price = n.side == Side::Buy ? 1002000 - static_cast<Price>(id % 50)
                                  : 1003000 + static_cast<Price>(id % 50);
    n.qty = 100;
    engine.on_new(n, ++ts);
    ids.push_back(id);
  }
  ASSERT_EQ(engine.book().order_count(), 500U);
  for (OrderId id : ids) engine.on_cancel(CancelOrder{id}, ++ts);

  EXPECT_EQ(engine.book().order_count(), 0U);
  EXPECT_EQ(engine.book().best(Side::Buy), kInvalidPrice);
  EXPECT_EQ(engine.book().best(Side::Sell), kInvalidPrice);
  EXPECT_EQ(engine.book().level_count(Side::Buy), 0U);
  EXPECT_EQ(engine.book().level_count(Side::Sell), 0U);
  EXPECT_TRUE(check_book(engine.book()).ok());
  EXPECT_EQ(sink.count(EventType::Reject), 0U);
}

TEST(Invariants, FifoWithIdenticalOrdersFillsTheEarliestEntered) {
  VectorSink sink;
  Engine engine(sink);
  TsNanos ts = 0;
  constexpr int kK = 25;
  for (OrderId id = 1; id <= kK; ++id) {
    NewOrder n{};
    n.id = id;
    n.side = Side::Sell;
    n.price = 1002200;
    n.qty = 100;
    engine.on_new(n, ++ts);
  }
  NewOrder agg{};
  agg.id = 1000;
  agg.side = Side::Buy;
  agg.price = 1002200;
  agg.qty = 100;
  engine.on_new(agg, ++ts);

  const auto trades = sink.of_type(EventType::Trade);
  ASSERT_EQ(trades.size(), 1U);
  EXPECT_EQ(trades[0].counter_id, 1U);
  EXPECT_EQ(engine.book().orders_at(Side::Sell, 1002200),
            static_cast<std::size_t>(kK - 1));
}

TEST(Invariants, TotalBuyAndSellTradedQuantitiesBalance) {
  VectorSink sink;
  Engine engine(sink);
  for (const Command& c : generate(7, 20000)) engine.apply(c);

  ShareLedger ledger;
  for (const Event& e : sink.events) ledger.on_event(e);
  const auto ok = ledger.check();
  ASSERT_TRUE(ok.ok()) << ok.what;
  EXPECT_EQ(ledger.traded_shares(), engine.stats().shares_traded);
}

TEST(Invariants, FillableQtyAgreesWithWhatMatchingActuallyFills) {
  for (std::uint64_t seed = 200; seed <= 215; ++seed) {
    VectorSink sink;
    Engine engine(sink);
    for (const Command& c : generate(seed, 800)) engine.apply(c);

    // Ask the non-mutating pre-scan, then actually send the order and see.
    const Price ask = engine.book().best(Side::Sell);
    if (ask == kInvalidPrice) continue;
    const Price limit = ask + 20;
    const Qty want = 100000;
    const AggQty predicted = engine.fillable_qty(Side::Buy, limit, want, 0);

    NewOrder n{};
    n.id = 900000;
    n.side = Side::Buy;
    n.price = limit;
    n.qty = static_cast<Qty>(predicted == 0 ? 1 : predicted);
    n.tif = TimeInForce::IOC;
    const std::size_t before = sink.size();
    engine.on_new(n, 1'000'000);

    AggQty actually_filled = 0;
    for (std::size_t i = before; i < sink.size(); ++i)
      if (sink[i].type == EventType::Trade) actually_filled += sink[i].qty;

    if (predicted > 0) {
      EXPECT_EQ(actually_filled, predicted)
          << "seed " << seed
          << ": the FOK pre-scan and the matching loop "
             "must agree exactly, or FOK is a lie";
    }
  }
}

} // namespace
} // namespace miniex
