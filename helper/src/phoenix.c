/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "atr.h"
#include "log.h"
#include "phoenix.h"
#include "t0.h"

/*
 * Phoenix / Smartmouse: a USB-serial bridge whose RX and TX both sit on the
 * card's single I/O line, with the card reset on RTS or DTR and a fixed card
 * clock from the reader. There is no VCC control: "power" is the reset line.
 *
 * Test hook: RSIM_TEST_MCTRL=<path> makes the backend write "RTS=0|1" /
 * "DTR=0|1" lines to that file instead of driving the modem-control lines,
 * because a pseudo-terminal has none and the simulated card needs to see the
 * reset pulse. It is read only here and set only by tests/test_e2e.py; a
 * real reader is never opened with it.
 */

#define RESET_HOLD_MS	50
/* per-byte wait for the echo of a byte just written: one character is ~1.3
 * ms at 9600 baud, the rest is USB latency (FTDI's latency timer alone is
 * 16 ms by default) */
#define ECHO_TIMEOUT_MS	200
/* added to the card's own waiting times for the same USB round trip */
#define USB_MARGIN_MS	50

struct phoenix {
	struct rsim_backend be;
	struct phoenix_cfg cfg;
	int fd;
	int mctrl_fd;		/* test hook, -1 on real hardware */
	struct termios tio;
	unsigned baud;

