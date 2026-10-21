/* SPDX-License-Identifier: BUSL-1.1
 * PACE — licensing.
 *
 * The SDK is licensed under the Business Source License 1.1. The analyzer is
 * Apache-2.0. This file explains why, and gives the exact header a source
 * file must carry.
 */
# Licensing

## The two licences, and why they differ

| Product | Licence | Why |
|---|---|---|
| `pace-analyzer` | **Apache-2.0** | It is the proof of accuracy and the adoption funnel. A prospective licensee evaluates the attribution by running it, and it has to be free to run. It also makes the method reviewable, which is what makes the SDK's claim credible. |
| `pace-sdk` | **BUSL 1.1** | It is the thing being licensed. Apache-2.0 would let a competitor resell it; AGPL would make a commercial vendor refuse to link it into a proprietary profiler. BUSL is the shape that works: the source is visible to licensees, production use requires a paid licence, and the code becomes Apache-2.0 after the Change Date. |

## Why not Elastic License 2.0

ELv2 forbids "managing or offering the Licensed Work as a product or service
to third parties". A tool vendor that links PACE into their profiler and
sells the profiler to their customers is doing exactly that. ELv2 would make
the product unsellable, so BUSL is used instead, with an Additional Use Grant
that names the permitted production use.

## The exact terms

```
Business Source License 1.1

Licensor:      PACE
Change Date:   2030-11-01
Change License: Apache-2.0

Additional Use Grant:
Production use of the Licensed Work is permitted to any holder of a
current, valid PACE commercial licence. All other rights are reserved
under the Business Source License 1.1.
```

The Business Source License 1.1 text is at <https://mariadb.com/bsl11/> and
the SPDX record is at <https://spdx.org/licenses/BUSL-1.1.html>.

Mechanics that matter:

- "The Licensor may make an Additional Use Grant, above, permitting limited
  production use." — this is what makes a paying licensee's production use
  legal before the Change Date.
- "Effective on the Change Date, or the fourth anniversary of the first
  publicly available distribution of a specific version of the Licensed Work
  under this License, the Licensor hereby grants you rights under the terms of
  the Change License" — the four years is the **default maximum**; the Change
  Date itself can be earlier. PACE sets 2030-11-01.

## The per-file header

Every source file in `pace-sdk` must carry this header. It is what the
SPDX tooling reads, and it is what tells a licensee exactly what they are
looking at.

```c
/* SPDX-License-Identifier: BUSL-1.1
 *
 * PACE — Preemption-Aware Cycle Attribution Engine
 * Copyright (c) 2026 PACE contributors
 *
 * Business Source License 1.1
 * Licensor: PACE
 * Change Date: 2030-11-01
 * Change License: Apache-2.0
 *
 * Additional Use Grant: Production use of the Licensed Work is permitted to
 * any holder of a current, valid PACE commercial licence. All other rights
 * are reserved under the Business Source License 1.1.
 *
 * NOTICE: This file is part of PACE. The complete terms of the Business
 * Source License 1.1 are available at:
 *   https://mariadb.com/bsl11/
 */
```

## What a licensee gets, and what they do not

**The evaluation licence** gives a licensee:

- the library, for all three tiers;
- the benchmark suite and the accuracy harness;
- the accuracy report and the environment artifact from their own machine;
- the right to publish that result as part of their own evaluation.

**The evaluation licence does not** give the right to ship PACE in a
released product. That requires a commercial licence.

**The commercial licence** adds the right to use PACE in a product offered
to third parties, for the term and seats purchased.

**Neither licence** grants a trademark, a patent, or a right to the PACE
name. The BUSL's Additional Use Grant covers use of the code, not use of the
mark.

## The patent position

The design relies entirely on operating-system primitives:
`perf_event_open`, `/proc/<tid>/schedstat`, `GetThreadTimes`,
`QueryThreadCycleTime`, `thread_info`, and the scheduler's own
`sched_wakeup`/`sched_switch` tracepoints. PACE adds no novel measurement
mechanism; the invention is in the aggregation (a portable per-thread
attribution with an explicit provenance model) and in the embedding (a
library a vendor links into an existing profiler).

That is a materially different freedom-to-operate posture from a patented
cycle-accounting mechanism. A full patent search across USPTO, Espacenet and
Google Patents is a **pre-licensing gate** and has not been performed. No
patent number appears anywhere in this repository, and none may be published
until that search is done and reviewed.

## For contributors

Every contribution to `pace-sdk` is made under the Business Source License
1.1 with the terms above, and every contribution to `pace-analyzer` is made
under Apache-2.0.
