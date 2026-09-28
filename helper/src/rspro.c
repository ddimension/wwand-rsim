/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * RSPRO messages of a remsim client, BER by hand (rspro.h). The module is
 * IMPLICIT TAGS: a [n] field replaces the type's own tag — except on a
 * CHOICE, which X.680 §31.2.7 always tags explicitly; that is why RsproPDU's
 * `msg [2]` wraps the alternative (itself [0]..[20] implicit) once more.
 *
 * Tags used: 0x02 INTEGER, 0x01 BOOLEAN (TRUE as 0xFF, as DER has it), 0x04
 * OCTET STRING, 0x0A ENUMERATED, 0x16 IA5String, 0x30 SEQUENCE; context
 * class 0x80 | n primitive, 0xA0 | n constructed. Every tag number here is
 * below 31, so a tag is one byte. Decoding skips what it does not know: the
 * module is EXTENSIBILITY IMPLIED, a newer peer may send more fields.
 */
#include <string.h>

#include "rspro.h"

/* ---- BER writer ---------------------------------------------------------- */

#define NEST_MAX 8

struct bw {
	uint8_t *b;
	size_t cap, len;
	bool bad;
	size_t open[NEST_MAX];	/* where each open element's content starts */
	int depth;
};

static void bw_raw(struct bw *w, const void *p, size_t n)
{
	if (w->bad || n > w->cap - w->len) {
		w->bad = true;
		return;
	}
	memcpy(w->b + w->len, p, n);
	w->len += n;
}

static void bw_byte(struct bw *w, uint8_t v)
{
	bw_raw(w, &v, 1);
}

/* the minimal length encoding of n (DER), into l; its size */
static size_t len_enc(size_t n, uint8_t l[4])
{
	if (n < 0x80) {
		l[0] = (uint8_t)n;
		return 1;
	}
	if (n < 0x100) {
		l[0] = 0x81;
		l[1] = (uint8_t)n;
		return 2;
	}
	l[0] = 0x82;
	l[1] = (uint8_t)(n >> 8);
	l[2] = (uint8_t)n;
	return 3;
}

static void bw_tlv(struct bw *w, uint8_t tag, const void *v, size_t n)
{
	uint8_t l[4];

	if (n > 0xffff) {
		w->bad = true;
		return;
	}
	bw_byte(w, tag);
	bw_raw(w, l, len_enc(n, l));
	bw_raw(w, v, n);
}

/* a constructed element: its content follows until bw_end, which then
 * inserts the length in front of it */
static void bw_begin(struct bw *w, uint8_t tag)
{
	bw_byte(w, tag);
	if (w->depth == NEST_MAX) {
		w->bad = true;
		return;
	}
	w->open[w->depth++] = w->len;
}

static void bw_end(struct bw *w)
{
	uint8_t l[4];
	size_t start, n, ln;

	if (w->bad || !w->depth) {
		w->bad = true;
		return;
	}
	start = w->open[--w->depth];
	n = w->len - start;
	ln = len_enc(n, l);
	if (n > 0xffff || ln > w->cap - w->len) {
		w->bad = true;
		return;
	}
	memmove(w->b + start + ln, w->b + start, n);
	memcpy(w->b + start, l, ln);
	w->len += ln;
}

/* an unsigned value as a (signed, minimal) INTEGER or ENUMERATED content */
static void bw_uint(struct bw *w, uint8_t tag, uint32_t v)
{
	uint8_t c[5];
	size_t n = 0;
	int i;

	for (i = 3; i > 0 && !(v >> (8 * i)); i--)
		;
	/* a leading 0 keeps a value with the top bit set positive */
	if ((v >> (8 * i)) & 0x80)
		c[n++] = 0;
	for (; i >= 0; i--)
		c[n++] = (uint8_t)(v >> (8 * i));
	bw_tlv(w, tag, c, n);
}

