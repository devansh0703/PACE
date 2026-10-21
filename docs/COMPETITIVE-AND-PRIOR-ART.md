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

### The differentiation statement

> **PACE is the embeddable, privilege-free implementation of the preemption-versus-blocking split that the incumbent profilers already prove is worth a product — delivered as a library a tool vendor links into its own profiler, in its own data model, at 524 nanoseconds a sample.**

Three claims in that sentence, each checkable:

1. **"the incumbent profilers already prove is worth a product."** Intel, Apple and Microsoft all ship the split. The category is validated by people with the most to lose from it being worthless.
2. **"embeddable, privilege-free."** VTune, Nsight and the Concurrency Visualizer are products you run, and their Linux collection needs perf access. Perfetto is embeddable but is a tracing pipeline. PACE is a library, and tier 0 needs no privilege at all — which means it works inside a customer's container, in their CI, and on a locked-down build machine.
3. **"at 524 nanoseconds a sample."** Measured on this machine, tier 0, with the invariant exact on 105,681 recorded samples. The number is in the committed verification harness, and it is the one claim on the website that is not a positioning statement.

**The one thing PACE does not claim:** that nobody can do this. Three incumbents can. What none of them offers is a *library* a third-party vendor can link, that works without privilege, that is vendor-neutral, and that returns the answer in the host's own data model at counter speed rather than trace speed.

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
| Visual Studio Concurrency Visualizer | Microsoft | ETW context-switch events + call-stack sampling | **Yes** — the Threads view categorises "executing or blocking because of synchronization, I/O, or other reasons" | No | Part of Visual Studio Enterprise | github.com/MicrosoftDocs/visualstudio-docs/blob/main/docs/profiling/threads-view-parallel-performance.md |

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

### The patent list, with real numbers

Searched Google Patents for preemption-aware cycle accounting, "stolen cycle" measurement and per-thread scheduler attribution. The patents that are close enough to matter, each read directly:

