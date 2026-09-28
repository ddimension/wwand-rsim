/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * A card in an osmo-remsim SIM bank (rspro:): the helper is a remsim
 * client. osmo-remsim has three parts — remsim-bankd owns the readers of a
 * SIM bank, remsim-server keeps the slot mappings (bank slot <-> client
 * slot), a client stands in for a modem's SIM slot — and they talk RSPRO
 * (rspro.c) over TCP:
 *
 *   1. client -> server  ConnectClientReq(identity, client id:slot)
 *   2. server -> client  ConfigClientBankReq(bank id:slot, bankd ip:port)
 *                        once a mapping for this client slot exists; the
 *                        same with 0.0.0.0:0 when it is taken away
 *   3. client -> bankd   ConnectClientReq(identity, client id:slot)
 *   4. bankd -> client   SetAtrReq(ATR) once it has the card up
 *   5. client -> bankd   TpduModemToCard(TPDU) / bankd -> client
 *                        TpduCardToModem(response + SW)
 *   6. client -> bankd   ClientSlotStatusInd(RST, VCC, CLK) when the modem
 *                        resets or powers the card
 *
 * The mapping is the server operator's (its REST API or VTY). A reader spec
 * that names a bank slot, `rspro:<server>/<bank>:<slot>`, makes it here
 * instead: the helper creates it over the REST API before it connects and
 * removes it at the end — a card taken from the bank for as long as it is
 * used. RSPRO has no message to list banks or slots (a client only ever
 * learns the one slot mapped to it), so `--list` asks the REST API too.
 *
 * Not verified against a running osmo-remsim (2026-09-27): the message
 * layout follows asn1/RSPRO.asn, the REST paths and JSON keys follow
 * remsim-server's rest_api.c, the reset signalling what a card emulator
 * reports. The tests run against a simulated server and bankd built from
 * the same reading (tests/test_e2e_rspro.py).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "atr.h"
#include "jtok.h"
#include "log.h"
#include "remsim.h"
#include "rspro.h"

#define CONNECT_TIMEOUT_MS	5000
/* the server's ConnectClientRes */
#define HELLO_TIMEOUT_MS	10000
/* a mapping, the bankd and its ATR: below the plugin's 15 s for an
 * answer, so a card that is not there is an answer, not a restart */
#define ATR_WAIT_MS		10000
/* a reset's fresh ATR, when the bankd sends one; else the last one stands */
#define RESET_ATR_WAIT_MS	1500
/* the card's answer; the plugin waits 30 s for it */
#define TPDU_TIMEOUT_MS		25000
#define BANK_RETRY_MS		5000
#define REMAP_MS		30000
#define HTTP_TIMEOUT_MS		5000
/* a REST answer; remsim-server lists every bank and mapping in one */
#define HTTP_MAX		(1024 * 1024)

#define SW_NAME			"rsim-card"
#define SW_VERSION		"1"

struct conn {
	int fd;			/* -1: not connected */
	bool connecting;	/* connect() under way (the bankd's) */
	long connect_deadline;
	uint8_t buf[RSPRO_MSG_MAX + 8];
	size_t len;
};

struct rp {
	struct rsim_backend be;
	char reader[300];
	char host[256];		/* the server, without brackets */
	uint16_t port, rest_port;
	struct rspro_slot cs;		/* our client slot */
	/* a bank slot the spec names, mapped by us over REST */
	bool want_bank, made_map;
	/* the mapping was there already, to our client slot: ours to remove —
	 * unless a live session holds that identity (the server says so) */
	bool adopted;
	/* the server took our slot away while the spec names it: mapped again,
	 * at most every REMAP_MS */
	bool lost_map;
	long remap_at;
	struct rspro_slot want;

	struct conn srv, bank;
	uint32_t tag;
	int srv_result;			/* ConnectClientRes; -1 not yet */
	char srv_name[RSPRO_NAME_MAX], srv_sw[RSPRO_NAME_MAX], srv_ver[RSPRO_NAME_MAX];

	/* what the server said to use: the bank slot and its bankd */
	bool have_target;
	struct rspro_slot bs;
	char bank_ip[INET6_ADDRSTRLEN];
	uint16_t bank_port;
	bool bank_ready;		/* the bankd accepted us */
	long bank_retry_at;

	uint8_t atr[ATR_MAX];
	size_t atr_len;
	bool have_atr;
	/* an ATR the bankd sent on its own (it brought the card up) that no
	 * power-up has answered with yet: that one needs no reset pulse */
	bool atr_fresh;
	unsigned atr_seq;		/* bumped by every SetAtrReq */
	unsigned atr_seq_seen;		/* what a reset waits to change */

	bool waiting;			/* a TPDU is out */
	bool got_resp;
	uint8_t resp[RSPRO_DATA_MAX];
	size_t resp_len;
};

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ---- TCP ----------------------------------------------------------------- */

static int tcp_connect(const char *host, uint16_t port, int timeout_ms, char *why, size_t whylen)
{
	struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res, *ai;
	char ps[8];
	int fd = -1, r;

	snprintf(ps, sizeof(ps), "%u", port);
	r = getaddrinfo(host, ps, &hints, &res);
	if (r) {
		snprintf(why, whylen, "%s: %s", host, gai_strerror(r));
		return -1;
	}
	snprintf(why, whylen, "%s port %u: no address", host, port);
	for (ai = res; ai; ai = ai->ai_next) {
		struct pollfd pfd;
		int err = 0;
		socklen_t el = sizeof(err);

		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0)
			continue;
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		r = connect(fd, ai->ai_addr, ai->ai_addrlen);
		if (r < 0 && errno == EINPROGRESS) {
			pfd.fd = fd;
			pfd.events = POLLOUT;
			r = poll(&pfd, 1, timeout_ms);
			if (r == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) && !err)
				r = 0;
			else {
				errno = r == 0 ? ETIMEDOUT : (err ? err : errno);
				r = -1;
			}
		}
		if (!r)
			break;
		snprintf(why, whylen, "%s port %u: %s", host, port, strerror(errno));
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

