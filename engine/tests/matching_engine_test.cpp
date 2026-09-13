// Directed tests for spec section 8: price-time priority, trade price, and
// every row of the order type / time-in-force table. One test per rule, named
// for the rule, so a failure names the semantic that broke.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "book/naive_book.hpp"
#include "engine/event_format.hpp"
#include "engine/event_sink.hpp"
#include "engine/invariants.hpp"
#include "engine/matching_engine.hpp"
#include "engine/messages.hpp"

namespace miniex {
namespace {

using Engine = MatchingEngine<NaiveBook, VectorSink>;

/// Test fixture: an engine, a recording sink, and helpers that keep the test
/// bodies about semantics rather than struct filling.
class EngineTest : public ::testing::Test {
protected:
  explicit EngineTest(EngineConfig cfg = {}) : engine_(sink_, cfg) {}

  void send(OrderId id, Side side, Price price, Qty qty,
            TimeInForce tif = TimeInForce::Day,
            OrderType type = OrderType::Limit, ParticipantId participant = 0) {
    NewOrder n{};
    n.id = id;
    n.price = price;
    n.qty = qty;
    n.side = side;
    n.tif = tif;
    n.type = type;
    n.participant = participant;
    engine_.on_new(n, ++clock_);
  }
  void cancel(OrderId id) { engine_.on_cancel(CancelOrder{id}, ++clock_); }
  void replace(OrderId id, Price price, Qty qty) {
    engine_.on_replace(ReplaceOrder{id, price, qty}, ++clock_);
  }

  [[nodiscard]] std::vector<Event> trades() const {
    return sink_.of_type(EventType::Trade);
  }
  [[nodiscard]] const Event& last() const { return sink_.back(); }

  void expect_invariants() {
    const auto book_ok = check_book(engine_.book());
    EXPECT_TRUE(book_ok.ok()) << book_ok.what;
    ShareLedger ledger;
    for (const Event& e : sink_.events) ledger.on_event(e);
    const auto ledger_ok = ledger.check();
    EXPECT_TRUE(ledger_ok.ok()) << ledger_ok.what;
  }

  VectorSink sink_;
  Engine engine_;
  TsNanos clock_{0};
};

// ---------------------------------------------------------------------------
// 8.1 Price-time priority
// ---------------------------------------------------------------------------

TEST_F(EngineTest, RestingOrderSitsOnTheBook) {
  send(1, Side::Buy, 1002100, 100);
  EXPECT_EQ(engine_.book().best(Side::Buy), 1002100);
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002100), 100U);
  EXPECT_EQ(sink_.count(EventType::Ack), 1U);
  EXPECT_EQ(sink_.count(EventType::Trade), 0U);
  expect_invariants();
}

TEST_F(EngineTest, BetterPriceExecutesFirst) {
  send(1, Side::Sell, 1002400, 100);
  send(2, Side::Sell, 1002200, 100); // better ask
  send(3, Side::Sell, 1002300, 100);
  send(4, Side::Buy, 1002400, 300); // sweeps all three

  const auto t = trades();
  ASSERT_EQ(t.size(), 3U);
  EXPECT_EQ(t[0].price, 1002200);
  EXPECT_EQ(t[1].price, 1002300);
  EXPECT_EQ(t[2].price, 1002400);
  expect_invariants();
}

TEST_F(EngineTest, WithinAPriceLevelStrictFifoByArrival) {
  send(1, Side::Sell, 1002200, 100);
  send(2, Side::Sell, 1002200, 100);
  send(3, Side::Sell, 1002200, 100);
  send(4, Side::Buy, 1002200, 100);

  const auto t = trades();
  ASSERT_EQ(t.size(), 1U);
  EXPECT_EQ(t[0].counter_id, 1U)
      << "the earliest-entered order must fill first";
  EXPECT_EQ(engine_.book().qty_at(Side::Sell, 1002200), 200U);
  expect_invariants();
}

