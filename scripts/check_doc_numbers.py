#!/usr/bin/env python3
"""Check that the numbers published in the documents match the artifact.

Every measured number in the repository is produced by the verification
harness and lands in `pace-sdk/results/accuracy.json`. A number written by
hand in a document is a number that drifts, and a drifted number in a
document is a false claim that nobody notices.

This checks the two kinds of number that appear in prose:

  - the sample count of the recorded trace, and
  - the median agreement percentages.

A document may quote a *past* run's numbers as long as it says so, which
is why the historical documents are checked for a different rule: they
must carry a date.

Run: python3 scripts/check_doc_numbers.py
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# The artifact's location, so the check can run from a CI layout where
# the workspace and the SDK are checked out into different directories.
ARTIFACT = pathlib.Path(
    os.environ.get("PACE_ARTIFACT", ROOT / "pace-sdk" / "results" / "accuracy.json")
)

# The documents' root, for the same reason.
DOC_ROOT = pathlib.Path(os.environ.get("PACE_DOC_ROOT", ROOT))

# The documents that publish the current numbers, and the rule each one
# must satisfy.
CURRENT_DOCS = [
    "README.md",
    "PLAN.md",
    "pace-sdk/README.md",
    "pace-sdk/docs/ACCURACY.md",
    "pace-sdk/docs/PREMISE-VERIFICATION.md",
    "pace-sdk/docs/INTEGRATION.md",
    "docs/OS-PRIMITIVES.md",
    "docs/DEPENDENCY-ADOPTION.md",
]

# The documents that describe a past state, and therefore must be dated.
HISTORICAL_DOCS = [
    "docs/CEO-REVIEW.md",
    "docs/ENG-REVIEW.md",
    "docs/DX-REVIEW.md",
    "docs/DESIGN-REVIEW.md",
    "docs/COMPETITIVE-AND-PRIOR-ART.md",
]


def current_numbers(artifact: dict) -> dict:
    """The published figures, taken from the artifact.

    Every measurement the harness produces is published somewhere, so all
    of them are collected here rather than a hand-picked two. A
    measurement that is not published cannot go stale, and one that is
    published must match.
    """
    by_name = {m["name"]: m for m in artifact["measurements"]}

    def measurement(prefix: str) -> dict:
        """The measurement whose name starts with the given one.

        A measurement's name carries a suffix that says how it is
        reported -- "(reported, not gated)" -- and an exact lookup
        missed every suffixed name. The check then agreed with a
        document's stale figure, because it had the same fault.
        """
        if prefix in by_name:
            return by_name[prefix]
        for name, value in by_name.items():
            if name.startswith(prefix):
                return value
        raise KeyError(prefix)

    published = []
    for m in artifact["measurements"]:
        if m.get("skipped"):
            continue
        # Both the median and the p95 are published, in different tables.
        published.append(m["median_relative_error"] * 100.0)
        published.append(m["p95_relative_error"] * 100.0)

    # The same figures, but indexed by the source they were measured
    # against, so a line's figure is checked against the source that
    # line names.
    #
    # Without this, a line's figure was accepted if it matched ANY
    # published figure, and the decomposition's median is 0.0 -- the
    # invariant is exact -- so a stale "0.0000 %" for a different
    # measurement matched it and the check passed.
    by_source: dict[str, list[float]] = {}
    for m in artifact["measurements"]:
        if m.get("skipped"):
            continue
        by_source[m["name"]] = [
            m["median_relative_error"] * 100.0,
            m["p95_relative_error"] * 100.0,
        ]
    thresholds = sorted(
        {m["threshold"] * 100.0 for m in artifact["measurements"] if m.get("threshold")}
    )
    return {
        "samples": artifact["decomposition"]["total_samples"],
        "cfs": measurement(
            "on-CPU time vs /proc/<tid>/schedstat (kernel cfs accounting)"
        )["median_relative_error"]
        * 100.0,
        "perf": measurement(
            "on-CPU time vs perf sched timehist 'run time' column"
        )["median_relative_error"]
        * 100.0,
        "published": published,
        "by_source": by_source,
        "thresholds": thresholds,
    }


# The phrase that identifies a measurement's source, in a line.
#
# The documents write the source in a shorter form than the harness
# names it: "perf sched timehist, \"sch delay\" column" against the
# harness's "preempted time vs perf sched timehist 'sch delay' column".
# So the source is identified by the words that are common to both.
SOURCE_PHRASES = [
    ("cfs accounting", "on-CPU time vs /proc/<tid>/schedstat (kernel cfs accounting)"),
    ("utime+stime", "on-CPU time vs /proc/<tid>/stat utime+stime (tick granularity)"),
    ("run-queue delay", "preempted time vs /proc/<tid>/schedstat (kernel run-queue delay)"),
    ("sch delay", "preempted time vs perf sched timehist 'sch delay' column"),
    ("wait time", "off-CPU time (preempted + blocked) vs perf 'wait time' column"),
    ("run time", "on-CPU time vs perf sched timehist 'run time' column"),
    ("CLOCK_MONOTONIC", "wall time vs CLOCK_MONOTONIC read by the verifier"),
    ("wall minus on-CPU", "blocked time vs wall minus on-CPU (sleeping window)"),
    ("decomposition", "decomposition balance on a recorded trace"),
    ("the tier's own cumulative", "the trace's preempted total vs the tier's own cumulative"),
]


def _normalise(text: str) -> str:
    """Lowercase, with the hyphens turned into spaces.

    The documents write "run time" and "run-time" interchangeably, and
    a matcher that sees them as different sources misses half the
    figures.
    """
    return text.lower().replace("-", " ").replace("_", " ")


def _sources_in_order(line: str, by_source: dict[str, list[float]]) -> list:
    """The sources the line names, in the order they appear.

    Each is the position in the line and the figures published for it.
    """
    lowered = _normalise(line)
    found = []
    for phrase, name in SOURCE_PHRASES:
        index = lowered.find(_normalise(phrase))
        if index < 0:
            continue
        for measurement_name, figures in by_source.items():
            if measurement_name.startswith(name):
                found.append((index, figures))
                break
    found.sort(key=lambda item: item[0])
    return found


def main() -> int:
    if not ARTIFACT.exists():
        print(f"no artifact at {ARTIFACT}; the harness has not been run")
        return 1
    artifact = json.loads(ARTIFACT.read_text())
    now = current_numbers(artifact)
    samples_text = f"{now['samples']:,}"

    failures: list[str] = []

    # 1. Every current document must quote the current sample count if it
    #    quotes a sample count at all, and must not quote a stale one.
    for rel in CURRENT_DOCS:
        path = DOC_ROOT / rel
        if not path.exists():
            failures.append(f"{rel}: the document does not exist")
            continue
        text = path.read_text()

        # A sample count is a comma-formatted number in a phrase that
        # mentions samples or the trace's size. A nanosecond duration and a
        # cycle count are also comma-formatted, so the surrounding words
        # are what distinguishes them.
        # The rule is for the trace's sample count, so the phrase must tie
        # the number to the trace or the invariant. "20,000 samples per
        # measurement" is the benchmark's own count and is not affected.
        # A line that talks about samples is a candidate, and the number on
        # it is the sample count. Phrasing varies, so the rule looks at
        # the line rather than at a fixed phrase.
        for i, line in enumerate(text.split("\n"), 1):
            if "sample" not in line.lower():
                continue
            for m in re.finditer(r"\b(\d{1,3}(?:,\d{3})+)\b", line):
                value = m.group(1)
                # The analyzer's committed trace has its own, stable count.
                if value == "106,023":
                    continue
                if value == samples_text:
                    continue
                # A count that is not about the trace's size: a duration,
                # a cycle count, a benchmark's own sample count.
                if "cycles" in line.lower() and "ns" not in line.lower():
                    continue
                if re.search(r"per measurement|per call|per repetition", line, re.I):
                    continue
                if int(value.replace(",", "")) < 50000:
                    continue
                failures.append(
                    f"{rel}:{i}: quotes {value} samples, but the artifact "
                    f"says {samples_text}"
                )

        # The agreement percentages, where they are quoted with their value.
        #
        # Only the published figures are checked. Other percentages in the
        # documents are thresholds, ratios, turbo figures and costs, and a
        # blanket rule would flag them all. So the rule is per-figure: a
        # percentage that is a *measurement* must match the artifact, and
        # the way to tell is the line it is on.
        for i, line in enumerate(text.split("\n"), 1):
            # A harness figure is one compared against a source the harness
            # names. A percentage measured elsewhere -- the TSC against
            # hw_ref_cycles, for instance -- is checked by the primitive
            # tests, not here, and the rule must not claim it.
            if not re.search(
                r"cfs accounting|schedstat|perf sched|timehist|"
                r"utime\+stime|CLOCK_MONOTONIC|decomposition",
                line,
                re.I,
            ):
                continue
            # Each figure is attributed to the source that PRECEDES it,
            # because a sentence can name two sources and carry a figure
            # for each:
            #
            #   "agrees with the kernel's own cfs accounting to 0.0006 %
            #    and with perf's run-time column to 0.0574 %"
            #
            # A line with one source behaves the same way.
            #
            # The sources are located in the line, and each figure takes
            # the nearest source before it.
            sources = _sources_in_order(line, now["by_source"])
            for m in re.finditer(r"(\d+\.\d+)\s*%", line):
                value = float(m.group(1))
                preceding = [s for s in sources if s[0] < m.start()]
                if preceding:
                    allowed = preceding[-1][1]
                elif sources:
                    allowed = sources[0][1]
                else:
                    # The line does not name its source, so the figure
                    # cannot be attributed to one. The "any published
                    # figure" rule is better than nothing.
                    allowed = now["published"]
                if any(abs(value - figure) < 0.00005 for figure in allowed):
                    continue
                # A threshold is not a measurement. The harness's own
                # thresholds are published in the artifact.
                if value in now["thresholds"]:
                    continue
                failures.append(
                    f"{rel}:{i}: quotes {value:.4f} % as an agreement figure, "
                    f"but the artifact says "
                    + ", ".join(f"{p:.4f} %" for p in allowed)
                )

    # 2. Every historical document must be dated, so a reader can tell that
    #    it describes a past state.
    for rel in HISTORICAL_DOCS:
        path = DOC_ROOT / rel
        if not path.exists():
            failures.append(f"{rel}: the document does not exist")
            continue
        text = path.read_text()
        if not re.search(r"20\d{2}-\d{2}-\d{2}", text):
            failures.append(
                f"{rel}: describes a past state but carries no date, so a "
                f"reader cannot tell it is historical"
            )

    if failures:
        print(f"DOC NUMBERS: {len(failures)} problem(s)\n")
        for f in failures:
            print(f"  {f}")
        print(
            "\nthe measured numbers come from pace-sdk/results/accuracy.json and "
            "pace-sdk/results/overhead.json; regenerate the site with"
        )
        print("  cd websites/pace && npm run build")
        print(
            "the site renders its figures from those artifacts, so a document "
            "that disagrees is the document that is wrong"
        )
        return 1

    print(
        f"DOC NUMBERS: the current documents quote {samples_text} samples and "
        f"the historical ones are dated"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
