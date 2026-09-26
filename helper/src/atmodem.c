/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * The card of another modem, over its AT port. The modem keeps the card; its
 * APDUs travel as AT+CSIM (3GPP TS 27.007 §8.17: `AT+CSIM=<length>,"<cmd>"`,
 * the length in hex CHARACTERS, answered `+CSIM: <length>,"<response>"`),
 * exactly the path wwand-rsim's AT donor takes inside the router.
 *
 * Two things this backend cannot do and says so honestly:
 * - the ATR: plain AT has no standard command for it, so power-up answers the
 *   minimal T=0 ATR 3B 00 (TS direct convention, T0 00: no interface and no
 *   historical bytes, ISO/IEC 7816-3:2006 §8.2) — what the UIM Remote service
 *   needs to know is that the card speaks T=0;
 * - power and reset: the card stays with its modem, powered; both answer that
 *   ATR again.
 *
 * One card, one registration: unless told to keep it, the modem's radio is
 * switched off (AT+CFUN=4: RF off, the SIM stays reachable) for as long as
 * the card is used elsewhere, and its previous mode is restored at the end.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "atmodem.h"
#include "json.h"
#include "log.h"

#define LINE_MAX_AT 1200	/* +CSIM with 258 bytes = 516 hex characters */

struct at_backend {
	struct rsim_backend be;
	int fd;
	int cfun_prev;		/* -1: not changed by us */
	char rbuf[4096];
	size_t rlen;
};

static const uint8_t ATR_T0_MINIMAL[] = { 0x3B, 0x00 };

