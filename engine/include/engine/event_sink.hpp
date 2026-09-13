/// \file event_sink.hpp
/// \brief Where outbound events go. One template parameter, four choices.
///
/// | Sink          | Purpose                          | Cost               |
/// |---------------|----------------------------------|--------------------|
/// | `NullSink`    | benchmark the engine alone       | compiles to nothing|
/// | `CountingSink`| sanity counters                  | one increment      |
/// | `VectorSink`  | tests: record everything          | allocates; test-only|
/// | `TapSink`     | shared-memory observability (M9) | one bounded store  |
///
/// The engine is templated on the sink rather than calling through a virtual
/// interface so that `NullSink`'s `emit` is deleted outright by the
/// optimiser. That matters: with a virtual sink, the benchmark measures the
/// call, not the match.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/messages.hpp"

namespace miniex {

/// Discards everything. Used for benchmarking the matching core in
/// isolation; with this sink the engine's outbound path costs nothing at all.
struct NullSink {
  void emit(const Event&) noexcept {}
};

/// Counts events by type. Enough to prove the engine did something without
/// paying for storage; used by throughput runs.
struct CountingSink {
  std::uint64_t total{0};
  std::uint64_t by_type[8]{};

  void emit(const Event& e) noexcept {
    ++total;
    ++by_type[static_cast<std::size_t>(e.type) & 7U];
  }
  [[nodiscard]] std::uint64_t count(EventType t) const noexcept {
    return by_type[static_cast<std::size_t>(t) & 7U];
  }
  void reset() noexcept {
    total = 0;
    for (auto& c : by_type) c = 0;
  }
};

/// Records every event for assertions. Allocates, and is therefore never
/// used in a benchmark build.
struct VectorSink {
  std::vector<Event> events;

  void emit(const Event& e) { events.push_back(e); }
  void clear() noexcept { events.clear(); }

  [[nodiscard]] std::size_t size() const noexcept { return events.size(); }
  [[nodiscard]] const Event& operator[](std::size_t i) const {
    return events[i];
  }
  [[nodiscard]] const Event& back() const { return events.back(); }

  /// Events of one type, in order. Test convenience.
  [[nodiscard]] std::vector<Event> of_type(EventType t) const {
    std::vector<Event> out;
    for (const Event& e : events)
      if (e.type == t) out.push_back(e);
    return out;
  }
  [[nodiscard]] std::size_t count(EventType t) const noexcept {
    std::size_t n = 0;
    for (const Event& e : events)
      if (e.type == t) ++n;
    return n;
  }
};

} // namespace miniex
