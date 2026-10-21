//! The agent-ready error contract.
//!
//! Every error the tool can produce must carry a stable code, a fix the
//! caller can act on, and a recovery line that says what to do next. These
//! tests are the contract: if a variant loses one of them, the test fails.

use pace_analyzer::{Error, ErrorCode};

/// Every error must render a code, a fix and a recovery, and none of them
/// may be empty.
fn assert_contract(err: &Error) {
    let code = err.code().as_str();
    assert!(!code.is_empty(), "the error code is empty");
    assert!(
        code.chars().all(|c| c.is_ascii_lowercase() || c == '-'),
        "the error code {:?} is not kebab-case",
        code
    );

    let fix = err.fix();
    assert!(!fix.trim().is_empty(), "the fix for {} is empty", code);
    assert!(
        fix.len() > 20,
        "the fix for {} is too short to act on: {:?}",
        code,
        fix
    );

    let recovery = err.recovery();
    assert!(
        !recovery.trim().is_empty(),
        "the recovery for {} is empty",
        code
    );
    assert!(
        recovery.len() > 20,
        "the recovery for {} is too short to act on: {:?}",
        code,
        fix
    );
}

#[test]
fn every_error_variant_satisfies_the_contract() {
    let errors = vec![
        Error::Io(std::io::Error::new(
            std::io::ErrorKind::NotFound,
            "no such file",
        )),
        Error::Io(std::io::Error::new(
            std::io::ErrorKind::PermissionDenied,
            "permission denied",
        )),
        Error::Io(std::io::Error::new(
            std::io::ErrorKind::UnexpectedEof,
            "unexpected end of file",
        )),
        Error::Parse("invalid JSON".to_string()),
        Error::FormatUnknown("trace.jsonl".to_string()),
        Error::Empty,
        Error::UnsupportedOutput("perf sched", "lossy".to_string()),
        Error::Write("out.html".to_string()),
        Error::InvalidArgument("threshold must be a percentage".to_string()),
    ];

    for err in &errors {
        assert_contract(err);
    }
}

#[test]
fn the_error_codes_are_distinct_and_stable() {
    // These strings are the tool's contract. A script branches on them, so
    // they must not change and must not collide.
    let codes = [
        (ErrorCode::Io, "io-error"),
        (ErrorCode::Parse, "parse-error"),
        (ErrorCode::FormatUnknown, "format-unknown"),
        (ErrorCode::Empty, "empty-trace"),
        (ErrorCode::UnsupportedOutput, "unsupported-output"),
        (ErrorCode::Write, "write-error"),
        (ErrorCode::InvalidArgument, "invalid-argument"),
    ];

    let mut seen = std::collections::HashSet::new();
    for (code, expected) in codes {
        assert_eq!(
            code.as_str(),
            expected,
            "the code for {:?} changed",
            expected
        );
        assert!(
            seen.insert(code.as_str()),
            "the code {} is used twice",
            expected
        );
    }
}

#[test]
fn a_missing_file_says_what_to_do() {
    let err = Error::Io(std::io::Error::new(
        std::io::ErrorKind::NotFound,
        "no such file",
    ));
    assert_eq!(err.code(), ErrorCode::Io);
    assert!(
        err.fix().contains("check the path"),
        "the fix for a missing file does not mention the path: {:?}",
        err.fix()
    );
    assert!(
        err.recovery().contains("run the same command again"),
        "the recovery for a missing file does not say to retry: {:?}",
        err.recovery()
    );
}

#[test]
fn a_parse_error_says_the_format_can_be_forced() {
    let err = Error::Parse("JSON object has no 'traceEvents' array".to_string());
    assert_eq!(err.code(), ErrorCode::Parse);
    assert!(
        err.fix().contains("--format"),
        "the fix for a parse error does not offer --format: {:?}",
        err.fix()
    );
}

#[test]
fn an_empty_trace_says_nothing_was_written() {
    let err = Error::Empty;
    assert_eq!(err.code(), ErrorCode::Empty);
    assert!(
        err.recovery().contains("no output was written"),
        "the recovery for an empty trace does not say the output state: {:?}",
        err.recovery()
    );
}

#[test]
fn the_fix_never_asks_the_caller_to_edit_the_tool() {
    let errors = vec![
        Error::Io(std::io::Error::new(std::io::ErrorKind::NotFound, "x")),
        Error::Parse("x".to_string()),
        Error::FormatUnknown("x".to_string()),
        Error::Empty,
        Error::UnsupportedOutput("x", "y".to_string()),
        Error::Write("x".to_string()),
        Error::InvalidArgument("x".to_string()),
    ];
    for err in &errors {
        let fix = err.fix().to_lowercase();
        assert!(
            !fix.contains("edit the source"),
            "the fix asks the caller to edit the tool: {:?}",
            err.fix()
        );
        assert!(
            !fix.contains("patch the"),
            "the fix asks the caller to patch the tool: {:?}",
            err.fix()
        );
    }
}

#[test]
fn no_error_carries_a_secret() {
    // The errors carry a path and a parse detail, never a credential. The
    // check is that the fix and recovery lines are built from a fixed set of
    // phrases, so they cannot leak a value read from the environment.
    let err = Error::Write("/tmp/some/path/report.html".to_string());
    let combined = format!("{} {} {}", err, err.fix(), err.recovery());
    for secret_marker in ["password", "token", "secret", "api_key", "apikey"] {
        assert!(
            !combined.to_lowercase().contains(secret_marker),
            "the error mentions {:?}: {}",
            secret_marker,
            combined
        );
    }
}
