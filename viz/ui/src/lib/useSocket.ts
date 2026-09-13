/**
 * The WebSocket connection, with reconnection.
 *
 * Two rules from the spec, both load-bearing:
 *
 * * **No polling; push only.** The server publishes at a fixed rate and the
 *   browser draws what arrives. Asking for state on a timer would double the
 *   work and still be behind.
 * * **Handle disconnect with visible state and exponential backoff.** A
 *   dashboard that silently stops updating looks exactly like a quiet market.
 *   The header shows the connection state at all times, and the reconnect
 *   backs off so a dead server is not hammered.
 *
 * Frames arrive faster than React should re-render (30 Hz of book frames is
 * fine, but the tape can carry hundreds of trades per frame). So the hot data
 * is written into refs that the canvases read on their own animation frame,
 * and only the slow state -- stats, transport, connection -- goes through
 * React.
 */

import { useCallback, useEffect, useRef, useState } from 'react';

import {
  parseFrame,
  type BookFrame,
  type HelloFrame,
  type StatsFrame,
  type TradeItem,
  type TransportCommand,
  type TransportFrame,
} from '../protocol';

export type ConnectionState =
  | 'connecting'
  | 'open'
  | 'reconnecting'
  | 'closed';

export interface SocketData {
  /** Latest book frame. A ref: the canvases read it, React does not. */
  book: React.MutableRefObject<BookFrame | null>;
  /** Newest-first, capped. */
  trades: React.MutableRefObject<TradeItem[]>;
  /** Bumped whenever a book frame lands, so canvases know to redraw. */
  revision: React.MutableRefObject<number>;
  hello: HelloFrame | null;
  stats: StatsFrame | null;
  transport: TransportFrame | null;
  connection: ConnectionState;
  /** Frames the client refused because they did not match the schema. */
  rejected: number;
  send: (command: TransportCommand) => void;
}

const MAX_TAPE = 500;
const BACKOFF_MS = [250, 500, 1000, 2000, 4000, 8000];

export function useSocket(url: string): SocketData {
  const book = useRef<BookFrame | null>(null);
  const trades = useRef<TradeItem[]>([]);
  const revision = useRef(0);
  const socket = useRef<WebSocket | null>(null);
  const attempt = useRef(0);
  const timer = useRef<number | null>(null);
  const closed = useRef(false);

  const [hello, setHello] = useState<HelloFrame | null>(null);
  const [stats, setStats] = useState<StatsFrame | null>(null);
  const [transport, setTransport] = useState<TransportFrame | null>(null);
  const [connection, setConnection] = useState<ConnectionState>('connecting');
  const [rejected, setRejected] = useState(0);

  const connect = useCallback(() => {
    if (closed.current) return;
    const ws = new WebSocket(url);
    socket.current = ws;

    // Every handler checks it still owns the current socket before doing
    // anything.
    //
    // Without this, React's StrictMode double-mount leaves an orphaned
    // socket behind: the effect runs, connects, is torn down, and runs again,
    // and the first socket's `onclose` then fires *after* the second effect
    // has reset the closed flag -- so it schedules another reconnect, which
    // orphans another socket, which schedules another. Four live connections
    // to the server were the symptom; the cause was that a closed socket's
    // handler had no way to know it was no longer the one in charge.
    const isCurrent = () => socket.current === ws;

    ws.onopen = () => {
      if (!isCurrent()) return;
      attempt.current = 0;
      setConnection('open');
    };

    ws.onmessage = (event) => {
      if (!isCurrent()) return;
      let raw: unknown;
      try {
        raw = JSON.parse(event.data as string);
      } catch {
        setRejected((n) => n + 1);
        return;
      }
      const frame = parseFrame(raw);
      if (frame === null) {
        // Counted and shown, not swallowed. A schema mismatch that renders as
        // a blank panel is indistinguishable from a quiet market.
        setRejected((n) => n + 1);
        return;
      }
      switch (frame.type) {
        case 'hello':
          setHello(frame);
          break;
        case 'book':
          // Only count it as a new tick if the book actually moved.
          //
          // The heatmap treats each revision as one time column, so a server
          // that re-sent an identical snapshot would draw a wall of identical
          // columns -- which is exactly what a paused transport looked like
          // before the server stopped doing that. Belt and braces: the client
          // should not depend on the server never repeating itself.
          if (book.current === null || book.current.seq !== frame.seq) {
            revision.current += 1;
          }
          book.current = frame;
          break;
        case 'trades':
          // Newest first, capped. An unbounded tape is a memory leak with a
          // scrollbar.
          trades.current = [
            ...frame.items.slice().reverse(),
            ...trades.current,
          ].slice(0, MAX_TAPE);
          break;
        case 'stats':
          setStats(frame);
          break;
        case 'transport':
          setTransport(frame);
          break;
        case 'error':
          console.warn('server error frame:', frame.message);
          break;
      }
    };

    ws.onclose = () => {
      if (!isCurrent() || closed.current) return;
      setConnection('reconnecting');
      const delay =
        BACKOFF_MS[Math.min(attempt.current, BACKOFF_MS.length - 1)] ?? 8000;
      attempt.current += 1;
      timer.current = window.setTimeout(connect, delay);
    };

    ws.onerror = () => {
      if (isCurrent()) ws.close();
    };
  }, [url]);

  useEffect(() => {
    closed.current = false;
    connect();
    return () => {
      closed.current = true;
      if (timer.current !== null) window.clearTimeout(timer.current);
      const ws = socket.current;
      // Clear the ref *before* closing, so the socket's own onclose sees it
      // is no longer current and does not schedule a reconnect.
      socket.current = null;
      ws?.close();
      setConnection('closed');
    };
  }, [connect]);

  const send = useCallback((command: TransportCommand) => {
    const ws = socket.current;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(command));
    }
  }, []);

  return {
    book,
    trades,
    revision,
    hello,
    stats,
    transport,
    connection,
    rejected,
    send,
  };
}
