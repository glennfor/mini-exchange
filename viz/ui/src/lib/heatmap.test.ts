import { describe, expect, it } from 'vitest';

import {
  ColumnRing,
  makeColumn,
  rowFor,
  shiftWindow,
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
   * side. Against a fixed 120-tick window that means near-continuous
   * re-centring, which is the jumpiness this replaces.
   */
  it('scrolls smoothly and never clips, over 30 seconds of drift', () => {
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
    let previousLow = 0;

    for (let tick = 0; tick < 900; tick++) {
      // The mid walks, bounded, as the generator's does.
      mid += Math.round((rand() - 0.5) * 12);
      mid = Math.max(base - 200, Math.min(base + 200, mid));
      const lowPrice = mid - 21;
      const highPrice = mid + 21;

      const previous = w;
      const wanted = updateWindow(previous, { lowPrice, highPrice });
      if (previous === null) {
        w = wanted;
      } else if (wanted !== previous) {
        const shift = shiftWindow(previous, wanted!, rows);
        w = shift.window;
        if (shift.dy === null) {
          rescales++;
        } else {
          moves++;
          // How much further the window travelled than the data did.
          const dataMoved = Math.abs(lowPrice - previousLow);
          const windowMoved =
            (Math.abs(shift.window.low - previous.low) * rows) /
            windowSpan(previous);
          worstExcess = Math.max(
            worstExcess,
            windowMoved - (dataMoved * rows) / windowSpan(previous),
          );
        }
      }
      previousLow = lowPrice;

      // Nothing is ever silently dropped.
      expect(rowFor(lowPrice, w!, rows)).toBeGreaterThanOrEqual(0);
      expect(rowFor(highPrice, w!, rows)).toBeGreaterThanOrEqual(0);
    }

    // The window never travels further than the data did, give or take the
    // row it is quantised to. That is the whole difference: the axis moves
    // because the market moved, by as much as the market moved. The old code
    // re-centred on the mid, moving the axis -- and repainting all 900
    // columns of history at the new mapping -- by up to a quarter of the
    // window in a single frame regardless of how far price had gone.
    expect(worstExcess).toBeLessThanOrEqual(1.0);
    // It moves on about half the frames under a walk this fast, and not at
    // all while the walk stays clear of the edges -- the window is only ever
    // pushed, never re-centred, so there is no motion it invents on its own.
    expect(moves).toBeGreaterThan(0);
    expect(moves).toBeLessThan(900);
    // And the scale, the only thing that costs a full repaint, settles at
    // once and then stays put.
    expect(rescales).toBeLessThanOrEqual(2);
  });
});

