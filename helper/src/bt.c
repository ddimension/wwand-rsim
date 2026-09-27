/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * The SIM of a paired phone, over the Bluetooth SIM Access Profile (SAP
 * v1.1) — the phone is the SAP server and hands its card over, this is the
 * client. While the link lasts the phone's own radio has no SIM: one card,
 * one registration, as with the at: backend's radio switch-off.
 *
 * Only kernel sockets: L2CAP for the SDP lookup of the SAP channel, RFCOMM
 * for SAP itself. Pairing and the link keys are BlueZ's business (the phone
 * is paired with bluetoothctl beforehand, bluetoothd hands the keys to the
 * kernel), so nothing here needs libbluetooth or D-Bus and the backend costs
 * a few kilobytes. The socket constants are the kernel's ABI, spelled out
 * below instead of taken from the <bluetooth/...> headers, which only BlueZ's -dev
 * package installs.
 *
 * SAP moves APDUs: the TPDU the modem sends is passed on as CommandAPDU (or
 * CommandAPDU7816), the answer comes back as it is — 61xx/6Cxx included,
 * like every other backend. A link that drops ends the helper (like an SSH
 * link that drops): the plugin gives the modem its own SIM back and tries
 * again later.
 */
#include <ctype.h>
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "bt.h"
#include "json.h"
#include "log.h"
#include "sap.h"

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_L2CAP 0
#define BTPROTO_RFCOMM 3
#define SOL_BLUETOOTH 274
#define BT_SECURITY 4
#define BT_SECURITY_MEDIUM 2
#define BT_SECURITY_HIGH 3
#define SDP_PSM 1

struct bt_security {
	uint8_t level;
	uint8_t key_size;
};

/* struct sockaddr_rc and sockaddr_l2 of <bluetooth/rfcomm.h>, <l2cap.h> */
struct sa_rc {
	sa_family_t family;
	uint8_t bdaddr[6];
	uint8_t channel;
};

struct sa_l2 {
	sa_family_t family;
	uint16_t psm;		/* little endian */
	uint8_t bdaddr[6];
	uint16_t cid;
	uint8_t bdaddr_type;	/* 0: BR/EDR */
};

#define SDP_TIMEOUT_MS		10000
#define RFCOMM_TIMEOUT_MS	15000
/* the phone may ask its user whether to allow SIM access; all of the
 * bring-up together stays below the plugin's 60 s for the first power-up */
#define CONNECT_TIMEOUT_MS	25000
#define STATUS_TIMEOUT_MS	5000
/* ...and that bound is kept as a whole, not per step: SDP, RFCOMM, a connect
 * the phone's user has to allow and the size retries each have their own
 * timeout, and added up they went well past it — the plugin then started a
 * second helper while the first still waited at the phone's prompt */
#define BRINGUP_MS		55000
/* an eUICC works through a profile download in single commands that take
 * seconds each; below the plugin's 30 s for a TPDU */
#define REQUEST_TIMEOUT_MS	25000

/* RSIM_TEST_SAP_TIMEOUT_MS shortens it for the tests */
static int request_timeout_ms = REQUEST_TIMEOUT_MS;

struct bt_backend {
	struct rsim_backend be;
	int fd;
	char addr[18];
	uint8_t apdu_param;
	uint16_t max_msg;
	int channel;		/* the RFCOMM channel it runs on */
	bool secure_high;
	int card;		/* the phone's last word: 1 usable, 0 not */
	bool sim_on;		/* powered on at the phone */
	bool served;		/* an ATR went out since the target last saw no card */
	bool bounce;		/* reset by the phone behind the target's back */
	bool gone;		/* the link ended */
	long bringup_until;	/* the open as a whole ends by then (BRINGUP_MS) */
	uint8_t rbuf[4096];
	size_t rlen;
	uint8_t msg[4096];	/* the message being looked at, sap_msg points here */
};

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int bt_parse_addr(const char *s, unsigned char out[6])
{
	int i;

	if (strlen(s) != 17)
		return -1;
	for (i = 0; i < 6; i++) {
		const char *h = s + 3 * i;

		if (!isxdigit((unsigned char)h[0]) || !isxdigit((unsigned char)h[1]) || (i < 5 && h[2] != ':'))
			return -1;
		out[5 - i] = (unsigned char)strtoul((char[]){ h[0], h[1], '\0' }, NULL, 16);
	}
	return 0;
}

/* connect a non-blocking socket within timeout_ms; 0, or -1 with errno */
static int connect_wait(int fd, const void *sa, socklen_t len, int timeout_ms)
{
	struct pollfd p = { .fd = fd, .events = POLLOUT };
	int err = 0, r;
	socklen_t el = sizeof(err);

	if (connect(fd, sa, len) == 0)
		return 0;
	if (errno != EINPROGRESS && errno != EAGAIN)
		return -1;
	while ((r = poll(&p, 1, timeout_ms)) < 0 && errno == EINTR)
		;
	if (r == 0) {
		errno = ETIMEDOUT;
		return -1;
	}
	if (r < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0)
		return -1;
	if (err) {
		errno = err;
		return -1;
	}
	return 0;
}

/* The phone's SAP channel, from its SDP record; -1 when it has none, -2
 * when it could not be asked. */
