"""An independent Python implementation of the LOBSTER validation.

Two jobs, both of which exist so the C++ `lobster_check` is checked by
something that was not written alongside it:

* ``validate`` -- reconstruct the book from a message file and compare it
  against the orderbook file, message by message. Identical in purpose to the
  C++ tool and written from the LOBSTER format description rather than from
  ``tools/lobster_check.cpp``.
* ``generate`` -- emit a synthetic but *format-correct* LOBSTER pair. Real
  LOBSTER samples are not redistributable and are hundreds of megabytes, so a
  small generated pair is what lets the end-to-end validation path be tested
  in CI on a machine with no market data at all. The generated book file is
  produced by this module's own reconstruction, so a C++ run against it is a
  genuine cross-implementation check -- exactly the same relationship the
  matching engine has with ``reference_engine.py``.

A pass against generated data is **not** the same claim as a pass against a
real LOBSTER day. The README says which one has been done.
"""

from __future__ import annotations

import argparse
import random
import sys
from dataclasses import dataclass, field

# LOBSTER message types.
SUBMISSION = 1
PARTIAL_CANCEL = 2
DELETION = 3
VISIBLE_EXECUTION = 4
HIDDEN_EXECUTION = 5
AUCTION_CROSS = 6
TRADING_HALT = 7

# The sentinel LOBSTER writes for a level that does not exist.
EMPTY_ASK_PRICE = 9999999999
EMPTY_BID_PRICE = -9999999999


@dataclass
class Message:
    ts_nanos: int
    type: int
    order_id: int
    size: int
    price: int
    direction: int  # 1 = the order concerned is a bid, -1 = an ask

    @property
    def side(self) -> str:
        """The side of the order this message concerns.

        For an execution this is the side of the order that WAS EXECUTED, not
        the aggressor. See the module docstring of ``pyref/README.md`` and
        ``docs/validation.md``.
        """
        return "B" if self.direction == 1 else "S"

    @property
    def aggressor_side(self) -> str:
        if self.type in (VISIBLE_EXECUTION, HIDDEN_EXECUTION):
            return "S" if self.direction == 1 else "B"
        return self.side

    def to_line(self) -> str:
        seconds, nanos = divmod(self.ts_nanos, 1_000_000_000)
        return (
            f"{seconds}.{nanos:09d},{self.type},{self.order_id},"
            f"{self.size},{self.price},{self.direction}"
        )


@dataclass
class Book:
    """The visible book, reconstructed. Dictionaries, no cleverness."""

    orders: dict[int, tuple[str, int, int]] = field(default_factory=dict)
    bids: dict[int, int] = field(default_factory=dict)
    asks: dict[int, int] = field(default_factory=dict)
    unknown_reference: int = 0
    oversized_reduction: int = 0

    def _levels(self, side: str) -> dict[int, int]:
        return self.bids if side == "B" else self.asks

    def add(self, order_id: int, side: str, price: int, size: int) -> None:
        if order_id in self.orders:
            return
        self.orders[order_id] = (side, price, size)
        levels = self._levels(side)
        levels[price] = levels.get(price, 0) + size

    def reduce(self, order_id: int, size: int) -> None:
        entry = self.orders.get(order_id)
        if entry is None:
            self.unknown_reference += 1
            return
        side, price, remaining = entry
        if size > remaining:
            self.oversized_reduction += 1
            size = remaining
        levels = self._levels(side)
        levels[price] -= size
        if levels[price] <= 0:
            del levels[price]
        remaining -= size
        if remaining == 0:
            del self.orders[order_id]
        else:
            self.orders[order_id] = (side, price, remaining)

    def remove(self, order_id: int) -> None:
        entry = self.orders.get(order_id)
        if entry is None:
            self.unknown_reference += 1
            return
        self.reduce(order_id, entry[2])

    def top_n(self, n: int) -> tuple[list[tuple[int, int]], list[tuple[int, int]]]:
        """(asks, bids), each n levels from the touch outwards, padded."""
        asks = sorted(self.asks.items())[:n]
        bids = sorted(self.bids.items(), reverse=True)[:n]
        asks += [(EMPTY_ASK_PRICE, 0)] * (n - len(asks))
        bids += [(EMPTY_BID_PRICE, 0)] * (n - len(bids))
        return asks, bids

    def book_line(self, n: int) -> str:
        asks, bids = self.top_n(n)
        fields: list[str] = []
        for i in range(n):
            fields += [str(asks[i][0]), str(asks[i][1])]
            fields += [str(bids[i][0]), str(bids[i][1])]
        return ",".join(fields)


