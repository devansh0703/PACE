// SPDX-License-Identifier: Apache-2.0
//! Tests for the analyzer's attribution model and parsers.

use pace_analyzer::model::{
    analyze, Sample, Source, Thresholds, FLAG_HAS_BLOCKED, FLAG_HAS_PREEMPTED,
};
use pace_analyzer::{Format, ThreadTrace, Trace};

/// A sample with a real preemption/blocking split, as the SDK produces.
fn sample(wall: u64, on_cpu: u64, preempted: u64, blocked: u64) -> Sample {
    Sample {
        wall_ns: wall,
        on_cpu_ns: on_cpu,
        preempted_ns: preempted,
        blocked_ns: blocked,
        core_cycles: 0,
        ref_cycles: 0,
        core_per_ref_ratio: 0.0,
        involuntary_switches: 0,
        voluntary_switches: 0,
        source: Source::Clock,
        flags: FLAG_HAS_PREEMPTED | FLAG_HAS_BLOCKED,
        phase: None,
    }
}

/// A sample from a format that carries no attribution at all, such as a
/// plain Chrome trace event.
fn sample_without_attribution(wall: u64) -> Sample {
    Sample {
        wall_ns: wall,
        on_cpu_ns: wall,
        preempted_ns: 0,
        blocked_ns: 0,
        core_cycles: 0,
        ref_cycles: 0,
        core_per_ref_ratio: 0.0,
        involuntary_switches: 0,
        voluntary_switches: 0,
        source: Source::Unknown,
        flags: 0,
        phase: None,
    }
}

#[test]
fn balanced_sample_has_no_error() {
    let s = sample(100, 40, 50, 10);
    assert!(s.is_balanced());
    assert_eq!(s.balance_error_ns(), 0);
}

#[test]
fn unbalanced_sample_reports_the_gap() {
    let s = sample(100, 40, 50, 5);
    assert!(!s.is_balanced());
    assert_eq!(s.balance_error_ns(), 5);
}

#[test]
fn verify_counts_violations() {
    let trace = Trace {
        threads: vec![ThreadTrace {
            tid: 1,
            name: None,
            samples: vec![sample(100, 40, 50, 10), sample(100, 40, 50, 5)],
            summary: None,
            summary_verify: None,
        }],
        reference_hz: None,
        ..Default::default()
    };
    let report = trace.verify();
    assert_eq!(report.total_samples, 2);
    assert_eq!(report.violations, 1);
    assert!(!report.is_ok());
}

#[test]
fn verify_passes_on_a_balanced_trace() {
    let trace = Trace {
        threads: vec![ThreadTrace {
            tid: 1,
            name: None,
            samples: vec![sample(100, 40, 50, 10)],
            summary: None,
            summary_verify: None,
        }],
        reference_hz: None,
        ..Default::default()
    };
    assert!(trace.verify().is_ok());
}

#[test]
fn analysis_aggregates_per_thread() {
    let trace = Trace {
        threads: vec![ThreadTrace {
            tid: 7,
            name: Some("worker".to_string()),
            samples: vec![sample(100, 40, 50, 10), sample(100, 60, 30, 10)],
            summary: None,
            summary_verify: None,
        }],
        reference_hz: None,
        ..Default::default()
    };
    let analysis = analyze(&trace, &Thresholds::default());
    assert_eq!(analysis.threads.len(), 1);
    let thread = &analysis.threads[0];
    assert_eq!(thread.wall_ns, 200);
    assert_eq!(thread.on_cpu_ns, 100);
    assert_eq!(thread.preempted_ns, 80);
    assert_eq!(thread.blocked_ns, 20);
    assert_eq!(thread.sample_count, 2);
    assert!((thread.preempted_fraction() - 0.4).abs() < 1e-9);
    assert!((thread.on_cpu_fraction() - 0.5).abs() < 1e-9);
}

#[test]
fn analysis_raises_a_finding_for_a_preempted_thread() {
    let trace = Trace {
        threads: vec![ThreadTrace {
            tid: 9,
            name: None,
            samples: vec![sample(100, 10, 80, 10)],
            summary: None,
            summary_verify: None,
        }],
        reference_hz: None,
        ..Default::default()
    };
    let analysis = analyze(&trace, &Thresholds::default());
    assert!(!analysis.findings.is_empty());
    let finding = analysis
        .findings
        .iter()
        .find(|f| f.tid == Some(9))
        .expect("a per-thread finding");
    assert!(finding.summary.contains("preemption"));
    assert!((finding.value - 80.0).abs() < 1e-9);
}

#[test]
fn analysis_raises_a_process_level_finding() {
    let trace = Trace {
        threads: vec![ThreadTrace {
            tid: 1,
            name: None,
            samples: vec![sample(100, 10, 80, 10)],
            summary: None,
            summary_verify: None,
        }],
        reference_hz: None,
        ..Default::default()
    };
    let analysis = analyze(&trace, &Thresholds::default());
    assert!(analysis.findings.iter().any(|f| f.tid.is_none()));
}

