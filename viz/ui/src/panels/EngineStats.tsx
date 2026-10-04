/**
 * The numbers panel.
 *
 * Big numerals for the two that matter at a glance -- ops/sec and p99 -- and
 * the loss counters below them, because a silently-lossy dashboard that looks
 * fine is worse than no dashboard.
 *
 * The three kinds of loss are shown **separately and described differently**.
 * They were briefly merged into one "drops" number, which made the panel
 * report engine data loss during a demo where the engine had lost nothing --
 * the viewer was simply behind. A warning that blames the wrong component is
 * worse than no warning, because it sends you to debug the wrong thing.
 *
 * Every non-zero counter gets a word as well as a colour: the panel has to be
 * readable without colour vision.
 */

import { formatCount, formatNanos, type StatsFrame } from '../protocol';

interface Props {
  stats: StatsFrame | null;
  rejectedFrames: number;
}

export function EngineStats({ stats, rejectedFrames }: Props) {
  if (!stats) {
    return (
      <div className="flex h-full items-center justify-center text-xs text-muted">
        waiting for a stats frame
      </div>
    );
  }

  // Three different failures, and the panel must not blur them: the engine
  // discarding events is data loss at the source; the server skipping ahead
  // means the view is live but not continuous; a rejected frame means the
  // schema has drifted. They have different causes and different fixes.
  const engineDropping = stats.ring_drops > 0;
  const viewSkipping = stats.reader_skipped > 0;
  const schemaDrift = rejectedFrames > 0;
  const saturated = stats.schedule_misses > 0;
  const rebuilt = stats.book_resyncs > 0;
  // `tap_producer` publishes zeros for the percentiles on purpose: the
  // matching core does not time itself. Rendering that as "0 ns" claims a
  // measurement of zero nanoseconds, which is both false and flattering.
  const timed = stats.max_ns > 0;
  const ns = (value: number) => (timed ? formatNanos(value) : 'not measured');

  return (
    <div className="flex h-full flex-col gap-2 overflow-hidden p-2">
      <div className="grid grid-cols-2 gap-2">
        <Big label="ops / sec" value={formatCount(stats.ops_per_sec)} />
        <Big label="p99" value={ns(stats.p99_ns)} />
      </div>

      <dl className="grid grid-cols-[1fr_auto] gap-x-3 gap-y-[2px] text-[11px]">
        <Row label="p50" value={ns(stats.p50_ns)} />
        <Row label="p99.9" value={ns(stats.p999_ns)} />
        <Row label="max" value={ns(stats.max_ns)} />
        <Row label="orders resting" value={formatCount(stats.orders_resting)} />
        <Row
          label="engine drops"
          value={formatCount(stats.ring_drops)}
          tone={engineDropping ? 'bad' : undefined}
          suffix={engineDropping ? 'DATA LOST' : undefined}
        />
        <Row
          label="view skipped"
          value={formatCount(stats.reader_skipped)}
          tone={viewSkipping ? 'warn' : undefined}
          suffix={viewSkipping ? 'BEHIND' : undefined}
        />
        <Row label="reader lag" value={formatCount(stats.reader_lag)} />
        <Row
          label="schedule misses"
          value={formatCount(stats.schedule_misses)}
          tone={saturated ? 'warn' : undefined}
          suffix={saturated ? 'SATURATED' : undefined}
        />
        <Row
          label="book rebuilds"
          value={formatCount(stats.book_resyncs)}
          tone={rebuilt ? 'warn' : undefined}
          suffix={rebuilt ? 'PARTIAL' : undefined}
        />
        <Row
          label="bad frames"
          value={formatCount(rejectedFrames)}
          tone={schemaDrift ? 'bad' : undefined}
          suffix={schemaDrift ? 'SCHEMA' : undefined}
        />
      </dl>

      <div className="mt-auto space-y-1 text-[10px] leading-tight">
        {engineDropping && (
          <p className="text-ask">
            The engine&rsquo;s tap ring filled and it discarded events. That is
            by design &mdash; stalling matching to feed a dashboard would be
            backwards &mdash; but this view has gaps the engine itself created.
          </p>
        )}
        {viewSkipping && !engineDropping && (
          <p className="text-accent">
            This viewer cannot drain the tap fast enough, so it jumped forward
            to stay current. The book above is live; the events in between were
            skipped. The engine was not affected.
          </p>
        )}
        {rebuilt && (
          <p className="text-accent">
            A delta book cannot survive a gap: an order whose cancellation was
            lost rests for ever. So the depth view is discarded and rebuilt
            after every gap, and what you see is the book as reconstructed
            since the last one &mdash; thin at first, then correct.
          </p>
        )}
        {!timed && (
          <p className="text-muted">
            This producer does not time itself &mdash; measuring latency is the
            benchmark harness&rsquo;s job, and an engine that instruments its
            own hot path is measuring the instrumentation.{' '}
            <span className="text-gray-300">engine_bench</span> produces the
            percentiles.
          </p>
        )}
        {schemaDrift && (
          <p className="text-ask">
            Frames arrived that this build could not parse. The server and the
            UI disagree about the wire schema.
          </p>
        )}
      </div>
    </div>
  );
}

function Big({ label, value }: { label: string; value: string }) {
  return (
    <div className="rounded-sm bg-ink-700 px-2 py-1">
      <div className="text-[9px] uppercase tracking-widest text-muted">
        {label}
      </div>
      <div className="numeral text-xl leading-tight text-gray-100">{value}</div>
    </div>
  );
}

function Row({
  label,
  value,
  tone,
  suffix,
}: {
  label: string;
  value: string;
  /** `bad` is data actually lost; `warn` is the view being behind. */
  tone?: 'bad' | 'warn';
  suffix?: string;
}) {
  const colour =
    tone === 'bad' ? 'text-ask' : tone === 'warn' ? 'text-accent' : 'text-gray-200';
  return (
    <>
      <dt className="text-muted">{label}</dt>
      <dd className={`numeral text-right ${colour}`}>
        {value}
        {suffix ? <span className="ml-1 text-[9px]">{suffix}</span> : null}
      </dd>
    </>
  );
}
