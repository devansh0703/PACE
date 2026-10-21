//! The trace model: samples, threads, attribution and findings.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

/// The provenance of a sample's cycle numbers.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "lowercase")]
pub enum Source {
    /// Nanosecond clocks; cycles derived from a measured reference frequency.
    Clock,
    /// Hardware performance counter; cycles measured directly.
    Perf,
    /// No cycle data.
    #[default]
    Unknown,
}

/// The SDK's `PACE_SAMPLE_F_HAS_PREEMPTED` flag.
pub const FLAG_HAS_PREEMPTED: u32 = 1 << 6;
/// The SDK's `PACE_SAMPLE_F_HAS_BLOCKED` flag.
pub const FLAG_HAS_BLOCKED: u32 = 1 << 7;

/// One measurement window for one thread.
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
pub struct Sample {
    /// Monotonic wall-clock duration of the window, in nanoseconds.
    #[serde(default)]
    pub wall_ns: u64,
    /// Time the thread spent running, in nanoseconds.
    #[serde(default)]
    pub on_cpu_ns: u64,
    /// Time the thread spent runnable but not scheduled, in nanoseconds.
    #[serde(default)]
    pub preempted_ns: u64,
    /// Time the thread was neither running nor runnable, in nanoseconds.
    #[serde(default)]
    pub blocked_ns: u64,
    /// Measured core cycles, when available.
    #[serde(default)]
    pub core_cycles: u64,
    /// Reference cycles, measured or derived.
    #[serde(default)]
    pub ref_cycles: u64,
    /// Measured on-CPU core cycles per reference cycle.
    #[serde(default)]
    pub core_per_ref_ratio: f64,
    /// Times the thread was descheduled while runnable.
    #[serde(default)]
    pub involuntary_switches: u64,
    /// Times the thread blocked waiting to be scheduled.
    #[serde(default)]
    pub voluntary_switches: u64,
    /// Where the cycle numbers came from.
    #[serde(default)]
    pub source: Source,
    /// Bit flags; see the SDK's `PACE_SAMPLE_F_*` constants.
    #[serde(default)]
    pub flags: u32,
    /// Optional phase label, used to compare named phases of a run.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub phase: Option<String>,
}

impl Sample {
    /// True when the sample's three time buckets sum to its wall time.
    pub fn is_balanced(&self) -> bool {
        self.on_cpu_ns + self.preempted_ns + self.blocked_ns == self.wall_ns
    }

    /// The absolute difference between the wall time and the sum of the buckets.
    pub fn balance_error_ns(&self) -> u64 {
        let sum = self.on_cpu_ns + self.preempted_ns + self.blocked_ns;
        sum.abs_diff(self.wall_ns)
    }

    /// True when the source format carried a real preemption/blocking split.
    ///
    /// A plain Chrome trace event has a duration and nothing else, so the
    /// analyzer treats the window as on-CPU. Reporting that as "0 %
    /// preemption" would be a fabricated finding, so the sample is marked as
    /// carrying no attribution instead.
    pub fn has_attribution(&self) -> bool {
        (self.flags & (crate::model::FLAG_HAS_PREEMPTED | crate::model::FLAG_HAS_BLOCKED))
            == (crate::model::FLAG_HAS_PREEMPTED | crate::model::FLAG_HAS_BLOCKED)
    }
}

/// One thread's samples.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct ThreadTrace {
    /// The OS thread id.
    pub tid: u64,
    /// A human-readable name, if known.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// The samples for this thread, in time order.
    pub samples: Vec<Sample>,
}

impl ThreadTrace {
    /// True when at least one sample carried a real preemption/blocking split.
    ///
    /// A trace in a format that does not carry the split (a plain Chrome
    /// trace) produces threads that are on-CPU for their whole duration. That
    /// is not a measurement of "no preemption", so the report says the
    /// attribution was unavailable rather than showing a 0 % figure.
    pub fn has_attribution(&self) -> bool {
        self.samples.iter().any(|s| s.has_attribution())
    }
}

/// A trace: samples grouped by thread.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Trace {
    /// The threads in the trace.
    #[serde(default)]
    pub threads: Vec<ThreadTrace>,
    /// The reference frequency in Hz, if the trace carried one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reference_hz: Option<f64>,
    /// Lines the parser could not read.
    ///
    /// A trace captured under stress is routinely truncated at the end, and a
    /// tool whose job is to analyse such traces should use the readable part
    /// rather than refuse the whole file. The count is reported so the reader
    /// knows what was dropped.
    #[serde(default)]
    pub malformed_lines: usize,
}

impl Trace {
    /// Find a thread by id.
    pub fn thread(&self, tid: u64) -> Option<&ThreadTrace> {
        self.threads.iter().find(|t| t.tid == tid)
    }

    /// A human-readable title for the trace, derived from the first thread's
    /// name when one is known.
    pub fn trace_title(&self) -> Option<String> {
        self.threads
            .iter()
            .find_map(|t| t.name.clone())
            .map(|name| format!("PACE trace — {}", name))
    }

