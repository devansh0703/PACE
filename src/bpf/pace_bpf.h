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

#endif /* PACE_INTERNAL_BPF_H */