#[test]
fn analysis_keeps_phase_attribution() {
    let mut a = sample(100, 40, 50, 10);
    a.phase = Some("warmup".to_string());
    let mut b = sample(100, 80, 10, 10);
    b.phase = Some("steady".to_string());
    let trace = Trace {
        threads: vec![ThreadTrace {
            tid: 1,
            name: None,
            samples: vec![a, b],
            summary: None,
            summary_verify: None,
        }],
        reference_hz: None,
        ..Default::default()
    };
    let analysis = analyze(&trace, &Thresholds::default());
    let phases = &analysis.threads[0].phases;
    assert_eq!(phases.len(), 2);
    assert_eq!(phases["warmup"].preempted_ns, 50);
    assert_eq!(phases["steady"].preempted_ns, 10);
}

#[test]
fn format_detection_finds_pace_jsonl() {
    let dir = std::env::temp_dir().join("pace-analyzer-format-test");
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("trace.jsonl");
    std::fs::write(
        &path,
        "{\"name\":\"pace.sample\",\"ph\":\"X\",\"tid\":1,\"args\":{\"wall_ns\":100,\"on_cpu_ns\":40,\"preempted_ns\":50,\"blocked_ns\":10}}\n",
    )
    .unwrap();
    assert_eq!(Format::detect(&path).unwrap(), Format::Pace);
    std::fs::remove_file(&path).unwrap();
}

#[test]
fn format_detection_finds_chrome_trace() {
    let dir = std::env::temp_dir().join("pace-analyzer-chrome-test");
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("trace.json");
    std::fs::write(
        &path,
        "{\"traceEvents\":[{\"ph\":\"X\",\"tid\":1,\"dur\":5}]}",
    )
    .unwrap();
    assert_eq!(Format::detect(&path).unwrap(), Format::Chrome);
    std::fs::remove_file(&path).unwrap();
}

#[test]
fn pace_jsonl_parser_round_trips() {
    let dir = std::env::temp_dir().join("pace-analyzer-parse-test");
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("trace.jsonl");
    std::fs::write(
        &path,
        "{\"tid\":1,\"args\":{\"wall_ns\":100,\"on_cpu_ns\":40,\"preempted_ns\":50,\"blocked_ns\":10}}\n\
         {\"tid\":1,\"args\":{\"wall_ns\":200,\"on_cpu_ns\":90,\"preempted_ns\":80,\"blocked_ns\":30}}\n\
         {\"tid\":2,\"args\":{\"wall_ns\":50,\"on_cpu_ns\":50,\"preempted_ns\":0,\"blocked_ns\":0}}\n",
    )
    .unwrap();

    let trace = pace_analyzer::parsers::parse_file(&path, Format::Pace).unwrap();
    assert_eq!(trace.threads.len(), 2);
    assert_eq!(trace.thread(1).unwrap().samples.len(), 2);
    assert!(trace.verify().is_ok());
    std::fs::remove_file(&path).unwrap();
}

#[test]
fn chrome_parser_reads_duration_into_wall() {
    let dir = std::env::temp_dir().join("pace-analyzer-chrome-parse-test");
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("trace.json");
    std::fs::write(
        &path,
        "{\"traceEvents\":[{\"ph\":\"X\",\"tid\":1,\"dur\":100.0,\"args\":{\"wall_ns\":100000,\"on_cpu_ns\":40000,\"preempted_ns\":50000,\"blocked_ns\":10000}}]}",
    )
    .unwrap();
    let trace = pace_analyzer::parsers::parse_file(&path, Format::Chrome).unwrap();
    assert_eq!(trace.threads.len(), 1);
    assert_eq!(trace.threads[0].samples[0].on_cpu_ns, 40000);
    assert!(trace.verify().is_ok());
    std::fs::remove_file(&path).unwrap();
}

#[test]
fn html_report_contains_the_numbers() {
    let trace = Trace {
        threads: vec![ThreadTrace {
            tid: 42,
            name: None,
            samples: vec![sample(1_000_000, 250_000, 600_000, 150_000)],
            summary: None,
            summary_verify: None,
        }],
        reference_hz: None,
        ..Default::default()
    };
    let analysis = analyze(&trace, &Thresholds::default());
    let html = pace_analyzer::render_html(&analysis, &trace, "test");
    assert!(html.contains("pace-analyzer"));
    assert!(html.contains("42"));
    assert!(html.contains("preempted"));
    assert!(
        !html.contains("http://"),
        "the report must not fetch anything"
    );
}

#[test]
fn terminal_report_lists_the_thread() {
    let trace = Trace {
        threads: vec![ThreadTrace {
            tid: 5,
            name: None,
            samples: vec![sample(1_000_000, 250_000, 600_000, 150_000)],
            summary: None,
            summary_verify: None,
        }],
        reference_hz: None,
        ..Default::default()
    };
    let analysis = analyze(&trace, &Thresholds::default());
    let text = pace_analyzer::render_terminal(&analysis, &trace);
    assert!(text.contains("PACE thread attribution"));
    assert!(text.contains("exact"));
}

