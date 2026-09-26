/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_WBSM_H
#define RSIM_WBSM_H

#include <stddef.h>

#include "phoenix.h"

struct wbsm_cfg {
	const char *serial;     /* the USB serial number, NULL = the first one */
	unsigned clock_khz;     /* 3580, 3680 or 6000 */
	int smartmouse;         /* 1 = Smartmouse wiring, 0 = Phoenix */
};

/* Sets the reader's clock and mode and returns its serial side as a Phoenix
 * transport over libusb (ftdi_usb.c) -- no kernel serial driver involved.
 * name is for logs and must outlive the transport. NULL on failure
 * (logged). */
struct phx_io *wbsm_open(const struct wbsm_cfg *cfg, const char *name);

/* every Smartmouse USB attached, one JSON line each ({"backend":"wbsm",
 * "spec":"wbsm:<serial>","serial"}); the device is only looked at, its mode
 * and clock stay as they are. Returns how many. */
int wbsm_list(void);

#endif
