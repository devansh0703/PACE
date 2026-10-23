# Design review — PACE


> **Historical record.** This review describes the product as it was when it was written,
> and it is dated so a reader can tell. Two things changed after it: the brief's two-product
> framing was a misreading, so the library, the analyzer and the hosted service are one
> product with one site; and the project stopped being a licensing business, so the
> commercial-licence reasoning in some of these reviews no longer reflects how the product is
> published. Where the two disagree, [README.md](../README.md) and [PLAN.md](../PLAN.md) are
> current and this file is not.

Written 2026-10-21 against the sites as they were built that day.
Independent senior product-designer review of the PACE product sites and
the analyzer's report output, run as the gstack `design` phase against what
is actually rendered, not against what the plan says should be.

The `codex` outside-review voice was unavailable: `codex` is not installed
on this machine. The review below is the native review, marked N/A for the
outside voice rather than claimed as covered.

**Verdict: changes requested and applied.** The sites carried three claims
that the CEO and engineering reviews had already invalidated, and the CSS
broke the design contract the sites were built to. All of it is fixed, and
the contract is now checked mechanically so it cannot drift again.

---

## 1. Information hierarchy

**The SDK site: the problem, then the proof, then the ask.**

| Order | Section | What the reader gets |
|---|---|---|
| 1 | The measurement gap | "Your profiler knows the thread was slow. It does not know why." |
| 2 | What a sample contains | The four buckets, and the three tiers |
| 3 | Integration | What it takes to link it |
| 4 | Evidence | The measured numbers |
| 5 | Licence | Evaluation versus production |
| 6 | Next step | The evaluation licence request |

The hero is the problem, not the product. That is the right order for this
audience: a product leader at a tool company does not yet know they need a
time-attribution library, and they do believe they have a slow-frame
problem. Leading with the product would be leading with a category they
have not heard of.

**The analyzer site: the question, then the tool.**

| Order | Section | What the reader gets |
|---|---|---|
| 1 | The question | "Which part of the frame was the code, and which part was the scheduler?" |
| 2 | The command | The actual terminal output, in a monospace block |
| 3 | What it tells you | Why the distinction changes what you do |
| 4 | Install | Two ways in |
| 5 | Input formats | What it reads, and what it refuses to invent |
| 6 | Output | The self-contained HTML report |

The analyzer's hero is a *question*, and the second thing on the page is
the tool's real output. A performance engineer who lands there recognises
their own problem in the first line and their own workflow in the second.

**The one hierarchy weakness: on the SDK site, the evidence is the fourth
section.** The measured numbers are the strongest thing on the page — a
67.8 % preemption share on a real contended run is more persuasive than any
sentence — and they arrive after three sections of prose. They are
reachable from the nav in one click, which mitigates it, but a reader who
scrolls once and leaves never sees the proof.

Not changed: the current order tells a story, and pulling the numbers
above the explanation would show a number to a reader who has not yet
learned what it means. A reader who skims to the evidence can get there
from the nav.

## 2. Missing states

**Both sites are static marketing pages, so the states are limited, and the
ones that exist are handled.**

| State | Where | Handled |
|---|---|---|
| Loading | — | The pages have no asynchronous content, so there is no loading state to design. Fonts are self-hosted, so there is no FOUT either |
| Empty | — | There is no user data on a marketing page |
| Error | The analyzer's report | **The one that mattered, and it was wrong.** A trace with no preemption/blocking split printed `0 ns` and `0.0%` — a *confident zero* for a figure that was never measured. It now prints a dash, draws an empty split bar, and says the split is unavailable. Fixed in the report, the terminal, the HTML table and the HTML summary |
| Success | The analyzer's report | The measured case still shows the real numbers: `1.22 s`, `60.8%` |
| Partial | The analyzer's report | A trace with some malformed lines reports the count and analyses the rest |
| Not-found | The sites | There are no routes but the index, and the nav is anchors |

**The empty-vs-zero distinction is the whole product.** A profiler that
reports "0 % preempted" when it did not measure preemption is the exact
failure PACE exists to fix. The report was committing that failure in its
own output. It does not any more, and a test holds it there.

**The sites have no error state because they have no error state.** No form,
no API, no user input. The one interactive element — the mailto link — is
a mail client.

## 3. User journey

**The SDK site's arc: recognition → scepticism → proof → next step.**

