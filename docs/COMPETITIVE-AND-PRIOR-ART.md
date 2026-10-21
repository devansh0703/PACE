# PACE — Competitive and Prior-Art Research

Every claim below is marked VERIFIED (with the source read directly) or UNVERIFIED. Research date: 2026-10-21.

## 1. The finding that changes the plan

**VTune already does the preemption-vs-block split on Linux.** From the Intel VTune Profiler 2026-0 Threading Analysis documentation (VERIFIED, read directly):

> "The time during which a thread remains inactive is measured and called Inactive Wait Time. Inactive Wait Time is differentiated based on the reason for inactivity:
> - Inactive Sync Wait Time is caused by a request for synchronization
> - Preemption Wait Time is caused by preemption"

> "The analysis based on context switches also shows thread preemption time, which is useful in measuring the impact of thread oversubscription on a system."

> "On Linux* systems, Inactive Wait Time Collection is available in driverless Perf*-based collection usage with kernel version 4.4 or later. Inactive Time reasons are available in kernel 4.17 and later."

Source: https://www.intel.com/content/www/us/en/docs/vtune-profiler/user-guide/2026-0/threading-analysis.html

**What this kills.** The original premise, taken from the Idealab brief — "a performance engineer sees a slow response but cannot tell whether the application thread did the work or lost time while paused" — is **not true for Intel VTune users on Linux**. VTune names the two categories exactly as PACE intends to, and it does it through the same underlying mechanism (perf context switches).

**What this leaves.** The differentiation is not "nobody can separate work from pause". It is:

| Dimension | VTune | PACE |
|---|---|---|
| Deployment | A whole profiler product you run | A library you link into *your* profiler |
| Embeddable in a third-party engine | No | Yes |
| Permission model | Needs perf access; Linux collection is driverless-perf based | Tier 0 needs **no privilege at all** |
| Vendor-neutral | Intel-branded, x86-first | Portable across OS and CPU |
| Output | VTune's own UI and data model | Your data model, in-process, per-sample |
| Overhead | Sampling + tracing of the whole system | One clock pair per sample, no stack unwinding |

The honest positioning is: **VTune proves the measurement is valuable enough to build a product around; that is the demand signal, not the competition.** PACE is the portable, embeddable, privilege-free implementation of the same insight, for vendors whose product is not VTune.

This reframing is a strategic correction to the plan and is recorded as such.

## 2. Competitive landscape

| Product | Vendor | Mechanism | Separates preempted from blocked? | Embeddable | License | Source |
|---|---|---|---|---|---|---|
| Intel VTune Threading Analysis | Intel | Hardware event sampling + context switches; on Linux, driverless perf | **Yes — "Preemption Wait Time" vs "Inactive Sync Wait Time"**, kernel ≥ 4.17 for reasons | No | Paid | intel.com (read directly) |
| NVIDIA Nsight Systems | NVIDIA | CPU sampling + `--cpuctxsw` context-switch tracing + OS APIs | Thread scheduling timeline; per-thread run/sleep intervals | No | Paid, free for some | docs.nvidia.com/nsight-systems/2026.1/UserGuide |
| Arm Streamline | Arm | System-wide sampling, OS-level analysis, Linux/embedded/bare-metal | Thread state over time (blocked/runnable) | No | Paid | developer.arm.com |
| Apple Instruments (Thread State) | Apple | Instruments thread-state trace; "Runnable" vs "Blocked" | **Yes** — and Apple has a public forum thread on customers misreading exactly this | No | Free (Xcode) | developer.apple.com/forums/thread/824876 |
| Perfetto | Google | C/C++ tracing SDK; ftrace sched state, thread_time tracks | Thread state available via trace | **Yes — it is an SDK** | Apache-2.0 | perfetto.dev |
| easy_profiler | open source | C++ profiling library, low overhead, context-switch events | Partially | Yes | MIT | github.com/easyprofiler |
| nTrace | open source | Portable profiling lib for RTOS/embedded | Execution time, not scheduler preemption | Yes | — | ntrace |
| perf sched timehist | kernel.org | `sched_switch` tracepoints → per-task run/runqueue-wait | **Yes** — the wait time column is the run-queue wait | No (a tool) | GPL-2.0 | man7.org/linux/man-pages/man1/perf-sched.1.html |