static int sdp_channel(const uint8_t bdaddr[6], const char *addr)
{
	struct sa_l2 sa;
	uint8_t req[64], rsp[1024], attrs[2048];
	size_t alen = 0, cont_len = 0;
	uint8_t cont[16];
	long deadline = now_ms() + SDP_TIMEOUT_MS;
	uint16_t tid = 1;
	int fd, ch = -1;

	fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, BTPROTO_L2CAP);
	if (fd < 0) {
		log_err("bt: no Bluetooth sockets here (%s) — kernel without Bluetooth, or no adapter", strerror(errno));
		return -2;
	}
	memset(&sa, 0, sizeof(sa));
	sa.family = AF_BLUETOOTH;
	sa.psm = htole16(SDP_PSM);
	memcpy(sa.bdaddr, bdaddr, 6);
	if (connect_wait(fd, &sa, sizeof(sa), SDP_TIMEOUT_MS) < 0) {
		log_err("bt: %s: cannot reach the phone (%s) — in range, Bluetooth on?", addr, strerror(errno));
		close(fd);
		return -2;
	}
	/* a long answer comes in parts, each with the state to ask for the
	 * next; the attribute bytes are one data element sequence over all */
	for (;;) {
		const uint8_t *a, *c;
		size_t al, cl;
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int n = sdp_search_req(req, sizeof(req), tid, SAP_UUID16, cont, cont_len);
		ssize_t r;
		long left = deadline - now_ms();

		if (n < 0 || send(fd, req, (size_t)n, 0) != n || left <= 0 || poll(&p, 1, (int)left) <= 0 ||
		    (r = recv(fd, rsp, sizeof(rsp), 0)) <= 0) {
			log_err("bt: %s: no answer to the SDP query", addr);
			ch = -2;
			break;
		}
		if (sdp_search_rsp(rsp, (size_t)r, tid, &a, &al, &c, &cl) < 0 || alen + al > sizeof(attrs)) {
			log_err("bt: %s: an SDP answer that is not one", addr);
			ch = -2;
			break;
		}
		memcpy(attrs + alen, a, al);
		alen += al;
		if (!cl) {
			ch = sdp_rfcomm_channel(attrs, alen);
			break;
		}
		memcpy(cont, c, cl);
		cont_len = cl;
		tid++;
	}
	close(fd);
	return ch;
}

static int open_rfcomm(const uint8_t bdaddr[6], int channel, bool high, const char *addr)
{
	struct sa_rc sa;
	struct bt_security sec = { .level = high ? BT_SECURITY_HIGH : BT_SECURITY_MEDIUM };
	int fd = socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, BTPROTO_RFCOMM);

	if (fd < 0) {
		log_err("bt: no RFCOMM sockets here (%s) — kernel without rfcomm?", strerror(errno));
		return -1;
	}
	/* SAP carries the SIM's secrets: an encrypted link with a stored key,
	 * never the kernel's default (no encryption) */
	if (setsockopt(fd, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec)) < 0) {
		log_err("bt: cannot require an encrypted link (%s); not connecting", strerror(errno));
		close(fd);
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.family = AF_BLUETOOTH;
	memcpy(sa.bdaddr, bdaddr, 6);
	sa.channel = (uint8_t)channel;
	if (connect_wait(fd, &sa, sizeof(sa), RFCOMM_TIMEOUT_MS) < 0) {
		log_err("bt: %s channel %d: %s%s", addr, channel, strerror(errno),
			(errno == EACCES || errno == ECONNREFUSED || errno == EHOSTDOWN)
				? " — paired? SIM access allowed for this device on the phone?" : "");
		close(fd);
		return -1;
	}
	return fd;
}

/* RSIM_TEST_SAP_SOCK: a unix socket with a simulated phone behind it, for
 * the tests (tests/test_e2e_sap.py); no SDP, no Bluetooth */
static int open_test(const char *path)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		log_err("bt: test socket %s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	return fd;
}

/* ---- the SAP session ------------------------------------------------- */

static int send_msg(struct bt_backend *b, uint8_t id, const struct sap_param *p, int n)
{
	uint8_t out[SAP_MSG_WANT + 16];
	int len = sap_encode(out, sizeof(out), id, p, n), o = 0;
	long deadline = now_ms() + REQUEST_TIMEOUT_MS;

	if (len < 0 || (b->max_msg && len > b->max_msg)) {
		snprintf(b->be.detail, RSIM_DETAIL_MAX, "a %s larger than the phone takes", sap_msg_name(id));
		return RSIM_E_BAD_REQUEST;
	}
	log_dbg("sap> %s", sap_msg_name(id));
	while (o < len) {
		ssize_t w = write(b->fd, out + o, (size_t)(len - o));
		struct pollfd p = { .fd = b->fd, .events = POLLOUT };

		if (w > 0) {
			o += (int)w;
			continue;
		}
		if (w < 0 && errno != EAGAIN && errno != EINTR)
			break;
		if (now_ms() >= deadline || poll(&p, 1, (int)(deadline - now_ms())) < 0)
			break;
	}
	if (o < len) {
		b->gone = true;
		snprintf(b->be.detail, RSIM_DETAIL_MAX, "the link to the phone: %s", strerror(errno));
		return RSIM_E_IO;
	}
	return RSIM_OK;
}

/* The next message, within deadline (0: only what is already there).
 * RSIM_OK with m filled, 1 when nothing came, an error when the link failed
 * (then b->gone). */
static int read_msg(struct bt_backend *b, long deadline, struct sap_msg *m)
{
	for (;;) {
		int f = sap_frame(b->rbuf, b->rlen);

		if (f < 0 || (!f && b->rlen == sizeof(b->rbuf))) {
			log_err("bt: %s: the phone sent something that is not SAP", b->addr);
			b->gone = true;
			return RSIM_E_PROTOCOL;
		}
		if (f > 0) {
			memcpy(b->msg, b->rbuf, (size_t)f);
			b->rlen -= (size_t)f;
			memmove(b->rbuf, b->rbuf + f, b->rlen);
			sap_decode(b->msg, (size_t)f, m);
			log_dbg("sap< %s", sap_msg_name(m->id));
			return RSIM_OK;
		}

		struct pollfd p = { .fd = b->fd, .events = POLLIN };
		long left = deadline ? deadline - now_ms() : 0;
		ssize_t r;
		int pr;

		if (left < 0)
			left = 0;
		pr = poll(&p, 1, (int)left);
		if (pr < 0 && errno == EINTR)
			continue;
		if (pr == 0)
			return 1;
		r = read(b->fd, b->rbuf + b->rlen, sizeof(b->rbuf) - b->rlen);
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (r <= 0) {
			log_notice("bt: %s: the link to the phone ended%s%s", b->addr, r ? ": " : "",
				   r ? strerror(errno) : "");
			b->gone = true;
			return RSIM_E_IO;
		}
		b->rlen += (size_t)r;
	}
}

