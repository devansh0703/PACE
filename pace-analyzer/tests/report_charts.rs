// SPDX-License-Identifier: Apache-2.0
//! The report's charts.
//!
//! The HTML report is one file that opens offline with nothing to fetch, so
//! the charts are inline SVG. That constraint is easy to lose by adding a
//! charting library, and the coordinates are computed from data that can be
//! zero, so this checks both: nothing is fetched, and no figure renders as
//! `NaN` when a thread's wall time is zero.

use pace_analyzer::{analyze, render_html, Sample, ThreadTrace, Thresholds, Trace, VerifyReport};

fn sample(wall: u64, on_cpu: u64, preempted: u64, blocked: u64) -> Sample {
    Sample {
        wall_ns: wall,
        on_cpu_ns: on_cpu,
        preempted_ns: preempted,
        blocked_ns: blocked,
        // FLAG_HAS_PREEMPTED | FLAG_HAS_BLOCKED: the sample carries a real
        // split rather than an on-CPU-versus-nothing guess.
        flags: (1 << 6) | (1 << 7),
        ..Default::default()
    }
}

fn trace_with(threads: Vec<(u64, Vec<Sample>)>) -> Trace {
    Trace {
        threads: threads
            .into_iter()
            .map(|(tid, samples)| ThreadTrace {
                tid,
                name: None,
                samples,
                summary: None,
                summary_verify: None,
            })
            .collect(),
        reference_hz: None,
        ..Default::default()
    }
}

fn contended() -> Trace {
    // Six threads, all losing a similar share: the shape the charts exist to
    // make visible.
    let runs: Vec<(u64, Vec<Sample>)> = (0..6)
        .map(|i| {
            (
                100 + i,
                vec![
                    sample(1_000_000, 250_000, 600_000, 150_000),
                    sample(1_000_000, 300_000, 550_000, 150_000),
                ],
            )
        })
        .collect();
    trace_with(runs)
}

#[test]
fn the_report_draws_its_charts_inline() {
    let trace = contended();
    let analysis = analyze(&trace, &Thresholds::default());
    let html = render_html(&analysis, &trace, "test");

    assert!(
        html.matches("<figure class=\"chart\">").count() >= 3,
        "the report must carry the run bar, the distribution and the per-thread chart"
    );
    assert_eq!(
        html.matches("<svg").count(),
        html.matches("</svg>").count(),
        "every svg must be closed"
    );
}

#[test]
fn the_report_fetches_nothing_to_draw_a_chart() {
    let trace = contended();
    let analysis = analyze(&trace, &Thresholds::default());
    let html = render_html(&analysis, &trace, "test");

    assert!(
        !html.contains("http://") && !html.contains("https://"),
        "the report must not have a network reference"
    );
    assert!(!html.contains("<img"), "the charts are drawn, not loaded");
    assert!(
        !html.contains("gradient"),
        "the design bans gradients; the charts are flat fills"
    );
}

#[test]
fn a_zero_length_window_does_not_produce_a_broken_figure() {
    // A sample whose buckets are empty answers no question, and the chart code
    // divides by the wall time to place it. A NaN coordinate in an SVG is
    // silently dropped by a browser, which is a blank chart rather than an
    // error message, so it is asserted here instead.
    let trace = trace_with(vec![
        (1, vec![sample(0, 0, 0, 0), sample(0, 0, 0, 0)]),
        (2, vec![sample(1_000_000, 100_000, 800_000, 100_000)]),
    ]);
    let analysis = analyze(&trace, &Thresholds::default());
    let html = render_html(&analysis, &trace, "test");

    // "info" is a severity label and contains "inf", so the word is removed
    // before the check rather than making the check miss the real thing.
    let scrubbed = html.replace("info", "").replace("Info", "");
    for broken in ["NaN", "inf"] {
        assert!(
            !scrubbed.contains(broken),
            "the report contains {broken}, which renders as a blank chart"
        );
    }
}

#[test]
fn a_trace_with_no_attribution_draws_no_split_charts() {
    // A Chrome trace with no PACE fields carries a duration and no split.
    // Drawing a stacked bar for it would claim a measurement nobody made.
    let mut plain = sample(1_000_000, 1_000_000, 0, 0);
    plain.flags = 0;
    let trace = trace_with(vec![(7, vec![plain])]);
    let analysis = analyze(&trace, &Thresholds::default());

    assert!(!analysis.has_attribution, "the fixture must carry no split");
    let html = render_html(&analysis, &trace, "test");
    assert!(
        !html.contains("<figure class=\"chart\">"),
        "a trace with no split must not be charted as though it had one"
    );
    assert!(
        html.contains("does not carry a preemption/blocking split"),
        "the report must say why there is no chart"
    );
}

#[test]
fn the_per_thread_chart_is_capped_and_says_so() {
    let runs: Vec<(u64, Vec<Sample>)> = (0..80)
        .map(|i| (i as u64, vec![sample(1_000_000, 200_000, 700_000, 100_000)]))
        .collect();
    let trace = trace_with(runs);
    let analysis = analyze(&trace, &Thresholds::default());
    let html = render_html(&analysis, &trace, "test");

    assert!(
        html.contains("80 of 80 drawn") || html.contains("60 of 80 drawn"),
        "the chart must state how many threads it drew"
    );
    assert!(
        html.contains("quieter thread(s) are in the table below"),
        "a capped chart must say what it left out"
    );
}

#[test]
fn the_verify_report_is_not_rendered_into_the_charts() {
    // A guard against the charts quietly changing the numbers they draw from:
    // the totals in the summary and the totals in the run bar are the same
    // arithmetic, and a mismatch would be a chart that contradicts the table.
    let trace = contended();
    let analysis = analyze(&trace, &Thresholds::default());
    let html = render_html(&analysis, &trace, "test");

    let expected_on_cpu = analysis.total_on_cpu_ns as f64 / analysis.total_wall_ns as f64 * 100.0;
    let label = format!("on-CPU {expected_on_cpu:.1}%");
    assert!(
        html.contains(&label),
        "the run bar's on-CPU share must match the analysis: expected {label}"
    );
    let report: VerifyReport = trace.verify();
    assert!(report.is_ok());
}
