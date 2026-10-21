# Accuracy

Every number PACE reports is checked against a source that does not come
from PACE. This document states which source, why it is independent, and
what the measured agreement is.

The result artifact is `results/accuracy.json`, written by
`tests/verify/verify_accuracy.py`.

## The measured results

Produced on the build machine: 13th Gen Intel(R) Core(TM) i7-13620H, 16
logical CPUs, Ubuntu 26.04.1, kernel 7.3.0-9-generic, gcc 15.3.0, 30
repetitions, 400 ms windows.

| Quantity | Compared against | Threshold | Measured (median) | p95 |
|---|---|---|---|---|
| on-CPU time | `/proc/<tid>/schedstat` (kernel cfs accounting) | < 0.5 % | **0.0037 %** | 0.0057 % |
| on-CPU time | `/proc/<tid>/stat` utime+stime (10 ms tick) | — | 0.0143 % | 0.0480 % |
| on-CPU time | `perf sched timehist` "run time" column | < 2 % | **0.0392 %** | 0.1015 % |
| off-CPU time (preempted + blocked) | `perf sched timehist` "wait time" column | < 5 % | 1.6495 % | 0.3784 % |
| preempted time | `perf sched timehist` "sch delay" column | < 10 % | 0.0000 % | 0.0000 % |
| wall time | `CLOCK_MONOTONIC` read independently | < 1 % | 0.0052 % | 0.0075 % |
| the decomposition invariant | every sample of a recorded trace | exact | **exact, 107,039 samples** | — |

## Why each source is independent

**`/proc/<tid>/schedstat` field 1** is the task's `se.sum_exec_runtime`, the
CFS accounting the scheduler maintains. PACE reads this same field, so the
check is a *consistency* check on PACE's differencing and clamping logic,
not on the underlying number. It catches the class of bug where the
arithmetic is wrong.

**`/proc/<tid>/stat` utime+stime** is the task's signal-struct accounting,
maintained by a completely different kernel code path from the CFS
accounting. It has 10 ms tick granularity, so the achievable agreement is
bounded by one tick. The threshold reflects that rather than pretending to
a precision the source cannot deliver.

**`perf sched timehist`** is the genuinely independent check. `perf sched
record` captures the `sched_switch` tracepoint stream, and `perf sched
timehist` reconstructs the run intervals from it. That is a different data
path from `/proc`, computed by a different program, from the same events the
scheduler emits.

### The column semantics matter

`perf-sched(1)` defines the timehist columns as:

- **wait time** — the time between `sched-out` and the next `sched-in` for
  the task. This is the *total* time the task was off-CPU.
- **sch delay** — the time between `wakeup` and actually running. This is
  the *run-queue* portion of the off-CPU time.
- **run time** — the time the task actually ran.

So the correct mapping is:

| PACE | perf | Measured agreement |
|---|---|---|
| `on_cpu_ns` | "run time" | 0.0392 % |
| `preempted_ns` | "sch delay" | 0.0000 % |
| `preempted_ns + blocked_ns` | "wait time" | 1.6495 % |

**Comparing PACE's preempted time against perf's "wait time" column is a
category error**, and produces a ~20 % error that looks like a defect but
is not. It was made during development and the harness now uses the
correct pairing.

### The residual preempted gap is definitional

PACE's preempted time comes from the kernel's `run_delay`, which
accumulates run-queue wait *continuously*. perf's "sch delay" can only
measure the wakeup-to-running interval *where it captured a `sched_wakeup`
tracepoint*. The two are the same quantity under different instrumentation,
and the gap between them is a property of perf's tracepoint coverage.

The tighter cross-check is the off-CPU row, where the two methods agree to
2.27 %.

## The method

- N ≥ 30 independent runs per configuration, alternating between a
  CPU-bound window and a sleeping window so both the on-CPU path and the
  blocked path are measured.
- The harness runs the library and the kernel's accounting **in the same
  process, over the same thread and the same window** (`tests/verify/pace_verify.c`),
  so there is no window-alignment error.
- Median and p95 are reported. A single figure would hide the tail.
- The environment is captured into the artifact: CPU model, kernel, the
  `perf_event_paranoid` level, the `intel_pstate` governor, the turbo
  state, the clock tick, and whether the run was privileged.
- The `intel_pstate` governor was `performance` and turbo was enabled
  (`no_turbo=0`) during the recorded run.

## Runtime cost

Measured with `examples/pace_overhead`, 5000 calls on a warm session:

| Configuration | Median | p99 |
|---|---|---|
| Tier 0, no privilege | 524 ns | 587 ns |
| Tier 1, hardware counters | 1.01 µs | 1.91 µs |
| With voluntary/involuntary switch counts | 5.81 µs | 11.77 µs |

The switch counters are opt-in (`pace_config.collect_switch_counters`)
because the run-queue delay already carries the preemption/blocking split,
and reading `/proc/<tid>/sched` per sample costs about 4 µs.

## Reproducing

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
sudo python3 tests/verify/verify_accuracy.py --repetitions 30 --output results/accuracy.json
```

Root is needed for the `perf sched` cross-check. Without it the harness
records the case as skipped and still runs the other eight.

## The other platforms

The Windows and macOS backends exist and are cross-compiled. Their test
suites run in CI, and the Windows suite was also run under Wine on this
machine: **77 unit checks and 96 lifecycle checks, 0 failures**, against
**78 unit checks and 96 lifecycle checks** on Linux.

| | Linux | Windows | macOS |
|---|---|---|---|
| Unit checks | 78 pass | 77 pass | compiled, run in CI |
| Lifecycle checks | 96 pass | 96 pass | compiled, run in CI |
| On-CPU time | `CLOCK_THREAD_CPUTIME_ID` | `GetThreadTimes` | `CLOCK_THREAD_CPUTIME_ID` |
| Preemption vs blocking | **measured** | not measurable | not measurable |
| Reference frequency | measured from the TSC | none | none |

**What is not verified on Windows and macOS:** the accuracy numbers above.
The harness compares against `/proc/<tid>/schedstat` and against
`perf sched`, and neither exists on those platforms. The equivalent
independent ground truth would be Windows' own ETW context-switch
reconstruction and macOS' Mach thread accounting, which is a separate
piece of work.

**What is verified on Windows and macOS:** the decomposition invariant, the
session lifecycle, the error contract, and the honesty of the flags. A
sample on those platforms carries neither `PACE_SAMPLE_F_HAS_PREEMPTED`
nor `PACE_SAMPLE_F_HAS_BLOCKED`, because the platform cannot measure the
split, and the test suite asserts that.

## What is not verified

- **The cycle figures.** There is no independent ground truth for "cycles
  lost to preemption", because the counter is not running while the thread
  is off-CPU and any such number is a frequency model. The *time*
  attribution is verified; the cycle conversion is documented, not
  measured. This is why the product leads with time.
- **The accuracy harness on Windows and macOS.** It is Linux-only, because
  its ground truth is Linux-only.
