/// \file injector.hpp
/// \brief ITCH order flow into our own matching engine, and the accounting
///        that compares the result against what the exchange actually did.
///
/// ## Read this before reading the numbers
///
/// **ITCH is a market data feed. It is not an order entry stream.** It reports
/// what Nasdaq already matched. You therefore cannot "replay ITCH through a
/// matching engine" and expect identical fills, and any project claiming to is
/// either confused or lying.
///
/// What you *can* do, and what this does:
///
/// * treat the `A`/`F` add messages as an **order arrival process** and submit
///   them to our engine as fresh limit orders;
/// * apply `X`/`D`/`U` by reference, so the population of resting orders
///   tracks the real one;
/// * compare the trades **our** engine generates against the `E`/`C`/`P`
///   executions in the feed;
/// * characterise and explain every difference.
///
/// The last point is the deliverable. A divergence you can name and quantify
/// is a finding; an unexplained one is a bug. See `docs/divergence.md`.
///
/// ## The message mapping, and why it is not one-to-one
///
/// | ITCH | Injected as | Note |
/// |---|---|---|
/// | `A`/`F` | `NEW` limit/day, id = the ITCH order reference | |
/// | `X` | `REPLACE` to `open - cancelled` at the same price | ITCH cancels
/// *reduce*; our replace takes the new open quantity, so the injector must know
/// the current one | | `D` | `CANCEL` | | | `U` | `CANCEL old` then `NEW new` |
/// ITCH replace introduces a new reference number, and our replace keeps the
/// id. Cancel-then-add reproduces both the new id and the loss of time priority
/// | | `E`/`C` | **not injected** | these are the exchange's fills, and they
/// are the thing being compared against | | `P` | **not injected** | a
/// non-displayed execution against liquidity we never saw | | `Q` | **not
/// injected** | an auction cross our engine does not implement |
///
/// A `U` injected as cancel-then-add is *not* the same code path as
/// `on_replace`, and that is deliberate: ITCH's replace semantics (new
/// reference, back of the queue) are exactly cancel-then-add, whereas our
/// engine's `REPLACE` keeps the client's id. Using `on_replace` here would
/// keep the old reference alive and every later message for the new reference
/// would be an unknown order.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/types.hpp"

#include "engine/messages.hpp"
#include "feed/normalizer.hpp"

namespace miniex {

/// The row of spec 13.3, plus the supporting counts needed to compute it.
struct DivergenceReport {
  // --- what went in --------------------------------------------------------
  std::uint64_t adds_injected{0};
  std::uint64_t cancels_injected{0};
  std::uint64_t deletes_injected{0};
  std::uint64_t replaces_injected{0};
  std::uint64_t adds_rejected{0};

  // --- what our engine produced -------------------------------------------
  std::uint64_t engine_trades{0};
  std::uint64_t engine_shares{0};

  // --- what the feed reported ---------------------------------------------
  std::uint64_t feed_executions{0}; ///< `E` + `C` messages
  std::uint64_t feed_exec_shares{0};
  std::uint64_t feed_hidden_prints{0}; ///< `P`
  std::uint64_t feed_hidden_shares{0};
  std::uint64_t feed_crosses{0}; ///< `Q`
  std::uint64_t feed_cross_shares{0};

  // --- the comparison ------------------------------------------------------
  /// Feed execution shares for which our engine produced a trade against the
  /// same resting order at the same price.
  std::uint64_t matched_shares{0};
  /// Feed execution shares against a reference our engine never had -- the
  /// add happened before the window started, or the add was rejected.
  std::uint64_t unknown_reference_shares{0};
  /// Feed execution shares against an order our engine *did* have, but which
  /// our engine had already traded away or which it never traded at all.
  std::uint64_t unmatched_shares{0};
  /// Shares our engine traded that the feed never reported against that
  /// resting order.
  std::uint64_t engine_only_shares{0};

  /// Executions whose price did not agree even though the resting order and
  /// quantity did. This should be zero; a non-zero value is a bug, not a
  /// finding.
  std::uint64_t price_disagreements{0};

