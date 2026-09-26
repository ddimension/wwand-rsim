/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ftdi_proto.h"
#include "ftdi_usb.h"
#include "log.h"

/*
 * FT232BM over libusb, without ftdi_sio. Request numbers, request types and
 * value layouts are the ones in drivers/usb/serial/ftdi_sio.h (linux
 * 6.18.41); wIndex is 0 throughout because the FT232BM has a single
 * interface (ftdi_sio adds a channel number only for multi-port chips,
 * change_speed()). Data goes over bulk OUT 0x02 and comes back over bulk
 * IN 0x81, each IN packet led by a modem-status and a line-status byte.
 */

#define REQ_OUT		(LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT)
#define REQ_IN		(LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN)
#define SIO_RESET		0x00
#define SIO_SET_MODEM_CTRL	0x01
#define SIO_SET_FLOW_CTRL	0x02
#define SIO_SET_BAUDRATE	0x03
#define SIO_SET_DATA		0x04
#define SIO_POLL_MODEM_STATUS	0x05
#define SIO_SET_LATENCY_TIMER	0x09

#define SIO_RESET_SIO		0
/* The purge values are named from the chip's side: 1 empties what the
 * host sent and is waiting to go out on the line, 2 empties what came in
 * from the line for the host -- libftdi 1.5 got this backwards before and
 * now spells it SIO_TCIFLUSH = 2 (libftdi1-1.5/src/ftdi.h, the WARNING
 * above SIO_RESET_PURGE_RX). Input is what a flush here must drop. */
#define SIO_PURGE_FROM_LINE	2

#define EP_IN		0x81
#define EP_OUT		0x02
#define CTRL_TIMEOUT_MS	1000
/* How long the chip holds a part-filled IN packet before sending it
 * (1..255 ms, 16 by default -- ftdi_sio's priv->latency, linux 6.18.41).
 * At 16 ms every procedure byte would wait that long; 2 ms keeps a
 * character to the host within about one character time. */
#define LATENCY_MS	2
/* above this the UART's mid-bit sampling runs out of margin over an
 * 11-bit character (half a bit over 11 bits is ~4.5 %, shared by both
 * ends), so it is worth a warning */
#define RATE_WARN_BP	200	/* basis points: 2 % */

struct ftdi_io {
	struct phx_io io;
	libusb_context *ctx;
	libusb_device_handle *h;
	bool detached;
	const char *name;
	int mps;
	bool drop_errors;
	unsigned baud;		/* last rate set, 0 none */
	int data;		/* last SET_DATA value, -1 none */
	struct ftdi_rx_stat st;
	unsigned errs_logged;
	uint8_t raw[512];	/* a multiple of the 64-byte packet size */
	uint8_t pend[512];
	size_t pend_pos, pend_len;
};

static int usb_errno(int r)
{
	switch (r) {
	case LIBUSB_ERROR_NO_DEVICE:	return -ENODEV;
	case LIBUSB_ERROR_ACCESS:	return -EACCES;
	case LIBUSB_ERROR_TIMEOUT:	return -ETIMEDOUT;
	case LIBUSB_ERROR_PIPE:		return -EPIPE;
	case LIBUSB_ERROR_BUSY:		return -EBUSY;
	case LIBUSB_ERROR_NO_MEM:	return -ENOMEM;
	default:			return -EIO;
	}
}

