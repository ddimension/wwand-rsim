/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atr.h"
#include "log.h"
#include "phoenix.h"
#include "t0.h"

/*
 * Phoenix / Smartmouse: a USB-serial bridge whose RX and TX both sit on the
 * card's single I/O line, with the card reset on RTS or DTR and a fixed card
 * clock from the reader. There is no VCC control: "power" is the reset line.
 * The serial side is a struct phx_io (a kernel tty, or FTDI over libusb).
 */

#define RESET_HOLD_MS	50
/* how long a card held in reset must stay silent: longer than the reader's
 * USB latency (2 ms) plus a few characters at 9600 baud */
#define RESET_QUIET_MS	20
#define DRAIN_MAX_MS	300
/* per-byte wait for the echo of a byte just written: one character is ~1.3
 * ms at 9600 baud, the rest is USB latency (FTDI's latency timer alone is
 * 16 ms by default) */
#define ECHO_TIMEOUT_MS	200
/* added to the card's own waiting times for the same USB round trip */
#define USB_MARGIN_MS	50

struct phoenix {
	struct rsim_backend be;
	struct phoenix_cfg cfg;
	struct phx_io *io;
	unsigned baud;

	bool reset_dtr;		/* reset on DTR, else on RTS */
	bool reset_inv;		/* line cleared = reset asserted */
	bool polarity_known;

	bool inverse;		/* inverse convention: all bytes mapped */
	int echo;		/* -1 not yet known, 0 off, 1 on */
	int pushback;		/* a byte read ahead during echo detection */
	unsigned wwt_ms;

	uint8_t rx[128];
	size_t rx_pos, rx_len;
};

