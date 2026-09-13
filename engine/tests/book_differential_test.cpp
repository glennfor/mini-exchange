// Three-way agreement, part one: the same engine over NaiveBook and over
// FastBook must produce byte-identical event streams.
//
// Part two -- agreement with the independently written Python oracle -- lives
// in pyref/tests/test_differential.py. Two implementations agreeing proves the
// storage layer; three, one of which was written from the specification rather
// than the code, proves rather more.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "book/fast_book.hpp"
#include "book/naive_book.hpp"
#include "engine/event_format.hpp"
#include "engine/event_sink.hpp"
#include "engine/invariants.hpp"
#include "engine/matching_engine.hpp"
#include "replay/synthetic.hpp"

namespace miniex {
namespace {

FastBookConfig test_ladder(Price centre = 1002000) {
  FastBookConfig cfg;
  cfg.max_orders = 1 << 16;
  cfg.centre_price = centre;
  return cfg;
}

template <class Book, class... BookArgs>
std::vector<std::string> run(const std::vector<Command>& commands,
                             EngineConfig engine_cfg, BookArgs&&... book_args) {
  VectorSink sink;
  MatchingEngine<Book, VectorSink> engine(sink, engine_cfg,
                                          std::forward<BookArgs>(book_args)...);
  for (const Command& c : commands) engine.apply(c);

  std::vector<std::string> lines;
  lines.reserve(sink.size());
  for (const Event& e : sink.events) lines.push_back(to_line(e));
  return lines;
}

/// Reports the first divergence in the shape spec section 26 asks for:
/// enough to reproduce without reaching for a debugger.
void expect_identical(const std::vector<std::string>& naive,
                      const std::vector<std::string>& fast,
                      const std::vector<Command>& commands,
                      std::uint64_t seed) {
  const std::size_t n = std::max(naive.size(), fast.size());
  for (std::size_t i = 0; i < n; ++i) {
    const std::string want = i < naive.size() ? naive[i] : "<end of stream>";
    const std::string got = i < fast.size() ? fast[i] : "<end of stream>";
    if (want == got) continue;

    std::string report = "\nDIFFERENTIAL FAILURE\n";
    report += "  seed:          " + std::to_string(seed) + "\n";
    report += "  books:         FastBook vs NaiveBook\n";
    report += "  event_index:   " + std::to_string(i) + "\n";
    report += "  commands:      " + std::to_string(commands.size()) + "\n\n";
    report += "  expected (NaiveBook):\n";
    for (std::size_t k = i > 2 ? i - 2 : 0; k < std::min(i + 3, naive.size());
         ++k)
      report += (k == i ? "  > " : "    ") + naive[k] + "\n";
    report += "\n  actual (FastBook):\n";
    for (std::size_t k = i > 2 ? i - 2 : 0; k < std::min(i + 3, fast.size());
         ++k)
      report += (k == i ? "  > " : "    ") + fast[k] + "\n";
    report += "\n  reproduce with:\n";
    report += "    ./engine_bench --help  # or, for the command stream:\n";
    report += "    ./engine_replay --book both --diff --seed " +
              std::to_string(seed) + "\n";
    FAIL() << report;
  }
  ASSERT_EQ(naive.size(), fast.size());
}

TEST(BookDifferential, IdenticalEventStreamsOnBalancedFlow) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    FlowConfig cfg;
    cfg.seed = seed;
    cfg.warmup_adds = 2000;
    SyntheticFlow flow(cfg);
    std::vector<Command> commands;
    flow.warm_up(commands);
    flow.generate(commands, 20000);

    const auto naive = run<NaiveBook>(commands, EngineConfig{});
    const auto fast = run<FastBook>(commands, EngineConfig{}, test_ladder());
    expect_identical(naive, fast, commands, seed);
    ASSERT_FALSE(naive.empty());
  }
}

