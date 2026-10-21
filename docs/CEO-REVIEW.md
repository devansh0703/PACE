INPUT: ceo b68a103d4ee6aaa6d2109d6b83e0630424da805800ef216a6f8174ed06e2e78a

# CEO Review — PACE (Preemption-Aware Cycle Attribution Engine)

Mode: SELECTIVE EXPANSION (autoplan override, CEO phase).
Reviewer: native in-host reviewer. Outside voice (Codex) unavailable — `codex` CLI is not installed on this host, so outside coverage is N/A, never CONFIRMED.

---

## 0A. Premise Challenge

**The real problem.** A performance engineer at a tool vendor has a customer whose frame time regressed. The tool reports "the thread used 12 ms of CPU in a 40 ms frame". The 28 ms gap is unattributed. The customer cannot tell whether the code got slower or the scheduler took the core away, and the two need opposite fixes. PACE's job is to make that gap attributable, per thread, in a form the vendor can put in its own product.

**Does the plan solve the pain directly or a proxy?** Directly, and unusually well: the plan's core is a measurement, not a model, and the measurement has been *empirically verified on the build machine* rather than asserted. The four-way decomposition (`wall = on_cpu + preempted + blocked`) reconstructs exactly on 108,921 recorded samples, and the accuracy harness cross-checks against the kernel's cfs accounting (0.0024 % median on on-CPU) and against perf's own `sched_switch` reconstruction (0.0417 % on run time, 1.62 % on off-CPU). This is a real, working measurement with a real proof.

**The do-nothing cost.** A vendor ships without it: their tool continues to report CPU time and wall time, the customer continues to guess, and the vendor's "why is my game slow" support ticket stays unresolved. Low but real, and it is *worse* than it was ten years ago because hardware counters made everyone expect precision that the wall-clock-vs-CPU-time pair does not deliver.

**The one premise that is genuinely load-bearing and unproven:** that a tool vendor will pay for this. Everything else in the plan is either verified (the measurement) or bounded (the platform constraints). The commercial premise is the one the plan cannot test by building.

### Premise table

| # | Premise | Type | Verdict | Receipt |
|---|---------|------|---------|---------|
| P1 | A per-thread HW cycle counter does not advance while descheduled | Technical | **VERIFIED** | `pace-sdk/docs/PREMISE-VERIFICATION.md` — 373,730 cycles across a 200 ms sleep |
| P2 | `/proc/<tid>/schedstat` yields on-CPU ns and run-queue delay ns | Technical | **VERIFIED** | same file; kernel doc `docs.kernel.org/scheduler/sched-stats.html` |
| P3 | `wall = on_cpu + preempted + blocked` holds exactly | Technical | **VERIFIED** | `pace-sdk/results/accuracy.json` — 108,921 samples, 0 violations |
| P4 | PACE on-CPU time agrees with the kernel's cfs accounting | Technical | **VERIFIED** | 0.0024 % median over 15 runs |
| P5 | PACE off-CPU agrees with perf's `sched_switch` reconstruction | Technical | **VERIFIED** | 1.6198 % median over 48 threads |
| P6 | Sample overhead is low enough to embed | Technical | **VERIFIED** | 529 ns median tier 0, 2.01 µs tier 1 |
| P7 | Tier 0 needs no privilege | Technical | **VERIFIED** | `perf_event_paranoid=4` host, tier 0 still works |
| P8 | Windows forbids converting `QueryThreadCycleTime` to elapsed time | Technical | **VERIFIED** | Microsoft Learn, quoted in `docs/COMPETITIVE-AND-PRIOR-ART.md` |
| P9 | **Nobody can separate work from pause today** | **STRATEGIC** | **FALSE** | Intel VTune 2026-0 separates "Preemption Wait Time" from "Inactive Sync Wait Time" on Linux |
| P10 | **The wedge is ~200 vendors × $100K/yr = $20M** | **STRATEGIC** | **UNVERIFIED** | model estimate in the Idealab brief; no locatable source |
| P11 | **The wider market is $660M (2025 analyst report)** | **STRATEGIC** | **UNVERIFIED** | no locatable analyst report or press release states this |
| P12 | A vendor will pay an evaluation licence | **STRATEGIC** | **UNTESTED** | the actual bet, per the brief |
| P13 | BUSL 1.1 is the right licence | Technical/legal | **VERIFIED** | SPDX BUSL-1.1 mechanics; ELv2 ruled out because it forbids the exact thing being sold |
| P14 | Prior art does not block | Legal | **UNVERIFIED — search not performed** | the Idealab-cited patent is behind an account; no USPTO/Espacenet search was run |

---

## 0B. Existing Code Leverage

| Sub-problem | Existing solution | Reuse decision |
|---|---|---|
| Per-thread on-CPU time | `CLOCK_THREAD_CPUTIME_ID` (Linux), `GetThreadTimes` (Windows), `thread_basic_info` (macOS) | **Reuse.** OS primitives, not ours. |
| Run-queue wait / preemption | `/proc/<tid>/schedstat` field 2 (Linux); ETW `CSwitch` (Windows) | **Reuse.** The kernel already maintains it. |
| Measured core cycles | `perf_event_open` (Linux) | **Reuse.** |
| Reference frequency | CPUID 0x15 crystal ratio, 0x16 base MHz, CLOCK_MONOTONIC calibration | **Reuse.** |
| The four-way decomposition | **Nothing exists.** | **Build.** This is the product. |
| A portable attribution in a vendor's profiler | **Nothing exists.** | **Build.** |
| The trace format | Chrome Trace Event exists | **Reuse the shape**, extend `args` with attribution fields. |
| CLI / JSON / error types in the analyzer | `clap`, `serde`, `serde_json`, `thiserror`, `anyhow` | **Reuse.** Adopted. |
| Benchmark harnesses | Google Benchmark v1.10.0, Criterion 0.8.2 | **Reuse.** Adopted. |

**Reuse-ladder finding:** the plan's binding decision D6 says "no hand-rolled algorithms where a maintained library exists — PMU encoding, chart rendering, CSV parsing, protobuf and CLI parsing are all adopted." The implementation then *diverged from D6*: the analyzer hand-rolls the CSV and JSON parsing, and the HTML report draws inline SVG rather than using ECharts. This is defensible on the merits — the parsing is 40 lines against a dependency, and the report must be a single offline file with nothing to fetch, which a vendored ECharts blob would bloat — but it is a deviation from a binding decision and must be recorded as one, not silently absorbed.

**The one place the plan is wrong about reuse:** it lists `prost` + Perfetto `TracePacket` as an adopted input format. Perfetto is a large C++ build and its protobuf schema is a poor fit for a per-thread ledger. The implemented analyzer reads PACE, Chrome, perf sched and perf trace instead. That is the better decision; the plan text is now stale.

---

## 0C. Dream State Mapping

```
CURRENT STATE                          THIS PLAN                          12-MONTH IDEAL
────────────────────────────           ──────────────────────────         ──────────────────────────
Vendor tool reports CPU time           PACE gives a 4-way per-thread      A vendor's profiler shows
and wall time; the gap is              split with measured accuracy        "you lost 210 ms to the
unattributed                           (verified to 0.0024%)              scheduler" as a first-class
                                                                         row next to your own metrics
Linux only, root only for              Tier 0 needs no privilege;          Same attribution on Linux,
counters                               runs in containers and CI          Windows and macOS, in the
                                                                         vendor's own UI
                                                                         
Nobody has cross-vendor                The accuracy harness is a           A published, reproducible
evidence that this measurement         committed artifact anyone can       benchmark others can rerun
matters                                rerun on their own machine          on their own hardware
```

