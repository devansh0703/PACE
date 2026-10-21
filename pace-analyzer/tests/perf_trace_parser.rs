// SPDX-License-Identifier: Apache-2.0
//! The `perf trace` parser, tested against the format perf actually emits.
//!
//! The parser was written against a CSV header that `perf trace` does not
//! produce, so it never matched a real trace. These cases use the real
//! line shape, captured from `perf trace -e sched:sched_switch`:
//!
//! ```text
//!      0.000 busy/4194266 sched:sched_switch(prev_comm: "busy",
//!        prev_pid: 4194266 (busy), prev_prio: 120, prev_state: 256,
//!        next_comm: "busy", next_pid: 4194268 (busy), next_prio: 120)
//! ```

use pace_analyzer::parsers::{parse_perf_trace, Format};
use pace_analyzer::Trace;
use std::io::Write;

/// A real `perf trace` line, with the fields the parser reads.
fn line(ts: &str, prev_pid: u64, next_pid: u64) -> String {
    format!(
        "{ts:>11} busy/{prev_pid} sched:sched_switch(prev_comm: \"busy\", \
         prev_pid: {prev_pid} (busy), prev_prio: 120, prev_state: 256, \
         next_comm: \"busy\", next_pid: {next_pid} (busy), next_prio: 120)"
    )
}

fn write_trace(name: &str, lines: &[String]) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).expect("cannot create the trace");
    for l in lines {
        writeln!(f, "{l}").expect("cannot write");
    }
    path
}

#[test]
fn a_real_perf_trace_line_is_parsed() {
    let path = write_trace(
        "pace-perf-trace-simple.csv",
        &[
            line("0.000", 10, 11),
            line("0.500", 11, 12),
            line("1.250", 12, 10),
        ],
    );

    let trace: Trace = parse_perf_trace(&path).expect("the trace must parse");

    // A sched_switch at time T says "prev was running until T, next runs
    // from T". So thread 11 ran from 0 ms to 500 ms and thread 12 ran from
    // 500 ms to 1250 ms.
    let thread11 = trace
        .threads
        .iter()
        .find(|t| t.tid == 11)
        .expect("thread 11");
    let on_cpu: u64 = thread11.samples.iter().map(|s| s.on_cpu_ns).sum();
    assert_eq!(on_cpu, 500_000, "thread 11 ran from 0 ms to 500 ms");

    let thread12 = trace
        .threads
        .iter()
        .find(|t| t.tid == 12)
        .expect("thread 12");
    let on_cpu: u64 = thread12.samples.iter().map(|s| s.on_cpu_ns).sum();
    assert_eq!(on_cpu, 750_000, "thread 12 ran from 500 ms to 1250 ms");

    // Thread 10 was running when the trace began and is running again at
    // the end, and no switch away from it falls inside the trace, so it has
    // no closed interval and is not reported. Reporting the gap would be
    // inventing a measurement.
    assert!(
        !trace.threads.iter().any(|t| t.tid == 10),
        "thread 10 has no closed interval inside the trace, so it must not be          reported"
    );
}

#[test]
fn the_perf_trace_timestamps_are_milliseconds() {
    // A one-second gap in the trace is 1000 in the time column, because the
    // column is milliseconds. Reading it as seconds inflates every interval
    // by a factor of a thousand, which is the bug this test exists for.
    let path = write_trace(
        "pace-perf-trace-units.csv",
        &[line("0.000", 10, 11), line("1000.000", 11, 10)],
    );

    let trace = parse_perf_trace(&path).expect("the trace must parse");
    let total: u64 = trace
        .threads
        .iter()
        .flat_map(|t| t.samples.iter())
        .map(|s| s.on_cpu_ns)
        .sum();
    assert_eq!(
        total, 1_000_000_000,
        "one thread ran for the one second between the two switches, so one \
         second of on-CPU time"
    );
}

