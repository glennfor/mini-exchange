/// \file timestamp.hpp
/// \brief Portable cycle counter and its calibration to nanoseconds.
///
/// READ THIS BEFORE BELIEVING ANY LATENCY NUMBER FROM THIS REPOSITORY.
///
/// **x86-64.** `rdtscp` reads the invariant TSC and, unlike `rdtsc`, waits
/// for all *prior* instructions to have executed -- but it does **not**
/// prevent *later* instructions from being hoisted above it. Bracketing a
/// measured region with `rdtscp` therefore gives a lower bound that can be
/// short by a few tens of cycles. The harness measures and publishes the
/// back-to-back read overhead so a reader can judge the floor, and it
/// subtracts nothing. Publishable runs additionally require `constant_tsc`
/// and `nonstop_tsc`; without them the TSC changes rate with the P-state and
/// every number is fiction.
///
/// **AArch64 / Apple Silicon.** `cntvct_el0` is the architectural virtual
/// counter. On M-series parts it ticks at ~24 MHz, so its **resolution is
/// about 41 nanoseconds**. That is coarser than the operation being measured:
/// a p50 of "0 or 41 ns" is not a measurement. It is fine for throughput and
/// for relative A/B work during development, and useless for sub-microsecond
/// percentiles. This is precisely why the spec says publishable numbers come
/// from Linux x86-64, and why `CycleCalibration::coarse()` exists and the
/// harness prints a loud warning when it is true.
///
/// **Everything else.** `std::chrono::steady_clock`, which on most platforms
/// is a `clock_gettime(CLOCK_MONOTONIC)` vDSO call: ~20 ns and a function
/// call, so it perturbs what it measures. Development only.
///
/// No floating point appears anywhere here: the cycles-to-nanoseconds
/// conversion is fixed point in Q32, because a `double` multiply in the
/// measurement path is both a latency cost and a determinism hazard.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define MINIEX_TIMING_BACKEND "rdtscp"
#define MINIEX_TIMING_X86 1
#elif defined(__aarch64__)
#define MINIEX_TIMING_BACKEND "cntvct_el0"
#define MINIEX_TIMING_ARM 1
#else
#define MINIEX_TIMING_BACKEND "steady_clock"
#define MINIEX_TIMING_CHRONO 1
#endif

