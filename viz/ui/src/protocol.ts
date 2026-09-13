/**
 * The wire protocol, typed.
 *
 * These types are the TypeScript half of `viz/server/protocol.py`. They are
 * hand-written on both sides rather than generated, and kept honest by
 * `protocol.test.ts`, which parses the committed sample recording -- the same
 * file the mock server replays -- and asserts every frame in it satisfies
 * these types at runtime. Drift between the two halves does not throw; it
 * produces silently blank panels, which is why the check exists.
 */

export const SCHEMA_VERSION = 1;

/** `[price_ticks, qty, orders]`. Prices are integer ticks, never dollars. */
export type Level = [number, number, number];

export interface HelloFrame {
  v: number;
  type: 'hello';
  symbol: string;
  dataset: string;
  build_sha: string;
  tap: 'ON' | 'OFF';
  publish_hz: number;
}

export interface BookFrame {
  v: number;
  type: 'book';
  t_ns: number;
  symbol: string;
  seq: number;
  bids: Level[];
  asks: Level[];
  last_trade: { price: number; qty: number; side: 'B' | 'S' } | null;
}

export interface TradeItem {
  t_ns: number;
  price: number;
  qty: number;
  aggressor: 'B' | 'S';
  resting_id: number;
}

export interface TradesFrame {
  v: number;
  type: 'trades';
  items: TradeItem[];
}

export interface StatsFrame {
  v: number;
  type: 'stats';
  ops_per_sec: number;
  p50_ns: number;
  p99_ns: number;
  p999_ns: number;
  max_ns: number;
  orders_resting: number;
  ring_drops: number;
  schedule_misses: number;
  reader_lag: number;
}

export interface TransportFrame {
  v: number;
  type: 'transport';
  state: 'playing' | 'paused';
  speed: number;
  event_index: number;
  total_events: number;
}

export interface ErrorFrame {
  v: number;
  type: 'error';
  message: string;
}

export type Frame =
  | HelloFrame
  | BookFrame
  | TradesFrame
  | StatsFrame
  | TransportFrame
  | ErrorFrame;

/** Commands the client sends back. Mirrors `VALID_TRANSPORT_COMMANDS`. */
export type TransportCommand =
  | { action: 'play' }
  | { action: 'pause' }
  | { action: 'step'; count?: number }
  | { action: 'speed'; speed: number }
  | { action: 'seek'; event_index: number }
  | { action: 'reset' };

/**
 * Runtime validation.
 *
 * A frame that fails this is dropped and counted rather than rendered. The
 * alternative -- trusting the shape and letting a missing field surface as
 * `undefined` three layers down in a canvas draw call -- produces a blank
 * panel with no explanation, and a blank panel is indistinguishable from a
 * quiet market.
 */
export function parseFrame(raw: unknown): Frame | null {
  if (typeof raw !== 'object' || raw === null) return null;
  const f = raw as Record<string, unknown>;
  if (f.v !== SCHEMA_VERSION) return null;

  switch (f.type) {
    case 'hello':
      return isString(f.symbol) && isString(f.dataset) && isString(f.build_sha)
        ? (f as unknown as HelloFrame)
        : null;
    case 'book':
      return isNumber(f.t_ns) &&
        isNumber(f.seq) &&
        isLevelArray(f.bids) &&
        isLevelArray(f.asks)
        ? (f as unknown as BookFrame)
        : null;
    case 'trades':
      return Array.isArray(f.items) && f.items.every(isTradeItem)
        ? (f as unknown as TradesFrame)
        : null;
    case 'stats':
      return isNumber(f.ops_per_sec) && isNumber(f.p99_ns)
        ? (f as unknown as StatsFrame)
        : null;
    case 'transport':
      return (f.state === 'playing' || f.state === 'paused') &&
        isNumber(f.speed)
        ? (f as unknown as TransportFrame)
        : null;
    case 'error':
      return isString(f.message) ? (f as unknown as ErrorFrame) : null;
    default:
      return null;
  }
}

function isNumber(v: unknown): v is number {
  return typeof v === 'number' && Number.isFinite(v);
}
function isString(v: unknown): v is string {
  return typeof v === 'string';
}
function isLevelArray(v: unknown): v is Level[] {
  return (
    Array.isArray(v) &&
    v.every(
      (level) =>
        Array.isArray(level) && level.length === 3 && level.every(isNumber),
    )
  );
}
function isTradeItem(v: unknown): v is TradeItem {
  if (typeof v !== 'object' || v === null) return false;
  const t = v as Record<string, unknown>;
  return (
    isNumber(t.t_ns) &&
    isNumber(t.price) &&
    isNumber(t.qty) &&
    (t.aggressor === 'B' || t.aggressor === 'S')
  );
}

/** Ticks to a dollar string. Four implied decimals, no floating point drift. */
export function formatPrice(ticks: number): string {
  const negative = ticks < 0;
  const abs = Math.abs(ticks);
  const whole = Math.floor(abs / 10000);
  const frac = abs % 10000;
  return `${negative ? '-' : ''}${whole}.${frac.toString().padStart(4, '0')}`;
}

/** Nanoseconds since midnight to HH:MM:SS.mmm. */
export function formatTime(ns: number): string {
  const totalSeconds = Math.floor(ns / 1e9);
  const hh = Math.floor(totalSeconds / 3600);
  const mm = Math.floor((totalSeconds % 3600) / 60);
  const ss = totalSeconds % 60;
  const ms = Math.floor((ns % 1e9) / 1e6);
  return (
    `${pad(hh)}:${pad(mm)}:${pad(ss)}.` + ms.toString().padStart(3, '0')
  );
}

function pad(n: number): string {
  return n.toString().padStart(2, '0');
}

/** Compact counts: 6120000 -> "6.12M". */
export function formatCount(n: number): string {
  if (n >= 1e9) return `${(n / 1e9).toFixed(2)}B`;
  if (n >= 1e6) return `${(n / 1e6).toFixed(2)}M`;
  if (n >= 1e3) return `${(n / 1e3).toFixed(1)}k`;
  return n.toString();
}

/** Nanoseconds, rendered at a readable scale. */
export function formatNanos(ns: number): string {
  if (ns >= 1e9) return `${(ns / 1e9).toFixed(2)} s`;
  if (ns >= 1e6) return `${(ns / 1e6).toFixed(2)} ms`;
  if (ns >= 1e3) return `${(ns / 1e3).toFixed(2)} µs`;
  return `${ns} ns`;
}
