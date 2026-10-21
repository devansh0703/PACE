# PACE — Preemption-Aware Time Attribution Engine

Two products from one idea (Idealab #1352567): a licensed C/C++ SDK and an open-source trace analyzer, each with its own website.

> **Status after the CEO review (2026-10-21).** The measurement is built and
> **empirically verified**: the accuracy harness passes nine independent
> cross-checks, and the decomposition invariant holds exactly on every
> recorded sample. The CEO review found the plan's *differentiation premise*
> false — Intel VTune already separates preemption from blocking on Linux —
> and reframed the positioning accordingly. The measurement is unchanged; the
> claim around it is corrected. See `docs/CEO-REVIEW.md`.

## Implementation plan

### 0. What we are building and why

A performance engineer watching a game engine sees a 40 ms frame. The wall clock says 40 ms. `CLOCK_THREAD_CPUTIME_ID` says 12 ms. **Where did the other 28 ms go?** It was either the scheduler taking the core away, or the thread waiting on a lock or on I/O. Those need opposite fixes, and the tools that report the symptom do not separate the cause.

PACE is a small, embeddable library that answers it per thread, per sample, on Linux, Windows and macOS, and a companion analyzer that turns the samples into an attribution report.

**Measured proof the gap is real and the method works** (this machine, i7-13620H, kernel 7.3.0-9-generic; see `pace-sdk/docs/PREMISE-VERIFICATION.md`):

48 spinners on 16 hardware threads, one thread instrumented, 310 ms of wall time:

| Quantity | Measured |
|---|---|
| Wall time | 310,446,432 ns |
| On-CPU time (`CLOCK_THREAD_CPUTIME_ID`) | 100,153,447 ns |
| **Run-queue wait (involuntary preemption)** | **210,321,285 ns (67.8 % of wall)** |
| Blocked on lock/IO | ~0 (the thread never blocked) |
| `nr_involuntary_switches` | 134 |
| `nr_voluntary_switches` | 0 |
| HW core cycles consumed | 235,103,222 |
| Decomposition `wall = on_cpu + preempted + blocked` | **exact** |

**The corrected positioning.** Intel VTune's Threading Analysis already separates "Preemption Wait Time" from "Inactive Sync Wait Time" on Linux, through perf-based context switches (VTune 2026-0 user guide, read directly). So the claim is *not* that nobody can do this. The claim is:

| | VTune | Perfetto | PACE |
|---|---|---|---|
| A whole product you run | yes | no | no — a library |
| Embeddable in a third-party engine | no | yes | yes |
| Needs perf access / privilege | yes on Linux | varies | **no** (tier 0) |
| Vendor-neutral | Intel-branded | Google, but neutral | yes |
| Output | VTune's own model | a trace | **your** model, in-process |
| Cost model | whole-system profiling | trace buffers, IPC | 529 ns per sample |

*VTune proves the measurement is worth a product. It cannot be linked into your engine, it cannot run without perf access, and it is x86-first. PACE is the portable, embeddable, privilege-free implementation of the same insight, for vendors whose product is not VTune.*

Perfetto is the sharper competitor — the only incumbent that is also an embeddable C/C++ SDK. The differentiator is that a **counter** is the right shape for a vendor that already has a profiler: no trace buffer, no IPC, no background writer.

### 1. Architecture and the central design decision

The central problem: **cycles lost to preemption can never be measured directly**, because the hardware counter is not running while the thread is off-CPU. Every implementation must estimate them. PACE estimates them from a measured, portable frequency anchor rather than a guess.

The anchor: the x86 invariant TSC ticks at a constant rate independent of core frequency, and `PERF_COUNT_HW_REF_CPU_CYCLES` counts at the same rate. Measured on this machine: TSC delta 133,778,748 vs `hw_ref_cycles` 133,689,928 over the same window — **0.07 % agreement**.

**The correction the CEO review forced:** the verified core is the *time* attribution, not the cycle attribution. The on-CPU time agrees with the kernel's own cfs accounting to **0.0030 %** and with perf's run-time column to **0.0415 %**. There is no equivalent ground truth for a "cycles lost to preemption" number, because it is a frequency model on top of the time attribution. The product therefore leads with **time attribution**; cycles are a labelled convenience unit, exactly as the `pace_sample` struct already treats them.

Three tiers, each an addition to the one below:

**Tier 0 — always available, zero privilege.**
`CLOCK_THREAD_CPUTIME_ID` + `/proc/<tid>/schedstat` + `/proc/<tid>/sched` (Linux), `GetThreadTimes` + `QueryThreadCycleTime` (Windows), `thread_info` (macOS). The complete four-way decomposition in nanoseconds with **no elevated privilege**, so PACE works inside containers, in CI, and on locked-down customer machines. Cycles are derived by multiplying nanoseconds by the measured reference frequency.

**Tier 1 — optional, needs `CAP_PERFMON` or `perf_event_paranoid <= 1`.**
`PERF_COUNT_HW_CPU_CYCLES` gives *measured* core cycles — the actual work the CPU did, turbo included — instead of a frequency-derived estimate. `PERF_COUNT_HW_REF_CPU_CYCLES` gives the reference anchor. This host has `perf_event_paranoid=4`, so unprivileged `perf_event_open` for hardware events returns `EACCES`; the SDK must not require it.

**Tier 2 — optional, the eBPF scheduler tier.**
A CO-RE BPF program attached to `sched_wakeup` and `sched_switch`, counting the scheduler's own events per thread. It gives a direct measurement of involuntary switches that does not depend on `/proc` being readable, and it works where the procfs tier is restricted. It needs BTF and privilege to attach, so it is an *addition*, never a requirement. **Verified on this machine: the program loads and attaches as root (`LOAD OK` / `ATTACH OK`), and the SDK falls back cleanly when it cannot.**

**The honest claim:** PACE reports *reference cycles* exactly in tier 0 and *core cycles* exactly in tier 1. Core cycles in tier 0 are an estimate labelled as such. The `pace_sample` struct carries `source` and the `PACE_SAMPLE_F_HAS_*` flags, so no consumer can mistake an estimate — or an absent measurement — for a measured one.

```
                      pace-session (C API)
                             │
        ┌────────────────────┼────────────────────┐
        │                    │                    │
   backend              backend              backend
   linux                windows               macos
   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐
   │ tier0 procfs │   │ tier0 times  │   │ tier0 thread │
   │ tier1 perf   │   │ tier1 etw    │   │     _info    │
   │ tier2 ebpf   │   │              │   │              │
   └──────┬───────┘   └──────┬───────┘   └──────┬───────┘
          └──────────────────┼──────────────────┘
                    pace_sample (C struct)
                    { struct_size,
                      wall_ns, on_cpu_ns,
                      preempted_ns, blocked_ns,
                      core_cycles, ref_cycles,
                      core_per_ref_ratio, source, flags }
                             │
              ┌──────────────┴──────────────┐
              │                             │
     vendor's profiler                pace_trace (JSONL)
     (in-process callback)                    │
                                    pace-analyzer (Rust)
                                               │
                              report: terminal + self-contained HTML
```

### 2. Product 1 — `pace-sdk` (commercial, licensed)

Repository: `pace-sdk/`. C11 + C++17. CMake build with install/export.

**Public C API** (`include/pace/pace.h`), ABI-stable, opaque handles only. `pace_sample` and `pace_config` both carry a `struct_size` so the ABI can grow without breaking a licensee.

```c
typedef struct pace_session pace_session;
typedef struct pace_thread  pace_thread;

typedef enum {
  PACE_SOURCE_UNKNOWN = 0,
  PACE_SOURCE_CLOCK   = 1,   /* tier 0: nanosecond + reference-cycle derived */
  PACE_SOURCE_PERF    = 2,   /* tier 1: measured core cycles */
} pace_source;

typedef struct pace_sample {
  uint32_t struct_size;
  uint32_t _reserved0;
  uint64_t wall_ns, on_cpu_ns, preempted_ns, blocked_ns;
  uint64_t core_cycles, ref_cycles;
  double   core_per_ref_ratio;
  uint64_t involuntary_switches, voluntary_switches;
  pace_source source;
  uint32_t flags;   /* PACE_SAMPLE_F_HAS_PREEMPTED, _HAS_BLOCKED, ... */
  uint32_t _reserved1;
} pace_sample;

pace_status pace_session_create(const pace_config*, pace_session**);
void        pace_session_destroy(pace_session*);
pace_status pace_session_sample(pace_session*, pace_thread*, pace_sample*);
pace_status pace_session_sample_self(pace_session*, pace_sample*);
pace_status pace_session_register_thread(pace_session*, pace_thread**);
pace_status pace_session_set_observer(pace_session*, pace_observer_fn, void*);
pace_status pace_session_stats(const pace_session*, pace_stats*);
```

**Threading contract**, stated in the public header: which calls are thread-safe, that an observer must not re-enter the session, that a `pace_thread` is owned by the thread that registered it, and that `pace_session_destroy` invalidates every handle it returned.

**C++17 wrapper** (`include/pace/pace.hpp`): `PaceSession` RAII, `pace::describe()` for a one-line summary.

**Per-thread state.** A `pace_thread` handle owns the cumulative counters and the cached file descriptors. A `thread_local` fast pointer avoids the registry lookup on the hot path. The registry is a fixed 4096-slot array under a mutex, used only at registration and teardown.

**Hot path cost** (`pace_session_sample`), measured over 5000 calls:

| Configuration | Median | p99 |
|---|---|---|
| Tier 0, no privilege | **529 ns** | 1.19 µs |
| Tier 1, hardware counters | **2.01 µs** | 2.09 µs |
| With voluntary/involuntary switch counts | 5.81 µs | 11.77 µs |

The switch counters are opt-in (`pace_config.collect_switch_counters`) because the run-queue delay already carries the preemption/blocking split; reading `/proc/<tid>/sched` per sample costs about 4 µs and dominated the path.

**Files:**

```
pace-sdk/
  CMakeLists.txt                 # presets, install/export, SOVERSION, package config
  cmake/paceConfig.cmake.in
  include/pace/pace.h            # stable C ABI + threading contract
  include/pace/pace.hpp          # C++17 RAII
  src/pace_session.c             # lifecycle, config, sample hot path, stats
  src/pace_linux.c               # tier 0 procfs, tier 1 perf, TSC detection
  src/pace_trace.c               # JSONL trace writer
  src/pace_version.c
  src/bpf/pace_sched.bpf.c       # tier 2: the CO-RE BPF program
  src/bpf/pace_bpf.c             # tier 2: loader and map reader
  src/bpf/pace_bpf.h
  src/bpf/CMakeLists.txt          # vmlinux.h from BTF, clang BPF target, bpftool
  tests/unit/test_main.c         # 76 checks
  tests/verify/premise_probe.c   # the empirical premise probe
  tests/verify/pace_verify.c     # the in-process accuracy verifier
  tests/verify/verify_accuracy.py# the harness that writes results/accuracy.json
  bench/bench_sample.cc          # per-sample overhead
  bench/bench_instrumented.cc    # end-to-end overhead
  examples/thread_attribution.c  # the demo
  examples/pace_record.c         # record a real trace
  examples/pace_overhead.c       # measure the overhead
  examples/pace_bpf_probe.c      # the eBPF tier's state
  docs/PREMISE-VERIFICATION.md
  docs/ACCURACY.md
  docs/INTEGRATION.md
  LICENSE                        # BUSL 1.1
  LICENSING.md
```

**Licensing.** Business Source License 1.1 — `Licensor: PACE`, `Change Date:` 4 years from first publication, `Change License: Apache-2.0`, `Additional Use Grant:` production use by a holder of a current paid license. Elastic License 2.0 is *wrong* for this product: it forbids "offering the Licensed Work as a product or service to third parties", which is exactly what a tool vendor does with it. AGPL is commercially unacceptable to a vendor linking it into a proprietary profiler. The per-file BUSL header template goes in `LICENSING.md`.

**Packaging.** `find_package(pace)` via `install(EXPORT)` + `CMakePackageConfigHelpers`; vcpkg port and a ConanCenter recipe; GitHub Actions CI on Linux/Windows/macOS with ccache.

### 3. Product 2 — `pace-analyzer` (open source, Apache-2.0)

Repository: `pace-analyzer/`. Rust 2021, one crate with a library and a `pace` binary. Open source on purpose: it is the proof of accuracy, the adoption funnel, and the thing a prospective licensee evaluates before paying.

**Input formats** (all parsed, none invented):

| Format | Parser | Source |
|---|---|---|
| PACE JSONL | `serde_json` | our own |
| Chrome Trace Event | `serde_json` | `https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU` |
| `perf sched timehist` text | hand-written column parser | `perf-sched(1)` |
| `perf trace` CSV | hand-written column parser | `perf-trace(1)` |

The plan originally listed `prost` and the Perfetto `TracePacket` protobuf. That was changed after the research: Perfetto is a large C++ build and its schema is a poor fit for a per-thread ledger, while the two formats that matter (PACE and Chrome) are JSON. The hand-written CSV and text parsers are ~40 lines each against a dependency, and they remove the catastrophic-backtracking risk that a `regex` would introduce on untrusted trace files.

**Honesty rules, which are the point of the tool:**
- A line that cannot be read is **counted and skipped**, not fatal. A trace captured under stress is routinely truncated; the readable part is still worth analysing. The count appears in the report.
- A trace whose format carries **no** preemption/blocking split (a plain Chrome trace) is reported as "no attribution" rather than showing a confident 0 % preempted, and no findings are raised against it.
- `pace verify` **fails on an empty trace**. The invariant holds vacuously there, and a vacuous pass is a false green in a CI gate.

**Attribution model:** per thread and per phase, bucket wall time into on-CPU, preempted and blocked; rank findings by severity against configurable thresholds; raise a process-level finding when the run as a whole lost most of its wall time to preemption.

**Outputs:**
1. Terminal report — per-thread table, per-phase breakdown, ranked findings.
2. A **self-contained HTML report** — one file, no network, the split drawn as inline SVG. The test suite asserts the report contains no `http://`.

**CLI:**

```
pace analyze <trace> [--format pace|chrome|perfsched|perftrace]
                 [--output report.html] [--json]
                 [--preempted-threshold 25] [--blocked-threshold 25]
pace inspect <trace>          # per-thread raw table
pace convert <in> --to <fmt>  # translate between formats
pace verify <trace>           # re-check the decomposition invariant
```

**Adopted crates** (no hand-rolling where a library exists): `clap` 4.6.7 (CLI), `serde` + `serde_json` (JSON), `thiserror` 2.0.21 (errors), `anyhow` (binary). Criterion 0.8.2 for the benches.

### 4. Benchmark and verification suite

Adopted harnesses: **Google Benchmark v1.10.0** (C++, for the SDK) and **Criterion 0.8.2** (Rust, for the analyzer). Both are the current standard and both handle repetitions and aggregation correctly.

**Workloads** (all real, all open source): `pbr` (MIT, a real multi-threaded path tracer — present on this machine), SQLite (public domain, real lock contention), `llvm-test-suite` (Apache-2.0 with LLVM exceptions), `rt-tests` sched-migrate / hackbench (GPL-2.0, invoked as an external binary as independent ground truth), `stress-ng` (GPL-2.0, external binary), `sysbench` (GPL-2.0).

**Ground truth strategy** — independence matters, because our own implementation also reads the kernel's accounting, so perf alone is not an independent check.

| Quantity | Independent ground truth | Measured agreement |
|---|---|---|
| on-CPU time of thread T | `/proc/<tid>/schedstat` cfs accounting | **0.0030 %** |
| on-CPU time of thread T | `/proc/<tid>/stat` utime+stime (10 ms tick) | 0.0132 % |
| on-CPU time of thread T | `perf sched timehist` "run time" column | **0.0415 %** |
| off-CPU time (preempted + blocked) | `perf sched timehist` "wait time" column | 2.2721 % |
| preempted time | `perf sched timehist` "sch delay" column | 2.3112 % |
| wall time | `CLOCK_MONOTONIC` read independently | 0.0031 % |
| the decomposition invariant | every sample of a recorded trace | **exact on 103,553 samples** |

The preempted-vs-sch-delay gap is **definitional, not a defect**: the kernel's `run_delay` accumulates run-queue wait continuously, while perf can only measure the wakeup-to-running interval where it captured a `sched_wakeup` tracepoint. The off-CPU row is the tighter cross-check and passes at 2.27 %.

**Accuracy acceptance criteria** (hard numbers, checked by the harness in CI):

| Metric | Threshold | Measured |
|---|---|---|
| on-CPU vs the kernel's cfs accounting | < 0.5 % | 0.0030 % |
| on-CPU vs `perf sched` run time | < 2 % | 0.0415 % |
| off-CPU vs `perf sched` wait time | < 5 % | 2.2721 % |
| preempted vs `perf sched` sch delay | < 10 % | 2.3112 % |
| wall vs an independent `CLOCK_MONOTONIC` | < 1 % | 0.0031 % |
| the decomposition invariant | exact | exact on 103,553 samples |
| `pace_session_sample` overhead, tier 0 | < 3 µs median | 529 ns |
| `pace_session_sample` overhead, tier 1 | — | 2.01 µs |

**Method.** N ≥ 30 independent runs per configuration; median and p95 reported; `taskset` for core pinning; the `intel_pstate` governor and turbo state recorded in a machine-readable environment artifact; the Google paper *"Rigorous Benchmarking in C++"* rules applied. The result artifact is `pace-sdk/results/accuracy.json`.

### 5. Websites

Two separate sites, one per product, in `websites/`.

- `websites/pace-sdk/` — the commercial product. Audience: product leaders at developer-tool firms. Story: the gap, the proof, the integration, the evaluation licence.
- `websites/pace-analyzer/` — the open-source product. Audience: performance engineers. Story: the tool, the formats it reads, the report it produces.

**Stack.** Astro 7.4.1, deployable to GitHub Pages. Vanilla CSS. Self-hosted fonts via `@fontsource/ibm-plex-mono` and `@fontsource/ibm-plex-sans` 5.3.0. Icons via `@phosphor-icons/web` 2.1.2.

**Design direction — "Wire Terminal"**, a field-instrument aesthetic:

- Ground `#131311` (warm charcoal), accent `#e8a33d` (amber). Saturated colour only for severity.
- IBM Plex Mono for structure and all numerics; IBM Plex Sans for prose.
- Phosphor icons, light weight. No Lucide, no emoji.
- Radii 3 px. Elevation from borders and contrast, never soft shadows.
- Banned: purple/teal/blue gradients, glassmorphism, badge pills above a hero, all-caps mono kickers above every heading, three-identical-card layouts, numbered `01/02/03` step lists, stat banner rows, coloured left borders, pulsing dots, emoji icons.

**Content.** Every claim on both sites is backed by a number produced by the benchmark suite, with the measurement environment attached. The SDK site shows the contended-run example — 654 ms on-CPU, 1.22 s preempted, 131 ms blocked, 60.8 % preempted — because it is the clearest possible statement of the problem, and it is real.

### 6. Delivery order

Each step ends with something runnable and committed.

1. `pace-sdk`: CMake skeleton, C API header, clock + TSC detection, Linux tier-0 backend, tier-1 perf backend, sample hot path, unit tests. **DONE.**
2. `pace-sdk`: the eBPF tier. **DONE.**
3. `pace-sdk`: JSONL trace writer, C++ wrapper, examples. **DONE.**
4. `pace-sdk`: Windows and macOS backends. **NOT STARTED** — the CEO review scoped this honestly: Windows tier 0 is on-CPU vs blocked (no privilege), tier 1 is + ETW context switches (administrator), and macOS tier 0 is on-CPU vs blocked. Neither can do the three-way split without privilege, and the plan now says so rather than implying parity.
5. `pace-analyzer`: crate, parsers, attribution model, terminal report, HTML report, `verify`, benches. **DONE.**
6. Benchmarks and verification: the accuracy harness with real ground truth, results committed. **DONE.**
7. Websites: both sites, with the real measured numbers. **DONE.**
8. Licensing and packaging: BUSL text, vcpkg port, Conan recipe, `find_package` docs, integration guide. **NOT STARTED.**
9. CI: GitHub Actions on Linux/Windows/macOS, running the accuracy harness and committing its artifact. **NOT STARTED.**

### 7. Explicitly not in scope

- Sampling profilers (stack unwinding, symbolisation) — that is the vendor's product, not ours. PACE supplies the time attribution those profilers cannot make.
- GPU, cache and memory-event attribution — different counters, different product.
- eBPF-based continuous production profiling — a different buyer with different requirements.
- A hosted SaaS offering — the brief's vehicle is licensing to tool vendors.
- Windows and macOS three-way splits without privilege — documented as a limitation rather than faked.
- A streaming analyzer mode for traces above ~10 M samples — deferred with a documented limit.

### 8. Binding decisions

- **D1**: Three tiers, each an addition to the one below, and none of them required. A product that requires root is unshippable inside a customer's engine.
- **D2**: Reference cycles are the portable unit; core cycles are the local unit. Both are reported, always labelled.
- **D3**: Preempted cycles are an explicit *estimate*, computed from a measured frequency, and the analyzer shows the estimate's inputs.
- **D4**: `pace-analyzer` is Apache-2.0 open source; `pace-sdk` is BUSL 1.1 with a paid-licence use grant. The open analyzer is the proof and the funnel.
- **D5**: PACE integrates into the vendor's profiler in-process through a C callback/observer API; the file format is a secondary path.
- **D6**: No hand-rolled algorithms where a maintained library exists. **Amended by the CEO review**: the analyzer's CSV and text parsers are hand-written, because a `regex` over untrusted trace files carries a catastrophic-backtracking risk that a dependency-free 40-line parser does not, and the report must be a single offline file.
- **D7**: Every published accuracy number comes from the committed verification harness, with the environment artifact attached.
- **D8** (new, from the CEO review): The product's verified core is the **time** attribution. The cycle figures are a labelled convenience. The naming, the API and the website follow the struct.
- **D9** (new, from the CEO review): The differentiation is *portable, embeddable, privilege-free, in-process* — never "nobody can do this".

### 9. Risks and the plan for each

| Risk | Impact | Plan |
|---|---|---|
| `perf_event_paranoid=4` on customer machines | Tier 1 unavailable | Tier 0 is the product's floor; tier 1 is an upgrade, never a requirement |
| "Cycles lost to preemption" is an estimate | Over-selling risk | The time attribution is exact; the cycle conversion is a documented frequency model, and D8 makes the time attribution the headline |
| VTune and Apple Instruments already split preemption from blocking | **The differentiation premise was false** | **Corrected.** The positioning is portable, embeddable, privilege-free and in-process, and both sites name the incumbents |
| Perfetto is an embeddable SDK | The closest competitor | PACE is a counter, not a trace: no buffer, no IPC, no background writer, 529 ns a sample |
| Incumbents add this themselves | Market risk | VTune cannot be linked into a third-party game engine, and the licensed artefact is the portable implementation plus the cross-vendor evidence |
| Prior art / patents in cycle-accurate profiling | FTO risk | A full patent search is a **pre-licensing gate**, not a pre-building one. The design relies on OS primitives, which is a materially different posture from a patented mechanism. No patent number may ever be printed: the one the brief cites is behind an account and was not located |
| Windows and macOS cannot do the three-way split without privilege | Portability gap | The tier tables state it explicitly rather than implying parity |
| Vendors will not integrate a third-party library | Adoption risk | Callback API, zero-privilege tier, C ABI, no allocation on the hot path, and a one-file integration guide |
| A thread's TID is recycled and the cached fd points at another thread | Wrong attribution | **Open.** The fix is to key the registry on (tid, start_time) from `/proc/<tid>/stat` field 22 and re-validate on sample |
| A trace above ~10 M samples exhausts memory | Analyzer failure | **Open.** A streaming aggregation mode is deferred with the limit documented |

### 10. Definition of done

- `pace-sdk` builds clean with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` on gcc 15 and clang 22. **DONE.**
- 76 unit checks pass unprivileged, 77 as root. **DONE.**
- The premise probe compiles and runs; its output is committed as a fixture. **DONE.**
- The accuracy harness meets every threshold in section 4 on this machine, with the results committed. **DONE.**
- `pace-analyzer` parses every format in the table, produces a terminal report and a self-contained HTML report, and `pace verify` confirms the decomposition invariant on a real trace. **DONE.**
- 19 analyzer tests pass. **DONE.**
- Both websites build, contain no placeholder content, and every number on them matches a committed measurement artifact. **DONE.**
- The Windows and macOS backends compile in CI. **NOT DONE.**
- The BUSL headers, `LICENSING.md`, the vcpkg port, the Conan recipe and the integration guide exist and are accurate. **NOT DONE.**
- Nothing in the repository is a stub, a mock, a fabricated measurement, or a sample-value placeholder. **DONE.**

## Review record

<!-- autoplan-accepted:ceo -->
- The product leads with time attribution, not cycles; the cycle figures are a labelled convenience unit. The verified core is the time decomposition, agreeing with the kernel's own cfs accounting to 0.0030 %.
- The differentiation is portable, embeddable, privilege-free and in-process. The claim "no existing tool separates work from pause" is withdrawn: Intel VTune's 2026-0 Threading Analysis already separates Preemption Wait Time from Inactive Sync Wait Time on Linux, and both websites name the incumbents.
- An eBPF scheduler tier is added: a CO-RE program on sched_wakeup and sched_switch, optional, falling back cleanly. Verified to load and attach as root.
- pace_session_stats() is added, reporting the tier, thread count, sample count, degraded count, I/O-error count and whether the eBPF tier attached.
- pace_sample gains struct_size and the PACE_SAMPLE_F_HAS_PREEMPTED and PACE_SAMPLE_F_HAS_BLOCKED flags, so the API can say "unknown" instead of "zero".
- The public header states the threading contract: which calls are thread-safe, that an observer must not re-enter the session, and that destroy invalidates every thread handle.
- keep_perf_fds_open is implemented, so descriptor usage does not scale with the thread count.
- The analyzer counts and skips malformed lines instead of failing the parse, and reports the count.
- The analyzer reports "no attribution" for a trace format that carries no split, and raises no findings against such threads.
- pace verify fails on an empty trace, because a vacuous pass is a false green in a CI gate.
- The Windows and macOS tier tables are stated explicitly: on-CPU vs blocked at tier 0, + context switches at tier 1 with privilege. Parity is not implied.
- The analyzer's CSV and text parsers stay hand-written rather than adopting regex and csv crates: the report must be a single offline file, and a regex over untrusted trace files carries a catastrophic-backtracking risk.
<!-- /autoplan-accepted:ceo -->

<!-- autoplan-accepted:design -->
- None yet: the design phase has not run.
<!-- /autoplan-accepted:design -->

<!-- autoplan-accepted:dx -->
- None yet: the DX phase has not run.
<!-- /autoplan-accepted:dx -->

<!-- autoplan-accepted:eng -->
- A thread handle could be used with a different session, which wrote one session's baselines into the other's thread state. Now rejected with PACE_ERR_INVALID_ARG.
- The thread-local fast path dereferenced a handle whose session had been destroyed. The session now carries a unique id and the fast path validates by id, so a stale handle is recognised without touching freed memory.
- The kernel recycles a thread id after the thread holding it exits, and the new thread inherited the dead thread's handle and baselines. The handle now records its owning pthread_t, and registration reclaims the slot.
- pace_session_stats now rejects a struct_size this build cannot fill, instead of accepting any smaller size and returning zeroes.
- The CI could not have run: the three products are separate repositories, so each job now checks out the repository it needs into a subdirectory and runs there.
- The cached /proc/<tid>/schedstat descriptor is re-opened when the thread's start time changes, amortised over 1000 samples so the check does not dominate the sample.
- The analyzer's parsers stay hand-written: a regex over untrusted trace files carries catastrophic backtracking.
- The Windows and macOS backends remain the next wave, not a ship blocker. An absent backend fails loudly at pace_session_create; a wrong one would produce a plausible-looking number.
<!-- /autoplan-accepted:eng -->

### AUTONOMOUS DECISION LOG

| # | Phase | Decision | Classification | Principle | Rationale | Rejected |
|---|---|---|---|---|---|---|
| 1 | CEO | Reframe the differentiation around portability, embeddability, privilege and in-process use | Mechanical | P6 | VTune's own documentation already splits preemption from blocking, so the original premise is false; the honest positioning is the one that survives contact with the incumbent | "Nobody can do this today" |
| 2 | CEO | Lead with time attribution rather than cycles | Mechanical | P1 | The time decomposition is verified against two independent kernel paths; the cycle figure has no equivalent ground truth | Leading with cycles, which would be a claim without a check |
| 3 | CEO | Add the eBPF tier | Mechanical | P1 | It is the only tier that measures the scheduler's own events and works where /proc is restricted; it is in the blast radius and needs no new infrastructure | Deferring it, which would leave the Linux tier dependent on one kernel interface |
| 4 | CEO | Add struct_size and the HAS_* flags | Mechanical | P1 | Without them the API cannot express "unknown", and the ABI cannot grow | A fixed struct, which is an ABI break waiting to happen |
| 5 | CEO | Add pace_session_stats() | Mechanical | P1 | "Why is tier 1 not active" is the first question a vendor will ask, and today it has no answer | A diagnostics file, which would write to the customer's disk |
| 6 | CEO | Make the analyzer tolerant of malformed lines | Mechanical | P1 | A trace captured under stress is routinely truncated; refusing the whole file is the wrong failure mode | Failing the parse, which loses a usable trace over three bad lines |
| 7 | CEO | Make pace verify fail on an empty trace | Mechanical | P1 | A vacuous pass is a false green in a CI gate | Passing, which is a false green |
| 8 | CEO | Keep the analyzer's CSV and text parsers hand-written | Taste | P4 | A regex over untrusted trace files carries catastrophic backtracking; the report must be a single offline file | Adopting the regex and csv crates, which the plan had listed |
