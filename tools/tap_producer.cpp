/// \file tap_producer.cpp
/// \brief Runs the engine into a shared-memory tap. Used by the demo and by
///        the Python end-to-end test.
///
/// Deliberately minimal: generate synthetic flow, match it, write every event
/// into the tap, publish a statistics snapshot periodically, and exit. It is
/// what `viz/server/server.py` attaches to when there is no real data.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "core/timestamp.hpp"

#include "book/fast_book.hpp"
#include "engine/matching_engine.hpp"
#include "replay/synthetic.hpp"
#include "viz/tap_layout.hpp"
#include "viz/tap_sink.hpp"

#include "tools/cli.hpp"

int main(int argc, char** argv) {
  using namespace miniex;

  Cli cli("tap_producer",
          "run synthetic flow through the engine into a shared-memory tap");
  cli.option("shm", "tap segment name", "/miniex_demo");
  cli.option("capacity", "tap ring capacity in records (power of two)",
             "1048576");
  cli.option("commands", "commands to run (0 = forever)", "0");
  cli.option("warmup", "resting orders to build first", "20000");
  cli.option("seed", "flow seed", "1");
  cli.option("pattern", "synthetic flow pattern", "balanced");
  cli.option("rate", "commands per second to emit (0 = as fast as possible)",
             "200000");
  cli.option("max-orders", "engine pool capacity", "262144");
  if (!cli.parse(argc, argv)) return 2;

  const CycleCalibration cal(100);
  Cli::print_provenance(&cal);

  TapSink sink;
  const std::string name = cli.get("shm");
  const auto capacity =
      static_cast<std::size_t>(cli.get_int("capacity", 1048576));
  if (!sink.open(name, capacity)) {
    std::fprintf(stderr, "tap_producer: %s\n", sink.error().c_str());
    return 3;
  }
  std::fprintf(stderr, "# tap %s open: %zu records, %zu MB\n", name.c_str(),
               capacity, tap_segment_size(capacity) / (1024 * 1024));

  FastBookConfig ladder;
  ladder.max_orders =
      static_cast<std::size_t>(cli.get_int("max-orders", 262144));
  ladder.centre_price = 1002000;
  MatchingEngine<FastBook, TapSink> engine(sink, EngineConfig{}, ladder);

  FlowConfig flow_cfg;
  flow_cfg.seed = static_cast<std::uint64_t>(cli.get_int("seed", 1));
  flow_cfg.warmup_adds = static_cast<std::size_t>(cli.get_int("warmup", 20000));
  if (!parse_pattern(cli.get("pattern"), flow_cfg.pattern)) {
    std::fprintf(stderr, "tap_producer: unknown --pattern %s\n",
                 cli.get("pattern").c_str());
    return 2;
  }

  const auto total = static_cast<std::size_t>(cli.get_int("commands", 0));
  const auto rate = static_cast<std::int64_t>(cli.get_int("rate", 200000));
  // A batch of commands is generated ahead of the loop, never inside it.
  constexpr std::size_t kBatch = 4096;
  SyntheticFlow flow(flow_cfg);
  std::vector<Command> warm;
  flow.warm_up(warm);
  for (const Command& c : warm) engine.apply(c);
  std::fprintf(stderr, "# warmed up: %zu orders resting\n",
               engine.book().order_count());

  std::vector<Command> batch;
  std::size_t done = 0;
  const Cycles started = now_cycles();
  Cycles last_stats = started;
  std::uint64_t commands_at_last_stats = 0;

  while (total == 0 || done < total) {
    batch.clear();
    const std::size_t want =
        total == 0 ? kBatch : std::min(kBatch, total - done);
    flow.generate(batch, want);

    const Cycles batch_start = now_cycles();
    for (const Command& c : batch) engine.apply(c);
    done += batch.size();

    if (rate > 0) {
      // Pace the producer so a dashboard shows a market rather than a blur.
      const std::int64_t want_ns =
          static_cast<std::int64_t>(batch.size()) * 1'000'000'000 / rate;
      const std::int64_t spent_ns =
          cal.cycles_to_ns(now_cycles() - batch_start);
      if (spent_ns < want_ns)
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(want_ns - spent_ns));
    }

    const Cycles now = now_cycles();
    if (cal.cycles_to_ns(now - last_stats) > 100'000'000) {
      const double seconds =
          static_cast<double>(cal.cycles_to_ns(now - last_stats)) / 1e9;
      TapStats stats{};
      stats.ops_per_sec = static_cast<std::uint64_t>(
          static_cast<double>(engine.stats().commands -
                              commands_at_last_stats) /
          seconds);
      // The engine does not measure its own latency -- that is the harness's
      // job -- so these are zero here rather than invented.
      stats.orders_resting = engine.book().order_count();
      sink.publish_stats(stats);
      last_stats = now;
      commands_at_last_stats = engine.stats().commands;
    }
  }

  std::fprintf(stderr,
               "# done: %llu commands, %llu trades, %llu tap records, "
               "%llu dropped\n",
               (unsigned long long)engine.stats().commands,
               (unsigned long long)engine.stats().trades,
               (unsigned long long)sink.produced(),
               (unsigned long long)sink.dropped());
  return 0;
}
