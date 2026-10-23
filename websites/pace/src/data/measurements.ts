// The site's numbers, read from the committed measurement artifacts at build
// time.
//
// Nothing on the site is a hand-typed figure. A number that is typed by hand
// is a number that drifts from the measurement, and a drifted number on a
// product site is a false claim nobody notices. So the two artifacts below
// are parsed here and every page renders what this module returns; if an
// artifact is missing the build fails rather than shipping a page with a
// blank where a measurement should be.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));

// The repository root, so the two artifacts below can be found.
//
// This cannot be derived from this module's own path: Astro bundles the page
// into dist/.prerender/chunks, so import.meta.url points into the build
// output by the time the module runs. The astro project lives at
// websites/pace, so the repository is two directories above the build's
// working directory, and PACE_REPO overrides it for a layout where that is
// not true (the CI jobs check the repo out at one path and may run from
// another).
const repo = path.resolve(
  process.env.PACE_REPO ?? path.resolve(process.cwd(), '..', '..')
);

function readArtifact(relative: string): string {
  const absolute = path.join(repo, relative);
  if (!fs.existsSync(absolute)) {
    throw new Error(
      `the measurement artifact ${relative} is missing; the site publishes ` +
        `numbers it has not measured, so the build stops here`
    );
  }
  return fs.readFileSync(absolute, 'utf8');
}

// -------------------------------------------------------------- accuracy

export type Measurement = {
  name: string;
  short: string;
  unit: string;
  n: number;
  median: number;
  p95: number;
  max: number;
  threshold: number;
  skipped: boolean;
  skipReason: string | null;
  passed: boolean;
};

type RawMeasurement = {
  name: string;
  unit: string;
  n: number;
  median_relative_error: number;
  p95_relative_error: number;
  max_relative_error: number;
  threshold: number;
  skipped: boolean;
  skip_reason: string | null;
  passed: boolean;
};

const artifact = JSON.parse(readArtifact('pace-sdk/results/accuracy.json')) as {
  environment: Record<string, unknown>;
  measurements: RawMeasurement[];
  decomposition: { total_samples: number; violating_samples: number };
  perf_sched_available: boolean;
  raw_records: RawRecord[];
};

export type RawRecord = {
  mode: string;
  tid: number;
  pace_on_cpu_s: number;
  stat_on_cpu_s: number;
  schedstat_on_cpu_s: number;
  pace_preempted_s: number;
  schedstat_preempted_s: number;
  pace_wall_s: number;
  pace_reported_wall_s: number;
  pace_blocked_s: number;
  source: string;
  ref_cycles: number;
  core_cycles: number;
  repetition: number;
};

// The harness's own per-repetition records, so a page can show the
// distribution rather than only the summary statistic.
export const rawRecords: RawRecord[] = artifact.raw_records;

// The harness names a measurement by the whole comparison; a chart axis
// needs the short form. The order here is the order the page shows them in.
const SHORT: Record<string, string> = {
  'on-CPU time vs /proc/<tid>/schedstat (kernel cfs accounting)': 'on-CPU vs schedstat',
  'on-CPU time vs /proc/<tid>/stat utime+stime (tick granularity)': 'on-CPU vs utime+stime',
  'on-CPU time vs perf sched timehist \'run time\' column': 'on-CPU vs perf run time',
  'preempted time vs /proc/<tid>/schedstat (kernel run-queue delay)': 'preempted vs schedstat',
  'preempted time vs perf sched timehist \'sch delay\' column (reported, not gated)':
    'preempted vs perf sch delay',
  'off-CPU time (preempted + blocked) vs perf \'wait time\' column': 'off-CPU vs perf wait time',
  'blocked time vs wall minus on-CPU (sleeping window)': 'blocked vs wall minus on-CPU',
  'wall time vs CLOCK_MONOTONIC read by the verifier': 'wall vs CLOCK_MONOTONIC',
  "the trace's preempted total vs the tier's own cumulative": 'trace total vs tier total',
  'decomposition balance on a recorded trace': 'decomposition balance',
};

function toPercent(fraction: number): number {
  return fraction * 100;
}

export const measurements: Measurement[] = artifact.measurements.map((m) => ({
  name: m.name,
  short: SHORT[m.name] ?? m.name,
  unit: m.unit,
  n: m.n,
  median: toPercent(m.median_relative_error),
  p95: toPercent(m.p95_relative_error),
  max: toPercent(m.max_relative_error),
  threshold: toPercent(m.threshold),
  skipped: m.skipped,
  skipReason: m.skip_reason,
  passed: m.passed,
}));

export const published = measurements.filter((m) => !m.skipped);

export const decomposition = {
  samples: artifact.decomposition.total_samples,
  violating: artifact.decomposition.violating_samples,
};

export const environment = artifact.environment as {
  cpu_model: string;
  cpu_count: number;
  uname: string;
  kernel_perf_event_paranoid: string;
  governor: string;
  clk_tck: number;
  timestamp_utc: string;
};

export const perfSchedAvailable = artifact.perf_sched_available;

// -------------------------------------------------------------- overhead

export type OverheadRow = {
  configuration: string;
  medianNs: number;
  /** Absent where the harness reports only a median, which Google Benchmark does. */
  p99Ns: number | null;
  note: string;
};