TEST_F(EngineTest, AggressorWalksLevelsUntilItsLimitIsExceeded) {
  send(1, Side::Sell, 1002200, 100);
  send(2, Side::Sell, 1002300, 100);
  send(3, Side::Sell, 1002400, 100);
  send(4, Side::Buy, 1002300, 300); // limit stops it at 1002300

  const auto t = trades();
  ASSERT_EQ(t.size(), 2U);
  EXPECT_EQ(t[0].price, 1002200);
  EXPECT_EQ(t[1].price, 1002300);
  // The residual rests as the new best bid.
  EXPECT_EQ(engine_.book().best(Side::Buy), 1002300);
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002300), 100U);
  EXPECT_EQ(engine_.book().best(Side::Sell), 1002400);
  expect_invariants();
}

// ---------------------------------------------------------------------------
// 8.2 Trade price -- the rule everyone gets wrong
// ---------------------------------------------------------------------------

TEST_F(EngineTest, TradeExecutesAtTheRestingOrdersPriceNotTheIncomingOrders) {
  send(1, Side::Sell, 1000000, 100); // resting ask at 100.0000
  send(2, Side::Buy, 1010000, 100);  // aggressive buy willing to pay 101.0000

  const auto t = trades();
  ASSERT_EQ(t.size(), 1U);
  EXPECT_EQ(t[0].price, 1000000)
      << "the trade must print at the resting ask, not the aggressor's limit";
  expect_invariants();
}

TEST_F(EngineTest, TradeAtRestingPriceAlsoWhenTheRestingSideIsTheBid) {
  send(1, Side::Buy, 1010000, 100);
  send(2, Side::Sell, 1000000, 100);
  const auto t = trades();
  ASSERT_EQ(t.size(), 1U);
  EXPECT_EQ(t[0].price, 1010000);
  expect_invariants();
}

// ---------------------------------------------------------------------------
// 8.3 Order types and time in force -- one test per row of the table
// ---------------------------------------------------------------------------

TEST_F(EngineTest, LimitDayMatchesWhatItCanAndRestsTheResidual) {
  send(1, Side::Sell, 1002200, 40);
  send(2, Side::Buy, 1002200, 100, TimeInForce::Day);
  EXPECT_EQ(trades().size(), 1U);
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002200), 60U);
  EXPECT_EQ(sink_.count(EventType::Cancelled), 0U);
  expect_invariants();
}

TEST_F(EngineTest, LimitIocMatchesWhatItCanAndCancelsTheResidual) {
  send(1, Side::Sell, 1002200, 40);
  send(2, Side::Buy, 1002200, 100, TimeInForce::IOC);
  EXPECT_EQ(trades().size(), 1U);
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002200), 0U)
      << "an IOC residual must not rest";
  const auto cancels = sink_.of_type(EventType::Cancelled);
  ASSERT_EQ(cancels.size(), 1U);
  EXPECT_EQ(cancels[0].id, 2U);
  EXPECT_EQ(cancels[0].qty, 60U);
  expect_invariants();
}

TEST_F(EngineTest, LimitIocWithNoLiquidityCancelsEntirelyWithoutResting) {
  send(1, Side::Buy, 1002200, 100, TimeInForce::IOC);
  EXPECT_EQ(trades().size(), 0U);
  EXPECT_EQ(engine_.book().order_count(), 0U);
  ASSERT_EQ(sink_.count(EventType::Cancelled), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Cancelled)[0].qty, 100U);
  expect_invariants();
}

TEST_F(EngineTest, LimitFokExecutesNothingWhenItCannotFillEntirely) {
  send(1, Side::Sell, 1002200, 40);
  send(2, Side::Buy, 1002200, 100, TimeInForce::FOK);

  EXPECT_EQ(trades().size(), 0U) << "a FOK that cannot fill must not trade";
  EXPECT_EQ(engine_.book().qty_at(Side::Sell, 1002200), 40U)
      << "the book must be untouched";
  ASSERT_EQ(sink_.count(EventType::Reject), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::FokUnfillable);
  EXPECT_EQ(sink_.count(EventType::Ack), 1U) << "only order 1 was acked";
  expect_invariants();
}