/* the whole buffer, the socket being non-blocking */
static int send_all(int fd, const uint8_t *b, size_t n, int timeout_ms)
{
	while (n) {
		ssize_t w = send(fd, b, n, MSG_NOSIGNAL);

		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd pfd = { .fd = fd, .events = POLLOUT };

			if (poll(&pfd, 1, timeout_ms) != 1)
				return -1;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		b += w;
		n -= (size_t)w;
	}
	return 0;
}

/* ---- RSPRO over IPA ------------------------------------------------------ */

static void conn_close(struct conn *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->len = 0;
	c->connecting = false;
}

static int conn_send_frame(struct conn *c, const uint8_t *frame, size_t n)
{
	if (c->fd < 0 || send_all(c->fd, frame, n, 5000))
		return -1;
	return 0;
}

/* IPA's own signalling (CCM), answered as an IPA *client* does — libosmocore's
 * ipa_ccm_rcvmsg_bts_base, not the server side's ipa_ccm_rcvmsg_base: PING
 * gets PONG, ID_GET our unit name, ID_ACK nothing (a server acknowledges an
 * ID_ACK with one of its own; so would we, and the two would never stop). */
static void ccm(struct conn *c, const uint8_t *pl, size_t n)
{
	if (pl[0] == IPA_CCM_PING) {
		uint8_t f[4] = { 0, 1, IPA_PROTO_CCM, IPA_CCM_PONG };

		conn_send_frame(c, f, sizeof(f));
	} else if (pl[0] == IPA_CCM_ID_GET) {
		/* ID_RESP: per tag u16 length (tag + value), u8 tag, the value
		 * with its NUL (ipa_ccm_make_id_resp) */
		static const char name[] = SW_NAME;
		uint8_t f[4 + 3 + sizeof(name)];
		size_t l = 1 + 3 + sizeof(name);

		f[0] = (uint8_t)(l >> 8);
		f[1] = (uint8_t)l;
		f[2] = IPA_PROTO_CCM;
		f[3] = IPA_CCM_ID_RESP;
		f[4] = 0;
		f[5] = 1 + sizeof(name);
		f[6] = IPA_IDTAG_UNITNAME;
		memcpy(f + 7, name, sizeof(name));
		conn_send_frame(c, f, sizeof(f));
	}
	(void)n;
}

/* the next RSPRO PDU from what has been received: 1 one, 0 need more, -1
 * the stream is broken. IPA's own signalling is answered here. */
static int conn_take(struct conn *c, struct rspro_pdu *p, const char *who)
{
	for (;;) {
		const uint8_t *pl;
		size_t used, pln;
		int proto, ext, r;

		r = rspro_ipa_parse(c->buf, c->len, &used, &proto, &ext, &pl, &pln);
		if (r <= 0)
			return r;
		if (proto == IPA_PROTO_CCM && pln >= 1) {
			ccm(c, pl, pln);
			r = 0;
		} else if (proto == IPA_PROTO_OSMO && ext == IPA_EXT_RSPRO) {
			int d = rspro_decode(pl, pln, p);

			r = d ? -2 : 1;
			if (d == RSPRO_E_VERSION)
				log_warn("rspro: %s speaks RSPRO version %u, not %d — its %s skipped", who, p->version,
					 RSPRO_VERSION, rspro_msg_name(p->msg));
			else if (d)
				log_warn("rspro: %s sent a PDU that does not decode (%zu bytes), skipped", who, pln);
		} else {
			r = 0;
		}
		memmove(c->buf, c->buf + used, c->len - used);
		c->len -= used;
		if (r == 1)
			return 1;
	}
}

/* read what is there: >0 bytes, 0 the peer closed, -1 an error */
static int conn_fill(struct conn *c)
{
	ssize_t n;

	if (c->len == sizeof(c->buf))
		return -1;
	n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len, 0);
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return 1;
	if (n <= 0)
		return n < 0 ? -1 : 0;
	c->len += (size_t)n;
	return (int)n;
}

/* frame: a buffer whose message the encoder wrote at frame + 4 */
static int send_pdu(struct conn *c, uint8_t *frame, size_t msg_len)
{
	if (!msg_len)
		return -1;
	return conn_send_frame(c, frame, rspro_ipa_wrap(frame, msg_len));
}

static void hello(struct rp *rp, struct conn *c)
{
	uint8_t f[512];

	send_pdu(c, f, rspro_enc_connect_client_req(f + 4, sizeof(f) - 4, rp->tag++,
							&rp->cs, SW_NAME, SW_NAME, SW_VERSION));
}

static void answer(struct conn *c, int msg, uint32_t tag, int result)
{
	uint8_t f[64];

	send_pdu(c, f, rspro_enc_result_res(f + 4, sizeof(f) - 4, msg, tag, result));
}

static void slot_status(struct rp *rp, bool rst, bool vcc)
{
	uint8_t f[128];

	if (!rp->bank_ready)
		return;
	send_pdu(&rp->bank, f, rspro_enc_client_slot_status_ind(f + 4, sizeof(f) - 4, rp->tag++,
								      &rp->cs, &rp->bs, rst, vcc, vcc, true));
}

/* ---- the bankd ----------------------------------------------------------- */

static void bank_drop(struct rp *rp, const char *why)
{
	if (rp->bank.fd >= 0)
		log_notice("rspro: bankd %s port %u: %s", rp->bank_ip, rp->bank_port, why);
	conn_close(&rp->bank);
	rp->bank_ready = false;
	rp->have_atr = false;
	rp->atr_fresh = false;
	rp->atr_len = 0;
}

static void bank_failed(struct rp *rp, const char *why)
{
	log_warn("rspro: bankd %s port %u: %s", rp->bank_ip, rp->bank_port, why);
	snprintf(rp->be.detail, sizeof(rp->be.detail), "bankd %.46s port %u: %.80s", rp->bank_ip, rp->bank_port, why);
	conn_close(&rp->bank);
	rp->bank_retry_at = now_ms() + BANK_RETRY_MS;
}