  [[nodiscard]] double matched_fraction() const {
    return feed_exec_shares == 0 ? 0.0
                                 : static_cast<double>(matched_shares) /
                                       static_cast<double>(feed_exec_shares);
  }
  /// Volume our engine structurally cannot produce, as a fraction of all
  /// trade volume in the feed.
  [[nodiscard]] double structurally_impossible_fraction() const {
    const std::uint64_t all =
        feed_exec_shares + feed_hidden_shares + feed_cross_shares;
    return all == 0
               ? 0.0
               : static_cast<double>(feed_hidden_shares + feed_cross_shares) /
                     static_cast<double>(all);
  }
  [[nodiscard]] double unexplained_fraction() const {
    return feed_exec_shares == 0 ? 0.0
                                 : static_cast<double>(unmatched_shares) /
                                       static_cast<double>(feed_exec_shares);
  }
};

/// One divergent case, kept for the write-ups `docs/divergence.md` requires.
struct DivergenceCase {
  std::size_t message_index{0};
  TsNanos ts{0};
  OrderId ref{0};
  Price feed_price{0};
  Qty feed_qty{0};
  Price engine_price{0};
  Qty engine_qty{0};
  const char* reason{""};
};

/// What the engine currently holds for a resting order. The injector asks for
/// it rather than shadowing the book with a second copy, which would drift.
struct RestingState {
  Price price{0};
  Qty qty{0};
  bool found{false};
};

/// Turns normalised ITCH events into engine commands and keeps the accounting
/// that compares the two.
///
/// Templated on the engine so it works over either book; the caller owns the
/// engine and feeds events in.
/// At namespace scope so it can be a default argument on the constructor.
struct InjectorConfig {
  /// Only inject this stock locate. 0 means every symbol, which is only
  /// useful for smoke tests -- a single book holding every symbol's orders is
  /// not a book.
  SymbolId symbol{1};
  /// Stop after this many injected commands (0 = no limit).
  std::size_t max_commands{0};
  /// How many divergent cases to keep for the write-up.
  std::size_t keep_cases{32};
};

class Injector {
public:
  using Config = InjectorConfig;

  explicit Injector(Config cfg = {}) : cfg_(cfg) {}

  /// Converts one normalised event into zero or more engine commands,
  /// appended to \p out. Returns false once the command limit is reached.
  ///
  /// \param resting  a callback returning the engine's current state for a
  ///        resting order. The injector needs it to turn an ITCH cancel (a
  ///        *reduction*) into our replace (an absolute new quantity), and it
  ///        needs the price because an ITCH cancel does not carry one.
  ///        Asking the engine is more honest than shadowing the book with a
  ///        second copy that can drift.
  template <class RestingFn>
  bool translate(const NormalizedEvent& e, std::vector<Command>& out,
                 RestingFn&& resting) {
    if (cfg_.symbol != 0 && e.symbol != cfg_.symbol) return true;
    if (cfg_.max_commands != 0 && emitted_ >= cfg_.max_commands) return false;

    switch (e.action) {
    case MdAction::Add: {
      NewOrder n{};
      n.id = e.ref;
      n.price = e.price;
      n.qty = e.qty;
      n.side = e.side;
      n.symbol = e.symbol;
      n.type = OrderType::Limit;
      n.tif = TimeInForce::Day;
      out.push_back(Command::make_new(e.ts, n));
      ++report_.adds_injected;
      ++emitted_;
      injected_refs_.insert(e.ref);
      break;
    }
    case MdAction::Cancel: {
      // ITCH 'X' reduces by `cancelled shares`; our replace takes the new
      // absolute open quantity, and the message carries no price.
      const RestingState state = resting(e.ref);
      if (!state.found) {
        // Our engine no longer has it -- it traded out earlier than the
        // exchange's did. That divergence is already counted on the
        // execution side; there is nothing to cancel here.
        break;
      }
      if (e.qty >= state.qty) {
        out.push_back(Command::make_cancel(e.ts, e.ref));
        ++report_.deletes_injected;
      } else {
        out.push_back(Command::make_replace(
            e.ts, e.ref, state.price, static_cast<Qty>(state.qty - e.qty)));
        ++report_.cancels_injected;
      }
      ++emitted_;
      break;
    }
    case MdAction::Delete:
      out.push_back(Command::make_cancel(e.ts, e.ref));
      ++report_.deletes_injected;
      ++emitted_;
      break;
    case MdAction::Replace: {
      // A new reference number and the back of the queue: cancel-then-add,
      // not our engine's REPLACE, which would keep the old id alive.
      out.push_back(Command::make_cancel(e.ts, e.ref));
      NewOrder n{};
      n.id = e.new_ref;
      n.price = e.price;
      n.qty = e.qty;
      n.side = side_of(e.ref);
      n.symbol = e.symbol;
      out.push_back(Command::make_new(e.ts, n));
      ++report_.replaces_injected;
      emitted_ += 2;
      injected_refs_.insert(e.new_ref);
      sides_[e.new_ref] = n.side;
      break;
    }
    default: break;
    }
    if (e.action == MdAction::Add) sides_[e.ref] = e.side;
    return true;
  }

