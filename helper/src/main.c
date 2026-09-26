/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atr.h"
#include "json.h"
#include "log.h"
#include "phoenix.h"
#ifdef WITH_LIBUSB
#include "wbsm.h"
#endif
#ifdef WITH_PCSC
#include "pcsc.h"
#endif

/*
 * rsim-card: owns one smart-card reader and answers one JSON request line
 * with one JSON answer line (docs/plan.md §2). Events are written only
 * between requests, never inside an answer, so the plugin can read "the
 * next line that has ok" without a parser for interleaving.
 */

/* the longest valid request is a tpdu with 261 bytes = 522 hex digits plus
 * the keys; anything past this is not a request we could honour */
#define LINE_MAX_LEN	2048
#define EVENT_POLL_MS	500

struct state {
	struct rsim_backend *be;
	bool powered;
	bool had_atr;		/* the last power_up/reset produced an ATR */
	int last_present;	/* -1: backend cannot tell */
	uint8_t atr[ATR_MAX];
	size_t atr_len;
};

static void reply_err(int err, const char *detail)
{
	struct jw w;

	jw_begin(&w, stdout);
	jw_bool(&w, "ok", false);
	jw_str(&w, "error", rsim_err_name(err));
	if (detail && *detail)
		jw_str(&w, "detail", detail);
	jw_end(&w);
}

static void reply_atr(const struct state *st)
{
	struct jw w;

	jw_begin(&w, stdout);
	jw_bool(&w, "ok", true);
	jw_hex(&w, "atr", st->atr, st->atr_len);
	jw_end(&w);
}

static void emit_event(const char *ev)
{
	struct jw w;

	jw_begin(&w, stdout);
	jw_str(&w, "event", ev);
	jw_end(&w);
}

static void do_power(struct state *st, bool warm)
{
	struct rsim_backend *be = st->be;
	int r;

	be->detail[0] = '\0';
	/* a warm reset of a card that is not powered is a cold start: the
	 * modem asks for a reset after its own power cycles too, and the
	 * answer it needs either way is a fresh ATR */
	if (warm && st->powered)
		r = be->ops->reset(be, st->atr, &st->atr_len);
	else
		r = be->ops->power_up(be, st->atr, &st->atr_len);
	st->powered = r == RSIM_OK;
	st->had_atr = r == RSIM_OK;
	if (r) {
		st->atr_len = 0;
		log_notice("%s failed: %s%s%s", warm ? "reset" : "power_up",
			   rsim_err_name(r), *be->detail ? ": " : "", be->detail);
		reply_err(r, be->detail);
		return;
	}
	reply_atr(st);
}

static void do_power_down(struct state *st)
{
	struct rsim_backend *be = st->be;
	int r = RSIM_OK;
	struct jw w;

	be->detail[0] = '\0';
	if (st->powered)
		r = be->ops->power_down(be);
	st->powered = false;
	if (r) {
		reply_err(r, be->detail);
		return;
	}
	jw_begin(&w, stdout);
	jw_bool(&w, "ok", true);
	jw_end(&w);
}

static void do_tpdu(struct state *st, const char *line)
{
	struct rsim_backend *be = st->be;
	char hex[2 * RSIM_TPDU_MAX + 2];
	uint8_t tpdu[RSIM_TPDU_MAX], resp[RSIM_RESP_MAX];
	size_t resp_len = 0;
	int n, r;
	struct jw w;

	if (json_get_string(line, "data", hex, sizeof(hex)) != 1) {
		reply_err(RSIM_E_BAD_REQUEST, "data missing or not a string");
		return;
	}
	n = hex_decode(hex, tpdu, sizeof(tpdu));
	if (n < 4) {
		reply_err(RSIM_E_BAD_REQUEST, "data is not a 4..261 byte hex TPDU");
		return;
	}
	if (!st->powered) {
		reply_err(RSIM_E_NOT_POWERED, NULL);
		return;
	}
	be->detail[0] = '\0';
	r = be->ops->transmit(be, tpdu, (size_t)n, resp, &resp_len);
	if (r) {
		log_dbg("tpdu %02X: %s %s", tpdu[1], rsim_err_name(r), be->detail);
		/* a card that is gone is no longer powered from our side */
		if (r == RSIM_E_NO_CARD)
			st->powered = false;
		reply_err(r, be->detail);
		return;
	}
	log_dbg("tpdu INS %02X: SW %02X%02X, %zu data bytes", tpdu[1],
		resp[resp_len - 2], resp[resp_len - 1], resp_len - 2);
	jw_begin(&w, stdout);
	jw_bool(&w, "ok", true);
	jw_hex(&w, "data", resp, resp_len);
	jw_end(&w);
}

