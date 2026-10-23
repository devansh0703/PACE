#!/usr/bin/env python3
# SPDX-License-Identifier: BUSL-1.1
"""Collect the per-call cost figures into one committed artifact.

The accuracy harness produces `results/accuracy.json`. The per-call cost is a
different measurement from a different process, so it gets its own artifact
rather than being typed into a document: a cost figure copied by hand is a
figure that drifts, and the website renders this file at build time.

Two harnesses produce the numbers, and they disagree by about ten per cent:

  - `examples/pace_overhead.c` runs a fixed number of calls and reports the
    median and the p99 of those calls directly. This is the headline figure.
  - `bench/bench_sample.cc` is Google Benchmark, which reports the mean and,
    with repetitions, a median per benchmark. It also covers the costs a
    vendor pays once rather than per sample.

Both are recorded, because hiding the disagreement would be the opposite of
what this project is for.

Run: python3 scripts/collect_overhead.py
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
OUT = ROOT / "results" / "overhead.json"


def find_binary(relative: str) -> pathlib.Path | None:
    """The first build directory that has the binary, Release preferred.

    The SDK has several build directories on a development machine -- one per
    sanitizer, one for the degraded configuration -- and their binaries are not
    interchangeable for a published figure, so Release wins when it exists.
    """
    candidates = sorted(ROOT.glob(f"build*/{relative}"), key=lambda p: p.as_posix())
    release = [p for p in candidates if "release" in p.parent.parent.as_posix().lower()]
    for group in (release, candidates):
        if group:
            return group[0]
    return None


def run_example(binary: pathlib.Path, calls: int) -> dict:
    out = subprocess.run(
        [str(binary), str(calls)], capture_output=True, text=True, timeout=600, check=True
    ).stdout
    values = {}
    for name in ("min", "median", "p99", "max"):
        match = re.search(rf"^\s*{name}\s*:\s*(\d+) ns", out, re.MULTILINE)
        if match:
            values[name] = int(match.group(1))
    tier = re.search(r"^\s*tier\s*:\s*(.+)$", out, re.MULTILINE)
    return {
        "harness": "examples/pace_overhead.c",
        "calls": calls,
        "tier": tier.group(1).strip() if tier else "unknown",
        "min_ns": values.get("min"),
        "median_ns": values.get("median"),
        "p99_ns": values.get("p99"),
        "max_ns": values.get("max"),
    }


def run_benchmark(binary: pathlib.Path, repetitions: int, min_time: str) -> list[dict]:
    out = subprocess.run(
        [
            str(binary),
            f"--benchmark_min_time={min_time}",
            f"--benchmark_repetitions={repetitions}",
            "--benchmark_report_aggregates_only=true",
            "--benchmark_format=json",
        ],
        capture_output=True,
        text=True,
        timeout=900,
        check=True,
    ).stdout
    payload = json.loads(out)
    rows = []
    for entry in payload["benchmarks"]:
        name = entry["name"]
        if not name.endswith("_median"):
            continue
        rows.append(
            {
                "harness": "bench/bench_sample.cc (Google Benchmark)",
                "benchmark": name[: -len("_median")],
                "repetitions": entry["repetitions"],
                "median_ns": entry["real_time"] * unit_to_ns(entry["time_unit"]),
                "time_unit": entry["time_unit"],
            }
        )
    # The mean for the same benchmark, so the artifact shows both statistics
    # rather than only the flattering one.
    means = {
        e["name"][: -len("_mean")]: e["real_time"] * unit_to_ns(e["time_unit"])
        for e in payload["benchmarks"]
        if e["name"].endswith("_mean")
    }
    for row in rows:
        row["mean_ns"] = means.get(row["benchmark"])
    return rows


def unit_to_ns(unit: str) -> float:
    return {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}[unit]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--calls", type=int, default=10000, help="calls for the median/p99 harness")
    parser.add_argument("--repetitions", type=int, default=10)
    parser.add_argument("--min-time", default="0.2s")
    parser.add_argument("--out", type=pathlib.Path, default=OUT)
    parser.add_argument("--overhead-binary", type=pathlib.Path)
    parser.add_argument("--bench-binary", type=pathlib.Path)
    args = parser.parse_args()

    # Resolved to absolute paths up front: a relative path passed on the
    # command line would otherwise break the environment record below, which
    # stores each binary relative to the repository.
    overhead_binary = (
        args.overhead_binary.resolve() if args.overhead_binary else find_binary("examples/pace_overhead")
    )
    bench_binary = (
        args.bench_binary.resolve() if args.bench_binary else find_binary("bench/pace_bench_sample")
    )

    if overhead_binary is None or not overhead_binary.exists():
        print(
            "no pace_overhead binary found; configure a build with "
            "-DPACE_BUILD_EXAMPLES=ON and build it first",
            file=sys.stderr,
        )
        return 1
    if bench_binary is None or not bench_binary.exists():
        print(
            "no pace_bench_sample binary found; configure a build with "
            "-DPACE_BUILD_BENCH=ON and build it first",
            file=sys.stderr,
        )
        return 1

    artifact = {
        "tool": "collect_overhead",
        "note": (
            "per-call cost of the public API, from the SDK's own harnesses; "
            "regenerate with pace-sdk/scripts/collect_overhead.py"
        ),
        "example": run_example(overhead_binary, args.calls),
        "benchmarks": run_benchmark(bench_binary, args.repetitions, args.min_time),
        "environment": {
            "uname": subprocess.run(
                ["uname", "-srm"], capture_output=True, text=True, check=False
            ).stdout.strip(),
            "compiler": shutil.which("cc") or "cc",
            "binaries": {
                "overhead": str(overhead_binary.relative_to(ROOT)),
                "benchmark": str(bench_binary.relative_to(ROOT)),
            },
        },
    }

    # A self-check: the figure this artifact exists to publish must be present,
    # and a run that produced no median is a broken run rather than a zero.
    assert artifact["example"]["median_ns"], "the median/p99 harness reported no median"
    assert artifact["benchmarks"], "Google Benchmark reported no aggregates"

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"wrote {args.out.relative_to(ROOT)}")
    print(f"  tier 0 median {artifact['example']['median_ns']} ns, p99 {artifact['example']['p99_ns']} ns")
    for row in artifact["benchmarks"]:
        print(f"  {row['benchmark']}: median {row['median_ns']:.0f} ns")
    return 0


if __name__ == "__main__":
    sys.exit(main())
