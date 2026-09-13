/// \file engine_bench.cpp
/// \brief The end-to-end latency harness. Spec section 18, implemented
///        exactly.
///
/// ## Why open loop
///
/// A closed-loop benchmark sends a command, waits for it to complete, and
/// sends the next one. When the system stalls, the load generator stalls with
/// it and simply stops offering load -- so the stall never appears in the
/// distribution. This is *coordinated omission*, and it is why closed-loop
/// benchmarks of queueing systems report tail latencies that are wrong by
/// orders of magnitude.
///
/// The open-loop harness instead fixes the arrival schedule in advance:
/// command *i* is *intended* to be sent at `t0 + i/R`. Latency is measured
/// from that intended time, not from when the producer actually managed to
/// send it. If the engine stalls, the backlog of commands that were supposed
/// to have been sent during the stall each carry their full waiting time, and
/// the tail shows it.
///
/// `--closed-loop` runs the other way for comparison. Publishing both curves
/// side by side is far more persuasive than either number alone, which is why
/// the option exists rather than being a footnote.
///
/// ## What "completion" means
///
/// The latency sample ends when `engine.apply()` returns for that command --
/// that is, after the last outbound event it produced has been handed to the
/// sink. Measuring at "last event written" and at "handler returned" are the
/// same instant for every sink here, and the handler-return point is the one
/// that is well defined for a command that produces no events at all.
///
/// ## Threading
///
/// Producer on one thread, engine on another, connected by the SPSC ring --
/// the same path the real ingress uses. The engine thread records into the
/// histogram, which costs a few nanoseconds per sample and is *part of the
/// measurement*, not subtracted from it. The timer read overhead is measured
/// separately and published so a reader can see the floor.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "core/spsc_ring.hpp"
#include "core/timestamp.hpp"

#include "book/naive_book.hpp"
#include "engine/event_sink.hpp"
#include "engine/matching_engine.hpp"
#include "replay/synthetic.hpp"

#include "latency.hpp"
#include "tools/cli.hpp"

namespace {

using namespace miniex;

using Ring = SpscRing<Command, kRingCapacity>;

struct RunResult {
  LatencyHistogram histogram;
  RunCounters counters;
};

/// Pins the calling thread to a CPU where the platform allows it.
///
/// On Linux this is `pthread_setaffinity_np` and it matters a great deal:
/// without it the producer and the engine migrate between cores and the tail
/// fills up with scheduler noise. macOS has no equivalent that actually
/// pins -- `thread_policy_set` with an affinity tag is a hint the scheduler
/// may ignore -- which is one more reason publishable numbers come from
/// Linux. The function reports whether it succeeded so the provenance can say
/// so rather than implying a pinning that did not happen.
bool pin_to_core(int core) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)core;
  return false;
#endif
}

