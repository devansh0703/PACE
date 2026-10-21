//! PACE analyzer — turn a trace into a per-thread time attribution.
//!
//! The analyzer answers one question: for each thread, how much of its wall
//! time was spent actually running, how much was lost to OS preemption, and
//! how much was blocked. That split is what lets a performance engineer tell
//! a code regression from a scheduler problem.
//!
//! The invariant the whole product rests on:
//!
//! ```text
//! wall == on_cpu + preempted + blocked
//! ```
//!
//! [`Trace::verify`] checks it.

pub mod model;
pub mod parsers;
pub mod report;

pub use model::{
    analyze, fraction, Analysis, Finding, PhaseAttribution, Sample, Severity, Source,
    ThreadAttribution, ThreadTrace, Thresholds, Trace, VerifyReport,
};
pub use parsers::Format;
pub use report::{escape_html, format_cycles, format_duration, render_html, render_terminal};

/// A stable, machine-readable error code.
///
/// The codes never change meaning. A script can branch on them, and the
/// documentation anchors on them, so a caller does not have to parse an
/// English sentence to find out what went wrong.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorCode {
    /// The trace file could not be opened or read.
    Io,
    /// The trace file could not be parsed in the detected or requested format.
    Parse,
    /// The format of the trace could not be determined.
    FormatUnknown,
    /// The trace parsed but carried nothing to analyse.
    Empty,
    /// The requested output format cannot be written.
    UnsupportedOutput,
    /// The output file could not be written.
    Write,
    /// A value on the command line was not usable.
    InvalidArgument,
}

impl ErrorCode {
    /// The stable, kebab-case identifier for this code.
    ///
    /// This string is part of the tool's contract: it appears in the error
    /// output and in the documentation, and it does not change.
    pub const fn as_str(self) -> &'static str {
        match self {
            ErrorCode::Io => "io-error",
            ErrorCode::Parse => "parse-error",
            ErrorCode::FormatUnknown => "format-unknown",
            ErrorCode::Empty => "empty-trace",
            ErrorCode::UnsupportedOutput => "unsupported-output",
            ErrorCode::Write => "write-error",
            ErrorCode::InvalidArgument => "invalid-argument",
        }
    }
}

impl std::fmt::Display for ErrorCode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// The analyzer's error type.
///
/// Every variant carries a stable [`ErrorCode`], a `fix` that a caller can
/// act on without reading the source, and a `recovery` line that says what
/// to do next. This is the agent-ready error contract: a machine can branch
/// on the code, and a person can act on the fix.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// The trace file could not be read.
    #[error("i/o error: {0}")]
    Io(#[from] std::io::Error),

    /// The trace file could not be parsed.
    #[error("parse error: {0}")]
    Parse(String),

    /// The format of the trace could not be determined.
    #[error("cannot determine the format of {0}")]
    FormatUnknown(String),

    /// The trace was valid but carried nothing to analyse.
    #[error("the trace contains no samples")]
    Empty,

    /// The requested output format cannot be written.
    #[error("writing {0} output is not supported: {1}")]
    UnsupportedOutput(&'static str, String),

    /// The output file could not be written.
    #[error("cannot write {0}")]
    Write(String),

    /// A value on the command line was not usable.
    #[error("{0}")]
    InvalidArgument(String),
}

impl Error {
    /// The stable code for this error.
    pub fn code(&self) -> ErrorCode {
        match self {
            Error::Io(_) => ErrorCode::Io,
            Error::Parse(_) => ErrorCode::Parse,
            Error::FormatUnknown(_) => ErrorCode::FormatUnknown,
            Error::Empty => ErrorCode::Empty,
            Error::UnsupportedOutput(_, _) => ErrorCode::UnsupportedOutput,
            Error::Write(_) => ErrorCode::Write,
            Error::InvalidArgument(_) => ErrorCode::InvalidArgument,
        }
    }

    /// What the caller can do about it, in one line.
    ///
    /// The fix is a safe, authorised action: a command to run, a flag to
    /// pass, or the exact thing a human has to supply. It never asks the
    /// caller to edit the tool or to disable a check.
    pub fn fix(&self) -> String {
        match self {
            Error::Io(e) => match e.kind() {
                std::io::ErrorKind::NotFound => {
                    "check the path, then pass the trace file: pace analyze <trace>".to_string()
                }
                std::io::ErrorKind::PermissionDenied => {
                    "the file may be readable only by its owner: run with the right user, or copy \
                     the trace to a readable location"
                        .to_string()
                }
                _ => "check that the path is a readable file, then retry".to_string(),
            },
            Error::Parse(_) => {
                "the file is not in the format that was detected: pass the format explicitly with \
                 --format pace|chrome|perf-sched|perf-trace"
                    .to_string()
            }
            Error::FormatUnknown(path) => {
                format!(
                    "pass the format explicitly: pace analyze {path} --format pace|chrome|perf-sched|perf-trace"
                )
            }
            Error::Empty => {
                "the trace has no usable samples: record one with the SDK's pace_record example, or \
                 check that the file is not truncated"
                    .to_string()
            }
            Error::UnsupportedOutput(_, _) => {
                "choose an output format that can be written: --to pace or --to chrome".to_string()
            }
            Error::Write(path) => {
                format!("check that {path} is writable and that its directory exists, then retry")
            }
            Error::InvalidArgument(_) => {
                "check the value against the command's --help output, then retry".to_string()
            }
        }
    }

    /// What to do next, in one line.
    ///
    /// Every error here is safe to retry after the stated action, and none of
    /// them has a side effect to reconcile: the tool only reads the trace and
    /// writes the output it was asked for.
    pub fn recovery(&self) -> String {
        match self {
            Error::Io(_) | Error::Write(_) => {
                "fix the cause, then run the same command again".to_string()
            }
            Error::Parse(_) | Error::FormatUnknown(_) => {
                "the input was read but not understood: no output was written; correct the format \
                 and run the same command again"
                    .to_string()
            }
            Error::Empty => {
                "the input was read but held nothing to analyse: no output was written; supply a \
                 trace with samples and run the same command again"
                    .to_string()
            }
            Error::UnsupportedOutput(_, _) => {
                "the input was read but the requested output cannot be produced: no output was \
                 written; choose a supported format and run the same command again"
                    .to_string()
            }
            Error::InvalidArgument(_) => {
                "nothing was read and nothing was written; correct the argument and run the same \
                 command again"
                    .to_string()
            }
        }
    }
}

/// A convenient result alias.
pub type Result<T> = std::result::Result<T, Error>;