static void do_status(struct state *st)
{
	int present = st->be->ops->present(st->be);
	struct jw w;

	jw_begin(&w, stdout);
	jw_bool(&w, "ok", true);
	/* without a detect line, the last answer to reset is the only
	 * evidence of a card there is */
	jw_bool(&w, "present", present >= 0 ? present : st->had_atr);
	jw_bool(&w, "powered", st->powered);
	jw_str(&w, "backend", st->be->ops->name);
	jw_str(&w, "reader", st->be->reader);
	if (st->powered)
		jw_hex(&w, "atr", st->atr, st->atr_len);
	else
		jw_null(&w, "atr");
	jw_end(&w);
}

static void handle_line(struct state *st, const char *line)
{
	char op[32];

	if (!json_is_object(line) || json_get_string(line, "op", op, sizeof(op)) != 1) {
		reply_err(RSIM_E_BAD_REQUEST, "not a request object with an op");
		return;
	}
	if (!strcmp(op, "power_up"))
		do_power(st, false);
	else if (!strcmp(op, "reset"))
		do_power(st, true);
	else if (!strcmp(op, "power_down"))
		do_power_down(st);
	else if (!strcmp(op, "tpdu"))
		do_tpdu(st, line);
	else if (!strcmp(op, "status"))
		do_status(st);
	else
		reply_err(RSIM_E_BAD_REQUEST, "unknown op");
}

static void poll_presence(struct state *st)
{
	int now = st->be->ops->present(st->be);

	if (now < 0 || now == st->last_present)
		return;
	st->last_present = now;
	if (!now && st->powered) {
		/* releases the reader's hold on a card that is gone */
		st->be->ops->power_down(st->be);
		st->powered = false;
	}
	if (!now)
		st->had_atr = false;
	log_notice("card %s", now ? "inserted" : "removed");
	emit_event(now ? "inserted" : "removed");
}

static int serve(struct state *st)
{
	static char buf[LINE_MAX_LEN + 1];
	size_t len = 0;
	bool discarding = false;

	st->last_present = st->be->ops->present(st->be);
	for (;;) {
		struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
		ssize_t n;
		char *nl;
		int r;

		r = poll(&pfd, 1, st->last_present >= 0 ? EVENT_POLL_MS : -1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			log_err("poll: %s", strerror(errno));
			return 1;
		}
		if (r == 0) {
			poll_presence(st);
			continue;
		}
		n = read(STDIN_FILENO, buf + len, LINE_MAX_LEN - len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return 0;	/* the plugin closed our stdin: done */
		len += (size_t)n;
		buf[len] = '\0';
		while ((nl = memchr(buf, '\n', len))) {
			size_t used = (size_t)(nl - buf) + 1;

			*nl = '\0';
			if (discarding)
				discarding = false;
			else if (strspn(buf, " \t\r") != strlen(buf))
				handle_line(st, buf);
			memmove(buf, buf + used, len - used);
			len -= used;
			buf[len] = '\0';
		}
		if (len == LINE_MAX_LEN) {
			/* one answer for the whole overlong line, then skip
			 * to its end */
			if (!discarding)
				reply_err(RSIM_E_BAD_REQUEST, "request line too long");
			discarding = true;
			len = 0;
		}
		/* insert/remove may also happen while requests keep coming */
		if (st->last_present >= 0)
			poll_presence(st);
	}
}

static void usage(FILE *f)
{
	fputs("usage: rsim-card [-v] [-s] [options] phoenix:<tty> | wbsm:[serial] | pcsc:<reader substring or index>\n"
	      "  -v, --verbose          debug logging\n"
	      "  -s, --syslog           log to syslog as well as stderr\n"
	      "phoenix options:\n"
	      "  --clock KHZ            card clock of the reader (default 3579)\n"
	      "  --reset MODE           auto|rts|rts_inv|dtr|dtr_inv (default auto)\n"
	      "  --detect LINE          none|cts|dsr|cd card-detect line (default none)\n"
	      "  --atr-timeout-ms N     wait for the first ATR byte (default 1000)\n"
	      "wbsm (WB Electronics Smartmouse USB, clock and mode set by software):\n"
	      "  --clock KHZ            3580 (default), 3680 or 6000\n"
	      "  --wbsm-mode MODE       phoenix (default) | smartmouse\n", f);
}

static int parse_enum(const char *arg, const char *const *names, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp(arg, names[i]))
			return i;
	return -1;
}

