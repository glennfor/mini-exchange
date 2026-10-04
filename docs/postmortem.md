# Post-mortem: every bug this project found in itself

Twenty-eight defects, each found by a test, a measurement, a count or a user
report — not by reading the code. Several of them were found by tests that
were already green, which is the recurring theme: a suite that cannot fail
against a bug is not testing for it.

They are kept in full because the interesting part of a bug is rarely the
fix. It is what made it invisible.

1. **A CI job was red and I had not run it.** `check_no_floats.sh` — the grep
   that enforces "no floating point in the price-carrying layers" — matched
   the words *"double free"* inside an `assert` message, and had therefore
   been failing since the commit that introduced it. Writing a guard is not
   the same as running it.

2. **A test passed locally and failed in a fresh clone.** `.gitignore`
   carried an unanchored `data/`, intended for downloaded market data, which
   also matched `replay/tests/data/` — so the committed LOBSTER validation
   fixture was never actually committed. Found by cloning the repository and
   running the tests in the clone, which is the only way that class of bug is
   ever found.

3. **The cycle counter silently returned garbage on Apple Silicon.**
   `asm("isb; mrs %0, cntvct_el0")` assembles to the `isb` alone on Apple's
   integrated assembler, because `;` introduces a comment there. The
   "timestamps" were whatever happened to be in the register. Caught by a
   monotonicity test on the first run.

4. **The flow generator was benchmarking an empty book.** The default mix had
   removals at 55% and additions at 40%, so the live order set drained to
   single digits within a few thousand commands and every subsequent cancel
   was substituted with an add. Every "before" number would have described a
   book with five orders in it.

5. **Two microbenchmarks were measuring Google Benchmark.** `CancelHead`
   reported 98 µs per cancel. The real figure is 28 ns; the rest was
   `PauseTiming`/`ResumeTiming` rebuilding the book on every iteration.

6. **The differential suite had a hole and was green anyway.** The mutation
   test for "size-down keeps time priority" was not caught, because the flow
   generator re-randomised the price on every replace and therefore almost
   never produced a replace at the order's own price.

7. **The event formatter wrote 204 bytes into a 192-byte buffer.** Caught by
   a test that fills every field with its maximum value — which existed only
   because the spec says to assert struct sizes rather than assume them.

8. **The normalised ITCH event left its padding indeterminate**, so `memcmp`
   of two events was order-dependent: the dispatch-equivalence test passed in
   isolation and failed in a full run.

9. **The shrinker stopped at "smaller", not minimal**, and produced
   16-command reproducers full of irrelevant commands — leaving the reader to
   do the work the shrinker existed to do.

10. **The benchmark made `FastBook` look slower than `NaiveBook`.** It sized
    the pool at the compile-time ceiling of 4 M orders: a 256 MB slab plus a
    128 MB index, nearly 400 MB of cold pages to fault through, against a
    `NaiveBook` that allocates lazily. An artefact of the harness presented as
    a fact about the data structures.

11. **`FastBook`'s open-loop tail was worse than `NaiveBook`'s** even though
    its service time was better — first-touch page faults on 40 MB of pool,
    index and ladder, invisible to a microbenchmark and fully visible to a
    tail-latency measurement. Pre-faulting at construction is the fix
    (DD-034), though this machine is too noisy to settle the size of the win.

12. **The tap could only be created once per boot.** A POSIX shm object
    outlives its process, and on macOS `ftruncate` on an already-sized object
    fails with `EINVAL`, so the second run reported "cannot open the tap
    segment" with no hint that a previous run was the cause.

13. **React's StrictMode left orphaned WebSockets reconnecting.** Four live
    connections to a server that should have had one, found by counting
    established sockets while the page was open.

14. **Vite's silent port fallback meant the first screenshot attempt captured
    a different project's dashboard**, because 5173 was already taken.

15. **The Python tap reader's magic constant was mistyped by one nibble**, and
    the symptom was "not a miniex tap segment" against a segment that plainly
    was one. It is now derived from the bytes of `"MINIEXTP"`.

16. **Pausing the transport did not stop the chart.** The publisher kept
    broadcasting a book frame every cycle regardless, and the heatmap treats
    each book frame as one time column — so a paused replay drew a wall of
    identical columns scrolling right, looking like a market frozen at a
    price rather than a stopped replay. The server now publishes only when
    the book's sequence number actually moves, and the client ignores a
    repeated snapshot as well, because it should not depend on the server
    never repeating itself.

17. **The dashboard blamed the engine for the viewer's lag.** Engine drops
    and server skips were added into one `ring_drops` counter, so a demo in
    which the engine had lost *nothing* showed a red "this view is missing
    data" banner. They are now three separate counters with three different
    messages: the engine discarding events, this server jumping forward, and
    the harness missing its schedule. A warning that points at the wrong
    component is worse than no warning — it sends you to debug the wrong
    thing.

18. **And the banner was permanently lit anyway**, because the server
    replayed whatever was already in the tap ring when it attached, then
    counted the catch-up as a skip. A live dashboard should start at *now*;
    it now passes over the backlog and reports the count once, at attach.

19. **The stats frame validator checked two fields out of ten.** Adding
    `reader_skipped` to the protocol sailed straight through it and crashed a
    render on `undefined.toString()`. A validator that checks a subset gives
    the impression of a schema guarantee while providing none, which is worse
    than no validator — the render code then trusts it.