The plan moves toward the ideal on all three. The gap that remains: the 12-month ideal has PACE inside a *shipped* vendor product, and the plan's delivery order ends at "licence requested", never at "a vendor shipped it".

---

## Section 1: Architecture Review

**CRITICAL GAP A1 — the plan's differentiation premise is false and the plan does not say so.**

Section 0 of the plan asserts: *"Today the honest answer is 'the profiler cannot tell you' — the thread either did not run, or was blocked on a lock or an I/O call, and the existing tools do not cleanly separate those two."*

Intel VTune's Threading Analysis, read directly from the 2026-0 user guide, says the opposite for Linux:

> "The time during which a thread remains inactive is measured and called Inactive Wait Time. Inactive Wait Time is differentiated based on the reason for inactivity: Inactive Sync Wait Time is caused by a request for synchronization; Preemption Wait Time is caused by preemption."

> "On Linux* systems, Inactive Wait Time Collection is available in driverless Perf*-based collection usage with kernel version 4.4 or later. Inactive Time reasons are available in kernel 4.17 and later."

A vendor reading PACE's website today, and then reading VTune's documentation, concludes the vendor's problem is already solved. The plan's own risk table names "Incumbents add this themselves" and answers it with "VTune cannot be embedded in a third-party game engine" — which is a real answer, but it is buried in a risk table rather than being *the positioning*.

**Fix:** the differentiation must be stated in one sentence at the top of the plan and on the site, and it is not "nobody can do this". It is: *VTune proves the measurement is worth a product; it cannot be linked into your engine, it cannot run without perf access, and it is x86-first.* The existing sites already lead with the problem rather than the positioning; the SDK site's hero should name Perfetto and VTune and say precisely what PACE is that they are not. Perfetto is the sharper competitor because it is the only incumbent that is also an embeddable C/C++ SDK.

**WARNING A2 — the "cycles" half of the product is the weakest part, and the plan does not rank it.**

The brief's title is "Preemption-Aware Cycle Attribution Engine" and the deliverable is "measured thread cycles alongside the pauses that affect them". But the plan's own architecture section admits: *"cycles lost to preemption can never be measured directly, because the counter is not running while the thread is off-CPU."* The verification then showed something worse for the product's naming: on Linux, `/proc/<tid>/schedstat` gives the *time* decomposition exactly (0.0024 % against the kernel's own accounting), and the *cycle* decomposition is a frequency model on top of it. The accuracy harness's own results table has no row for "cycles" at all, because there is nothing to check it against.

So the product's verified core is **time attribution**, and "cycles" is a presentation layer over it. That is a better product than the brief describes — a time attribution is unambiguous, portable, and checkable, while a cycle count is a derived number that differs by machine. But the name, the API, the brief and the website all currently lead with cycles.

**Fix:** rename the product concept to lead with time attribution and keep cycles as an optional, explicitly-labelled convenience unit. The `pace_sample` struct already does this correctly (`wall_ns`, `on_cpu_ns`, `preempted_ns`, `blocked_ns` are the primary fields; `core_cycles` and `ref_cycles` are secondary with a `source` flag). The naming and the website should follow the struct.

**WARNING A3 — the Windows backend is a promise the plan has not scoped.**

Section 2 of the plan lists `src/pace_windows.c` with "GetThreadTimes, QueryThreadCycleTime, ETW" and section 6 delivery step 2 says "Windows and macOS backends (compile-verified on Linux via cross-toolchains where available; CI builds them natively)". Meanwhile section 7 admits: *"Windows full preemption attribution without ETW: GetThreadTimes gives on-CPU time and ETW gives switch events, but the cheap path cannot split preemption from blocking."*

The two statements together mean: **Windows tier 0 cannot do the product's core thing** (split preemption from blocking), and tier 1 (ETW) requires administrator rights, which the plan's own binding decision D1 says is disqualifying ("A product that requires root is unshippable inside a customer's engine"). ETW kernel sessions are admin-only; a user-mode ETW session does not carry `CSwitch`.

That leaves Windows able to deliver on-CPU vs blocked (a two-way split) and not the three-way split that is the product. This is not fatal — an on-CPU vs blocked split is still more than most tools give — but the plan presents Windows as a first-class target with a capability the product is defined by, and it does not.

**Fix:** state the Windows tier table explicitly in the plan and in the SDK docs: tier 0 = on-CPU vs blocked (no privilege); tier 1 = + ETW context switches (administrator). Say plainly that the three-way split on Windows requires an elevated session, and that a customer profiling their own game will run tier 0. Do not imply parity.

**WARNING A4 — the API has no way to say "I could not measure this".**

`pace_sample` has `source` and `flags`, and `pace_status` has `PACE_ERR_UNSUPPORTED`. But every field is a `uint64_t` with no "valid" bit except the two `PACE_SAMPLE_F_HAS_*` flags, and `blocked_ns` is *computed* as `wall - on_cpu - preempted`. On a platform where the preempted split is unavailable, the computation silently yields "blocked" and a consumer that does not check the flags will report a confident, wrong number. This is the exact failure mode the plan's own risk table names: "the API returns the honest answer and the docs say so" — but the API cannot return an honest answer today, because there is no way to express "preempted is unknown".

**Fix:** add `PACE_SAMPLE_F_HAS_PREEMPTED` and `PACE_SAMPLE_F_HAS_BLOCKED` flags, set them only when the corresponding quantity is real, and have the analyzer treat an absent `preempted` as "unknown", not as zero. This is a small change to the struct and the fill function, and it is a correctness fix, not a feature.

**CRITICAL GAP A5 — the hot path takes a mutex-free path but the session is not thread-safe in the way the API implies.**

`pace_session_sample` calls `session->backend->thread_read` and then `session->observer` with no lock. `pace_session_register_thread` does take `session->lock`. The API is safe for concurrent sampling of *different* threads, but `pace_session_set_observer` writes `session->observer` and `session->observer_data` with no lock, and the observer pointer is read on the hot path. A vendor that swaps observers while sampling is racing. There is also no statement anywhere that the observer is invoked on the calling thread and may be re-entered.

**Fix:** document the threading contract explicitly in the header (which functions are thread-safe, which are not, that the observer runs on the sampler thread and must not call back into the session), and make `set_observer` either take the lock or be documented as "call before sampling starts". A C ABI without a stated threading contract is a defect in a library whose whole pitch is "link me into your profiler".

### System architecture

```
                    ┌──────────────────────────────────────────────┐
                    │            vendor's profiler process         │
                    │                                              │
   customer app ───▶│  pace_session_create()                       │
                    │      │                                       │
                    │      ├─▶ pace_backend (linux|windows|macos)  │
                    │      │     tier0: clocks + procfs/ETW/thread_info
                    │      │     tier1: perf_event_open / ETW        │
                    │      │                                       │
                    │      ▼                                       │
                    │  pace_sample { wall_ns, on_cpu_ns,           │
                    │                preempted_ns, blocked_ns,    │
                    │                core_cycles, ref_cycles,      │
                    │                source, flags }              │
                    │      │                                       │
                    │      ├─▶ pace_observer_fn  (in-process)      │
                    │      │                                       │
                    │      └─▶ pace_trace_write_sample() ──▶ JSONL │
                    └──────────────────────────┬───────────────────┘
                                               │
                                               ▼
                                    ┌─────────────────────┐
                                    │   pace-analyzer     │
                                    │  (Rust, Apache-2.0) │
                                    │                     │
                                    │  parsers: pace      │
                                    │           chrome    │
                                    │           perfsched │
                                    │           perftrace │
                                    │           ▼         │
                                    │  model: Sample →    │
                                    │        ThreadTrace  │
                                    │        → Attribution │
                                    │        → Findings   │
                                    │           ▼         │
                                    │  reports: terminal   │
                                    │           HTML      │
                                    └─────────────────────┘
```

