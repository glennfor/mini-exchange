#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include "core/config.hpp"
#include "core/types.hpp"

namespace miniex {
namespace {

TEST(Side, OppositeIsAnInvolution) {
  EXPECT_EQ(opposite(Side::Buy), Side::Sell);
  EXPECT_EQ(opposite(Side::Sell), Side::Buy);
  EXPECT_EQ(opposite(opposite(Side::Buy)), Side::Buy);
  EXPECT_EQ(opposite(opposite(Side::Sell)), Side::Sell);
}

TEST(Side, UsableAsAnArrayIndexWithoutBranching) {
  int counters[2] = {0, 0};
  ++counters[index_of(Side::Buy)];
  ++counters[index_of(Side::Sell)];
  ++counters[index_of(Side::Sell)];
  EXPECT_EQ(counters[0], 1);
  EXPECT_EQ(counters[1], 2);
  EXPECT_EQ(side_char(Side::Buy), 'B');
  EXPECT_EQ(side_char(Side::Sell), 'S');
}

TEST(Price, SentinelsAreDistinctAndOrdered) {
  EXPECT_NE(kInvalidPrice, kMarketSellPrice);
  EXPECT_NE(kInvalidPrice, kMarketBuyPrice);
  EXPECT_LT(kMarketSellPrice, kMarketBuyPrice);
  // A market buy must be willing to cross any real ask, and a market sell
  // must be willing to hit any real bid.
  EXPECT_TRUE(crosses(Side::Buy, kMarketBuyPrice, 1002100));
  EXPECT_TRUE(crosses(Side::Sell, kMarketSellPrice, 1002100));
}

TEST(Price, ScaleMatchesFourImpliedDecimals) {
  EXPECT_EQ(kPriceScale, 10000);
  // $100.2100 in ticks.
  EXPECT_EQ(Price{1002100}, 100 * kPriceScale + 2100);
}

TEST(Price, BetterPriceFollowsSideConvention) {
  EXPECT_TRUE(better_price(Side::Buy, 1002200, 1002100));
  EXPECT_FALSE(better_price(Side::Buy, 1002100, 1002200));
  EXPECT_TRUE(better_price(Side::Sell, 1002100, 1002200));
  EXPECT_FALSE(better_price(Side::Sell, 1002200, 1002100));
  EXPECT_FALSE(better_price(Side::Buy, 1002100, 1002100));
  EXPECT_FALSE(better_price(Side::Sell, 1002100, 1002100));
}

TEST(Price, CrossesIsInclusiveAtEqualPrices) {
  EXPECT_TRUE(crosses(Side::Buy, 1002100, 1002100));
  EXPECT_TRUE(crosses(Side::Buy, 1002200, 1002100));
  EXPECT_FALSE(crosses(Side::Buy, 1002000, 1002100));
  EXPECT_TRUE(crosses(Side::Sell, 1002100, 1002100));
  EXPECT_TRUE(crosses(Side::Sell, 1002000, 1002100));
  EXPECT_FALSE(crosses(Side::Sell, 1002200, 1002100));
}

TEST(Layout, HotPathStructsHaveTheDocumentedSize) {
  EXPECT_EQ(sizeof(Order), 64U);
  EXPECT_EQ(alignof(Order), 64U);
  EXPECT_TRUE(std::is_trivially_copyable_v<Order>);
}

TEST(Layout, NoFloatingPointInTheValueTypes) {
  EXPECT_TRUE(std::is_integral_v<Price>);
  EXPECT_TRUE(std::is_integral_v<Qty>);
  EXPECT_TRUE(std::is_integral_v<TsNanos>);
  EXPECT_TRUE(std::is_signed_v<Price>);
}

TEST(RejectReasonNames, EveryEnumeratorHasAName) {
  // Exhaustive: if a reason is added without a name, this fails.
  const RejectReason all[] = {
      RejectReason::None,
      RejectReason::DuplicateOrderId,
      RejectReason::UnknownOrder,
      RejectReason::TooLateToCancel,
      RejectReason::ZeroQuantity,
      RejectReason::InvalidPrice,
      RejectReason::PriceOutOfBand,
      RejectReason::PoolExhausted,
      RejectReason::FokUnfillable,
      RejectReason::PostOnlyWouldCross,
      RejectReason::MarketOrderNotFillable,
      RejectReason::MarketDayNotSupported,
      RejectReason::SelfTradePrevented,
  };
  for (RejectReason r : all) {
    EXPECT_STRNE(to_string(r), "Unknown");
  }
  EXPECT_STREQ(to_string(RejectReason::FokUnfillable), "FokUnfillable");
  EXPECT_STREQ(to_string(OrderType::Market), "Market");
  EXPECT_STREQ(to_string(TimeInForce::PostOnly), "PostOnly");
}

TEST(Config, SizingConstantsArePowersOfTwo) {
  EXPECT_EQ(kMaxOrders & (kMaxOrders - 1), 0U);
  EXPECT_EQ(kLadderTicks & (kLadderTicks - 1), 0U);
  EXPECT_EQ(kRingCapacity & (kRingCapacity - 1), 0U);
  EXPECT_GE(kTickSize, 1);
}

} // namespace
} // namespace miniex
