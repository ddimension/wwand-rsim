/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_SYSIO_H
#define RSIM_SYSIO_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Monotonic milliseconds, and every deadline built from them, are 64-bit:
 * in a 32-bit long the product tv_sec * 1000 wraps after 24.8 days of
 * uptime (2^31 ms) — the 32-bit MIPS and ARM routers this runs on stay up
 * longer than that, and a wrapped deadline is in the past or weeks away. */
int64_t rsim_ms_of(const struct timespec *ts);
int64_t rsim_now_ms(void);

/* All n bytes to a non-blocking socket within timeout_ms IN TOTAL — a
 * deadline for the whole send, not per wait: a peer that takes a byte now
 * and then would otherwise hold the helper, and the stdin it serves, for
 * as long as it likes. 0 sent, -1 not (errno: ETIMEDOUT, EPIPE for a peer
 * that hung up or errored, or send's own). */
int rsim_send_all(int fd, const void *buf, size_t n, int timeout_ms);

#endif