TEST_F(EngineTest, LimitFokFillsEntirelyAcrossSeveralLevelsWhenItCan) {
  send(1, Side::Sell, 1002200, 40);
  send(2, Side::Sell, 1002300, 40);
  send(3, Side::Sell, 1002400, 40);
  send(4, Side::Buy, 1002400, 100, TimeInForce::FOK);

  const auto t = trades();
  ASSERT_EQ(t.size(), 3U);
  EXPECT_EQ(t[0].qty + t[1].qty + t[2].qty, 100U);
  EXPECT_EQ(engine_.book().qty_at(Side::Sell, 1002400), 20U);
  EXPECT_EQ(sink_.count(EventType::Reject), 0U);
  expect_invariants();
}

TEST_F(EngineTest, FokPreScanIgnoresLevelsOutsideTheLimit) {
  send(1, Side::Sell, 1002200, 40);
  send(2, Side::Sell, 1002500, 1000); // plenty, but too expensive
  send(3, Side::Buy, 1002300, 100, TimeInForce::FOK);
  EXPECT_EQ(trades().size(), 0U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::FokUnfillable);
  expect_invariants();
}

TEST_F(EngineTest, PostOnlyRestsWhenItWouldNotCross) {
  send(1, Side::Sell, 1002400, 100);
  send(2, Side::Buy, 1002300, 100, TimeInForce::PostOnly);
  EXPECT_EQ(trades().size(), 0U);
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002300), 100U);
  EXPECT_EQ(sink_.count(EventType::Reject), 0U);
  expect_invariants();
}

TEST_F(EngineTest, PostOnlyIsRejectedEntirelyWhenItWouldCrossAndIsNotSlid) {
  send(1, Side::Sell, 1002400, 100);
  send(2, Side::Buy, 1002400, 100, TimeInForce::PostOnly);

  EXPECT_EQ(trades().size(), 0U);
  EXPECT_EQ(engine_.book().order_count(), 1U)
      << "the rejected order must not rest at a slid price either";
  ASSERT_EQ(sink_.count(EventType::Reject), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::PostOnlyWouldCross);
  expect_invariants();
}

TEST_F(EngineTest, MarketIocBehavesAsALimitAtInfinity) {
  send(1, Side::Sell, 1002200, 40);
  send(2, Side::Sell, 9999999, 40); // absurdly far away, still taken
  send(3, Side::Buy, 0, 80, TimeInForce::IOC, OrderType::Market);

  const auto t = trades();
  ASSERT_EQ(t.size(), 2U);
  EXPECT_EQ(t[0].price, 1002200);
  EXPECT_EQ(t[1].price, 9999999);
  EXPECT_EQ(engine_.book().order_count(), 0U);
  expect_invariants();
}

TEST_F(EngineTest, MarketDayIsRejected) {
  send(1, Side::Buy, 0, 100, TimeInForce::Day, OrderType::Market);
  ASSERT_EQ(sink_.count(EventType::Reject), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::MarketDayNotSupported);
  EXPECT_EQ(sink_.count(EventType::Ack), 0U);
  EXPECT_EQ(engine_.book().order_count(), 0U);
}

TEST_F(EngineTest, MarketFokFillsEntirelyAtAnyPriceOrRejects) {
  send(1, Side::Sell, 1002200, 40);
  send(2, Side::Buy, 0, 100, TimeInForce::FOK, OrderType::Market);
  EXPECT_EQ(trades().size(), 0U);
  ASSERT_EQ(sink_.count(EventType::Reject), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::MarketOrderNotFillable);

  send(3, Side::Sell, 9999999, 60);
  send(4, Side::Buy, 0, 100, TimeInForce::FOK, OrderType::Market);
  EXPECT_EQ(trades().size(), 2U);
  EXPECT_EQ(engine_.book().order_count(), 0U);
  expect_invariants();
}

