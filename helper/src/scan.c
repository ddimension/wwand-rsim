/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * `rsim-card --list`: what this machine offers as a card source, one JSON
 * line per candidate and a closing {"done":true,"backends":...} with the
 * backends this build has. Nothing is opened that could be disturbed: PC/SC
 * readers are what pcscd already knows, a Smartmouse USB is only described,
 * serial ports are read from sysfs — never written to, because one of them
 * may be the AT port of a modem in use.
 */
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json.h"
#include "scan.h"
#ifdef WITH_LIBUSB
#include "wbsm.h"
#endif
#ifdef WITH_PCSC
#include "pcsc.h"
#endif

/* the first line of a sysfs attribute, "" when there is none */
static void attr(const char *dir, const char *name, char *out, size_t cap)
{
	char p[PATH_MAX];
	FILE *f;

	out[0] = '\0';
	snprintf(p, sizeof(p), "%s/%s", dir, name);
	if (!(f = fopen(p, "r")))
		return;
	if (fgets(out, (int)cap, f))
		out[strcspn(out, "\r\n")] = '\0';
	fclose(f);
}

/* What a serial port most likely is, from its kernel driver: a USB-serial
 * adapter is what a Phoenix reader hangs on, a modem driver's port may be an
 * AT port. Only a hint — the port is not asked. */
static const char *hint_of(const char *driver)
{
	static const char *const serial[] = { "ftdi_sio", "pl2303", "cp210x", "ch341", "ch341-uart", NULL };
	/* the names in sysfs, which are not always the module's: the option
	 * driver registers as "option1" (drivers/usb/serial/option.c:2575,
	 * linux 6.18.41; HW-read on 245, 2026-09-27). usb_wwan is a library
	 * those drivers use, not a driver of its own. */
	static const char *const modem[] = { "option1", "option", "qcserial", "cdc_acm", "sierra", "qcaux", NULL };
	int i;

	for (i = 0; serial[i]; i++)
		if (!strcmp(driver, serial[i]))
			return "phoenix";
	for (i = 0; modem[i]; i++)
		if (!strcmp(driver, modem[i]))
			return "at";
	return "";
}

static int tty_list(const char *sysroot)
{
	char base[PATH_MAX];
	DIR *d;
	struct dirent *e;
	int n = 0;

	snprintf(base, sizeof(base), "%s/sys/class/tty", sysroot);
	if (!(d = opendir(base)))
		return 0;
	while ((e = readdir(d))) {
		char dev[PATH_MAX], link[PATH_MAX], real[PATH_MAX], usb[PATH_MAX];
		char driver[64] = "", vid[8] = "", pid[8] = "", ifnum[8] = "", spec[300], ids[20];
		const char *hint;
		ssize_t l;
		struct jw w;

		if (strncmp(e->d_name, "ttyUSB", 6) && strncmp(e->d_name, "ttyACM", 6))
			continue;

		/* a path that does not fit is not a port of ours to describe */
		if (snprintf(dev, sizeof(dev), "%s/%s/device", base, e->d_name) >= (int)sizeof(dev) ||
		    snprintf(link, sizeof(link), "%s/driver", dev) >= (int)sizeof(link))
			continue;
		if ((l = readlink(link, real, sizeof(real) - 1)) > 0) {
			const char *b;

			real[l] = '\0';
			b = strrchr(real, '/') ? strrchr(real, '/') + 1 : real;
			if (strlen(b) < sizeof(driver))
				strcpy(driver, b);
		}

		/* device -> the USB interface (ttyACM) or a port below it
		 * (ttyUSB): the interface number is on the interface, the ids on
		 * the USB device one or two levels up */
		if (realpath(dev, real)) {
			char *cut;

			attr(real, "bInterfaceNumber", ifnum, sizeof(ifnum));
			snprintf(usb, sizeof(usb), "%s", real);
			for (int up = 0; up < 3 && !vid[0]; up++) {
				if (!ifnum[0])
					attr(usb, "bInterfaceNumber", ifnum, sizeof(ifnum));
				attr(usb, "idVendor", vid, sizeof(vid));
				attr(usb, "idProduct", pid, sizeof(pid));
				if (!vid[0] && (cut = strrchr(usb, '/')))
					*cut = '\0';
			}
		}

#ifdef WITH_LIBUSB
		/* the Smartmouse USB behind a kernel serial driver: listed as wbsm:
		 * already, which also sets its clock and mode — its tty would only
		 * be the same reader without them */
		if (!strcmp(vid, "104f") && !strcmp(pid, "0002"))
			continue;
#endif
		hint = hint_of(driver);
		snprintf(spec, sizeof(spec), "%s:/dev/%s", hint[0] ? hint : "phoenix", e->d_name);
		snprintf(ids, sizeof(ids), "%s:%s", vid, pid);
		jw_begin(&w, stdout);
		jw_str(&w, "backend", "tty");
		jw_str(&w, "spec", spec);
		jw_str(&w, "device", spec + strcspn(spec, ":") + 1);
		jw_str(&w, "driver", driver);
		jw_str(&w, "usb", vid[0] ? ids : "");
		jw_str(&w, "interface", ifnum);
		jw_str(&w, "hint", hint);
		jw_end(&w);
		n++;
	}
	closedir(d);
	return n;
}

int scan_run(const char *sysroot)
{
	struct jw w;
	char backends[64] = "phoenix,at";

#ifdef WITH_LIBUSB
	strcat(backends, ",wbsm");
	wbsm_list();
#endif
#ifdef WITH_PCSC
	strcat(backends, ",pcsc");
	pcsc_list();
#endif
	tty_list(sysroot ? sysroot : "");

	jw_begin(&w, stdout);
	jw_bool(&w, "done", true);
	jw_str(&w, "backends", backends);
	jw_end(&w);
	return 0;
}