/* what the phone says on its own: the card's state, or that it ends */
static void unsolicited(struct bt_backend *b, const struct sap_msg *m)
{
	static const char *const st_names[] = { "unknown error", "reset", "not accessible", "removed",
						"inserted", "recovered" };
	int v;

	if (m->id == SAP_STATUS_IND) {
		v = sap_get_u8(m, SAP_P_STATUS_CHANGE);
		log_notice("bt: %s: the phone's SIM: %s", b->addr,
			   (v >= 0 && v <= SAP_ST_RECOVERED) ? st_names[v] : "?");
		switch (v) {
		case SAP_ST_RESET:
		case SAP_ST_RECOVERED:
			/* powered and fresh: an ATR the target has not seen */
			if (b->served)
				b->bounce = true;
			b->card = 1;
			b->sim_on = true;
			break;
		case SAP_ST_INSERTED:
			if (b->served)
				b->bounce = true;
			b->card = 1;
			b->sim_on = false;
			break;
		default:
			b->card = 0;
			b->sim_on = false;
			break;
		}
		return;
	}
	if (m->id == SAP_DISCONNECT_IND) {
		v = sap_get_u8(m, SAP_P_DISCONNECTION_TYPE);
		log_notice("bt: %s: the phone ends SIM access (%s)", b->addr,
			   v == SAP_DISC_IMMEDIATE ? "immediately" : "gracefully");
		/* graceful: it waits for our DISCONNECT_REQ; immediate: it has
		 * already stopped answering */
		if (v != SAP_DISC_IMMEDIATE)
			send_msg(b, SAP_DISCONNECT_REQ, NULL, 0);
		b->gone = true;
		b->card = 0;
		return;
	}
	log_dbg("bt: %s: %s not asked for, ignored", b->addr, sap_msg_name(m->id));
}

/* everything already on the link, without waiting */
static void drain(struct bt_backend *b)
{
	struct sap_msg m;

	while (!b->gone && read_msg(b, 0, &m) == RSIM_OK)
		unsolicited(b, &m);
}

/* one request, and wait for the answer `want`; unsolicited messages on the
 * way are taken in. RSIM_OK with m the answer. */
static int request(struct bt_backend *b, uint8_t id, const struct sap_param *p, int n, uint8_t want,
		   int timeout_ms, struct sap_msg *m)
{
	long deadline = now_ms() + timeout_ms;
	int r;

	if (b->gone) {
		snprintf(b->be.detail, RSIM_DETAIL_MAX, "the link to the phone has ended");
		return RSIM_E_IO;
	}
	if ((r = send_msg(b, id, p, n)))
		return r;
	for (;;) {
		r = read_msg(b, deadline, m);
		if (r == 1) {
			/* SAP answers carry no request id: a late answer would be
			 * taken for the next request's — the target would get the
			 * wrong card data. The session cannot be trusted after this;
			 * the helper ends and the plugin starts afresh. */
			snprintf(b->be.detail, RSIM_DETAIL_MAX, "no %s from the phone", sap_msg_name(want));
			log_err("bt: %s: no %s within %d ms — ending the session", b->addr, sap_msg_name(want), timeout_ms);
			b->gone = true;
			return RSIM_E_TIMEOUT;
		}
		if (r)
			return r;
		if (m->id == want)
			return RSIM_OK;
		if (m->id == SAP_ERROR_RESP) {
			snprintf(b->be.detail, RSIM_DETAIL_MAX, "the phone refused %s", sap_msg_name(id));
			return RSIM_E_PROTOCOL;
		}
		unsolicited(b, m);
		if (b->gone) {
			snprintf(b->be.detail, RSIM_DETAIL_MAX, "the phone ended SIM access");
			return RSIM_E_IO;
		}
	}
}

/* a ResultCode as an rsim error; ok_too is a code that means "fine" here
 * (already on for power-on, already off for power-off) */
static int result(struct bt_backend *b, const struct sap_msg *m, int ok_too)
{
	int rc = sap_get_u8(m, SAP_P_RESULT_CODE);

	if (rc == SAP_RC_OK || (ok_too >= 0 && rc == ok_too))
		return RSIM_OK;
	snprintf(b->be.detail, RSIM_DETAIL_MAX, "%s: %s", sap_msg_name(m->id), sap_result_name(rc));
	if (rc == SAP_RC_NOT_ACCESSIBLE || rc == SAP_RC_REMOVED) {
		b->card = 0;
		return RSIM_E_NO_CARD;
	}
	return (rc == SAP_RC_NOT_SUPPORTED) ? RSIM_E_PROTOCOL : RSIM_E_IO;
}

static int get_atr(struct bt_backend *b, uint8_t *atr, size_t *atr_len)
{
	struct sap_msg m;
	const struct sap_param *a;
	int r;

	/* A phone that says OK and sends no ATR is not ready with its SIM yet:
	 * asked again after a moment (Galaxy A5 2016 right after the connect,
	 * 2026-09-27) */
	for (int tries = 0;; tries++) {
		r = request(b, SAP_TRANSFER_ATR_REQ, NULL, 0, SAP_TRANSFER_ATR_RESP, request_timeout_ms, &m);
		if (r || (r = result(b, &m, -1)))
			return r;
		a = sap_get(&m, SAP_P_ATR);
		if (a || tries >= 4)
			break;
		log_dbg("bt: %s: TRANSFER_ATR_RESP without an ATR — asking again", b->addr);
		usleep(1000 * 1000);
	}
	if (!a || a->len < 2 || a->len > 33) {
		char hex[2 * 40 + 1] = "";

		for (int i = 0; a && i < a->len && i < 40; i++)
			snprintf(hex + 2 * i, 3, "%02X", a->val[i]);
		snprintf(b->be.detail, RSIM_DETAIL_MAX, a ? "the phone sent no usable ATR (%d bytes: %s%s)"
			 : "the phone sent no usable ATR (no ATR parameter, %d parameters)",
			 a ? (int)a->len : m.n, hex, (a && a->len > 40) ? "…" : "");
		return RSIM_E_PROTOCOL;
	}
	memcpy(atr, a->val, a->len);
	*atr_len = a->len;
	b->served = true;
	b->bounce = false;
	return RSIM_OK;
}

static int no_card(struct bt_backend *b)
{
	snprintf(b->be.detail, RSIM_DETAIL_MAX, b->gone ? "the link to the phone has ended"
		 : "the phone reports its SIM not accessible");
	return b->gone ? RSIM_E_IO : RSIM_E_NO_CARD;
}

