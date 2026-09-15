import { describe, expect, it } from 'vitest';

import {
  BUFFER_FACTOR,
  ColumnRing,
  bufferWindowFor,
  makeColumn,
  placeView,
  rowFor,
  updateWindow,
  windowHoldsEverything,
  windowSpan,
  type HeatColumn,
  type PriceWindow,
} from './heatmap';

function column(tNs: number): HeatColumn {
  return makeColumn(tNs, new Map(), new Map(), []);
}

/** A column of depth around a mid, as the server sends it: 20 levels a side. */
function book(tNs: number, mid: number, depth = 20): HeatColumn {
  const bids = new Map<number, number>();
  const asks = new Map<number, number>();
  for (let i = 0; i < depth; i++) {
    bids.set(mid - 1 - i, 100 + i);
    asks.set(mid + 1 + i, 100 + i);
  }
  return makeColumn(tNs, bids, asks, []);
}

describe('ColumnRing', () => {
  it('keeps the newest N columns, oldest first', () => {
    const ring = new ColumnRing(4);
    for (let i = 0; i < 10; i++) ring.push(column(i));
    expect(ring.size).toBe(4);
    expect(ring.toArray().map((c) => c.tNs)).toEqual([6, 7, 8, 9]);
    expect(ring.newest()?.tNs).toBe(9);
  });

  it('is empty before anything is pushed', () => {
    const ring = new ColumnRing(8);
    expect(ring.size).toBe(0);
    expect(ring.toArray()).toEqual([]);
    expect(ring.newest()).toBeNull();
  });

  it('handles fewer pushes than its capacity', () => {
    const ring = new ColumnRing(900);
    for (let i = 0; i < 3; i++) ring.push(column(i));
    expect(ring.toArray().map((c) => c.tNs)).toEqual([0, 1, 2]);
  });

  it('holds a full 30-second buffer at 30 Hz', () => {
    // The spec's constraint is 900 columns with the UI still at 60 fps.
    //
    // This test proves the buffer arithmetic and nothing else. The frame rate
    // has **not** been verified: it needs a browser, and the one available in
    // this environment could not be driven (see viz/ui/README.md). The
    // argument that it should hold is architectural -- one column drawn per
    // tick into an offscreen canvas, then a single blit -- not measured.
    const ring = new ColumnRing(900);
    for (let i = 0; i < 5000; i++) ring.push(column(i));
    expect(ring.size).toBe(900);
    expect(ring.toArray()[0]?.tNs).toBe(4100);
  });
});

