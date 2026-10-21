//! Benchmarks for the analyzer's hot paths.
//!
//! The attribution model runs over every sample in a trace, so it is the
//! path that matters for a large trace.

use criterion::{criterion_group, criterion_main, Criterion};
use pace_analyzer::model::{analyze, Sample, Thresholds, FLAG_HAS_BLOCKED, FLAG_HAS_PREEMPTED};
use pace_analyzer::Trace;

fn make_trace(threads: usize, samples_per_thread: usize) -> Trace {
    let mut trace = Trace::default();
    for t in 0..threads {
        let mut samples = Vec::with_capacity(samples_per_thread);
        for _s in 0..samples_per_thread {
            samples.push(Sample {
                wall_ns: 1_000_000,
                on_cpu_ns: 400_000,
                preempted_ns: 500_000,
                blocked_ns: 100_000,
                core_cycles: 1_200_000,
                ref_cycles: 1_000_000,
                core_per_ref_ratio: 1.2,
                involuntary_switches: 3,
                voluntary_switches: 0,
                source: pace_analyzer::Source::Clock,
                flags: FLAG_HAS_PREEMPTED | FLAG_HAS_BLOCKED,
                phase: Some("steady".to_string()),
            });
        }
        trace.threads.push(pace_analyzer::ThreadTrace {
            tid: t as u64,
            name: None,
            samples,
        });
    }
    trace
}

fn bench_aggregate(c: &mut Criterion) {
    let trace = make_trace(48, 2000);
    c.bench_function("aggregate_48_threads_x_2000_samples", |b| {
        b.iter(|| analyze(&trace, &Thresholds::default()))
    });
}

fn bench_verify(c: &mut Criterion) {
    let trace = make_trace(48, 2000);
    c.bench_function("verify_48_threads_x_2000_samples", |b| {
        b.iter(|| trace.verify())
    });
}

criterion_group!(benches, bench_aggregate, bench_verify);
criterion_main!(benches);