static int bt_power_up(struct rsim_backend *be, uint8_t *atr, size_t *atr_len)
{
	struct bt_backend *b = (struct bt_backend *)be;
	struct sap_msg m;
	int r;

	drain(b);
	if (!b->card || b->gone)
		return no_card(b);
	if (!b->sim_on) {
		r = request(b, SAP_POWER_SIM_ON_REQ, NULL, 0, SAP_POWER_SIM_ON_RESP, request_timeout_ms, &m);
		if (r || (r = result(b, &m, SAP_RC_ALREADY_ON)))
			return r;
		b->sim_on = true;
	}
	return get_atr(b, atr, atr_len);
}

static int bt_reset(struct rsim_backend *be, uint8_t *atr, size_t *atr_len)
{
	struct bt_backend *b = (struct bt_backend *)be;
	struct sap_msg m;
	int r;

	drain(b);
	if (!b->card || b->gone)
		return no_card(b);
	if (!b->sim_on)
		return bt_power_up(be, atr, atr_len);
	r = request(b, SAP_RESET_SIM_REQ, NULL, 0, SAP_RESET_SIM_RESP, request_timeout_ms, &m);
	if (r || (r = result(b, &m, -1)))
		return r;
	return get_atr(b, atr, atr_len);
}

static int bt_power_down(struct rsim_backend *be)
{
	struct bt_backend *b = (struct bt_backend *)be;
	struct sap_msg m;
	int r;

	if (b->gone || !b->sim_on)
		return RSIM_OK;
	r = request(b, SAP_POWER_SIM_OFF_REQ, NULL, 0, SAP_POWER_SIM_OFF_RESP, request_timeout_ms, &m);
	/* a card that is gone is off as well */
	if (!r && sap_get_u8(&m, SAP_P_RESULT_CODE) == SAP_RC_REMOVED)
		r = RSIM_OK;
	else if (!r)
		r = result(b, &m, SAP_RC_ALREADY_OFF);
	if (!r)
		b->sim_on = false;
	return r;
}

static int bt_transmit(struct rsim_backend *be, const uint8_t *tpdu, size_t len, uint8_t *resp, size_t *resp_len)
{
	struct bt_backend *b = (struct bt_backend *)be;
	struct sap_param p = { .id = b->apdu_param, .len = (uint16_t)len, .val = tpdu };
	const struct sap_param *a;
	struct sap_msg m;
	int r = request(b, SAP_TRANSFER_APDU_REQ, &p, 1, SAP_TRANSFER_APDU_RESP, request_timeout_ms, &m);

	if (r || (r = result(b, &m, -1)))
		return r;
	a = sap_get(&m, SAP_P_RESPONSE_APDU);
	if (!a || a->len < 2 || a->len > RSIM_RESP_MAX) {
		snprintf(b->be.detail, RSIM_DETAIL_MAX, "the phone's answer has no response APDU");
		return RSIM_E_PROTOCOL;
	}
	memcpy(resp, a->val, a->len);
	*resp_len = a->len;
	return RSIM_OK;
}

static int bt_present(struct rsim_backend *be)
{
	struct bt_backend *b = (struct bt_backend *)be;
	int now;

	drain(b);
	if (b->gone)
		be->ended = true;
	/* a card the phone reset or swapped behind the target's back: absent
	 * once, so the target hears removed/inserted and reads it again */
	now = (b->gone || b->bounce) ? 0 : b->card;
	b->bounce = false;
	if (!now)
		b->served = false;
	return now;
}

static void bt_close(struct rsim_backend *be)
{
	struct bt_backend *b = (struct bt_backend *)be;
	struct sap_msg m;

	/* The phone gets its SIM back at once, not after a link timeout. Its
	 * answer is not waited for long: Samsung's RIL takes ~15 s to confirm
	 * a disconnect (adb logcat of a Galaxy S20 FE, 2026-09-27) and has
	 * the card back regardless — no answer here is no fault. */
	if (!b->gone && send_msg(b, SAP_DISCONNECT_REQ, NULL, 0) == RSIM_OK) {
		long deadline = now_ms() + 2000;
		int r;

		while ((r = read_msg(b, deadline, &m)) == RSIM_OK && m.id != SAP_DISCONNECT_RESP)
			;
		if (r == RSIM_OK)
			log_notice("bt: %s: SIM access ended, the phone has its SIM back", b->addr);
		else
			log_dbg("bt: %s: DISCONNECT_REQ sent; the phone confirms it later", b->addr);
	}
	close(b->fd);
	free(b);
}

static void bt_info(struct rsim_backend *be, struct jw *w);

static const struct rsim_backend_ops BT_OPS = {
	.name = "bt",
	.power_up = bt_power_up,
	.reset = bt_reset,
	.power_down = bt_power_down,
	.transmit = bt_transmit,
	.present = bt_present,
	.close = bt_close,
	.info = bt_info,
};

/* CONNECT_REQ until the phone accepts a message size, then its first
 * STATUS_IND. 0, or -1 (logged). */