describe('updateWindow', () => {
  const at = (low: number, high: number) => ({ lowPrice: low, highPrice: high });

  it('centres on the data when there is no window yet', () => {
    const w = updateWindow(null, at(1002000, 1002010))!;
    expect(w.low).toBeLessThan(1002000);
    expect(w.high).toBeGreaterThan(1002010);
    expect((w.low + w.high) / 2).toBeCloseTo(1002005, 0);
  });

  it('returns null until there is data', () => {
    expect(updateWindow(null, null)).toBeNull();
  });

  it('does not move while the data stays clear of the edges', () => {
    const initial = updateWindow(null, at(1002000, 1002020))!;
    // A chart whose axis slides every frame is unreadable, so the window only
    // moves when it has to.
    expect(updateWindow(initial, at(1002001, 1002021))).toBe(initial);
  });

  it('follows a drifting market by the smallest step that works', () => {
    // The bug this replaces: the old window kept the mid centred, so it
    // snapped by a quarter of the span at a time and took the whole painted
    // history with it. A step that large is the jump the user saw.
    let w = updateWindow(null, at(1002000, 1002020))!;
    const span = windowSpan(w);
    for (let drift = 1; drift <= 200; drift++) {
      const next = updateWindow(w, at(1002000 + drift, 1002020 + drift))!;
      expect(windowSpan(next)).toBe(span);
      expect(next.high - w.high).toBeLessThanOrEqual(1);
      expect(next.high).toBeGreaterThanOrEqual(w.high);
      w = next;
    }
    // ...and it did in fact follow, rather than stand still.
    expect(w.high).toBeGreaterThan(1002020 + 150);
  });

  it('keeps every level inside the window, however wide the book', () => {
    // A fixed span clipped, and `rowFor` drops a clipped level without
    // saying so.
    let w: PriceWindow | null = null;
    for (const half of [10, 60, 300, 1500, 30, 20]) {
      for (let i = 0; i < 40; i++) {
        w = updateWindow(w, at(1002000 - half, 1002000 + half));
      }
      expect(rowFor(1002000 - half, w!, 400)).toBeGreaterThanOrEqual(0);
      expect(rowFor(1002000 + half, w!, 400)).toBeGreaterThanOrEqual(0);
    }
  });

  it('holds the data against the margin without a tick of slack', () => {
    // The fit and the nudge have to agree exactly. If the span is the
    // smallest that fits the data inside its margins, then bringing one edge
    // to its margin must not push the other edge past its own -- one tick of
    // disagreement there is one tick of the market clipped.
    for (let needed = 1; needed < 600; needed++) {
      let w = updateWindow(null, at(1002000, 1002000 + needed))!;
      // Drift up, then back down, staying at the same width.
      for (const base of [1002000, 1002040, 1001960, 1002000]) {
        for (let i = 0; i < 80; i++) {
          w = updateWindow(w, at(base + i, base + i + needed))!;
          expect(windowHoldsEverything(w, at(base + i, base + i + needed), 400))
            .toBe(true);
        }
      }
    }
  });

  it('grows at once but shrinks only past the deadband', () => {
    const narrow = updateWindow(null, at(1002000, 1002010))!;
    const wide = updateWindow(narrow, at(1000000, 1004000))!;
    expect(windowSpan(wide)).toBeGreaterThan(windowSpan(narrow));

    // Back to a narrower book: one rung down is inside the deadband, so the
    // scale holds rather than flapping.
    const oneRung = updateWindow(wide, at(1001100, 1004000))!;
    expect(windowSpan(oneRung)).toBe(windowSpan(wide));

    const muchNarrower = updateWindow(wide, at(1002000, 1002010))!;
    expect(windowSpan(muchNarrower)).toBeLessThan(windowSpan(wide));
  });
});

describe('the window under a drifting market', () => {
  /**
   * The conditions the panel actually runs in: replay/synthetic.hpp walks the
   * mid +/-200 ticks around its base and the server publishes 20 levels a
   * side. Against a fixed 120-tick window that meant near-continuous
   * re-centring, which is the jumpiness this replaces.
   */
  it('follows the market without travelling further than it does', () => {
    let seed = 12345;
    const rand = () => {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      return seed / 0x7fffffff;
    };

    const base = 1002000;
    let mid = base;
    let w: PriceWindow | null = null;
    let rescales = 0;
    let worstExcess = 0;
    let moves = 0;
    const rows = 400;
    let previousLow = base - 21;

    for (let tick = 0; tick < 900; tick++) {
      // The mid walks, bounded, as the generator's does.
      mid += Math.round((rand() - 0.5) * 12);
      mid = Math.max(base - 200, Math.min(base + 200, mid));
      const lowPrice = mid - 21;
      const highPrice = mid + 21;

      // Annotated: without it the narrowed type of `w` depends on the
      // assignment below, which depends on `wanted`, which depends on this.
      const previous: PriceWindow | null = w;
      const wanted: PriceWindow = updateWindow(previous, {
        lowPrice,
        highPrice,
      })!;
      if (previous === null) {
        w = wanted;
      } else if (wanted !== previous) {
        if (windowSpan(wanted) !== windowSpan(previous)) {
          rescales++;
        } else {
          moves++;
          // How much further the window travelled than the data did.
          const dataMoved = Math.abs(lowPrice - previousLow);
          worstExcess = Math.max(
            worstExcess,
            Math.abs(wanted.low - previous.low) - dataMoved,
          );
        }
        w = wanted;
      }
      previousLow = lowPrice;

      // Nothing the current book draws is ever clipped.
      expect(windowHoldsEverything(w!, { lowPrice, highPrice }, rows)).toBe(
        true,
      );
    }

    // The window never travels further than the data did. That is the whole
    // difference: the axis moves because the market moved, by as much as the
    // market moved. The old code re-centred on the mid, moving the axis -- and
    // repainting all 900 columns of history at the new mapping -- by up to a
    // quarter of the window in a single frame regardless of how far price had
    // actually gone.
    expect(worstExcess).toBeLessThanOrEqual(0);
    // It also holds still whenever the walk stays clear of the edges: the
    // window is only ever pushed, never re-centred, so there is no motion it
    // invents on its own.
    expect(moves).toBeGreaterThan(0);
    expect(moves).toBeLessThan(900);
    // And the scale, the only thing that costs a full repaint, settles at
    // once and then stays put.
    expect(rescales).toBeLessThanOrEqual(2);
  });
});

