/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_H
#define RSIM_H

/* Result codes shared by the T=0 engine, the backends and the protocol
 * layer. Negative so that a length or a status byte can never be mistaken
 * for one; each has exactly one wire name (rsim_err_name), which is what the
 * plugin matches on. */
enum rsim_err {
	RSIM_OK = 0,
	RSIM_E_NO_CARD = -1,
	RSIM_E_TIMEOUT = -2,
	RSIM_E_IO = -3,
	RSIM_E_BAD_REQUEST = -4,
	RSIM_E_NOT_POWERED = -5,
	RSIM_E_PROTOCOL = -6,
};

#define RSIM_DETAIL_MAX 160

/* The largest command accepted on the request line: 261 bytes is the QMI
 * UIM Remote APDU_IND limit (docs/plan.md §1). A T=0 TPDU itself is at
 * most CLA INS P1 P2 P3 + 255 bytes, which the engine enforces through
 * P3; the response is at most 256 data bytes + SW1 SW2 (ISO/IEC
 * 7816-3:2006 §10.3.2). */
#define RSIM_TPDU_MAX 261
#define RSIM_RESP_MAX 258

static inline const char *rsim_err_name(int err)
{
	switch (err) {
	case RSIM_E_NO_CARD:		return "no_card";
	case RSIM_E_TIMEOUT:		return "timeout";
	case RSIM_E_BAD_REQUEST:	return "bad_request";
	case RSIM_E_NOT_POWERED:	return "not_powered";
	case RSIM_E_PROTOCOL:		return "protocol";
	default:			return "io";
	}
}

#endif
