/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

#include "sysio.h"

int64_t rsim_ms_of(const struct timespec *ts)
{
	return (int64_t)ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
}

int64_t rsim_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return rsim_ms_of(&ts);
}

int rsim_send_all(int fd, const void *buf, size_t n, int timeout_ms)
{
	const uint8_t *b = buf;
	int64_t deadline = rsim_now_ms() + timeout_ms;

	while (n) {
		ssize_t w = send(fd, b, n, MSG_NOSIGNAL);

		if (w > 0) {
			b += w;
			n -= (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
			return -1;

		for (;;) {
			struct pollfd pfd = { .fd = fd, .events = POLLOUT };
			int64_t left = deadline - rsim_now_ms();
			int r;

			if (left <= 0) {
				errno = ETIMEDOUT;
				return -1;
			}
			r = poll(&pfd, 1, (int)left);
			if (r < 0 && errno == EINTR)
				continue;
			if (r < 0)
				return -1;
			if (!r)
				continue;	/* the deadline decides, above */
			/* a peer that hung up or a socket in error does not
			 * become writable: said now, not after the timeout */
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				errno = EPIPE;
				return -1;
			}
			break;
		}
	}
	return 0;
}
