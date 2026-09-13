/// \file flow_profile.cpp
/// \brief The empirical shape of real order flow.
///
/// The benchmark's flow mix should come from measurement, not invention. Real
/// equity flow is overwhelmingly adds and cancels; benchmarking a matching
/// engine on 50% aggressive orders benchmarks a fiction, and benchmarks the
/// one path that is *least* sensitive to the book's data structures.
///
/// Reports, for one symbol:
///
/// * the **message** mix (what ITCH sent) and the **command** mix (what an
///   order entry gateway would have had to send to produce it) -- these are
///   not the same thing and conflating them is the usual error;
/// * the distribution of order lifetimes, in time and in intervening
///   messages, which is what makes cancels hit recent orders;
/// * the fraction of adds that ever trade at all;
/// * the depth at which orders are placed relative to the touch.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/timestamp.hpp"

#include "book/md_book.hpp"
#include "feed/decoder.hpp"
#include "feed/mapped_file.hpp"

#include "tools/cli.hpp"

namespace {

using namespace miniex;

struct Lifetime {
  TsNanos added_ts{0};
  std::size_t added_index{0};
  Price price{0};
  Qty remaining{0};
  Side side{Side::Buy};
  bool traded{false};
};

struct Profile {
  MarketDataBook book;
  SymbolId symbol{1};
  std::size_t index{0};

  // Message counts, as ITCH sent them.
  std::uint64_t adds{0};
  std::uint64_t executes{0};
  std::uint64_t partial_cancels{0}; // 'X' -- a reduction
  std::uint64_t deletes{0};         // 'D' -- a full removal
  std::uint64_t replaces{0};        // 'U'
  std::uint64_t hidden{0};
  std::uint64_t cross_messages{0}; // 'Q'

  /// Per-order state. The reconstruction book does not expose a single
  /// order's remaining quantity -- it aggregates -- and the profiler needs it
  /// to know exactly when an order's life ends. Keeping it here rather than
  /// widening MarketDataBook's interface keeps the profiling concern out of
  /// the book.
  std::unordered_map<OrderId, Lifetime> live;

  std::vector<std::int64_t> lifetime_ns;
  std::vector<std::size_t> lifetime_messages;
  std::uint64_t adds_that_traded{0};
  std::uint64_t adds_completed{0};
  std::vector<std::int64_t> depth_ticks;
  std::uint64_t aggressive_adds{0}; ///< adds that crossed the opposite side

  void operator()(const NormalizedEvent& e) {
    if (e.symbol != symbol) return;
    ++index;

    switch (e.action) {
    case MdAction::Add: {
      // Depth relative to the touch, measured *before* the add lands.
      const Price opposite_best = book.best(opposite(e.side));
      const Price own_best = book.best(e.side);
      if (opposite_best != kInvalidPrice &&
          miniex::crosses(e.side, e.price, opposite_best))
        ++aggressive_adds;
      if (own_best != kInvalidPrice) {
        const std::int64_t depth =
            e.side == Side::Buy ? own_best - e.price : e.price - own_best;
        depth_ticks.push_back(depth);
      }
      (void)book.add(e.ref, e.side, e.price, e.qty);
      ++adds;
      live[e.ref] = Lifetime{e.ts, index, e.price, e.qty, e.side, false};
      break;
    }
    case MdAction::Execute:
    case MdAction::ExecuteWithPrice: {
      ++executes;
      if (e.action == MdAction::Execute)
        (void)book.execute(e.ref, e.qty);
      else
        (void)book.execute_with_price(e.ref, e.qty, e.price, e.printable);
      mark_traded(e.ref);
      reduce(e.ref, e.qty, e.ts);
      break;
    }
    case MdAction::Cancel:
      ++partial_cancels;
      (void)book.cancel(e.ref, e.qty);
      reduce(e.ref, e.qty, e.ts);
      break;
    case MdAction::Delete:
      ++deletes;
      (void)book.remove(e.ref);
      retire(e.ref, e.ts);
      break;
    case MdAction::Replace: {
      ++replaces;
      const auto it = live.find(e.ref);
      const bool traded = it != live.end() && it->second.traded;
      const Side side = it != live.end() ? it->second.side : Side::Buy;
      retire(e.ref, e.ts);
      (void)book.replace(e.ref, e.new_ref, e.qty, e.price);
      live[e.new_ref] = Lifetime{e.ts, index, e.price, e.qty, side, traded};
      break;
    }
    case MdAction::Trade:
      ++hidden;
      book.hidden_print(e.price, e.qty);
      break;
    case MdAction::Cross:
      ++cross_messages;
      book.cross_print(e.price, e.qty);
      break;
    default: break;
    }
  }

