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
//
// These are RAW tracepoints, not the tracepoint/ form, and the difference
// matters. Attaching a tracepoint/ program needs a BPF link onto a perf
// event, and the kernel gates that on bpf_capable() in the initial user
// namespace. On a host with a nested user namespace, or with a hardened BPF
// policy, that check fails with EACCES even for uid 0 holding every
// capability, and the tier silently never attaches.
//
// A raw tracepoint is opened with BPF_RAW_TRACEPOINT_OPEN, which is gated
// differently and works on those hosts. It also needs no perf event, so
// there is no per-CPU event to open.
//
// The cost of the raw form is that the tracepoint's arguments are the
// tracepoint's *proto* arguments rather than a typed context, and the
// verifier rejects a program that reads past the last one. The counts
// below were measured on this kernel:
//
//   sched_wakeup        1 argument: the task_struct that was woken
//   sched_switch        2 arguments: the previous and the next task_struct
//   sched_process_exit  2 arguments: the task_struct and the exit code
//
// The thread id is therefore read from the task struct with a CO-RE read,
// which is also how the program stays portable across kernels whose
// task_struct layout differs.
#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>

char LICENSE[] SEC("license") = "GPL";

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

/* The thread id of a task, read with a CO-RE read so the program does not
 * depend on the task_struct's layout. */
static __always_inline __u32 task_tid(const struct task_struct *task) {
  return (__u32)BPF_CORE_READ(task, pid);
}

SEC("raw_tracepoint/sched_wakeup")
int pace_sched_wakeup(struct bpf_raw_tracepoint_args *ctx) {
  const struct task_struct *task = (const struct task_struct *)ctx->args[0];
  __u32 tid = task_tid(task);
  __u64 now = bpf_ktime_get_ns();

  __u64 *slot = bpf_map_lookup_elem(&wakeup_time, &tid);
  if (slot) {
    *slot = now;
    return 0;
  }
  bpf_map_update_elem(&wakeup_time, &tid, &now, BPF_ANY);
  return 0;
}

SEC("raw_tracepoint/sched_switch")
int pace_sched_switch(struct bpf_raw_tracepoint_args *ctx) {
  const struct task_struct *prev = (const struct task_struct *)ctx->args[0];
  const struct task_struct *next = (const struct task_struct *)ctx->args[1];
  __u32 prev_tid = task_tid(prev);
  __u32 next_tid = task_tid(next);
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

SEC("raw_tracepoint/sched_process_exit")
int pace_sched_exit(struct bpf_raw_tracepoint_args *ctx) {
  const struct task_struct *task = (const struct task_struct *)ctx->args[0];
  __u32 tid = task_tid(task);
  bpf_map_delete_elem(&wakeup_time, &tid);
  bpf_map_delete_elem(&switch_in_time, &tid);
  bpf_map_delete_elem(&preempt_count, &tid);
  bpf_map_delete_elem(&block_count, &tid);
  return 0;
}