static void bank_up(struct rp *rp)
{
	rp->bank.connecting = false;
	log_notice("rspro: bank %u slot %u, bankd %s port %u", rp->bs.id, rp->bs.nr,
		   rp->bank_ip, rp->bank_port);
	hello(rp, &rp->bank);
}

/* Towards the bankd without waiting: the connect runs on while pump() polls
 * (a bankd that drops SYNs must not stall the helper — its stdin, the
 * server's PINGs — for the connect's timeout on every retry). Its address
 * is the server's, numeric: no name lookup. */
static void bank_connect(struct rp *rp)
{
	struct sockaddr_storage sa;
	socklen_t sl;
	int fd;

	if (!rp->have_target || rp->bank.fd >= 0 || now_ms() < rp->bank_retry_at)
		return;
	memset(&sa, 0, sizeof(sa));
	if (inet_pton(AF_INET, rp->bank_ip, &((struct sockaddr_in *)&sa)->sin_addr) == 1) {
		((struct sockaddr_in *)&sa)->sin_family = AF_INET;
		((struct sockaddr_in *)&sa)->sin_port = htons(rp->bank_port);
		sl = sizeof(struct sockaddr_in);
	} else if (inet_pton(AF_INET6, rp->bank_ip, &((struct sockaddr_in6 *)&sa)->sin6_addr) == 1) {
		((struct sockaddr_in6 *)&sa)->sin6_family = AF_INET6;
		((struct sockaddr_in6 *)&sa)->sin6_port = htons(rp->bank_port);
		sl = sizeof(struct sockaddr_in6);
	} else {
		bank_failed(rp, "not an address");
		return;
	}
	fd = socket(sa.ss_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		bank_failed(rp, strerror(errno));
		return;
	}
	rp->bank.fd = fd;
	rp->bank.len = 0;
	if (!connect(fd, (struct sockaddr *)&sa, sl)) {
		bank_up(rp);
	} else if (errno == EINPROGRESS) {
		rp->bank.connecting = true;
		rp->bank.connect_deadline = now_ms() + CONNECT_TIMEOUT_MS;
	} else {
		bank_failed(rp, strerror(errno));
	}
}

/* the connect under way: done, failed, or still waiting (writable says) */
static void bank_connect_check(struct rp *rp, bool writable)
{
	int err = 0;
	socklen_t el = sizeof(err);

	if (writable) {
		if (getsockopt(rp->bank.fd, SOL_SOCKET, SO_ERROR, &err, &el) || err)
			bank_failed(rp, strerror(err ? err : errno));
		else
			bank_up(rp);
	} else if (now_ms() >= rp->bank.connect_deadline) {
		bank_failed(rp, strerror(ETIMEDOUT));
	}
}

/* ---- what arrives -------------------------------------------------------- */

static void on_server(struct rp *rp, const struct rspro_pdu *p)
{
	switch (p->msg) {
	case RSPRO_CONNECT_CLIENT_RES:
		/* the decoder has made sure there is one: an absent result
		 * read as OK would take a refusal for an acceptance */
		rp->srv_result = p->result;
		snprintf(rp->srv_name, sizeof(rp->srv_name), "%s", p->comp_name);
		snprintf(rp->srv_sw, sizeof(rp->srv_sw), "%s", p->comp_software);
		snprintf(rp->srv_ver, sizeof(rp->srv_ver), "%s", p->comp_version);
		break;
	case RSPRO_CONFIG_CLIENT_ID_REQ:
		/* the server may assign the client slot itself */
		if (p->has_client && (p->client.id != rp->cs.id || p->client.nr != rp->cs.nr)) {
			log_notice("rspro: the server makes us client %u slot %u", p->client.id, p->client.nr);
			rp->cs = p->client;
		}
		answer(&rp->srv, RSPRO_CONFIG_CLIENT_ID_RES, p->tag, RSPRO_RES_OK);
		break;
	case RSPRO_CONFIG_CLIENT_BANK_REQ: {
		char ip[INET6_ADDRSTRLEN] = "";
		bool none;

		if (p->ip_len == 4 || p->ip_len == 16)
			inet_ntop(p->ip_len == 4 ? AF_INET : AF_INET6, p->ip, ip, sizeof(ip));
		/* no address: the mapping is gone (remsim-server sends the
		 * all-zero one when it is removed) */
		none = !p->has_bank || !p->port || !ip[0] ||
		       !strcmp(ip, "0.0.0.0") || !strcmp(ip, "::");
		answer(&rp->srv, RSPRO_CONFIG_CLIENT_BANK_RES, p->tag, RSPRO_RES_OK);
		if (none) {
			bank_drop(rp, "the server removed the mapping");
			rp->have_target = false;
			if (rp->want_bank) {
				rp->lost_map = true;
				rp->remap_at = now_ms();
			}
			break;
		}
		if (rp->have_target && !strcmp(ip, rp->bank_ip) && p->port == rp->bank_port &&
		    p->bank.id == rp->bs.id && p->bank.nr == rp->bs.nr && rp->bank.fd >= 0)
			break;
		bank_drop(rp, "the server moved us to another bank slot");
		rp->have_target = true;
		rp->bs = p->bank;
		snprintf(rp->bank_ip, sizeof(rp->bank_ip), "%s", ip);
		rp->bank_port = p->port;
		rp->bank_retry_at = 0;
		bank_connect(rp);
		break;
	}
	case RSPRO_RESET_STATE_REQ:
		bank_drop(rp, "the server reset our state");
		rp->have_target = false;
		answer(&rp->srv, RSPRO_RESET_STATE_RES, p->tag, RSPRO_RES_OK);
		break;
	case RSPRO_ERROR_IND:
		log_warn("rspro: server error %d (severity %d)%s%s", p->err_code, p->err_severity,
			 p->err_string[0] ? ": " : "", p->err_string);
		break;
	default:
		log_dbg("rspro: server sent %s, not acted on", rspro_msg_name(p->msg));
	}
}

