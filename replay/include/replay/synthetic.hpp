/// \file synthetic.hpp
/// \brief Seeded synthetic order flow, for benchmarking and property testing.
///
/// Two rules govern this file.
///
/// **Generation happens ahead of time, into a preallocated buffer.** Never
/// inside a measured loop. A generator called from the benchmark's inner loop
/// measures the generator, and a random-number call is comfortably more
/// expensive than a cancel.
///
/// **The flow must look like flow.** Real equity flow is overwhelmingly adds
/// and cancels; a mix with 50% aggressive orders benchmarks a fiction, and
/// benchmarks a matching engine on the one path that is *least* sensitive to
/// the book's data structures. The default mix here is a placeholder until
/// `tools/flow_profile` measures a real one from an ITCH day (M8); the
/// measured mix then replaces it and both are published.
///
/// Depth is placed with a geometric tail from the touch rather than
/// uniformly, so the book has the shape a book has: most size near the touch,
/// thinning out behind. Cancels are biased towards recently-added orders,
/// which produces the heavy-tailed order lifetimes real flow shows -- most
/// orders die young, a few live all day -- without having to model lifetimes
/// explicitly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/types.hpp"

#include "engine/messages.hpp"
#include "replay/random.hpp"

namespace miniex {

/// Command mix, in parts per hundred. Need not sum to 100; the generator
/// normalises, which means a caller can write `--mix add=30,cancel=55` and
/// get sensible behaviour.
///
/// **A mix is only meaningful if it can sustain a book.** Additions
/// (`add` + the resting part of `aggressive`) must at least keep pace with
/// removals (`cancel`), or the live order set drains to nothing within a few
/// thousand commands and every subsequent cancel becomes an add by
/// substitution. The measured consequence is stark: with 30/55/5/10 this
/// generator settles at a live set of single digits, a median order lifetime
/// of 3 commands, and a realised cancel share far below the 55 requested --
/// which is a benchmark of an empty book, not of an order book.
///
/// The default below is therefore add-heavy. `tools/flow_profile` (M8) will
/// replace it with a mix measured from a real ITCH day, and both will be
/// published; note that the ITCH message mix is *not* directly a command mix,
/// because an ITCH cancel (`X`) usually reduces an order rather than removing
/// it, and a delete (`D`) is the one that removes.
struct FlowMix {
  std::uint32_t add{45};
  std::uint32_t cancel{40};
  std::uint32_t replace{10};
  std::uint32_t aggressive{5};

  [[nodiscard]] std::uint32_t total() const noexcept {
    return add + cancel + replace + aggressive;
  }
};

/// Pathological shapes, available on request. Each one exists because it is
/// a worst case for some part of the implementation, and a benchmark suite
/// that only ever runs the pleasant case is marketing.
enum class FlowPattern : std::uint8_t {
  /// The default: a normal-looking two-sided book.
  Balanced,
  /// Thousands of resting orders spread far from the touch. Stresses level
  /// iteration, the FOK pre-scan, and cache residency of the ladder.
  DeepBook,
  /// Almost all activity at the best price: add, cancel, add, cancel.
  /// Stresses the best-price tracker, which has to re-find the touch every
  /// time the level empties.
  TouchChurn,
  /// A very wide spread with orders far out. Stresses the search for the
  /// next occupied level -- the case a linear ladder scan is worst at.
  WideSpread,
  /// Prices deliberately outside the resident ladder window, forcing the
  /// overflow path. Present so the overflow path is measured, not just
  /// implemented.
  LadderOverflow,
  /// Adds far outnumber cancels, so the order pool fills up. Exercises
  /// exhaustion as a normal condition.
  PoolPressure,
};

constexpr std::string_view to_string(FlowPattern p) noexcept {
  switch (p) {
  case FlowPattern::Balanced: return "balanced";
  case FlowPattern::DeepBook: return "deep-book";
  case FlowPattern::TouchChurn: return "touch-churn";
  case FlowPattern::WideSpread: return "wide-spread";
  case FlowPattern::LadderOverflow: return "ladder-overflow";
  case FlowPattern::PoolPressure: return "pool-pressure";
  }
  return "unknown";
}

/// Parses `--pattern` values. Returns false on an unknown name.
inline bool parse_pattern(std::string_view name, FlowPattern& out) noexcept {
  if (name == "balanced")
    out = FlowPattern::Balanced;
  else if (name == "deep-book")
    out = FlowPattern::DeepBook;
  else if (name == "touch-churn")
    out = FlowPattern::TouchChurn;
  else if (name == "wide-spread")
    out = FlowPattern::WideSpread;
  else if (name == "ladder-overflow")
    out = FlowPattern::LadderOverflow;
  else if (name == "pool-pressure")
    out = FlowPattern::PoolPressure;
  else
    return false;
  return true;
}

struct FlowConfig {
  std::uint64_t seed{1};
  FlowMix mix{};
  FlowPattern pattern{FlowPattern::Balanced};

