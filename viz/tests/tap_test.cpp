// The tap's contract is about what it does to the *engine*, not what it
// shows. These tests are mostly about the engine being unaffected.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include "book/fast_book.hpp"
#include "engine/matching_engine.hpp"
#include "replay/synthetic.hpp"
#include "viz/shm_ring.hpp"
#include "viz/tap_sink.hpp"

namespace miniex {
namespace {

/// A unique segment name per test, so a crashed run cannot poison the next.
std::string unique_name(const char* prefix) {
  static std::atomic<int> counter{0};
  std::string name = std::string("/miniex_test_") + prefix + "_" +
                     std::to_string(::getpid()) + "_" +
                     std::to_string(counter.fetch_add(1));
  // macOS caps a POSIX shared-memory name at 31 characters (PSHMNAMLEN) and
  // shm_open just fails past it. Caught here, where the name is built, rather
  // than as an unexplained `create` returning false three lines into a test.
  EXPECT_LT(name.size(), 32U)
      << "shared-memory name too long for macOS: " << name;
  return name;
}

struct SegmentGuard {
  std::string name;
  explicit SegmentGuard(std::string n) : name(std::move(n)) {}
  ~SegmentGuard() { ShmTapSegment::unlink(name); }
};

TapRecord make_record(std::uint64_t seq) {
  TapRecord r{};
  r.seq = seq;
  r.ts = static_cast<std::int64_t>(seq) * 1000;
  r.price = 1002100 + static_cast<std::int64_t>(seq % 17);
  r.qty = static_cast<std::uint32_t>(100 + seq % 7);
  r.id = seq * 3;
  r.type = static_cast<std::uint8_t>(seq % 7);
  return r;
}

// --- layout -----------------------------------------------------------------

TEST(TapLayout, TheOffsetsAreTheOnesTheProtocolDocuments) {
  // These numbers are re-declared in viz/server/shm_reader.py. If either side
  // moves, the other reinterprets bytes and produces a confidently wrong
  // dashboard, so both are pinned and compared.
  EXPECT_EQ(kTapHeaderOffset, 0U);
  EXPECT_EQ(kTapHeadOffset, 64U);
  EXPECT_EQ(kTapTailOffset, 128U);
  EXPECT_EQ(kTapCountersOffset, 192U);
  EXPECT_EQ(kTapStatsOffset, 256U);
  EXPECT_EQ(kTapDataOffset, 4096U);
  EXPECT_EQ(sizeof(TapRecord), 64U);
  EXPECT_EQ(sizeof(TapHeader), 64U);
  EXPECT_EQ(sizeof(TapStats), 64U);
  EXPECT_EQ(tap_segment_size(1024), 4096U + 1024U * 64U);
}

TEST(TapLayout, HeadAndTailAreOnSeparateCacheLines) {
  EXPECT_GE(kTapTailOffset - kTapHeadOffset, kTapCacheLine)
      << "the producer's and consumer's counters must not share a line, or "
         "every write invalidates the line the other is polling";
}

// --- create and attach ------------------------------------------------------

TEST(TapSegment, CreateThenAttachAgreesOnTheLayout) {
  const std::string name = unique_name("attach");
  SegmentGuard guard(name);

  ShmTapWriter writer;
  ASSERT_TRUE(writer.create(name, 1024)) << writer.error();

  ShmTapReader reader;
  ASSERT_TRUE(reader.attach(name)) << reader.error();
  EXPECT_TRUE(reader.producer_live());
  EXPECT_EQ(reader.produced(), 0U);
}

TEST(TapSegment, AttachingToSomethingElseIsRefused) {
  const std::string name = unique_name("notatap");
  SegmentGuard guard(name);
  // Create a segment with the right size but no valid header.
  ShmTapSegment raw;
  ASSERT_TRUE(raw.create(name, 64));
  raw.header()->magic = 0xDEADBEEF;

  ShmTapReader reader;
  EXPECT_FALSE(reader.attach(name));
  EXPECT_NE(reader.error().find("magic"), std::string::npos);
}

TEST(TapSegment, AVersionMismatchIsRefusedRatherThanReinterpreted) {
  const std::string name = unique_name("version");
  SegmentGuard guard(name);
  ShmTapSegment raw;
  ASSERT_TRUE(raw.create(name, 64));
  raw.header()->version = kTapVersion + 1;

  ShmTapReader reader;
  EXPECT_FALSE(reader.attach(name));
  EXPECT_NE(reader.error().find("version mismatch"), std::string::npos)
      << "reinterpreting bytes from another layout produces a dashboard that "
         "is confidently wrong, which is worse than no dashboard";
}

TEST(TapSegment, ANonPowerOfTwoCapacityIsRefused) {
  ShmTapWriter writer;
  EXPECT_FALSE(writer.create(unique_name("badcap"), 1000));
  EXPECT_NE(writer.error().find("power of two"), std::string::npos);
}

// --- the ring ---------------------------------------------------------------

TEST(TapRing, RecordsArriveIntactAndInOrder) {
  const std::string name = unique_name("roundtrip");
  SegmentGuard guard(name);
  ShmTapWriter writer;
  ASSERT_TRUE(writer.create(name, 64));
  ShmTapReader reader;
  ASSERT_TRUE(reader.attach(name));

  for (std::uint64_t i = 0; i < 40; ++i)
    ASSERT_TRUE(writer.write(make_record(i)));

  std::vector<TapRecord> out(64);
  ASSERT_EQ(reader.read(out.data(), out.size()), 40U);
  for (std::uint64_t i = 0; i < 40; ++i) {
    EXPECT_EQ(out[i].seq, i);
    EXPECT_EQ(out[i].qty, 100 + i % 7);
    EXPECT_EQ(out[i].price, 1002100 + static_cast<std::int64_t>(i % 17));
  }
  EXPECT_EQ(writer.produced(), 40U);
  EXPECT_EQ(writer.dropped(), 0U);
}

TEST(TapRing, AFullRingDropsAndCountsRatherThanBlocking) {
  const std::string name = unique_name("full");
  SegmentGuard guard(name);
  ShmTapWriter writer;
  ASSERT_TRUE(writer.create(name, 8));

  for (std::uint64_t i = 0; i < 8; ++i)
    ASSERT_TRUE(writer.write(make_record(i)));
  // Nine onwards must be dropped, and the call must return immediately.
  for (std::uint64_t i = 8; i < 100; ++i)
    EXPECT_FALSE(writer.write(make_record(i)));

  EXPECT_EQ(writer.produced(), 8U);
  EXPECT_EQ(writer.dropped(), 92U)
      << "dropping observability data is correct; stalling matching to feed a "
         "dashboard is not";
}

TEST(TapRing, WrapsAroundCorrectly) {
  const std::string name = unique_name("wrap");
  SegmentGuard guard(name);
  ShmTapWriter writer;
  ASSERT_TRUE(writer.create(name, 8));
  ShmTapReader reader;
  ASSERT_TRUE(reader.attach(name));

  TapRecord out[8];
  std::uint64_t expected = 0;
  for (std::uint64_t round = 0; round < 100; ++round) {
    for (std::uint64_t i = 0; i < 5; ++i)
      ASSERT_TRUE(writer.write(make_record(round * 5 + i)));
    const std::size_t n = reader.read(out, 8);
    ASSERT_EQ(n, 5U);
    for (std::size_t i = 0; i < n; ++i) EXPECT_EQ(out[i].seq, expected++);
  }
}

TEST(TapRing, TwoProcessesSeeTheSameData) {
  // The whole point of shared memory rather than a pipe: the reader is a
  // separate process on a separate core.
  const std::string name = unique_name("fork");
  SegmentGuard guard(name);
  ShmTapWriter writer;
  ASSERT_TRUE(writer.create(name, 1024));
  constexpr std::uint64_t kRecords = 4096;

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // Child: drain everything, verify the sequence, exit 0 on success.
    ShmTapReader reader;
    if (!reader.attach(name)) _exit(2);
    std::vector<TapRecord> buf(256);
    std::uint64_t expected = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (expected < kRecords) {
      const std::size_t n = reader.read(buf.data(), buf.size());
      for (std::size_t i = 0; i < n; ++i) {
        if (buf[i].seq != expected) _exit(3);
        ++expected;
      }
      if (n == 0 && std::chrono::steady_clock::now() > deadline) _exit(4);
    }
    _exit(0);
  }

