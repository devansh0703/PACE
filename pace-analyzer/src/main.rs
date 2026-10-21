// SPDX-License-Identifier: Apache-2.0
//! The `pace` command-line tool.

use clap::{Parser, Subcommand};
use pace_analyzer::parsers::{self, Format};
use pace_analyzer::{
    analyze, render_html, render_terminal, Error as PaceError, Result, Thresholds, Trace,
};
use std::path::{Path, PathBuf};

#[derive(Parser)]
#[command(
    name = "pace",
    version,
    about = "Attribution analyzer for PACE preemption-aware cycle traces",
    long_about = "Reads a PACE trace (or a foreign trace it can translate) and reports, per \
thread, how much wall time was spent on CPU, lost to OS preemption, and blocked. Preemption \
is the time a thread was runnable but not scheduled: the time that makes a slow frame look like \
a code regression when it is not."
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Analyse a trace and report the attribution.
    Analyze {
        /// The trace file.
        trace: PathBuf,
        /// The trace format. Detected from the file when omitted.
        #[arg(long, short, value_enum)]
        format: Option<Format>,
        /// Write a self-contained HTML report to this path.
        #[arg(long, short)]
        output: Option<PathBuf>,
        /// Emit the analysis as JSON on stdout.
        #[arg(long)]
        json: bool,
        /// Warn when a thread loses more than this fraction of its wall time to
        /// preemption. Accepts a percentage, e.g. 25 for 25%.
        #[arg(long, default_value = "25")]
        preempted_threshold: f64,
        /// Warn when a thread spends more than this fraction of its wall time
        /// blocked. Accepts a percentage.
        #[arg(long, default_value = "25")]
        blocked_threshold: f64,
    },
    /// Print the raw per-thread sample table.
    Inspect {
        /// The trace file.
        trace: PathBuf,
        /// The trace format. Detected from the file when omitted.
        #[arg(long, short, value_enum)]
        format: Option<Format>,
    },
    /// Check the decomposition invariant across every sample.
    Verify {
        /// The trace file.
        trace: PathBuf,
        /// The trace format. Detected from the file when omitted.
        #[arg(long, short, value_enum)]
        format: Option<Format>,
    },
    /// Convert a trace between formats.
    Convert {
        /// The input trace file.
        input: PathBuf,
        /// The output format.
        #[arg(long, short, value_enum)]
        to: Format,
        /// The output file. Defaults to stdout.
        #[arg(long, short)]
        output: Option<PathBuf>,
        /// The input format. Detected from the file when omitted.
        #[arg(long, short, value_enum)]
        format: Option<Format>,
    },
}

/// Load a trace file.
///
/// `retain_samples` is false for the commands that only need the
/// aggregate. A trace of ten million samples is a gigabyte of sample
/// structs, and the analysis needs only the sums, so the summarised
/// path reads the file line by line and drops each sample after folding
/// it in.
fn load(path: &Path, format: Option<Format>, retain_samples: bool) -> Result<Trace> {
    // The file must be readable before anything else is attempted, so a
    // missing or unreadable file is reported as what it is rather than as a
    // format problem.
    if !path.exists() {
        return Err(PaceError::Io(std::io::Error::new(
            std::io::ErrorKind::NotFound,
            format!("{}: no such file", path.display()),
        )));
    }
    let format = match format {
        Some(f) => f,
        None => parsers::Format::detect(path)
            .map_err(|_| PaceError::FormatUnknown(path.display().to_string()))?,
    };
    let parse = if retain_samples {
        parsers::parse_file(path, format)
    } else {
        parsers::parse_summarised(path, format)
    };
    let trace = parse.map_err(|e| match e {
        PaceError::Io(io) => PaceError::Io(io),
        PaceError::Parse(detail) => PaceError::Parse(detail),
        other => PaceError::Parse(other.to_string()),
    })?;
    // A trace with no samples at all is empty, whichever way it was read.
    let empty = trace
        .threads
        .iter()
        .all(|t| t.samples.is_empty() && t.summary.as_ref().is_none_or(|s| s.sample_count == 0));
    if empty {
        return Err(PaceError::Empty);
    }
    Ok(trace)
}

/// Print an error in the agent-ready contract, and return the exit code.
///
/// The format is stable and machine-readable:
///
/// ```text
/// pace: error [code]: <what happened>
///   fix: <the safe action to take>
///   recovery: <what to do next>
/// ```
///
/// A script can branch on the code in brackets. A person can act on the fix
/// without reading the source.
fn report_error(err: &pace_analyzer::Error) -> i32 {
    eprintln!("pace: error [{}]: {}", err.code(), err);
    eprintln!("  fix: {}", err.fix());
    eprintln!("  recovery: {}", err.recovery());
    1
}

fn main() {
    let cli = Cli::parse();

    if let Err(err) = run(cli) {
        std::process::exit(report_error(&err));
    }
}

