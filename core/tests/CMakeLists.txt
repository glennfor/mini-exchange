miniex_add_test(core_types_test types_test.cpp)
target_link_libraries(core_types_test PRIVATE miniex::core)

miniex_add_test(core_object_pool_test object_pool_test.cpp)
target_link_libraries(core_object_pool_test PRIVATE miniex::core)

miniex_add_test(core_open_hash_map_test open_hash_map_test.cpp)
target_link_libraries(core_open_hash_map_test PRIVATE miniex::core)

miniex_add_test(core_bitset_ladder_test bitset_ladder_test.cpp)
target_link_libraries(core_bitset_ladder_test PRIVATE miniex::core)

miniex_add_test(core_timestamp_test timestamp_test.cpp)
target_link_libraries(core_timestamp_test PRIVATE miniex::core)

# The ring is the only concurrent structure in the repository; its tests are
# labelled so the TSan preset can run exactly these and nothing else.
miniex_add_test(core_spsc_ring_test spsc_ring_test.cpp LABELS "unit;concurrency")
target_link_libraries(core_spsc_ring_test PRIVATE miniex::core Threads::Threads)