  void mark_traded(OrderId ref) {
    const auto it = live.find(ref);
    if (it != live.end()) it->second.traded = true;
  }

  /// Applies a reduction, closing the order's lifetime if it reaches zero.
  void reduce(OrderId ref, Qty qty, TsNanos ts) {
    const auto it = live.find(ref);
    if (it == live.end()) return;
    if (qty >= it->second.remaining) {
      retire(ref, ts);
    } else {
      it->second.remaining = static_cast<Qty>(it->second.remaining - qty);
    }
  }

  void retire(OrderId ref, TsNanos ts) {
    const auto it = live.find(ref);
    if (it == live.end()) return;
    lifetime_ns.push_back(ts - it->second.added_ts);
    lifetime_messages.push_back(index - it->second.added_index);
    ++adds_completed;
    if (it->second.traded) ++adds_that_traded;
    live.erase(it);
  }
};

template <class T>
void print_percentiles(const char* label, std::vector<T> values,
                       const char* unit) {
  if (values.empty()) {
    std::printf("  %-22s (no samples)\n", label);
    return;
  }
  std::sort(values.begin(), values.end());
  const auto at = [&](double p) {
    const std::size_t i = static_cast<std::size_t>(
        p / 100.0 * static_cast<double>(values.size() - 1));
    return static_cast<long long>(values[i]);
  };
  std::printf("  %-22s p10=%lld p50=%lld p90=%lld p99=%lld max=%lld %s\n",
              label, at(10), at(50), at(90), at(99), at(100), unit);
}

} // namespace

