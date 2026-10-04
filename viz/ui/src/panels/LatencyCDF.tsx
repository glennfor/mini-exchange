/**
 * The latency CDF, percentile against latency on a log-x axis.
 *
 * The panel that matters most to anyone who understands the project, which is
 * why it gets real estate rather than a corner.
 *
 * **Log x, always.** A linear axis spends 95% of its width on the region
 * between p0 and p50 and squashes the entire tail — the only interesting
 * part — into the last few pixels. Log-x shows p50 and p99.99 in the same
 * picture, which is the only way to see the *shape* of a tail rather than
 * just its end.
 *
 * The server sends percentile points, not samples: five numbers per second
 * instead of five million.
 *
 * **It is often empty, and it has to say why.** `tap_producer` -- the engine
 * behind `./scripts/demo.sh` -- publishes zeros for the percentiles on
 * purpose: the matching core does not time itself, because measuring latency
 * is the benchmark harness's job and an engine that instruments its own hot
 * path is measuring the instrumentation. So the demo never produces a curve.
 * Saying "no latency samples yet" to that is a panel implying samples are on
 * their way when none ever will be, which is the same failure as a blank
 * panel standing in for a quiet market.
 */

import { useRef } from 'react';

import { COLOR, useSizedCanvas } from '../lib/canvas';
import { formatNanos, type StatsFrame } from '../protocol';
import { drawPlaceholder } from './DepthLadder';

interface Props {
  stats: StatsFrame | null;
}

/** The percentile ladder the engine publishes. */
const POINTS: { p: number; key: keyof StatsFrame; label: string }[] = [
  { p: 50, key: 'p50_ns', label: 'p50' },
  { p: 99, key: 'p99_ns', label: 'p99' },
  { p: 99.9, key: 'p999_ns', label: 'p99.9' },
  { p: 100, key: 'max_ns', label: 'max' },
];

export function LatencyCDF({ stats }: Props) {
  const latest = useRef<StatsFrame | null>(null);
  if (stats) latest.current = stats;

  const canvasRef = useSizedCanvas((ctx, width, height) => {
    ctx.fillStyle = COLOR.background;
    ctx.fillRect(0, 0, width, height);

    const s = latest.current;
    if (!s) {
      drawPlaceholder(ctx, width, height, 'waiting for a stats frame');
      return;
    }
    if (s.max_ns <= 0) {
      // Stats are arriving and carry no percentiles: this producer does not
      // measure latency. Name the thing that does.
      drawPlaceholder(
        ctx,
        width,
        height,
        'this producer does not measure latency \u2014 run engine_bench',
      );
      return;
    }

    const padLeft = 34;
    const padRight = 10;
    const padTop = 10;
    const padBottom = 20;
    const plotW = Math.max(10, width - padLeft - padRight);
    const plotH = Math.max(10, height - padTop - padBottom);

    // Axis bounds: one decade either side of the data, so the curve does not
    // touch the frame.
    const minNs = Math.max(1, Math.min(s.p50_ns, 100) / 2);
    const maxNs = Math.max(s.max_ns * 1.5, minNs * 10);
    const logMin = Math.log10(minNs);
    const logMax = Math.log10(maxNs);

    const xOf = (ns: number) =>
      padLeft +
      ((Math.log10(Math.max(1, ns)) - logMin) / (logMax - logMin)) * plotW;
    // Percentile axis is itself compressed towards 100, because the whole
    // point is the region between p99 and p100.
    const yOf = (p: number) => {
      const t = p >= 100 ? 1 : Math.log10(100 / (100 - p)) / Math.log10(1e4);
      return padTop + plotH - Math.min(1, t) * plotH;
    };

    // Decade grid.
    ctx.strokeStyle = COLOR.grid;
    ctx.lineWidth = 1;
    ctx.font = '9px ui-monospace, Menlo, monospace';
    ctx.fillStyle = COLOR.muted;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    for (
      let decade = Math.ceil(logMin);
      decade <= Math.floor(logMax);
      decade++
    ) {
      const x = xOf(10 ** decade);
      ctx.beginPath();
      ctx.moveTo(x, padTop);
      ctx.lineTo(x, padTop + plotH);
      ctx.stroke();
      ctx.fillText(formatNanos(10 ** decade), x, padTop + plotH + 4);
    }

    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (const p of [50, 90, 99, 99.9, 99.99]) {
      const y = yOf(p);
      ctx.strokeStyle = COLOR.grid;
      ctx.beginPath();
      ctx.moveTo(padLeft, y);
      ctx.lineTo(padLeft + plotW, y);
      ctx.stroke();
      ctx.fillStyle = COLOR.muted;
      ctx.fillText(p === 99.99 ? 'p99.99' : `p${p}`, padLeft - 4, y);
    }

    // The curve.
    ctx.strokeStyle = COLOR.accent;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    POINTS.forEach((point, i) => {
      const ns = s[point.key] as number;
      const x = xOf(ns);
      const y = yOf(point.p);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();

    // Points and their labels, so the numbers are legible without hovering.
    ctx.font = '9px ui-monospace, Menlo, monospace';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'bottom';
    for (const point of POINTS) {
      const ns = s[point.key] as number;
      const x = xOf(ns);
      const y = yOf(point.p);
      ctx.fillStyle = COLOR.accent;
      ctx.beginPath();
      ctx.arc(x, y, 2.5, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillStyle = COLOR.text;
      ctx.fillText(`${point.label} ${formatNanos(ns)}`, x + 5, y - 3);
    }
  }, [stats]);

  return <canvas ref={canvasRef} className="block h-full w-full" />;
}