20. **The viz server died the moment a second browser tab connected.**
    `Hub.broadcast` iterated the live client set while `await ws.send(...)`
    was suspended, and the connection handler added the new client during
    exactly that window — `RuntimeError: Set changed size during iteration`,
    propagating out of the publisher task and taking the process with it. Not
    caught by any test, because every test until then had a fixed set of
    clients. It was found by running `./scripts/demo.sh` and opening the
    dashboard, which is the one thing none of the automated tests do.

21. **The heatmap's price axis jumped instead of scrolling.** It kept the mid
    dead-centre: held still while the mid stayed in the middle half, then
    re-centred in one step — up to a quarter of the window — and repainted
    all 900 columns of history at the new mapping, so the whole chart leapt.
    And the span was a fixed 120 ticks regardless of the book, while `rowFor`
    returns −1 outside the window and the caller skips it, so a book wider
    than 120 ticks lost its outer levels with no indication. Reported by
    someone looking at the running dashboard: *"it is moving around a bit …
    they are out of bounds it looks quite jumpy."*

    **And it took two more goes to fix.** The first shipped because every test
    passed and the invariant I had thought to state — "no level from *this
    frame* is clipped" — held. The same person came back: history that drifted
    off the top of the frame came back as a blank stripe. A scroll buffer is a
    **cache, never a store**; whatever the blit pushes past an edge is
    destroyed, and while that is correct going left, where time only moves one
    way, the price axis moves both ways. I had verified the frame I was
    drawing and not the history I was keeping.

    **The second was wrong too, in the opposite direction.** I made the clip
    impossible by sizing the window to the extent of every column in the ring.
    Nothing could leave the frame — and a 40-tick book was drawn as a hairline
    on an axis covering the 400 ticks the price had wandered over the last
    thirty seconds. *"Just squashing everything into the window means it looks
    tiny; I don't have an issue with it going out of bounds vertically — the
    issue was that if it comes back into view it should not be cut off."*
    Which is the correct requirement, and not the one I had solved. The
    answer was never the window: the offscreen canvas is five window-heights
    tall and is never scrolled vertically, so the window moves by taking a
    different slice and destroys nothing. Measured over 1,235 frames of real
    engine output in which prices left the frame 268 million times: zero gaps,
    four full repaints.

22. **The dashboard showed a book the engine could not have held** — bids at
    100.2195 against asks at 100.1840, crossed by 355 ticks and stuck that
    way for minutes. The server's book is a *delta* book: a level exists
    because an Ack was seen and nothing has taken it away. Pausing the
    transport stops the drain, the tap ring fills in seconds at 200k
    events/s, the reader skips forward to stay live, and every order whose
    cancellation fell in that hole rests for ever. Resume from a four-second
    pause and it goes from 2,949 resting orders to 6,313 and crosses by 194
    ticks, and it never recovers. A delta book cannot survive a gap; the
    repair is to throw it away and rebuild, not to patch around it.

23. **One backgrounded browser tab froze the dashboard for every tab.**
    Broadcasts gather their sends, so a publish cycle costs the slowest
    client — and "slowest" has no upper bound. A tab Chrome has backgrounded
    stops draining its socket, TCP flow control backs the send up, and the
    publish loop waits for ever: no book frames, no stats, a process at 50%
    CPU publishing to nobody. There *was* a test for this. It used a client
    that was slow, not one that had stopped, and it passed the whole time.

24. **The drain starved the publisher, and the heatmap went blank.** A tap
    record costs ~2.5 µs to unpack and apply in Python, so a 32768-record
    batch costs 80 ms against a 33 ms publish interval. Once the reader was
    behind enough to fill a batch every cycle it stayed there: five heatmap
    columns in thirty seconds while the engine ran at full speed and every
    component worked as designed. Bounding a loop by a count is not bounding
    it by time.

25. **Two demos silently shared one shared-memory segment.** `demo.sh` used a
    fixed name, so a second run put a second producer on a single-producer
    ring and the interleaved stream reconstructed into a nonsense market —
    which cost real debugging time, aimed at the wrong layer. Its UI was also
    talking to the *first* demo's server, because the default WebSocket port
    is baked into the page and the script printed a URL without the override.

26. **The test that exists to prevent a vacuous pass passed vacuously.** The
    zero-allocation suite replaces the global `operator new` and counts calls;
    its first test allocates deliberately to prove the replacement is wired
    up. It used a *new-expression*, which `[expr.new]/10` lets the compiler
    elide — and at `-O2` clang did, so the canary allocated nothing, counted
    nothing, and failed. Calling `::operator new` directly is not elidable.
    Writing the guard in a form the optimiser can delete is the same mistake
    the guard exists to catch, one level down.

27. **The dashboard reported a latency of zero nanoseconds.** `tap_producer`
    publishes zeros for the percentiles on purpose — the matching core does
    not time itself — and the panel rendered that as `0 ns` for p50, p99,
    p99.9 and max, with the CDF beside it saying "no latency samples yet", as
    though some were on the way. Four zeros and a patient placeholder, in a
    project whose stated rule is that a panel must admit what it does not
    have. It is the same fault as the blank panel standing in for a quiet
    market, and it sat in every screenshot I took without my noticing. The
    rows now read *not measured* and name `engine_bench`.

28. **The second-producer guard only guarded one end of the ring.** After two
    demos on one shared-memory segment cost real debugging time, I made
    `demo.sh` refuse a second engine — and left the consumer side open. The
    reader owns the ring's tail, so two viz servers on one segment each
    advance it and each see the half of the stream the other did not take.
    Fixing the end that bit me and not its mirror image is how a class of
    bug survives the fix for one instance of it.
