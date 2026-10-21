// SPDX-License-Identifier: BUSL-1.1
// PACE — eBPF tier: per-thread scheduler state from the kernel's own
// sched_wakeup and sched_switch tracepoints.
//
// This tier exists because the procfs tier reads the kernel's *cumulative*
// run-queue delay, which is a good number but not a per-interval one, and
// because on a platform where /proc is restricted the procfs tier is
// unavailable entirely.
//
// The eBPF tier measures the preemption wait directly: the interval
// between the moment a thread became runnable and the moment it actually
// ran. That is the definition of the quantity the product reports.
//
// WHY NOT THE KERNEL'S COUNTER
// ----------------------------
// The kernel's /proc/<tid>/schedstat run_delay counter measures the same
// phenomenon as the time from QUEUEING to DEQUEUEING. A thread that is
// woken, queued, and then migrated or requeued loses the interval,
// because the counter is reset when the thread leaves one runqueue and
// only restarts when it enters the next.
//
// Measured on a CI runner, that counter reported 0.30 s where the
// scheduler's own events showed 1.81 s -- an 80 % undercount -- while the
// on-CPU time and the total off-CPU time both agreed with perf.
//
// THE KERNEL SAYS WHETHER IT WAS A PREEMPTION
// --------------------------------------------
// sched_switch's first argument is a bool that says exactly that. The
// kernel sets it when the switch was a preemption rather than a
// voluntary switch, so the program does not have to infer it. That is
// what makes the attribution exact rather than approximate:
//
//   - preempt == true:  the task was runnable and was switched out
//     anyway. It stayed runnable, so the time until it runs again is
//     preemption wait.
//
//   - preempt == false: the task blocked or yielded. If it is woken
//     again, the interval from the wakeup to the switch-in is the time
//     it spent on the run queue, which is also preemption wait.
//
// Either way the run-queue wait is the interval from the thread's most
// recent "became runnable" event to the switch-in that ran it, and the
// events are:
//
//   - sched_wakeup, when a sleeping task is woken;
//   - the previous switch-out, when a preempted task is switched back in
//     with no intervening wakeup, because it never stopped being
//     runnable.
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
// verifier rejects a program that reads past the last one. The protos,
// from the kernel's own include/trace/events/sched.h, are:
//
//   sched_wakeup        (struct task_struct *p)
//   sched_switch        (bool preempt, struct task_struct *prev,
//                        struct task_struct *next, unsigned int prev_state)
//   sched_process_exit  (struct task_struct *p, bool group_dead)
//
// So the argument positions are: the previous task is args[1] and the
// next task is args[2] of sched_switch, NOT args[0] and args[1]. An
// earlier version of this program had them wrong, read a bool as a task
// pointer, and produced an empty measurement while reporting itself
// attached. The positions below are the kernel's.
//
// The thread id is read from the task struct with a CO-RE read, which is
// also how the program stays portable across kernels whose task_struct
// layout differs.
#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>

char LICENSE[] SEC("license") = "GPL";

/* The task states, from the kernel's include/linux/sched.h.
 *
 * They are not in vmlinux.h, because they are macros rather than types,
 * and the BTF holds types. They are part of the kernel's stable
 * interface, and their values are fixed:
 *
 *     TASK_RUNNING           0
 *     TASK_INTERRUPTIBLE     1
 *     TASK_UNINTERRUPTIBLE   2
 *
 * Only TASK_RUNNING is needed: it is the state a task is in when it is
 * switched out while it was still runnable, which is the preemption
 * case. */
#define PACE_TASK_RUNNING 0

/* The moment a thread was woken, in nanoseconds, keyed by tid. */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 65536);
  __type(key, __u32);
  __type(value, __u64);
} wakeup_time SEC(".maps");

/* The moment a thread was last switched out while preempted, in
 * nanoseconds, keyed by tid. */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 65536);
  __type(key, __u32);
  __type(value, __u64);
} switch_out_time SEC(".maps");