    /// The total number of samples in the trace.
    pub fn sample_count(&self) -> usize {
        self.threads.iter().map(|t| t.samples.len()).sum()
    }

    /// Check the decomposition invariant across every sample.
    ///
    /// Returns the number of samples that violate `wall == on_cpu +
    /// preempted + blocked`.
    pub fn verify(&self) -> VerifyReport {
        let mut total = 0usize;
        let mut violations = 0usize;
        let mut worst_error_ns = 0u64;
        let mut worst: Option<(u64, usize)> = None;

        for thread in &self.threads {
            for (index, sample) in thread.samples.iter().enumerate() {
                total += 1;
                let error = sample.balance_error_ns();
                if error > 0 {
                    violations += 1;
                }
                if error > worst_error_ns {
                    worst_error_ns = error;
                    worst = Some((thread.tid, index));
                }
            }
        }

        VerifyReport {
            total_samples: total,
            violations,
            worst_error_ns,
            worst,
        }
    }
}

/// The result of checking the decomposition invariant.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct VerifyReport {
    /// How many samples were checked.
    pub total_samples: usize,
    /// How many violated the invariant.
    pub violations: usize,
    /// The largest violation, in nanoseconds.
    pub worst_error_ns: u64,
    /// The thread and sample index of the largest violation.
    pub worst: Option<(u64, usize)>,
}

impl VerifyReport {
    /// True when every sample satisfied the invariant.
    pub fn is_ok(&self) -> bool {
        self.violations == 0
    }
}

/// The aggregated attribution for one thread.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct ThreadAttribution {
    /// The OS thread id.
    pub tid: u64,
    /// A human-readable name, if known.
    pub name: Option<String>,
    /// Total wall time across all samples.
    pub wall_ns: u64,
    /// Total on-CPU time.
    pub on_cpu_ns: u64,
    /// Total preempted time.
    pub preempted_ns: u64,
    /// Total blocked time.
    pub blocked_ns: u64,
    /// Total measured core cycles, when available.
    pub core_cycles: u64,
    /// Total reference cycles.
    pub ref_cycles: u64,
    /// Number of samples.
    pub sample_count: usize,
    /// True when this thread's samples carried a real preemption/blocking
    /// split. When false, the preempted and blocked figures are not
    /// measurements.
    pub has_attribution: bool,
    /// Total involuntary switches.
    pub involuntary_switches: u64,
    /// Total voluntary switches.
    pub voluntary_switches: u64,
    /// Per-phase attribution, when the trace carried phase labels.
    pub phases: BTreeMap<String, PhaseAttribution>,
}

/// The aggregated attribution for one phase of one thread.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct PhaseAttribution {
    /// Total wall time in this phase.
    pub wall_ns: u64,
    /// Total on-CPU time in this phase.
    pub on_cpu_ns: u64,
    /// Total preempted time in this phase.
    pub preempted_ns: u64,
    /// Total blocked time in this phase.
    pub blocked_ns: u64,
    /// Number of samples in this phase.
    pub sample_count: usize,
}

impl ThreadAttribution {
    /// The fraction of wall time the thread spent on CPU, in 0.0..=1.0.
    pub fn on_cpu_fraction(&self) -> f64 {
        fraction(self.on_cpu_ns, self.wall_ns)
    }

    /// The fraction of wall time lost to preemption, in 0.0..=1.0.
    pub fn preempted_fraction(&self) -> f64 {
        fraction(self.preempted_ns, self.wall_ns)
    }

    /// The fraction of wall time spent blocked, in 0.0..=1.0.
    pub fn blocked_fraction(&self) -> f64 {
        fraction(self.blocked_ns, self.wall_ns)
    }
}

/// Divide, returning 0.0 for a zero denominator.
pub fn fraction(part: u64, whole: u64) -> f64 {
    if whole == 0 {
        0.0
    } else {
        part as f64 / whole as f64
    }
}

/// A finding: something a performance engineer should act on.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Finding {
    /// How serious the finding is.
    pub severity: Severity,
    /// The thread the finding is about, when it is thread-specific.
    pub tid: Option<u64>,
    /// A one-line statement of the finding.
    pub summary: String,
    /// The measured value that triggered it.
    pub value: f64,
    /// The unit of `value`.
    pub unit: String,
}

/// How serious a finding is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Severity {
    /// Worth knowing.
    Info,
    /// Worth investigating.
    Warning,
    /// Needs attention now.
    Critical,
}

impl Severity {
    /// The label used in reports.
    pub fn label(&self) -> &'static str {
        match self {
            Severity::Info => "info",
            Severity::Warning => "warning",
            Severity::Critical => "critical",
        }
    }
}

/// The thresholds that turn measurements into findings.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct Thresholds {
    /// A thread losing more than this fraction of its wall time to preemption
    /// is reported as a finding.
    pub preempted_fraction: f64,
    /// A thread blocked more than this fraction of its wall time is reported.
    pub blocked_fraction: f64,
    /// A thread with more than this fraction of its wall time on CPU is
    /// considered healthy and is not reported for being busy.
    pub healthy_on_cpu_fraction: f64,
}

