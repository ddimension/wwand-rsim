/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * SIM Access Profile messages and the SDP lookup of its channel, as bytes.
 *
 * A SAP message (SAP v1.1 §5.1): MsgID, ParamCount, two reserved bytes, then
 * each parameter as ParamID, a reserved byte, its length (2 bytes, big
 * endian), the value and padding to a multiple of four.
 */
#include <string.h>

#include "sap.h"

static size_t pad4(size_t n)
{
	return (n + 3u) & ~(size_t)3u;
}

int sap_encode(uint8_t *out, size_t cap, uint8_t id, const struct sap_param *p, int n)
{
	size_t o = 4;
	int i;

	if (cap < 4 || n < 0 || n > SAP_PARAMS_MAX)
		return -1;
	out[0] = id;
	out[1] = (uint8_t)n;
	out[2] = out[3] = 0;
	for (i = 0; i < n; i++) {
		size_t l = p[i].len;

		if (o + 4 + pad4(l) > cap)
			return -1;
		out[o] = p[i].id;
		out[o + 1] = 0;
		out[o + 2] = (uint8_t)(l >> 8);
		out[o + 3] = (uint8_t)l;
		if (l)
			memcpy(out + o + 4, p[i].val, l);
		memset(out + o + 4 + l, 0, pad4(l) - l);
		o += 4 + pad4(l);
	}
	return (int)o;
}

int sap_frame(const uint8_t *buf, size_t len)
{
	size_t o = 4;
	int i, n;

	if (len < 4)
		return 0;
	n = buf[1];
	if (n > SAP_PARAMS_MAX)
		return -1;
	for (i = 0; i < n; i++) {
		size_t l;

		if (len < o + 4)
			return 0;
		l = ((size_t)buf[o + 2] << 8) | buf[o + 3];
		o += 4 + pad4(l);
		if (o > 0x7fff)
			return -1;
	}
	return len < o ? 0 : (int)o;
}

int sap_decode(const uint8_t *buf, size_t len, struct sap_msg *m)
{
	size_t o = 4;
	int i;

	if (sap_frame(buf, len) <= 0)
		return -1;
	m->id = buf[0];
	m->n = buf[1];
	for (i = 0; i < m->n; i++) {
		m->p[i].id = buf[o];
		m->p[i].len = (uint16_t)((buf[o + 2] << 8) | buf[o + 3]);
		m->p[i].val = buf + o + 4;
		o += 4 + pad4(m->p[i].len);
	}
	return 0;
}

const struct sap_param *sap_get(const struct sap_msg *m, uint8_t id)
{
	int i;

	for (i = 0; i < m->n; i++)
		if (m->p[i].id == id)
			return &m->p[i];
	return NULL;
}

int sap_get_u8(const struct sap_msg *m, uint8_t id)
{
	const struct sap_param *p = sap_get(m, id);

	return (p && p->len == 1) ? p->val[0] : -1;
}

const char *sap_msg_name(uint8_t id)
{
	static const char *const names[] = {
		"CONNECT_REQ", "CONNECT_RESP", "DISCONNECT_REQ", "DISCONNECT_RESP", "DISCONNECT_IND",
		"TRANSFER_APDU_REQ", "TRANSFER_APDU_RESP", "TRANSFER_ATR_REQ", "TRANSFER_ATR_RESP",
		"POWER_SIM_OFF_REQ", "POWER_SIM_OFF_RESP", "POWER_SIM_ON_REQ", "POWER_SIM_ON_RESP",
		"RESET_SIM_REQ", "RESET_SIM_RESP", "TRANSFER_CARD_READER_STATUS_REQ",
		"TRANSFER_CARD_READER_STATUS_RESP", "STATUS_IND", "ERROR_RESP",
		"SET_TRANSPORT_PROTOCOL_REQ", "SET_TRANSPORT_PROTOCOL_RESP",
	};

	return id < sizeof(names) / sizeof(names[0]) ? names[id] : "?";
}

const char *sap_result_name(int rc)
{
	static const char *const names[] = {
		"ok", "error, no reason given", "card not accessible", "card already powered off",
		"card removed", "card already powered on", "data not available", "not supported",
	};

	return (rc >= 0 && rc < (int)(sizeof(names) / sizeof(names[0]))) ? names[rc] : "unknown result";
}

/* ---- SDP ------------------------------------------------------------- */

#define SDP_SSA_REQ 0x06
#define SDP_SSA_RSP 0x07
#define SDP_ATTR_PROTO_DESC_LIST 0x0004
#define SDP_UUID_RFCOMM 0x0003