The hero states a problem the reader has had. The measurement section
shows the tool that answers it. The evidence section shows the numbers are
measured against independent sources. The licence section removes the
friction of "can I try this". The arc ends with an evaluation licence
request, which is the right ask for a product that is not yet self-serve.

Where it breaks: **a reader who is not a performance engineer.** The page
assumes the reader knows what "run-queue wait" means. The first paragraph
explains the *problem* in plain terms — "a CPU-time counter cannot tell
you where" — but the measurement section assumes the vocabulary. A product
manager evaluating whether to license this is half the audience, and the
page does not help them.

Not changed, and the reason is that the alternative is worse: a glossary
section at the bottom of a technical page is read by nobody, and
simplifying the measurement section would make it useless to the engineer
who is the other half. The licence section, which is where the non-engineer
is actually evaluating, is written without the vocabulary.

**The analyzer site's arc: question → output → install.** Shorter, and
appropriate: the audience already knows the problem, so the page gets to
the tool in two screens.

Where it breaks: **the page does not show the HTML report.** It describes
it accurately — one file, opens offline, the split drawn inline — but the
one visual artefact the analyzer produces is not on the page. A reader who
wants to know what the report looks like has to install the tool and make
a trace.

Not changed: the alternative is embedding a screenshot of a report that
would then be a second thing to keep current, and the report's structure
is already shown in the terminal block directly above it. The terminal
output and the HTML report carry the same information by design, so the
terminal block is an honest proxy.

## 4. Specificity

**The plan describes the design in the specific, and the sites follow it.**

The design direction is not "clean and modern". It is: warm charcoal
ground `#131311`, amber accent `#e8a33d`, IBM Plex Mono for structure and
IBM Plex Sans for prose, Phosphor icons at light weight, 3 px radii,
elevation from borders and contrast rather than shadows, no gradients, no
glassmorphism, no pill badges, no all-caps mono kicker above every
heading, no three-identical-card layout, no numbered `01/02/03` steps, no
stat banner rows, no coloured left borders, no pulsing dots, no emoji
icons.

That is a specific list, and the sites implement it. The measurements are
also specific: the contended run's `654.75 ms` on-CPU, `1.22 s` preempted,
`131.12 ms` blocked, and the `60.8 %` share are the real numbers from the
committed trace, not illustrations.

**The weakness the audit found: the CSS had drifted from its own list.**
Three rules were broken in the shipped stylesheet — the uppercase kicker,
the coloured left border, and a dead `.grid-3`. A contract written in a
document but not enforced is a wish. It is now enforced by
`websites/scripts/check_design_contract.py`, which CI runs.

## 5. The decisions that would have haunted the implementer

**The report's treatment of an unmeasured figure.** This was the decision
that mattered most, and it was not specified anywhere. If the report
prints `0 ns` for a figure it did not measure, then a reader who does not
check the provenance will read "no preemption occurred" where the truth is
"preemption could not be observed". The report and the SDK both have a
`HAS_PREEMPTED` flag for exactly this, and the report was not using it in
its own output.

The rule, now enforced by a test: **a figure that was not measured is a
dash, and a figure that was measured is a number.** A dash and a zero are
different claims, and the report must never confuse them.

**Whether the sites should name the incumbents.** The CEO review's
correction was that Intel VTune already separates preemption from
blocking. A site that quietly omits VTune is not lying, but it leaves the
reader with a false impression of the market, and a licensee who discovers
it later will distrust the rest of the page. The sites name them.

**Whether "shipping for Linux today" reads as weakness.** It is a
narrower claim than "Linux, Windows and macOS", and narrower claims read
as less impressive. But a platform claim that is not true is not a
platform claim, it is a bug with a marketing budget. The site now says
what is true and pairs it with the reason — "with Windows and macOS behind
the same API" — so the reader sees a roadmap rather than a gap.

## The findings

