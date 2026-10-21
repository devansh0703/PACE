// SPDX-License-Identifier: Apache-2.0
//! Trace format parsers.
//!
//! Each parser turns an external trace format into the common [`Trace`] model.
//! Nothing here invents data: a field the source format does not carry stays
//! at its default.

use crate::model::{Sample, Source, ThreadAttribution, ThreadTrace, Trace, VerifyReport};
use std::collections::HashMap;
use std::fs::File;
use std::io::{BufRead, BufReader, Read};
use std::path::Path;

/// The formats the analyzer can read.
#[derive(Debug, Clone, Copy, PartialEq, Eq, clap::ValueEnum)]
pub enum Format {
    /// PACE's own JSONL trace format.
    Pace,
    /// Chrome Trace Event JSON.
    Chrome,
    /// `perf sched timehist` text output.
    PerfSched,
    /// `perf trace` CSV output.
    PerfTrace,
}

impl Format {
    /// Detect the format from a file's contents.
    ///
    /// A PACE trace is JSON Lines (one JSON object per line) and a Chrome
    /// trace is one JSON document. Both start with `{`, so the distinction
    /// is made by looking for a `traceEvents` key, not by the first byte.
    pub fn detect(path: &Path) -> crate::Result<Format> {
        let mut head = String::new();
        {
            let mut file = File::open(path)?;
            let mut buf = vec![0u8; 8192];
            let n = file.read(&mut buf)?;
            head.push_str(&String::from_utf8_lossy(&buf[..n]));
        }

        let trimmed = head.trim_start();
        if trimmed.starts_with('{') {
            if head.contains("traceEvents") {
                return Ok(Format::Chrome);
            }
            // PACE's own writer emits Chrome-compatible events too, so the
            // presence of PACE's attribution fields is the reliable signal.
            if head.contains("on_cpu_ns") || head.contains("preempted_ns") {
                return Ok(Format::Pace);
            }
            return Ok(Format::Chrome);
        }
        if head.contains("wait time") && head.contains("run time") {
            return Ok(Format::PerfSched);
        }
        Ok(Format::PerfTrace)
    }
}

/// Parse a trace file in the given format, keeping every sample.
///
/// Use this when the individual samples are wanted: `inspect` prints
/// them, and `convert` rewrites them. The memory is proportional to the
/// number of samples.
pub fn parse_file(path: &Path, format: Format) -> crate::Result<Trace> {
    parse_with(path, format, true)
}

/// Parse a trace file in the given format, keeping only the aggregates.
///
/// The file is read line by line and each sample is folded into its
/// thread's aggregate and then dropped, so the memory is proportional
/// to the number of threads rather than the number of samples. A trace
/// of ten million samples is a gigabyte of `Sample` structs; this needs
/// kilobytes.
///
/// The numbers are identical to those from [`parse_file`], because the
/// fold is the same one the batch aggregator uses.
pub fn parse_summarised(path: &Path, format: Format) -> crate::Result<Trace> {
    parse_with(path, format, false)
}

fn parse_with(path: &Path, format: Format, retain_samples: bool) -> crate::Result<Trace> {
    match format {
        Format::Pace => parse_pace_jsonl(path, retain_samples),
        Format::Chrome => parse_chrome_trace(path, retain_samples),
        Format::PerfSched => parse_perf_sched(path, retain_samples),
        Format::PerfTrace => parse_perf_trace(path, retain_samples),
    }
}

/// Parse PACE's own JSONL trace format.
///
/// Each line is a JSON object with the SDK's sample fields, either at the top
/// level (the SDK's writer) or nested under `args` (the Chrome-compatible
/// shape).
///
/// A line that cannot be read is counted and skipped rather than failing the
/// whole parse: a trace captured under stress is routinely truncated at the
/// end, and the readable part is still worth analysing. The count lands in
/// [`Trace::malformed_lines`].
pub fn parse_pace_jsonl(path: &Path, retain_samples: bool) -> crate::Result<Trace> {
    let file = File::open(path)?;
    let reader = BufReader::new(file);
    let mut trace = Trace {
        retain_samples,
        ..Default::default()
    };
    let mut index: HashMap<u64, usize> = HashMap::new();

    for line in reader.lines() {
        let line = match line {
            Ok(line) => line,
            // An I/O error mid-file (a truncated read) ends the trace; the
            // part already read is still valid.
            Err(_) => break,
        };
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        let value: serde_json::Value = match serde_json::from_str(trimmed) {
            Ok(value) => value,
            Err(_) => {
                trace.malformed_lines += 1;
                continue;
            }
        };

        let args = value.get("args").unwrap_or(&value);
        let tid = args
            .get("tid")
            .or_else(|| value.get("tid"))
            .and_then(|v| v.as_u64())
            .unwrap_or(0);
        let mut sample: Sample = match serde_json::from_value(args.clone()) {
            Ok(sample) => sample,
            Err(_) => {
                trace.malformed_lines += 1;
                continue;
            }
        };
        // The PACE format always carries a real preemption/blocking split.
        // A trace recorded by an older SDK will not have the provenance flags,
        // so set them here rather than reporting the split as unavailable.
        if !sample.has_attribution() {
            sample.flags |= crate::model::FLAG_HAS_PREEMPTED | crate::model::FLAG_HAS_BLOCKED;
        }

        push_sample(&mut trace, &mut index, tid, sample);
    }

    Ok(trace)
}

