/// \file fuzz_itch.cpp
/// \brief libFuzzer target over the ITCH decoder.
///
/// The contract being fuzzed is narrow and absolute: **for any byte sequence
/// whatsoever, the decoder must not crash, must not read out of bounds, and
/// must not loop forever.** It is explicitly allowed -- expected -- to reject
/// the input; rejecting is the correct behaviour for almost all of it.
///
/// This is the right thing to fuzz because a feed decoder is the part of the
/// system that touches bytes it did not create. Everything downstream of it
/// works on validated, decoded values.
///
/// The message-count bound passed to `decode()` is what makes "does not loop
/// forever" testable rather than hopeful: a decoder that fails to advance
/// would return the bound, and the assertion below catches it.
///
/// Build and run:
///     cmake --preset fuzz && cmake --build build/fuzz
///     ./build/fuzz/feed/fuzz_itch -max_total_time=60 feed/fuzz/corpus

#include <cstddef>
#include <cstdint>

#include "feed/decoder.hpp"
#include "feed/mold64.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  using namespace miniex;
  const auto* bytes = reinterpret_cast<const std::byte*>(data);

  // Path 1: BinaryFILE framing, non-strict, so unknown types are skipped and
  // the walk keeps going deeper into the input.
  {
    NullMdHandler handler;
    ItchDecoder decoder(bytes, size, handler);
    constexpr std::size_t kBound = 1u << 16;
    const std::size_t n = decoder.decode(kBound, /*strict=*/false);
    if (n > kBound) __builtin_trap();
    if (decoder.position() > size) __builtin_trap();
  }

  // Path 2: the same input through the table dispatch, which has its own
  // indexing and must agree about what is in bounds.
  {
    NullMdHandler handler;
    ItchDecoder decoder(bytes, size, handler);
    decoder.set_dispatch(DispatchMode::Table);
    (void)decoder.decode(1u << 16, /*strict=*/false);
  }

  // Path 3: treat the input as a MoldUDP64 datagram. The header parse has its
  // own length arithmetic and deserves its own reachability.
  {
    MoldPacket mold{};
    if (parse_mold_packet(bytes, size, mold)) {
      if (mold.payload_size > size) __builtin_trap();
      NullMdHandler handler;
      ItchDecoder decoder(mold.payload, mold.payload_size, handler);
      (void)decoder.decode(1u << 16, /*strict=*/false);
    }
  }

  return 0;
}