static int set_detail(struct phoenix *p, int err, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static int set_detail(struct phoenix *p, int err, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(p->be.detail, sizeof(p->be.detail), fmt, ap);
	va_end(ap);
	return err;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

static int io_err(struct phoenix *p, int err, const char *what)
{
	return set_detail(p, RSIM_E_IO, "%s: %s", what, strerror(-err));
}

static int set_line(struct phoenix *p, bool dtr, bool level)
{
	int r = p->io->ops->set_modem(p->io, dtr ? -1 : level, dtr ? level : -1);

	return r ? io_err(p, r, "modem control") : RSIM_OK;
}

static int reset_line(struct phoenix *p, bool assert)
{
	return set_line(p, p->reset_dtr, assert != p->reset_inv);
}

/* Parity checking is armed only once TS has told the convention: an
 * inverse-convention character read by a direct-framed UART has odd total
 * parity (all nine data+parity levels are inverted, and an even count of
 * ones out of nine becomes odd), so with even parity checked the TS of an
 * inverse card would be dropped before it could be recognised. */
static int apply_line(struct phoenix *p, bool check_parity, bool odd)
{
	/* A byte that fails parity is dropped rather than handed on once
	 * checking is armed: the exchange then fails as a timeout, where a
	 * corrupted byte would have been read as data or a procedure byte. */
	int r = p->io->ops->set_line(p->io, p->baud,
				     odd ? PHX_PARITY_ODD : PHX_PARITY_EVEN,
				     check_parity);

	return r ? io_err(p, r, "line setup") : RSIM_OK;
}

static void rx_drop(struct phoenix *p)
{
	p->io->ops->flush_input(p->io);
	p->rx_pos = p->rx_len = 0;
	p->pushback = -1;
}

/* one byte as it came off the wire, before any convention mapping */
static int recv_raw(struct phoenix *p, uint8_t *b, unsigned timeout_ms)
{
	if (p->rx_pos == p->rx_len) {
		int n = p->io->ops->read(p->io, p->rx, sizeof(p->rx), timeout_ms);

		if (n == 0)
			return RSIM_E_TIMEOUT;
		if (n < 0)
			return io_err(p, n, "read");
		p->rx_pos = 0;
		p->rx_len = (size_t)n;
	}
	*b = p->rx[p->rx_pos++];
	return RSIM_OK;
}

static int recv_byte(struct phoenix *p, uint8_t *b, unsigned timeout_ms)
{
	int r;

	if (p->pushback >= 0) {
		*b = (uint8_t)p->pushback;
		p->pushback = -1;
		return RSIM_OK;
	}
	r = recv_raw(p, b, timeout_ms);
	if (!r && p->inverse)
		*b = atr_inverse(*b);
	return r;
}

/* the echo of byte i of what was just written, compared */
static int check_echo(struct phoenix *p, const uint8_t *buf, size_t from, size_t len)
{
	size_t i;
	uint8_t b;
	int r;

	for (i = from; i < len; i++) {
		r = recv_byte(p, &b, ECHO_TIMEOUT_MS);
		if (r == RSIM_E_TIMEOUT)
			return set_detail(p, RSIM_E_IO, "echo of byte %zu missing", i);
		if (r)
			return r;
		if (b != buf[i])
			return set_detail(p, RSIM_E_IO, "echo mismatch at byte %zu: sent %02X, read %02X",
					  i, buf[i], b);
	}
	return RSIM_OK;
}

static int chan_send(void *ctx, const uint8_t *buf, size_t len)
{
	struct phoenix *p = ctx;
	uint8_t wire[RSIM_TPDU_MAX];
	size_t i;
	uint8_t b;
	int r;

	if (len > sizeof(wire))
		return set_detail(p, RSIM_E_IO, "write of %zu bytes", len);
	for (i = 0; i < len; i++)
		wire[i] = p->inverse ? atr_inverse(buf[i]) : buf[i];
	r = p->io->ops->write(p->io, wire, len);
	if (r)
		return io_err(p, r, "write");
	if (p->echo == 0)
		return RSIM_OK;
	if (p->echo == 1)
		return check_echo(p, buf, 0, len);

	/* Echo not known yet: this is the first header this process sends.
	 * An echoing reader returns CLA first; a card without echo answers
	 * with a procedure byte (60, 6X/9X or INS/~INS). The two meet only
	 * when a card's instant SW1 equals CLA, which needs a class 6X
	 * (logical channels 4-19) -- the modem sends 00-03, 80-83 or A0. A
	 * wrong guess fails the echo compare below as an I/O error rather
	 * than passing silently, and the decision is taken once per process
	 * because echo is how the reader is wired, not how the card is. */
	r = recv_byte(p, &b, p->wwt_ms);
	if (r)
		return r;
	if (b == buf[0]) {
		p->echo = 1;
		log_dbg("%s: I/O line echoes", p->cfg.dev);
		return check_echo(p, buf, 1, len);
	}
	p->echo = 0;
	p->pushback = b;
	log_dbg("%s: no echo on the I/O line", p->cfg.dev);
	return RSIM_OK;
}

static int chan_recv(void *ctx, uint8_t *b, unsigned timeout_ms)
{
	return recv_byte(ctx, b, timeout_ms);
}

/* 9600 etu, the initial waiting time between ATR characters (§8.2 with
 * the default WI=10 of §10.2), at F=372 */
static unsigned atr_char_ms(const struct phoenix *p)
{
	return t0_wwt_ms(p->cfg.clock_khz, 10) + USB_MARGIN_MS;
}

static int read_atr(struct phoenix *p, uint8_t *atr, size_t *atr_len)
{
	struct atr_info info;
	size_t n = 1;
	uint8_t b;
	int r;

	p->inverse = false;
	r = recv_raw(p, &b, p->cfg.atr_timeout_ms);
	if (r == RSIM_E_TIMEOUT)
		return set_detail(p, RSIM_E_NO_CARD, "no ATR within %u ms", p->cfg.atr_timeout_ms);
	if (r)
		return r;
	/* §8.1: a direct-convention TS reads as 3B; an inverse one (3F)
	 * reads as its bit-reversed complement 03 */
	if (b == 0x3b) {
		atr[0] = 0x3b;
	} else if (b == atr_inverse(0x3f)) {
		atr[0] = 0x3f;
		p->inverse = true;
	} else {
		return set_detail(p, RSIM_E_PROTOCOL, "invalid TS %02X", b);
	}
	r = apply_line(p, true, p->inverse);
	if (r)
		return r;

	while ((r = atr_parse(atr, n, &info)) == ATR_NEED_MORE) {
		r = recv_byte(p, &atr[n], atr_char_ms(p));
		if (r == RSIM_E_TIMEOUT)
			return set_detail(p, RSIM_E_PROTOCOL, "ATR truncated after %zu bytes", n);
		if (r)
			return r;
		n++;
	}
	if (r == ATR_INVALID)
		return set_detail(p, RSIM_E_PROTOCOL, "malformed ATR");
	*atr_len = n;
	if (info.has_tck && !info.tck_ok)
		log_warn("%s: ATR checksum TCK does not match", p->cfg.dev);
	/* §8.3: in specific mode the card runs at TA1's rate at once, and
	 * this reader has no way to follow it */
	if (info.specific_mode && info.has_ta1 && (info.fi > 1 || info.di != 1))
		log_warn("%s: card is in specific mode at Fi=%u Di=%u; this reader runs at F=372 D=1",
			 p->cfg.dev, atr_fi_value(info.fi), atr_di_value(info.di));
	if (!(info.protocols & 1))
		return set_detail(p, RSIM_E_PROTOCOL, "card does not offer T=0");
	p->wwt_ms = t0_wwt_ms(p->cfg.clock_khz, info.wi) + USB_MARGIN_MS;
	log_dbg("%s: ATR %zu bytes, %s convention, WI %u, WWT %u ms", p->cfg.dev, n,
		p->inverse ? "inverse" : "direct", info.wi, p->wwt_ms);
	return RSIM_OK;
}

/* read and drop until nothing has come for RESET_QUIET_MS, at most
 * DRAIN_MAX_MS: an ATR is at most 33 characters, 35 ms at 9600 baud */
static void drain_quiet(struct phoenix *p)
{
	struct timespec t0, t1;

	rx_drop(p);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		if (p->io->ops->read(p->io, p->rx, sizeof(p->rx), RESET_QUIET_MS) <= 0)
			break;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 > DRAIN_MAX_MS)
			break;
	}
	p->rx_pos = p->rx_len = 0;
}