/**
 * The panel, reduced to the part that can be wrong.
 *
 * The offscreen buffer is a *cache of painted pixels*: a price is drawn for a
 * column only while the buffer covering it has not been re-placed since, and
 * re-placing repaints every column in the ring. This models exactly that, so
 * the question "would the panel show a gap?" becomes a question about sets.
 */
class PanelModel {
  readonly ring: ColumnRing;
  view: PriceWindow | null = null;
  buffer: PriceWindow | null = null;
  /** Column tNs -> the prices currently painted for it. */
  painted = new Map<number, Set<number>>();
  repaints = 0;
  moves = 0;

  constructor(
    readonly rows: number,
    readonly capacity = 900,
    readonly factor = BUFFER_FACTOR,
    /**
     * Model the behaviour this replaced: no buffer taller than the window,
     * scrolled vertically to follow it, destroying whatever it pushes past
     * an edge. Here so that the tests below can be shown to fail.
     */
    readonly scrollsVertically = false,
  ) {
    this.ring = new ColumnRing(capacity);
  }

  private pricesOf(column: HeatColumn): number[] {
    return [
      ...column.bids.keys(),
      ...column.asks.keys(),
      ...column.trades.map((t) => t.price),
    ];
  }

  private holds(window: PriceWindow, price: number): boolean {
    return rowFor(price, window, this.rows) >= 0;
  }

  /** What a full repaint does: everything in the ring that lands in range. */
  private repaintFromRing(): void {
    this.painted = new Map();
    for (const column of this.ring.toArray()) this.paint(column);
  }

  private paint(column: HeatColumn): void {
    this.painted.set(
      column.tNs,
      new Set(this.pricesOf(column).filter((p) => this.holds(this.buffer!, p))),
    );
  }

  push(column: HeatColumn): void {
    this.ring.push(column);
    const wanted =
      column.lo === null || column.hi === null
        ? this.view
        : updateWindow(this.view, { lowPrice: column.lo, highPrice: column.hi });
    if (wanted === null) return;

    const rescaled =
      this.view === null || windowSpan(this.view) !== windowSpan(wanted);
    const moved = this.view !== null && !rescaled && wanted.low !== this.view.low;

    if (this.scrollsVertically) {
      // What shipped: the offscreen canvas is exactly the window, and it is
      // blitted vertically to follow it.
      this.buffer = wanted;
      this.view = wanted;
      if (rescaled) {
        this.repaints++;
        this.repaintFromRing();
      } else if (moved) {
        this.moves++;
        // The blit. Whatever it pushed past an edge is gone, and nothing
        // ever paints it again.
        for (const [tNs, prices] of this.painted) {
          this.painted.set(
            tNs,
            new Set([...prices].filter((p) => this.holds(this.view!, p))),
          );
        }
      }
      this.paint(column);
      return;
    }

    const rowsPerTick = this.rows / windowSpan(wanted);
    let placed =
      rescaled || this.buffer === null
        ? null
        : placeView(
            this.buffer,
            wanted,
            rowsPerTick,
            this.rows * this.factor,
            this.rows,
          );

    if (placed === null) {
      this.repaints++;
      this.buffer = bufferWindowFor(wanted, this.ring.extent(), this.factor);
      placed = placeView(
        this.buffer,
        wanted,
        rowsPerTick,
        this.rows * this.factor,
        this.rows,
      );
      expect(placed).not.toBeNull();
      this.view = placed!.view;
      this.repaintFromRing();
    } else {
      if (moved) this.moves++;
      this.view = placed.view;
      this.paint(column); // only the new column is drawn
    }
  }

