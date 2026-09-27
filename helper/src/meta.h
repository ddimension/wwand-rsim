/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_META_H
#define RSIM_META_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "json.h"

/* What sysfs says about a serial port and the USB device it hangs on —
 * read only, nothing is opened. sysroot: prefix for /sys and /dev (tests),
 * "" for the real ones. */
struct tty_meta {
	char name[64];		/* ttyUSB0 */
	char driver[64];	/* option1, ftdi_sio, cdc_acm, ... */
	char vid[8], pid[8];
	char ifnum[8];		/* the USB interface number */
	char ifname[128];	/* its string descriptor, when it has one */
	char iclass[4], iproto[4];	/* bInterfaceClass / Protocol, hex */
	char usbdir[PATH_MAX];	/* the USB device's sysfs directory */
	char by_id[PATH_MAX];	/* /dev/serial/by-id/… naming it, if any */
	char by_path[PATH_MAX];
};

/* What the port is FOR, from what the device itself says — "diag" for a
 * diagnostic port (never to be spoken AT to), "" when it does not say.
 * Huawei marks its vendor-specific interfaces in bInterfaceProtocol:
 * 01 modem, 02 PC UI (both AT), 03 DIAG (ModemManager's huawei plugin;
 * E392 12d1:1506 reports ff/01/03 on if02, which does not answer AT —
 * seen on 245, 2026-09-27; that it IS the DIAG port rests on that
 * convention, not on a QCDM probe). */
const char *meta_tty_role(const struct tty_meta *m);

/* dev: /dev/ttyUSB0, a /dev/serial/by-id link, or a bare ttyUSB0. 0, or -1
 * when it is not a tty sysfs knows */
int meta_tty_read(const char *sysroot, const char *dev, struct tty_meta *m);

/* driver, usb (vid:pid), interface, interface_name, the usb_* fields,
 * by_id, by_path */
void meta_tty_write(struct jw *w, const struct tty_meta *m);

/* usb_manufacturer, usb_product, usb_serial, usb_path (e.g. 1-1.2),
 * usb_speed_mbps, usb_version (bcdDevice) of a USB device's sysfs dir */
void meta_usb_write(struct jw *w, const char *usbdir);

/* the sysfs dir of the USB device at bus / port chain (libusb's view), and
 * the driver bound to its interface 0 ("" none); -1 when it is not there */
int meta_usb_dir(const char *sysroot, int bus, const uint8_t *ports, int nports, char *out, size_t cap);
void meta_usb_driver(const char *usbdir, char *out, size_t cap);

/* the first line of a sysfs attribute, "" when there is none */
void meta_attr(const char *dir, const char *name, char *out, size_t cap);

#endif
