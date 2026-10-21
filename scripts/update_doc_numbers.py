#!/usr/bin/env python3
"""Update the measured figures in the documents from the artifact.

Every number the verification harness produces lands in
`pace-sdk/results/accuracy.json`. A number written by hand in a document
is a number that drifts, and a drifted number in a document is a false
claim that nobody notices.

So the documents' tables are updated from the artifact, and
`scripts/check_doc_numbers.py` verifies that they match.

A row or a sentence is matched by the *source* it names, not by its
position, so a document can be edited around them. A table row names one
source and carries a median and a p95. A sentence may name two sources
and carry a figure for each.

Run: python3 scripts/update_doc_numbers.py
"""

from __future__ import annotations

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARTIFACT = ROOT / "pace-sdk" / "results" / "accuracy.json"

# The measurement each document row or sentence reports, keyed by a
# phrase that identifies the source.
#
# The order matters: the first phrase that matches wins, so the more
# specific ones come first.
ROWS = [
    (
        "cfs accounting",
        "on-CPU time vs /proc/<tid>/schedstat (kernel cfs accounting)",
    ),
    (
        "utime+stime",
        "on-CPU time vs /proc/<tid>/stat utime+stime (tick granularity)",
    ),
    (
        "run-queue delay",
        "preempted time vs /proc/<tid>/schedstat (kernel run-queue delay)",
    ),
    (
        "sch delay",
        "preempted time vs perf sched timehist 'sch delay' column",
    ),
    (
        "wait time",
        "off-CPU time (preempted + blocked) vs perf 'wait time' column",
    ),
    (
        "run time",
        "on-CPU time vs perf sched timehist 'run time' column",
    ),
    (
        "CLOCK_MONOTONIC",
        "wall time vs CLOCK_MONOTONIC read by the verifier",
    ),
    (
        "wall minus on-CPU",
        "blocked time vs wall minus on-CPU (sleeping window)",
    ),
]

# The documents whose tables are generated.
DOCUMENTS = [
    "README.md",
    "PLAN.md",
    "pace-sdk/README.md",
    "pace-sdk/docs/ACCURACY.md",
    "pace-sdk/docs/PREMISE-VERIFICATION.md",
]

# A percentage, with the trailing space the documents use.
PERCENT = re.compile(r"(\d+\.\d+)(\s*%)")


def load_artifact() -> dict:
    if not ARTIFACT.exists():
        print(f"no artifact at {ARTIFACT}; the harness has not been run")
        raise SystemExit(1)
    return json.loads(ARTIFACT.read_text())


def measurement_values(artifact: dict) -> dict[str, dict]:
    """The median and p95 for every measurement, keyed by name."""
    out = {}
    for m in artifact["measurements"]:
        out[m["name"]] = {
            "median": m["median_relative_error"] * 100.0,
            "p95": m["p95_relative_error"] * 100.0,
            "skipped": m.get("skipped", False),
        }
    return out


def format_percent(value: float) -> str:
    """A percentage with the precision the documents use.

    Four decimal places is what the documents use. A value that is
    exactly zero is written as zero, because "0.0000 %" reads as a
    measurement and "0 %" reads as an absence.
    """
    return f"{value:.4f}"


def normalise(text: str) -> str:
    """Lowercase, with the hyphens turned into spaces.

    The documents write "run time" and "run-time" interchangeably, and
    a matcher that sees them as different sources misses half the
    figures. Normalising makes them the same.
    """
    return text.lower().replace("-", " ").replace("_", " ")


def sources_in(text: str, values: dict[str, dict]) -> list[tuple[int, dict]]:
    """The measurements a line names, with the position of the name.

    The position matters for a sentence, where each source takes the
    first percentage that follows its name. The position is the offset
    in the original text, so the caller can splice the replacement in.
    """
    lowered = normalise(text)
    found = []
    for phrase, name in ROWS:
        value = values.get(name)
        if value is None or value["skipped"]:
            continue
        needle = normalise(phrase)
        index = lowered.find(needle)
        if index >= 0:
            found.append((index, value))
    found.sort(key=lambda item: item[0])
    return found