int sdp_search_req(uint8_t *out, size_t cap, uint16_t tid, uint16_t uuid16,
		   const uint8_t *cont, size_t cont_len)
{
	size_t plen = 5 + 2 + 5 + 1 + cont_len;

	if (cont_len > 16 || cap < 5 + plen)
		return -1;
	out[0] = SDP_SSA_REQ;
	out[1] = (uint8_t)(tid >> 8);
	out[2] = (uint8_t)tid;
	out[3] = (uint8_t)(plen >> 8);
	out[4] = (uint8_t)plen;
	/* ServiceSearchPattern: DES { UUID16 } */
	out[5] = 0x35;
	out[6] = 3;
	out[7] = 0x19;
	out[8] = (uint8_t)(uuid16 >> 8);
	out[9] = (uint8_t)uuid16;
	/* MaximumAttributeByteCount */
	out[10] = 0x04;
	out[11] = 0x00;
	/* AttributeIDList: DES { UINT16 0x0004 } */
	out[12] = 0x35;
	out[13] = 3;
	out[14] = 0x09;
	out[15] = SDP_ATTR_PROTO_DESC_LIST >> 8;
	out[16] = SDP_ATTR_PROTO_DESC_LIST & 0xff;
	out[17] = (uint8_t)cont_len;
	if (cont_len)
		memcpy(out + 18, cont, cont_len);
	return (int)(18 + cont_len);
}

int sdp_search_rsp(const uint8_t *pdu, size_t len, uint16_t tid,
		   const uint8_t **attrs, size_t *attrs_len,
		   const uint8_t **cont, size_t *cont_len)
{
	size_t plen, alen;

	if (len < 5 + 3 || pdu[0] != SDP_SSA_RSP || ((pdu[1] << 8) | pdu[2]) != tid)
		return -1;
	plen = ((size_t)pdu[3] << 8) | pdu[4];
	if (5 + plen > len || plen < 3)
		return -1;
	alen = ((size_t)pdu[5] << 8) | pdu[6];
	if (7 + alen + 1 > 5 + plen)
		return -1;
	*attrs = pdu + 7;
	*attrs_len = alen;
	*cont_len = pdu[7 + alen];
	*cont = pdu + 8 + alen;
	if (*cont_len > 16 || 8 + alen + *cont_len > 5 + plen)
		return -1;
	return 0;
}

/* One data element at p: its type, where its value starts and how long it
 * is; the whole element's length, or 0 when it runs past end. */
static size_t de_head(const uint8_t *p, const uint8_t *end, int *type, const uint8_t **val, size_t *vlen)
{
	static const uint8_t fixed[] = { 1, 2, 4, 8, 16 };
	int sz;
	size_t hl = 1, l;

	if (p >= end)
		return 0;
	*type = p[0] >> 3;
	sz = p[0] & 7;
	if (*type == 0)
		l = 0;
	else if (sz < 5)
		l = fixed[sz];
	else {
		size_t nb = (size_t)1 << (sz - 5), i;

		if ((size_t)(end - p) < 1 + nb)
			return 0;
		for (l = 0, i = 0; i < nb; i++)
			l = (l << 8) | p[1 + i];
		hl += nb;
	}
	if ((size_t)(end - p) < hl || (size_t)(end - p) - hl < l)
		return 0;
	*val = p + hl;
	*vlen = l;
	return hl + l;
}

/* a UUID element that is 0x0003, in its 16, 32 or 128-bit form (the latter
 * on the Bluetooth base UUID 0000xxxx-0000-1000-8000-00805F9B34FB) */
static int is_rfcomm_uuid(const uint8_t *v, size_t l)
{
	static const uint8_t base[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB };

	if (l == 2)
		return v[0] == 0 && v[1] == SDP_UUID_RFCOMM;
	if (l == 4)
		return !v[0] && !v[1] && !v[2] && v[3] == SDP_UUID_RFCOMM;
	if (l == 16)
		return !v[0] && !v[1] && !v[2] && v[3] == SDP_UUID_RFCOMM && !memcmp(v + 4, base, 12);
	return 0;
}

static int walk(const uint8_t *p, const uint8_t *end, int depth)
{
	int prev_rfcomm = 0;

	if (depth > 8)
		return -1;
	while (p < end) {
		const uint8_t *v;
		size_t vl, n;
		int type, ch;

		if (!(n = de_head(p, end, &type, &v, &vl)))
			return -1;
		if (prev_rfcomm && type == 1 && vl == 1 && v[0] >= 1 && v[0] <= 30)
			return v[0];
		prev_rfcomm = type == 3 && is_rfcomm_uuid(v, vl);
		if ((type == 6 || type == 7) && (ch = walk(v, v + vl, depth + 1)) > 0)
			return ch;
		p += n;
	}
	return -1;
}

int sdp_rfcomm_channel(const uint8_t *attrs, size_t len)
{
	return walk(attrs, attrs + len, 0);
}
