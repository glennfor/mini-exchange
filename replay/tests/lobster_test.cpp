// LOBSTER CSV parsing, with particular attention to the two things that are
// easy to get wrong: the decimal timestamp and the direction field on an
// execution.

#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "book/md_book.hpp"
#include "replay/lobster.hpp"

namespace miniex {
namespace {

LobsterMessage parse(const std::string& line) {
  LobsterMessage msg{};
  std::string error;
  EXPECT_TRUE(parse_lobster_message(line, msg, error)) << error;
  return msg;
}

// --- timestamps -------------------------------------------------------------

TEST(LobsterTime, DecimalSecondsBecomeExactNanoseconds) {
  // 34200.017459617 is 09:30:00.017459617. A double holds ~15 significant
  // digits and this needs 14, so parsing through one is close enough to look
  // right and close enough to be wrong in the last place.
  const auto msg = parse("34200.017459617,1,11885113,21,2238100,-1");
  EXPECT_EQ(msg.ts, 34200017459617LL);
}

TEST(LobsterTime, FractionalPartsArePaddedAndTruncatedToNineDigits) {
  EXPECT_EQ(parse("34200.1,1,1,1,1,1").ts, 34200100000000LL);
  EXPECT_EQ(parse("34200.000000001,1,1,1,1,1").ts, 34200000000001LL);
  EXPECT_EQ(parse("34200,1,1,1,1,1").ts, 34200000000000LL);
  // More than nine digits: the extra precision is not representable in
  // nanoseconds and is dropped, not rounded.
  EXPECT_EQ(parse("34200.1234567891,1,1,1,1,1").ts, 34200123456789LL);
}

TEST(LobsterTime, TheWholeTradingDayIsRepresentable) {
  EXPECT_EQ(parse("0.0,1,1,1,1,1").ts, 0LL);
  EXPECT_EQ(parse("57600.0,1,1,1,1,1").ts, 57600000000000LL);
}

// --- the direction trap -----------------------------------------------------

TEST(LobsterDirection, OnASubmissionTheDirectionIsTheOrdersOwnSide) {
  EXPECT_EQ(parse("34200.0,1,1,100,2238100,1").side, Side::Buy);
  EXPECT_EQ(parse("34200.0,1,1,100,2238100,-1").side, Side::Sell);
  EXPECT_EQ(parse("34200.0,1,1,100,2238100,1").aggressor_side(), Side::Buy);
}

TEST(LobsterDirection,
     OnAnExecutionTheDirectionIsTheRestingOrderNotTheAggressor) {
  // THE single most common LOBSTER bug. A sell market order lifting a bid
  // appears with direction 1, because the *limit order that was executed* was
  // a bid. Reading it as the aggressor's side inverts every inferred trade
  // sign in the file -- and the book reconstruction still comes out right, so
  // the mistake hides until someone computes order-flow imbalance backwards.
  const auto executed_bid = parse("34200.0,4,11885113,100,2238100,1");
  EXPECT_EQ(executed_bid.side, Side::Buy)
      << "the order that was executed was a bid";
  EXPECT_EQ(executed_bid.aggressor_side(), Side::Sell)
      << "so the aggressor was a seller";

  const auto executed_ask = parse("34200.0,4,11885113,100,2238100,-1");
  EXPECT_EQ(executed_ask.side, Side::Sell);
  EXPECT_EQ(executed_ask.aggressor_side(), Side::Buy);
}

TEST(LobsterDirection, HiddenExecutionsInvertTheSameWay) {
  const auto hidden = parse("34200.0,5,0,100,2238100,1");
  EXPECT_EQ(hidden.side, Side::Buy);
  EXPECT_EQ(hidden.aggressor_side(), Side::Sell);
}

TEST(LobsterDirection, CancelsAndDeletionsAreNotInverted) {
  EXPECT_EQ(parse("34200.0,2,1,100,2238100,1").aggressor_side(), Side::Buy);
  EXPECT_EQ(parse("34200.0,3,1,100,2238100,-1").aggressor_side(), Side::Sell);
}

// --- which messages touch the visible book ----------------------------------

TEST(LobsterTypes, OnlyFourTypesChangeTheVisibleBook) {
  EXPECT_TRUE(parse("1.0,1,1,1,1,1").changes_visible_book());
  EXPECT_TRUE(parse("1.0,2,1,1,1,1").changes_visible_book());
  EXPECT_TRUE(parse("1.0,3,1,1,1,1").changes_visible_book());
  EXPECT_TRUE(parse("1.0,4,1,1,1,1").changes_visible_book());
  EXPECT_FALSE(parse("1.0,5,1,1,1,1").changes_visible_book())
      << "a hidden execution matched liquidity the visible book never had";
  EXPECT_FALSE(parse("1.0,6,1,1,1,1").changes_visible_book());
  EXPECT_FALSE(parse("1.0,7,1,1,1,1").changes_visible_book());
}

TEST(LobsterTypes, EveryTypeHasAName) {
  for (int t = 1; t <= 7; ++t)
    EXPECT_STRNE(to_string(static_cast<LobsterType>(t)), "Unknown") << t;
}

// --- fields and errors ------------------------------------------------------

TEST(LobsterMessageParse, AllFieldsAreRead) {
  const auto msg = parse("34200.017459617,1,11885113,21,2238100,-1");
  EXPECT_EQ(msg.type, LobsterType::Submission);
  EXPECT_EQ(msg.order_id, 11885113U);
  EXPECT_EQ(msg.size, 21U);
  EXPECT_EQ(msg.price, 2238100)
      << "LOBSTER prices are dollars x 10000, which is this project's Price "
         "scale exactly -- no conversion";
  EXPECT_EQ(msg.side, Side::Sell);
}

TEST(LobsterMessageParse, MalformedLinesAreRejected) {
  LobsterMessage msg{};
  std::string error;
  EXPECT_FALSE(parse_lobster_message("34200.0,1,1,1,1", msg, error));
  EXPECT_NE(error.find("6 columns"), std::string::npos);

  EXPECT_FALSE(parse_lobster_message("34200.0,9,1,1,1,1", msg, error));
  EXPECT_NE(error.find("type"), std::string::npos);

  EXPECT_FALSE(parse_lobster_message("34200.0,1,1,1,1,0", msg, error))
      << "direction must be 1 or -1";
  EXPECT_NE(error.find("direction"), std::string::npos);

  EXPECT_FALSE(parse_lobster_message("nope,1,1,1,1,1", msg, error));
  EXPECT_NE(error.find("time"), std::string::npos);
}

TEST(LobsterMessageParse, WhitespaceAndCarriageReturnsAreTolerated) {
  LobsterMessage msg{};
  std::string error;
  ASSERT_TRUE(
      parse_lobster_message(" 34200.0 , 1 , 5 , 10 , 100 , 1 \r", msg, error))
      << error;
  EXPECT_EQ(msg.order_id, 5U);
  EXPECT_EQ(msg.size, 10U);
}

// --- the orderbook file -----------------------------------------------------

TEST(LobsterBookParse, ColumnsAreAskPriceAskSizeBidPriceBidSize) {
  LobsterBookRow row{};
  std::string error;
  ASSERT_TRUE(parse_lobster_book(
      "2238200,100,2238100,200,2238300,300,2238000,400", row, error))
      << error;
  ASSERT_EQ(row.levels(), 2U);
  EXPECT_EQ(row.ask_price[0], 2238200);
  EXPECT_EQ(row.ask_size[0], 100U);
  EXPECT_EQ(row.bid_price[0], 2238100);
  EXPECT_EQ(row.bid_size[0], 200U);
  EXPECT_EQ(row.ask_price[1], 2238300);
  EXPECT_EQ(row.bid_size[1], 400U);
}

TEST(LobsterBookParse, AColumnCountThatIsNotAMultipleOfFourIsRejected) {
  LobsterBookRow row{};
  std::string error;
  EXPECT_FALSE(parse_lobster_book("1,2,3", row, error));
  EXPECT_NE(error.find("multiple of 4"), std::string::npos);
}

TEST(LobsterBookParse, TheEmptyLevelSentinelIsRecognised) {
  // LOBSTER pads a book thinner than its configured depth with an absurd
  // price and zero size. Without recognising it, "there is no fifth level"
  // reads as "there is a level at $999,999.9999".
  EXPECT_TRUE(is_lobster_empty_level(9999999999LL, 0));
  EXPECT_TRUE(is_lobster_empty_level(-9999999999LL, 0));
  EXPECT_TRUE(is_lobster_empty_level(2238100, 0)) << "zero size is empty";
  EXPECT_FALSE(is_lobster_empty_level(2238100, 100));
}

// --- end-to-end validation against a committed fixture ----------------------
//
// The fixture is a synthetic but format-correct LOBSTER pair, and -- this is
// the part that matters -- its orderbook file was produced by
// `pyref/lobster_check.py`, an independently written Python reconstruction.
// So this test is a cross-implementation check of exactly the same shape as
// the matching engine's: two reconstructions, written separately, compared
// message by message at ten levels.
//
// A pass here is **not** the same claim as a pass against a real LOBSTER day.
// It validates the machinery -- the parser, the reconstruction rules, the
// comparison -- on data whose every case is present by construction. The
// README states which of the two has actually been done.

TEST(LobsterValidation, ReconstructionMatchesTheIndependentPythonBook) {
  const std::string prefix =
      std::string(MINIEX_LOBSTER_FIXTURE_DIR) + "/SYNTH_2026-09-13_34200_57600";
  std::vector<LobsterMessage> messages;
  const LobsterParseError err =
      load_lobster_messages(prefix + "_message_10.csv", messages);
  ASSERT_TRUE(err.ok()) << "line " << err.line << ": " << err.message;
  ASSERT_GT(messages.size(), 500U);

  std::ifstream book_file(prefix + "_orderbook_10.csv");
  ASSERT_TRUE(book_file.is_open());

  MarketDataBook book;
  LobsterBookRow row;
  std::string line;
  std::size_t crossed_rows = 0;

  for (std::size_t i = 0; i < messages.size(); ++i) {
    const LobsterMessage& msg = messages[i];
    switch (msg.type) {
    case LobsterType::Submission:
      (void)book.add(msg.order_id, msg.side, msg.price, msg.size);
      break;
    case LobsterType::PartialCancel:
      (void)book.cancel(msg.order_id, msg.size);
      break;
    case LobsterType::Deletion: (void)book.remove(msg.order_id); break;
    case LobsterType::VisibleExecution:
      (void)book.execute(msg.order_id, msg.size);
      break;
    case LobsterType::HiddenExecution:
      book.hidden_print(msg.price, msg.size);
      break;
    case LobsterType::AuctionCross:
      book.cross_print(msg.price, msg.size);
      break;
    case LobsterType::TradingHalt: break;
    }

    ASSERT_TRUE(std::getline(book_file, line))
        << "the two files must be in lockstep; the book file ran out at "
           "message "
        << i;
    std::string parse_error;
    ASSERT_TRUE(parse_lobster_book(line, row, parse_error))
        << "row " << i << ": " << parse_error;

    if (book.is_crossed_or_locked()) ++crossed_rows;

    for (const Side side : {Side::Sell, Side::Buy}) {
      const auto ours = book.top_n(side, 10);
      const auto& their_price =
          side == Side::Sell ? row.ask_price : row.bid_price;
      const auto& their_size = side == Side::Sell ? row.ask_size : row.bid_size;
      for (std::size_t level = 0; level < 10; ++level) {
        const bool their_empty =
            is_lobster_empty_level(their_price[level], their_size[level]);
        const bool our_empty =
            ours[level].price == kInvalidPrice || ours[level].qty == 0;
        if (their_empty && our_empty) continue;
        ASSERT_EQ(their_empty, our_empty)
            << "message " << i << ", " << side_char(side) << " level "
            << level + 1;
        ASSERT_EQ(ours[level].price, their_price[level])
            << "message " << i << ", " << side_char(side) << " level "
            << level + 1;
        ASSERT_EQ(ours[level].qty, their_size[level])
            << "message " << i << ", " << side_char(side) << " level "
            << level + 1;
      }
    }
  }

  EXPECT_FALSE(std::getline(book_file, line))
      << "the book file has more rows than the message file has messages";
  EXPECT_TRUE(book.verify_aggregates());
  EXPECT_EQ(book.stats().oversized_reduction, 0U);
  EXPECT_GT(crossed_rows, 100U)
      << "the fixture is supposed to contain crossed and locked states; if it "
         "does not, this test is not exercising the tolerance that makes "
         "MarketDataBook different from a matching book";
}

} // namespace
} // namespace miniex