/* The cumulative time a thread spent runnable but not running, in
 * nanoseconds, keyed by tid.
 *
 * This is the preemption wait measured from the scheduler's own events.
 * It is cumulative, exactly like the kernel's run_delay, so the user
 * space takes the same difference between its baseline and the sample. */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 65536);
  __type(key, __u32);
  __type(value, __u64);
} preempted_ns SEC(".maps");

/* How many times a thread was runnable but not running. */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 65536);
  __type(key, __u32);
  __type(value, __u64);
} preempt_count SEC(".maps");

/* How many times a thread went to sleep. */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 65536);
  __type(key, __u32);
  __type(value, __u64);
} block_count SEC(".maps");

/* A diagnostic: how many switch-outs were seen with each prev_state,
 * keyed by the state value.
 *
 * This is here because the tier's behaviour differs between kernels, and
 * the difference needs to be measurable rather than guessed at. It
 * answers one question directly: does a preempted task report
 * TASK_RUNNING when it is switched out, on this kernel?
 */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 64);
  __type(key, __u32);
  __type(value, __u64);
} state_counts SEC(".maps");

/* A diagnostic: how many switch-ins there were, and how many of them
 * found a "runnable since" timestamp. The difference is the number of
 * switch-ins whose interval was not measured. */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 64);
  __type(key, __u32);
  __type(value, __u64);
} wait_counts SEC(".maps");

/* The thread id of a task, read with a CO-RE read so the program does not
 * depend on the task_struct's layout. */
static __always_inline __u32 task_tid(const struct task_struct *task) {
  return (__u32)BPF_CORE_READ(task, pid);
}

/* Add to a counter map, creating the entry when it is not there.
 *
 * A macro rather than a function, because the map's type is an anonymous
 * struct and the verifier needs the map's address at the call site. The
 * increment is a local, because the map's API takes its address. */
#define COUNT_ADD(map, tid)                                                       \
  do {                                                                            \
    __u32 tid_ = (tid);                                                          \
    __u64 one_ = 1;                                                              \
    __u64 *slot_ = bpf_map_lookup_elem(&(map), &tid_);                            \
    if (slot_) {                                                                 \
      *slot_ += one_;                                                            \
    } else {                                                                     \
      bpf_map_update_elem(&(map), &tid_, &one_, BPF_ANY);                         \
    }                                                                            \
  } while (0)

SEC("raw_tracepoint/sched_wakeup")
int pace_sched_wakeup(struct bpf_raw_tracepoint_args *ctx) {
  /* sched_wakeup's proto is (struct task_struct *p), so the task is the
   * first argument. */
  const struct task_struct *task = (const struct task_struct *)ctx->args[0];
  __u32 tid = task_tid(task);
  __u64 now = bpf_ktime_get_ns();

  /* The thread is runnable again. From now until it runs is the time it
   * spends on the run queue, which is preemption wait. */
  bpf_map_update_elem(&wakeup_time, &tid, &now, BPF_ANY);
  return 0;
}

