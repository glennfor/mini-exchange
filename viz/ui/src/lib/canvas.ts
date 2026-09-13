/**
 * Canvas plumbing shared by the panels.
 *
 * Every panel here draws on a canvas rather than in the DOM. At 30 Hz with
 * forty visible ladder rows, React reconciliation is measurable and the
 * flicker is visible; a canvas redraw of the same content is a few hundred
 * microseconds and never touches the layout engine.
 */

import { useEffect, useRef } from 'react';

/**
 * Sizes a canvas to its container in device pixels and keeps it sized.
 *
 * Without the devicePixelRatio scaling, every line on a retina display is
 * drawn at half resolution and the text is soft -- which on a panel made
 * entirely of small numbers is the difference between readable and not.
 */
export function useSizedCanvas(
  draw: (ctx: CanvasRenderingContext2D, width: number, height: number) => void,
  deps: unknown[] = [],
) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const drawRef = useRef(draw);
  drawRef.current = draw;

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const parent = canvas.parentElement;
    if (!parent) return;

    let frame = 0;
    const render = () => {
      const ctx = canvas.getContext('2d');
      if (!ctx) return;
      const dpr = window.devicePixelRatio || 1;
      const width = parent.clientWidth;
      const height = parent.clientHeight;
      if (canvas.width !== width * dpr || canvas.height !== height * dpr) {
        canvas.width = Math.max(1, Math.floor(width * dpr));
        canvas.height = Math.max(1, Math.floor(height * dpr));
        canvas.style.width = `${width}px`;
        canvas.style.height = `${height}px`;
      }
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      drawRef.current(ctx, width, height);
      frame = requestAnimationFrame(render);
    };
    frame = requestAnimationFrame(render);
    return () => cancelAnimationFrame(frame);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps);

  return canvasRef;
}

/** Colours, in one place so the panels agree. */
export const COLOR = {
  background: '#0e1013',
  grid: '#1e222a',
  text: '#d1d5db',
  muted: '#6b7280',
  bid: '#2f9e6e',
  bidDim: 'rgba(47, 158, 110, 0.28)',
  ask: '#c9524a',
  askDim: 'rgba(201, 82, 74, 0.28)',
  accent: '#d9a441',
  flash: 'rgba(217, 164, 65, 0.55)',
} as const;

/**
 * A log-scaled colour ramp for the heatmap.
 *
 * Log, not linear: resting quantity at a price level spans three or four
 * orders of magnitude across a book, and a linear ramp renders everything
 * except the one deepest level as the same near-black.
 */
export function heatColor(qty: number, max: number, side: 'bid' | 'ask'): string {
  if (qty <= 0 || max <= 0) return 'rgba(0,0,0,0)';
  const t = Math.log1p(qty) / Math.log1p(max);
  const alpha = 0.12 + 0.78 * Math.min(1, Math.max(0, t));
  return side === 'bid'
    ? `rgba(47, 158, 110, ${alpha.toFixed(3)})`
    : `rgba(201, 82, 74, ${alpha.toFixed(3)})`;
}