static void on_bank(struct rp *rp, const struct rspro_pdu *p)
{
	switch (p->msg) {
	case RSPRO_CONNECT_CLIENT_RES:
		if (p->result != RSPRO_RES_OK) {
			log_warn("rspro: the bankd refused us: %s", rspro_result_name(p->result));
			snprintf(rp->be.detail, sizeof(rp->be.detail), "bankd refused client %u:%u: %s",
				 rp->cs.id, rp->cs.nr, rspro_result_name(p->result));
			bank_drop(rp, "refused");
			rp->bank_retry_at = now_ms() + BANK_RETRY_MS;
			break;
		}
		rp->bank_ready = true;
		break;
	case RSPRO_SET_ATR_REQ:
		if (!p->data_len || p->data_len > ATR_MAX) {
			log_warn("rspro: the bankd sent an ATR of %zu bytes, not taken", p->data_len);
			answer(&rp->bank, RSPRO_SET_ATR_RES, p->tag, RSPRO_RES_CARD_UNRESPONSIVE);
			break;
		}
		memcpy(rp->atr, p->data, p->data_len);
		rp->atr_len = p->data_len;
		rp->have_atr = true;
		rp->atr_fresh = true;
		rp->atr_seq++;
		log_dbg("rspro: ATR from the bankd, %zu bytes", p->data_len);
		answer(&rp->bank, RSPRO_SET_ATR_RES, p->tag, RSPRO_RES_OK);
		break;
	case RSPRO_TPDU_CARD_TO_MODEM:
		if (!rp->waiting) {
			log_warn("rspro: an answer from the card nobody asked for, dropped");
			break;
		}
		memcpy(rp->resp, p->data, p->data_len);
		rp->resp_len = p->data_len;
		rp->got_resp = true;
		rp->waiting = false;
		break;
	case RSPRO_ERROR_IND:
		log_warn("rspro: bankd error %d (severity %d)%s%s", p->err_code, p->err_severity,
			 p->err_string[0] ? ": " : "", p->err_string);
		break;
	default:
		log_dbg("rspro: bankd sent %s, not acted on", rspro_msg_name(p->msg));
	}
}

/* Receive and act until done(rp) or timeout_ms have passed (0: what is
 * there now): 1 done, 0 not (yet), -1 the server is gone. */
static int pump(struct rp *rp, int timeout_ms, bool (*done)(struct rp *))
{
	long deadline = now_ms() + timeout_ms;
	bool last = false;

	for (;;) {
		struct rspro_pdu p;
		struct pollfd pfd[2];
		int n = 0, r;
		long rem;

		while ((r = conn_take(&rp->srv, &p, "the server")) == 1)
			on_server(rp, &p);
		if (r < 0) {
			log_err("rspro: the server's stream is broken");
			conn_close(&rp->srv);
			rp->be.ended = true;
		}
		if (rp->bank.fd >= 0 && !rp->bank.connecting) {
			while ((r = conn_take(&rp->bank, &p, "the bankd")) == 1)
				on_bank(rp, &p);
			if (r < 0)
				bank_drop(rp, "broken stream");
		}
		if (rp->be.ended)
			return -1;
		if (done && done(rp))
			return 1;
		rem = deadline - now_ms();
		if (rem <= 0) {
			if (last)
				return 0;
			last = true;
			rem = 0;
		}
		pfd[n].fd = rp->srv.fd;
		pfd[n++].events = POLLIN;
		if (rp->bank.fd >= 0) {
			pfd[n].fd = rp->bank.fd;
			pfd[n++].events = rp->bank.connecting ? POLLOUT : POLLIN;
		}
		r = poll(pfd, (nfds_t)n, (int)(rem > 1000 ? 1000 : rem));
		if (r < 0 && errno != EINTR)
			return -1;
		if (r > 0 && pfd[0].revents && conn_fill(&rp->srv) <= 0) {
			log_err("rspro: the server %s closed the connection", rp->host);
			conn_close(&rp->srv);
			rp->be.ended = true;
			return -1;
		}
		if (n > 1 && rp->bank.connecting)
			bank_connect_check(rp, r > 0 && pfd[1].revents);
		else if (r > 0 && n > 1 && pfd[1].revents && conn_fill(&rp->bank) <= 0)
			bank_drop(rp, "closed the connection");
		/* a bankd that was down gets another try while we wait */
		bank_connect(rp);
	}
}

static bool srv_answered(struct rp *rp)
{
	return rp->srv_result >= 0;
}

static bool card_ready(struct rp *rp)
{
	return rp->bank_ready && rp->have_atr;
}

static bool resp_in(struct rp *rp)
{
	return rp->got_resp || !rp->bank_ready;
}

static bool atr_new(struct rp *rp)
{
	return rp->atr_seq != rp->atr_seq_seen || !rp->bank_ready;
}

/* ---- REST (remsim-server's rest_api.c) ----------------------------------- */