static void bw_bool(struct bw *w, uint8_t tag, bool v)
{
	uint8_t c = v ? 0xff : 0x00;

	bw_tlv(w, tag, &c, 1);
}

static void bw_str(struct bw *w, uint8_t tag, const char *s)
{
	size_t n = strlen(s);

	/* ComponentName is SIZE (1..32) */
	if (!n)
		s = "-", n = 1;
	bw_tlv(w, tag, s, n > 32 ? 32 : n);
}

static void bw_slot(struct bw *w, const struct rspro_slot *s)
{
	bw_begin(w, 0x30);
	bw_uint(w, 0x02, s->id);
	bw_uint(w, 0x02, s->nr);
	bw_end(w);
}

/* RsproPDU { version [0], tag [1], msg [2] { <alternative [msg]> ... } };
 * the caller writes the alternative's fields, pdu_end closes all three */
static void pdu_begin(struct bw *w, uint8_t *out, size_t cap, uint32_t tag, int msg)
{
	memset(w, 0, sizeof(*w));
	w->b = out;
	w->cap = cap;
	bw_begin(w, 0x30);
	bw_uint(w, 0x80, RSPRO_VERSION);
	bw_uint(w, 0x81, tag);
	bw_begin(w, 0xA2);
	bw_begin(w, (uint8_t)(0xA0 | msg));
}

static size_t pdu_end(struct bw *w)
{
	bw_end(w);
	bw_end(w);
	bw_end(w);
	return (w->bad || w->depth) ? 0 : w->len;
}

size_t rspro_enc_connect_client_req(uint8_t *out, size_t cap, uint32_t tag,
				    const struct rspro_slot *client,
				    const char *name, const char *software,
				    const char *version)
{
	struct bw w;

	pdu_begin(&w, out, cap, tag, RSPRO_CONNECT_CLIENT_REQ);
	/* identity ComponentIdentity { type, name, software [0], swVersion [1] } */
	bw_begin(&w, 0x30);
	bw_uint(&w, 0x0A, RSPRO_COMP_CLIENT);
	bw_str(&w, 0x16, name);
	bw_str(&w, 0x80, software);
	bw_str(&w, 0x81, version);
	bw_end(&w);
	/* clientSlot: optional towards the server, mandatory towards a bankd;
	 * osmo-remsim's client sends it to both */
	bw_slot(&w, client);
	return pdu_end(&w);
}

size_t rspro_enc_result_res(uint8_t *out, size_t cap, int msg, uint32_t tag,
			    int result)
{
	struct bw w;

	pdu_begin(&w, out, cap, tag, msg);
	bw_uint(&w, 0x0A, (uint32_t)result);
	return pdu_end(&w);
}

size_t rspro_enc_tpdu_modem_to_card(uint8_t *out, size_t cap, uint32_t tag,
				    const struct rspro_slot *client,
				    const struct rspro_slot *bank,
				    const uint8_t *data, size_t len)
{
	struct bw w;

	pdu_begin(&w, out, cap, tag, RSPRO_TPDU_MODEM_TO_CARD);
	bw_slot(&w, client);
	bw_slot(&w, bank);
	/* TpduFlags: the whole command (header and data) in one message, the
	 * way remsim-client sends one it has complete — the bankd hands it to
	 * SCardTransmit as it is */
	bw_begin(&w, 0x30);
	bw_bool(&w, 0x01, true);	/* tpduHeaderPresent */
	bw_bool(&w, 0x01, true);	/* finalPart */
	bw_bool(&w, 0x01, false);	/* procByteContinueTx */
	bw_bool(&w, 0x01, false);	/* procByteContinueRx */
	bw_end(&w);
	bw_tlv(&w, 0x04, data, len);
	return pdu_end(&w);
}

