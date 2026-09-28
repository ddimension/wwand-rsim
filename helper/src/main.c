/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
/* ppoll (glibc and musl declare it under _GNU_SOURCE) */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atmodem.h"
#include "atr.h"
#ifdef WITH_BLUETOOTH
#include "bt.h"
#endif
#include "json.h"
#include "log.h"
#include "phoenix.h"
#ifdef WITH_RSPRO
#include "remsim.h"
#endif
#include "scan.h"
#include "serve.h"
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

/* SIGTERM, SIGHUP (an SSH link that dropped), SIGINT: leave serve() and
 * clean up like at the end of stdin — the card is powered down, a modem's
 * radio switched back on. Killed instead, the AT backend would leave a modem
 * with its radio off for good. */
static volatile sig_atomic_t stop_sig;
/* the mask serve() waits with: the stop signals are blocked everywhere else
 * and let through only inside ppoll, so one arriving between the check of
 * stop_sig and the wait cannot be lost (it is delivered as the wait starts) */
static sigset_t wait_mask;

static void on_stop(int sig)
{
	stop_sig = sig;
}

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
	/* INS and sizes only, never the bytes: a debug log is shared for
	 * support, and commands and answers carry the PIN (VERIFY), the
	 * authentication vectors and the card's files */
	log_dbg("tpdu > INS %02X, %d bytes", tpdu[1], n);
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

/* what is known about the reader and the card (the backend's info hook) */
static void write_info(struct state *st, struct jw *w)
{
	if (st->be->ops->info)
		st->be->ops->info(st->be, w);
}

/* Once, right after the open: the reader as the system and the backend see
 * it (sysfs, USB, the phone, the modem), so the plugin can show what it is
 * using before the first request. An event: a plugin that does not know it
 * passes it over. */
static void emit_info(struct state *st)
{
	struct jw w;

	jw_begin(&w, stdout);
	jw_str(&w, "event", "info");
	jw_str(&w, "backend", st->be->ops->name);
	jw_str(&w, "reader", st->be->reader);
	write_info(st, &w);
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
	write_info(st, &w);
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

		if (stop_sig)
			return 0;
		{
			struct timespec ts = { EVENT_POLL_MS / 1000, (EVENT_POLL_MS % 1000) * 1000000L };

			r = ppoll(&pfd, 1, (st->last_present >= 0 || st->be->ops->tick) ? &ts : NULL, &wait_mask);
		}
		if (r < 0) {
			if (errno == EINTR)
				continue;	/* the loop head sees a stop signal */
			log_err("poll: %s", strerror(errno));
			return 1;
		}
		if (r == 0) {
			if (st->be->ops->tick)
				st->be->ops->tick(st->be);
			if (st->last_present >= 0)
				poll_presence(st);
			if (st->be->ended)
				return 1;
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
		if (st->be->ended)
			return 1;
	}
}

