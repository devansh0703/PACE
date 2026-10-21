// SPDX-License-Identifier: BUSL-1.1
// PACE — eBPF tier: per-thread scheduler state from the kernel's own
// sched_wakeup and sched_switch tracepoints.
//
// This tier exists because the procfs tier reads the kernel's *cumulative*
// run-queue delay, which is a good number but not a per-interval one, and
// because on a platform where /proc is restricted the procfs tier is
// unavailable entirely.
//
// The eBPF tier reconstructs, per thread, the interval between the moment a
// thread became runnable and the moment it actually ran. That interval is
// the definition of preemption wait, computed from the tracepoints the
// scheduler itself emits.
//
// The program is loaded and attached by the user-space side. It keeps a
// per-thread map of the last wakeup timestamp; the user space reads that map
// and diffs it against the on-CPU time it obtains elsewhere.
#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>

char LICENSE[] SEC("license") = "GPL";

/*
 * The sched_wakeup tracepoint's raw context layout. This one is not in
 * vmlinux.h because it is a tracepoint format structure rather than a
 * kernel type; the field offsets are stable tracepoint ABI. The other two
 * tracepoints' layouts are present in vmlinux.h and are used from there.
 */
struct trace_event_raw_sched_wakeup {
  struct trace_entry ent;
  char __data[0];
  __u32 comm[16];
  __u32 pid;
  __u32 prio;
  __u32 success;
  __u32 target_cpu;
};


/* The moment a thread became runnable, in nanoseconds, keyed by tid. */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 65536);
  __type(key, __u32);
  __type(value, __u64);
} wakeup_time SEC(".maps");

/* The moment a thread last started running, in nanoseconds, keyed by tid. */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 65536);
  __type(key, __u32);
  __type(value, __u64);
} switch_in_time SEC(".maps");

/* How many times a thread was descheduled while runnable. */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 65536);
  __type(key, __u32);
  __type(value, __u64);
} preempt_count SEC(".maps");

/* How many times a thread blocked. */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 65536);
  __type(key, __u32);
  __type(value, __u64);
} block_count SEC(".maps");

SEC("tracepoint/sched/sched_wakeup")
int pace_sched_wakeup(struct trace_event_raw_sched_wakeup *ctx) {
  __u32 tid = (__u32)ctx->pid;
  __u64 *slot = bpf_map_lookup_elem(&wakeup_time, &tid);
  if (slot) {
    *slot = bpf_ktime_get_ns();
    return 0;
  }
  __u64 now = bpf_ktime_get_ns();
  bpf_map_update_elem(&wakeup_time, &tid, &now, BPF_ANY);
  return 0;
}

SEC("tracepoint/sched/sched_switch")
int pace_sched_switch(struct trace_event_raw_sched_switch *ctx) {
  __u32 prev_tid = (__u32)ctx->prev_pid;
  __u32 next_tid = (__u32)ctx->next_pid;
  __u64 now = bpf_ktime_get_ns();

  /* The task being switched out. If it was runnable when it was switched
   * out, the scheduler preempted it; if it was sleeping, it blocked. */
  __u64 *woke = bpf_map_lookup_elem(&wakeup_time, &prev_tid);
  if (woke) {
    __u64 *count = bpf_map_lookup_elem(&preempt_count, &prev_tid);
    if (count) {
      *count += 1;
    } else {
      __u64 one = 1;
      bpf_map_update_elem(&preempt_count, &prev_tid, &one, BPF_ANY);
    }
    bpf_map_delete_elem(&wakeup_time, &prev_tid);
  } else {
    __u64 *count = bpf_map_lookup_elem(&block_count, &prev_tid);
    if (count) {
      *count += 1;
    } else {
      __u64 one = 1;
      bpf_map_update_elem(&block_count, &prev_tid, &one, BPF_ANY);
    }
  }

  /* The task being switched in started running now. */
  __u64 *in = bpf_map_lookup_elem(&switch_in_time, &next_tid);
  if (in) {
    *in = now;
  } else {
    bpf_map_update_elem(&switch_in_time, &next_tid, &now, BPF_ANY);
  }

  return 0;
}

SEC("tracepoint/sched/sched_process_exit")
int pace_sched_exit(struct trace_event_raw_sched_process_exit *ctx) {
  __u32 tid = (__u32)ctx->pid;
  bpf_map_delete_elem(&wakeup_time, &tid);
  bpf_map_delete_elem(&switch_in_time, &tid);
  bpf_map_delete_elem(&preempt_count, &tid);
  bpf_map_delete_elem(&block_count, &tid);
  return 0;
}
