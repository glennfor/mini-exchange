add_library(miniex_feed INTERFACE)
add_library(miniex::feed ALIAS miniex_feed)

target_include_directories(miniex_feed INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(miniex_feed INTERFACE miniex::core)

if(MINIEX_BUILD_TESTS)
  add_subdirectory(tests)
endif()

add_subdirectory(fuzz)