static void usage(FILE *f)
{
	fputs("usage: rsim-card [-v] [-s] [options] phoenix:<tty> | wbsm:[serial] | pcsc:<reader substring or index>\n"
	      "                                    | at:<tty of a modem's AT port> | bt:<phone's address>\n"
	      "                                    | rspro:<server>[:port][/<bank>:<slot>]\n"
	      "       rsim-card --list               what this machine offers, JSON lines\n"
	      "       rsim-card --list --rspro-server <server>[:port]\n"
	      "                                      the bank slots an osmo-remsim server knows\n"
	      "       rsim-card --serve [SPEC...]    the command= of an authorized_keys line: runs only\n"
	      "                                      wwand-rsim's own calls, for the readers SPEC matches\n"
	      "  -v, --verbose          debug logging\n"
	      "  -s, --syslog           log to syslog as well as stderr\n"
	      "phoenix options:\n"
	      "  --clock KHZ            card clock of the reader (default 3579)\n"
	      "  --reset MODE           auto|rts|rts_inv|dtr|dtr_inv (default auto)\n"
	      "  --detect LINE          none|cts|dsr|cd card-detect line (default none)\n"
	      "  --atr-timeout-ms N     wait for the first ATR byte (default 1000)\n"
	      "wbsm (WB Electronics Smartmouse USB, clock and mode set by software):\n"
	      "  --clock KHZ            3580 (default), 3680 or 6000\n"
	      "  --wbsm-mode MODE       phoenix (default) | smartmouse\n"
	      "at (the SIM of another modem, over AT+CSIM; ATR is the minimal 3B00):\n"
	      "  --at-baud N            115200 (default); USB ports ignore it\n"
	      "  --at-radio off|keep    off (default): deregistered (AT+COPS=2) and AT+CFUN=4\n"
	      "                         while its card is used elsewhere, both restored at the end\n"
	      "bt (a paired phone's SIM over the Bluetooth SIM Access Profile):\n"
	      "  --bt-channel N         its RFCOMM channel (default: looked up over SDP)\n"
	      "  --bt-security LEVEL    medium (default, encrypted) | high (MITM-protected key)\n"
	      "  --bt-apdu FORMAT       gsm (default, CommandAPDU) | 7816 (CommandAPDU7816)\n"
	      "rspro (a card in an osmo-remsim SIM bank; this is a remsim client):\n"
	      "  --rspro-client ID[:SLOT]  our client id and slot at the server (default 0:0)\n"
	      "  --rspro-rest-port N    the server's REST API (default 9997), for /<bank>:<slot>\n"
	      "                         (mapped while in use) and --list\n", f);
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
		{ "at-baud", required_argument, NULL, 'B' },
		{ "at-radio", required_argument, NULL, 'R' },
		{ "bt-channel", required_argument, NULL, 'C' },
		{ "bt-security", required_argument, NULL, 'S' },
		{ "bt-apdu", required_argument, NULL, 'P' },
		{ "rspro-client", required_argument, NULL, 'I' },
		{ "rspro-rest-port", required_argument, NULL, 'T' },
		{ "rspro-server", required_argument, NULL, 'E' },
		{ "list", no_argument, NULL, 'L' },
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
	struct at_cfg atc = { .baud = 115200, .radio_keep = false };
	int bt_channel = 0, bt_high = 0, bt_7816 = 0;
	unsigned long rspro_id = 0, rspro_slot = 0, rspro_rest = 0;
	const char *rspro_server = NULL;
	int list = 0;
	int verbose = 0, use_syslog = 0, opt, v, ret;
	char *end;

	/* before getopt: what follows are reader patterns, not options */
	if (argc >= 2 && !strcmp(argv[1], "--serve"))
		return serve_run(argc - 2, argv + 2);

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
		case 'B':
			atc.baud = (unsigned)strtoul(optarg, &end, 10);
			if (*end || atmodem_speed(atc.baud) < 0) {
				fprintf(stderr, "rsim-card: --at-baud %s: expected a baud rate\n", optarg);
				return 2;
			}
			break;
		case 'R':
			if (!strcmp(optarg, "off") || !strcmp(optarg, "keep")) {
				atc.radio_keep = !strcmp(optarg, "keep");
				break;
			}
			fprintf(stderr, "rsim-card: --at-radio %s: off or keep\n", optarg);
			return 2;
		case 'C':
			bt_channel = (int)strtol(optarg, &end, 10);
			if (*end || bt_channel < 1 || bt_channel > 30) {
				fprintf(stderr, "rsim-card: --bt-channel %s: expected 1..30\n", optarg);
				return 2;
			}
			break;
		case 'S':
			if (!strcmp(optarg, "medium") || !strcmp(optarg, "high")) {
				bt_high = !strcmp(optarg, "high");
				break;
			}
			fprintf(stderr, "rsim-card: --bt-security %s: medium or high\n", optarg);
			return 2;
		case 'P':
			if (!strcmp(optarg, "gsm") || !strcmp(optarg, "7816")) {
				bt_7816 = !strcmp(optarg, "7816");
				break;
			}
			fprintf(stderr, "rsim-card: --bt-apdu %s: gsm or 7816\n", optarg);
			return 2;
		case 'I':
			rspro_id = strtoul(optarg, &end, 10);
			if (end != optarg && *end == ':')
				rspro_slot = strtoul(end + 1, &end, 10);
			if (end == optarg || *end || end[-1] == ':' || rspro_id > 65535 || rspro_slot > 65535) {
				fprintf(stderr, "rsim-card: --rspro-client %s: expected ID[:SLOT], 0..65535\n", optarg);
				return 2;
			}
			break;
		case 'T':
			rspro_rest = strtoul(optarg, &end, 10);
			if (*end || !rspro_rest || rspro_rest > 65535) {
				fprintf(stderr, "rsim-card: --rspro-rest-port %s: expected 1..65535\n", optarg);
				return 2;
			}
			break;
		case 'E':
			rspro_server = optarg;
			break;
		case 'L':
			list = 1;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (list) {
		if (optind != argc) {
			usage(stderr);
			return 2;
		}
		if (rspro_server) {
#ifdef WITH_RSPRO
			log_init(verbose, use_syslog);
			return rspro_list(rspro_server, (uint16_t)rspro_rest);
#else
			fprintf(stderr, "rsim-card: built without RSPRO (WITH_RSPRO=OFF)\n");
			return 1;
#endif
		}
		/* RSIM_TEST_SYSROOT: a fake /sys for the tests */
		return scan_run(getenv("RSIM_TEST_SYSROOT"));
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
	{
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sigset_t stop_set;

		sa.sa_handler = on_stop;	/* no SA_RESTART: ppoll returns EINTR */
		sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGHUP, &sa, NULL);
		sigaction(SIGINT, &sa, NULL);
		sigemptyset(&stop_set);
		sigaddset(&stop_set, SIGTERM);
		sigaddset(&stop_set, SIGHUP);
		sigaddset(&stop_set, SIGINT);
		sigprocmask(SIG_BLOCK, &stop_set, &wait_mask);
		sigdelset(&wait_mask, SIGTERM);
		sigdelset(&wait_mask, SIGHUP);
		sigdelset(&wait_mask, SIGINT);
	}

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
		/* In Phoenix mode this reader resets on inverted RTS
		 * (HW-observed on 104f:0002, 2026-09-26, twice). Known, so not
		 * probed: auto would try plain RTS first, which releases a card
		 * held in reset. Smartmouse mode is the other wiring, unmeasured,
		 * so it stays auto. */
		if (cfg.reset == PHX_RESET_AUTO && !wbsm_smartmouse)
			cfg.reset = PHX_RESET_RTS_INV;
		st.be = phoenix_open(&cfg, io);
#else
		(void)wbsm_smartmouse;
		log_err("built without libusb: the Smartmouse USB cannot be driven");
		return 1;
#endif
	} else if (!strncmp(spec, "at:", 3) && spec[3]) {
		atc.dev = spec + 3;
		st.be = atmodem_open(&atc);
	} else if (!strncmp(spec, "bt:", 3) && spec[3]) {
#ifdef WITH_BLUETOOTH
		struct bt_cfg b = {
			.addr = spec + 3,
			.channel = bt_channel,
			.secure_high = bt_high,
			.apdu7816 = bt_7816,
		};

		st.be = bt_open(&b);
#else
		(void)bt_channel;
		(void)bt_high;
		(void)bt_7816;
		log_err("built without Bluetooth support (WITH_BLUETOOTH=OFF)");
		return 1;
#endif
	} else if (!strncmp(spec, "rspro:", 6) && spec[6]) {
#ifdef WITH_RSPRO
		struct rspro_cfg rc = {
			.spec = spec + 6,
			.client_id = (uint16_t)rspro_id,
			.client_slot = (uint16_t)rspro_slot,
			.rest_port = (uint16_t)rspro_rest,
		};

		st.be = rspro_open(&rc);
#else
		(void)rspro_id;
		(void)rspro_slot;
		(void)rspro_rest;
		log_err("built without RSPRO support (WITH_RSPRO=OFF)");
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

	emit_info(&st);
	ret = serve(&st);
	if (st.be->ended)
		log_notice("%s: the reader is gone, ending", spec);
	if (st.powered)
		st.be->ops->power_down(st.be);
	st.be->ops->close(st.be);
	return ret;
}
