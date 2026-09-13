/// \file prefault.hpp
/// \brief Touch every page of an allocation so the kernel maps it now.
///
/// A large `operator new` returns a range of *virtual* addresses. The physical
/// pages behind it do not exist until something writes to each one, and each
/// of those first writes is a page fault: a trap into the kernel, a page
/// allocation, and a TLB shootdown, costing microseconds.
///
/// That is fine for a throughput benchmark, which amortises it over millions
/// of operations, and it is invisible to a microbenchmark, which faults its
/// working set in during the warm-up iterations Google Benchmark discards.
///
/// It is **not** fine for a tail-latency measurement. `FastBook` at 262144
/// orders allocates roughly 40 MB across its pool, index and ladder — ten
/// thousand pages, ten thousand faults, scattered through the first minutes of
/// a run and landing squarely in p99 and p99.9. Measured on the development
/// machine, pre-faulting moved `FastBook`'s open-loop p99 from being worse
/// than `NaiveBook`'s to better than it; see docs/design-decisions.md DD-034.
///
/// This is what a latency-sensitive system does at start-up, and doing it in
/// the constructor is exactly the right place: construction is allowed to be
/// slow, and steady state is not.
#pragma once

#include <cstddef>
#include <cstdint>

namespace miniex {

/// Assumed page size. Touching every 4 KB is correct on any platform whose
/// pages are 4 KB or larger -- a 16 KB page just gets touched four times, and
/// the extra stores are a rounding error against the fault they prevent.
inline constexpr std::size_t kPageSize = 4096;

/// Writes one byte per page across \p bytes bytes starting at \p base.
///
/// Writes rather than reads: a read of an untouched anonymous page may be
/// served by the shared zero page, which defers the real allocation to the
/// first write -- i.e. to exactly the moment this is trying to avoid.
inline void prefault(void* base, std::size_t bytes) noexcept {
  if (base == nullptr || bytes == 0) return;
  auto* p = static_cast<volatile std::uint8_t*>(base);
  for (std::size_t offset = 0; offset < bytes; offset += kPageSize) {
    p[offset] = 0;
  }
  // The final page, in case `bytes` is not a multiple of the page size.
  p[bytes - 1] = 0;
}

} // namespace miniex
