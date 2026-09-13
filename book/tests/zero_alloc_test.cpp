// "No allocation after construction" is the kind of claim that is either
// enforced by a test or quietly false. This translation unit replaces the
// global operator new and counts every call, then runs a million mixed
// operations through FastBook and asserts the counter did not move.
//
// It is a separate binary on purpose: a global operator new replacement is
// process-wide, and making every other test pay for the counter would be a
// silly price for a guarantee only this one needs.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#include <gtest/gtest.h>

#include "book/fast_book.hpp"
#include "replay/random.hpp"

namespace {

/// Counts allocations. Relaxed because nothing here is concurrent; the atomic
/// is only to keep the sanitizers happy about the replacement being called
/// from anywhere.
std::atomic<std::uint64_t> g_allocations{0};
std::atomic<bool> g_counting{false};

void* allocate(std::size_t bytes) {
  if (g_counting.load(std::memory_order_relaxed))
    g_allocations.fetch_add(1, std::memory_order_relaxed);
  // std::malloc(0) may return nullptr, which operator new must not.
  void* p = std::malloc(bytes == 0 ? 1 : bytes);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

} // namespace

void* operator new(std::size_t bytes) {
  return allocate(bytes);
}
void* operator new[](std::size_t bytes) {
  return allocate(bytes);
}
void* operator new(std::size_t bytes, std::align_val_t) {
  return allocate(bytes);
}
void* operator new[](std::size_t bytes, std::align_val_t) {
  return allocate(bytes);
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete[](void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}

namespace miniex {
namespace {

class AllocationScope {
public:
  AllocationScope() {
    g_allocations.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
  }
  ~AllocationScope() { g_counting.store(false, std::memory_order_relaxed); }
  [[nodiscard]] static std::uint64_t count() {
    return g_allocations.load(std::memory_order_relaxed);
  }
};

TEST(ZeroAllocation, TheCounterItselfWorks) {
  // If the replacement is not being called, the test below would pass
  // vacuously, which would be worse than failing.
  AllocationScope scope;
  auto* leak = new int(5);
  EXPECT_GE(AllocationScope::count(), 1U);
  delete leak;
}

TEST(ZeroAllocation, FastBookDoesNotAllocateInSteadyState) {
  FastBook::Config cfg;
  cfg.max_orders = 1 << 16;
  cfg.centre_price = 1002000;
  cfg.edge_fraction = 0; // re-centring is a separate question; see below
  FastBook book(cfg);

  // Touch the structures once outside the counted region, so that any
  // lazily-built state in the standard library is already built.
  ASSERT_TRUE(book.add(1, Side::Buy, 1002000, 10, 1, 0));
  ASSERT_TRUE(book.cancel(1));

  Rng rng(20260913);
  std::vector<OrderId> live;
  live.reserve(cfg.max_orders);
  OrderId next_id = 2;

  {
    AllocationScope scope;
    for (std::size_t step = 0; step < 1'000'000; ++step) {
      const std::uint32_t roll = rng.below(100);
      if ((roll < 50 || live.empty()) && book.can_add()) {
        // Strictly inside the ladder window: the overflow map is a std::map
        // and allocates by construction. That is the documented exception,
        // and it is exercised by its own test below.
        const Price price = 1002000 + static_cast<Price>(rng.below(400)) - 200;
        const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
        if (book.add(next_id, side, price, static_cast<Qty>(1 + rng.below(500)),
                     static_cast<TsNanos>(step), 0)) {
          live.push_back(next_id);
        }
        ++next_id;
      } else if (!live.empty()) {
        const std::size_t k =
            rng.below(static_cast<std::uint32_t>(live.size()));
        (void)book.cancel(live[k]);
        live[k] = live.back();
        live.pop_back();
      }
    }
    EXPECT_EQ(AllocationScope::count(), 0U)
        << "FastBook allocated during steady-state operation";
  }

  EXPECT_TRUE(book.verify_aggregates());
}

TEST(ZeroAllocation, TheOverflowPathIsTheDocumentedException) {
  // This is not a failure -- it is the cost of the window being finite, and
  // saying so with a test is better than a comment claiming it.
  FastBook::Config cfg;
  cfg.max_orders = 1 << 12;
  cfg.centre_price = 1002000;
  cfg.edge_fraction = 0;
  FastBook book(cfg);
  const Price outside =
      book.ladder_base() + static_cast<Price>(FastBook::ladder_width()) + 1;

  AllocationScope scope;
  ASSERT_TRUE(book.add(1, Side::Sell, outside, 10, 1, 0));
  EXPECT_GT(AllocationScope::count(), 0U)
      << "the overflow map is a std::map; if this stops allocating, the "
         "documentation in fast_book.hpp is out of date";
}

TEST(ZeroAllocation, MatchingConsumesTheBookWithoutAllocating) {
  FastBook::Config cfg;
  cfg.max_orders = 1 << 14;
  cfg.centre_price = 1002000;
  cfg.edge_fraction = 0;
  FastBook book(cfg);
  for (OrderId id = 1; id <= 8000; ++id) {
    ASSERT_TRUE(book.add(id, Side::Sell, 1002000 + static_cast<Price>(id % 64),
                         100, static_cast<TsNanos>(id), 0));
  }

  AllocationScope scope;
  std::size_t filled = 0;
  while (OrderHandle h = book.front(Side::Sell)) {
    book.fill(h, h->qty_open);
    ++filled;
  }
  EXPECT_EQ(AllocationScope::count(), 0U);
  EXPECT_EQ(filled, 8000U);
  EXPECT_EQ(book.order_count(), 0U);
}

} // namespace
} // namespace miniex
