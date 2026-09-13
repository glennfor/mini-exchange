/// \file book_dump.cpp
/// \brief Reconstruct a symbol from an ITCH file and print the book.
///
/// The tool you reach for when a reconstruction disagrees with something: run
/// it up to the event where the disagreement starts and look at the book.
/// `--at` stops at a message index, `--until` at a time of day.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/timestamp.hpp"

#include "book/md_book.hpp"
#include "feed/decoder.hpp"
#include "feed/mapped_file.hpp"

#include "tools/cli.hpp"

namespace {

using namespace miniex;

/// Applies the book-mutating subset of the normalised stream, filtered to one
/// symbol. Non-book messages are counted and dropped.
struct ReconstructHandler {
  MarketDataBook* book{nullptr};
  SymbolId symbol{0};
  bool all_symbols{false};
  std::size_t applied{0};
  std::size_t stop_after{0};
  TsNanos stop_at_ts{0};
  bool stopped{false};
  TsNanos last_ts{0};

  void operator()(const NormalizedEvent& e) {
    if (stopped) return;
    if (!all_symbols && e.symbol != symbol) return;
    if (stop_at_ts != 0 && e.ts > stop_at_ts) {
      stopped = true;
      return;
    }
    last_ts = e.ts;
    switch (e.action) {
    case MdAction::Add: (void)book->add(e.ref, e.side, e.price, e.qty); break;
    case MdAction::Execute: (void)book->execute(e.ref, e.qty); break;
    case MdAction::ExecuteWithPrice:
      (void)book->execute_with_price(e.ref, e.qty, e.price, e.printable);
      break;
    case MdAction::Cancel: (void)book->cancel(e.ref, e.qty); break;
    case MdAction::Delete: (void)book->remove(e.ref); break;
    case MdAction::Replace:
      (void)book->replace(e.ref, e.new_ref, e.qty, e.price);
      break;
    case MdAction::Trade: book->hidden_print(e.price, e.qty); break;
    case MdAction::Cross: book->cross_print(e.price, e.qty); break;
    case MdAction::Broken: book->broken_trade(); break;
    default: return;
    }
    ++applied;
    if (stop_after != 0 && applied >= stop_after) stopped = true;
  }
};

void print_time(TsNanos ns) {
  const std::int64_t s = ns / 1'000'000'000;
  std::printf("%02lld:%02lld:%02lld.%09lld", (long long)(s / 3600),
              (long long)((s / 60) % 60), (long long)(s % 60),
              (long long)(ns % 1'000'000'000));
}

/// Parses `HH:MM:SS` or `HH:MM:SS.fraction` into nanoseconds after midnight.
bool parse_time(const std::string& text, TsNanos& out) {
  if (text.empty()) return false;
  int hh = 0;
  int mm = 0;
  int ss = 0;
  double ignored = 0;
  (void)ignored;
  if (std::sscanf(text.c_str(), "%d:%d:%d", &hh, &mm, &ss) != 3) return false;
  TsNanos nanos = 0;
  const std::size_t dot = text.find('.');
  if (dot != std::string::npos) {
    for (std::size_t i = 0; i < 9; ++i) {
      const char c = dot + 1 + i < text.size() ? text[dot + 1 + i] : '0';
      if (c < '0' || c > '9') return false;
      nanos = nanos * 10 + (c - '0');
    }
  }
  out = ((static_cast<TsNanos>(hh) * 3600 + mm * 60 + ss) * 1'000'000'000) +
        nanos;
  return true;
}

} // namespace

int main(int argc, char** argv) {
  Cli cli("book_dump",
          "reconstruct a symbol from an ITCH file and print the top N levels");
  cli.option("file", "the ITCH file to read", "", true);
  cli.option("locate", "stock locate to reconstruct (see itch_stats)", "1");
  cli.option("levels", "levels to print per side", "10");
  cli.option("at", "stop after this many book-mutating messages (0 = all)",
             "0");
  cli.option("until", "stop at this time of day, HH:MM:SS[.fraction]", "");
  cli.flag("all-symbols", "reconstruct every symbol into one book (debugging)");
  cli.flag("stats", "print reconstruction counters as well as the book");
  if (!cli.parse(argc, argv)) return 2;

  const CycleCalibration cal(100);
  Cli::print_provenance(&cal);

  MappedFile file;
  if (!file.open(cli.get("file"))) {
    std::fprintf(stderr, "book_dump: %s\n", file.error().c_str());
    return 2;
  }

  MarketDataBook book;
  ReconstructHandler handler;
  handler.book = &book;
  handler.symbol = static_cast<SymbolId>(cli.get_int("locate", 1));
  handler.all_symbols = cli.has("all-symbols");
  handler.stop_after = static_cast<std::size_t>(cli.get_int("at", 0));
  if (!cli.get("until").empty() &&
      !parse_time(cli.get("until"), handler.stop_at_ts)) {
    std::fprintf(stderr, "book_dump: cannot parse --until %s\n",
                 cli.get("until").c_str());
    return 2;
  }

  ItchDecoder decoder(file.data(), file.size(), handler);
  const Cycles t0 = now_cycles();
  const std::size_t decoded = decoder.decode(~std::size_t{0}, /*strict=*/false);
  const std::int64_t elapsed_ns = cal.cycles_to_ns(now_cycles() - t0);

  if (!decoder.failure().ok()) {
    std::fprintf(stderr, "book_dump: decode stopped: %s\n",
                 decoder.failure().what().c_str());
    return 1;
  }

  const auto levels = static_cast<std::size_t>(cli.get_int("levels", 10));
  std::printf("\nstock locate %u after %zu book messages (of %zu decoded)\n",
              handler.symbol, handler.applied, decoded);
  std::printf("last timestamp ");
  print_time(handler.last_ts);
  std::printf("\n\n");

  const auto asks = book.top_n(Side::Sell, levels);
  const auto bids = book.top_n(Side::Buy, levels);
  std::printf("  %10s %8s %6s | %6s %8s %10s\n", "bid px", "bid qty", "ords",
              "ords", "ask qty", "ask px");
  for (std::size_t i = 0; i < levels; ++i) {
    const std::size_t bid_i = levels - 1 - i;
    (void)bid_i;
    std::printf("  %10lld %8llu %6u | %6u %8llu %10lld\n",
                (long long)bids[i].price, (unsigned long long)bids[i].qty,
                bids[i].orders, asks[i].orders, (unsigned long long)asks[i].qty,
                (long long)asks[i].price);
  }

  const Price bid = book.best(Side::Buy);
  const Price ask = book.best(Side::Sell);
  if (bid != kInvalidPrice && ask != kInvalidPrice) {
    std::printf("\n  spread %lld ticks%s\n", (long long)(ask - bid),
                book.is_crossed_or_locked()
                    ? "   <-- CROSSED OR LOCKED (this "
                      "is legal; see docs/validation.md)"
                    : "");
  }

  if (cli.has("stats")) {
    const MdStats& s = book.stats();
    std::printf("\n  adds %llu  executes %llu  cancels %llu  deletes %llu  "
                "replaces %llu\n",
                (unsigned long long)s.adds, (unsigned long long)s.executes,
                (unsigned long long)s.cancels, (unsigned long long)s.deletes,
                (unsigned long long)s.replaces);
    std::printf("  hidden prints %llu  crosses %llu  broken %llu\n",
                (unsigned long long)s.hidden_prints,
                (unsigned long long)s.crosses, (unsigned long long)s.broken);
    std::printf("  printable volume %llu over %llu trades, last %lld\n",
                (unsigned long long)s.printable_volume,
                (unsigned long long)s.printable_trades,
                (long long)s.last_trade_price);
    std::printf("  unknown references %llu   crossed states %llu   "
                "oversized reductions %llu\n",
                (unsigned long long)s.unknown_reference,
                (unsigned long long)s.crossed_states,
                (unsigned long long)s.oversized_reduction);
    if (elapsed_ns > 0) {
      const double seconds = (double)elapsed_ns / 1e9;
      std::printf("\n  decode + reconstruct: %.2f s, %.2f million messages/s, "
                  "%.1f MB/s\n",
                  seconds, (double)decoded / seconds / 1e6,
                  (double)file.size() / seconds / (1024.0 * 1024.0));
    }
  }
  return 0;
}