size_t rspro_enc_client_slot_status_ind(uint8_t *out, size_t cap, uint32_t tag,
					const struct rspro_slot *client,
					const struct rspro_slot *bank,
					bool reset_active, bool vcc, bool clk,
					bool card_present)
{
	struct bw w;

	pdu_begin(&w, out, cap, tag, RSPRO_CLIENT_SLOT_STATUS_IND);
	bw_slot(&w, client);
	bw_slot(&w, bank);
	/* SlotPhysStatus { resetActive [0], vccPresent [1], clkActive [2],
	 * cardPresent [3] } — all four given, as a card emulator reports them */
	bw_begin(&w, 0x30);
	bw_bool(&w, 0x80, reset_active);
	bw_bool(&w, 0x81, vcc);
	bw_bool(&w, 0x82, clk);
	bw_bool(&w, 0x83, card_present);
	bw_end(&w);
	return pdu_end(&w);
}

/* ---- BER reader ---------------------------------------------------------- */

struct br {
	const uint8_t *p;
	size_t n;
};

/* the next element: 1 with its tag and content, 0 at the end, -1 malformed
 * (a multi-byte tag, an indefinite or oversized length, a short buffer) */
static int br_next(struct br *r, uint8_t *tag, struct br *c)
{
	size_t len, i, ll;

	if (!r->n)
		return 0;
	if (r->n < 2 || (r->p[0] & 0x1f) == 0x1f)
		return -1;
	*tag = r->p[0];
	len = r->p[1];
	i = 2;
	if (len & 0x80) {
		ll = len & 0x7f;
		if (!ll || ll > 3 || r->n < 2 + ll)
			return -1;
		for (len = 0; ll; ll--)
			len = (len << 8) | r->p[i++];
	}
	if (len > r->n - i)
		return -1;
	c->p = r->p + i;
	c->n = len;
	r->p += i + len;
	r->n -= i + len;
	return 1;
}

static int br_uint(const struct br *c, uint32_t *v)
{
	size_t i;

	/* at most 32 bits plus the sign byte, never negative */
	if (!c->n || c->n > 5 || (c->p[0] & 0x80) || (c->n == 5 && c->p[0]))
		return -1;
	for (*v = 0, i = 0; i < c->n; i++)
		*v = (*v << 8) | c->p[i];
	return 0;
}

static int br_u16(const struct br *c, uint16_t *v)
{
	uint32_t u;

	if (br_uint(c, &u) || u > 0xffff)
		return -1;
	*v = (uint16_t)u;
	return 0;
}

static int br_int(const struct br *c, int *v)
{
	uint32_t u;

	if (br_uint(c, &u) || u > 0x7fffffff)
		return -1;
	*v = (int)u;
	return 0;
}

static void br_cstr(const struct br *c, char *out, size_t cap)
{
	size_t n = c->n < cap - 1 ? c->n : cap - 1, i;

	for (i = 0; i < n; i++)
		out[i] = (c->p[i] >= 0x20 && c->p[i] < 0x7f) ? (char)c->p[i] : '?';
	out[n] = '\0';
}

/* ClientSlot / BankSlot: two INTEGERs, then possibly extensions */
static int br_slot(struct br c, struct rspro_slot *s)
{
	struct br v;
	uint8_t t;

	if (br_next(&c, &t, &v) != 1 || t != 0x02 || br_u16(&v, &s->id))
		return -1;
	if (br_next(&c, &t, &v) != 1 || t != 0x02 || br_u16(&v, &s->nr))
		return -1;
	return 0;
}

/* ComponentIdentity { type, name, software [0], swVersion [1], ... }: the
 * type and the name are mandatory */
static int dec_identity(struct br c, struct rspro_pdu *p)
{
	struct br v;
	uint8_t t;
	int r;
	bool type = false, name = false;

	while ((r = br_next(&c, &t, &v)) == 1) {
		if (t == 0x0A) {
			if (br_int(&v, &p->comp_type))
				return -1;
			type = true;
		} else if (t == 0x16) {
			br_cstr(&v, p->comp_name, sizeof(p->comp_name));
			name = true;
		} else if (t == 0x80)
			br_cstr(&v, p->comp_software, sizeof(p->comp_software));
		else if (t == 0x81)
			br_cstr(&v, p->comp_version, sizeof(p->comp_version));
	}
	return (r < 0 || !type || !name) ? -1 : 0;
}

