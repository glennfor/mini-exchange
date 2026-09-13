add_library(miniex_tool_support INTERFACE)
target_include_directories(miniex_tool_support INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(miniex_tool_support INTERFACE miniex_flags miniex::core)

add_executable(engine_replay engine_replay.cpp)
target_link_libraries(engine_replay PRIVATE
  miniex_flags miniex::engine miniex::replay miniex_tool_support)