#[test]
fn a_thread_without_attribution_raises_no_finding() {
    // A plain Chrome trace has a duration and no split. Reporting "0 %
    // preempted" as a finding would be an artefact of the format.
    let trace = Trace {
        threads: vec![ThreadTrace {
            tid: 3,
            name: None,
            samples: vec![sample_without_attribution(1_000_000)],
            summary: None,
            summary_verify: None,
        }],
        reference_hz: None,
        ..Default::default()
    };
    let analysis = analyze(&trace, &Thresholds::default());
    assert!(
        !analysis.has_attribution,
        "the analysis reports no attribution"
    );
    assert!(
        analysis.findings.is_empty(),
        "no findings for a thread with no attribution: {:?}",
        analysis.findings
    );

    let text = pace_analyzer::render_terminal(&analysis, &trace);
    assert!(
        text.contains("carries no preemption/blocking split"),
        "the report says the attribution was unavailable: {}",
        text
    );

    let html = pace_analyzer::render_html(&analysis, &trace, "test");
    assert!(
        html.contains("no attribution"),
        "the HTML report says so too"
    );

    // The report must not print a confident 0.0% for a figure that was never
    // measured. A dash says "not measured"; a zero says "measured, and it
    // was zero", which is a different and much stronger claim.
    assert!(
        !text.contains("0.0%"),
        "the report printed a 0.0% for an unmeasured split: {}",
        text
    );
    assert!(
        text.contains('-'),
        "the report does not show a dash for the unmeasured split: {}",
        text
    );
    assert!(
        !html.contains("0.0%"),
        "the HTML report printed a 0.0% for an unmeasured split: {}",
        html
    );
}

#[test]
fn a_thread_with_attribution_still_raises_a_finding() {
    let trace = Trace {
        threads: vec![ThreadTrace {
            tid: 3,
            name: None,
            samples: vec![sample(1_000_000, 100_000, 800_000, 100_000)],
            summary: None,
            summary_verify: None,
        }],
        reference_hz: None,
        ..Default::default()
    };
    let analysis = analyze(&trace, &Thresholds::default());
    assert!(analysis.has_attribution);
    assert!(!analysis.findings.is_empty());
}

#[test]
fn malformed_lines_are_counted_not_fatal() {
    let dir = std::env::temp_dir().join("pace-analyzer-malformed-test");
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("trace.jsonl");
    // Three good lines, two truncated, one complete-but-wrong-shape.
    std::fs::write(
        &path,
        "{\"tid\":1,\"args\":{\"wall_ns\":100,\"on_cpu_ns\":40,\"preempted_ns\":50,\"blocked_ns\":10}}\n\
         {\"tid\":1,\"args\":{\"wall_ns\":200,\"on_cpu_ns\":90,\"preempted_ns\":80,\"blocked_ns\":30}}\n\
         {\"tid\":1,\"args\":{\"wall_ns\":100,\"on_cpu_ns\":40,\"preempted_ns\":50,\"blocked_ns\":10}}\n\
         {\"tid\":1,\"args\":{\"wall_ns\":2\n\
         {\"tid\":1,\"args\":{\"wall_ns\":300,\"on_cpu_ns\":100,\"preempted_ns\":150,\"blocked_ns\":50}}\n\
         {\"tid\":1,\"args\":{\"wall_ns\":\"not a number\"}}\n",
    )
    .unwrap();

    let trace = pace_analyzer::parsers::parse_file(&path, Format::Pace).unwrap();
    assert_eq!(trace.malformed_lines, 2, "two lines could not be read");
    assert_eq!(
        trace.thread(1).unwrap().samples.len(),
        4,
        "the good lines survived"
    );
    assert!(
        trace.verify().is_ok(),
        "the surviving samples still balance"
    );
    std::fs::remove_file(&path).unwrap();
}

#[test]
fn an_empty_trace_verifies_vacuously_but_is_reported() {
    // The invariant holds trivially on an empty trace. The CLI refuses to
    // call that a pass, so the model must expose the sample count.
    let trace = Trace::default();
    let report = trace.verify();
    assert_eq!(report.total_samples, 0);
    assert!(report.is_ok());
}

#[test]
fn the_html_report_flags_a_partial_trace() {
    let mut trace = Trace {
        threads: vec![ThreadTrace {
            tid: 1,
            name: None,
            samples: vec![sample(100, 40, 50, 10)],
            summary: None,
            summary_verify: None,
        }],
        reference_hz: None,
        ..Default::default()
    };
    trace.malformed_lines = 7;
    let analysis = analyze(&trace, &Thresholds::default());
    let html = pace_analyzer::render_html(&analysis, &trace, "test");
    assert!(
        html.contains("partial trace"),
        "the HTML report flags the partial trace"
    );
    assert!(html.contains("7 line(s)"), "and says how many");
}