/// Open-loop: submit on a precomputed schedule, measure from the intended
/// send time.
void run_open_loop(const std::vector<Command>& commands,
                   const CycleCalibration& cal, double rate_per_second,
                   int producer_core, int engine_core, RunResult& result) {
  const std::size_t n = commands.size();
  auto ring = std::make_unique<Ring>();

  // The schedule, precomputed. Generating it inside the loop would measure
  // the division.
  std::vector<Cycles> intended(n);
  const Cycles period =
      rate_per_second <= 0.0
          ? 0
          : cal.ns_to_cycles(static_cast<std::int64_t>(1e9 / rate_per_second));

  std::atomic<bool> engine_ready{false};
  std::atomic<std::uint64_t> processed{0};
  CountingSink sink;
  MatchingEngine<NaiveBook, CountingSink> engine(sink);

  std::thread engine_thread([&] {
    (void)pin_to_core(engine_core);
    engine_ready.store(true, std::memory_order_release);
    std::size_t i = 0;
    Command cmd{};
    while (i < n) {
      if (!ring->try_pop(cmd)) continue;
      engine.apply(cmd);
      const Cycles done = now_cycles();
      result.histogram.record(cal.cycles_to_ns(done - intended[i]));
      ++i;
      processed.store(i, std::memory_order_relaxed);
    }
  });

  while (!engine_ready.load(std::memory_order_acquire)) {
  }
  (void)pin_to_core(producer_core);

  const Cycles t0 = now_cycles();
  for (std::size_t i = 0; i < n; ++i) intended[i] = t0 + period * i;

  std::uint64_t misses = 0;
  std::uint64_t ring_full = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (period != 0) {
      while (now_cycles() < intended[i]) {
        // Busy-wait. A sleep here would quantise the schedule to the timer
        // granularity and invent latency that the engine did not cause.
      }
      // A miss is "we were already late before we even tried to send".
      // Reported, never hidden: a run with many misses is a saturated run and
      // its percentiles describe the harness, not the engine.
      if (now_cycles() > intended[i] + period) ++misses;
    }
    while (!ring->try_push(commands[i])) ++ring_full;
  }
  engine_thread.join();
  const Cycles t1 = now_cycles();

  result.counters.commands = n;
  result.counters.events = sink.total;
  result.counters.trades = engine.stats().trades;
  result.counters.schedule_misses = misses;
  result.counters.ring_full_events = ring_full;
  result.counters.wall_ns = cal.cycles_to_ns(t1 - t0);
  result.counters.offered_rate = rate_per_second;
  result.counters.achieved_rate =
      result.counters.wall_ns > 0
          ? static_cast<double>(n) * 1e9 /
                static_cast<double>(result.counters.wall_ns)
          : 0.0;
  result.counters.open_loop = true;
}

/// Closed-loop: send, wait for completion, send the next. Present only so the
/// two curves can be published together; never report these percentiles as
/// if they were open-loop.
void run_closed_loop(const std::vector<Command>& commands,
                     const CycleCalibration& cal, int producer_core,
                     int engine_core, RunResult& result) {
  const std::size_t n = commands.size();
  auto ring = std::make_unique<Ring>();
  std::atomic<std::uint64_t> done_count{0};
  std::atomic<bool> engine_ready{false};
  CountingSink sink;
  MatchingEngine<NaiveBook, CountingSink> engine(sink);

  std::thread engine_thread([&] {
    (void)pin_to_core(engine_core);
    engine_ready.store(true, std::memory_order_release);
    std::size_t i = 0;
    Command cmd{};
    while (i < n) {
      if (!ring->try_pop(cmd)) continue;
      engine.apply(cmd);
      ++i;
      done_count.store(i, std::memory_order_release);
    }
  });

  while (!engine_ready.load(std::memory_order_acquire)) {
  }
  (void)pin_to_core(producer_core);

  const Cycles t0 = now_cycles();
  for (std::size_t i = 0; i < n; ++i) {
    const Cycles sent = now_cycles();
    while (!ring->try_push(commands[i])) {
    }
    while (done_count.load(std::memory_order_acquire) <= i) {
    }
    result.histogram.record(cal.cycles_to_ns(now_cycles() - sent));
  }
  engine_thread.join();
  const Cycles t1 = now_cycles();

  result.counters.commands = n;
  result.counters.events = sink.total;
  result.counters.trades = engine.stats().trades;
  result.counters.wall_ns = cal.cycles_to_ns(t1 - t0);
  result.counters.achieved_rate =
      result.counters.wall_ns > 0
          ? static_cast<double>(n) * 1e9 /
                static_cast<double>(result.counters.wall_ns)
          : 0.0;
  result.counters.open_loop = false;
}

