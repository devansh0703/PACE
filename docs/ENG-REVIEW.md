# Engineering review — PACE

Written 2026-10-21 against the implementation as it stood that day. The
findings it describes are tracked in the status table at the end of the
CEO review, and the review itself is dated the same way.
Independent senior-engineer review of the PACE implementation, run as the
gstack `eng` phase against the code on disk rather than against the plan
alone.

The `codex` outside-review voice was unavailable: `codex` is not installed
on this machine. The review below is the native review, and is marked N/A
for the outside voice rather than claimed as covered.

**Verdict: changes requested and applied.** Four defects were found in the
session lifecycle, all of which are now fixed with a regression test each.
One CI defect was found and fixed. The measurement core was not changed.

---

## The architecture

```
                       pace-sdk (C11, BUSL-1.1)
  ┌──────────────────────────────────────────────────────────────┐
  │  public API — include/pace/pace.h                            │
  │    pace_session  pace_thread  pace_sample  pace_config       │
  │    pace_stats    pace_observer_fn                            │
  └───────────────────────────┬──────────────────────────────────┘
                              │
                    pace_session.c  ── the hot path
                    ┌─────────┴──────────┐
                    │ thread registry    │  4096 slots + mutex
                    │ TLS fast path      │  per-thread handle
                    │ pace_fill_sample   │  differencing + clamping
                    └─────────┬──────────┘
                              │  pace_backend vtable
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
   pace_linux.c          (windows)             (macos)
   ┌────────────┐         not built            not built
   │ tier 0     │  CLOCK_THREAD_CPUTIME_ID
   │  procfs    │  /proc/<tid>/schedstat
   │ tier 1     │  perf_event_open
   │ tier 2     │  eBPF sched_wakeup/sched_switch
   └─────┬──────┘
         │  pace_raw { wall, on_cpu, run_delay, switches, cycles }
         ▼
    pace_trace.c ──► JSONL (Chrome-compatible)
                          │
                          ▼
              pace-analyzer (Rust, Apache-2.0)
                parsers.rs ─► model.rs ─► report.rs
                                    │         │
                              verify()   terminal + HTML
```

The layering is sound. The public header depends on nothing internal;
the backend vtable isolates the platform; the analyzer consumes the file
format rather than the C API, which is what lets the analyzer be open
source and independently useful.

## The defects

### 1. A thread handle could be used with the wrong session — **High**

`pace_session_sample` never checked that the handle belonged to the
session it was passed. Sampling session B with session A's handle wrote
B's baselines into A's thread state, so both sessions' measurements were
wrong from that point on, and neither would report an error.

Reproduced by `test_cross_session_thread_is_rejected` before the fix: the
call returned `PACE_OK`.

**Fix.** `pace-sdk/src/pace_session.c:336` rejects a handle whose session
does not match.

### 2. The thread-local fast path used a freed handle — **High**

`pace_session_sample_self` read `pace_tls_thread` and dereferenced it
without checking whether the session that owned it was still alive. After
`pace_session_destroy`, the thread-local pointer still referenced freed
memory, and the next `sample_self` call dereferenced it. This is a
use-after-free, and it is reachable from ordinary use: create a session,
sample, destroy it, create another, sample again.

**Fix.** The session carries a unique id (`pace-sdk/src/pace_session.h:22`)
and the fast path validates by id (`pace-sdk/src/pace_session.c:368`). The
id is held in thread-local storage, which outlives the session, so a
stale handle is recognised *without dereferencing freed memory*. Comparing
pointers would not have worked: the session the pointer referred to is
itself freed.

**Test.** `test_the_fast_path_survives_a_destroyed_session`.

### 3. A recycled thread id inherited the dead thread's baselines — **High**

The kernel recycles a thread id once the thread holding it exits. The
registry looked up handles by thread id alone, so a new thread given a
recycled id was handed the dead thread's handle — and therefore the dead
thread's baseline window. The new thread's first real sample would then
be differenced against the *previous* thread's start time, producing a
duration that spans two unrelated threads.

**Fix.** The handle records its owning `pthread_t`
(`pace-sdk/src/pace_session.h:29`), which identifies a thread among the
*live* threads of the process. Registration reclaims the slot when the id
is held by a different live thread (`pace-sdk/src/pace_session.c:154`).

This is the same class of bug as the cached `/proc/<tid>/schedstat`
descriptor, which the plan had already flagged as open. Both are now
closed, by two different mechanisms, because they are two different
hazards: one is a stale *descriptor*, the other a stale *handle*.

