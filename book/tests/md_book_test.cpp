// MarketDataBook: the reconstruction rules of spec section 11, including the
// ones that make it different from a matching book.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "book/md_book.hpp"

namespace miniex {
namespace {

class MdBookTest : public ::testing::Test {
protected:
  MarketDataBook book_;
};

// --- the message table ------------------------------------------------------

TEST_F(MdBookTest, AddInsertsAndAggregates) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 500));
  ASSERT_TRUE(book_.add(2, Side::Buy, 1002100, 300));
  ASSERT_TRUE(book_.add(3, Side::Sell, 1002400, 200));
  EXPECT_EQ(book_.best(Side::Buy), 1002100);
  EXPECT_EQ(book_.best(Side::Sell), 1002400);
  EXPECT_EQ(book_.qty_at(Side::Buy, 1002100), 800U);
  EXPECT_EQ(book_.order_count(), 3U);
  EXPECT_EQ(book_.stats().adds, 3U);
  EXPECT_TRUE(book_.verify_aggregates());
}

TEST_F(MdBookTest, ExecuteReducesAndPrintsAtTheRestingPrice) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 500));
  ASSERT_TRUE(book_.execute(1, 200));
  EXPECT_EQ(book_.qty_at(Side::Buy, 1002100), 300U);
  EXPECT_EQ(book_.stats().printable_volume, 200U);
  EXPECT_EQ(book_.stats().last_trade_price, 1002100)
      << "an 'E' carries no price; the trade prints where the order rested";
  EXPECT_TRUE(book_.verify_aggregates());
}

TEST_F(MdBookTest, ExecuteToZeroRemovesTheOrder) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 500));
  ASSERT_TRUE(book_.execute(1, 500));
  EXPECT_EQ(book_.order_count(), 0U);
  EXPECT_EQ(book_.best(Side::Buy), kInvalidPrice);
  EXPECT_EQ(book_.level_count(Side::Buy), 0U);
}

TEST_F(MdBookTest, ExecuteWithPricePrintsAtTheMessagePriceAndHonoursPrintable) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 500));
  ASSERT_TRUE(book_.execute_with_price(1, 100, 1002050, /*printable=*/true));
  EXPECT_EQ(book_.stats().last_trade_price, 1002050);
  EXPECT_EQ(book_.stats().printable_volume, 100U);

  ASSERT_TRUE(book_.execute_with_price(1, 100, 1002040, /*printable=*/false));
  EXPECT_EQ(book_.qty_at(Side::Buy, 1002100), 300U)
      << "a non-printable execution still changes the book";
  EXPECT_EQ(book_.stats().printable_volume, 100U)
      << "...but is excluded from volume";
  EXPECT_EQ(book_.stats().last_trade_price, 1002050)
      << "...and does not move the last price";
}

TEST_F(MdBookTest, CancelReducesWhileDeleteRemoves) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 500));
  ASSERT_TRUE(book_.cancel(1, 200));
  EXPECT_EQ(book_.qty_at(Side::Buy, 1002100), 300U)
      << "an 'X' reduces; the order survives";
  EXPECT_EQ(book_.order_count(), 1U);

  ASSERT_TRUE(book_.remove(1));
  EXPECT_EQ(book_.order_count(), 0U) << "a 'D' removes whatever is left";
  EXPECT_EQ(book_.stats().cancels, 1U);
  EXPECT_EQ(book_.stats().deletes, 1U);
}

TEST_F(MdBookTest, ReplaceRetiresTheOldReferenceAndIntroducesANewOne) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 500));
  ASSERT_TRUE(book_.replace(1, 2, 300, 1002200));

  EXPECT_EQ(book_.qty_at(Side::Buy, 1002100), 0U);
  EXPECT_EQ(book_.qty_at(Side::Buy, 1002200), 300U);
  EXPECT_EQ(book_.order_count(), 1U);
  EXPECT_EQ(book_.stats().replaces, 1U);
  EXPECT_EQ(book_.stats().adds, 1U)
      << "the replace's add is not a separate add";

  // The old reference is dead; touching it is an unknown-reference event.
  EXPECT_FALSE(book_.execute(1, 10));
  EXPECT_EQ(book_.stats().unknown_reference, 1U);
  EXPECT_TRUE(book_.verify_aggregates());
}

