/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_PHOENIX_H
#define RSIM_PHOENIX_H

#include "backend.h"

enum phoenix_reset {
	PHX_RESET_AUTO,
	PHX_RESET_RTS,
	PHX_RESET_RTS_INV,
	PHX_RESET_DTR,
	PHX_RESET_DTR_INV,
};

enum phoenix_detect {
	PHX_DETECT_NONE,
	PHX_DETECT_CTS,
	PHX_DETECT_DSR,
	PHX_DETECT_CD,
};

struct phoenix_cfg {
	const char *dev;
	unsigned clock_khz;
	enum phoenix_reset reset;
	enum phoenix_detect detect;
	unsigned atr_timeout_ms;
};

struct rsim_backend *phoenix_open(const struct phoenix_cfg *cfg);

/* in phoenix_baud.c, the only file that sees <asm/termbits.h> (its struct
 * termios clashes with the libc one): sets an arbitrary rate with
 * termios2/BOTHER. 0 or -errno. */
int phoenix_set_baud(int fd, unsigned baud);

#endif
