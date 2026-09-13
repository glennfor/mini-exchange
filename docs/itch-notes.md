# ITCH 5.0 Notes

Implementation notes for `feed/`: what the protocol actually says, where the
traps are, and the measurements behind the choices.

---

## 1. The layout, and how it is checked

Every message is: **type (1), stock locate (2), tracking number (2),
timestamp (6)** — eleven bytes — followed by the type's own fields. All
multi-byte integers are big-endian. Prices are 4-byte unsigned with four
implied decimals, which is exactly this project's `Price` scale, so decoding a
price is a widening integer load and no conversion at all.

The length table in `itch50.hpp` is not taken on trust. A test
(`MessageLengthsEqualHeaderPlusFieldWidths`) re-derives every length from the
header size plus the individual field widths, so a typo in the table fails the
build rather than silently desynchronising the stream.

| Type | Name | Len | Book |
|---|---|---|---|
| `S` | System Event | 12 | |
| `R` | Stock Directory | 39 | |
| `H` | Stock Trading Action | 25 | |
| `Y` | Reg SHO Restriction | 20 | |
| `L` | Market Participant Position | 26 | |
| `V` | MWCB Decline Level | 35 | |
| `W` | MWCB Status | 12 | |
| `K` | IPO Quoting Period | 28 | |
| `J` | LULD Auction Collar | 35 | |
| `h` | Operational Halt | 21 | |
| `A` | Add Order | 36 | **yes** |
| `F` | Add Order w/ MPID | 40 | **yes** |
| `E` | Order Executed | 31 | **yes** |
| `C` | Order Executed w/ Price | 36 | **yes** |
| `X` | Order Cancel | 23 | **yes** |
| `D` | Order Delete | 19 | **yes** |
| `U` | Order Replace | 35 | **yes** |
| `P` | Trade (non-cross) | 44 | print only |
| `Q` | Cross Trade | 40 | print only |
| `B` | Broken Trade | 19 | statistics only |
| `I` | NOII | 50 | |
| `N` | RPII | 20 | |

## 2. Traps

Each of these has a test named for it.

**`Q` sizes its share count at eight bytes, not four.** Every other share
count in the protocol is 4 bytes, so reading `Q` the same way is an easy
mistake — and one that produces a plausible-looking number rather than an
obvious failure, because the high half of a cross's volume is usually zero.
Every field after it is then four bytes out of place.

**`X` reduces an order; `D` removes one.** A cancel carries the number of
shares cancelled and the order usually survives with less quantity. Treating
`X` as a removal deletes liquidity that is still there.

**`U` introduces a new reference number.** The original reference is dead
after a replace and the new order is a different order, at the tail of its
level. Reusing the old reference number is a reconstruction bug that surfaces
much later, as an execution against an order that no longer exists.

**`E` carries no price.** The trade prints at the resting order's price, which
the reconstructor knows and the message does not repeat. Only `C` carries an
explicit execution price.

**`C` has a printable flag.** `printable == 'N'` means the execution still
changes the book but is excluded from volume and last-price statistics.
Counting non-printable executions in volume overstates it.

**Order reference numbers exceed 32 bits.** They are dense and sequential and
they pass 2^32 within a normal day on a liquid name. The fixtures deliberately
use 4,294,967,373 to catch a 32-bit truncation.

**A side byte that is not `B` or `S` is rejected, not guessed.** Guessing puts
liquidity on the wrong half of the book, and every state after that point is
wrong in a way that looks like a subtle reconstruction bug rather than a
corrupt input.

## 3. Framing

**BinaryFILE** — what a sample day file is: each message preceded by a 2-byte
big-endian length. That is the entire format.

**MoldUDP64** — what the live multicast feed is: a 20-byte header (10-byte
session, 8-byte sequence number, 2-byte message count) then that many
length-prefixed messages. This project never opens a socket, but the layout is
implemented because the sequence number is where the interesting question is:
`MoldPacket::expected_next()` is the whole of gap detection, and a feed handler
that does not compare against it will happily reconstruct a book with messages
missing from the middle.

## 4. Validation policy

A message whose framed length disagrees with the length its type requires is a
**hard error reporting the byte offset**. It is never skipped. Skipping is how
a decoder ends up half a message out of phase, after which it reconstructs a
confident, entirely fictional book.

An unknown *type* is different: in strict mode it is an error, and otherwise
it is counted and skipped using its framed length, because real sample files
occasionally carry types a given decoder does not model and the framing is
still trustworthy. A length mismatch is never tolerated either way.

