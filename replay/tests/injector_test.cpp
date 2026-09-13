// The injector's job is the message mapping and the accounting. Both are
// testable without any market data, which is what these do.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "book/fast_book.hpp"
#include "engine/event_sink.hpp"
#include "engine/matching_engine.hpp"
#include "replay/injector.hpp"

namespace miniex {
namespace {

NormalizedEvent add_event(OrderId ref, Side side, Price price, Qty qty,
                          TsNanos ts = 1, SymbolId symbol = 1) {
  NormalizedEvent e{};
  e.action = MdAction::Add;
  e.ref = ref;
  e.side = side;
  e.price = price;
  e.qty = qty;
  e.ts = ts;
  e.symbol = symbol;
  return e;
}

NormalizedEvent simple(MdAction action, OrderId ref, Qty qty, TsNanos ts = 2,
                       SymbolId symbol = 1) {
  NormalizedEvent e{};
  e.action = action;
  e.ref = ref;
  e.qty = qty;
  e.ts = ts;
  e.symbol = symbol;
  return e;
}

/// A stand-in for the engine's view of a resting order.
struct FakeBook {
  std::vector<std::pair<OrderId, RestingState>> entries;
  RestingState operator()(OrderId ref) const {
    for (const auto& [id, state] : entries)
      if (id == ref) return state;
    return RestingState{};
  }
};

// --- the message mapping ----------------------------------------------------

TEST(Injector, AnAddBecomesALimitDayOrderCarryingTheItchReference) {
  Injector injector;
  std::vector<Command> out;
  FakeBook book;
  ASSERT_TRUE(
      injector.translate(add_event(77, Side::Buy, 1002100, 500), out, book));
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].type, CommandType::New);
  EXPECT_EQ(out[0].body.neworder.id, 77U)
      << "the ITCH order reference IS the engine order id, so every later "
         "message lines up without a translation table";
  EXPECT_EQ(out[0].body.neworder.price, 1002100);
  EXPECT_EQ(out[0].body.neworder.qty, 500U);
  EXPECT_EQ(out[0].body.neworder.side, Side::Buy);
  EXPECT_EQ(out[0].body.neworder.tif, TimeInForce::Day);
  EXPECT_EQ(injector.report().adds_injected, 1U);
}

TEST(Injector, AnItchCancelBecomesAReplaceToTheNewAbsoluteQuantity) {
  // ITCH 'X' says "cancel 100 shares"; our REPLACE says "the new open
  // quantity is N". The injector has to do the subtraction, and it needs the
  // price too because the cancel message does not carry one.
  Injector injector;
  std::vector<Command> out;
  FakeBook book;
  ASSERT_TRUE(
      injector.translate(add_event(5, Side::Buy, 1002100, 500), out, book));
  book.entries.push_back({5, RestingState{1002100, 500, true}});

  out.clear();
  ASSERT_TRUE(injector.translate(simple(MdAction::Cancel, 5, 100), out, book));
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].type, CommandType::Replace);
  EXPECT_EQ(out[0].body.replace.new_qty, 400U);
  EXPECT_EQ(out[0].body.replace.new_price, 1002100)
      << "the price must come from the engine; an ITCH cancel carries none";
  EXPECT_EQ(injector.report().cancels_injected, 1U);
}

TEST(Injector, AnItchCancelForTheWholeQuantityBecomesACancel) {
  Injector injector;
  std::vector<Command> out;
  FakeBook book;
  ASSERT_TRUE(
      injector.translate(add_event(5, Side::Buy, 1002100, 500), out, book));
  book.entries.push_back({5, RestingState{1002100, 500, true}});
  out.clear();
  ASSERT_TRUE(injector.translate(simple(MdAction::Cancel, 5, 500), out, book));
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].type, CommandType::Cancel);
  EXPECT_EQ(injector.report().deletes_injected, 1U);
}

TEST(Injector, ACancelForAnOrderTheEngineNoLongerHasEmitsNothing) {
  // Our engine traded the order out before the exchange's did. There is
  // nothing to cancel, and the divergence is already counted on the
  // execution side.
  Injector injector;
  std::vector<Command> out;
  FakeBook book; // knows nothing
  ASSERT_TRUE(
      injector.translate(add_event(5, Side::Buy, 1002100, 500), out, book));
  out.clear();
  ASSERT_TRUE(injector.translate(simple(MdAction::Cancel, 5, 100), out, book));
  EXPECT_TRUE(out.empty());
}

TEST(Injector, ADeleteBecomesACancel) {
  Injector injector;
  std::vector<Command> out;
  FakeBook book;
  ASSERT_TRUE(injector.translate(simple(MdAction::Delete, 9, 0), out, book));
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].type, CommandType::Cancel);
  EXPECT_EQ(out[0].body.cancel.id, 9U);
}