static int dec_data(const struct br *v, struct rspro_pdu *p)
{
	if (v->n > sizeof(p->data))
		return -1;
	memcpy(p->data, v->p, v->n);
	p->data_len = v->n;
	return 0;
}

/* TpduFlags: four BOOLEANs, the second is finalPart */
static int dec_flags(struct br c, struct rspro_pdu *p)
{
	struct br v;
	uint8_t t;
	int i = 0, r;

	while ((r = br_next(&c, &t, &v)) == 1)
		if (t == 0x01) {
			if (v.n != 1)
				return -1;
			if (i++ == 1)
				p->final_part = v.p[0];
		}
	return (r < 0 || i < 4) ? -1 : 0;
}

/* IpPort { ip IpAddress CHOICE { ipv4 [0], ipv6 [1] }, port INTEGER } */
static int dec_ipport(struct br c, struct rspro_pdu *p)
{
	struct br f;
	uint8_t ft;
	int r;
	bool port = false;

	while ((r = br_next(&c, &ft, &f)) == 1) {
		if ((ft == 0x80 && f.n == 4) || (ft == 0x81 && f.n == 16)) {
			memcpy(p->ip, f.p, f.n);
			p->ip_len = f.n;
		} else if (ft == 0x80 || ft == 0x81) {
			return -1;
		} else if (ft == 0x02) {
			if (br_u16(&f, &p->port))
				return -1;
			port = true;
		}
	}
	return (r < 0 || !p->ip_len || !port) ? -1 : 0;
}

/* the messages whose content is one ResultCode (and extensions) */
static bool result_only(int msg)
{
	return msg == RSPRO_CREATE_MAPPING_RES || msg == RSPRO_REMOVE_MAPPING_RES ||
	       msg == RSPRO_CONFIG_CLIENT_ID_RES || msg == RSPRO_CONFIG_CLIENT_BANK_RES ||
	       msg == RSPRO_SET_ATR_RES || msg == RSPRO_RESET_STATE_RES;
}

/* The fields of the alternative, by position and tag. What a message must
 * carry (asn1/RSPRO.asn: the fields without OPTIONAL) is required: a
 * response without its result would otherwise read as one that did not
 * refuse, a slot or an address that is missing as slot 0:0 or port 0. */
