/// \file tape_print.cpp
/// \brief The trade tape, in a form a person can read.
///
/// Every print in the feed with its time, price, size, aggressor side and
/// provenance. The provenance column is the useful part: a `P` print matched
/// non-displayed liquidity, a `Q` is an auction cross, and an `E`/`C` is an
/// execution against a resting order the book knows about. Those three are
/// very different events and a tape that renders them identically hides the
/// most interesting thing in the file.
///
/// The aggressor side for an `E`/`C` is **derived**, not carried: the message
/// names the resting order, and the aggressor is on the other side. The
/// reconstruction knows which side the resting order was on, which is why
/// this tool reconstructs rather than just decoding.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "book/md_book.hpp"
#include "feed/decoder.hpp"
#include "feed/mapped_file.hpp"

#include "tools/cli.hpp"

namespace {

using namespace miniex;

void print_time(TsNanos ns) {
  const std::int64_t s = ns / 1'000'000'000;
  std::printf("%02lld:%02lld:%02lld.%09lld", (long long)(s / 3600),
              (long long)((s / 60) % 60), (long long)(s % 60),
              (long long)(ns % 1'000'000'000));
}

/// Prints a price in dollars from integer ticks, without ever converting to
/// floating point: the integer and fractional parts are printed separately.
void print_price(Price ticks) {
  const Price whole = ticks / kPriceScale;
  const Price frac = ticks % kPriceScale;
  std::printf("%8lld.%04lld", (long long)whole,
              (long long)(frac < 0 ? -frac : frac));
}

struct Tape {
  MarketDataBook book;
  SymbolId symbol{1};
  std::size_t index{0};
  std::size_t printed{0};
  std::size_t limit{0};
  TsNanos from_ts{0};
  TsNanos to_ts{0};
  bool hidden_only{false};
  std::uint64_t volume{0};

  /// The side of the resting order, which the reconstruction knows and the
  /// execution message does not repeat.
  Side resting_side(OrderId ref) const {
    // Ask the book which side holds this reference by checking both touches
    // is not reliable; instead the book's own record is consulted through a
    // narrow accessor kept for exactly this.
    return book.side_of(ref);
  }

  void operator()(const NormalizedEvent& e) {
    if (e.symbol != symbol) return;
    ++index;

    Price price = 0;
    Qty qty = 0;
    Side aggressor = Side::Buy;
    const char* kind = nullptr;
    bool printable = true;

    switch (e.action) {
    case MdAction::Add: (void)book.add(e.ref, e.side, e.price, e.qty); return;
    case MdAction::Cancel: (void)book.cancel(e.ref, e.qty); return;
    case MdAction::Delete: (void)book.remove(e.ref); return;
    case MdAction::Replace:
      (void)book.replace(e.ref, e.new_ref, e.qty, e.price);
      return;
    case MdAction::Execute: {
      const Side resting = resting_side(e.ref);
      price = book.price_of(e.ref);
      aggressor = opposite(resting);
      qty = e.qty;
      kind = "lit";
      (void)book.execute(e.ref, e.qty);
      break;
    }
    case MdAction::ExecuteWithPrice: {
      const Side resting = resting_side(e.ref);
      aggressor = opposite(resting);
      price = e.price;
      qty = e.qty;
      printable = e.printable;
      kind = printable ? "lit" : "lit*";
      (void)book.execute_with_price(e.ref, e.qty, e.price, e.printable);
      break;
    }
    case MdAction::Trade:
      // 'P' names the side of the non-displayed order, so the aggressor is
      // the other one -- the same inversion as LOBSTER's execution
      // direction.
      aggressor = opposite(e.side);
      price = e.price;
      qty = e.qty;
      kind = "hidden";
      book.hidden_print(e.price, e.qty);
      break;
    case MdAction::Cross:
      aggressor = Side::Buy; // a cross has no aggressor
      price = e.price;
      qty = e.qty;
      kind = "cross";
      book.cross_print(e.price, e.qty);
      break;
    case MdAction::Broken:
      book.broken_trade();
      std::printf("%-18s %10s %8s %-3s %-7s  BROKEN match=%llu\n", "", "", "",
                  "", "", (unsigned long long)e.match);
      return;
    default: return;
    }

    if (kind == nullptr) return;
    if (hidden_only && kind[0] != 'h' && kind[0] != 'c') return;
    if (from_ts != 0 && e.ts < from_ts) return;
    if (to_ts != 0 && e.ts > to_ts) return;
    if (limit != 0 && printed >= limit) return;

    print_time(e.ts);
    std::printf("  ");
    print_price(price);
    std::printf(" %8u  %c  %-7s%s\n", qty,
                kind[0] == 'c' ? '-' : side_char(aggressor), kind,
                printable ? "" : "  (non-printable: excluded from volume)");
    ++printed;
    if (printable) volume += qty;
  }
};

} // namespace

int main(int argc, char** argv) {
  Cli cli("tape_print",
          "human-readable trade tape with aggressor side and provenance");
  cli.option("file", "the ITCH file to read", "", true);
  cli.option("locate", "stock locate", "1");
  cli.option("limit", "stop after this many prints (0 = all)", "50");
  cli.flag("hidden-only", "show only non-displayed prints and crosses");
  if (!cli.parse(argc, argv)) return 2;
  Cli::print_provenance();

  MappedFile file;
  if (!file.open(cli.get("file"))) {
    std::fprintf(stderr, "tape_print: %s\n", file.error().c_str());
    return 2;
  }

  Tape tape;
  tape.symbol = static_cast<SymbolId>(cli.get_int("locate", 1));
  tape.limit = static_cast<std::size_t>(cli.get_int("limit", 50));
  tape.hidden_only = cli.has("hidden-only");

  std::printf("%-18s %13s %8s  %s  %s\n", "time", "price", "size", "agg",
              "kind");
  ItchDecoder decoder(file.data(), file.size(), tape);
  (void)decoder.decode(~std::size_t{0}, /*strict=*/false);
  if (!decoder.failure().ok()) {
    std::fprintf(stderr, "tape_print: decode stopped: %s\n",
                 decoder.failure().what().c_str());
    return 1;
  }

  const MdStats& s = tape.book.stats();
  std::printf("\n%zu prints shown, %llu printable shares on this tape\n",
              tape.printed, (unsigned long long)tape.volume);
  std::printf("whole file: %llu lit, %llu hidden, %llu crosses, %llu broken; "
              "%llu printable shares\n",
              (unsigned long long)s.executes,
              (unsigned long long)s.hidden_prints,
              (unsigned long long)s.crosses, (unsigned long long)s.broken,
              (unsigned long long)s.printable_volume);
  std::printf("\n'lit*' marks a non-printable execution: it changed the book "
              "but is excluded\nfrom volume and last-price statistics by "
              "definition.\n");
  return 0;
}