namespace miniex {

/// A raw hardware counter tick. Not a nanosecond, not a CPU cycle on every
/// platform -- deliberately a distinct name so the two are never confused.
using Cycles = std::uint64_t;

/// Read the cycle counter. Not a full barrier; see the file comment.
inline Cycles now_cycles() noexcept {
#if defined(MINIEX_TIMING_X86)
  unsigned aux;
  return __rdtscp(&aux);
#elif defined(MINIEX_TIMING_ARM)
  std::uint64_t v;
  // The isb is what makes the read ordered with respect to surrounding
  // instructions; without it the counter read floats and short intervals
  // come out negative. It costs ~10ns, which is why this platform is for
  // development only.
  //
  // The instructions are separated by a newline, not by ';': Apple's
  // integrated assembler treats ';' as a comment introducer, so "isb; mrs"
  // assembles to an isb and nothing else and the "counter" is whatever was
  // in the register. That failure is silent and produces plausible-looking
  // garbage, which is exactly the kind of bug a timing harness must not have.
  asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
  return v;
#else
  return static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

/// Wall-clock-ish nanoseconds from a steady source. Used for calibration and
/// for the arrival schedule, never inside a measured region.
inline std::int64_t now_steady_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/// Converts counter ticks to nanoseconds using a fixed-point ratio measured
/// at start-up. Construct one per process and pass it around by const ref.
class CycleCalibration {
public:
  /// Q32 fixed point: nanoseconds per tick, scaled by 2^32.
  static constexpr unsigned kShift = 32;

  /// Measures against steady_clock for at least \p min_ms milliseconds.
  /// 100 ms is the default because the 41 ns granularity of the Apple
  /// Silicon counter needs a long baseline to calibrate to better than
  /// 1e-6 relative error.
  explicit CycleCalibration(int min_ms = 100) noexcept { calibrate(min_ms); }

  /// Ticks -> nanoseconds.
  [[nodiscard]] std::int64_t cycles_to_ns(Cycles c) const noexcept {
#if defined(__SIZEOF_INT128__)
    return static_cast<std::int64_t>(
        (static_cast<unsigned __int128>(c) * ns_per_tick_q32_) >> kShift);
#else
    // 64-bit fallback: correct for intervals up to a few seconds, which is
    // all a latency sample can ever be.
    return static_cast<std::int64_t>((c * ns_per_tick_q32_) >> kShift);
#endif
  }

  /// Nanoseconds -> ticks, for the busy-wait in the open-loop harness.
  [[nodiscard]] Cycles ns_to_cycles(std::int64_t ns) const noexcept {
    if (ns <= 0) return 0;
#if defined(__SIZEOF_INT128__)
    return static_cast<Cycles>(
        (static_cast<unsigned __int128>(static_cast<std::uint64_t>(ns))
         << kShift) /
        ns_per_tick_q32_);
#else
    return static_cast<Cycles>((static_cast<std::uint64_t>(ns) << 16) /
                               (ns_per_tick_q32_ >> 16));
#endif
  }

  /// Ticks per second, as measured.
  [[nodiscard]] std::uint64_t ticks_per_second() const noexcept {
    return ticks_per_second_;
  }

  /// Q32 nanoseconds per tick; exposed so provenance output can print the
  /// exact constant a run used.
  [[nodiscard]] std::uint64_t ns_per_tick_q32() const noexcept {
    return ns_per_tick_q32_;
  }

  /// True when one tick is worth 10 ns or more -- i.e. the counter cannot
  /// resolve the operations being measured. The harness turns this into a
  /// loud "development machine only" banner on every report.
  [[nodiscard]] bool coarse() const noexcept {
    return cycles_to_ns(1) >= 10 || ticks_per_second_ < 100'000'000ULL;
  }

  /// Median cost of two back-to-back counter reads, in ticks. Published
  /// alongside every histogram so the reader can see the measurement floor.
  /// Nothing is ever subtracted from a sample.
  [[nodiscard]] static Cycles
  measure_read_overhead(int samples = 1001) noexcept {
    constexpr int kMaxSamples = 4096;
    if (samples > kMaxSamples) samples = kMaxSamples;
    if (samples < 3) samples = 3;
    Cycles buf[kMaxSamples];
    for (int i = 0; i < samples; ++i) {
      const Cycles a = now_cycles();
      const Cycles b = now_cycles();
      buf[i] = b - a;
    }
    // Median by insertion sort; this runs once at start-up.
    for (int i = 1; i < samples; ++i) {
      const Cycles key = buf[i];
      int j = i - 1;
      while (j >= 0 && buf[j] > key) {
        buf[j + 1] = buf[j];
        --j;
      }
      buf[j + 1] = key;
    }
    return buf[samples / 2];
  }

  [[nodiscard]] static constexpr const char* backend() noexcept {
    return MINIEX_TIMING_BACKEND;
  }

private:
  void calibrate(int min_ms) noexcept {
    const std::int64_t target_ns =
        static_cast<std::int64_t>(min_ms) * 1'000'000;
    const std::int64_t ns0 = now_steady_ns();
    const Cycles c0 = now_cycles();
    std::int64_t ns1 = ns0;
    Cycles c1 = c0;
    while (ns1 - ns0 < target_ns) {
      ns1 = now_steady_ns();
      c1 = now_cycles();
    }
    const std::uint64_t dns = static_cast<std::uint64_t>(ns1 - ns0);
    const std::uint64_t dticks = c1 - c0;
    if (dticks == 0 || dns == 0) {
      ns_per_tick_q32_ = 1ULL << kShift; // 1 ns per tick
      ticks_per_second_ = 1'000'000'000ULL;
      return;
    }
#if defined(__SIZEOF_INT128__)
    ns_per_tick_q32_ = static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(dns) << kShift) / dticks);
    ticks_per_second_ = static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(dticks) * 1'000'000'000ULL) / dns);
#else
    ns_per_tick_q32_ = (dns << 20) / dticks << 12;
    ticks_per_second_ = (dticks / (dns / 1'000'000ULL)) * 1000ULL;
#endif
    if (ns_per_tick_q32_ == 0) ns_per_tick_q32_ = 1;
  }

  std::uint64_t ns_per_tick_q32_{1ULL << kShift};
  std::uint64_t ticks_per_second_{1'000'000'000ULL};
};

} // namespace miniex
