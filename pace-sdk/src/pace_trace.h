/* SPDX-License-Identifier: BUSL-1.1
 * PACE — internal trace-writer interface. */
#ifndef PACE_INTERNAL_TRACE_H
#define PACE_INTERNAL_TRACE_H

#include "pace/pace.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pace_trace pace_trace;

pace_status pace_trace_open(const char *path, pace_trace **out_trace);
void pace_trace_close(pace_trace *trace);
pace_status pace_trace_write_sample(pace_trace *trace, uint64_t thread_id,
                                    const pace_sample *sample);
int pace_trace_flush(pace_trace *trace);

#ifdef __cplusplus
}
#endif

#endif /* PACE_INTERNAL_TRACE_H */
