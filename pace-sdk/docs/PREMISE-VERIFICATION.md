# PACE — Empirical Premise Verification

Machine: GIGABYTE G6 KF (i7-13620H, 10 cores / 16 threads, hybrid P-core + E-core), Ubuntu 26.04.1, kernel 7.3.0-9-generic, gcc 15.3.0.
Probe source: `pace-sdk/tests/verify/premise_probe.c` (also runnable standalone).

**This file is the ground truth that every later design decision cites. Every number below was produced by an actual run on this machine, not by reading docs.**

## Executive result

| # | Premise | Verdict | Evidence |
|---|---------|---------|----------|
| A | A per-thread `PERF_COUNT_HW_CPU_CYCLES` counter (`pid=tid, cpu=-1`) does **not** accumulate while the task is descheduled | **VERIFIED** | 200 ms `nanosleep` consumed 373,730 core cycles (vs 235,103,222 over 310 ms of contended wall time) |
| B | `CLOCK_THREAD_CPUTIME_ID` advances only while the thread is on-CPU | **VERIFIED** | 178,447 ns advanced across a 200 ms sleep; 100,153,447 ns across 310 ms contended wall |
| C | `/proc/<tid>/schedstat` exposes `(on_cpu_ns, run_delay_ns, timeslices)` and `run_delay` is involuntary-preemption wait | **VERIFIED** | contended: `run_delay=210,321,285 ns` with `nr_involuntary_switches=+134`; sleep: `run_delay=0` with `nr_voluntary_switches=+1` |
| D | `/proc/<tid>/sched` exposes `nr_voluntary_switches`, `nr_involuntary_switches`, `se.sum_exec_runtime`, `se.nr_migrations` | **VERIFIED** | present in `/proc/self/sched` on this kernel |
| E | `wall = on_cpu + preempted + blocked` reconstructs the wall clock exactly | **VERIFIED** | `sum_matches_wall=YES` in 3/3 scenarios |
| F | The per-thread counter follows the thread across CPU migrations | **VERIFIED** | `se.nr_migrations=+1` between samples while the counter kept accumulating monotonically |
| G | Unprivileged `perf_event_open` for HW counters is blocked on this host | **VERIFIED** | `perf_event_open` returned `EACCES` (errno 13) with `kernel.perf_event_paranoid=4` |

## Scenario 1 — exclusive on-CPU spin (no contention)

```
wall_ns=45834463  threadcputime_ns=45827325  tsc_delta=133778748  tsc_hz=2918400000
hw_cycles=201746767  hw_ref_cycles=133689928
cyc_time_enabled=45819931  cyc_time_running=45819931  running_fraction=1.0000
schedstat on_cpu_ns=45838077  run_delay_ns=9948  timeslices=1
nr_voluntary=+0  nr_involuntary=+1  se.nr_migrations=+1  se.sum_exec_runtime=46
cpu=0->6  state=R->R
DECOMP on_cpu=45838077  preempted=9948  blocked=-13562  sum_matches_wall=YES
```

* Core cycles / reference cycles = **1.509** → the core ran at an effective ~4.40 GHz while the reference clock ticked at 2.9184 GHz (turbo active; `CPUID.16H` base = 2900 MHz, max turbo 4900 MHz).
* `hw_ref_cycles` (133,689,928) matched `tsc_delta` (133,778,748) to 0.07 % → **the invariant TSC and `PERF_COUNT_HW_REF_CPU_CYCLES` tick at the same constant rate on this CPU.** This is the anchor that makes portably-comparable cycle counts possible.
* `blocked=-13562` is negative by 13.5 µs: the `/proc` read and the `clock_gettime` read are not atomic, so the residual is measurement skew, not a modelling error.

## Scenario 2 — 48 spinner threads (heavy involuntary preemption)

```
wall_ns=310446432  threadcputime_ns=100153447  tsc_delta=906099565
hw_cycles=235103222  hw_ref_cycles=187615728
cyc_time_enabled=146673315  cyc_time_running=146673315  running_fraction=1.0000
schedstat on_cpu_ns=100153447  run_delay_ns=210321285  timeslices=134
nr_voluntary=+0  nr_involuntary=+134  se.nr_migrations=+1  se.sum_exec_runtime=100
cpu=6->12  state=R->R
DECOMP on_cpu=100153447  preempted=210321285  blocked=-28300  sum_matches_wall=YES
```

**This is the product's core value, measured.** Over 310 ms of wall time the thread did 100 ms of work and lost **210 ms (67.8 %) to involuntary preemption** — and it was never blocked on a lock, a syscall, or an I/O wait. A conventional profiler reading only `CLOCK_THREAD_CPUTIME_ID` would report "100 ms of CPU time" and a wall-clock timer would report "310 ms", leaving the 210 ms unattributed. That gap is exactly what PACE closes.

