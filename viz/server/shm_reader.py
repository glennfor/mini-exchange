"""Attaches to the engine's shared-memory tap and drains records.

This module re-declares the binary layout that `viz/tap/include/viz/
tap_layout.hpp` defines. **The two must agree exactly.** They are checked
against each other by `viz/server/tests/test_layout.py`, which runs a small
C++ tool that prints the C++ side's constants and compares them field by
field. A layout that drifts between the two languages does not fail loudly --
it produces a dashboard that is confidently wrong -- so the drift check is
worth more than it costs.

The reader is deliberately *untrusted* from the engine's point of view. It may
be slow, it may die, it may never start. The engine drops records when the
ring fills and carries on at the same speed; nothing here can stall it.
"""

from __future__ import annotations

import mmap
import os
import struct
from dataclasses import dataclass

# --- the layout, mirroring viz/tap/include/viz/tap_layout.hpp ---------------

# Derived from the bytes rather than written as a hex literal, because a hex
# literal is exactly the kind of thing that gets one nibble wrong and then
# fails as "not a miniex tap" against a segment that plainly is one. (It did,
# during development.)
MAGIC = int.from_bytes(b"MINIEXTP", "little")
VERSION = 1

HEADER_OFFSET = 0
HEAD_OFFSET = 64
TAIL_OFFSET = 128
COUNTERS_OFFSET = 192
STATS_OFFSET = 256
DATA_OFFSET = 4096

RECORD_SIZE = 64
# seq(Q) ts(q) price(q) qty(I) qty_remaining(I) id(Q) counter_id(Q)
# level_index(I) type(B) side(B) reason(B) flags(B) + 8 bytes of padding
RECORD_FORMAT = "<QqqIIQQIBBBB8x"
assert struct.calcsize(RECORD_FORMAT) == RECORD_SIZE

HEADER_FORMAT = "<QIIQII q 24x"
STATS_FORMAT = "<QQqqqqQQ"
assert struct.calcsize(STATS_FORMAT) == 64

# Event type codes, mirroring engine/messages.hpp EventType.
EVENT_ACK = 0
EVENT_REJECT = 1
EVENT_FILL = 2
EVENT_CANCELLED = 3
EVENT_REPLACED = 4
EVENT_TRADE = 5
EVENT_BOOK_DELTA = 6

EVENT_NAMES = {
    EVENT_ACK: "ACK",
    EVENT_REJECT: "REJECT",
    EVENT_FILL: "FILL",
    EVENT_CANCELLED: "CANCELLED",
    EVENT_REPLACED: "REPLACED",
    EVENT_TRADE: "TRADE",
    EVENT_BOOK_DELTA: "DELTA",
}


class TapVersionError(RuntimeError):
    """The segment's layout is not the one this reader speaks.

    Raised rather than worked around. Reinterpreting bytes from a different
    layout produces plausible-looking output, which is the worst possible
    failure for an observability tool.
    """


@dataclass(frozen=True)
class TapRecord:
    seq: int
    ts: int
    price: int
    qty: int
    qty_remaining: int
    id: int
    counter_id: int
    level_index: int
    type: int
    side: int
    reason: int
    flags: int

    @property
    def type_name(self) -> str:
        return EVENT_NAMES.get(self.type, "UNKNOWN")

    @property
    def side_char(self) -> str:
        return "B" if self.side == 0 else "S"


@dataclass
class TapStats:
    seq: int = 0
    ops_per_sec: int = 0
    p50_ns: int = 0
    p99_ns: int = 0
    p999_ns: int = 0
    max_ns: int = 0
    orders_resting: int = 0
    schedule_misses: int = 0