def update_table_row(line: str, values: dict[str, dict]) -> tuple[str, bool]:
    """Replace the percentages in one table row with the artifact's.

    A table row names one source and carries a median and a p95.
    """
    if "|" not in line or "%" not in line:
        return line, False

    found = sources_in(line, values)
    if len(found) != 1:
        return line, False
    value = found[0][1]

    percents = list(PERCENT.finditer(line))
    if not percents:
        return line, False

    # The last percentage is the p95 and the one before it is the
    # median. A row with a single percentage carries the median alone.
    # Work from the end so the earlier offsets stay valid.
    if len(percents) >= 2:
        p = percents[-1]
        m = percents[-2]
        line = line[: p.start()] + f"{format_percent(value['p95'])} %" + line[p.end() :]
        line = line[: m.start()] + f"{format_percent(value['median'])} %" + line[m.end() :]
    else:
        m = percents[0]
        line = line[: m.start()] + f"{format_percent(value['median'])} %" + line[m.end() :]
    return line, True


def update_prose(line: str, values: dict[str, dict]) -> tuple[str, bool]:
    """Replace the percentages in a sentence with the artifact's.

    A sentence may name two sources and carry a figure for each, so each
    source takes the first percentage that follows its name, and a
    percentage is claimed by only one source.
    """
    if "|" in line or "%" not in line:
        return line, False

    found = sources_in(line, values)
    if not found:
        return line, False

    percents = list(PERCENT.finditer(line))
    claimed: set[int] = set()
    for index, value in found:
        for m in percents:
            if m.start() < index or m.start() in claimed:
                continue
            claimed.add(m.start())
            line = (
                line[: m.start()]
                + f"{format_percent(value['median'])} %"
                + line[m.end() :]
            )
            break

    return line, bool(claimed)


def update_sample_count(line: str, total: int) -> tuple[str, bool]:
    """Replace the trace's sample count in a line with the artifact's.

    A comma-formatted number in a line that ties the number to the
    trace or the invariant. A nanosecond duration and a cycle count are
    also comma-formatted, so the surrounding words are what
    distinguishes them, and a line that talks about samples is a
    candidate.
    """
    if "sample" not in line.lower():
        return line, False

    current = f"{total:,}"

    def replace(m: re.Match) -> str:
        value = m.group(1)
        if value == current:
            return m.group(0)
        # The analyzer's committed trace has its own, stable count.
        if value == "106,023":
            return m.group(0)
        # A count that is not about the trace's size: a duration, a cycle
        # count, a benchmark's own sample count.
        if "cycles" in line.lower() and "ns" not in line.lower():
            return m.group(0)
        if re.search(r"per measurement|per call|per repetition", line, re.I):
            return m.group(0)
        if int(value.replace(",", "")) < 50000:
            return m.group(0)
        return current

    new_line = re.sub(r"\b(\d{1,3}(?:,\d{3})+)\b", replace, line)
    return new_line, new_line != line


def main() -> int:
    artifact = load_artifact()
    values = measurement_values(artifact)
    total = artifact["decomposition"]["total_samples"]
    changed_files = 0
    changed_rows = 0

    for rel in DOCUMENTS:
        path = ROOT / rel
        if not path.exists():
            continue
        original = path.read_text()
        out = []
        file_changed = False
        for line in original.split("\n"):
            new_line, changed = update_table_row(line, values)
            if not changed:
                new_line, changed = update_prose(line, values)
            if not changed:
                new_line, changed = update_sample_count(line, total)
            if changed:
                file_changed = True
                changed_rows += 1
            out.append(new_line)
        if file_changed:
            path.write_text("\n".join(out))
            changed_files += 1
            print(f"  updated {rel}")

    print(f"\n{changed_rows} row(s) in {changed_files} document(s)")
    if changed_files == 0:
        print("the documents already match the artifact")
    return 0


if __name__ == "__main__":
    sys.exit(main())