static int sap_connect(struct bt_backend *b)
{
	/* The sizes asked for, largest first. A server that cannot take one
	 * should answer "message size not supported" with its own (status 2),
	 * but Samsung's SAP RIL answers a plain failure (status 1) to 1024 and
	 * takes 261 at most (adb logcat of a Galaxy S20 FE, 2026-09-27:
	 * "connectResponse: ... sapConnectRsp 1 maxMsgSize 261") — so a failure
	 * is asked again, smaller. */
	static const uint16_t sizes[] = { SAP_MSG_WANT, 512, 300, SAP_MSG_MIN, SAP_MSG_LOW };
	uint16_t want = sizes[0];
	size_t next = 1;
	long deadline;
	int tries;

	for (tries = 0; tries < 8; tries++) {
		uint8_t sz[2] = { (uint8_t)(want >> 8), (uint8_t)want };
		struct sap_param p = { .id = SAP_P_MAX_MSG_SIZE, .len = 2, .val = sz };
		const struct sap_param *ms;
		struct sap_msg m;
		int st;

		long left = b->bringup_until - now_ms();

		if (left < 1000) {
			log_err("bt: %s: no SIM access within %d s — given up for now", b->addr, BRINGUP_MS / 1000);
			return -1;
		}
		b->be.detail[0] = '\0';
		if (request(b, SAP_CONNECT_REQ, &p, 1, SAP_CONNECT_RESP,
			    left < CONNECT_TIMEOUT_MS ? (int)left : CONNECT_TIMEOUT_MS, &m)) {
			log_err("bt: %s: SIM access not granted (%s)", b->addr, b->be.detail);
			return -1;
		}
		st = sap_get_u8(&m, SAP_P_CONNECTION_STATUS);
		ms = sap_get(&m, SAP_P_MAX_MSG_SIZE);
		log_dbg("bt: %s: CONNECT_REQ %u bytes -> status %d", b->addr, want, st);
		if (st == SAP_CONN_OK || st == SAP_CONN_OK_CALL) {
			b->max_msg = want;
			if (want < SAP_MSG_MIN)
				log_warn("bt: %s: the phone takes SAP messages of %u bytes: a command longer than %u bytes cannot be passed",
					 b->addr, want, (unsigned)(want - 8));
			if (st == SAP_CONN_OK_CALL)
				log_notice("bt: %s: a call is going on at the phone; its SIM follows when it ends", b->addr);
			break;
		}
		if (st == SAP_CONN_MSGSIZE && ms && ms->len == 2) {
			uint16_t theirs = (uint16_t)((ms->val[0] << 8) | ms->val[1]);

			if (theirs >= SAP_MSG_LOW && theirs != want) {
				log_dbg("bt: %s: the phone takes messages of %u bytes", b->addr, theirs);
				want = theirs;
				continue;
			}
			log_err("bt: %s: the phone takes SAP messages of %u bytes only, %u needed at least", b->addr, theirs, SAP_MSG_LOW);
			return -1;
		}
		/* a failure (or "too small" to a smaller size): the next size */
		if ((st == SAP_CONN_FAIL || st == SAP_CONN_TOOSMALL) && next < sizeof(sizes) / sizeof(sizes[0])) {
			log_notice("bt: %s: the phone refuses SIM access with %u-byte messages (status %d) — asking with %u",
				   b->addr, want, st, sizes[next]);
			want = sizes[next++];
			continue;
		}
		log_err("bt: %s: the phone refuses SIM access (connection status %d)", b->addr, st);
		return -1;
	}
	if (!b->max_msg) {
		log_err("bt: %s: no agreement on the message size", b->addr);
		return -1;
	}

	/* the server's STATUS_IND says the card is ready; without it (a call
	 * still going on) the card is reported absent until it comes */
	deadline = now_ms() + STATUS_TIMEOUT_MS;
	if (deadline > b->bringup_until + STATUS_TIMEOUT_MS)
		deadline = b->bringup_until + STATUS_TIMEOUT_MS;
	while (!b->gone && b->card < 0) {
		struct sap_msg m;
		int r = read_msg(b, deadline, &m);

		if (r)
			break;
		unsolicited(b, &m);
	}
	if (b->gone)
		return -1;
	if (b->card < 0)
		b->card = 0;
	return 0;
}

struct rsim_backend *bt_open(const struct bt_cfg *cfg)
{
	struct bt_backend *b;
	uint8_t bdaddr[6];
	const char *test = getenv("RSIM_TEST_SAP_SOCK");
	int channel = cfg->channel;

	if (bt_parse_addr(cfg->addr, bdaddr) < 0) {
		log_err("bt: %s: not a Bluetooth address (AA:BB:CC:DD:EE:FF)", cfg->addr);
		return NULL;
	}
	if (!(b = calloc(1, sizeof(*b))))
		return NULL;
	snprintf(b->addr, sizeof(b->addr), "%s", cfg->addr);
	b->bringup_until = now_ms() + BRINGUP_MS;
	b->apdu_param = cfg->apdu7816 ? SAP_P_COMMAND_APDU7816 : SAP_P_COMMAND_APDU;
	b->card = -1;
	b->be.ops = &BT_OPS;
	b->be.reader = cfg->addr;

	if (test) {
		const char *t = getenv("RSIM_TEST_SAP_TIMEOUT_MS");

		if (t && atoi(t) > 0)
			request_timeout_ms = atoi(t);
		b->fd = open_test(test);
	}
	else {
		if (!channel) {
			channel = sdp_channel(bdaddr, cfg->addr);
			if (channel == -1)
				log_err("bt: %s: the phone offers no SIM Access (SAP) — not supported, or not enabled on it",
					cfg->addr);
		}
		b->fd = channel > 0 ? open_rfcomm(bdaddr, channel, cfg->secure_high, cfg->addr) : -1;
	}
	if (b->fd < 0) {
		free(b);
		return NULL;
	}
	if (sap_connect(b) < 0) {
		close(b->fd);
		free(b);
		return NULL;
	}
	b->channel = channel;
	b->secure_high = cfg->secure_high;
	log_notice("bt: %s: SIM access over channel %d, messages up to %u bytes, the SIM %s", cfg->addr,
		   channel, b->max_msg, b->card ? "ready" : "not (yet) accessible");
	return &b->be;
}

/* ---- what is known about a phone: BlueZ's storage, the kernel's mgmt API -- */

static int is_addr(const char *s)
{
	unsigned char b[6];

	return bt_parse_addr(s, b) == 0;
}

/* one device's BlueZ files: <adapter>/<device>/info and <adapter>/cache/<device> */
struct bt_dev {
	char name[128], alias[128];
	unsigned long cls;
	int sap;		/* 1 offered, 0 not, -1 no service list stored */
	int sap_channel;	/* from the cached SDP record, 0 unknown */
	int paired;		/* a BR/EDR link key (an LE-only bond is not) */
	int key_type;		/* its Type, -1 none */
	int pin_len;
	int trusted, blocked;
	char services[256];	/* short names, comma separated */
	long vendor, product, version;	/* [DeviceID], -1 none */
	long vendor_src;
};

