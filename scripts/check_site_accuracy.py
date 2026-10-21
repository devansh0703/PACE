#!/usr/bin/env python3
"""Check that the site's accuracy table still matches the committed artifact.

This is the CI gate for the generator: it fails when the site and the
artifact disagree, which is what happens when someone edits a number by
hand or when the harness is re-run without regenerating the site.

Run: python3 scripts/check_site_accuracy.py
"""

from __future__ import annotations

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARTIFACT = ROOT.parent / "pace-sdk" / "results" / "accuracy.json"
SITE = ROOT / "pace-sdk" / "src" / "pages" / "index.astro"

# The measurements the site publishes. A check the site does not publish is
# not checked here.
PUBLISHED = [
    "on-CPU time vs /proc/<tid>/schedstat (kernel cfs accounting)",
    "on-CPU time vs perf sched timehist 'run time' column",
    "preempted time vs perf sched timehist 'sch delay' column",
    "off-CPU time (preempted + blocked) vs perf 'wait time' column",
    "wall time vs CLOCK_MONOTONIC read by the verifier",
]


def main() -> int:
    if not ARTIFACT.exists():
        print(f"no artifact at {ARTIFACT}; the accuracy harness has not been run")
        return 1
    artifact = json.loads(ARTIFACT.read_text())
    site = SITE.read_text()
    by_name = {m["name"]: m for m in artifact["measurements"]}

    failures: list[str] = []

    for name in PUBLISHED:
        m = by_name.get(name)
        if m is None:
            failures.append(f"the artifact has no measurement named {name!r}")
            continue
        for field in ("median_relative_error", "p95_relative_error"):
            value = f"{m[field] * 100.0:.4f} %"
            if value not in site:
                failures.append(
                    f"the site does not carry {value} for {name!r} ({field})"
                )

    samples = artifact["decomposition"]["total_samples"]
    if f"{samples:,}" not in site:
        failures.append(
            f"the site does not carry the sample count {samples:,}"
        )

    # The sample count must appear exactly once, in the decomposition row.
    stale = re.findall(r"<td>(\d{3,}(?:,\d{3})+)</td>", site)
    for value in stale:
        if value != f"{samples:,}":
            failures.append(
                f"the site carries a stale sample count {value}; the artifact says {samples:,}"
            )

    if failures:
        print(f"SITE ACCURACY: {len(failures)} mismatch(es)\n")
        for f in failures:
            print(f"  {f}")
        print("\nrun: python3 scripts/update_site_accuracy.py")
        return 1

    print(
        f"SITE ACCURACY: the site's table matches the artifact "
        f"({len(PUBLISHED)} measurements, {samples:,} samples)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
