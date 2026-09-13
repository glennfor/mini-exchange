#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include "core/timestamp.hpp"

namespace miniex {
namespace {

TEST(Timestamp, CounterIsMonotonic) {
  Cycles prev = now_cycles();
  for (int i = 0; i < 10'000; ++i) {
    const Cycles c = now_cycles();
    ASSERT_GE(c, prev) << "cycle counter went backwards at iteration " << i;
    prev = c;
  }
}

TEST(Timestamp, CounterAdvances) {
  const Cycles a = now_cycles();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const Cycles b = now_cycles();
  EXPECT_GT(b, a);
}

TEST(Timestamp, CalibrationRecoversAPlausibleFrequency) {
  const CycleCalibration cal(100);
  // Anything from the 24 MHz Apple Silicon counter to a 6 GHz TSC.
  EXPECT_GT(cal.ticks_per_second(), 1'000'000ULL);
  EXPECT_LT(cal.ticks_per_second(), 20'000'000'000ULL);
  EXPECT_GT(cal.ns_per_tick_q32(), 0U);
}

TEST(Timestamp, CyclesToNanosMatchesASleep) {
  const CycleCalibration cal(100);
  const Cycles c0 = now_cycles();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const std::int64_t measured = cal.cycles_to_ns(now_cycles() - c0);
  // Sleep overshoots and the scheduler is noisy; assert the order of
  // magnitude, which is all a calibration test can honestly assert.
  EXPECT_GT(measured, 40'000'000);
  EXPECT_LT(measured, 500'000'000);
}

TEST(Timestamp, NsToCyclesRoundTrips) {
  const CycleCalibration cal(100);
  for (std::int64_t ns : {1'000LL, 100'000LL, 10'000'000LL}) {
    const Cycles c = cal.ns_to_cycles(ns);
    const std::int64_t back = cal.cycles_to_ns(c);
    const std::int64_t err = back > ns ? back - ns : ns - back;
    // One tick of quantisation error, whatever a tick is worth here.
    EXPECT_LE(err, cal.cycles_to_ns(1) + ns / 1000)
        << "ns=" << ns << " back=" << back;
  }
  EXPECT_EQ(cal.ns_to_cycles(0), 0U);
  EXPECT_EQ(cal.ns_to_cycles(-5), 0U);
}

TEST(Timestamp, ReadOverheadIsMeasuredAndReported) {
  const CycleCalibration cal(100);
  const Cycles overhead = CycleCalibration::measure_read_overhead(501);
  const std::int64_t overhead_ns = cal.cycles_to_ns(overhead);
  // Not an assertion about speed -- an assertion that it is measurable and
  // not absurd. The number itself is published, never subtracted.
  EXPECT_GE(overhead_ns, 0);
  EXPECT_LT(overhead_ns, 10'000);
  RecordProperty("timing_backend", CycleCalibration::backend());
  RecordProperty("ticks_per_second", std::to_string(cal.ticks_per_second()));
  RecordProperty("read_overhead_ns", std::to_string(overhead_ns));
  RecordProperty("coarse", cal.coarse() ? "yes" : "no");
  std::cerr << "[ timing   ] backend=" << CycleCalibration::backend()
            << " ticks/s=" << cal.ticks_per_second()
            << " tick=" << cal.cycles_to_ns(1) << "ns"
            << " read_overhead=" << overhead_ns << "ns"
            << " coarse=" << (cal.coarse() ? "YES (development only)" : "no")
            << "\n";
}

TEST(Timestamp, BackendIsNamedSoResultsCanSayHowTheyWereTimed) {
  const std::string backend = CycleCalibration::backend();
  EXPECT_FALSE(backend.empty());
  EXPECT_TRUE(backend == "rdtscp" || backend == "cntvct_el0" ||
              backend == "steady_clock")
      << backend;
}

} // namespace
} // namespace miniex
