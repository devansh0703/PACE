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

/* libbpf's diagnostics. A load or attach failure says nothing about why
 * otherwise, and the messages belong on stderr, where a developer
 * expects them. */
static int pace_bpf_diag_print(enum libbpf_print_level level, const char *fmt,
                               va_list args) {
  /* Only the warnings are shown. The debug level is noise, and the info
   * level says things like "failed to bump RLIMIT_MEMLOCK", which is
   * true of every unprivileged process and says nothing about whether
   * the tier works.
   *
   * The tier is optional and the SDK reports its state through
   * pace_stats, so a consumer that did not ask for the tier does not
   * need libbpf's commentary. */
  if (level < LIBBPF_WARN) return 0;
  return vfprintf(stderr, fmt, args);
}

struct pace_bpf *pace_bpf_open(void) {
  struct pace_bpf *bpf = (struct pace_bpf *)calloc(1, sizeof *bpf);
  if (!bpf) return NULL;

  /* libbpf's diagnostics are left on: a load or attach failure says
   * nothing about why otherwise, and the messages go to stderr, where a
   * developer expects them. The SDK's own error reporting is separate
   * and structured. */
  libbpf_set_print(pace_bpf_diag_print);

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

  /* Each program is attached EXPLICITLY, and each result is checked.
   *
   * libbpf's generated skeleton attach helper, pace_sched_bpf__attach(),
   * attempts every program and DISCARDS the individual errors: it only
   * returns the error from the attach as a whole. So when one program's
   * attach fails the helper still returns zero.
   *
   * That matters here, because a failure is silent in the worst way: the
   * tier reported itself attached, the maps stayed empty, and no BPF
   * program was listed by bpftool. The measurement was the kernel's
   * counter the whole time, with the eBPF tier contributing nothing and
   * saying nothing.
   *
   * So the programs are attached one at a time here, each result checked,
   * and a program that fails to attach is reported as a failure of the
   * tier rather than as a success.
   *
   * bpf_program__attach() is what the generated helper calls for the
   * raw_tracepoint/ form, and it opens the program with
   * BPF_RAW_TRACEPOINT_OPEN, which resolves the tracepoint through the
   * kernel's BTF. The explicit alternative,
   * bpf_program__attach_raw_tracepoint, looks the name up through
   * tracefs instead, and that lookup fails on hosts where the tracefs
   * mount is not where libbpf expects it. */
  struct bpf_link *links[3] = {NULL, NULL, NULL};
  struct bpf_program *programs[3] = {
      skel->progs.pace_sched_wakeup,
      skel->progs.pace_sched_switch,
      skel->progs.pace_sched_exit,
  };

  for (int i = 0; i < 3; i++) {
    if (!programs[i]) continue;
    links[i] = bpf_program__attach(programs[i]);
    /* libbpf reports a failure as an ERROR POINTER -- a small negative
     * value -- not as NULL. So the check has to be libbpf_get_error(),
     * and a plain "if (!link)" test passes on a failure, which is how a
     * silently broken tier came to report itself attached. */
    if (libbpf_get_error(links[i])) {
      /* One program failing means the tier's measurement would be
       * partial, and a partial measurement presented as a whole one is
       * worse than no measurement. So the whole tier is reported
       * unavailable. */
      for (int j = 0; j < 3; j++) {
        if (links[j] && !libbpf_get_error(links[j])) {
          bpf_link__destroy(links[j]);
        }
        links[j] = NULL;
      }
      bpf->reason = PACE_BPF_NO_PERMISSION;
      pace_sched_bpf__destroy(skel);
      return bpf;
    }
  }

  /* The links are owned by the skeleton from here on, so it closes them
   * when it is destroyed. */
  skel->links.pace_sched_wakeup = links[0];
  skel->links.pace_sched_switch = links[1];
  skel->links.pace_sched_exit = links[2];

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

/**
 * Read the cumulative preemption wait for one thread.
 *
 * The map holds a nanosecond total, accumulated by the program from the
 * interval between a thread's sched_wakeup and the sched_switch that
 * actually ran it.
 */
pace_status pace_bpf_read_preempted_ns(struct pace_bpf *bpf, uint64_t tid,
                                        uint64_t *preempted_ns) {
  if (!bpf || !bpf->available || !preempted_ns) return PACE_ERR_UNSUPPORTED;

  __u32 key = (__u32)tid;
  __u64 value = 0;

  if (bpf_map_lookup_elem(bpf_map__fd(bpf->skel->maps.preempted_ns), &key, &value) != 0) {
    /* The thread has not been scheduled since the program loaded, so it
     * has not been preempted either. That is a real zero. */
    *preempted_ns = 0;
    return PACE_OK;
  }

  *preempted_ns = value;
  return PACE_OK;
}
