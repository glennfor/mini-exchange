cmake_minimum_required(VERSION 3.24)

project(mini_exchange
  VERSION 0.1.0
  DESCRIPTION "Price-time-priority matching engine and ITCH 5.0 feed handler"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MINIEX_BUILD_TESTS "Build the unit/property test suite" ON)
option(MINIEX_BUILD_BENCH "Build microbenchmarks and the latency harness" OFF)
option(MINIEX_BUILD_TOOLS "Build the command-line tools" ON)
option(MINIEX_TAP         "Compile the observability tap into the engine" OFF)
option(MINIEX_WERROR      "Treat warnings as errors" ON)
option(MINIEX_NATIVE      "Build with -march=native" OFF)

include(MiniExchangeConfig)

# ---------------------------------------------------------------------------
# Common compile settings, applied via an interface target
# ---------------------------------------------------------------------------
add_library(miniex_flags INTERFACE)
target_compile_features(miniex_flags INTERFACE cxx_std_20)
target_compile_options(miniex_flags INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wcast-align -Wunused
  -Woverloaded-virtual -Wdouble-promotion -Wformat=2)
if(MINIEX_WERROR)
  target_compile_options(miniex_flags INTERFACE -Werror)
endif()
if(MINIEX_NATIVE)
  # Apple clang on Apple Silicon prefers -mcpu=native.
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-march=native MINIEX_HAS_MARCH_NATIVE)
  if(MINIEX_HAS_MARCH_NATIVE)
    target_compile_options(miniex_flags INTERFACE -march=native)
  else()
    target_compile_options(miniex_flags INTERFACE -mcpu=native)
  endif()
endif()
if(MINIEX_TAP)
  target_compile_definitions(miniex_flags INTERFACE MINIEX_TAP=1)
endif()

# ---------------------------------------------------------------------------
# Third-party dependencies (fetched, never vendored -- see spec 3.3)
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

if(MINIEX_BUILD_TESTS)
  enable_testing()
  # GoogleTest is always fetched, never taken from the environment: a stray
  # system/conda install produces link-time surprises and, worse, silently
  # different assertion behaviour between developer machines and CI.
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)
endif()

# ---------------------------------------------------------------------------
# Helper: declare a test executable for a module
# ---------------------------------------------------------------------------
function(miniex_add_test name)
  if(NOT MINIEX_BUILD_TESTS)
    return()
  endif()
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE miniex_flags GTest::gtest_main)
  gtest_discover_tests(${name}
    PROPERTIES LABELS unit
    DISCOVERY_TIMEOUT 60)
endfunction()

# ---------------------------------------------------------------------------
# Modules
# ---------------------------------------------------------------------------
add_subdirectory(core)
