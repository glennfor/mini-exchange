# `viz/server/` — the observability server

Attaches to the engine's shared-memory tap, maintains its own copy of the
book, and publishes coalesced snapshots over a WebSocket.

## Why coalescing is the whole point

A full ITCH day is tens of millions of events. Forwarding them per-event to a
browser is **not a performance problem to be optimised — it is a design
error.** The server drains the tap as fast as it can, applies every event to
its own book, and publishes a *snapshot* at a fixed rate (30 Hz by default).
The browser never sees an event it cannot draw.

The server does the aggregation, not the engine. Putting it in the engine
would mean the matching core doing arithmetic on behalf of a dashboard.

## Falling behind

Python cannot drain a C++ matching engine running at a million commands a
second, and it does not try. When the backlog exceeds `--max-lag`, the reader
**skips forward** to the newest records and counts what it skipped.

A reader that simply falls behind shows a lagging replay while claiming to be
live, which is the worst of both — and the lag grows without bound until the
engine's ring wraps and the data is lost anyway. Skipping keeps the view
current. The skip count is published in the `stats` frame and the UI shows it
in red, because a dashboard that quietly shows a partial market is worse than
one that admits it is missing data.

Measured on the development machine at 800k commands/s offered: the reader
sustains roughly 250k events/s, skips the rest, and holds its lag at ~140k
records instead of growing without limit. **The engine is unaffected either
way** — it drops into a full ring and carries on.

## Running it

```bash
# against a live engine
cmake --preset release && cmake --build build/release --target tap_producer
./build/release/tools/tap_producer --shm /miniex_demo --rate 200000 &
python -m viz.server.server --shm /miniex_demo --port 8765

# against the committed recording -- no C++ build, no market data
python -m viz.server.server --mock viz/server/sample_frames.jsonl.gz
```

## Fan-out

`Hub.broadcast` snapshots the client set before iterating it and sends to
every client concurrently.

Both matter. `ws.send()` suspends, and while it is suspended the connection
handler runs — adding a client for a new tab, removing one for a closing tab.
Iterating the live set across that await raises `Set changed size during
iteration`, which propagates out of the publisher task and kills the server;
the symptom was the process dying the moment a second tab was opened.
Awaiting clients in turn is the milder version of the same mistake: the
publish cycle then costs the *sum* of every client's send, so one tab that
has stopped reading stalls the dashboard for all the others.

## Files

| File | What |
|---|---|
| `shm_reader.py` | the binary layout, mirrored from `viz/tap/include/viz/tap_layout.hpp` |
| `book_state.py` | applies engine events to a depth view |
| `protocol.py` | every wire frame is built here and nowhere else |
| `server.py` | asyncio, coalescing, transport, WebSocket |
| `record_frames.py` | generates the mock recording |
| `sample_frames.jsonl.gz` | 30 seconds of frames, committed (56 KB) |

## The layout drift check

`shm_reader.py` re-declares the binary layout that the C++ header defines. A
mismatch between the two does **not** fail loudly: the Python side would
unpack the wrong offsets and produce plausible numbers. So
`tests/test_layout.py` runs `tools/tap_layout_dump`, which prints the C++
side's constants, and compares them field by field — including the offset of
every field within a record, so a reordering is caught as well as a resize.

`tests/test_end_to_end.py` is the dynamic counterpart: it runs the real
engine into a real segment and asserts the Python side reads back sane prices,
contiguous sequence numbers and an uncrossed book.
