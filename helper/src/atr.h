/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_ATR_H
#define RSIM_ATR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TS + T0 + interface bytes + 15 historical bytes + TCK can never exceed
 * 32 characters after TS (ISO/IEC 7816-3:2006 §8.2.1) */
#define ATR_MAX 33

enum atr_result {
	ATR_INVALID = -1,
	ATR_NEED_MORE = 0,
	ATR_COMPLETE = 1,
};

struct atr_info {
	/* the length the ATR has at least, from what was parsed so far; it
	 * only grows as further TDi reveal more, and is exact once the
	 * parse returns ATR_COMPLETE -- so a reader can stop exactly at the
	 * last character instead of waiting out a timeout */
	size_t expected;
	bool inverse;		/* TS = 3F */
	uint16_t protocols;	/* bit T set for every T indicated (T=0 by default) */
	bool has_tck;
	bool tck_ok;		/* only meaningful with has_tck */
	uint8_t wi;		/* TC2, default 10 */
	bool has_ta1;
	uint8_t fi, di;		/* TA1 nibbles (indices, not values); 1/1 by default */
	bool specific_mode;	/* TA2 present */
	size_t hist_off, hist_len;
};

/* Parses the first len characters of an ATR, in logical values (TS already
 * 3B or 3F, i.e. after any inverse-convention decoding). */
int atr_parse(const uint8_t *atr, size_t len, struct atr_info *info);

/* The inverse convention sends each character MSB first with inverted
 * levels (ISO/IEC 7816-3:2006 §8.1). A UART framed for the direct
 * convention therefore sees the bit-reversed complement; the mapping is its
 * own inverse, so it encodes and decodes. */
uint8_t atr_inverse(uint8_t b);

/* Fi and Di for a TA1 nibble; 0 for an RFU index (Table 7 and Table 8) */
unsigned atr_fi_value(uint8_t fi);
unsigned atr_di_value(uint8_t di);

#endif
