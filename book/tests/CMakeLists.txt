miniex_add_test(book_naive_test naive_book_test.cpp)
target_link_libraries(book_naive_test PRIVATE miniex::book)

miniex_add_test(book_fast_test fast_book_test.cpp)
target_link_libraries(book_fast_test PRIVATE miniex::book miniex::replay)

# The zero-allocation test needs its own translation unit because it replaces
# the global operator new; linking it with anything else would make every
# other test in the binary pay for the counter.
miniex_add_test(book_zero_alloc_test zero_alloc_test.cpp)
target_link_libraries(book_zero_alloc_test PRIVATE miniex::book miniex::replay)

miniex_add_test(book_md_test md_book_test.cpp)
target_link_libraries(book_md_test PRIVATE miniex::book)
