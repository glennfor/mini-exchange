add_library(miniex_replay INTERFACE)
add_library(miniex::replay ALIAS miniex_replay)

target_include_directories(miniex_replay INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(miniex_replay INTERFACE miniex::core miniex::engine)

if(MINIEX_BUILD_TESTS)
  add_subdirectory(tests)
endif()