static const char *uuid_name(unsigned u)
{
	static const struct { unsigned u; const char *n; } t[] = {
		{ 0x1101, "SPP" }, { 0x1103, "DUN" }, { 0x1105, "OPP" }, { 0x1108, "HSP" }, { 0x110a, "A2DP-source" },
		{ 0x110b, "A2DP-sink" }, { 0x110c, "AVRCP-target" }, { 0x110e, "AVRCP" }, { 0x1112, "HSP-AG" },
		{ 0x1115, "PANU" }, { 0x1116, "NAP" }, { 0x111e, "HFP" }, { 0x111f, "HFP-AG" }, { 0x112d, "SAP" },
		{ 0x112f, "PBAP" }, { 0x1132, "MAP" }, { 0x1200, "PnP" }, { 0x1800, "GAP" }, { 0x1801, "GATT" },
		{ 0x180a, "DIS" }, { 0x180f, "Battery" },
	};
	size_t i;

	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (t[i].u == u)
			return t[i].n;
	return NULL;
}

static void add_service(struct bt_dev *d, const char *uuid)
{
	char n[16];
	const char *name;
	unsigned u;
	size_t o = strlen(d->services);

	/* 0000xxxx-0000-1000-8000-00805f9b34fb: a 16-bit one; others skipped */
	if (strlen(uuid) < 36 || strncmp(uuid, "0000", 4) || strncasecmp(uuid + 8, "-0000-1000-8000-00805f9b34fb", 28))
		return;
	u = (unsigned)strtoul((char[]){ uuid[4], uuid[5], uuid[6], uuid[7], 0 }, NULL, 16);
	if (!(name = uuid_name(u))) {
		snprintf(n, sizeof(n), "0x%04x", u);
		name = n;
	}
	if (u == SAP_UUID16)
		d->sap = 1;
	snprintf(d->services + o, sizeof(d->services) - o, "%s%s", o ? "," : "", name);
}

static void read_info(const char *path, struct bt_dev *d)
{
	char line[4096], section[32] = "";
	FILE *f = fopen(path, "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			snprintf(section, sizeof(section), "%.*s", (int)strcspn(line + 1, "]"), line + 1);
			if (!strcmp(section, "LinkKey"))
				d->paired = 1;
			continue;
		}
		if (!strcmp(section, "General")) {
			if (!strncmp(line, "Name=", 5))
				snprintf(d->name, sizeof(d->name), "%.*s", (int)sizeof(d->name) - 1, line + 5);
			else if (!strncmp(line, "Alias=", 6))
				snprintf(d->alias, sizeof(d->alias), "%.*s", (int)sizeof(d->alias) - 1, line + 6);
			else if (!strncmp(line, "Class=", 6))
				d->cls = strtoul(line + 6, NULL, 16);
			else if (!strncmp(line, "Trusted=", 8))
				d->trusted = !strcmp(line + 8, "true");
			else if (!strncmp(line, "Blocked=", 8))
				d->blocked = !strcmp(line + 8, "true");
			else if (!strncmp(line, "Services=", 9)) {
				char *p, *save = NULL;

				if (d->sap < 0)
					d->sap = 0;
				for (p = strtok_r(line + 9, ";", &save); p; p = strtok_r(NULL, ";", &save))
					add_service(d, p);
			}
		} else if (!strcmp(section, "LinkKey")) {
			if (!strncmp(line, "Type=", 5))
				d->key_type = atoi(line + 5);
			else if (!strncmp(line, "PINLength=", 10))
				d->pin_len = atoi(line + 10);
		} else if (!strcmp(section, "DeviceID")) {
			if (!strncmp(line, "Source=", 7))
				d->vendor_src = strtol(line + 7, NULL, 0);
			else if (!strncmp(line, "Vendor=", 7))
				d->vendor = strtol(line + 7, NULL, 0);
			else if (!strncmp(line, "Product=", 8))
				d->product = strtol(line + 8, NULL, 0);
			else if (!strncmp(line, "Version=", 8))
				d->version = strtol(line + 8, NULL, 0);
		}
	}
	fclose(f);
}

/* The SDP records BlueZ cached from the phone ([ServiceRecords], one hex
 * string each): the SIM Access one names its RFCOMM channel — known without
 * asking the phone. */
static void read_cache(const char *path, struct bt_dev *d)
{
	char line[8192], section[32] = "";
	FILE *f = fopen(path, "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		uint8_t rec[2048];
		char *eq;
		int n, ch;

		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			snprintf(section, sizeof(section), "%.*s", (int)strcspn(line + 1, "]"), line + 1);
			continue;
		}
		if (!strcmp(section, "General") && !strncmp(line, "Name=", 5) && !d->name[0])
			snprintf(d->name, sizeof(d->name), "%.*s", (int)sizeof(d->name) - 1, line + 5);
		if (strcmp(section, "ServiceRecords") || !(eq = strchr(line, '=')))
			continue;
		if ((n = hex_decode(eq + 1, rec, sizeof(rec))) < 3)
			continue;
		/* its service class list names 0x112D (UUID16 element 19 11 2D) */
		for (int i = 0; i + 2 < n; i++)
			if (rec[i] == 0x19 && rec[i + 1] == 0x11 && rec[i + 2] == 0x2d) {
				d->sap = 1;
				if ((ch = sdp_rfcomm_channel(rec, (size_t)n)) > 0)
					d->sap_channel = ch;
				break;
			}
	}
	fclose(f);
}

static void dev_init(struct bt_dev *d)
{
	memset(d, 0, sizeof(*d));
	d->sap = -1;
	d->key_type = -1;
	d->vendor = d->product = d->version = d->vendor_src = -1;
}

/* <sysroot>/var/lib/bluetooth/<adapter>/<addr>: its files, the adapter's
 * address in adapter[]; 0 found, -1 not */
static int dev_lookup(const char *sysroot, const char *addr, struct bt_dev *d, char *adapter, size_t cap)
{
	char base[PATH_MAX], p[PATH_MAX];
	DIR *ad;
	struct dirent *a;
	int found = -1;

	dev_init(d);
	snprintf(base, sizeof(base), "%s/var/lib/bluetooth", sysroot);
	if (!(ad = opendir(base)))
		return -1;
	while (found && (a = readdir(ad))) {
		if (!is_addr(a->d_name))
			continue;
		if (snprintf(p, sizeof(p), "%s/%s/%s/info", base, a->d_name, addr) >= (int)sizeof(p) || access(p, R_OK))
			continue;
		read_info(p, d);
		if (snprintf(p, sizeof(p), "%s/%s/cache/%s", base, a->d_name, addr) < (int)sizeof(p))
			read_cache(p, d);
		snprintf(adapter, cap, "%.*s", (int)cap - 1, a->d_name);
		found = 0;
	}
	closedir(ad);
	return found;
}