TEST(Injector, AnItchReplaceBecomesCancelThenAddWithTheNewReference) {
  // ITCH's replace introduces a NEW reference number and goes to the back of
  // the queue. Our engine's REPLACE keeps the client's id -- using it here
  // would leave the old reference alive and every later message for the new
  // one would be an unknown order.
  Injector injector;
  std::vector<Command> out;
  FakeBook book;
  ASSERT_TRUE(
      injector.translate(add_event(10, Side::Sell, 1002400, 300), out, book));

  out.clear();
  NormalizedEvent e = simple(MdAction::Replace, 10, 250);
  e.new_ref = 11;
  e.price = 1002500;
  ASSERT_TRUE(injector.translate(e, out, book));

  ASSERT_EQ(out.size(), 2U);
  EXPECT_EQ(out[0].type, CommandType::Cancel);
  EXPECT_EQ(out[0].body.cancel.id, 10U);
  EXPECT_EQ(out[1].type, CommandType::New);
  EXPECT_EQ(out[1].body.neworder.id, 11U);
  EXPECT_EQ(out[1].body.neworder.price, 1002500);
  EXPECT_EQ(out[1].body.neworder.qty, 250U);
  EXPECT_EQ(out[1].body.neworder.side, Side::Sell)
      << "an ITCH replace carries no side; it must come from the original";
  EXPECT_EQ(injector.report().replaces_injected, 1U);
}

TEST(Injector, ExecutionsAndPrintsAreNeverInjected) {
  Injector injector;
  std::vector<Command> out;
  FakeBook book;
  for (const MdAction action :
       {MdAction::Execute, MdAction::ExecuteWithPrice, MdAction::Trade,
        MdAction::Cross, MdAction::Broken}) {
    out.clear();
    ASSERT_TRUE(injector.translate(simple(action, 1, 100), out, book));
    EXPECT_TRUE(out.empty())
        << "action " << to_string(action)
        << " is the exchange's output, not an order entry command";
  }
}

TEST(Injector, OtherSymbolsAreIgnored) {
  Injector::Config cfg;
  cfg.symbol = 7;
  Injector injector(cfg);
  std::vector<Command> out;
  FakeBook book;
  ASSERT_TRUE(
      injector.translate(add_event(1, Side::Buy, 100, 10, 1, 3), out, book));
  EXPECT_TRUE(out.empty());
  ASSERT_TRUE(
      injector.translate(add_event(2, Side::Buy, 100, 10, 1, 7), out, book));
  EXPECT_EQ(out.size(), 1U);
}

TEST(Injector, TheCommandLimitStopsTranslation) {
  Injector::Config cfg;
  cfg.max_commands = 2;
  Injector injector(cfg);
  std::vector<Command> out;
  FakeBook book;
  EXPECT_TRUE(injector.translate(add_event(1, Side::Buy, 100, 10), out, book));
  EXPECT_TRUE(injector.translate(add_event(2, Side::Buy, 100, 10), out, book));
  EXPECT_FALSE(injector.translate(add_event(3, Side::Buy, 100, 10), out, book));
  EXPECT_EQ(injector.commands_emitted(), 2U);
}

// --- the accounting ---------------------------------------------------------

TEST(InjectorAccounting, AFeedExecutionMatchedByAnEngineFillCountsAsMatched) {
  Injector injector;
  std::vector<Command> out;
  FakeBook book;
  ASSERT_TRUE(
      injector.translate(add_event(5, Side::Buy, 1002100, 500), out, book));

  injector.on_engine_fill(5, 1002100, 200);
  NormalizedEvent exec = simple(MdAction::Execute, 5, 200);
  injector.on_feed_execution(1, exec);
  injector.finish();

  const auto& r = injector.report();
  EXPECT_EQ(r.matched_shares, 200U);
  EXPECT_EQ(r.unmatched_shares, 0U);
  EXPECT_EQ(r.engine_only_shares, 0U);
  EXPECT_DOUBLE_EQ(r.matched_fraction(), 1.0);
}

TEST(InjectorAccounting, PartialMatchesAreSplitCorrectly) {
  Injector injector;
  std::vector<Command> out;
  FakeBook book;
  ASSERT_TRUE(
      injector.translate(add_event(5, Side::Buy, 1002100, 500), out, book));
  injector.on_engine_fill(5, 1002100, 120);
  injector.on_feed_execution(1, simple(MdAction::Execute, 5, 300));
  injector.finish();

  const auto& r = injector.report();
  EXPECT_EQ(r.matched_shares, 120U);
  EXPECT_EQ(r.unmatched_shares, 180U)
      << "the exchange filled 300; our engine only filled 120 of them";
}

