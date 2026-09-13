// FastBook is checked two ways: directly, for the things only it has (the
// ladder window, the overflow path, re-centring, zero allocation), and
// differentially against NaiveBook, which is where the real confidence comes
// from.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "book/fast_book.hpp"
#include "book/naive_book.hpp"
#include "replay/random.hpp"

namespace miniex {
namespace {

FastBook::Config small_config(Price centre = 1002000) {
  FastBook::Config cfg;
  cfg.max_orders = 1 << 14;
  cfg.centre_price = centre;
  return cfg;
}

class FastBookTest : public ::testing::Test {
protected:
  FastBookTest() : book_(small_config()) {}

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
  FastBook book_;
  TsNanos ts_{0};
};

// --- the basics, mirroring naive_book_test.cpp ------------------------------

TEST_F(FastBookTest, EmptyBookReportsNoPrices) {
  EXPECT_EQ(book_.best(Side::Buy), kInvalidPrice);
  EXPECT_EQ(book_.best(Side::Sell), kInvalidPrice);
  EXPECT_EQ(book_.qty_at(Side::Buy, 1002000), 0U);
  EXPECT_EQ(book_.order_count(), 0U);
  EXPECT_EQ(book_.front(Side::Buy), nullptr);
  EXPECT_EQ(book_.find(1), nullptr);
  EXPECT_TRUE(book_.verify_aggregates());
}

TEST_F(FastBookTest, BestIsTheHighestBidAndTheLowestAsk) {
  add(1, Side::Buy, 1002100, 100);
  add(2, Side::Buy, 1002300, 100);
  add(3, Side::Buy, 1002200, 100);
  add(4, Side::Sell, 1002600, 100);
  add(5, Side::Sell, 1002400, 100);
  EXPECT_EQ(book_.best(Side::Buy), 1002300);
  EXPECT_EQ(book_.best(Side::Sell), 1002400);
  EXPECT_TRUE(book_.verify_aggregates());
}

TEST_F(FastBookTest, AddsAppendAtTheTailAndFillConsumesFromTheHead) {
  add(1, Side::Sell, 1002400, 10);
  add(2, Side::Sell, 1002400, 20);
  add(3, Side::Sell, 1002400, 30);
  EXPECT_EQ(queue_at(Side::Sell, 1002400), (std::vector<OrderId>{1, 2, 3}));
  EXPECT_EQ(book_.qty_at(Side::Sell, 1002400), 60U);
  EXPECT_EQ(book_.orders_at(Side::Sell, 1002400), 3U);

  book_.fill(book_.front(Side::Sell), 4);
  EXPECT_EQ(book_.front(Side::Sell)->id, 1U);
  EXPECT_EQ(book_.qty_at(Side::Sell, 1002400), 56U);
  book_.fill(book_.front(Side::Sell), 6);
  EXPECT_EQ(book_.front(Side::Sell)->id, 2U);
  EXPECT_EQ(book_.order_count(), 2U);
  EXPECT_TRUE(book_.verify_aggregates());
}

TEST_F(FastBookTest, CancelIsPositionIndependent) {
  for (OrderId id = 1; id <= 5; ++id) add(id, Side::Buy, 1002100, 10);
  EXPECT_TRUE(book_.cancel(3)); // middle
  EXPECT_EQ(queue_at(Side::Buy, 1002100), (std::vector<OrderId>{1, 2, 4, 5}));
  EXPECT_TRUE(book_.cancel(1)); // head
  EXPECT_EQ(queue_at(Side::Buy, 1002100), (std::vector<OrderId>{2, 4, 5}));
  EXPECT_TRUE(book_.cancel(5)); // tail
  EXPECT_EQ(queue_at(Side::Buy, 1002100), (std::vector<OrderId>{2, 4}));
  EXPECT_EQ(book_.qty_at(Side::Buy, 1002100), 20U);
  EXPECT_FALSE(book_.cancel(3));
  EXPECT_TRUE(book_.verify_aggregates());
}

TEST_F(FastBookTest, EmptyingALevelRetiresIt) {
  add(1, Side::Buy, 1002100, 10);
  add(2, Side::Buy, 1002000, 10);
  EXPECT_EQ(book_.level_count(Side::Buy), 2U);
  ASSERT_TRUE(book_.cancel(1));
  EXPECT_EQ(book_.level_count(Side::Buy), 1U);
  EXPECT_EQ(book_.best(Side::Buy), 1002000)
      << "the bitset must find the next occupied level, not the emptied one";
  ASSERT_TRUE(book_.cancel(2));
  EXPECT_EQ(book_.best(Side::Buy), kInvalidPrice);
  EXPECT_EQ(book_.level_count(Side::Buy), 0U);
}

TEST_F(FastBookTest, NextPriceWalksAwayFromTheTouch) {
  add(1, Side::Buy, 1002100, 10);
  add(2, Side::Buy, 1002200, 10);
  add(3, Side::Buy, 1002300, 10);
  EXPECT_EQ(book_.next_price(Side::Buy, 1002300), 1002200);
  EXPECT_EQ(book_.next_price(Side::Buy, 1002200), 1002100);
  EXPECT_EQ(book_.next_price(Side::Buy, 1002100), kInvalidPrice);

  add(4, Side::Sell, 1002400, 10);
  add(5, Side::Sell, 1002600, 10);
  EXPECT_EQ(book_.next_price(Side::Sell, 1002400), 1002600);
  EXPECT_EQ(book_.next_price(Side::Sell, 1002600), kInvalidPrice);
}

TEST_F(FastBookTest, ReduceAndQtyAtExcluding) {
  add(1, Side::Sell, 1002400, 100, 7);
  add(2, Side::Sell, 1002400, 50, 9);
  EXPECT_TRUE(book_.reduce(1, 40));
  EXPECT_EQ(book_.qty_at(Side::Sell, 1002400), 110U);
  EXPECT_EQ(book_.qty_at_excluding(Side::Sell, 1002400, 7), 50U);
  EXPECT_FALSE(book_.reduce(1, 1000));
  EXPECT_TRUE(book_.reduce(1, 60));
  EXPECT_EQ(book_.orders_at(Side::Sell, 1002400), 1U);
  EXPECT_TRUE(book_.verify_aggregates());
}

TEST_F(FastBookTest, PoolExhaustionIsReportedNotFatal) {
  FastBook::Config cfg;
  cfg.max_orders = 8;
  cfg.centre_price = 1002000;
  FastBook book(cfg);
  for (OrderId id = 1; id <= 8; ++id)
    ASSERT_TRUE(book.add(id, Side::Buy, 1002000 - static_cast<Price>(id), 10,
                         static_cast<TsNanos>(id), 0));
  EXPECT_FALSE(book.can_add());
  EXPECT_FALSE(book.add(9, Side::Buy, 1001000, 10, 9, 0));
  EXPECT_EQ(book.order_count(), 8U);
  ASSERT_TRUE(book.cancel(1));
  EXPECT_TRUE(book.can_add());
  EXPECT_TRUE(book.add(9, Side::Buy, 1001000, 10, 9, 0));
}

TEST_F(FastBookTest, DuplicateIdIsRefusedAndLeavesNoLeakedSlot) {
  add(1, Side::Buy, 1002100, 10);
  const std::size_t free_before = book_.pool().free_count();
  EXPECT_FALSE(book_.add(1, Side::Buy, 1002200, 10, ++ts_, 0));
  EXPECT_EQ(book_.pool().free_count(), free_before)
      << "a refused add must not consume a pool slot";
  EXPECT_EQ(book_.order_count(), 1U);
}

// --- the ladder window and its overflow -------------------------------------

TEST_F(FastBookTest, PricesOutsideTheWindowUseTheOverflowPathAndStillWork) {
  const Price base = book_.ladder_base();
  const Price above = base + static_cast<Price>(FastBook::ladder_width()) + 500;
  const Price below = base - 500;

  add(1, Side::Sell, above, 10);
  add(2, Side::Buy, below, 20);
  EXPECT_EQ(book_.overflow_order_count(), 2U);
  EXPECT_EQ(book_.best(Side::Sell), above);
  EXPECT_EQ(book_.best(Side::Buy), below);
  EXPECT_EQ(book_.qty_at(Side::Sell, above), 10U);
  EXPECT_TRUE(book_.verify_aggregates());

  ASSERT_TRUE(book_.cancel(1));
  EXPECT_EQ(book_.overflow_order_count(), 1U);
  EXPECT_EQ(book_.best(Side::Sell), kInvalidPrice);
}

TEST_F(FastBookTest, BestAndNextPriceMergeResidentAndOverflowLevels) {
  const Price base = book_.ladder_base();
  const Price far_below = base - 1000;  // overflow, worst bid
  const Price resident_low = base + 10; // resident
  const Price resident_high = 1002100;  // resident, best bid

  add(1, Side::Buy, far_below, 10);
  add(2, Side::Buy, resident_low, 10);
  add(3, Side::Buy, resident_high, 10);

  EXPECT_EQ(book_.best(Side::Buy), resident_high);
  EXPECT_EQ(book_.next_price(Side::Buy, resident_high), resident_low);
  EXPECT_EQ(book_.next_price(Side::Buy, resident_low), far_below);
  EXPECT_EQ(book_.next_price(Side::Buy, far_below), kInvalidPrice);

  // And the same on the ask side, where "worse" means higher.
  const Price far_above =
      base + static_cast<Price>(FastBook::ladder_width()) + 1000;
  add(4, Side::Sell, far_above, 10);
  add(5, Side::Sell, 1002500, 10);
  EXPECT_EQ(book_.best(Side::Sell), 1002500);
  EXPECT_EQ(book_.next_price(Side::Sell, 1002500), far_above);
  EXPECT_EQ(book_.next_price(Side::Sell, far_above), kInvalidPrice);
  EXPECT_TRUE(book_.verify_aggregates());
}

TEST_F(FastBookTest, LevelIterationCoversOverflowAndResidentInPriceOrder) {
  const Price base = book_.ladder_base();
  add(1, Side::Buy, base - 50, 10);
  add(2, Side::Buy, 1002000, 10);
  add(3, Side::Buy, 1002200, 10);
  std::vector<Price> prices;
  book_.for_each_level(
      Side::Buy, [&](Price p, AggQty, std::size_t) { prices.push_back(p); });
  EXPECT_EQ(prices, (std::vector<Price>{1002200, 1002000, base - 50}));
}

// --- re-centring ------------------------------------------------------------

TEST(FastBookRecentre, MovesTheWindowWithoutTouchingAnyOrder) {
  FastBook::Config cfg;
  cfg.max_orders = 1 << 12;
  cfg.centre_price = 1000000;
  cfg.edge_fraction = 0; // manual control for this test
  FastBook book(cfg);

  const Price base_before = book.ladder_base();
  for (OrderId id = 1; id <= 50; ++id)
    ASSERT_TRUE(book.add(id, Side::Buy, 1000000 - static_cast<Price>(id), 10,
                         static_cast<TsNanos>(id), 0));
  // One order deliberately outside the window, so the migration back in is
  // exercised too.
  const Price far = base_before - 1000;
  ASSERT_TRUE(book.add(99, Side::Buy, far, 10, 99, 0));
  ASSERT_EQ(book.overflow_order_count(), 1U);

  // Re-centre between the two clusters, so the window covers both: the
  // formerly-outside order migrates in and nothing migrates out.
  book.recentre(far + (1000000 - far) / 2);

  EXPECT_NE(book.ladder_base(), base_before);
  EXPECT_EQ(book.recentre_count(), 1U);
  EXPECT_EQ(book.order_count(), 51U);
  EXPECT_EQ(book.overflow_order_count(), 0U)
      << "the formerly-outside order is now inside the window";
  EXPECT_EQ(book.best(Side::Buy), 999999);
  EXPECT_EQ(book.qty_at(Side::Buy, far), 10U);
  EXPECT_TRUE(book.verify_aggregates());

  // Everything is still findable and still in FIFO order.
  for (OrderId id = 1; id <= 50; ++id) {
    const ConstOrderHandle h = book.find(id);
    ASSERT_NE(h, nullptr) << "order " << id;
    EXPECT_EQ(h->price, 1000000 - static_cast<Price>(id));
  }
}

TEST(FastBookRecentre, PushesOrdersOutOfTheWindowIntoOverflowWhenNeeded) {
  FastBook::Config cfg;
  cfg.max_orders = 1 << 12;
  cfg.centre_price = 1000000;
  cfg.edge_fraction = 0;
  FastBook book(cfg);
  ASSERT_TRUE(book.add(1, Side::Buy, 1000000, 10, 1, 0));
  EXPECT_EQ(book.overflow_order_count(), 0U);

  // Move the window a long way up; the old order falls out of the bottom.
  book.recentre(1000000 + static_cast<Price>(FastBook::ladder_width()) * 2);
  EXPECT_EQ(book.overflow_order_count(), 1U);
  EXPECT_EQ(book.best(Side::Buy), 1000000);
  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_TRUE(book.verify_aggregates());
}

TEST(FastBookRecentre, FiresAutomaticallyWhenTheTouchDriftsToTheEdge) {
  FastBook::Config cfg;
  cfg.max_orders = 1 << 14;
  cfg.centre_price = 1000000;
  cfg.edge_fraction = 4;
  cfg.recentre_cooldown = 1;
  FastBook book(cfg);

  ASSERT_TRUE(book.add(1, Side::Buy, 1000000, 10, 1, 0));
  EXPECT_EQ(book.recentre_count(), 0U);

  // Walk the touch out past the outer quarter of the window.
  const Price far =
      book.ladder_base() + static_cast<Price>(FastBook::ladder_width()) - 100;
  ASSERT_TRUE(book.add(2, Side::Buy, far, 10, 2, 0));
  EXPECT_GE(book.recentre_count(), 1U);
  EXPECT_EQ(book.best(Side::Buy), far);
  EXPECT_EQ(book.order_count(), 2U);
  EXPECT_TRUE(book.verify_aggregates());
}

TEST(FastBookRecentre, CooldownStopsAnEdgeStraddlingBookFromThrashing) {
  FastBook::Config cfg;
  cfg.max_orders = 1 << 14;
  cfg.centre_price = 1000000;
  cfg.edge_fraction = 4;
  cfg.recentre_cooldown = 1000;
  FastBook book(cfg);

  const Price low = book.ladder_base() + 10;
  const Price high =
      book.ladder_base() + static_cast<Price>(FastBook::ladder_width()) - 10;
  for (OrderId id = 1; id <= 200; ++id) {
    const Price p = (id % 2) ? low : high;
    (void)book.add(id, Side::Buy, p, 10, static_cast<TsNanos>(id), 0);
    (void)book.cancel(id);
  }
  EXPECT_LE(book.recentre_count(), 1U)
      << "the cooldown exists so that this cannot re-centre 200 times";
}

TEST(FastBookLadder, BaseIsChosenFromTheFirstPriceWhenNotConfigured) {
  FastBook::Config cfg;
  cfg.max_orders = 64;
  cfg.centre_price = 0; // decide lazily
  FastBook book(cfg);
  ASSERT_TRUE(book.add(1, Side::Buy, 5000000, 10, 1, 0));
  EXPECT_EQ(book.ladder_base(),
            5000000 - static_cast<Price>(FastBook::ladder_width() / 2));
  EXPECT_EQ(book.overflow_order_count(), 0U)
      << "the first order must land inside the window it just defined";
}

// --- differential against NaiveBook -----------------------------------------
//
// The real test. Two independent implementations of the same mechanism must
// agree on every observable, after every operation.

struct BookPair {
  NaiveBook naive;
  FastBook fast{small_config()};
};

::testing::AssertionResult agree(const NaiveBook& naive, const FastBook& fast,
                                 std::size_t step) {
  if (naive.order_count() != fast.order_count())
    return ::testing::AssertionFailure()
           << "step " << step << ": order_count " << naive.order_count()
           << " vs " << fast.order_count();
  for (const Side side : {Side::Buy, Side::Sell}) {
    std::vector<Price> naive_prices;
    std::vector<AggQty> naive_qty;
    std::vector<std::size_t> naive_counts;
    naive.for_each_level(side, [&](Price p, AggQty q, std::size_t n) {
      naive_prices.push_back(p);
      naive_qty.push_back(q);
      naive_counts.push_back(n);
    });
    std::vector<Price> fast_prices;
    std::vector<AggQty> fast_qty;
    std::vector<std::size_t> fast_counts;
    fast.for_each_level(side, [&](Price p, AggQty q, std::size_t n) {
      fast_prices.push_back(p);
      fast_qty.push_back(q);
      fast_counts.push_back(n);
    });
    if (naive_prices != fast_prices)
      return ::testing::AssertionFailure()
             << "step " << step << ": level prices differ on side "
             << side_char(side) << " (" << naive_prices.size() << " vs "
             << fast_prices.size() << ")";
    if (naive_qty != fast_qty)
      return ::testing::AssertionFailure()
             << "step " << step << ": level quantities differ on side "
             << side_char(side);
    if (naive_counts != fast_counts)
      return ::testing::AssertionFailure()
             << "step " << step << ": level order counts differ on side "
             << side_char(side);

    // And the FIFO order within every level, which is the property that
    // decides who gets filled.
    for (Price p : naive_prices) {
      std::vector<OrderId> a;
      std::vector<OrderId> b;
      naive.for_each_order(side, p, [&](const Order& o) { a.push_back(o.id); });
      fast.for_each_order(side, p, [&](const Order& o) { b.push_back(o.id); });
      if (a != b)
        return ::testing::AssertionFailure()
               << "step " << step << ": FIFO order differs at price " << p;
    }
  }
  return ::testing::AssertionSuccess();
}

TEST(FastBookDifferential, AgreesWithNaiveBookUnderRandomChurn) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    BookPair books;
    Rng rng(seed);
    std::vector<OrderId> live;
    OrderId next_id = 1;
    const Price centre = 1002000;

