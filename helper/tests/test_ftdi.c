/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <string.h>

#include "ftdi_proto.h"
#include "check.h"

/*
 * The expected divisors are what ftdi_232bm_baud_base_to_divisor(baud,
 * 48000000) in drivers/usb/serial/ftdi_sio.c (linux 6.18.41) returns,
 * worked out by hand from that function so the test does not share the
 * implementation it checks:
 *   9600:    24e6/9600    = 2500    -> 312 + 4/8  -> 312 | code(4)=1 << 14 = 0x04138
 *   9624:    24e6/9624    = 2493.77 -> 2494 = 311 + 6/8 -> 311 | 6 << 14 = 0x18137
 *   16129:   24e6/16129   = 1488.0  -> 186 + 0     = 0x000ba
 *   115200:  24e6/115200  = 208.33  -> 208 = 26 + 0 = 0x0001a
 *   10753:   24e6/10753   = 2231.94 -> 2232 = 279 + 0 ... = 0x00117
 *   2 Mbaud: 12 = 1 + 4/8 -> 0x4001 -> special code 1
 *   3 Mbaud: 8 = 1 + 0    -> 1      -> special code 0
 */
static void test_divisor(void)
{
	static const struct { unsigned baud; uint32_t div; } v[] = {
		{ 9600, 0x04138 }, { 9624, 0x18137 }, { 16129, 0x000ba },
		{ 115200, 0x0001a }, { 10753, 0x00117 },
		{ 2000000, 0x00001 }, { 3000000, 0x00000 },
		/* 24e6/9620 = 2494.8 -> 2495 = 311 + 7/8 -> code 7 */
		{ 9620, 0x1c137 },
		/* 24e6/300 = 80000 = 10000 + 0 */
		{ 300, 0x02710 },
	};
	size_t i;
	uint32_t d;

	current_test = "divisor";
	for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		d = 0xdeadbeef;
		CHECK(ftdi_bm_divisor(v[i].baud, &d) == 0);
		if (d != v[i].div)
			fprintf(stderr, "  %u baud: divisor %05x, want %05x\n",
				v[i].baud, (unsigned)d, (unsigned)v[i].div);
		CHECK(d == v[i].div);
	}
	/* wValue / wIndex split of a divisor with the high code bit */
	CHECK((uint16_t)0x18137 == 0x8137 && (uint16_t)(0x18137 >> 16) == 1);
	/* outside what 14 integer bits can express */
	CHECK(ftdi_bm_divisor(100, &d) == -1);
	CHECK(ftdi_bm_divisor(3000001, &d) == -1);
	CHECK(ftdi_bm_divisor(0, &d) == -1);
}

static void test_rate(void)
{
	uint32_t d;

	current_test = "rate";
	CHECK(ftdi_bm_rate(0) == 3000000);
	CHECK(ftdi_bm_rate(1) == 2000000);
	CHECK(ftdi_bm_rate(0x04138) == 9600);
	/* 24e6 / 2494 = 9623.1: the Smartmouse at 3.58 MHz asks for 9624 */
	CHECK(ftdi_bm_rate(0x18137) == 9623);
	CHECK(ftdi_bm_rate(0x000ba) == 16129);
	/* across the card rates: the achieved rate encodes back to the same
	 * divisor, and is off by at most half an eighth of the divisor
	 * d8 = 24e6/b, i.e. |got - b| <= b / (2 * d8) = b^2 / 48e6, plus one
	 * for rounding got to whole baud */
	for (unsigned b = 9000; b < 17000; b += 7) {
		uint32_t d2;
		unsigned got;

		CHECK(ftdi_bm_divisor(b, &d) == 0);
		got = ftdi_bm_rate(d);
		CHECK(ftdi_bm_divisor(got, &d2) == 0 && d2 == d);
		CHECK((unsigned long long)(got > b ? got - b : b - got) * 48000000ull <=
		      (unsigned long long)b * b + 48000000ull);
	}
}

static void test_parse(void)
{
	uint8_t in[200], out[200];
	struct ftdi_rx_stat st;
	size_t n;

	current_test = "parse";
	/* one short packet: status 31 60 (CTS, THRE|TEMT) + two bytes */
	memset(&st, 0, sizeof(st));
	memcpy(in, "\x31\x60\xA4\x9F", 4);
	n = ftdi_rx_parse(in, 4, 64, true, out, &st);
	CHECK(n == 2 && out[0] == 0xA4 && out[1] == 0x9F);
	CHECK(st.modem == 0x31);

	/* status only: nothing, no error counted even with error bits */
	memset(&st, 0, sizeof(st));
	memcpy(in, "\x01\x0C", 2);
	n = ftdi_rx_parse(in, 2, 64, true, out, &st);
	CHECK(n == 0 && st.parity == 0 && st.framing == 0);

	/* a full 64-byte packet then a short one: the status bytes of BOTH
	 * are stripped, not just the transfer's first two */
	memset(&st, 0, sizeof(st));
	in[0] = 0x01; in[1] = 0x60;
	for (n = 2; n < 64; n++)
		in[n] = (uint8_t)n;
	in[64] = 0x11; in[65] = 0x60; in[66] = 0xEE;
	n = ftdi_rx_parse(in, 67, 64, true, out, &st);
	CHECK(n == 63);
	CHECK(out[0] == 2 && out[61] == 63 && out[62] == 0xEE);
	CHECK(st.modem == 0x11);

	/* parity error: the packet's data goes when dropping, stays when not */
	memset(&st, 0, sizeof(st));
	memcpy(in, "\x01\x64\x42\x43" "\x01\x60\x90", 7);
	n = ftdi_rx_parse(in, 7, 4, true, out, &st);
	CHECK(n == 1 && out[0] == 0x90);
	CHECK(st.parity == 1 && st.dropped == 2);
	memset(&st, 0, sizeof(st));
	n = ftdi_rx_parse(in, 7, 4, false, out, &st);
	CHECK(n == 3 && out[0] == 0x42 && out[2] == 0x90);
	CHECK(st.parity == 1 && st.dropped == 0);

	/* framing error is dropped like parity (IGNPAR covers both);
	 * overrun alone only counts */
	memset(&st, 0, sizeof(st));
	memcpy(in, "\x01\x68\x42" "\x01\x62\x43", 6);
	n = ftdi_rx_parse(in, 6, 3, true, out, &st);
	CHECK(n == 1 && out[0] == 0x43);
	CHECK(st.framing == 1 && st.overrun == 1 && st.dropped == 1);

	/* a trailing fragment shorter than the status bytes is ignored */
	memset(&st, 0, sizeof(st));
	memcpy(in, "\x01\x60\x55" "\x01", 4);
	n = ftdi_rx_parse(in, 4, 3, true, out, &st);
	CHECK(n == 1 && out[0] == 0x55);

	/* nothing in, nothing out */
	CHECK(ftdi_rx_parse(in, 0, 64, true, out, &st) == 0);
}

int main(void)
{
	test_divisor();
	test_rate();
	test_parse();
	return check_done("test_ftdi");
}
