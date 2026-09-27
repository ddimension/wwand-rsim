#define _GNU_SOURCE	/* strcasestr */
/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * What the system already knows about a reader, for people choosing one and
 * for the status of the one in use: sysfs for USB and serial ports. Only
 * files are read — no port is opened, no device asked.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "meta.h"

void meta_attr(const char *dir, const char *name, char *out, size_t cap)
{
	char p[PATH_MAX];
	FILE *f;

	out[0] = '\0';
	if (snprintf(p, sizeof(p), "%s/%s", dir, name) >= (int)sizeof(p) || !(f = fopen(p, "r")))
		return;
	if (fgets(out, (int)cap, f))
		out[strcspn(out, "\r\n")] = '\0';
	fclose(f);
}

static const char *base(const char *s)
{
	const char *b = strrchr(s, '/');

	return b ? b + 1 : s;
}

/* the entry of /dev/serial/<kind> that links to name */
static void serial_link(const char *sysroot, const char *kind, const char *name, char *out, size_t cap)
{
	char dir[PATH_MAX], link[PATH_MAX], target[PATH_MAX];
	DIR *d;
	struct dirent *e;

	out[0] = '\0';
	snprintf(dir, sizeof(dir), "%s/dev/serial/%s", sysroot, kind);
	if (!(d = opendir(dir)))
		return;
	while ((e = readdir(d))) {
		ssize_t l;

		if (e->d_name[0] == '.' || snprintf(link, sizeof(link), "%s/%s", dir, e->d_name) >= (int)sizeof(link))
			continue;
		if ((l = readlink(link, target, sizeof(target) - 1)) <= 0)
			continue;
		target[l] = '\0';
		if (!strcmp(base(target), name)) {
			snprintf(out, cap, "/dev/serial/%s/%s", kind, e->d_name);
			break;
		}
	}
	closedir(d);
}

int meta_tty_read(const char *sysroot, const char *dev, struct tty_meta *m)
{
	char path[PATH_MAX], real[PATH_MAX], link[PATH_MAX];
	const char *name = dev;
	ssize_t l;

	memset(m, 0, sizeof(*m));
	/* a by-id link or /dev/ttyX: the tty's own name */
	if (strchr(dev, '/')) {
		char full[PATH_MAX];

		snprintf(full, sizeof(full), "%s%s", sysroot, dev);
		name = realpath(full, real) ? base(real) : base(dev);
	}
	if (strlen(name) >= sizeof(m->name))
		return -1;
	strcpy(m->name, name);

	if (snprintf(path, sizeof(path), "%s/sys/class/tty/%s/device", sysroot, m->name) >= (int)sizeof(path))
		return -1;
	if (snprintf(link, sizeof(link), "%s/driver", path) < (int)sizeof(link) &&
	    (l = readlink(link, real, sizeof(real) - 1)) > 0) {
		real[l] = '\0';
		snprintf(m->driver, sizeof(m->driver), "%s", base(real));
	}

	/* device -> the USB interface (ttyACM) or a port below it (ttyUSB):
	 * the interface number and name on the interface, the ids on the USB
	 * device a level or two up */
	if (!realpath(path, real))
		return -1;
	snprintf(m->usbdir, sizeof(m->usbdir), "%s", real);
	for (int up = 0; up < 4 && !m->vid[0]; up++) {
		char *cut;

		if (!m->ifnum[0]) {
			meta_attr(m->usbdir, "bInterfaceNumber", m->ifnum, sizeof(m->ifnum));
			if (m->ifnum[0]) {
				meta_attr(m->usbdir, "interface", m->ifname, sizeof(m->ifname));
				meta_attr(m->usbdir, "bInterfaceClass", m->iclass, sizeof(m->iclass));
				meta_attr(m->usbdir, "bInterfaceProtocol", m->iproto, sizeof(m->iproto));
			}
		}
		meta_attr(m->usbdir, "idVendor", m->vid, sizeof(m->vid));
		meta_attr(m->usbdir, "idProduct", m->pid, sizeof(m->pid));
		if (!m->vid[0] && (cut = strrchr(m->usbdir, '/')))
			*cut = '\0';
	}
	if (!m->vid[0])
		m->usbdir[0] = '\0';

	serial_link(sysroot, "by-id", m->name, m->by_id, sizeof(m->by_id));
	serial_link(sysroot, "by-path", m->name, m->by_path, sizeof(m->by_path));
	return 0;
}

const char *meta_tty_role(const struct tty_meta *m)
{
	if (!strcmp(m->vid, "12d1") && !strcmp(m->iclass, "ff") && !strcmp(m->iproto, "03"))
		return "diag";
	if (strcasestr(m->ifname, "diag"))
		return "diag";
	return "";
}

void meta_usb_write(struct jw *w, const char *usbdir)
{
	char v[256];

	if (!usbdir || !*usbdir)
		return;
	meta_attr(usbdir, "manufacturer", v, sizeof(v));
	jw_opt(w, "usb_manufacturer", v);
	meta_attr(usbdir, "product", v, sizeof(v));
	jw_opt(w, "usb_product", v);
	meta_attr(usbdir, "serial", v, sizeof(v));
	jw_opt(w, "usb_serial", v);
	jw_opt(w, "usb_path", base(usbdir));
	meta_attr(usbdir, "speed", v, sizeof(v));
	jw_opt(w, "usb_speed_mbps", v);
	meta_attr(usbdir, "bcdDevice", v, sizeof(v));
	jw_opt(w, "usb_version", v);
}

void meta_tty_write(struct jw *w, const struct tty_meta *m)
{
	char ids[20];

	jw_str(w, "driver", m->driver);
	snprintf(ids, sizeof(ids), "%s:%s", m->vid, m->pid);
	jw_str(w, "usb", m->vid[0] ? ids : "");
	jw_str(w, "interface", m->ifnum);
	jw_opt(w, "interface_name", m->ifname);
	jw_opt(w, "role", meta_tty_role(m));
	meta_usb_write(w, m->usbdir);
	jw_opt(w, "by_id", m->by_id);
	jw_opt(w, "by_path", m->by_path);
}

int meta_usb_dir(const char *sysroot, int bus, const uint8_t *ports, int nports, char *out, size_t cap)
{
	size_t o;
	int i;

	o = (size_t)snprintf(out, cap, "%s/sys/bus/usb/devices/%d-", sysroot, bus);
	for (i = 0; i < nports && o < cap; i++)
		o += (size_t)snprintf(out + o, cap - o, i ? ".%u" : "%u", ports[i]);
	if (o >= cap || nports <= 0)
		return -1;
	return access(out, F_OK) ? -1 : 0;
}

void meta_usb_driver(const char *usbdir, char *out, size_t cap)
{
	char link[PATH_MAX], target[PATH_MAX];
	ssize_t l;

	out[0] = '\0';
	/* interface 0 of configuration 1: <dir>/<name>:1.0/driver */
	if (snprintf(link, sizeof(link), "%s/%s:1.0/driver", usbdir, base(usbdir)) >= (int)sizeof(link))
		return;
	if ((l = readlink(link, target, sizeof(target) - 1)) > 0) {
		target[l] = '\0';
		snprintf(out, cap, "%s", base(target));
	}
}
