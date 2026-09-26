/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <string.h>

#include "atr.h"
#include "json.h"
#include "check.h"

static size_t unhex(const char *h, uint8_t *b)
{
	int n = hex_decode(h, b, ATR_MAX + 8);

	return n < 0 ? 0 : (size_t)n;
}

/* Feeds the ATR one byte at a time the way the reader does: every prefix
 * must ask for more and must never promise a length beyond the real one
 * (a reader stopping at `expected` would otherwise wait for bytes that do
 * not come), and the whole must complete at exactly its own length. */
static int parse_incremental(const uint8_t *atr, size_t len, struct atr_info *info)
{
	size_t n;
	int r = ATR_NEED_MORE;

	for (n = 1; n <= len; n++) {
		r = atr_parse(atr, n, info);
		if (r != ATR_NEED_MORE)
			break;
		CHECK(info->expected > n);
		CHECK(info->expected <= len);
	}
	if (r == ATR_COMPLETE)
		CHECK(n == len && info->expected == len);
	return r;
}

static void test_usim(void)
{
	uint8_t a[40];
	size_t len = unhex("3B9F96801FC78031A073BE21136743200718000001A5", a);
	struct atr_info i;

	current_test = "usim";
	CHECK(parse_incremental(a, len, &i) == ATR_COMPLETE);
	CHECK(len == 22);
	CHECK(!i.inverse);
	/* TD1=80: T=0, TD2=1F: T=15 -> TCK present */
	CHECK(i.protocols == ((1u << 0) | (1u << 15)));
	CHECK(i.has_tck && i.tck_ok);
	CHECK(i.wi == 10);
	CHECK(i.has_ta1 && atr_fi_value(i.fi) == 512 && atr_di_value(i.di) == 32);
	CHECK(!i.specific_mode);
	CHECK(i.hist_off == 6 && i.hist_len == 15);
}

static void test_euicc(void)
{
	uint8_t a[40];
	/* this string lacks its TCK although T=15 is indicated: the parser
	 * must keep asking for it instead of reporting a complete ATR */
	size_t len = unhex("3B9F95803FC7A08031E073FE211B63F100AD830F9000", a);
	struct atr_info i;

	current_test = "euicc";
	CHECK(atr_parse(a, len, &i) == ATR_NEED_MORE);
	CHECK(i.expected == 23);
	a[len++] = 0x17;	/* the XOR of T0..last historical byte */
	CHECK(parse_incremental(a, len, &i) == ATR_COMPLETE);
	CHECK(i.has_tck && i.tck_ok);
	CHECK(i.protocols == ((1u << 0) | (1u << 15)));
	CHECK(i.hist_len == 15 && i.hist_off == 7);
	a[len - 1] ^= 1;
	CHECK(atr_parse(a, len, &i) == ATR_COMPLETE && !i.tck_ok);
}

static void test_t0_only(void)
{
	uint8_t a[40];
	/* TA1 TB1 TC1, no TD1 -> T=0 by default, no TCK */
	size_t len = unhex("3B7D960000574448494D4686930900000000", a);
	struct atr_info i;

	current_test = "t0_only";
	CHECK(len == 18);
	CHECK(parse_incremental(a, len, &i) == ATR_COMPLETE);
	CHECK(i.protocols == 1);
	CHECK(!i.has_tck);
	CHECK(i.hist_off == 5 && i.hist_len == 13);
	/* one historical byte short of what T0 announces */
	len = unhex("3B7D96000057444B4F4686930900000000", a);
	CHECK(len == 17);
	CHECK(atr_parse(a, len, &i) == ATR_NEED_MORE && i.expected == 18);

	/* TA1 TB1 only, 15 historical bytes */
	len = unhex("3B3F94008069AF0307015900000A0E833E9F16", a);
	CHECK(len == 19);
	CHECK(parse_incremental(a, len, &i) == ATR_COMPLETE);
	CHECK(i.protocols == 1 && !i.has_tck && i.hist_len == 15);
}

static void test_inverse(void)
{
	uint8_t a[40];
	size_t len = unhex("3F6525002C09699000", a);
	struct atr_info i;
	unsigned b;

	current_test = "inverse";
	CHECK(parse_incremental(a, len, &i) == ATR_COMPLETE);
	CHECK(i.inverse);
	CHECK(i.protocols == 1 && !i.has_tck);
	CHECK(i.hist_len == 5);
	/* §8.1: inverse TS 3F reads as 03 through a direct-framed UART */
	CHECK(atr_inverse(0x3f) == 0x03);
	CHECK(atr_inverse(0x03) == 0x3f);
	/* the mapping is an involution, so one table encodes and decodes */
	for (b = 0; b < 256; b++)
		CHECK(atr_inverse(atr_inverse((uint8_t)b)) == b);
	CHECK(atr_inverse(0x00) == 0xff && atr_inverse(0x01) == 0x7f);
}

static void test_t1_tck(void)
{
	uint8_t a[40];
	size_t len = unhex("3BF81300008131FE15597562696B657934D4", a);
	struct atr_info i;

	current_test = "t1_tck";
	CHECK(parse_incremental(a, len, &i) == ATR_COMPLETE);
	CHECK(len == 18);
	CHECK(i.protocols == (1u << 1));	/* T=0 not offered */
	CHECK(i.has_tck && i.tck_ok);
	CHECK(i.hist_off == 9 && i.hist_len == 8);
}

static void test_wi(void)
{
	uint8_t a[8];
	size_t len = unhex("3B804014", a);
	struct atr_info i;

	current_test = "wi";
	/* TD1=40: TC2 follows, T=0 -> WI 20 */
	CHECK(parse_incremental(a, len, &i) == ATR_COMPLETE);
	CHECK(i.wi == 20 && i.protocols == 1 && !i.has_tck);
	/* TC2 = 00 is reserved: default stays */
	len = unhex("3B804000", a);
	CHECK(atr_parse(a, len, &i) == ATR_COMPLETE && i.wi == 10);
	/* TD1=10: TA2 follows, T=0 -> specific mode */
	len = unhex("3B90111080", a);
	CHECK(atr_parse(a, len, &i) == ATR_COMPLETE && i.specific_mode && i.fi == 1);
}

static void test_invalid(void)
{
	uint8_t a[64];
	struct atr_info i;
	size_t n;

	current_test = "invalid";
	a[0] = 0x3a;
	CHECK(atr_parse(a, 1, &i) == ATR_INVALID);
	a[0] = 0x03;	/* raw inverse TS must be decoded before parsing */
	CHECK(atr_parse(a, 1, &i) == ATR_INVALID);
	CHECK(atr_parse(a, 0, &i) == ATR_NEED_MORE && i.expected == 1);
	/* TD chain that never ends: 3B 8F then TDi = 8F... runs past 33 */
	a[0] = 0x3b;
	for (n = 1; n < sizeof(a); n++)
		a[n] = 0x8f;
	CHECK(atr_parse(a, sizeof(a), &i) == ATR_INVALID);
	/* 15 historical bytes + many TDs: over the limit */
	CHECK(atr_parse(a, 20, &i) == ATR_INVALID);
}

int main(void)
{
	test_usim();
	test_euicc();
	test_t0_only();
	test_inverse();
	test_t1_tck();
	test_wi();
	test_invalid();
	return check_done("test_atr");
}
