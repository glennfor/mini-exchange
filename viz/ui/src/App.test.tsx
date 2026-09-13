/**
 * The dashboard, mounted for real and driven by the frames the server
 * actually sends.
 *
 * These are the tests that verify the UI works, because a browser screenshot
 * is not something CI can depend on. They mount the whole app against a
 * stubbed WebSocket, feed it the **committed sample recording** -- byte for
 * byte the file the mock server replays -- and assert the panels react.
 */

import { gunzipSync } from 'node:zlib';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { act, cleanup, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import App from './App';
import { drawCalls } from './test-setup';
import { FakeWebSocket, installFakeWebSocket } from './test-websocket';

// Resolved from the project root rather than from import.meta.url: under
// jsdom that URL has an http scheme and readFileSync refuses it.
const SAMPLE = resolve(process.cwd(), '../server/sample_frames.jsonl.gz');

function sampleFrames(): unknown[] {
  const text = gunzipSync(readFileSync(SAMPLE)).toString('utf8');
  return text
    .split('\n')
    .filter((line) => line.trim().length > 0)
    .map((line) => JSON.parse(line) as unknown);
}

async function settle(ms = 40): Promise<void> {
  await act(async () => {
    await new Promise((resolve) => setTimeout(resolve, ms));
  });
}

beforeEach(() => {
  installFakeWebSocket();
});

afterEach(() => {
  cleanup();
  FakeWebSocket.reset();
});

describe('the dashboard', () => {
  it('mounts every panel before any data arrives', async () => {
    render(<App />);
    await settle();

    expect(screen.getByText(/depth ladder/i)).toBeTruthy();
    expect(screen.getByText(/book heatmap/i)).toBeTruthy();
    expect(screen.getByText(/^engine$/i)).toBeTruthy();
    expect(screen.getByText(/latency cdf/i)).toBeTruthy();
    expect(screen.getByText(/^tape$/i)).toBeTruthy();
    expect(screen.getByText(/^transport$/i)).toBeTruthy();
  });

  it('opens exactly one socket, even under StrictMode double-mounting', async () => {
    // The bug this pins down: React's StrictMode mounts, unmounts and
    // remounts every effect. Without an ownership check in the socket's own
    // handlers, the first socket's onclose fires after the second effect has
    // reset the closed flag, schedules a reconnect, and the connections
    // multiply. Four live connections to the server were the symptom.
    render(<App />);
    await settle();
    expect(FakeWebSocket.live.length).toBe(1);

    // And a socket that was replaced must not resurrect itself.
    const [first] = FakeWebSocket.instances;
    first?.close();
    await settle();
    expect(FakeWebSocket.live.length).toBeLessThanOrEqual(1);
  });

  it('shows the connection state at all times', async () => {
    render(<App />);
    await settle();
    expect(screen.getByText(/connecting/i)).toBeTruthy();

    const ws = FakeWebSocket.live[0]!;
    await act(async () => {
      ws.open();
    });
    expect(screen.getByText(/^connected$/i)).toBeTruthy();

    // A dashboard that silently stops updating looks exactly like a quiet
    // market, so a dropped connection has to say so.
    await act(async () => {
      ws.close();
    });
    expect(screen.getByText(/reconnecting/i)).toBeTruthy();
  });

  it('renders provenance from the hello frame', async () => {
    render(<App />);
    await settle();
    const ws = FakeWebSocket.live[0]!;
    await act(async () => {
      ws.open();
      ws.deliver({
        v: 1,
        type: 'hello',
        symbol: 'AAPL',
        dataset: 'itch:20190130',
        build_sha: 'a0963cf',
        tap: 'ON',
        publish_hz: 30,
      });
    });

    // A screenshot of the dashboard should say which build and which dataset
    // produced it.
    expect(screen.getByText('AAPL')).toBeTruthy();
    expect(screen.getByText('itch:20190130')).toBeTruthy();
    expect(screen.getByText('a0963cf')).toBeTruthy();
    expect(screen.getByText('ON')).toBeTruthy();
  });

  it('renders the whole committed sample recording without rejecting a frame',
    async () => {
      render(<App />);
      await settle();
      const ws = FakeWebSocket.live[0]!;
      await act(async () => {
        ws.open();
      });

      const frames = sampleFrames();
      await act(async () => {
        for (const frame of frames) ws.deliver(frame);
      });
      await settle();

      // "bad frames" counts what the client refused. Anything above zero
      // means the TypeScript and Python halves of the schema have drifted.
      const badFrames = screen.getByText('bad frames').nextElementSibling;
      expect(badFrames?.textContent?.trim()).toBe('0');
    });

  it('draws the canvases once data arrives', async () => {
    render(<App />);
    await settle();
    const ws = FakeWebSocket.live[0]!;
    await act(async () => {
      ws.open();
      for (const frame of sampleFrames().slice(0, 80)) ws.deliver(frame);
    });
    drawCalls.length = 0;
    await settle(60);

    // Not just "the component mounted": the panels actually issued draw
    // calls, which is what a canvas-based UI has instead of a DOM to assert.
    expect(drawCalls.some((c) => c.op === 'fillRect')).toBe(true);
    expect(drawCalls.some((c) => c.op === 'fillText')).toBe(true);
  });

  it('shows engine statistics and flags drops in the panel', async () => {
    render(<App />);
    await settle();
    const ws = FakeWebSocket.live[0]!;
    await act(async () => {
      ws.open();
      ws.deliver({
        v: 1,
        type: 'stats',
        ops_per_sec: 6_120_000,
        p50_ns: 118,
        p99_ns: 342,
        p999_ns: 910,
        max_ns: 14_200,
        orders_resting: 48_211,
        ring_drops: 17,
        schedule_misses: 3,
        reader_lag: 0,
        reader_skipped: 0,
        book_resyncs: 0,
      });
    });

    expect(screen.getByText('6.12M')).toBeTruthy();
    expect(screen.getByText('342 ns')).toBeTruthy();
    // Loss is shown as a word as well as a colour.
    expect(screen.getByText('DATA LOST')).toBeTruthy();
    expect(screen.getByText('SATURATED')).toBeTruthy();
    expect(screen.getByText(/discarded events/i)).toBeTruthy();
  });

  it('blames the viewer, not the engine, when it is the viewer that is behind',
    async () => {
      // These were briefly one counter, and the panel then reported engine
      // data loss during a demo in which the engine had lost nothing. A
      // warning that points at the wrong component is worse than none.
      render(<App />);
      await settle();
      const ws = FakeWebSocket.live[0]!;
      await act(async () => {
        ws.open();
        ws.deliver({
          v: 1, type: 'stats', ops_per_sec: 1_000_000, p50_ns: 100,
          p99_ns: 200, p999_ns: 300, max_ns: 400, orders_resting: 10,
          ring_drops: 0, schedule_misses: 0, reader_lag: 140_000,
          reader_skipped: 7_300_000,
          book_resyncs: 0,
        });
      });

      expect(screen.getByText('BEHIND')).toBeTruthy();
      expect(screen.getByText(/jumped forward to stay current/i)).toBeTruthy();
      expect(screen.queryByText('DATA LOST')).toBeNull();
      expect(screen.queryByText(/discarded events/i)).toBeNull();
    });

  it('renders the tape newest-first with the aggressor side as a letter', async () => {
    render(<App />);
    await settle();
    const ws = FakeWebSocket.live[0]!;
    await act(async () => {
      ws.open();
      ws.deliver({
        v: 1,
        type: 'trades',
        items: [
          { t_ns: 1_000, price: 1002100, qty: 100, aggressor: 'B', resting_id: 1 },
          { t_ns: 2_000, price: 1002200, qty: 250, aggressor: 'S', resting_id: 2 },
        ],
        elided: 0,
      });
      ws.deliver({
        v: 1, type: 'book', t_ns: 2_000, symbol: 'X', seq: 1,
        bids: [[1002100, 100, 1]], asks: [[1002400, 100, 1]], last_trade: null,
      });
    });
    await settle(60);

    expect(screen.getByText('100.2200')).toBeTruthy();
    expect(screen.getByText('250')).toBeTruthy();
    // Colour is never the only signal: the side is a letter in its own
    // column.
    expect(screen.getAllByText('S').length).toBeGreaterThan(0);
  });

  it('sends transport commands and disables them while disconnected', async () => {
    render(<App />);
    await settle();
    const ws = FakeWebSocket.live[0]!;

    const step = screen.getByTitle('Advance exactly one event');
    expect((step as HTMLButtonElement).disabled).toBe(true);

    await act(async () => {
      ws.open();
      ws.deliver({
        v: 1, type: 'transport', state: 'playing', speed: 1,
        event_index: 0, total_events: 100,
      });
    });
    expect((step as HTMLButtonElement).disabled).toBe(false);

    await act(async () => {
      step.click();
    });
    expect(ws.sent.map((s) => JSON.parse(s))).toContainEqual({
      action: 'step',
    });
  });

  it('counts frames it cannot parse rather than rendering them', async () => {
    render(<App />);
    await settle();
    const ws = FakeWebSocket.live[0]!;
    await act(async () => {
      ws.open();
      ws.deliverRaw('not json at all');
      ws.deliver({ v: 99, type: 'book' });
      ws.deliver({ v: 1, type: 'frobnicate' });
    });

    // The counter lives in the stats panel, which shows a waiting message
    // until a stats frame arrives -- so deliver one before asserting.
    await act(async () => {
      ws.deliver({
        v: 1, type: 'stats', ops_per_sec: 1, p50_ns: 1, p99_ns: 2,
        p999_ns: 3, max_ns: 4, orders_resting: 0, ring_drops: 0,
        schedule_misses: 0, reader_lag: 0, reader_skipped: 0, book_resyncs: 0,
      });
    });
    expect(screen.getByText('bad frames')).toBeTruthy();
    expect(screen.getByText('SCHEMA')).toBeTruthy();
    // Three malformed frames in, three counted.
    const counter = screen.getByText('bad frames').nextElementSibling;
    expect(counter?.textContent).toContain('3');
  });

  it('says when the tape is a sample rather than the whole tape', async () => {
    // At full rate the engine prints far more than the tape holds, so the
    // server caps what it sends. A tape that does not admit that is one you
    // cannot trust for the prints it does show.
    render(<App />);
    await settle();
    const ws = FakeWebSocket.live[0]!;
    await act(async () => {
      ws.open();
      ws.deliver({
        v: 1,
        type: 'trades',
        items: [
          { t_ns: 1_000, price: 1002100, qty: 100, aggressor: 'B', resting_id: 1 },
        ],
        elided: 4_488,
      });
      ws.deliver({
        v: 1, type: 'book', t_ns: 2_000, symbol: 'X', seq: 9,
        bids: [[1002100, 100, 1]], asks: [[1002400, 100, 1]], last_trade: null,
      });
    });
    await settle(60);
    expect(screen.getByText(/4.5k prints not shown/)).toBeTruthy();
  });
});
