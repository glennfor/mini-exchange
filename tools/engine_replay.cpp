/// \file engine_replay.cpp
/// \brief Run a command file (or a built-in scenario) through the engine and
///        print the canonical event stream.
///
/// This is the tool the determinism check in CI uses: `--record` writes the
/// stream, `--verify` replays and compares line by line, reporting the first
/// divergence with enough context to reproduce it without a debugger.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "book/naive_book.hpp"
#include "engine/command_parser.hpp"
#include "engine/event_format.hpp"
#include "engine/event_sink.hpp"
#include "engine/invariants.hpp"
#include "engine/matching_engine.hpp"

#include "tools/cli.hpp"

namespace {

using namespace miniex;

/// Streams events straight out as text. Used instead of VectorSink so that a
/// long replay does not need the whole event history in memory.
struct WriterSink {
  std::ostream* out{nullptr};
  ShareLedger* ledger{nullptr};
  std::uint64_t count{0};

  void emit(const Event& e) {
    ++count;
    if (ledger != nullptr) ledger->on_event(e);
    if (out == nullptr) return;
    char buf[kEventLineMax];
    const std::size_t n = format_event(e, buf, sizeof(buf));
    out->write(buf, static_cast<std::streamsize>(n));
    out->put('\n');
  }
};

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

/// A small deterministic scenario, so the tool is demonstrable with no input
/// file at all: a quiet book, a sweep through three levels, a replace that
/// loses priority, and a cancel that arrives too late.
std::vector<Command> builtin_scenario() {
  std::vector<Command> cmds;
  const std::string script =
      "NEW ts=1000 id=1 side=S price=1002400 qty=100\n"
      "NEW ts=1001 id=2 side=S price=1002500 qty=200\n"
      "NEW ts=1002 id=3 side=S price=1002600 qty=300\n"
      "NEW ts=1003 id=4 side=B price=1002100 qty=500\n"
      "NEW ts=1004 id=5 side=B price=1002200 qty=400\n"
      "NEW ts=1005 id=6 side=B price=1002600 qty=450\n" // sweeps three levels
      "NEW ts=1006 id=7 side=B price=1002100 qty=100\n"
      "REPLACE ts=1007 id=4 price=1002100 qty=800\n" // loses priority
      "REPLACE ts=1008 id=7 price=1002100 qty=50\n"  // keeps priority
      "CANCEL ts=1009 id=1\n"                        // too late: filled
      "CANCEL ts=1010 id=999\n"                      // never existed
      "NEW ts=1011 id=8 side=S price=1002100 qty=1000 tif=IOC\n";
  (void)parse_commands(script, cmds);
  return cmds;
}

int run(const Cli& cli) {
  std::vector<Command> commands;
  if (cli.get("commands").empty()) {
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

  // Build the event stream into memory only when we need to compare it.
  const bool verifying = !cli.get("verify").empty();
  std::ostringstream captured;
  std::ofstream recorded;
  std::ostream* out = &std::cout;
  if (verifying) {
    out = &captured;
  } else if (!cli.get("record").empty()) {
    recorded.open(cli.get("record"), std::ios::binary | std::ios::trunc);
    if (!recorded) {
      std::cerr << "engine_replay: cannot write " << cli.get("record") << "\n";
      return 2;
    }
    out = &recorded;
  }

  ShareLedger ledger;
  WriterSink sink;
  sink.out = cli.has("quiet") && !verifying ? nullptr : out;
  sink.ledger = &ledger;

  MatchingEngine<NaiveBook, WriterSink> engine(sink, cfg);
  for (const Command& c : commands) engine.apply(c);

  const auto book_ok = check_book(engine.book());
  const auto ledger_ok = ledger.check();
  if (!book_ok.ok()) {
    std::cerr << "engine_replay: BOOK INVARIANT VIOLATED: " << book_ok.what
              << "\n";
    return 3;
  }
  if (!ledger_ok.ok()) {
    std::cerr << "engine_replay: LEDGER INVARIANT VIOLATED: " << ledger_ok.what
              << "\n";
    return 3;
  }

  std::cerr << "# commands=" << engine.stats().commands
            << " events=" << sink.count << " trades=" << engine.stats().trades
            << " shares=" << engine.stats().shares_traded
            << " rejects=" << engine.stats().rejects
            << " resting=" << engine.book().order_count() << "\n";

  if (!verifying) return 0;

  std::string expected_text;
  if (!read_file(cli.get("verify"), expected_text)) {
    std::cerr << "engine_replay: cannot read " << cli.get("verify") << "\n";
    return 2;
  }
  std::istringstream expected(expected_text);
  std::istringstream actual(captured.str());
  std::string want;
  std::string got;
  std::size_t line = 0;
  while (true) {
    const bool has_want = static_cast<bool>(std::getline(expected, want));
    const bool has_got = static_cast<bool>(std::getline(actual, got));
    ++line;
    if (!has_want && !has_got) break;
    if (has_want != has_got || want != got) {
      std::cerr << "VERIFY FAILED at line " << line << "\n"
                << "  expected: " << (has_want ? want : "<end of stream>")
                << "\n"
                << "  actual:   " << (has_got ? got : "<end of stream>")
                << "\n";
      return 1;
    }
  }
  std::cerr << "# verify OK: " << line - 1 << " event lines identical\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  Cli cli("engine_replay",
          "run commands through the matching engine and emit the canonical "
          "event stream");
  cli.option("commands", "command file to replay (omit for a built-in demo)");
  cli.option("record", "write the event stream to this file");
  cli.option("verify", "compare the event stream against this file");
  cli.option("symbol", "symbol id stamped on every event", "0");
  cli.flag("stp", "enable self-trade prevention (CancelResting)");
  cli.flag("quiet", "suppress the event stream, print only the summary");
  if (!cli.parse(argc, argv)) return 2;
  Cli::print_provenance();
  return run(cli);
}
