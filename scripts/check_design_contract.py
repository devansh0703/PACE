#!/usr/bin/env python3
"""Check the sites against the Wire Terminal design contract.

The contract is written down in the plan and in the sites' own README. A
violation is a rule that has drifted from the design, and the cheapest way
to stop the drift is to fail the build when it happens.

Run: python3 scripts/check_design_contract.py
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Each rule is a pattern that must NOT appear in a stylesheet or a page.
# The reason is carried alongside so a failure explains itself.
BANNED: list[tuple[str, str, str]] = [
    (
        "gradient",
        r"gradient\(",
        "the contract bans gradients; elevation comes from borders and contrast",
    ),
    (
        "glassmorphism",
        r"backdrop-filter|backdrop-blur",
        "the contract bans glassmorphism",
    ),
    (
        "uppercase kicker",
        r"text-transform:\s*uppercase",
        "the contract bans an all-caps mono kicker above every heading",
    ),
    (
        "coloured left border",
        r"border-left:\s*\d+px",
        "the contract bans coloured left borders as an elevation cue",
    ),
    (
        "pulsing dot",
        r"@keyframes\s+(pulse|blink)",
        "the contract bans pulsing dots",
    ),
    (
        "soft shadow",
        r"box-shadow:\s*(?!none)[^;]*",
        "the contract bans soft shadows; elevation comes from borders",
    ),
    (
        "oversized radius",
        r"border-radius:\s*(?:[5-9]|[1-9][0-9]+)px",
        "the contract fixes the radius at 3px",
    ),
    (
        "emoji icon",
        r"[\U0001F300-\U0001FAFF]",
        "the contract uses Phosphor icons, not emoji",
    ),
    (
        "lucide",
        r"lucide",
        "the contract uses Phosphor icons, not Lucide",
    ),
    (
        "inter or geist font",
        r"['\"]?(Inter|Geist|Space Grotesk)['\"]?\s*,",
        "the contract uses IBM Plex Mono and IBM Plex Sans only",
    ),
]

# The colours the contract bans outright.
BANNED_COLOURS = ["#8b5cf6", "#14b8a6", "#3b82f6", "#6366f1", "#06b6d4", "#0ea5e9"]


def collect_files() -> list[pathlib.Path]:
    files: list[pathlib.Path] = []
    for site in sorted(ROOT.iterdir()):
        if not site.is_dir():
            continue
        for pattern in ("src/styles/*.css", "src/pages/*.astro"):
            files.extend(sorted(site.glob(pattern)))
    return files


def main() -> int:
    failures: list[str] = []
    checked = 0

    for path in collect_files():
        rel = path.relative_to(ROOT)
        text = path.read_text(encoding="utf-8")
        lines = text.split("\n")
        checked += 1

        for name, pattern, reason in BANNED:
            for i, line in enumerate(lines, 1):
                if re.search(pattern, line, re.IGNORECASE):
                    failures.append(
                        f"{rel}:{i}: the {name} rule was broken. {reason}.\n"
                        f"    {line.strip()[:100]}"
                    )

        for colour in BANNED_COLOURS:
            for i, line in enumerate(lines, 1):
                if colour.lower() in line.lower():
                    failures.append(
                        f"{rel}:{i}: the colour {colour} is banned by the contract.\n"
                        f"    {line.strip()[:100]}"
                    )

        # The contract requires the two IBM Plex faces and the Phosphor set.
        if path.suffix == ".astro":
            if "ibm-plex-mono" not in text:
                failures.append(f"{rel}: the page does not load IBM Plex Mono")
            if "ibm-plex-sans" not in text:
                failures.append(f"{rel}: the page does not load IBM Plex Sans")
            if "phosphor-icons" not in text:
                failures.append(f"{rel}: the page does not load the Phosphor icons")

    if failures:
        print(f"DESIGN CONTRACT: {len(failures)} violation(s) in {checked} file(s)\n")
        for f in failures:
            print(f"  {f}")
        return 1

    print(f"DESIGN CONTRACT: clean across {checked} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