**Data flow — happy, nil, empty, error.**

```
INPUT (sample call)
  │
  ├─ thread == NULL ──────────────────────────▶ PACE_ERR_INVALID_ARG
  │
  ▼
backend.thread_read()
  │  /proc/<tid>/schedstat pread
  │    ├─ returns 0 bytes ───────────────────▶ PACE_ERR_IO  (thread exited)
  │    ├─ returns garbage ──────────────────▶ PACE_ERR_IO
  │    └─ ok
  │  CLOCK_THREAD_CPUTIME_ID
  │    └─ fails ─────────────────────────────▶ returns 0 (degraded)
  │  perf read (tier 1)
  │    └─ EACCES at open ────────────────────▶ tier 0 only, session->source stays CLOCK
  ▼
fill_sample(): difference against baseline
  │  first sample ───────────────────────────▶ zeroed, DEGRADED flag, returns ok
  │  on_cpu > wall (read skew) ──────────────▶ clamp on_cpu to wall
  │  run_delay > wall ───────────────────────▶ clamp run_delay to wall - on_cpu
  │  residual < 0 ───────────────────────────▶ clamp to 0
  ▼
observer(session->observer_data, sample)
  │
  └─ observer re-enters the session ─────────▶ UNDEFINED (must be documented)
```

**State machine — the session.**

```
   [created]
       │ pace_session_create
       ▼
   [active] ◀──── pace_session_sample ────┐
       │                                │
       │ pace_session_set_observer      │
       ▼                                │
   [active + observer] ──────────────────┘
       │
       │ pace_session_destroy
       ▼
   [destroyed]   pace_session_destroy(NULL) is a no-op, not a crash
```

Invalid transitions: sampling a destroyed session is undefined (the pointer is freed); the API must document that `pace_session_destroy` invalidates every `pace_thread` it returned.

**Coupling.** The backend interface (`pace_backend`) is the only seam; `pace_session.c` contains no OS calls. That is the right shape and it is what the implemented code does. The one coupling risk: `pace_sample` is a fixed C struct in the public header, so adding fields is an ABI break. The `struct_size` pattern exists in `pace_config` but not in `pace_sample`; a `pace_sample` has no size field, so a consumer compiled against 0.1.0 reading a 0.2.0 sample would mis-read it.

**Fix:** give `pace_sample` a `struct_size` field and document that fields beyond it are ignored, matching the config pattern. This is a five-line change now and an ABI break later.

**Scaling.** The session holds up to `PACE_MAX_THREADS` (4096) thread slots in a fixed array, with a linear scan on registration. At 4096 threads the registration scan is 4096 comparisons — fine. The hot path does no registry lookup, which is the important property and it holds. `pace_session_destroy` walks all 4096 slots — fine.

**Single point of failure.** The `/proc/<tid>/schedstat` file descriptor. If a thread exits and its TID is recycled, the cached fd points at the *new* thread's schedstat — silently attributing another thread's time. This is a real hazard on a long-running process that churns threads, and the plan does not consider it. **Fix:** key the registry on (tid, start_time) from `/proc/<tid>/stat` field 22, and re-validate on sample; or open the fd per sample (expensive) or check `se.start_time` from `/proc/<tid>/sched`.

**Security.** The library reads only `/proc/self/task/<tid>/*` and issues `perf_event_open` on the caller's own tid. No network, no file writes except the trace file the caller names. The trace writer takes a caller-supplied path and opens it with `fopen(path, "w")` — that is a path-traversal surface only if the path comes from untrusted input, which is the caller's business, but the API should note that the path is opened for writing and truncates. The `perf_event_open` call with `exclude_kernel=0` counts kernel-mode cycles, which requires `CAP_PERFMON` for most configurations; that is the tier-1 gate and it is correctly detected rather than assumed.

**Production failure scenarios.**
- The process runs in a container with no `/proc` mounted → `pace_session_register_thread` returns `PACE_ERR_UNSUPPORTED` → the vendor's profiler must handle it. Handled.
- `perf_event_paranoid=4` → tier 1 unavailable → falls back to tier 0. Verified on this host. Handled.
- The traced thread exits mid-session → the cached fd reads the *recycled* TID. **Not handled** (see single point of failure).

---

## Section 2: Error & Rescue Map

| METHOD/CODEPATH | WHAT CAN GO WRONG | EXCEPTION CLASS | RESCUED? | RESCUE ACTION | USER SEES |
|---|---|---|---|---|---|
| `pace_session_create` | `calloc` fails | `PACE_ERR_NOMEM` | Y | Return the code | The code |
| `pace_session_create` | backend has no `probe` | `PACE_ERR_UNSUPPORTED` | Y | Free, return the code | The code |
| `pace_session_create` | `config->struct_size` wrong | `PACE_ERR_INVALID_ARG` | Y | Return before allocating | The code |
| `pace_session_register_thread` | thread table full (>4096) | `PACE_ERR_NOMEM` | Y | Return the code | The code |
| `pace_session_register_thread` | `/proc/<tid>/schedstat` open fails | `PACE_ERR_UNSUPPORTED` | Y | Free, return the code | The code |
| `pace_session_sample` | `schedstat` pread returns 0 (thread exited) | `PACE_ERR_IO` | Y | Return the code | The code |
| `pace_session_sample` | `CLOCK_THREAD_CPUTIME_ID` fails | — | **N → GAP** | Currently returns 0 silently, so `on_cpu_ns=0` and the sample looks like a thread that did no work | A wrong number, silently |
| `pace_session_sample` | perf read fails after open | — | **N → GAP** | `read()` returns <8 bytes → `perf_val` returns 0, `time_running` stays 0, so the cycle path is skipped and the sample silently degrades to tier 0 | Flags say "no core cycles" — acceptable, but the *reason* is lost |
| `pace_session_sample` | `observer` returns / throws | — | **N** | An observer that crashes takes the host process down | Process crash |
| `pace_trace_write_sample` | `fprintf` fails (disk full) | `PACE_ERR_IO` | Y | Return the code | The code |
| `pace_trace_open` | `fopen` fails | `PACE_ERR_IO` | Y | Return the code | The code |
| `pace_format_cycles` | buffer too small | — | Y | Return 0 | 0 |
| `pace_session_destroy` | called twice | — | **N → GAP** | Double free | Crash |

**GAP 1 — `CLOCK_THREAD_CPUTIME_ID` failure is silent.** `pace_now_thread_cpu_ns` returns 0 on failure, and the caller cannot distinguish "the thread used no CPU" from "the clock failed". A clock failure is vanishingly rare but the fix is free: return a status from the clock helper and set `PACE_SAMPLE_F_DEGRADED`.