def apply(book: Book, msg: Message) -> None:
    """The LOBSTER message table, as it applies to the visible book."""
    if msg.type == SUBMISSION:
        book.add(msg.order_id, msg.side, msg.price, msg.size)
    elif msg.type == PARTIAL_CANCEL:
        book.reduce(msg.order_id, msg.size)
    elif msg.type == DELETION:
        book.remove(msg.order_id)
    elif msg.type == VISIBLE_EXECUTION:
        book.reduce(msg.order_id, msg.size)
    # Hidden executions, auction crosses and halts do not touch the visible
    # book at all.


def parse_message(line: str) -> Message:
    parts = [p.strip() for p in line.split(",")]
    if len(parts) < 6:
        raise ValueError(f"expected 6 columns, got {len(parts)}")
    whole, _, frac = parts[0].partition(".")
    nanos = int(whole) * 1_000_000_000 + int((frac + "000000000")[:9] or 0)
    return Message(
        ts_nanos=nanos,
        type=int(parts[1]),
        order_id=int(parts[2]),
        size=int(parts[3]),
        price=int(parts[4]),
        direction=int(parts[5]),
    )


def parse_book_line(line: str) -> list[int]:
    return [int(p.strip()) for p in line.split(",")]


def validate(message_path: str, book_path: str, levels: int) -> int:
    """Returns 0 on success, 1 on the first mismatch."""
    book = Book()
    with open(message_path, encoding="utf-8") as messages, open(
        book_path, encoding="utf-8"
    ) as rows:
        # strict=True: the two files are required to be in lockstep, one
        # book row per message. A length mismatch means the pair is not what
        # LOBSTER published, and silently comparing the shorter of the two
        # would report a pass over a truncated file.
        paired = zip(messages, rows, strict=True)
        for index, (message_line, book_line) in enumerate(paired):
            if not message_line.strip():
                continue
            msg = parse_message(message_line)
            apply(book, msg)

            expected = parse_book_line(book_line)
            actual = parse_book_line(book.book_line(levels))
            width = levels * 4
            if expected[:width] != actual[:width]:
                print(f"LOBSTER MISMATCH at message {index}", file=sys.stderr)
                print(f"  message:  {message_line.strip()}", file=sys.stderr)
                print(f"  expected: {expected[:width]}", file=sys.stderr)
                print(f"  actual:   {actual[:width]}", file=sys.stderr)
                return 1

    print(
        f"# pyref: reconstruction matches at {levels} levels; "
        f"{len(book.orders)} orders resting, "
        f"{book.unknown_reference} unknown references",
        file=sys.stderr,
    )
    return 0


