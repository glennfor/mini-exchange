// Hand-laid-out ITCH 5.0 byte fixtures, and the tests that read them.
//
// **These fixtures were written from the field layouts, not produced by this
// project's encoder.** That matters: if the fixtures came from the encoder,
// a shared misunderstanding of an offset would cancel out and the test would
// pass while the decoder read the wrong bytes. Each array below is annotated
// field by field with the value it is supposed to carry, and each test
// asserts every decoded field against that value.
//
// The test-only encoder in itch_encoder.hpp is checked *against* these
// fixtures first, and only then used for the round-trip property test.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "feed/decoder.hpp"
#include "feed/itch50.hpp"
#include "feed/normalizer.hpp"

#include "itch_encoder.hpp"

namespace miniex {
namespace {

/// The timestamp every fixture carries: 34,200,123,456,789 ns after midnight
/// Eastern, i.e. 09:30:00.123456789 -- the first instant of continuous
/// trading, which is where the interesting messages are.
constexpr TsNanos kFixtureTs = 34200123456789;

const std::byte* as_bytes(const std::uint8_t* p) {
  return reinterpret_cast<const std::byte*>(p);
}

// 'A' Add Order, 36 bytes.
//   type='A'  locate=1  tracking=2  ts=34200123456789
//   order_ref=4294967373  side='B'  shares=1500  stock="AAPL    "
//   price=1002100 ($100.2100)
constexpr std::uint8_t kAddOrder[] = {
    0x41, 0x00, 0x01, 0x00, 0x02, 0x1F, 0x1A, 0xD6, 0x35, 0xBD, 0x15, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4D, 0x42, 0x00, 0x00, 0x05, 0xDC,
    0x41, 0x41, 0x50, 0x4C, 0x20, 0x20, 0x20, 0x20, 0x00, 0x0F, 0x4A, 0x74,
};
static_assert(sizeof(kAddOrder) == 36);

// 'F' Add Order with MPID, 40 bytes: 'A' plus a 4-byte attribution.
//   locate=3  tracking=4  ts=+1  order_ref=987654321  side='S'
//   shares=200  stock="MSFT    "  price=3155000  attribution="NSDQ"
constexpr std::uint8_t kAddOrderMpid[] = {
    0x46, 0x00, 0x03, 0x00, 0x04, 0x1F, 0x1A, 0xD6, 0x35, 0xBD,
    0x16, 0x00, 0x00, 0x00, 0x00, 0x3A, 0xDE, 0x68, 0xB1, 0x53,
    0x00, 0x00, 0x00, 0xC8, 0x4D, 0x53, 0x46, 0x54, 0x20, 0x20,
    0x20, 0x20, 0x00, 0x30, 0x24, 0x38, 0x4E, 0x53, 0x44, 0x51,
};
static_assert(sizeof(kAddOrderMpid) == 40);

// 'E' Order Executed, 31 bytes.
//   locate=1  tracking=5  ts=+2  order_ref=4294967373
//   executed_shares=500  match_number=90001
constexpr std::uint8_t kOrderExecuted[] = {
    0x45, 0x00, 0x01, 0x00, 0x05, 0x1F, 0x1A, 0xD6, 0x35, 0xBD, 0x17,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4D, 0x00, 0x00, 0x01,
    0xF4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5F, 0x91,
};
static_assert(sizeof(kOrderExecuted) == 31);

// 'C' Order Executed with Price, 36 bytes.
//   locate=1  tracking=6  ts=+3  order_ref=4294967373
//   executed_shares=250  match_number=90002  printable='N'  price=1002050
// printable='N' is deliberate: a non-printable execution still changes the
// book but is excluded from volume and last-price statistics.
constexpr std::uint8_t kOrderExecutedWithPrice[] = {
    0x43, 0x00, 0x01, 0x00, 0x06, 0x1F, 0x1A, 0xD6, 0x35, 0xBD, 0x18, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4D, 0x00, 0x00, 0x00, 0xFA, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x5F, 0x92, 0x4E, 0x00, 0x0F, 0x4A, 0x42,
};
static_assert(sizeof(kOrderExecutedWithPrice) == 36);

// 'X' Order Cancel, 23 bytes.
//   locate=1  tracking=7  ts=+4  order_ref=4294967373  cancelled_shares=100
constexpr std::uint8_t kOrderCancel[] = {
    0x58, 0x00, 0x01, 0x00, 0x07, 0x1F, 0x1A, 0xD6, 0x35, 0xBD, 0x19, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4D, 0x00, 0x00, 0x00, 0x64,
};
static_assert(sizeof(kOrderCancel) == 23);

// 'D' Order Delete, 19 bytes.
//   locate=1  tracking=8  ts=+5  order_ref=4294967373
constexpr std::uint8_t kOrderDelete[] = {
    0x44, 0x00, 0x01, 0x00, 0x08, 0x1F, 0x1A, 0xD6, 0x35, 0xBD,
    0x1A, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4D,
};
static_assert(sizeof(kOrderDelete) == 19);

// 'U' Order Replace, 35 bytes.
//   locate=1  tracking=9  ts=+6  original_ref=4294967373
//   new_ref=4294967374  shares=650  price=1002400
constexpr std::uint8_t kOrderReplace[] = {
    0x55, 0x00, 0x01, 0x00, 0x09, 0x1F, 0x1A, 0xD6, 0x35, 0xBD, 0x1B, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4D, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x4E, 0x00, 0x00, 0x02, 0x8A, 0x00, 0x0F, 0x4B, 0xA0,
};
static_assert(sizeof(kOrderReplace) == 35);

// 'P' Trade (non-cross), 44 bytes.
//   locate=1  tracking=10  ts=+7  order_ref=0  side='S'  shares=300
//   stock="AAPL    "  price=1002200  match_number=90003
constexpr std::uint8_t kTrade[] = {
    0x50, 0x00, 0x01, 0x00, 0x0A, 0x1F, 0x1A, 0xD6, 0x35, 0xBD, 0x1C,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x00, 0x00,
    0x01, 0x2C, 0x41, 0x41, 0x50, 0x4C, 0x20, 0x20, 0x20, 0x20, 0x00,
    0x0F, 0x4A, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5F, 0x93,
};
static_assert(sizeof(kTrade) == 44);

// 'Q' Cross Trade, 40 bytes. Note shares is EIGHT bytes here, not four.
//   locate=1  tracking=11  ts=+8  shares=5000000  stock="AAPL    "
//   cross_price=1002000  match_number=90004  cross_type='O' (opening)
constexpr std::uint8_t kCrossTrade[] = {
    0x51, 0x00, 0x01, 0x00, 0x0B, 0x1F, 0x1A, 0xD6, 0x35, 0xBD,
    0x1D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x4B, 0x40, 0x41,
    0x41, 0x50, 0x4C, 0x20, 0x20, 0x20, 0x20, 0x00, 0x0F, 0x4A,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5F, 0x94, 0x4F,
};
static_assert(sizeof(kCrossTrade) == 40);

// 'B' Broken Trade, 19 bytes.
//   locate=1  tracking=12  ts=+9  match_number=90003 (breaks the 'P' above)
constexpr std::uint8_t kBrokenTrade[] = {
    0x42, 0x00, 0x01, 0x00, 0x0C, 0x1F, 0x1A, 0xD6, 0x35, 0xBD,
    0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5F, 0x93,
};
static_assert(sizeof(kBrokenTrade) == 19);

// 'S' System Event, 12 bytes.
//   locate=0  tracking=13  ts=+10  event_code='Q' (start of market hours)
constexpr std::uint8_t kSystemEvent[] = {
    0x53, 0x00, 0x00, 0x00, 0x0D, 0x1F, 0x1A, 0xD6, 0x35, 0xBD, 0x1F, 0x51,
};
static_assert(sizeof(kSystemEvent) == 12);

// 'R' Stock Directory, 39 bytes.
//   locate=1  tracking=14  ts=+11  stock="AAPL    "  market_category='Q'
//   financial_status='N'  round_lot_size=100  round_lots_only='N'
//   issue_classification='C'  issue_subtype="  "  authenticity='P'
//   short_sale_threshold='N'  ipo_flag='N'  luld_tier='1'  etp_flag='N'
//   etp_leverage=0  inverse='N'
constexpr std::uint8_t kStockDirectory[] = {
    0x52, 0x00, 0x01, 0x00, 0x0E, 0x1F, 0x1A, 0xD6, 0x35, 0xBD,
    0x20, 0x41, 0x41, 0x50, 0x4C, 0x20, 0x20, 0x20, 0x20, 0x51,
    0x4E, 0x00, 0x00, 0x00, 0x64, 0x4E, 0x43, 0x20, 0x20, 0x50,
    0x4E, 0x4E, 0x31, 0x4E, 0x00, 0x00, 0x00, 0x00, 0x4E,
};
static_assert(sizeof(kStockDirectory) == 39);

// 'H' Stock Trading Action, 25 bytes.
//   locate=1  tracking=15  ts=+12  stock="AAPL    "  state='T' (trading)
//   reserved=' '  reason="    "
constexpr std::uint8_t kTradingAction[] = {
    0x48, 0x00, 0x01, 0x00, 0x0F, 0x1F, 0x1A, 0xD6, 0x35,
    0xBD, 0x21, 0x41, 0x41, 0x50, 0x4C, 0x20, 0x20, 0x20,
    0x20, 0x54, 0x20, 0x20, 0x20, 0x20, 0x20,
};
static_assert(sizeof(kTradingAction) == 25);

// ---------------------------------------------------------------------------
// Field-by-field assertions against the hand-laid-out fixtures
// ---------------------------------------------------------------------------

TEST(ItchFixtures, MessageLengthsEqualHeaderPlusFieldWidths) {
  // Re-derives every length from the field widths, so a typo in the length
  // table fails the build rather than the reconstruction. The right-hand
  // sides are the field widths in bytes, in message order.
  EXPECT_EQ(itch_message_length('S'), kItchHeaderLength + 1);
  EXPECT_EQ(itch_message_length('R'), kItchHeaderLength + 8 + 1 + 1 + 4 + 1 +
                                          1 + 2 + 1 + 1 + 1 + 1 + 1 + 4 + 1);
  EXPECT_EQ(itch_message_length('H'), kItchHeaderLength + 8 + 1 + 1 + 4);
  EXPECT_EQ(itch_message_length('Y'), kItchHeaderLength + 8 + 1);
  EXPECT_EQ(itch_message_length('L'), kItchHeaderLength + 4 + 8 + 1 + 1 + 1);
  EXPECT_EQ(itch_message_length('V'), kItchHeaderLength + 8 + 8 + 8);
  EXPECT_EQ(itch_message_length('W'), kItchHeaderLength + 1);
  EXPECT_EQ(itch_message_length('K'), kItchHeaderLength + 8 + 4 + 1 + 4);
  EXPECT_EQ(itch_message_length('J'), kItchHeaderLength + 8 + 4 + 4 + 4 + 4);
  EXPECT_EQ(itch_message_length('h'), kItchHeaderLength + 8 + 1 + 1);
  EXPECT_EQ(itch_message_length('A'), kItchHeaderLength + 8 + 1 + 4 + 8 + 4);
  EXPECT_EQ(itch_message_length('F'),
            kItchHeaderLength + 8 + 1 + 4 + 8 + 4 + 4);
  EXPECT_EQ(itch_message_length('E'), kItchHeaderLength + 8 + 4 + 8);
  EXPECT_EQ(itch_message_length('C'), kItchHeaderLength + 8 + 4 + 8 + 1 + 4);
  EXPECT_EQ(itch_message_length('X'), kItchHeaderLength + 8 + 4);
  EXPECT_EQ(itch_message_length('D'), kItchHeaderLength + 8);
  EXPECT_EQ(itch_message_length('U'), kItchHeaderLength + 8 + 8 + 4 + 4);
  EXPECT_EQ(itch_message_length('P'),
            kItchHeaderLength + 8 + 1 + 4 + 8 + 4 + 8);
  EXPECT_EQ(itch_message_length('Q'), kItchHeaderLength + 8 + 8 + 4 + 8 + 1);
  EXPECT_EQ(itch_message_length('B'), kItchHeaderLength + 8);
  EXPECT_EQ(itch_message_length('I'),
            kItchHeaderLength + 8 + 8 + 1 + 8 + 4 + 4 + 4 + 1 + 1);
  EXPECT_EQ(itch_message_length('N'), kItchHeaderLength + 8 + 1);

  EXPECT_EQ(itch_message_length('z'), 0U) << "unknown types report 0";
  EXPECT_EQ(itch_message_length('\0'), 0U);
}

TEST(ItchFixtures, EveryFixtureIsTheLengthItsTypeRequires) {
  EXPECT_EQ(sizeof(kAddOrder), itch_message_length('A'));
  EXPECT_EQ(sizeof(kAddOrderMpid), itch_message_length('F'));
  EXPECT_EQ(sizeof(kOrderExecuted), itch_message_length('E'));
  EXPECT_EQ(sizeof(kOrderExecutedWithPrice), itch_message_length('C'));
  EXPECT_EQ(sizeof(kOrderCancel), itch_message_length('X'));
  EXPECT_EQ(sizeof(kOrderDelete), itch_message_length('D'));
  EXPECT_EQ(sizeof(kOrderReplace), itch_message_length('U'));
  EXPECT_EQ(sizeof(kTrade), itch_message_length('P'));
  EXPECT_EQ(sizeof(kCrossTrade), itch_message_length('Q'));
  EXPECT_EQ(sizeof(kBrokenTrade), itch_message_length('B'));
  EXPECT_EQ(sizeof(kSystemEvent), itch_message_length('S'));
  EXPECT_EQ(sizeof(kStockDirectory), itch_message_length('R'));
  EXPECT_EQ(sizeof(kTradingAction), itch_message_length('H'));
}

TEST(ItchFixtures, HeaderIsDecodedFromEveryMessage) {
  const ItchHeader h = decode_header(as_bytes(kAddOrder));
  EXPECT_EQ(h.type, 'A');
  EXPECT_EQ(h.stock_locate, 1U);
  EXPECT_EQ(h.tracking_number, 2U);
  EXPECT_EQ(h.timestamp, kFixtureTs)
      << "the 48-bit timestamp must widen to 64 bits without sign or byte "
         "order surprises";
}

TEST(ItchFixtures, AddOrder) {
  ItchAddOrder m{};
  ASSERT_TRUE(decode_add_order(as_bytes(kAddOrder), m));
  EXPECT_EQ(m.header.type, 'A');
  EXPECT_EQ(m.header.stock_locate, 1U);
  EXPECT_EQ(m.header.tracking_number, 2U);
  EXPECT_EQ(m.header.timestamp, kFixtureTs);
  EXPECT_EQ(m.order_ref, 4294967373ULL)
      << "order reference numbers exceed 32 bits by mid-morning";
  EXPECT_EQ(m.side, Side::Buy);
  EXPECT_EQ(m.shares, 1500U);
  EXPECT_EQ(m.stock, "AAPL    ");
  EXPECT_EQ(trim_symbol(m.stock), "AAPL");
  EXPECT_EQ(m.price, 1002100) << "four implied decimals: $100.2100";
  EXPECT_EQ(m.attribution, 0U) << "'A' has no attribution field";
}

TEST(ItchFixtures, AddOrderWithMpid) {
  ItchAddOrder m{};
  ASSERT_TRUE(decode_add_order(as_bytes(kAddOrderMpid), m));
  EXPECT_EQ(m.header.type, 'F');
  EXPECT_EQ(m.header.stock_locate, 3U);
  EXPECT_EQ(m.header.timestamp, kFixtureTs + 1);
  EXPECT_EQ(m.order_ref, 987654321ULL);
  EXPECT_EQ(m.side, Side::Sell);
  EXPECT_EQ(m.shares, 200U);
  EXPECT_EQ(trim_symbol(m.stock), "MSFT");
  EXPECT_EQ(m.price, 3155000);
  // "NSDQ" big-endian.
  EXPECT_EQ(m.attribution, 0x4E534451U);
}

TEST(ItchFixtures, OrderExecuted) {
  ItchOrderExecuted m{};
  ASSERT_TRUE(decode_order_executed(as_bytes(kOrderExecuted), m));
  EXPECT_EQ(m.header.type, 'E');
  EXPECT_EQ(m.header.timestamp, kFixtureTs + 2);
  EXPECT_EQ(m.order_ref, 4294967373ULL);
  EXPECT_EQ(m.executed_shares, 500U);
  EXPECT_EQ(m.match_number, 90001ULL);
  EXPECT_FALSE(m.has_price)
      << "an 'E' carries no price; the trade prints at the resting price";
  EXPECT_TRUE(m.printable);
}

TEST(ItchFixtures, OrderExecutedWithPrice) {
  ItchOrderExecuted m{};
  ASSERT_TRUE(decode_order_executed(as_bytes(kOrderExecutedWithPrice), m));
  EXPECT_EQ(m.header.type, 'C');
  EXPECT_EQ(m.header.timestamp, kFixtureTs + 3);
  EXPECT_EQ(m.order_ref, 4294967373ULL);
  EXPECT_EQ(m.executed_shares, 250U);
  EXPECT_EQ(m.match_number, 90002ULL);
  EXPECT_TRUE(m.has_price);
  EXPECT_FALSE(m.printable)
      << "printable='N': the book changes, the statistics do not";
  EXPECT_EQ(m.price, 1002050);
}

TEST(ItchFixtures, OrderCancel) {
  ItchOrderCancel m{};
  ASSERT_TRUE(decode_order_cancel(as_bytes(kOrderCancel), m));
  EXPECT_EQ(m.header.type, 'X');
  EXPECT_EQ(m.header.timestamp, kFixtureTs + 4);
  EXPECT_EQ(m.order_ref, 4294967373ULL);
  EXPECT_EQ(m.cancelled_shares, 100U)
      << "an 'X' reduces an order; it does not necessarily remove it";
  EXPECT_FALSE(m.is_delete);
}

TEST(ItchFixtures, OrderDelete) {
  ItchOrderCancel m{};
  ASSERT_TRUE(decode_order_cancel(as_bytes(kOrderDelete), m));
  EXPECT_EQ(m.header.type, 'D');
  EXPECT_EQ(m.header.timestamp, kFixtureTs + 5);
  EXPECT_EQ(m.order_ref, 4294967373ULL);
  EXPECT_TRUE(m.is_delete);
  EXPECT_EQ(m.cancelled_shares, 0U);
}

TEST(ItchFixtures, OrderReplace) {
  ItchOrderReplace m{};
  ASSERT_TRUE(decode_order_replace(as_bytes(kOrderReplace), m));
  EXPECT_EQ(m.header.type, 'U');
  EXPECT_EQ(m.header.timestamp, kFixtureTs + 6);
  EXPECT_EQ(m.original_ref, 4294967373ULL);
  EXPECT_EQ(m.new_ref, 4294967374ULL)
      << "a replace introduces a NEW reference number; the old one is gone";
  EXPECT_EQ(m.shares, 650U);
  EXPECT_EQ(m.price, 1002400);
}

TEST(ItchFixtures, TradeNonCross) {
  ItchTrade m{};
  ASSERT_TRUE(decode_trade(as_bytes(kTrade), m));
  EXPECT_EQ(m.header.type, 'P');
  EXPECT_EQ(m.header.timestamp, kFixtureTs + 7);
  EXPECT_EQ(m.order_ref, 0ULL);
  EXPECT_EQ(m.side, Side::Sell);
  EXPECT_EQ(m.shares, 300U);
  EXPECT_EQ(trim_symbol(m.stock), "AAPL");
  EXPECT_EQ(m.price, 1002200);
  EXPECT_EQ(m.match_number, 90003ULL);
  EXPECT_FALSE(m.is_cross);
}

TEST(ItchFixtures, CrossTradeHasAnEightByteShareCount) {
  ItchTrade m{};
  ASSERT_TRUE(decode_cross_trade(as_bytes(kCrossTrade), m));
  EXPECT_EQ(m.header.type, 'Q');
  EXPECT_EQ(m.header.timestamp, kFixtureTs + 8);
  EXPECT_EQ(m.shares, 5000000U)
      << "'Q' sizes shares at 8 bytes, not 4; reading it as 4 is the classic "
         "ITCH bug";
  EXPECT_EQ(trim_symbol(m.stock), "AAPL");
  EXPECT_EQ(m.price, 1002000);
  EXPECT_EQ(m.match_number, 90004ULL);
  EXPECT_EQ(m.cross_type, 'O');
  EXPECT_TRUE(m.is_cross);
}

TEST(ItchFixtures, BrokenTrade) {
  ItchBrokenTrade m{};
  ASSERT_TRUE(decode_broken_trade(as_bytes(kBrokenTrade), m));
  EXPECT_EQ(m.header.type, 'B');
  EXPECT_EQ(m.header.timestamp, kFixtureTs + 9);
  EXPECT_EQ(m.match_number, 90003ULL)
      << "it names the match number of the trade being broken";
}

TEST(ItchFixtures, SystemEvent) {
  ItchSystemEvent m{};
  ASSERT_TRUE(decode_system_event(as_bytes(kSystemEvent), m));
  EXPECT_EQ(m.header.type, 'S');
  EXPECT_EQ(m.header.stock_locate, 0U) << "system events are not per-symbol";
  EXPECT_EQ(m.header.timestamp, kFixtureTs + 10);
  EXPECT_EQ(m.event_code, 'Q') << "'Q' is start of market hours";
}

TEST(ItchFixtures, StockDirectory) {
  ItchStockDirectory m{};
  ASSERT_TRUE(decode_stock_directory(as_bytes(kStockDirectory), m));
  EXPECT_EQ(m.header.type, 'R');
  EXPECT_EQ(m.header.stock_locate, 1U);
  EXPECT_EQ(m.header.timestamp, kFixtureTs + 11);
  EXPECT_EQ(trim_symbol(m.stock), "AAPL");
  EXPECT_EQ(m.market_category, 'Q');
  EXPECT_EQ(m.financial_status, 'N');
  EXPECT_EQ(m.round_lot_size, 100U);
  EXPECT_FALSE(m.round_lots_only);
  EXPECT_EQ(m.issue_classification, 'C');
  EXPECT_EQ(m.authenticity, 'P');
  EXPECT_EQ(m.short_sale_threshold, 'N');
  EXPECT_EQ(m.ipo_flag, 'N');
  EXPECT_EQ(m.luld_tier, '1');
  EXPECT_EQ(m.etp_flag, 'N');
  EXPECT_EQ(m.etp_leverage, 0U);
  EXPECT_EQ(m.inverse_indicator, 'N');
}

TEST(ItchFixtures, TradingAction) {
  ItchTradingAction m{};
  ASSERT_TRUE(decode_trading_action(as_bytes(kTradingAction), m));
  EXPECT_EQ(m.header.type, 'H');
  EXPECT_EQ(m.header.timestamp, kFixtureTs + 12);
  EXPECT_EQ(trim_symbol(m.stock), "AAPL");
  EXPECT_EQ(m.state, 'T') << "'T' is trading";
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

TEST(ItchFixtures, ASideThatIsNeitherBuyNorSellIsRejectedNotGuessed) {
  std::array<std::uint8_t, sizeof(kAddOrder)> bad{};
  std::memcpy(bad.data(), kAddOrder, bad.size());
  bad[19] = static_cast<std::uint8_t>('X'); // the side byte
  ItchAddOrder m{};
  EXPECT_FALSE(decode_add_order(as_bytes(bad.data()), m))
      << "guessing a side puts liquidity on the wrong half of the book and "
         "every state after it is wrong";
}

TEST(ItchFixtures, APrintableFlagThatIsNeitherYNorNIsRejected) {
  std::array<std::uint8_t, sizeof(kOrderExecutedWithPrice)> bad{};
  std::memcpy(bad.data(), kOrderExecutedWithPrice, bad.size());
  bad[31] = static_cast<std::uint8_t>('?');
  ItchOrderExecuted m{};
  EXPECT_FALSE(decode_order_executed(as_bytes(bad.data()), m));
}

// ---------------------------------------------------------------------------
// The test-only encoder, checked against the fixtures and then round-tripped
// ---------------------------------------------------------------------------

TEST(ItchEncoder, ReproducesTheHandWrittenFixturesExactly) {
  // This is what licenses the round-trip test below. Until the encoder agrees
  // with the independently-written fixtures, a round trip through it proves
  // only that it is self-consistent.
  const auto add = test::encode_add_order(1, 2, kFixtureTs, 4294967373ULL,
                                          Side::Buy, 1500, "AAPL", 1002100);
  ASSERT_EQ(add.size(), sizeof(kAddOrder));
  EXPECT_EQ(std::memcmp(add.data(), kAddOrder, add.size()), 0);

  const auto mpid =
      test::encode_add_order(3, 4, kFixtureTs + 1, 987654321ULL, Side::Sell,
                             200, "MSFT", 3155000, "NSDQ");
  ASSERT_EQ(mpid.size(), sizeof(kAddOrderMpid));
  EXPECT_EQ(std::memcmp(mpid.data(), kAddOrderMpid, mpid.size()), 0);

  const auto exec = test::encode_order_executed(1, 5, kFixtureTs + 2,
                                                4294967373ULL, 500, 90001);
  ASSERT_EQ(exec.size(), sizeof(kOrderExecuted));
  EXPECT_EQ(std::memcmp(exec.data(), kOrderExecuted, exec.size()), 0);

  const auto execp = test::encode_order_executed_with_price(
      1, 6, kFixtureTs + 3, 4294967373ULL, 250, 90002, false, 1002050);
  ASSERT_EQ(execp.size(), sizeof(kOrderExecutedWithPrice));
  EXPECT_EQ(std::memcmp(execp.data(), kOrderExecutedWithPrice, execp.size()),
            0);

  const auto cancel =
      test::encode_order_cancel(1, 7, kFixtureTs + 4, 4294967373ULL, 100);
  ASSERT_EQ(cancel.size(), sizeof(kOrderCancel));
  EXPECT_EQ(std::memcmp(cancel.data(), kOrderCancel, cancel.size()), 0);

  const auto del =
      test::encode_order_delete(1, 8, kFixtureTs + 5, 4294967373ULL);
  ASSERT_EQ(del.size(), sizeof(kOrderDelete));
  EXPECT_EQ(std::memcmp(del.data(), kOrderDelete, del.size()), 0);

  const auto repl = test::encode_order_replace(
      1, 9, kFixtureTs + 6, 4294967373ULL, 4294967374ULL, 650, 1002400);
  ASSERT_EQ(repl.size(), sizeof(kOrderReplace));
  EXPECT_EQ(std::memcmp(repl.data(), kOrderReplace, repl.size()), 0);

  const auto trade = test::encode_trade(1, 10, kFixtureTs + 7, 0, Side::Sell,
                                        300, "AAPL", 1002200, 90003);
  ASSERT_EQ(trade.size(), sizeof(kTrade));
  EXPECT_EQ(std::memcmp(trade.data(), kTrade, trade.size()), 0);

  const auto cross = test::encode_cross_trade(1, 11, kFixtureTs + 8, 5000000,
                                              "AAPL", 1002000, 90004, 'O');
  ASSERT_EQ(cross.size(), sizeof(kCrossTrade));
  EXPECT_EQ(std::memcmp(cross.data(), kCrossTrade, cross.size()), 0);

  const auto sys = test::encode_system_event(0, 13, kFixtureTs + 10, 'Q');
  ASSERT_EQ(sys.size(), sizeof(kSystemEvent));
  EXPECT_EQ(std::memcmp(sys.data(), kSystemEvent, sys.size()), 0);
}

TEST(ItchEncoder, AddOrderRoundTripsOverAWideRangeOfValues) {
  struct Case {
    OrderId ref;
    Side side;
    Qty shares;
    const char* stock;
    Price price;
  };
  const Case cases[] = {
      {0, Side::Buy, 1, "A", 1},
      {1, Side::Sell, 100, "AAPL", 1002100},
      {0xFFFFFFFFFFFFFFFFULL, Side::Buy, 0xFFFFFFFFU, "ZZZZZZZZ", 0xFFFFFFFFLL},
      {4294967296ULL, Side::Sell, 999999, "BRK.B", 54321000},
  };
  for (const Case& c : cases) {
    const auto bytes = test::encode_add_order(7, 9, kFixtureTs, c.ref, c.side,
                                              c.shares, c.stock, c.price);
    ASSERT_EQ(bytes.size(), itch_message_length('A'));
    ItchAddOrder m{};
    ASSERT_TRUE(decode_add_order(as_bytes(bytes.data()), m));
    EXPECT_EQ(m.order_ref, c.ref);
    EXPECT_EQ(m.side, c.side);
    EXPECT_EQ(m.shares, c.shares);
    EXPECT_EQ(trim_symbol(m.stock), std::string_view(c.stock));
    EXPECT_EQ(m.price, c.price);
    EXPECT_EQ(m.header.stock_locate, 7U);
    EXPECT_EQ(m.header.tracking_number, 9U);
    EXPECT_EQ(m.header.timestamp, kFixtureTs);
  }
}

TEST(ItchEncoder, TimestampsRoundTripAcrossTheFullFortyEightBitRange) {
  // A whole trading day is 8.64e13 ns; the field holds up to 2.81e14.
  const TsNanos cases[] = {0, 1, 34200000000000LL, 57600000000000LL,
                           281474976710655LL};
  for (TsNanos ts : cases) {
    const auto bytes = test::encode_order_delete(1, 1, ts, 12345);
    const ItchHeader h = decode_header(as_bytes(bytes.data()));
    EXPECT_EQ(h.timestamp, ts) << "ts=" << ts;
  }
}

TEST(ItchEncoder, SymbolsShorterThanEightBytesArePaddedAndTrimmedBack) {
  for (const char* symbol : {"A", "AB", "ABC", "ABCD", "ABCDEFGH"}) {
    const auto bytes =
        test::encode_add_order(1, 1, kFixtureTs, 1, Side::Buy, 1, symbol, 1);
    ItchAddOrder m{};
    ASSERT_TRUE(decode_add_order(as_bytes(bytes.data()), m));
    EXPECT_EQ(m.stock.size(), 8U) << "the raw field is always 8 bytes";
    EXPECT_EQ(trim_symbol(m.stock), std::string_view(symbol));
  }
}

} // namespace
} // namespace miniex
