# Integrating PACE into your profiler

This is the whole integration. If it takes more than a page, something has
gone wrong.

## 1. Link it

```cmake
find_package(pace REQUIRED)
target_link_libraries(my_profiler PRIVATE pace::pace)
```

vcpkg: the port is in this repository, as an overlay directory.

```sh
git clone https://github.com/devansh0703/PACE
vcpkg install pace --overlay-ports=/path/to/PACE/ports
```

or, in a manifest, with the repository added as an overlay:

```json
{
  "dependencies": [ "pace" ],
  "vcpkg-configuration": {
    "overlay-ports": [ "/path/to/PACE/ports" ]
  }
}
```

Conan:

```toml
[tool.conan.dependencies]
pace = "0.1.2"
```

## 2. Create a session

Do this once, at profiler start-up.

```c
#include <pace/pace.h>

pace_config cfg;
pace_config_init(&cfg);

pace_session *session = NULL;
pace_status status = pace_session_create(&cfg, &session);
if (status != PACE_OK) {
  /* The platform does not support the measurement. Continue without it,
     and say so in your own UI. */
  log_error("PACE unavailable: %s", pace_status_string(status));
}
```

The defaults are right for most callers:

| `pace_config` field | Default | What it does |
|---|---|---|
| `prefer_perf_counters` | 1 | Use the hardware counter when it is available, fall back silently when it is not |
| `keep_perf_fds_open` | 1 | Keep one descriptor per thread. Turn it off in a process with thousands of threads |
| `collect_switch_counters` | 0 | Read the voluntary/involuntary switch counters. Costs about 4 µs per sample |
| `prefer_ebpf_tier` | 1 | Use the eBPF tier when it is available. Falls back silently |

## 3. Sample the thread you are profiling

One call, on the thread being profiled, wherever you already take a sample.

```c
pace_thread *thread = NULL;
pace_session_register_thread(session, &thread);

pace_sample sample;
if (pace_session_sample(session, thread, &sample) == PACE_OK) {
  /* sample.on_cpu_ns    time it ran
     sample.preempted_ns time it was runnable and lost the core
     sample.blocked_ns   time it was neither */
}
```

`pace_session_sample_self(session, &sample)` does the registration for you
when you are only ever sampling the calling thread.

**The first sample for a thread establishes the baseline** and returns a
zero-duration sample with `PACE_SAMPLE_F_DEGRADED` set. Discard it.

## 4. Use the numbers

```c
uint64_t wall      = sample.wall_ns;
uint64_t on_cpu    = sample.on_cpu_ns;
uint64_t preempted = sample.preempted_ns;
uint64_t blocked   = sample.blocked_ns;
```

`wall == on_cpu + preempted + blocked` holds exactly.

Check `sample.flags` before you report a number:

| Flag | Meaning when set |
|---|---|
| `PACE_SAMPLE_F_HAS_PREEMPTED` | `preempted_ns` is a real measurement |
| `PACE_SAMPLE_F_HAS_BLOCKED` | `blocked_ns` is a real measurement |
| `PACE_SAMPLE_F_HAS_CORE_CYCLES` | `core_cycles` is measured, not derived |
| `PACE_SAMPLE_F_DEGRADED` | some field in this window is partial |
| `PACE_SAMPLE_F_WAS_PREEMPTED` | the thread was preempted during the window |

A clear `PACE_SAMPLE_F_HAS_PREEMPTED` means the platform could not separate
preemption from blocking. Report "unknown", not "0 % preempted".

`sample.source` says where the cycle numbers came from:
`PACE_SOURCE_CLOCK` (derived from a measured frequency) or
`PACE_SOURCE_PERF` (measured by a hardware counter).

## 5. Or let the observer see everything

```c
static void on_sample(void *user_data, const pace_sample *sample) {
  my_profiler_record(user_data, sample);
}

pace_session_set_observer(session, on_sample, my_profiler);
```

The observer runs on the thread that called `pace_session_sample`. It must
not call back into the session, must not longjmp, and must not throw
across the C boundary.

## 6. Tell your users which tier you got

```c
pace_stats stats;
stats.struct_size = sizeof stats;
pace_session_stats(session, &stats);

printf("tier: %s, threads: %u, samples: %llu, degraded: %llu, io errors: %llu\n",
       stats.source == PACE_SOURCE_PERF ? "measured core cycles" : "derived cycles",
       stats.threads_registered,
       (unsigned long long)stats.samples_taken,
       (unsigned long long)stats.degraded_samples,
       (unsigned long long)stats.io_errors);
```

