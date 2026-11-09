// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <time.h>

// libusb's null USB backend uses the shared POSIX event/timeout machinery.
// Darwin normally defines these clocks in its IOKit backend; iOS uses the
// public clock API while host USB passthrough remains unavailable.
void usbi_get_monotonic_time(struct timespec* value) {
    if (clock_gettime(CLOCK_MONOTONIC, value) != 0) abort();
}

void usbi_get_real_time(struct timespec* value) {
    if (clock_gettime(CLOCK_REALTIME, value) != 0) abort();
}
