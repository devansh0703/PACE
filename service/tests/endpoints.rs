// SPDX-License-Identifier: Apache-2.0
//! The endpoints, over real HTTP.
//!
//! The routing is inside a function that needs a live request, so it is tested
//! by starting the actual binary on a free port and talking to it. This is
//! worth the setup: a HEAD to /health answering 404 is exactly the kind of bug
//! that only shows up once the service is deployed, and it is the kind of
//! dishonest status report this project exists to avoid.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

const TRACE: &str = r#"{"name":"pace.sample","ph":"X","pid":1,"tid":1,"ts":0.000,"dur":1000.0,"cat":"pace","args":{"wall_ns":1000000,"on_cpu_ns":400000,"preempted_ns":500000,"blocked_ns":100000,"ref_cycles":1000}}
"#;

struct Server {
    child: Child,
    port: u16,
}

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// Start the service on a free port and wait until it answers.
fn start() -> Server {
    let listener = TcpListener::bind("127.0.0.1:0").expect("a free port");
    let port = listener.local_addr().expect("a local address").port();
    drop(listener);

    let child = Command::new(env!("CARGO_BIN_EXE_pace-service"))
        .env("PORT", port.to_string())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .expect("the service starts");

    let server = Server { child, port };

    // A spawned process is not instantly listening, so the first probe is
    // retried rather than assumed to have failed.
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        if let Some((status, _)) = request(port, "GET", "/health", None) {
            if status == 200 {
                return server;
            }
        }
        assert!(
            Instant::now() < deadline,
            "the service did not answer GET /health within 20 s"
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

/// One HTTP/1.1 request, returning the status line's code and the whole
/// response. The service is HTTP/1.1 and speaks no chunking for these paths.
fn request(port: u16, method: &str, path: &str, body: Option<&[u8]>) -> Option<(u16, Vec<u8>)> {
    let mut stream = TcpStream::connect(("127.0.0.1", port)).ok()?;
    stream
        .set_read_timeout(Some(Duration::from_secs(10)))
        .ok()?;

    let mut head = format!("{method} {path} HTTP/1.1\r\nHost: 127.0.0.1\r\n");
    if let Some(body) = body {
        head.push_str("Content-Type: application/octet-stream\r\n");
        head.push_str(&format!("Content-Length: {}\r\n", body.len()));
    }
    head.push_str("Connection: close\r\n\r\n");
    stream.write_all(head.as_bytes()).ok()?;
    if let Some(body) = body {
        stream.write_all(body).ok()?;
    }
    stream.flush().ok()?;

    let mut response = Vec::new();
    stream.read_to_end(&mut response).ok()?;
    let status = response
        .windows(12)
        .position(|w| w.starts_with(b"HTTP/1.1 "))
        .and_then(|at| {
            response[at + 9..]
                .get(..3)
                .and_then(|bytes| std::str::from_utf8(bytes).ok())
                .and_then(|digits| digits.parse::<u16>().ok())
        })?;
    Some((status, response))
}

fn body_of(response: &[u8]) -> &[u8] {
    response
        .windows(4)
        .position(|w| w == b"\r\n\r\n")
        .map(|at| &response[at + 4..])
        .unwrap_or(&[])
}

#[test]
fn health_answers_get_and_head() {
    let server = start();
    let (status, _) = request(server.port, "GET", "/health", None).expect("GET /health");
    assert_eq!(status, 200);

    // A HEAD probe must not report the service down.
    let (status, response) = request(server.port, "HEAD", "/health", None).expect("HEAD /health");
    assert_eq!(
        status, 200,
        "a HEAD liveness probe must answer 200, not 404"
    );
    assert!(
        body_of(&response).is_empty(),
        "a HEAD response must carry no body"
    );
}

#[test]
fn the_root_serves_a_form() {
    let server = start();
    let (status, response) = request(server.port, "GET", "/", None).expect("GET /");
    assert_eq!(status, 200);
    let body = String::from_utf8_lossy(body_of(&response));
    assert!(
        body.contains("<textarea"),
        "the root must serve a form to paste a trace"
    );
    assert!(
        body.contains("action=\"/analyze"),
        "the form must post to the analyzer"
    );
}

#[test]
fn analyze_accepts_a_post_and_rejects_a_get() {
    let server = start();

    let (status, response) =
        request(server.port, "POST", "/analyze", Some(TRACE.as_bytes())).expect("POST /analyze");
    assert_eq!(status, 200);
    let body = String::from_utf8_lossy(body_of(&response));
    assert!(
        body.contains("PACE thread attribution"),
        "the report is the terminal report"
    );

    let (status, _) = request(server.port, "GET", "/analyze", None).expect("GET /analyze");
    assert_eq!(
        status, 405,
        "the analyzer is a POST endpoint; a GET must say so rather than 404"
    );
}

#[test]
fn verify_returns_a_status_code_not_a_report() {
    let server = start();
    let (status, response) = request(
        server.port,
        "POST",
        "/analyze?verify=1",
        Some(TRACE.as_bytes()),
    )
    .expect("POST /analyze?verify=1");
    assert_eq!(status, 200);
    let body = String::from_utf8_lossy(body_of(&response));
    assert!(
        body.contains("\"decomposition\":\"exact\""),
        "an exact trace is 200: {body}"
    );
}

#[test]
fn an_unknown_path_is_a_404_and_not_a_crash() {
    let server = start();
    let (status, _) =
        request(server.port, "GET", "/nothing-here", None).expect("GET /nothing-here");
    assert_eq!(status, 404);
}

#[test]
fn options_is_answered_for_preflight() {
    let server = start();
    let (status, response) =
        request(server.port, "OPTIONS", "/analyze", None).expect("OPTIONS /analyze");
    assert_eq!(status, 204);
    let raw = String::from_utf8_lossy(&response);
    assert!(
        raw.to_lowercase().contains("access-control-allow-origin"),
        "a preflight must be answered with CORS"
    );
}

#[test]
fn the_service_answers_a_second_request_after_the_first() {
    // The server is one process serving requests in a loop; a service that
    // handled exactly one request would look fine in every test above.
    let server = start();
    for _ in 0..3 {
        let (status, _) = request(server.port, "GET", "/health", None).expect("GET /health");
        assert_eq!(status, 200);
    }
}
