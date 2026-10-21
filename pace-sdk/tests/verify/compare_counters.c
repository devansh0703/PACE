// SPDX-License-Identifier: BUSL-1.1
// Compare the kernel's run-queue delay counter against the eBPF tier's
// measurement of the same quantity, for the same threads, in the same
// window.
//
// The kernel's counter approximates the preemption wait as the time from
// queueing to dequeuing. The eBPF tier measures the interval from the
// thread's sched_wakeup to the sched_switch that ran it. Where the two
// disagree, the kernel's counter loses the interval a thread spent
// queued and then migrated or requeued.
//
// The two are cumulative counters, so the totals are compared.
#include "pace/pace.h"
#include "bpf/pace_bpf.h"

#include <dirent.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WORKERS 48

static volatile unsigned long g_sink;

static void *worker(void *arg) {
  (void)arg;
  unsigned long local = 0;
  /* Long enough to still be running when the counters are read. The
   * enumeration happens after a sleep, so the workers have to outlive
   * that sleep by a wide margin. */
  for (long i = 0; i < 40000000000L; i++) local += (unsigned long)i;
  g_sink += local;
  return NULL;
}

int main(void) {
  struct pace_bpf *bpf = pace_bpf_open();
  if (!pace_bpf_is_available(bpf)) {
    printf("the eBPF tier is unavailable\n");
    return 1;
  }
  printf("the eBPF tier is attached\n");

  pthread_t threads[WORKERS];
  int started = 0;
  for (int i = 0; i < WORKERS; i++) {
    if (pthread_create(&threads[i], NULL, worker, NULL) == 0) started++;
  }

  /* Let them contend for a while. */
  sleep(2);

  /* The thread id is the name of the directory under /proc/self/task, so
   * the directory is enumerated rather than indexed. A pthread_t is not a
   * thread id, and the TIDs are not a dense range either. */
  DIR *dir = opendir("/proc/self/task");
  if (!dir) {
    printf("cannot open /proc/self/task\n");
    return 1;
  }

  long compared = 0;
  double kd_total = 0.0, bpf_total = 0.0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;

    char path[256];
    snprintf(path, sizeof path, "/proc/self/task/%s/schedstat", entry->d_name);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    unsigned long long oncpu = 0, rundelay = 0, slices = 0;
    int got = fscanf(f, "%llu %llu %llu", &oncpu, &rundelay, &slices);
    fclose(f);
    if (got != 3) continue;

    uint64_t tid = (uint64_t)strtoull(entry->d_name, NULL, 10);
    uint64_t bpf_preempted = 0;
    if (pace_bpf_read_preempted_ns(bpf, tid, &bpf_preempted) != PACE_OK) continue;

    if (compared < 8) {
      printf("  thread %-8s the kernel's run_delay %12llu ns   the eBPF measurement %12llu ns\n",
             entry->d_name, rundelay, (unsigned long long)bpf_preempted);
    }
    kd_total += (double)rundelay;
    bpf_total += (double)bpf_preempted;
    compared++;
  }
  closedir(dir);

  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);

  printf("%ld threads compared\n", compared);
  printf("  the kernel's total run_delay: %.0f ns\n", kd_total);
  printf("  the eBPF tier's total:         %.0f ns\n", bpf_total);
  if (kd_total > 0.0) {
    printf("  the eBPF measurement is %+.2f %% of the kernel's counter\n",
           (bpf_total - kd_total) / kd_total * 100.0);
  }

  /* What the tier actually observed. The state counts say whether a
   * preempted task reported TASK_RUNNING when it was switched out, and
   * the wait counts say how many switch-ins had a timestamp to measure
   * from. Between them they account for the measurement, so a
   * difference between two machines is attributable rather than
   * mysterious. */
  uint64_t switch_ins = 0, with_timestamp = 0;
  pace_bpf_read_diagnostic(bpf, "wait_counts", 0, &switch_ins);
  pace_bpf_read_diagnostic(bpf, "wait_counts", 1, &with_timestamp);
  printf("  the switch-ins the tier saw: %" PRIu64 ", of which %" PRIu64
         " had a timestamp to measure from\n",
         switch_ins, with_timestamp);

  printf("  the switch-outs the tier saw, by the task's state:\n");
  for (uint64_t state = 0; state < 8; state++) {
    uint64_t count = 0;
    pace_bpf_read_diagnostic(bpf, "state_counts", state, &count);
    if (count == 0) continue;
    const char *name = "another state";
    if (state == 0) name = "TASK_RUNNING, so it was runnable and preempted";
    else if (state == 1) name = "TASK_INTERRUPTIBLE, so it was sleeping";
    else if (state == 2) name = "TASK_UNINTERRUPTIBLE, so it was sleeping";
    printf("    state %" PRIu64 ": %" PRIu64 " -- %s\n", state, count, name);
  }

  pace_bpf_close(bpf);
  return 0;
}