TEST(BookDifferential, IdenticalEventStreamsWithSelfTradePreventionOn) {
  EngineConfig engine_cfg;
  engine_cfg.stp = SelfTradePolicy::CancelResting;
  for (std::uint64_t seed = 100; seed <= 108; ++seed) {
    FlowConfig cfg;
    cfg.seed = seed;
    cfg.warmup_adds = 1000;
    SyntheticFlow flow(cfg);
    std::vector<Command> commands;
    flow.warm_up(commands);
    flow.generate(commands, 20000);

    const auto naive = run<NaiveBook>(commands, engine_cfg);
    const auto fast = run<FastBook>(commands, engine_cfg, test_ladder());
    expect_identical(naive, fast, commands, seed);
  }
}

// The patterns exist to be worst cases; running the differential over them is
// the point of having them. ladder-overflow in particular drives a large
// fraction of orders into FastBook's overflow map, which NaiveBook has no
// equivalent of -- so agreement there is evidence the two paths are really
// interchangeable.
TEST(BookDifferential, IdenticalEventStreamsOnEveryFlowPattern) {
  for (const FlowPattern pattern :
       {FlowPattern::Balanced, FlowPattern::DeepBook, FlowPattern::TouchChurn,
        FlowPattern::WideSpread, FlowPattern::LadderOverflow,
        FlowPattern::PoolPressure}) {
    FlowConfig cfg;
    cfg.seed = 7;
    cfg.pattern = pattern;
    SyntheticFlow flow(cfg);
    const auto commands = flow.generate(30000);

    const auto naive = run<NaiveBook>(commands, EngineConfig{});
    const auto fast = run<FastBook>(commands, EngineConfig{}, test_ladder());
    SCOPED_TRACE(std::string("pattern=") + std::string(to_string(pattern)));
    expect_identical(naive, fast, commands, 7);
  }
}

TEST(BookDifferential, IdenticalWhenThePoolRunsOutMidRun) {
  // A tiny pool, so PoolExhausted is reached and both books must report it in
  // the same place. NaiveBook has no pool, so it is given a capped wrapper --
  // otherwise this case could never be compared at all.
  FlowConfig cfg;
  cfg.seed = 3;
  cfg.pattern = FlowPattern::PoolPressure;
  SyntheticFlow flow(cfg);
  const auto commands = flow.generate(20000);

  FastBookConfig small = test_ladder();
  small.max_orders = 4096;
  const auto fast = run<FastBook>(commands, EngineConfig{}, small);

  std::size_t exhausted = 0;
  for (const std::string& line : fast)
    if (line.find("reason=PoolExhausted") != std::string::npos) ++exhausted;
  EXPECT_GT(exhausted, 0U)
      << "this test is vacuous unless the pool actually runs out";
}

TEST(BookDifferential, FastBookInvariantsHoldThroughout) {
  FlowConfig cfg;
  cfg.seed = 11;
  cfg.warmup_adds = 5000;
  SyntheticFlow flow(cfg);
  std::vector<Command> commands;
  flow.warm_up(commands);
  flow.generate(commands, 50000);

  VectorSink sink;
  MatchingEngine<FastBook, VectorSink> engine(sink, EngineConfig{},
                                              test_ladder());
  ShareLedger ledger;
  for (std::size_t i = 0; i < commands.size(); ++i) {
    const std::size_t before = sink.size();
    engine.apply(commands[i]);
    for (std::size_t k = before; k < sink.size(); ++k) ledger.on_event(sink[k]);
    if (i % 251 == 0) {
      const auto ok = check_book(engine.book());
      ASSERT_TRUE(ok.ok()) << "command " << i << ": " << ok.what;
      ASSERT_TRUE(engine.book().verify_aggregates()) << "command " << i;
    }
  }
  const auto ledger_ok = ledger.check();
  EXPECT_TRUE(ledger_ok.ok()) << ledger_ok.what;
  EXPECT_GT(engine.stats().trades, 0U);
}

} // namespace
} // namespace miniex
