/// \file cache.hpp
/// \brief Cache-line constants shared by the lock-free primitives.
#pragma once

#include <cstddef>

namespace miniex {

/// Assumed cache line size. 64 bytes on x86-64 and on Apple Silicon's L1
/// (the M-series L2 line is 128, which is why the SPSC ring separates its
/// counters by kDestructiveInterference rather than by exactly one line).
inline constexpr std::size_t kCacheLine = 64;

/// Padding used to keep independently-written variables off each other's
/// coherence unit. std::hardware_destructive_interference_size would be the
/// standard spelling, but libc++ does not ship it and GCC warns that its ABI
/// is unstable, so the value is fixed here and asserted in tests instead.
inline constexpr std::size_t kDestructiveInterference = 128;

} // namespace miniex
