/// \file lobster_check.cpp
/// \brief Event-by-event validation of the reconstruction against LOBSTER.
///
/// The procedure (spec section 12.2), exactly:
///
///   1. Load the message file.
///   2. Feed every message through `MarketDataBook`.
///   3. **After every message**, compare the top N levels on both sides --
///      price and size -- against the corresponding orderbook-file row.
///   4. On a mismatch, print the diagnostic report and stop.
///
/// Success is **zero mismatches over the full day at N = 10**. Comparing at a
/// coarser granularity than every message -- every hundredth row, or only at
/// the close -- and calling the result validated is explicitly not what this
/// does, because a reconstruction can be wrong for ten thousand messages and
/// right again by the time anyone looks.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "book/md_book.hpp"
#include "replay/lobster.hpp"

#include "tools/cli.hpp"

namespace {

using namespace miniex;

/// Applies one LOBSTER message to the reconstruction.
///
/// The mapping is the same one ITCH uses, which is the point: LOBSTER is
/// derived from ITCH, so a reconstruction that agrees with LOBSTER is
/// evidence about the ITCH path too.
void apply(MarketDataBook& book, const LobsterMessage& msg) {
  switch (msg.type) {
  case LobsterType::Submission:
    (void)book.add(msg.order_id, msg.side, msg.price, msg.size);
    break;
  case LobsterType::PartialCancel:
    (void)book.cancel(msg.order_id, msg.size);
    break;
  case LobsterType::Deletion: (void)book.remove(msg.order_id); break;
  case LobsterType::VisibleExecution:
    // The direction on an execution is the side of the order being
    // executed, which is exactly the side the book already has it on. The
    // aggressor is the other side; see LobsterMessage::aggressor_side.
    (void)book.execute(msg.order_id, msg.size);
    break;
  case LobsterType::HiddenExecution:
    // A hidden execution does not change the visible book at all.
    book.hidden_print(msg.price, msg.size);
    break;
  case LobsterType::AuctionCross: book.cross_print(msg.price, msg.size); break;
  case LobsterType::TradingHalt: break;
  }
}

struct Mismatch {
  std::size_t index{0};
  Side side{Side::Buy};
  std::size_t level{0};
  DepthLevel ours;
  Price their_price{0};
  AggQty their_size{0};
  bool price_differs{false};
};

/// Compares our top N against one orderbook row. Returns true when they
/// agree.
bool compare_row(const MarketDataBook& book, const LobsterBookRow& row,
                 std::size_t levels, std::size_t index, Mismatch& out) {
  std::vector<DepthLevel> ours;
  for (const Side side : {Side::Sell, Side::Buy}) {
    book.top_n(side, levels, ours);
    const auto& their_price =
        side == Side::Sell ? row.ask_price : row.bid_price;
    const auto& their_size = side == Side::Sell ? row.ask_size : row.bid_size;
    for (std::size_t i = 0; i < levels && i < their_price.size(); ++i) {
      const bool their_empty =
          is_lobster_empty_level(their_price[i], their_size[i]);
      const bool our_empty = ours[i].price == kInvalidPrice || ours[i].qty == 0;
      if (their_empty && our_empty) continue;
      if (their_empty != our_empty || ours[i].price != their_price[i] ||
          ours[i].qty != their_size[i]) {
        out = Mismatch{index,
                       side,
                       i,
                       ours[i],
                       their_price[i],
                       their_size[i],
                       ours[i].price != their_price[i]};
        return false;
      }
    }
  }
  return true;
}

/// The diagnostic the spec asks for: the message index, the message itself,
/// both books' top N, and the first differing level marked.
void report(const MarketDataBook& book, const LobsterMessage& msg,
            const LobsterBookRow& row, const Mismatch& m, std::size_t levels) {
  std::printf("\nLOBSTER MISMATCH\n");
  std::printf("  message index: %zu\n", m.index);
  std::printf("  message:       type=%s(%d) id=%llu size=%u price=%lld "
              "side=%c ts=%lld\n",
              to_string(msg.type), static_cast<int>(msg.type),
              static_cast<unsigned long long>(msg.order_id), msg.size,
              static_cast<long long>(msg.price), side_char(msg.side),
              static_cast<long long>(msg.ts));
  std::printf("  first differs: %s side, level %zu (%s)\n",
              m.side == Side::Buy ? "bid" : "ask", m.level + 1,
              m.price_differs ? "price" : "size");

  std::printf("\n  %-34s | %s\n", "reconstructed", "LOBSTER");
  std::printf("  %-16s %-17s | %-16s %s\n", "price", "size", "price", "size");
  for (const Side side : {Side::Sell, Side::Buy}) {
    std::printf("  -- %s --\n", side == Side::Sell ? "asks" : "bids");
    const auto ours = book.top_n(side, levels);
    const auto& their_price =
        side == Side::Sell ? row.ask_price : row.bid_price;
    const auto& their_size = side == Side::Sell ? row.ask_size : row.bid_size;
    for (std::size_t i = 0; i < levels; ++i) {
      const bool mark = side == m.side && i == m.level;
      std::printf(
          "%s %-16lld %-17llu | %-16lld %llu\n", mark ? "  >" : "   ",
          static_cast<long long>(ours[i].price),
          static_cast<unsigned long long>(ours[i].qty),
          i < their_price.size() ? static_cast<long long>(their_price[i]) : 0LL,
          i < their_size.size() ? static_cast<unsigned long long>(their_size[i])
                                : 0ULL);
    }
  }
  std::printf("\n  reproduce: lobster_check --messages <file> --book <file> "
              "--levels %zu --stop-at %zu\n\n",
              levels, m.index);
}

} // namespace

