add_library(miniex_book INTERFACE)
add_library(miniex::book ALIAS miniex_book)

target_include_directories(miniex_book INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(miniex_book INTERFACE miniex::core)

if(MINIEX_BUILD_TESTS)
  add_subdirectory(tests)
endif()
