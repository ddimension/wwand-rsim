/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * WB Electronics "Smartmouse USB" (USB 104f:0002): an FTDI FT232BM behind
 * WB's own USB ids, with the card clock and the Phoenix/Smartmouse wiring
 * selected by software instead of switches. After that it is a plain
 * Phoenix reader on the FTDI serial port, which phoenix.c drives.
 *
 * The selection is latched from the FT232's data pins in bitbang mode
 * (WB's own Linux tool smusbutil 1.1, 2005, written with the vendor's
 * information: http://www.infinityusb.com/files/smusbutil.tar.bz2, archived
 * at web.archive.org 20160829):
 *   bits 4-5  clock: 1 = 6.00 MHz, 2 = 3.58 MHz, 3 = 3.68 MHz
 *   bit 6     1 = Smartmouse wiring, 0 = Phoenix
 *   bit 7     RI, the strobe: the byte is written twice, then once more
 *             with bit 7 set
 * It is not kept across a power loss (the product page: "the frequency can
 * be selected manually"), so it is set every time the reader is opened.
 *
 * FTDI requests (the FT232BM's vendor interface, as libftdi and the
 * kernel's ftdi_sio.h use them): SET_BITMODE = request 0x0B, value
 * (mode << 8) | pin mask, mode 0x01 = asynchronous bitbang, 0x00 = off.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libusb.h>

#include "log.h"
#include "wbsm.h"

#define WBSM_VID		0x104f
#define WBSM_PID		0x0002
#define FTDI_SET_BITMODE	0x0B
#define FTDI_BITMODE_BITBANG	0x01
#define FTDI_EP_OUT		0x02
#define USB_TIMEOUT_MS		1000

/* the clock code for a frequency in kHz; the reader has these three */
static int clock_code(unsigned khz)
{
	if (khz >= 3570 && khz <= 3590)
		return 2;
	if (khz >= 3670 && khz <= 3690)
		return 3;
	if (khz >= 5990 && khz <= 6010)
		return 1;
	return -1;
}

static int matches(libusb_device *dev, libusb_device_handle *h, const char *serial)
{
	struct libusb_device_descriptor d;
	unsigned char buf[64];

	if (libusb_get_device_descriptor(dev, &d) || d.idVendor != WBSM_VID || d.idProduct != WBSM_PID)
		return 0;
	if (!serial || !*serial)
		return 1;
	if (libusb_get_string_descriptor_ascii(h, d.iSerialNumber, buf, sizeof(buf)) < 0)
		return 0;
	return !strcmp((const char *)buf, serial);
}

/* /sys/bus/usb/devices/<bus>-<port.port...>:1.0/ttyUSBn -> /dev/ttyUSBn */
static int find_tty(libusb_device *dev, char *tty, size_t len)
{
	uint8_t ports[8];
	char path[128];
	int n, i, off;
	DIR *d;
	struct dirent *e;

	n = libusb_get_port_numbers(dev, ports, sizeof(ports));
	if (n <= 0)
		return -1;
	off = snprintf(path, sizeof(path), "/sys/bus/usb/devices/%u-", libusb_get_bus_number(dev));
	for (i = 0; i < n; i++)
		off += snprintf(path + off, sizeof(path) - off, i ? ".%u" : "%u", ports[i]);
	snprintf(path + off, sizeof(path) - off, ":1.0");
	if (!(d = opendir(path)))
		return -1;
	while ((e = readdir(d))) {
		if (!strncmp(e->d_name, "ttyUSB", 6)) {
			snprintf(tty, len, "/dev/%s", e->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

#define FTDI_NEW_ID "/sys/bus/usb-serial/drivers/ftdi_sio/new_id"

static int bind_ftdi_sio(void)
{
	FILE *f = fopen(FTDI_NEW_ID, "w");
	int bad;

	if (!f) {
		log_err("Smartmouse USB: cannot hand it to ftdi_sio (%s: %s)%s", FTDI_NEW_ID, strerror(errno),
		        errno == ENOENT ? " — load the ftdi_sio module" :
		        errno == EACCES ? " — needs root: echo 104f 0002 > " FTDI_NEW_ID : "");
		return -1;
	}
	bad = fprintf(f, "%04x %04x\n", WBSM_VID, WBSM_PID) < 0;
	/* EEXIST: the ids are known already, the driver just was not bound */
	if ((fclose(f) || bad) && errno != EEXIST) {
		log_err("Smartmouse USB: writing %s failed: %s", FTDI_NEW_ID, strerror(errno));
		return -1;
	}
	log_notice("Smartmouse USB: ftdi_sio told about 104f:0002");
	return 0;
}

static int bitbang(libusb_device_handle *h, unsigned char mode, unsigned char mask)
{
	return libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
	                               LIBUSB_ENDPOINT_OUT, FTDI_SET_BITMODE,
	                               (uint16_t)(mode << 8 | mask), 0, NULL, 0, USB_TIMEOUT_MS);
}

static int put(libusb_device_handle *h, unsigned char b)
{
	int done = 0;
	int r = libusb_bulk_transfer(h, FTDI_EP_OUT, &b, 1, &done, USB_TIMEOUT_MS);

	return (r || done != 1) ? -1 : 0;
}

int wbsm_prepare(const struct wbsm_cfg *cfg, char *tty, size_t ttylen)
{
	libusb_context *ctx = NULL;
	libusb_device **list = NULL;
	libusb_device *dev = NULL;
	libusb_device_handle *h = NULL;
	int code = clock_code(cfg->clock_khz);
	int detached = 0, claimed = 0, ret = -1, i, r;
	ssize_t n;
	unsigned char b;

	if (code < 0) {
		log_err("Smartmouse USB: %u kHz is not a clock it has (3580, 3680 or 6000)", cfg->clock_khz);
		return -1;
	}
	if (libusb_init(&ctx)) {
		log_err("Smartmouse USB: libusb_init failed");
		return -1;
	}
	n = libusb_get_device_list(ctx, &list);
	for (i = 0; i < n && !h; i++) {
		if (libusb_open(list[i], &h))
			continue;
		if (matches(list[i], h, cfg->serial)) {
			dev = list[i];
			break;
		}
		libusb_close(h);
		h = NULL;
	}
	if (!h) {
		log_err("Smartmouse USB (%04x:%04x%s%s) not found, or no permission to open it",
		        WBSM_VID, WBSM_PID, cfg->serial ? ", serial " : "", cfg->serial ? cfg->serial : "");
		goto out;
	}

	/* the serial driver has to let go of the interface while the pins
	 * are driven directly; it is given the device back afterwards */
	if (libusb_kernel_driver_active(h, 0) == 1) {
		if ((r = libusb_detach_kernel_driver(h, 0))) {
			log_err("Smartmouse USB: cannot detach the serial driver: %s", libusb_strerror(r));
			goto out;
		}
		detached = 1;
	}
	if ((r = libusb_claim_interface(h, 0))) {
		log_err("Smartmouse USB: cannot claim it: %s", libusb_strerror(r));
		goto out;
	}
	claimed = 1;

	b = (unsigned char)(code << 4 | (cfg->smartmouse ? 1 << 6 : 0));
	if (bitbang(h, FTDI_BITMODE_BITBANG, 0xFF) < 0 || put(h, b) || put(h, b) ||
	    put(h, b | 0x80) || bitbang(h, 0, 0) < 0) {
		log_err("Smartmouse USB: setting clock and mode failed");
		goto out;
	}
	log_notice("Smartmouse USB: %s mode, %s MHz", cfg->smartmouse ? "Smartmouse" : "Phoenix",
	         code == 1 ? "6.00" : code == 2 ? "3.58" : "3.68");
	ret = 0;

out:
	if (claimed)
		libusb_release_interface(h, 0);
	if (detached)
		libusb_attach_kernel_driver(h, 0);
	/* No serial driver had it: ftdi_sio does not list WB's ids, so it is
	 * told about them (new_id also binds the device already present). As
	 * root, which the wwand daemon is; anywhere else the error says what
	 * to run. */
	if (!ret && !detached && bind_ftdi_sio())
		ret = -1;
	if (!ret) {
		/* ftdi_sio needs a moment to register the tty */
		for (i = 0; i < 30 && find_tty(dev, tty, ttylen); i++)
			usleep(100000);
		if (i == 30) {
			log_err("Smartmouse USB: no serial port appeared for it (is ftdi_sio loaded?)");
			ret = -1;
		}
	}
	if (h)
		libusb_close(h);
	if (list)
		libusb_free_device_list(list, 1);
	libusb_exit(ctx);
	return ret;
}
