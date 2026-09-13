/**
 * The depth ladder: one row per tick, centred on the touch.
 *
 * Canvas, not DOM. At 30 Hz with forty visible rows, React reconciliation is
 * measurable and the flicker is visible.
 *
 * Colour is never the only signal: bids and asks differ by *column position*
 * and by an explicit label as well as by hue, so the panel is readable
 * without colour vision.
 */

import { useRef } from 'react';

import { COLOR, useSizedCanvas } from '../lib/canvas';
import { formatPrice, type BookFrame } from '../protocol';

const ROW_HEIGHT = 15;
const FLASH_MS = 300;

interface Props {
  book: React.MutableRefObject<BookFrame | null>;
}

export function DepthLadder({ book }: Props) {
  // Last-seen quantity per price, so a change can flash. Kept in a ref: it is
  // per-frame state that no React render needs to see.
  const lastQty = useRef(new Map<number, number>());
  const flashAt = useRef(new Map<number, number>());

  const canvasRef = useSizedCanvas((ctx, width, height) => {
    ctx.fillStyle = COLOR.background;
    ctx.fillRect(0, 0, width, height);

    const frame = book.current;
    if (!frame) {
      drawPlaceholder(ctx, width, height, 'waiting for a book frame');
      return;
    }

    const rows = Math.max(4, Math.floor(height / ROW_HEIGHT) - 1);
    const perSide = Math.floor(rows / 2);
    const bids = frame.bids.slice(0, perSide);
    const asks = frame.asks.slice(0, perSide);

    const maxQty = Math.max(
      1,
      ...bids.map((l) => l[1]),
      ...asks.map((l) => l[1]),
    );

    const now = performance.now();
    const note = (price: number, qty: number) => {
      const previous = lastQty.current.get(price);
      if (previous !== undefined && previous !== qty) {
        flashAt.current.set(price, now);
      }
      lastQty.current.set(price, qty);
    };
    for (const [price, qty] of bids) note(price, qty);
    for (const [price, qty] of asks) note(price, qty);

    const midX = width * 0.5;
    const barMax = width * 0.34;

    ctx.font = '11px ui-monospace, Menlo, monospace';
    ctx.textBaseline = 'middle';

    // Header. The labels are what make the two sides distinguishable without
    // relying on hue.
    ctx.fillStyle = COLOR.muted;
    ctx.textAlign = 'left';
    ctx.fillText('BID qty', 6, 10);
    ctx.textAlign = 'center';
    ctx.fillText('price', midX, 10);
    ctx.textAlign = 'right';
    ctx.fillText('ASK qty', width - 6, 10);

    // Asks descend from the top so the touch is in the middle, the way a
    // ladder is read.
    let y = 18 + ROW_HEIGHT * (perSide - 1);
    for (let i = 0; i < asks.length; i++) {
      const level = asks[i];
      if (!level) continue;
      drawRow(ctx, {
        y: y - i * ROW_HEIGHT,
        width,
        midX,
        barMax,
        level,
        maxQty,
        side: 'ask',
        isTouch: i === 0,
        flashing: isFlashing(flashAt.current.get(level[0]), now),
      });
    }

    y = 18 + ROW_HEIGHT * perSide + 2;
    for (let i = 0; i < bids.length; i++) {
      const level = bids[i];
      if (!level) continue;
      drawRow(ctx, {
        y: y + i * ROW_HEIGHT,
        width,
        midX,
        barMax,
        level,
        maxQty,
        side: 'bid',
        isTouch: i === 0,
        flashing: isFlashing(flashAt.current.get(level[0]), now),
      });
    }

    // The spread, between the two halves.
    const bestBid = bids[0]?.[0];
    const bestAsk = asks[0]?.[0];
    if (bestBid !== undefined && bestAsk !== undefined) {
      const spreadY = 18 + ROW_HEIGHT * perSide - 5;
      ctx.fillStyle = COLOR.grid;
      ctx.fillRect(0, spreadY - 6, width, 12);
      ctx.fillStyle = COLOR.accent;
      ctx.textAlign = 'center';
      ctx.fillText(`spread ${bestAsk - bestBid}`, midX, spreadY);
    }
  }, []);

  return <canvas ref={canvasRef} className="block h-full w-full" />;
}

function isFlashing(at: number | undefined, now: number): number {
  if (at === undefined) return 0;
  const age = now - at;
  // Decays over ~300 ms, so a change is caught out of the corner of an eye
  // but a busy level does not strobe.
  return age < FLASH_MS ? 1 - age / FLASH_MS : 0;
}

interface RowArgs {
  y: number;
  width: number;
  midX: number;
  barMax: number;
  level: [number, number, number];
  maxQty: number;
  side: 'bid' | 'ask';
  isTouch: boolean;
  flashing: number;
}

function drawRow(ctx: CanvasRenderingContext2D, a: RowArgs) {
  const [price, qty, orders] = a.level;
  const barWidth = (qty / a.maxQty) * a.barMax;
  const isBid = a.side === 'bid';

  if (a.isTouch) {
    ctx.fillStyle = 'rgba(217, 164, 65, 0.10)';
    ctx.fillRect(0, a.y - 7, a.width, ROW_HEIGHT - 1);
  }
  if (a.flashing > 0) {
    ctx.fillStyle = `rgba(217, 164, 65, ${(a.flashing * 0.35).toFixed(3)})`;
    ctx.fillRect(0, a.y - 7, a.width, ROW_HEIGHT - 1);
  }

  ctx.fillStyle = isBid ? COLOR.bidDim : COLOR.askDim;
  if (isBid) {
    ctx.fillRect(a.midX - 42 - barWidth, a.y - 6, barWidth, ROW_HEIGHT - 3);
  } else {
    ctx.fillRect(a.midX + 42, a.y - 6, barWidth, ROW_HEIGHT - 3);
  }

  ctx.fillStyle = isBid ? COLOR.bid : COLOR.ask;
  if (isBid) {
    ctx.textAlign = 'left';
    ctx.fillText(`${qty}`, 6, a.y);
    ctx.fillStyle = COLOR.muted;
    ctx.fillText(`(${orders})`, 60, a.y);
  } else {
    ctx.textAlign = 'right';
    ctx.fillText(`${qty}`, a.width - 6, a.y);
    ctx.fillStyle = COLOR.muted;
    ctx.fillText(`(${orders})`, a.width - 60, a.y);
  }

  ctx.fillStyle = a.isTouch ? COLOR.accent : COLOR.text;
  ctx.textAlign = 'center';
  ctx.fillText(formatPrice(price), a.midX, a.y);
}

export function drawPlaceholder(
  ctx: CanvasRenderingContext2D,
  width: number,
  height: number,
  message: string,
) {
  ctx.fillStyle = COLOR.muted;
  ctx.font = '11px ui-monospace, Menlo, monospace';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(message, width / 2, height / 2);
}
