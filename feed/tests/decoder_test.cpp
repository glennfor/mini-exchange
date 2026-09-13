// The walk over a framed stream: framing, length validation, error reporting,
// dispatch equivalence, and normalisation.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "feed/decoder.hpp"
#include "feed/mold64.hpp"
#include "feed/normalizer.hpp"

#include "itch_encoder.hpp"

namespace miniex {
namespace {

constexpr TsNanos kTs = 34200000000000;

const std::byte* as_bytes(const std::vector<std::uint8_t>& v) {
  return reinterpret_cast<const std::byte*>(v.data());
}

/// A small but complete day: a system event, two adds, an execution, a
/// partial cancel, a replace, a delete, a hidden trade and a cross.
std::vector<std::uint8_t> sample_stream() {
  return test::frame_all({
      test::encode_system_event(0, 1, kTs, 'Q'),
      test::encode_add_order(1, 1, kTs + 1, 100, Side::Buy, 500, "AAPL",
                             1002100),
      test::encode_add_order(1, 2, kTs + 2, 101, Side::Sell, 300, "AAPL",
                             1002400),
      test::encode_order_executed(1, 3, kTs + 3, 100, 200, 9001),
      test::encode_order_cancel(1, 4, kTs + 4, 100, 100),
      test::encode_order_replace(1, 5, kTs + 5, 101, 102, 250, 1002300),
      test::encode_order_delete(1, 6, kTs + 6, 102),
      test::encode_trade(1, 7, kTs + 7, 0, Side::Sell, 77, "AAPL", 1002250,
                         9002),
      test::encode_cross_trade(1, 8, kTs + 8, 1000000, "AAPL", 1002000, 9003,
                               'O'),
  });
}

struct RecordingHandler {
  std::vector<NormalizedEvent> events;
  void operator()(const NormalizedEvent& e) { events.push_back(e); }
};

// --- framing ---------------------------------------------------------------

TEST(Framing, WalksEveryMessageInOrder) {
  const auto stream = sample_stream();
  BinaryFileFramer framer(as_bytes(stream), stream.size());
  FramedMessage msg{};
  std::string types;
  while (framer.next(msg) == FrameStatus::Ok)
    types.push_back(static_cast<char>(msg.data[0]));
  EXPECT_EQ(types, "SAAEXUDPQ");
}

TEST(Framing, ReportsTheOffsetOfATruncatedMessage) {
  auto stream = sample_stream();
  const std::size_t full = stream.size();
  stream.resize(full - 5); // chop the tail of the last message
  BinaryFileFramer framer(as_bytes(stream), stream.size());
  FramedMessage msg{};
  FrameStatus status = FrameStatus::Ok;
  std::size_t count = 0;
  while ((status = framer.next(msg)) == FrameStatus::Ok) ++count;
  EXPECT_EQ(status, FrameStatus::Truncated);
  EXPECT_EQ(count, 8U) << "the first eight messages are intact";
  EXPECT_GT(framer.last_error_offset(), 0U);
}

TEST(Framing, ATrailingSingleByteIsTruncatedNotEndOfStream) {
  std::vector<std::uint8_t> stream = {0x00};
  BinaryFileFramer framer(as_bytes(stream), stream.size());
  FramedMessage msg{};
  EXPECT_EQ(framer.next(msg), FrameStatus::Truncated);
}

TEST(Framing, AZeroLengthFrameIsAnErrorNotAnInfiniteLoop) {
  std::vector<std::uint8_t> stream = {0x00, 0x00, 0x41};
  BinaryFileFramer framer(as_bytes(stream), stream.size());
  FramedMessage msg{};
  EXPECT_EQ(framer.next(msg), FrameStatus::ZeroLength);
}

TEST(Framing, EmptyInputIsEndOfStream) {
  BinaryFileFramer framer(nullptr, 0);
  FramedMessage msg{};
  EXPECT_EQ(framer.next(msg), FrameStatus::EndOfStream);
}

// --- MoldUDP64 -------------------------------------------------------------

TEST(Mold64, ParsesAHeaderAndComputesTheNextExpectedSequence) {
  std::vector<std::uint8_t> packet;
  const char* session = "SESSION001";
  packet.insert(packet.end(), session, session + 10);
  for (int shift = 56; shift >= 0; shift -= 8)
    packet.push_back(static_cast<std::uint8_t>(1000000ULL >> shift));
  packet.push_back(0x00);
  packet.push_back(0x03);
  const auto body = test::frame_all({
      test::encode_order_delete(1, 1, kTs, 1),
      test::encode_order_delete(1, 2, kTs, 2),
      test::encode_order_delete(1, 3, kTs, 3),
  });
  packet.insert(packet.end(), body.begin(), body.end());

  MoldPacket mold{};
  ASSERT_TRUE(parse_mold_packet(as_bytes(packet), packet.size(), mold));
  EXPECT_EQ(mold.session, "SESSION001");
  EXPECT_EQ(mold.sequence, 1000000ULL);
  EXPECT_EQ(mold.message_count, 3U);
  EXPECT_EQ(mold.expected_next(), 1000003ULL)
      << "this arithmetic is the whole of gap detection";
  EXPECT_FALSE(mold.is_heartbeat());
  EXPECT_EQ(mold.payload_size, body.size());

  // The payload is ordinary length-prefixed messages.
  BinaryFileFramer framer(mold.payload, mold.payload_size);
  FramedMessage msg{};
  int seen = 0;
  while (framer.next(msg) == FrameStatus::Ok) ++seen;
  EXPECT_EQ(seen, 3);
}

TEST(Mold64, TooShortForAHeaderIsRefused) {
  std::vector<std::uint8_t> tiny(19, 0);
  MoldPacket mold{};
  EXPECT_FALSE(parse_mold_packet(as_bytes(tiny), tiny.size(), mold));
}

TEST(Mold64, HeartbeatAndEndOfSessionAreRecognised) {
  std::vector<std::uint8_t> packet(20, 0);
  MoldPacket mold{};
  ASSERT_TRUE(parse_mold_packet(as_bytes(packet), packet.size(), mold));
  EXPECT_TRUE(mold.is_heartbeat());
  EXPECT_EQ(mold.expected_next(), mold.sequence)
      << "a heartbeat does not advance the sequence";

  packet[18] = 0xFF;
  packet[19] = 0xFF;
  ASSERT_TRUE(parse_mold_packet(as_bytes(packet), packet.size(), mold));
  EXPECT_TRUE(mold.is_end_of_session());
}

// --- the decoder -----------------------------------------------------------

TEST(Decoder, DecodesAWholeStreamAndCountsByType) {
  const auto stream = sample_stream();
  RecordingHandler handler;
  ItchDecoder decoder(as_bytes(stream), stream.size(), handler);
  const std::size_t n = decoder.decode();

  EXPECT_EQ(n, 9U);
  EXPECT_TRUE(decoder.failure().ok()) << decoder.failure().what();
  EXPECT_EQ(decoder.counts().total, 9U);
  EXPECT_EQ(decoder.counts().count('A'), 2U);
  EXPECT_EQ(decoder.counts().count('E'), 1U);
  EXPECT_EQ(decoder.counts().count('X'), 1U);
  EXPECT_EQ(decoder.counts().count('U'), 1U);
  EXPECT_EQ(decoder.counts().count('D'), 1U);
  EXPECT_EQ(decoder.counts().count('P'), 1U);
  EXPECT_EQ(decoder.counts().count('Q'), 1U);
  EXPECT_EQ(decoder.counts().count('S'), 1U);
  EXPECT_EQ(decoder.counts().book_mutating(), 6U);
  EXPECT_EQ(decoder.counts().bytes, stream.size() - 9 * 2)
      << "counted bytes exclude the framing prefixes";
}

TEST(Decoder, NormalisesEveryActionCorrectly) {
  const auto stream = sample_stream();
  RecordingHandler handler;
  ItchDecoder decoder(as_bytes(stream), stream.size(), handler);
  ASSERT_EQ(decoder.decode(), 9U);
  const auto& e = handler.events;
  ASSERT_EQ(e.size(), 9U);

  EXPECT_EQ(e[0].action, MdAction::SystemEvent);
  EXPECT_EQ(e[0].detail, 'Q');

  EXPECT_EQ(e[1].action, MdAction::Add);
  EXPECT_EQ(e[1].ref, 100U);
  EXPECT_EQ(e[1].side, Side::Buy);
  EXPECT_EQ(e[1].qty, 500U);
  EXPECT_EQ(e[1].price, 1002100);
  EXPECT_EQ(e[1].symbol, 1U) << "the stock locate is used as the symbol id";
  EXPECT_EQ(e[1].ts, kTs + 1);

  EXPECT_EQ(e[3].action, MdAction::Execute);
  EXPECT_EQ(e[3].ref, 100U);
  EXPECT_EQ(e[3].qty, 200U);
  EXPECT_EQ(e[3].match, 9001U);
  EXPECT_EQ(e[3].price, 0) << "an 'E' has no price of its own";

  EXPECT_EQ(e[4].action, MdAction::Cancel);
  EXPECT_EQ(e[4].qty, 100U);

  EXPECT_EQ(e[5].action, MdAction::Replace);
  EXPECT_EQ(e[5].ref, 101U);
  EXPECT_EQ(e[5].new_ref, 102U);
  EXPECT_EQ(e[5].qty, 250U);
  EXPECT_EQ(e[5].price, 1002300);

  EXPECT_EQ(e[6].action, MdAction::Delete);
  EXPECT_EQ(e[6].ref, 102U);
  EXPECT_EQ(e[6].qty, 0U);

  EXPECT_EQ(e[7].action, MdAction::Trade);
  EXPECT_EQ(e[7].qty, 77U);
  EXPECT_EQ(e[7].match, 9002U);

  EXPECT_EQ(e[8].action, MdAction::Cross);
  EXPECT_EQ(e[8].qty, 1000000U);
  EXPECT_EQ(e[8].detail, 'O');
}

TEST(Decoder, ALengthMismatchIsAHardErrorWithAFileOffset) {
  auto stream = sample_stream();
  // The second message is the first Add, framed at offset 14 (2 + 12 for the
  // system event, then its own 2-byte prefix). Corrupt its length prefix.
  const std::size_t prefix = 2 + itch_message_length('S');
  stream[prefix] = 0x00;
  stream[prefix + 1] = 0x22; // 34, not 36

  RecordingHandler handler;
  ItchDecoder decoder(as_bytes(stream), stream.size(), handler);
  const std::size_t n = decoder.decode();

  EXPECT_EQ(n, 1U) << "the system event decoded; the walk stopped at the Add";
  const DecodeFailure& f = decoder.failure();
  EXPECT_EQ(f.error, DecodeError::LengthMismatch);
  EXPECT_EQ(f.type, 'A');
  EXPECT_EQ(f.framed_length, 34U);
  EXPECT_EQ(f.expected_length, 36U);
  EXPECT_EQ(f.offset, prefix + 2);
  EXPECT_NE(f.what().find("LengthMismatch"), std::string::npos);
  EXPECT_NE(f.what().find("byte offset"), std::string::npos);
}

TEST(Decoder, AnUnknownTypeIsAnErrorInStrictModeAndCountedOtherwise) {
  auto messages = test::frame_all({
      test::encode_order_delete(1, 1, kTs, 1),
      test::encode_order_delete(1, 2, kTs, 2),
  });
  // Turn the second message's type byte into something undefined.
  const std::size_t second = 2 + itch_message_length('D') + 2;
  messages[second] = static_cast<std::uint8_t>('z');

  {
    RecordingHandler handler;
    ItchDecoder decoder(as_bytes(messages), messages.size(), handler);
    EXPECT_EQ(decoder.decode(~std::size_t{0}, /*strict=*/true), 1U);
    EXPECT_EQ(decoder.failure().error, DecodeError::UnknownType);
    EXPECT_EQ(decoder.failure().type, 'z');
  }
  {
    RecordingHandler handler;
    ItchDecoder decoder(as_bytes(messages), messages.size(), handler);
    EXPECT_EQ(decoder.decode(~std::size_t{0}, /*strict=*/false), 1U);
    EXPECT_TRUE(decoder.failure().ok());
    EXPECT_EQ(decoder.unknown_types(), 1U);
  }
}

TEST(Decoder, AMalformedFieldStopsTheWalkAndSaysWhere) {
  auto stream = test::frame_all({
      test::encode_add_order(1, 1, kTs, 1, Side::Buy, 10, "AAPL", 100),
  });
  stream[2 + 19] = static_cast<std::uint8_t>('?'); // the side byte
  RecordingHandler handler;
  ItchDecoder decoder(as_bytes(stream), stream.size(), handler);
  EXPECT_EQ(decoder.decode(), 0U);
  EXPECT_EQ(decoder.failure().error, DecodeError::MalformedField);
  EXPECT_EQ(decoder.failure().type, 'A');
}

TEST(Decoder, ATruncatedStreamStopsCleanly) {
  auto stream = sample_stream();
  stream.resize(stream.size() - 3);
  RecordingHandler handler;
  ItchDecoder decoder(as_bytes(stream), stream.size(), handler);
  EXPECT_EQ(decoder.decode(), 8U);
  EXPECT_EQ(decoder.failure().error, DecodeError::Truncated);
}

TEST(Decoder, DecodeRespectsTheMessageLimit) {
  const auto stream = sample_stream();
  RecordingHandler handler;
  ItchDecoder decoder(as_bytes(stream), stream.size(), handler);
  EXPECT_EQ(decoder.decode(3), 3U);
  EXPECT_EQ(handler.events.size(), 3U);
  EXPECT_EQ(decoder.decode(100), 6U) << "the walk resumes where it stopped";
  EXPECT_EQ(handler.events.size(), 9U);
}

TEST(Decoder, SwitchAndTableDispatchProduceIdenticalOutput) {
  // The two dispatch strategies exist to be compared for speed; they must be
  // indistinguishable in behaviour, or the comparison is meaningless.
  const auto stream = sample_stream();
  RecordingHandler a;
  RecordingHandler b;
  ItchDecoder da(as_bytes(stream), stream.size(), a);
  da.set_dispatch(DispatchMode::Switch);
  ItchDecoder db(as_bytes(stream), stream.size(), b);
  db.set_dispatch(DispatchMode::Table);
  ASSERT_EQ(da.decode(), db.decode());
  ASSERT_EQ(a.events.size(), b.events.size());
  for (std::size_t i = 0; i < a.events.size(); ++i) {
    EXPECT_EQ(std::memcmp(&a.events[i], &b.events[i], sizeof(NormalizedEvent)),
              0)
        << "dispatch strategies disagree at event " << i;
  }
}

TEST(Decoder, CountingHandlerCompilesToCounters) {
  const auto stream = sample_stream();
  CountingMdHandler handler;
  ItchDecoder decoder(as_bytes(stream), stream.size(), handler);
  ASSERT_EQ(decoder.decode(), 9U);
  EXPECT_EQ(handler.total, 9U);
  EXPECT_EQ(handler.count(MdAction::Add), 2U);
  EXPECT_EQ(handler.count(MdAction::Cross), 1U);
}

// --- the fuzz surface, exercised deterministically in the unit suite --------
//
// The libFuzzer target lives in fuzz_itch.cpp and runs in CI. This test is
// the cheap version: take a valid stream and corrupt one byte at a time,
// asserting only that nothing crashes, reads out of bounds (ASan in CI) or
// loops forever. It is not a substitute for the fuzzer; it is the part of the
// fuzzer's job that can run in 20 milliseconds on every developer's machine.
TEST(Decoder, SingleByteCorruptionsNeverCrashOrHang) {
  const auto original = sample_stream();
  for (std::size_t i = 0; i < original.size(); ++i) {
    for (const int value : {0x00, 0x01, 0xFF, 0x41, 0x7F}) {
      auto stream = original;
      stream[i] = static_cast<std::uint8_t>(value);
      NullMdHandler handler;
      ItchDecoder decoder(as_bytes(stream), stream.size(), handler);
      // A bound on the message count is what makes "does not hang" a testable
      // property rather than a hopeful one.
      const std::size_t n = decoder.decode(1000, /*strict=*/false);
      EXPECT_LE(n, 1000U);
    }
  }
}

TEST(Decoder, ArbitraryBytesAreRejectedRatherThanMisread) {
  // Not a valid stream at all. The decoder must stop, not wander.
  std::vector<std::uint8_t> junk;
  for (std::size_t i = 0; i < 1024; ++i)
    junk.push_back(static_cast<std::uint8_t>(i * 37 + 11));
  NullMdHandler handler;
  ItchDecoder decoder(as_bytes(junk), junk.size(), handler);
  const std::size_t n = decoder.decode(10000, /*strict=*/false);
  EXPECT_LE(n, 10000U);
  EXPECT_LE(decoder.position(), junk.size());
}

} // namespace
} // namespace miniex
