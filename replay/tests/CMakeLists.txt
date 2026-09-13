miniex_add_test(replay_synthetic_test synthetic_test.cpp)
target_link_libraries(replay_synthetic_test PRIVATE miniex::replay miniex::book)

miniex_add_test(replay_lobster_test lobster_test.cpp)
target_link_libraries(replay_lobster_test PRIVATE miniex::replay miniex::book)
target_compile_definitions(replay_lobster_test PRIVATE
  MINIEX_LOBSTER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
