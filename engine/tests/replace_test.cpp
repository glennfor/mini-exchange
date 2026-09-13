// Spec section 8.5: cancel/replace priority. Every row of the table gets its
// own test, plus the atomicity requirement and the crossing case.

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "book/naive_book.hpp"
#include "engine/event_sink.hpp"
#include "engine/invariants.hpp"
#include "engine/matching_engine.hpp"

namespace miniex {
namespace {

using Engine = MatchingEngine<NaiveBook, VectorSink>;

class ReplaceTest : public ::testing::Test {
protected:
  ReplaceTest() : engine_(sink_) {}

  void send(OrderId id, Side side, Price price, Qty qty,
            ParticipantId participant = 0) {
    NewOrder n{};
    n.id = id;
    n.price = price;
    n.qty = qty;
    n.side = side;
    n.participant = participant;
    engine_.on_new(n, ++clock_);
  }
  void replace(OrderId id, Price price, Qty qty) {
    engine_.on_replace(ReplaceOrder{id, price, qty}, ++clock_);
  }

  /// The ids resting at one price, head (oldest) first -- i.e. the queue.
  [[nodiscard]] std::vector<OrderId> queue_at(Side side, Price price) const {
    std::vector<OrderId> ids;
    engine_.book().for_each_order(side, price,
                                  [&](const Order& o) { ids.push_back(o.id); });
    return ids;
  }

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

// --- row 1: quantity decrease only -> KEEPS time priority -------------------

TEST_F(ReplaceTest, QuantityDecreaseKeepsTimePriority) {
  send(1, Side::Buy, 1002100, 100);
  send(2, Side::Buy, 1002100, 100);
  send(3, Side::Buy, 1002100, 100);

  replace(2, 1002100, 50);

  EXPECT_EQ(queue_at(Side::Buy, 1002100), (std::vector<OrderId>{1, 2, 3}))
      << "a size-down must not move the order to the back of the queue";
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002100), 250U);
  ASSERT_EQ(sink_.count(EventType::Replaced), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Replaced)[0].qty, 50U);
  expect_invariants();
}

TEST_F(ReplaceTest, QuantityDecreaseKeepsItsPlaceInTheFillOrder) {
  send(1, Side::Buy, 1002100, 100);
  send(2, Side::Buy, 1002100, 100);
  replace(1, 1002100, 40);
  send(3, Side::Sell, 1002100, 40);

  const auto t = sink_.of_type(EventType::Trade);
  ASSERT_EQ(t.size(), 1U);
  EXPECT_EQ(t[0].counter_id, 1U) << "order 1 kept priority, so it fills first";
  expect_invariants();
}

TEST_F(ReplaceTest, ReplaceToTheSameQuantityAndPriceIsANoOpThatKeepsPriority) {
  send(1, Side::Buy, 1002100, 100);
  send(2, Side::Buy, 1002100, 100);
  replace(1, 1002100, 100);
  EXPECT_EQ(queue_at(Side::Buy, 1002100), (std::vector<OrderId>{1, 2}));
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002100), 200U);
  expect_invariants();
}

// --- row 2: quantity increase -> LOSES time priority ------------------------

TEST_F(ReplaceTest, QuantityIncreaseLosesTimePriority) {
  send(1, Side::Buy, 1002100, 100);
  send(2, Side::Buy, 1002100, 100);
  send(3, Side::Buy, 1002100, 100);

  replace(1, 1002100, 200);

  EXPECT_EQ(queue_at(Side::Buy, 1002100), (std::vector<OrderId>{2, 3, 1}))
      << "asking for more size must send the order to the tail";
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002100), 400U);
  expect_invariants();
}

// --- row 3: price change in either direction -> LOSES time priority ---------

TEST_F(ReplaceTest, PriceImprovementLosesTimePriorityAtTheNewLevel) {
  send(1, Side::Buy, 1002100, 100);
  send(2, Side::Buy, 1002200, 100);
  send(3, Side::Buy, 1002200, 100);

  replace(1, 1002200, 100);

  EXPECT_EQ(queue_at(Side::Buy, 1002200), (std::vector<OrderId>{2, 3, 1}));
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002100), 0U);
  expect_invariants();
}

TEST_F(ReplaceTest, PriceWorseningAlsoLosesTimePriority) {
  send(1, Side::Buy, 1002200, 100);
  send(2, Side::Buy, 1002100, 100);
  send(3, Side::Buy, 1002100, 100);

  replace(1, 1002100, 100);

  EXPECT_EQ(queue_at(Side::Buy, 1002100), (std::vector<OrderId>{2, 3, 1}));
  EXPECT_EQ(engine_.book().best(Side::Buy), 1002100);
  expect_invariants();
}

// --- row 4: price change that crosses -> matches immediately ----------------

TEST_F(ReplaceTest, PriceChangeThatCrossesMatchesImmediately) {
  send(1, Side::Sell, 1002300, 60);
  send(2, Side::Buy, 1002100, 100);

  replace(2, 1002300, 100);

  const auto t = sink_.of_type(EventType::Trade);
  ASSERT_EQ(t.size(), 1U);
  EXPECT_EQ(t[0].price, 1002300) << "still prints at the resting price";
  EXPECT_EQ(t[0].qty, 60U);
  EXPECT_EQ(t[0].id, 2U) << "the replaced order is the aggressor";
  // The residual rests at the new price.
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002300), 40U);
  EXPECT_EQ(engine_.book().best(Side::Sell), kInvalidPrice);
  expect_invariants();
}

