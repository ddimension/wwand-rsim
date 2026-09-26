/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_LOG_H
#define RSIM_LOG_H

#include <syslog.h>

/* stdout is the protocol channel, so every diagnostic goes to stderr (the
 * plugin forwards it) and, with -s, to syslog as well */
void log_init(int verbose, int use_syslog);
void log_msg(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define log_err(...)	log_msg(LOG_ERR, __VA_ARGS__)
#define log_warn(...)	log_msg(LOG_WARNING, __VA_ARGS__)
#define log_notice(...)	log_msg(LOG_NOTICE, __VA_ARGS__)
#define log_dbg(...)	log_msg(LOG_DEBUG, __VA_ARGS__)

#endif