type OverheadArtifact = {
  example: {
    calls: number;
    tier: string;
    min_ns: number;
    median_ns: number;
    p99_ns: number;
    max_ns: number;
  };
  benchmarks: {
    benchmark: string;
    repetitions: number;
    median_ns: number;
  }[];
  environment: { uname: string; binaries: { overhead: string; benchmark: string } };
};

const BENCH_LABEL: Record<string, string> = {
  'BM_SampleOverhead/real_time': 'per sample, over a hundred thousand calls',
  BM_SessionLifecycle: 'session create + destroy, once per process',
  BM_ThreadRegistration: 'thread registration, once per thread',
};

// The per-call cost benchmark. It is produced by scripts/collect_overhead.py
// from the SDK's own harnesses, which is a separate process from the accuracy
// harness, so its artifact is separate. This module tolerates its absence
// rather than inventing a figure: a page that would show a cost chart simply
// does not, and the site never prints a cost it did not measure.
function readOverhead(): { rows: OverheadRow[]; example: OverheadArtifact['example'] } | null {
  const absolute = path.join(repo, 'pace-sdk/results/overhead.json');
  if (!fs.existsSync(absolute)) return null;
  const parsed = JSON.parse(fs.readFileSync(absolute, 'utf8')) as OverheadArtifact;
  const rows: OverheadRow[] = [
    {
      configuration: 'per sample, tier 0, no privilege',
      medianNs: parsed.example.median_ns,
      p99Ns: parsed.example.p99_ns,
      note: `median and p99 of ${parsed.example.calls.toLocaleString('en-US')} calls`,
    },
    ...parsed.benchmarks.map((b) => ({
      configuration: BENCH_LABEL[b.benchmark] ?? b.benchmark,
      medianNs: b.median_ns,
      p99Ns: null,
      note: `Google Benchmark, ${b.repetitions} repetitions`,
    })),
  ];
  return { rows, example: parsed.example };
}

export const overhead = readOverhead();



// ------------------------------------------------------------------- run

export type Thread = {
  tid: number;
  onCpuNs: number;
  preemptedNs: number;
  blockedNs: number;
  wallNs: number;
  preemptPct: number;
  switches: number;
};

const UNIT_NS: Record<string, number> = {
  ns: 1,
  'µs': 1e3,
  us: 1e3,
  ms: 1e6,
  s: 1e9,
};

function parseDuration(value: string, unit: string): number {
  return Number(value) * (UNIT_NS[unit] ?? 1);
}

// The terminal report is the analyzer's own committed output, so parsing it
// is how the site gets per-thread figures without re-running the trace.
const reportText = readArtifact('pace-analyzer/results/real-trace-report.txt');

function parseThreads(text: string): Thread[] {
  const threads: Thread[] = [];
  for (const line of text.split('\n')) {
    const t = line.trim().split(/\s+/);
    if (t.length < 10 || !/^\d+$/.test(t[0])) continue;
    const onCpuNs = parseDuration(t[1], t[2]);
    const preemptedNs = parseDuration(t[3], t[4]);
    const blockedNs = parseDuration(t[5], t[6]);
    threads.push({
      tid: Number(t[0]),
      onCpuNs,
      preemptedNs,
      blockedNs,
      wallNs: onCpuNs + preemptedNs + blockedNs,
      preemptPct: Number(t[7].replace('%', '')),
      switches: Number(t[8]),
    });
  }
  return threads;
}

export const threads: Thread[] = parseThreads(reportText).sort(
  (a, b) => b.preemptPct - a.preemptPct
);

const summaryMatch = reportText.match(/samples:\s*([\d]+)\s*across\s*(\d+)\s*threads\s*decomposition:\s*(\w+)/);

export const run = {
  samples: summaryMatch ? Number(summaryMatch[1]) : 0,
  threadCount: summaryMatch ? Number(summaryMatch[2]) : 0,
  decomposition: summaryMatch ? summaryMatch[3] : 'unknown',
  findings: (reportText.match(/^\[(\w+)\]/gm) ?? []).length,
  worst: threads[0],
  best: threads[threads.length - 1],
  totals: threads.reduce(
    (acc, t) => ({
      onCpuNs: acc.onCpuNs + t.onCpuNs,
      preemptedNs: acc.preemptedNs + t.preemptedNs,
      blockedNs: acc.blockedNs + t.blockedNs,
      wallNs: acc.wallNs + t.wallNs,
    }),
    { onCpuNs: 0, preemptedNs: 0, blockedNs: 0, wallNs: 0 }
  ),
};

// ------------------------------------------------------------- formatting

export function ms(ns: number, digits = 2): string {
  return `${(ns / 1e6).toFixed(digits)} ms`;
}

export function seconds(ns: number, digits = 2): string {
  return `${(ns / 1e9).toFixed(digits)} s`;
}

/** A duration in the unit that reads best for its magnitude. */
export function human(ns: number): string {
  if (ns >= 1e9) return seconds(ns);
  return ms(ns);
}

export function pct(value: number, digits = 1): string {
  return `${value.toFixed(digits)} %`;
}

/** Four decimal places, because the top of the table is 0.0006 %. */
export function fine(value: number): string {
  return `${value.toFixed(4)} %`;
}

export function share(part: number, whole: number): number {
  return whole === 0 ? 0 : (part / whole) * 100;
}