static speed_t speed_of(unsigned baud)
{
	switch (baud) {
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 230400: return B230400;
	case 460800: return B460800;
	case 921600: return B921600;
	default: return B115200;
	}
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* one line from the port, CR/LF stripped; 1 a line, 0 timeout, -1 error */
static int read_line(struct at_backend *a, char *out, size_t cap, long deadline)
{
	for (;;) {
		char *nl = memchr(a->rbuf, '\n', a->rlen);
		char *cr = memchr(a->rbuf, '\r', a->rlen);
		char *end = (!nl || (cr && cr < nl)) ? cr : nl;

		if (end) {
			size_t n = (size_t)(end - a->rbuf);

			if (n >= cap)
				n = cap - 1;
			memcpy(out, a->rbuf, n);
			out[n] = '\0';
			a->rlen -= (size_t)(end - a->rbuf) + 1;
			memmove(a->rbuf, end + 1, a->rlen);
			if (n)
				return 1;
			continue;	/* the empty line between CR and LF */
		}

		long left = deadline - now_ms();
		struct pollfd p = { .fd = a->fd, .events = POLLIN };
		ssize_t r;

		if (left <= 0)
			return 0;
		if (poll(&p, 1, (int)left) <= 0)
			continue;
		if (a->rlen >= sizeof(a->rbuf))
			a->rlen = 0;	/* a line longer than any AT answer: garbage */
		r = read(a->fd, a->rbuf + a->rlen, sizeof(a->rbuf) - a->rlen);
		if (r < 0 && errno != EINTR && errno != EAGAIN)
			return -1;
		if (r > 0)
			a->rlen += (size_t)r;
	}
}

/* Send one command and collect its answer. want: the prefix of the line to
 * keep (e.g. "+CSIM:"), copied into got. Returns RSIM_OK on OK, RSIM_E_IO on
 * ERROR (err gets the line), RSIM_E_TIMEOUT without a final result. */
static int at_cmd(struct at_backend *a, const char *cmd, const char *want, char *got, size_t gotcap,
		  int timeout_ms, char *err, size_t errcap)
{
	char line[LINE_MAX_AT];
	long deadline = now_ms() + timeout_ms;
	int r;

	if (got && gotcap)
		got[0] = '\0';
	tcflush(a->fd, TCIFLUSH);
	a->rlen = 0;
	log_dbg("at> %s", cmd);
	if (write(a->fd, cmd, strlen(cmd)) < 0 || write(a->fd, "\r", 1) < 0)
		return RSIM_E_IO;

	while ((r = read_line(a, line, sizeof(line), deadline)) == 1) {
		log_dbg("at< %s", line);
		if (!strcmp(line, "OK"))
			return RSIM_OK;
		if (!strcmp(line, "ERROR") || !strncmp(line, "+CME ERROR", 10) || !strncmp(line, "+CMS ERROR", 10)) {
			if (err && errcap)
				snprintf(err, errcap, "%s", line);
			return RSIM_E_IO;
		}
		if (want && got && !strncmp(line, want, strlen(want)))
			snprintf(got, gotcap, "%s", line);
	}
	return r < 0 ? RSIM_E_IO : RSIM_E_TIMEOUT;
}

int atmodem_csim_answer(const char *line, unsigned char *resp, int cap)
{
	const char *p = strchr(line, ',');
	char hex[2 * 260 + 2];
	size_t n = 0;

	if (strncmp(line, "+CSIM:", 6) || !p)
		return -1;
	for (p++; *p == ' ' || *p == '"'; p++)
		;
	while (*p && *p != '"' && n < sizeof(hex) - 1)
		hex[n++] = *p++;
	hex[n] = '\0';
	return hex_decode(hex, resp, (size_t)cap);
}

static int at_power_up(struct rsim_backend *be, uint8_t *atr, size_t *atr_len)
{
	struct at_backend *a = (struct at_backend *)be;
	char got[200], err[120];
	int r = at_cmd(a, "AT+CPIN?", "+CPIN:", got, sizeof(got), 5000, err, sizeof(err));

	/* a card that asks for its PIN is still a card: the APDUs are the
	 * target modem's business, and it verifies the PIN itself */
	if (r == RSIM_E_IO) {
		snprintf(be->detail, RSIM_DETAIL_MAX, "the modem reports no usable card (%s)", err);
		return RSIM_E_NO_CARD;
	}
	if (r)
		return r;
	memcpy(atr, ATR_T0_MINIMAL, sizeof(ATR_T0_MINIMAL));
	*atr_len = sizeof(ATR_T0_MINIMAL);
	return RSIM_OK;
}

static int at_reset(struct rsim_backend *be, uint8_t *atr, size_t *atr_len)
{
	return at_power_up(be, atr, atr_len);
}

static int at_power_down(struct rsim_backend *be)
{
	(void)be;	/* the card stays with its modem */
	return RSIM_OK;
}

static int at_transmit(struct rsim_backend *be, const uint8_t *tpdu, size_t len, uint8_t *resp, size_t *resp_len)
{
	struct at_backend *a = (struct at_backend *)be;
	char cmd[2 * RSIM_TPDU_MAX + 32], got[LINE_MAX_AT], err[120];
	size_t i, o;
	int r, n;

	o = (size_t)snprintf(cmd, sizeof(cmd), "AT+CSIM=%zu,\"", 2 * len);
	for (i = 0; i < len; i++)
		o += (size_t)snprintf(cmd + o, sizeof(cmd) - o, "%02X", tpdu[i]);
	snprintf(cmd + o, sizeof(cmd) - o, "\"");

	r = at_cmd(a, cmd, "+CSIM:", got, sizeof(got), 10000, err, sizeof(err));
	if (r == RSIM_E_IO) {
		snprintf(be->detail, RSIM_DETAIL_MAX, "AT+CSIM refused (%s)", err);
		return RSIM_E_IO;
	}
	if (r)
		return r;
	n = atmodem_csim_answer(got, resp, RSIM_RESP_MAX);
	if (n < 2) {
		snprintf(be->detail, RSIM_DETAIL_MAX, "no +CSIM answer");
		return RSIM_E_PROTOCOL;
	}
	*resp_len = (size_t)n;
	return RSIM_OK;
}

static int at_present(struct rsim_backend *be)
{
	(void)be;
	return -1;	/* asking the modem every second is not worth the traffic */
}

static void at_close(struct rsim_backend *be)
{
	struct at_backend *a = (struct at_backend *)be;
	char cmd[24];

	if (a->cfun_prev >= 0) {
		snprintf(cmd, sizeof(cmd), "AT+CFUN=%d", a->cfun_prev);
		if (at_cmd(a, cmd, NULL, NULL, 0, 15000, NULL, 0) == RSIM_OK)
			log_notice("%s: radio back to CFUN=%d", a->be.reader, a->cfun_prev);
		else
			log_warn("%s: could not restore CFUN=%d", a->be.reader, a->cfun_prev);
	}
	close(a->fd);
	free(a);
}

static const struct rsim_backend_ops AT_OPS = {
	.name = "at",
	.power_up = at_power_up,
	.reset = at_reset,
	.power_down = at_power_down,
	.transmit = at_transmit,
	.present = at_present,
	.close = at_close,
};

struct rsim_backend *atmodem_open(const struct at_cfg *cfg)
{
	struct at_backend *a = calloc(1, sizeof(*a));
	struct termios t;
	char got[80];
	int prev;

	if (!a)
		return NULL;
	a->cfun_prev = -1;
	a->fd = open(cfg->dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (a->fd < 0) {
		log_err("%s: %s", cfg->dev, strerror(errno));
		free(a);
		return NULL;
	}
	if (tcgetattr(a->fd, &t) == 0) {
		cfmakeraw(&t);
		t.c_cflag |= CLOCAL | CREAD;
		cfsetspeed(&t, speed_of(cfg->baud ? cfg->baud : 115200));
		tcsetattr(a->fd, TCSANOW, &t);
	}
	a->be.ops = &AT_OPS;
	a->be.reader = cfg->dev;

	/* echo off, so a command never reads as its own answer; numeric
	 * errors, so "no card" can be told from a refused command */
	if (at_cmd(a, "ATE0", NULL, NULL, 0, 3000, NULL, 0) != RSIM_OK) {
		log_err("%s: no answer to AT — not a modem's AT port?", cfg->dev);
		close(a->fd);
		free(a);
		return NULL;
	}
	at_cmd(a, "AT+CMEE=1", NULL, NULL, 0, 3000, NULL, 0);

	if (!cfg->radio_keep) {
		if (at_cmd(a, "AT+CFUN?", "+CFUN:", got, sizeof(got), 5000, NULL, 0) == RSIM_OK &&
		    sscanf(got, "+CFUN: %d", &prev) == 1 && prev != 4) {
			if (at_cmd(a, "AT+CFUN=4", NULL, NULL, 0, 15000, NULL, 0) == RSIM_OK) {
				a->cfun_prev = prev;
				log_notice("%s: radio off (CFUN=4) while its card is used elsewhere", cfg->dev);
			} else {
				/* two modems must not register with one card */
				log_err("%s: cannot switch the radio off (AT+CFUN=4); --at-radio keep if that is intended",
					cfg->dev);
				close(a->fd);
				free(a);
				return NULL;
			}
		}
	}
	log_notice("%s: the card of this modem, over AT+CSIM", cfg->dev);
	return &a->be;
}
