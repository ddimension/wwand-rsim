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
#include <strings.h>
#include <termios.h>
#include <time.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atmodem.h"
#include "json.h"
#include "log.h"
#include "meta.h"

#define LINE_MAX_AT 1200	/* +CSIM with 258 bytes = 516 hex characters */

/* at_cmd's answers besides the RSIM codes: the modem answered ERROR/+CME */
#define AT_REFUSED 1

struct at_backend {
	struct rsim_backend be;
	int fd;
	bool radio_keep;
	bool dirty;		/* a command timed out: its answer may still come */
	int cfun_prev;		/* -1: not changed by us */
	char mark[300];		/* where the mode before ours is kept */
	/* who the modem is and which card it has, read once at the open */
	char manuf[64], model[64], rev[96], imei[32], iccid[32];
	long next_check;	/* the next look at the radio (at_tick) */
	char rbuf[4096];
	size_t rlen;
};

static const uint8_t ATR_T0_MINIMAL[] = { 0x3B, 0x00 };

int atmodem_speed(unsigned baud)
{
	switch (baud) {
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 921600: return B921600;
	default: return -1;
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
		/* a port that went away (USB unplugged) signals hang-up and reads
		 * 0 for ever: not a timeout to spin out at full CPU */
		if (p.revents & (POLLHUP | POLLERR | POLLNVAL) && !(p.revents & POLLIN)) {
			errno = EIO;
			return -1;
		}
		if (a->rlen >= sizeof(a->rbuf))
			a->rlen = 0;	/* a line longer than any AT answer: garbage */
		r = read(a->fd, a->rbuf + a->rlen, sizeof(a->rbuf) - a->rlen);
		if (r == 0) {
			errno = EIO;
			return -1;
		}
		if (r < 0 && errno != EINTR && errno != EAGAIN)
			return -1;
		if (r > 0)
			a->rlen += (size_t)r;
	}
}

/* All of buf, on a non-blocking port: a short write or EAGAIN would send half
 * a command, or one without its CR, and the modem would never answer it. */
static int write_all(int fd, const char *buf, size_t len, long deadline)
{
	while (len) {
		ssize_t w = write(fd, buf, len);

		if (w > 0) {
			buf += w;
			len -= (size_t)w;
			continue;
		}
		if (w < 0 && errno != EAGAIN && errno != EINTR)
			return -1;

		long left = deadline - now_ms();
		struct pollfd p = { .fd = fd, .events = POLLOUT };

		if (left <= 0 || poll(&p, 1, (int)left) < 0)
			return -1;
	}
	return 0;
}

/* the number of a +CME ERROR line, -1 for anything else: "ERROR: 10" as a
 * substring is also CME 100..109 — 100, "unknown", is a common AT+CSIM
 * refusal and not a card that left */
static int cme_of(const char *line)
{
	int c;

	return (sscanf(line, "+CME ERROR: %d", &c) == 1) ? c : -1;
}

/* After a timeout the modem may still answer that command: its late OK or
 * +CSIM would be taken for the next command's. A plain AT with its OK read
 * shows the line is quiet again; everything before that OK is discarded. */
static void resync(struct at_backend *a)
{
	char line[LINE_MAX_AT];
	long deadline = now_ms() + 3000;
	int r;

	a->dirty = false;
	tcflush(a->fd, TCIFLUSH);
	a->rlen = 0;
	if (write_all(a->fd, "AT\r", 3, deadline) < 0)
		return;
	/* The first OK may be the late one of the command that timed out: after
	 * an OK the line has to stay quiet for a moment, or it was not ours */
	while ((r = read_line(a, line, sizeof(line), deadline)) == 1) {
		if (strcmp(line, "OK"))
			continue;
		if (read_line(a, line, sizeof(line), now_ms() + 300) == 0)
			return;
		if (!strcmp(line, "OK"))	/* ours, after the late one */
			if (read_line(a, line, sizeof(line), now_ms() + 300) == 0)
				return;
	}
	a->dirty = true;	/* still not quiet: again before the next one */
}

