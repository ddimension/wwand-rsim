/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winscard.h>

#include "atr.h"
#include "json.h"
#include "log.h"
#include "pcsc.h"

/*
 * PC/SC (pcsc-lite + a CCID driver). The reader driver runs the T=0
 * procedure-byte exchange itself, so a TPDU goes through SCardTransmit as
 * it is; a 61xx/6Cxx answer comes back unchanged because pcsc-lite only
 * chains GET RESPONSE for callers that ask for it, and we do not.
 */

struct pcsc {
	struct rsim_backend be;
	SCARDCONTEXT ctx;
	SCARDHANDLE card;
	bool connected;
	DWORD proto;
	char *reader;
};

static int map_err(struct pcsc *p, LONG rv, const char *what)
{
	snprintf(p->be.detail, sizeof(p->be.detail), "%s: %s", what,
		 pcsc_stringify_error(rv));
	switch (rv) {
	case SCARD_E_NO_SMARTCARD:
	case SCARD_W_REMOVED_CARD:
	case SCARD_W_UNPOWERED_CARD:
	/* no answer to reset is what the Phoenix backend calls no_card too */
	case SCARD_W_UNRESPONSIVE_CARD:
	case SCARD_E_UNKNOWN_READER:
	case SCARD_E_READER_UNAVAILABLE:
		return RSIM_E_NO_CARD;
	case SCARD_E_TIMEOUT:
		return RSIM_E_TIMEOUT;
	case SCARD_E_PROTO_MISMATCH:
	case SCARD_W_UNSUPPORTED_CARD:
		return RSIM_E_PROTOCOL;
	default:
		return RSIM_E_IO;
	}
}

static int read_atr(struct pcsc *p, uint8_t *atr, size_t *atr_len)
{
	BYTE buf[MAX_ATR_SIZE];
	DWORD len = sizeof(buf), state, proto, rlen = 0;
	LONG rv;

	rv = SCardStatus(p->card, NULL, &rlen, &state, &proto, buf, &len);
	if (rv != SCARD_S_SUCCESS)
		return map_err(p, rv, "SCardStatus");
	if (len > ATR_MAX)
		len = ATR_MAX;
	memcpy(atr, buf, len);
	*atr_len = len;
	log_dbg("%s: connected with T=%d", p->reader, proto == SCARD_PROTOCOL_T1 ? 1 : 0);
	return RSIM_OK;
}

static int pc_power_up(struct rsim_backend *be, uint8_t *atr, size_t *atr_len)
{
	struct pcsc *p = (struct pcsc *)be;
	LONG rv;

	if (p->connected) {
		/* already connected: a cold start is an unpower + reconnect */
		rv = SCardReconnect(p->card, SCARD_SHARE_EXCLUSIVE,
				    SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
				    SCARD_UNPOWER_CARD, &p->proto);
		if (rv != SCARD_S_SUCCESS)
			return map_err(p, rv, "SCardReconnect");
		return read_atr(p, atr, atr_len);
	}
	/* T=0 first: the modem sends T=0 TPDUs, and with T0|T1 pcsc-lite
	 * picks T=1 whenever the card offers both */
	rv = SCardConnect(p->ctx, p->reader, SCARD_SHARE_EXCLUSIVE,
			  SCARD_PROTOCOL_T0, &p->card, &p->proto);
	if (rv == SCARD_E_PROTO_MISMATCH)
		rv = SCardConnect(p->ctx, p->reader, SCARD_SHARE_EXCLUSIVE,
				  SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
				  &p->card, &p->proto);
	if (rv != SCARD_S_SUCCESS)
		return map_err(p, rv, "SCardConnect");
	p->connected = true;
	return read_atr(p, atr, atr_len);
}

static int pc_reset(struct rsim_backend *be, uint8_t *atr, size_t *atr_len)
{
	struct pcsc *p = (struct pcsc *)be;
	LONG rv;

	if (!p->connected)
		return pc_power_up(be, atr, atr_len);
	rv = SCardReconnect(p->card, SCARD_SHARE_EXCLUSIVE,
			    SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
			    SCARD_RESET_CARD, &p->proto);
	if (rv != SCARD_S_SUCCESS)
		return map_err(p, rv, "SCardReconnect");
	return read_atr(p, atr, atr_len);
}

static int pc_power_down(struct rsim_backend *be)
{
	struct pcsc *p = (struct pcsc *)be;
	LONG rv;

	if (!p->connected)
		return RSIM_OK;
	p->connected = false;
	rv = SCardDisconnect(p->card, SCARD_UNPOWER_CARD);
	/* a card pulled out is as powered down as it gets */
	if (rv != SCARD_S_SUCCESS && rv != SCARD_W_REMOVED_CARD &&
	    rv != SCARD_E_NO_SMARTCARD)
		return map_err(p, rv, "SCardDisconnect");
	return RSIM_OK;
}

static int pc_transmit(struct rsim_backend *be, const uint8_t *tpdu, size_t len,
		       uint8_t *resp, size_t *resp_len)
{
	struct pcsc *p = (struct pcsc *)be;
	BYTE buf[RSIM_RESP_MAX];
	DWORD rlen = sizeof(buf);
	LONG rv;

	if (!p->connected)
		return RSIM_E_NOT_POWERED;
	rv = SCardTransmit(p->card,
			   p->proto == SCARD_PROTOCOL_T1 ? SCARD_PCI_T1 : SCARD_PCI_T0,
			   tpdu, (DWORD)len, NULL, buf, &rlen);
	if (rv != SCARD_S_SUCCESS)
		return map_err(p, rv, "SCardTransmit");
	if (rlen < 2) {
		snprintf(be->detail, sizeof(be->detail), "response of %lu bytes", (unsigned long)rlen);
		return RSIM_E_PROTOCOL;
	}
	memcpy(resp, buf, rlen);
	*resp_len = rlen;
	return RSIM_OK;
}