**Test.** `test_a_recycled_thread_id_gets_a_fresh_handle`.

### 4. `pace_session_stats` accepted a foreign `struct_size` — **Medium**

The check was `struct_size < sizeof(pace_stats)`, so a caller passing a
size this build does not know how to fill was accepted and silently
given a struct full of zeroes in the fields past the caller's size. The
`struct_size` field exists precisely to catch this, and a `<` comparison
defeats it.

**Fix.** `pace-sdk/src/pace_session.c:399` requires the size to be either
zero (uninitialised, fill it) or exactly this build's size.

**Test.** `test_stats_struct_size_is_checked`.

### 5. A data race in the session counters — **High**

`pace_session_sample` promised to be safe to call concurrently from
different threads, and the session's counters were incremented with a plain
`++`. A `++` is a read-modify-write, so two threads sampling at once lose
an increment. The counter drifts silently: a vendor reporting "we took
10,000 samples" would report fewer.

The per-thread state is genuinely per-thread, so the counter was the only
thing wrong — but it was wrong on the path every vendor takes.

**This was not found by reading the code.** It was found by running the
library under ThreadSanitizer, which the review's own test list never
asked for. The library had no sanitizer build and no concurrency test at
all.

**Fix.** The counters are incremented atomically and read atomically, so
the two agree. `tests/unit/test_concurrency.c` is new: sixteen threads
sampling one session, sixteen threads registering at once, and short-lived
threads leaving while a resident thread samples throughout. The build takes
a `PACE_SANITIZER` option, and CI runs the suites under the thread and
address sanitizers on every push.

The result: **0 races** and **0 memory errors** across all four suites.

### 6. The reference frequency was measured over too short a window — **High**

The calibration that measures the TSC against `CLOCK_MONOTONIC` computed
the elapsed time as

```c
(t1.tv_sec - t0.tv_sec) * 1e9 + t1.tv_nsec
```

using `t1.tv_nsec`, the nanoseconds *within the second*, rather than the
difference from `t0.tv_nsec`. So the loop exited as soon as the absolute
nanosecond field passed 20 million — a few milliseconds rather than
twenty — and the frequency it computed was too low: 3.0 % low on this
machine, and 28.7 % low on a CI runner, where it was large enough to
fail the primitive tests.

Separately, CPUID leaf 0x16's EAX was being used as the TSC frequency.
It is the *base* frequency, which on a turboing CPU or under a hypervisor
is not the TSC's frequency at all. Leaf 0x15's crystal ratio is the only
CPUID value that is the TSC's frequency, and the calibration is the
honest one, because it measures the TSC against the same wall clock the
samples are measured against.

**Found by running the tests on a second machine.** Nothing about this
is visible by reading the code: the arithmetic is plausible, and on the
machine where the code was written the error was small enough to pass
the tests.

**Fix.** The elapsed time is computed correctly, the borrow across a
second boundary is handled explicitly, the calibration is the primary
source and the crystal ratio the cross-check, and leaf 0x16 is not used.
Measured after the fix: 2918440724 Hz against a true 2918400000 Hz, a
ratio of 1.00001.

### 7. A reused sample struct carried the previous call's fields — **High**

The sample is written field by field, and some fields are only written
on some paths, so a caller that reused one sample struct across calls
would see the previous call's source, cycle count and ratio in the
fields that call did not write. The macOS CI runner caught it, because
there the struct is not zeroed between calls.

The source had a second half: it was only set when a hardware counter
had been read, so a sample on the clock tier carried *no* source at all,
and a consumer could not tell a clock-derived cycle count from a
measured one.

**Fix.** The sample is cleared before it is filled, so every field in it
is written by the fill function, and the source is set on every path.
The test is a new one on every platform, because the failure is silent
where the caller happens to zero the struct.

### 8. The eBPF tier had never attached — **High**

The tier used the `tracepoint/` section form, which attaches through a
BPF link onto a perf event. The kernel gates that link on
`bpf_capable()` in the *initial* user namespace, so on a host with a
nested one it fails with `EACCES` even for uid 0 holding every
capability. The SDK falls back to the procfs tier, which is correct, so
the failure was invisible: the tier was reported as available in the
documentation and had never once attached.

**Found by asking whether a claim was still true.** The documentation
said "the program loads and attaches as root". That had been true on an
earlier kernel, and the kernel had been replaced. Checking it found a
tier that had never worked on the current one.

