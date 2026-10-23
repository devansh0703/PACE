#!/usr/bin/env python3
"""Check the site against the Wire Terminal design contract.

The contract is written down in the plan and in the site's own README. A
violation is a rule that has drifted from the design, and the cheapest way to
stop the drift is to fail the build when it happens. This has caught real
drift twice, so it is a check with a record rather than a formality.

Run: python3 scripts/check_design_contract.py
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SITE = ROOT / "pace"

# Each rule is a pattern that must NOT appear in a stylesheet or a page. The
# reason is carried alongside so a failure explains itself.
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
        "uppercase",
        r"text-transform:\s*uppercase",
        "the contract bans an all-caps mono kicker above a heading",
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
    (
        "dark-by-default document",
        r"<html[^>]*data-theme=['\"]dark",
        "light is the default theme; dark is applied by the inline script after "
        "the reader's stored choice and the OS preference are read",
    ),
    (
        "hardcoded theme colour outside the palette",
        r"(?:fill|stroke|background|color):\s*#(?:[0-9a-f]{6})",
        "colours come from the custom properties in wire-terminal.css, so that "
        "both palettes stay in one place",
    ),
]

# The colours the contract bans outright.
BANNED_COLOURS = ["#8b5cf6", "#14b8a6", "#3b82f6", "#6366f1", "#06b6d4", "#0ea5e9"]

# The palette file is where hex values are allowed to live: it defines them.
PALETTE = SITE / "src" / "styles" / "wire-terminal.css"


def collect_files() -> list[pathlib.Path]:
    """Every page, component, layout and stylesheet, at any depth.

    Recursive on purpose: the site has pages in subdirectories and charts in a
    component subdirectory, and a non-recursive glob checked four files out of
    twenty. It reported clean while most of the site was unchecked.
    """
    files: list[pathlib.Path] = []
    for pattern in ("src/**/*.css", "src/**/*.astro"):
        files.extend(sorted(SITE.glob(pattern)))
    return [f for f in files if f.is_file()]


def main() -> int:
    failures: list[str] = []
    checked = 0
    layouts: list[pathlib.Path] = []

    files = collect_files()
    if not files:
        print(f"DESIGN CONTRACT: no files found under {SITE}/src; the paths moved")
        return 1

    for path in files:
        rel = path.relative_to(ROOT)
        text = path.read_text(encoding="utf-8")
        lines = text.split("\n")
        checked += 1

        for name, pattern, reason in BANNED:
            for i, line in enumerate(lines, 1):
                # The palette file is where a hex literal is the definition
                # rather than an escape from the tokens.
                if name == "hardcoded theme colour outside the palette" and path == PALETTE:
                    continue
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

        if "layouts" in path.parts:
            layouts.append(path)

    # The fonts and the icon set are loaded once, in the layout every page
    # uses, so the rule is checked where they are declared rather than repeated
    # on every page.
    if not layouts:
        failures.append("no layout under src/layouts; the fonts have nowhere to load from")
    for layout in layouts:
        text = layout.read_text(encoding="utf-8")
        rel = layout.relative_to(ROOT)
        if "ibm-plex-mono" not in text:
            failures.append(f"{rel}: the layout does not load IBM Plex Mono")
        if "ibm-plex-sans" not in text:
            failures.append(f"{rel}: the layout does not load IBM Plex Sans")
        if "phosphor-icons" not in text:
            failures.append(f"{rel}: the layout does not load the Phosphor icons")

    if failures:
        print(f"DESIGN CONTRACT: {len(failures)} violation(s) in {checked} file(s)\n")
        for f in failures:
            print(f"  {f}")
        return 1

    print(
        f"DESIGN CONTRACT: clean across {checked} file(s), "
        f"{len(layouts)} layout(s) load the two Plex faces and the Phosphor set"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