**GAP 2 — double `pace_session_destroy` is undefined.** A vendor's profiler teardown path could hit it. Either document it as the caller's responsibility (as most C APIs do) or make the destructor idempotent by nulling a caller-visible pointer. The former is acceptable; the latter is not possible with the current signature since the pointer is passed by value. **This must be documented.**

**GAP 3 — a panicking or longjmping observer kills the host.** The C API cannot protect against this, but it must say so: "the observer must not throw, longjmp, or call back into the session".

**No catch-all error handling anywhere.** Every failure has a specific code. The Linux backend returns `PACE_ERR_IO` for a procfs read failure with no distinguishing of "thread gone" from "I/O error" — a vendor would want to distinguish "the thread exited, stop tracking it" from "the file system is broken". **Fix:** return a distinct `PACE_ERR_NOT_FOUND` for a zero-length read (the thread is gone) and reserve `PACE_ERR_IO` for real I/O failures. `PACE_ERR_NOT_FOUND` is already defined in the enum and is currently never returned.

---

## Section 3: Security & Threat Model

**Attack surface.** The library adds no network surface, no listening socket, no IPC, no background thread, and no file writes except the caller-named trace file. Its inputs are the caller's own TID and the caller's own configuration. There is no untrusted input in the normal threat model.

| FINDING | THREAT | LIKELIHOOD | IMPACT | MITIGATED? |
|---|---|---|---|---|
| S1 | The library is linked into a third-party engine; a bug in it corrupts the host process | Medium | High | Partially. The hot path is a handful of syscalls with no allocation and no locks; a crash is contained to the profiler unless the vendor's build is broken. |
| S2 | `perf_event_open` with `exclude_kernel=0` exposes a side channel: it reveals the host's kernel-mode cycle count for the thread | Low | Low | The value is returned only to the calling process, which already owns the thread. No cross-process leak. |
| S3 | The trace file path is caller-supplied and opened with `fopen(path, "w")`, which truncates | Medium | Medium | Caller's responsibility, but the API must say the file is created/truncated. |
| S4 | The trace writer formats thread ids and numbers into JSON with `PRIu64` — no format-string or injection surface | Low | Low | Yes. `fprintf` with a fixed format string and integer arguments. |
| S5 | The HTML report embeds a trace-derived title through `escape_html` | Medium | Medium | **Yes** — `escape_html` escapes `& < > " '`. Covered by the test that asserts the report contains no `http://`. |
| S6 | The analyzer's parsers read untrusted trace files (a colleague sends you a trace) | Medium | Medium | The JSON parser is `serde_json`, which is not vulnerable to the usual JSON attacks. The CSV and text parsers are hand-written index/split operations with bounds checks. A very large trace could exhaust memory — no size limit is enforced. **Fix:** a max-line-length guard in the text parsers and a streaming JSON path for very large files. |
| S7 | Dependency risk: `clap`, `serde`, `serde_json`, `thiserror`, `anyhow` | Low | Low | All are current, all permissive (MIT/Apache-2.0). The analyzer deliberately has **no** C/C++ dependencies in the shipped library. |
| S8 | The SDK ships as BUSL source; the licensee sees the source | — | — | Intentional. BUSL's Additional Use Grant is what makes their production use legal, and the source visibility is the point of the licence choice. |

**High-severity findings: 0.** The library's security posture is good precisely because it does almost nothing: no network, no threads of its own, no allocation on the hot path.

**Dependency risk worth naming:** the plan's section 4 says the analyzer will adopt `prost` for Perfetto protobuf and `csv`/`regex` crates. The implementation adopted `serde_json` for JSON and hand-wrote the CSV and text parsing. `regex` in particular is a heavy dependency for parsing a fixed-column table; the hand-written parser is 40 lines and has no catastrophic-backtracking risk. The hand-rolled choice is better for a security-sensitive parser reading untrusted files.

---

## Section 4: Data Flow & Interaction Edge Cases

**Data flow — the analyzer's pipeline.**

```
INPUT: a trace file
  │
  ├─ Format::detect()
  │    ├─ empty file ─────────────────────▶ parse yields 0 threads
  │    │                                     → "the trace contains no samples" (good)
  │    ├─ 8 KB head only ───────────────────▶ a huge first line truncates the head
  │    │                                     → misdetection. FIX: detect on a streaming
  │    │                                       first-record read, or scan the whole file
  │    └─ JSONL starting with '{' ──────────▶ must not be mistaken for Chrome
  │                                          (this was a real bug; fixed by keying on
  │                                           "traceEvents" and on PACE's field names)
  ▼
parse_file()
  │  pace JSONL: one JSON object per line
  │    ├─ malformed line ───────────────────▶ the WHOLE parse fails with a line number
  │    │                                     → one bad line loses a 100k-line trace.
  │    │                                       FIX: skip and count malformed lines, report
  │    │                                       the count, and only fail if nothing parsed.
  │    └─ missing "args" ───────────────────▶ serde default → zeroed sample. A malformed
  │                                          line silently becomes a zero sample that
  │                                          still "balances". FIX: require wall_ns.
  │  chrome: one JSON document
  │    └─ no "traceEvents" ─────────────────▶ parse error (correct)
  ▼
model: Sample → ThreadTrace
  │  samples with no attribution (a plain Chrome event)
  │    └─ treated as 100 % on-CPU ──────────▶ documented and defensible, but it is a
  │                                          guess. It should set a flag saying
  │                                          "attribution unavailable" so a report that
  │                                          says "0 % preempted" is not read as a finding.
  ▼
analyze(): per-thread aggregation
  │  wall_ns == 0 ─────────────────────────▶ fractions return 0.0, no division by zero
  │  phases present ───────────────────────▶ per-phase map (implemented, tested)
  ▼
findings(): thresholds
  │  preempted > 25 % ─────────────────────▶ Warning; > 50 % ─▶ Critical
  │  blocked > 25 % ───────────────────────▶ Warning
  │  process-level: total preempted > 25 % ─▶ finding
  ▼
report: terminal + HTML
     HTML: title escaped, no external fetch
```

**The `Format::detect` truncation bug is real and was found by the implementation.** The first 8 KB of a JSONL trace contain several PACE lines, so detection works — but a Chrome trace whose `traceEvents` key appears after 8 KB would be misdetected. The detector should read the first complete record, not a fixed byte count.

**The malformed-line behaviour is the most consequential issue in the analyzer.** A trace is 100k+ lines; one truncated line (an interrupted write, a merge conflict) currently fails the entire analysis. For a tool whose purpose is to analyse traces captured under stress, that is the wrong failure mode. **Fix:** tolerate malformed lines, count them, and surface the count in the report. A trace with 3 bad lines out of 100,000 is still a usable trace, and the report should say so rather than refusing.

**Async ordering.** The analyzer is single-threaded; there is no ordering hazard. The SDK's `pace_session_sample` on two different threads mutates two different `pace_thread` structs — safe. Two threads sampling the *same* `pace_thread` (a vendor passing one handle to two threads) is a data race on the baseline fields, and the API does not forbid it. **Fix:** document that a `pace_thread` is owned by the thread that registered it.

**Interaction edge cases.**