    for (std::size_t step = 0; step < 20000; ++step) {
      const std::uint32_t roll = rng.below(100);
      if (roll < 45 || live.empty()) {
        const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
        // Mostly near the touch, sometimes deliberately outside the window so
        // the overflow path is part of the comparison rather than a footnote.
        Price price;
        if (rng.chance(4)) {
          price = centre + (rng.chance(50) ? 1 : -1) *
                               static_cast<Price>(FastBook::ladder_width() / 2 +
                                                  1 + rng.below(5000));
        } else {
          price = centre + static_cast<Price>(rng.below(200)) - 100;
        }
        const Qty qty = static_cast<Qty>(1 + rng.below(500));
        const auto participant = static_cast<ParticipantId>(rng.below(4));
        const auto ts = static_cast<TsNanos>(step);
        const bool a =
            books.naive.add(next_id, side, price, qty, ts, participant);
        const bool b =
            books.fast.add(next_id, side, price, qty, ts, participant);
        ASSERT_EQ(a, b) << "seed " << seed << " step " << step;
        if (a) live.push_back(next_id);
        ++next_id;
      } else if (roll < 75) {
        const std::size_t k =
            rng.below(static_cast<std::uint32_t>(live.size()));
        const OrderId id = live[k];
        live[k] = live.back();
        live.pop_back();
        ASSERT_EQ(books.naive.cancel(id), books.fast.cancel(id));
      } else if (roll < 90) {
        const std::size_t k =
            rng.below(static_cast<std::uint32_t>(live.size()));
        const OrderId id = live[k];
        const Qty by = static_cast<Qty>(1 + rng.below(200));
        ASSERT_EQ(books.naive.reduce(id, by), books.fast.reduce(id, by))
            << "seed " << seed << " step " << step;
      } else {
        const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
        OrderHandle na = books.naive.front(side);
        OrderHandle fa = books.fast.front(side);
        ASSERT_EQ(na == nullptr, fa == nullptr);
        if (na == nullptr) continue;
        ASSERT_EQ(na->id, fa->id) << "seed " << seed << " step " << step;
        const Qty fill = std::min<Qty>(na->qty_open, 1 + rng.below(300));
        books.naive.fill(na, fill);
        books.fast.fill(fa, fill);
      }

      if (step % 97 == 0) {
        ASSERT_TRUE(agree(books.naive, books.fast, step)) << "seed " << seed;
        ASSERT_TRUE(books.fast.verify_aggregates())
            << "seed " << seed << " step " << step;
      }
    }
    ASSERT_TRUE(agree(books.naive, books.fast, 20000)) << "seed " << seed;
  }
}

} // namespace
} // namespace miniex
