add_library(miniex_tool_support INTERFACE)
target_include_directories(miniex_tool_support INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(miniex_tool_support INTERFACE miniex_flags miniex::core)

add_executable(engine_replay engine_replay.cpp)
target_link_libraries(engine_replay PRIVATE
  miniex_flags miniex::engine miniex::replay miniex_tool_support)

add_executable(itch_stats itch_stats.cpp)
target_link_libraries(itch_stats PRIVATE
  miniex_flags miniex::feed miniex_tool_support)

add_executable(book_dump book_dump.cpp)
target_link_libraries(book_dump PRIVATE
  miniex_flags miniex::feed miniex::book miniex_tool_support)

add_executable(lobster_check lobster_check.cpp)
target_link_libraries(lobster_check PRIVATE
  miniex_flags miniex::book miniex::replay miniex_tool_support)

add_executable(inject inject.cpp)
target_link_libraries(inject PRIVATE
  miniex_flags miniex::feed miniex::engine miniex::replay miniex_tool_support)

add_executable(flow_profile flow_profile.cpp)
target_link_libraries(flow_profile PRIVATE
  miniex_flags miniex::feed miniex::book miniex_tool_support)

add_executable(tape_print tape_print.cpp)
target_link_libraries(tape_print PRIVATE
  miniex_flags miniex::feed miniex::book miniex_tool_support)

# Prints the tap layout as JSON, so the Python reader's idea of it can be
# compared against the C++ header's. See viz/server/tests/test_layout.py.
add_executable(tap_layout_dump tap_layout_dump.cpp)
target_link_libraries(tap_layout_dump PRIVATE miniex_flags miniex::tap)

add_executable(tap_producer tap_producer.cpp)
target_link_libraries(tap_producer PRIVATE
  miniex_flags miniex::tap miniex::book miniex::replay miniex_tool_support)
