/**
 * The TypeScript and Python halves of the wire protocol must agree.
 *
 * This parses the committed sample recording -- byte for byte the file the
 * mock server replays -- and asserts every frame in it satisfies the types in
 * `protocol.ts`. That is the drift check: a field the server renamed, or a
 * type it changed, shows up here as a failing test rather than as a silently
 * blank panel in the browser.
 */

import { gunzipSync } from 'node:zlib';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import {
  formatCount,
  formatNanos,
  formatPrice,
  formatTime,
  parseFrame,
  SCHEMA_VERSION,
  type BookFrame,
  type Frame,
  type StatsFrame,
} from './protocol';

// Resolved from the project root rather than from import.meta.url: under
// jsdom that URL has an http scheme and readFileSync refuses it.
const SAMPLE = resolve(process.cwd(), '../server/sample_frames.jsonl.gz');

function loadSample(): unknown[] {
  const text = gunzipSync(readFileSync(SAMPLE)).toString('utf8');
  return text
    .split('\n')
    .filter((line) => line.trim().length > 0)
    .map((line) => JSON.parse(line) as unknown);
}

describe('the committed sample recording', () => {
  const raw = loadSample();

  it('is not empty', () => {
    expect(raw.length).toBeGreaterThan(100);
  });

  it('parses entirely -- every frame satisfies the TypeScript types', () => {
    const rejected: unknown[] = [];
    const frames: Frame[] = [];
    for (const line of raw) {
      const frame = parseFrame(line);
      if (frame === null) rejected.push(line);
      else frames.push(frame);
    }
    expect(rejected).toEqual([]);
    expect(frames.length).toBe(raw.length);
  });

  it('contains every frame type the UI renders', () => {
    const kinds = new Set(raw.map((f) => (f as { type: string }).type));
    for (const kind of ['hello', 'book', 'trades', 'stats', 'transport']) {
      expect(kinds).toContain(kind);
    }
  });

  it('has books that are ordered and never crossed', () => {
    const books = raw
      .map(parseFrame)
      .filter((f): f is BookFrame => f?.type === 'book');
    expect(books.length).toBeGreaterThan(100);

    for (const book of books) {
      const bidPrices = book.bids.map((l) => l[0]);
      const askPrices = book.asks.map((l) => l[0]);
      expect(bidPrices).toEqual([...bidPrices].sort((a, b) => b - a));
      expect(askPrices).toEqual([...askPrices].sort((a, b) => a - b));
      if (bidPrices.length > 0 && askPrices.length > 0) {
        expect(bidPrices[0]!).toBeLessThan(askPrices[0]!);
      }
    }
  });

  it('has a latency distribution with an actual tail', () => {
    // A UI drawn against a flat distribution has a useless x axis, so the
    // fixture must have the shape a real one has.
    const stats = raw
      .map(parseFrame)
      .filter((f): f is StatsFrame => f?.type === 'stats');
    expect(stats.length).toBeGreaterThan(5);
    for (const s of stats) {
      expect(s.p50_ns).toBeLessThan(s.p99_ns);
      expect(s.p99_ns).toBeLessThan(s.p999_ns);
      expect(s.p999_ns).toBeLessThan(s.max_ns);
    }
  });
});

describe('parseFrame', () => {
  it('rejects a frame from another schema version', () => {
    expect(
      parseFrame({ v: SCHEMA_VERSION + 1, type: 'stats', ops_per_sec: 1 }),
    ).toBeNull();
  });

  it('rejects a book frame with a malformed level', () => {
    const bad = {
      v: SCHEMA_VERSION,
      type: 'book',
      t_ns: 1,
      symbol: 'X',
      seq: 1,
      bids: [[1, 2]], // three numbers required
      asks: [],
      last_trade: null,
    };
    expect(parseFrame(bad)).toBeNull();
  });

  it('rejects non-objects and unknown types', () => {
    expect(parseFrame(null)).toBeNull();
    expect(parseFrame(42)).toBeNull();
    expect(parseFrame({ v: SCHEMA_VERSION, type: 'frobnicate' })).toBeNull();
  });
});

describe('formatting', () => {
  it('renders integer ticks as dollars with four decimals', () => {
    expect(formatPrice(1002100)).toBe('100.2100');
    expect(formatPrice(1)).toBe('0.0001');
    expect(formatPrice(0)).toBe('0.0000');
    expect(formatPrice(-1002100)).toBe('-100.2100');
    expect(formatPrice(2238100)).toBe('223.8100');
  });

  it('renders nanoseconds since midnight as a wall clock', () => {
    expect(formatTime(34200123456789)).toBe('09:30:00.123');
    expect(formatTime(0)).toBe('00:00:00.000');
    expect(formatTime(57600000000000)).toBe('16:00:00.000');
  });

  it('renders counts and durations at a readable scale', () => {
    expect(formatCount(6120000)).toBe('6.12M');
    expect(formatCount(1500)).toBe('1.5k');
    expect(formatCount(42)).toBe('42');
    expect(formatNanos(118)).toBe('118 ns');
    expect(formatNanos(14200)).toBe('14.20 µs');
    expect(formatNanos(1_500_000)).toBe('1.50 ms');
  });
});

describe('the stats validator', () => {
  const complete = {
    v: SCHEMA_VERSION,
    type: 'stats',
    ops_per_sec: 1,
    p50_ns: 2,
    p99_ns: 3,
    p999_ns: 4,
    max_ns: 5,
    orders_resting: 6,
    ring_drops: 7,
    schedule_misses: 8,
    reader_lag: 9,
    reader_skipped: 10,
  };

  it('accepts a complete frame', () => {
    expect(parseFrame(complete)).not.toBeNull();
  });

  it('rejects a frame missing ANY field, not just the two it used to check', () => {
    // It checked ops_per_sec and p99_ns only. A frame missing
    // reader_skipped passed validation and then crashed a render on
    // undefined.toString(). A validator that checks a subset gives the
    // impression of a guarantee while providing none.
    for (const field of Object.keys(complete)) {
      if (field === 'v' || field === 'type') continue;
      const partial: Record<string, unknown> = { ...complete };
      delete partial[field];
      expect(parseFrame(partial)).toBeNull();
    }
  });

  it('rejects non-numeric values', () => {
    expect(parseFrame({ ...complete, ring_drops: 'lots' })).toBeNull();
    expect(parseFrame({ ...complete, p99_ns: null })).toBeNull();
    expect(parseFrame({ ...complete, max_ns: Number.NaN })).toBeNull();
  });
});