  /// Records a trade our engine produced, so the feed's executions can be
  /// matched against it. Called with the engine's `Fill` events, which carry
  /// the *resting* order's id -- the same identity ITCH's `E`/`C` use.
  void on_engine_fill(OrderId resting_ref, Price price, Qty qty) {
    ++report_.engine_trades;
    report_.engine_shares += qty;
    pending_[resting_ref].push_back(Fill{price, qty});
  }

  /// Records a feed execution and tries to account for it.
  void on_feed_execution(std::size_t index, const NormalizedEvent& e) {
    ++report_.feed_executions;
    report_.feed_exec_shares += e.qty;

    if (injected_refs_.count(e.ref) == 0) {
      report_.unknown_reference_shares += e.qty;
      keep_case(DivergenceCase{index, e.ts, e.ref, e.price, e.qty, 0, 0,
                               "reference never injected (add fell outside "
                               "the window, or was rejected)"});
      return;
    }

    Qty remaining = e.qty;
    const auto it = pending_.find(e.ref);
    if (it != pending_.end()) {
      while (remaining > 0 && !it->second.empty()) {
        Fill& fill = it->second.front();
        // An 'E' carries no price of its own; only a 'C' does, and then it
        // must agree with where our engine traded.
        if (e.action == MdAction::ExecuteWithPrice && fill.price != e.price) {
          ++report_.price_disagreements;
          keep_case(DivergenceCase{index, e.ts, e.ref, e.price, e.qty,
                                   fill.price, fill.qty,
                                   "execution price disagrees"});
          break;
        }
        const Qty take = remaining < fill.qty ? remaining : fill.qty;
        report_.matched_shares += take;
        remaining -= take;
        fill.qty -= take;
        if (fill.qty == 0) it->second.pop_front();
      }
      if (it->second.empty()) pending_.erase(it);
    }

    if (remaining > 0) {
      report_.unmatched_shares += remaining;
      keep_case(DivergenceCase{
          index, e.ts, e.ref, e.price, remaining, 0, 0,
          "the exchange filled this resting order and our engine did not"});
    }
  }

  void on_feed_hidden(Qty qty) {
    ++report_.feed_hidden_prints;
    report_.feed_hidden_shares += qty;
  }
  void on_feed_cross(Qty qty) {
    ++report_.feed_crosses;
    report_.feed_cross_shares += qty;
  }
  void on_add_rejected() { ++report_.adds_rejected; }

  /// Sweeps up engine fills the feed never accounted for. Call once at the
  /// end.
  void finish() {
    for (const auto& [ref, fills] : pending_) {
      for (const Fill& f : fills) report_.engine_only_shares += f.qty;
    }
  }

  [[nodiscard]] const DivergenceReport& report() const { return report_; }
  [[nodiscard]] const std::vector<DivergenceCase>& cases() const {
    return cases_;
  }
  [[nodiscard]] std::size_t commands_emitted() const { return emitted_; }

private:
  struct Fill {
    Price price;
    Qty qty;
  };

  [[nodiscard]] Side side_of(OrderId ref) const {
    const auto it = sides_.find(ref);
    return it == sides_.end() ? Side::Buy : it->second;
  }

  void keep_case(DivergenceCase c) {
    if (cases_.size() < cfg_.keep_cases) cases_.push_back(c);
  }

  Config cfg_;
  DivergenceReport report_{};
  std::size_t emitted_{0};
  std::unordered_set<OrderId> injected_refs_;
  std::unordered_map<OrderId, Side> sides_;
  std::unordered_map<OrderId, std::deque<Fill>> pending_;
  std::vector<DivergenceCase> cases_;
};

} // namespace miniex
