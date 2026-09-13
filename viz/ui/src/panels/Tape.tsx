/**
 * The trade tape: newest at the top, capped at 500 rendered rows.
 *
 * Virtualised, because the tape is the one panel whose row count grows without
 * anyone asking. Only the rows that fit are in the DOM; the rest are a spacer.
 *
 * The aggressor side is shown by a *letter* as well as a colour -- `B` or `S`
 * in its own column -- so the panel is readable without colour vision, which
 * is the rule for every panel here.
 *
 * At full rate the engine prints far more than 500 trades a second, so the
 * server caps what it sends and says how many it dropped. That count is in
 * the header: this is a sample of the tape, and a tape that does not admit
 * that is one you cannot trust for the prints it does show.
 */

import { useCallback, useEffect, useRef, useState } from 'react';

import { formatCount, formatPrice, formatTime, type TradeItem } from '../protocol';

const ROW_HEIGHT = 16;
const OVERSCAN = 6;
const MAX_ROWS = 500;

interface Props {
  trades: React.MutableRefObject<TradeItem[]>;
  elided: React.MutableRefObject<number>;
  revision: React.MutableRefObject<number>;
}

export function Tape({ trades, elided, revision }: Props) {
  const containerRef = useRef<HTMLDivElement | null>(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [viewportHeight, setViewportHeight] = useState(0);
  const [, forceRender] = useState(0);
  const lastRevision = useRef(-1);

  // The tape data lives in a ref written at 30 Hz. Rather than re-rendering
  // on every frame, poll the revision counter on an animation frame and
  // re-render only when it moved -- which collapses a burst of frames into
  // one render.
  useEffect(() => {
    let handle = 0;
    const tick = () => {
      if (revision.current !== lastRevision.current) {
        lastRevision.current = revision.current;
        forceRender((n) => n + 1);
      }
      handle = requestAnimationFrame(tick);
    };
    handle = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(handle);
  }, [revision]);

  useEffect(() => {
    const element = containerRef.current;
    if (!element) return;
    const observer = new ResizeObserver(() => {
      setViewportHeight(element.clientHeight);
    });
    observer.observe(element);
    setViewportHeight(element.clientHeight);
    return () => observer.disconnect();
  }, []);

  const onScroll = useCallback(() => {
    setScrollTop(containerRef.current?.scrollTop ?? 0);
  }, []);

  const rows = trades.current.slice(0, MAX_ROWS);
  const first = Math.max(0, Math.floor(scrollTop / ROW_HEIGHT) - OVERSCAN);
  const visible = Math.ceil(viewportHeight / ROW_HEIGHT) + OVERSCAN * 2;
  const slice = rows.slice(first, first + visible);

  return (
    <div className="flex h-full min-h-0 flex-col">
      {elided.current > 0 && (
        <div className="shrink-0 truncate px-2 py-[1px] text-[9px] text-accent">
          sample &middot; {formatCount(elided.current)} prints not shown
        </div>
      )}
      <div className="grid shrink-0 grid-cols-[86px_1fr_70px_28px] gap-2 border-b border-ink-600 px-2 py-[2px] text-[9px] uppercase tracking-widest text-muted">
        <span>time</span>
        <span className="text-right">price</span>
        <span className="text-right">size</span>
        <span className="text-center">agg</span>
      </div>
      <div
        ref={containerRef}
        onScroll={onScroll}
        className="min-h-0 flex-1 overflow-y-auto"
      >
        <div style={{ height: rows.length * ROW_HEIGHT, position: 'relative' }}>
          <div
            style={{
              transform: `translateY(${first * ROW_HEIGHT}px)`,
              position: 'absolute',
              left: 0,
              right: 0,
            }}
          >
            {slice.map((trade, i) => (
              <div
                key={`${trade.t_ns}-${trade.resting_id}-${first + i}`}
                className="grid grid-cols-[86px_1fr_70px_28px] gap-2 px-2 text-[11px] leading-4"
                style={{ height: ROW_HEIGHT }}
              >
                <span className="numeral text-muted">
                  {formatTime(trade.t_ns)}
                </span>
                <span
                  className={`numeral text-right ${
                    trade.aggressor === 'B' ? 'text-bid' : 'text-ask'
                  }`}
                >
                  {formatPrice(trade.price)}
                </span>
                <span className="numeral text-right text-gray-300">
                  {trade.qty}
                </span>
                <span
                  className={`text-center ${
                    trade.aggressor === 'B' ? 'text-bid' : 'text-ask'
                  }`}
                >
                  {trade.aggressor}
                </span>
              </div>
            ))}
          </div>
        </div>
        {rows.length === 0 && (
          <div className="p-3 text-center text-xs text-muted">
            no trades yet
          </div>
        )}
      </div>
    </div>
  );
}