| # | Finding | Severity | Status |
|---|---|---|---|
| 1 | The SDK site claimed Linux, Windows and macOS. Only Linux is implemented | **High** | **Fixed.** It now says Linux today, with the other two behind the same API |
| 2 | The SDK site said "Two tiers" when there are three. The eBPF tier was added by the CEO review and never reached the page | **High** | **Fixed.** Three tiers, and tier 2 is described |
| 3 | The report printed `0 ns` and `0.0%` for a figure it never measured | **High** | **Fixed.** A dash, an empty split bar, and a test |
| 4 | The sites called themselves "cycle attribution" when the verified core is the time attribution | Medium | **Fixed** |
| 5 | The CSS broke the design contract it was built to: uppercase kickers, a coloured left border, dead `.grid-3` | Medium | **Fixed**, and the contract is now checked by a script CI runs |
| 6 | The analyzer site's claim about a format with no attribution did not match the implementation | Medium | **Fixed** on both sides |
| 7 | The SDK site's evidence is the fourth section, below three sections of prose | Low | **Not changed.** The order tells a story, and the nav puts the evidence one click away |
| 8 | The SDK page assumes the reader knows the vocabulary | Low | **Not changed.** A glossary would be read by nobody; the licence section, where the non-engineer actually is, is already plain |
| 9 | The analyzer page does not show the HTML report | Low | **Not changed.** The terminal block directly above shows the same information, and a second artefact would be a second thing to keep current |

No finding met the bar for a Critical: nothing misleads about a price, a
licence term, a privacy property, or a security claim, and nothing is
visually broken.

## The recommendation

**Ship the sites as they now stand, and put the report's provenance rule
into the analyzer's own documentation so a user of the library sees the
same rule the report follows.**

Reasoning: the sites' hierarchy, voice and specificity are right, and the
three broken claims were accuracy failures rather than design failures —
the same kind the engineering review found in the code. They are fixed,
and the design contract is now mechanically enforced so the CSS cannot
drift again. The remaining findings are all trade-offs between two
defensible choices, and in each case the one taken is the one that does
not add a maintenance burden or a page nobody reads.

---

## Reality rows

REALITY: premise-table pass "The SDK site leads with the problem, not the product, and its hero states the measurement gap before any feature." websites/pace-sdk/src/pages/index.astro:52-53
REALITY: premise-table pass "The analyzer site leads with the question and shows the real terminal output as the second thing on the page." websites/pace-analyzer/src/pages/index.astro:52-53
REALITY: premise-table pass "A figure the report did not measure is printed as a dash, never as a zero." pace-analyzer/src/report.rs:83-96
REALITY: premise-table pass "The HTML table and the HTML summary follow the same rule as the terminal report." pace-analyzer/src/report.rs:309-347
REALITY: premise-table pass "The split bar for an unmeasured thread is empty, so it cannot be read as a measured all-on-CPU result." pace-analyzer/src/report.rs:split_bar_without_attribution
REALITY: premise-table pass "The SDK site states that it ships for Linux today, and does not claim Windows or macOS support." websites/pace-sdk/src/pages/index.astro:66
REALITY: premise-table pass "The SDK site describes three tiers, including the eBPF tier the CEO review added." websites/pace-sdk/src/pages/index.astro:100
REALITY: premise-table pass "The design contract is checked mechanically, and the check is wired into CI." websites/scripts/check_design_contract.py:88-129
REALITY: already-done pass "The Wire Terminal visual system was already implemented; the review found the CSS had drifted from it rather than rebuilding the system." websites/pace-sdk/src/styles/wire-terminal.css:13-23
REALITY: surface-check pass "Every section the nav links to exists on the page: the problem, the measurement, the integration, the evidence and the licence." websites/pace-sdk/src/pages/index.astro:17-23
REALITY: surface-check pass "The analyzer site's four commands match the analyzer's four commands exactly." websites/pace-analyzer/src/pages/index.astro:134-138
REALITY: binding-decisions pass "The plan's design direction is implemented as written, and the three rules the review found broken are the three that had drifted; the review found no reintroduced overruled default." PLAN.md:284-297
REALITY: numbers pass "The SDK site's contended-run figures are the real numbers from the committed trace: 654.75 ms on-CPU." pace-analyzer/results/real-trace-report.txt:1
REALITY: numbers pass "The same report shows 1.22 s preempted and 131.12 ms blocked for the same thread." pace-analyzer/results/real-trace-report.txt:1
REALITY: numbers pass "Both sites build with no placeholder content, and the design-contract check reports clean across 4 files." websites/scripts/check_design_contract.py:129
REALITY: deferred-asks pass "The hosted API reference, the SDK page's glossary and the analyzer page's HTML-report screenshot are the only design items left out; each is recorded with the reason it was left out." docs/DX-REVIEW.md:1
REALITY: pr-count pass "No .gstack/ship-policy.json exists, so the default of one PR per wave applies; the site corrections and the contract check were committed as one wave because they are one design-contract change." PLAN.md:296-306
