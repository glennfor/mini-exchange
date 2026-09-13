# `viz/ui/` — the dashboard

A dense, professional trading window. It contains **no matching logic at
all**: every number on screen came off the wire, and if a panel needs a value
the server computes it.

## Running it

```bash
# 1. a data source -- either the committed recording (no C++ build needed)
python -m viz.server.server --mock viz/server/sample_frames.jsonl.gz

#    ...or a live engine
cmake --preset release-tap && cmake --build build/release-tap --target tap_producer
./build/release-tap/tools/tap_producer --shm /miniex_demo --rate 200000 &
python -m viz.server.server --shm /miniex_demo

# 2. the UI
cd viz/ui && npm install && npm run dev     # http://127.0.0.1:5199
```

The dev server is on **5199**, not Vite's default 5173, because every other
project on a developer's machine is also on 5173 — and `strictPort` is on so a
collision is a loud error rather than a silent move to another port. (That
silent move is how you end up screenshotting somebody else's dashboard, which
happened while this was being built.)

Point it at a different server with `?ws=ws://host:port`.

## Panels

| Panel | Why it is drawn the way it is |
|---|---|
| **DepthLadder** | Canvas, not DOM. At 30 Hz with forty rows, React reconciliation is measurable and the flicker is visible. Changes flash and decay over ~300 ms. |
| **BookHeatmap** | x = time, y = price, colour = log-scaled resting size, trades over-plotted. 900 columns of history; each tick draws **one new column** into an offscreen canvas that is scrolled left, and the visible canvas is a single blit. Redrawing 900 × 600 cells every frame would be 16 M cells per frame. |
| **EngineStats** | Big numerals for ops/sec and p99. Drops and schedule misses **in red and in words** when non-zero. |
| **LatencyCDF** | Percentile against latency on a **log x axis**. A linear axis spends 95% of its width on the part nobody cares about and squashes the entire tail into the last few pixels. |
| **Tape** | Virtualised, capped at 500 rows. An unbounded tape is a memory leak with a scrollbar. |
| **TransportControls** | Play, pause, **step one event**, speed, seek, reset. |

**Step is the one worth building first.** "What did that one message do to the
book" is the question you ask over and over, and without it the only way to
answer is to add a print statement and rebuild.

## Rules the panels follow

* **Colour is never the only signal.** Bids and asks differ by column
  position and by an explicit label; the tape shows the aggressor as a letter;
  drops are flagged with a word as well as a colour.
* **Tabular figures everywhere.** A price ticking from 100.19 to 100.20 must
  not shift every digit sideways.
* **Prices are integer ticks on the wire**, formatted for display here.
  Nothing in this project stores a price as a float.
* **No polling.** WebSocket push only, with a visible connection state and
  exponential-backoff reconnect. A dashboard that silently stops updating
  looks exactly like a quiet market.
* **No `localStorage` dependence for correctness.** Nothing is persisted.

## Testing

```bash
npm test          # 46 tests
npm run typecheck
```

Three layers, and the third is the one that matters:

1. **Pure logic** (`lib/heatmap.test.ts`) — the column ring buffer and the
   price-window arithmetic, in node: that the window follows a drifting
   market by the smallest step that works rather than snapping, that no level
   ever falls outside it however wide the book, and that a window move is
   always a whole number of rows so the painted history can be blitted
   instead of repainted.
2. **Schema drift** (`protocol.test.ts`) — parses the committed sample
   recording, byte for byte the file the mock server replays, and asserts
   every frame satisfies the TypeScript types. Drift between the TypeScript
   and Python halves of the protocol does not throw; it produces a silently
   blank panel.
3. **The whole dashboard** (`App.test.tsx`) — mounts the app in jsdom against
   a stubbed WebSocket, feeds it the entire sample recording, and asserts the
   panels react: canvases issue draw calls, the tape renders newest-first,
   transport commands go out on the socket, and **zero frames are rejected**.

A browser screenshot is not something CI can depend on, so the jsdom tests do
that job instead.

One bug those tests pin down, found while building this: React's StrictMode
mounts, unmounts and remounts every effect, and without an ownership check in
the socket's own handlers the first socket's `onclose` fires *after* the
second effect has reset the closed flag — so it schedules a reconnect, which
orphans another socket, which schedules another. The symptom was four live
connections to a server that should have had one.

## What is not here

**No screenshot or GIF is committed.** Producing one needs a browser session
this environment could not drive: the Chrome window available here was
occluded, which freezes the page's timers, so every capture attempt timed out
against a renderer that was alive but frozen. Rather than commit a mock-up,
the README says the picture is missing and how to take it:

```bash
python -m viz.server.server --mock viz/server/sample_frames.jsonl.gz &
cd viz/ui && npm run dev
# then screen-record http://127.0.0.1:5199 while stepping through the
# transport controls
```
