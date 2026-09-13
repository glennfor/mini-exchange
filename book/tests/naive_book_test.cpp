// NaiveBook is the in-repo oracle, so its own behaviour is pinned down
// directly rather than only through the engine.

#include <vector>

#include <gtest/gtest.h>

#include "book/naive_book.hpp"

namespace miniex {
namespace {

class NaiveBookTest : public ::testing::Test {
protected:
  void add(OrderId id, Side side, Price price, Qty qty,
           ParticipantId participant = 0) {
    ASSERT_TRUE(book_.add(id, side, price, qty, ++ts_, participant));
  }
  [[nodiscard]] std::vector<OrderId> queue_at(Side side, Price price) {
    std::vector<OrderId> ids;
    book_.for_each_order(side, price,
                         [&](const Order& o) { ids.push_back(o.id); });
    return ids;
  }
  NaiveBook book_;
  TsNanos ts_{0};
};

TEST_F(NaiveBookTest, EmptyBookReportsNoPrices) {
  EXPECT_EQ(book_.best(Side::Buy), kInvalidPrice);
  EXPECT_EQ(book_.best(Side::Sell), kInvalidPrice);
  EXPECT_EQ(book_.qty_at(Side::Buy, 100), 0U);
  EXPECT_EQ(book_.order_count(), 0U);
  EXPECT_EQ(book_.front(Side::Buy), nullptr);
  EXPECT_EQ(book_.find(1), nullptr);
}

TEST_F(NaiveBookTest, BestIsTheHighestBidAndTheLowestAsk) {
  add(1, Side::Buy, 1002100, 100);
  add(2, Side::Buy, 1002300, 100);
  add(3, Side::Buy, 1002200, 100);
  add(4, Side::Sell, 1002600, 100);
  add(5, Side::Sell, 1002400, 100);
  add(6, Side::Sell, 1002500, 100);
  EXPECT_EQ(book_.best(Side::Buy), 1002300);
  EXPECT_EQ(book_.best(Side::Sell), 1002400);
}

TEST_F(NaiveBookTest, NextPriceWalksAwayFromTheTouchOnBothSides) {
  add(1, Side::Buy, 1002100, 100);
  add(2, Side::Buy, 1002200, 100);
  add(3, Side::Buy, 1002300, 100);
  EXPECT_EQ(book_.next_price(Side::Buy, 1002300), 1002200);
  EXPECT_EQ(book_.next_price(Side::Buy, 1002200), 1002100);
  EXPECT_EQ(book_.next_price(Side::Buy, 1002100), kInvalidPrice);

  add(4, Side::Sell, 1002400, 100);
  add(5, Side::Sell, 1002500, 100);
  EXPECT_EQ(book_.next_price(Side::Sell, 1002400), 1002500);
  EXPECT_EQ(book_.next_price(Side::Sell, 1002500), kInvalidPrice);
}

TEST_F(NaiveBookTest, AddsAppendAtTheTailOfTheirLevel) {
  add(1, Side::Buy, 1002100, 10);
  add(2, Side::Buy, 1002100, 20);
  add(3, Side::Buy, 1002100, 30);
  EXPECT_EQ(queue_at(Side::Buy, 1002100), (std::vector<OrderId>{1, 2, 3}));
  EXPECT_EQ(book_.qty_at(Side::Buy, 1002100), 60U);
  EXPECT_EQ(book_.orders_at(Side::Buy, 1002100), 3U);
  EXPECT_EQ(book_.front(Side::Buy)->id, 1U);
}

TEST_F(NaiveBookTest, DuplicateIdIsRefused) {
  add(1, Side::Buy, 1002100, 10);
  EXPECT_FALSE(book_.add(1, Side::Buy, 1002200, 10, ++ts_, 0));
  EXPECT_EQ(book_.order_count(), 1U);
}

TEST_F(NaiveBookTest, CancelRemovesTheOrderAndEmptyLevelsDisappear) {
  add(1, Side::Buy, 1002100, 10);
  add(2, Side::Buy, 1002100, 20);
  EXPECT_TRUE(book_.cancel(1));
  EXPECT_EQ(book_.qty_at(Side::Buy, 1002100), 20U);
  EXPECT_EQ(queue_at(Side::Buy, 1002100), (std::vector<OrderId>{2}));
  EXPECT_TRUE(book_.cancel(2));
  EXPECT_EQ(book_.level_count(Side::Buy), 0U);
  EXPECT_EQ(book_.best(Side::Buy), kInvalidPrice);
  EXPECT_FALSE(book_.cancel(1));
}

TEST_F(NaiveBookTest, ReduceKeepsPositionAndRemovesAtZero) {
  add(1, Side::Buy, 1002100, 100);
  add(2, Side::Buy, 1002100, 100);
  EXPECT_TRUE(book_.reduce(1, 40));
  EXPECT_EQ(book_.qty_at(Side::Buy, 1002100), 160U);
  EXPECT_EQ(queue_at(Side::Buy, 1002100), (std::vector<OrderId>{1, 2}));
  EXPECT_FALSE(book_.reduce(1, 100)) << "reducing past zero must fail";
  EXPECT_TRUE(book_.reduce(1, 60));
  EXPECT_EQ(queue_at(Side::Buy, 1002100), (std::vector<OrderId>{2}));
  EXPECT_FALSE(book_.reduce(99, 1));
}

TEST_F(NaiveBookTest, FillConsumesFromTheHeadAndRemovesExhaustedOrders) {
  add(1, Side::Sell, 1002400, 100);
  add(2, Side::Sell, 1002400, 100);
  OrderHandle h = book_.front(Side::Sell);
  ASSERT_NE(h, nullptr);
  book_.fill(h, 40);
  EXPECT_EQ(book_.qty_at(Side::Sell, 1002400), 160U);
  EXPECT_EQ(book_.front(Side::Sell)->id, 1U);
  EXPECT_EQ(book_.front(Side::Sell)->qty_open, 60U);
  book_.fill(book_.front(Side::Sell), 60);
  EXPECT_EQ(book_.front(Side::Sell)->id, 2U);
  EXPECT_EQ(book_.order_count(), 1U);
}

TEST_F(NaiveBookTest, QtyAtExcludingSkipsOneParticipant) {
  add(1, Side::Sell, 1002400, 100, 7);
  add(2, Side::Sell, 1002400, 50, 9);
  add(3, Side::Sell, 1002400, 25, 7);
  EXPECT_EQ(book_.qty_at(Side::Sell, 1002400), 175U);
  EXPECT_EQ(book_.qty_at_excluding(Side::Sell, 1002400, 7), 50U);
  EXPECT_EQ(book_.qty_at_excluding(Side::Sell, 1002400, 9), 125U);
  EXPECT_EQ(book_.qty_at_excluding(Side::Sell, 1002400, 0), 175U)
      << "participant 0 is unattributed and excludes nothing";
}

TEST_F(NaiveBookTest, HandlesRemainValidAsOtherOrdersComeAndGo) {
  add(1, Side::Buy, 1002100, 100);
  OrderHandle h = book_.front(Side::Buy);
  for (OrderId id = 2; id <= 50; ++id) add(id, Side::Buy, 1002100, 10);
  for (OrderId id = 2; id <= 40; ++id) ASSERT_TRUE(book_.cancel(id));
  EXPECT_EQ(h->id, 1U) << "list nodes must not move";
  EXPECT_EQ(book_.front(Side::Buy), h);
}

TEST_F(NaiveBookTest, ClearEmptiesEverything) {
  add(1, Side::Buy, 1002100, 100);
  add(2, Side::Sell, 1002400, 100);
  book_.clear();
  EXPECT_EQ(book_.order_count(), 0U);
  EXPECT_EQ(book_.best(Side::Buy), kInvalidPrice);
  EXPECT_EQ(book_.best(Side::Sell), kInvalidPrice);
}

TEST_F(NaiveBookTest, LevelsAreVisitedFromTheTouchOutwards) {
  add(1, Side::Buy, 1002100, 10);
  add(2, Side::Buy, 1002300, 10);
  add(3, Side::Buy, 1002200, 10);
  std::vector<Price> prices;
  book_.for_each_level(
      Side::Buy, [&](Price p, AggQty, std::size_t) { prices.push_back(p); });
  EXPECT_EQ(prices, (std::vector<Price>{1002300, 1002200, 1002100}));

  add(4, Side::Sell, 1002600, 10);
  add(5, Side::Sell, 1002400, 10);
  prices.clear();
  book_.for_each_level(
      Side::Sell, [&](Price p, AggQty, std::size_t) { prices.push_back(p); });
  EXPECT_EQ(prices, (std::vector<Price>{1002400, 1002600}));
}

} // namespace
} // namespace miniex
