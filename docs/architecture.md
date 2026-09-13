# Architecture

```text
        ┌──────────────────────────────────────────────────┐
        │              Data sources (on disk)              │
        │  • Nasdaq ITCH 5.0 sample day (binary)           │
        │  • LOBSTER sample day (message + book CSV)       │
        │  • Synthetic flow generator (seeded)             │
        └───────────────┬──────────────────────────────────┘
                        │ raw bytes / generated orders
                        ▼
        ┌──────────────────────────────────────────────────┐
        │  feed/   ITCH 5.0 decoder                        │
        │  zero-copy, mmap'd, big-endian, branch-light     │
        │  → NormalizedEvent stream                        │
        └───────────────┬──────────────────────────────────┘
                        │
          ┌─────────────┴───────────────┐
          ▼                             ▼
┌───────────────────────┐   ┌───────────────────────────────┐
│ book/ MarketDataBook  │   │ engine/ MatchingEngine        │
│ L3 reconstruction of  │   │ price-time priority, full     │
│ Nasdaq's book from    │   │ order lifecycle, our fills    │
│ ITCH add/exec/cancel  │   │ (order-flow injection)        │
└───────────┬───────────┘   └───────────────┬───────────────┘
            │ validate vs LOBSTER           │ trades + book deltas
            ▼                               ▼
     ┌──────────────────────────────────────────────────────┐
     │ bench/  open-loop harness, HdrHistogram, perf counters│
     └──────────────────────────────────────────────────────┘
                        │ (compile-time optional tap)
                        ▼
     ┌──────────────────────────────────────────────────────┐
     │ viz/  SPSC shm ring → viz_server → WebSocket → React │
     └──────────────────────────────────────────────────────┘
```

## Module boundaries

The dependency graph is a DAG and is enforced by the CMake target links, not
by convention:

```text
core  ←  book  ←  engine  ←  tools / bench
  ↑        ↑
  └──── feed ─────┘
```

* `core/` knows nothing about order books. It is allocation-free primitives:
  value types, an SPSC ring, a slab pool, an open-addressed map, a
  hierarchical bitset, a cycle counter.
* `book/` knows nothing about ITCH. It owns storage and ordering.
* `engine/` knows nothing about the UI or the feed. It owns matching policy.
* `viz/` is removable: the engine builds and the whole test suite passes with
  it absent, and the tap compiles out entirely when `MINIEX_TAP=OFF`.

## Threading model

The matching core is **single-threaded by design**, and this is a decision
rather than a limitation — see `docs/design-decisions.md`. Concurrency exists
in exactly two places:

1. The **ingress ring**: one producer thread (feed handler, load generator, or
   replay driver) and the engine thread as the only consumer.
2. The **observability tap**: the engine thread as the only producer, and a
   separate reader *process* as the only consumer, over POSIX shared memory.

Nothing else is shared. There is no lock anywhere in the engine, because
there is nothing to lock.

## Data flow through one command

```text
producer thread                     engine thread
───────────────                     ─────────────
Command{ts, NEW ...}
  → ring.try_push()  ──release──▶  engine.drain(ring, n)
                                     → on_new()
                                        → validate       → Reject
                                        → PostOnly / FOK pre-checks
                                        → emit Ack
                                        → match()        → Trade + Fill ×N
                                        → book.add()     (residual)
                                        → emit Cancelled (IOC residual)
                                     → sink.emit(Event)  ──▶ Null / Counting /
                                                              Vector / Tap
```

The engine never blocks. A full ingress ring is the producer's problem and is
reported as a backpressure event by the harness; a full tap ring is dropped
and counted, because stalling matching to feed a dashboard would be exactly
backwards.

## Determinism

The event stream is a pure function of the command stream and the build. See
`docs/matching-semantics.md` §9 for the mechanism and the tests. This is what
makes the three-way agreement possible: `NaiveBook`, `FastBook`, and the
Python oracle all consume the same command file and their outputs are
compared with `diff`.

## Canonical wire formats

| Format | Where | Purpose |
|---|---|---|
| Command text | `engine/command_parser.hpp`, `pyref/` | hand-written tests, recorded replays, minimised reproducers |
| Event text | `engine/event_format.hpp`, `pyref/` | differential comparison by `diff` |
| Tap record | `viz/tap/` (M9) | 64-byte binary, versioned shm header |
| WebSocket JSON | `viz/server/` (M9) | versioned `v` field on every frame |

The first two are specified in `docs/matching-semantics.md` and implemented
twice on purpose. The last two are versioned because a schema that drifts
silently produces blank panels rather than errors.