class ShmTapReader:
    """Drains the engine's tap. One instance per process."""

    def __init__(self, name: str) -> None:
        self.name = name if name.startswith("/") else "/" + name
        self._fd: int | None = None
        self._map: mmap.mmap | None = None
        self.capacity = 0
        self._mask = 0
        self._attach()

    # -- lifecycle ----------------------------------------------------------

    def _attach(self) -> None:
        # POSIX shared memory appears as a file under /dev/shm on Linux; macOS
        # has no such path, so shm_open is reached through the C library.
        path = f"/dev/shm{self.name}"
        if os.path.exists(path):
            self._fd = os.open(path, os.O_RDWR)
        else:
            self._fd = _shm_open(self.name)

        size = os.fstat(self._fd).st_size
        if size < DATA_OFFSET:
            raise TapVersionError(
                f"{self.name} is {size} bytes, too small for a tap header"
            )
        self._map = mmap.mmap(self._fd, size, mmap.MAP_SHARED,
                              mmap.PROT_READ | mmap.PROT_WRITE)

        magic, version, record_size, capacity, flags, _pad, _created = (
            struct.unpack_from(HEADER_FORMAT, self._map, HEADER_OFFSET)
        )
        if magic != MAGIC:
            raise TapVersionError(
                f"{self.name}: bad magic 0x{magic:016X}; not a miniex tap"
            )
        if version != VERSION:
            raise TapVersionError(
                f"{self.name}: segment speaks tap v{version}, this reader "
                f"speaks v{VERSION}. Refusing to attach rather than "
                f"reinterpreting bytes."
            )
        if record_size != RECORD_SIZE:
            raise TapVersionError(
                f"{self.name}: record size {record_size}, expected "
                f"{RECORD_SIZE}"
            )
        if size < DATA_OFFSET + capacity * RECORD_SIZE:
            raise TapVersionError(f"{self.name}: segment smaller than declared")

        self.capacity = capacity
        self._mask = capacity - 1
        self.flags = flags

    def close(self) -> None:
        if self._map is not None:
            self._map.close()
            self._map = None
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None

    def __enter__(self) -> ShmTapReader:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # -- cursors ------------------------------------------------------------

    def _u64(self, offset: int) -> int:
        return struct.unpack_from("<Q", self._map, offset)[0]

    def _set_u64(self, offset: int, value: int) -> None:
        struct.pack_into("<Q", self._map, offset, value)

    @property
    def head(self) -> int:
        return self._u64(HEAD_OFFSET)

    @property
    def tail(self) -> int:
        return self._u64(TAIL_OFFSET)

    @property
    def produced(self) -> int:
        return self._u64(COUNTERS_OFFSET)

    @property
    def dropped(self) -> int:
        return self._u64(COUNTERS_OFFSET + 8)

    @property
    def lag(self) -> int:
        return self.head - self.tail

    @property
    def producer_live(self) -> bool:
        _, _, _, _, flags, _, _ = struct.unpack_from(
            HEADER_FORMAT, self._map, HEADER_OFFSET
        )
        return bool(flags & 1)

    # -- draining -----------------------------------------------------------

    def read(self, max_records: int = 4096) -> list[TapRecord]:
        """Drains up to `max_records`, oldest first.

        Batched on purpose. The reader is allowed to be as slow as it likes,
        and batching is the whole of its throughput -- a full ITCH day is tens
        of millions of events and a per-record Python loop with a syscall in
        it would never keep up.
        """
        tail = self.tail
        head = self.head
        available = min(head - tail, max_records)
        if available <= 0:
            return []

        out = []
        unpack = struct.unpack_from
        buf = self._map
        mask = self._mask
        for i in range(available):
            offset = DATA_OFFSET + ((tail + i) & mask) * RECORD_SIZE
            out.append(TapRecord(*unpack(RECORD_FORMAT, buf, offset)))
        self._set_u64(TAIL_OFFSET, tail + available)
        return out

    def seek_to_live(self) -> int:
        """Starts reading at the producer's current head, discarding backlog.

        Returns how many records were passed over.

        A dashboard attaching to an engine that has been running for a while
        finds a ring already full of history. Replaying it would show a market
        that is minutes stale and then race to catch up; and because the
        catch-up exceeds the lag ceiling, it would also be reported as a skip
        -- so the panel showed a permanent "the viewer is behind" warning for
        something that happened once, at startup, by design.

        Declining to replay history the dashboard was not present for is the
        right default for a *live* view. The number is reported once, at
        attach, rather than folded into the running loss counters.
        """
        head = self.head
        skipped = head - self.tail
        self._set_u64(TAIL_OFFSET, head)
        return skipped

    def skip_to_recent(self, keep: int) -> int:
        """Jumps the tail forward, keeping only the newest `keep` records.

        Returns how many records were skipped.

        A dashboard's job is to show the market *now*. A reader that cannot
        keep up and simply falls behind shows a lagging replay while claiming
        to be live, which is the worst of both -- and the lag grows without
        bound until the engine's ring wraps and the data is lost anyway.
        Skipping forward keeps the view current, and the skip count is
        published so nobody mistakes a gap for a quiet market.
        """
        head = self.head
        tail = self.tail
        behind = head - tail
        if behind <= keep:
            return 0
        new_tail = head - keep
        self._set_u64(TAIL_OFFSET, new_tail)
        return new_tail - tail

    def read_stats(self, attempts: int = 16) -> TapStats | None:
        """Reads the seqlock-protected statistics snapshot.

        Retries on a torn read rather than locking: the engine must never wait
        for this process.
        """
        for _ in range(attempts):
            before = self._u64(STATS_OFFSET)
            if before & 1:
                continue  # a write is in progress
            fields = struct.unpack_from(STATS_FORMAT, self._map, STATS_OFFSET)
            if self._u64(STATS_OFFSET) != before:
                continue  # torn
            return TapStats(*fields)
        return None


def _shm_open(name: str) -> int:
    """shm_open(3) via ctypes, for platforms with no /dev/shm."""
    import ctypes
    import ctypes.util

    libc_name = ctypes.util.find_library("c")
    libc = ctypes.CDLL(libc_name, use_errno=True)
    libc.shm_open.restype = ctypes.c_int
    libc.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint]
    fd = libc.shm_open(name.encode(), os.O_RDWR, 0o600)
    if fd < 0:
        err = ctypes.get_errno()
        raise FileNotFoundError(
            err, f"shm_open({name}): {os.strerror(err)} -- is the engine "
            f"running with MINIEX_TAP=ON?"
        )
    return fd