  /** Prices that are on screen but have no pixel. The bug, as a number. */
  gaps(): number {
    let missing = 0;
    for (const c of this.ring.toArray()) {
      for (const price of this.pricesOf(c)) {
        if (!this.holds(this.view!, price)) continue; // off screen: fine
        if (!this.painted.get(c.tNs)?.has(price)) missing++;
      }
    }
    return missing;
  }
}

describe('the buffer the window looks through', () => {
  /**
   * The bug, stated as a property.
   *
   * "When the heatmap goes out of frame down or up and then returns to frame
   * the part that went out of frame remains cut off." Going out of frame is
   * fine and expected -- the window stays tight on the book so the market
   * fills the panel. Coming back to a blank stripe is not.
   *
   * The old code scrolled the offscreen buffer vertically to follow the
   * window, which destroyed whatever it pushed past an edge, so there was
   * nothing to come back to. This walks the market out of frame and back,
   * repeatedly, and asserts every price on screen has a pixel.
   */
  it('has no gaps when the market leaves the frame and returns', () => {
    const panel = new PanelModel(400);
    let mid = 1002000;
    let tick = 0;

    // Out of frame one way, back, out the other way, back. Each leg is far
    // enough to push the window well past its own height.
    for (const leg of [+4, -4, -4, +4, +6, -6]) {
      for (let i = 0; i < 300; i++) {
        mid += leg;
        panel.push(book(tick++, mid));
        expect(panel.gaps()).toBe(0);
      }
    }
    // It really did leave the frame, or the test proves nothing.
    expect(panel.repaints).toBeGreaterThan(0);
    expect(panel.moves).toBeGreaterThan(100);
  });

  it('and the version this replaced left exactly those gaps', () => {
    // The same walk against a window-sized buffer scrolled vertically, which
    // is what shipped. Without this the test above could be passing because
    // the model cannot express the bug.
    const panel = new PanelModel(400, 900, 1, true);
    let mid = 1002000;
    let tick = 0;
    let worst = 0;
    for (const leg of [+4, -4, -4, +4, +6, -6]) {
      for (let i = 0; i < 300; i++) {
        mid += leg;
        panel.push(book(tick++, mid));
        worst = Math.max(worst, panel.gaps());
      }
    }
    expect(worst).toBeGreaterThan(0);
  });

  it('keeps the book filling the panel rather than squashing it', () => {
    // The window tracks the book, not the thirty seconds of history around
    // it: an axis stretched over everything the price wandered past leaves a
    // 40-tick book as a hairline.
    const panel = new PanelModel(400);
    let mid = 1002000;
    for (let i = 0; i < 900; i++) {
      mid += i < 450 ? 2 : -2; // 900 ticks of wandering
      panel.push(book(i, mid));
    }
    const book_ = panel.ring.newest()!;
    const occupied = (book_.hi! - book_.lo!) / windowSpan(panel.view!);
    expect(occupied).toBeGreaterThan(0.4);
  });

  it('places a buffer that always contains the window', () => {
    // The invariant the whole design rests on. If it can fail, the panel can
    // blit from rows that were never painted.
    for (const span of [32, 64, 128, 512]) {
      for (const offset of [-5000, -137, 0, 42, 9999]) {
        const view = { low: 1002000, high: 1002000 + span };
        const extent = {
          lowPrice: view.low + offset,
          highPrice: view.high + offset,
        };
        const buffer = bufferWindowFor(view, extent, BUFFER_FACTOR);
        expect(windowSpan(buffer)).toBe(span * BUFFER_FACTOR);
        expect(buffer.low).toBeLessThanOrEqual(view.low);
        expect(buffer.high).toBeGreaterThanOrEqual(view.high);
      }
    }
  });

  it('centres the buffer on the history, so more of it is retained', () => {
    // Centring on the window wastes half the margin when the market has been
    // trending: the history is all on one side.
    const view = { low: 1002000, high: 1002064 };
    const below = bufferWindowFor(
      view,
      { lowPrice: 1001600, highPrice: 1002064 },
      3,
    );
    expect(below.low).toBeLessThan(view.low - 64);
    expect(below.high).toBe(view.high);
  });

  it('reports the window as unplaceable once it leaves the buffer', () => {
    const view = { low: 1002000, high: 1002100 };
    const buffer = bufferWindowFor(view, null, 3);
    const rowsPerTick = 400 / 100;
    expect(placeView(buffer, view, rowsPerTick, 1200, 400)).not.toBeNull();

    const far = { low: 1002500, high: 1002600 };
    expect(placeView(buffer, far, rowsPerTick, 1200, 400)).toBeNull();
  });

  it('snaps the window to a whole buffer row so the blit does not resample', () => {
    const buffer = { low: 1000, high: 1300 };
    const view = { low: 1100.37, high: 1200.37 };
    const placed = placeView(buffer, view, 4, 1200, 400)!;
    expect(Number.isInteger(placed.sy)).toBe(true);
    expect((buffer.high - placed.view.high) * 4).toBeCloseTo(placed.sy, 9);
    expect(Math.abs(placed.view.high - view.high)).toBeLessThan(1 / 4);
  });

  it('counts trade prints, which can land outside the depth around them', () => {
    // A print lands at the *resting* order's price, and that level may be
    // gone from the book by the time the column is built. Leaving prints out
    // of the extent is how a trade mark gets drawn outside the window.
    const c = makeColumn(
      0,
      new Map([[1002000, 10]]),
      new Map([[1002010, 10]]),
      [{ price: 1002500, qty: 100, side: 'B' }],
    );
    expect(c.hi).toBe(1002500);
    expect(c.lo).toBe(1002000);
  });

  it('forgets the range of columns that have scrolled off', () => {
    const ring = new ColumnRing(4);
    ring.push(book(0, 1_000_000));
    ring.push(book(1, 1_009_000));
    expect(ring.extent()!.highPrice).toBeGreaterThan(1_008_000);
    for (let i = 2; i < 8; i++) ring.push(book(i, 1_000_000));
    expect(ring.extent()!.highPrice).toBeLessThan(1_000_100);
  });

  it('reports no extent until something has been drawn', () => {
    const ring = new ColumnRing(8);
    expect(ring.extent()).toBeNull();
    ring.push(makeColumn(0, new Map(), new Map(), []));
    expect(ring.extent()).toBeNull();
  });
});

describe('rowFor', () => {
  const window = { low: 1000, high: 1100 };

  it('puts higher prices at the top', () => {
    expect(rowFor(1100, window, 100)).toBe(0);
    expect(rowFor(1000, window, 100)).toBe(99);
  });

  it('rejects prices outside the window', () => {
    expect(rowFor(999, window, 100)).toBe(-1);
    expect(rowFor(1101, window, 100)).toBe(-1);
  });

  it('never returns a row past the end', () => {
    for (let p = 1000; p <= 1100; p++) {
      const row = rowFor(p, window, 40);
      expect(row).toBeGreaterThanOrEqual(0);
      expect(row).toBeLessThan(40);
    }
  });
});
