/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <string.h>

#include "atr.h"

uint8_t atr_inverse(uint8_t b)
{
	uint8_t r = 0;
	int i;

	for (i = 0; i < 8; i++)
		if (b & (1u << i))
			r |= (uint8_t)(0x80u >> i);
	return (uint8_t)~r;
}

/* ISO/IEC 7816-3:2006 Table 7 (Fi) and Table 8 (Di) */
unsigned atr_fi_value(uint8_t fi)
{
	static const unsigned tab[16] = {
		372, 372, 558, 744, 1116, 1488, 1860, 0,
		0, 512, 768, 1024, 1536, 2048, 0, 0,
	};

	return tab[fi & 0x0f];
}

unsigned atr_di_value(uint8_t di)
{
	static const unsigned tab[16] = {
		0, 1, 2, 4, 8, 16, 32, 64, 12, 20, 0, 0, 0, 0, 0, 0,
	};

	return tab[di & 0x0f];
}

static int popcount4(uint8_t y)
{
	return (y & 1) + (y >> 1 & 1) + (y >> 2 & 1) + (y >> 3 & 1);
}

int atr_parse(const uint8_t *atr, size_t len, struct atr_info *info)
{
	size_t pos = 2, i;
	uint8_t y, k, x;
	int group = 1;
	bool any_td = false;

	memset(info, 0, sizeof(*info));
	info->wi = 10;
	info->fi = 1;
	info->di = 1;
	info->expected = 2;
	if (len < 1) {
		info->expected = 1;
		return ATR_NEED_MORE;
	}
	/* §8.1: TS is 3B (direct) or 3F (inverse), nothing else */
	if (atr[0] == 0x3f)
		info->inverse = true;
	else if (atr[0] != 0x3b)
		return ATR_INVALID;
	if (len < 2)
		return ATR_NEED_MORE;

	y = atr[1] >> 4;
	k = atr[1] & 0x0f;
	for (;;) {
		size_t need = pos + (size_t)popcount4(y);
		uint8_t td = 0;

		/* §8.2.1: the whole ATR fits in ATR_MAX characters; a
		 * chain of TDi that runs past it is garbage, not an ATR */
		if (need + k + (info->has_tck ? 1 : 0) > ATR_MAX)
			return ATR_INVALID;
		if (len < need) {
			info->expected = need + k + (info->has_tck ? 1 : 0);
			return ATR_NEED_MORE;
		}
		if (y & 1) {		/* TAi */
			if (group == 1) {
				info->has_ta1 = true;
				info->fi = atr[pos] >> 4;
				info->di = atr[pos] & 0x0f;
			} else if (group == 2) {
				info->specific_mode = true;
			}
			pos++;
		}
		if (y & 2)		/* TBi: VPP, deprecated since 2006 */
			pos++;
		if (y & 4) {		/* TCi */
			/* §10.2: TC2 is the T=0 waiting time integer WI;
			 * 00 is reserved, so the default stands */
			if (group == 2 && atr[pos])
				info->wi = atr[pos];
			pos++;
		}
		if (!(y & 8))
			break;
		td = atr[pos++];
		any_td = true;
		info->protocols |= (uint16_t)(1u << (td & 0x0f));
		/* §8.2.5: TCK is absent only when T=0 is the sole type
		 * indicated -- any other T, including the global T=15,
		 * makes it present */
		if (td & 0x0f)
			info->has_tck = true;
		y = td >> 4;
		group++;
	}
	if (!any_td)
		info->protocols = 1;	/* §8.2.3: absent TD1 means T=0 */

	info->hist_off = pos;
	info->hist_len = k;
	info->expected = pos + k + (info->has_tck ? 1 : 0);
	if (info->expected > ATR_MAX)
		return ATR_INVALID;
	if (len < info->expected)
		return ATR_NEED_MORE;
	if (info->has_tck) {
		/* §8.2.5: T0 through TCK XOR to zero */
		x = 0;
		for (i = 1; i < info->expected; i++)
			x ^= atr[i];
		info->tck_ok = x == 0;
	}
	return ATR_COMPLETE;
}