static int dec_msg(struct br c, struct rspro_pdu *p)
{
	struct br v;
	uint8_t t;
	int seq = 0, r;
	bool identity = false, flags = false, data = false, phys = false;

	while ((r = br_next(&c, &t, &v)) == 1) {
		switch (p->msg) {
		case RSPRO_CONNECT_CLIENT_RES:
		case RSPRO_CONNECT_BANK_RES:
			if (t == 0x30 && !identity) {
				if (dec_identity(v, p) < 0)
					return -1;
				identity = true;
			}
			if (t == 0x0A && (p->result >= 0 || br_int(&v, &p->result)))
				return -1;
			break;
		case RSPRO_CONFIG_CLIENT_ID_REQ:
			if (t == 0x30 && !p->has_client) {
				if (br_slot(v, &p->client))
					return -1;
				p->has_client = true;
			}
			break;
		case RSPRO_CONFIG_CLIENT_BANK_REQ:
			/* bankSlot BankSlot, bankd IpPort */
			if (t == 0x30 && seq == 0) {
				if (br_slot(v, &p->bank))
					return -1;
				p->has_bank = true;
			} else if (t == 0x30 && seq == 1) {
				if (dec_ipport(v, p))
					return -1;
			}
			if (t == 0x30)
				seq++;
			break;
		case RSPRO_SET_ATR_REQ:
			if (t == 0x30 && !p->has_client) {
				if (br_slot(v, &p->client))
					return -1;
				p->has_client = true;
			} else if (t == 0x04 && !data) {
				if (dec_data(&v, p))
					return -1;
				data = true;
			}
			break;
		case RSPRO_TPDU_CARD_TO_MODEM:
		case RSPRO_TPDU_MODEM_TO_CARD:
		case RSPRO_BANK_SLOT_STATUS_IND: {
			/* fromBankSlot, toClientSlot, flags | slotPhysStatus, data;
			 * modem-to-card (only decoded for the tests) has the two
			 * slots the other way round */
			bool m2c = p->msg == RSPRO_TPDU_MODEM_TO_CARD;

			if (t == 0x30 && seq < 2) {
				bool bank = (seq == 0) != m2c;

				if (br_slot(v, bank ? &p->bank : &p->client))
					return -1;
				if (bank)
					p->has_bank = true;
				else
					p->has_client = true;
			} else if (t == 0x30 && seq == 2 && p->msg != RSPRO_BANK_SLOT_STATUS_IND) {
				if (dec_flags(v, p))
					return -1;
				flags = true;
			} else if (t == 0x30 && seq == 2) {
				phys = true;
			} else if (t == 0x04 && !data) {
				if (dec_data(&v, p))
					return -1;
				data = true;
			}
			if (t == 0x30)
				seq++;
			break;
		}
		case RSPRO_ERROR_IND:
			/* sender, severity, code: three ENUMERATEDs in order */
			if (t == 0x0A) {
				int e;

				if (br_int(&v, &e))
					return -1;
				if (seq == 1)
					p->err_severity = e;
				else if (seq == 2)
					p->err_code = e;
				seq++;
			} else if (t == 0x82) {
				br_cstr(&v, p->err_string, sizeof(p->err_string));
			}
			break;
		default:
			/* a response that is one ResultCode, or a message a
			 * client does not act on: its result where it has one */
			if (t == 0x0A && p->result < 0 && br_int(&v, &p->result))
				return -1;
			break;
		}
	}
	if (r < 0)
		return -1;

	switch (p->msg) {
	case RSPRO_CONNECT_CLIENT_RES:
	case RSPRO_CONNECT_BANK_RES:
		return (identity && p->result >= 0) ? 0 : -1;
	case RSPRO_CONFIG_CLIENT_ID_REQ:
		return p->has_client ? 0 : -1;
	case RSPRO_CONFIG_CLIENT_BANK_REQ:
		return (p->has_bank && p->ip_len && seq >= 2) ? 0 : -1;
	case RSPRO_SET_ATR_REQ:
		return (p->has_client && data) ? 0 : -1;
	case RSPRO_TPDU_CARD_TO_MODEM:
	case RSPRO_TPDU_MODEM_TO_CARD:
		return (p->has_bank && p->has_client && flags && data) ? 0 : -1;
	case RSPRO_BANK_SLOT_STATUS_IND:
		return (p->has_bank && p->has_client && phys) ? 0 : -1;
	case RSPRO_ERROR_IND:
		return (seq >= 3) ? 0 : -1;
	default:
		return (result_only(p->msg) && p->result < 0) ? -1 : 0;
	}
}

