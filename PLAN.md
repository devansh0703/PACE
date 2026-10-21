# PACE — Preemption-Aware Cycle Attribution Engine

Two products from one idea (Idealab #1352567): a licensed C/C++ SDK and an open-source trace analyzer, each with its own website.

## Implementation plan

### 0. What we are building and why

A performance engineer watching a game engine sees a 40 ms frame. The wall clock says 40 ms. `CLOCK_THREAD_CPUTIME_ID` says 12 ms. **Where did the other 28 ms go?** Today the honest answer is "the profiler cannot tell you" — the thread either did not run, or was blocked on a lock or an I/O call, and the existing tools do not cleanly separate those two. A frame-time regression that is pure scheduler preemption looks identical to a frame-time regression that is a code regression, and teams chase the wrong one.

PACE is a small, embeddable library that answers that question per thread, per sample, on Linux, Windows and macOS, and a companion analyzer that turns the samples into an attribution report.

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

A conventional profiler shows "100 ms CPU" and "310 ms wall" and leaves 210 ms unassigned. PACE assigns it, and proves it is preemption rather than blocking, because the voluntary-switch counter stayed at zero.

### 1. Architecture and the central design decision

The central problem: **cycles lost to preemption can never be measured directly**, because the hardware counter is not running while the thread is off-CPU. Every implementation must estimate them. PACE estimates them from a measured, portable frequency anchor rather than a guess.

The anchor: the x86 invariant TSC ticks at a constant rate independent of core frequency, and `PERF_COUNT_HW_REF_CPU_CYCLES` counts at the same rate. Measured on this machine: TSC delta 133,778,748 vs `hw_ref_cycles` 133,689,928 over the same window — **0.07 % agreement**. So reference cycles are an exact, portable, turbo-independent unit.

This yields a two-tier design, and it is the decision that makes the product shippable:

**Tier 0 — always available, zero privilege.**
`CLOCK_THREAD_CPUTIME_ID` + `/proc/<tid>/schedstat` + `/proc/<tid>/sched` (Linux), `GetThreadTimes` + `QueryThreadCycleTime` (Windows), `thread_info` (macOS). These give the complete four-way decomposition in nanoseconds with **no elevated privilege**, so PACE works inside containers, in CI, and on locked-down customer machines. Cycles are derived by multiplying nanoseconds by the measured reference frequency.

**Tier 1 — optional, needs `CAP_PERFMON` or `kernel.perf_event_paranoid <= 1`.**
`PERF_COUNT_HW_CPU_CYCLES` gives *measured* core cycles — the actual work the CPU did, turbo included — instead of a frequency-derived estimate. `PERF_COUNT_HW_REF_CPU_CYCLES` gives the reference anchor. This host has `perf_event_paranoid=4`, so unprivileged `perf_event_open` for hardware events returns `EACCES`; the SDK must not require it. A tool that only works as root is a tool nobody embeds in a customer's game engine.

**Consequence for the honest claim:** PACE reports *reference cycles* exactly in Tier 0, and *core cycles* exactly in Tier 1. Core cycles in Tier 0 are an estimate labelled as such, with the measured on-CPU frequency shown alongside so the user can judge the uncertainty. The analyzer carries a per-sample `source` field so no consumer can mistake an estimate for a measurement.

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
   └──────┬───────┘   └──────┬───────┘   └──────┬───────┘
          └──────────────────┼──────────────────┘
                    pace_sample (C struct)
                    { wall_ns, on_cpu_ns,
                      preempted_ns, blocked_ns,
                      core_cycles, ref_cycles,
                      core_per_ref_ratio, source }
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

**Public C API** (`include/pace/pace.h`), ABI-stable, opaque handles only:

```c
typedef struct pace_session pace_session;
typedef struct pace_config  pace_config;

typedef enum {
  PACE_SOURCE_UNKNOWN = 0,
  PACE_SOURCE_CLOCK   = 1,   /* tier 0: nanosecond + reference-cycle derived */
  PACE_SOURCE_PERF    = 2,   /* tier 1: measured core cycles */
} pace_source;

typedef struct {
  uint64_t    wall_ns;            /* monotonic wall delta since previous sample */
  uint64_t    on_cpu_ns;          /* time the thread was actually running */
  uint64_t    preempted_ns;       /* run-queue wait: runnable but not running */
  uint64_t    blocked_ns;         /* wall - on_cpu - preempted: sleeping/IO/lock */
  uint64_t    core_cycles;        /* measured core cycles, or 0 in tier 0 */
  uint64_t    ref_cycles;         /* reference cycles (exact in tier 0) */
  double      core_per_ref_ratio; /* measured on-CPU core/ref frequency ratio */
  uint64_t    involuntary_switches;
  uint64_t    voluntary_switches;
  pace_source source;
  uint32_t    flags;              /* PACE_SAMPLE_F_HAS_CORE_CYCLES, ... */
} pace_sample;

pace_status pace_session_create(const pace_config*, pace_session**);
void        pace_session_destroy(pace_session*);
pace_status pace_session_sample(pace_session*, pace_sample*);
pace_status pace_session_sample_thread(pace_session*, pace_thread*, pace_sample*);
pace_status pace_session_subscribe(pace_session*, pace_observer_fn, void*);
```

**C++17 wrapper** (`include/pace/pace.hpp`): `PaceSession` RAII, `PaceScope` scoped marker, `PaceThreadScope`.

**Per-thread state.** A `pace_thread` handle owns the cumulative counters and the cached file descriptors. Registered threads are tracked in a sharded registry; a `thread_local` fast pointer avoids the registry lookup on the hot path. Thread exit is handled by a `pthread_key_create` destructor (POSIX) / `FlsAlloc` (Windows) so a thread that dies without unregistering does not leak a registry entry.

**Hot path cost** (`pace_session_sample`): 2 × `clock_gettime` (vDSO, ~20 ns each), 1 × `pread` on a cached `/proc/<tid>/schedstat` fd (~1 µs), 1 × `read` on the perf fd when Tier 1 is available (~1 µs). Target: **< 3 µs median** in Tier 0, **< 1 µs** with `PACE_SAMPLE_F_FAST` (clocks + perf only, no `/proc`).

**Files:**

```
pace-sdk/
  CMakeLists.txt                 # presets, install/export, SOVERSION, package config
  CMakePresets.json
  cmake/paceConfig.cmake.in
  include/pace/pace.h            # stable C ABI
  include/pace/pace.hpp          # C++17 RAII
  src/pace_session.c             # lifecycle, config, sample loop
  src/pace_thread.c              # per-thread state, registry, TLS
  src/pace_sample.c              # hot path
  src/pace_clock.c               # clock abstraction
  src/pace_tsc.c                 # CPUID 0x15/0x16 TSC frequency + calibration fallback
  src/pace_linux_procfs.c        # tier 0: /proc/<tid>/schedstat + /proc/<tid>/sched
  src/pace_linux_perf.c          # tier 1: perf_event_open
  src/pace_windows.c             # GetThreadTimes, QueryThreadCycleTime, ETW
  src/pace_macos.c               # thread_info, mach_absolute_time
  src/pace_trace.c               # JSONL trace writer
  src/pace_version.c
  tests/
    unit/                        # per-module unit tests
    verify/premise_probe.c       # the empirical premise probe (already written)
    verify/accuracy/             # accuracy harness vs perf/ground truth
    bench/                       # Google Benchmark harnesses
  bench/
    bench_sample.cc              # per-sample overhead
    bench_instrumented.cc        # end-to-end overhead on a real workload
  examples/
    vendor_embed/                # minimal in-process profiler integration
    thread_attribution/          # standalone demo
  docs/
    PREMISE-VERIFICATION.md
    INTEGRATION.md
    ACCURACY.md
    API.md
  LICENSE                        # BUSL 1.1
  LICENSING.md
```

**Licensing.** Business Source License 1.1 — `Licensor: PACE`, `Change Date:` 4 years from first publication, `Change License: Apache-2.0`, `Additional Use Grant:` production use by a holder of a current paid license. This is the Elastic/MariaDB pattern for a source-visible commercial SDK: the customer reads the source, and the source becomes open after four years. Apache-2.0 is not viable for the SDK (it would let a competitor resell it), and AGPL is not viable (the customer is a commercial vendor and would never accept copyleft on a linked library). BUSL 1.1 is the current standard for exactly this shape. The per-file BUSL header template goes in `LICENSING.md`.

**Packaging.** `find_package(pace)` via `install(EXPORT)` + `CMakePackageConfigHelpers`; vcpkg port (`vcpkg.json` manifest, git ref, SHA256) and a ConanCenter recipe; GitHub Actions CI on Linux/Windows/macOS with ccache.

### 3. Product 2 — `pace-analyzer` (open source, Apache-2.0)

Repository: `pace-analyzer/`. Rust 2021, one crate with a library and a `pace` binary. Open source on purpose: it is the proof of accuracy, the adoption funnel, and the thing a prospective licensee evaluates before paying.

**Input formats** (all parsed, none invented):

| Format | Parser | Source |
|---|---|---|
| PACE JSONL trace | `serde_json` | our own |
| Chrome Trace Event | `serde_json` | `https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU` |
| `perf sched timehist` text | `regex` + manual parse | `perf-sched(1)` |
| `perf trace` CSV | `csv` crate | `perf-trace(1)` |
| Perfetto `TracePacket` protobuf | `prost` | `https://perfetto.dev/docs/trace-event-format` |

**Attribution model** (`pace-analyzer-core`): per thread, bucket wall time into `on_cpu`, `preempted`, `blocked`; convert to reference and core cycles using the frequency carried in the trace; compute per-interval attribution; detect the anomalies that matter to a performance engineer — a thread that lost more than X % of its wall to preemption, a thread whose on-CPU time dropped between two phases, a frame budget exceeded with the cause attributed.

**Outputs:**
1. Terminal report — per-thread table, phase comparison, ranked findings.
2. A **self-contained HTML report** — one file, no network, ECharts (Apache-2.0) for the attribution stack and the thread-state Gantt. The ECharts dist file is vendored into the crate and inlined, so the file opens offline.

**CLI:**

```
pace analyze <trace> [--format pace|chrome|perfsched|perftrace|perfetto]
                 [--output report.html] [--json] [--phase NAME=LABEL,...]
pace inspect <trace>          # per-thread raw table
pace convert <in> --to <fmt>  # translate between formats
pace verify <trace>           # re-check the decomposition invariant
```

**Adopted crates** (no hand-rolling where a library exists): `clap` (CLI), `serde` + `serde_json` (JSON), `csv` (quoted CSV), `regex` (text formats), `prost` (Perfetto protobuf), `thiserror` (errors), `anyhow` (binary), `html-escape` (report escaping). ECharts vendored as a static asset.

### 4. Benchmark and verification suite

Adopted harnesses: **Google Benchmark** (C++, for the SDK) and **Criterion** (Rust, for the analyzer). Both are the current standard and both handle repetitions and aggregation correctly.

**Workloads** (all real, all open source):
- `pbr` (MIT) — a real multi-threaded path tracer; realistic CPU-bound game-like workload.
- SQLite (public domain) — real lock-contention workload; produces genuine *blocking*, not preemption.
- `llvm-test-suite` (Apache-2.0 with LLVM exceptions) — broad CPU coverage.
- `rt-tests` sched-migrate / hackbench (GPL-2.0) — real scheduler-delay tests, used as *independent ground truth* for preemption, invoked as an external binary and not linked.
- `stress-ng` (GPL-2.0) — CPU oversubscription to induce preemption; invoked as an external binary.
- `sysbench` (GPL-2.0) — database contention.

**Ground truth strategy** (independence matters — our own implementation also uses `perf_event_open`, so perf alone is not an independent check):

| Quantity | Independent ground truth |
|---|---|
| on-CPU cycles of thread T | `perf stat -e cycles -t <tid>`; `/proc/<pid>/stat` utime+stime; `perf sched timehist` run-interval reconstruction |
| preempted (run-queue wait) | `perf sched timehist` runqueue-wait column; `rt-tests` known scheduling delays; ftrace `sched_switch`/`sched_wakeup` reconstruction |
| blocked | `futex` tracepoints; `perf sched timehist` sleep intervals; voluntary-switch counters |
| wall | `CLOCK_MONOTONIC` |

**Accuracy acceptance criteria** (hard numbers, checked by CI):

| Metric | Threshold |
|---|---|
| PACE on-CPU cycles vs `perf stat -t` | median relative error < 1 % |
| PACE on-CPU ns vs `/proc` utime+stime | median relative error < 0.5 % |
| PACE preempted ns vs `perf sched timehist` | median relative error < 5 % |
| `wall = on_cpu + preempted + blocked` | within 0.1 % of wall, median |
| `pace_session_sample` overhead (Tier 0) | < 3 µs median, < 10 µs p99 |
| `pace_session_sample` overhead (fast mode) | < 1 µs median |
| End-to-end instrumentation overhead on `pbr` | < 1 % |

**Method.** N ≥ 30 independent runs per configuration; median and p95 reported; cores pinned with `taskset`; `intel_pstate` governor and turbo state recorded in a machine-readable environment artifact; the Google paper *"Rigorous Benchmarking in C++"* rules applied (separate compilation for baseline vs candidate, dead-code elimination, frequency control). Analysis in Python with `scipy` (Wilcoxon signed-rank) and `numpy` (bootstrap CI); figures in `matplotlib`.

### 5. Websites

Two separate sites, one per product, in `websites/`.

- `websites/pace-sdk/` — the commercial product. Audience: product leaders at developer-tool firms. Story: the gap, the proof, the integration, the evaluation license.
- `websites/pace-analyzer/` — the open-source product. Audience: performance engineers. Story: the tool, the formats it reads, the report it produces.

**Stack.** Astro (latest) — the current best-practice static-site generator, deployable to GitHub Pages with the official adapter. Vanilla CSS (no Tailwind; the design is a specific system, not a utility soup). Self-hosted fonts via `@fontsource/ibm-plex-mono` and `@fontsource/ibm-plex-sans`. Icons via `@phosphor-icons/web` (the standalone web-font package, `weight="light"`).

**Design direction — "Wire Terminal"**, a field-instrument aesthetic carried over from the user's established house style:

- Ground `#131311` (warm charcoal), accent `#e8a33d` (amber). Saturated colour only for severity, never for decoration.
- IBM Plex Mono for structure and all numerics; IBM Plex Sans for prose.
- Phosphor icons, light weight. No Lucide, no emoji.
- Radii 3 px. Elevation from borders and contrast, never soft shadows.
- Banned: purple/teal/blue gradients, glassmorphism, badge pills above a hero, all-caps mono kickers above every heading, three-identical-card layouts, numbered `01/02/03` step lists, stat banner rows, coloured left borders, pulsing dots, emoji icons.

**Content.** Every claim on both sites is backed by a number produced by the benchmark suite, with the measurement environment attached. The SDK site shows the contended-run example above — 310 ms wall, 100 ms on-CPU, 210 ms preempted — because it is the clearest possible statement of the problem, and it is real.

### 6. Delivery order

Each step ends with something runnable and committed.

1. `pace-sdk`: CMake skeleton, C API header, clock + TSC detection, Linux tier-0 backend, tier-1 perf backend, sample hot path, unit tests. *Runnable on this machine, in the privileged and unprivileged case.*
2. `pace-sdk`: Windows and macOS backends (compile-verified on Linux via cross-toolchains where available; CI builds them natively).
3. `pace-sdk`: JSONL trace writer, C++ wrapper, examples.
4. `pace-analyzer`: crate, PACE + Chrome + perf sched + perf trace parsers, attribution model, terminal report.
5. `pace-analyzer`: HTML report, `verify` command, Criterion benches.
6. Benchmarks and verification: Google Benchmark harnesses, accuracy harness against the ground truth above, CI wiring, results committed as machine-readable artifacts.
7. Websites: both sites, with the real measured numbers.
8. Licensing and packaging: BUSL text, vcpkg port, Conan recipe, `find_package` docs, integration guide.

### 7. Explicitly not in scope

- Sampling profilers (stack unwinding, symbolisation) — that is the vendor's product, not ours. PACE supplies the time attribution those profilers cannot make.
- GPU, cache and memory-event attribution — different counters, different product.
- eBPF-based continuous production profiling — a different buyer with different requirements.
- A hosted SaaS offering — the brief's vehicle is licensing to tool vendors.
- Windows full preemption attribution without ETW: `GetThreadTimes` gives on-CPU time and ETW gives switch events, but the cheap path cannot split preemption from blocking. Documented as a limitation rather than faked.

### 8. Binding decisions

- D1: Two-tier measurement (privilege-free tier 0, optional measured-cycle tier 1). A product that requires root is unshippable inside a customer's engine.
- D2: Reference cycles are the portable unit; core cycles are the local unit. Both are reported, always labelled.
- D3: Preempted cycles are an explicit *estimate*, computed from a measured frequency, and the analyzer shows the estimate's inputs.
- D4: `pace-analyzer` is Apache-2.0 open source; `pace-sdk` is BUSL 1.1 with a paid-licence use grant. The open analyzer is the proof and the funnel.
- D5: PACE integrates into the vendor's profiler in-process through a C callback/observer API; the file format is a secondary path.
- D6: No hand-rolled algorithms where a maintained library exists — PMU encoding, chart rendering, CSV parsing, protobuf and CLI parsing are all adopted.
- D7: Every published accuracy number comes from the committed verification harness, with the environment artifact attached.

### 9. Risks and the plan for each

| Risk | Impact | Plan |
|---|---|---|
| `perf_event_paranoid=4` on customer machines | Tier 1 unavailable | Tier 0 is the product's floor; Tier 1 is an upgrade, never a requirement |
| "Cycles lost to preemption" is an estimate, not a measurement | Over-selling risk | Report it as an estimate with its inputs; the *time* attribution is exact, the cycle conversion is a documented frequency model |
| Incumbents (VTune, Nsight, Streamline) add this themselves | Market risk | The licensed feature is the *portable, embeddable* one plus the cross-vendor benchmark evidence; VTune cannot be embedded in a third-party game engine |
| Prior art / patents in cycle-accurate profiling | FTO risk | Patent search in the market report; BUSL keeps source private-but-visible; design-around is to rely on OS primitives rather than any patented mechanism |
| Linux-only deep preemption data | Portability gap | Windows ETW and macOS thread_info paths are implemented; where the OS cannot split preemption from blocking, the API returns the honest answer and the docs say so |
| Vendors will not integrate a third-party library | Adoption risk | Callback API, zero-privilege tier, C ABI, no allocation on the hot path, and a one-file integration guide |
| Busybox/CI environments without `/proc` | Backend unavailable | Feature-detect at `pace_session_create`; report `PACE_ERR_UNSUPPORTED` with the reason rather than failing silently |

### 10. Definition of done

- `pace-sdk` builds clean with `-Wall -Wextra -Werror` on gcc 15 and clang 22, on Linux, and the Windows/macOS backends compile in CI.
- Unit tests cover the sample path, the decomposition arithmetic, the TSC detection, the `/proc` parsers, and every error path.
- The premise probe compiles and runs, and its output is committed as a fixture.
- The accuracy harness meets every threshold in section 4 on this machine, with results committed.
- `pace-analyzer` parses every format in the table, produces a terminal report and a self-contained HTML report, and `pace verify` confirms the decomposition invariant on a real trace.
- Both websites build and deploy, contain no placeholder content, and every number on them matches a committed measurement artifact.
- The BUSL headers, `LICENSING.md`, the vcpkg port, the Conan recipe and the integration guide exist and are accurate.
- Nothing in the repository is a stub, a mock, a fabricated measurement, or a sample-value placeholder.

## Review record

<!-- autoplan-accepted:ceo -->
- None yet: the CEO phase has not run.
<!-- /autoplan-accepted:ceo -->

<!-- autoplan-accepted:design -->
- None yet: the design phase has not run.
<!-- /autoplan-accepted:design -->

<!-- autoplan-accepted:dx -->
- None yet: the DX phase has not run.
<!-- /autoplan-accepted:dx -->

<!-- autoplan-accepted:eng -->
- None yet: the eng phase has not run.
<!-- /autoplan-accepted:eng -->

### AUTOPLAN DECISION LOG

| # | Phase | Decision | Classification | Principle | Rationale | Rejected |
|---|---|---|---|---|---|---|
