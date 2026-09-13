/**
 * Play, pause, step, speed, seek, reset.
 *
 * **Step is the important one.** It advances the replay by exactly one event,
 * and it is worth building first: "what did that one message do to the book"
 * is the question you end up asking over and over, and without a step button
 * the only way to answer it is to add a print statement and rebuild.
 */

import { useCallback } from 'react';

import type { TransportCommand, TransportFrame } from '../protocol';

interface Props {
  transport: TransportFrame | null;
  send: (command: TransportCommand) => void;
  disabled: boolean;
}

const SPEEDS = [1, 10, 100, 1000];

export function TransportControls({ transport, send, disabled }: Props) {
  const playing = transport?.state === 'playing';
  const speed = transport?.speed ?? 1;
  const index = transport?.event_index ?? 0;
  const total = transport?.total_events ?? 0;

  const onSeek = useCallback(
    (event: React.ChangeEvent<HTMLInputElement>) => {
      send({ action: 'seek', event_index: Number(event.target.value) });
    },
    [send],
  );

  return (
    <div className="flex h-full items-center gap-2 px-2 text-[11px]">
      <Button
        onClick={() => send({ action: playing ? 'pause' : 'play' })}
        disabled={disabled}
        active={playing}
        title={playing ? 'Pause' : 'Play'}
      >
        {playing ? '❚❚ pause' : '▶ play'}
      </Button>
      <Button
        onClick={() => send({ action: 'step' })}
        disabled={disabled}
        title="Advance exactly one event"
      >
        ▸| step
      </Button>
      <Button
        onClick={() => send({ action: 'reset' })}
        disabled={disabled}
        title="Back to the beginning at 1x"
      >
        ⟲ reset
      </Button>

      <div className="ml-2 flex items-center gap-1">
        <span className="text-muted">speed</span>
        {SPEEDS.map((s) => (
          <Button
            key={s}
            onClick={() => send({ action: 'speed', speed: s })}
            disabled={disabled}
            active={speed === s}
            title={`${s}x`}
          >
            {s}×
          </Button>
        ))}
      </div>

      <label className="ml-2 flex min-w-0 flex-1 items-center gap-2">
        <span className="shrink-0 text-muted">seek</span>
        <input
          type="range"
          min={0}
          max={Math.max(1, total)}
          value={Math.min(index, Math.max(1, total))}
          onChange={onSeek}
          disabled={disabled || total === 0}
          className="h-1 min-w-0 flex-1 cursor-pointer appearance-none rounded bg-ink-600 accent-accent"
        />
      </label>

      <span className="numeral shrink-0 text-muted">
        event {index.toLocaleString()}
        {total > 0 ? ` / ${total.toLocaleString()}` : ''}
      </span>
    </div>
  );
}

function Button({
  children,
  onClick,
  disabled,
  active,
  title,
}: {
  children: React.ReactNode;
  onClick: () => void;
  disabled?: boolean;
  active?: boolean;
  title?: string;
}) {
  return (
    <button
      type="button"
      onClick={onClick}
      disabled={disabled}
      title={title}
      className={`rounded-sm border px-2 py-[2px] transition-colors ${
        active
          ? 'border-accent bg-accent/15 text-accent'
          : 'border-ink-600 bg-ink-700 text-gray-300 hover:border-ink-500'
      } disabled:cursor-not-allowed disabled:opacity-40`}
    >
      {children}
    </button>
  );
}
