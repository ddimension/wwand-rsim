/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_TEST_CHECK_H
#define RSIM_TEST_CHECK_H

#include <stdio.h>

static int checks, failures;

#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "%s:%d: %s: FAILED %s\n",		\
			__FILE__, __LINE__, current_test, #cond);	\
	}								\
} while (0)

static const char *current_test = "";

static inline int check_done(const char *suite)
{
	printf("%s: %d checks, %d failed\n", suite, checks, failures);
	return failures ? 1 : 0;
}

#endif