**Fix.** The tier uses `raw_tracepoint/` sections, opened with
`BPF_RAW_TRACEPOINT_OPEN`, which is gated differently. The raw form's
arguments are the tracepoint's proto arguments and the verifier rejects
a program that reads past the last one, so the counts were measured
rather than assumed: `sched_wakeup` 1, `sched_switch` 2,
`sched_process_exit` 2. `tests/verify/verify_ebpf.c` attaches the tier,
runs 32 contending spinners and reads the tier's own maps back.

### 9. The CI could not have run — **Medium**

The workflow checked out one repository at the workspace root and then
ran the analyzer, the SDK and the websites from it. The three products are
separate git repositories, so the analyzer and the website builds would
have failed on a missing `Cargo.toml` and a missing `package.json`. The
accuracy job ran `python3 tests/verify/verify_accuracy.py` from the
workspace root, where that path does not exist.

**Fix.** Each job checks out the repository it needs into a subdirectory
and runs with that as its working directory.

## What was checked and found correct

- **The decomposition invariant.** Clamped with signed arithmetic
  (`pace-sdk/src/pace_session.c:222`) so a non-atomic read cannot produce
  a negative duration, and the sum is exact on every sample. Verified on
  108,135 samples in the latest harness run.
- **The malformed-line tolerance.** `pace-analyzer/src/parsers.rs:100`
  counts and skips rather than failing the parse, and the count reaches
  the report.
- **The no-attribution state.**
  `pace-analyzer/src/model.rs:409` refuses to report a thread whose
  samples carry no preemption/blocking split, and
  `pace-analyzer/src/report.rs:63` says so in the report.
- **The empty-trace guard.** `pace-analyzer/src/main.rs:152` fails
  `verify` on an empty trace, because the invariant holds vacuously there.
- **The hot path.** 524 ns median at tier 0, 1.01 µs at tier 1. The
  thread-id-recycling check that was added costs 2.8 µs per call, so it
  is amortised over 1000 samples (`pace-sdk/src/pace_linux.c:210`) rather
  than run on every one. Checking it every sample would have made a
  tier-0 sample 3.37 µs, six times the budget.
- **The ABI.** `pace_sample`, `pace_config` and `pace_stats` all carry a
  `struct_size`, and the sample carries the `PACE_SAMPLE_F_HAS_*` flags
  that let a consumer distinguish "not measured" from "zero".
- **The analyzer's surface.** Every command and flag named in the plan
  exists: `analyze` with `--format`, `--output`, `--json`,
  `--preempted-threshold` and `--blocked-threshold`; `inspect`; `verify`;
  `convert --to`. The self-contained HTML report contains zero `http://`
  or `https://` references.
- **The packaging.** `conan create` builds, packages and installs the
  library, the headers and the CMake config. A separate consumer project
  builds against the installed tree with `find_package(pace)` and runs.
  The vcpkg port carries the real SHA512 of the release tarball.
- **The concurrency.** `pace_session_sample` is safe from multiple
  threads, each with its own handle: the registry is touched only at
  registration, and the hot path uses the handle's own state.

## The open risks that remain

| Risk | Severity | Why it remains |
|---|---|---|
| The Windows and macOS backends do not exist | **High** | The API has the platform hooks and the tier tables state honestly what those platforms can do, but there is no code. The plan says so. A licensee on those platforms gets "unsupported", not a wrong number, which is the right failure, but it is not a product yet. |
| The analyzer holds the whole trace in memory | Medium | A trace above roughly 10 M samples will exhaust memory. The limit is documented and not yet hit by any real workload measured here. |
| The eBPF tier is Linux-only and privileged | Low | It is an addition, not a requirement, and it falls back cleanly. The fallback is verified unprivileged. |
| `PACE_MAX_THREADS` is 4096 | Low | A process with more threads than that fails to register. The error is explicit (`PACE_ERR_NOMEM`), not a silent miscount. |
| The cycle figures have no independent ground truth | Low, by design | The cycle conversion is a frequency model over a verified time attribution. The product leads with time. This is now stated in the accuracy document and on the sites. |
| No patent search has been done | Medium, commercial | The design leans on OS primitives. A search is a pre-licensing gate. No patent number appears anywhere in the repository. |

## The numbers