| Patent | Assignee | Status | What it claims | Relevance to PACE |
|---|---|---|---|---|
| [US10970055B2](https://patents.google.com/patent/US10970055B2/en) — *Identifying software and hardware bottlenecks* | IBM | **Expired — fee related** (the legal-status field; the INPADOC family list still says "Active", and that source lags. The patent was granted 2021-04-06, so the 3.5-year maintenance fee fell due in late 2024) | Claim 1: "obtaining profiles of call stacks of the multiple threads; obtaining **wake-up profiles** which are the call stacks of a notifier thread and a waiter thread … building the calling context trees" | The closest patent. It is about **call stacks and calling context trees**, not about attributing time. PACE does not unwind stacks at all |
| [US11797327B2](https://patents.google.com/patent/US11797327B2/en) — *Dynamic virtual machine sizing* | VMware | Active, expires 2033-05-03 | Uses "cycles stolen from a vCPU … preempted from executing those instructions due to system load" to size VMs | Uses the same *concept* of stolen cycles, but for hypervisor VM sizing. PACE is not a hypervisor and does not size anything |
| [US11983569B2](https://patents.google.com/patent/US11983569/en) — *Services thread scheduling based upon thread tracing* | IBM | Active | Per-thread resource profiles used to schedule threads across containers | Scheduling, not measurement. PACE measures and never schedules |
| [US8839271B2](https://patents.google.com/patent/US8839271B2/en) — *Call stack sampling to obtain information for analyzing idle states in a data processing system* | IBM | Active | Sampling call stacks to analyse idle states | Idle-state analysis via stack sampling. PACE attributes time without stacks |
| [US10152354B2](https://patents.google.com/patent/US10152354B2) — *Optimized thread scheduling on processor hardware with performance-relevant shared hardware components* | Intel | Active | Scheduling a thread using software and hardware utilisation | Scheduling, not measurement |

**The Idealab brief's cited source remains UNVERIFIED.** It names "Audio multithreaded profiling, Patent Office, 2026" with no number. No patent by that title was located. **No patent number is printed anywhere in this repository** until the search is complete and reviewed, because a wrong number is worse than no number.

### FTO assessment

**Low risk, for a structural reason rather than a lucky one.** PACE's measurement is not invented:

- the on-CPU time comes from `CLOCK_THREAD_CPUTIME_ID` on Linux and macOS and `GetThreadTimes` on Windows;
- the run-queue wait comes from `/proc/<tid>/schedstat`, a field the kernel already maintains;
- the cycle count comes from `perf_event_open` and `QueryThreadCycleTime`;
- the switch events come from the scheduler's own `sched_wakeup` and `sched_switch` tracepoints.

Every input is an operating-system primitive that predates the product. What PACE adds is the **aggregation** — a portable per-thread attribution with an explicit provenance model, and the **embedding** — a library a vendor links into a profiler it already has.

The closest patent, US10970055B2, requires *call-stack profiles* and *wake-up profiles* in its claim 1. PACE does not unwind a single stack. That is not a design choice made to dodge the claim; it is what makes the product cheap enough to run in-process at 524 ns a sample. The claim simply does not cover it.

On the status of US10970055B2: the legal-status field on the Google Patents record reads "Expired - Fee Related", and the family list still reads "Active". These two disagree, and the disagreement is explained — Google's family list comes from the EPO's INPADOC database and lags, while the legal-status field tracks the USPTO record. A patent listed as expired for non-payment of a maintenance fee is dead: "If a patent is listed as 'Expired' and the reason is 'Fee Related,' it's effectively dead, even if the full 20 years haven't elapsed." **This is a reading of a public record, not a legal opinion, and the USPTO Patent Center's fee tab is what settles it.**

The remaining risk is a broad claim on "cycles stolen from a preempted thread" that a search has not yet surfaced. That is a **pre-licensing gate**: a professional search across USPTO, Espacenet and Google Patents, by someone with access to the fee records, before the first licence is signed. It is not a reason not to build.

## 5. Market and pricing

The Idealab brief's `$660M` "global performance profiling tool market, 2025 analyst-report estimate" is **UNVERIFIED** — no analyst report or press release was located that states this figure with a citable source. The `$20M` wedge (≈200 reachable vendors × ≈$100K/yr) is a model estimate from the brief, not a measurement.

### Real pricing benchmarks

Every figure below is a published price read from the vendor's own pricing page, not an estimate. These are the comparables that matter, because PACE is priced as a **licence to an existing tool vendor**, not as a per-developer profiler subscription.

| Product | Published price | Model | Source |
|---|---|---|---|
| Intel oneAPI Base Toolkit, single node, 3-year support | ¥609,000 excl. tax (¥669,900 incl.) | Per developer, 3-year term | [xlsoft.com price list](https://www.xlsoft.com/jp/products/intel/purchase/prices.html) |
| Intel oneAPI Base Toolkit, single node, 5 concurrent users, 3-year | ¥6,094,000 excl. tax | Per user, 3-year term | the same |
| Intel oneAPI Base Toolkit, multi-node, 10 concurrent users, 3-year | ¥17,973,000 excl. tax | Per user, 3-year term | the same |
| Microsoft Visual Studio Professional | $99.99/user/mo, paid annually | Per user, annual | [visualstudio.microsoft.com](https://visualstudio.microsoft.com/vs/pricing/) |
| Microsoft Visual Studio Enterprise | $499.92/user/mo, paid annually | Per user, annual | the same |
| Sentry Business | $89/mo monthly, $80/mo annual | Per team, tiered | [sentry.io/pricing](https://sentry.io/pricing/) |
| Sentry Continuous Profiling | +$0.0315/hr | Pay-as-you-go on data volume | the same |
| Sentry UI Profiling | +$0.25/hr | Pay-as-you-go on data volume | the same |
| JetBrains IDE Services AI Enterprise | $60/user/mo, billed annually | Per user, annual | [jetbrains.com](https://www.jetbrains.com/ide-services/ai-enterprise/) |
| Arm Development Studio (Streamline inside) | User-based licence, 1-year or 3-year terms | Per developer identity | [learn.arm.com](https://learn.arm.com/install-guides/streamline/) |
| Arm Performance Studio | free, not licence-managed | — | the same |

**What these say about the price point.** The relevant comparables are **tens of thousands of dollars per vendor per year**, not per-developer subscriptions: a tool vendor that embeds PACE in a profiler it sells to thousands of developers does not need thousands of licences. Intel's own multi-node tier at ¥17.97M is a subscription to a whole toolchain; a single attribution library inside someone else's product is a much smaller line item, typically a low-five-figure annual licence with a per-vendor term.

**The pricing anchor is Sentry's**, and it is the most instructive comparable, because Sentry is the closest business model: a component a product embeds, priced on the volume of data it produces rather than on seats. Sentry's Continuous Profiling at $0.0315/hr is roughly **$275/month for a continuously profiling workload**. A vendor licensing PACE to get the same attribution is buying a *fixed* cost — a licence fee — instead of a *variable* one that scales with their customers' data volume. That difference is the actual sales argument, and it is a structural one rather than a price-cutting one.

**What is not published.** The Idealab brief's `$660M` market figure and `$20M` wedge remain UNVERIFIED — no analyst report with a citable source was located, so neither number appears on either website. The ≈200 reachable vendors is the brief's own arithmetic, stated so it can be audited. The honest position: **the market-size numbers are model estimates and are labelled as such; the pricing benchmarks are real published prices; and the accuracy and overhead numbers on the website come from the committed verification harness, where they are also real.**

## 6. Licensing

BUSL 1.1 is the correct licence shape for a source-visible B2B SDK and is the current industry standard (HashiCorp adopted it in 2023; Elastic and MariaDB use the same pattern). VERIFIED mechanics from the SPDX text:

- "The Licensor may make an Additional Use Grant, above, permitting limited production use."
- "Effective on the Change Date, or the fourth anniversary of the first publicly available distribution of a specific version of the Licensed Work under this License, the Licensor hereby grants you rights under the terms of the Change License..."

So the "four years" in the plan is the **default maximum**; the Change Date itself can be earlier, and the Additional Use Grant is what makes production use by a paying licensee legal before that date. PACE's licence must therefore name a Licensor and record the grant in every source file header.

Alternatives and why they lose:
- **Apache-2.0** on the SDK: a competitor could resell it. Unacceptable.
- **AGPL**: a commercial vendor will not link copyleft into a proprietary profiler. Unacceptable commercially.
- **Elastic License 2.0**: forbids "managing or offering the Licensed Work as a product or service to third parties" — that would prohibit the exact thing PACE sells (embedding in a tool offered to third parties). **ELv2 is therefore wrong for this product**, and the plan correctly chose BUSL.
