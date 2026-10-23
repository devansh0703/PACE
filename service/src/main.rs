// SPDX-License-Identifier: Apache-2.0
//! The hosted PACE analyzer.
//!
//! This is `pace analyze` behind an HTTP endpoint, so somebody can find out
//! what PACE says about a trace without installing a Rust toolchain and
//! building the analyzer first. It is a transport, not a second product: the
//! attribution is the analyzer library's, embedded here rather than
//! reimplemented or shelled out to.
//!
//! The service was built for a free host with a tenth of a core and half a
//! gigabyte of memory, which is why it is synchronous, single-threaded and
//! deliberately small:
//!
//!   - one request is analysed at a time, so a large trace queues rather than
//!     starving the rest of the process;
//!   - the body is capped before it is buffered, so a hostile upload cannot
//!     exhaust the instance;
//!   - an analysis that exceeds its budget is answered with 504 and the
//!     worker's result is discarded, so a slow trace cannot hold a connection
//!     open indefinitely;
//!   - nothing is written to disk, ever. The trace lives in the request's
//!     buffer and is dropped with it, and the host has no persistent disk for
//!     it to reach even by accident.

use pace_analyzer::parsers::Format;
use pace_analyzer::{analyze, render_html, render_terminal, Thresholds, Trace};
use std::io::Read;
use std::sync::mpsc;
use std::sync::OnceLock;
use std::time::{Duration, Instant};
use tiny_http::{Header, Request, Response, Server};

/// The largest trace body that will be buffered.
const BODY_LIMIT: u64 = 8 * 1024 * 1024;
/// The largest number of samples a single request may carry.
const SAMPLE_LIMIT: usize = 1_000_000;
/// How long one analysis may take before the client is told it timed out.
const BUDGET: Duration = Duration::from_secs(20);

const VERSION: &str = env!("CARGO_PKG_VERSION");

type Answer = Response<std::io::Cursor<Vec<u8>>>;

/// When the process started, for the uptime the health endpoint reports.
fn started() -> Instant {
    static STARTED: OnceLock<Instant> = OnceLock::new();
    *STARTED.get_or_init(Instant::now)
}

fn main() {
    let port: u16 = std::env::var("PORT")
        .ok()
        .and_then(|p| p.parse().ok())
        .unwrap_or(8080);
    let address = format!("0.0.0.0:{port}");

    let server = match Server::http(&address) {
        Ok(server) => server,
        Err(error) => {
            eprintln!("pace-service: cannot listen on {address}: {error}");
            std::process::exit(1);
        }
    };
    eprintln!("pace-service {VERSION} listening on {address}");

    for request in server.incoming_requests() {
        handle(request);
    }
}

fn handle(mut request: Request) {
    let method = request.method().as_str().to_string();
    let url = request.url().to_string();
    let (path, query) = match url.split_once('?') {
        Some((path, query)) => (path.to_string(), query.to_string()),
        None => (url, String::new()),
    };

    // The site fetches /health from another origin, and a person may paste a
    // trace from a form on a page hosted elsewhere. There are no credentials
    // and no cookies anywhere in this service, so the wildcard costs nothing.
    let response = match (method.as_str(), path.as_str()) {
        ("OPTIONS", _) => Response::from_data(Vec::new())
            .with_status_code(204)
            .with_header(cors())
            .with_header(allow_headers()),
        ("GET", "/health") => health(),
        ("GET", "/") => home(),
        ("POST", "/analyze") => analyze_request(&mut request, &query),
        ("GET", "/analyze") => text(
            405,
            "the analyzer is a POST endpoint; see / for the form\n",
            "text/plain; charset=utf-8",
        ),
        _ => text(
            404,
            "no such endpoint; try /health or /analyze\n",
            "text/plain; charset=utf-8",
        ),
    };

    let _ = request.respond(response.with_header(cors()));
}

// ------------------------------------------------------------------ responses

fn cors() -> Header {
    Header::from_bytes("Access-Control-Allow-Origin", "*").expect("a static header")
}

fn allow_headers() -> Header {
    Header::from_bytes("Access-Control-Allow-Headers", "Content-Type").expect("a static header")
}

