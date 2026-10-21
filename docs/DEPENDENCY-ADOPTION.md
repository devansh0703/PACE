# PACE — Dependency Adoption Review

What PACE uses, what it could adopt, and why each choice was made. Every
version was checked against the upstream tag list, not a package index
snapshot, on 2026-10-21.

The rule this document applies is the project's own: **adopt an existing
implementation rather than hand-rolling one, and always take the latest
version** — except where a dependency would cost more than it saves, which
has to be argued rather than assumed.

---

## 1. The summary

| What | Adopted | Version | Licence | Verdict |
|---|---|---|---|---|
| Rust CLI parsing | `clap` (derive) | 4.6.7 | MIT OR Apache-2.0 | **Keep.** The right tool, already current |
| Rust JSON | `serde` + `serde_json` | 1.0.229 / 1.0.154 | MIT OR Apache-2.0 | **Keep.** Both current |
| Rust errors | `thiserror` | 2.0.21 | MIT OR Apache-2.0 | **Keep** |
| Rust benchmarks | `criterion` | 0.8.2 | MIT | **Keep** |
| C micro-benchmarks | Google Benchmark | **v1.10.0** | Apache-2.0 | **Keep.** The newest release |
| eBPF loading | `libbpf` | **v1.8.0** | LGPL-2.1 OR BSD-2-Clause | **Keep**, but bump from 1.6.3 |
| Perf events | `perf_event_open` | kernel ABI | n/a (system call) | **Keep.** There is no library to adopt |
| JSON in the SDK's trace writer | cJSON | v1.7.19 | MIT | **Rejected** — see §4 |
| CSV in the analyzer | `csv` | 1.4.0 | MIT OR Unlicense | **Rejected** — see §5 |
| Regex in the analyzer | `regex` | — | MIT OR Apache-2.0 | **Rejected** — the code uses no regex at all |
| Benchmark harnesses for the SDK | nanobench | — | MIT | **Rejected** — Google Benchmark is better and already adopted |

**Nothing in the analyzer is hand-rolled that a maintained library covers.**
The one place a library would help — the CSV parser — is covered below,
because the reason it is hand-written turned out to be wrong.

## 2. The Rust analyzer's dependencies

All four are current, and the transitive tree is small.

| Crate | Version | Licence | Why it is here | Runner-up, and the tradeoff |
|---|---|---|---|---|
| `clap` | 4.6.7 | MIT OR Apache-2.0 | The CLI: subcommands, `--help`, argument types, value enums for the format flags | `argh` (MIT) is smaller and has no dependencies, but it has no derive and no value enums, so the format flags would be hand-parsed. `clap` earns its weight in the `--help` output alone |
| `serde` | 1.0.229 | MIT OR Apache-2.0 | The derive macros on `Sample` and `Trace`, and the JSON field names | `miniserde` is lighter, but the derive output is not stable enough to be worth the maintenance |
| `serde_json` | 1.0.154 | MIT OR Apache-2.0 | Parsing and emitting JSON | `simd-json` is faster on large inputs, but this tool parses traces of a few hundred thousand samples, and `simd-json`'s API is less pleasant for the error messages the agent-ready contract needs |
| `thiserror` | 2.0.21 | MIT OR Apache-2.0 | The `Error` enum with its `code()`, `fix()` and `recovery()` methods | `anyhow` for the binary and `Box<dyn Error>` for the library. `anyhow` was in the tree and was **removed**: it could not carry the typed error, and its default rendering printed "parse error: parse error:" |
| `criterion` | 0.8.2 | MIT | The analyzer's benchmarks: the aggregation and the verification, over every sample | `divan` is faster to compile, but Criterion's statistics and its comparison against a saved baseline are what the accuracy report needs |

`Cargo.lock` is committed for the binary, so the published analysis is
reproducible, and it is gitignored for the workspace's convenience only.

## 3. The SDK's dependencies

