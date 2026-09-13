/// \file decoder.hpp
/// \brief The walk over a framed ITCH stream: frame, validate, dispatch.
///
/// ## Validation
///
/// Every message's framed length is checked against the length its type
/// requires before a single field is read. A mismatch is a hard error that
/// reports the byte offset in the source. Skipping a bad message and carrying
/// on is how a decoder ends up half a message out of phase, at which point it
/// will happily reconstruct a plausible-looking book out of noise.
///
/// ## Dispatch
///
/// Two implementations, both here, both benchmarked (`bench/micro`, results
/// in `docs/itch-notes.md`):
///
/// * `DispatchMode::Switch` -- a `switch` on the type byte. Compilers turn a
///   dense character switch into a bounds check plus a jump table.
/// * `DispatchMode::Table` -- an explicit 256-entry function-pointer table
///   indexed by the raw byte, which removes the bounds check at the cost of
///   an indirect call the branch predictor has to learn.
///
/// Computed goto is the third option the spec asks about. It is a GNU
/// extension, and the project bans compiler extensions in public headers
/// (spec 3.1), so it is measured in the benchmark translation unit rather
/// than offered here. The number is reported alongside the other two.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "feed/itch50.hpp"
#include "feed/mold64.hpp"
#include "feed/normalizer.hpp"

namespace miniex {

enum class DecodeError : std::uint8_t {
  None,
  /// A type byte that is not one of the 22 defined messages.
  UnknownType,
  /// The framed length disagrees with the length the type requires.
  LengthMismatch,
  /// The stream ended part-way through a message.
  Truncated,
  /// A zero-length frame, which would make the walk loop forever.
  ZeroLength,
  /// A field that has only a few legal values held something else -- a side
  /// that is neither 'B' nor 'S', a printable flag that is neither 'Y' nor
  /// 'N'. Reported rather than guessed: guessing a side puts liquidity on the
  /// wrong half of the book and every later state is wrong.
  MalformedField,
};

constexpr const char* to_string(DecodeError e) noexcept {
  switch (e) {
  case DecodeError::None: return "None";
  case DecodeError::UnknownType: return "UnknownType";
  case DecodeError::LengthMismatch: return "LengthMismatch";
  case DecodeError::Truncated: return "Truncated";
  case DecodeError::ZeroLength: return "ZeroLength";
  case DecodeError::MalformedField: return "MalformedField";
  }
  return "Unknown";
}

struct DecodeFailure {
  DecodeError error{DecodeError::None};
  std::size_t offset{0}; ///< byte offset in the source
  std::size_t index{0};  ///< message index within the stream
  char type{'\0'};
  std::size_t framed_length{0};
  std::size_t expected_length{0};

  [[nodiscard]] bool ok() const noexcept { return error == DecodeError::None; }

  /// A message a human can act on without opening a hex editor.
  [[nodiscard]] std::string what() const {
    std::string out = to_string(error);
    out += " at byte offset " + std::to_string(offset) + " (message #" +
           std::to_string(index) + ")";
    if (type != '\0') {
      out += ", type '";
      out += type;
      out += "'";
    }
    if (error == DecodeError::LengthMismatch) {
      out += ": framed length " + std::to_string(framed_length) +
             ", type requires " + std::to_string(expected_length);
    }
    return out;
  }
};

/// Counts by message type, for `itch_stats` and for the flow-mix profile.
struct MessageCounts {
  std::uint64_t by_type[256]{};
  std::uint64_t total{0};
  std::uint64_t bytes{0};

  void record(char type, std::size_t length) noexcept {
    ++by_type[static_cast<unsigned char>(type)];
    ++total;
    bytes += length;
  }
  [[nodiscard]] std::uint64_t count(char type) const noexcept {
    return by_type[static_cast<unsigned char>(type)];
  }
  [[nodiscard]] std::uint64_t book_mutating() const noexcept {
    std::uint64_t n = 0;
    for (const char t : {'A', 'F', 'E', 'C', 'X', 'D', 'U'}) n += count(t);
    return n;
  }
};

enum class DispatchMode : std::uint8_t {
  Switch,
  Table
};

/// Walks a BinaryFILE-framed buffer, validating and normalising every message.
///
/// \tparam Handler  called as `handler(const NormalizedEvent&)`; return void.
/// The handler is a template parameter rather than a `std::function` so that
/// a counting run compiles down to the counter and nothing else.
template <class Handler>
class ItchDecoder {
public:
  ItchDecoder(const std::byte* data, std::size_t size, Handler& handler)
      : framer_(data, size), handler_(handler) {}

