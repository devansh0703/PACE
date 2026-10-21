# PACE

**P**reemption-**A**ware **T**ime **A**ttribution — a portable, embeddable
measurement of where a thread's wall time actually went.

Two products, from one idea
([Idealab #1352567](https://idealab.ai/idea-details/1352567)).

| | | |
|---|---|---|
| **[pace-sdk](pace-sdk/)** | a licensed C and C++ library you link into your profiler | BUSL-1.1 |
| **[pace-analyzer](pace-analyzer/)** | the open-source trace analyzer that proves the measurement | Apache-2.0 |
| **[websites](websites/)** | the two product sites | — |

The product sites are live: **[devansh0703.github.io/PACE](https://devansh0703.github.io/PACE/)**
and **[the analyzer's site](https://devansh0703.github.io/PACE/analyzer/)**.

## The problem

A game engine frame takes 40 ms. The wall clock says 40 ms. The thread's CPU
time says 12 ms. **The other 28 ms went somewhere**, and the two candidate
causes need opposite fixes:

- the **scheduler** took the core away → fix the thread count, the priority,
  the affinity, or the machine;
- the thread was **blocked** on a lock or on I/O → fix the code.

Every mainstream profiler reports the symptom (a slow frame) and the CPU
time. None of them separates the cause, because separating it requires
scheduler accounting that has to be gathered in-process, at sample time,
before the information is gone.

## What PACE measures

For every thread, at every sample:

```
wall_ns == on_cpu_ns + preempted_ns + blocked_ns
```

| | meaning |
|---|---|
| `on_cpu_ns` | the thread was running |
| `preempted_ns` | the thread was **runnable** and the scheduler had other work |
| `blocked_ns` | the thread was neither running nor runnable — sleeping, on a lock, on I/O |

The invariant holds exactly, on every sample, and the analyzer checks it.

## The measured proof

48 spinners on 16 hardware threads, one thread instrumented, 310 ms of wall
time on an i7-13620H:

| Quantity | Measured |
|---|---|
| Wall time | 310,446,432 ns |
| On-CPU time | 100,153,447 ns |
| **Run-queue wait (involuntary preemption)** | **210,321,285 ns — 67.8 % of wall** |
| Involuntary switches | 134 |
| Voluntary switches | 0 |
| Decomposition | **exact** |

A profiler that reports 100 ms of CPU time for a 310 ms frame has told you
nothing about the 210 ms that was lost.

## Measured accuracy

Every figure is checked against a source that does not come from PACE. Full
method in [pace-sdk/docs/ACCURACY.md](pace-sdk/docs/ACCURACY.md).

| Quantity | Against | Agreement |
|---|---|---|
| on-CPU time | the kernel's own CFS accounting | 0.0029 % |
| on-CPU time | `perf sched timehist` "run time" | 0.0357 % |
| off-CPU time | `perf sched timehist` "wait time" | 2.6638 % |
| preempted time | `perf sched timehist` "sch delay" | 0.0000 % |
| the decomposition invariant | 107,405 real samples | **exact** |

## Cost

| Tier | Needs | Cost per sample |
|---|---|---|
| 0 | nothing | **524 ns** |
| 1 | `perf_event_paranoid <= 1` | 1.01 µs |
| 2 | BTF + privilege | same as tier 0 |

At 1000 samples per second per thread, tier 0 costs 0.05 % of a core.

## Installing

```bash
# vcpkg
vcpkg install pace

# or Conan
conan install --requires=pace/0.1.0
```

Both install from the published release, so the version you get is the
version that was measured.

## Building from a checkout

```bash
# the SDK
cd pace-sdk && cmake -S . -B build -G Ninja && cmake --build build -j 8 && ctest --test-dir build

# the analyzer
cd pace-analyzer && cargo build --release && cargo test --release

# the sites
cd websites/pace-sdk && npm ci && npm run build
```

## What is in this repository

```
PLAN.md                     the implementation plan and the decision record
docs/CEO-REVIEW.md          the independent review that corrected the positioning
docs/ENG-REVIEW.md          the engineering review
docs/DX-REVIEW.md           the developer-experience review
docs/DESIGN-REVIEW.md       the design review
docs/IDEA-BRIEF.md          the source brief
docs/OS-PRIMITIVES.md       the OS primitives, and their measured costs
docs/DEPENDENCY-ADOPTION.md what is adopted, and why
docs/COMPETITIVE-AND-PRIOR-ART.md
pace-sdk/                   product 1: the C and C++ library
pace-analyzer/              product 2: the Rust trace analyzer
websites/                   the two product sites
```

They are **one repository**, so the products ship together and the licences
still differ per directory: `pace-sdk/` is BUSL-1.1 and `pace-analyzer/` is
Apache-2.0.

## The honest positioning

Intel VTune's Threading Analysis already separates "Preemption Wait Time"
from "Inactive Sync Wait Time" on Linux. Perfetto is already an embeddable
SDK. **This is not a claim that nobody can do this.** The claim is narrower
and survives contact with both:

| | VTune | Perfetto | PACE |
|---|---|---|---|
| Embeddable in a third-party engine | no | yes | yes |
| Needs perf access | yes | varies | **no** |
| Vendor-neutral | Intel-branded | yes | yes |
| Output | VTune's model | a trace | **your** model, in-process |
| Cost per sample | — | trace buffers, IPC | 524 ns |

See [docs/COMPETITIVE-AND-PRIOR-ART.md](docs/COMPETITIVE-AND-PRIOR-ART.md).

## Licence

`pace-sdk` is Business Source License 1.1.
`pace-analyzer` is Apache-2.0.