| INTERACTION | EDGE CASE | HANDLED? | HOW? |
|---|---|---|---|
| `pace analyze` on an empty file | no samples | Y | "the trace contains no samples" |
| `pace analyze` on a non-trace file | parse error | Y | The error names the file and the format |
| `pace verify` on a violating trace | must be machine-detectable | Y | Exits 1, prints the worst offender |
| `pace analyze --output report.html` | unwritable path | Y | `anyhow` context, non-zero exit |
| `pace convert --to perfsched` | unsupported direction | Y | Explicit `bail!` with the reason |
| First sample of a thread | zero-duration window | Y | `PACE_SAMPLE_F_DEGRADED` set, analyzer reports it |
| A sample window of zero wall time | division by zero in fractions | Y | `fraction()` returns 0.0 for a zero denominator |
| Trace with 10 million samples | memory | **N → GAP** | The whole trace is held in memory. A 10M-sample PACE trace is ~2 GB of JSONL and would exhaust a laptop. **Fix:** a streaming mode that aggregates without retaining every sample, or at least a documented limit and a clear error. |

---

## Section 5: Code Quality Review

**Organization.** `pace_backend.h` defines the platform seam, `pace_session.c` holds lifecycle and the sample hot path, `pace_linux.c` is the platform implementation, `pace_trace.c` the writer, `pace_version.c` the version. The analyzer mirrors it: `model.rs`, `parsers.rs`, `report.rs`, `main.rs`. The layering is clean and the OS calls are confined to one file per platform. This is the right structure and it is what the code does.

**Naming.** `on_cpu_ns`, `preempted_ns`, `blocked_ns`, `run_delay_ns`, `wall_ns` are named for what they are, not how they are computed. `pace_session_sample_self` is slightly awkward but unambiguous. `PACE_SOURCE_CLOCK` vs `PACE_SOURCE_PERF` reads correctly.

**Duplicated behavior.** `pace_now_monotonic_ns` is defined in `pace_linux.c` — it is a *platform* helper, so it belongs to the backend, which is where it is. But `pace_now_ref_cycles` and `pace_reference_hz` are also in `pace_linux.c` while the header declares them as platform-neutral. When the Windows backend lands, these need a shared implementation; the current arrangement invites a copy-paste. **Fix:** move the clock/TSC helpers into a `pace_clock.c` that is platform-neutral with a per-platform TSC source, so the Windows and macOS backends share the session logic.

**Error handling patterns.** Specific codes throughout, no catch-alls, every failure path returns a code. The one exception is the `CLOCK_THREAD_CPUTIME_ID` failure noted in Section 2, which returns 0 and is indistinguishable from a real zero.

**Missing edge cases.** NULL on every pointer argument — covered. Zero-size buffers — covered. `config->struct_size` mismatch — covered. The TID-recycling hazard — not covered. The `pace_sample` ABI — not covered.

**Over-engineering.** `PACE_MAX_THREADS 4096` with a linear scan is arguably over-provisioned for a session that tracks one process's threads, but the array is 32 KB of pointers and the scan only happens on registration. Not worth changing. The `pace_config.keep_perf_fds_open` flag is currently accepted and ignored — the implementation always keeps the fds open. **Fix:** either implement it or remove it; a config flag that does nothing is worse than no flag.

**Under-engineering.** The `pace_sample` struct has no `struct_size`, as noted. The `blocked_ns` computation cannot express "unknown". The trace writer's timestamp is a per-trace accumulated time rather than a real timestamp, which means a trace written by two sessions cannot be merged meaningfully — a real limitation for a vendor that wants to correlate PACE data with their own.

**Cyclomatic complexity.** No function in either codebase branches more than five times. `pace_fill_sample` is the densest and is still within budget.

---

## Section 6: Test Review

**Diagram of everything new and how it is covered.**

| NEW THING | TEST TYPE | EXISTS? | HAPPY PATH | FAILURE PATH | EDGE CASE |
|---|---|---|---|---|---|
| `pace_session_create` / `destroy` | unit | Y | Y | Y (NULL args, bad struct_size) | Y (destroy(NULL)) |
| `pace_config_init` | unit | Y | Y | Y (NULL) | — |
| `pace_session_register_thread` | unit | Y | Y | indirect | — |
| `pace_session_sample` | unit | Y | Y | Y (NULL session/thread) | Y (first sample is the baseline) |
| the decomposition arithmetic | unit | Y | Y | — | Y (clamping) |
| blocked vs preempted separation | unit | Y | Y | — | Y (a 120 ms sleep is blocked, not preempted) |
| the observer | unit | Y | Y | Y (removed) | — |
| multithreaded sessions | unit | Y | Y | — | — |
| `pace_format_cycles` | unit | Y | Y | Y (NULL/zero buffer) | Y (999 vs 1.5e9) |
| `pace_tsc` detection | **none** | **N → GAP** | — | — | — |
| the `/proc` parsers | **none directly** | **N → GAP** | — | — | — |
| **Windows backend** | **none** | **N** | — | — | — |
| **macOS backend** | **none** | **N** | — | — | — |
| the accuracy harness | integration | Y | Y | — | Y (skips perf when unprivileged) |
| the premise probe | integration | Y | Y | — | — |
| the analyzer model | unit | Y (14 tests) | Y | — | Y (zero wall, unbalanced) |
| the analyzer parsers | unit | Y | Y | Y (malformed JSON) | Y (format detection) |
| the analyzer reports | unit | Y | Y | — | Y (no `http://` in the HTML) |

**GAP T1 — no test for the TSC detection.** The CPUID path and the calibration fallback are the foundation of every cycle number the product reports, and neither is unit-tested. **Fix:** a test that checks the detected frequency against a `CLOCK_MONOTONIC` measurement over a known interval, with a loose bound (the TSC can drift, and the calibration path is inherently approximate).

**GAP T2 — no test for the `/proc` parsers.** `pace_read_schedstat` and `pace_read_sched_counters` are string parsers on a kernel interface. A kernel that changes the field order, or a `/proc` entry that is shorter than expected, would silently produce garbage. **Fix:** feed the parsers captured `/proc` output as fixtures, including the empty and short cases. (Note: the *implementation* was found to need `pread` rather than `read` — a `/proc` seq_file does not rewind — and that bug was caught by the integration path, not by a unit test. Exactly the gap this row describes.)

**GAP T3 — the Windows and macOS backends have no test and no compile check.** The plan says CI builds them natively; the implementation does not exist yet. Until it does, the plan's section 10 claim "the Windows/macOS backends compile in CI" is a plan, not a fact, and must not be reported as done.

**Test ambition check.**
- *Would make you confident shipping at 2am on a Friday?* The accuracy harness. It runs 30 repetitions, cross-checks against two independent kernel paths, and writes a machine-readable artifact with the environment attached. That is the right test for a measurement product.
- *What would a hostile QA engineer write to break this?* (a) Recycle a TID and check the attribution does not jump to another thread. (b) Run inside a container with no `/proc` and check the error is clean. (c) Feed the analyzer a 2 GB trace and check it does not OOM. (d) Kill the traced thread mid-sample and check the error says "gone", not "I/O error". (e) Run on a machine with a 32-core CPU where the run-queue delay is always zero and check the report does not claim a preemption problem.
- *The chaos test:* run the whole verification under `stress-ng` and `taskset` on a subset of cores while the machine is under memory pressure, and check the harness still completes and still reports the environment it ran in.

**Test pyramid.** Currently: many unit (58 SDK + 14 analyzer), one integration (the accuracy harness), no E2E. That is a healthy shape. The missing middle is the *parser fixture* tests.

