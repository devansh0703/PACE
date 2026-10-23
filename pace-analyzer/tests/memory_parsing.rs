// SPDX-License-Identifier: Apache-2.0
//! The in-memory entry point must agree with the file one.
//!
//! `parse_memory` exists so the service can analyse an uploaded trace without
//! writing it to a temporary file first. A second entry point is a second
//! place for the numbers to differ, so this asserts that the two produce
//! identical totals rather than assuming the refactor was faithful.

use pace_analyzer::parsers::{parse_file, parse_memory, Format};
use pace_analyzer::{analyze, Thresholds, Trace};
use std::io::Write;
use std::path::PathBuf;

/// A small but real PACE JSONL trace: two threads, the first contended and
/// the second mostly blocked.
const TRACE: &str = r#"{"name":"sample","tid":101,"wall_ns":1000000,"on_cpu_ns":400000,"preempted_ns":500000,"blocked_ns":100000,"ref_cycles":1000,"flags":3}
{"name":"sample","tid":101,"wall_ns":1000000,"on_cpu_ns":300000,"preempted_ns":600000,"blocked_ns":100000,"ref_cycles":900,"flags":3}
{"name":"sample","tid":202,"wall_ns":2000000,"on_cpu_ns":100000,"preempted_ns":100000,"blocked_ns":1800000,"ref_cycles":200,"flags":3}
{"name":"sample","tid":202,"wall_ns":2000000,"on_cpu_ns":200000,"preempted_ns":100000,"blocked_ns":1700000,"ref_cycles":300,"flags":3}
"#;

fn totals(trace: &Trace) -> Vec<(u64, u64, u64, u64)> {
    let analysis = analyze(trace, &Thresholds::default());
    let mut rows: Vec<(u64, u64, u64, u64)> = analysis
        .threads
        .iter()
        .map(|t| (t.tid, t.on_cpu_ns, t.preempted_ns, t.blocked_ns))
        .collect();
    rows.sort();
    rows
}

/// A scratch path under the system temp directory, unique to this process.
///
/// The test only needs a file that `parse_file` can open. Pulling in a
/// temporary-file crate for that would be a dependency in the published
/// manifest for four lines of test setup.
fn scratch(name: &str) -> PathBuf {
    let mut path = std::env::temp_dir();
    path.push(format!("pace-memory-{}-{}", std::process::id(), name));
    path
}

#[test]
fn the_in_memory_path_agrees_with_the_file_path() {
    let path = scratch("trace.jsonl");
    let mut file = std::fs::File::create(&path).expect("create the scratch trace");
    file.write_all(TRACE.as_bytes()).expect("write the trace");
    drop(file);

    let from_file = parse_file(&path, Format::Pace).expect("the file parses");
    let from_memory =
        parse_memory(TRACE.as_bytes(), Some(Format::Pace), true).expect("the buffer parses");

    assert_eq!(
        totals(&from_file),
        totals(&from_memory),
        "the file and in-memory paths must produce the same attribution"
    );
    assert_eq!(from_file.sample_count(), from_memory.sample_count());
    assert_eq!(from_file.malformed_lines, 0);

    let _ = std::fs::remove_file(&path);
}

#[test]
fn an_in_memory_trace_is_detected_as_the_same_format_as_a_file() {
    assert_eq!(Format::detect_bytes(TRACE.as_bytes()), Format::Pace);

    // And with detection left to the parser, the result is the same as when
    // the format is stated, which is the behaviour the service relies on.
    let detected_parse = parse_memory(TRACE.as_bytes(), None, true).expect("detected parse");
    let stated_parse =
        parse_memory(TRACE.as_bytes(), Some(Format::Pace), true).expect("stated parse");
    assert_eq!(totals(&detected_parse), totals(&stated_parse));
}

#[test]
fn the_in_memory_decomposition_still_balances() {
    let trace = parse_memory(TRACE.as_bytes(), None, true).expect("the buffer parses");
    let report = trace.verify();
    assert!(
        report.is_ok(),
        "the invariant must hold on a trace read from memory"
    );
    assert_eq!(report.violations, 0);
    assert_eq!(report.total_samples, 4);
}

#[test]
fn a_buffer_that_is_not_a_trace_is_reported_rather_than_panicking() {
    // The service is on a public endpoint, so a hostile body has to be a
    // handled error and not a crash.
    let trace =
        parse_memory(b"not a trace at all\n", None, true).expect("no samples is not a crash");
    assert_eq!(
        trace.sample_count(),
        0,
        "a buffer with no samples must carry none"
    );
}
