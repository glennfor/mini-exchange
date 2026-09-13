miniex_add_test(viz_tap_test tap_test.cpp LABELS "unit;concurrency")
target_link_libraries(viz_tap_test PRIVATE
  miniex::tap miniex::book miniex::replay Threads::Threads)