/* one HTTP/1.0 exchange: the status, -1 no answer; the body into out */
static int http(const char *host, uint16_t port, const char *method, const char *path,
		const char *body, char *out, size_t cap, char *why, size_t whylen)
{
	char req[1024];
	size_t len = 0;
	int fd, status = -1, n;
	long deadline = now_ms() + HTTP_TIMEOUT_MS;
	char *hdr_end;
	bool v6 = strchr(host, ':') != NULL;

	fd = tcp_connect(host, port, HTTP_TIMEOUT_MS, why, whylen);
	if (fd < 0)
		return -1;
	/* HTTP/1.0 and Connection: close: the body ends where the stream
	 * does, no chunking to undo */
	n = snprintf(req, sizeof(req),
		     "%s %s HTTP/1.0\r\nHost: %s%s%s:%u\r\nAccept: application/json\r\n"
		     "Connection: close\r\n",
		     method, path, v6 ? "[" : "", host, v6 ? "]" : "", port);
	if (n > 0 && (size_t)n < sizeof(req))
		n += body ? snprintf(req + n, sizeof(req) - (size_t)n,
				     "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
				     strlen(body), body)
			  : snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");
	if (n < 0 || (size_t)n >= sizeof(req) || send_all(fd, (const uint8_t *)req, (size_t)n, HTTP_TIMEOUT_MS)) {
		snprintf(why, whylen, "%s port %u: request not sent", host, port);
		close(fd);
		return -1;
	}
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		long rem = deadline - now_ms();
		ssize_t r;

		if (rem <= 0 || poll(&pfd, 1, (int)rem) != 1) {
			snprintf(why, whylen, "%s port %u: no answer", host, port);
			close(fd);
			return -1;
		}
		r = recv(fd, out + len, cap - 1 - len, 0);
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (r <= 0)
			break;
		len += (size_t)r;
		if (len == cap - 1) {
			snprintf(why, whylen, "%s port %u: an answer of more than %zu bytes", host, port, cap - 1);
			close(fd);
			return -1;
		}
	}
	close(fd);
	out[len] = '\0';
	if (sscanf(out, "HTTP/%*d.%*d %d", &status) != 1) {
		snprintf(why, whylen, "%s port %u: not an HTTP answer", host, port);
		return -1;
	}
	hdr_end = strstr(out, "\r\n\r\n");
	if (hdr_end)
		memmove(out, hdr_end + 4, strlen(hdr_end + 4) + 1);
	else
		out[0] = '\0';
	return status;
}

struct slotmap {
	struct rspro_slot bank, client;
	char state[32];
};

/* A JSON answer of the REST API: GET path, 200 expected, parsed into
 * tokens sized for it (every token takes at least two characters, a comma
 * or a quote included). The body and tokens are the caller's to free; the
 * token count, -1 with why. */
static int rest_get(const char *host, uint16_t port, const char *path, char **body,
		    struct jtok **t, char *why, size_t whylen)
{
	size_t max;
	int st, n;

	*t = NULL;
	*body = malloc(HTTP_MAX);
	if (!*body) {
		snprintf(why, whylen, "out of memory");
		return -1;
	}
	st = http(host, port, "GET", path, NULL, *body, HTTP_MAX, why, whylen);
	if (st != 200) {
		if (st > 0)
			snprintf(why, whylen, "REST %s: HTTP %d", path, st);
		return -1;
	}
	max = strlen(*body) / 2 + 2;
	*t = malloc(max * sizeof(**t));
	n = *t ? jtok_parse(*body, strlen(*body), *t, (int)max) : -1;
	if (n <= 0)
		snprintf(why, whylen, "REST %s: not JSON", path);
	return n;
}

/* GET /api/backend/v1/slotmaps: {"slotmaps":[{"bank":{"bankId","slotNr"},
 * "client":{"clientId","slotNr"},"state"}]} into *out (the caller frees);
 * the count, -1 on failure */
static int get_slotmaps(const char *host, uint16_t port, struct slotmap **out,
			char *why, size_t whylen)
{
	char *body;
	struct jtok *t;
	int n, arr, i, k = -1;

	*out = NULL;
	n = rest_get(host, port, "/api/backend/v1/slotmaps", &body, &t, why, whylen);
	arr = n > 0 ? jtok_get(body, t, n, 0, "slotmaps") : -1;
	if (n > 0 && (arr < 0 || t[arr].type != JT_ARR))
		snprintf(why, whylen, "REST slotmaps: not the expected JSON");
	else if (n > 0 && (*out = calloc((size_t)t[arr].size + 1, sizeof(**out)))) {
		struct slotmap *m = *out;
		int e = arr + 1;

		/* the elements one after the other, each skipped past as a whole */
		for (i = 0, k = 0; i < t[arr].size; i++, e = jtok_skip(t, n, e)) {
			int b = jtok_get(body, t, n, e, "bank"), c = jtok_get(body, t, n, e, "client");

			m[k].bank.id = (uint16_t)jtok_long(body, t, jtok_get(body, t, n, b, "bankId"), 0);
			m[k].bank.nr = (uint16_t)jtok_long(body, t, jtok_get(body, t, n, b, "slotNr"), 0);
			m[k].client.id = (uint16_t)jtok_long(body, t, jtok_get(body, t, n, c, "clientId"), 0);
			m[k].client.nr = (uint16_t)jtok_long(body, t, jtok_get(body, t, n, c, "slotNr"), 0);
			jtok_str(body, t, jtok_get(body, t, n, e, "state"), m[k].state, sizeof(m[k].state));
			k++;
		}
	}
	free(t);
	free(body);
	return k;
}

/* Map the bank slot the spec names to our client slot. One of the slot to
 * another client is theirs: refused. One to OUR client slot is taken as
 * ours and removed at the end like one made now: the client slot is this
 * reader's identity (the server refuses a second connection with it), and a
 * mapping left by a run that could not clean up (killed, power lost) would
 * otherwise hold the slot for good. */
