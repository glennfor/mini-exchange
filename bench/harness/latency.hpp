/// \file latency.hpp
/// \brief Latency recording and reporting. Read spec section 18 before
///        changing anything here.
///
/// Rules this file exists to enforce:
///
/// * **Never report a mean without a distribution.** `summary()` always
///   carries p50, p90, p99, p99.9, p99.99 and max together, and the CSV
///   carries the full percentile dump.
/// * **Never subtract the measurement overhead.** It is measured and
///   published so a reader can see the floor, and left in the numbers.
/// * **Never publish a number without provenance.** Every report carries the
///   build, the git SHA, the timing backend, the tap state, the flow mix and
///   the seed.
///
/// Recording uses HdrHistogram: constant memory, constant per-sample cost,
/// stated precision. The alternative -- push every sample into a vector and
/// sort at the end -- costs 8 bytes per sample and, worse, a reallocation
/// somewhere in the middle of the measured run, which shows up as a
/// latency spike that is the benchmark's own fault.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <hdr/hdr_histogram.h>

#include "core/timestamp.hpp"

namespace miniex {

/// A latency distribution in nanoseconds.
class LatencyHistogram {
public:
  /// 1 ns to 60 s, three significant figures. Three figures means the
  /// reported p99 is within 0.1% of the true p99, which is finer than the
  /// run-to-run variation of any real machine.
  LatencyHistogram() {
    if (hdr_init(1, 60LL * 1000 * 1000 * 1000, 3, &hist_) != 0) hist_ = nullptr;
  }
  ~LatencyHistogram() {
    if (hist_ != nullptr) hdr_close(hist_);
  }
  LatencyHistogram(const LatencyHistogram&) = delete;
  LatencyHistogram& operator=(const LatencyHistogram&) = delete;

  void record(std::int64_t ns) noexcept {
    if (hist_ == nullptr) return;
    hdr_record_value(hist_, ns < 1 ? 1 : ns);
  }

  [[nodiscard]] std::int64_t percentile(double p) const noexcept {
    return hist_ == nullptr ? 0 : hdr_value_at_percentile(hist_, p);
  }
  [[nodiscard]] std::int64_t max() const noexcept {
    return hist_ == nullptr ? 0 : hdr_max(hist_);
  }
  [[nodiscard]] std::int64_t min() const noexcept {
    return hist_ == nullptr ? 0 : hdr_min(hist_);
  }
  [[nodiscard]] double mean() const noexcept {
    return hist_ == nullptr ? 0.0 : hdr_mean(hist_);
  }
  [[nodiscard]] std::int64_t count() const noexcept {
    return hist_ == nullptr ? 0 : hist_->total_count;
  }
  [[nodiscard]] bool valid() const noexcept { return hist_ != nullptr; }

  /// The percentile points that every report must carry. There is no API to
  /// ask for p99 alone, on purpose.
  struct Summary {
    std::int64_t count{0};
    std::int64_t min{0};
    std::int64_t p50{0};
    std::int64_t p90{0};
    std::int64_t p99{0};
    std::int64_t p999{0};
    std::int64_t p9999{0};
    std::int64_t max{0};
    double mean{0.0};
  };

  [[nodiscard]] Summary summary() const noexcept {
    return Summary{count(),
                   min(),
                   percentile(50.0),
                   percentile(90.0),
                   percentile(99.0),
                   percentile(99.9),
                   percentile(99.99),
                   max(),
                   mean()};
  }

  /// Percentile / value pairs on a log-spaced ladder, for the CDF plot.
  /// This is the dump the README's latency chart is generated from.
  [[nodiscard]] std::vector<std::pair<double, std::int64_t>> cdf() const {
    std::vector<std::pair<double, std::int64_t>> out;
    static const double kPercentiles[] = {
        0.0,  10.0,  20.0,  30.0,   40.0,   50.0,    60.0,
        70.0, 80.0,  90.0,  95.0,   97.5,   99.0,    99.5,
        99.9, 99.95, 99.99, 99.995, 99.999, 99.9999, 100.0};
    for (double p : kPercentiles) out.emplace_back(p, percentile(p));
    return out;
  }

private:
  hdr_histogram* hist_{nullptr};
};

/// Everything that must accompany a number for it to mean anything.
struct RunProvenance {
  std::string build_type;
  std::string git_sha;
  std::string timing_backend;
  bool tap_on{false};
  std::uint64_t ticks_per_second{0};
  std::int64_t tick_ns{0};
  bool coarse_timer{false};
  std::int64_t timer_overhead_ns{0};
  std::string book;
  std::string sink;
  std::string pattern;
  std::string mix;
  std::uint64_t seed{0};
  std::size_t warmup_orders{0};
  std::size_t max_orders{0};
  std::size_t ladder_ticks{0};
  std::size_t ring_capacity{0};
};

/// Counters that must be reported next to the percentiles, because a
/// beautiful distribution with 40% schedule misses is a saturated run, not a
/// latency result.
struct RunCounters {
  std::uint64_t commands{0};
  std::uint64_t events{0};
  std::uint64_t trades{0};
  std::uint64_t schedule_misses{0};
  std::uint64_t ring_full_events{0};
  std::uint64_t sink_drops{0};
  std::int64_t wall_ns{0};
  double offered_rate{0.0};
  double achieved_rate{0.0};
  bool open_loop{true};
};

inline const char* csv_header() {
  return "git_sha,build,timing_backend,tap,coarse_timer,timer_overhead_ns,"
         "book,sink,pattern,mix,seed,warmup_orders,max_orders,ladder_ticks,"
         "ring_capacity,loop,commands,events,trades,schedule_misses,"
         "ring_full,sink_drops,wall_ns,offered_rate,achieved_rate,"
         "min_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,mean_ns\n";
}

inline std::string csv_row(const RunProvenance& p, const RunCounters& c,
                           const LatencyHistogram::Summary& s) {
  char buf[1024];
  const int n = std::snprintf(
      buf, sizeof(buf),
      "%s,%s,%s,%s,%s,%lld,%s,%s,%s,%s,%llu,%zu,%zu,%zu,%zu,%s,"
      "%llu,%llu,%llu,%llu,%llu,%llu,%lld,%.0f,%.0f,"
      "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%.1f\n",
      p.git_sha.c_str(), p.build_type.c_str(), p.timing_backend.c_str(),
      p.tap_on ? "ON" : "OFF", p.coarse_timer ? "yes" : "no",
      static_cast<long long>(p.timer_overhead_ns), p.book.c_str(),
      p.sink.c_str(), p.pattern.c_str(), p.mix.c_str(),
      static_cast<unsigned long long>(p.seed), p.warmup_orders, p.max_orders,
      p.ladder_ticks, p.ring_capacity, c.open_loop ? "open" : "closed",
      static_cast<unsigned long long>(c.commands),
      static_cast<unsigned long long>(c.events),
      static_cast<unsigned long long>(c.trades),
      static_cast<unsigned long long>(c.schedule_misses),
      static_cast<unsigned long long>(c.ring_full_events),
      static_cast<unsigned long long>(c.sink_drops),
      static_cast<long long>(c.wall_ns), c.offered_rate, c.achieved_rate,
      static_cast<long long>(s.min), static_cast<long long>(s.p50),
      static_cast<long long>(s.p90), static_cast<long long>(s.p99),
      static_cast<long long>(s.p999), static_cast<long long>(s.p9999),
      static_cast<long long>(s.max), s.mean);
  return std::string(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
}

} // namespace miniex