void print_human(const RunProvenance& p, const RunCounters& c,
                 const LatencyHistogram& hist) {
  const auto s = hist.summary();
  std::printf("\n");
  if (p.coarse_timer) {
    std::printf(
        "!! DEVELOPMENT MACHINE ONLY !!\n"
        "!! The timing source (%s) ticks at %llu Hz, i.e. one tick is %lld "
        "ns.\n"
        "!! Sub-microsecond percentiles from this machine are quantisation "
        "noise,\n"
        "!! not measurements. Publishable numbers come from x86-64 Linux with "
        "an\n"
        "!! invariant TSC. See docs/benchmarking.md.\n\n",
        p.timing_backend.c_str(),
        static_cast<unsigned long long>(p.ticks_per_second),
        static_cast<long long>(p.tick_ns));
  }
  std::printf("loop            %s\n",
              c.open_loop ? "open" : "CLOSED (comparison only)");
  std::printf("book / sink     %s / %s\n", p.book.c_str(), p.sink.c_str());
  std::printf("flow            pattern=%s mix=%s seed=%llu warmup=%zu\n",
              p.pattern.c_str(), p.mix.c_str(),
              static_cast<unsigned long long>(p.seed), p.warmup_orders);
  std::printf("commands        %llu\n",
              static_cast<unsigned long long>(c.commands));
  std::printf("events          %llu  (trades %llu)\n",
              static_cast<unsigned long long>(c.events),
              static_cast<unsigned long long>(c.trades));
  std::printf("throughput      %.0f cmd/s achieved", c.achieved_rate);
  if (c.offered_rate > 0.0) std::printf(", %.0f offered", c.offered_rate);
  std::printf("\n");
  std::printf("schedule misses %llu%s\n",
              static_cast<unsigned long long>(c.schedule_misses),
              c.schedule_misses * 100 > c.commands
                  ? "   <-- SATURATED: these percentiles describe the harness"
                  : "");
  std::printf("ring-full       %llu\n",
              static_cast<unsigned long long>(c.ring_full_events));
  std::printf("timer overhead  %lld ns (published, never subtracted)\n",
              static_cast<long long>(p.timer_overhead_ns));
  std::printf("\nlatency (ns)\n");
  std::printf("  min      %10lld\n", static_cast<long long>(s.min));
  std::printf("  p50      %10lld\n", static_cast<long long>(s.p50));
  std::printf("  p90      %10lld\n", static_cast<long long>(s.p90));
  std::printf("  p99      %10lld\n", static_cast<long long>(s.p99));
  std::printf("  p99.9    %10lld\n", static_cast<long long>(s.p999));
  std::printf("  p99.99   %10lld\n", static_cast<long long>(s.p9999));
  std::printf("  max      %10lld\n", static_cast<long long>(s.max));
  std::printf("  mean     %10.1f   (never read this without the above)\n",
              s.mean);
  std::printf("\n");
}

bool parse_mix(const std::string& text, FlowMix& out) {
  if (text.empty()) return true;
  FlowMix mix{0, 0, 0, 0};
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t comma = text.find(',', pos);
    const std::string item = text.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
    const std::size_t eq = item.find('=');
    if (eq == std::string::npos) return false;
    const std::string key = item.substr(0, eq);
    const auto value = static_cast<std::uint32_t>(
        std::strtoul(item.c_str() + eq + 1, nullptr, 10));
    if (key == "add")
      mix.add = value;
    else if (key == "cancel")
      mix.cancel = value;
    else if (key == "replace")
      mix.replace = value;
    else if (key == "aggressive")
      mix.aggressive = value;
    else
      return false;
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  if (mix.total() == 0) return false;
  out = mix;
  return true;
}

std::string mix_to_string(const FlowMix& m) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "add=%u/cancel=%u/replace=%u/aggressive=%u",
                m.add, m.cancel, m.replace, m.aggressive);
  return buf;
}

} // namespace