static int rest_map(struct rp *rp)
{
	static char body[4096];
	char req[200], why[RSIM_DETAIL_MAX] = "";
	struct slotmap *m;
	int st, n, i, r = -1;

	snprintf(req, sizeof(req),
		 "{\"bank\":{\"bankId\":%u,\"slotNr\":%u},\"client\":{\"clientId\":%u,\"slotNr\":%u}}",
		 rp->want.id, rp->want.nr, rp->cs.id, rp->cs.nr);
	st = http(rp->host, rp->rest_port, "POST", "/api/backend/v1/slotmaps", req, body, sizeof(body),
		  why, sizeof(why));
	if (st >= 200 && st < 300) {
		rp->made_map = true;
		log_notice("rspro: mapped bank %u slot %u to client %u slot %u", rp->want.id, rp->want.nr,
			   rp->cs.id, rp->cs.nr);
		return 0;
	}
	if (st < 0) {
		log_err("rspro: REST %s", why);
		return -1;
	}
	/* refused: already mapped (to us, or to someone else)? */
	n = get_slotmaps(rp->host, rp->rest_port, &m, why, sizeof(why));
	for (i = 0; i < n; i++) {
		if (m[i].bank.id != rp->want.id || m[i].bank.nr != rp->want.nr)
			continue;
		if (m[i].client.id == rp->cs.id && m[i].client.nr == rp->cs.nr) {
			log_notice("rspro: bank %u slot %u is mapped to us already — ours, removed at the end",
				   rp->want.id, rp->want.nr);
			rp->made_map = true;
			rp->adopted = true;
			r = 0;
		} else {
			log_err("rspro: bank %u slot %u is in use by client %u slot %u", rp->want.id, rp->want.nr,
				m[i].client.id, m[i].client.nr);
		}
		break;
	}
	if (n >= 0 && i == n)
		log_err("rspro: the server did not map bank %u slot %u: HTTP %d %s", rp->want.id, rp->want.nr,
			st, body);
	else if (n < 0)
		log_err("rspro: the server did not map bank %u slot %u (HTTP %d), and %s", rp->want.id, rp->want.nr,
			st, why);
	free(m);
	return r;
}

static void rest_unmap(struct rp *rp)
{
	static char body[4096];
	char path[80], why[RSIM_DETAIL_MAX];
	int st;

	/* a mapping's id in remsim-server is its bank slot: bank << 16 | slot */
	snprintf(path, sizeof(path), "/api/backend/v1/slotmaps/%lu",
		 ((unsigned long)rp->want.id << 16) | rp->want.nr);
	st = http(rp->host, rp->rest_port, "DELETE", path, NULL, body, sizeof(body), why, sizeof(why));
	if (st < 200 || st >= 300)
		log_warn("rspro: bank %u slot %u stays mapped: %s", rp->want.id, rp->want.nr,
			 st < 0 ? why : "the server refused");
	else
		log_notice("rspro: bank %u slot %u unmapped", rp->want.id, rp->want.nr);
}

/* ---- the backend --------------------------------------------------------- */

static struct rp *RP(struct rsim_backend *be)
{
	return (struct rp *)be;
}

/* the card's ATR once a bank slot, its bankd and its card are there */
static int wait_card(struct rp *rp)
{
	int r;

	bank_connect(rp);
	r = pump(rp, ATR_WAIT_MS, card_ready);
	if (r == 1)
		return RSIM_OK;
	if (r < 0) {
		snprintf(rp->be.detail, sizeof(rp->be.detail), "the remsim-server is gone");
		return RSIM_E_IO;
	}
	if (!rp->have_target) {
		snprintf(rp->be.detail, sizeof(rp->be.detail),
			 "no bank slot mapped to client %u slot %u", rp->cs.id, rp->cs.nr);
		return RSIM_E_NO_CARD;
	}
	if (rp->bank.fd < 0 || rp->bank.connecting) {
		if (!rp->be.detail[0] || rp->bank.connecting)
			snprintf(rp->be.detail, sizeof(rp->be.detail), "bankd %s port %u unreachable",
				 rp->bank_ip, rp->bank_port);
		return RSIM_E_IO;
	}
	snprintf(rp->be.detail, sizeof(rp->be.detail), "bank %u slot %u: no card (no ATR from the bankd)",
		 rp->bs.id, rp->bs.nr);
	return RSIM_E_NO_CARD;
}

/* the card through a reset as the modem signals it: RST active, then
 * released; the bankd resets the card. It may send the new ATR — then that
 * is the answer, else the one it sent before. */
static int signal_reset(struct rp *rp, uint8_t *atr, size_t *atr_len)
{
	slot_status(rp, true, true);
	slot_status(rp, false, true);
	rp->atr_seq_seen = rp->atr_seq;
	pump(rp, RESET_ATR_WAIT_MS, atr_new);
	/* whatever came in the wait answers this reset, it is no cold start
	 * for a later power-up to skip */
	rp->atr_fresh = false;
	if (!card_ready(rp)) {
		snprintf(rp->be.detail, sizeof(rp->be.detail), "the bankd went away in the reset");
		return RSIM_E_NO_CARD;
	}
	memcpy(atr, rp->atr, rp->atr_len);
	*atr_len = rp->atr_len;
	return RSIM_OK;
}

static int rp_power_up(struct rsim_backend *be, uint8_t *atr, size_t *atr_len)
{
	struct rp *rp = RP(be);
	int r = wait_card(rp);

	if (r)
		return r;
	/* the bankd has just brought the card up and sent its ATR: that is a
	 * cold start already, a reset pulse on top would only repeat it */
	if (rp->atr_fresh) {
		rp->atr_fresh = false;
		memcpy(atr, rp->atr, rp->atr_len);
		*atr_len = rp->atr_len;
		return RSIM_OK;
	}
	return signal_reset(rp, atr, atr_len);
}

static int rp_reset(struct rsim_backend *be, uint8_t *atr, size_t *atr_len)
{
	struct rp *rp = RP(be);

	if (!card_ready(rp))
		return rp_power_up(be, atr, atr_len);
	return signal_reset(rp, atr, atr_len);
}

static int rp_power_down(struct rsim_backend *be)
{
	/* the card held in reset, no VCC, no clock; the next power-up signals
	 * a cold start whatever ATR the bankd sent before */
	slot_status(RP(be), true, false);
	RP(be)->atr_fresh = false;
	return RSIM_OK;
}

