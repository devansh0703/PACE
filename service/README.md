# pace-service

The hosted PACE analyzer: `pace analyze` behind an HTTP endpoint.

The project is one product with three ways to use it — a C and C++ library that
measures, an analyzer that reads a trace and proves the measurement, and this
service, which is the analyzer without the build step. This crate is a
transport in front of the analyzer library, not a second implementation: it
embeds `pace-analyzer` by path so the numbers it returns are the numbers the
command line returns.

## Run it

```bash
# from a checkout, with a Rust toolchain
cd service
cargo run --release
# -> pace-service 0.1.0 listening on 0.0.0.0:8080

# or in the container the deployment uses
docker build -t pace-service .
docker run -p 8080:8080 pace-service
```

`PORT` selects the port and defaults to 8080.

## Call it

```bash
# the terminal report
curl --data-binary @trace.jsonl http://localhost:8080/analyze

# the self-contained HTML report, one offline file
curl -o report.html --data-binary @trace.jsonl \
  "http://localhost:8080/analyze?format=html"

# JSON, for a script
curl --data-binary @trace.jsonl \
  "http://localhost:8080/analyze?format=json" | jq .summary

# the invariant as a status code: 200 when it holds on every sample, 422 when it does not
curl -o /dev/null -w '%{http_code}\n' --data-binary @trace.jsonl \
  "http://localhost:8080/analyze?verify=1"
```

| Endpoint | What it does |
|---|---|
| `GET /health` | liveness, the version, the limits, and the uptime |
| `GET /` | a form, so a person can paste a trace without reading this file |
| `POST /analyze` | a trace in the body, a report out |

Query parameters: `format=terminal\|html\|json`, `verify=1`, and
`trace_format=pace\|chrome\|perf-sched\|perf-trace` to skip detection.

Errors use the analyzer's own contract — a stable code, a fix, and a recovery
line — so a client that handles the command line's errors handles these:

```json
{"error": {"code": "empty-trace",
           "message": "the trace carried no samples",
           "fix": "check that the body is a trace and not an error message from a failed capture"}}
```

## What it does not do

- **It stores nothing.** The trace is read into the request's buffer and
  dropped with it. There is no database, and the free host has no persistent
  disk for a trace to reach even by accident.
- **It serves one request at a time.** The free instance has a tenth of a core,
  so a large trace queues rather than starving the process.
- **It is not the product.** Every limit it has is a limit of the free host.
  Run `pace analyze` locally and none of them apply.

## Limits

| Limit | Value | Why |
|---|---|---|
| Body | 8 MiB | the instance has 512 MB of memory, and a body is buffered before it is parsed |
| Samples | 1,000,000 | a bound on the work one request can ask for |
| Budget | 20 s | past it the client is answered with 504 and the worker's result is discarded |

A rejected body is a handled error with the limit in the message, never a
crash. The body cap is enforced from `Content-Length` before buffering, and
again from the bytes actually read for a chunked request.

## Deployment

`render.yaml` at the repository root is the manifest the deployment reads, and
the root `Dockerfile` is what it builds — the Dockerfile sits at the root
because the build context is the whole repository (the service depends on the
analyzer crate by path) and that is Render's default path, so there is no
override for a deployment to lose. The service is deployed as a single free web
instance: a Render workspace gets 750 free instance hours per calendar month,
and a month is 730-744 hours, so exactly one always-on service fits and
everything lives on it.

A free instance sleeps after fifteen minutes without a request and takes about
a minute to wake. An uptime monitor on `/health` at a five-minute interval is
what keeps that from being visible.

## Licence

Apache-2.0, matching the analyzer it embeds.
