/// \file feed_bench.cpp
/// \brief ITCH decode throughput, and the dispatch comparison.
///
/// Spec 10.4 asks for three dispatch strategies to be measured and all three
/// numbers recorded: a `switch`, a 256-entry function-pointer table, and
/// computed goto. Two of them live in `feed/decoder.hpp`; computed goto is a
/// GNU extension and the project bans extensions in public headers, so it is
/// implemented here, in a benchmark translation unit, where an extension
/// costs nothing.
///
/// The stream is generated once, outside the measured loop, with the
/// test-only encoder.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

#include "feed/decoder.hpp"
#include "feed/itch50.hpp"
#include "feed/normalizer.hpp"
#include "replay/random.hpp"

#include "tests/itch_encoder.hpp"

namespace miniex {
namespace {

/// A stream with a realistic message mix: mostly adds and deletes, some
/// cancels and replaces, a few executions and prints. Benchmarking a decoder
/// on a stream of one message type measures the branch predictor's memory,
/// not the decoder.
const std::vector<std::uint8_t>& stream() {
  static const std::vector<std::uint8_t> s = [] {
    Rng rng(4711);
    std::vector<std::vector<std::uint8_t>> messages;
    messages.reserve(200000);
    TsNanos ts = 34200000000000;
    OrderId ref = 1;
    for (std::size_t i = 0; i < 200000; ++i) {
      ts += rng.between(1, 5000);
      const std::uint32_t roll = rng.below(100);
      if (roll < 45) {
        messages.push_back(test::encode_add_order(
            1, static_cast<std::uint16_t>(i), ts, ref++,
            rng.chance(50) ? Side::Buy : Side::Sell,
            static_cast<Qty>(1 + rng.below(1000)), "AAPL",
            1002000 + static_cast<Price>(rng.below(100))));
      } else if (roll < 75) {
        messages.push_back(test::encode_order_delete(
            1, static_cast<std::uint16_t>(i), ts, 1 + rng.below(1000)));
      } else if (roll < 85) {
        messages.push_back(test::encode_order_cancel(
            1, static_cast<std::uint16_t>(i), ts, 1 + rng.below(1000),
            static_cast<Qty>(1 + rng.below(100))));
      } else if (roll < 93) {
        messages.push_back(test::encode_order_replace(
            1, static_cast<std::uint16_t>(i), ts, 1 + rng.below(1000), ref++,
            static_cast<Qty>(1 + rng.below(500)),
            1002000 + static_cast<Price>(rng.below(100))));
      } else if (roll < 98) {
        messages.push_back(test::encode_order_executed(
            1, static_cast<std::uint16_t>(i), ts, 1 + rng.below(1000),
            static_cast<Qty>(1 + rng.below(200)), i));
      } else {
        messages.push_back(test::encode_trade(
            1, static_cast<std::uint16_t>(i), ts, 0, Side::Sell,
            static_cast<Qty>(1 + rng.below(300)), "AAPL", 1002000, i));
      }
    }
    return test::frame_all(messages);
  }();
  return s;
}

const std::byte* bytes() {
  return reinterpret_cast<const std::byte*>(stream().data());
}

void decode_with(benchmark::State& state, DispatchMode mode) {
  const std::size_t size = stream().size();
  std::uint64_t messages = 0;
  for (auto _ : state) {
    NullMdHandler handler;
    ItchDecoder decoder(bytes(), size, handler);
    decoder.set_dispatch(mode);
    const std::size_t n = decoder.decode(~std::size_t{0}, /*strict=*/false);
    messages = n;
    benchmark::DoNotOptimize(messages);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(messages));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

void BM_ItchDecode_Switch(benchmark::State& state) {
  decode_with(state, DispatchMode::Switch);
}
void BM_ItchDecode_Table(benchmark::State& state) {
  decode_with(state, DispatchMode::Table);
}

#if defined(__GNUC__)
// -Wpedantic rejects the address-of-label extension, correctly: it is not
// standard C++. It is suppressed for exactly this function, because the whole
// point of the function is to measure the extension.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wgnu-label-as-value"
#endif

/// Computed goto: a label-address table indexed by the type byte, jumped to
/// directly. It removes the call/return of the function-pointer table and
/// gives each message type its own indirect-branch site, which historically
/// helped older predictors distinguish between them.
///
/// This is `&&label` and `goto *ptr`, a GNU extension, which is why it is not
/// in the header (spec 3.1 bans extensions in public headers). The number it
/// produces is reported next to the other two in docs/itch-notes.md.
void BM_ItchDecode_ComputedGoto(benchmark::State& state) {
  const std::size_t size = stream().size();
  std::uint64_t messages = 0;

  for (auto _ : state) {
    BinaryFileFramer framer(bytes(), size);
    FramedMessage msg{};
    NormalizedEvent event{};
    std::uint64_t n = 0;
    // The same per-type bookkeeping ItchDecoder does. Without it this
    // benchmark would be measuring a different, smaller loop and the
    // comparison against switch and table would be worthless.
    MessageCounts counts{};

    static void* targets[256];
    static bool built = false;
    if (!built) {
      for (auto& t : targets) t = &&do_other;
      targets[static_cast<unsigned char>('A')] = &&do_add;
      targets[static_cast<unsigned char>('F')] = &&do_add;
      targets[static_cast<unsigned char>('E')] = &&do_exec;
      targets[static_cast<unsigned char>('C')] = &&do_exec;
      targets[static_cast<unsigned char>('X')] = &&do_cancel;
      targets[static_cast<unsigned char>('D')] = &&do_cancel;
      targets[static_cast<unsigned char>('U')] = &&do_replace;
      targets[static_cast<unsigned char>('P')] = &&do_trade;
      built = true;
    }

    while (framer.next(msg) == FrameStatus::Ok) {
      const char type = load_char(msg.data);
      if (itch_message_length(type) != msg.length) break;
      goto* targets[static_cast<unsigned char>(type)];

    do_add:
      (void)normalize(msg.data, type, event);
      goto done;
    do_exec:
      (void)normalize(msg.data, type, event);
      goto done;
    do_cancel:
      (void)normalize(msg.data, type, event);
      goto done;
    do_replace:
      (void)normalize(msg.data, type, event);
      goto done;
    do_trade:
      (void)normalize(msg.data, type, event);
      goto done;
    do_other:
      (void)normalize(msg.data, type, event);
    done:
      counts.record(type, msg.length);
      ++n;
      benchmark::DoNotOptimize(event);
      benchmark::DoNotOptimize(counts);
    }
    messages = n;
    benchmark::DoNotOptimize(messages);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(messages));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}
BENCHMARK(BM_ItchDecode_ComputedGoto)->Unit(benchmark::kMillisecond);
#pragma GCC diagnostic pop
#endif

/// The field decoders on their own, with no framing and no dispatch: the
/// floor a decoder could approach.
void BM_DecodeAddOrderOnly(benchmark::State& state) {
  const auto message = test::encode_add_order(1, 2, 34200000000000, 12345,
                                              Side::Buy, 500, "AAPL", 1002100);
  const auto* p = reinterpret_cast<const std::byte*>(message.data());
  ItchAddOrder decoded{};
  for (auto _ : state) {
    bool ok = decode_add_order(p, decoded);
    benchmark::DoNotOptimize(ok);
    benchmark::DoNotOptimize(decoded);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_LoadBe48(benchmark::State& state) {
  alignas(1) static const std::uint8_t raw[7] = {0x1F, 0x1A, 0xD6, 0x35,
                                                 0xBD, 0x15, 0x00};
  // Deliberately offset by one byte: nothing in an ITCH file is aligned.
  const auto* p = reinterpret_cast<const std::byte*>(raw + 1);
  for (auto _ : state) {
    std::uint64_t v = load_be48(p);
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_ItchDecode_Switch)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ItchDecode_Table)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_DecodeAddOrderOnly);
BENCHMARK(BM_LoadBe48);

} // namespace
} // namespace miniex