TEST_F(MdBookTest, ReplaceKeepsTheOrderOnItsOriginalSide) {
  ASSERT_TRUE(book_.add(1, Side::Sell, 1002400, 500));
  ASSERT_TRUE(book_.replace(1, 2, 300, 1002500));
  EXPECT_EQ(book_.best(Side::Sell), 1002500);
  EXPECT_EQ(book_.best(Side::Buy), kInvalidPrice)
      << "a replace never changes the side; the message does not carry one";
}

TEST_F(MdBookTest, HiddenPrintsAndCrossesDoNotTouchTheBook) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 500));
  book_.hidden_print(1002150, 250);
  book_.cross_print(1002000, 1000000);

  EXPECT_EQ(book_.qty_at(Side::Buy, 1002100), 500U)
      << "a 'P' matched liquidity this book never saw";
  EXPECT_EQ(book_.order_count(), 1U);
  EXPECT_EQ(book_.stats().hidden_prints, 1U);
  EXPECT_EQ(book_.stats().crosses, 1U);
  EXPECT_EQ(book_.stats().printable_volume, 1000250U);
}

TEST_F(MdBookTest, BrokenTradesAdjustStatisticsOnly) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 500));
  book_.hidden_print(1002150, 250);
  book_.broken_trade();
  EXPECT_EQ(book_.stats().broken, 1U);
  EXPECT_EQ(book_.order_count(), 1U);
}

// --- the gotchas of section 11 ---------------------------------------------

TEST_F(MdBookTest, AnUnknownReferenceIsCountedNotFatal) {
  // Exactly what happens when a file is joined mid-day: executions and
  // cancels arrive for orders that were added before the first message read.
  EXPECT_FALSE(book_.execute(999, 100));
  EXPECT_FALSE(book_.cancel(999, 100));
  EXPECT_FALSE(book_.remove(999));
  EXPECT_FALSE(book_.replace(999, 1000, 100, 1002100));
  EXPECT_EQ(book_.stats().unknown_reference, 4U);
  EXPECT_EQ(book_.order_count(), 0U);
  EXPECT_TRUE(book_.verify_aggregates());
}

TEST_F(MdBookTest, ACrossedBookIsToleratedNotAsserted) {
  // The case that kills a naive reconstructor. A replace can move an order
  // through the opposite side, and the book stays crossed until the other
  // side reacts. Real feeds do this; MarketDataBook must survive it.
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 500));
  ASSERT_TRUE(book_.add(2, Side::Sell, 1002400, 500));
  EXPECT_FALSE(book_.is_crossed_or_locked());

  ASSERT_TRUE(book_.replace(2, 3, 500, 1002000)); // ask now below the bid

  EXPECT_TRUE(book_.is_crossed_or_locked());
  EXPECT_EQ(book_.best(Side::Buy), 1002100);
  EXPECT_EQ(book_.best(Side::Sell), 1002000);
  EXPECT_GT(book_.stats().crossed_states, 0U);
  EXPECT_TRUE(book_.verify_aggregates())
      << "the book must remain internally consistent while crossed";
}

TEST_F(MdBookTest, ALockedBookIsAlsoTolerated) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 500));
  ASSERT_TRUE(book_.add(2, Side::Sell, 1002100, 500));
  EXPECT_TRUE(book_.is_crossed_or_locked())
      << "best bid == best ask is locked, and legal in a market data book";
  EXPECT_TRUE(book_.verify_aggregates());
}

TEST_F(MdBookTest, AnOversizedReductionIsClampedAndCounted) {
  // Should never happen on a clean feed read from the start. If it does,
  // something is wrong -- but going negative would corrupt every later
  // comparison, so it is clamped and the counter says it happened.
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 100));
  ASSERT_TRUE(book_.cancel(1, 500));
  EXPECT_EQ(book_.stats().oversized_reduction, 1U);
  EXPECT_EQ(book_.order_count(), 0U);
  EXPECT_EQ(book_.qty_at(Side::Buy, 1002100), 0U);
  EXPECT_TRUE(book_.verify_aggregates());
}

TEST_F(MdBookTest, ADuplicateReferenceIsRefusedAndCounted) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 100));
  EXPECT_FALSE(book_.add(1, Side::Sell, 1002400, 100));
  EXPECT_EQ(book_.stats().duplicate_reference, 1U);
  EXPECT_EQ(book_.order_count(), 1U);
  EXPECT_TRUE(book_.verify_aggregates());
}

// --- the depth view ---------------------------------------------------------