def generate(
    seed: int, count: int, levels: int
) -> tuple[list[Message], list[str]]:
    """Emits a format-correct synthetic LOBSTER pair.

    The flow is deliberately *not* matched: LOBSTER describes what an exchange
    already did, so submissions may cross the opposite side and sit there
    until an execution clears them, exactly as a real reconstruction must
    tolerate. Hidden executions and an auction cross are included so the
    "does not touch the visible book" paths are exercised.
    """
    rng = random.Random(seed)
    book = Book()
    messages: list[Message] = []
    book_lines: list[str] = []
    live: list[int] = []
    next_id = 1
    ts = 34_200_000_000_000
    mid = 2238100

    for _ in range(count):
        ts += rng.randrange(1, 2_000_000)
        roll = rng.randrange(100)

        if roll < 45 or not live:
            side_is_bid = rng.random() < 0.5
            if rng.randrange(100) < 8:
                # Deliberately through the opposite side. An exchange feed
                # does produce transiently crossed and locked books -- a
                # replace can move an order past the touch, and the state
                # persists for the handful of messages until the other side
                # reacts. A reconstructor that asserts no-cross dies here, so
                # the fixture has to contain the case.
                offset = -100 * (1 + rng.randrange(4))
            else:
                offset = 100 * (1 + rng.randrange(8))
            price = mid - offset if side_is_bid else mid + offset
            msg = Message(ts, SUBMISSION, next_id, 1 + rng.randrange(500),
                          price, 1 if side_is_bid else -1)
            next_id += 1
            live.append(msg.order_id)
        elif roll < 62:
            victim = live[rng.randrange(len(live))]
            side, price, size = book.orders.get(victim, ("B", mid, 1))
            msg = Message(ts, PARTIAL_CANCEL, victim,
                          max(1, min(size - 1, 1 + rng.randrange(100))), price,
                          1 if side == "B" else -1)
        elif roll < 80:
            k = rng.randrange(len(live))
            victim = live[k]
            side, price, size = book.orders.get(victim, ("B", mid, 1))
            msg = Message(ts, DELETION, victim, size, price,
                          1 if side == "B" else -1)
            live[k] = live[-1]
            live.pop()
        elif roll < 94:
            victim = live[rng.randrange(len(live))]
            side, price, size = book.orders.get(victim, ("B", mid, 1))
            msg = Message(ts, VISIBLE_EXECUTION, victim,
                          max(1, min(size, 1 + rng.randrange(200))), price,
                          1 if side == "B" else -1)
        elif roll < 99:
            # A hidden execution: no order id, no book change.
            msg = Message(ts, HIDDEN_EXECUTION, 0, 1 + rng.randrange(200),
                          mid + rng.randrange(-50, 50),
                          1 if rng.random() < 0.5 else -1)
        else:
            msg = Message(ts, AUCTION_CROSS, 0, 1000 * (1 + rng.randrange(50)),
                          mid, 1)

        apply(book, msg)
        # Drop references the book no longer has, so later messages stay
        # meaningful.
        live = [o for o in live if o in book.orders]
        messages.append(msg)
        book_lines.append(book.book_line(levels))

    return messages, book_lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="pyref.lobster_check")
    sub = parser.add_subparsers(dest="command", required=True)

    v = sub.add_parser("validate", help="reconstruct and compare")
    v.add_argument("--messages", required=True)
    v.add_argument("--book", required=True)
    v.add_argument("--levels", type=int, default=10)

    g = sub.add_parser("generate", help="emit a synthetic LOBSTER pair")
    g.add_argument("--out-prefix", required=True)
    g.add_argument("--seed", type=int, default=1)
    g.add_argument("--count", type=int, default=2000)
    g.add_argument("--levels", type=int, default=10)

    args = parser.parse_args(argv)

    if args.command == "validate":
        return validate(args.messages, args.book, args.levels)

    messages, book_lines = generate(args.seed, args.count, args.levels)
    message_path = f"{args.out_prefix}_message_{args.levels}.csv"
    book_path = f"{args.out_prefix}_orderbook_{args.levels}.csv"
    with open(message_path, "w", encoding="utf-8") as handle:
        for msg in messages:
            handle.write(msg.to_line() + "\n")
    with open(book_path, "w", encoding="utf-8") as handle:
        for line in book_lines:
            handle.write(line + "\n")
    print(f"# wrote {message_path} and {book_path} ({len(messages)} messages)",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