static int rp_transmit(struct rsim_backend *be, const uint8_t *tpdu, size_t len,
		       uint8_t *resp, size_t *resp_len)
{
	struct rp *rp = RP(be);
	uint8_t f[RSIM_TPDU_MAX + 128];
	int r;

	if (!card_ready(rp)) {
		snprintf(be->detail, sizeof(be->detail), "no card from the bank");
		return RSIM_E_NO_CARD;
	}
	rp->got_resp = false;
	rp->waiting = true;
	if (send_pdu(&rp->bank, f, rspro_enc_tpdu_modem_to_card(f + 4, sizeof(f) - 4, rp->tag++,
								    &rp->cs, &rp->bs, tpdu, len))) {
		rp->waiting = false;
		bank_drop(rp, "send failed");
		return RSIM_E_NO_CARD;
	}
	r = pump(rp, TPDU_TIMEOUT_MS, resp_in);
	rp->waiting = false;
	if (r < 0)
		return RSIM_E_IO;
	if (!rp->bank_ready)
		return RSIM_E_NO_CARD;
	if (!rp->got_resp) {
		snprintf(be->detail, sizeof(be->detail), "no answer from the bank in %d s", TPDU_TIMEOUT_MS / 1000);
		/* a TpduCardToModem names no command: a late one would be taken
		 * for the next command's answer. A fresh bankd link has none in
		 * flight (and gives the card a new ATR). */
		bank_drop(rp, "no answer in time — reconnecting, so a late one cannot answer the next command");
		rp->bank_retry_at = 0;
		return RSIM_E_TIMEOUT;
	}
	if (rp->resp_len < 2 || rp->resp_len > RSIM_RESP_MAX) {
		snprintf(be->detail, sizeof(be->detail), "an answer of %zu bytes", rp->resp_len);
		return RSIM_E_PROTOCOL;
	}
	memcpy(resp, rp->resp, rp->resp_len);
	*resp_len = rp->resp_len;
	return RSIM_OK;
}

/* polled every 500 ms between requests: the server's mapping changes and
 * the bankd's ATR arrive here, and turn into inserted/removed */
static int rp_present(struct rsim_backend *be)
{
	struct rp *rp = RP(be);

	bank_connect(rp);
	pump(rp, 0, NULL);
	/* the slot the spec names was taken away (by the operator, or by a
	 * helper before this one that unmapped on its way out): take it again */
	if (rp->lost_map && !rp->have_target && now_ms() >= rp->remap_at) {
		rp->remap_at = now_ms() + REMAP_MS;
		if (!rest_map(rp))
			rp->lost_map = false;
	}
	return card_ready(rp) ? 1 : 0;
}

static void rp_info(struct rsim_backend *be, struct jw *w)
{
	struct rp *rp = RP(be);
	char s[300];

	snprintf(s, sizeof(s), "%s:%u", rp->host, rp->port);
	jw_str(w, "server", s);
	jw_opt(w, "server_name", rp->srv_name);
	jw_opt(w, "server_software", rp->srv_sw);
	jw_opt(w, "server_version", rp->srv_ver);
	snprintf(s, sizeof(s), "%u:%u", rp->cs.id, rp->cs.nr);
	jw_str(w, "client", s);
	if (rp->have_target) {
		snprintf(s, sizeof(s), "%u:%u", rp->bs.id, rp->bs.nr);
		jw_str(w, "bank", s);
		snprintf(s, sizeof(s), "%s:%u", rp->bank_ip, rp->bank_port);
		jw_str(w, "bankd", s);
	} else if (rp->want_bank) {
		snprintf(s, sizeof(s), "%u:%u", rp->want.id, rp->want.nr);
		jw_str(w, "bank", s);
	}
	/* who made the mapping: us (and we remove it), or the operator */
	if (rp->want_bank)
		jw_str(w, "mapping", rp->made_map ? "helper" : "server");
}

static void rp_close(struct rsim_backend *be)
{
	struct rp *rp = RP(be);

	conn_close(&rp->bank);
	conn_close(&rp->srv);
	if (rp->made_map)
		rest_unmap(rp);
	free(rp);
}

static const struct rsim_backend_ops rspro_ops = {
	.name = "rspro",
	.power_up = rp_power_up,
	.reset = rp_reset,
	.power_down = rp_power_down,
	.transmit = rp_transmit,
	.present = rp_present,
	.close = rp_close,
	.info = rp_info,
};

/* <server>[:<port>][/<bank>:<slot>], the server a name, an IPv4 address or
 * [an IPv6 address]; 0 parsed */
static int parse_spec(const char *spec, char *host, size_t hostlen, uint16_t *port,
		      bool *has_bank, struct rspro_slot *bank)
{
	const char *p = spec, *h_end, *slash = strchr(spec, '/');
	unsigned long v, b, s;
	char *end;
	size_t hl;

	if (*p == '[') {
		h_end = strchr(p, ']');
		if (!h_end)
			return -1;
		p++;
		hl = (size_t)(h_end - p);
		h_end++;
	} else {
		h_end = p + strcspn(p, ":/");
		hl = (size_t)(h_end - p);
	}
	if (!hl || hl >= hostlen)
		return -1;
	memcpy(host, p, hl);
	host[hl] = '\0';
	p = h_end;
	*port = REMSIM_SERVER_PORT;
	if (*p == ':') {
		v = strtoul(p + 1, &end, 10);
		if (end == p + 1 || !v || v > 65535 || (*end && *end != '/'))
			return -1;
		*port = (uint16_t)v;
		p = end;
	}
	*has_bank = false;
	if (*p == '/') {
		slash = p;
		b = strtoul(slash + 1, &end, 10);
		if (end == slash + 1 || *end != ':' || b > 65535)
			return -1;
		s = strtoul(end + 1, &end, 10);
		if (*end || s > 65535 || end[-1] == ':')
			return -1;
		bank->id = (uint16_t)b;
		bank->nr = (uint16_t)s;
		*has_bank = true;
	} else if (*p) {
		return -1;
	}
	return 0;
}

struct rsim_backend *rspro_open(const struct rspro_cfg *cfg)
{
	struct rp *rp = calloc(1, sizeof(*rp));
	char why[RSIM_DETAIL_MAX];
	int fd, r;

