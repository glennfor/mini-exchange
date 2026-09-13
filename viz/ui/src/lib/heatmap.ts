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
  /** Lowest price shown, in ticks. Inclusive. */
  low: number;
  /** Highest price shown, in ticks. Inclusive. */
  high: number;
}

export function windowSpan(w: PriceWindow): number {
  return w.high - w.low;
}

/**
 * The spans the window is allowed to take, in ticks.
 *
 * A discrete ladder rather than a continuous fit, because changing the span
 * changes how many pixels a tick is worth, which invalidates the whole
 * painted history and forces a full repaint. Snapping to powers of two means
 * that happens when the market's depth genuinely changes scale, not every
 * time one order arrives a tick further out.
 */
export const SPANS = [32, 64, 128, 256, 512, 1024, 2048, 4096, 8192] as const;

/** Keeps the visible range this far from the edge, as a fraction of the span. */
const EDGE_MARGIN = 0.12;

/**
 * How much larger than necessary the span may be before it is allowed to
 * shrink. The ladder doubles, so 2.5 means the data has to fit inside a span
 * two rungs down -- a deadband wide enough that a book breathing around a
 * rung boundary cannot make the axis flap between two scales.
 */
const SHRINK_DEADBAND = 2.5;

export interface WindowInput {
  /** Lowest price currently being drawn, in ticks. */
  lowPrice: number;
  /** Highest price currently being drawn. */
  highPrice: number;
}

/**
 * Chooses the visible price range.
 *
 * Two rules, and the first version of this got both wrong.
 *
 * **Move by the minimum needed, never re-centre.** Snapping the touch back to
 * the middle of the window moves it by up to a quarter of the span in one
 * step, and because the y axis is price, the entire painted history jumps
 * vertically with it. Nudging the window by the few ticks required to bring
 * the data back inside the margin turns that into a smooth scroll: the window
 * follows a drifting market a tick at a time.
 *
 * **Size the span to the data.** A fixed span silently clips: `rowFor`
 * returns -1 for anything outside and the level simply is not drawn, so a
 * book that widens loses its outer levels with no indication that it has.
 * The span grows the moment the data does not fit and shrinks only once the
 * data would fit two rungs down, so a book that widens keeps all its levels
 * and one that narrows does not set the axis flapping.
 */
export function updateWindow(
  current: PriceWindow | null,
  input: WindowInput | null,
): PriceWindow | null {
  if (input === null) return current;
  const { lowPrice, highPrice } = input;
  const needed = Math.max(1, highPrice - lowPrice);

  // Smallest span on the ladder that fits the data plus its margins.
  const withMargin = needed / (1 - 2 * EDGE_MARGIN);
  let span: number = SPANS.find((s) => s >= withMargin) ?? SPANS[SPANS.length - 1]!;

  if (current !== null) {
    const currentSpan = windowSpan(current);
    // Keep the current span unless the data no longer fits it, or has shrunk
    // far enough inside it to be worth the repaint a rescale costs.
    if (currentSpan >= withMargin && currentSpan <= span * SHRINK_DEADBAND) {
      span = currentSpan;
    }
  }

  const margin = Math.max(1, Math.round(span * EDGE_MARGIN));

  if (current === null || windowSpan(current) !== span) {
    const centre = Math.round((lowPrice + highPrice) / 2);
    const low = centre - Math.floor(span / 2);
    return { low, high: low + span };
  }

  // Nudge by the least amount that brings the data back inside the margins.
  let { low, high } = current;
  if (highPrice > high - margin) {
    const shift = highPrice - (high - margin);
    low += shift;
    high += shift;
  } else if (lowPrice < low + margin) {
    const shift = low + margin - lowPrice;
    low -= shift;
    high -= shift;
  }
  if (low === current.low) return current;
  return { low, high };
}

/** Row index for a price within a window, or -1 if outside. */
export function rowFor(price: number, window: PriceWindow, rows: number): number {
  if (price < window.low || price > window.high) return -1;
  const span = windowSpan(window);
  if (span <= 0) return 0;
  // Higher prices at the top, which is how every depth chart is read.
  return Math.min(rows - 1, Math.floor(((window.high - price) / span) * rows));
}

/** A window move, quantised to whole rows of the offscreen canvas. */
export interface WindowShift {
  /** The window to use from now on. */
  window: PriceWindow;
  /**
   * Rows to blit the painted history by, positive meaning downward, or null
   * if the history cannot be reused and must be repainted.
   */
  dy: number | null;
}

/**
 * Quantises a window move so the painted history can be blitted rather than
 * repainted.
 *
 * `rowFor` is `(high - price) * rows / span`, so raising the window's top by
 * d ticks moves every price d * rows / span rows down the canvas. If that is
 * a whole number of rows the history is still correct after a blit by it --
 * which is what makes a moving window read as a scroll rather than a jump.
 *
 * It is generally *not* a whole number, so rather than round the blit (whose
 * error would accumulate over thousands of moves into a history painted at
 * the wrong price) this rounds the **window**, to the nearest whole row in
 * the direction of travel. The window is then a fraction of a tick off what
 * was asked for and every blit is exact.
 *
 * Returns dy = null when the span changed, because then the price-to-row
 * mapping itself changed and no blit can fix the history.
 */
export function shiftWindow(
  from: PriceWindow,
  to: PriceWindow,
  rows: number,
): WindowShift {
  const span = windowSpan(from);
  if (span <= 0 || Math.abs(windowSpan(to) - span) > 1e-6 || rows <= 0) {
    return { window: to, dy: null };
  }
  const exact = ((to.high - from.high) * rows) / span;
  // Round away from zero, so a move the window genuinely wants is never
  // quantised away to nothing and left pending forever.
  const dy = exact >= 0 ? Math.ceil(exact - 1e-9) : Math.floor(exact + 1e-9);
  if (dy === 0) return { window: from, dy: 0 };
  if (Math.abs(dy) >= rows) return { window: to, dy: null };
  const ticks = (dy * span) / rows;
  return { window: { low: from.low + ticks, high: from.high + ticks }, dy };
}
