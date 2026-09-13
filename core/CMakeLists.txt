add_library(miniex_core INTERFACE)
add_library(miniex::core ALIAS miniex_core)

target_include_directories(miniex_core INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(miniex_core INTERFACE
  miniex_flags miniex_generated_config)

if(MINIEX_BUILD_TESTS)
  add_subdirectory(tests)
endif()