| What | Version | Licence | Notes |
|---|---|---|---|
| Google Benchmark | v1.10.0 | Apache-2.0 | Fetched, not vendored, so the repository carries no third-party source tree. `GIT_TAG` is pinned, so the published overhead numbers come from an exact version |
| libbpf | v1.8.0 upstream | LGPL-2.1 OR BSD-2-Clause | The eBPF tier's loader. Upstream's newest release is v1.8.0. The loader uses `bpf_object__open_skeleton`, `bpf_program__attach` and `bpf_map__fd`, none of which changed in 1.7 or 1.8 |
| pthreads | system | n/a | The session's registry mutex and the thread-local fast path |
| `perf_event_open` | kernel ABI | n/a | There is no library to adopt here: the perf interface is a system call, and the only wrapper (libpfm) adds a build dependency for no gain at this level of use |
| BTF / `vmlinux.h` | from the running kernel's `/sys/kernel/btf/vmlinux` | n/a | The CO-RE skeleton is generated at build time by `bpftool`, so the object matches the *building* kernel's BTF and relocates at load time |

**The libbpf licence deserves a note.** libbpf is LGPL-2.1 OR BSD-2-Clause.
A commercial SDK linking it must respect the BSD-2-Clause option, which is
permissive: no copyleft, no source disclosure. The choice of the
BSD-2-Clause option is deliberate and must stay deliberate. The eBPF tier
is optional, so a consumer who cannot accept the licence still gets tier 0
and tier 1 with no libbpf at all — the library is only linked when
`PACE_ENABLE_BPF=ON`.

## 4. JSON in the SDK's trace writer: cJSON considered and rejected