## 5. Dispatch: three strategies, measured

Spec 10.4 asks for `switch` against a 256-entry function-pointer table against
computed goto, with all three numbers recorded. Here they are.

**Development machine, Apple M1 Max, Apple clang 17, `-O3`, 200,000-message
synthetic stream with a realistic mix (45% add, 30% delete, 10% cancel, 8%
replace, 5% execute, 2% print). Null handler.**
**This machine was under heavy load from another job during the run**, which
inflates absolute times; the *ratios* between the three are the usable part,
and they are stable across repeats.

| Strategy | ms / 200k msgs | messages/s | GiB/s | vs switch |
|---|---|---|---|---|
| Computed goto (GNU extension) | 1.73 | 116.3 M | 3.41 | **1.18x** |
| `switch` | 2.14 | 98.7 M | 2.89 | 1.00x |
| 256-entry function-pointer table | 2.34 | 94.4 M | 2.77 | 0.96x |

Supporting numbers: `decode_add_order` alone is **1.75 ns** (594 M/s) and
`load_be48` on a deliberately unaligned pointer is **0.33 ns** — one
instruction, as intended.

**What was kept: the `switch`.** Reasons, in order:

1. The table is *slower*, not faster. The bounds check a compiler emits for a
   dense character switch costs less than an indirect call through a pointer
   the predictor has to learn, and the switch's jump table lives in the same
   cache line region as the code using it.
2. Computed goto is genuinely faster here, by about 18%, and it is a GNU
   extension. The project bans compiler extensions in public headers
   (spec 3.1) because the header has to compile under any conforming C++20
   compiler. It is measured in the benchmark translation unit, where an
   extension costs nothing, and the number is reported rather than hidden.
3. The gap is smaller than it first appeared. The computed-goto benchmark
   initially skipped the per-type counter bookkeeping the real decoder does,
   which flattered it by a further 7 percentage points. It now does the same
   work, and the number above is the corrected one. A benchmark comparing two
   loops that do different amounts of work is not a comparison.

The switch also keeps the *exhaustiveness* property: adding a message type
without a length is a `-Wswitch` warning, and this project builds `-Werror`.
The table silently defaults.

## 6. Throughput

**Decode-only, null handler, `switch` dispatch, on a 12 MB synthetic file with
the mix above, development machine under load:**

```
$ ./build/release/tools/itch_stats --file synthetic.itch
messages        400001
decode throughput (null handler, dispatch=switch)
  20.11 million messages/s
  602.6 MB/s
```

For orientation, the spec's own guidance is that below ~20 MB/s something is
badly wrong and a tight decoder should be well into the hundreds of MB/s. This
is 602 MB/s on a contended machine with a coarse timer, so the decoder is in
the expected range and is not the bottleneck in anything built on top of it.

**This is decode-only.** Decode-plus-reconstruct is the number that matters
for anything downstream, it will be lower, and it arrives with M7. Neither
number has been produced on the Linux reference machine yet; both are
development figures.

## 7. Fuzzing

`feed/fuzz/fuzz_itch.cpp` is a libFuzzer target over three entry points: the
BinaryFILE walk, the same input through table dispatch, and the input treated
as a MoldUDP64 datagram. The contract is narrow and absolute: **for any byte
sequence whatsoever, no crash, no out-of-bounds read, no infinite loop.**
Rejecting the input is correct behaviour for almost all of it.

The "no infinite loop" part is made testable by passing a message-count bound
to `decode()` and trapping if the decoder returns it — a decoder that failed to
advance would hit the bound rather than spinning forever undetected.

Local run, Homebrew clang 20, 26 seconds, six committed seeds:

```
Done 21537748 runs in 26 second(s)
stat::average_exec_per_sec:     828374
stat::new_units_added:          1587
```

No crashes. Note that this local run is **without AddressSanitizer**: the
compiler-rt ASan runtime does not work on this host (DD-006), so the local
fuzzer verifies "does not crash or hang" but not "does not read out of
bounds". CI runs it with ASan on, which is where the out-of-bounds coverage
comes from. The seeds are committed; the inputs the fuzzer discovers are not.

The unit suite also contains the cheap version of the same idea:
`SingleByteCorruptionsNeverCrashOrHang` takes a valid stream and corrupts one
byte at a time across five values, which runs in 20 ms on every developer's
machine and catches the obvious regressions without waiting for CI.
