/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <stdarg.h>
#include <stdio.h>

#include "log.h"

static int log_verbose;
static int log_syslog;

void log_init(int verbose, int use_syslog)
{
	log_verbose = verbose;
	log_syslog = use_syslog;
	if (use_syslog)
		openlog("rsim-card", LOG_PID, LOG_DAEMON);
}

void log_msg(int prio, const char *fmt, ...)
{
	va_list ap;

	if (prio == LOG_DEBUG && !log_verbose)
		return;
	va_start(ap, fmt);
	fputs("rsim-card: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	if (log_syslog) {
		va_start(ap, fmt);
		vsyslog(prio, fmt, ap);
		va_end(ap);
	}
}