**The candidate.** [cJSON](https://github.com/DaveGamble/cJSON) v1.7.19
(MIT), the standard single-file C JSON library. The newest tag, confirmed
against the upstream tag list. It is C89, which fits the SDK's portability
target, and it is what most embedded C projects use.

**The runner-up.** [mjson](https://github.com/mattiaswerner/mjson) (MIT), a
single-header C JSON writer with no allocation.

**Why neither was adopted.** The SDK's trace writer does not *parse* JSON;
it only *emits* one fixed object per sample, and every value in it is a
`uint64_t` or a `double` except one field, the `source` string, which is
one of three compile-time constants. Adding a JSON library to a commercial
SDK for that would mean:

- a new dependency in the public build, which a licensee has to audit,
  vendor or waive;
- a second licence to track in the BUSL header of every file;
- an allocation per sample in cJSON's case, on a path whose entire selling
  point is that it does not allocate.

`fprintf` with a fixed format string is not "hand-rolling JSON" in the
sense the rule is aimed at. There is no parser, no escaping logic, no
schema, and no untrusted input. The one thing that could go wrong — a
string in the output needing escaping — cannot happen, because the only
strings are `"perf"`, `"clock"` and `"unknown"`.

**What would change the decision.** If the trace writer ever gained a
user-supplied string (a thread name, a label, a phase), the answer becomes
"adopt cJSON" immediately, because that is when escaping becomes a
correctness problem rather than a non-issue. The decision is recorded so
the next person does not have to re-derive it.

## 5. CSV in the analyzer: the `csv` crate considered, and the rationale corrected

**This is where the CEO review's reasoning was wrong, and finding that
mattered.**

The CEO review amended binding decision D6 to keep the analyzer's CSV and
text parsers hand-written, on the grounds that "a regex over untrusted
trace files carries catastrophic backtracking". That is a sound argument
against the `regex` crate — but the `csv` crate is **not** `regex`. The
`csv` crate is a hand-written RFC 4180 state machine with no backtracking
and no regular expressions at all. The review conflated two different
crates.

**The candidate.** [csv](https://crates.io/crates/csv) 1.4.0 (MIT OR
Unlicense), the standard Rust CSV reader, with serde support.

**The runner-up.** [simd-csv](https://crates.io/crates/simd-csv) 0.14.0
(MIT), a SIMD-accelerated reader.

**Why the hand-written parser is still correct — for a different reason.**
The `perf trace` format is not CSV. It is:

```text
     0.000 busy/4194266 sched:sched_switch(prev_comm: "busy", prev_pid: 4194266 (busy), ...)
```

The fields are separated by spaces, the arguments are a parenthesised
`key: value` list with quoted strings, and the time column is
**milliseconds** relative to the first event. There is no header row, so
the `csv` crate's entire model — a header followed by records — does not
apply. `simd-csv` is the same model.

**And the hand-written parser was broken.** The version that shipped
expected a CSV header of `comm,timestamp,event,event_type`, which `perf
trace` **does not produce** — this perf version has no CSV output mode at
all. So the parser matched no real perf trace, and every sample it
produced was all-zero. It was tested only against synthetic input in the
shape it assumed.

The parser has been rewritten for the format perf actually emits, and
`tests/perf_trace_parser.rs` now tests it against captured real output.
It recovers real on-CPU intervals from a real `sched_switch` stream.

**What this changes about the rule.** The correct version of the decision
is: *adopt a library when the format is the one the library parses.* The
`perf sched timehist` format is a whitespace-aligned table, which the
hand-written parser handles and which `csv` does not either. The `perf
trace` format is a key-value event log, which no CSV library handles. So
both parsers stay hand-written — but for the right reason, and now they
work.

## 6. What is deliberately not adopted

| Rejected | Why |
|---|---|
| **Perfetto's trace SDK** | The original plan listed it. It is a large C++ build, its schema is a poor fit for a per-thread ledger, and it brings a trace pipeline where PACE's selling point is a counter. The CEO review removed it |
| **The protobuf `TracePacket` schema** | Same reason. PACE reads JSON, not protobuf, and the two formats that matter are PACE's own and Chrome's |
| **`regex` for the text parsers** | The original reason was sound: catastrophic backtracking over untrusted input. The current code uses no regex at all, so the argument is now moot but the decision stands |
| **A JSON library in the SDK** | §4 |
| **A CSV library in the analyzer** | §5 — the formats are not CSV |
| **libpfm for perf events** | A wrapper around a system call, adding a build dependency for no gain |
| **A thread-pool library** | The SDK must not add threads to the host process. That is a product requirement, not a preference |
| **A logging framework** | The SDK has one observer callback and no logging. A framework would be a dependency with no user |
| **RapidJSON, jsoncpp, yyjson in the SDK** | All heavier than cJSON and all solve a parsing problem the SDK does not have |

## 7. The version currency check

Every version above was verified against the upstream tag list, not a
package index, on 2026-10-21:

| Project | Newest tag | In the tree | Current? |
|---|---|---|---|
| google/benchmark | v1.10.0 | v1.10.0 | yes |
| libbpf | v1.8.0 | **1.6.3 → 1.8.0** | now yes |
| cJSON | v1.7.19 | not adopted | n/a |
| csv | 1.4.0 | not adopted | n/a |
| clap | 4.6.7 | 4.6.7 | yes |
| serde | 1.0.229 | 1.0.229 | yes |
| serde_json | 1.0.154 | 1.0.154 | yes |
| thiserror | 2.0.21 | 2.0.21 | yes |
| criterion | 0.8.2 | 0.8.2 | yes |

**One dependency was unsatisfiable and is now fixed.** The Conan recipe
required `libbpf/1.6.3`, but **ConanCenter's newest libbpf is 1.3.0** —
there is no 1.6.3 and no 1.8.0 there. A consumer enabling the eBPF tier
could not have resolved the requirement at all. The recipe now requires
`libbpf/[>=1.3.0 <2.0.0]`, which conancenter satisfies and which a
consumer with a newer libbpf on another remote can still pin. Verified:
`conan create -o with_ebpf=True --build=missing` builds and packages.

The vcpkg port resolves `libbpf` through vcpkg's own registry, so it
always takes the version vcpkg ships.

**The general lesson.** A pinned dependency version is a claim that the
version exists in the registry the package is consumed from. Pinning
upstream's newest is not the same as pinning something a consumer can
install. The two must be checked separately, and the second one was not.

## 8. The rule, restated after this review

The original rule — "adopt an existing implementation rather than
hand-rolling one" — is right, and this review found one place where it was
applied for the wrong reason and one place where it was not applied at all.

**Corrected rule.** *Adopt a maintained library when it solves the problem
you actually have. Do not adopt one because the general rule says to, and
do not hand-roll because a rule was misread. When you hand-roll, write
down the reason, because the reason is what the next person has to
re-check.*

Two things this review changed, both of which were real defects:

1. The `perf trace` parser was rewritten, because the version that shipped
   matched no real perf output and produced only zero-length samples.
2. The libbpf requirement was unsatisfiable: it pinned 1.6.3, and
   ConanCenter's newest is 1.3.0. It is now a range that resolves, and the
   eBPF-enabled package was built to prove it.

And one thing it did not change: the SDK's trace writer stays on
`fprintf`, because the JSON it emits has no variable content.