TEST_F(EngineTest, MarketOrderResidualIsCancelledNeverRested) {
  send(1, Side::Sell, 1002200, 40);
  send(2, Side::Buy, 0, 100, TimeInForce::IOC, OrderType::Market);
  EXPECT_EQ(engine_.book().best(Side::Buy), kInvalidPrice);
  ASSERT_EQ(sink_.count(EventType::Cancelled), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Cancelled)[0].qty, 60U);
  expect_invariants();
}

// ---------------------------------------------------------------------------
// 8.4 Cancel
// ---------------------------------------------------------------------------

TEST_F(EngineTest, CancelRemovesTheOrderAndReportsTheOpenQuantity) {
  send(1, Side::Buy, 1002100, 100);
  cancel(1);
  EXPECT_EQ(engine_.book().order_count(), 0U);
  ASSERT_EQ(sink_.count(EventType::Cancelled), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Cancelled)[0].qty, 100U);
  expect_invariants();
}

TEST_F(EngineTest, CancelOfAPartiallyFilledOrderReportsOnlyWhatWasStillOpen) {
  send(1, Side::Buy, 1002100, 100);
  send(2, Side::Sell, 1002100, 30);
  cancel(1);
  const auto cancels = sink_.of_type(EventType::Cancelled);
  ASSERT_EQ(cancels.size(), 1U);
  EXPECT_EQ(cancels[0].qty, 70U);
  expect_invariants();
}

TEST_F(EngineTest, CancelOfAnUnknownIdIsRejectedNotACrash) {
  cancel(999);
  ASSERT_EQ(sink_.count(EventType::Reject), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::UnknownOrder);
}

TEST_F(EngineTest, CancelOfAFullyFilledOrderIsTooLateNotUnknown) {
  send(1, Side::Buy, 1002100, 100);
  send(2, Side::Sell, 1002100, 100); // fills order 1 completely
  cancel(1);
  const auto rejects = sink_.of_type(EventType::Reject);
  ASSERT_EQ(rejects.size(), 1U);
  EXPECT_EQ(rejects[0].reason, RejectReason::TooLateToCancel)
      << "the engine must distinguish 'already gone' from 'never existed'";
  expect_invariants();
}

TEST_F(EngineTest, CancelTwiceIsTooLateTheSecondTime) {
  send(1, Side::Buy, 1002100, 100);
  cancel(1);
  cancel(1);
  ASSERT_EQ(sink_.count(EventType::Reject), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::TooLateToCancel);
}

TEST_F(EngineTest, CompletedIdMemoryIsBoundedAndSaysSoAtTheBoundary) {
  // With a two-entry memory, the third completion evicts the first, and a
  // cancel of the first then reports UnknownOrder. This is the documented
  // boundary behaviour, not a bug -- see engine/completed_ids.hpp.
  EngineConfig cfg;
  cfg.completed_capacity = 2;
  VectorSink sink;
  Engine engine(sink, cfg);
  TsNanos t = 0;
  for (OrderId id = 1; id <= 3; ++id) {
    NewOrder n{};
    n.id = id;
    n.price = 1002100;
    n.qty = 10;
    n.side = Side::Buy;
    engine.on_new(n, ++t);
    engine.on_cancel(CancelOrder{id}, ++t);
  }
  engine.on_cancel(CancelOrder{3}, ++t);
  engine.on_cancel(CancelOrder{1}, ++t);
  const auto rejects = sink.of_type(EventType::Reject);
  ASSERT_EQ(rejects.size(), 2U);
  EXPECT_EQ(rejects[0].reason, RejectReason::TooLateToCancel);
  EXPECT_EQ(rejects[1].reason, RejectReason::UnknownOrder);
}

// ---------------------------------------------------------------------------
// 8.6 Self-trade prevention
// ---------------------------------------------------------------------------

class StpEngineTest : public EngineTest {
protected:
  StpEngineTest() : EngineTest(make_cfg()) {}
  static EngineConfig make_cfg() {
    EngineConfig cfg;
    cfg.stp = SelfTradePolicy::CancelResting;
    return cfg;
  }
};