  for (std::uint64_t i = 0; i < kRecords; ++i) {
    while (!writer.write(make_record(i))) {
      // The ring is smaller than the run, so the producer waits for the
      // reader here. In the engine it would drop instead -- this test is
      // about the data path, not the policy.
    }
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0) << "the reader process saw a bad sequence";
}

// --- the property that actually matters -------------------------------------

TEST(TapRing, AReaderThatDiesMidRunDoesNotPerturbTheProducer) {
  // Spec 19.1: the reader is untrusted. If it dies, the engine is unaffected.
  // This measures it rather than asserting it.
  //
  // The comparison that matters is **live reader vs killed reader on the same
  // work**. Once the reader is gone the ring fills, and every subsequent
  // write is an early return -- so the producer can only get *cheaper*, never
  // more expensive. That is the property, and it is what is asserted.
  //
  // The no-reader baseline is reported too, but it is not a fair comparison
  // and is labelled as such: with nothing draining, almost every write is a
  // drop that never copies a record, so it measures the drop path rather than
  // the write path.
  const std::string name = unique_name("kill");
  SegmentGuard guard(name);
  ShmTapWriter writer;
  ASSERT_TRUE(writer.create(name, 4096));
  constexpr std::uint64_t kRecords = 200000;

  const auto time_writes = [&](std::uint64_t n) {
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < n; ++i) (void)writer.write(make_record(i));
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
  };