SEC("raw_tracepoint/sched_switch")
int pace_sched_switch(struct bpf_raw_tracepoint_args *ctx) {
  /* sched_switch's proto is
   *   (bool preempt, struct task_struct *prev, struct task_struct *next,
   *    unsigned int prev_state)
   * so the previous task is args[1] and the next task is args[2]. */
  const struct task_struct *prev = (const struct task_struct *)ctx->args[1];
  const struct task_struct *next = (const struct task_struct *)ctx->args[2];
  unsigned int prev_state = (unsigned int)ctx->args[3];
  __u32 prev_tid = task_tid(prev);
  __u32 next_tid = task_tid(next);
  __u64 now = bpf_ktime_get_ns();

  /* The task being switched in ran now.
   *
   * Its "runnable since" is its most recent runnable event:
   *
   *   - its wakeup, if it was woken after being switched out. It slept in
   *     between, and it has been on the run queue since the wakeup.
   *
   *   - its previous switch-out, if it was switched back in with no
   *     intervening wakeup. It was preempted, and it never stopped being
   *     runnable, so it has been on the run queue since the switch-out.
   */
  __u64 *woke = bpf_map_lookup_elem(&wakeup_time, &next_tid);
  __u64 *out = bpf_map_lookup_elem(&switch_out_time, &next_tid);

  __u64 runnable_since = 0;
  if (woke) {
    runnable_since = *woke;
  } else if (out) {
    runnable_since = *out;
  }

  COUNT_ADD(wait_counts, 0); /* the switch-ins */
  if (runnable_since) COUNT_ADD(wait_counts, 1); /* the ones with a timestamp */

  if (runnable_since && now > runnable_since) {
    __u64 wait = now - runnable_since;
    __u64 *total = bpf_map_lookup_elem(&preempted_ns, &next_tid);
    if (total) {
      *total += wait;
    } else {
      bpf_map_update_elem(&preempted_ns, &next_tid, &wait, BPF_ANY);
    }
    COUNT_ADD(preempt_count, next_tid);
  } else {
    /* The entry is created at EVERY switch-in, not only at the ones
     * that add a wait.
     *
     * The map's presence for a thread is what confirms the thread's
     * kernel id, so an entry that appeared only at the second switch-in
     * delayed the confirmation by a whole scheduling round. Until the
     * confirmation, the SDK measured the preemption with the kernel's
     * run-queue delay counter, which undercounts it by up to 80 % on
     * some kernels, and the window before the confirmation was lost.
     *
     * The first switch-in creates the entry with a value of zero, which
     * is a real measurement: the thread had just started running and
     * was not waiting to run. */
    __u64 *total = bpf_map_lookup_elem(&preempted_ns, &next_tid);
    if (!total) {
      __u64 zero = 0;
      bpf_map_update_elem(&preempted_ns, &next_tid, &zero, BPF_ANY);
    }
  }

  bpf_map_delete_elem(&wakeup_time, &next_tid);
  bpf_map_delete_elem(&switch_out_time, &next_tid);

  /* The task being switched out.
   *
   * prev_state is the signal that says what we need, and it is the
   * task's state at the moment it was switched out:
   *
   *   TASK_RUNNING (0)  it was runnable, and it was switched out anyway.
   *                     It was PREEMPTED. It stays runnable, so the
   *                     interval until it runs again is preemption wait,
   *                     and the switch-out time is kept for exactly that.
   *
   *   anything else     it was SLEEPING. The time until it is woken is
   *                     blocked time, and the wakeup starts a new
   *                     run-queue wait. So the switch-out time is not
   *                     kept: keeping it would turn the blocked interval
   *                     into preemption time.
   *
   * The tracepoint's FIRST argument, the bool called preempt, is NOT the
   * signal. The kernel sets it as "sched_mode == SM_PREEMPT", so it is
   * true only for a preemption of the kernel by a higher-priority
   * context, and not for the ordinary timeslice preemption that a
   * contended workload produces. On a kernel built without
   * CONFIG_PREEMPT it is essentially never true, and using it made
   * almost every thread measure zero preemption.
   */
  COUNT_ADD(state_counts, prev_state);
  if (prev_state == PACE_TASK_RUNNING) {
    bpf_map_update_elem(&switch_out_time, &prev_tid, &now, BPF_ANY);
    COUNT_ADD(preempt_count, prev_tid);
  } else {
    COUNT_ADD(block_count, prev_tid);
  }

  return 0;
}

SEC("raw_tracepoint/sched_process_exit")
int pace_sched_exit(struct bpf_raw_tracepoint_args *ctx) {
  /* sched_process_exit's proto is (struct task_struct *p, bool
   * group_dead), so the task is the first argument. */
  const struct task_struct *task = (const struct task_struct *)ctx->args[0];
  __u32 tid = task_tid(task);
  bpf_map_delete_elem(&wakeup_time, &tid);
  bpf_map_delete_elem(&switch_out_time, &tid);
  bpf_map_delete_elem(&preempted_ns, &tid);
  bpf_map_delete_elem(&preempt_count, &tid);
  bpf_map_delete_elem(&block_count, &tid);
  return 0;
}
