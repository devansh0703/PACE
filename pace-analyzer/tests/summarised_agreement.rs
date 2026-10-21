// SPDX-License-Identifier: Apache-2.0
//! The streaming path and the retaining path must agree.
//!
//! The analyzer has two ways to read a trace: retaining every sample,
//! which `inspect` and `convert` need, and summarising, which folds
//! each sample into its thread's aggregate and drops it, which is what
//! `analyze` uses so that a trace of ten million samples does not need
//! a gigabyte of memory.
//!
//! The two must produce the same numbers. If they do not, then the
//! report a vendor reads depends on which path read the file, and the
//! summarised path -- the one that runs on the big traces -- is the one
//! nobody checks.

use pace_analyzer::model::analyze;
use pace_analyzer::parsers::{parse_file, parse_summarised, Format};
use pace_analyzer::Thresholds;
use std::fs;
use std::path::Path;

/// A trace with several threads, phases, and a mix of attributed and
/// unattributed samples.
fn write_trace(path: &Path) {
    let mut text = String::new();
    for tid in [101u64, 202, 303] {
        for i in 0..50u64 {
            let wall = 1_000_000 + i * 1000;
            let on_cpu = wall / 2;
            let preempted = wall / 4;
            let blocked = wall - on_cpu - preempted;
            let phase = if i % 2 == 0 {
                "\"render\""
            } else {
                "\"update\""
            };
            text.push_str(&format!(
                "{{\"ph\":\"X\",\"name\":\"sample\",\"ts\":{},\"dur\":{},\"tid\":{},\
                 \"args\":{{\"wall_ns\":{},\"on_cpu_ns\":{},\"preempted_ns\":{},\
                 \"blocked_ns\":{},\"phase\":{}}}}}\n",
                i * 1000,
                wall,
                tid,
                wall,
                on_cpu,
                preempted,
                blocked,
                phase
            ));
        }
    }
    // A thread with no attribution, so the "no attribution" path is
    // covered too.
    for i in 0..10u64 {
        let wall = 500_000u64;
        text.push_str(&format!(
            "{{\"ph\":\"X\",\"name\":\"sample\",\"ts\":{},\"dur\":{},\"tid\":404,\
             \"args\":{{\"wall_ns\":{},\"on_cpu_ns\":{},\"preempted_ns\":0,\"blocked_ns\":0}}}}\n",
            i * 1000,
            wall,
            wall,
            wall
        ));
    }
    fs::write(path, text).unwrap();
}