int main(int argc, char** argv) {
  Cli cli("lobster_check",
          "validate the reconstruction against a LOBSTER orderbook file, "
          "message by message");
  cli.option("messages", "LOBSTER message CSV", "", true);
  cli.option("book", "LOBSTER orderbook CSV (omit to reconstruct only)", "");
  cli.option("levels", "levels to compare on each side", "10");
  cli.option("stop-at", "stop after this message index (0 = all)", "0");
  cli.flag("strict",
           "treat an unknown order reference as an error (only valid when the "
           "file starts at the opening cross)");
  cli.flag("quiet", "summary only");
  if (!cli.parse(argc, argv)) return 2;
  Cli::print_provenance();

  std::vector<LobsterMessage> messages;
  const LobsterParseError err =
      load_lobster_messages(cli.get("messages"), messages);
  if (!err.ok()) {
    std::fprintf(stderr, "lobster_check: %s:%zu: %s\n",
                 cli.get("messages").c_str(), err.line, err.message.c_str());
    return 2;
  }
  std::fprintf(stderr, "# loaded %zu messages from %s\n", messages.size(),
               cli.get("messages").c_str());

  const auto levels = static_cast<std::size_t>(cli.get_int("levels", 10));
  const auto stop_at = static_cast<std::size_t>(cli.get_int("stop-at", 0));

  std::ifstream book_file;
  const bool comparing = !cli.get("book").empty();
  if (comparing) {
    book_file.open(cli.get("book"));
    if (!book_file) {
      std::fprintf(stderr, "lobster_check: cannot open %s\n",
                   cli.get("book").c_str());
      return 2;
    }
  }

  MarketDataBook book;
  LobsterBookRow row;
  Mismatch mismatch;
  std::string line;
  std::size_t compared = 0;

  for (std::size_t i = 0; i < messages.size(); ++i) {
    if (stop_at != 0 && i > stop_at) break;
    apply(book, messages[i]);

    if (!comparing) continue;
    if (!std::getline(book_file, line)) {
      std::fprintf(stderr,
                   "lobster_check: the orderbook file ran out at message %zu; "
                   "the two files must be in lockstep, one row per message\n",
                   i);
      return 2;
    }
    std::string parse_error;
    if (!parse_lobster_book(line, row, parse_error)) {
      std::fprintf(stderr, "lobster_check: %s:%zu: %s\n",
                   cli.get("book").c_str(), i + 1, parse_error.c_str());
      return 2;
    }
    ++compared;
    if (!compare_row(book, row, std::min(levels, row.levels()), i, mismatch)) {
      report(book, messages[i], row, mismatch, std::min(levels, row.levels()));
      return 1;
    }
  }

  if (comparing && std::getline(book_file, line)) {
    std::fprintf(stderr,
                 "lobster_check: the orderbook file has more rows than the "
                 "message file has messages\n");
    return 2;
  }

  const MdStats& s = book.stats();
  std::printf("\nreconstruction\n");
  std::printf("  messages          %zu\n", messages.size());
  std::printf("  adds              %llu\n", (unsigned long long)s.adds);
  std::printf("  executions        %llu\n", (unsigned long long)s.executes);
  std::printf("  cancels           %llu\n", (unsigned long long)s.cancels);
  std::printf("  deletes           %llu\n", (unsigned long long)s.deletes);
  std::printf("  hidden prints     %llu\n",
              (unsigned long long)s.hidden_prints);
  std::printf("  crosses           %llu\n", (unsigned long long)s.crosses);
  std::printf("  printable volume  %llu over %llu trades\n",
              (unsigned long long)s.printable_volume,
              (unsigned long long)s.printable_trades);
  std::printf("  orders resting    %zu\n", book.order_count());
  std::printf("  levels            %zu bid / %zu ask\n",
              book.level_count(Side::Buy), book.level_count(Side::Sell));

  std::printf("\nnoted, not errors\n");
  std::printf(
      "  unknown reference %llu%s\n", (unsigned long long)s.unknown_reference,
      s.unknown_reference > 0 ? "   (normal when the file starts mid-day)"
                              : "");
  std::printf(
      "  crossed/locked    %llu%s\n", (unsigned long long)s.crossed_states,
      s.crossed_states > 0 ? "   (real feeds do this; see docs/validation.md)"
                           : "");
  std::printf("  oversized reduce  %llu%s\n",
              (unsigned long long)s.oversized_reduction,
              s.oversized_reduction > 0 ? "   <-- INVESTIGATE" : "");

  if (cli.has("strict") && s.unknown_reference > 0) {
    std::fprintf(stderr,
                 "\nlobster_check: --strict and %llu unknown references\n",
                 (unsigned long long)s.unknown_reference);
    return 1;
  }
  if (!book.verify_aggregates()) {
    std::fprintf(stderr,
                 "\nlobster_check: the book's own level aggregates disagree "
                 "with its order records\n");
    return 3;
  }

  if (comparing) {
    std::printf("\nVALIDATION PASSED: %zu messages, %zu book rows compared at "
                "%zu levels per side, zero mismatches\n",
                messages.size(), compared, levels);
  } else {
    std::printf("\n(no --book given; reconstructed only, nothing compared)\n");
  }
  return 0;
}
