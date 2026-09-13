// The generator's job is to be reproducible and to produce flow that looks
// like flow. Both are testable, and both matter: a benchmark run on
// unrealistic flow measures the wrong thing, and a generator that is not
// byte-reproducible makes every benchmark comparison meaningless.

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "book/naive_book.hpp"
#include "engine/command_parser.hpp"
#include "engine/event_sink.hpp"
#include "engine/invariants.hpp"
#include "engine/matching_engine.hpp"
#include "replay/random.hpp"
#include "replay/synthetic.hpp"

namespace miniex {
namespace {

std::vector<std::string> lines_of(const std::vector<Command>& cmds) {
  std::vector<std::string> out;
  out.reserve(cmds.size());
  for (const Command& c : cmds) out.push_back(to_line(c));
  return out;
}

// --- the RNG ---------------------------------------------------------------

TEST(Rng, SameSeedSameStream) {
  Rng a(12345);
  Rng b(12345);
  for (int i = 0; i < 1000; ++i) ASSERT_EQ(a.next(), b.next()) << "draw " << i;
}

TEST(Rng, DifferentSeedsDiverge) {
  Rng a(1);
  Rng b(2);
  int same = 0;
  for (int i = 0; i < 100; ++i) same += (a.next() == b.next()) ? 1 : 0;
  EXPECT_EQ(same, 0);
}

TEST(Rng, BelowStaysInRangeAndCoversIt) {
  Rng rng(7);
  std::set<std::uint32_t> seen;
  for (int i = 0; i < 20000; ++i) {
    const std::uint32_t v = rng.below(10);
    ASSERT_LT(v, 10U);
    seen.insert(v);
  }
  EXPECT_EQ(seen.size(), 10U) << "every value in range must be reachable";
  EXPECT_EQ(rng.below(0), 0U);
  EXPECT_EQ(rng.below(1), 0U);
}

TEST(Rng, BelowIsApproximatelyUniform) {
  Rng rng(99);
  std::uint64_t counts[16] = {};
  constexpr int kDraws = 320000;
  for (int i = 0; i < kDraws; ++i) ++counts[rng.below(16)];
  for (std::uint64_t c : counts) {
    // Expected 20000 each; allow a generous 10% band. This is a smoke test
    // for a broken range reduction, not a randomness test.
    EXPECT_GT(c, 18000U);
    EXPECT_LT(c, 22000U);
  }
}

TEST(Rng, GeometricConcentratesNearZeroAndRespectsTheCap) {
  Rng rng(3);
  std::map<std::uint32_t, int> hist;
  for (int i = 0; i < 20000; ++i) ++hist[rng.geometric(50, 8)];
  EXPECT_GT(hist[0], hist[1]);
  EXPECT_GT(hist[1], hist[2]);
  for (const auto& [value, count] : hist) {
    EXPECT_LE(value, 8U);
    (void)count;
  }
}

// --- reproducibility -------------------------------------------------------

TEST(SyntheticFlow, SameSeedProducesByteIdenticalFlow) {
  FlowConfig cfg;
  cfg.seed = 4711;
  const auto a = SyntheticFlow(cfg).generate(5000);
  const auto b = SyntheticFlow(cfg).generate(5000);
  ASSERT_EQ(a.size(), b.size());
  EXPECT_EQ(lines_of(a), lines_of(b));
}

TEST(SyntheticFlow, DifferentSeedsProduceDifferentFlow) {
  FlowConfig cfg;
  cfg.seed = 1;
  const auto a = SyntheticFlow(cfg).generate(2000);
  cfg.seed = 2;
  const auto b = SyntheticFlow(cfg).generate(2000);
  EXPECT_NE(lines_of(a), lines_of(b));
}

TEST(SyntheticFlow, GeneratingInTwoChunksMatchesGeneratingInOne) {
  FlowConfig cfg;
  cfg.seed = 88;
  const auto whole = SyntheticFlow(cfg).generate(3000);
  SyntheticFlow split(cfg);
  std::vector<Command> parts;
  split.generate(parts, 1000);
  split.generate(parts, 2000);
  EXPECT_EQ(lines_of(whole), lines_of(parts));
}

// --- shape -----------------------------------------------------------------

TEST(SyntheticFlow,
     RealisedMixIsCloseToTheRequestedMixWhenTheMixSustainsABook) {
  FlowConfig cfg;
  cfg.seed = 5;
  cfg.mix = FlowMix{45, 40, 10, 5};
  SyntheticFlow flow(cfg);
  const auto cmds = flow.generate(200000);

  const auto& r = flow.realised();
  const auto n = static_cast<double>(cmds.size());
  EXPECT_NEAR(static_cast<double>(r.adds) / n, 0.45, 0.02);
  EXPECT_NEAR(static_cast<double>(r.cancels) / n, 0.40, 0.02);
  EXPECT_NEAR(static_cast<double>(r.replaces) / n, 0.10, 0.02);
  EXPECT_NEAR(static_cast<double>(r.aggressives) / n, 0.05, 0.02);
}

// The realised mix is published rather than the requested one because the
// two differ, and this is the case where they differ a lot. A mix whose
// removals outpace its additions cannot sustain a book: the live set drains,
// every subsequent cancel is substituted with an add, and what is actually
// being benchmarked is an almost-empty book. Making that visible in a test
// is the point; silently benchmarking it would not be.
TEST(SyntheticFlow, ADrainingMixIsVisibleInTheRealisedNumbers) {
  FlowConfig cfg;
  cfg.seed = 5;
  cfg.mix = FlowMix{30, 55, 5, 10}; // removals 55 > additions ~35
  SyntheticFlow flow(cfg);
  const auto cmds = flow.generate(200000);

  const auto& r = flow.realised();
  const auto n = static_cast<double>(cmds.size());
  EXPECT_LT(static_cast<double>(r.cancels) / n, 0.50)
      << "the requested 55% cancels cannot all be produced";
  EXPECT_GT(static_cast<double>(r.adds) / n, 0.35)
      << "the shortfall shows up as substituted adds";
}

TEST(SyntheticFlow, WarmUpBuildsABookBeforeTheMixStarts) {
  FlowConfig cfg;
  cfg.seed = 9;
  cfg.warmup_adds = 5000;
  SyntheticFlow flow(cfg);
  std::vector<Command> cmds;
  flow.warm_up(cmds);
  ASSERT_EQ(cmds.size(), 5000U);
  for (const Command& c : cmds) EXPECT_EQ(c.type, CommandType::New);

  CountingSink sink;
  MatchingEngine<NaiveBook, CountingSink> engine(sink);
  for (const Command& c : cmds) engine.apply(c);
  EXPECT_GT(engine.book().order_count(), 4000U);
  EXPECT_GT(engine.book().level_count(Side::Buy), 5U);
  EXPECT_GT(engine.book().level_count(Side::Sell), 5U);
}

TEST(SyntheticFlow, DepthIsConcentratedNearTheTouchNotUniform) {
  FlowConfig cfg;
  cfg.seed = 11;
  cfg.mix = FlowMix{100, 0, 0, 0};
  cfg.mid_move_percent = 0;
  SyntheticFlow flow(cfg);
  const auto cmds = flow.generate(50000);

  std::map<Price, int> depth_hist;
  for (const Command& c : cmds) {
    const NewOrder& n = c.body.neworder;
    const Price depth = n.side == Side::Buy ? cfg.base_price - 1 - n.price
                                            : n.price - cfg.base_price - 1;
    ++depth_hist[depth];
  }
  ASSERT_GT(depth_hist[0], 0);
  EXPECT_GT(depth_hist[0], depth_hist[1]);
  EXPECT_GT(depth_hist[1], depth_hist[4]);
  EXPECT_GT(depth_hist[4], depth_hist[12]);
  // And it must actually reach some depth, or the "deep book" cases are lies.
  EXPECT_GT(depth_hist.rbegin()->first, 10);
}

TEST(SyntheticFlow, OrderLifetimesAreHeavyTailed) {
  FlowConfig cfg;
  cfg.seed = 21;
  SyntheticFlow flow(cfg);
  const auto cmds = flow.generate(200000);

  std::map<OrderId, std::size_t> born;
  std::vector<std::size_t> lifetimes;
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    if (cmds[i].type == CommandType::New) {
      born[cmds[i].body.neworder.id] = i;
    } else if (cmds[i].type == CommandType::Cancel) {
      const auto it = born.find(cmds[i].body.cancel.id);
      if (it != born.end()) lifetimes.push_back(i - it->second);
    }
  }
  ASSERT_GT(lifetimes.size(), 1000U);
  std::sort(lifetimes.begin(), lifetimes.end());
  const std::size_t median = lifetimes[lifetimes.size() / 2];
  const std::size_t p99 = lifetimes[lifetimes.size() * 99 / 100];
  // Most orders die young; a few live a very long time. A uniform cancel
  // choice would give p99 only a few times the median, and pure recency
  // gives about 18x -- the mixture of the two is what produces a real tail.
  EXPECT_GT(p99, median * 50)
      << "median lifetime " << median << ", p99 " << p99;
}

TEST(SyntheticFlow, EveryCommandIdRefersToAnOrderThatWasAdded) {
  FlowConfig cfg;
  cfg.seed = 31;
  const auto cmds = SyntheticFlow(cfg).generate(50000);
  std::set<OrderId> added;
  for (const Command& c : cmds) {
    switch (c.type) {
    case CommandType::New:
      EXPECT_TRUE(added.insert(c.body.neworder.id).second)
          << "order id " << c.body.neworder.id << " reused";
      break;
    case CommandType::Cancel:
      EXPECT_EQ(added.count(c.body.cancel.id), 1U);
      break;
    case CommandType::Replace:
      EXPECT_EQ(added.count(c.body.replace.id), 1U);
      break;
    }
  }
}

// --- patterns --------------------------------------------------------------

using Engine = MatchingEngine<NaiveBook, CountingSink>;

struct PatternOutcome {
  std::size_t resting{0};
  std::size_t bid_levels{0};
  std::size_t ask_levels{0};
  std::uint64_t trades{0};
  Price widest{0};
};

PatternOutcome run_pattern(FlowPattern pattern, std::size_t count) {
  FlowConfig cfg;
  cfg.seed = 1234;
  cfg.pattern = pattern;
  SyntheticFlow flow(cfg);
  const auto cmds = flow.generate(count);

  CountingSink sink;
  Engine engine(sink);
  for (const Command& c : cmds) engine.apply(c);

  PatternOutcome out;
  out.resting = engine.book().order_count();
  out.bid_levels = engine.book().level_count(Side::Buy);
  out.ask_levels = engine.book().level_count(Side::Sell);
  out.trades = engine.stats().trades;
  Price lowest = kMarketBuyPrice;
  Price highest = 0;
  engine.book().for_each_level(Side::Buy, [&](Price p, AggQty, std::size_t) {
    lowest = std::min(lowest, p);
    highest = std::max(highest, p);
  });
  engine.book().for_each_level(Side::Sell, [&](Price p, AggQty, std::size_t) {
    lowest = std::min(lowest, p);
    highest = std::max(highest, p);
  });
  out.widest = highest > lowest ? highest - lowest : 0;
  return out;
}

TEST(SyntheticFlowPatterns, EachPatternProducesTheShapeItPromises) {
  const auto balanced = run_pattern(FlowPattern::Balanced, 60000);
  const auto deep = run_pattern(FlowPattern::DeepBook, 60000);
  const auto churn = run_pattern(FlowPattern::TouchChurn, 60000);
  const auto wide = run_pattern(FlowPattern::WideSpread, 60000);
  const auto overflow = run_pattern(FlowPattern::LadderOverflow, 60000);
  const auto pressure = run_pattern(FlowPattern::PoolPressure, 60000);

  // deep-book: many more resting orders and many more levels than balanced.
  EXPECT_GT(deep.resting, balanced.resting * 3);
  EXPECT_GT(deep.bid_levels + deep.ask_levels,
            balanced.bid_levels + balanced.ask_levels);

  // touch-churn: everything crammed into a handful of price levels.
  EXPECT_LT(churn.bid_levels + churn.ask_levels, 12U);

  // wide-spread and ladder-overflow: prices spread far apart, the overflow
  // one further than the default resident ladder window.
  EXPECT_GT(wide.widest, 1000);
  EXPECT_GT(overflow.widest, static_cast<Price>(kLadderTicks));

  // pool-pressure: adds vastly outnumber cancels, so resting count balloons.
  EXPECT_GT(pressure.resting, 50000U);

  // Balanced flow must actually trade, or the mix is broken.
  EXPECT_GT(balanced.trades, 100U);
}

TEST(SyntheticFlowPatterns, EveryPatternKeepsTheEngineInvariantsIntact) {
  for (const FlowPattern pattern :
       {FlowPattern::Balanced, FlowPattern::DeepBook, FlowPattern::TouchChurn,
        FlowPattern::WideSpread, FlowPattern::LadderOverflow,
        FlowPattern::PoolPressure}) {
    FlowConfig cfg;
    cfg.seed = 55;
    cfg.pattern = pattern;
    const auto cmds = SyntheticFlow(cfg).generate(20000);

    VectorSink sink;
    MatchingEngine<NaiveBook, VectorSink> engine(sink);
    for (const Command& c : cmds) engine.apply(c);

    const auto book_ok = check_book(engine.book());
    EXPECT_TRUE(book_ok.ok()) << to_string(pattern) << ": " << book_ok.what;
    ShareLedger ledger;
    for (const Event& e : sink.events) ledger.on_event(e);
    const auto ledger_ok = ledger.check();
    EXPECT_TRUE(ledger_ok.ok()) << to_string(pattern) << ": " << ledger_ok.what;
  }
}

TEST(SyntheticFlowPatterns, PatternNamesRoundTrip) {
  for (const FlowPattern pattern :
       {FlowPattern::Balanced, FlowPattern::DeepBook, FlowPattern::TouchChurn,
        FlowPattern::WideSpread, FlowPattern::LadderOverflow,
        FlowPattern::PoolPressure}) {
    FlowPattern parsed{};
    ASSERT_TRUE(parse_pattern(to_string(pattern), parsed));
    EXPECT_EQ(parsed, pattern);
  }
  FlowPattern ignored{};
  EXPECT_FALSE(parse_pattern("nonsense", ignored));
}

} // namespace
} // namespace miniex