fn run(cli: Cli) -> Result<()> {
    match cli.command {
        Command::Analyze {
            trace,
            format,
            output,
            json,
            preempted_threshold,
            blocked_threshold,
        } => {
            // Only the aggregate is needed, so the samples are folded and
            // dropped as the file is read.
            let trace = load(&trace, format, false)?;
            let thresholds = Thresholds {
                preempted_fraction: preempted_threshold / 100.0,
                blocked_fraction: blocked_threshold / 100.0,
                healthy_on_cpu_fraction: 0.0,
            };
            let analysis = analyze(&trace, &thresholds);

            if json {
                let rendered = serde_json::to_string_pretty(&analysis).map_err(|e| {
                    PaceError::InvalidArgument(format!("cannot render the analysis as JSON: {}", e))
                })?;
                println!("{}", rendered);
            } else {
                print!("{}", render_terminal(&analysis, &trace));
            }

            if let Some(output) = output {
                let title = trace
                    .trace_title()
                    .unwrap_or_else(|| "PACE trace".to_string());
                let html = render_html(&analysis, &trace, &title);
                std::fs::write(&output, html)
                    .map_err(|_| PaceError::Write(output.display().to_string()))?;
                if !json {
                    eprintln!("\nHTML report written to {}", output.display());
                }
            }
        }
        Command::Inspect { trace, format } => {
            // Every sample is printed, so they are all retained.
            let trace = load(&trace, format, true)?;
            for thread in &trace.threads {
                println!("thread {} ({} samples)", thread.tid, thread.samples.len());
                for (index, sample) in thread.samples.iter().enumerate() {
                    println!(
                        "  #{:<4} wall={:>12} on_cpu={:>12} preempted={:>12} blocked={:>12} balance={}",
                        index,
                        sample.wall_ns,
                        sample.on_cpu_ns,
                        sample.preempted_ns,
                        sample.blocked_ns,
                        if sample.is_balanced() { "ok" } else { "OFF" }
                    );
                }
            }
        }
        Command::Verify { trace, format } => {
            // The samples are kept: the invariant is checked on every one
            // of them, and a summarised trace has none to check. Summing
            // the check would report zero violations on every trace,
            // which is a false green in a CI gate.
            let trace = load(&trace, format, true)?;
            let report = trace.verify();
            // An empty trace satisfies the invariant vacuously. Reporting
            // that as a pass would be a false green in a CI gate, so an empty
            // trace is a failure.
            if report.total_samples == 0 {
                println!("FAIL: the trace contains no samples, so nothing was verified");
                std::process::exit(1);
            }
            if report.is_ok() {
                println!(
                    "PASS: all {} samples satisfy wall == on_cpu + preempted + blocked",
                    report.total_samples
                );
                if trace.malformed_lines > 0 {
                    println!(
                        "NOTE: {} line(s) could not be read and were skipped.",
                        trace.malformed_lines
                    );
                }
                return Ok(());
            }
            println!(
                "FAIL: {} of {} samples violate the decomposition invariant; worst error {} ns",
                report.violations, report.total_samples, report.worst_error_ns
            );
            if let Some((tid, index)) = report.worst {
                println!("  worst: thread {}, sample #{}", tid, index);
            }
            std::process::exit(1);
        }
        Command::Convert {
            input,
            to,
            output,
            format,
        } => {
            // Every sample is rewritten, so they are all retained.
            let trace = load(&input, format, true)?;
            let text = match to {
                Format::Pace => pace_jsonl(&trace),
                Format::Chrome => chrome_json(&trace),
                Format::PerfSched => {
                    return Err(PaceError::UnsupportedOutput(
                        "perf sched",
                        "it is a lossy, one-way view of a trace".to_string(),
                    ))
                }
                Format::PerfTrace => {
                    return Err(PaceError::UnsupportedOutput(
                        "perf trace",
                        "it is a lossy, one-way view of a trace".to_string(),
                    ))
                }
            };
            match output {
                Some(path) => {
                    std::fs::write(&path, text)
                        .map_err(|_| PaceError::Write(path.display().to_string()))?;
                    eprintln!("wrote {}", path.display());
                }
                None => print!("{}", text),
            }
        }
    }

    Ok(())
}

/// Serialise a trace in PACE's own JSONL format.
fn pace_jsonl(trace: &Trace) -> String {
    let mut out = String::new();
    for thread in &trace.threads {
        for sample in &thread.samples {
            let mut value = serde_json::to_value(sample).unwrap_or_default();
            if let Some(object) = value.as_object_mut() {
                object.insert("tid".to_string(), serde_json::json!(thread.tid));
            }
            out.push_str(&serde_json::to_string(&value).unwrap_or_default());
            out.push('\n');
        }
    }
    out
}

/// Serialise a trace in the Chrome Trace Event format.
fn chrome_json(trace: &Trace) -> String {
    let mut events: Vec<serde_json::Value> = Vec::new();
    for thread in &trace.threads {
        let mut timestamp_us = 0.0f64;
        for sample in &thread.samples {
            let duration_us = sample.wall_ns as f64 / 1000.0;
            events.push(serde_json::json!({
                "name": "pace.sample",
                "ph": "X",
                "pid": 1,
                "tid": thread.tid,
                "ts": timestamp_us,
                "dur": duration_us,
                "cat": "pace",
                "args": sample,
            }));
            timestamp_us += duration_us;
        }
    }
    serde_json::to_string_pretty(&serde_json::json!({ "traceEvents": events })).unwrap_or_default()
}
