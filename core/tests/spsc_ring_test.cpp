#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/spsc_ring.hpp"

namespace miniex {
namespace {

struct Payload {
  std::uint64_t seq;
  std::uint64_t checksum;
};

TEST(SpscRing, StartsEmpty) {
  SpscRing<int, 8> ring;
  EXPECT_TRUE(ring.empty());
  EXPECT_EQ(ring.size(), 0U);
  EXPECT_EQ(ring.capacity(), 8U);
  int out = -1;
  EXPECT_FALSE(ring.try_pop(out));
  EXPECT_EQ(out, -1);
}

TEST(SpscRing, FillThenDrainPreservesOrder) {
  SpscRing<int, 8> ring;
  for (int i = 0; i < 8; ++i) EXPECT_TRUE(ring.try_push(i));
  EXPECT_TRUE(ring.full());
  EXPECT_EQ(ring.size(), 8U);
  for (int i = 0; i < 8; ++i) {
    int out = -1;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, i);
  }
  EXPECT_TRUE(ring.empty());
}

TEST(SpscRing, PushOnFullRingFailsAndLeavesStateIntact) {
  SpscRing<int, 4> ring;
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(ring.try_push(i));
  EXPECT_FALSE(ring.try_push(99));
  EXPECT_FALSE(ring.try_push(99));
  EXPECT_EQ(ring.size(), 4U);
  // The rejected pushes must not have overwritten anything.
  for (int i = 0; i < 4; ++i) {
    int out = -1;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, i);
  }
}

TEST(SpscRing, WrapsAroundTheCapacityBoundaryCorrectly) {
  SpscRing<int, 4> ring;
  // Walk the head around the buffer several times, keeping it partly full so
  // that every push and pop straddles the wrap point at some iteration.
  int expected_next_pop = 0;
  int next_push = 0;
  for (int round = 0; round < 100; ++round) {
    while (ring.size() < 3) ASSERT_TRUE(ring.try_push(next_push++));
    int out = -1;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, expected_next_pop++);
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, expected_next_pop++);
  }
}

TEST(SpscRing, BatchPushAndPopMoveWhatTheyCan) {
  SpscRing<int, 8> ring;
  const int src[6] = {10, 11, 12, 13, 14, 15};
  EXPECT_EQ(ring.try_push_n(src, 6), 6U);
  EXPECT_EQ(ring.try_push_n(src, 6), 2U); // only two slots left
  EXPECT_TRUE(ring.full());

  int dst[8] = {};
  EXPECT_EQ(ring.try_pop_n(dst, 8), 8U);
  EXPECT_EQ(dst[0], 10);
  EXPECT_EQ(dst[5], 15);
  EXPECT_EQ(dst[6], 10); // the two that fitted from the second batch
  EXPECT_EQ(dst[7], 11);
  EXPECT_EQ(ring.try_pop_n(dst, 8), 0U);
}

TEST(SpscRing, BatchOperationsStraddleTheWrapPoint) {
  SpscRing<int, 8> ring;
  const int warm[5] = {0, 1, 2, 3, 4};
  ASSERT_EQ(ring.try_push_n(warm, 5), 5U);
  int drain[5] = {};
  ASSERT_EQ(ring.try_pop_n(drain, 5), 5U);
  // Head and tail now sit at index 5; a 6-element batch must wrap.
  const int src[6] = {100, 101, 102, 103, 104, 105};
  ASSERT_EQ(ring.try_push_n(src, 6), 6U);
  int dst[6] = {};
  ASSERT_EQ(ring.try_pop_n(dst, 6), 6U);
  for (int i = 0; i < 6; ++i) EXPECT_EQ(dst[i], 100 + i);
}

TEST(SpscRing, CountersReportTotalsAcrossWraps) {
  SpscRing<int, 4> ring;
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(ring.try_push(i));
    int out = 0;
    ASSERT_TRUE(ring.try_pop(out));
  }
  EXPECT_EQ(ring.produced(), 20U);
  EXPECT_EQ(ring.consumed(), 20U);
  EXPECT_TRUE(ring.empty());
}

// The headline correctness test: two real threads, no gaps, no duplicates,
// no reordering. Run this one under TSan.
TEST(SpscRingConcurrency, ProducerConsumerSeesExactlyTheSameSequence) {
  constexpr std::uint64_t kItems = 1'000'000;
  auto ring = std::make_unique<SpscRing<Payload, 1024>>();
  std::atomic<bool> failed{false};
  std::atomic<std::uint64_t> received{0};

  std::thread consumer([&] {
    std::uint64_t expect = 0;
    Payload p{};
    while (expect < kItems) {
      if (ring->try_pop(p)) {
        if (p.seq != expect || p.checksum != p.seq * 2654435761ULL) {
          failed.store(true, std::memory_order_relaxed);
          break;
        }
        ++expect;
      }
    }
    received.store(expect, std::memory_order_relaxed);
  });

  for (std::uint64_t i = 0; i < kItems; ++i) {
    const Payload p{i, i * 2654435761ULL};
    while (!ring->try_push(p)) {
      // Busy-wait: the container never spins, the caller decides.
    }
  }
  consumer.join();

  EXPECT_FALSE(failed.load());
  EXPECT_EQ(received.load(), kItems);
  EXPECT_TRUE(ring->empty());
}

TEST(SpscRingConcurrency, BatchedTransferIsAlsoGapFree) {
  constexpr std::uint64_t kItems = 500'000;
  auto ring = std::make_unique<SpscRing<std::uint64_t, 4096>>();
  std::atomic<bool> failed{false};

  std::thread consumer([&] {
    std::uint64_t expect = 0;
    std::vector<std::uint64_t> buf(256);
    while (expect < kItems) {
      const std::size_t n = ring->try_pop_n(buf.data(), buf.size());
      for (std::size_t k = 0; k < n; ++k) {
        if (buf[k] != expect++) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
      }
    }
  });

  std::vector<std::uint64_t> batch(128);
  std::uint64_t next = 0;
  while (next < kItems) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(batch.size(), kItems - next));
    for (std::size_t k = 0; k < want; ++k) batch[k] = next + k;
    std::size_t pushed = 0;
    while (pushed < want) {
      pushed += ring->try_push_n(batch.data() + pushed, want - pushed);
    }
    next += want;
  }
  consumer.join();
  EXPECT_FALSE(failed.load());
}

} // namespace
} // namespace miniex
