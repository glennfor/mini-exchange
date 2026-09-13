/**
 * jsdom does not implement canvas, ResizeObserver or requestAnimationFrame in
 * a way the panels need, so they are stubbed here.
 *
 * The stubs are deliberately *recording* rather than empty: a test can assert
 * that a panel actually drew something, which is the difference between
 * "the component mounted" and "the component works".
 */

import { afterEach, vi } from 'vitest';

export interface DrawCall {
  op: string;
  args: unknown[];
}

/** Every 2D context handed out during a test, with what was drawn on it. */
export const drawCalls: DrawCall[] = [];

const CONTEXT_METHODS = [
  'fillRect', 'clearRect', 'fillText', 'beginPath', 'arc', 'fill', 'stroke',
  'moveTo', 'lineTo', 'drawImage', 'setTransform', 'save', 'restore',
  'closePath', 'rect', 'strokeRect', 'measureText', 'createLinearGradient',
] as const;

function makeContext(): CanvasRenderingContext2D {
  const ctx: Record<string, unknown> = {
    canvas: null,
    globalCompositeOperation: 'source-over',
    imageSmoothingEnabled: true,
  };
  for (const method of CONTEXT_METHODS) {
    ctx[method] = (...args: unknown[]) => {
      drawCalls.push({ op: method, args });
      if (method === 'measureText') return { width: 10 };
      return undefined;
    };
  }
  return ctx as unknown as CanvasRenderingContext2D;
}

HTMLCanvasElement.prototype.getContext = vi.fn(
  () => makeContext(),
) as unknown as HTMLCanvasElement['getContext'];

// jsdom reports zero for every layout box, which would make every panel draw
// into a 0x0 canvas and assert nothing.
Object.defineProperty(HTMLElement.prototype, 'clientWidth', {
  configurable: true,
  get: () => 800,
});
Object.defineProperty(HTMLElement.prototype, 'clientHeight', {
  configurable: true,
  get: () => 600,
});

class StubResizeObserver {
  observe(): void {}
  unobserve(): void {}
  disconnect(): void {}
}
globalThis.ResizeObserver =
  StubResizeObserver as unknown as typeof ResizeObserver;

// A real rAF, so the panels' draw loops run; capped so a test cannot spin.
let rafHandle = 0;
const rafCallbacks = new Map<number, FrameRequestCallback>();
globalThis.requestAnimationFrame = ((cb: FrameRequestCallback) => {
  const handle = ++rafHandle;
  rafCallbacks.set(handle, cb);
  setTimeout(() => {
    const callback = rafCallbacks.get(handle);
    rafCallbacks.delete(handle);
    callback?.(performance.now());
  }, 8);
  return handle;
}) as typeof requestAnimationFrame;
globalThis.cancelAnimationFrame = ((handle: number) => {
  rafCallbacks.delete(handle);
}) as typeof cancelAnimationFrame;

afterEach(() => {
  drawCalls.length = 0;
  rafCallbacks.clear();
});