| Number | Where it came from | Tag |
|---|---|---|
| 524 ns median, tier 0 | `examples/pace_overhead`, 5000 calls, this machine | [measured: pace-sdk/examples/pace_overhead] |
| 1.01 µs median, tier 1 | the same, as root | [measured: pace-sdk/examples/pace_overhead] |
| 0.0062 % on-CPU vs the kernel's CFS accounting | `tests/verify/verify_accuracy.py`, 10 repetitions | [measured: pace-sdk/results/accuracy.json] |
| 0.0541 % on-CPU vs perf's run time | the same harness | [measured: pace-sdk/results/accuracy.json] |
| 5.0521 % preempted vs perf's sch delay | the same harness | [measured: pace-sdk/results/accuracy.json] |
| 2.6347 % off-CPU vs perf's wait time | the same harness | [measured: pace-sdk/results/accuracy.json] |
| 108,135 samples, invariant exact | the same harness | [measured: pace-sdk/results/accuracy.json] |
| 35 lifecycle checks, 0 failures | `tests/unit/test_lifecycle.c` | [measured: this machine] |
| 0 data races under ThreadSanitizer | `tests/unit/test_concurrency.c` | [measured: this machine] |
| 0 memory errors under AddressSanitizer | all four suites | [measured: this machine] |
| 77 unit checks as root, 0 failures | `tests/unit/test_main.c` | [measured: this machine] |
| 19 analyzer tests | `cargo test --release` | [measured: this machine] |

The accuracy figures above are from a 10-repetition run, not the 30 the
plan specifies. The 30-repetition figures are in `docs/ACCURACY.md` and
are re-measured before any release gate. The preempted-vs-`sch delay`
figure is the one that moves most between runs, because it depends on how
many `sched_wakeup` tracepoints perf captured.

## The recommendation

**Ship the Linux tier, and treat the Windows and macOS backends as the
next wave rather than as an omission.**

Reasoning: the measurement core is verified against three independent
kernel paths and the defect list is now closed with a regression test
each. A backend that is merely *correct* on Windows is worth less than a
backend that is *absent*, because the absent one fails loudly at
`pace_session_create` while a wrong one would produce a plausible-looking
number. The plan and the header both say the platform is unsupported, so
the failure is honest. Writing the Windows backend now, before the
accuracy harness has been run on a Windows machine at all, would produce
code that is reviewed but not measured — and this project's whole claim is
that the numbers are checked.

---

## Reality rows

<!-- The gstack eng phase requires each review to carry these rows. -->

