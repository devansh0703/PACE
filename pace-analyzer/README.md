# pace-analyzer

Turns a trace into a per-thread answer to the question a profiler cannot
answer on its own: **how much of this thread's wall time was spent running,
how much was lost to OS preemption, and how much was blocked?**

Open source, Apache-2.0.

```
$ pace analyze trace.jsonl
PACE thread attribution
======================

samples: 106023 across 48 threads   decomposition: exact

thread           on_cpu    preempted      blocked preempt%   switches  core cycles
----------------------------------------------------------------------------------
3164905       654.75 ms       1.22 s    131.12 ms    60.8%          0            -
3164906       673.13 ms       1.18 s    150.41 ms    58.9%          0            -
...
TOTAL      30824.56 ms      65.31 s       0.00 ms    67.9%

findings
--------
[critical] the run lost 67.9% of its total wall time to preemption
```

A thread that lost 60 % of its wall time to preemption is not a code
problem. The fix is a different thread count, a different priority, a
different affinity, or a different machine. A profiler that reports only
CPU time cannot tell you that.

## Install

```bash
cargo build --release
./target/release/pace --help
```

No runtime dependencies beyond the Rust standard library.

## Commands

```
pace analyze <trace>   # the attribution report
pace inspect <trace>  # the raw per-thread samples
pace verify <trace>   # check the decomposition invariant
pace convert <in> --to pace|chrome
```

`verify` exits non-zero when a trace violates the invariant
`wall == on_cpu + preempted + blocked`, so it works as a CI gate. It also
exits non-zero on an empty trace: the invariant holds vacuously there, and
a vacuous pass is a false green.

`analyze --output report.html` writes a **single self-contained file** that
opens offline with nothing to fetch. The per-thread split is drawn as
inline SVG, so there is no charting library to load.

## Formats

| Format | Source |
|---|---|
| PACE JSONL | the `pace-sdk` writer |
| Chrome Trace Event | the standard trace format |
| `perf sched timehist` | perf's scheduling analysis |
| `perf trace` CSV | perf's event trace |

## What it will not do

It does not invent data.

- A line that cannot be read is **counted and skipped**, and the count
  appears in the report. A trace captured under stress is routinely
  truncated; the readable part is still worth analysing.
- A trace whose format carries **no** preemption/blocking split (a plain
  Chrome trace) is reported as "no attribution", not as a confident 0 %
  preempted, and no findings are raised against it.
- A thread's "0 % preempted" is only ever shown when the source format
  actually measured it.

## Relationship to the SDK

`pace-sdk` is the licensed library that produces the samples. This analyzer
is the open-source consumer of them, and the proof that the attribution is
real: it verifies the decomposition on every sample and refuses to report on
a trace that does not hold together.

## Licence

Apache-2.0.
