/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * WB Electronics "Smartmouse USB" (USB 104f:0002): an FTDI FT232BM behind
 * WB's own USB ids, with the card clock and the Phoenix/Smartmouse wiring
 * selected by software instead of switches. After that it is a plain
 * Phoenix reader on the FTDI serial port, which phoenix.c drives through
 * ftdi_usb.c on the same libusb handle: the target kernels have no
 * ftdi_sio, and none is needed.
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
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <libusb.h>

#include "ftdi_usb.h"
#include "json.h"
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

struct phx_io *wbsm_open(const struct wbsm_cfg *cfg, const char *name)
{
	libusb_context *ctx = NULL;
	libusb_device **list = NULL;
	libusb_device_handle *h = NULL;
	int code = clock_code(cfg->clock_khz);
	bool detached = false;
	ssize_t n;
	unsigned char b;
	int i, r;

	if (code < 0) {
		log_err("Smartmouse USB: %u kHz is not a clock it has (3580, 3680 or 6000)", cfg->clock_khz);
		return NULL;
	}
	if (libusb_init(&ctx)) {
		log_err("Smartmouse USB: libusb_init failed");
		return NULL;
	}
	n = libusb_get_device_list(ctx, &list);
	for (i = 0; i < n && !h; i++) {
		if (libusb_open(list[i], &h))
			continue;
		if (!matches(list[i], h, cfg->serial)) {
			libusb_close(h);
			h = NULL;
		}
	}
	if (list)
		libusb_free_device_list(list, 1);
	if (!h) {
		log_err("Smartmouse USB (%04x:%04x%s%s) not found, or no permission to open it",
		        WBSM_VID, WBSM_PID, cfg->serial ? ", serial " : "", cfg->serial ? cfg->serial : "");
		goto fail;
	}

	/* A kernel serial driver that holds the interface (ftdi_sio, where
	 * one exists and was told WB's ids) is detached for as long as we own
	 * the reader and gets it back on close, so the two never drive the
	 * chip at once. */
	if (libusb_kernel_driver_active(h, 0) == 1) {
		if ((r = libusb_detach_kernel_driver(h, 0))) {
			log_err("Smartmouse USB: cannot detach the kernel driver: %s", libusb_strerror(r));
			goto fail;
		}
		detached = true;
	}
	if ((r = libusb_claim_interface(h, 0))) {
		log_err("Smartmouse USB: cannot claim it: %s", libusb_strerror(r));
		goto fail;
	}

	b = (unsigned char)(code << 4 | (cfg->smartmouse ? 1 << 6 : 0));
	if (bitbang(h, FTDI_BITMODE_BITBANG, 0xFF) < 0 || put(h, b) || put(h, b) ||
	    put(h, b | 0x80) || bitbang(h, 0, 0) < 0) {
		log_err("Smartmouse USB: setting clock and mode failed");
		libusb_release_interface(h, 0);
		goto fail;
	}
	log_notice("Smartmouse USB: %s mode, %s MHz", cfg->smartmouse ? "Smartmouse" : "Phoenix",
	         code == 1 ? "6.00" : code == 2 ? "3.58" : "3.68");
	/* bitbang off leaves the chip a UART again; the handle, claim and
	 * detach state go to the transport, which releases them on close */
	return ftdi_io_open(ctx, h, detached, name);

fail:
	if (h) {
		if (detached)
			libusb_attach_kernel_driver(h, 0);
		libusb_close(h);
	}
	libusb_exit(ctx);
	return NULL;
}

int wbsm_list(void)
{
	libusb_context *ctx = NULL;
	libusb_device **list = NULL;
	ssize_t n;
	int i, found = 0;

	if (libusb_init(&ctx))
		return 0;
	n = libusb_get_device_list(ctx, &list);
	for (i = 0; i < n; i++) {
		struct libusb_device_descriptor d;
		libusb_device_handle *h = NULL;
		unsigned char serial[64] = "";
		char spec[80];
		struct jw w;

		if (libusb_get_device_descriptor(list[i], &d) || d.idVendor != WBSM_VID || d.idProduct != WBSM_PID)
			continue;
		/* the serial needs the device opened; without permission it is
		 * still listed, as the first one */
		if (!libusb_open(list[i], &h)) {
			if (libusb_get_string_descriptor_ascii(h, d.iSerialNumber, serial, sizeof(serial)) < 0)
				serial[0] = '\0';
			libusb_close(h);
		}
		snprintf(spec, sizeof(spec), "wbsm:%s", (const char *)serial);
		jw_begin(&w, stdout);
		jw_str(&w, "backend", "wbsm");
		jw_str(&w, "spec", spec);
		jw_str(&w, "serial", (const char *)serial);
		jw_end(&w);
		found++;
	}
	if (list)
		libusb_free_device_list(list, 1);
	libusb_exit(ctx);
	return found;
}
