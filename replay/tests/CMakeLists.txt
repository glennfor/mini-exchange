miniex_add_test(replay_synthetic_test synthetic_test.cpp)
target_link_libraries(replay_synthetic_test PRIVATE miniex::replay miniex::book)