/// Parse a Chrome Trace Event JSON file.
///
/// Chrome trace events carry the thread state in the event name and the
/// duration in `dur` (microseconds). PACE-specific attribution is read from
/// `args` when present.
pub fn parse_chrome_trace(path: &Path, retain_samples: bool) -> crate::Result<Trace> {
    let text = std::fs::read_to_string(path)?;
    let value: serde_json::Value = serde_json::from_str(&text)
        .map_err(|e| crate::Error::Parse(format!("invalid JSON: {}", e)))?;

    let events = match &value {
        serde_json::Value::Array(items) => items.clone(),
        serde_json::Value::Object(map) => match map.get("traceEvents") {
            Some(serde_json::Value::Array(items)) => items.clone(),
            _ => {
                return Err(crate::Error::Parse(
                    "JSON object has no 'traceEvents' array".to_string(),
                ))
            }
        },
        _ => {
            return Err(crate::Error::Parse(
                "expected a JSON array or an object with 'traceEvents'".to_string(),
            ))
        }
    };

    let mut trace = Trace {
        retain_samples,
        ..Default::default()
    };
    let mut index: HashMap<u64, usize> = HashMap::new();

    for event in events {
        let phase = event.get("ph").and_then(|v| v.as_str()).unwrap_or("");
        // "X" is a complete event with a duration; "i"/"I" are instant events.
        if phase != "X" {
            continue;
        }
        let tid = event.get("tid").and_then(|v| v.as_u64()).unwrap_or(0);
        let dur_us = event.get("dur").and_then(|v| v.as_f64()).unwrap_or(0.0);
        let wall_ns = (dur_us * 1000.0) as u64;

        let args = event
            .get("args")
            .cloned()
            .unwrap_or(serde_json::Value::Null);
        let mut sample: Sample = serde_json::from_value(args.clone()).unwrap_or_default();
        if sample.wall_ns == 0 {
            sample.wall_ns = wall_ns;
        }
        if sample.on_cpu_ns == 0 && sample.preempted_ns == 0 && sample.blocked_ns == 0 {
            // A plain Chrome event carries no attribution; treat the whole
            // window as on-CPU rather than inventing a split, and mark it so
            // the report does not claim a measured "0 % preempted".
            sample.on_cpu_ns = wall_ns;
            sample.flags &= !(crate::model::FLAG_HAS_PREEMPTED | crate::model::FLAG_HAS_BLOCKED);
        } else if !sample.has_attribution() {
            // The event carried explicit attribution fields but not the
            // provenance flags; treat the fields as real.
            sample.flags |= crate::model::FLAG_HAS_PREEMPTED | crate::model::FLAG_HAS_BLOCKED;
        }

        push_sample(&mut trace, &mut index, tid, sample);
    }

    Ok(trace)
}

