# PACE — the library

**P**reemption-**A**ware **T**ime **A**ttribution.

This is the C and C++ half of one product. The other halves are the
[analyzer](../pace-analyzer/), which proves the measurement, and the
[hosted service](../service/), which runs the analyzer for you. They share one
model of time, and this directory is the artifact a vendor links into their own
profiler.

A portable C and C++ library that tells a developer-tool vendor **where a
thread's wall time actually went**: how much was spent running, how much was
lost to OS preemption, and how much was blocked.

You link it into your profiler. It adds no threads to your process, allocates
nothing on the hot path, and needs no elevated privilege.

```c
pace_config cfg;
pace_config_init(&cfg);

pace_session *session;
pace_session_create(&cfg, &session);

pace_thread *thread;
pace_session_register_thread(session, &thread);

pace_sample sample;
pace_session_sample(session, thread, &sample);

printf("on_cpu=%llu preempted=%llu blocked=%llu\n",
       sample.on_cpu_ns, sample.preempted_ns, sample.blocked_ns);
```

## The invariant

Every sample satisfies

```
wall_ns == on_cpu_ns + preempted_ns + blocked_ns
```

exactly. The analyzer checks it on every sample of a trace and refuses to
report on a trace that does not hold together.

- `on_cpu_ns` — the thread was running.
- `preempted_ns` — the thread was *runnable* and the scheduler had other
  work. This is the time that makes a scheduler problem look like a code
  problem.
- `blocked_ns` — the thread was neither running nor runnable: sleeping, on a
  lock, or waiting on I/O.

## The tiers

| Tier | Needs | Gives you |
|---|---|---|
| 0 | nothing | on-CPU, preempted, blocked, in nanoseconds |
| 1 | `CAP_PERFMON` or `perf_event_paranoid <= 1` | + measured core cycles |
| 2 | BTF + privilege to attach tracepoints | + the scheduler's own switch events |

Each tier is an addition to the one below. None of them is required, and the
sample always carries the tier it came from so you never present a derived
number as a measured one.

The per-call cost is **measured**, not quoted: `results/overhead.json`, produced
by `scripts/collect_overhead.py`, holds the median and p99 for a sample, the
median cost of creating a session, and the median cost of registering a thread.
The site renders those figures from the artifact rather than repeating them in
prose.

### The platforms

| Platform | Tier 0 | Tier 1 | Tier 2 | Three-way split |
|---|---|---|---|---|
| Linux | clocks + `/proc/<tid>/schedstat` | hardware counters | eBPF | **yes** |
| Windows | `GetThreadTimes` + `QueryThreadCycleTime` | — | — | no, without an elevated ETW session |
| macOS | `CLOCK_THREAD_CPUTIME_ID` + `thread_info` | — | — | no |

Linux reports the run-queue delay directly, so it can separate preemption
from blocking. **Windows and macOS cannot**, so their samples carry neither
`PACE_SAMPLE_F_HAS_PREEMPTED` nor `PACE_SAMPLE_F_HAS_BLOCKED`, and the
off-CPU time is reported as "not running" rather than as a split. A consumer
must read the flags: a missing flag means "not measured", not "zero".

The Windows and macOS backends are cross-compiled and their test suites run
in CI. The macOS backend is compiled against the Mach interface; the
Windows backend is compiled with the MinGW toolchain.

## Building

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

The eBPF tier needs `clang` with a BPF target, `bpftool`, and `libbpf-dev`.
Without them the SDK builds without it and tier 0 carries the measurement.

## Installing

```bash
cmake --install build --prefix /usr/local
```

Consumers use it with `find_package`:

```cmake
find_package(pace REQUIRED)
target_link_libraries(my_profiler PRIVATE pace::pace)
```

vcpkg and Conan recipes are in `vcpkg/` and `conanfile.py`.

## Accuracy

Every number this library reports is checked against the kernel's own
accounting and against perf's independent reconstruction of the scheduler
events. The results are in `results/accuracy.json`, produced by
`tests/verify/verify_accuracy.py`. See [docs/ACCURACY.md](docs/ACCURACY.md)
for the method, the ground-truth sources and the measured figures.

## Documentation

- [docs/INTEGRATION.md](docs/INTEGRATION.md) — integrating into a profiler.
- [docs/ACCURACY.md](docs/ACCURACY.md) — how the numbers are verified.
- [../docs/OS-PRIMITIVES.md](../docs/OS-PRIMITIVES.md) — the
  operating-system primitives, their permission requirements, and their
  measured per-call costs.
- [docs/PREMISE-VERIFICATION.md](docs/PREMISE-VERIFICATION.md) — the raw
  measurements the method rests on.
- [../service/README.md](../service/README.md) — the hosted analyzer.
- [LICENSING.md](LICENSING.md) — the licence, and what a licensee gets.

## Licence

Business Source License 1.1. See [LICENSING.md](LICENSING.md).

That is source-available rather than open source, and it is stated plainly
rather than described as if it were the other thing: reading, modifying and
developing against the library are free, production use inside a shipped
product needs terms until the Change Date, and every version converts to
Apache-2.0 on 2030-11-01. The full terms and the permitted-use grant are in
LICENSING.md, and the site's licence page carries the table.

The companion analyzer, `pace-analyzer`, is Apache-2.0.