`stats.ebpf_available` says whether the eBPF tier attached. A non-zero
`stats.io_errors` means some thread's accounting could not be read.

## 7. Optionally write a trace

```c
pace_trace *trace;
pace_trace_open("profile.pace.jsonl", &trace);
...
pace_trace_write_sample(trace, thread_id, &sample);
...
pace_trace_close(trace);
```

The file is Chrome Trace Event compatible, so a viewer that already reads
Chrome traces opens it. `pace-analyzer` adds the attribution report on top:

```bash
pace analyze profile.pace.jsonl --output report.html
```

## Threading contract

- `pace_session_create`, `pace_session_destroy`, `pace_session_set_observer`
  and `pace_session_stats` are **not** safe to call concurrently with
  sampling. Call `set_observer` before sampling starts.
- `pace_session_sample` and `pace_session_sample_self` **are** safe to call
  concurrently from different threads, each with its own `pace_thread`. This
  is checked under the thread sanitizer: sixteen threads sampling one
  session, and threads leaving while others sample. The session's counters
  are incremented atomically, because a plain `++` from two threads loses an
  increment.
- A `pace_thread` is owned by the thread that registered it. Passing one
  thread's handle to another thread to sample is a data race.
- `pace_session_destroy` invalidates every `pace_thread` it returned.
  Using one afterwards is undefined.
- `pace_session_destroy` must be called exactly once per session.

## What PACE does not do

It is not a profiler. It does not unwind stacks, symbolise frames, sample
memory, or attribute GPU time. It does not add threads to your process. It
does not require privilege, except for the optional tiers.

**On Windows and macOS, PACE cannot separate preemption from blocking.**
Neither platform has a public API that reports how long a thread was
runnable but not scheduled, and the ETW and Mach interfaces that could
reconstruct it need an elevated session. On those platforms:

- `on_cpu_ns` and `blocked_ns` are measured, in nanoseconds;
- `preempted_ns` is not measured, and is reported as zero;
- `PACE_SAMPLE_F_HAS_PREEMPTED` and `PACE_SAMPLE_F_HAS_BLOCKED` are
  **clear**, so the split is not claimed;
- `source` is `PACE_SOURCE_CLOCK` and `reference_hz` is 0, because neither
  platform has a constant-rate cycle counter. The cycle count and the
  elapsed time are reported separately and are never derived from one
  another.

So on Windows and macOS, check the flags before you display a split:

```c
if (sample.flags & PACE_SAMPLE_F_HAS_PREEMPTED) {
  /* the split is real */
} else {
  /* display "not available", never "0 % preempted" */
}
```

Linux is the platform where the three-way split is a measurement. The
others are on-CPU versus blocked.

## Verifying it yourself

The concurrency claim is checkable. The SDK's build takes a
`PACE_SANITIZER` option:

```bash
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPACE_SANITIZER=thread
cmake --build build-tsan -j 8
./build-tsan/pace_concurrency_tests
```

`PACE_SANITIZER` also takes `address` and `undefined`. The test is in
`tests/unit/test_concurrency.c`, and it checks the decomposition invariant
on every sample, because a race in the differencing shows up as a broken
sum long before it shows up as a crash.

## Cost

One `pace_session_sample` call costs:

| Tier | Median | p99 |
|---|---|---|
| 0 (no privilege) | 524 ns | 587 ns |
| 1 (hardware counters) | 1.01 µs | 1.91 µs |

At 1000 samples per second per thread, tier 0 costs 0.05 % of a core.

## Runbook

| Symptom | Cause | What to do |
|---|---|---|
| `pace_session_create` returns `PACE_ERR_UNSUPPORTED` | the platform has none of the required primitives | continue without PACE and say so in your UI |
| `pace_session_create` returns `PACE_ERR_PERMISSION` | tier 1 was requested and is unavailable | check `CAP_PERFMON` or `perf_event_paranoid`; tier 0 still works |
| `stats.source` is `PACE_SOURCE_CLOCK` on a machine you expect measured cycles on | `perf_event_paranoid` is above 1, or the process lacks `CAP_PERFMON` | check `cat /proc/sys/kernel/perf_event_paranoid` |
| `stats.io_errors` is climbing | threads are exiting while being sampled | normal for short-lived threads; a large count means the consumer is sampling threads it does not own |
| `PACE_SAMPLE_F_DEGRADED` on every sample | the platform cannot measure part of the window | check the flags to see which part is missing |
| `stats.ebpf_available` is 0 | the kernel has no BTF, or attaching to the tracepoints needs privilege | tier 0 and tier 1 are unaffected |