int main(int argc, char **argv)
{
	static const char *const reset_names[] = { "auto", "rts", "rts_inv", "dtr", "dtr_inv" };
	static const char *const detect_names[] = { "none", "cts", "dsr", "cd" };
	static const struct option longopts[] = {
		{ "verbose", no_argument, NULL, 'v' },
		{ "syslog", no_argument, NULL, 's' },
		{ "clock", required_argument, NULL, 'c' },
		{ "reset", required_argument, NULL, 'r' },
		{ "detect", required_argument, NULL, 'd' },
		{ "atr-timeout-ms", required_argument, NULL, 'a' },
		{ "wbsm-mode", required_argument, NULL, 'w' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct phoenix_cfg cfg = {
		.clock_khz = 3579,
		.reset = PHX_RESET_AUTO,
		.detect = PHX_DETECT_NONE,
		.atr_timeout_ms = 1000,
	};
	struct state st;
	const char *spec;
	int wbsm_smartmouse = 0;
	int verbose = 0, use_syslog = 0, opt, v, ret;
	char *end;

	while ((opt = getopt_long(argc, argv, "vsh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'v':
			verbose = 1;
			break;
		case 's':
			use_syslog = 1;
			break;
		case 'c':
			cfg.clock_khz = (unsigned)strtoul(optarg, &end, 10);
			if (*end || cfg.clock_khz < 1000 || cfg.clock_khz > 20000) {
				fprintf(stderr, "rsim-card: --clock %s: expected kHz, 1000..20000\n", optarg);
				return 2;
			}
			break;
		case 'r':
			v = parse_enum(optarg, reset_names, 5);
			if (v < 0) {
				fprintf(stderr, "rsim-card: --reset %s: unknown mode\n", optarg);
				return 2;
			}
			cfg.reset = (enum phoenix_reset)v;
			break;
		case 'd':
			v = parse_enum(optarg, detect_names, 4);
			if (v < 0) {
				fprintf(stderr, "rsim-card: --detect %s: unknown line\n", optarg);
				return 2;
			}
			cfg.detect = (enum phoenix_detect)v;
			break;
		case 'a':
			cfg.atr_timeout_ms = (unsigned)strtoul(optarg, &end, 10);
			if (*end || !cfg.atr_timeout_ms || cfg.atr_timeout_ms > 7000) {
				fprintf(stderr, "rsim-card: --atr-timeout-ms %s: expected 1..7000\n", optarg);
				return 2;
			}
			break;
		case 'w':
			if (!strcmp(optarg, "phoenix") || !strcmp(optarg, "smartmouse")) {
				wbsm_smartmouse = !strcmp(optarg, "smartmouse");
				break;
			}
			fprintf(stderr, "rsim-card: --wbsm-mode %s: phoenix or smartmouse\n", optarg);
			return 2;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (optind != argc - 1) {
		usage(stderr);
		return 2;
	}
	spec = argv[optind];
	log_init(verbose, use_syslog);
	/* a plugin that went away mid-answer must not kill us before the
	 * card is powered down; the write error is enough */
	signal(SIGPIPE, SIG_IGN);

	memset(&st, 0, sizeof(st));
	if (!strncmp(spec, "phoenix:", 8) && spec[8]) {
		cfg.dev = spec + 8;
		st.be = phoenix_open(&cfg, NULL);
	} else if (!strncmp(spec, "wbsm:", 5)) {
#ifdef WITH_LIBUSB
		/* the reader's clock is set to what --clock says, so the baud
		 * rate phoenix.c derives from it is right by construction */
		struct wbsm_cfg w = {
			.serial = spec[5] ? spec + 5 : NULL,
			.clock_khz = cfg.clock_khz == 3579 ? 3580 : cfg.clock_khz,
			.smartmouse = wbsm_smartmouse,
		};
		struct phx_io *io = wbsm_open(&w, spec);

		if (!io)
			return 1;
		cfg.dev = spec;
		cfg.clock_khz = w.clock_khz;
		st.be = phoenix_open(&cfg, io);
#else
		(void)wbsm_smartmouse;
		log_err("built without libusb: the Smartmouse USB cannot be driven");
		return 1;
#endif
	} else if (!strncmp(spec, "pcsc:", 5)) {
#ifdef WITH_PCSC
		st.be = pcsc_open(spec + 5);
#else
		log_err("built without PC/SC support");
		return 1;
#endif
	} else {
		usage(stderr);
		return 2;
	}
	if (!st.be)
		return 1;

	ret = serve(&st);
	if (st.powered)
		st.be->ops->power_down(st.be);
	st.be->ops->close(st.be);
	return ret;
}