  /// Decodes up to \p max messages. Returns the number decoded; on failure
  /// `failure()` says what went wrong and where.
  ///
  /// \param strict when false, a message whose type is unknown is counted and
  ///        skipped using its framed length rather than being an error. Real
  ///        sample files occasionally contain types this decoder does not
  ///        model; a *length* mismatch is never tolerated either way, because
  ///        that is the error that desynchronises the stream.
  std::size_t decode(std::size_t max = ~std::size_t{0}, bool strict = true) {
    std::size_t decoded = 0;
    FramedMessage framed{};
    while (decoded < max) {
      const FrameStatus status = framer_.next(framed);
      if (status == FrameStatus::EndOfStream) break;
      if (status != FrameStatus::Ok) {
        failure_.error = status == FrameStatus::Truncated
                             ? DecodeError::Truncated
                             : DecodeError::ZeroLength;
        failure_.offset = framer_.last_error_offset();
        failure_.index = counts_.total;
        return decoded;
      }

      const char type = load_char(framed.data);
      const std::size_t expected = itch_message_length(type);
      if (expected == 0) {
        counts_.record(type, framed.length);
        ++unknown_types_;
        if (strict) {
          failure_.error = DecodeError::UnknownType;
          failure_.offset = framed.offset;
          failure_.index = counts_.total - 1;
          failure_.type = type;
          return decoded;
        }
        continue;
      }
      if (expected != framed.length) {
        failure_.error = DecodeError::LengthMismatch;
        failure_.offset = framed.offset;
        failure_.index = counts_.total;
        failure_.type = type;
        failure_.framed_length = framed.length;
        failure_.expected_length = expected;
        return decoded;
      }

      counts_.record(type, framed.length);
      NormalizedEvent event{};
      if (!dispatch(framed.data, type, event)) {
        failure_.error = DecodeError::MalformedField;
        failure_.offset = framed.offset;
        failure_.index = counts_.total - 1;
        failure_.type = type;
        return decoded;
      }
      handler_(event);
      ++decoded;
    }
    return decoded;
  }

  [[nodiscard]] const MessageCounts& counts() const noexcept { return counts_; }
  [[nodiscard]] const DecodeFailure& failure() const noexcept {
    return failure_;
  }
  [[nodiscard]] std::size_t unknown_types() const noexcept {
    return unknown_types_;
  }
  [[nodiscard]] std::size_t position() const noexcept {
    return framer_.position();
  }

  void set_dispatch(DispatchMode mode) noexcept { mode_ = mode; }

private:
  bool dispatch(const std::byte* p, char type, NormalizedEvent& out) noexcept {
    if (mode_ == DispatchMode::Switch) return normalize(p, type, out);
    return table_dispatch(p, type, out);
  }

  using DecodeFn = bool (*)(const std::byte*, NormalizedEvent&) noexcept;

  /// The 256-entry function-pointer alternative. Built once, at namespace
  /// scope, so the table is in `.rodata` rather than rebuilt per decoder.
  static bool table_dispatch(const std::byte* p, char type,
                             NormalizedEvent& out) noexcept {
    return table()[static_cast<unsigned char>(type)](p, out);
  }

  template <char Type>
  static bool decode_one(const std::byte* p, NormalizedEvent& out) noexcept {
    return normalize(p, Type, out);
  }
  static bool decode_unknown(const std::byte* p,
                             NormalizedEvent& out) noexcept {
    return normalize(p, '\0', out);
  }

  static const DecodeFn* table() noexcept {
    static const auto* t = [] {
      static DecodeFn entries[256];
      for (auto& e : entries) e = &decode_unknown;
      entries[static_cast<unsigned char>('A')] = &decode_one<'A'>;
      entries[static_cast<unsigned char>('F')] = &decode_one<'F'>;
      entries[static_cast<unsigned char>('E')] = &decode_one<'E'>;
      entries[static_cast<unsigned char>('C')] = &decode_one<'C'>;
      entries[static_cast<unsigned char>('X')] = &decode_one<'X'>;
      entries[static_cast<unsigned char>('D')] = &decode_one<'D'>;
      entries[static_cast<unsigned char>('U')] = &decode_one<'U'>;
      entries[static_cast<unsigned char>('P')] = &decode_one<'P'>;
      entries[static_cast<unsigned char>('Q')] = &decode_one<'Q'>;
      entries[static_cast<unsigned char>('B')] = &decode_one<'B'>;
      entries[static_cast<unsigned char>('H')] = &decode_one<'H'>;
      entries[static_cast<unsigned char>('S')] = &decode_one<'S'>;
      return entries;
    }();
    return t;
  }

  BinaryFileFramer framer_;
  Handler& handler_;
  MessageCounts counts_{};
  DecodeFailure failure_{};
  std::size_t unknown_types_{0};
  DispatchMode mode_{DispatchMode::Switch};
};

/// Discards every event. Used by throughput runs and by the fuzzer.
struct NullMdHandler {
  void operator()(const NormalizedEvent&) noexcept {}
};

/// Counts events by action.
struct CountingMdHandler {
  std::uint64_t by_action[16]{};
  std::uint64_t total{0};
  void operator()(const NormalizedEvent& e) noexcept {
    ++by_action[static_cast<std::size_t>(e.action) & 15U];
    ++total;
  }
  [[nodiscard]] std::uint64_t count(MdAction a) const noexcept {
    return by_action[static_cast<std::size_t>(a) & 15U];
  }
};

} // namespace miniex
