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
	int card;		/* the phone's last word: 1 usable, 0 not */
	bool sim_on;		/* powered on at the phone */
	bool served;		/* an ATR went out since the target last saw no card */
	bool bounce;		/* reset by the phone behind the target's back */
	bool gone;		/* the link ended */
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
	int r = request(b, SAP_TRANSFER_ATR_REQ, NULL, 0, SAP_TRANSFER_ATR_RESP, request_timeout_ms, &m);

	if (r || (r = result(b, &m, -1)))
		return r;
	a = sap_get(&m, SAP_P_ATR);
	if (!a || a->len < 2 || a->len > 33) {
		snprintf(b->be.detail, RSIM_DETAIL_MAX, "the phone sent no usable ATR");
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

	/* the phone gets its SIM back at once, not after a link timeout */
	if (!b->gone && request(b, SAP_DISCONNECT_REQ, NULL, 0, SAP_DISCONNECT_RESP, 2000, &m) == RSIM_OK)
		log_notice("bt: %s: SIM access ended, the phone has its SIM back", b->addr);
	close(b->fd);
	free(b);
}

static const struct rsim_backend_ops BT_OPS = {
	.name = "bt",
	.power_up = bt_power_up,
	.reset = bt_reset,
	.power_down = bt_power_down,
	.transmit = bt_transmit,
	.present = bt_present,
	.close = bt_close,
};

/* CONNECT_REQ until the phone accepts a message size, then its first
 * STATUS_IND. 0, or -1 (logged). */
static int sap_connect(struct bt_backend *b)
{
	uint16_t want = SAP_MSG_WANT;
	long deadline;
	int tries;

	for (tries = 0; tries < 3; tries++) {
		uint8_t sz[2] = { (uint8_t)(want >> 8), (uint8_t)want };
		struct sap_param p = { .id = SAP_P_MAX_MSG_SIZE, .len = 2, .val = sz };
		const struct sap_param *ms;
		struct sap_msg m;
		int st;

		b->be.detail[0] = '\0';
		if (request(b, SAP_CONNECT_REQ, &p, 1, SAP_CONNECT_RESP, CONNECT_TIMEOUT_MS, &m)) {
			log_err("bt: %s: SIM access not granted (%s)", b->addr, b->be.detail);
			return -1;
		}
		st = sap_get_u8(&m, SAP_P_CONNECTION_STATUS);
		ms = sap_get(&m, SAP_P_MAX_MSG_SIZE);
		if (st == SAP_CONN_OK || st == SAP_CONN_OK_CALL) {
			b->max_msg = want;
			if (st == SAP_CONN_OK_CALL)
				log_notice("bt: %s: a call is going on at the phone; its SIM follows when it ends", b->addr);
			break;
		}
		if (st == SAP_CONN_MSGSIZE && ms && ms->len == 2) {
			uint16_t theirs = (uint16_t)((ms->val[0] << 8) | ms->val[1]);

			if (theirs >= SAP_MSG_MIN && theirs != want) {
				log_dbg("bt: %s: the phone takes messages of %u bytes", b->addr, theirs);
				want = theirs;
				continue;
			}
			log_err("bt: %s: the phone takes SAP messages of %u bytes only, %u needed", b->addr, theirs, SAP_MSG_MIN);
			return -1;
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
	log_notice("bt: %s: SIM access over channel %d, messages up to %u bytes, the SIM %s", cfg->addr,
		   channel, b->max_msg, b->card ? "ready" : "not (yet) accessible");
	return &b->be;
}

/* ---- --list: the paired phones ----------------------------------------- */

static int is_addr(const char *s)
{
	unsigned char b[6];

	return bt_parse_addr(s, b) == 0;
}

/* one device's BlueZ info file: name, class, services and whether it holds
 * a BR/EDR link key (paired for SAP; an LE-only bond is not) */
struct bt_dev {
	char name[128];
	unsigned long cls;
	int sap;		/* 1 offered, 0 not, -1 no service list stored */
	int paired;
};

static void read_info(const char *path, struct bt_dev *d)
{
	char line[4096], section[32] = "";
	FILE *f = fopen(path, "r");

	memset(d, 0, sizeof(*d));
	d->sap = -1;
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
		if (strcmp(section, "General"))
			continue;
		if (!strncmp(line, "Name=", 5))
			snprintf(d->name, sizeof(d->name), "%.*s", (int)sizeof(d->name) - 1, line + 5);
		else if (!strncmp(line, "Class=", 6))
			d->cls = strtoul(line + 6, NULL, 16);
		else if (!strncmp(line, "Services=", 9)) {
			const char *p;

			d->sap = 0;
			for (p = line + 9; *p; p += strcspn(p, ";"), p += (*p == ';'))
				if (!strncasecmp(p, "0000112d-0000-1000-8000-00805f9b34fb", 36))
					d->sap = 1;
		}
	}
	fclose(f);
}

int bt_list(const char *sysroot, char *note, size_t note_cap)
{
	char base[PATH_MAX];
	DIR *ad;
	struct dirent *a;
	int n = 0;

	note[0] = '\0';
	snprintf(base, sizeof(base), "%s/var/lib/bluetooth", sysroot);
	if (!(ad = opendir(base))) {
		/* no BlueZ is no news; BlueZ that we may not read is */
		if (errno == EACCES)
			snprintf(note, note_cap, "bt: %s is not readable (root only) — paired phones not listed", base);
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
			char info[PATH_MAX], spec[8 + sizeof(e->d_name)];
			struct bt_dev d;
			struct jw w;
			int phone;

			if (!is_addr(e->d_name) || snprintf(info, sizeof(info), "%s/%s/info", adir, e->d_name) >= (int)sizeof(info))
				continue;
			read_info(info, &d);
			/* major device class 2: phone (Assigned Numbers §2.8.2) */
			phone = ((d.cls >> 8) & 0x1f) == 2;
			if (!d.paired || !(phone || d.sap == 1))
				continue;
			snprintf(spec, sizeof(spec), "bt:%s", e->d_name);
			jw_begin(&w, stdout);
			jw_str(&w, "backend", "bt");
			jw_str(&w, "spec", spec);
			jw_str(&w, "device", e->d_name);
			jw_str(&w, "name", d.name);
			jw_str(&w, "adapter", a->d_name);
			jw_bool(&w, "phone", phone);
			if (d.sap < 0)
				jw_null(&w, "sap");
			else
				jw_bool(&w, "sap", d.sap == 1);
			jw_end(&w);
			n++;
		}
		closedir(dd);
	}
	closedir(ad);
	return n;
}