static int pulse_reset(struct phoenix *p, uint8_t *atr, size_t *atr_len)
{
	int r;

	/* parity unchecked until TS has decided the convention */
	r = apply_line(p, false, false);
	/* whatever the card sent before the reset is from the old session —
	 * including an ATR still on its way: opening the reader leaves the
	 * line released, the card answers that, and over USB those bytes
	 * arrive tens of milliseconds late. A flush alone catches only what is
	 * already in the host buffer; wait for the line to go quiet, or the
	 * silence check below reads that ATR as a card talking in reset
	 * (HW-observed with the Smartmouse USB, 2026-09-26). */
	drain_quiet(p);
	if (!r)
		r = reset_line(p, true);
	if (r)
		return r;
	sleep_ms(RESET_HOLD_MS);
	/* A card held in reset is silent. One that talks now was RELEASED by
	 * what we meant as the reset: the polarity is the wrong one, and the
	 * "release" that follows would put it into reset for good. Dropping
	 * these bytes is not enough — over USB part of such an ATR arrives
	 * after the flush and passes for an answer to the release, and the
	 * first command then times out on a card held in reset (HW-observed
	 * with the Smartmouse USB on the RG650E, 2026-09-26). */
	int quiet = p->io->ops->read(p->io, p->rx, sizeof(p->rx), RESET_QUIET_MS);

	if (quiet > 0) {
		char hex[3 * 16 + 1];
		int i;

		for (i = 0; i < quiet && i < 16; i++)
			snprintf(hex + 2 * i, sizeof(hex) - 2 * i, "%02X", p->rx[i]);
		log_dbg("%s: %d bytes while held in reset: %s%s", p->cfg.dev, quiet, hex,
			quiet > 16 ? "..." : "");
		rx_drop(p);
		reset_line(p, false);
		return set_detail(p, RSIM_E_NO_CARD, "the card talks while held in reset: wrong reset polarity");
	}
	r = reset_line(p, false);
	if (r)
		return r;
	return read_atr(p, atr, atr_len);
}

static void set_polarity(struct phoenix *p, enum phoenix_reset mode)
{
	p->reset_dtr = mode == PHX_RESET_DTR || mode == PHX_RESET_DTR_INV;
	p->reset_inv = mode == PHX_RESET_RTS_INV || mode == PHX_RESET_DTR_INV;
}

static int ph_power_up(struct rsim_backend *be, uint8_t *atr, size_t *atr_len)
{
	struct phoenix *p = (struct phoenix *)be;
	int r;

	if (p->polarity_known)
		return pulse_reset(p, atr, atr_len);
	/* auto: the two common wirings differ only in RTS polarity. Trying
	 * one then the other costs one ATR timeout, which the default keeps
	 * well inside the modem's 7 s ATR budget. */
	set_polarity(p, PHX_RESET_RTS);
	r = pulse_reset(p, atr, atr_len);
	if (r == RSIM_E_NO_CARD) {
		log_dbg("%s: no ATR with RTS reset, trying inverted RTS", p->cfg.dev);
		set_polarity(p, PHX_RESET_RTS_INV);
		r = pulse_reset(p, atr, atr_len);
	}
	if (!r) {
		p->polarity_known = true;
		log_notice("%s: card answers to %s reset", p->cfg.dev,
			   p->reset_inv ? "inverted RTS" : "RTS");
	}
	return r;
}

