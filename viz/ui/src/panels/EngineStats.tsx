/**
 * The numbers panel.
 *
 * Big numerals for the two that matter at a glance -- ops/sec and p99 -- and
 * drops and schedule misses **in red when non-zero**, because a
 * silently-dropping dashboard that looks fine is worse than no dashboard.
 *
 * Non-zero drops also get a word, not just a colour: the panel has to be
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

  const problems =
    stats.ring_drops > 0 || stats.schedule_misses > 0 || rejectedFrames > 0;

  return (
    <div className="flex h-full flex-col gap-2 overflow-hidden p-2">
      <div className="grid grid-cols-2 gap-2">
        <Big label="ops / sec" value={formatCount(stats.ops_per_sec)} />
        <Big label="p99" value={formatNanos(stats.p99_ns)} />
      </div>

      <dl className="grid grid-cols-[1fr_auto] gap-x-3 gap-y-[2px] text-[11px]">
        <Row label="p50" value={formatNanos(stats.p50_ns)} />
        <Row label="p99.9" value={formatNanos(stats.p999_ns)} />
        <Row label="max" value={formatNanos(stats.max_ns)} />
        <Row label="orders resting" value={formatCount(stats.orders_resting)} />
        <Row
          label="tap drops"
          value={formatCount(stats.ring_drops)}
          bad={stats.ring_drops > 0}
          suffix={stats.ring_drops > 0 ? 'DROPPING' : undefined}
        />
        <Row
          label="schedule misses"
          value={formatCount(stats.schedule_misses)}
          bad={stats.schedule_misses > 0}
          suffix={stats.schedule_misses > 0 ? 'SATURATED' : undefined}
        />
        <Row label="reader lag" value={formatCount(stats.reader_lag)} />
        <Row
          label="bad frames"
          value={formatCount(rejectedFrames)}
          bad={rejectedFrames > 0}
          suffix={rejectedFrames > 0 ? 'SCHEMA' : undefined}
        />
      </dl>

      {problems && (
        <p className="mt-auto text-[10px] leading-tight text-ask">
          This view is missing data. Drops and schedule misses are shown rather
          than hidden: a dashboard that quietly shows a partial market is worse
          than one that admits it.
        </p>
      )}
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
  bad,
  suffix,
}: {
  label: string;
  value: string;
  bad?: boolean;
  suffix?: string;
}) {
  return (
    <>
      <dt className="text-muted">{label}</dt>
      <dd className={`numeral text-right ${bad ? 'text-ask' : 'text-gray-200'}`}>
        {value}
        {suffix ? <span className="ml-1 text-[9px]">{suffix}</span> : null}
      </dd>
    </>
  );
}