TEST_F(ReplaceTest, ReplacedEventPrecedesTheTradesItCauses) {
  send(1, Side::Sell, 1002300, 60);
  send(2, Side::Buy, 1002100, 100);
  const std::size_t before = sink_.size();
  replace(2, 1002300, 100);

  ASSERT_GT(sink_.size(), before + 1);
  EXPECT_EQ(sink_[before].type, EventType::Replaced);
  EXPECT_EQ(sink_[before + 1].type, EventType::Trade);
  expect_invariants();
}

TEST_F(ReplaceTest, ACrossingReplaceThatFillsCompletelyLeavesNothingResting) {
  send(1, Side::Sell, 1002300, 200);
  send(2, Side::Buy, 1002100, 100);
  replace(2, 1002300, 100);
  EXPECT_EQ(engine_.book().best(Side::Buy), kInvalidPrice);
  EXPECT_EQ(engine_.book().qty_at(Side::Sell, 1002300), 100U);
  expect_invariants();
}

// --- error paths ------------------------------------------------------------

TEST_F(ReplaceTest, ReplaceOfAnUnknownOrderIsRejected) {
  replace(42, 1002100, 100);
  ASSERT_EQ(sink_.count(EventType::Reject), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::UnknownOrder);
}

TEST_F(ReplaceTest, ReplaceOfACompletedOrderIsTooLate) {
  send(1, Side::Buy, 1002100, 100);
  send(2, Side::Sell, 1002100, 100);
  replace(1, 1002200, 50);
  const auto rejects = sink_.of_type(EventType::Reject);
  ASSERT_EQ(rejects.size(), 1U);
  EXPECT_EQ(rejects[0].reason, RejectReason::TooLateToCancel);
  expect_invariants();
}

TEST_F(ReplaceTest, ReplaceToZeroQuantityIsRejectedAndLeavesTheOrderAlone) {
  send(1, Side::Buy, 1002100, 100);
  replace(1, 1002100, 0);
  ASSERT_EQ(sink_.count(EventType::Reject), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::ZeroQuantity);
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002100), 100U)
      << "a rejected replace must leave the original order untouched";
  expect_invariants();
}

TEST_F(ReplaceTest, ReplaceToAnInvalidPriceIsRejectedAndLeavesTheOrderAlone) {
  send(1, Side::Buy, 1002100, 100);
  replace(1, 0, 50);
  ASSERT_EQ(sink_.count(EventType::Reject), 1U);
  EXPECT_EQ(sink_.of_type(EventType::Reject)[0].reason,
            RejectReason::InvalidPrice);
  EXPECT_EQ(engine_.book().qty_at(Side::Buy, 1002100), 100U);
  expect_invariants();
}

/// A book whose storage can be made to refuse the next add, so the atomicity
/// requirement of section 8.5 can actually be exercised. Everything else
/// delegates to NaiveBook.
class CappedBook : public NaiveBook {
public:
  void set_capacity(std::size_t cap) { cap_ = cap; }
  [[nodiscard]] bool can_add() const { return order_count() < cap_; }
  bool add(OrderId id, Side side, Price price, Qty qty, TsNanos ts,
           ParticipantId participant) {
    if (!can_add()) return false;
    return NaiveBook::add(id, side, price, qty, ts, participant);
  }

private:
  std::size_t cap_{~std::size_t{0}};
};
static_assert(OrderBook<CappedBook>);

TEST(ReplaceAtomicity, ReplaceIntoAnExhaustedPoolLeavesTheOriginalUntouched) {
  VectorSink sink;
  MatchingEngine<CappedBook, VectorSink> engine(sink);
  NewOrder n{};
  n.id = 1;
  n.price = 1002100;
  n.qty = 100;
  n.side = Side::Buy;
  engine.on_new(n, 1);
  ASSERT_EQ(engine.book().order_count(), 1U);

  engine.book().set_capacity(1); // the next add must fail
  engine.on_replace(ReplaceOrder{1, 1002200, 100}, 2);

  const auto rejects = sink.of_type(EventType::Reject);
  ASSERT_EQ(rejects.size(), 1U);
  EXPECT_EQ(rejects[0].reason, RejectReason::PoolExhausted);
  EXPECT_EQ(engine.book().order_count(), 1U)
      << "the client's order must not vanish because the replace failed";
  EXPECT_EQ(engine.book().qty_at(Side::Buy, 1002100), 100U);
  EXPECT_EQ(engine.book().qty_at(Side::Buy, 1002200), 0U);
}

TEST(ReplaceAtomicity, AnOrderThatCannotRestAfterMatchingIsCancelledNotLost) {
  VectorSink sink;
  MatchingEngine<CappedBook, VectorSink> engine(sink);
  NewOrder n{};
  n.id = 1;
  n.price = 1002100;
  n.qty = 40;
  n.side = Side::Sell;
  engine.on_new(n, 1);
  engine.book().set_capacity(0); // nothing further may rest

  n.id = 2;
  n.price = 1002100;
  n.qty = 100;
  n.side = Side::Buy;
  engine.on_new(n, 2);

  // It traded against what was there, then had nowhere to rest.
  EXPECT_EQ(sink.count(EventType::Trade), 1U);
  const auto cancels = sink.of_type(EventType::Cancelled);
  ASSERT_EQ(cancels.size(), 1U);
  EXPECT_EQ(cancels[0].id, 2U);
  EXPECT_EQ(cancels[0].qty, 60U);
  EXPECT_EQ(cancels[0].reason, RejectReason::PoolExhausted)
      << "the client must be able to tell 'no room' from 'you said IOC'";
}

} // namespace
} // namespace miniex
