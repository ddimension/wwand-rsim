/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_WBSM_H
#define RSIM_WBSM_H

#include <stddef.h>

struct wbsm_cfg {
	const char *serial;     /* the USB serial number, NULL = the first one */
	unsigned clock_khz;     /* 3580, 3680 or 6000 */
	int smartmouse;         /* 1 = Smartmouse wiring, 0 = Phoenix */
};

/* Set the reader's clock and mode, and find the serial port it has once the
 * kernel's ftdi_sio driver owns it. 0 with `tty` filled, or -1 (logged). */
int wbsm_prepare(const struct wbsm_cfg *cfg, char *tty, size_t ttylen);

#endif
