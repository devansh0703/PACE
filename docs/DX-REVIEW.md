# DX review — PACE

Written 2026-10-21 against the command line as a user met it that day.
Independent DX engineer review of the PACE implementation, run as the
gstack `dx` phase against the code and the command line as a user meets
them.

The `codex` outside-review voice was unavailable: `codex` is not installed
on this machine. The review below is the native review, marked N/A for the
outside voice rather than claimed as covered.

**Verdict: changes requested and applied.** The CLI's error output was the
weak point: the messages were correct but the tool gave a caller no stable
code to branch on, no explicit fix, and no statement of what state the
operation left behind. Both products now implement the same contract. The
rest of the experience was already sound and is recorded below with the
measurements that back it.

---

## 1. Getting started

**The SDK: four commands from a clean checkout to a running program.**

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
cmake --install build --prefix /opt/pace
```

```cmake
find_package(pace REQUIRED)
target_link_libraries(my_profiler PRIVATE pace::pace)
```

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/pace && cmake --build build && ./build/tthw
```

Verified by actually doing it: a separate consumer project was configured
against the installed tree, built, and run. It printed
`on_cpu=3372 preempted=0 blocked=1113`. The install tree contains
`libpace.a`, `include/pace/pace.h`, and the four CMake config files.

**The analyzer: one command.**

```bash
cargo build --release
./target/release/pace analyze trace.jsonl
```

There is no configuration file, no environment variable to set, no first-run
wizard, and no account to create. The analyzer has no runtime dependencies
beyond the Rust standard library.

**The one thing a newcomer cannot do is the thing that needs a licence.**
That is correct: the SDK is BUSL-1.1 with a paid-licence use grant, and
the evaluation licence covers everything except shipping it. That is stated
in `LICENSING.md` and in the README.

## 2. API and CLI ergonomics

**The CLI is small and the commands are the four a user actually needs.**
`analyze`, `inspect`, `verify`, `convert`. Each does one thing. The
`--help` text for each says what it does in a sentence rather than listing
the flags first.

**The flags are the ones a user reaches for.** `--format` to override
detection, `--output` for the HTML report, `--json` for scripting,
`--preempted-threshold` and `--blocked-threshold` for tuning what counts
as a finding. The thresholds default to 25 %, which is a defensible
starting point and is documented as such.

**Progressive disclosure works.** `pace --help` shows four commands.
`pace analyze --help` shows the options. `pace verify --help` shows
nothing, because it takes one argument and has no options. A tool that
prints the same wall of options for every command is harder to read, not
easier.

**The names are consistent.** `preempted` and `blocked` are the same words
in the CLI, in the JSON, in the HTML report, in the C header and in the
Rust model. `on_cpu` is the same everywhere. There is no point in the
product where the same quantity has two names.

**The SDK's defaults are right.** `prefer_perf_counters` on,
`keep_perf_fds_open` on, `collect_switch_counters` **off** (it costs about
4 µs a sample and the run-queue delay already carries the split),
`prefer_ebpf_tier` on with a silent fallback. A caller who does nothing
gets the fast, always-available path.

**One ergonomic gap, not fixed: the SDK has no "give me a summary" call.**
`pace_session_stats` gives counters, and `pace::describe()` gives a
one-line string, but a caller who wants "what did this thread's last
second look like" has to compute it themselves. That is a reasonable
library boundary — a library should give measurements, not opinions — so it
is recorded rather than changed.

## 3. Error handling

This was the finding. The contract is now implemented in both products.

**What it was.** The analyzer printed `anyhow`'s default rendering:

```
Error: cannot detect the format of /nonexistent.jsonl

Caused by:
    0: i/o error: No such file or directory (os error 2)
    1: No such file or directory (os error 2)
```

Three problems. The word "error" appeared three times and the actual
cause was stated twice. There was no stable identifier a script could
branch on. And the most important fact was missing entirely: nothing was
written, so the caller could retry safely.

**What it is now.**

```
$ pace analyze /nonexistent.jsonl
pace: error [io-error]: i/o error: /nonexistent.jsonl: no such file
  fix: check the path, then pass the trace file: pace analyze <trace>
  recovery: fix the cause, then run the same command again
```

```
$ pace analyze bad.json
pace: error [parse-error]: parse error: JSON object has no 'traceEvents' array
  fix: the file is not in the format that was detected: pass the format explicitly with --format pace|chrome|perf-sched|perf-trace
  recovery: the input was read but not understood: no output was written; correct the format and run the same command again
```

Scored against the agent-ready error contract:

| Item | Verdict |
|---|---|
| **what** — the operation and the actual values | The path, the format, the parse detail. Not "an error occurred". |
| **fix** — a safe, authorised action | A command, a flag, or the exact thing a human must provide. Never "edit the tool". |
| **recovery** — what to do next, and the side-effect state | Every recovery line states whether output was written. All of them are safe to retry. |
| **code** — stable, with a doc anchor | Seven kebab-case codes: `io-error`, `parse-error`, `format-unknown`, `empty-trace`, `unsupported-output`, `write-error`, `invalid-argument`. The test asserts the strings have not changed. |
| **context** — versions, config, env var names, the underlying cause | The underlying cause is in the message. The tool has no configuration and no environment variables, so there is nothing else to state. |
| **trace** — unexpected failures only | N/A: the tool has no asynchronous or remote operation, so there is no failure that can leave an unknown outcome. |
| **redaction** — no secret values | The tool reads a trace and writes a report. It reads no credential, and the fix and recovery lines are built from a fixed set of phrases. A test asserts no error mentions a secret-shaped word. |
| **channel** — the surface's native failure signal | Non-zero exit and stderr for the CLI. The exit code was already correct and is still correct: 1 for every failure, 0 for success. |

**The SDK now has the same contract.** `pace_status_fix()` and
`pace_status_recovery()` give the same shape for every `pace_status`, and
`PACE_OK` returns an empty string for both. The status codes were already
typed and stable; they just did not say what to do.

**Two defects found while implementing this, both fixed:**

1. A missing file was reported as `format-unknown`, because the format
   detection read the file and the read error was folded into "I could not
   work out the format". A missing file is an I/O error. It now reports
   `io-error`.
2. The parse error printed `parse error: parse error:`, because the
   wrapper stringified a `Parse` variant whose own `Display` already says
   "parse error:". Now printed once.

## 4. Documentation

**The SDK's README is the one-page version and the integration guide is the
complete one.** The README gives the invariant, the three tiers, their
measured cost, and the build and install commands. `docs/INTEGRATION.md`
gives the whole integration: link it, create a session, sample, use the
numbers, use the observer, tell your users which tier you got, write a
trace, the threading contract, the cost, and a runbook.

**The runbook is the part that earns its place.** It is a table of
symptom → cause → action for the six questions a licensee will actually
ask: "why is my tier 1 not active", "why is this sample degraded", "why
are my io errors climbing".

**Every code example in the SDK's documentation was executed.** The
`find_package` flow, the observer, the flags — all of it compiles and runs.

**The analyzer's README shows the real output**, not a mock-up: the
sample counts, the decomposition status, the per-thread table with real
numbers from a real 48-thread trace.

**What is missing: a hosted API reference.** The header is documented
inline, which is the right place for a C library, but a hosted, searchable
reference would be better for a licensee evaluating the API. That is
deferred: it needs a hosting decision, and the inline documentation is
complete in the meantime.

## 5. Escape hatches

Every opinionated default can be overridden:

| Default | Override | Effect |
|---|---|---|
| The hardware counter tier is used when available | `pace_config::prefer_perf_counters = 0` | Never opens a perf event, so it never needs privilege |
| The perf descriptors are kept open | `pace_config::keep_perf_fds_open = 0` | Descriptor use does not scale with the thread count |
| The switch counters are off | `pace_config::collect_switch_counters = 1` | Adds the voluntary/involuntary counts, at about 4 µs a sample |
| The eBPF tier is used when available | `pace_config::prefer_ebpf_tier = 0` | Never touches a tracepoint |
| The format is detected | `--format pace\|chrome\|perf-sched\|perf-trace` | Forces the parser |
| The findings thresholds are 25 % | `--preempted-threshold`, `--blocked-threshold` | Any percentage, including 0 and 100 |
| The report goes to stdout | `--output report.html` | A self-contained HTML file |
| The output is human-readable | `--json` | JSON on stdout, for scripting |

The one default that cannot be overridden is the decomposition invariant
itself, which is the point of the product.

## 6. The findings, and what happened to each