static int ctrl_out(struct ftdi_io *f, uint8_t req, uint16_t value, uint16_t index)
{
	int r = libusb_control_transfer(f->h, REQ_OUT, req, value, index, NULL, 0,
					CTRL_TIMEOUT_MS);

	if (r < 0) {
		log_dbg("%s: request %02x: %s", f->name, req, libusb_strerror(r));
		return usb_errno(r);
	}
	return 0;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int fu_set_line(struct phx_io *io, unsigned baud, enum phx_parity parity,
		       bool drop_errors)
{
	struct ftdi_io *f = (struct ftdi_io *)io;
	int data, r;

	if (baud != f->baud) {
		uint32_t div;
		unsigned got;
		long bp;

		if (ftdi_bm_divisor(baud, &div))
			return -EINVAL;
		r = ctrl_out(f, SIO_SET_BAUDRATE, (uint16_t)div, (uint16_t)(div >> 16));
		if (r)
			return r;
		f->baud = baud;
		got = ftdi_bm_rate(div);
		bp = ((long)got - (long)baud) * 10000 / (long)baud;
		log_msg(labs(bp) > RATE_WARN_BP ? LOG_WARNING : LOG_NOTICE,
			"%s: %u baud asked, %u baud set (%c%ld.%02ld %%)", f->name, baud, got,
			bp < 0 ? '-' : '+', labs(bp) / 100, labs(bp) % 100);
	}
	/* 8 data bits, parity in bits 8-10 (0 none, 1 odd, 2 even), stop
	 * bits in 11-12 (2 = two): ftdi_sio.h FTDI_SIO_SET_DATA_* */
	data = 8 | (parity == PHX_PARITY_ODD ? 1 : parity == PHX_PARITY_EVEN ? 2 : 0) << 8 | 2 << 11;
	if (data != f->data) {
		r = ctrl_out(f, SIO_SET_DATA, (uint16_t)data, 0);
		if (r)
			return r;
		f->data = data;
	}
	/* the chip reports errors either way; dropping is our decision */
	f->drop_errors = drop_errors;
	return 0;
}

static int fu_set_modem(struct phx_io *io, int rts, int dtr)
{
	struct ftdi_io *f = (struct ftdi_io *)io;
	uint16_t v = 0;

	/* high byte: which lines to change, low byte: their levels
	 * (FTDI_SIO_SET_DTR_HIGH etc.), so one request sets both */
	if (dtr >= 0)
		v |= 0x0100 | (dtr ? 0x01 : 0);
	if (rts >= 0)
		v |= 0x0200 | (rts ? 0x02 : 0);
	return v ? ctrl_out(f, SIO_SET_MODEM_CTRL, v, 0) : 0;
}

static int fu_get_modem(struct phx_io *io, int *cts, int *dsr, int *cd)
{
	struct ftdi_io *f = (struct ftdi_io *)io;
	uint8_t b[2];
	int r = libusb_control_transfer(f->h, REQ_IN, SIO_POLL_MODEM_STATUS, 0, 0, b,
					sizeof(b), CTRL_TIMEOUT_MS);

	if (r < 1)
		return r < 0 ? usb_errno(r) : -EIO;
	*cts = !!(b[0] & FTDI_MS_CTS);
	*dsr = !!(b[0] & FTDI_MS_DSR);
	*cd = !!(b[0] & FTDI_MS_CD);
	return 0;
}

static int fu_write(struct phx_io *io, const uint8_t *buf, size_t len)
{
	struct ftdi_io *f = (struct ftdi_io *)io;
	size_t off = 0;

	while (off < len) {
		int done = 0;
		/* at ~1 ms per character the chip's 128-byte TX FIFO drains
		 * slowly; the timeout is sized for the whole write */
		int r = libusb_bulk_transfer(f->h, EP_OUT, (unsigned char *)buf + off,
					     (int)(len - off), &done,
					     CTRL_TIMEOUT_MS + 2 * (unsigned)len);

		if (done > 0)
			off += (size_t)done;
		if (r && off < len)
			return usb_errno(r);
	}
	return 0;
}

static size_t take_pending(struct ftdi_io *f, uint8_t *buf, size_t len)
{
	size_t n = f->pend_len - f->pend_pos;

	if (n > len)
		n = len;
	memcpy(buf, f->pend + f->pend_pos, n);
	f->pend_pos += n;
	return n;
}

static int fu_read(struct phx_io *io, uint8_t *buf, size_t len, unsigned timeout_ms)
{
	struct ftdi_io *f = (struct ftdi_io *)io;
	long deadline = now_ms() + (long)timeout_ms;

	if (f->pend_pos < f->pend_len)
		return (int)take_pending(f, buf, len);
	/* The chip answers every IN request within the latency timer, with
	 * a status-only packet when the line was quiet, so this loop turns
	 * over every 2 ms until data comes or the time is up. */
	for (;;) {
		long left = deadline - now_ms();
		unsigned errs;
		size_t n;
		int got = 0, r;

		if (left <= 0)
			return 0;
		r = libusb_bulk_transfer(f->h, EP_IN, f->raw, sizeof(f->raw), &got,
					 (unsigned)left);
		if (r && r != LIBUSB_ERROR_TIMEOUT)
			return usb_errno(r);
		n = ftdi_rx_parse(f->raw, (size_t)got, (size_t)f->mps, f->drop_errors,
				  f->pend, &f->st);
		errs = f->st.parity + f->st.framing + f->st.overrun;
		if (errs != f->errs_logged) {
			log_dbg("%s: line errors: parity %u, framing %u, overrun %u; %u bytes dropped",
				f->name, f->st.parity, f->st.framing, f->st.overrun, f->st.dropped);
			f->errs_logged = errs;
		}
		if (n) {
			f->pend_pos = 0;
			f->pend_len = n;
			return (int)take_pending(f, buf, len);
		}
		if (r == LIBUSB_ERROR_TIMEOUT)
			return 0;
	}
}

static void fu_flush_input(struct phx_io *io)
{
	struct ftdi_io *f = (struct ftdi_io *)io;

	f->pend_pos = f->pend_len = 0;
	ctrl_out(f, SIO_RESET, SIO_PURGE_FROM_LINE, 0);
}

static void fu_close(struct phx_io *io)
{
	struct ftdi_io *f = (struct ftdi_io *)io;

	libusb_release_interface(f->h, 0);
	if (f->detached)
		libusb_attach_kernel_driver(f->h, 0);
	libusb_close(f->h);
	libusb_exit(f->ctx);
	free(f);
}

static const struct phx_io_ops ftdi_ops = {
	.set_line = fu_set_line,
	.set_modem = fu_set_modem,
	.get_modem = fu_get_modem,
	.write = fu_write,
	.read = fu_read,
	.flush_input = fu_flush_input,
	.close = fu_close,
};

struct phx_io *ftdi_io_open(libusb_context *ctx, libusb_device_handle *h,
			    bool detached, const char *name)
{
	struct ftdi_io *f = calloc(1, sizeof(*f));
	int r;

	if (!f) {
		libusb_release_interface(h, 0);
		if (detached)
			libusb_attach_kernel_driver(h, 0);
		libusb_close(h);
		libusb_exit(ctx);
		return NULL;
	}
	f->io.ops = &ftdi_ops;
	f->ctx = ctx;
	f->h = h;
	f->detached = detached;
	f->name = name;
	f->data = -1;
	/* the status bytes lead every packet, so the packet size must be
	 * the endpoint's real one; 64 is the full-speed bulk maximum */
	f->mps = libusb_get_max_packet_size(libusb_get_device(h), EP_IN);
	if (f->mps <= 2 || (size_t)f->mps > sizeof(f->raw) || sizeof(f->raw) % (size_t)f->mps)
		f->mps = 64;

	r = ctrl_out(f, SIO_RESET, SIO_RESET_SIO, 0);
	if (!r)
		r = ctrl_out(f, SIO_SET_LATENCY_TIMER, LATENCY_MS, 0);
	if (!r)
		/* wIndex high byte 0: no RTS/CTS, DTR/DSR or XON/XOFF, the
		 * modem lines are ours (FTDI_SIO_DISABLE_FLOW_CTRL) */
		r = ctrl_out(f, SIO_SET_FLOW_CTRL, 0, 0);
	if (r) {
		log_err("%s: FTDI setup failed: %s", name, strerror(-r));
		fu_close(&f->io);
		return NULL;
	}
	fu_flush_input(&f->io);
	log_dbg("%s: FTDI over libusb, %d-byte packets, latency %d ms", name, f->mps, LATENCY_MS);
	return &f->io;
}
