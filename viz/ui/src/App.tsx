/**
 * The dashboard.
 *
 * A dense, professional trading window rather than a consumer dashboard, and
 * it contains **no matching logic at all**: every number on screen came off
 * the wire. If a panel needs a value, the server computes it.
 *
 * Desktop-first, a single screen, and the main frame never scrolls -- only
 * the tape does.
 */

import { BookHeatmap } from './panels/BookHeatmap';
import { DepthLadder } from './panels/DepthLadder';
import { EngineStats } from './panels/EngineStats';
import { LatencyCDF } from './panels/LatencyCDF';
import { Tape } from './panels/Tape';
import { TransportControls } from './panels/TransportControls';
import { useSocket, type ConnectionState } from './lib/useSocket';

const WS_URL =
  new URLSearchParams(window.location.search).get('ws') ??
  `ws://${window.location.hostname || '127.0.0.1'}:8765`;

export default function App() {
  const socket = useSocket(WS_URL);
  const live = socket.connection === 'open';

  return (
    <div className="flex h-full flex-col gap-1 p-1">
      <Header
        symbol={socket.hello?.symbol ?? '—'}
        dataset={socket.hello?.dataset ?? '—'}
        sha={socket.hello?.build_sha ?? '—'}
        tap={socket.hello?.tap ?? 'OFF'}
        hz={socket.hello?.publish_hz ?? 0}
        connection={socket.connection}
      />

      <main className="grid min-h-0 flex-1 grid-cols-[300px_minmax(0,1fr)_300px] gap-1">
        <section className="panel">
          <h2 className="panel-title">Depth ladder</h2>
          <div className="min-h-0 flex-1">
            <DepthLadder book={socket.book} />
          </div>
        </section>

        <section className="panel">
          <h2 className="panel-title">
            Book heatmap · time × price, colour = resting size
          </h2>
          <div className="min-h-0 flex-1">
            <BookHeatmap
              book={socket.book}
              trades={socket.trades}
              revision={socket.revision}
            />
          </div>
        </section>

        <div className="grid min-h-0 grid-rows-[minmax(0,auto)_minmax(0,1fr)] gap-1">
          <section className="panel">
            <h2 className="panel-title">Engine</h2>
            <div className="min-h-0 flex-1">
              <EngineStats
                stats={socket.stats}
                rejectedFrames={socket.rejected}
              />
            </div>
          </section>
          <section className="panel">
            <h2 className="panel-title">Latency CDF · log x</h2>
            <div className="min-h-0 flex-1">
              <LatencyCDF stats={socket.stats} />
            </div>
          </section>
        </div>
      </main>

      <footer className="grid h-[188px] shrink-0 grid-cols-[minmax(0,1fr)_minmax(0,1.2fr)] gap-1">
        <section className="panel">
          <h2 className="panel-title">Tape</h2>
          <div className="min-h-0 flex-1">
            <Tape trades={socket.trades} revision={socket.revision} />
          </div>
        </section>
        <section className="panel">
          <h2 className="panel-title">Transport</h2>
          <div className="h-9 shrink-0">
            <TransportControls
              transport={socket.transport}
              send={socket.send}
              disabled={!live}
            />
          </div>
          <div className="min-h-0 flex-1 overflow-auto border-t border-ink-600 p-2 text-[10px] leading-relaxed text-muted">
            <p>
              <span className="text-gray-300">Step</span> advances the replay by
              exactly one event. It is the button worth building first: “what
              did that one message do to the book” is the question you ask over
              and over.
            </p>
            <p className="mt-1">
              Every number here came off the wire. The UI contains no matching
              logic; the server aggregates, and the engine only emits events.
            </p>
            <p className="mt-1">
              Prices are integer ticks at 1e-4 dollars, formatted here. Nothing
              in this project stores a price as a float.
            </p>
          </div>
        </section>
      </footer>
    </div>
  );
}

function Header(props: {
  symbol: string;
  dataset: string;
  sha: string;
  tap: 'ON' | 'OFF';
  hz: number;
  connection: ConnectionState;
}) {
  return (
    <header className="flex h-7 shrink-0 items-center gap-4 border border-ink-600 bg-ink-800 px-2 text-[11px]">
      <span className="text-sm font-semibold tracking-wide text-gray-100">
        {props.symbol}
      </span>
      <Field label="dataset" value={props.dataset} />
      <Field label="build" value={props.sha} />
      <Field
        label="tap"
        value={props.tap}
        tone={props.tap === 'ON' ? 'accent' : 'muted'}
      />
      <Field label="publish" value={props.hz ? `${props.hz} Hz` : '—'} />
      <div className="ml-auto flex items-center gap-2">
        <ConnectionBadge state={props.connection} />
      </div>
    </header>
  );
}

function Field({
  label,
  value,
  tone,
}: {
  label: string;
  value: string;
  tone?: 'accent' | 'muted';
}) {
  return (
    <span className="flex items-baseline gap-1">
      <span className="text-[9px] uppercase tracking-widest text-muted">
        {label}
      </span>
      <span
        className={
          tone === 'accent'
            ? 'text-accent'
            : tone === 'muted'
              ? 'text-muted'
              : 'text-gray-300'
        }
      >
        {value}
      </span>
    </span>
  );
}

/**
 * Connection state, always visible.
 *
 * A dashboard that silently stops updating looks exactly like a quiet market,
 * so the state is shown as a word as well as a colour and a dot.
 */
function ConnectionBadge({ state }: { state: ConnectionState }) {
  const tone =
    state === 'open'
      ? 'text-bid'
      : state === 'connecting'
        ? 'text-accent'
        : 'text-ask';
  const label =
    state === 'open'
      ? 'connected'
      : state === 'connecting'
        ? 'connecting…'
        : state === 'reconnecting'
          ? 'reconnecting…'
          : 'disconnected';
  return (
    <span className={`flex items-center gap-1 ${tone}`}>
      <span
        className="inline-block h-[6px] w-[6px] rounded-full"
        style={{ background: 'currentColor' }}
      />
      {label}
    </span>
  );
}
