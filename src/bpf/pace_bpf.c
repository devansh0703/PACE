/* SPDX-License-Identifier: BUSL-1.1
 * PACE — user-space side of the eBPF tier.
 *
 * Loads the sched tracepoint program, exposes the per-thread wakeup times
 * and switch counters, and lets the Linux backend read them.
 *
 * The BPF tier is optional. It needs a kernel with BTF and tracepoints
 * (5.8+ for the BTF CO-RE relocation, 4.17+ for the sched tracepoints with
 * the reason field), and it needs privilege to attach to the tracepoints.
 * When any of that is missing, the tier reports itself unavailable and the
 * procfs tier carries the measurement.
 */
#include "pace_bpf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "pace_sched.skel.h"

struct pace_bpf {
  struct pace_sched_bpf *skel;
  int available;
  int reason;
};

const char *pace_bpf_unavailable_reason(int reason) {
  switch (reason) {
    case PACE_BPF_OK: return "available";
    case PACE_BPF_NO_BTF: return "the kernel exposes no BTF (/sys/kernel/btf/vmlinux)";
    case PACE_BPF_NO_PERMISSION: return "attaching to the sched tracepoints needs privilege";
    case PACE_BPF_LOAD_FAILED: return "the BPF program could not be loaded";
    case PACE_BPF_NO_TRACEPOINTS: return "the kernel exposes no sched tracepoints";
    default: return "unknown";
  }
}

struct pace_bpf *pace_bpf_open(void) {
  struct pace_bpf *bpf = (struct pace_bpf *)calloc(1, sizeof *bpf);
  if (!bpf) return NULL;

  libbpf_set_print(NULL);

  struct pace_sched_bpf *skel = pace_sched_bpf__open();
  if (!skel) {
    bpf->reason = PACE_BPF_LOAD_FAILED;
    return bpf;
  }

  int err = pace_sched_bpf__load(skel);
  if (err) {
    bpf->reason = PACE_BPF_LOAD_FAILED;
    pace_sched_bpf__destroy(skel);
    return bpf;
  }

  err = pace_sched_bpf__attach(skel);
  if (err) {
    bpf->reason = PACE_BPF_NO_PERMISSION;
    pace_sched_bpf__destroy(skel);
    return bpf;
  }

  bpf->skel = skel;
  bpf->available = 1;
  bpf->reason = PACE_BPF_OK;
  return bpf;
}

void pace_bpf_close(struct pace_bpf *bpf) {
  if (!bpf) return;
  if (bpf->skel) pace_sched_bpf__destroy(bpf->skel);
  free(bpf);
}

int pace_bpf_is_available(const struct pace_bpf *bpf) {
  return bpf && bpf->available;
}

/**
 * Read one thread's scheduler state from the BPF maps.
 *
 * Returns PACE_OK and fills the outputs, or PACE_ERR_NOT_FOUND when the
 * thread has no entry (it has not been scheduled since the program loaded).
 */
pace_status pace_bpf_read_thread(struct pace_bpf *bpf, uint64_t tid, uint64_t *wakeup_ns,
                                  uint64_t *preempt_count, uint64_t *block_count) {
  if (!bpf || !bpf->available) return PACE_ERR_UNSUPPORTED;

  __u32 key = (__u32)tid;
  __u64 value = 0;

  if (wakeup_ns) {
    value = 0;
    if (bpf_map_lookup_elem(bpf_map__fd(bpf->skel->maps.wakeup_time), &key, &value) != 0) {
      *wakeup_ns = 0;
    } else {
      *wakeup_ns = value;
    }
  }

  if (preempt_count) {
    value = 0;
    if (bpf_map_lookup_elem(bpf_map__fd(bpf->skel->maps.preempt_count), &key, &value) != 0) {
      *preempt_count = 0;
    } else {
      *preempt_count = value;
    }
  }

  if (block_count) {
    value = 0;
    if (bpf_map_lookup_elem(bpf_map__fd(bpf->skel->maps.block_count), &key, &value) != 0) {
      *block_count = 0;
    } else {
      *block_count = value;
    }
  }

  return PACE_OK;
}