	if (!rp)
		return NULL;
	rp->bank.fd = rp->srv.fd = -1;
	rp->srv_result = -1;
	rp->cs.id = cfg->client_id;
	rp->cs.nr = cfg->client_slot;
	rp->rest_port = cfg->rest_port ? cfg->rest_port : REMSIM_REST_PORT;
	if (parse_spec(cfg->spec, rp->host, sizeof(rp->host), &rp->port, &rp->want_bank, &rp->want)) {
		log_err("rspro:%s: expected <server>[:<port>][/<bank>:<slot>]", cfg->spec);
		free(rp);
		return NULL;
	}
	snprintf(rp->reader, sizeof(rp->reader), "rspro:%s", cfg->spec);
	rp->be.ops = &rspro_ops;
	rp->be.reader = rp->reader;

	if (rp->want_bank && rest_map(rp)) {
		free(rp);
		return NULL;
	}
	fd = tcp_connect(rp->host, rp->port, CONNECT_TIMEOUT_MS, why, sizeof(why));
	if (fd < 0) {
		log_err("rspro: remsim-server %s", why);
		goto fail;
	}
	rp->srv.fd = fd;
	hello(rp, &rp->srv);
	r = pump(rp, HELLO_TIMEOUT_MS, srv_answered);
	if (r != 1) {
		log_err("rspro: the remsim-server %s did not answer our ConnectClientReq", rp->host);
		goto fail;
	}
	if (rp->srv_result != RSPRO_RES_OK) {
		log_err("rspro: the remsim-server refused client %u slot %u: %s", rp->cs.id, rp->cs.nr,
			rspro_result_name(rp->srv_result));
		goto fail;
	}
	log_notice("rspro: client %u slot %u at %s port %u (%s %s)", rp->cs.id, rp->cs.nr, rp->host,
		   rp->port, rp->srv_sw[0] ? rp->srv_sw : "remsim-server", rp->srv_ver);
	return &rp->be;
fail:
	conn_close(&rp->srv);
	/* a mapping we found and the server says a live session holds this
	 * client identity: it is that session's (a `wwandctl rsim test` next
	 * to the running reader), not ours to take away */
	if (rp->made_map && !(rp->adopted && rp->srv_result == RSPRO_RES_IDENTITY_IN_USE))
		rest_unmap(rp);
	free(rp);
	return NULL;
}

/* ---- --list -------------------------------------------------------------- */

int rspro_list(const char *server, uint16_t rest_port)
{
	char *body = NULL;
	struct jtok *t = NULL;
	struct slotmap *m = NULL;
	char host[256], why[RSIM_DETAIL_MAX] = "", spec[320];
	struct rspro_slot unused;
	uint16_t port;
	bool has_bank;
	int n, arr, i, e, nm, rows = 0;
	struct jw w;

	if (!rest_port)
		rest_port = REMSIM_REST_PORT;
	if (parse_spec(server, host, sizeof(host), &port, &has_bank, &unused) || has_bank) {
		snprintf(why, sizeof(why), "--rspro-server %s: expected <server>[:<port>]", server);
		goto done;
	}
	nm = get_slotmaps(host, rest_port, &m, why, sizeof(why));
	if (nm < 0)
		goto done;
	n = rest_get(host, rest_port, "/api/backend/v1/banks", &body, &t, why, sizeof(why));
	if (n <= 0)
		goto done;
	arr = jtok_get(body, t, n, 0, "banks");
	if (arr < 0 || t[arr].type != JT_ARR) {
		snprintf(why, sizeof(why), "REST banks: not the expected JSON");
		goto done;
	}
	/* {"banks":[{"peer","state","component_id":{"name",...},"bankId",
	 * "numberOfSlots"}]} — a row per slot, with its mapping when it has one */
	for (i = 0, e = arr + 1; i < t[arr].size; i++, e = jtok_skip(t, n, e)) {
		int cid = jtok_get(body, t, n, e, "component_id");
		long bank = jtok_long(body, t, jtok_get(body, t, n, e, "bankId"), -1);
		long slots = jtok_long(body, t, jtok_get(body, t, n, e, "numberOfSlots"), 0);
		char name[64], state[48], peer[80];
		long s;

		if (bank < 0 || bank > 65535 || slots < 0 || slots > 1024)
			continue;
		jtok_str(body, t, jtok_get(body, t, n, cid, "name"), name, sizeof(name));
		jtok_str(body, t, jtok_get(body, t, n, e, "state"), state, sizeof(state));
		jtok_str(body, t, jtok_get(body, t, n, e, "peer"), peer, sizeof(peer));
		for (s = 0; s < slots; s++) {
			char c[16];
			int k;

			snprintf(spec, sizeof(spec), "rspro:%s/%ld:%ld", server, bank, s);
			jw_begin(&w, stdout);
			jw_str(&w, "backend", "rspro");
			jw_str(&w, "spec", spec);
			jw_str(&w, "server", server);
			jw_int(&w, "bank", bank);
			jw_int(&w, "slot", s);
			jw_opt(&w, "name", name);
			jw_opt(&w, "bank_state", state);
			jw_opt(&w, "peer", peer);
			for (k = 0; k < nm; k++) {
				if (m[k].bank.id != bank || m[k].bank.nr != s)
					continue;
				snprintf(c, sizeof(c), "%u:%u", m[k].client.id, m[k].client.nr);
				jw_str(&w, "mapped_to", c);
				jw_opt(&w, "map_state", m[k].state);
				break;
			}
			jw_end(&w);
			rows++;
		}
	}
	why[0] = '\0';
done:
	jw_begin(&w, stdout);
	jw_bool(&w, "done", true);
	jw_str(&w, "backends", "rspro");
	jw_str(&w, "server", server);
	jw_int(&w, "slots", rows);
	jw_opt(&w, "error", why);
	jw_end(&w);
	free(m);
	free(t);
	free(body);
	return why[0] ? 1 : 0;
}
