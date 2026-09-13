miniex_add_test(feed_itch_test itch50_test.cpp)
target_link_libraries(feed_itch_test PRIVATE miniex::feed)

miniex_add_test(feed_decoder_test decoder_test.cpp)
target_link_libraries(feed_decoder_test PRIVATE miniex::feed)