	int reset_bit;		/* TIOCM_RTS or TIOCM_DTR */
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

static int set_line(struct phoenix *p, int bit, bool level)
{
	if (p->mctrl_fd >= 0) {
		dprintf(p->mctrl_fd, "%s=%d\n", bit == TIOCM_RTS ? "RTS" : "DTR",
			level ? 1 : 0);
		return RSIM_OK;
	}
	if (ioctl(p->fd, level ? TIOCMBIS : TIOCMBIC, &bit) < 0)
		return set_detail(p, RSIM_E_IO, "modem control: %s", strerror(errno));
	return RSIM_OK;
}

static int reset_line(struct phoenix *p, bool assert)
{
	return set_line(p, p->reset_bit, assert != p->reset_inv);
}

/* Parity checking is armed only once TS has told the convention: an
 * inverse-convention character read by a direct-framed UART has odd total
 * parity (all nine data+parity levels are inverted, and an even count of
 * ones out of nine becomes odd), so with even parity checked the TS of an
 * inverse card would be dropped before it could be recognised. */
static int apply_line(struct phoenix *p, bool check_parity, bool odd)
{
	struct termios *t = &p->tio;
	int r;

	t->c_iflag &= ~(tcflag_t)(INPCK | PARMRK | IGNPAR);
	if (check_parity)
		/* a byte that fails parity is dropped rather than handed on:
		 * the exchange then fails as a timeout, where a corrupted
		 * byte would have been read as data or a procedure byte */
		t->c_iflag |= INPCK | IGNPAR;
	t->c_cflag &= ~(tcflag_t)PARODD;
	if (odd)
		t->c_cflag |= PARODD;
	if (tcsetattr(p->fd, TCSANOW, t) < 0 && p->mctrl_fd < 0)
		return set_detail(p, RSIM_E_IO, "tcsetattr: %s", strerror(errno));
	/* re-applied every time: whether a libc tcsetattr keeps a BOTHER
	 * rate or rewrites it from its own speed fields is not something to
	 * depend on */
	r = phoenix_set_baud(p->fd, p->baud);
	if (r < 0) {
		static bool warned;

		if (!warned)
			log_warn("%s: exact %u baud not settable (%s), using nearest standard rate",
				 p->cfg.dev, p->baud, strerror(-r));
		warned = true;
	}
	return RSIM_OK;
}

static speed_t nearest_speed(unsigned baud)
{
	static const struct { unsigned rate; speed_t code; } tab[] = {
		{ 4800, B4800 }, { 9600, B9600 }, { 19200, B19200 },
		{ 38400, B38400 }, { 57600, B57600 }, { 115200, B115200 },
	};
	size_t i, best = 0;

	for (i = 1; i < sizeof(tab) / sizeof(tab[0]); i++)
		if (abs((int)tab[i].rate - (int)baud) < abs((int)tab[best].rate - (int)baud))
			best = i;
	return tab[best].code;
}

static void rx_drop(struct phoenix *p)
{
	tcflush(p->fd, TCIFLUSH);
	p->rx_pos = p->rx_len = 0;
	p->pushback = -1;
}

/* one byte as it came off the wire, before any convention mapping */
static int recv_raw(struct phoenix *p, uint8_t *b, unsigned timeout_ms)
{
	if (p->rx_pos == p->rx_len) {
		struct pollfd pfd = { .fd = p->fd, .events = POLLIN };
		ssize_t n;
		int r;

		do
			r = poll(&pfd, 1, (int)timeout_ms);
		while (r < 0 && errno == EINTR);
		if (r < 0)
			return set_detail(p, RSIM_E_IO, "poll: %s", strerror(errno));
		if (r == 0)
			return RSIM_E_TIMEOUT;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			return set_detail(p, RSIM_E_IO, "reader gone");
		n = read(p->fd, p->rx, sizeof(p->rx));
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return RSIM_E_TIMEOUT;
		if (n <= 0)
			return set_detail(p, RSIM_E_IO, "read: %s",
					  n ? strerror(errno) : "end of file");
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
	size_t i, off = 0;
	uint8_t b;
	int r;

	if (len > sizeof(wire))
		return set_detail(p, RSIM_E_IO, "write of %zu bytes", len);
	for (i = 0; i < len; i++)
		wire[i] = p->inverse ? atr_inverse(buf[i]) : buf[i];
	while (off < len) {
		ssize_t n = write(p->fd, wire + off, len - off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return set_detail(p, RSIM_E_IO, "write: %s", strerror(errno));
		off += (size_t)n;
	}
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

static int pulse_reset(struct phoenix *p, uint8_t *atr, size_t *atr_len)
{
	int r;

	/* parity unchecked until TS has decided the convention */
	r = apply_line(p, false, false);
	if (!r)
		r = reset_line(p, true);
	if (r)
		return r;
	sleep_ms(RESET_HOLD_MS);
	/* whatever the card sent before the reset is from the old session */
	rx_drop(p);
	r = reset_line(p, false);
	if (r)
		return r;
	return read_atr(p, atr, atr_len);
}

static void set_polarity(struct phoenix *p, enum phoenix_reset mode)
{
	p->reset_bit = mode == PHX_RESET_DTR || mode == PHX_RESET_DTR_INV ?
		       TIOCM_DTR : TIOCM_RTS;
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
	int bits, want;

	switch (p->cfg.detect) {
	case PHX_DETECT_CTS:	want = TIOCM_CTS; break;
	case PHX_DETECT_DSR:	want = TIOCM_DSR; break;
	case PHX_DETECT_CD:	want = TIOCM_CD; break;
	default:		return -1;
	}
	if (ioctl(p->fd, TIOCMGET, &bits) < 0)
		return -1;
	return !!(bits & want);
}

static void ph_close(struct rsim_backend *be)
{
	struct phoenix *p = (struct phoenix *)be;

	close(p->fd);
	if (p->mctrl_fd >= 0)
		close(p->mctrl_fd);
	free(p);
}

static const struct rsim_backend_ops phoenix_ops = {
	.name = "phoenix",
	.power_up = ph_power_up,
	.reset = ph_reset,
	.power_down = ph_power_down,
	.transmit = ph_transmit,
	.present = ph_present,
	.close = ph_close,
};

struct rsim_backend *phoenix_open(const struct phoenix_cfg *cfg)
{
	struct phoenix *p = calloc(1, sizeof(*p));
	const char *mctrl = getenv("RSIM_TEST_MCTRL");
	int fl;

	if (!p)
		return NULL;
	p->be.ops = &phoenix_ops;
	p->be.reader = cfg->dev;
	p->cfg = *cfg;
	p->mctrl_fd = -1;
	p->echo = -1;
	p->pushback = -1;
	p->baud = (cfg->clock_khz * 1000u + 186u) / 372u;
	p->wwt_ms = t0_wwt_ms(cfg->clock_khz, 10) + USB_MARGIN_MS;

	/* O_NONBLOCK only so that open does not wait for carrier; the
	 * descriptor is used blocking with poll() and VMIN=VTIME=0 */
	p->fd = open(cfg->dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (p->fd < 0) {
		log_err("%s: %s", cfg->dev, strerror(errno));
		free(p);
		return NULL;
	}
	fl = fcntl(p->fd, F_GETFL);
	if (fl >= 0)
		fcntl(p->fd, F_SETFL, fl & ~O_NONBLOCK);
	if (mctrl) {
		p->mctrl_fd = open(mctrl, O_WRONLY | O_CLOEXEC);
		if (p->mctrl_fd < 0) {
			log_err("%s: %s", mctrl, strerror(errno));
			goto fail;
		}
		log_warn("test mode: modem-control lines go to %s", mctrl);
	}

	if (tcgetattr(p->fd, &p->tio) < 0) {
		log_err("%s: not a tty: %s", cfg->dev, strerror(errno));
		goto fail;
	}
	cfmakeraw(&p->tio);
	/* §7.1/§7.2: one start bit, 8 data bits, even parity, and a guard
	 * time of 2 etu, which two stop bits provide */
	p->tio.c_cflag &= ~(tcflag_t)(CSIZE | CRTSCTS | HUPCL);
	p->tio.c_cflag |= CS8 | PARENB | CSTOPB | CREAD | CLOCAL;
	p->tio.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);
	p->tio.c_cc[VMIN] = 0;
	p->tio.c_cc[VTIME] = 0;
	cfsetispeed(&p->tio, nearest_speed(p->baud));
	cfsetospeed(&p->tio, nearest_speed(p->baud));
	if (apply_line(p, false, false))
		goto fail;

	if (cfg->reset != PHX_RESET_AUTO) {
		set_polarity(p, cfg->reset);
		p->polarity_known = true;
	} else {
		set_polarity(p, PHX_RESET_RTS);
	}
	/* some readers take their supply from DTR, so it stays high unless
	 * it is the reset line itself */
	if (p->reset_bit != TIOCM_DTR && set_line(p, TIOCM_DTR, true)) {
		log_err("%s: %s", cfg->dev, p->be.detail);
		goto fail;
	}
	log_notice("%s: phoenix reader, %u kHz card clock, %u baud", cfg->dev,
		   cfg->clock_khz, p->baud);
	return &p->be;

fail:
	if (p->mctrl_fd >= 0)
		close(p->mctrl_fd);
	close(p->fd);
	free(p);
	return NULL;
}
