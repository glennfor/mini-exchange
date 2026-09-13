/// \file mold64.hpp
/// \brief The two framings an ITCH stream arrives in.
///
/// The decoder is framing-agnostic: it is handed a pointer and a length and
/// knows nothing about how the message was delimited. Both layouts are
/// implemented as POD views so that the difference between a file and a
/// multicast feed is a few lines rather than an architecture.
///
/// **BinaryFILE** — what a Nasdaq sample day file is. Each message is
/// preceded by a 2-byte big-endian length. That is the whole format.
///
/// **MoldUDP64** — what the live multicast feed is. Each datagram is a
/// 20-byte header (10-byte session id, 8-byte sequence number, 2-byte message
/// count) followed by that many length-prefixed messages. This project never
/// opens a socket, but the layout is here because sequence numbers are where
/// the interesting questions live: a gap in the sequence means messages were
/// lost, and a feed handler that does not notice will reconstruct a book that
/// is quietly wrong. `MoldPacket::expected_next()` is what a gap detector
/// would compare against.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "feed/endian.hpp"

namespace miniex {

/// One message located inside a buffer.
struct FramedMessage {
  const std::byte* data{nullptr};
  std::size_t length{0};
  std::size_t offset{0}; ///< byte offset in the source, for error reporting
};

/// Why a walk stopped.
enum class FrameStatus : std::uint8_t {
  Ok,
  EndOfStream,
  /// Fewer bytes remain than the length prefix says the message occupies.
  Truncated,
  /// A length prefix of zero, which would make the walk loop forever.
  ZeroLength,
};

/// Walks BinaryFILE framing: [u16 be length][payload] repeated.
class BinaryFileFramer {
public:
  BinaryFileFramer(const std::byte* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  /// Reads the next message. Returns Ok, or a reason it could not.
  FrameStatus next(FramedMessage& out) noexcept {
    if (pos_ >= size_) return FrameStatus::EndOfStream;
    if (size_ - pos_ < 2) {
      last_error_offset_ = pos_;
      return FrameStatus::Truncated;
    }
    const std::size_t length = load_be16(data_ + pos_);
    if (length == 0) {
      last_error_offset_ = pos_;
      return FrameStatus::ZeroLength;
    }
    if (size_ - pos_ - 2 < length) {
      last_error_offset_ = pos_;
      return FrameStatus::Truncated;
    }
    out.data = data_ + pos_ + 2;
    out.length = length;
    out.offset = pos_ + 2;
    pos_ += 2 + length;
    return FrameStatus::Ok;
  }

  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] std::size_t last_error_offset() const noexcept {
    return last_error_offset_;
  }
  void reset() noexcept { pos_ = 0; }

private:
  const std::byte* data_;
  std::size_t size_;
  std::size_t pos_{0};
  std::size_t last_error_offset_{0};
};

/// A MoldUDP64 downstream packet header.
struct MoldPacket {
  static constexpr std::size_t kHeaderLength = 20;
  static constexpr std::size_t kSessionLength = 10;

  std::string_view session;
  std::uint64_t sequence{0}; ///< sequence of the *first* message in the packet
  std::uint16_t message_count{0};
  const std::byte* payload{nullptr};
  std::size_t payload_size{0};

  /// The sequence number the next packet should start at. A packet whose
  /// `sequence` is greater than this means messages were lost; one that is
  /// smaller is a retransmission. Neither is handled here -- this project
  /// never opens a socket -- but the arithmetic is the whole of gap
  /// detection and it belongs next to the layout it depends on.
  [[nodiscard]] std::uint64_t expected_next() const noexcept {
    return sequence + message_count;
  }

  /// A heartbeat carries no messages and does not advance the sequence.
  [[nodiscard]] bool is_heartbeat() const noexcept {
    return message_count == 0;
  }
  /// 0xFFFF means "end of session".
  [[nodiscard]] bool is_end_of_session() const noexcept {
    return message_count == 0xFFFF;
  }
};

/// Parses a MoldUDP64 datagram header. Returns false if the datagram is too
/// short to contain one.
inline bool parse_mold_packet(const std::byte* data, std::size_t size,
                              MoldPacket& out) noexcept {
  if (size < MoldPacket::kHeaderLength) return false;
  out.session = std::string_view(reinterpret_cast<const char*>(data),
                                 MoldPacket::kSessionLength);
  out.sequence = load_be64(data + 10);
  out.message_count = load_be16(data + 18);
  out.payload = data + MoldPacket::kHeaderLength;
  out.payload_size = size - MoldPacket::kHeaderLength;
  return true;
}

} // namespace miniex
