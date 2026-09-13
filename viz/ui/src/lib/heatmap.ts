/**
 * The heatmap's column ring buffer.
 *
 * The panel keeps the last N publish ticks and blits them. It does **not**
 * redraw history every frame: at 30 Hz with 900 columns and 5000 price rows
 * that would be 135 million cells a second, which no browser will do.
 *
 * Instead each new column is drawn once, into an offscreen canvas that is
 * scrolled left by one column per tick. Redrawing the whole buffer only
 * happens when the price window changes, which is rare.
 *
 * This file is the pure part -- the ring buffer and the price-window
 * arithmetic -- so it can be tested in node without a canvas.
 */

export interface HeatColumn {
  tNs: number;
  /** price tick -> resting quantity, bids. */
  bids: Map<number, number>;
  asks: Map<number, number>;
  trades: { price: number; qty: number; side: 'B' | 'S' }[];
}

export class ColumnRing {
  private readonly buffer: (HeatColumn | null)[];
  private head = 0;
  private filled = 0;

  constructor(public readonly capacity: number) {
    this.buffer = new Array<HeatColumn | null>(capacity).fill(null);
  }

  push(column: HeatColumn): void {
    this.buffer[this.head] = column;
    this.head = (this.head + 1) % this.capacity;
    if (this.filled < this.capacity) this.filled += 1;
  }

  get size(): number {
    return this.filled;
  }

  /** Oldest first. */
  toArray(): HeatColumn[] {
    const out: HeatColumn[] = [];
    const start = (this.head - this.filled + this.capacity) % this.capacity;
    for (let i = 0; i < this.filled; i++) {
      const column = this.buffer[(start + i) % this.capacity];
      if (column) out.push(column);
    }
    return out;
  }

  newest(): HeatColumn | null {
    if (this.filled === 0) return null;
    return this.buffer[(this.head - 1 + this.capacity) % this.capacity] ?? null;
  }

  clear(): void {
    this.buffer.fill(null);
    this.head = 0;
    this.filled = 0;
  }
}

export interface PriceWindow {
  low: number;
  high: number;
}

/**
 * Chooses the visible price range.
 *
 * Hysteresis, deliberately: recomputing the window from the current book every
 * frame makes the whole heatmap slide vertically whenever one deep order
 * arrives or leaves, and a chart whose axis moves is unreadable. The window
 * only moves when the touch leaves the middle half of it.
 */
export function updateWindow(
  current: PriceWindow | null,
  bestBid: number | undefined,
  bestAsk: number | undefined,
  span: number,
): PriceWindow | null {
  const mid =
    bestBid !== undefined && bestAsk !== undefined
      ? Math.round((bestBid + bestAsk) / 2)
      : (bestBid ?? bestAsk);
  if (mid === undefined) return current;

  const half = Math.max(2, Math.floor(span / 2));
  if (current === null) return { low: mid - half, high: mid + half };

  const quarter = (current.high - current.low) / 4;
  const inside = mid > current.low + quarter && mid < current.high - quarter;
  if (inside) return current;
  return { low: mid - half, high: mid + half };
}

/** Row index for a price within a window, or -1 if outside. */
export function rowFor(price: number, window: PriceWindow, rows: number): number {
  if (price < window.low || price > window.high) return -1;
  const span = window.high - window.low;
  if (span <= 0) return 0;
  // Higher prices at the top, which is how every depth chart is read.
  return Math.min(rows - 1, Math.floor(((window.high - price) / span) * rows));
}
