/**
 * The book heatmap: x is time, y is price, colour is resting quantity, with
 * trades over-plotted.
 *
 * The visually striking panel, and the one with the only real performance
 * constraint in the UI: 900 columns of 30-second history at up to 5000 price
 * rows. Redrawing that every frame would be 135 million cells a second.
 *
 * So: each publish tick draws **one new column** into an offscreen canvas,
 * and the offscreen canvas is scrolled left by one column. The visible canvas
 * is a single blit.
 *
 * The price window moves with the market, and moving it moves the painted
 * history with it: the same blit that scrolls left by a column scrolls
 * vertically by the rows the window moved, so history stays attached to price
 * without being repainted. The whole buffer is only re-rendered when the
 * *scale* changes -- a different number of ticks on the axis -- which the
 * deadband in `updateWindow` makes rare.
 */

import { useEffect, useRef } from 'react';

import { COLOR, heatColor, useSizedCanvas } from '../lib/canvas';
import {
  ColumnRing,
  rowFor,
  shiftWindow,
  updateWindow,
  windowSpan,
  type HeatColumn,
  type PriceWindow,
  type WindowInput,
} from '../lib/heatmap';
import { formatPrice, type BookFrame, type TradeItem } from '../protocol';
import { drawPlaceholder } from './DepthLadder';

const COLUMNS = 900; // 30 s at 30 Hz
const COLUMN_PX = 2;

interface Props {
  book: React.MutableRefObject<BookFrame | null>;
  trades: React.MutableRefObject<TradeItem[]>;
  revision: React.MutableRefObject<number>;
}

export function BookHeatmap({ book, trades, revision }: Props) {
  const ring = useRef(new ColumnRing(COLUMNS));
  const window_ = useRef<PriceWindow | null>(null);
  const lastRevision = useRef(-1);
  const lastTradeSeen = useRef(0);
  const offscreen = useRef<HTMLCanvasElement | null>(null);
  const offscreenRows = useRef(0);
  const needsFullRedraw = useRef(true);

  useEffect(() => {
    offscreen.current = document.createElement('canvas');
  }, []);

  const canvasRef = useSizedCanvas((ctx, width, height) => {
    ctx.fillStyle = COLOR.background;
    ctx.fillRect(0, 0, width, height);

    const frame = book.current;
    if (!frame) {
      drawPlaceholder(ctx, width, height, 'waiting for a book frame');
      return;
    }

    const rows = Math.max(8, Math.min(5000, Math.floor(height)));
    const off = offscreen.current;
    if (!off) return;
    if (off.width !== COLUMNS * COLUMN_PX || offscreenRows.current !== rows) {
      off.width = COLUMNS * COLUMN_PX;
      off.height = rows;
      offscreenRows.current = rows;
      needsFullRedraw.current = true;
    }
    const offCtx = off.getContext('2d');
    if (!offCtx) return;

    // A new publish tick: capture a column.
    if (revision.current !== lastRevision.current) {
      lastRevision.current = revision.current;

      // The window is sized from every level the server sent, not from the
      // touch: a window sized to the touch clips the depth behind it, and
      // `rowFor` drops a clipped level silently.
      const previous = window_.current;
      const wanted = updateWindow(previous, priceExtent(frame));
      let dy = 0;
      if (previous === null || wanted === null) {
        window_.current = wanted;
        needsFullRedraw.current = true;
      } else if (wanted !== previous) {
        const shift = shiftWindow(previous, wanted, rows);
        window_.current = shift.window;
        if (shift.dy === null) needsFullRedraw.current = true;
        else dy = shift.dy;
      }

      // Trades that arrived since the previous column, over-plotted on it.
      const fresh: HeatColumn['trades'] = [];
      for (const trade of trades.current) {
        if (trade.t_ns <= lastTradeSeen.current) break;
        fresh.push({ price: trade.price, qty: trade.qty, side: trade.aggressor });
      }
      if (trades.current[0]) lastTradeSeen.current = trades.current[0].t_ns;

      ring.current.push({
        tNs: frame.t_ns,
        bids: new Map(frame.bids.map((l) => [l[0], l[1]])),
        asks: new Map(frame.asks.map((l) => [l[0], l[1]])),
        trades: fresh,
      });

      if (needsFullRedraw.current) {
        redrawAll(offCtx, off, ring.current, window_.current, rows);
        needsFullRedraw.current = false;
      } else {
        // Scroll left by one column -- and down by however far the price
        // window moved -- then draw only the new column. One drawImage does
        // both; this is the whole performance argument for the panel.
        offCtx.globalCompositeOperation = 'copy';
        offCtx.drawImage(off, -COLUMN_PX, dy);
        offCtx.globalCompositeOperation = 'source-over';
        const newest = ring.current.newest();
        if (newest && window_.current) {
          drawColumn(
            offCtx,
            newest,
            off.width - COLUMN_PX,
            window_.current,
            rows,
          );
        }
      }
    }

    // One blit.
    ctx.imageSmoothingEnabled = false;
    ctx.drawImage(off, 0, 0, off.width, off.height, 0, 0, width, height);

    drawAxis(ctx, width, height, window_.current, ring.current.size);
  }, []);

  return <canvas ref={canvasRef} className="block h-full w-full" />;
}

