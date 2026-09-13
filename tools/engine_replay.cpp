/// \file engine_replay.cpp
/// \brief Run commands through the engine and print the canonical event
///        stream.
///
/// Three jobs:
///   * `--record` / `--verify` -- the determinism check CI runs.
///   * `--book both --diff` -- the in-repo differential: the same engine over
///     `NaiveBook` and over `FastBook`, compared line by line, reporting the
///     first divergence in the form spec section 26 asks for.
///   * a demonstrable default: with no arguments it runs a built-in scenario
///     covering a sweep, both replace-priority outcomes, and both cancel
///     reject reasons.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "book/fast_book.hpp"
#include "book/naive_book.hpp"
#include "engine/command_parser.hpp"
#include "engine/event_format.hpp"
#include "engine/event_sink.hpp"
#include "engine/invariants.hpp"
#include "engine/matching_engine.hpp"
#include "replay/synthetic.hpp"

#include "tools/cli.hpp"

namespace {

using namespace miniex;

struct RunSummary {
  std::vector<std::string> events;
  std::uint64_t commands{0};
  std::uint64_t trades{0};
  std::uint64_t shares{0};
  std::uint64_t rejects{0};
  std::size_t resting{0};
  std::string invariant_failure;
};

/// Runs the command stream and returns the canonical event lines plus the
/// invariant verdict. Templated on the book so that "the same engine over two
/// books" is literally that.
template <class Book, class... BookArgs>
RunSummary run_engine(const std::vector<Command>& commands, EngineConfig cfg,
                      BookArgs&&... book_args) {
  VectorSink sink;
  MatchingEngine<Book, VectorSink> engine(sink, cfg,
                                          std::forward<BookArgs>(book_args)...);
  ShareLedger ledger;
  for (const Command& c : commands) engine.apply(c);
  for (const Event& e : sink.events) ledger.on_event(e);

  RunSummary out;
  out.events.reserve(sink.size());
  for (const Event& e : sink.events) out.events.push_back(to_line(e));
  out.commands = engine.stats().commands;
  out.trades = engine.stats().trades;
  out.shares = engine.stats().shares_traded;
  out.rejects = engine.stats().rejects;
  out.resting = engine.book().order_count();

  const auto book_ok = check_book(engine.book());
  const auto ledger_ok = ledger.check();
  if (!book_ok.ok())
    out.invariant_failure = "book: " + book_ok.what;
  else if (!ledger_ok.ok())
    out.invariant_failure = "ledger: " + ledger_ok.what;
  return out;
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

/// A small deterministic scenario, so the tool demonstrates something with no
/// input file: a quiet book, a sweep through three levels, a replace that
/// loses priority, a replace that keeps it, and both cancel reject reasons.
std::vector<Command> builtin_scenario() {
  std::vector<Command> cmds;
  const std::string script =
      "NEW ts=1000 id=1 side=S price=1002400 qty=100\n"
      "NEW ts=1001 id=2 side=S price=1002500 qty=200\n"
      "NEW ts=1002 id=3 side=S price=1002600 qty=300\n"
      "NEW ts=1003 id=4 side=B price=1002100 qty=500\n"
      "NEW ts=1004 id=5 side=B price=1002200 qty=400\n"
      "NEW ts=1005 id=6 side=B price=1002600 qty=450\n"
      "NEW ts=1006 id=7 side=B price=1002100 qty=100\n"
      "REPLACE ts=1007 id=4 price=1002100 qty=800\n"
      "REPLACE ts=1008 id=7 price=1002100 qty=50\n"
      "CANCEL ts=1009 id=1\n"
      "CANCEL ts=1010 id=999\n"
      "NEW ts=1011 id=8 side=S price=1002100 qty=1000 tif=IOC\n";
  (void)parse_commands(script, cmds);
  return cmds;
}

void write_stream(std::ostream& out, const std::vector<std::string>& lines) {
  for (const std::string& line : lines) out << line << '\n';
}

/// The spec section 26 divergence report: enough to reproduce immediately,
/// without re-running under a debugger.
int report_divergence(const std::vector<std::string>& expected,
                      const std::vector<std::string>& actual,
                      const std::vector<Command>& commands,
                      const std::string& repro_path) {
  const std::size_t n = std::max(expected.size(), actual.size());
  for (std::size_t i = 0; i < n; ++i) {
    const std::string want =
        i < expected.size() ? expected[i] : "<end of stream>";
    const std::string got = i < actual.size() ? actual[i] : "<end of stream>";
    if (want == got) continue;

    std::cerr << "\nDIFFERENTIAL FAILURE\n"
              << "  books:         FastBook vs NaiveBook\n"
              << "  event_index:   " << i << "\n"
              << "  commands:      " << commands.size() << "\n\n"
              << "  expected (NaiveBook):\n";
    for (std::size_t k = i > 2 ? i - 2 : 0;
         k < std::min(i + 3, expected.size()); ++k)
      std::cerr << (k == i ? "  > " : "    ") << expected[k] << "\n";
    std::cerr << "\n  actual (FastBook):\n";
    for (std::size_t k = i > 2 ? i - 2 : 0; k < std::min(i + 3, actual.size());
         ++k)
      std::cerr << (k == i ? "  > " : "    ") << actual[k] << "\n";

    if (!repro_path.empty()) {
      std::ofstream repro(repro_path, std::ios::trunc);
      for (const Command& c : commands) repro << to_line(c) << "\n";
      std::cerr << "\n  command stream written to: " << repro_path << "\n"
                << "  rerun: engine_replay --commands " << repro_path
                << " --book both --diff\n";
    }
    std::cerr << "\n";
    return 1;
  }
  std::cerr << "# diff OK: " << expected.size()
            << " event lines identical between NaiveBook and FastBook\n";
  return 0;
}

int run(const Cli& cli) {
  std::vector<Command> commands;

  if (cli.get_int("synthetic", 0) > 0) {
    FlowConfig cfg;
    cfg.seed = static_cast<std::uint64_t>(cli.get_int("seed", 1));
    cfg.warmup_adds = static_cast<std::size_t>(cli.get_int("warmup", 0));
    if (!parse_pattern(cli.get("pattern"), cfg.pattern)) {
      std::cerr << "engine_replay: unknown --pattern " << cli.get("pattern")
                << "\n";
      return 2;
    }
    SyntheticFlow flow(cfg);
    flow.warm_up(commands);
    flow.generate(commands,
                  static_cast<std::size_t>(cli.get_int("synthetic", 0)));
    std::cerr << "# generated " << commands.size() << " commands ("
              << to_string(cfg.pattern) << ", seed " << cfg.seed << ")\n";
  } else if (cli.get("commands").empty()) {
    commands = builtin_scenario();
    std::cerr << "# no --commands given; running the built-in scenario ("
              << commands.size() << " commands)\n";
  } else {
    std::string text;
    if (!read_file(cli.get("commands"), text)) {
      std::cerr << "engine_replay: cannot read " << cli.get("commands") << "\n";
      return 2;
    }
    const ParseError err = parse_commands(text, commands);
    if (!err.ok()) {
      std::cerr << "engine_replay: " << cli.get("commands") << ":" << err.line
                << ": " << err.message << "\n";
      return 2;
    }
  }

  EngineConfig cfg;
  cfg.symbol = static_cast<SymbolId>(cli.get_int("symbol", 0));
  if (cli.has("stp")) cfg.stp = SelfTradePolicy::CancelResting;

  FastBookConfig ladder;
  ladder.max_orders = static_cast<std::size_t>(
      cli.get_int("max-orders", static_cast<long long>(kMaxOrders)));
  ladder.centre_price = cli.get_int("centre-price", 0);

  const std::string book = cli.get("book");
  const bool want_naive = book == "naive" || book == "both";
  const bool want_fast = book == "fast" || book == "both";
  if (!want_naive && !want_fast) {
    std::cerr << "engine_replay: --book must be naive, fast or both\n";
    return 2;
  }

  RunSummary naive_run;
  RunSummary fast_run;
  if (want_naive) naive_run = run_engine<NaiveBook>(commands, cfg);
  if (want_fast) fast_run = run_engine<FastBook>(commands, cfg, ladder);
  const RunSummary& primary = want_fast ? fast_run : naive_run;

  if (!primary.invariant_failure.empty()) {
    std::cerr << "engine_replay: INVARIANT VIOLATED: "
              << primary.invariant_failure << "\n";
    return 3;
  }
  if (want_naive && !naive_run.invariant_failure.empty()) {
    std::cerr << "engine_replay: INVARIANT VIOLATED (NaiveBook): "
              << naive_run.invariant_failure << "\n";
    return 3;
  }

  std::cerr << "# book=" << book << " commands=" << primary.commands
            << " events=" << primary.events.size()
            << " trades=" << primary.trades << " shares=" << primary.shares
            << " rejects=" << primary.rejects << " resting=" << primary.resting
            << "\n";

  if (cli.has("diff")) {
    if (book != "both") {
      std::cerr << "engine_replay: --diff requires --book both\n";
      return 2;
    }
    return report_divergence(naive_run.events, fast_run.events, commands,
                             cli.get("repro"));
  }

  if (!cli.get("verify").empty()) {
    std::string expected_text;
    if (!read_file(cli.get("verify"), expected_text)) {
      std::cerr << "engine_replay: cannot read " << cli.get("verify") << "\n";
      return 2;
    }
    std::istringstream expected(expected_text);
    std::size_t line = 0;
    std::string want;
    for (; line < primary.events.size(); ++line) {
      if (!std::getline(expected, want)) {
        std::cerr << "VERIFY FAILED at line " << line + 1
                  << ": recorded stream ended early\n";
        return 1;
      }
      if (want != primary.events[line]) {
        std::cerr << "VERIFY FAILED at line " << line + 1 << "\n"
                  << "  expected: " << want << "\n"
                  << "  actual:   " << primary.events[line] << "\n";
        return 1;
      }
    }
    if (std::getline(expected, want)) {
      std::cerr << "VERIFY FAILED: recorded stream has more events than this "
                   "run produced\n";
      return 1;
    }
    std::cerr << "# verify OK: " << line << " event lines identical\n";
    return 0;
  }

  if (!cli.get("record").empty()) {
    std::ofstream out(cli.get("record"), std::ios::binary | std::ios::trunc);
    if (!out) {
      std::cerr << "engine_replay: cannot write " << cli.get("record") << "\n";
      return 2;
    }
    write_stream(out, primary.events);
    return 0;
  }

  if (!cli.has("quiet")) write_stream(std::cout, primary.events);
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  Cli cli("engine_replay",
          "run commands through the matching engine and emit the canonical "
          "event stream");
  cli.option("commands", "command file to replay (omit for a built-in demo)");
  cli.option("synthetic", "generate N synthetic commands instead of a file",
             "0");
  cli.option("seed", "synthetic flow seed", "1");
  cli.option("pattern", "synthetic flow pattern", "balanced");
  cli.option("warmup", "synthetic warm-up adds", "0");
  cli.option("book", "naive | fast | both", "naive");
  cli.option("max-orders", "FastBook pool capacity", "");
  cli.option("centre-price", "FastBook ladder centre (0 = from the first add)",
             "0");
  cli.option("record", "write the event stream to this file");
  cli.option("verify", "compare the event stream against this file");
  cli.option("repro", "on a --diff failure, write the command stream here");
  cli.option("symbol", "symbol id stamped on every event", "0");
  cli.flag("diff", "compare NaiveBook against FastBook (requires --book both)");
  cli.flag("stp", "enable self-trade prevention (CancelResting)");
  cli.flag("quiet", "suppress the event stream, print only the summary");
  if (!cli.parse(argc, argv)) return 2;
  Cli::print_provenance();
  return run(cli);
}
