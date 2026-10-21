# PACE — OS Primitives Reference

Every operating-system primitive PACE uses, what it measures, whether it
counts only on-CPU time, what it needs permission for, and what it costs —
**measured on the machine that ran the measurement**, not quoted from a
vendor's marketing.

The per-call costs come from `pace-sdk/tests/verify/primitive_costs.c`,
which times each primitive in isolation. The environment is printed with
the numbers.

---

## 1. The measured costs

Machine: 13th Gen Intel Core i7-13620H, kernel 7.3.0-9-generic, gcc 15.3.0,
20,000 samples per measurement, median reported. `perf_event_paranoid` was 4
(unprivileged) and 0 (as root).

| Primitive | Median | Min | Counts only on-CPU? | Permission |
|---|---|---|---|---|
| `clock_gettime(CLOCK_MONOTONIC)` | **22–37 ns** | 21 ns | n/a (wall clock) | none |
| `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` | **193–195 ns** | 182 ns | **yes** | none |
| `pread(/proc/<tid>/schedstat)`, cached fd | **244–265 ns** | 232 ns | the second field is the run-queue wait | none |
| `read(PERF_COUNT_HW_CPU_CYCLES)` | **211–217 ns** | 204 ns | **yes** | `CAP_PERFMON` or `perf_event_paranoid < 2` |
| `getpid()`, a bare syscall, for scale | **113–117 ns** | 106 ns | n/a | none |
| `open` + `pread` + `close(/proc/<tid>/schedstat)` | **1523–1639 ns** | 1439 ns | as above | none |
| `open` + `read` + `parse(/proc/<tid>/stat)` | **2734–2840 ns** | 2591 ns | n/a | none |
| **the whole `pace_session_sample()`**, tier 0 | **571–574 ns** | 547 ns | — | none |
| **the whole `pace_session_sample()`**, tier 1 | **1000–1004 ns** | 936 ns | — | `CAP_PERFMON` |

**The total is accounted for.** Tier 0: 195 ns for the CPU clock + 265 ns
for the `schedstat` read + 37 ns for the wall clock = 497 ns, and the sample
takes 571 ns. The remaining 74 ns is the differencing, the clamping and the
flag arithmetic.

Tier 1 adds the hardware counter's 211 ns, and the total goes to 1000 ns.

**The tier-1 total has a second cost, and it is accounted for.** 571 ns
plus one counter read of 211 ns is 782 ns, but the measured tier-1 sample
is 1000 ns. The difference is that tier 1 reads **two** counters, not one:
`PERF_COUNT_HW_CPU_CYCLES` for the core cycles and
`PERF_COUNT_HW_REF_CPU_CYCLES` for the reference anchor. Two reads at
211 ns is 422 ns, and 571 + 422 = 993 ns, which is the measured 1000 ns.

The reference counter is not optional: it is the anchor that converts the
nanosecond time into reference cycles and carries the measured frequency,
which is what makes the cycle figures comparable across machines.

**The two numbers that decide the design:**

- `open` + `pread` + `close` of `schedstat` costs **1583 ns**, which is
  **2.8× the whole tier-0 sample**. So the descriptor is cached and reused,
  and the file is opened once per thread rather than once per sample.
  Opening it per sample instead would make the sample **1889 ns** instead
  of 571 ns.
- `open` + `read` + `parse` of `/proc/<tid>/stat` costs **2800 ns**, which is
  **4.9× the whole tier-0 sample**. So the thread-id-recycling check, which
  needs the thread's start time, is amortised over 1000 samples rather than
  run every time. Running it every sample would have made a tier-0 sample
  **3.37 µs** instead of 571 ns.

## 2. Linux

### `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` — the on-CPU time

**What it measures.** The CPU time consumed by the calling thread. It
advances only while the thread is executing, and it does not advance while
the thread is descheduled, blocked, or migrated.

**Permission.** None.

**Cost.** 193–195 ns median, measured above.

