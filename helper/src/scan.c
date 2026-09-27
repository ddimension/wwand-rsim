/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * `rsim-card --list`: what this machine offers as a card source, one JSON
 * line per candidate and a closing {"done":true,"backends":...} with the
 * backends this build has. Nothing is opened that could be disturbed: PC/SC
 * readers are what pcscd already knows, a Smartmouse USB is only described,
 * serial ports are read from sysfs — never written to, because one of them
 * may be the AT port of a modem in use; paired phones are what BlueZ has
 * stored about them (a phone is not called up: that would take its SIM); on a
 * router with wwand-rsim, the cards of its modems (wwandctl rsim proxy).
 */
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json.h"
#include "meta.h"
#include "scan.h"
#ifdef WITH_LIBUSB
#include "wbsm.h"
#endif
#ifdef WITH_PCSC
#include "pcsc.h"
#endif
#ifdef WITH_BLUETOOTH
#include "bt.h"
#endif

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
		struct tty_meta m;
		char spec[300];
		const char *hint;
		struct jw w;

		if (strncmp(e->d_name, "ttyUSB", 6) && strncmp(e->d_name, "ttyACM", 6))
			continue;
		if (meta_tty_read(sysroot, e->d_name, &m) < 0)
			continue;

#ifdef WITH_LIBUSB
		/* the Smartmouse USB behind a kernel serial driver: listed as wbsm:
		 * already, which also sets its clock and mode — its tty would only
		 * be the same reader without them */
		if (!strcmp(m.vid, "104f") && !strcmp(m.pid, "0002"))
			continue;
#endif
		/* a diagnostic port speaks no AT: listed as what it is, not as a
		 * reader (its spec stays empty) */
		hint = !strcmp(meta_tty_role(&m), "diag") ? "diag" : hint_of(m.driver);
		if (!strcmp(hint, "diag"))
			spec[0] = '\0';
		else
			snprintf(spec, sizeof(spec), "%s:/dev/%s", hint[0] ? hint : "phoenix", e->d_name);
		jw_begin(&w, stdout);
		jw_str(&w, "backend", "tty");
		jw_str(&w, "spec", spec);
		{
			char devp[80];

			snprintf(devp, sizeof(devp), "/dev/%.60s", e->d_name);
			jw_str(&w, "device", devp);
		}
		jw_str(&w, "hint", hint);
		meta_tty_write(&w, &m);
		jw_end(&w);
		n++;
	}
	closedir(d);
	return n;
}

/* On a router with wwand and wwand-rsim: the cards of its modems, which
 * another router's wwand-rsim can borrow through `wwandctl rsim proxy` — with
 * the wwand_sim settings of each. Its lines are passed on as they are.
 * RSIM_TEST_WWAND_LIST: the command to run instead (tests). -1 when there is
 * no wwand-rsim here. */
static int wwand_list(void)
{
	const char *cmd = getenv("RSIM_TEST_WWAND_LIST");
	char line[8192];
	FILE *p;
	int n = 0;

	if (!cmd) {
		if (access("/usr/share/ucode/wwand/ctl/rsim.uc", R_OK) || access("/usr/bin/wwandctl", X_OK))
			return -1;
		cmd = "/usr/bin/wwandctl rsim proxy --list 2>/dev/null";
	}
	fflush(stdout);
	if (!(p = popen(cmd, "r")))
		return -1;
	while (fgets(line, sizeof(line), p)) {
		size_t l = strlen(line);

		/* a line cut by the buffer is not passed on in pieces */
		if (!l || line[l - 1] != '\n') {
			int c;

			while ((c = fgetc(p)) != EOF && c != '\n')
				;
			continue;
		}
		if (line[0] != '{')
			continue;
		fputs(line, stdout);
		n++;
	}
	pclose(p);
	fflush(stdout);
	return n;
}

int scan_run(const char *sysroot)
{
	struct jw w;
	char backends[64] = "phoenix,at", note[PATH_MAX + 80] = "", adapters[400] = "";

#ifdef WITH_LIBUSB
	strcat(backends, ",wbsm");
	wbsm_list();
#endif
#ifdef WITH_PCSC
	strcat(backends, ",pcsc");
	pcsc_list();
#endif
#ifdef WITH_BLUETOOTH
	strcat(backends, ",bt");
	bt_list(sysroot ? sysroot : "", note, sizeof(note), adapters, sizeof(adapters));
#endif
	tty_list(sysroot ? sysroot : "");
	if (wwand_list() >= 0)
		strcat(backends, ",wwand");

	jw_begin(&w, stdout);
	jw_bool(&w, "done", true);
	jw_str(&w, "backends", backends);
	if (note[0])
		jw_str(&w, "note", note);
	jw_opt(&w, "bt_adapters", adapters);
	jw_end(&w);
	return 0;
}
