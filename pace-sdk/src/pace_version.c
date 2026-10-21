/* SPDX-License-Identifier: BUSL-1.1
 * PACE — version information. */
#include "pace/pace.h"

const char *pace_version_string(void) {
  return "0.1.0";
}

unsigned pace_version_number(void) { return (unsigned)PACE_VERSION_NUMBER; }
