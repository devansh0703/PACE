# PACE

**P**reemption-**A**ware **T**ime **A**ttribution — a portable, embeddable
measurement of where a thread's wall time actually went.

For every thread, at every sample:

```
wall_ns == on_cpu_ns + preempted_ns + blocked_ns
```

The invariant holds exactly, on every sample, and the analyzer checks it.

**One product, three ways to use it.** A small C and C++ library you link into
your own profiler. An open-source analyzer that reads a trace and proves the
measurement. A hosted service that runs the analysis for you, so you can see
what PACE says about a run without building anything.

The site: **[devansh0703.github.io/PACE](https://devansh0703.github.io/PACE/)**.
The service: **[pace-service.onrender.com](https://pace-service.onrender.com/)**.
The source: **[github.com/devansh0703/PACE](https://github.com/devansh0703/PACE)**.
The maintainer: **[devanshraulo@gmail.com](mailto:devanshraulo@gmail.com)**.

## Why it exists

A thread that spends most of its wall time waiting for a core and a thread that
spends the same time waiting on a lock look identical to a CPU-time counter, and
they need opposite fixes. The first is a thread-count problem; the second is a
code problem. A profiler that reports only CPU time cannot tell you which one
you have, so teams optimise code that was never the bottleneck.

Every mainstream profiler reports the symptom. The two candidate causes need
opposite fixes, and separating them requires scheduler accounting gathered
in-process, at sample time, before the information is gone.

## What it measures

| | meaning |
|---|---|
| `on_cpu_ns` | the thread was running |
| `preempted_ns` | the thread was **runnable** and the scheduler had other work |
| `blocked_ns` | the thread was neither running nor runnable — sleeping, on a lock, on I/O |

Preemption is the run-queue wait. Blocking is the remainder. Both are measured,
not modelled, and the analyzer refuses to report on a trace where the three do
not add up to the wall time.

## Use it

```bash
# 1. link the library into your profiler (vcpkg, Conan, or a checkout)
vcpkg install pace --overlay-ports=./PACE/ports

# 2. or read a trace with the analyzer
git clone https://github.com/devansh0703/PACE
cd PACE/pace-analyzer && cargo build --release
./target/release/pace analyze trace.jsonl

# 3. or skip the build entirely
curl --data-binary @trace.jsonl https://pace-service.onrender.com/analyze
```

## The measured proof

48 spinners on 16 hardware threads, one thread instrumented. Every thread lost
roughly 60 % of its wall time to preemption while never blocking on anything:

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

Every figure is checked against a source that does not come from PACE. Full
method in [pace-sdk/docs/ACCURACY.md](pace-sdk/docs/ACCURACY.md).

| Quantity | Against | Agreement |
|---|---|---|
| on-CPU time | the kernel's own CFS accounting | 0.0006 % |
| on-CPU time | `perf sched timehist` "run time" | 0.0574 % |
| off-CPU time | `perf sched timehist` "wait time" | 1.9094 % |
| preempted time | `perf sched timehist` "sch delay" | 1.1025 % |
| the decomposition invariant | 31,811 real samples | **exact** |

## Cost

The per-sample cost is measured by the SDK's own harnesses, not quoted. The
figures are in `pace-sdk/results/overhead.json` and the site renders them.

| Configuration | Median per call |
|---|---|
| Tier 0, per sample, no privilege | hundreds of nanoseconds |
| Session create + destroy, once per process | tens of microseconds |
| Thread registration, once per thread | a hundred nanoseconds |

At a thousand samples a second per thread, the first row is thousandths of one
per cent of a core. `pace-sdk/scripts/collect_overhead.py` regenerates it.

## Where the claim stops

Intel VTune's Threading Analysis already separates "Preemption Wait Time" from
"Inactive Sync Wait Time" on Linux. Perfetto is already an embeddable SDK.
**This is not a claim that nobody can do this.** The claim is narrower:

| | VTune | Perfetto | PACE |
|---|---|---|---|
| Embeddable in a third-party engine | no | yes | yes |
| Needs perf access | yes | varies | **no** |
| Vendor-neutral | Intel-branded | yes | yes |
| Output | VTune's model | a trace | **your** model, in-process |
| Cost per sample | — | trace buffers, IPC | a few hundred ns |

See [docs/COMPETITIVE-AND-PRIOR-ART.md](docs/COMPETITIVE-AND-PRIOR-ART.md).

## The repository

```
README.md                      this file: what the product is and how to get it
PLAN.md                        the implementation plan and the decision record
pace-sdk/                      the C and C++ library you link into a profiler
pace-analyzer/                 the open-source analyzer and the `pace` CLI
service/                       the hosted analyzer: pace analyze over HTTP
websites/pace/                 one product site, light by default
ports/pace/                    the vcpkg port
scripts/                       the checks that keep the numbers honest
docs/                          the research half: the method and the reviews
```

They are **one repository**, so the parts ship together. The licences differ
per directory: `pace-sdk/` is BUSL-1.1 and converts to Apache-2.0 on
2030-11-01; `pace-analyzer/` and `service/` are Apache-2.0.

## How the numbers stay honest

A published figure is either measured or it is not printed.

- `pace-sdk/results/accuracy.json` is produced by `tests/verify/verify_accuracy.py`
  and cross-checked against the kernel's own accounting and against perf.
- `pace-sdk/results/overhead.json` is produced by `scripts/collect_overhead.py`.
- `pace-analyzer/results/real-trace-report.txt` is the analyzer's own output on a
  real contended run.
- `scripts/check_doc_numbers.py` fails the build when a document quotes a number
  that does not match the artifact.
- `websites/scripts/check_site_accuracy.py` re-checks the **built** site against
  the same artifact, so the page cannot drift from the measurement it renders.
- `websites/scripts/check_design_contract.py` fails the build when the site
  drifts from the visual contract it was designed against.

Every figure on the site is generated from those artifacts at build time. None
of them is typed into a page.

## The hosted service

`service/` is a transport in front of the analyzer, not a second product. It
embeds the same crate the CLI is built from, so the numbers it returns are the
numbers the command line returns.

| | |
|---|---|
| URL | https://pace-service.onrender.com |
| Endpoints | `GET /health`, `GET /`, `POST /analyze` |
| Limits | 8 MiB body, one million samples, 20 s budget, one request at a time |
| Storage | none — the trace is read into the request's buffer and dropped with it |

It runs on a single free Render instance. That is a real constraint: a free
instance has a tenth of a core, half a gigabyte of memory, sleeps after
fifteen minutes without a request and takes about a minute to wake. An uptime
monitor on `/health` every five minutes is what keeps that from being visible.
Every limit it has is a limit of the free host, not of the method — run
`pace analyze` locally and none of them apply.

## Contact

Questions, bugs and feature requests: **[devanshraulo@gmail.com](mailto:devanshraulo@gmail.com)**
or [the issue tracker](https://github.com/devansh0703/PACE/issues).

It is one maintainer, not a company. There is no support contract, no paid tier
and nothing gated behind a form.

## Licence

`pace-sdk` is Business Source License 1.1, which is source-available rather than
open source: reading, modifying and developing against it are free, and
production use inside a shipped product needs terms until the Change Date, when
it becomes Apache-2.0.

`pace-analyzer` and `service` are Apache-2.0 — OSI open source, attribution only.

The site's [licence page](https://devansh0703.github.io/PACE/license/) has the
table of what each licence permits.