  /// The notional price the book is centred on, in ticks.
  Price base_price{1002000};
  /// How far the mid is allowed to wander from `base_price`, in ticks.
  std::uint32_t mid_drift_ticks{200};
  /// Probability, in percent, that the mid moves a tick on any command.
  std::uint32_t mid_move_percent{2};

  Qty min_qty{1};
  Qty max_qty{1000};

  /// Depth away from the touch is `geometric(depth_continue_percent,
  /// depth_max_steps) * depth_scale_ticks + uniform(0, depth_scale_ticks)`.
  ///
  /// The scale factor exists because a pure coin-flip geometric cannot reach
  /// far: at 97% continue the expected draw is only ~32 steps, so a
  /// "wide spread" pattern asking for 20000 ticks would quietly produce 500,
  /// and the loop would cost one iteration per tick of depth. Scaling the
  /// step size gives a genuinely long tail at a bounded, small cost, and
  /// keeps every draw in integer arithmetic -- `log()` is not
  /// bit-reproducible across platforms, and reproducible flow is the whole
  /// point of the seed.
  std::uint32_t depth_continue_percent{55};
  std::uint32_t depth_max_steps{60};
  std::uint32_t depth_scale_ticks{1};

  /// Probability, in percent, that a cancel or replace targets a *recent*
  /// order rather than a uniformly chosen live one. The mixture is what
  /// makes order lifetimes heavy-tailed: the recency arm kills most orders
  /// within a few commands, and the uniform arm reaches back to orders that
  /// have been resting a long time. Pure recency gives a p99 only ~18x the
  /// median; real flow is far more skewed than that.
  std::uint32_t target_recent_percent{85};

  /// Number of distinct participants; 0 means every order is unattributed
  /// (participant 0) and self-trade prevention never fires.
  std::uint32_t participants{4};

  /// Fraction of aggressive orders sent as IOC rather than Day.
  std::uint32_t aggressive_ioc_percent{40};
  /// Fraction of new orders sent as PostOnly or FOK; kept small because real
  /// flow is mostly plain limit orders.
  std::uint32_t exotic_tif_percent{4};

  /// Passive adds emitted before the mix takes over, to start from a book
  /// with depth rather than an empty one. The benchmark's cold-vs-warm
  /// comparison (spec 18.6.5) is exactly this knob at 0 and at 10000.
  std::size_t warmup_adds{0};
};

/// Generates a fixed sequence of commands. Construct once, call `generate`
/// once, then feed the buffer to whatever is being measured.
class SyntheticFlow {
public:
  explicit SyntheticFlow(FlowConfig cfg)
      : cfg_(cfg), rng_(cfg.seed), mid_(cfg.base_price) {
    apply_pattern();
  }

  /// Emits `warmup_adds` passive adds. Counted in `realised()` like any
  /// other command, so a published mix always reflects what was actually
  /// sent, warm-up included.
  void warm_up(std::vector<Command>& out) {
    out.reserve(out.size() + cfg_.warmup_adds);
    for (std::size_t i = 0; i < cfg_.warmup_adds; ++i) {
      ++ts_;
      out.push_back(make_passive());
    }
  }

