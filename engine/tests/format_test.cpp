// The canonical text format is the comparison surface between the C++ engine
// and the Python oracle, so it gets its own tests: exact bytes for known
// events, and a round trip through the command parser.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/command_parser.hpp"
#include "engine/event_format.hpp"
#include "engine/messages.hpp"

namespace miniex {
namespace {

Event make(EventType type) {
  Event e{};
  e.type = type;
  e.side = Side::Buy;
  e.reason = RejectReason::None;
  return e;
}

TEST(EventFormat, AckLineIsExactlyThis) {
  Event e = make(EventType::Ack);
  e.seq = 7;
  e.id = 4471;
  e.price = 1002300;
  e.qty = 500;
  e.qty_remaining = 500;
  EXPECT_EQ(to_line(e),
            "7 ACK id=4471 counter=0 match=0 side=B price=1002300 qty=500 "
            "rem=500 reason=None");
}

TEST(EventFormat, TradeLineCarriesBothSidesOfTheMatch) {
  Event e = make(EventType::Trade);
  e.seq = 91882;
  e.id = 12;
  e.counter_id = 8821;
  e.match_id = 33;
  e.side = Side::Sell;
  e.price = 1002400;
  e.qty = 100;
  e.qty_remaining = 0;
  EXPECT_EQ(to_line(e),
            "91882 TRADE id=12 counter=8821 match=33 side=S price=1002400 "
            "qty=100 rem=0 reason=None");
}

TEST(EventFormat, RejectLineNamesTheReason) {
  Event e = make(EventType::Reject);
  e.seq = 1;
  e.id = 9;
  e.reason = RejectReason::PostOnlyWouldCross;
  EXPECT_EQ(to_line(e),
            "1 REJECT id=9 counter=0 match=0 side=B price=0 qty=0 rem=0 "
            "reason=PostOnlyWouldCross");
}

TEST(EventFormat, NegativePricesAndExtremesFormatCorrectly) {
  Event e = make(EventType::Ack);
  e.price = -1;
  EXPECT_NE(to_line(e).find("price=-1"), std::string::npos);
  e.price = kMarketSellPrice;
  EXPECT_NE(to_line(e).find("price=-9223372036854775807"), std::string::npos);
  e.price = kInvalidPrice; // INT64_MIN -- the negation edge case
  EXPECT_NE(to_line(e).find("price=-9223372036854775808"), std::string::npos);
  e.price = kMarketBuyPrice;
  EXPECT_NE(to_line(e).find("price=9223372036854775807"), std::string::npos);
}

TEST(EventFormat, EveryEventTypeFitsInTheDeclaredBuffer) {
  for (int i = 0; i <= static_cast<int>(EventType::BookDelta); ++i) {
    Event e = make(static_cast<EventType>(i));
    e.seq = ~std::uint64_t{0};
    e.id = ~std::uint64_t{0};
    e.counter_id = ~std::uint64_t{0};
    e.match_id = ~std::uint64_t{0};
    e.price = kInvalidPrice;
    e.qty = ~Qty{0};
    e.qty_remaining = ~Qty{0};
    e.reason = RejectReason::MarketOrderNotFillable;
    char buf[kEventLineMax];
    const std::size_t n = format_event(e, buf, sizeof(buf));
    EXPECT_LT(n, kEventLineMax) << "type " << i;
  }
}

// --- command parser --------------------------------------------------------

TEST(CommandParser, ParsesEachVerb) {
  const std::string text =
      "# a comment\n"
      "\n"
      "NEW ts=10 id=1 side=B price=1002100 qty=100 type=Limit tif=Day "
      "participant=3 symbol=2\n"
      "CANCEL ts=11 id=1\n"
      "REPLACE ts=12 id=1 price=1002200 qty=50   # trailing comment\n";
  std::vector<Command> cmds;
  const ParseError err = parse_commands(text, cmds);
  ASSERT_TRUE(err.ok()) << "line " << err.line << ": " << err.message;
  ASSERT_EQ(cmds.size(), 3U);

  EXPECT_EQ(cmds[0].type, CommandType::New);
  EXPECT_EQ(cmds[0].ts, 10);
  EXPECT_EQ(cmds[0].body.neworder.id, 1U);
  EXPECT_EQ(cmds[0].body.neworder.side, Side::Buy);
  EXPECT_EQ(cmds[0].body.neworder.price, 1002100);
  EXPECT_EQ(cmds[0].body.neworder.qty, 100U);
  EXPECT_EQ(cmds[0].body.neworder.participant, 3U);
  EXPECT_EQ(cmds[0].body.neworder.symbol, 2U);

  EXPECT_EQ(cmds[1].type, CommandType::Cancel);
  EXPECT_EQ(cmds[1].body.cancel.id, 1U);

  EXPECT_EQ(cmds[2].type, CommandType::Replace);
  EXPECT_EQ(cmds[2].body.replace.new_price, 1002200);
  EXPECT_EQ(cmds[2].body.replace.new_qty, 50U);
}

TEST(CommandParser, OptionalFieldsDefaultAndOrderDoesNotMatter) {
  std::vector<Command> cmds;
  const ParseError err =
      parse_commands("NEW qty=5 side=S id=9 price=100\n", cmds);
  ASSERT_TRUE(err.ok()) << err.message;
  ASSERT_EQ(cmds.size(), 1U);
  EXPECT_EQ(cmds[0].ts, 0);
  EXPECT_EQ(cmds[0].body.neworder.participant, 0U);
  EXPECT_EQ(cmds[0].body.neworder.tif, TimeInForce::Day);
  EXPECT_EQ(cmds[0].body.neworder.type, OrderType::Limit);
  EXPECT_EQ(cmds[0].body.neworder.side, Side::Sell);
}

TEST(CommandParser, MalformedLinesReportTheLineNumberAndWhat) {
  std::vector<Command> cmds;
  ParseError err =
      parse_commands("NEW id=1 side=B price=1 qty=1\nFROB id=2\n", cmds);
  EXPECT_FALSE(err.ok());
  EXPECT_EQ(err.line, 2U);
  EXPECT_NE(err.message.find("FROB"), std::string::npos);

  cmds.clear();
  err = parse_commands("NEW id=1 side=X price=1 qty=1\n", cmds);
  EXPECT_FALSE(err.ok());
  EXPECT_NE(err.message.find("side"), std::string::npos);

  cmds.clear();
  err = parse_commands("NEW id=1 price=1 qty=1\n", cmds);
  EXPECT_FALSE(err.ok());
  EXPECT_NE(err.message.find("side"), std::string::npos);

  cmds.clear();
  err = parse_commands("NEW id=1 side=B price=1 qty=1 colour=red\n", cmds);
  EXPECT_FALSE(err.ok());
  EXPECT_NE(err.message.find("colour"), std::string::npos);
}

TEST(CommandParser, RoundTripsThroughItsOwnWriter) {
  const std::string text =
      "NEW ts=10 id=1 side=B price=1002100 qty=100 type=Market tif=IOC "
      "participant=3 symbol=2\n"
      "CANCEL ts=11 id=1\n"
      "REPLACE ts=12 id=1 price=1002200 qty=50\n";
  std::vector<Command> first;
  ASSERT_TRUE(parse_commands(text, first).ok());

  std::string rendered;
  for (const Command& c : first) rendered += to_line(c) + "\n";

  std::vector<Command> second;
  ASSERT_TRUE(parse_commands(rendered, second).ok());
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(to_line(first[i]), to_line(second[i]));
  }
}

} // namespace
} // namespace miniex
