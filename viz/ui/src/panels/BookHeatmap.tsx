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
 * is a single blit out of it.
 *
 * The offscreen canvas is three window-heights tall and is **never scrolled
 * vertically**. The price window tracks the market by taking a different
 * slice of it, which destroys nothing, so history that has drifted off the
 * top or bottom of the frame is still painted and comes back intact when the
 * market does. When the window reaches the edge of that range the buffer is
 * re-placed and repainted from the ring -- the actual store -- which is the
 * only operation that can restore history the buffer never held.
 *
 * See `lib/heatmap.ts` for why a vertically-scrolled buffer cannot work: it
 * destroys what it pushes past an edge, and the price axis, unlike time,
 * moves both ways.
 */

import { useEffect, useRef } from 'react';

import { COLOR, heatColor, useSizedCanvas } from '../lib/canvas';
import {
  BUFFER_FACTOR,
  ColumnRing,
  bufferWindowFor,
  makeColumn,
  placeView,
  rowFor,
  updateWindow,
  windowSpan,
  type HeatColumn,
  type PriceWindow,
} from '../lib/heatmap';
import { formatPrice, type BookFrame, type TradeItem } from '../protocol';
import { drawPlaceholder } from './DepthLadder';

const COLUMNS = 900; // 30 s at 30 Hz
const COLUMN_PX = 2;

/**
 * Ceiling on the offscreen canvas, in pixels.
 *
 * Six million pixels is 24 MB at four bytes each. Bounding the area rather
 * than the height keeps the figure the same whatever the panel's shape: the
 * retained-history factor is reduced instead, so a very tall panel repaints
 * more often rather than asking the browser for a canvas it may refuse.
 */
const MAX_BUFFER_PX = 6_000_000;

interface Props {
  book: React.MutableRefObject<BookFrame | null>;
  trades: React.MutableRefObject<TradeItem[]>;
  revision: React.MutableRefObject<number>;
}

export function BookHeatmap({ book, trades, revision }: Props) {
  const ring = useRef(new ColumnRing(COLUMNS));
  /** What the panel shows. Tight on the book, so the market fills the panel. */
  const view = useRef<PriceWindow | null>(null);
  /** What the offscreen canvas covers. Several times taller, same scale. */
  const buffer = useRef<PriceWindow | null>(null);
  const lastRevision = useRef(-1);
  const lastTradeSeen = useRef(0);
  const offscreen = useRef<HTMLCanvasElement | null>(null);
  const offscreenRows = useRef(0);
  const viewRows = useRef(0);
  /** First buffer row the window shows. */
  const viewOffset = useRef(0);

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
    const factor = Math.max(
      1,
      Math.min(
        BUFFER_FACTOR,
        Math.floor(MAX_BUFFER_PX / (COLUMNS * COLUMN_PX * rows)),
      ),
    );
    const off = offscreen.current;
    if (!off) return;
    const offCtx = off.getContext('2d');
    if (!offCtx) return;

    // A new publish tick: capture a column. Everything below the blit is
    // skipped when nothing new has arrived -- the panel then just re-blits
    // what it already has.
    if (
      revision.current !== lastRevision.current &&
      frame.bids.length + frame.asks.length > 0
    ) {
      lastRevision.current = revision.current;

      // Trades that arrived since the previous column, over-plotted on it.
      const fresh: HeatColumn['trades'] = [];
      for (const trade of trades.current) {
        if (trade.t_ns <= lastTradeSeen.current) break;
        fresh.push({
          price: trade.price,
          qty: trade.qty,
          side: trade.aggressor,
        });
      }
      if (trades.current[0]) lastTradeSeen.current = trades.current[0].t_ns;

      const column = makeColumn(
        frame.t_ns,
        new Map(frame.bids.map((l) => [l[0], l[1]])),
        new Map(frame.asks.map((l) => [l[0], l[1]])),
        fresh,
      );
      ring.current.push(column);

      // The window follows this column only -- the market, not the history
      // around it -- so the book keeps the height of the panel.
      const wanted =
        column.lo === null || column.hi === null
          ? view.current
          : updateWindow(view.current, {
              lowPrice: column.lo,
              highPrice: column.hi,
            });
      if (wanted !== null) {
        const rescaled =
          view.current === null ||
          windowSpan(view.current) !== windowSpan(wanted);
        const resized =
          off.width !== COLUMNS * COLUMN_PX ||
          viewRows.current !== rows ||
          offscreenRows.current !== rows * factor;

        // Does the window still fit the buffer we have? A miss is not a loss:
        // the ring holds everything, and re-placing the buffer repaints from
        // it. It is only the cache running out.
        const rowsPerTick = rows / windowSpan(wanted);
        let placed =
          rescaled || resized || buffer.current === null
            ? null
            : placeView(
                buffer.current,
                wanted,
                rowsPerTick,
                offscreenRows.current,
                rows,
              );

        if (placed === null) {
          if (
            off.width !== COLUMNS * COLUMN_PX ||
            off.height !== rows * factor
          ) {
            off.width = COLUMNS * COLUMN_PX;
            off.height = rows * factor;
          }
          offscreenRows.current = rows * factor;
          viewRows.current = rows;
          buffer.current = bufferWindowFor(
            wanted,
            ring.current.extent(),
            factor,
          );
          placed = placeView(
            buffer.current,
            wanted,
            rowsPerTick,
            offscreenRows.current,
            rows,
          );
          if (placed === null) return; // cannot happen; bufferWindowFor fits it
          view.current = placed.view;
          redrawAll(
            offCtx,
            off,
            ring.current,
            buffer.current,
            offscreenRows.current,
          );
        } else {
          view.current = placed.view;
          // Scroll left by one column and draw only the new one, at its price
          // in the *buffer*. No vertical component: that is the whole point.
          offCtx.globalCompositeOperation = 'copy';
          offCtx.drawImage(off, -COLUMN_PX, 0);
          offCtx.globalCompositeOperation = 'source-over';
          drawColumn(
            offCtx,
            column,
            off.width - COLUMN_PX,
            buffer.current!,
            offscreenRows.current,
          );
        }
        viewOffset.current = placed.sy;
      }
    }

    // One blit: the slice of the buffer the window is looking at.
    ctx.imageSmoothingEnabled = false;
    if (buffer.current) {
      ctx.drawImage(
        off,
        0,
        viewOffset.current,
        off.width,
        viewRows.current,
        0,
        0,
        width,
        height,
      );
    }

    drawAxis(ctx, width, height, view.current, ring.current.size);
  }, []);

  return <canvas ref={canvasRef} className="block h-full w-full" />;
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
  const tickPx = Math.max(
    1,
    Math.floor(rows / Math.max(1, windowSpan(window_))),
  );

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
  ctx.fillText(
    `${columns} cols · ${(columns / 30).toFixed(0)}s`,
    width - 4,
    height - 3,
  );
}