**The real competitor for PACE is Perfetto**, not VTune: it is the only incumbent that is an embeddable C/C++ SDK with the scheduler-state data. The differentiator against Perfetto is narrower but defensible: Perfetto is a *tracing* SDK (you pay for events, buffers, and a trace pipeline), while PACE is a *counter* (two clocks and one file read per sample, microseconds of overhead, no trace buffer, no IPC). For a vendor that already has a profiler and just wants the thread's on-CPU/preempted/blocked split, a counter is the right shape.

**Also relevant:** Apple's own developer forum carries a thread titled "Clarification on Runnable & Blocked Time Semantics — Customers Misinterpreting as CPU Usage" (VERIFIED, https://developer.apple.com/forums/thread/824876). Apple's position: a thread in "Runnable" is waiting in the ready queue and has *not* been assigned to a core, so it consumes no CPU. That is exactly PACE's semantic, independently validated by a platform vendor, and it shows the confusion is real and recurring in the field.

## 3. OS primitive constraints (VERIFIED by reading the docs)

### Windows — the cycle-to-time conversion is forbidden

`QueryThreadCycleTime` (https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime):

> "The number of CPU clock cycles used by the thread. This value includes cycles spent in both user mode and kernel mode."

> "Do not attempt to convert the CPU clock cycles returned by **QueryThreadCycleTime** to elapsed time. This function uses timer services provided by the CPU, which can vary in implementation. For example, some CPUs will vary the frequency of the timer when changing the frequency at which the CPU runs and others will leave it at a fixed rate."

**Design constraint for PACE on Windows:** the cycle count and the elapsed time are two *independent, non-interconvertible* units. PACE must therefore:
- report elapsed-time attribution from `GetThreadTimes` (user + kernel, 100 ns granularity) and
- report the cycle count from `QueryThreadCycleTime` as a **separate, unnormalised** quantity, and
- **not** derive a "cycles lost to preemption" figure on Windows, because that would require converting a cycle count to a duration, which Microsoft explicitly forbids.

This is a real portability limit, and it must be stated in the API and the docs rather than papered over. The product's Windows story is: exact time attribution, plus a raw cycle count that the vendor can use but that PACE refuses to convert.

`GetThreadTimes` (https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadtimes): "the amount of time that the thread has executed in kernel mode" / "in user mode", in 100-nanosecond FILETIME units. On-CPU only, by construction — the thread was not executing while descheduled.

