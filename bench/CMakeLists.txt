# Benchmarks link against the fetched HdrHistogram and Google Benchmark.
# Neither is vendored; both are pinned to an exact tag so that a number
# produced today can be reproduced next year.

add_library(miniex_bench_support INTERFACE)
target_include_directories(miniex_bench_support INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/harness")
target_link_libraries(miniex_bench_support INTERFACE
  miniex_flags miniex::core miniex::engine miniex::book miniex::replay
  miniex_tool_support hdr_histogram_static Threads::Threads)
if(MINIEX_TAP)
  target_link_libraries(miniex_bench_support INTERFACE miniex::tap)
endif()

add_executable(engine_bench harness/engine_bench.cpp)
target_link_libraries(engine_bench PRIVATE miniex_bench_support)

add_executable(micro_bench
  micro/book_bench.cpp
  micro/core_bench.cpp
  micro/feed_bench.cpp)
target_link_libraries(micro_bench PRIVATE
  miniex_bench_support miniex::feed benchmark::benchmark
  benchmark::benchmark_main)
# feed_bench.cpp uses the test-only ITCH encoder to build its input stream.
target_include_directories(micro_bench PRIVATE "${CMAKE_SOURCE_DIR}/feed")