/// Parse `perf sched timehist` output.
///
/// The output is a fixed-column table:
///
/// ```text
///            time    cpu  task name                       wait time  sch delay   run time
///                         [tid/pid]                          (msec)     (msec)     (msec)
///    18209.317747 [0000]  perf[3036051]                       0.000      0.000      0.000
/// ```
///
/// The `wait time` column is the run-queue wait — the time the task spent
/// runnable but not running. `sch delay` is the time spent waiting for a CPU
/// to become available, and `run time` is the time it was scheduled in.
///
/// `wait time` and `sch delay` overlap in perf's accounting, so the analyzer
/// uses `sch delay` as the preemption bucket and `run time` as the on-CPU
/// bucket, and takes the blocked time as the remainder of the wall window.
pub fn parse_perf_sched(path: &Path, retain_samples: bool) -> crate::Result<Trace> {
    let file = File::open(path)?;
    let reader = BufReader::new(file);
    let mut trace = Trace {
        retain_samples,
        ..Default::default()
    };
    let mut index: HashMap<u64, usize> = HashMap::new();
    let mut previous_end_ns: HashMap<u64, u64> = HashMap::new();

    for line in reader.lines() {
        let line = line?;
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        // Skip the header lines.
        if trimmed.starts_with("time")
            || trimmed.starts_with("---")
            || trimmed.starts_with("Samples")
        {
            continue;
        }
        if !trimmed.starts_with(|c: char| c.is_ascii_digit()) {
            continue;
        }

        let columns: Vec<&str> = trimmed.split_whitespace().collect();
        if columns.len() < 7 {
            continue;
        }
        // columns: time [cpu] name[tid/pid] wait sch_delay run_time
        let time_us: f64 = match columns[0].parse() {
            Ok(v) => v,
            Err(_) => continue,
        };
        let task_column = columns[2];
        let open = match task_column.find('[') {
            Some(pos) => pos,
            None => continue,
        };
        let tid: u64 = match task_column[open + 1..]
            .split(|c: char| !c.is_ascii_digit())
            .next()
            .and_then(|s| s.parse().ok())
        {
            Some(v) => v,
            None => continue,
        };
        let wait_ms: f64 = columns[3].parse().unwrap_or(0.0);
        let sch_delay_ms: f64 = columns[4].parse().unwrap_or(0.0);
        let run_ms: f64 = columns[5].parse().unwrap_or(0.0);

        let on_cpu_ns = (run_ms * 1e6) as u64;
        let preempted_ns = (sch_delay_ms * 1e6) as u64;
        let blocked_ns = (wait_ms * 1e6) as u64;

        // The wall window is the interval this task held the CPU plus the
        // time it was off it. perf's timestamps are microseconds.
        let ts_ns = (time_us * 1000.0) as u64;
        let wall_ns = on_cpu_ns + preempted_ns + blocked_ns;

        let previous = previous_end_ns.insert(tid, ts_ns + on_cpu_ns);
        let _ = previous;

        let sample = Sample {
            wall_ns,
            on_cpu_ns,
            preempted_ns,
            blocked_ns,
            core_cycles: 0,
            ref_cycles: 0,
            core_per_ref_ratio: 0.0,
            involuntary_switches: 0,
            voluntary_switches: 0,
            source: Source::Unknown,
            flags: 0,
            phase: None,
        };

        push_sample(&mut trace, &mut index, tid, sample);
    }

    Ok(trace)
}