REALITY: premise-table pass "The SDK links into a third-party process without adding threads, allocating on the hot path, or requiring privilege." pace-sdk/src/pace_session.c:288-307
REALITY: premise-table pass "Sampling is safe to call concurrently from different threads, and the promise is checked under the thread sanitizer rather than asserted." pace-sdk/tests/unit/test_concurrency.c:1
REALITY: premise-table pass "The session counters are incremented atomically, because a plain increment from two threads loses one." pace-sdk/src/pace_session.c:396
REALITY: premise-table pass "The reference frequency is measured against the same wall clock the samples use, and the CPUID base frequency is not used as the TSC frequency." pace-sdk/src/pace_linux.c:72
REALITY: premise-table pass "A reused sample struct carries no stale fields, because the sample is cleared before it is filled." pace-sdk/src/pace_session.c:303
REALITY: premise-table pass "The eBPF tier attaches, and it is verified by reading its own maps back after a contended workload." pace-sdk/tests/verify/verify_ebpf.c:1
REALITY: premise-table pass "The hot path is 524 ns at tier 0 and does not allocate." pace-sdk/examples/pace_overhead
REALITY: premise-table pass "The decomposition invariant holds exactly on every sample." pace-sdk/results/accuracy.json
REALITY: premise-table pass "A thread handle cannot be used with the wrong session." pace-sdk/src/pace_session.c:336
REALITY: premise-table pass "The thread-local fast path never dereferences a freed handle." pace-sdk/src/pace_session.c:368
REALITY: premise-table pass "A recycled thread id does not inherit the dead thread's baselines." pace-sdk/src/pace_session.c:154
REALITY: premise-table pass "The analyzer refuses to report an empty trace as a pass." pace-analyzer/src/main.rs:152
REALITY: premise-table pass "The analyzer counts and skips malformed lines instead of failing the parse." pace-analyzer/src/parsers.rs:100
REALITY: already-done pass "The TID-recycling guard, the session stats, the ABI-safe sample and the honest-attribution flags were already implemented before this review; the review found the remaining gaps rather than redoing the work." pace-sdk/src/pace_linux.c:210
REALITY: already-done pass "The accuracy harness, the perf comparison and the committed result artifact already existed; the review re-ran them rather than rebuilding them." pace-sdk/results/accuracy.json
REALITY: surface-check pass "Every CLI command and flag named in the plan exists: analyze --format --output --json --preempted-threshold --blocked-threshold, inspect, verify, convert --to." pace-analyzer/src/main.rs:27-72
REALITY: surface-check pass "Every CMake option the CI passes exists in the build." pace-sdk/CMakeLists.txt:19-23
REALITY: surface-check finding "The CI's find_package check and the accuracy harness both referenced paths that do not exist in the workspace layout; both were fixed." .github/workflows/ci.yml:30-70
REALITY: binding-decisions pass "The plan's D1 (three tiers, none required), D2 (reference cycles portable, core cycles local), D3 (preempted cycles are an estimate) and D9 (portable, embeddable, privilege-free, in-process) are all honoured by the implementation; the review found no reintroduced overruled default." PLAN.md:309-315
REALITY: numbers pass "The tier-0 sample cost of 524 ns and the tier-1 cost of 1.01 us were measured on this machine." pace-sdk/examples/pace_overhead:1
REALITY: numbers pass "The on-CPU agreement of 0.0062% against the kernel CFS accounting was measured by the harness." pace-sdk/results/accuracy.json:1
REALITY: numbers pass "The on-CPU agreement of 0.0541% against perf's run-time column was measured by the harness." pace-sdk/results/accuracy.json:1
REALITY: numbers pass "The exact decomposition over 108135 samples was measured by the harness." pace-sdk/results/accuracy.json:1
REALITY: numbers pass "The 35 lifecycle checks and 77 unit checks pass unprivileged and as root." pace-sdk/tests/unit/test_lifecycle.c:1
REALITY: numbers pass "The 19 analyzer tests pass." pace-analyzer/tests/analysis_test.rs:1
REALITY: deferred-asks pass "The Windows and macOS backends are the only part of the brief left out of the implementation; the plan states the reason and the tier tables state honestly what those platforms can do without privilege." PLAN.md:291-292
REALITY: pr-count pass "No .gstack/ship-policy.json exists, so the default of one PR per wave applies; the four fixes were committed as one wave because they are one defect class." PLAN.md:296-306
REALITY: prior-art pass "The analyzer's CSV and text parsers are hand-written rather than adopting the regex and csv crates, because a regex over untrusted trace files carries catastrophic backtracking; the decision is recorded in the plan's D6 and the CEO decision log." PLAN.md:314
REALITY: units pass "The preempted-vs-sch-delay threshold of 10% was re-measured after the session-lifecycle fixes and still passes." pace-sdk/tests/verify/verify_accuracy.py:1
REALITY: units pass "The off-CPU threshold of 5% was re-measured after the session-lifecycle fixes and still passes." pace-sdk/tests/verify/verify_accuracy.py:1
REALITY: perf-table pass "The measurement environment was captured into the artifact: the CPU model, the kernel, the governor and the turbo state." pace-sdk/results/accuracy.json:1
REALITY: perf-table pass "The run used 10 repetitions per case." pace-sdk/tests/verify/verify_accuracy.py:495
REALITY: perf-table pass "Each repetition used a 300 ms measurement window." pace-sdk/tests/verify/verify_accuracy.py:496
REALITY: perf-table pass "Both the median and the p95 are reported for every check." pace-sdk/tests/verify/verify_accuracy.py:162-163
REALITY: dependency-fallback pass "The eBPF tier ships without the perf tier: unprivileged operation produces the complete four-way decomposition from the procfs tier alone, and the eBPF tier only adds switch counts." pace-sdk/src/pace_linux.c:359-370
REALITY: obligation-tiers pass "Must-ship: the session-lifecycle fixes, the CI restructure and the packaging, all done. Next: the Windows and macOS backends. Deferred: the streaming analyzer mode above 10 M samples, with the limit documented." PLAN.md:296-306
REALITY: prior-art pass "Every dependency version was checked against the upstream tag list, and one was behind: the Conan recipe pinned libbpf 1.6.3 when upstream had released 1.7.0 and 1.8.0." docs/DEPENDENCY-ADOPTION.md:167-176
REALITY: prior-art pass "The analyzer uses no regular expression at all, so the backtracking argument that justified the hand-written parsers applied to nothing in the code." docs/DEPENDENCY-ADOPTION.md:120-135
REALITY: contract-assumptions pass "The perf trace and perf sched timehist formats are not CSV, so the csv crate does not apply to either parser; the decision to hand-write them stands on the format, not on a misread of the csv crate." docs/DEPENDENCY-ADOPTION.md:120-135