TEST_F(EngineTest, SelfTradePreventionIsOffByDefault) {
  send(1, Side::Sell, 1002200, 100, TimeInForce::Day, OrderType::Limit, 7);
  send(2, Side::Buy, 1002200, 100, TimeInForce::Day, OrderType::Limit, 7);
  EXPECT_EQ(trades().size(), 1U)
      << "with STP off, a participant may self-trade";
  expect_invariants();
}

TEST_F(StpEngineTest, RestingOrderIsCancelledAndTheAggressorKeepsWalking) {
  send(1, Side::Sell, 1002200, 100, TimeInForce::Day, OrderType::Limit, 7);
  send(2, Side::Sell, 1002300, 100, TimeInForce::Day, OrderType::Limit, 9);
  send(3, Side::Buy, 1002300, 100, TimeInForce::Day, OrderType::Limit, 7);

  const auto t = trades();
  ASSERT_EQ(t.size(), 1U);
  EXPECT_EQ(t[0].counter_id, 2U) << "it should have skipped its own order";
  EXPECT_EQ(t[0].price, 1002300);

  const auto cancels = sink_.of_type(EventType::Cancelled);
  ASSERT_EQ(cancels.size(), 1U);
  EXPECT_EQ(cancels[0].id, 1U);
  EXPECT_EQ(cancels[0].reason, RejectReason::SelfTradePrevented);
  EXPECT_EQ(engine_.stats().stp_cancels, 1U);
  expect_invariants();
}

TEST_F(StpEngineTest, ParticipantZeroIsUnattributedAndNeverSelfTrades) {
  send(1, Side::Sell, 1002200, 100);
  send(2, Side::Buy, 1002200, 100);
  EXPECT_EQ(trades().size(), 1U);
  EXPECT_EQ(engine_.stats().stp_cancels, 0U);
  expect_invariants();
}

TEST_F(StpEngineTest, FokPreScanDoesNotCountLiquidityItWillCancel) {
  // 100 shares are resting, but they belong to the aggressor, so STP will
  // cancel rather than trade with them. A FOK for 100 must therefore reject
  // rather than cancel the resting order and then fail to fill.
  send(1, Side::Sell, 1002200, 100, TimeInForce::Day, OrderType::Limit, 7);
  send(2, Side::Buy, 1002200, 100, TimeInForce::FOK, OrderType::Limit, 7);

  EXPECT_EQ(trades().size(), 0U);
  ASSERT_EQ(sink_.count(EventType::Reject), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::FokUnfillable);
  EXPECT_EQ(engine_.book().qty_at(Side::Sell, 1002200), 100U)
      << "a rejected FOK must leave the book untouched, STP included";
  expect_invariants();
}

// ---------------------------------------------------------------------------
// 8.7 Rejection reasons
// ---------------------------------------------------------------------------

TEST_F(EngineTest, ZeroQuantityIsRejectedNotIgnored) {
  send(1, Side::Buy, 1002100, 0);
  ASSERT_EQ(sink_.count(EventType::Reject), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::ZeroQuantity);
  EXPECT_EQ(sink_.count(EventType::Ack), 0U);
}

TEST_F(EngineTest, NonPositivePricesAreRejected) {
  send(1, Side::Buy, 0, 100);
  send(2, Side::Buy, -5, 100);
  const auto rejects = sink_.of_type(EventType::Reject);
  ASSERT_EQ(rejects.size(), 2U);
  EXPECT_EQ(rejects[0].reason, RejectReason::InvalidPrice);
  EXPECT_EQ(rejects[1].reason, RejectReason::InvalidPrice);
}

TEST_F(EngineTest, DuplicateLiveOrderIdIsRejected) {
  send(1, Side::Buy, 1002100, 100);
  send(1, Side::Buy, 1002100, 100);
  ASSERT_EQ(sink_.count(EventType::Reject), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::DuplicateOrderId);
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002100), 100U);
}