**Verification.** Confirmed empirically during the premise probe: under 48
contending threads, the on-CPU time stayed flat while the wall time grew,
and the difference was exactly the run-queue wait. Under `nanosleep` the
on-CPU time did not move at all.

### `/proc/<tid>/schedstat` — the run-queue wait

**What it measures.** Three fields: the time the thread spent on CPU, the
time it spent runnable but not scheduled (the run-queue delay), and the
number of timeslices it was given.

**The second field is PACE's preemption measurement** in the procfs tier,
and it is the only unprivileged source of it on Linux. It is also a
*lower bound*: the kernel computes it from the time of queueing to the
time of dequeuing, so a thread that is queued and then migrated or
requeued loses the interval. The eBPF tier measures the interval
directly, and the difference is large on some kernels — see the eBPF
section below.

**The kernel must be collecting the scheduler statistics.** The kernel's
`sched_info_on()` decides whether the second field is populated:

| Kernel | The file reads |
|---|---|
| `CONFIG_SCHEDSTATS` compiled in | the real values, always |
| only `CONFIG_TASK_DELAY_ACCT` | `0 0 0` until `/proc/sys/kernel/task_delayacct` is 1 |
| neither | `0 0 0`, always |

The second case is the common one on a distribution kernel, which
compiles in `CONFIG_TASK_DELAY_ACCT` without `CONFIG_SCHEDSTATS` and
leaves the `delayacct` toggle off by default. The file then reads `0 0 0`
and the preemption is reported as zero — on a machine where the threads
were preempted constantly.

**A zero preemption is a measurement, and there it is a false one.** So
the SDK detects whether the kernel is collecting the statistics, and when
it is not the sample does not claim the split at all: the absence of a
measurement is reported as an absence, and never as a confident zero.

Measured on a CI runner: the preemption was reported as zero while perf's
own `sch delay` said 88 % of the wall time had been lost to the run
queue. Where the kernel does not collect the statistics, neither number
is claimed.

**And where the kernel does collect them, the counter can still be far
short.** On a CI runner with the statistics on, the counter reported
0.30 s where perf reconstructed 1.81 s. So the counter being present is
not the same as the counter being complete, and the eBPF tier exists to
measure the interval the counter loses.

**Permission.** None to read the file. The toggle needs root, but the
toggle is a machine-wide setting and not something PACE changes on a
customer's machine.

**Cost.** 244–265 ns for a `pread` of the cached descriptor; 1523–1639 ns
if the file is opened and closed each time.

**Why the descriptor is cached.** `/proc/<pid>/` files are seq files, and
reading one twice needs a `pread` at offset 0 rather than a second `read`,
or the second read returns EOF. The descriptor is opened once per thread
and re-read with `pread`, which is what makes the sample 571 ns instead of
1889 ns.

