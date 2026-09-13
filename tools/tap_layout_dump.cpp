/// \file tap_layout_dump.cpp
/// \brief Prints the C++ side of the tap layout as JSON.
///
/// Exists so that `viz/server/tests/test_layout.py` can compare the two
/// language's ideas of the binary layout field by field. That drift check is
/// worth having because a mismatch does not fail loudly: the Python reader
/// would happily unpack the wrong offsets and the dashboard would show
/// confident nonsense.

#include <cstdio>

#include "viz/tap_layout.hpp"

int main() {
  using namespace miniex;
  std::printf("{\n");
  std::printf("  \"magic\": %llu,\n", (unsigned long long)kTapMagic);
  std::printf("  \"version\": %u,\n", kTapVersion);
  std::printf("  \"header_offset\": %zu,\n", kTapHeaderOffset);
  std::printf("  \"head_offset\": %zu,\n", kTapHeadOffset);
  std::printf("  \"tail_offset\": %zu,\n", kTapTailOffset);
  std::printf("  \"counters_offset\": %zu,\n", kTapCountersOffset);
  std::printf("  \"stats_offset\": %zu,\n", kTapStatsOffset);
  std::printf("  \"data_offset\": %zu,\n", kTapDataOffset);
  std::printf("  \"record_size\": %zu,\n", sizeof(TapRecord));
  std::printf("  \"header_size\": %zu,\n", sizeof(TapHeader));
  std::printf("  \"stats_size\": %zu,\n", sizeof(TapStats));
  std::printf("  \"segment_size_1024\": %zu,\n", tap_segment_size(1024));
  // Field offsets within a record, so a reordering is caught too, not just a
  // size change.
  std::printf("  \"record_fields\": {\n");
  std::printf("    \"seq\": %zu,\n", offsetof(TapRecord, seq));
  std::printf("    \"ts\": %zu,\n", offsetof(TapRecord, ts));
  std::printf("    \"price\": %zu,\n", offsetof(TapRecord, price));
  std::printf("    \"qty\": %zu,\n", offsetof(TapRecord, qty));
  std::printf("    \"qty_remaining\": %zu,\n",
              offsetof(TapRecord, qty_remaining));
  std::printf("    \"id\": %zu,\n", offsetof(TapRecord, id));
  std::printf("    \"counter_id\": %zu,\n", offsetof(TapRecord, counter_id));
  std::printf("    \"level_index\": %zu,\n", offsetof(TapRecord, level_index));
  std::printf("    \"type\": %zu,\n", offsetof(TapRecord, type));
  std::printf("    \"side\": %zu,\n", offsetof(TapRecord, side));
  std::printf("    \"reason\": %zu,\n", offsetof(TapRecord, reason));
  std::printf("    \"flags\": %zu\n", offsetof(TapRecord, flags));
  std::printf("  },\n");
  std::printf("  \"stats_fields\": {\n");
  std::printf("    \"seq\": %zu,\n", offsetof(TapStats, seq));
  std::printf("    \"ops_per_sec\": %zu,\n", offsetof(TapStats, ops_per_sec));
  std::printf("    \"p50_ns\": %zu,\n", offsetof(TapStats, p50_ns));
  std::printf("    \"p99_ns\": %zu,\n", offsetof(TapStats, p99_ns));
  std::printf("    \"p999_ns\": %zu,\n", offsetof(TapStats, p999_ns));
  std::printf("    \"max_ns\": %zu,\n", offsetof(TapStats, max_ns));
  std::printf("    \"orders_resting\": %zu,\n",
              offsetof(TapStats, orders_resting));
  std::printf("    \"schedule_misses\": %zu\n",
              offsetof(TapStats, schedule_misses));
  std::printf("  }\n");
  std::printf("}\n");
  return 0;
}
