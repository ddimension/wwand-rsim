/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_FTDI_USB_H
#define RSIM_FTDI_USB_H

#include <stdbool.h>

#include <libusb.h>

#include "phoenix.h"

/* The Phoenix transport on an FTDI FT232BM driven over libusb alone, for a
 * kernel without ftdi_sio. Takes ownership of ctx and h, whose interface 0
 * the caller has claimed; `detached` says a kernel driver was detached
 * from it and gets it back on close. name is for logs and must outlive the
 * transport. NULL on failure, with everything released. */
struct phx_io *ftdi_io_open(libusb_context *ctx, libusb_device_handle *h,
			    bool detached, const char *name);

#endif
