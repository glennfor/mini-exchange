import { describe, expect, it } from 'vitest';

import { ColumnRing, rowFor, updateWindow, type HeatColumn } from './heatmap';

function column(tNs: number): HeatColumn {
  return { tNs, bids: new Map(), asks: new Map(), trades: [] };
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
  it('centres on the mid when there is no window yet', () => {
    expect(updateWindow(null, 1002000, 1002010, 100)).toEqual({
      low: 1002005 - 50,
      high: 1002005 + 50,
    });
  });

  it('does not move while the touch stays in the middle half', () => {
    const initial = updateWindow(null, 1002000, 1002000, 100)!;
    // A chart whose axis slides every frame is unreadable, so the window is
    // sticky.
    const moved = updateWindow(initial, 1002010, 1002012, 100);
    expect(moved).toBe(initial);
  });

  it('recentres once the touch leaves the middle half', () => {
    const initial = updateWindow(null, 1002000, 1002000, 100)!;
    const moved = updateWindow(initial, 1002045, 1002047, 100)!;
    expect(moved).not.toBe(initial);
    expect(moved.low).toBeGreaterThan(initial.low);
  });

  it('copes with a one-sided book', () => {
    expect(updateWindow(null, 1002000, undefined, 10)).toEqual({
      low: 1001995,
      high: 1002005,
    });
    expect(updateWindow(null, undefined, undefined, 10)).toBeNull();
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
