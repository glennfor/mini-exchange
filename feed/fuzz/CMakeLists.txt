# The fuzz target is only built when the compiler supports libFuzzer and the
# option is on. It is not part of the default build: -fsanitize=fuzzer changes
# how main() is provided and would break every other executable in the tree.
if(NOT MINIEX_BUILD_FUZZ)
  return()
endif()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer-no-link")
check_cxx_source_compiles("
  #include <cstdint>
  #include <cstddef>
  extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }
  int main() { return 0; }" MINIEX_HAS_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)

if(NOT MINIEX_HAS_LIBFUZZER)
  message(STATUS "libFuzzer not available; skipping the ITCH fuzz target")
  return()
endif()

# AddressSanitizer is what turns "did not crash" into "did not read out of
# bounds", so it is on by default. It is separable because the compiler-rt
# ASan runtime does not work on the primary development host (DD-006): every
# ASan-instrumented binary hangs before main, fuzz targets included. Turning
# it off keeps the crash-and-hang coverage usable locally; CI runs with it on.
option(MINIEX_FUZZ_ASAN "Instrument the fuzz targets with AddressSanitizer" ON)

set(MINIEX_FUZZ_SANITIZERS fuzzer)
if(MINIEX_FUZZ_ASAN)
  set(MINIEX_FUZZ_SANITIZERS fuzzer,address)
endif()

add_executable(fuzz_itch fuzz_itch.cpp)
target_link_libraries(fuzz_itch PRIVATE miniex_flags miniex::feed)
target_compile_options(fuzz_itch PRIVATE -fsanitize=${MINIEX_FUZZ_SANITIZERS})
target_link_options(fuzz_itch PRIVATE -fsanitize=${MINIEX_FUZZ_SANITIZERS})