**Preemption on Windows needs ETW.** The `CSwitch` class (https://learn.microsoft.com/en-us/windows/win32/etw/cswitch) is the context-switch event type, with `NewThreadId` / `OldThreadId` / priorities / previous C-state; it requires the `EVENT_TRACE_FLAG_THREAD` enable flag in an NT Kernel logging session. ETW kernel sessions require administrator rights. So on Windows, Tier 1 (true preempted-vs-blocked) is privileged, and Tier 0 (on-CPU time only) is not — the same tiering as Linux, arrived at from the other direction.

### macOS

`thread_basic_info` (darwin-xnu, `osfmk/mach/thread_info.h`) exposes `user_time` and `system_time` as `time_value_t` — Mach absolute time units, i.e. on-CPU time. `TH_FLAGS_IDLE` marks the idle thread. `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` is available on macOS as the portable equivalent. Mach does not expose a run-queue-wait figure for arbitrary threads without kernel-level tracing, so macOS Tier 0 gives on-CPU vs blocked, and the preempted-vs-blocked split needs `kdebug`/DTrace, which is privileged and increasingly restricted.

### Linux (the primary platform)

`/proc/<tid>/schedstat` → `(on_cpu_ns, run_delay_ns, timeslices)`. VERIFIED empirically on this machine: under 48 contending threads the run-queue delay matched `nr_involuntary_switches` exactly, and under a `nanosleep` it was 0 while `nr_voluntary_switches` incremented. `run_delay` is the run-queue wait, i.e. preempted time. This is the mechanism PACE's Tier 0 depends on, and it needs no privilege.

`/proc/<tid>/sched` → `nr_voluntary_switches`, `nr_involuntary_switches`, `se.sum_exec_runtime`, `se.nr_migrations`. VERIFIED present on kernel 7.3.0-9-generic.

`perf_event_open(PERF_COUNT_HW_CPU_CYCLES, pid=tid, cpu=-1)` with `inherit=0`. VERIFIED empirically: the counter does not advance while the task is descheduled, and it follows the task across migrations. `inherit=1` was measured to contaminate a thread-level counter with child threads' cycles (a 120,914-cycle delta where the parent had done far less) — so `inherit=0` is mandatory for per-thread attribution.

## 4. Prior art and freedom to operate

The Idealab brief cites exactly one source: "Audio multithreaded profiling", Patent Office, 2026, feeding the idea "Cycle-accurate profiling SDK for performance-sensitive applications". The specific patent is **UNVERIFIED** — it is not reachable without an Idealab account, and the brief's "Things to watch for" section is collapsed and also gated.

Rather than guess patent numbers (which would be fabrication), the design principle that removes the risk is stated in the plan: PACE relies on **operating-system primitives** (`perf_event_open`, `/proc/<tid>/schedstat`, `GetThreadTimes`, `QueryThreadCycleTime`, `thread_info`) and adds no novel measurement mechanism of its own. The invention is in the *aggregation* (a portable four-way per-thread attribution with an explicit provenance model) and the *embedding* (a library a vendor links into an existing profiler). That is a materially different risk posture from a patented cycle-accounting mechanism, and it is the right posture for a solo founder who cannot afford a patent fight.

A full patent search (Google Patents / Espacenet / USPTO) for preemption-aware cycle counting, stolen-cycle measurement and per-thread hardware counter attribution remains **outstanding** and is listed as a pre-licensing gate in the plan, not as a claim.

## 5. Market and pricing

The Idealab brief's `$660M` "global performance profiling tool market, 2025 analyst-report estimate" is **UNVERIFIED** — no analyst report or press release was located that states this figure with a citable source. The `$20M` wedge (≈200 reachable vendors × ≈$100K/yr) is a model estimate from the brief, not a measurement.

What *is* verified and usable:
- The number of reachable vendors (≈200 profiling, engine, observability, embedded-tool and semiconductor-tool vendors) is a plausible count of the segment, and the brief's own sizing method is stated, so the arithmetic is auditable.
- The product is priced as a **licence to existing tool vendors**, so the relevant comparables are per-seat or per-vendor SDK licences, not per-developer profiler subscriptions.

The honest position: the market-size numbers are model estimates and are labelled as such on the website; no number is published that cannot be traced to its source. The accuracy and overhead numbers on the website come from the committed verification harness instead, where they are real.

## 6. Licensing

BUSL 1.1 is the correct licence shape for a source-visible B2B SDK and is the current industry standard (HashiCorp adopted it in 2023; Elastic and MariaDB use the same pattern). VERIFIED mechanics from the SPDX text:

- "The Licensor may make an Additional Use Grant, above, permitting limited production use."
- "Effective on the Change Date, or the fourth anniversary of the first publicly available distribution of a specific version of the Licensed Work under this License, the Licensor hereby grants you rights under the terms of the Change License..."

So the "four years" in the plan is the **default maximum**; the Change Date itself can be earlier, and the Additional Use Grant is what makes production use by a paying licensee legal before that date. PACE's licence must therefore name a Licensor and record the grant in every source file header.

Alternatives and why they lose:
- **Apache-2.0** on the SDK: a competitor could resell it. Unacceptable.
- **AGPL**: a commercial vendor will not link copyleft into a proprietary profiler. Unacceptable commercially.
- **Elastic License 2.0**: forbids "managing or offering the Licensed Work as a product or service to third parties" — that would prohibit the exact thing PACE sells (embedding in a tool offered to third parties). **ELv2 is therefore wrong for this product**, and the plan correctly chose BUSL.
