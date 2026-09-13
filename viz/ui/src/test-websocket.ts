/**
 * A scriptable WebSocket stand-in.
 *
 * The point is to drive the UI with exactly the frames the real server sends
 * -- including the committed sample recording -- without a server, a browser
 * or a network.
 */

export class FakeWebSocket {
  static instances: FakeWebSocket[] = [];
  static readonly CONNECTING = 0;
  static readonly OPEN = 1;
  static readonly CLOSING = 2;
  static readonly CLOSED = 3;

  readonly CONNECTING = 0;
  readonly OPEN = 1;
  readonly CLOSING = 2;
  readonly CLOSED = 3;

  readyState = 0;
  sent: string[] = [];

  onopen: (() => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  onmessage: ((event: { data: string }) => void) | null = null;

  constructor(public readonly url: string) {
    FakeWebSocket.instances.push(this);
  }

  open(): void {
    this.readyState = this.OPEN;
    this.onopen?.();
  }

  deliver(frame: unknown): void {
    this.onmessage?.({ data: JSON.stringify(frame) });
  }

  deliverRaw(text: string): void {
    this.onmessage?.({ data: text });
  }

  send(payload: string): void {
    this.sent.push(payload);
  }

  close(): void {
    if (this.readyState === this.CLOSED) return;
    this.readyState = this.CLOSED;
    this.onclose?.();
  }

  static reset(): void {
    FakeWebSocket.instances = [];
  }

  static get live(): FakeWebSocket[] {
    return FakeWebSocket.instances.filter((ws) => ws.readyState !== ws.CLOSED);
  }
}

export function installFakeWebSocket(): void {
  FakeWebSocket.reset();
  globalThis.WebSocket = FakeWebSocket as unknown as typeof WebSocket;
}