/// Parse `perf trace` CSV output.
///
/// The CSV has a header row and columns for timestamp, CPU, event, symbol
/// and arguments. Thread context-switch events (`sched_switch`) are the ones
/// that carry scheduler information.
pub fn parse_perf_trace(path: &Path, retain_samples: bool) -> crate::Result<Trace> {
    let file = File::open(path)?;
    let reader = BufReader::new(file);
    let mut trace = Trace {
        retain_samples,
        ..Default::default()
    };
    let mut index: HashMap<u64, usize> = HashMap::default();
    // When each thread last stopped running, so the next switch can close
    // its on-CPU interval.
    let mut ran_until_ns: HashMap<u64, u64> = HashMap::default();
    // When each thread was first seen, so the interval of a thread that was
    // already running when the trace began can be measured from there.
    let mut first_seen_ns: HashMap<u64, u64> = HashMap::default();

    for line in reader.lines() {
        let line = line?;
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }

        // A real `perf trace` line looks like:
        //
        //   0.469 sleep/4158694 sched:sched_switch(prev_comm: "sleep",
        //     prev_pid: 4158694 (sleep), prev_prio: 120, prev_state: 256,
        //     next_comm: "migration/13", next_pid: 60 (migration/13))
        //
        // The timestamp is seconds, relative to the first event, and is
        // right-aligned in a fixed-width column. The comm/pid field is
        // "<comm>/<pid>", where the comm may itself contain a slash.
        let (timestamp_s, rest) = match trimmed.split_once(char::is_whitespace) {
            Some((t, r)) => (t, r.trim_start()),
            None => continue,
        };
        // The perf trace time column is in MILLISECONDS relative to the
        // first event. Multiplying it by a second's worth of nanoseconds
        // inflates every interval by a factor of a thousand.
        let timestamp_ns: f64 = match timestamp_s.parse::<f64>() {
            Ok(v) => v * 1_000_000.0,
            Err(_) => continue,
        };

        // The event name and its arguments. The event name is the last
        // whitespace-separated token before the '(': the field before it is
        // the "<comm>/<pid>" of the thread that generated the event.
        let Some(open) = rest.find('(') else { continue };
        let head = &rest[..open];
        let event = head.rsplit(char::is_whitespace).next().unwrap_or("");
        let args = &rest[open..];

        if !event.contains("sched_switch") {
            // Only the scheduler events carry the attribution.
            continue;
        }

        let prev = perf_trace_arg(args, "prev_pid");
        let next = perf_trace_arg(args, "next_pid");
        let (Some(prev_pid), Some(next_pid)) = (prev, next) else {
            continue;
        };
        let ts = timestamp_ns as u64;

        // A sched_switch at time T says: prev was running until T, and next
        // runs from T. So the interval of prev is closed here, and the
        // interval of next opens here.
        //
        // The interval of a thread that was already running when the trace
        // began is unknown, so it is measured from the first switch that
        // mentions the thread. That is the earliest the trace can say
        // anything about it, and it is the only honest choice.
        let started = ran_until_ns
            .remove(&prev_pid)
            .unwrap_or_else(|| first_seen_ns.get(&prev_pid).copied().unwrap_or(ts));

        if ts > started {
            push_sample(
                &mut trace,
                &mut index,
                prev_pid,
                Sample {
                    wall_ns: ts - started,
                    on_cpu_ns: ts - started,
                    preempted_ns: 0,
                    blocked_ns: 0,
                    core_cycles: 0,
                    ref_cycles: 0,
                    core_per_ref_ratio: 0.0,
                    involuntary_switches: 0,
                    voluntary_switches: 1,
                    source: Source::Unknown,
                    // A sched_switch shows a voluntary or involuntary
                    // switch, but it does not carry the on-CPU versus
                    // blocked split, so the sample does not claim it.
                    flags: 0,
                    phase: None,
                },
            );
        }

        // The thread that starts now begins its next on-CPU interval.
        ran_until_ns.insert(next_pid, ts);
        // The thread that just stopped has been seen, so if it runs again
        // its next interval is measured from the switch that starts it.
        first_seen_ns.entry(prev_pid).or_insert(ts);
        first_seen_ns.entry(next_pid).or_insert(ts);
    }

    Ok(trace)
}

/// Pull one numeric argument out of a perf trace event's argument list.
///
/// The arguments look like `prev_pid: 4158694 (sleep)`, so the value is the
/// number after the colon.
fn perf_trace_arg(args: &str, name: &str) -> Option<u64> {
    let needle = format!("{name}: ");
    let start = args.find(&needle)? + needle.len();
    let rest = &args[start..];
    let end = rest
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(rest.len());
    rest[..end].parse::<u64>().ok()
}

/// Append a sample to a thread, creating the thread if it is new.
fn push_sample(trace: &mut Trace, index: &mut HashMap<u64, usize>, tid: u64, sample: Sample) {
    match index.get(&tid) {
        Some(&position) => {
            let thread = &mut trace.threads[position];
            if trace.retain_samples {
                thread.samples.push(sample);
            } else {
                let summary = thread.summary.get_or_insert_with(|| ThreadAttribution {
                    tid: thread.tid,
                    name: thread.name.clone(),
                    ..Default::default()
                });
                summary.fold(&sample);
                fold_verification(thread, &sample);
            }
        }
        None => {
            index.insert(tid, trace.threads.len());
            let mut thread = ThreadTrace {
                tid,
                name: None,
                samples: Vec::new(),
                summary: None,
                summary_verify: None,
            };
            if trace.retain_samples {
                thread.samples.push(sample);
            } else {
                let mut summary = ThreadAttribution {
                    tid,
                    name: None,
                    ..Default::default()
                };
                summary.fold(&sample);
                thread.summary = Some(summary);
                fold_verification(&mut thread, &sample);
            }
            trace.threads.push(thread);
        }
    }
}

/// Fold one sample's decomposition check into a thread's verification.
///
/// A summarised trace checks the invariant as it reads, so the report
/// can say whether the decomposition held. It must not say "exact" for a
/// trace it never checked.
fn fold_verification(thread: &mut ThreadTrace, sample: &Sample) {
    let report = thread
        .summary_verify
        .get_or_insert_with(VerifyReport::default);
    report.total_samples += 1;
    let error = sample.balance_error_ns();
    if error > 0 {
        report.violations += 1;
    }
    if error > report.worst_error_ns {
        report.worst_error_ns = error;
        report.worst = Some((thread.tid, report.total_samples - 1));
    }
}