/** The full price range the server sent, which is what must stay visible. */
function priceExtent(frame: BookFrame): WindowInput | null {
  let lowPrice = Infinity;
  let highPrice = -Infinity;
  for (const [price] of frame.bids) {
    if (price < lowPrice) lowPrice = price;
    if (price > highPrice) highPrice = price;
  }
  for (const [price] of frame.asks) {
    if (price < lowPrice) lowPrice = price;
    if (price > highPrice) highPrice = price;
  }
  if (!Number.isFinite(lowPrice)) return null;
  return { lowPrice, highPrice };
}

function redrawAll(
  ctx: CanvasRenderingContext2D,
  canvas: HTMLCanvasElement,
  ring: ColumnRing,
  window_: PriceWindow | null,
  rows: number,
) {
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  if (!window_) return;
  const columns = ring.toArray();
  const firstX = canvas.width - columns.length * COLUMN_PX;
  columns.forEach((column, i) => {
    drawColumn(ctx, column, firstX + i * COLUMN_PX, window_, rows);
  });
}

function drawColumn(
  ctx: CanvasRenderingContext2D,
  column: HeatColumn,
  x: number,
  window_: PriceWindow,
  rows: number,
) {
  ctx.clearRect(x, 0, COLUMN_PX, rows);

  // One tick tall, so a level occupies exactly the price it is at. This used
  // to be rows/120 from the days of a fixed 120-tick span; with the span now
  // sized to the book, that constant would draw a level 64 ticks tall on a
  // wide window.
  const tickPx = Math.max(1, Math.floor(rows / Math.max(1, windowSpan(window_))));

  let max = 1;
  for (const qty of column.bids.values()) max = Math.max(max, qty);
  for (const qty of column.asks.values()) max = Math.max(max, qty);

  const paint = (map: Map<number, number>, side: 'bid' | 'ask') => {
    for (const [price, qty] of map) {
      const row = rowFor(price, window_, rows);
      if (row < 0) continue;
      ctx.fillStyle = heatColor(qty, max, side);
      ctx.fillRect(x, row, COLUMN_PX, tickPx);
    }
  };
  paint(column.bids, 'bid');
  paint(column.asks, 'ask');

  // Trades, sized by quantity, coloured by aggressor side -- and drawn as a
  // distinct *shape* rather than only a distinct colour, so they are visible
  // against either half of the book.
  for (const trade of column.trades) {
    const row = rowFor(trade.price, window_, rows);
    if (row < 0) continue;
    const radius = Math.min(4, 1 + Math.log10(Math.max(1, trade.qty)));
    ctx.beginPath();
    ctx.arc(x + COLUMN_PX / 2, row, radius, 0, Math.PI * 2);
    ctx.fillStyle = trade.side === 'B' ? '#7be3b0' : '#ff9a92';
    ctx.fill();
  }
}

function drawAxis(
  ctx: CanvasRenderingContext2D,
  width: number,
  height: number,
  window_: PriceWindow | null,
  columns: number,
) {
  ctx.font = '10px ui-monospace, Menlo, monospace';
  ctx.fillStyle = COLOR.muted;
  ctx.textBaseline = 'top';
  ctx.textAlign = 'left';
  if (window_) {
    // The bounds land on fractions of a tick once the window has been
    // quantised to whole rows; formatPrice wants an integer.
    ctx.fillText(formatPrice(Math.round(window_.high)), 4, 3);
    ctx.textBaseline = 'bottom';
    ctx.fillText(formatPrice(Math.round(window_.low)), 4, height - 3);
    // The scale is not fixed, so say what it currently is.
    ctx.textAlign = 'right';
    ctx.textBaseline = 'top';
    ctx.fillText(`${Math.round(windowSpan(window_))} ticks`, width - 4, 3);
  }
  ctx.textBaseline = 'bottom';
  ctx.textAlign = 'right';
  ctx.fillText(`${columns} cols · ${(columns / 30).toFixed(0)}s`, width - 4, height - 3);
}
