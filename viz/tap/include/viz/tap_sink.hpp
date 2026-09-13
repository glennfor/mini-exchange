/// \file tap_sink.hpp
/// \brief The engine-side sink that feeds the shared-memory tap.
///
/// Compiled into the engine **only** under `-DMINIEX_TAP=ON`. With the flag
/// off this header is never included by anything the engine builds, and the
/// tap costs exactly nothing -- not a branch, not a byte.
///
/// The cost when it is on is one bounded store sequence per event, and it is
/// **measured and published** next to the tap-off numbers rather than
/// asserted to be negligible. That comparison exists to pre-empt the obvious
/// question: "doesn't your dashboard pollute the numbers?"
#pragma once

#include <cstdint>

#include "engine/messages.hpp"
#include "viz/shm_ring.hpp"

namespace miniex {

/// Copies each outbound event into the shared-memory ring.
///
/// Drops when the ring is full and counts the drops. The dashboard displays
/// the drop count, in red, because a silently-dropping dashboard that looks
/// fine is worse than no dashboard.
class TapSink {
public:
  TapSink() = default;

  bool open(const std::string& name, std::size_t capacity) {
    return writer_.create(name, capacity);
  }

  void emit(const Event& e) noexcept {
    TapRecord r;
    r.seq = e.seq;
    r.ts = e.ts;
    r.price = e.price;
    r.qty = e.qty;
    r.qty_remaining = e.qty_remaining;
    r.id = e.id;
    r.counter_id = e.counter_id;
    // The level index is what a depth ladder wants and what the engine has
    // cheaply to hand; the server does not have to reconstruct it from the
    // price and a base it would have to be told separately.
    r.level_index = level_index_;
    r.type = static_cast<std::uint8_t>(e.type);
    r.side = static_cast<std::uint8_t>(e.side);
    r.reason = static_cast<std::uint8_t>(e.reason);
    r.flags = e.flags;
    (void)writer_.write(r);
  }

  /// Set by the engine harness when it knows the ladder base, so records
  /// carry a level index the server can use directly.
  void set_level_index(std::uint32_t index) noexcept { level_index_ = index; }

  void publish_stats(const TapStats& s) noexcept { writer_.publish_stats(s); }

  [[nodiscard]] std::uint64_t produced() const { return writer_.produced(); }
  [[nodiscard]] std::uint64_t dropped() const { return writer_.dropped(); }
  [[nodiscard]] bool ready() const { return writer_.ready(); }
  [[nodiscard]] const std::string& error() const { return writer_.error(); }

private:
  ShmTapWriter writer_;
  std::uint32_t level_index_{0};
};

} // namespace miniex