static const char *class_kind(unsigned long cls)
{
	static const char *const major[] = { "misc", "computer", "phone", "network", "audio/video", "peripheral",
					     "imaging", "wearable", "toy", "health" };
	static const char *const phone[] = { "phone", "cellular", "cordless", "smartphone", "modem", "ISDN" };
	unsigned mj = (cls >> 8) & 0x1f, mn = (cls >> 2) & 0x3f;

	if (!cls)
		return "";
	if (mj == 2 && mn < 6)
		return phone[mn];
	return mj < 10 ? major[mj] : "other";
}

/* how the pairing was made: what `--bt-security high` needs is an
 * authenticated key (MITM-protected: a PIN or a compared number) */
static const char *key_kind(int type, int pin_len)
{
	switch (type) {
	case -1: return "";
	case 0: return pin_len >= 16 ? "legacy PIN (16 digits)" : "legacy PIN";
	case 4: case 7: return "unauthenticated";
	case 5: case 8: return "authenticated";
	default: return "other";
	}
}

static const char *vendor_name(long src, long v)
{
	/* Bluetooth SIG company ids (Source 1); a few that make phones */
	static const struct { long id; const char *n; } t[] = {
		{ 0x004c, "Apple" }, { 0x0075, "Samsung" }, { 0x00e0, "Google" }, { 0x027d, "Huawei" },
		{ 0x038f, "Xiaomi" }, { 0x0046, "MediaTek" }, { 0x001d, "Qualcomm" }, { 0x000f, "Broadcom" },
		{ 0x0001, "Nokia" }, { 0x0056, "Sony Ericsson" }, { 0x0072, "OnePlus/Oppo" },
	};
	size_t i;

	if (src != 1)
		return NULL;
	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (t[i].id == v)
			return t[i].n;
	return NULL;
}

/* The kernel's Bluetooth management API (HCI_CHANNEL_CONTROL), read only:
 * the adapters, whether they are on, and who is connected now. It needs
 * CAP_NET_ADMIN; without it (or without Bluetooth) ok stays 0. */
#define BTPROTO_HCI 1
#define HCI_CHANNEL_CONTROL 3
#define MGMT_INDEX_NONE 0xFFFF
#define MGMT_EV_CMD_COMPLETE 0x0001
#define MGMT_EV_CMD_STATUS 0x0002
#define MGMT_OP_READ_INDEX_LIST 0x0003
#define MGMT_OP_READ_INFO 0x0004
#define MGMT_OP_GET_CONNECTIONS 0x0015

struct sa_hci {
	sa_family_t family;
	unsigned short dev;
	unsigned short channel;
};

struct bt_mgmt {
	int ok;
	int nadp;
	struct { int index; char addr[18]; char name[64]; int powered; } adp[8];
	int nconn;
	char conn[32][18];
};

static void addr_str(const uint8_t *b, char *out)
{
	snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", b[5], b[4], b[3], b[2], b[1], b[0]);
}

/* one command, its reply's data into out; the data length or -1 */
static int mgmt_cmd(int fd, uint16_t op, uint16_t index, uint8_t *out, size_t cap)
{
	uint8_t req[6] = { (uint8_t)op, (uint8_t)(op >> 8), (uint8_t)index, (uint8_t)(index >> 8), 0, 0 };
	uint8_t buf[1024];
	long deadline = now_ms() + 1000;

	if (write(fd, req, sizeof(req)) != (ssize_t)sizeof(req))
		return -1;
	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		long left = deadline - now_ms();
		ssize_t n;
		uint16_t ev, len, rop;

		if (left <= 0 || poll(&p, 1, (int)left) <= 0)
			return -1;
		if ((n = read(fd, buf, sizeof(buf))) < 9)
			continue;
		ev = (uint16_t)(buf[0] | buf[1] << 8);
		len = (uint16_t)(buf[4] | buf[5] << 8);
		rop = (uint16_t)(buf[6] | buf[7] << 8);
		if (rop != op || (ev != MGMT_EV_CMD_COMPLETE && ev != MGMT_EV_CMD_STATUS))
			continue;	/* another event */
		if (ev == MGMT_EV_CMD_STATUS || buf[8] != 0 || (size_t)n < 6u + len || len < 3)
			return -1;
		len -= 3;
		if (len > cap)
			len = (uint16_t)cap;
		memcpy(out, buf + 9, len);
		return len;
	}
}

static void mgmt_read(struct bt_mgmt *m)
{
	struct sa_hci sa = { .family = AF_BLUETOOTH, .dev = MGMT_INDEX_NONE, .channel = HCI_CHANNEL_CONTROL };
	uint8_t d[512];
	int fd, n, i, cnt;

	memset(m, 0, sizeof(*m));
	fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, BTPROTO_HCI);
	if (fd < 0)
		return;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    (n = mgmt_cmd(fd, MGMT_OP_READ_INDEX_LIST, MGMT_INDEX_NONE, d, sizeof(d))) < 2) {
		close(fd);
		return;
	}
	m->ok = 1;
	cnt = d[0] | d[1] << 8;
	for (i = 0; i < cnt && m->nadp < 8 && 2 + 2 * i + 1 < n; i++) {
		int idx = d[2 + 2 * i] | d[3 + 2 * i] << 8;
		uint8_t info[300], c[512];
		int k, cn;

		m->adp[m->nadp].index = idx;
		if (mgmt_cmd(fd, MGMT_OP_READ_INFO, (uint16_t)idx, info, sizeof(info)) >= 20) {
			addr_str(info, m->adp[m->nadp].addr);
			/* current settings, bit 0: powered */
			m->adp[m->nadp].powered = info[13] & 1;
			snprintf(m->adp[m->nadp].name, sizeof(m->adp[m->nadp].name), "%.*s", 63, (const char *)info + 20);
		}
		if ((cn = mgmt_cmd(fd, MGMT_OP_GET_CONNECTIONS, (uint16_t)idx, c, sizeof(c))) >= 2)
			for (k = 0; k < (c[0] | c[1] << 8) && 2 + 7 * k + 6 < cn && m->nconn < 32; k++)
				addr_str(c + 2 + 7 * k, m->conn[m->nconn++]);
		m->nadp++;
	}
	close(fd);
}

