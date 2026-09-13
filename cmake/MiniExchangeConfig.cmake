# Compile-time engine sizing constants.
#
# Every one of these is a cache variable so that `bench/` can sweep them
# without editing headers (spec section 6.2). The values used for any
# published benchmark number are recorded in the run's provenance header.

set(MINIEX_MAX_ORDERS    4194304 CACHE STRING "Live order capacity (power of two)")
set(MINIEX_LADDER_TICKS   131072 CACHE STRING "Price ladder window width in ticks (power of two)")
set(MINIEX_TICK_SIZE           1 CACHE STRING "Tick size in Price units (1e-4 dollars)")
set(MINIEX_RING_CAPACITY   65536 CACHE STRING "SPSC ingress ring capacity (power of two)")
set(MINIEX_TAP_CAPACITY  1048576 CACHE STRING "Observability tap ring capacity (power of two)")
set(MINIEX_COMPLETED_RING  65536 CACHE STRING "Recently-completed order id ring (power of two)")

option(MINIEX_PAD_PRICE_LEVEL
  "Pad PriceLevel to a full cache line (see DD-021; measure both ways)" ON)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/core/config.hpp"
  @ONLY)

add_library(miniex_generated_config INTERFACE)
target_include_directories(miniex_generated_config INTERFACE
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
