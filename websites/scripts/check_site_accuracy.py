#!/usr/bin/env python3
"""Check that the built site carries the artifact's numbers, and only them.

The pages render their figures from `pace-sdk/results/accuracy.json` at build
time, so a number cannot be typed by hand and drift. What this checks is the
other half of that: that the build actually rendered the numbers it should
have, on the page that is supposed to carry them.

It runs against `dist/`, not against the source page, because the source no
longer contains the figures -- the render does. A page whose data module
silently failed would have the right source and the wrong output.

Run: python3 scripts/check_site_accuracy.py   (after `npm run build` in pace/)
"""

from __future__ import annotations

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPO = ROOT.parent
ARTIFACT = REPO / "pace-sdk" / "results" / "accuracy.json"
BREADTH = REPO / "pace-sdk" / "results" / "overhead.json"

# The page that publishes the accuracy table, as the built output.
PAGE = ROOT / "pace" / "dist" / "research" / "accuracy" / "index.html"

# The measurements the page publishes. A check the page does not publish is
# not checked here.
PUBLISHED = [
    "on-CPU time vs /proc/<tid>/schedstat (kernel cfs accounting)",
    "on-CPU time vs perf sched timehist 'run time' column",
    "preempted time vs perf sched timehist 'sch delay' column (reported, not gated)",
    "off-CPU time (preempted + blocked) vs perf 'wait time' column",
    "wall time vs CLOCK_MONOTONIC read by the verifier",
]


def main() -> int:
    if not ARTIFACT.exists():
        print(f"no artifact at {ARTIFACT}; the accuracy harness has not been run")
        return 1
    if not PAGE.exists():
        print(f"no built page at {PAGE}; run `npm run build` in websites/pace first")
        return 1

    artifact = json.loads(ARTIFACT.read_text())
    page = PAGE.read_text()
    by_name = {m["name"]: m for m in artifact["measurements"]}

    failures: list[str] = []

    for name in PUBLISHED:
        m = by_name.get(name)
        if m is None:
            failures.append(f"the artifact has no measurement named {name!r}")
            continue
        for field in ("median_relative_error", "p95_relative_error"):
            value = f"{m[field] * 100.0:.4f} %"
            if value not in page:
                failures.append(f"the page does not carry {value} for {name!r} ({field})")

    samples = artifact["decomposition"]["total_samples"]
    if f"{samples:,}" not in page:
        failures.append(f"the page does not carry the sample count {samples:,}")

    # No other comma-grouped count may appear in a table cell. A hand-typed
    # sample count that went stale is the failure this catches.
    for value in re.findall(r"<td[^>]*>(\d{3,}(?:,\d{3})+)</td>", page):
        if value != f"{samples:,}":
            failures.append(f"the page carries a stale count {value}; the artifact says {samples:,}")

    # The site must not print a cost it did not measure: if the cost artifact
    # is absent, no page may show a nanosecond figure from it.
    home = (ROOT / "pace" / "dist" / "index.html")
    cost_artifact = BREADTH.exists()
    for built in sorted((ROOT / "pace" / "dist").rglob("*.html")):
        text = built.read_text()
        if not cost_artifact and "results/overhead.json" in text:
            failures.append(
                f"{built.relative_to(REPO)} cites the cost artifact, which does not exist"
            )
    if home.exists() and cost_artifact:
        overhead = json.loads(BREADTH.read_text())
        median = f"{overhead['example']['median_ns']} ns"
        for built in (ROOT / "pace" / "dist").rglob("*.html"):
            if "results/overhead.json" in built.read_text() and median not in built.read_text():
                failures.append(
                    f"{built.relative_to(REPO)} cites the cost artifact but does not "
                    f"carry its median {median}"
                )

    if failures:
        print(f"SITE ACCURACY: {len(failures)} mismatch(es)\n")
        for f in failures:
            print(f"  {f}")
        print("\nthe figures come from the artifacts; rebuild websites/pace and re-run")
        return 1

    print(
        f"SITE ACCURACY: the built page matches the artifact "
        f"({len(PUBLISHED)} measurements, {samples:,} samples)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
