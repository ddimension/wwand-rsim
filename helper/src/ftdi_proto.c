/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <string.h>

#include "ftdi_proto.h"

/*
 * FT232BM baud divisor (checked against ftdi_232bm_baud_base_to_divisor()
 * in drivers/usb/serial/ftdi_sio.c, linux 6.18.41): the rate is 3 MHz
 * divided by n + e/8, with n in 14 bits and the eighths e in 3 bits above
 * them. The eighths are not stored as such: the chip's code for e is
 * {0, 3, 2, 4, 1, 5, 6, 7}[e] (so code 1 means 0.5, code 3 means 0.125).
 * n + e/8 = 1 and 1.5 have their own codes 0 and 1 (3 and 2 Mbaud).
 */
static const uint8_t eighth_code[8] = { 0, 3, 2, 4, 1, 5, 6, 7 };

int ftdi_bm_divisor(unsigned baud, uint32_t *div)
{
	unsigned long d8;

	if (baud < 183 || baud > 3000000)
		return -1;
	/* 3 MHz * 8 / baud in eighths, rounded to nearest exactly as the
	 * kernel's DIV_ROUND_CLOSEST(48 MHz, 2 * baud) */
	d8 = (48000000ul + baud) / (2ul * baud);
	if (d8 >> 3 > 0x3fff)
		return -1;
	*div = (uint32_t)(d8 >> 3) | (uint32_t)eighth_code[d8 & 7] << 14;
	if (*div == 1)
		*div = 0;
	else if (*div == 0x4001)
		*div = 1;
	return 0;
}

unsigned ftdi_bm_rate(uint32_t div)
{
	unsigned long d8;
	unsigned code = div >> 14 & 7, e;

	if (div == 0)
		return 3000000;
	if (div == 1)
		return 2000000;
	for (e = 0; eighth_code[e] != code; e++)
		;
	d8 = (unsigned long)(div & 0x3fff) * 8 + e;
	return (unsigned)((24000000ul + d8 / 2) / d8);
}

size_t ftdi_rx_parse(const uint8_t *in, size_t len, size_t mps, bool drop_errors,
		     uint8_t *out, struct ftdi_rx_stat *st)
{
	size_t off, n = 0;

	for (off = 0; off < len; off += mps) {
		size_t plen = len - off < mps ? len - off : mps;
		const uint8_t *pk = in + off;

		/* a packet shorter than its two status bytes carries nothing
		 * we can place; ftdi_sio drops it the same way */
		if (plen < 2)
			break;
		st->modem = pk[0];
		if (plen == 2)
			continue;	/* status only: errors count with data */
		if (pk[1] & FTDI_LS_OE)
			st->overrun++;
		/*
		 * The error bits are one set per packet, not per byte, so
		 * which byte was hit is unknown. ftdi_sio flags every byte
		 * of such a packet (ftdi_process_packet(), linux 6.18.41),
		 * so INPCK|IGNPAR on the tty drops the whole packet; this
		 * does the same, which keeps the two transports alike. At
		 * the 2 ms latency timer a packet holds one or two card
		 * characters, so little good data goes with the bad.
		 */
		if (pk[1] & (FTDI_LS_PE | FTDI_LS_FE)) {
			if (pk[1] & FTDI_LS_PE)
				st->parity++;
			else
				st->framing++;
			if (drop_errors) {
				st->dropped += (unsigned)(plen - 2);
				continue;
			}
		}
		memcpy(out + n, pk + 2, plen - 2);
		n += plen - 2;
	}
	return n;
}