#[test]
fn the_summarised_and_the_retained_path_agree() {
    let dir = std::env::temp_dir().join("pace-summarised-test");
    fs::create_dir_all(&dir).unwrap();
    let path = dir.join("trace.jsonl");
    write_trace(&path);

    let full = parse_file(&path, Format::Pace).unwrap();
    let summarised = parse_summarised(&path, Format::Pace).unwrap();

    // The retained path has the samples; the summarised path has the
    // aggregate and none of the samples.
    assert!(
        full.threads.iter().all(|t| !t.samples.is_empty()),
        "the retained trace must have its samples"
    );
    assert!(
        summarised
            .threads
            .iter()
            .all(|t| t.samples.is_empty() && t.summary.is_some()),
        "the summarised trace must have the aggregate and no samples"
    );

    let thresholds = Thresholds::default();
    let a = analyze(&full, &thresholds);
    let b = analyze(&summarised, &thresholds);

    assert_eq!(
        a.total_wall_ns, b.total_wall_ns,
        "the total wall time must be the same"
    );
    assert_eq!(
        a.total_on_cpu_ns, b.total_on_cpu_ns,
        "the total on-CPU time must be the same"
    );
    assert_eq!(
        a.total_preempted_ns, b.total_preempted_ns,
        "the total preempted time must be the same"
    );
    assert_eq!(
        a.total_blocked_ns, b.total_blocked_ns,
        "the total blocked time must be the same"
    );
    assert_eq!(
        a.has_attribution, b.has_attribution,
        "attribution must agree"
    );
    assert_eq!(
        a.findings.len(),
        b.findings.len(),
        "the findings must be the same count"
    );

    // The verification must agree too, and it must be a real check: the
    // summarised path checks the invariant as it reads, so the report
    // never says "exact" for a trace it did not check.
    let va = full.verify();
    let vb = summarised.verify();
    assert_eq!(
        va.total_samples, vb.total_samples,
        "the verified sample count must be the same"
    );
    assert_eq!(
        va.violations, vb.violations,
        "the violation count must be the same"
    );
    assert_eq!(
        va.worst_error_ns, vb.worst_error_ns,
        "the worst violation must be the same"
    );
    assert!(
        va.total_samples > 0,
        "the verification must have checked something"
    );
    assert_eq!(
        a.threads.len(),
        b.threads.len(),
        "the thread count must agree"
    );

    for (x, y) in a.threads.iter().zip(b.threads.iter()) {
        assert_eq!(x.tid, y.tid, "the threads must be in the same order");
        assert_eq!(x.wall_ns, y.wall_ns, "thread {} wall", x.tid);
        assert_eq!(x.on_cpu_ns, y.on_cpu_ns, "thread {} on-CPU", x.tid);
        assert_eq!(x.preempted_ns, y.preempted_ns, "thread {} preempted", x.tid);
        assert_eq!(x.blocked_ns, y.blocked_ns, "thread {} blocked", x.tid);
        assert_eq!(
            x.sample_count, y.sample_count,
            "thread {} sample count",
            x.tid
        );
        assert_eq!(
            x.has_attribution, y.has_attribution,
            "thread {} attribution",
            x.tid
        );
        assert_eq!(
            x.phases.len(),
            y.phases.len(),
            "thread {} must have the same phases",
            x.tid
        );
        for (name, phase) in &x.phases {
            let other = y.phases.get(name).expect("the phase must be there");
            assert_eq!(phase.wall_ns, other.wall_ns, "phase {} wall", name);
            assert_eq!(
                phase.sample_count, other.sample_count,
                "phase {} sample count",
                name
            );
        }
    }

    fs::remove_dir_all(&dir).ok();
}

#[test]
fn the_summarised_path_does_not_grow_with_the_sample_count() {
    let dir = std::env::temp_dir().join("pace-summarised-size-test");
    fs::create_dir_all(&dir).unwrap();
    let path = dir.join("trace.jsonl");

    // Ten thousand samples for one thread.
    let mut text = String::new();
    for i in 0..10_000u64 {
        text.push_str(&format!(
            "{{\"ph\":\"X\",\"name\":\"sample\",\"ts\":{},\"dur\":1000,\"tid\":7,\
             \"args\":{{\"wall_ns\":1000,\"on_cpu_ns\":500,\"preempted_ns\":300,\
             \"blocked_ns\":200}}}}\n",
            i * 1000
        ));
    }
    fs::write(&path, text).unwrap();

    let full = parse_file(&path, Format::Pace).unwrap();
    let summarised = parse_summarised(&path, Format::Pace).unwrap();

    assert_eq!(full.threads[0].samples.len(), 10_000);
    assert_eq!(summarised.threads[0].samples.len(), 0);

    // The aggregate is the same, and the summarised trace holds no
    // samples at all, so the heap it needs is proportional to the
    // number of threads rather than the number of samples.
    let full_capacity = full.threads[0].samples.capacity();
    let summarised_capacity = summarised.threads[0].samples.capacity();
    assert!(
        full_capacity >= 10_000,
        "the retained trace holds them all: {}",
        full_capacity
    );
    assert_eq!(
        summarised_capacity, 0,
        "the summarised trace holds none of them"
    );

    let thresholds = Thresholds::default();
    let a = analyze(&full, &thresholds);
    let b = analyze(&summarised, &thresholds);
    assert_eq!(a.total_wall_ns, b.total_wall_ns);
    assert_eq!(
        a.threads[0].sample_count, b.threads[0].sample_count,
        "the sample count must be the same"
    );

    fs::remove_dir_all(&dir).ok();
}