/* Send one command and collect its answer. want: the prefix of the line to
 * keep (e.g. "+CSIM:"), copied into got; lines of other kinds (echo,
 * unsolicited results such as +QIND or RDY) are passed over. Returns RSIM_OK
 * on OK, AT_REFUSED on ERROR/+CME/+CMS (err gets the line), RSIM_E_IO when
 * the port fails (err says how), RSIM_E_TIMEOUT without a final result. */
static int at_cmd(struct at_backend *a, const char *cmd, const char *want, char *got, size_t gotcap,
		  int timeout_ms, char *err, size_t errcap)
{
	char line[LINE_MAX_AT];
	long deadline = now_ms() + timeout_ms;
	int r;

	if (got && gotcap)
		got[0] = '\0';
	if (err && errcap)
		snprintf(err, errcap, "no answer");
	if (a->dirty)
		resync(a);
	tcflush(a->fd, TCIFLUSH);
	a->rlen = 0;
	log_dbg("at> %s", cmd);
	if (write_all(a->fd, cmd, strlen(cmd), deadline) < 0 || write_all(a->fd, "\r", 1, deadline) < 0) {
		if (err && errcap)
			snprintf(err, errcap, "write: %s", strerror(errno));
		return RSIM_E_IO;
	}

	while ((r = read_line(a, line, sizeof(line), deadline)) == 1) {
		log_dbg("at< %s", line);
		if (!strcmp(line, "OK"))
			return RSIM_OK;
		/* A phone whose application side filters the AT commands meant for
		 * its modem answers them with a refusal instead of ERROR, followed
		 * by OK (Samsung: "PACM(AP),NOT_ALLOWED_CRO", HW-seen on a Galaxy
		 * S20 FE, 2026-09-27) — that is no value and no success. */
		if (strstr(line, "NOT_ALLOWED")) {
			if (err && errcap)
				snprintf(err, errcap, "%.*s", (int)(errcap - 1), line);
			a->dirty = true;	/* its OK is still to come */
			return AT_REFUSED;
		}
		/* "+CME Error:" too — Samsung spells it so (Galaxy S20 FE) */
		if (!strcmp(line, "ERROR") || !strncasecmp(line, "+CME ERROR", 10) || !strncasecmp(line, "+CMS ERROR", 10)) {
			/* an error line is short; the cut is only for the compiler */
			if (err && errcap)
				snprintf(err, errcap, "%.*s", (int)(errcap - 1), line);
			return AT_REFUSED;
		}
		if (want && got && !strncmp(line, want, strlen(want)))
			snprintf(got, gotcap, "%s", line);
	}
	if (r < 0) {
		if (err && errcap)
			snprintf(err, errcap, "read: %s", strerror(errno));
		return RSIM_E_IO;
	}
	a->dirty = true;
	return RSIM_E_TIMEOUT;
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

/* The mode the modem had before we switched its radio off, kept in a file
 * until it is restored. A helper killed hard (SIGKILL, power loss on the SIM
 * host) cannot restore it, and the next start would read CFUN=4 as "how it
 * was" and never switch the radio back on: the file tells it otherwise. In
 * /tmp — a reboot of the SIM host restarts the modem anyway. */
static void mark_path(const char *dev, char *out, size_t cap)
{
	size_t o;
	const char *p;

	o = (size_t)snprintf(out, cap, "/tmp/rsim-card-cfun-");
	for (p = dev; *p && o + 1 < cap; p++)
		out[o++] = (*p == '/') ? '_' : *p;
	out[o] = '\0';
}

static int mark_read(const char *path)
{
	FILE *f = fopen(path, "r");
	int v = -1;

	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

/* The value of a query answered with one line (+CGMI: "Quectel", or just
 * Quectel): without prefix and quotes; "" when refused. */
static void at_value(struct at_backend *a, const char *cmd, char *out, size_t cap)
{
	char got[200], *v;

	out[0] = '\0';
	if (at_cmd(a, cmd, "", got, sizeof(got), 3000, NULL, 0) != RSIM_OK || !got[0])
		return;
	v = got;
	if (*v == '+' || *v == '^') {
		char *c = strchr(v, ':');

		if (c)
			v = c + 1;
	}
	while (*v == ' ' || *v == '"')
		v++;
	snprintf(out, cap, "%.*s", (int)cap - 1, v);
	out[strcspn(out, "\"")] = '\0';
}

/* The card's ICCID: every vendor spells the command differently; the first
 * answer with 18..22 digits wins (a trailing F of 19-digit ones dropped). */
static void at_iccid(struct at_backend *a)
{
	static const char *const cmds[] = { "AT+CCID", "AT+QCCID", "AT^ICCID?", "AT+ICCID", NULL };
	int i;

	for (i = 0; cmds[i] && !a->iccid[0]; i++) {
		char v[64], *p;
		size_t n;

		at_value(a, cmds[i], v, sizeof(v));
		for (p = v; *p && !(*p >= '0' && *p <= '9'); p++)
			;
		n = strspn(p, "0123456789Ff");
		while (n && (p[n - 1] == 'F' || p[n - 1] == 'f'))
			n--;
		if (n >= 18 && n <= 22)
			snprintf(a->iccid, sizeof(a->iccid), "%.*s", (int)n, p);
	}
}

static void at_info(struct rsim_backend *be, struct jw *w)
{
	struct at_backend *a = (struct at_backend *)be;
	struct tty_meta m;

	if (!meta_tty_read("", be->reader, &m))
		meta_tty_write(w, &m);
	jw_opt(w, "modem_manufacturer", a->manuf);
	jw_opt(w, "modem_model", a->model);
	jw_opt(w, "modem_revision", a->rev);
	jw_opt(w, "modem_imei", a->imei);
	jw_opt(w, "iccid", a->iccid);
	jw_str(w, "radio", a->radio_keep ? "kept" : (a->cfun_prev >= 0 ? "off while lent" : "was off already"));
}

static int at_power_up(struct rsim_backend *be, uint8_t *atr, size_t *atr_len)
{
	struct at_backend *a = (struct at_backend *)be;
	char got[200], err[120] = "";
	int r = at_cmd(a, "AT+CPIN?", "+CPIN:", got, sizeof(got), 5000, err, sizeof(err));

	/* a card that asks for its PIN is still a card: the APDUs are the
	 * target modem's business, and it verifies the PIN itself. "SIM busy"
	 * (CME 14) is a card that is there — tried again, not reported gone */
	if (r == AT_REFUSED) {
		snprintf(be->detail, RSIM_DETAIL_MAX, "the modem reports no usable card (%s)", err);
		return (cme_of(err) == 14) ? RSIM_E_IO : RSIM_E_NO_CARD;
	}
	if (r) {
		snprintf(be->detail, RSIM_DETAIL_MAX, "the modem's AT port: %s", err);
		return r;
	}

	/* A modem that rebooted meanwhile (URC RDY) is back in its default
	 * mode, usually online, with the card still lent: every power-up and
	 * reset from the target checks the radio is still off. */
	if (!a->radio_keep) {
		char c[40];
		int mode;

		if (at_cmd(a, "AT+CFUN?", "+CFUN:", c, sizeof(c), 5000, NULL, 0) == RSIM_OK &&
		    sscanf(c, "+CFUN: %d", &mode) == 1 && mode != 4 && mode != 0) {
			log_warn("%s: radio is on again (CFUN=%d) while its card is lent — switching it off", be->reader, mode);
			/* a reboot also reset its echo and error format */
			at_cmd(a, "ATE0", NULL, NULL, 0, 3000, NULL, 0);
			at_cmd(a, "AT+CMEE=1", NULL, NULL, 0, 3000, NULL, 0);
			/* refused or unanswered: the card is not lent — two modems
			 * must not register with it — until a later power-up gets
			 * the radio off */
			if (at_cmd(a, "AT+CFUN=4", NULL, NULL, 0, 15000, NULL, 0) != RSIM_OK) {
				snprintf(be->detail, RSIM_DETAIL_MAX, "cannot switch the radio of this modem off again (CFUN=%d)", mode);
				return RSIM_E_IO;
			}
		}
	}
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
	char cmd[2 * RSIM_TPDU_MAX + 32], got[LINE_MAX_AT], err[120] = "";
	size_t i, o;
	int r, n;

	o = (size_t)snprintf(cmd, sizeof(cmd), "AT+CSIM=%zu,\"", 2 * len);
	for (i = 0; i < len; i++)
		o += (size_t)snprintf(cmd + o, sizeof(cmd) - o, "%02X", tpdu[i]);
	snprintf(cmd + o, sizeof(cmd) - o, "\"");

	r = at_cmd(a, cmd, "+CSIM:", got, sizeof(got), 10000, err, sizeof(err));
	if (r == AT_REFUSED) {
		snprintf(be->detail, RSIM_DETAIL_MAX, "AT+CSIM refused (%s)", err);
		/* CME 10, SIM not inserted: the card left — the powered state
		 * has to go, not only this command */
		return (cme_of(err) == 10) ? RSIM_E_NO_CARD : RSIM_E_IO;
	}
	if (r) {
		snprintf(be->detail, RSIM_DETAIL_MAX, "the modem's AT port: %s", err);
		return r;
	}
	n = atmodem_csim_answer(got, resp, RSIM_RESP_MAX);
	if (n < 2) {
		snprintf(be->detail, RSIM_DETAIL_MAX, "no +CSIM answer");
		return RSIM_E_PROTOCOL;
	}
	*resp_len = (size_t)n;
	return RSIM_OK;
}

/* While the card is lent, the modem's radio stays off: every 10 s — the
 * wwand plugin's tick for a QMI sponsor — it is read, and parked again when
 * something switched it back on (a modem that restarted, a hand elsewhere). */
static void at_tick(struct rsim_backend *be)
{
	struct at_backend *a = (struct at_backend *)be;
	char c[40];
	int mode;
	long now = now_ms();

	if (a->radio_keep || a->cfun_prev < 0 || now < a->next_check)
		return;
	a->next_check = now + 10000;
	if (at_cmd(a, "AT+CFUN?", "+CFUN:", c, sizeof(c), 5000, NULL, 0) == RSIM_OK &&
	    sscanf(c, "+CFUN: %d", &mode) == 1 && mode != 4 && mode != 0) {
		log_warn("%s: radio is on again (CFUN=%d) while its card is lent — parking it again", be->reader, mode);
		at_cmd(a, "AT+CFUN=4", NULL, NULL, 0, 15000, NULL, 0);
	}
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
		int tries;

		snprintf(cmd, sizeof(cmd), "AT+CFUN=%d", a->cfun_prev);
		/* once more after a pause: a modem still settling from the park
		 * refuses the first one. Not woken after that, the mark stays,
		 * and the next run on this port restores it. */
		for (tries = 0; tries < 2; tries++) {
			if (tries)
				usleep(2000000);
			if (at_cmd(a, cmd, NULL, NULL, 0, 15000, NULL, 0) == RSIM_OK)
				break;
		}
		if (tries < 2) {
			log_notice("%s: radio back to CFUN=%d", a->be.reader, a->cfun_prev);
			unlink(a->mark);
		} else
			log_warn("%s: could not restore CFUN=%d — the next run on this port tries again", a->be.reader, a->cfun_prev);
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
	.info = at_info,
	.tick = at_tick,
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
	a->radio_keep = cfg->radio_keep;
	/* a diagnostic port is not spoken AT to, even when named: what it
	 * would make of the bytes is the firmware's secret */
	{
		struct tty_meta m;

		if (!meta_tty_read("", cfg->dev, &m) && !strcmp(meta_tty_role(&m), "diag")) {
			log_err("%s: a diagnostic port (%s, interface %s) — not an AT port, not opened", cfg->dev, m.vid[0] ? m.vid : "?", m.ifnum);
			free(a);
			return NULL;
		}
	}
	a->fd = open(cfg->dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (a->fd < 0) {
		log_err("%s: %s", cfg->dev, strerror(errno));
		free(a);
		return NULL;
	}
	/* a modem port is a character device: anything else named here (a file,
	 * through a key that allows `at:*`) would be read out and written over
	 * with AT commands */
	{
		struct stat st;

		if (fstat(a->fd, &st) || !S_ISCHR(st.st_mode)) {
			log_err("%s: not a character device — not a modem port, not used", cfg->dev);
			close(a->fd);
			free(a);
			return NULL;
		}
	}
	/* One helper per port. Two would read each other's answers, and the
	 * first to end would switch the radio back on under the other, which
	 * still lends the card. flock, not TIOCEXCL: the lock ends with its
	 * process, a killed one included; the tty's exclusive flag outlives it
	 * while anything else holds the port open, and would lock out the very
	 * run that is to switch the radio back on. */
	/* Waited for, briefly: the previous helper on this port may still be
	 * switching the radio back on (up to 15 s) after its session ended. */
	{
		long until = now_ms() + 20000;

		while (flock(a->fd, LOCK_EX | LOCK_NB) != 0) {
			if (now_ms() >= until) {
				log_err("%s: in use by another rsim-card (or another program holding a lock)", cfg->dev);
				close(a->fd);
				free(a);
				return NULL;
			}
			usleep(200000);
		}
	}
	if (tcgetattr(a->fd, &t) == 0) {
		cfmakeraw(&t);
		t.c_cflag |= CLOCAL | CREAD;
		cfsetspeed(&t, (speed_t)atmodem_speed(cfg->baud ? cfg->baud : 115200));
		tcsetattr(a->fd, TCSANOW, &t);
	}
	a->be.ops = &AT_OPS;
	a->be.reader = cfg->dev;

	/* echo off, so a command never reads as its own answer; numeric
	 * errors, so "no card" can be told from a refused command */
	{
		char why[120] = "";

		if (at_cmd(a, "ATE0", NULL, NULL, 0, 3000, why, sizeof(why)) != RSIM_OK) {
			/* Samsung's AP-side filter: "PACM(AP),NOT_ALLOWED_CRO" or
			 * "+CME Error:PACM(AP),UNREGISTED" */
			if (strstr(why, "NOT_ALLOWED") || strstr(why, "PACM"))
				log_err("%s: the device refuses AT commands for its modem on this port (%s) — a phone whose AT access is locked",
					cfg->dev, why);
			else
				log_err("%s: no answer to AT — not a modem's AT port?", cfg->dev);
			close(a->fd);
			free(a);
			return NULL;
		}
	}
	at_cmd(a, "AT+CMEE=1", NULL, NULL, 0, 3000, NULL, 0);

	/* Does this port pass APDUs at all? A phone may answer AT and CPIN yet
	 * refuse AT+CSIM (Samsung, HW-seen on a Galaxy S20 FE, 2026-09-27): the
	 * modem using the card would wait on commands that never go through.
	 * SELECT MF changes nothing; any +CSIM answer (whatever its status
	 * word) says the path works. */
	{
		char got[200], why[120] = "";
		int r = at_cmd(a, "AT+CSIM=14,\"00A40004023F00\"", "+CSIM:", got, sizeof(got), 10000, why, sizeof(why));

		/* refused as a COMMAND — not a card that is missing or busy (CME
		 * 10/13/14 are the card's state, which power_up reports) */
		int locked = (r == AT_REFUSED && (!strcmp(why, "ERROR") || strstr(why, "NOT_ALLOWED") ||
						  !strcmp(why, "+CME ERROR: 3") || !strcmp(why, "+CME ERROR: 4")));

		if (locked || (r == RSIM_OK && !got[0])) {
			log_err("%s: the modem refuses AT+CSIM (%s) — its card cannot be used this way",
				cfg->dev, why[0] ? why : "no +CSIM answer");
			close(a->fd);
			free(a);
			return NULL;
		}
	}

	at_value(a, "AT+CGMI", a->manuf, sizeof(a->manuf));
	at_value(a, "AT+CGMM", a->model, sizeof(a->model));
	at_value(a, "AT+CGMR", a->rev, sizeof(a->rev));
	at_value(a, "AT+CGSN", a->imei, sizeof(a->imei));
	at_iccid(a);

	mark_path(cfg->dev, a->mark, sizeof(a->mark));
	if (!cfg->radio_keep) {
		int kept = mark_read(a->mark);

		/* a modem that does not say: taken as online, which is what the
		 * switch-off below is for */
		if (at_cmd(a, "AT+CFUN?", "+CFUN:", got, sizeof(got), 5000, NULL, 0) != RSIM_OK ||
		    sscanf(got, "+CFUN: %d", &prev) != 1)
			prev = 1;

		if (prev == 4 && kept >= 0 && kept != 4) {
			/* off since a run that could not clean up: that run's
			 * "before" is the one to go back to */
			log_notice("%s: radio still off from an earlier run; CFUN=%d is restored at the end", cfg->dev, kept);
			a->cfun_prev = kept;
		} else if (prev != 4) {
			/* the "before" first, on disk: without it a run killed
			 * after the switch-off would leave the radio off for good */
			char tmp[320];
			FILE *m = NULL;
			int ok = 0;

			int mfd;

			snprintf(tmp, sizeof(tmp), "%s.tmp", a->mark);
			/* not through a link someone left in /tmp */
			unlink(tmp);
			mfd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
			if (mfd >= 0 && !(m = fdopen(mfd, "w")))
				close(mfd);
			if (mfd >= 0 && m) {
				ok = fprintf(m, "%d\n", prev) > 0 && fflush(m) == 0 && fsync(fileno(m)) == 0;
				ok = (fclose(m) == 0) && ok && rename(tmp, a->mark) == 0;
				if (!ok)
					unlink(tmp);
			}
			if (!ok) {
				log_err("%s: cannot keep the radio's mode in %s (%s); not switching it off",
					cfg->dev, a->mark, strerror(errno));
				close(a->fd);
				free(a);
				return NULL;
			}
			int rc = at_cmd(a, "AT+CFUN=4", NULL, NULL, 0, 15000, NULL, 0);

			/* confirmed, like a QMI park: the mode read back */
			if (rc == RSIM_OK) {
				char c2[40];
				int now = -1;

				if (at_cmd(a, "AT+CFUN?", "+CFUN:", c2, sizeof(c2), 5000, NULL, 0) != RSIM_OK ||
				    sscanf(c2, "+CFUN: %d", &now) != 1 || now != 4) {
					log_err("%s: AT+CFUN=4 answered OK, but the radio reads back as %d — not lending", cfg->dev, now);
					/* whatever it did, the way back is to what it was */
					a->cfun_prev = prev;
					at_close(&a->be);
					return NULL;
				}
			}

			if (rc == RSIM_OK) {
				a->cfun_prev = prev;
				log_notice("%s: radio off (CFUN=4) while its card is used elsewhere", cfg->dev);
			} else {
				/* two modems must not register with one card. The kept
				 * mode goes only when the modem REFUSED: after a timeout
				 * it may still switch off late, and the next run must
				 * know what to switch back to. */
				if (rc == AT_REFUSED)
					unlink(a->mark);
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