int main(int argc, char** argv) {
  Cli cli("engine_bench",
          "open-loop, coordinated-omission-free latency harness for the "
          "matching engine");
  cli.option("commands", "number of commands to run", "2000000");
  cli.option("rate", "offered rate in commands/sec (0 = as fast as possible)",
             "1000000");
  cli.option("seed", "flow generator seed", "1");
  cli.option("pattern",
             "balanced|deep-book|touch-churn|wide-spread|ladder-overflow|"
             "pool-pressure",
             "balanced");
  cli.option("mix", "add=45,cancel=40,replace=10,aggressive=5", "");
  cli.option("warmup", "resting orders to build before measuring", "0");
  cli.option("csv", "append a result row to this CSV file");
  cli.option("cdf", "write the percentile dump to this CSV file");
  cli.option("producer-core", "CPU to pin the load generator to (Linux)", "2");
  cli.option("engine-core", "CPU to pin the engine to (Linux)", "4");
  cli.flag("closed-loop", "closed-loop comparison run (not a latency result)");
  if (!cli.parse(argc, argv)) return 2;

  const CycleCalibration cal(200);
  Cli::print_provenance(&cal);

  FlowConfig flow_cfg;
  flow_cfg.seed = static_cast<std::uint64_t>(cli.get_int("seed", 1));
  flow_cfg.warmup_adds = static_cast<std::size_t>(cli.get_int("warmup", 0));
  if (!parse_pattern(cli.get("pattern"), flow_cfg.pattern)) {
    std::cerr << "engine_bench: unknown --pattern " << cli.get("pattern")
              << "\n";
    return 2;
  }
  if (!parse_mix(cli.get("mix"), flow_cfg.mix)) {
    std::cerr << "engine_bench: cannot parse --mix " << cli.get("mix") << "\n";
    return 2;
  }

  const auto count = static_cast<std::size_t>(cli.get_int("commands", 2000000));

  // Generated ahead of time, into a buffer that is fully allocated before the
  // clock starts. Generating inside the measured loop would measure the RNG.
  std::cerr << "# generating " << count << " commands ("
            << to_string(flow_cfg.pattern) << ", seed " << flow_cfg.seed
            << ")\n";
  SyntheticFlow flow(flow_cfg);
  std::vector<Command> commands;
  commands.reserve(count + flow_cfg.warmup_adds);
  flow.warm_up(commands);
  flow.generate(commands, count);

  RunResult result;
  if (!result.histogram.valid()) {
    std::cerr << "engine_bench: could not allocate the histogram\n";
    return 3;
  }

  const int producer_core = static_cast<int>(cli.get_int("producer-core", 2));
  const int engine_core = static_cast<int>(cli.get_int("engine-core", 4));
  if (cli.has("closed-loop")) {
    run_closed_loop(commands, cal, producer_core, engine_core, result);
  } else {
    run_open_loop(commands, cal,
                  static_cast<double>(cli.get_int("rate", 1000000)),
                  producer_core, engine_core, result);
  }

  RunProvenance prov;
  prov.build_type = MINIEX_BUILD_TYPE;
  prov.git_sha = MINIEX_GIT_SHA;
  prov.timing_backend = CycleCalibration::backend();
#ifdef MINIEX_TAP
  prov.tap_on = true;
#endif
  prov.ticks_per_second = cal.ticks_per_second();
  prov.tick_ns = cal.cycles_to_ns(1);
  prov.coarse_timer = cal.coarse();
  prov.timer_overhead_ns =
      cal.cycles_to_ns(CycleCalibration::measure_read_overhead());
  prov.book = "NaiveBook";
  prov.sink = "CountingSink";
  prov.pattern = std::string(to_string(flow_cfg.pattern));
  prov.mix = mix_to_string(flow.config().mix);
  prov.seed = flow_cfg.seed;
  prov.warmup_orders = flow_cfg.warmup_adds;
  prov.max_orders = kMaxOrders;
  prov.ladder_ticks = kLadderTicks;
  prov.ring_capacity = kRingCapacity;

  print_human(prov, result.counters, result.histogram);

  if (!cli.get("csv").empty()) {
    const std::string path = cli.get("csv");
    const bool fresh = !std::ifstream(path).good();
    std::ofstream out(path, std::ios::app);
    if (fresh) out << csv_header();
    out << csv_row(prov, result.counters, result.histogram.summary());
    std::cerr << "# appended a row to " << path << "\n";
  }
  if (!cli.get("cdf").empty()) {
    std::ofstream out(cli.get("cdf"), std::ios::trunc);
    out << "percentile,latency_ns\n";
    for (const auto& [p, v] : result.histogram.cdf())
      out << p << "," << v << "\n";
    std::cerr << "# wrote the percentile dump to " << cli.get("cdf") << "\n";
  }
  return 0;
}
