/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * Monotonic milliseconds past 2^31, and a send that has one deadline.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sysio.h"
#include "check.h"

/* A 32-bit long cannot hold it, whatever the host: the type is the fix */
_Static_assert(sizeof(rsim_now_ms()) == 8, "monotonic milliseconds must be 64-bit");

int main(void)
{
	current_test = "ms";
	{
		/* 35 days of uptime: 3.0e9 ms, past what a 32-bit long holds */
		struct timespec ts = { 3000000, 999999999 };

		CHECK(rsim_ms_of(&ts) == INT64_C(3000000999));
		CHECK(rsim_ms_of(&ts) + 15000 > rsim_ms_of(&ts));
		CHECK(rsim_now_ms() > 0);
	}

	/* A peer that takes a few bytes every now and then: each of its reads
	 * makes the socket writable again, so a timeout per wait never ran out
	 * while the whole send took as long as the peer liked. */
	current_test = "trickle";
	{
		int sv[2], sz = 4096;
		static char big[256 << 10];
		pid_t pid;
		int64_t t0, took;
		int r;

		CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
		setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
		fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
		pid = fork();
		if (!pid) {
			char b[8192];

			close(sv[0]);
			for (;;) {
				usleep(200000);
				if (read(sv[1], b, sizeof(b)) <= 0)
					_exit(0);
			}
		}
		close(sv[1]);
		t0 = rsim_now_ms();
		r = rsim_send_all(sv[0], big, sizeof(big), 1000);
		took = rsim_now_ms() - t0;
		CHECK(r < 0 && errno == ETIMEDOUT);
		CHECK(took >= 900 && took < 2500);
		close(sv[0]);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
	}

	/* a peer that is gone: said at once, not after the timeout */
	current_test = "hangup";
	{
		int sv[2], sz = 4096;
		static char big[1 << 20];
		int64_t t0;
		int r;

		CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
		setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
		t0 = rsim_now_ms();
		if (!fork()) {
			usleep(200000);
			close(sv[1]);
			_exit(0);
		}
		close(sv[1]);
		r = rsim_send_all(sv[0], big, sizeof(big), 5000);
		CHECK(r < 0 && rsim_now_ms() - t0 < 2000);
		close(sv[0]);
		wait(NULL);
	}

	/* all of it, when the peer reads */
	current_test = "whole";
	{
		int sv[2];
		static char big[200000], got[200000];
		size_t n = 0;
		pid_t pid;

		memset(big, 'x', sizeof(big));
		CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
		fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
		pid = fork();
		if (!pid) {
			close(sv[1]);
			_exit(rsim_send_all(sv[0], big, sizeof(big), 5000) ? 1 : 0);
		}
		close(sv[0]);
		while (n < sizeof(got)) {
			ssize_t r = read(sv[1], got + n, sizeof(got) - n);

			if (r <= 0)
				break;
			n += (size_t)r;
		}
		{
			int st;

			waitpid(pid, &st, 0);
			CHECK(WIFEXITED(st) && !WEXITSTATUS(st));
		}
		CHECK(n == sizeof(big) && !memcmp(big, got, n));
		close(sv[1]);
	}
	return check_done("test_sysio");
}