int main(int argc, char** argv) {
  Cli cli("flow_profile",
          "measure the empirical flow mix and order-lifetime distribution of "
          "an ITCH file");
  cli.option("file", "the ITCH file to read", "", true);
  cli.option("locate", "stock locate to profile", "1");
  if (!cli.parse(argc, argv)) return 2;

  const CycleCalibration cal(100);
  Cli::print_provenance(&cal);

  MappedFile file;
  if (!file.open(cli.get("file"))) {
    std::fprintf(stderr, "flow_profile: %s\n", file.error().c_str());
    return 2;
  }

  Profile profile;
  profile.symbol = static_cast<SymbolId>(cli.get_int("locate", 1));
  ItchDecoder decoder(file.data(), file.size(), profile);
  const std::size_t decoded = decoder.decode(~std::size_t{0}, /*strict=*/false);
  if (!decoder.failure().ok()) {
    std::fprintf(stderr, "flow_profile: decode stopped: %s\n",
                 decoder.failure().what().c_str());
    return 1;
  }

  const std::uint64_t total = profile.adds + profile.executes +
                              profile.partial_cancels + profile.deletes +
                              profile.replaces + profile.hidden +
                              profile.cross_messages;
  const auto pct = [&](std::uint64_t n) {
    return total == 0
               ? 0.0
               : 100.0 * static_cast<double>(n) / static_cast<double>(total);
  };

  std::printf("\nstock locate %u, %zu messages decoded, %llu for this symbol\n",
              profile.symbol, decoded, (unsigned long long)total);

  std::printf("\nITCH message mix (what the feed sent)\n");
  std::printf("  A/F add               %10llu  %6.2f%%\n",
              (unsigned long long)profile.adds, pct(profile.adds));
  std::printf("  X   cancel (partial)  %10llu  %6.2f%%\n",
              (unsigned long long)profile.partial_cancels,
              pct(profile.partial_cancels));
  std::printf("  D   delete (full)     %10llu  %6.2f%%\n",
              (unsigned long long)profile.deletes, pct(profile.deletes));
  std::printf("  U   replace           %10llu  %6.2f%%\n",
              (unsigned long long)profile.replaces, pct(profile.replaces));
  std::printf("  E/C execute           %10llu  %6.2f%%\n",
              (unsigned long long)profile.executes, pct(profile.executes));
  std::printf("  P   hidden print      %10llu  %6.2f%%\n",
              (unsigned long long)profile.hidden, pct(profile.hidden));
  std::printf("  Q   cross             %10llu  %6.2f%%\n",
              (unsigned long long)profile.cross_messages,
              pct(profile.cross_messages));

  // The command mix is what a benchmark should be configured from, and it is
  // NOT the message mix. Executions are not commands -- nobody sends them;
  // they are the engine's output. Hidden prints and crosses are not commands
  // either. Adds split into passive and aggressive by whether they crossed.
  const std::uint64_t commands = profile.adds + profile.partial_cancels +
                                 profile.deletes + profile.replaces;
  const auto cpct = [&](std::uint64_t n) {
    return commands == 0
               ? 0.0
               : 100.0 * static_cast<double>(n) / static_cast<double>(commands);
  };
  const std::uint64_t passive_adds =
      profile.adds > profile.aggressive_adds
          ? profile.adds - profile.aggressive_adds
          : 0;

  std::printf(
      "\nderived COMMAND mix (what an order gateway would have sent)\n");
  std::printf("  add (passive)         %10llu  %6.2f%%\n",
              (unsigned long long)passive_adds, cpct(passive_adds));
  std::printf("  add (aggressive)      %10llu  %6.2f%%\n",
              (unsigned long long)profile.aggressive_adds,
              cpct(profile.aggressive_adds));
  std::printf("  cancel (reduce)       %10llu  %6.2f%%\n",
              (unsigned long long)profile.partial_cancels,
              cpct(profile.partial_cancels));
  std::printf("  cancel (remove)       %10llu  %6.2f%%\n",
              (unsigned long long)profile.deletes, cpct(profile.deletes));
  std::printf("  replace               %10llu  %6.2f%%\n",
              (unsigned long long)profile.replaces, cpct(profile.replaces));
  std::printf("\n  engine_bench --mix add=%.0f,cancel=%.0f,replace=%.0f,"
              "aggressive=%.0f\n",
              cpct(passive_adds),
              cpct(profile.partial_cancels + profile.deletes),
              cpct(profile.replaces), cpct(profile.aggressive_adds));

  std::printf("\norder lifetimes (completed orders only)\n");
  print_percentiles("time to completion", profile.lifetime_ns, "ns");
  print_percentiles("messages survived", profile.lifetime_messages, "messages");
  std::printf(
      "  %-22s %llu of %llu completed (%.2f%%)\n", "adds that ever traded",
      (unsigned long long)profile.adds_that_traded,
      (unsigned long long)profile.adds_completed,
      profile.adds_completed == 0 ? 0.0
                                  : 100.0 * (double)profile.adds_that_traded /
                                        (double)profile.adds_completed);
  std::printf("  %-22s %zu\n", "still live at the end", profile.live.size());

  std::printf("\nplacement depth relative to the same side's touch\n");
  print_percentiles("ticks from touch", profile.depth_ticks, "ticks");
  std::printf("  %-22s %llu of %llu adds (%.2f%%)\n", "crossed on arrival",
              (unsigned long long)profile.aggressive_adds,
              (unsigned long long)profile.adds,
              profile.adds == 0 ? 0.0
                                : 100.0 * (double)profile.aggressive_adds /
                                      (double)profile.adds);
  return 0;
}