  /// Appends \p count commands to \p out. Reserves up front so that the
  /// caller's buffer is allocated exactly once.
  void generate(std::vector<Command>& out, std::size_t count) {
    out.reserve(out.size() + count);
    for (std::size_t i = 0; i < count; ++i) out.push_back(next());
  }

  [[nodiscard]] std::vector<Command> generate(std::size_t count) {
    std::vector<Command> out;
    generate(out, count);
    return out;
  }

  /// Counts of what was actually produced, so a benchmark can publish the
  /// realised mix rather than the requested one -- they differ, because a
  /// cancel cannot be produced when nothing is live.
  struct Realised {
    std::uint64_t adds{0};
    std::uint64_t aggressives{0};
    std::uint64_t cancels{0};
    std::uint64_t replaces{0};
  };
  [[nodiscard]] const Realised& realised() const noexcept { return realised_; }
  [[nodiscard]] const FlowConfig& config() const noexcept { return cfg_; }

private:
  void apply_pattern() {
    switch (cfg_.pattern) {
    case FlowPattern::Balanced: break;
    case FlowPattern::DeepBook:
      cfg_.mix = FlowMix{80, 10, 5, 5};
      cfg_.depth_continue_percent = 85;
      cfg_.depth_max_steps = 60;
      cfg_.depth_scale_ticks = 8;
      break;
    case FlowPattern::TouchChurn:
      cfg_.mix = FlowMix{45, 45, 5, 5};
      cfg_.depth_continue_percent = 2;
      cfg_.depth_max_steps = 2;
      cfg_.depth_scale_ticks = 1;
      cfg_.mid_move_percent = 0;
      break;
    case FlowPattern::WideSpread:
      cfg_.mix = FlowMix{60, 30, 5, 5};
      cfg_.depth_continue_percent = 88;
      cfg_.depth_max_steps = 60;
      cfg_.depth_scale_ticks = 250;
      break;
    case FlowPattern::LadderOverflow:
      // Deliberately reaches well past the default resident window
      // (kLadderTicks = 131072), so a real fraction of orders land in the
      // overflow map rather than a token few.
      cfg_.mix = FlowMix{70, 20, 5, 5};
      cfg_.depth_continue_percent = 92;
      cfg_.depth_max_steps = 60;
      cfg_.depth_scale_ticks = 4000;
      break;
    case FlowPattern::PoolPressure:
      // A static mid, so passive orders never cross and every add sticks.
      // With a drifting mid the book keeps trading itself out and the pool
      // never actually fills, which would make this pattern a no-op.
      cfg_.mix = FlowMix{95, 2, 3, 0};
      cfg_.mid_move_percent = 0;
      break;
    }
  }

  Command next() {
    ++ts_;
    drift_mid();

    const std::uint32_t total = cfg_.mix.total();
    const std::uint32_t roll = total == 0 ? 0 : rng_.below(total);
    std::uint32_t threshold = cfg_.mix.add;

    if (roll < threshold || live_.empty()) return make_passive();
    threshold += cfg_.mix.aggressive;
    if (roll < threshold) return make_aggressive();
    threshold += cfg_.mix.cancel;
    if (roll < threshold) return make_cancel();
    return make_replace();
  }

  void drift_mid() {
    if (cfg_.mid_move_percent == 0) return;
    if (!rng_.chance(cfg_.mid_move_percent)) return;
    const Price step = rng_.chance(50) ? 1 : -1;
    const Price moved = mid_ + step;
    const auto drift = static_cast<Price>(cfg_.mid_drift_ticks);
    if (moved >= cfg_.base_price - drift && moved <= cfg_.base_price + drift)
      mid_ = moved;
  }

  [[nodiscard]] Qty draw_qty() noexcept {
    return static_cast<Qty>(rng_.between(cfg_.min_qty, cfg_.max_qty));
  }

  [[nodiscard]] ParticipantId draw_participant() noexcept {
    if (cfg_.participants == 0) return 0;
    // Participant 0 is "unattributed" and disables self-trade prevention, so
    // generated participants start at 1.
    return static_cast<ParticipantId>(1 + rng_.below(cfg_.participants));
  }