int rspro_decode(const uint8_t *b, size_t n, struct rspro_pdu *p)
{
	struct br r = { b, n }, pdu, v, alt;
	uint8_t t;
	bool have_version = false, have_tag = false;
	int rr;

	memset(p, 0, sizeof(*p));
	p->msg = -1;
	p->result = -1;
	p->comp_type = -1;
	p->err_severity = p->err_code = -1;
	/* one RsproPDU and nothing after it: a frame carries exactly one */
	if (br_next(&r, &t, &pdu) != 1 || t != 0x30 || r.n)
		return -1;
	while ((rr = br_next(&pdu, &t, &v)) == 1) {
		if (t == 0x80) {
			uint32_t u;

			if (have_version || br_uint(&v, &u))
				return -1;
			p->version = u;
			have_version = true;
		} else if (t == 0x81) {
			if (have_tag || br_uint(&v, &p->tag))
				return -1;
			have_tag = true;
		} else if (t == 0xA2) {
			/* the explicit CHOICE wrapper: exactly one alternative */
			if (p->msg >= 0 || br_next(&v, &t, &alt) != 1 || (t & 0xE0) != 0xA0 || v.n)
				return -1;
			p->msg = t & 0x1f;
			if (dec_msg(alt, p) < 0)
				return -1;
		}
	}
	if (rr < 0 || !have_version || !have_tag || p->msg < 0)
		return -1;
	/* another version may mean other fields under the same tags */
	return (p->version == RSPRO_VERSION) ? 0 : RSPRO_E_VERSION;
}

const char *rspro_msg_name(int msg)
{
	static const char *const names[] = {
		"connectBankReq", "connectBankRes", "connectClientReq",
		"connectClientRes", "createMappingReq", "createMappingRes",
		"removeMappingReq", "removeMappingRes", "configClientIdReq",
		"configClientIdRes", "configClientBankReq", "configClientBankRes",
		"errorInd", "setAtrReq", "setAtrRes", "tpduModemToCard",
		"tpduCardToModem", "clientSlotStatusInd", "bankSlotStatusInd",
		"resetStateReq", "resetStateRes",
	};

	return (msg >= 0 && msg < (int)(sizeof(names) / sizeof(names[0]))) ? names[msg] : "unknown";
}

const char *rspro_result_name(int result)
{
	switch (result) {
	case RSPRO_RES_OK:			return "ok";
	case RSPRO_RES_ILLEGAL_CLIENT_ID:	return "illegalClientId";
	case RSPRO_RES_ILLEGAL_BANK_ID:		return "illegalBankId";
	case RSPRO_RES_ILLEGAL_SLOT_ID:		return "illegalSlotId";
	case RSPRO_RES_UNSUPPORTED_VERSION:	return "unsupportedProtocolVersion";
	case RSPRO_RES_UNKNOWN_SLOTMAP:		return "unknownSlotmap";
	case RSPRO_RES_IDENTITY_IN_USE:		return "identityInUse";
	case RSPRO_RES_CARD_NOT_PRESENT:	return "cardNotPresent";
	case RSPRO_RES_CARD_UNRESPONSIVE:	return "cardUnresponsive";
	case RSPRO_RES_CARD_TRANSMISSION_ERR:	return "cardTransmissionError";
	default:				return "unknown";
	}
}

/* ---- IPA ----------------------------------------------------------------- */

size_t rspro_ipa_wrap(uint8_t *out, size_t msg_len)
{
	size_t l = msg_len + 1;	/* the extension byte counts */

	out[0] = (uint8_t)(l >> 8);
	out[1] = (uint8_t)l;
	out[2] = IPA_PROTO_OSMO;
	out[3] = IPA_EXT_RSPRO;
	return msg_len + 4;
}

int rspro_ipa_parse(const uint8_t *b, size_t n, size_t *used, int *proto,
		    int *ext, const uint8_t **payload, size_t *payload_len)
{
	size_t l;

	if (n < 3)
		return 0;
	l = ((size_t)b[0] << 8) | b[1];
	if (l > RSPRO_MSG_MAX)
		return -1;
	if (n < 3 + l)
		return 0;
	*used = 3 + l;
	*proto = b[2];
	if (b[2] == IPA_PROTO_OSMO) {
		if (!l)
			return -1;
		*ext = b[3];
		*payload = b + 4;
		*payload_len = l - 1;
	} else {
		*ext = -1;
		*payload = b + 3;
		*payload_len = l;
	}
	return 1;
}