static int pc_present(struct rsim_backend *be)
{
	struct pcsc *p = (struct pcsc *)be;
	SCARD_READERSTATE rs;
	LONG rv;

	memset(&rs, 0, sizeof(rs));
	rs.szReader = p->reader;
	rs.dwCurrentState = SCARD_STATE_UNAWARE;
	rv = SCardGetStatusChange(p->ctx, 0, &rs, 1);
	/* a reader unplugged has no card in it either */
	if (rv == SCARD_E_UNKNOWN_READER || rv == SCARD_E_READER_UNAVAILABLE)
		return 0;
	if (rv != SCARD_S_SUCCESS && rv != SCARD_E_TIMEOUT)
		return -1;
	return !!(rs.dwEventState & SCARD_STATE_PRESENT);
}

static void pc_close(struct rsim_backend *be)
{
	struct pcsc *p = (struct pcsc *)be;

	if (p->connected)
		SCardDisconnect(p->card, SCARD_UNPOWER_CARD);
	SCardReleaseContext(p->ctx);
	free(p->reader);
	free(p);
}

static const struct rsim_backend_ops pcsc_ops = {
	.name = "pcsc",
	.power_up = pc_power_up,
	.reset = pc_reset,
	.power_down = pc_power_down,
	.transmit = pc_transmit,
	.present = pc_present,
	.close = pc_close,
};

/* spec: all digits is an index into the reader list, anything else a
 * substring of the reader name (names carry serials and slot numbers the
 * operator should not have to spell out) */
static char *pick_reader(const char *list, DWORD len, const char *spec)
{
	const char *r;
	bool numeric = *spec != '\0';
	unsigned idx = 0, want;
	const char *s;

	for (s = spec; *s; s++)
		if (!isdigit((unsigned char)*s))
			numeric = false;
	want = numeric ? (unsigned)strtoul(spec, NULL, 10) : 0;
	for (r = list; r < list + len && *r; r += strlen(r) + 1, idx++) {
		log_dbg("pcsc reader %u: %s", idx, r);
		if (numeric ? idx == want : strstr(r, spec) != NULL)
			return strdup(r);
	}
	return NULL;
}

struct rsim_backend *pcsc_open(const char *spec)
{
	struct pcsc *p = calloc(1, sizeof(*p));
	char *list = NULL;
	DWORD len = 0;
	LONG rv;

	if (!p)
		return NULL;
	p->be.ops = &pcsc_ops;
	rv = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &p->ctx);
	if (rv != SCARD_S_SUCCESS) {
		log_err("pcsc: SCardEstablishContext: %s (is pcscd running?)",
			pcsc_stringify_error(rv));
		free(p);
		return NULL;
	}
	rv = SCardListReaders(p->ctx, NULL, NULL, &len);
	if (rv == SCARD_S_SUCCESS && len) {
		list = malloc(len);
		if (list)
			rv = SCardListReaders(p->ctx, NULL, list, &len);
	}
	if (rv != SCARD_S_SUCCESS || !list) {
		log_err("pcsc: no readers: %s", pcsc_stringify_error(rv));
		goto fail;
	}
	p->reader = pick_reader(list, len, spec);
	free(list);
	list = NULL;
	if (!p->reader) {
		log_err("pcsc: no reader matches '%s'", spec);
		goto fail;
	}
	p->be.reader = p->reader;
	log_notice("pcsc reader: %s", p->reader);
	return &p->be;

fail:
	free(list);
	SCardReleaseContext(p->ctx);
	free(p);
	return NULL;
}

int pcsc_list(void)
{
	SCARDCONTEXT ctx;
	char *list = NULL;
	DWORD len = 0;
	LONG rv;
	const char *r;
	unsigned idx = 0;

	rv = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &ctx);
	if (rv != SCARD_S_SUCCESS) {
		log_notice("pcsc: %s (is pcscd running?)", pcsc_stringify_error(rv));
		return -1;
	}
	rv = SCardListReaders(ctx, NULL, NULL, &len);
	if (rv == SCARD_S_SUCCESS && len && (list = malloc(len)))
		rv = SCardListReaders(ctx, NULL, list, &len);
	for (r = list; rv == SCARD_S_SUCCESS && r && r < list + len && *r; r += strlen(r) + 1, idx++) {
		SCARD_READERSTATE st;
		char spec[300];
		struct jw w;

		/* card or not, without touching it: the state pcscd already has */
		memset(&st, 0, sizeof(st));
		st.szReader = r;
		st.dwCurrentState = SCARD_STATE_UNAWARE;
		SCardGetStatusChange(ctx, 0, &st, 1);
		snprintf(spec, sizeof(spec), "pcsc:%s", r);
		jw_begin(&w, stdout);
		jw_str(&w, "backend", "pcsc");
		jw_str(&w, "spec", spec);
		jw_str(&w, "name", r);
		jw_bool(&w, "card", (st.dwEventState & SCARD_STATE_PRESENT) != 0);
		jw_end(&w);
	}
	free(list);
	SCardReleaseContext(ctx);
	return 0;
}