  const std::int64_t no_reader_us = time_writes(kRecords);
  EXPECT_GT(writer.dropped(), 0U)
      << "with nothing draining, the ring must fill and start dropping";

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    ShmTapReader reader;
    if (!reader.attach(name)) _exit(2);
    std::vector<TapRecord> buf(256);
    for (;;) (void)reader.read(buf.data(), buf.size());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  const std::int64_t with_reader_us = time_writes(kRecords);

  ::kill(pid, SIGKILL);
  int status = 0;
  (void)::waitpid(pid, &status, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  const std::int64_t after_kill_us = time_writes(kRecords);

  std::cerr << "[ tap      ] producer wall time per " << kRecords
            << " records:\n"
            << "[ tap      ]   live reader             " << with_reader_us
            << " us\n"
            << "[ tap      ]   reader killed mid-run   " << after_kill_us
            << " us\n"
            << "[ tap      ]   no reader ever attached " << no_reader_us
            << " us  (mostly the drop path, not comparable)\n";

  // The assertion: losing the reader never makes the producer slower. A small
  // allowance for scheduler noise on a desktop, and no more.
  EXPECT_LT(after_kill_us, with_reader_us + (with_reader_us / 4) + 2000)
      << "the producer got slower after the reader died, which means it is "
         "waiting on something it must not wait on";
}

TEST(TapRing, AProducerThatNeverStartsLeavesTheReaderEmptyHanded) {
  const std::string name = unique_name("noproducer");
  SegmentGuard guard(name);
  ShmTapWriter writer;
  ASSERT_TRUE(writer.create(name, 64));
  ShmTapReader reader;
  ASSERT_TRUE(reader.attach(name));
  TapRecord out[8];
  EXPECT_EQ(reader.read(out, 8), 0U);
  EXPECT_EQ(reader.lag(), 0U);
}

// --- the stats seqlock ------------------------------------------------------

TEST(TapStatsSnapshot, ReadsAreConsistentOrRetried) {
  const std::string name = unique_name("stats");
  SegmentGuard guard(name);
  ShmTapWriter writer;
  ASSERT_TRUE(writer.create(name, 64));
  ShmTapReader reader;
  ASSERT_TRUE(reader.attach(name));

  TapStats s{};
  s.ops_per_sec = 6120000;
  s.p50_ns = 118;
  s.p99_ns = 342;
  s.p999_ns = 910;
  s.max_ns = 14200;
  s.orders_resting = 48211;
  writer.publish_stats(s);

  TapStats got{};
  ASSERT_TRUE(reader.read_stats(got));
  EXPECT_EQ(got.ops_per_sec, 6120000U);
  EXPECT_EQ(got.p99_ns, 342);
  EXPECT_EQ(got.max_ns, 14200);
  EXPECT_EQ(got.orders_resting, 48211U);
}

TEST(TapStatsSnapshot, ConcurrentWritesAreNeverSeenTorn) {
  // Safety, against a writer that never stops. This says nothing about how
  // *often* the reader gets through -- see the test below for that -- only
  // that whatever it does get through is coherent.
  const std::string name = unique_name("seqlock");
  SegmentGuard guard(name);
  ShmTapWriter writer;
  ASSERT_TRUE(writer.create(name, 64));
  ShmTapReader reader;
  ASSERT_TRUE(reader.attach(name));

  std::atomic<bool> stop{false};
  std::thread producer([&] {
    std::uint64_t n = 1;
    while (!stop.load(std::memory_order_relaxed)) {
      TapStats s{};
      // Every field derived from the same n, so a torn read is detectable
      // from the values alone rather than only by the sequence counter.
      s.ops_per_sec = n;
      s.p50_ns = static_cast<std::int64_t>(n);
      s.p99_ns = static_cast<std::int64_t>(n);
      s.p999_ns = static_cast<std::int64_t>(n);
      s.max_ns = static_cast<std::int64_t>(n);
      s.orders_resting = n;
      s.schedule_misses = n;
      writer.publish_stats(s);
      ++n;
    }
  });

  int good = 0;
  for (int i = 0; i < 20000; ++i) {
    TapStats got{};
    if (!reader.read_stats(got)) continue;
    ASSERT_EQ(got.p50_ns, static_cast<std::int64_t>(got.ops_per_sec));
    ASSERT_EQ(got.p99_ns, static_cast<std::int64_t>(got.ops_per_sec));
    ASSERT_EQ(got.max_ns, static_cast<std::int64_t>(got.ops_per_sec));
    ASSERT_EQ(got.orders_resting, got.ops_per_sec);
    ASSERT_EQ(got.schedule_misses, got.ops_per_sec);
    ++good;
  }
  stop.store(true);
  producer.join();
  EXPECT_GT(good, 0) << "the reader never got through at all, so this test "
                        "checked nothing";
}

TEST(TapStatsSnapshot, AReaderGetsThroughAgainstARealisticWriter) {
  // Liveness, and it has to be stated against a writer that pauses.
  //
  // A seqlock gives the writer absolute priority: it never waits, and a
  // reader that sees the sequence move retries. So a writer in a tight loop
  // can starve a reader indefinitely -- that is the design working, not a
  // fault. Asserting "the reader succeeds most of the time" against the
  // hostile writer above therefore asserted something about the *machine*:
  // it held on an idle one and failed on a loaded one, 683 successes out of
  // 20,000 when six test binaries ran at once. Never a torn read; just a lost
  // race, over and over.
  //
  // The engine publishes stats once a second. At anything like that rate the
  // window in which the sequence is odd is vanishing, and the reader getting
  // through is a real property worth pinning down.
  const std::string name = unique_name("live");
  SegmentGuard guard(name);
  ShmTapWriter writer;
  ASSERT_TRUE(writer.create(name, 64));
  ShmTapReader reader;
  ASSERT_TRUE(reader.attach(name));

  std::atomic<bool> stop{false};
  std::thread producer([&] {
    std::uint64_t n = 1;
    while (!stop.load(std::memory_order_relaxed)) {
      TapStats s{};
      s.ops_per_sec = n;
      s.p50_ns = static_cast<std::int64_t>(n);
      s.orders_resting = n;
      writer.publish_stats(s);
      ++n;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });

  int good = 0;
  for (int i = 0; i < 2000; ++i) {
    TapStats got{};
    if (!reader.read_stats(got)) continue;
    ASSERT_EQ(got.p50_ns, static_cast<std::int64_t>(got.ops_per_sec));
    ASSERT_EQ(got.orders_resting, got.ops_per_sec);
    ++good;
  }
  stop.store(true);
  producer.join();
  EXPECT_GT(good, 1900) << "against a writer publishing every 200 us the "
                           "reader should essentially always get through";
}

// --- the sink ---------------------------------------------------------------

TEST(TapSinkTest, EngineEventsReachTheRingUnchanged) {
  const std::string name = unique_name("sink");
  SegmentGuard guard(name);

  TapSink sink;
  ASSERT_TRUE(sink.open(name, 4096)) << sink.error();

  FastBookConfig ladder;
  ladder.max_orders = 4096;
  ladder.centre_price = 1002000;
  MatchingEngine<FastBook, TapSink> engine(sink, EngineConfig{}, ladder);

  NewOrder n{};
  n.id = 1;
  n.side = Side::Sell;
  n.price = 1002400;
  n.qty = 300;
  engine.on_new(n, 1000);
  n.id = 2;
  n.side = Side::Buy;
  n.qty = 100;
  engine.on_new(n, 1001);

  ShmTapReader reader;
  ASSERT_TRUE(reader.attach(name));
  std::vector<TapRecord> out(64);
  const std::size_t got = reader.read(out.data(), out.size());
  ASSERT_GE(got, 4U); // two Acks, a Trade and a Fill

  EXPECT_EQ(out[0].type, static_cast<std::uint8_t>(EventType::Ack));
  EXPECT_EQ(out[0].id, 1U);
  EXPECT_EQ(out[0].price, 1002400);
  for (std::size_t i = 0; i < got; ++i) EXPECT_EQ(out[i].seq, i);

  bool saw_trade = false;
  for (std::size_t i = 0; i < got; ++i) {
    if (out[i].type == static_cast<std::uint8_t>(EventType::Trade)) {
      saw_trade = true;
      EXPECT_EQ(out[i].price, 1002400) << "trades print at the resting price";
      EXPECT_EQ(out[i].qty, 100U);
      EXPECT_EQ(out[i].counter_id, 1U);
    }
  }
  EXPECT_TRUE(saw_trade);
  EXPECT_EQ(sink.dropped(), 0U);
}

TEST(TapSinkTest, AFullRingDoesNotStopTheEngine) {
  const std::string name = unique_name("sinkfull");
  SegmentGuard guard(name);

  TapSink sink;
  ASSERT_TRUE(sink.open(name, 8)); // absurdly small on purpose

  FastBookConfig ladder;
  ladder.max_orders = 1 << 16;
  ladder.centre_price = 1002000;
  MatchingEngine<FastBook, TapSink> engine(sink, EngineConfig{}, ladder);

  FlowConfig cfg;
  cfg.seed = 3;
  SyntheticFlow flow(cfg);
  const auto commands = flow.generate(20000);
  for (const Command& c : commands) engine.apply(c);

  EXPECT_EQ(engine.stats().commands, 20000U)
      << "the engine must process every command regardless of the tap";
  EXPECT_GT(sink.dropped(), 0U);
  EXPECT_GT(engine.stats().trades, 0U);
  std::cerr << "[ tap      ] " << sink.produced() << " records written, "
            << sink.dropped() << " dropped into an 8-slot ring\n";
}

} // namespace
} // namespace miniex