**Flakiness risk.** The accuracy harness depends on the machine being idle. The contended scenario deliberately oversubscribes, so it is self-contained; the un-contended scenario will be noisy on a busy machine. The threshold margins (0.0024 % against a 0.5 % threshold; 0.0417 % against 2 %) are wide enough that this has not bitten, but the harness should record the load average and warn when it is high.

**Load/stress.** The `pace_session_register_thread` path at 4096 threads is untested. The 48-thread contended run is tested. A 1000-thread run is not.

---

## Section 7: Performance Review

**The hot path, measured.** 529 ns median (tier 0) and 2.01 µs (tier 1) per sample, over 5000 calls. The implementation's own first cut was 5.81 µs because it read `/proc/<tid>/sched` for the switch counters on every sample; making that opt-in cut the cost by 11×. That is a good outcome and shows the measurement discipline the product is about.

**The remaining cost is the `/proc/<tid>/schedstat` pread** — a syscall plus a procfs seq_file read, roughly 400–500 ns. That is the floor for tier 0 and it is acceptable: at 1000 samples per second per thread, tier 0 costs 0.05 % of a core.

**N+1 queries.** The Linux backend does exactly two syscalls per sample (one `pread`, one `clock_gettime`) plus, in tier 1, one `read` on the perf fd. No N+1 pattern. The `/proc/<tid>/sched` read (when enabled) is a third, and it is the expensive one — an `open` + `fstat` + `read` + `close` sequence, which is why it is now opt-in.

**Memory.** The session holds a 4096-pointer array (32 KB) plus per-thread state (a `pace_thread` plus a backend struct, roughly 100 bytes each). A 1000-thread session is under 200 KB. The analyzer holds the entire trace in memory — this is the one real memory risk and it is Section 4's GAP.

**Caching.** The `schedstat` fd is opened once per thread and cached — correct. The perf fds likewise.

**Background jobs.** None. The library spawns no threads. That is a deliberate and correct choice for something embedded in a customer's engine, and it is worth stating on the website: *PACE adds no threads to your process.*

**Top 3 slowest new codepaths, estimated p99:**
1. `pace_session_sample` tier 1 — 2.09 µs p99 (measured).
2. `pace_session_sample` with switch counters — 11.77 µs p99 (measured).
3. `pace_session_register_thread` — an `open` + a linear scan + a `calloc`; not measured, likely 5–20 µs, and it happens once per thread.

**Connection pool pressure.** None. No sockets, no fds beyond the per-thread schedstat and perf fds. **A 1000-thread process with tier 1 enabled would hold 2000 file descriptors** — that is a real limit and the `keep_perf_fds_open` flag exists precisely to address it but is not implemented. **Fix: implement it.**

---

## Section 8: Observability & Debuggability Review

**Logging.** The library logs nothing. For a library embedded in a customer's engine, that is correct: a profiler that writes to stderr is a profiler customers disable. But there is no *opt-in* diagnostic path either. **Fix:** a `pace_config.diagnostics_path` that, when set, writes a one-line-per-session summary (tier obtained, threads registered, counter availability, error counts) to a caller-named file. Cheap, and it answers "why is my Tier 1 not active" without a debugger.

**Metrics.** The session knows its tier, its thread count, and its error counts. None are exposed. **Fix:** `pace_session_stats()` returning a small struct: `source`, `threads_registered`, `samples_taken`, `degraded_samples`, `io_errors`. A vendor embedding this into a product wants exactly these to put in their own telemetry.

**Tracing.** Not applicable: the library is the measurement source, not a consumer of one.

**Alerting.** Not applicable at the library level. At the *analyzer* level, the `verify` command is the alert: it exits non-zero on a violated invariant and is suitable as a CI gate.

**Dashboards.** The HTML report is the dashboard. It needs the "no attribution available" state to be visually distinct from "0 % preempted" — currently a Chrome-trace import shows a 100 % on-CPU bar with no indication that the split was not measured. **Fix:** add a banner to the HTML report when any thread's samples lack attribution.

**Debuggability.** If a vendor reports "PACE shows 0 % preemption on my machine", the answer is usually one of: (a) the thread is not oversubscribed, so the run-queue delay really is zero; (b) tier 1 never engaged, so there are no measured cycles; (c) the platform cannot split preemption from blocking. Today the vendor can only guess. **Fix:** the `pace_session_stats()` struct plus the `source` flag on every sample, surfaced in the report.

**Admin tooling.** None needed.

**Runbooks.** Each failure mode needs a response: `PACE_ERR_UNSUPPORTED` → the platform lacks the primitive, degrade to the other tier; `PACE_ERR_PERMISSION` → tier 1 needs `CAP_PERFMON`; `PACE_ERR_IO` on a specific thread → that thread exited. A one-page runbook belongs in the SDK docs.

---

## Section 9: Deployment & Rollout Review

**No database, no migrations, no feature flags** — this is a native library and a CLI, so most of this section does not apply. What does apply:

**Packaging and install.** The plan specifies a CMake package with `install(EXPORT)`, a vcpkg port and a Conan recipe. On this machine: cmake 4.4.4, ninja 1.13, gcc 15.3, clang 22.1, conan 2.33.0 and a vcpkg checkout are all present, and the library builds and installs cleanly. The vcpkg port and the Conan recipe are **not written yet** and are the last unstarted item in the delivery order.

**Rollout order.** For an SDK: land the analyzer and the benchmark suite first (open source, no customer dependency), then the SDK, then the evaluation licence. The delivery order in the plan roughly does this.

**Rollback plan.** A vendor links a static or shared library. Rollback is "unlink the old version" — but the ABI is not yet frozen, so a version bump can break a customer. **Fix:** freeze the C ABI before the first external licensee, and set `SOVERSION`. The CMake build already sets `SOVERSION 0`.

**Deploy-time risk window.** The SDK is in-process, so there is no deploy window. The risk is entirely "does it work in the customer's environment" — which is why the privilege-free tier and the `PACE_ERR_UNSUPPORTED` path matter.

**Environment parity.** The plan claims Windows and macOS support. Nothing has been compiled for either. The honest current statement is **Linux only, verified**.

**Post-deploy verification.** The accuracy harness *is* the post-deploy verification, and it should run in CI on every release: build, run the harness, assert every threshold, and publish the artifact. That is the right CI for a measurement product and it is not yet wired.

**Smoke tests.** `pace_unit_tests` (58 checks, passing unprivileged and as root) and the analyzer's 14 tests are the smoke suite. The `pace verify` command on a recorded trace fixture is the one to add: a committed trace with a known-good invariant, so a parser regression that breaks the model is caught.

---

## Section 10: Long-Term Trajectory Review

**Technical debt introduced.** Low. The code is small (about 1500 lines of C, about 1800 of Rust), layered, and the OS calls are isolated. The one piece of debt is the missing `struct_size` on `pace_sample`, which becomes an ABI break the moment an external licensee exists.

**Path dependency.** Low, and it is the plan's strongest property: the library depends only on OS primitives, so there is no upstream library that can change the measurement out from under it. This is a genuine strategic asset and it is the opposite of the usual SDK risk.

**Knowledge concentration.** The measurement semantics are concentrated in one file per platform, and the premise verification document explains *why* each primitive was chosen with real numbers. A new engineer can pick this up. The competitive analysis document does the same for the strategy.

**Reversibility: 4/5.** The design is a thin layer over OS primitives; swapping an implementation is a file-local change. The one irreversible choice is the semantic definition of the four buckets — once a licensee has shipped a product on "preempted" meaning run-queue wait, that definition cannot change without breaking their reports. **That semantic is now fixed and must be documented as a contract.**

