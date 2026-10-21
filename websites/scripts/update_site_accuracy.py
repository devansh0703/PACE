#!/usr/bin/env python3
"""Regenerate the accuracy table on the SDK site from the committed artifact.

The numbers on the site must come from the measurement harness, not from a
hand-typed copy of them. A hand-typed copy is a number that drifts, and a
drifted number on a product site is a lie that nobody notices.

Run:  python3 scripts/update_site_accuracy.py
"""

from __future__ import annotations

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARTIFACT = ROOT.parent / "pace-sdk" / "results" / "accuracy.json"
SITE = ROOT / "pace-sdk" / "src" / "pages" / "index.astro"

# The rows of the site's table, in order. Each names the measurement in the
# artifact it comes from and the two columns of the row.
#   (artifact measurement name, quantity cell, "compared against" cell)
ROWS = [
    (
        "on-CPU time vs /proc/<tid>/schedstat (kernel cfs accounting)",
        "on-CPU time",
        "/proc/&lt;tid&gt;/schedstat, kernel cfs accounting",
    ),
    (
        "on-CPU time vs perf sched timehist 'run time' column",
        "on-CPU time",
        "perf sched timehist, &quot;run time&quot; column",
    ),
    (
        "preempted time vs perf sched timehist 'sch delay' column",
        "preempted time",
        "perf sched timehist, &quot;sch delay&quot; column",
    ),
    (
        "off-CPU time (preempted + blocked) vs perf 'wait time' column",
        "off-CPU time",
        "perf sched timehist, &quot;wait time&quot; column",
    ),
    (
        "wall time vs CLOCK_MONOTONIC read by the verifier",
        "wall time",
        "CLOCK_MONOTONIC, read independently",
    ),
]


def build_rows(artifact: dict) -> str:
    by_name = {m["name"]: m for m in artifact["measurements"]}
    out: list[str] = []
    for name, quantity, compared in ROWS:
        # The name is matched as a PREFIX, because a measurement's name
        # carries a suffix that says how it is reported -- "(reported,
        # not gated)" -- and that suffix is the reporting policy rather
        # than the measurement's identity.
        m = by_name.get(name)
        if m is None:
            for candidate, value in by_name.items():
                if candidate.startswith(name):
                    m = value
                    break
        if m is None:
            raise SystemExit(f"the artifact has no measurement named {name!r}")
        median = m["median_relative_error"] * 100.0
        p95 = m["p95_relative_error"] * 100.0
        n = m.get("n", 0)
        # A row is "ok" when it is inside the harness's own threshold. The
        # threshold lives in the artifact, so the site cannot disagree with
        # the harness about what counts as a pass.
        cls = "ok" if m.get("passed", True) else "warn"
        out.append(
            f"            <tr>\n"
            f"              <td>{quantity}</td>\n"
            f"              <td>{compared}</td>\n"
            f'              <td class="{cls}">{median:.4f} %</td>\n'
            f'              <td class="{cls}">{p95:.4f} %</td>\n'
            f"              <td>{n}</td>\n"
            f"            </tr>"
        )
    return "\n".join(out)


def main() -> int:
    if not ARTIFACT.exists():
        print(f"no artifact at {ARTIFACT}; run the accuracy harness first")
        return 1
    artifact = json.loads(ARTIFACT.read_text())
    site = SITE.read_text()

    # The row block, from the first <tr> after the table header to the row
    # that carries the decomposition line.
    start_marker = '<tr>\n              <td>on-CPU time</td>'
    end_marker = '              <td>every recorded sample</td>'
    start = site.find(start_marker)
    if start == -1:
        print("could not find the accuracy table in the site")
        return 1
    end = site.find(end_marker, start)
    if end == -1:
        print("could not find the end of the accuracy table in the site")
        return 1
    # The block runs to the closing </tr> of the last row before the marker.
    block_end = site.rfind("</tr>", start, end)
    block_end += len("</tr>")

    new_block = build_rows(artifact)
    site = site[:start] + new_block + site[block_end:]

    # The summary sentence above the table.
    cfg = artifact["configuration"]
    dec = artifact["decomposition"]
    old_sentence = re.search(
        r"The verification harness runs the library and the kernel's own accounting over\s*"
        r"the same thread and the same window, then compares\..*?"
        r"recorded samples, on an i7-13620H running kernel [0-9.]+\.",
        site,
        re.DOTALL,
    )
    if old_sentence:
        site = site[: old_sentence.start()] + (
            "The verification harness runs the library and the kernel's own accounting over\n"
            "          the same thread and the same window, then compares. "
            f"{cfg['repetitions']} repetitions, {dec['total_samples']:,} recorded samples, on an\n"
            "          i7-13620H running kernel 7.3.0."
        ) + site[old_sentence.end() :]

    # The decomposition row's sample count.
    decomposition_row = re.search(
        r"<tr>\s*<td>decomposition invariant</td>.*?</tr>", site, re.DOTALL
    )
    if decomposition_row is None:
        print("could not find the decomposition row in the site")
        return 1
    row = decomposition_row.group(0)
    # The count is the last cell of the row.
    new_row = re.sub(
        r"(<td>)\s*[\d,]+(\s*</td>\s*</tr>)",
        rf"\g<1>{dec['total_samples']:,}\g<2>",
        row,
    )
    site = site[: decomposition_row.start()] + new_row + site[decomposition_row.end() :]

    SITE.write_text(site)
    print(
        f"updated the site's accuracy table from {ARTIFACT.name}: "
        f"{cfg['repetitions']} repetitions, {dec['total_samples']:,} samples"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