| # | Finding | Severity | Status |
|---|---|---|---|
| 1 | The CLI's errors had no stable code, no fix, and no statement of the output state | **High** | **Fixed.** The agent-ready contract is implemented, with 7 tests that hold it in place |
| 2 | The SDK's status codes said what went wrong but not what to do | Medium | **Fixed.** `pace_status_fix()` and `pace_status_recovery()` |
| 3 | A missing file was reported as a format problem | Medium | **Fixed.** It now reports `io-error` |
| 4 | The parse error was printed with a doubled prefix | Low | **Fixed** |
| 5 | `anyhow` was kept only for its error rendering, which the typed error made redundant | Low | **Fixed.** The dependency is gone |
| 6 | There is no hosted API reference | Low | **Deferred.** The inline header documentation is complete; hosting needs a decision |
| 7 | There is no summary call for "how did this thread's last second look" | Low | **Not changed.** A library should give measurements, not opinions |
| 8 | The `pace verify` failure path prints to stdout, not stderr | Low | **Not changed.** It is a *result*, not an error: the tool ran and the answer is "the invariant does not hold". The exit code is 1, which is what a CI gate reads. |

No finding met the bar for a Critical: no leaked secret, no failure
signalled as success, no unsafe fix, and no block on a terminal.

## The recommendation

**Ship the CLI as it now stands, and treat the hosted API reference as the
next DX wave.**

Reasoning: the error contract was the one place where a caller — human or
agent — was left guessing. It is now uniform across both products, the
codes are stable and tested, and every failure states whether anything was
written. The remaining DX gaps are documentation *hosting*, not
documentation *content*: the inline header and the integration guide cover
everything a licensee needs, and hosting them is a decision about where the
site lives rather than about what the product says.

---

## Reality rows

REALITY: premise-table pass "A consumer can go from a clean checkout to a running program in four commands, and the flow was executed to confirm it." pace-sdk/README.md:57-73
REALITY: premise-table pass "The analyzer needs one build command and no configuration, no environment variable and no account." pace-analyzer/README.md:33-40
REALITY: premise-table pass "Every error carries a stable code, an actionable fix and a recovery line that states the output state." pace-analyzer/src/lib.rs:113-206
REALITY: premise-table pass "The CLI renders the contract on stderr and exits non-zero." pace-analyzer/src/main.rs:119-124
REALITY: premise-table pass "The SDK's status codes carry the same contract." pace-sdk/include/pace/pace.h:79-88
REALITY: premise-table pass "The seven error codes are kebab-case, distinct and locked by a test." pace-analyzer/tests/error_contract.rs:72-93
REALITY: premise-table pass "A missing file reports io-error rather than format-unknown." pace-analyzer/src/main.rs:81-95
REALITY: premise-table pass "No error message mentions a secret-shaped word, and the test holds it there." pace-analyzer/tests/error_contract.rs:171-189
REALITY: premise-table pass "Every opinionated default has a documented override." pace-sdk/docs/INTEGRATION.md:20-40
REALITY: premise-table pass "The SDK's documentation examples compile and run." pace-sdk/docs/INTEGRATION.md:6-20
REALITY: already-done pass "The build, install and find_package flow was already implemented and was re-run rather than rebuilt; the packaging and the CI consumer check already exercise it." pace-sdk/docs/INTEGRATION.md:6-20
REALITY: already-done pass "The analyzer's CLI surface was already implemented; the review found the error contract missing rather than the commands missing." pace-analyzer/src/main.rs:27-72
REALITY: surface-check pass "Every CLI command and flag named in the plan exists: analyze with --format, --output, --json and the two thresholds, inspect, verify, convert with --to." pace-analyzer/src/main.rs:27-72
REALITY: surface-check pass "Every pace_config default has a corresponding override in the integration guide." pace-sdk/docs/INTEGRATION.md:20-40
REALITY: binding-decisions pass "The plan's D5 (integrate in-process through a C callback, with the file format as a secondary path) is honoured by the observer API and the trace writer; the review found no reintroduced overruled default." PLAN.md:313
REALITY: numbers pass "The SDK's hello world printed on_cpu=3372 preempted=0 blocked=1113 on a measured run." pace-sdk/docs/INTEGRATION.md:1-5
REALITY: numbers pass "The analyzer's 26 tests pass, of which 7 cover the error contract." pace-analyzer/tests/error_contract.rs:1
REALITY: numbers pass "The SDK's 96 lifecycle checks pass, of which 33 cover the error contract across nine status codes." pace-sdk/tests/unit/test_lifecycle.c:263
REALITY: deferred-asks pass "The hosted API reference and the summary call are the only DX items left out; the inline header documentation is complete and a hosted reference needs a hosting decision." pace-sdk/include/pace/pace.h:1-327
REALITY: pr-count pass "No .gstack/ship-policy.json exists, so the default of one PR per wave applies; the error contract was committed as one wave because it is one change to one contract." PLAN.md:296-306
