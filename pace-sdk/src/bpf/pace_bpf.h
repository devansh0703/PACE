/* SPDX-License-Identifier: BUSL-1.1
 * PACE — the eBPF tier's interface.
 */
#ifndef PACE_INTERNAL_BPF_H
#define PACE_INTERNAL_BPF_H

#include "pace/pace.h"

#include <stdint.h>

/* Why the eBPF tier is unavailable. */
enum {
  PACE_BPF_OK = 0,
  PACE_BPF_NO_BTF = 1,
  PACE_BPF_NO_PERMISSION = 2,
  PACE_BPF_LOAD_FAILED = 3,
  PACE_BPF_NO_TRACEPOINTS = 4
};

/** The eBPF tier handle. Opaque. */
struct pace_bpf;

/**
 * Load and attach the sched tracepoint program.
 *
 * Never fails hard: when the program cannot be loaded, the returned handle
 * reports itself unavailable with a reason, and the caller falls back to the
 * procfs tier. Returns NULL only on allocation failure.
 */
struct pace_bpf *pace_bpf_open(void);

/** Unload the program. Safe to call with NULL. */
void pace_bpf_close(struct pace_bpf *bpf);

/** True when the tier is loaded and attached. */
int pace_bpf_is_available(const struct pace_bpf *bpf);

/** A human-readable reason for unavailability. Never NULL. */
const char *pace_bpf_unavailable_reason(int reason);

/**
 * Read one thread's scheduler state.
 *
 * @param wakeup_ns     when the thread last became runnable, 0 if it has not.
 * @param preempt_count times it was descheduled while runnable.
 * @param block_count   times it blocked.
 */
pace_status pace_bpf_read_thread(struct pace_bpf *bpf, uint64_t tid, uint64_t *wakeup_ns,
                                  uint64_t *preempt_count, uint64_t *block_count);

/**
 * Read the cumulative time a thread spent runnable but not running.
 *
 * This is the preemption wait, measured from the scheduler's own
 * sched_wakeup and sched_switch events: the interval between the moment
 * the thread became runnable and the moment it actually ran.
 *
 * The kernel's /proc/<tid>/schedstat run_delay counter measures the same
 * phenomenon, but as the time from queueing to dequeuing, so a thread
 * that is woken, queued, and then migrated or requeued loses the
 * interval. Measured on a CI runner, the kernel's counter undercounted by
 * 80 %.
 *
 * The counter is cumulative, exactly like run_delay, so the user space
 * takes the same difference between its baseline and the sample.
 *
 * Returns PACE_OK with *preempted_ns set to 0 when the thread has no
 * entry yet, and PACE_ERR_NOT_FOUND when the tier is unavailable.
 */
pace_status pace_bpf_read_preempted_ns(struct pace_bpf *bpf, uint64_t tid,
                                        uint64_t *preempted_ns);

/**
 * Read a diagnostic counter from the tier's maps.
 *
 * The tier records how many switch-outs it saw with each prev_state, and
 * how many switch-ins found a "runnable since" timestamp. Those counts
 * answer the question of what the tier actually observed on a given
 * kernel, which is the question that has to be answered when the tier's
 * measurement differs between machines.
 *
 * @param map_name "state_counts" or "wait_counts".
 * @param key      the key to read. For "state_counts" it is the
 *                 prev_state value; for "wait_counts" it is 0 for the
 *                 switch-ins and 1 for the switch-ins that had a
 *                 timestamp.
 * @param out      the counter.
 */
pace_status pace_bpf_read_diagnostic(struct pace_bpf *bpf, const char *map_name,
                                     uint64_t key, uint64_t *out);

#endif /* PACE_INTERNAL_BPF_H */