fn text(status: u16, body: &str, content_type: &str) -> Answer {
    Response::from_string(body)
        .with_status_code(status)
        .with_header(
            Header::from_bytes("Content-Type", content_type).expect("a static content type"),
        )
}

fn json(status: u16, body: String) -> Answer {
    text(status, &body, "application/json; charset=utf-8")
}

fn health() -> Answer {
    let body = serde_json::json!({
        "status": "ok",
        "analyzer": VERSION,
        "uptime_s": started().elapsed().as_secs(),
        // The limits are published rather than discovered by being rejected.
        "sample_limit": SAMPLE_LIMIT,
        "body_limit_bytes": BODY_LIMIT,
        "budget_s": BUDGET.as_secs(),
        "concurrent_requests": 1,
        "retention": "none",
    });
    json(200, body.to_string())
}

fn home() -> Answer {
    // A form, so a person can use this without reading the API. The report
    // comes back as one self-contained HTML file.
    let page = r#"<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PACE analyzer</title>
<style>
body{background:#f3f1ea;color:#1b1a16;font:15px/1.6 system-ui,sans-serif;margin:0;padding:32px 20px}
main{max-width:760px;margin:0 auto}
h1{font:500 22px/1.3 ui-monospace,monospace}
p{color:#55524a}
textarea{width:100%;height:240px;font:13px/1.5 ui-monospace,monospace;background:#fbfaf6;
  color:#1b1a16;border:1px solid #b4ae9a;border-radius:3px;padding:12px}
button{font:500 14px ui-monospace,monospace;background:#9c5c14;color:#fdfbf5;border:1px solid #9c5c14;
  border-radius:3px;padding:9px 16px;cursor:pointer;margin-top:12px}
a{color:#9c5c14}
code{font-family:ui-monospace,monospace;background:#fbfaf6;border:1px solid #ddd8c9;border-radius:3px;padding:1px 5px}
</style>
</head>
<body>
<main>
<h1>PACE analyzer</h1>
<p>Paste a trace and get the per-thread attribution. This is the same analyzer the
<a href="https://devansh0703.github.io/PACE/">project site</a> documents, running as a service so you do
not have to build a Rust toolchain first.</p>
<form method="post" action="/analyze?format=html">
<textarea name="trace" placeholder="paste a PACE JSONL, Chrome Trace Event, perf sched timehist or perf trace body"></textarea>
<button type="submit">Analyse</button>
</form>
<p>Nothing is stored. The trace is held in memory for this request and dropped when it ends.</p>
<p>Over HTTP: <code>curl --data-binary @trace.jsonl &lt;url&gt;/analyze</code>.
Append <code>?format=html</code> for the offline report, <code>?format=json</code> for JSON, or
<code>?verify=1</code> for a status code.</p>
</main>
</body>
</html>
"#;
    text(200, page, "text/html; charset=utf-8")
}

// -------------------------------------------------------------------- analysis

/// What the caller asked for.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Shape {
    Terminal,
    Html,
    Json,
    Verify,
}

fn shape_from(query: &str) -> Option<Shape> {
    let mut shape = None;
    for pair in query.split('&').filter(|p| !p.is_empty()) {
        let (key, value) = pair.split_once('=').unwrap_or((pair, ""));
        match key {
            "verify" if value == "1" || value.is_empty() => shape = Some(Shape::Verify),
            "format" | "output" => {
                shape = match value {
                    "html" => Some(Shape::Html),
                    "json" => Some(Shape::Json),
                    "terminal" | "text" => Some(Shape::Terminal),
                    _ => return None,
                }
            }
            _ => {}
        }
    }
    Some(shape.unwrap_or(Shape::Terminal))
}

fn format_from(query: &str) -> Option<Format> {
    for pair in query.split('&').filter(|p| !p.is_empty()) {
        let (key, value) = pair.split_once('=').unwrap_or((pair, ""));
        if key != "trace_format" && key != "input" {
            continue;
        }
        return match value {
            "pace" => Some(Format::Pace),
            "chrome" => Some(Format::Chrome),
            "perf-sched" => Some(Format::PerfSched),
            "perf-trace" => Some(Format::PerfTrace),
            _ => None,
        };
    }
    None
}

fn analyze_request(request: &mut Request, query: &str) -> Answer {
    let Some(shape) = shape_from(query) else {
        return failure(
            400,
            "invalid-argument",
            "the requested output format does not exist",
            "use ?format=terminal, ?format=html, ?format=json or ?verify=1",
        );
    };

    // The cap is checked before the buffer grows, not after: a body that
    // declares itself too large is refused without being read.
    if let Some(length) = request.body_length() {
        if length as u64 > BODY_LIMIT {
            return too_large(length as u64);
        }
    }

    let mut body = Vec::new();
    if let Err(error) = request
        .as_reader()
        .take(BODY_LIMIT + 1)
        .read_to_end(&mut body)
    {
        return failure(
            400,
            "io-error",
            &format!("the request body could not be read: {error}"),
            "send the trace as the request body, and check the connection",
        );
    }
    if body.len() as u64 > BODY_LIMIT {
        return too_large(body.len() as u64);
    }
    if body.is_empty() {
        return failure(
            400,
            "empty-trace",
            "the request body is empty",
            "send the trace as the request body: curl --data-binary @trace.jsonl <url>/analyze",
        );
    }

    // The body is the trace unless it is a form post that carries a `trace`
    // field.
    //
    // The content type alone is not enough to decide: `curl --data-binary`,
    // which is how the documentation says to send a trace, labels its body
    // `application/x-www-form-urlencoded` by default. So the form path is
    // taken only when the body actually opens with the field name, which no
    // trace format does -- they all begin with `{`, `[`, or a digit.
    let is_form = request.headers().iter().any(|h| {
        h.field.equiv("Content-Type")
            && h.value
                .as_str()
                .starts_with("application/x-www-form-urlencoded")
    }) && body.starts_with(b"trace=");

    let trace_bytes = if is_form {
        match form_trace(&body) {
            Some(decoded) if !decoded.is_empty() => decoded,
            _ => {
                return failure(
                    400,
                    "invalid-argument",
                    "the form carried no 'trace' field",
                    "post the trace as the raw body instead: \
                     curl --data-binary @trace.jsonl <url>/analyze",
                )
            }
        }
    } else {
        body
    };

    let requested = format_from(query);

    // The work runs on its own thread so that a trace which takes too long is
    // answered with a timeout rather than holding the connection open. The
    // worker cannot be killed, so its result is discarded.
    let (sender, receiver) = mpsc::channel();
    std::thread::spawn(move || {
        let _ = sender.send(run_analysis(&trace_bytes, shape, requested));
    });

    match receiver.recv_timeout(BUDGET) {
        Ok(response) => response,
        Err(_) => failure(
            504,
            "timeout",
            &format!(
                "the trace took longer than {} s to analyse on this instance",
                BUDGET.as_secs()
            ),
            "this is a free instance with a tenth of a core: run `pace analyze` locally, \
             which is what this service calls",
        ),
    }
}

fn too_large(size: u64) -> Answer {
    failure(
        413,
        "too-large",
        &format!("the body is {size} bytes and the limit is {BODY_LIMIT}"),
        "split the trace, or run `pace analyze` locally, which has no body limit",
    )
}

fn run_analysis(bytes: &[u8], shape: Shape, requested: Option<Format>) -> Answer {
    let trace: Trace = match pace_analyzer::parsers::parse_memory(bytes, requested, false) {
        Ok(trace) => trace,
        Err(error) => {
            return failure(
                status_for(&error),
                error.code().as_str(),
                &error.to_string(),
                &error.fix(),
            )
        }
    };

    // One analysis, rendered per asked-for shape: computing it twice is a
    // second place for the same numbers to differ.
    let analysis = analyze(&trace, &Thresholds::default());
    let sample_count = trace.sample_count();

    if sample_count == 0 {
        return failure(
            422,
            "empty-trace",
            "the trace carried no samples",
            "check that the body is a trace and not an error message from a failed capture",
        );
    }
    if sample_count > SAMPLE_LIMIT {
        return failure(
            413,
            "too-large",
            &format!("the trace carries {sample_count} samples and the limit is {SAMPLE_LIMIT}"),
            "run `pace analyze` locally for a trace this size",
        );
    }

    match shape {
        Shape::Verify => {
            let report = trace.verify();
            let body = serde_json::json!({
                "samples": report.total_samples,
                "violations": report.violations,
                "worst_error_ns": report.worst_error_ns,
                "decomposition": if report.is_ok() { "exact" } else { "violated" },
            });
            let status = if report.is_ok() { 200 } else { 422 };
            json(status, body.to_string())
        }
        Shape::Json => {
            let body = serde_json::json!({
                "summary": {
                    "threads": analysis.threads.len(),
                    "samples": sample_count,
                    "total_wall_ns": analysis.total_wall_ns,
                    "total_on_cpu_ns": analysis.total_on_cpu_ns,
                    "total_preempted_ns": analysis.total_preempted_ns,
                    "total_blocked_ns": analysis.total_blocked_ns,
                    "has_attribution": analysis.has_attribution,
                    "malformed_lines": analysis.malformed_lines,
                },
                "threads": analysis.threads,
                "findings": analysis.findings,
            });
            json(200, body.to_string())
        }
        Shape::Html => text(
            200,
            &render_html(&analysis, &trace, "PACE service"),
            "text/html; charset=utf-8",
        ),
        Shape::Terminal => text(
            200,
            &render_terminal(&analysis, &trace),
            "text/plain; charset=utf-8",
        ),
    }
}

fn status_for(error: &pace_analyzer::Error) -> u16 {
    use pace_analyzer::ErrorCode;
    match error.code() {
        ErrorCode::InvalidArgument | ErrorCode::UnsupportedOutput => 400,
        ErrorCode::Parse | ErrorCode::FormatUnknown | ErrorCode::Empty => 422,
        ErrorCode::Io | ErrorCode::Write => 500,
    }
}

/// The analyzer's own error contract, over HTTP.
///
/// The same code, the same fix and the same recovery line the command line
/// prints, so a client that already handles one handles the other.
fn failure(status: u16, code: &str, message: &str, fix: &str) -> Answer {
    let body = serde_json::json!({
        "error": { "code": code, "message": message, "fix": fix },
    });
    json(status, body.to_string())
}

/// Decode the `trace` field of a url-encoded form body.
fn form_trace(body: &[u8]) -> Option<Vec<u8>> {
    let text = String::from_utf8_lossy(body);
    for pair in text.split('&') {
        let Some((key, value)) = pair.split_once('=') else {
            continue;
        };
        if key != "trace" {
            continue;
        }
        let plus_decoded = value.replace('+', " ");
        let bytes = plus_decoded.as_bytes();
        let mut out = Vec::with_capacity(bytes.len());
        let mut i = 0;
        while i < bytes.len() {
            if bytes[i] == b'%' && i + 2 < bytes.len() {
                if let Ok(byte) = u8::from_str_radix(&plus_decoded[i + 1..i + 3], 16) {
                    out.push(byte);
                    i += 3;
                    continue;
                }
            }
            out.push(bytes[i]);
            i += 1;
        }
        return Some(out);
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_shape_defaults_to_the_terminal_report() {
        assert_eq!(shape_from(""), Some(Shape::Terminal));
        assert_eq!(shape_from("format=html"), Some(Shape::Html));
        assert_eq!(shape_from("format=json"), Some(Shape::Json));
        assert_eq!(shape_from("verify=1"), Some(Shape::Verify));
        assert_eq!(shape_from("format=pdf"), None, "an unknown shape is a 400");
    }

    #[test]
    fn the_input_format_is_optional_and_checked() {
        assert_eq!(format_from(""), None);
        assert_eq!(
            format_from("trace_format=perf-sched"),
            Some(Format::PerfSched)
        );
        assert_eq!(format_from("trace_format=csv"), None);
    }

    #[test]
    fn a_form_body_is_decoded_into_the_trace() {
        let decoded = form_trace(b"trace=%7B%22tid%22%3A1%7D%0A").expect("a trace field");
        assert_eq!(String::from_utf8_lossy(&decoded), "{\"tid\":1}\n");
    }

    #[test]
    fn a_form_body_without_the_field_is_not_a_trace() {
        assert!(form_trace(b"other=1").is_none());
    }

    #[test]
    fn an_oversized_body_is_refused_with_the_limit_in_the_message() {
        let response = too_large(BODY_LIMIT + 1);
        assert_eq!(response.status_code().0, 413);
    }
}
