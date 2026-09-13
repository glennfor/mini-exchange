# The tap is a header-only library over POSIX shared memory. It is always
# *buildable* -- the tests exercise it regardless -- but the engine only links
# it when MINIEX_TAP is on, which is what makes "the tap compiles out
# entirely" a testable claim rather than a hopeful one.
add_library(miniex_tap INTERFACE)
add_library(miniex::tap ALIAS miniex_tap)

target_include_directories(miniex_tap INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/tap/include")
target_link_libraries(miniex_tap INTERFACE miniex::core miniex::engine)

# librt carries shm_open on Linux; on macOS and the BSDs it is in libc.
include(CheckLibraryExists)
check_library_exists(rt shm_open "" MINIEX_HAVE_LIBRT)
if(MINIEX_HAVE_LIBRT)
  target_link_libraries(miniex_tap INTERFACE rt)
endif()

if(MINIEX_BUILD_TESTS)
  add_subdirectory(tests)
endif()
