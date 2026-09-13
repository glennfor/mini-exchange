add_library(miniex_engine INTERFACE)
add_library(miniex::engine ALIAS miniex_engine)

target_include_directories(miniex_engine INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(miniex_engine INTERFACE miniex::core miniex::book)

if(MINIEX_BUILD_TESTS)
  add_subdirectory(tests)
endif()
