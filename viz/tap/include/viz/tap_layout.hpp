/// \file tap_layout.hpp
/// \brief The shared-memory layout the engine writes and the server reads.
///
/// This header is the *contract* between a C++ process and a Python process,
/// so every offset and size in it is fixed by hand and asserted. The Python
/// side (`viz/server/shm_reader.py`) re-declares the same numbers, and a test
/// compares the two. A layout that drifts between the two languages does not
/// fail loudly; it produces plausible garbage.
///
/// ```text
/// offset    0 : Header   { magic, version, capacity, record_size, flags, ... }
/// offset   64 : producer head   -- engine writes, server reads
/// offset  128 : consumer tail   -- server writes, engine reads
/// offset  192 : counters        { produced, dropped, epoch }
/// offset  256 : stats snapshot  (seqlock-protected)
/// offset 4096 : record array    (capacity * 64 bytes)
/// ```
///
/// Head and tail are a page apart from everything else and from each other so
/// that the engine's stores never invalidate the line the reader is polling,
/// and vice versa -- the same argument as `core/spsc_ring.hpp`, but now across
/// processes, where getting it wrong costs more because the reader is free to
/// spin as hard as it likes.
///
/// **Versioning.** `kTapVersion` changes whenever any offset, size or field
/// meaning changes. The reader refuses to attach on a mismatch rather than
/// reinterpreting bytes. There is no compatibility shim and there should not
/// be one: the two sides are built from the same tree.
#pragma once

#include <cstddef>
#include <cstdint>

namespace miniex {

/// "MINIEXTP" as a little-endian 64-bit value, so a hexdump of the first
/// eight bytes of the segment reads as text.
inline constexpr std::uint64_t kTapMagic = 0x5054584549'4E494DULL;

/// Bump on ANY layout change. The reader refuses a mismatch.
inline constexpr std::uint32_t kTapVersion = 1;

/// Where the record array starts. A page, so the records begin on a page
/// boundary and the control block never shares a page with them.
inline constexpr std::size_t kTapDataOffset = 4096;

inline constexpr std::size_t kTapCacheLine = 64;

/// Flags in the header.
enum TapFlags : std::uint32_t {
  kTapFlagNone = 0,
  /// The engine is still attached. Cleared on a clean shutdown, so a reader
  /// can tell "the producer finished" from "the producer died".
  kTapFlagProducerLive = 1U << 0,
};

/// The event record. One cache line, trivially copyable, no pointers: the
/// same 64 bytes the engine's `Event` occupies, minus the fields a dashboard
/// cannot use and plus the level index it can.
struct alignas(64) TapRecord {
  std::uint64_t seq;
  std::int64_t ts;
  std::int64_t price;
  std::uint32_t qty;
  std::uint32_t qty_remaining;
  std::uint64_t id;
  std::uint64_t counter_id;
  std::uint32_t level_index;
  std::uint8_t type;
  std::uint8_t side;
  std::uint8_t reason;
  std::uint8_t flags;
};
static_assert(sizeof(TapRecord) == 64, "a tap record must be one cache line");
static_assert(alignof(TapRecord) == 64);

/// Header, at offset 0.
struct TapHeader {
  std::uint64_t magic;
  std::uint32_t version;
  std::uint32_t record_size;
  std::uint64_t capacity; ///< records; always a power of two
  std::uint32_t flags;
  std::uint32_t _pad0;
  std::int64_t created_ns;
  std::uint64_t _pad1[3];
};
static_assert(sizeof(TapHeader) == 64);

/// A 64-bit counter alone on its own cache line.
struct alignas(64) TapCursor {
  std::uint64_t value;
  std::uint8_t _pad[56];
};
static_assert(sizeof(TapCursor) == 64);

/// Free-running counters, at offset 192.
struct alignas(64) TapCounters {
  std::uint64_t produced;
  std::uint64_t dropped;
  std::uint64_t epoch; ///< incremented when the engine restarts a run
  std::uint64_t _pad[5];
};
static_assert(sizeof(TapCounters) == 64);

/// A periodic snapshot of engine statistics, at offset 256.
///
/// Protected by a **seqlock** rather than a mutex: the engine writes it from
/// the hot thread and must never block on a reader, and the reader must never
/// see a half-written snapshot. Writer increments `seq` to odd, writes the
/// fields, increments to even. Reader reads `seq`, the fields, then `seq`
/// again, and retries if either read was odd or the two differ. A torn read
/// is detected, not prevented, which is exactly the right trade when the
/// writer must not wait.
struct alignas(64) TapStats {
  std::uint64_t seq; ///< odd while being written
  std::uint64_t ops_per_sec;
  std::int64_t p50_ns;
  std::int64_t p99_ns;
  std::int64_t p999_ns;
  std::int64_t max_ns;
  std::uint64_t orders_resting;
  std::uint64_t schedule_misses;
};
static_assert(sizeof(TapStats) == 64);

inline constexpr std::size_t kTapHeaderOffset = 0;
inline constexpr std::size_t kTapHeadOffset = 64;
inline constexpr std::size_t kTapTailOffset = 128;
inline constexpr std::size_t kTapCountersOffset = 192;
inline constexpr std::size_t kTapStatsOffset = 256;

/// Total segment size for a ring of `capacity` records.
constexpr std::size_t tap_segment_size(std::size_t capacity) {
  return kTapDataOffset + capacity * sizeof(TapRecord);
}

} // namespace miniex
