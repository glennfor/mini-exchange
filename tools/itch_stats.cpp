/// \file itch_stats.cpp
/// \brief Message-type histogram, time span and decode throughput for an ITCH
///        file.
///
/// The first thing to run against a new sample day, and the thing that tells
/// you whether the file is what you think it is: which types appear, how
/// many, over what time span, and how fast the decoder gets through them.
///
/// The throughput figure it prints is decode-only, with a null handler. That
/// is the honest ceiling for the decoder in isolation; `--reconstruct` (M7)
/// will report decode-plus-book, which is the number that actually matters
/// and which will be lower.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/timestamp.hpp"

#include "feed/decoder.hpp"
#include "feed/mapped_file.hpp"

#include "tools/cli.hpp"

namespace {

using namespace miniex;

/// Tracks the time span and the symbol range without storing anything
/// per-message.
struct SpanHandler {
  TsNanos first_ts{0};
  TsNanos last_ts{0};
  bool seen{false};
  std::uint32_t max_locate{0};
  std::uint64_t adds{0};
  std::uint64_t cancels{0};
  std::uint64_t deletes{0};
  std::uint64_t replaces{0};
  std::uint64_t executes{0};
  std::uint64_t trades{0};
  std::uint64_t crosses{0};

  void operator()(const NormalizedEvent& e) noexcept {
    if (!seen) {
      first_ts = e.ts;
      seen = true;
    }
    last_ts = e.ts;
    max_locate = std::max<std::uint32_t>(max_locate, e.symbol);
    switch (e.action) {
    case MdAction::Add: ++adds; break;
    case MdAction::Cancel: ++cancels; break;
    case MdAction::Delete: ++deletes; break;
    case MdAction::Replace: ++replaces; break;
    case MdAction::Execute:
    case MdAction::ExecuteWithPrice: ++executes; break;
    case MdAction::Trade: ++trades; break;
    case MdAction::Cross: ++crosses; break;
    default: break;
    }
  }
};

const char* type_name(char t) {
  switch (t) {
  case 'S': return "System Event";
  case 'R': return "Stock Directory";
  case 'H': return "Stock Trading Action";
  case 'Y': return "Reg SHO Restriction";
  case 'L': return "Market Participant Position";
  case 'V': return "MWCB Decline Level";
  case 'W': return "MWCB Status";
  case 'K': return "IPO Quoting Period";
  case 'J': return "LULD Auction Collar";
  case 'h': return "Operational Halt";
  case 'A': return "Add Order";
  case 'F': return "Add Order w/ MPID";
  case 'E': return "Order Executed";
  case 'C': return "Order Executed w/ Price";
  case 'X': return "Order Cancel";
  case 'D': return "Order Delete";
  case 'U': return "Order Replace";
  case 'P': return "Trade (non-cross)";
  case 'Q': return "Cross Trade";
  case 'B': return "Broken Trade";
  case 'I': return "NOII";
  case 'N': return "RPII";
  default: return "unknown";
  }
}

void print_time(TsNanos ns) {
  const std::int64_t total_seconds = ns / 1'000'000'000;
  std::printf("%02lld:%02lld:%02lld.%09lld",
              static_cast<long long>(total_seconds / 3600),
              static_cast<long long>((total_seconds / 60) % 60),
              static_cast<long long>(total_seconds % 60),
              static_cast<long long>(ns % 1'000'000'000));
}

} // namespace

int main(int argc, char** argv) {
  Cli cli("itch_stats",
          "message-type histogram, time span and decode throughput for an "
          "ITCH 5.0 file");
  cli.option("file", "the ITCH file to read (BinaryFILE framing)", "", true);
  cli.option("max", "stop after this many messages (0 = all)", "0");
  cli.option("dispatch", "switch | table", "switch");
  cli.flag("strict", "treat an unknown message type as an error");
  if (!cli.parse(argc, argv)) return 2;

  const CycleCalibration cal(100);
  Cli::print_provenance(&cal);

  MappedFile file;
  if (!file.open(cli.get("file"))) {
    std::fprintf(stderr, "itch_stats: %s\n", file.error().c_str());
    return 2;
  }

  SpanHandler handler;
  ItchDecoder decoder(file.data(), file.size(), handler);
  if (cli.get("dispatch") == "table")
    decoder.set_dispatch(DispatchMode::Table);
  else if (cli.get("dispatch") != "switch") {
    std::fprintf(stderr, "itch_stats: --dispatch must be switch or table\n");
    return 2;
  }

  const auto max = static_cast<std::size_t>(cli.get_int("max", 0));
  const Cycles t0 = now_cycles();
  const std::size_t decoded =
      decoder.decode(max == 0 ? ~std::size_t{0} : max, cli.has("strict"));
  const std::int64_t elapsed_ns = cal.cycles_to_ns(now_cycles() - t0);

  const MessageCounts& counts = decoder.counts();
  std::printf("\nfile            %s\n", cli.get("file").c_str());
  std::printf("size            %.2f MB\n",
              static_cast<double>(file.size()) / (1024.0 * 1024.0));
  std::printf("messages        %llu\n",
              static_cast<unsigned long long>(counts.total));
  if (handler.seen) {
    std::printf("time span       ");
    print_time(handler.first_ts);
    std::printf("  ..  ");
    print_time(handler.last_ts);
    std::printf("\n");
  }
  std::printf("stock locates   0 .. %u\n", handler.max_locate);

  std::printf("\nmessage types\n");
  std::vector<std::pair<char, std::uint64_t>> rows;
  for (int i = 0; i < 256; ++i) {
    if (counts.by_type[i] != 0)
      rows.emplace_back(static_cast<char>(i), counts.by_type[i]);
  }
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  for (const auto& [type, n] : rows) {
    std::printf("  %c  %-28s %12llu  %6.2f%%%s\n", type, type_name(type),
                static_cast<unsigned long long>(n),
                100.0 * static_cast<double>(n) /
                    static_cast<double>(counts.total == 0 ? 1 : counts.total),
                itch_mutates_book(type) ? "  <- mutates the book" : "");
  }

  // The empirical command mix, which is what the benchmark should be
  // configured from. Note it is a *message* mix, not a command mix: an ITCH
  // cancel usually reduces an order rather than removing it, and a delete is
  // the one that removes. tools/flow_profile turns this into a command mix.
  std::printf("\nbook activity\n");
  std::printf("  adds        %12llu\n",
              static_cast<unsigned long long>(handler.adds));
  std::printf("  cancels     %12llu  (partial reductions)\n",
              static_cast<unsigned long long>(handler.cancels));
  std::printf("  deletes     %12llu  (full removals)\n",
              static_cast<unsigned long long>(handler.deletes));
  std::printf("  replaces    %12llu\n",
              static_cast<unsigned long long>(handler.replaces));
  std::printf("  executions  %12llu\n",
              static_cast<unsigned long long>(handler.executes));
  std::printf("  hidden prints %10llu\n",
              static_cast<unsigned long long>(handler.trades));
  std::printf("  crosses     %12llu\n",
              static_cast<unsigned long long>(handler.crosses));

  if (elapsed_ns > 0) {
    const double seconds = static_cast<double>(elapsed_ns) / 1e9;
    std::printf("\ndecode throughput (null handler, dispatch=%s)\n",
                cli.get("dispatch").c_str());
    std::printf("  %.2f s\n", seconds);
    std::printf("  %.2f million messages/s\n",
                static_cast<double>(decoded) / seconds / 1e6);
    std::printf("  %.1f MB/s\n",
                static_cast<double>(file.size()) / seconds / (1024.0 * 1024.0));
    if (cal.coarse()) {
      std::printf(
          "  (timing source resolves to %lld ns; throughput over a whole file "
          "is\n   still meaningful at this granularity, per-message latency is "
          "not)\n",
          static_cast<long long>(cal.cycles_to_ns(1)));
    }
  }

  if (!decoder.failure().ok()) {
    std::fprintf(stderr, "\nitch_stats: decode stopped: %s\n",
                 decoder.failure().what().c_str());
    return 1;
  }
  if (decoder.unknown_types() > 0) {
    std::fprintf(stderr,
                 "\nitch_stats: %zu messages had a type this decoder does not "
                 "model (skipped; use --strict to make this an error)\n",
                 decoder.unknown_types());
  }
  return 0;
}