TEST_F(MdBookTest, TopNIsOrderedFromTheTouchOutwardsOnBothSides) {
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(book_.add(static_cast<OrderId>(100 + i), Side::Buy, 1002100 - i,
                          static_cast<Qty>(10 * (i + 1))));
    ASSERT_TRUE(book_.add(static_cast<OrderId>(200 + i), Side::Sell,
                          1002400 + i, static_cast<Qty>(20 * (i + 1))));
  }
  const auto bids = book_.top_n(Side::Buy, 5);
  EXPECT_EQ(bids[0].price, 1002100);
  EXPECT_EQ(bids[1].price, 1002099);
  EXPECT_EQ(bids[4].price, 1002096);
  EXPECT_EQ(bids[0].qty, 10U);
  EXPECT_EQ(bids[4].qty, 50U);

  const auto asks = book_.top_n(Side::Sell, 5);
  EXPECT_EQ(asks[0].price, 1002400);
  EXPECT_EQ(asks[4].price, 1002404);
  EXPECT_EQ(asks[0].qty, 20U);
}

TEST_F(MdBookTest, TopNPadsAThinBookWithEmptyLevels) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 100));
  const auto bids = book_.top_n(Side::Buy, 10);
  ASSERT_EQ(bids.size(), 10U);
  EXPECT_EQ(bids[0].price, 1002100);
  for (std::size_t i = 1; i < 10; ++i) {
    EXPECT_EQ(bids[i].price, kInvalidPrice) << "level " << i;
    EXPECT_EQ(bids[i].qty, 0U);
  }
}

TEST_F(MdBookTest, OrderCountsPerLevelAreMaintained) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 100));
  ASSERT_TRUE(book_.add(2, Side::Buy, 1002100, 100));
  ASSERT_TRUE(book_.add(3, Side::Buy, 1002100, 100));
  EXPECT_EQ(book_.top_n(Side::Buy, 1)[0].orders, 3U);
  ASSERT_TRUE(book_.remove(2));
  EXPECT_EQ(book_.top_n(Side::Buy, 1)[0].orders, 2U);
  EXPECT_EQ(book_.top_n(Side::Buy, 1)[0].qty, 200U);
  EXPECT_TRUE(book_.verify_aggregates());
}

// --- churn ------------------------------------------------------------------

TEST_F(MdBookTest, AggregatesSurviveHeavyChurn) {
  std::uint64_t state = 12345;
  const auto next = [&state] {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::uint32_t>(state >> 33);
  };

  std::vector<OrderId> live;
  OrderId next_ref = 1;
  for (int step = 0; step < 50000; ++step) {
    const std::uint32_t roll = next() % 100;
    if (roll < 45 || live.empty()) {
      const Side side = (next() % 2) ? Side::Buy : Side::Sell;
      const Price price = 1002000 + static_cast<Price>(next() % 200) - 100;
      if (book_.add(next_ref, side, price, static_cast<Qty>(1 + next() % 500)))
        live.push_back(next_ref);
      ++next_ref;
    } else {
      const std::size_t k = next() % live.size();
      const OrderId ref = live[k];
      if (roll < 60) {
        (void)book_.cancel(ref, static_cast<Qty>(1 + next() % 100));
      } else if (roll < 75) {
        (void)book_.execute(ref, static_cast<Qty>(1 + next() % 100));
      } else if (roll < 90) {
        (void)book_.remove(ref);
        live[k] = live.back();
        live.pop_back();
      } else {
        (void)book_.replace(ref, next_ref, static_cast<Qty>(1 + next() % 500),
                            1002000 + static_cast<Price>(next() % 200) - 100);
        live[k] = next_ref;
        ++next_ref;
      }
    }
    if (step % 997 == 0)
      ASSERT_TRUE(book_.verify_aggregates()) << "step " << step;
  }
  EXPECT_TRUE(book_.verify_aggregates());
  EXPECT_GT(book_.stats().adds, 1000U);
}

TEST_F(MdBookTest, ClearResetsEverything) {
  ASSERT_TRUE(book_.add(1, Side::Buy, 1002100, 100));
  book_.hidden_print(1002100, 50);
  book_.clear();
  EXPECT_EQ(book_.order_count(), 0U);
  EXPECT_EQ(book_.best(Side::Buy), kInvalidPrice);
  EXPECT_EQ(book_.stats().adds, 0U);
  EXPECT_EQ(book_.stats().printable_volume, 0U);
  EXPECT_TRUE(book_.verify_aggregates());
}

} // namespace
} // namespace miniex
