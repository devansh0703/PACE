// SPDX-License-Identifier: Apache-2.0
//! Report rendering: a terminal report and a self-contained HTML report.
//!
//! The HTML report is one file with no network dependencies. The chart is
//! drawn with inline SVG generated here, so the file opens offline with
//! nothing to fetch.

use crate::model::{Analysis, Severity, ThreadAttribution, Trace};
use std::fmt::Write as _;

/// Format a nanosecond duration for display.
pub fn format_duration(ns: u64) -> String {
    if ns < 1_000 {
        format!("{} ns", ns)
    } else if ns < 1_000_000 {
        format!("{:.2} us", ns as f64 / 1e3)
    } else if ns < 1_000_000_000 {
        format!("{:.2} ms", ns as f64 / 1e6)
    } else {
        format!("{:.2} s", ns as f64 / 1e9)
    }
}

/// Format a cycle count for display.
pub fn format_cycles(cycles: u64) -> String {
    if cycles == 0 {
        return "-".to_string();
    }
    if cycles < 1_000 {
        format!("{} cyc", cycles)
    } else if cycles < 1_000_000 {
        format!("{:.2} Kcyc", cycles as f64 / 1e3)
    } else if cycles < 1_000_000_000 {
        format!("{:.2} Mcyc", cycles as f64 / 1e6)
    } else {
        format!("{:.2} Gcyc", cycles as f64 / 1e9)
    }
}

/// Render the terminal report.
pub fn render_terminal(analysis: &Analysis, trace: &Trace) -> String {
    let mut out = String::new();

    out.push_str("PACE thread attribution\n");
    out.push_str("======================\n\n");

    let verify = trace.verify();
    out.push_str(&format!(
        "samples: {} across {} threads   decomposition: {}\n",
        verify.total_samples,
        analysis.threads.len(),
        if verify.is_ok() { "exact" } else { "VIOLATED" }
    ));
    if !verify.is_ok() {
        out.push_str(&format!(
            "  worst violation: {} ns (thread {}, sample {})\n",
            verify.worst_error_ns,
            verify.worst.map(|w| w.0).unwrap_or(0),
            verify.worst.map(|w| w.1).unwrap_or(0)
        ));
    }
    if !analysis.has_attribution {
        out.push_str(
            "  NOTE: this trace format carries no preemption/blocking split; the\n        \
             preempted and blocked figures below are not measurements.\n",
        );
    }
    if analysis.malformed_lines > 0 {
        out.push_str(&format!(
            "  NOTE: {} line(s) could not be read and were skipped.\n",
            analysis.malformed_lines
        ));
    }
    out.push('\n');

    out.push_str(&format!(
        "{:<10} {:>12} {:>12} {:>12} {:>8} {:>10} {:>12}\n",
        "thread", "on_cpu", "preempted", "blocked", "preempt%", "switches", "core cycles"
    ));
    out.push_str(&format!("{}\n", "-".repeat(82)));

    for thread in &analysis.threads {
        // A format with no preemption/blocking split must not print a
        // confident 0.0%: the figure is not measured, and a dash says so.
        let preempt_share = if analysis.has_attribution {
            format!("{:.1}%", thread.preempted_fraction() * 100.0)
        } else {
            "-".to_string()
        };
        let preempted = if analysis.has_attribution {
            format_duration(thread.preempted_ns)
        } else {
            "-".to_string()
        };
        let blocked = if analysis.has_attribution {
            format_duration(thread.blocked_ns)
        } else {
            "-".to_string()
        };
        out.push_str(&format!(
            "{:<10} {:>12} {:>12} {:>12} {:>8} {:>10} {:>12}\n",
            thread.tid,
            format_duration(thread.on_cpu_ns),
            preempted,
            blocked,
            preempt_share,
            thread.involuntary_switches + thread.voluntary_switches,
            format_cycles(thread.core_cycles),
        ));
    }

    out.push_str(&format!("{}\n", "-".repeat(82)));
    // The TOTAL row follows the same rule: an unmeasured split is a dash.
    let (total_preempted, total_blocked, total_share) = if analysis.has_attribution {
        (
            format_duration(analysis.total_preempted_ns),
            format_duration(analysis.total_blocked_ns),
            format!(
                "{:.1}%",
                if analysis.total_wall_ns == 0 {
                    0.0
                } else {
                    analysis.total_preempted_ns as f64 / analysis.total_wall_ns as f64 * 100.0
                }
            ),
        )
    } else {
        ("-".to_string(), "-".to_string(), "-".to_string())
    };
    out.push_str(&format!(
        "{:<10} {:>12} {:>12} {:>12} {:>8}\n",
        "TOTAL",
        format_duration(analysis.total_on_cpu_ns),
        total_preempted,
        total_blocked,
        total_share,
    ));

    if !analysis.findings.is_empty() {
        out.push_str("\nfindings\n--------\n");
        for finding in &analysis.findings {
            out.push_str(&format!(
                "[{}] {}\n",
                finding.severity.label(),
                finding.summary
            ));
        }
    }

    out
}