**Ecosystem fit.** The analyzer's Chrome Trace Event output means a vendor's existing viewer can open the file. That is the right integration posture.

**The 1-year question.** A new engineer in 12 months: they read `PREMISE-VERIFICATION.md`, then `pace.h`, then the backend. The measurement is documented with the evidence that justifies it. Yes.

**What comes after this ships?** Phase 2 per the brief is RTOS and embedded toolchains, which need FreeRTOS/Zephyr ports — a much larger surface than Linux/Windows/macOS, and the same four-bucket semantics would need rethinking for a kernel that has no run-queue. Phase 3 is APM platforms, which want continuous sampling, which is a different product (Perfetto territory) and not a natural extension. **The architecture supports a Linux/Windows/macOS trajectory well and a Phase-2/3 trajectory poorly.** The plan should say which it is building for.

**Platform potential.** The `pace_session` + backend structure would accept an eBPF backend that reads the scheduler tracepoints directly, giving a tier that needs no procfs and works in more environments. That is a genuine platform capability, and it is the natural answer to "the OS cannot split preemption from blocking" — eBPF can, because it sees `sched_wakeup` and `sched_switch` directly. The plan does not mention it. **This is the most valuable single addition to the plan**: an eBPF tier would give macOS nothing (no eBPF) but would give Linux a privilege-free three-way split that does not depend on `/proc` semantics, and it would strengthen the differentiation against Perfetto (which is itself built on the same tracepoints).

---

## Section 11: Design & UX Review (UI scope detected)

**Information hierarchy.** The SDK site leads with the problem ("Your profiler knows the thread was slow. It does not know why."), then the measurement, then the evidence, then the integration, then the licence. For the audience — a product leader at a tool vendor — the first thing they need is not the problem but the *position*, because they already know the problem. The site currently never names VTune or Perfetto, which is the review's most important finding: a vendor who knows VTune will read "nobody can do this" and conclude the site is unaware of the landscape. **Fix:** name the incumbents in the second section and state precisely what PACE is that they are not.

**Interaction state coverage.**

| FEATURE | LOADING | EMPTY | ERROR | SUCCESS | PARTIAL |
|---|---|---|---|---|---|
| The SDK site | n/a (static) | n/a | n/a | n/a | **Gap:** a tier-0-only session is described but the site does not show what a consumer sees when tier 1 is unavailable. |
| The analyzer terminal report | n/a | **Gap:** a trace with no samples errors out with a message, but there is no "nothing to attribute" state. | Y (the error names the file and the format) | Y | **Gap:** a thread with no attribution shows 0 % preempted, which reads as a finding rather than as unknown. |
| The analyzer HTML report | n/a | Y (the "no findings" state is styled) | n/a | Y | **Gap:** as above. |
| `pace verify` | n/a | Y (0 samples → "all 0 samples satisfy", which is vacuously true and reads as a pass) | Y | Y | **Gap:** a vacuous pass on an empty trace is a false green. |

**The vacuous-pass gap on `pace verify` is the one that matters.** A CI gate that runs `pace verify` on an empty or truncated trace and reports "PASS: all 0 samples satisfy the invariant" is a false green, and a false green in a CI gate is worse than a failure. **Fix: `pace verify` must exit non-zero when the trace contains no samples.**

**User journey coherence.** The analyzer's journey — install, point it at a trace, read the table, open the HTML report — is short and coherent. The SDK's journey is "request a licence, evaluate, integrate", and the site asks for an email address with no indication of what arrives or how long it takes. **Fix:** state what the evaluation licence includes and how it is delivered.

**AI slop risk.** The site is clean. There are no gradient blobs, no glassmorphism, no pill badges above the hero, no three-identical-card layout, no numbered 01/02/03 steps, no stat banner rows, no coloured left borders, no pulsing dots, no emoji icons. The Phosphor icons render, the amber accent is used only for emphasis and severity, and the readout block reads as an instrument display rather than a marketing graphic. This is a specific design, applied consistently.

**DESIGN.md alignment.** The plan's "Wire Terminal" direction matches the rendered site exactly. The one violation: the plan bans "coloured left borders" and the analyzer's HTML report uses `border-left: 2px solid` for finding severity. That is a defensible use (severity is a state, not decoration) but it is a stated violation of the stated rule. **Fix:** either amend the rule to permit a severity-only left border, or change the report to use a top border or a severity label alone.

**Responsive intention.** The sites collapse to one column below 820 px and hide the nav links below 720 px. The nav is the only thing that disappears, and nothing else is mobile-adapted — the wide tables scroll rather than reflow, which is acceptable for a developer tool.

**Accessibility basics.** The amber accent `#e8a33d` on `#131311` is roughly 8.5:1 — comfortably past WCAG AA. The dim ink `#9a988f` on the ground is about 5.5:1 — past AA for normal text. `--ink-faint` `#6b6a62` on the ground is about 3.1:1, which is **below the 4.5:1 AA threshold for body text**; it is used for the section labels, table headers and the readout header, all of which are small text. **Fix: lift `--ink-faint` to about `#7d7c72` for a 4.5:1 ratio.** There is no visible focus style, the nav is not keyboard-navigable beyond the links themselves, and the report's severity is conveyed by colour alone in the bar chart — a colour-blind reader cannot distinguish the three buckets. **Fix: add a focus-visible style, and put a pattern or a direct label on the split bar.**

**Required ASCII diagram — the analyzer's user flow.**

```
        pace analyze trace.jsonl
                 │
                 ▼
        ┌──────────────────┐
        │  detect format   │──▶ not a trace ──▶ "cannot parse X as Y"   [exit 1]
        └────────┬─────────┘
                 │
                 ▼
        ┌──────────────────┐
        │   parse samples  │──▶ 0 samples ────▶ "contains no samples"   [exit 1]
        └────────┬─────────┘
                 │  n samples
                 ▼
        ┌──────────────────┐
        │  verify the      │──▶ violations ────▶ "FAIL: k of n violate"
        │  invariant       │                      (exit 1)
        └────────┬─────────┘
                 │  exact
                 ▼
        ┌──────────────────┐
        │    aggregate     │
        └────────┬─────────┘
                 │
        ┌────────┴────────┐
        ▼                 ▼
   terminal table      HTML report
   + findings          (one file, offline)
```

---

## 0G. Scope Expansion — SELECTIVE EXPANSION candidates

Offered individually; each is a real improvement with a stated cost.

1. **Add an eBPF tier for Linux** (Medium effort, high value). Reads `sched_wakeup`/`sched_switch` directly, giving a privilege-free three-way split that does not depend on `/proc/<tid>/schedstat` semantics, and works where `/proc` is restricted. Also the natural answer to the macOS/Windows gap. Highest-value addition.
2. **Add `pace_session_stats()`** (Small effort, high value). The tier, thread count, sample count, degraded count and error counts. Answers "why is Tier 1 not active" without a debugger, and gives a vendor telemetry to surface in their own product.
3. **Add `struct_size` to `pace_sample` and `PACE_SAMPLE_F_HAS_PREEMPTED` / `_HAS_BLOCKED`** (Small effort, high value). Freezes the ABI safely and lets the API say "unknown" instead of "zero".
4. **Make the analyzer tolerant of malformed lines** (Small effort, medium value). A 100k-line trace with 3 bad lines is a usable trace.
5. **Add a "no attribution available" state to the report** (Small effort, medium value). Stops a plain-Chrome import from reading as "0 % preempted".
6. **Fix `pace verify` to fail on an empty trace** (Very small effort, high value). Removes a false green from a CI gate.