#[test]
fn a_perf_trace_carries_no_preemption_split() {
    let path = write_trace(
        "pace-perf-trace-nosplit.csv",
        &[line("0.000", 10, 11), line("0.500", 11, 10)],
    );

    let trace = parse_perf_trace(&path).expect("the trace must parse");
    for thread in &trace.threads {
        for sample in &thread.samples {
            assert!(
                !sample.has_attribution(),
                "a bare perf trace does not carry a preemption/blocking split, \
                 so the sample must not claim one"
            );
            assert_eq!(sample.preempted_ns, 0);
            assert_eq!(sample.blocked_ns, 0);
        }
    }
}

#[test]
fn a_perf_trace_with_a_comm_containing_a_slash_still_parses() {
    // A Linux thread name may contain a slash, so the comm/pid field cannot
    // be split on the first slash.
    let path = std::env::temp_dir().join("pace-perf-trace-slash.csv");
    std::fs::write(
        &path,
        "     0.000 irq/81-FTCS1000/77 sched:sched_switch(prev_comm: \
         \"irq/81-FTCS1000\", prev_pid: 77 (irq/81-FTCS1000:01), prev_prio: 49, \
         prev_state: 256, next_comm: \"irq/81-FTCS1000\", next_pid: 88 \
         (irq/81-FTCS1000:01), next_prio: 49)\n\
         250.000 irq/81-FTCS1000/88 sched:sched_switch(prev_comm: \
         \"irq/81-FTCS1000\", prev_pid: 88 (irq/81-FTCS1000:01), prev_prio: 49, \
         prev_state: 256, next_comm: \"irq/81-FTCS1000\", next_pid: 77 \
         (irq/81-FTCS1000:01), next_prio: 49)\n",
    )
    .expect("cannot write");

    let trace = parse_perf_trace(&path).expect("the trace must parse");
    // The thread id is read from prev_pid, not by splitting the comm/pid
    // field, so a comm containing a slash cannot corrupt it. The only
    // thread with a closed interval is 88, which ran from 0 ms to 250 ms.
    let thread = trace
        .threads
        .iter()
        .find(|t| t.tid == 88)
        .expect("the thread id is read from prev_pid, not from the comm field");
    assert_eq!(thread.tid, 88);
    let on_cpu: u64 = thread.samples.iter().map(|s| s.on_cpu_ns).sum();
    assert_eq!(
        on_cpu, 250_000_000,
        "thread 88 ran from 0 ms to 250 ms, so 250 ms of on-CPU time"
    );
}

#[test]
fn a_line_that_is_not_a_sched_switch_is_skipped() {
    let path = write_trace(
        "pace-perf-trace-other.csv",
        &[
            "     0.000 busy/10 sched:sched_switch(prev_comm: \"busy\", \
             prev_pid: 10 (busy), prev_prio: 120, prev_state: 256, \
             next_comm: \"busy\", next_pid: 11 (busy), next_prio: 120)"
                .to_string(),
            "     0.100 busy/11 sched:syscalls:sys_enter_openat(\
             comm: \"busy\", pid: 11)"
                .to_string(),
            "     0.200 busy/11 sched:sched_switch(prev_comm: \"busy\", \
             prev_pid: 11 (busy), prev_prio: 120, prev_state: 256, \
             next_comm: \"busy\", next_pid: 10 (busy), next_prio: 120)"
                .to_string(),
        ],
    );

    let trace = parse_perf_trace(&path).expect("the trace must parse");
    // The syscall event carries no attribution and must not become a sample.
    // The first switch has no known start for thread 10, and the third
    // closes thread 11's interval, so one sample is produced.
    let samples: usize = trace.threads.iter().map(|t| t.samples.len()).sum();
    assert_eq!(samples, 1, "only the sched_switch events produce samples");
    assert!(
        !trace.threads.iter().any(|t| t.tid == 0),
        "the idle task must not become a thread"
    );
}

#[test]
fn the_format_is_detected_as_a_perf_trace() {
    // A perf trace has no CSV header, so the detection must not treat the
    // first line as one.
    let path = write_trace(
        "pace-perf-trace-detect.csv",
        &[line("0.000", 10, 11), line("0.500", 11, 10)],
    );
    let detected = Format::detect(&path).expect("the format must be detected");
    assert_eq!(
        detected,
        Format::PerfTrace,
        "a perf trace must be detected as a perf trace"
    );
}