**Source.** [proc(5)](https://man7.org/linux/man-pages/man5/proc.5.html),
and the kernel's own accounting in
[`kernel/sched/stats.c`](https://elixir.bootlin.com/linux/v6.16/source/kernel/sched/stats.c).

### `/proc/<tid>/stat` — the thread's start time

**What it measures.** Field 22 is the thread's start time in clock ticks
since boot. The kernel recycles a thread id when the thread holding it
exits, so a cached descriptor for a `schedstat` file could end up pointing
at a *different* thread. Pairing the id with the start time makes the pair
unique for the life of the thread.

**Permission.** None.

**Cost.** 2734–2840 ns, so it is read once every 1000 samples.

### `perf_event_open` — the hardware cycle counter

**What it measures.** `PERF_COUNT_HW_CPU_CYCLES` is the core-cycle count
for the task, and it **stops advancing while the thread is off-CPU**, so it
is an on-CPU-only measure like the thread CPU clock. It counts the work
the core actually did, turbo included.

**`inherit = 0` is mandatory.** A per-thread counter with `inherit = 1`
absorbs the child threads' cycles, which was measured during the premise
probe: a parent thread saw a 120,914-cycle delta it had not earned. A
per-thread measurement must be a per-thread measurement.

**Permission.** `CAP_PERFMON` (Linux 5.8+), or `perf_event_paranoid < 2`
for the calling process, or root. This machine has
`perf_event_paranoid = 4`, so an unprivileged `perf_event_open` for a
hardware event returns `EACCES` — which is exactly why tier 0 must not
require it.

**Cost.** 211–217 ns for the `read`, measured as root.

**Source.** [perf_event_open(2)](https://man7.org/linux/man-pages/man2/perf_event_open.2.html).

### The scheduler's own tracepoints — the eBPF tier

**What they measure.** `sched_wakeup` and `sched_switch` are the
scheduler's own events, so counting them is a direct measurement of the
involuntary switches rather than an inference from a cumulative counter.

**They also measure the preemption WAIT, which is the tier's real
value.** The kernel's `run_delay` counter measures the same phenomenon
as the time from **queuing** to **dequeuing**:

```c
/* kernel/sched/stats.h */
static inline void sched_info_queued(struct rq *rq, struct task_struct *t) {
        if (unlikely(sched_info_on())) {
                if (!t->sched_info.last_queued)
                        t->sched_info.last_queued = rq_clock(rq);
        }
}
```

A thread that is woken, queued, and then migrated or requeued loses the
interval, because the counter is reset when the thread leaves one
runqueue and only restarts when it enters the next.

**Measured on a CI runner**, the kernel's counter reported **0.30 s** of
preemption where perf reconstructed **1.81 s** — an 80 % undercount —
while PACE's on-CPU time and its total off-CPU time both agreed with
perf. So the counter is a lower bound, and the lower bound is loose.

The eBPF tier measures the interval directly: from the thread's
`sched_wakeup`, or from the switch-out that preempted it, to the
`sched_switch` that actually ran it. Measured against the kernel's
counter for the same threads in the same window, on 48 workers over 4
CPUs:

```
the kernel's total run_delay: 87808354476 ns
the eBPF tier's total:         86437714750 ns
the eBPF measurement is -1.56 % of the kernel's counter
```

and per thread the two agree to within 0.001 %. The harness's
preemption check against perf improves from 5.78 % to **0.42 %**.

**Permission.** Attaching needs the kernel's raw tracepoints opened, which
needs `CAP_BPF` and `CAP_PERFMON`, and the kernel needs BTF for CO-RE
relocation. The object is built against the building kernel's BTF and
relocated at load time, so one binary runs anywhere with BTF.

**Raw tracepoints, not tracepoint programs.** The first implementation used
the `tracepoint/sched/sched_wakeup` section form. That form attaches
through a BPF link onto a perf event, and the kernel gates the link on
`bpf_capable()` **in the initial user namespace**. On a host with a nested
user namespace, or with a hardened BPF policy, that check fails with
`EACCES` even for uid 0 holding every capability.

The measurement was:

| | `perf_event_open` | `bpf_link_create` |
|---|---|---|
| a program with no maps | succeeds | succeeds |
| a program with maps | succeeds | **`EACCES`** |

`PERF_EVENT_IOC_SET_BPF` fails the same way, and `perf_event_paranoid` has
no effect on it: the check is a capability check, not a sysctl check. The
raw tracepoint form is opened with `BPF_RAW_TRACEPOINT_OPEN`, which is
gated differently, and it attaches. The tier therefore uses
`raw_tracepoint/` sections.

**The raw form's cost.** A raw tracepoint's arguments are the tracepoint's
*proto* arguments, and the verifier rejects a program that reads past the
last one. The protos are in the kernel's own
`include/trace/events/sched.h`, and they are:

| Tracepoint | Proto | What the arguments are |
|---|---|---|
| `sched_wakeup` | `(struct task_struct *p)` | the task that was woken |
| `sched_switch` | `(bool preempt, struct task_struct *prev, struct task_struct *next, unsigned int prev_state)` | a flag, the previous task, the next task, the previous task's state |
| `sched_process_exit` | `(struct task_struct *p, bool group_dead)` | the task that is exiting |

So for `sched_switch` the previous task is `args[1]` and the next is
`args[2]` — **not** `args[0]` and `args[1]`. An earlier version of the
program had them wrong, and the consequence was silent: it read the
`bool` as a task pointer, produced garbage thread ids, wrote nothing to
its maps, and reported itself attached the whole time.

The `bool preempt` is **not** the signal for "was this thread runnable".
The kernel sets it as `sched_mode == SM_PREEMPT`, so it is true only for
a preemption of the kernel by a higher-priority context, and not for the
ordinary timeslice preemption a contended workload produces. On a kernel
built without `CONFIG_PREEMPT` it is essentially never true.

The signal that says what is needed is the fourth argument,
`prev_state`: `TASK_RUNNING` (0) means the task was runnable when it was
switched out, which is the preemption case; any other value means it was
sleeping, which is the blocking case. So the attribution is exact rather
than inferred.

The thread id is read from the `task_struct` with a CO-RE read, which is
also what keeps the program portable across kernels whose `task_struct`
layout differs.

**Verification.** Two programs, because attaching is not working.

`tests/verify/verify_ebpf.c` attaches the tier, runs 32 contending
spinners, and reads the tier's own maps back.

`tests/verify/compare_counters.c` reads the kernel's `run_delay` and the
eBPF tier's measurement for the same threads in the same window, and
compares them. This is the check that was missing, and its absence is
why four bugs in the tier went unnoticed: **the tier's own maps were
never read back and compared against an independent source.** The
tier's verification program reported a pass while the measurement it
checked came from the fallback rather than from the tier.

The tier's numbers are also compared against perf's own reconstruction in
the accuracy harness, which is the third independent source.

Unprivileged the tier reports itself unavailable and the procfs tier
carries the measurement.

### The reference clock

`PERF_COUNT_HW_REF_CPU_CYCLES` counts at the same rate as the invariant
TSC, which is the portable way to convert nanoseconds into reference
cycles. Measured agreement between a TSC delta and an `hw_ref_cycles`
delta over the same window: **0.07 %**.

## 3. Windows

### `GetThreadTimes` — the on-CPU time

**What it measures.** The time the thread executed in kernel mode and in
user mode, as two `FILETIME` values in 100-nanosecond units. The sum is
the on-CPU time; it does not advance while the thread is descheduled.

**Permission.** `THREAD_QUERY_LIMITED_INFORMATION`, which a thread has on
itself. No elevation needed for the calling thread.

**Cost.** Not measured on this machine: no native Windows host was
available. The number is not stated rather than estimated.

**Source.** [GetThreadTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadtimes).

### `QueryThreadCycleTime` — the cycle count

**What it measures.** The CPU clock cycles the thread used, user and
kernel mode together.

**The conversion is forbidden.** Microsoft's documentation says plainly:

> "Do not attempt to convert the CPU clock cycles returned by
> QueryThreadCycleTime to elapsed time. This function uses timer services
> provided by the CPU, which can vary in implementation."

**So PACE does not.** On Windows the cycle count and the elapsed time are
two independent quantities, and the SDK reports the elapsed-time
attribution from `GetThreadTimes` and the cycle count as a separate,
unnormalised value. The `reference_hz` is 0 on Windows, and the sample
carries neither `PACE_SAMPLE_F_HAS_PREEMPTED` nor
`PACE_SAMPLE_F_HAS_BLOCKED`, because Windows has no public API that
reports the run-queue wait.

**Source.** [QueryThreadCycleTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime).

### ETW `CSwitch` — the preemption measurement, and its cost

**What it measures.** The context-switch event, with the old and new
thread ids, the priorities and the previous state. It is the only public
Windows source of the run-queue wait.

**Permission.** An NT Kernel logging session with the
`EVENT_TRACE_FLAG_THREAD` flag, which **requires administrator rights**.

**So Windows is tiered the same way as Linux, arrived at from the other
direction:** tier 0 is on-CPU versus blocked with no privilege, and the
three-way split needs an elevated session.

**Source.** [CSwitch class](https://learn.microsoft.com/en-us/windows/win32/etw/cswitch).

### What Windows cannot do

There is no public API that reports how long a thread was runnable but not
scheduled, without elevation. That is a platform limit, and PACE states it
rather than papering over it.

## 4. macOS

### `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` — the on-CPU time

**What it measures.** The CPU time consumed by the calling thread. Like
Linux's, it advances only while the thread is executing.

**Permission.** None.

**Cost.** Not measured: no native macOS host was available. The number is
not stated rather than estimated.

**Note.** `CLOCK_THREAD_CPUTIME_ID` arrived in macOS 10.12. On older
versions the Mach interface below serves the same purpose.

### `thread_info(THREAD_BASIC_INFO)` — another thread's on-CPU time

**What it measures.** `user_time` and `system_time` as `time_value_t`
values, which is the same quantity the clock above reports. Its advantage
is that it works for *any* thread, not only the calling one, which
`CLOCK_THREAD_CPUTIME_ID` does not.

**Permission.** None for the calling thread's own port. A thread's Mach
port is `pthread_mach_thread_np(pthread_self())`.

**Source.** [`thread_basic_info`](https://github.com/apple/darwin-xnu/blob/main/osfmk/mach/thread_info.h)
and [`thread_info(2)`](https://www.mit.edu/darwin/src/modules/xnu/osfmk/man/thread_info.html).

### `mach_absolute_time` — the wall clock

**What it measures.** A monotonic nanosecond counter, and unlike
`CLOCK_MONOTONIC` on some configurations, it keeps counting while the
machine is asleep.

**Cost.** Not measured on this machine.

### What macOS cannot do

Darwin exposes no per-thread run-queue wait, and the mechanisms that could
reconstruct it — `kdebug` and DTrace — are privileged and increasingly
restricted. So macOS tier 0 is on-CPU versus blocked, and the sample says
so through the flags.

## 5. The summary table

| Platform | On-CPU time | Cycle count | Run-queue wait | Three-way split without privilege |
|---|---|---|---|---|
| **Linux** | `CLOCK_THREAD_CPUTIME_ID` | `perf_event_open` | `/proc/<tid>/schedstat` | **yes** |
| **Windows** | `GetThreadTimes` | `QueryThreadCycleTime` | ETW `CSwitch` (admin) | no |
| **macOS** | `CLOCK_THREAD_CPUTIME_ID` / `thread_info` | none public | `kdebug`/DTrace (privileged) | no |

## 6. The rule this establishes

**The cycle count and the elapsed time are independent on every platform,
and on Windows the platform vendor forbids converting one to the other.**
So PACE never derives one from the other. The on-CPU time comes from the
CPU-time clock, the cycle count comes from the hardware counter, and where
a platform has no cycle counter the field is absent rather than derived.

**The run-queue wait is the scarce measurement.** Linux has it for free in
procfs. Windows needs an administrator. macOS needs a restricted
debugging interface. That scarcity is the whole reason the product exists
in the form it does: a library that gets the run-queue wait without
privilege, and marks it unavailable where it cannot be had.

## 7. What is not measured here

- **The Windows and macOS per-call costs.** No native host was available.
  The Windows backend was cross-compiled with MinGW and its test suite was
  run under Wine; the macOS backend was cross-compiled to Mach-O arm64
  against the real Mach headers. Neither was run on its own operating
  system, so no cost is claimed for either.
- **The eBPF tier's per-sample cost.** The tier adds switch counts rather
  than time, and its cost is dominated by the map lookups, which were not
  isolated.
- **Anything under a load.** These are single-threaded medians on an
  otherwise idle machine. The numbers describe the primitive, not the
  machine's behaviour under contention.
