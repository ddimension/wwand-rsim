/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rsim.h"
#include "t0.h"

unsigned t0_wwt_ms(unsigned clock_khz, unsigned wi)
{
	unsigned long cycles = 960ul * wi * 372ul;

	if (!clock_khz)
		return 0;
	return (unsigned)((cycles + clock_khz - 1) / clock_khz);
}

static int fail(int err, char *detail, size_t dlen, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));
static int chan_fail(int err, char *detail, size_t dlen, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

static int fail(int err, char *detail, size_t dlen, const char *fmt, ...)
{
	va_list ap;

	if (detail && dlen) {
		va_start(ap, fmt);
		vsnprintf(detail, dlen, fmt, ap);
		va_end(ap);
	}
	return err;
}

/* A channel failure: the channel may already have said what went wrong
 * (an echo mismatch, a vanished reader); that reason is kept and the
 * engine's context put in front of it, so neither is lost. */
static int chan_fail(int err, char *detail, size_t dlen, const char *fmt, ...)
{
	char prev[RSIM_DETAIL_MAX], what[80];
	va_list ap;

	if (!detail || !dlen)
		return err;
	snprintf(prev, sizeof(prev), "%s", detail);
	va_start(ap, fmt);
	vsnprintf(what, sizeof(what), fmt, ap);
	va_end(ap);
	if (*prev)
		snprintf(detail, dlen, "%s: %s", what, prev);
	else
		snprintf(detail, dlen, "%s", what);
	return err;
}

static int is_sw1(uint8_t b)
{
	/* §10.3.3: 6X (except the NULL byte 60) and 9X are SW1 */
	return (b & 0xf0) == 0x90 || ((b & 0xf0) == 0x60 && b != 0x60);
}

int t0_transceive(const struct t0_chan *ch, unsigned wwt_ms,
		  const uint8_t *tpdu, size_t len,
		  uint8_t *resp, size_t *resp_len,
		  char *detail, size_t dlen)
{
	uint8_t hdr[5], ins, pb, sw2;
	const uint8_t *out = NULL;
	size_t out_len = 0, in_len = 0, sent = 0, got = 0;
	int r;

	*resp_len = 0;
	if (len < 4)
		return fail(RSIM_E_BAD_REQUEST, detail, dlen,
			    "TPDU of %zu bytes, CLA INS P1 P2 at least", len);
	memcpy(hdr, tpdu, 4);
	/* §10.3.2 case 1 is CLA INS P1 P2 with P3 = 00 */
	hdr[4] = len >= 5 ? tpdu[4] : 0;
	ins = hdr[1];
	/* §10.3.2: INS 6X and 9X are invalid -- they would be read back as
	 * SW1, so the card could not acknowledge them */
	if ((ins & 0xf0) == 0x60 || (ins & 0xf0) == 0x90)
		return fail(RSIM_E_BAD_REQUEST, detail, dlen,
			    "INS %02X is invalid in T=0", ins);
	if (len > 5) {
		/* data out: P3 is the count, and a mismatch would leave
		 * card and reader disagreeing on where the command ends.
		 * This also bounds the TPDU to 5 + 255 bytes. */
		out = tpdu + 5;
		out_len = len - 5;
		if (out_len != hdr[4])
			return fail(RSIM_E_BAD_REQUEST, detail, dlen,
				    "P3 %u but %zu data bytes", hdr[4], out_len);
	} else {
		/* data in (or case 1): P3 = 00 announces 256 bytes; a
		 * case-1 card never acknowledges and answers SW directly */
		in_len = hdr[4] ? hdr[4] : 256;
	}

	r = ch->send(ch->ctx, hdr, 5);
	if (r)
		return chan_fail(r, detail, dlen, "sending header");

	for (;;) {
		r = ch->recv(ch->ctx, &pb, wwt_ms);
		if (r)
			return chan_fail(r, detail, dlen, "waiting for procedure byte");
		/* §10.3.3: NULL asks for more time; the work waiting time
		 * restarts, which the next recv() does by itself */
		if (pb == 0x60)
			continue;
		if (is_sw1(pb)) {
			r = ch->recv(ch->ctx, &sw2, wwt_ms);
			if (r)
				return chan_fail(r, detail, dlen, "waiting for SW2");
			resp[got] = pb;
			resp[got + 1] = sw2;
			*resp_len = got + 2;
			return RSIM_OK;
		}
		/* ACK: the 2006 edition compares with INS as sent (odd
		 * INS included); the 1997 VPP variants INS^01 are gone */
		if (pb == ins || pb == (uint8_t)~ins) {
			size_t n;

			if (sent == out_len && got == in_len)
				return fail(RSIM_E_PROTOCOL, detail, dlen,
					    "procedure byte %02X with nothing left to transfer", pb);
			/* INS: all remaining bytes; ~INS: exactly one */
			if (out_len) {
				n = pb == ins ? out_len - sent : 1;
				r = ch->send(ch->ctx, out + sent, n);
				if (r)
					return chan_fail(r, detail, dlen, "sending data");
				sent += n;
			} else {
				n = pb == ins ? in_len - got : 1;
				while (n--) {
					/* §10.2: WWT bounds the gap between
					 * any two characters from the card */
					r = ch->recv(ch->ctx, &resp[got], wwt_ms);
					if (r)
						return chan_fail(r, detail, dlen,
								 "receiving data byte %zu of %zu",
								 got + 1, in_len);
					got++;
				}
			}
			continue;
		}
		return fail(RSIM_E_PROTOCOL, detail, dlen,
			    "unexpected procedure byte %02X for INS %02X", pb, ins);
	}
}
