miniex_add_test(engine_matching_test matching_engine_test.cpp)
target_link_libraries(engine_matching_test PRIVATE miniex::engine)

miniex_add_test(engine_replace_test replace_test.cpp)
target_link_libraries(engine_replace_test PRIVATE miniex::engine)

miniex_add_test(engine_format_test format_test.cpp)
target_link_libraries(engine_format_test PRIVATE miniex::engine)

miniex_add_test(engine_invariant_test invariant_test.cpp)
target_link_libraries(engine_invariant_test PRIVATE miniex::engine)

miniex_add_test(engine_book_differential_test book_differential_test.cpp)
target_link_libraries(engine_book_differential_test PRIVATE
  miniex::engine miniex::replay)