/// Render the self-contained HTML report.
///
/// The design follows the "Wire Terminal" direction: warm charcoal ground,
/// amber accent, IBM Plex Mono for structure, borders instead of shadows, and
/// severity-only colour.
pub fn render_html(analysis: &Analysis, trace: &Trace, title: &str) -> String {
    let verify = trace.verify();
    let mut html = String::with_capacity(64 * 1024);

    html.push_str("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
    html.push_str("<meta charset=\"utf-8\">\n");
    html.push_str("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    html.push_str(&format!(
        "<title>{} — PACE attribution</title>\n",
        escape_html(title)
    ));

    html.push_str(
        "<style>
:root {
  --ground: #131311;
  --panel: #191917;
  --panel-2: #1f1f1c;
  --line: #2e2e29;
  --line-strong: #45453d;
  --ink: #e6e4dd;
  --ink-dim: #9a988f;
  --ink-faint: #6b6a62;
  --accent: #e8a33d;
  --accent-dim: #8a6327;
  --sev-critical: #e0574a;
  --sev-warning: #e8a33d;
  --sev-info: #7f9bb5;
  --radius: 3px;
}
* { box-sizing: border-box; }
html, body { margin: 0; padding: 0; }
body {
  background: var(--ground);
  color: var(--ink);
  font-family: 'IBM Plex Sans', system-ui, -apple-system, sans-serif;
  font-size: 14px;
  line-height: 1.55;
  -webkit-font-smoothing: antialiased;
}
.mono { font-family: 'IBM Plex Mono', ui-monospace, 'SF Mono', Menlo, monospace; }
.wrap { max-width: 1120px; margin: 0 auto; padding: 40px 24px 72px; }
header { border-bottom: 1px solid var(--line); padding-bottom: 20px; margin-bottom: 28px; }
h1 { font-family: 'IBM Plex Mono', monospace; font-size: 20px; font-weight: 500; margin: 0 0 6px; letter-spacing: -0.01em; }
.sub { color: var(--ink-dim); font-size: 13px; }
.sub .mono { color: var(--accent); }
h2 { font-family: 'IBM Plex Mono', monospace; font-size: 13px; font-weight: 500; text-transform: uppercase;
     letter-spacing: 0.08em; color: var(--ink-dim); margin: 36px 0 12px; }
h2:first-of-type { margin-top: 0; }
table { width: 100%; border-collapse: collapse; font-family: 'IBM Plex Mono', monospace; font-size: 12.5px; }
th { text-align: right; color: var(--ink-faint); font-weight: 500; padding: 7px 10px;
     border-bottom: 1px solid var(--line-strong); white-space: nowrap; }
th:first-child, td:first-child { text-align: left; }
td { text-align: right; padding: 7px 10px; border-bottom: 1px solid var(--line); white-space: nowrap; }
tr.total td { border-top: 1px solid var(--line-strong); border-bottom: none; color: var(--accent); }
.bar { display: block; height: 4px; border-radius: 0; background: var(--line); overflow: hidden; margin-top: 5px; }
.bar i { display: block; height: 100%; background: var(--accent); }
.stack { display: flex; height: 10px; border: 1px solid var(--line-strong); border-radius: var(--radius); overflow: hidden; }
.stack .oncpu { background: var(--accent); }
.stack .preempt { background: var(--sev-critical); }
.stack .blocked { background: var(--sev-info); }
.legend { display: flex; gap: 18px; margin: 10px 0 0; font-size: 12px; color: var(--ink-dim); }
.legend span { display: flex; align-items: center; gap: 6px; }
.key { width: 9px; height: 9px; border-radius: 1px; display: inline-block; }
.finding { border: 1px solid var(--line); border-left-width: 2px; border-radius: var(--radius);
           padding: 11px 14px; margin-bottom: 8px; background: var(--panel); }
.finding.critical { border-left-color: var(--sev-critical); }
.finding.warning { border-left-color: var(--sev-warning); }
.finding.info { border-left-color: var(--sev-info); }
.finding .tag { font-family: 'IBM Plex Mono', monospace; font-size: 10.5px; text-transform: uppercase;
                letter-spacing: 0.08em; color: var(--ink-faint); }
.finding.critical .tag { color: var(--sev-critical); }
.finding.warning .tag { color: var(--sev-warning); }
.finding .body { margin-top: 3px; }
.kv { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 1px;
      background: var(--line); border: 1px solid var(--line); border-radius: var(--radius); overflow: hidden; }
.kv div { background: var(--panel); padding: 12px 14px; }
.kv .k { font-family: 'IBM Plex Mono', monospace; font-size: 10.5px; text-transform: uppercase;
          letter-spacing: 0.08em; color: var(--ink-faint); }
.kv .v { font-family: 'IBM Plex Mono', monospace; font-size: 17px; margin-top: 5px; }
.ok { color: #7fae72; }
.bad { color: var(--sev-critical); }
footer { margin-top: 44px; padding-top: 16px; border-top: 1px solid var(--line);
         color: var(--ink-faint); font-size: 12px; }
</style>\n</head>\n<body>\n<div class=\"wrap\">\n",
    );

    // Header
    let _ = writeln!(
        html,
        "<header><h1>{}</h1><div class=\"sub\">PACE thread attribution &middot; <span class=\"mono\">{}</span> threads &middot; <span class=\"mono\">{}</span> samples</div></header>",
        escape_html(title),
        analysis.threads.len(),
        verify.total_samples
    );

    // Summary
    html.push_str("<h2>Summary</h2>\n<div class=\"kv\">\n");
    let overall_preempted = if analysis.total_wall_ns == 0 {
        0.0
    } else {
        analysis.total_preempted_ns as f64 / analysis.total_wall_ns as f64 * 100.0
    };
    let on_cpu_text = format_duration(analysis.total_on_cpu_ns);
    // The summary follows the same rule as the table: a figure that was not
    // measured is a dash, not a zero.
    let (preempted_text, blocked_text, preempted_share_text) = if analysis.has_attribution {
        (
            format_duration(analysis.total_preempted_ns),
            format_duration(analysis.total_blocked_ns),
            format!("{overall_preempted:.1}%"),
        )
    } else {
        ("-".to_string(), "-".to_string(), "-".to_string())
    };
    let decomposition_class = if verify.is_ok() { "ok" } else { "bad" };
    let decomposition_text = if verify.is_ok() {
        "exact".to_string()
    } else {
        format!("{} bad", verify.violations)
    };

    let _ = write!(
        html,
        "<div><div class=\"k\">On-CPU</div><div class=\"v\">{}</div></div>\n\
         <div><div class=\"k\">Preempted</div><div class=\"v\">{}</div></div>\n\
         <div><div class=\"k\">Blocked</div><div class=\"v\">{}</div></div>\n\
         <div><div class=\"k\">Preempted share</div><div class=\"v\">{}</div></div>\n\
         <div><div class=\"k\">Decomposition</div><div class=\"v {}\">{}</div></div>\n</div>\n",
        on_cpu_text,
        preempted_text,
        blocked_text,
        preempted_share_text,
        decomposition_class,
        decomposition_text,
    );
    html.push_str("</div>\n");

    // The source format may not carry the preemption/blocking split at all.
    // Saying "0 % preempted" in that case would be an artefact of the
    // format, so the report says the attribution was unavailable.
    if !analysis.has_attribution {
        html.push_str(
            "<div class=\"finding warning\" style=\"margin-top:18px\"><div class=\"tag\">no attribution</div>\
             <div class=\"body\">This trace format does not carry a preemption/blocking split, so the \
             preemption and blocked figures above are not measurements. Convert it with \
             <code>pace convert</code> or record it with the PACE SDK.</div></div>\n",
        );
    }
    if analysis.malformed_lines > 0 {
        let _ = writeln!(
            html,
            "<div class=\"finding warning\" style=\"margin-top:18px\"><div class=\"tag\">partial trace</div>\
             <div class=\"body\">{} line(s) could not be read and were skipped. A trace captured under \
             stress is routinely truncated; the readable part is reported.</div></div>",
            analysis.malformed_lines
        );
    }

    // Per-thread table
    html.push_str("<h2>Threads</h2>\n<table>\n<thead><tr><th>Thread</th><th>On-CPU</th><th>Preempted</th><th>Blocked</th><th>Preempt%</th><th>Switches</th><th>Core cycles</th><th>Split</th></tr></thead>\n<tbody>\n");
    for thread in &analysis.threads {
        // A format with no preemption/blocking split must not print a
        // confident 0.0%: the figure is not measured, and a dash says so.
        let (preempted, blocked, share) = if analysis.has_attribution {
            (
                format_duration(thread.preempted_ns),
                format_duration(thread.blocked_ns),
                format!("{:.1}%", thread.preempted_fraction() * 100.0),
            )
        } else {
            ("-".to_string(), "-".to_string(), "-".to_string())
        };
        let _ = writeln!(
            html,
            "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>",
            thread.tid,
            format_duration(thread.on_cpu_ns),
            preempted,
            blocked,
            share,
            thread.involuntary_switches + thread.voluntary_switches,
            format_cycles(thread.core_cycles),
            if analysis.has_attribution {
                split_bar(thread)
            } else {
                split_bar_without_attribution()
            }
        );
    }
    let (total_preempted, total_blocked, total_share) = if analysis.has_attribution {
        (
            format_duration(analysis.total_preempted_ns),
            format_duration(analysis.total_blocked_ns),
            format!("{:.1}%", overall_preempted),
        )
    } else {
        ("-".to_string(), "-".to_string(), "-".to_string())
    };
    let _ = writeln!(
        html,
        "<tr class=\"total\"><td>TOTAL</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td></td><td></td><td></td></tr>",
        format_duration(analysis.total_on_cpu_ns),
        total_preempted,
        total_blocked,
        total_share
    );
    html.push_str("</tbody>\n</table>\n");

    html.push_str(
        "<div class=\"legend\"><span><i class=\"key\" style=\"background:var(--accent)\"></i>on-CPU</span>\
         <span><i class=\"key\" style=\"background:var(--sev-critical)\"></i>preempted</span>\
         <span><i class=\"key\" style=\"background:var(--sev-info)\"></i>blocked</span></div>\n",
    );

    // Findings
    html.push_str("<h2>Findings</h2>\n");
    if analysis.findings.is_empty() {
        html.push_str("<div class=\"finding info\"><div class=\"tag\">info</div><div class=\"body\">No thread exceeded the preemption or blocking thresholds.</div></div>\n");
    } else {
        for finding in &analysis.findings {
            let _ = writeln!(
                html,
                "<div class=\"finding {}\"><div class=\"tag\">{}</div><div class=\"body\">{}</div></div>",
                finding.severity.label(),
                finding.severity.label(),
                escape_html(&finding.summary)
            );
        }
    }

    // Per-phase breakdown
    let has_phases = analysis.threads.iter().any(|t| !t.phases.is_empty());
    if has_phases {
        html.push_str("<h2>Phases</h2>\n<table>\n<thead><tr><th>Thread</th><th>Phase</th><th>On-CPU</th><th>Preempted</th><th>Blocked</th><th>Samples</th></tr></thead>\n<tbody>\n");
        for thread in &analysis.threads {
            for (name, phase) in &thread.phases {
                let _ = writeln!(
                    html,
                    "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>",
                    thread.tid,
                    escape_html(name),
                    format_duration(phase.on_cpu_ns),
                    format_duration(phase.preempted_ns),
                    format_duration(phase.blocked_ns),
                    phase.sample_count
                );
            }
        }
        html.push_str("</tbody>\n</table>\n");
    }

    let _ = write!(
        html,
        "<footer>Generated by pace-analyzer. On-CPU, preempted and blocked times are the exact decomposition produced by the PACE SDK; the cycle figures are measured core cycles where the hardware counter was available and are marked accordingly by the source tier.</footer>\n</div>\n</body>\n</html>\n"
    );

    html
}

fn split_bar(thread: &ThreadAttribution) -> String {
    let wall = thread.wall_ns.max(1);
    let on_cpu = (thread.on_cpu_ns * 100 / wall).min(100);
    let preempted = (thread.preempted_ns * 100 / wall).min(100);
    let blocked = 100 - on_cpu - preempted;
    format!(
        "<span class=\"stack\"><i class=\"oncpu\" style=\"width:{}%\"></i><i class=\"preempt\" style=\"width:{}%\"></i><i class=\"blocked\" style=\"width:{}%\"></i></span>",
        on_cpu, preempted, blocked
    )
}

/// The split bar for a thread whose source carried no preemption/blocking
/// split.
///
/// Drawing the on-CPU segment alone would be indistinguishable from a
/// measured "no preemption at all", which is the exact confusion the
/// analyzer exists to prevent. An empty bar says the split is unavailable.
fn split_bar_without_attribution() -> String {
    "<span class=\"stack\" title=\"no preemption/blocking split in the source\"></span>".to_string()
}

/// Escape text for inclusion in HTML.
pub fn escape_html(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for ch in text.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&#39;"),
            _ => out.push(ch),
        }
    }
    out
}

/// The severity ordering used in the report, exposed for tests.
pub fn severity_rank(severity: Severity) -> u8 {
    match severity {
        Severity::Info => 0,
        Severity::Warning => 1,
        Severity::Critical => 2,
    }
}