TEST_F(EngineTest, PricesOutsideAConfiguredBandAreRejected) {
  EngineConfig cfg;
  cfg.min_price = 1000000;
  cfg.max_price = 1100000;
  VectorSink sink;
  Engine engine(sink, cfg);
  NewOrder n{};
  n.id = 1;
  n.price = 900000;
  n.qty = 10;
  n.side = Side::Buy;
  engine.on_new(n, 1);
  n.id = 2;
  n.price = 1200000;
  engine.on_new(n, 2);
  n.id = 3;
  n.price = 1050000;
  engine.on_new(n, 3);

  const auto rejects = sink.of_type(EventType::Reject);
  ASSERT_EQ(rejects.size(), 2U);
  EXPECT_EQ(rejects[0].reason, RejectReason::PriceOutOfBand);
  EXPECT_EQ(rejects[1].reason, RejectReason::PriceOutOfBand);
  EXPECT_EQ(engine.book().order_count(), 1U);
}

// ---------------------------------------------------------------------------
// 9.5 Determinism, and 9.2 sequence numbering
// ---------------------------------------------------------------------------

TEST_F(EngineTest, EverySequenceNumberIsUsedExactlyOnceInOrder) {
  send(1, Side::Sell, 1002200, 100);
  send(2, Side::Buy, 1002200, 60);
  cancel(1);
  cancel(1);
  for (SeqNum i = 0; i < sink_.size(); ++i) EXPECT_EQ(sink_[i].seq, i);
  EXPECT_EQ(engine_.next_seq(), sink_.size());
}

TEST(EngineDeterminism, TheSameCommandsProduceTheSameEventStream) {
  const auto run = [] {
    VectorSink sink;
    Engine engine(sink);
    TsNanos t = 0;
    for (OrderId i = 1; i <= 200; ++i) {
      NewOrder n{};
      n.id = i;
      n.side = (i % 2 == 0) ? Side::Buy : Side::Sell;
      n.price = 1002000 + static_cast<Price>((i * 37) % 40);
      n.qty = static_cast<Qty>(10 + (i % 7) * 10);
      engine.on_new(n, ++t);
      if (i % 5 == 0) engine.on_cancel(CancelOrder{i - 3}, ++t);
      if (i % 11 == 0)
        engine.on_replace(ReplaceOrder{i - 1, n.price + 3, 50}, ++t);
    }
    std::vector<std::string> lines;
    lines.reserve(sink.size());
    for (const Event& e : sink.events) lines.push_back(to_line(e));
    return lines;
  };
  const auto a = run();
  const auto b = run();
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a, b);
}

TEST_F(EngineTest, EngineNeverReadsTheClock) {
  // The timestamp on every outbound event is exactly the one that came in
  // with the command. If the engine ever called now(), this would fail.
  send(1, Side::Sell, 1002200, 100);
  send(2, Side::Buy, 1002200, 100);
  const TsNanos second_command_ts = clock_;
  for (const Event& e : sink_.events) {
    EXPECT_TRUE(e.ts == second_command_ts || e.ts == second_command_ts - 1);
  }
}

// ---------------------------------------------------------------------------
// 9.6 Ingress
// ---------------------------------------------------------------------------

TEST_F(EngineTest, DrainAppliesCommandsFromTheIngressRing) {
  auto ring = std::make_unique<SpscRing<Command, kRingCapacity>>();
  NewOrder n{};
  n.id = 1;
  n.price = 1002200;
  n.qty = 100;
  n.side = Side::Sell;
  ASSERT_TRUE(ring->try_push(Command::make_new(1, n)));
  n.id = 2;
  n.side = Side::Buy;
  ASSERT_TRUE(ring->try_push(Command::make_new(2, n)));
  ASSERT_TRUE(ring->try_push(Command::make_cancel(3, 1)));

  EXPECT_EQ(engine_.drain(*ring, 2), 2U);
  EXPECT_EQ(engine_.stats().commands, 2U);
  EXPECT_EQ(engine_.drain(*ring, 10), 1U);
  EXPECT_EQ(engine_.drain(*ring, 10), 0U);
  EXPECT_EQ(engine_.stats().trades, 1U);
}

} // namespace
} // namespace miniex