static int ph_reset(struct rsim_backend *be, uint8_t *atr, size_t *atr_len)
{
	/* without VCC control a warm and a cold start are the same pulse */
	return ph_power_up(be, atr, atr_len);
}

static int ph_power_down(struct rsim_backend *be)
{
	struct phoenix *p = (struct phoenix *)be;

	/* the card cannot be unpowered from here; held in reset it is
	 * inactive, which is what the modem asked for */
	if (!p->polarity_known)
		return RSIM_OK;
	return reset_line(p, true);
}

static int ph_transmit(struct rsim_backend *be, const uint8_t *tpdu, size_t len,
		       uint8_t *resp, size_t *resp_len)
{
	struct phoenix *p = (struct phoenix *)be;
	struct t0_chan ch = { chan_send, chan_recv, p };

	/* a byte still in the buffer now is a late answer to an exchange
	 * that already failed; left there it would be read as this one's
	 * procedure byte */
	rx_drop(p);
	return t0_transceive(&ch, p->wwt_ms, tpdu, len, resp, resp_len,
			     be->detail, sizeof(be->detail));
}

static int ph_present(struct rsim_backend *be)
{
	struct phoenix *p = (struct phoenix *)be;
	int cts, dsr, cd;

	if (p->cfg.detect == PHX_DETECT_NONE || !p->io->ops->get_modem ||
	    p->io->ops->get_modem(p->io, &cts, &dsr, &cd))
		return -1;
	switch (p->cfg.detect) {
	case PHX_DETECT_CTS:	return cts;
	case PHX_DETECT_DSR:	return dsr;
	default:		return cd;
	}
}

static void ph_close(struct rsim_backend *be)
{
	struct phoenix *p = (struct phoenix *)be;

	p->io->ops->close(p->io);
	free(p);
}

static void ph_info(struct rsim_backend *be, struct jw *w)
{
	struct phoenix *p = (struct phoenix *)be;
	static const char *const resets[] = { "rts", "rts_inv", "dtr", "dtr_inv" };

	jw_int(w, "clock_khz", (long)p->cfg.clock_khz);
	if (p->baud)
		jw_int(w, "baud", (long)p->baud);
	/* what the card answered to, once one did */
	if (p->polarity_known)
		jw_str(w, "reset_line", resets[(p->reset_dtr ? 2 : 0) + (p->reset_inv ? 1 : 0)]);
	if (p->echo >= 0)
		jw_bool(w, "echo", p->echo == 1);
	jw_str(w, "convention", p->inverse ? "inverse" : "direct");
	if (p->io->ops->info)
		p->io->ops->info(p->io, w);
}

static const struct rsim_backend_ops phoenix_ops = {
	.name = "phoenix",
	.power_up = ph_power_up,
	.reset = ph_reset,
	.power_down = ph_power_down,
	.transmit = ph_transmit,
	.present = ph_present,
	.close = ph_close,
	.info = ph_info,
};

struct rsim_backend *phoenix_open(const struct phoenix_cfg *cfg, struct phx_io *io)
{
	struct phoenix *p;

	if (!io)
		io = phx_tty_open(cfg->dev);
	if (!io)
		return NULL;
	p = calloc(1, sizeof(*p));
	if (!p) {
		io->ops->close(io);
		return NULL;
	}
	p->be.ops = &phoenix_ops;
	p->be.reader = cfg->dev;
	p->cfg = *cfg;
	p->io = io;
	p->echo = -1;
	p->pushback = -1;
	p->baud = (cfg->clock_khz * 1000u + 186u) / 372u;
	p->wwt_ms = t0_wwt_ms(cfg->clock_khz, 10) + USB_MARGIN_MS;
	if (apply_line(p, false, false)) {
		log_err("%s: %s", cfg->dev, p->be.detail);
		goto fail;
	}

	if (cfg->reset != PHX_RESET_AUTO) {
		set_polarity(p, cfg->reset);
		p->polarity_known = true;
	} else {
		set_polarity(p, PHX_RESET_RTS);
	}
	/* some readers take their supply from DTR, so it stays high unless
	 * it is the reset line itself */
	if (!p->reset_dtr && set_line(p, true, true)) {
		log_err("%s: %s", cfg->dev, p->be.detail);
		goto fail;
	}
	log_notice("%s: phoenix reader, %u kHz card clock, %u baud", cfg->dev,
		   cfg->clock_khz, p->baud);
	return &p->be;

fail:
	io->ops->close(io);
	free(p);
	return NULL;
}