impl Default for Thresholds {
    fn default() -> Self {
        Thresholds {
            preempted_fraction: 0.25,
            blocked_fraction: 0.25,
            healthy_on_cpu_fraction: 0.0,
        }
    }
}

/// The result of analysing a trace.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Analysis {
    /// Per-thread attribution, sorted by wall time descending.
    pub threads: Vec<ThreadAttribution>,
    /// The findings.
    pub findings: Vec<Finding>,
    /// The total wall time across all threads.
    pub total_wall_ns: u64,
    /// The total on-CPU time across all threads.
    pub total_on_cpu_ns: u64,
    /// The total preempted time across all threads.
    pub total_preempted_ns: u64,
    /// The total blocked time across all threads.
    pub total_blocked_ns: u64,
    /// True when at least one thread carried a real preemption/blocking split.
    ///
    /// When false, the preemption and blocked figures are not measurements and
    /// the report must say so rather than showing a confident 0 %.
    pub has_attribution: bool,
    /// Lines the parser could not read.
    pub malformed_lines: usize,
}

/// Analyse a trace.
pub fn analyze(trace: &Trace, thresholds: &Thresholds) -> Analysis {
    let mut analysis = Analysis::default();

    for thread in &trace.threads {
        let mut attribution = ThreadAttribution {
            tid: thread.tid,
            name: thread.name.clone(),
            has_attribution: thread.has_attribution(),
            ..Default::default()
        };

        for sample in &thread.samples {
            attribution.wall_ns += sample.wall_ns;
            attribution.on_cpu_ns += sample.on_cpu_ns;
            attribution.preempted_ns += sample.preempted_ns;
            attribution.blocked_ns += sample.blocked_ns;
            attribution.core_cycles += sample.core_cycles;
            attribution.ref_cycles += sample.ref_cycles;
            attribution.sample_count += 1;
            attribution.involuntary_switches += sample.involuntary_switches;
            attribution.voluntary_switches += sample.voluntary_switches;

            if let Some(phase) = &sample.phase {
                let entry = attribution.phases.entry(phase.clone()).or_default();
                entry.wall_ns += sample.wall_ns;
                entry.on_cpu_ns += sample.on_cpu_ns;
                entry.preempted_ns += sample.preempted_ns;
                entry.blocked_ns += sample.blocked_ns;
                entry.sample_count += 1;
            }
        }

        analysis.total_wall_ns += attribution.wall_ns;
        analysis.total_on_cpu_ns += attribution.on_cpu_ns;
        analysis.total_preempted_ns += attribution.preempted_ns;
        analysis.total_blocked_ns += attribution.blocked_ns;

        analysis.threads.push(attribution);
    }

    analysis
        .threads
        .sort_by_key(|t| std::cmp::Reverse(t.wall_ns));

    // A thread whose samples carry no preemption/blocking split cannot be
    // reported on: its "0 % preempted" would be an artefact of the source
    // format, not a measurement.
    for thread in &analysis.threads {
        if thread.has_attribution {
            analysis.has_attribution = true;
        }
    }

    for thread in &analysis.threads {
        if !thread.has_attribution {
            continue;
        }
        let preempted = thread.preempted_fraction();
        if preempted > thresholds.preempted_fraction {
            let severity = if preempted > 0.5 {
                Severity::Critical
            } else {
                Severity::Warning
            };
            analysis.findings.push(Finding {
                severity,
                tid: Some(thread.tid),
                summary: format!(
                    "thread {} lost {:.1}% of its wall time to preemption",
                    thread.tid,
                    preempted * 100.0
                ),
                value: preempted * 100.0,
                unit: "%".to_string(),
            });
        }

        let blocked = thread.blocked_fraction();
        if blocked > thresholds.blocked_fraction {
            let severity = if blocked > 0.5 {
                Severity::Critical
            } else {
                Severity::Warning
            };
            analysis.findings.push(Finding {
                severity,
                tid: Some(thread.tid),
                summary: format!(
                    "thread {} spent {:.1}% of its wall time blocked",
                    thread.tid,
                    blocked * 100.0
                ),
                value: blocked * 100.0,
                unit: "%".to_string(),
            });
        }
    }

    // A process-level finding: if the whole run lost most of its wall time to
    // preemption, no amount of code optimisation will fix it.
    let overall_preempted = fraction(analysis.total_preempted_ns, analysis.total_wall_ns);
    if analysis.has_attribution && overall_preempted > thresholds.preempted_fraction {
        analysis.findings.push(Finding {
            severity: if overall_preempted > 0.5 {
                Severity::Critical
            } else {
                Severity::Warning
            },
            tid: None,
            summary: format!(
                "the run lost {:.1}% of its total wall time to preemption",
                overall_preempted * 100.0
            ),
            value: overall_preempted * 100.0,
            unit: "%".to_string(),
        });
    }

    analysis.findings.sort_by(|a, b| {
        b.severity
            .cmp(&a.severity)
            .then(b.value.total_cmp(&a.value))
    });

    analysis.malformed_lines = trace.malformed_lines;

    analysis
}