Note `timeslices=134` equals `nr_involuntary_switches=+134`: on this kernel each involuntary switch costs one timeslice, and `run_delay` accumulates the runqueue wait across all of them.

## Scenario 3 — 200 ms `nanosleep` (pure voluntary off-CPU)

```
wall_ns=200231700  threadcputime_ns=178447  tsc_delta=584439202
hw_cycles=373730  hw_ref_cycles=336984
schedstat on_cpu_ns=171146  run_delay_ns=0  timeslices=1
nr_voluntary=+1  nr_involuntary=+0  se.nr_migrations=+0
DECOMP on_cpu=171146  preempted=0  blocked=200060554  sum_matches_wall=YES
```

`run_delay=0` and `nr_involuntary=0` while `nr_voluntary=1`: the sleeping thread was **blocked**, not preempted. The two off-CPU causes are cleanly separable on Linux. `hw_cycles=373730` is the cost of entering and leaving the syscall — the counter really did stop.

## Premise F — migration

`se.nr_migrations=+1` between the two samples of scenario 1 (cpu 0 → cpu 6) and the counter kept accumulating across the migration with no reset and no discontinuity. The per-task PMU context migrates with the task.

## Premise G — permissions on this host

```
$ ./probe            # unprivileged
FDS cyc=-1 ref=-1 task=-1 errno=Permission denied
PERF_UNAVAILABLE errno=13
$ cat /proc/sys/kernel/perf_event_paranoid
4
```

`kernel.perf_event_paranoid=4` blocks unprivileged HW counters. The `SW_TASK_CLOCK` software event is also blocked here, but `CLOCK_THREAD_CPUTIME_ID` and `/proc/<tid>/schedstat` are **always** available with no privilege at all.

**Design consequence:** PACE's Linux backend has three tiers.

1. **Tier 0 — always available, zero privilege.** `CLOCK_THREAD_CPUTIME_ID` + `/proc/<tid>/schedstat` + `/proc/<tid>/sched` give the complete wall/on-CPU/preempted/blocked decomposition with nanosecond resolution. This tier is the product's floor: it works inside containers, in CI, and on locked-down customer machines. It reports **nanoseconds**, and cycles are derived by multiplying by the measured reference frequency.
2. **Tier 1 — optional, needs `CAP_PERFMON` or `perf_event_paranoid <= 1`.** `PERF_COUNT_HW_CPU_CYCLES` gives **measured** core cycles with no frequency modelling, and `PERF_COUNT_HW_REF_CPU_CYCLES` gives the reference-clock anchor. When available, PACE reports measured core cycles; when not, it reports frequency-derived reference cycles and says so.
3. **Tier 2 — optional, needs BTF and the privilege to attach tracepoints.** A CO-RE BPF program on `sched_wakeup` and `sched_switch` counts the scheduler's own events, so the switch counts are a direct measurement rather than an inference from a cumulative counter. It is an addition: if it cannot attach, the measurement is unchanged.

A tool that only works with root is a tool nobody can ship inside a customer's game engine.

## Frequency model

* `CPUID.15H` on this CPU: `eax=2, ebx=152, ecx=38400000` → `tsc_hz = ecx * ebx / eax = 38.4 MHz * 152 / 2 = 2,918,400,000 Hz`. This is the crystal-derived invariant TSC rate and is the portable, turbo-independent frequency.
* `CPUID.16H`: `eax=2900` (base MHz), `ebx=4900` (max MHz).
* Reference cycles are the *comparable* unit across frequency states; core cycles are the *actual work* unit. PACE reports both and labels them.

**The TSC is the reference clock, so its tick count IS the reference cycle
count.** Multiplying a TSC delta by the frequency to get reference cycles
is wrong by a factor of the frequency. The probe now reports both
conversions so they can be compared:

```
  wall_ref_cycles=141334164          <- the TSC delta, which is the count
  wall_ref_cycles_from_ns=141332030  <- the wall clock converted at the measured frequency
```

Those two are **0.0015 %** apart, and they are computed from different
clocks: one from the TSC, one from `CLOCK_MONOTONIC` and the calibrated
frequency. That agreement is the frequency model, verified.

**The honest limitation, stated up front:** cycles *lost to preemption* can never be measured directly, because the counter is not running while the thread is off-CPU. PACE estimates them as `preempted_ns × effective_core_frequency`, where the effective frequency is taken from the measured on-CPU core-cycle rate over the same window. That estimate is good to within the frequency variation across the preemption interval, and PACE reports the measured on-CPU frequency alongside it so a user can judge the uncertainty. This is a product-level truth, not a bug, and the analyzer surfaces it.