describe('the window over the ring', () => {
  /**
   * The bug, stated as a property: nothing the panel has drawn may leave the
   * frame.
   *
   * The symptom was that it did, and could not come back. `rowFor` returns
   * -1 outside the window and the caller skips the level, so a window sized
   * to the newest book frame clipped the history around it -- and because
   * history lives in a scrolled pixel buffer, whatever the vertical blit
   * pushed past an edge was destroyed. The market would wander off the top
   * of the frame and come back to a blank stripe where its own history
   * should have been: correctly scrolled, correctly aligned, and empty.
   *
   * Deriving the window from the extent of the whole ring makes the clip
   * impossible, which is what this checks, on every tick, for a market that
   * spends thirty seconds wandering.
   */
  it('never lets a column leave the frame, over thirty seconds of drift', () => {
    let seed = 987654321;
    const rand = () => {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      return seed / 0x7fffffff;
    };

    const ring = new ColumnRing(900);
    const rows = 400;
    let w: PriceWindow | null = null;
    let mid = 1002000;

    for (let tick = 0; tick < 2000; tick++) {
      // A walk with the occasional jump, so the window is pushed hard in
      // both directions rather than drifting one way.
      mid += Math.round((rand() - 0.5) * 10);
      if (rand() < 0.01) mid += rand() < 0.5 ? -90 : 90;
      ring.push(book(tick, mid));
      const extent = ring.extent()!;
      w = updateWindow(w, extent);

      // The extent covers every column in the ring, so holding it is holding
      // the whole of what is on screen -- not just the newest column.
      expect(windowHoldsEverything(w!, extent, rows)).toBe(true);
    }

    // And spot-check the equivalence the line above relies on.
    for (const column of ring.toArray()) {
      expect(rowFor(column.lo!, w!, rows)).toBeGreaterThanOrEqual(0);
      expect(rowFor(column.hi!, w!, rows)).toBeGreaterThanOrEqual(0);
    }
  });

  it('discards only empty rows when the window moves', () => {
    // The vertical blit destroys whatever it pushes past an edge. That is
    // only acceptable because those rows are outside the ring's price range,
    // so there is nothing in them -- this is the check that they are.
    const ring = new ColumnRing(900);
    const rows = 400;
    let w: PriceWindow | null = null;
    let mid = 1002000;

    for (let tick = 0; tick < 600; tick++) {
      mid += tick < 300 ? 3 : -3; // out of frame one way, then back
      ring.push(book(tick, mid));
      const previous = w;
      const wanted = updateWindow(previous, ring.extent());
      if (previous === null || wanted === null) {
        w = wanted;
        continue;
      }
      const shift = shiftWindow(previous, wanted, rows);
      w = shift.window;
      if (shift.dy === null || shift.dy === 0) continue;

      // Rows the blit is about to drop, in the mapping they had.
      const extent = ring.extent()!;
      const dropped =
        shift.dy > 0
          ? { from: rows - shift.dy, to: rows - 1 } // pushed off the bottom
          : { from: 0, to: -shift.dy - 1 }; //        pushed off the top
      for (let row = dropped.from; row <= dropped.to; row++) {
        // The price that row held, under the window before the move.
        const price =
          previous.high - ((row + 0.5) * windowSpan(previous)) / rows;
        expect(
          price > extent.highPrice || price < extent.lowPrice,
        ).toBe(true);
      }
    }
  });

  it('reports no extent until something has been drawn', () => {
    const ring = new ColumnRing(8);
    expect(ring.extent()).toBeNull();
    ring.push(makeColumn(0, new Map(), new Map(), []));
    expect(ring.extent()).toBeNull();
  });

  it('forgets the range of columns that have scrolled off', () => {
    // The window must be free to shrink back once a spike has left the ring;
    // an extent that only ever grew would leave the axis stretched around a
    // price nothing on screen still reaches.
    const ring = new ColumnRing(4);
    ring.push(book(0, 1_000_000));
    ring.push(book(1, 1_009_000));
    expect(ring.extent()!.highPrice).toBeGreaterThan(1_008_000);
    for (let i = 2; i < 8; i++) ring.push(book(i, 1_000_000));
    expect(ring.extent()!.highPrice).toBeLessThan(1_000_100);
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
});

describe('shiftWindow', () => {
  it('quantises the move so the blit is exact', () => {
    const from = { low: 1000, high: 1128 };
    const to = { low: 1003, high: 1131 };
    const { window, dy } = shiftWindow(from, to, 256);
    // 128 ticks over 256 rows: 3 ticks is exactly 6 rows.
    expect(dy).toBe(6);
    expect(window.high).toBeCloseTo(1131, 9);
  });

  it('rounds the window, not the blit, when a move lands between rows', () => {
    // Rounding the blit would leave history painted at the wrong price, and
    // the error would accumulate over thousands of moves.
    const from = { low: 0, high: 100 };
    const { window, dy } = shiftWindow(from, { low: 1, high: 101 }, 30);
    expect(Number.isInteger(dy)).toBe(true);
    expect(((window.high - from.high) * 30) / 100).toBeCloseTo(dy!, 9);
  });

  it('never quantises a wanted move away to nothing', () => {
    // One tick is a third of a row here; rounding to nearest would give 0 and
    // the window would never move at all.
    const { dy } = shiftWindow({ low: 0, high: 900 }, { low: 1, high: 901 }, 300);
    expect(dy).toBe(1);
  });

  it('stays exact over a long drift', () => {
    let w: PriceWindow = { low: 0, high: 100 };
    for (let i = 0; i < 1000; i++) {
      const next = shiftWindow(w, { low: w.low + 1, high: w.high + 1 }, 30);
      // Every step is a whole number of rows from the previous one, which is
      // the property that lets the history be blitted instead of repainted.
      expect(((next.window.high - w.high) * 30) / 100).toBeCloseTo(next.dy!, 9);
      w = next.window;
    }
    expect(windowSpan(w)).toBeCloseTo(100, 6);
  });

  it('refuses to blit across a change of scale', () => {
    const { dy } = shiftWindow({ low: 0, high: 100 }, { low: 0, high: 200 }, 50);
    expect(dy).toBeNull();
  });

  it('refuses to blit a move bigger than the canvas', () => {
    const { dy } = shiftWindow({ low: 0, high: 100 }, { low: 500, high: 600 }, 50);
    expect(dy).toBeNull();
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