static int mgmt_connected(const struct bt_mgmt *m, const char *addr)
{
	int i;

	for (i = 0; i < m->nconn; i++)
		if (!strcasecmp(m->conn[i], addr))
			return 1;
	return 0;
}

/* every field known about one phone */
static void dev_write(struct jw *w, const char *addr, const struct bt_dev *d, const char *adapter,
		      const struct bt_mgmt *m)
{
	const char *vn = vendor_name(d->vendor_src, d->vendor);
	char v[32];
	int i;

	jw_str(w, "name", d->name);
	if (d->alias[0] && strcmp(d->alias, d->name))
		jw_str(w, "alias", d->alias);
	if (d->cls) {
		snprintf(v, sizeof(v), "0x%06lx", d->cls);
		jw_str(w, "class", v);
		jw_opt(w, "kind", class_kind(d->cls));
	}
	jw_bool(w, "paired", d->paired);
	jw_bool(w, "trusted", d->trusted);
	if (d->blocked)
		jw_bool(w, "blocked", true);
	jw_opt(w, "key", key_kind(d->key_type, d->pin_len));
	jw_opt(w, "services", d->services);
	if (d->vendor >= 0) {
		snprintf(v, sizeof(v), "%s0x%04lx", d->vendor_src == 2 ? "usb:" : "", d->vendor);
		jw_str(w, "vendor_id", v);
		jw_opt(w, "vendor", vn);
		snprintf(v, sizeof(v), "0x%04lx/0x%04lx", d->product, d->version);
		jw_str(w, "product_version", v);
	}
	if (d->sap < 0)
		jw_null(w, "sap");
	else
		jw_bool(w, "sap", d->sap == 1);
	if (d->sap_channel)
		jw_int(w, "sap_channel", d->sap_channel);
	jw_opt(w, "adapter", adapter);
	if (m && m->ok) {
		jw_bool(w, "connected", mgmt_connected(m, addr));
		for (i = 0; i < m->nadp; i++)
			if (!strcasecmp(m->adp[i].addr, adapter)) {
				jw_opt(w, "adapter_name", m->adp[i].name);
				jw_bool(w, "adapter_powered", m->adp[i].powered);
			}
	}
}

static void bt_info(struct rsim_backend *be, struct jw *w)
{
	struct bt_backend *b = (struct bt_backend *)be;
	struct bt_dev d;
	struct bt_mgmt m;
	char adapter[32] = "";

	if (!dev_lookup("", b->addr, &d, adapter, sizeof(adapter))) {
		mgmt_read(&m);
		dev_write(w, b->addr, &d, adapter, &m);
	}
	if (b->channel > 0)
		jw_int(w, "channel", b->channel);
	jw_int(w, "max_msg", b->max_msg);
	jw_str(w, "apdu_format", b->apdu_param == SAP_P_COMMAND_APDU7816 ? "7816" : "gsm");
	jw_str(w, "security", b->secure_high ? "high" : "medium");
	jw_str(w, "sim", b->gone ? "link ended" : b->card > 0 ? "accessible" : "not accessible");
}

int bt_list(const char *sysroot, char *note, size_t note_cap, char *adapters, size_t acap)
{
	char base[PATH_MAX];
	DIR *ad;
	struct dirent *a;
	struct bt_mgmt m;
	int n = 0, i;
	size_t o = 0;

	note[0] = '\0';
	adapters[0] = '\0';
	/* the adapters the kernel has, and whether they are on */
	mgmt_read(&m);
	for (i = 0; m.ok && i < m.nadp && o < acap; i++)
		o += (size_t)snprintf(adapters + o, acap - o, "%shci%d %s%s%s %s", i ? ", " : "", m.adp[i].index,
				      m.adp[i].addr, m.adp[i].name[0] ? " " : "", m.adp[i].name,
				      m.adp[i].powered ? "on" : "off");
	if (m.ok && !m.nadp)
		snprintf(adapters, acap, "none");

	snprintf(base, sizeof(base), "%s/var/lib/bluetooth", sysroot);
	if (!(ad = opendir(base))) {
		/* no BlueZ is no news; BlueZ that we may not read is */
		if (errno == EACCES)
			snprintf(note, note_cap, "bt: %s is not readable — run as root to list the paired phones", base);
		return 0;
	}
	while ((a = readdir(ad))) {
		char adir[PATH_MAX];
		DIR *dd;
		struct dirent *e;

		if (!is_addr(a->d_name) || snprintf(adir, sizeof(adir), "%s/%s", base, a->d_name) >= (int)sizeof(adir))
			continue;
		if (!(dd = opendir(adir))) {
			if (errno == EACCES)
				snprintf(note, note_cap, "bt: %s is not readable (root only) — paired phones not listed", adir);
			continue;
		}
		while ((e = readdir(dd))) {
			char p[PATH_MAX], spec[8 + sizeof(e->d_name)];
			struct bt_dev d;
			struct jw w;
			int phone;

			if (!is_addr(e->d_name) || snprintf(p, sizeof(p), "%s/%s/info", adir, e->d_name) >= (int)sizeof(p))
				continue;
			dev_init(&d);
			read_info(p, &d);
			if (snprintf(p, sizeof(p), "%s/cache/%s", adir, e->d_name) < (int)sizeof(p))
				read_cache(p, &d);
			/* major device class 2: phone (Assigned Numbers §2.8.2) */
			phone = ((d.cls >> 8) & 0x1f) == 2;
			if (!d.paired || !(phone || d.sap == 1))
				continue;
			snprintf(spec, sizeof(spec), "bt:%s", e->d_name);
			jw_begin(&w, stdout);
			jw_str(&w, "backend", "bt");
			jw_str(&w, "spec", spec);
			jw_str(&w, "device", e->d_name);
			jw_bool(&w, "phone", phone);
			dev_write(&w, e->d_name, &d, a->d_name, &m);
			jw_end(&w);
			n++;
		}
		closedir(dd);
	}
	closedir(ad);
	return n;
}