  [[nodiscard]] Price passive_price(Side side) noexcept {
    const std::uint32_t steps =
        rng_.geometric(cfg_.depth_continue_percent, cfg_.depth_max_steps);
    const auto depth = static_cast<Price>(static_cast<std::uint64_t>(steps) *
                                              cfg_.depth_scale_ticks +
                                          rng_.below(cfg_.depth_scale_ticks));
    const Price price = side == Side::Buy ? mid_ - 1 - depth : mid_ + 1 + depth;
    return price < 1 ? 1 : price;
  }

  Command make_passive() {
    NewOrder n{};
    n.id = next_id_++;
    n.side = rng_.chance(50) ? Side::Buy : Side::Sell;
    n.price = passive_price(n.side);
    n.qty = draw_qty();
    n.participant = draw_participant();
    n.tif = TimeInForce::Day;
    if (rng_.chance(cfg_.exotic_tif_percent))
      n.tif = rng_.chance(50) ? TimeInForce::PostOnly : TimeInForce::FOK;
    remember_if_it_can_rest(n.id, n.tif);
    ++realised_.adds;
    return Command::make_new(ts_, n);
  }

  Command make_aggressive() {
    NewOrder n{};
    n.id = next_id_++;
    n.side = rng_.chance(50) ? Side::Buy : Side::Sell;
    // Reach across the spread by a few ticks so it takes one to three levels
    // rather than always exactly one.
    const auto reach = static_cast<Price>(rng_.geometric(45, 6));
    n.price = n.side == Side::Buy ? mid_ + 1 + reach : mid_ - 1 - reach;
    if (n.price < 1) n.price = 1;
    n.qty = draw_qty();
    n.participant = draw_participant();
    n.tif = rng_.chance(cfg_.aggressive_ioc_percent) ? TimeInForce::IOC
                                                     : TimeInForce::Day;
    remember_if_it_can_rest(n.id, n.tif);
    ++realised_.aggressives;
    return Command::make_new(ts_, n);
  }

  Command make_cancel() {
    const OrderId id = take_recent();
    ++realised_.cancels;
    return Command::make_cancel(ts_, id);
  }

  Command make_replace() {
    const std::size_t k = pick_recent_index();
    const OrderId id = live_[k];
    const Side side = rng_.chance(50) ? Side::Buy : Side::Sell;
    ++realised_.replaces;
    return Command::make_replace(ts_, id, passive_price(side), draw_qty());
  }

  /// An IOC or FOK order can never rest, so cancelling one later would be
  /// guaranteed to reject. Tracking only orders that *can* rest keeps the
  /// generated cancels aimed at real resting liquidity, which is what makes
  /// the cancel path -- the one that dominates real flow -- actually get
  /// benchmarked rather than the unknown-id reject path.
  void remember_if_it_can_rest(OrderId id, TimeInForce tif) {
    if (tif == TimeInForce::Day || tif == TimeInForce::PostOnly)
      live_.push_back(id);
  }

  /// Picks a live order to cancel or replace: usually a recent one, but
  /// sometimes any of them.
  ///
  /// This mixture is what makes order lifetimes heavy-tailed without
  /// modelling lifetimes explicitly. The recency arm kills most orders
  /// within a handful of commands; the uniform arm occasionally reaches an
  /// order that has been resting since the start. Pure recency is not
  /// enough -- measured on this generator it gives a p99 lifetime only about
  /// 18x the median, where real flow is far more skewed.
  [[nodiscard]] std::size_t pick_recent_index() noexcept {
    const auto n = static_cast<std::uint32_t>(live_.size());
    if (!rng_.chance(cfg_.target_recent_percent)) return rng_.below(n);
    const std::uint32_t back = rng_.geometric(70, n > 1 ? n - 1 : 0);
    return live_.size() - 1 - back;
  }

  OrderId take_recent() {
    const std::size_t k = pick_recent_index();
    const OrderId id = live_[k];
    live_[k] = live_.back();
    live_.pop_back();
    return id;
  }

  FlowConfig cfg_;
  Rng rng_;
  Price mid_;
  TsNanos ts_{0};
  OrderId next_id_{1};
  std::vector<OrderId> live_;
  Realised realised_{};
};

} // namespace miniex