TEST(InjectorAccounting,
     AnExecutionAgainstAnUninjectedReferenceIsItsOwnBucket) {
  // A window that starts mid-day sees executions for orders added before the
  // first message read. That is not a modelling failure and must not be
  // counted as one.
  Injector injector;
  std::vector<Command> out;
  FakeBook book;
  ASSERT_TRUE(
      injector.translate(add_event(5, Side::Buy, 1002100, 500), out, book));
  injector.on_feed_execution(1, simple(MdAction::Execute, 999, 400));
  injector.finish();

  const auto& r = injector.report();
  EXPECT_EQ(r.unknown_reference_shares, 400U);
  EXPECT_EQ(r.unmatched_shares, 0U);
  ASSERT_FALSE(injector.cases().empty());
  EXPECT_EQ(injector.cases()[0].ref, 999U);
}

TEST(InjectorAccounting, EngineFillsTheFeedNeverReportedAreCounted) {
  Injector injector;
  std::vector<Command> out;
  FakeBook book;
  ASSERT_TRUE(
      injector.translate(add_event(5, Side::Buy, 1002100, 500), out, book));
  injector.on_engine_fill(5, 1002100, 500);
  injector.finish();
  EXPECT_EQ(injector.report().engine_only_shares, 500U)
      << "our engine crossed something the exchange did not";
}

TEST(InjectorAccounting, HiddenAndCrossVolumeIsTrackedSeparately) {
  Injector injector;
  injector.on_feed_hidden(1000);
  injector.on_feed_cross(50000);
  injector.on_feed_execution(1, simple(MdAction::Execute, 1, 500));

  const auto& r = injector.report();
  EXPECT_EQ(r.feed_hidden_shares, 1000U);
  EXPECT_EQ(r.feed_cross_shares, 50000U);
  // 51000 of 51500 shares of feed volume cannot be produced by a matching
  // engine that implements neither hidden orders nor auctions.
  EXPECT_NEAR(r.structurally_impossible_fraction(), 51000.0 / 51500.0, 1e-9);
}

TEST(InjectorAccounting,
     APriceDisagreementIsFlaggedSeparatelyFromAMissingFill) {
  // A 'C' carries its own execution price. If our engine traded the same
  // resting order at a different price, that is a bug in one of us, not a
  // structural divergence, and it gets its own counter.
  Injector injector;
  std::vector<Command> out;
  FakeBook book;
  ASSERT_TRUE(
      injector.translate(add_event(5, Side::Buy, 1002100, 500), out, book));
  injector.on_engine_fill(5, 1002100, 200);

  NormalizedEvent exec = simple(MdAction::ExecuteWithPrice, 5, 200);
  exec.price = 1001900; // disagrees
  injector.on_feed_execution(1, exec);

  EXPECT_EQ(injector.report().price_disagreements, 1U);
  EXPECT_EQ(injector.report().matched_shares, 0U);
}

// --- end to end against a real engine ---------------------------------------

TEST(InjectorEndToEnd, ASmallStreamRunsThroughTheEngineAndBalances) {
  struct Sink {
    Injector* injector;
    void emit(const Event& e) {
      if (e.type == EventType::Fill)
        injector->on_engine_fill(e.id, e.price, e.qty);
    }
  };

  Injector injector;
  Sink sink{&injector};
  FastBookConfig ladder;
  ladder.max_orders = 1024;
  ladder.centre_price = 1002000;
  MatchingEngine<FastBook, Sink> engine(sink, EngineConfig{}, ladder);

  const auto resting = [&engine](OrderId ref) {
    RestingState out;
    const ConstOrderHandle h = engine.book().find(ref);
    if (h != nullptr) {
      out.price = h->price;
      out.qty = h->qty_open;
      out.found = true;
    }
    return out;
  };

  std::vector<Command> out;
  const auto run = [&](const NormalizedEvent& e) {
    out.clear();
    ASSERT_TRUE(injector.translate(e, out, resting));
    for (const Command& c : out) engine.apply(c);
  };

  run(add_event(1, Side::Sell, 1002400, 300, 1));
  run(add_event(2, Side::Buy, 1002100, 500, 2));
  EXPECT_EQ(engine.book().order_count(), 2U);

  // A partial cancel of the bid.
  run(simple(MdAction::Cancel, 2, 200, 3));
  EXPECT_EQ(engine.book().qty_at(Side::Buy, 1002100), 300U);

  // An aggressive add that crosses the resting ask -- our engine trades it,
  // which is exactly the kind of fill the feed may or may not have made.
  run(add_event(3, Side::Buy, 1002400, 100, 4));
  EXPECT_EQ(injector.report().engine_shares, 100U);

  // The feed says the same resting order was executed for 100.
  injector.on_feed_execution(5, simple(MdAction::Execute, 1, 100, 5));
  injector.finish();

  const auto& r = injector.report();
  EXPECT_EQ(r.adds_injected, 3U);
  EXPECT_EQ(r.cancels_injected, 1U);
  EXPECT_EQ(r.matched_shares, 100U);
  EXPECT_EQ(r.unmatched_shares, 0U);
  EXPECT_EQ(r.engine_only_shares, 0U);
}

} // namespace
} // namespace miniex