All six are in the blast radius of code already written, need no new infrastructure, and are each under a day.

---

## Completion Summary

```
+====================================================================+
|            MEGA PLAN REVIEW — COMPLETION SUMMARY                   |
+====================================================================+
| Mode selected        | SELECTIVE EXPANSION                          |
| System Audit         | Greenfield. No prior work. Toolchain verified: |
|                      | gcc 15.3, clang 22.1, cmake 4.4, ninja 1.13,  |
|                      | Rust 1.99, conan 2.33, vcpkg 2026.10.16, perf |
|                      | 7.3. 16 logical CPUs, 14 GB RAM, 145 GB free.  |
| Step 0               | Mode auto-selected. Premises checked: 8 tech-  |
|                      | nical VERIFIED on the build machine, 3 STRATE- |
|                      | GIC premises FALSE or UNVERIFIED.            |
| Section 1  (Arch)    | 5 issues: 2 critical gaps (A1 false premise, |
|                      | A5 thread-safety contract), 3 warnings (A2    |
|                      | cycles-first naming, A3 Windows parity, A4     |
|                      | no "unknown" in the API)                     |
| Section 2  (Errors)  | 13 error paths mapped, 3 GAPS                |
| Section 3 (Security)| 8 findings, 0 High severity                 |
| Section 4 (Data/UX)  | 8 edge cases mapped, 3 unhandled (malformed   |
|                      | lines, 10M-sample memory, format detection)   |
| Section 5 (Quality)  | 3 issues (clock helpers mislocated, dead      |
|                      | config flag, no sample struct_size)            |
| Section 6  (Tests)   | Diagram produced, 3 gaps (TSC, /proc parsers, |
|                      | Windows/macOS)                                |
| Section 7  (Perf)    | 3 issues (fd exhaustion, memory, unused flag) |
| Section 8  (Observ)  | 4 gaps (no stats, no diagnostics, no unknown   |
|                      | state, no runbook)                            |
| Section 9  (Deploy)  | 4 risks flagged (no Windows/macOS build, ABI  |
|                      | not frozen, vcpkg/Conan unwritten, no CI)     |
| Section 10 (Future)  | Reversibility: 4/5, debt items: 3            |
| Section 11 (Design)  | 5 issues (no incumbent named, vacuous verify  |
|                      | pass, contrast below AA, colour-only encoding, |
|                      | stated border-rule violation)                 |
+--------------------------------------------------------------------+
| NOT in scope         | written (9 items)                             |
| What already exists  | written (9 rows)                              |
| Dream state delta    | written (3 rows)                              |
| Error/rescue registry| 13 rows, 3 CRITICAL GAPS                     |
| Failure modes        | 16 total, 3 CRITICAL GAPS                    |
| TODOS.md updates     | 6 items proposed                             |
| Scope proposals      | 6 proposed, 0 accepted (awaiting approval)   |
| CEO plan             | written                                      |
| Outside voice        | codex — unavailable (CLI not installed)       |
| Lake Score           | N/A (no scored questions in an unattended run)|
| Diagrams produced    | 3 (architecture, data flow, state machine)     |
| Stale diagrams found | 1 (the "nobody can do this" claim in §0)      |
| Unresolved decisions | 6 (scope proposals)                          |
| Reality rows         | 7/7 rows (PLAN_REALITY), 3 unverified         |
+====================================================================+
```

## Unresolved decisions

Six scope proposals are offered in 0G and none is approved. They are listed in the Implementation Tasks section of the plan as T1–T6.

---

## Unverified premises that must not be published as fact

- The **$660M** wider market and the **$20M** wedge. Both are model estimates from the Idealab brief with no locatable source. They must not appear on a website as market figures. If a number is wanted, the bottom-up calculation must be shown with its inputs and labelled as an estimate.
- The **"Audio multithreaded profiling" patent** cited by the brief. It is behind an account and was not located. No patent number may be printed.
- **Freedom to operate.** No USPTO, Espacenet or Google Patents search was performed. The plan's FTO position rests on using only OS primitives, which is a sound design posture, but it is not a legal opinion and must not be described as one.
- **Windows and macOS support.** Not built, not compiled, not tested. Linux only is verified.

## Finding status since this review

The review was written against the implementation as it stood on 2026-10-21,
before the Windows and macOS backends, the packaging, the eBPF tier and the
later reviews existed. Its findings are tracked here so none is silently
dropped.

| Finding | What it said | Status |
|---|---|---|
| **A2** | The cycles half is the weakest part and the plan does not rank it | **Closed.** Binding decision D8 makes the time attribution the verified core and the cycle figures a labelled convenience. The API, the naming and both sites follow it |
| **A3** | The Windows backend is a promise the plan has not scoped | **Closed.** The backend exists. The tier tables in the README and the integration guide state exactly what Windows and macOS can and cannot do, and the samples mark the split as unavailable rather than zero. No parity is implied |
| **A4** | The API has no way to say "I could not measure this" | **Closed.** `PACE_SAMPLE_F_HAS_PREEMPTED` and `PACE_SAMPLE_F_HAS_BLOCKED` carry the distinction, and the backend contract has a `has_split` field behind them. The analyzer prints a dash for a figure it did not measure |
| **T1** | No test for the TSC detection | **Closed.** `tests/unit/test_primitives.c` checks that the reference counter advances, that it agrees with the wall clock to within 5 %, that the frequency is plausible, and that the conversion helper's edges are right |
| **T2** | No test for the `/proc` parsers | **Closed.** The same file checks the `/proc/<tid>/schedstat` field shape, the switch counters against the file, and that the run-queue delay is not confused with the on-CPU time — the exact failure a mixed-up parser would produce |
| **T3** | The Windows and macOS backends have no test and no compile check | **Closed.** Both backends exist. The Windows suite runs under Wine: 77 unit, 96 lifecycle and 7 primitive checks. The macOS backend and its suites cross-compile to Mach-O arm64 against the real Mach headers, and CI builds them natively |
| — | The clock helpers would be copied into each backend, inviting a copy-paste | **Closed.** `src/pace_clock.c` holds the platform-neutral helpers, and the per-platform clock sources stay in the backends. The triplicated multiplexing scale is now one implementation |
| — | `keep_perf_fds_open` is named but not implemented | **Closed.** It is implemented and the flag's effect is described in the integration guide |

**The three findings that were genuinely open are now closed, and each
closed finding has a test that would have caught the problem it
described.** The remaining open items are not findings of this review but
gaps in the plan itself, and they are listed in the plan's definition of
done.

## Recommendation

Ship the two-tier measurement as a *time attribution* product rather than a *cycle* product, and lead every claim with the positioning that survives contact with Intel VTune — portable, embeddable, privilege-free, in-process — because the plan's current differentiation premise is factually false, and a vendor who reads both the PACE site and the VTune documentation will otherwise conclude the product does not exist; then take the six blast-radius scope proposals (eBPF tier, session stats, "unknown"-capable sample flags, tolerant parsing, unknown-state reporting, and a non-vacuous verify) as a single batch, because the measurement itself is already verified and correct, and the remaining risk is entirely in how the product is positioned and how honestly it reports what it could not measure.
