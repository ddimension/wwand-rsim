/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <string.h>

#include "json.h"
#include "rsim.h"
#include "t0.h"
#include "check.h"

/*
 * A scripted card. The script is a sequence of steps, each "T:<hex>" (the
 * engine must send exactly these bytes next), "R:<hex>" (the card sends
 * these, one per recv) or "X" (the next recv times out). Order is
 * enforced: a recv while the script expects a send is a failure, so an
 * engine that reads before it writes cannot pass by accident.
 */

#define MAXSTEP 16

struct step {
	char kind;		/* 'T', 'R', 'X' */
	uint8_t b[300];
	size_t len, pos;
};

struct fake {
	struct step s[MAXSTEP];
	int n, cur;
	int errors;
	unsigned min_timeout, max_timeout;
};

static void load(struct fake *f, const char *script)
{
	char buf[1024], *tok, *save = NULL;

	memset(f, 0, sizeof(*f));
	strncpy(buf, script, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
		struct step *st = &f->s[f->n++];
		int n;

		st->kind = tok[0];
		if (st->kind == 'X')
			continue;
		n = hex_decode(tok + 2, st->b, sizeof(st->b));
		CHECK(n > 0);
		st->len = n > 0 ? (size_t)n : 0;
	}
}

/* appends n copies of byte b to the last step (for 256-byte bodies) */
static void fill(struct fake *f, uint8_t b, size_t n)
{
	struct step *st = &f->s[f->n - 1];

	while (n--)
		st->b[st->len++] = b++;
}

static void add(struct fake *f, const char *script)
{
	struct fake tmp;
	int i;

	load(&tmp, script);
	for (i = 0; i < tmp.n; i++)
		f->s[f->n++] = tmp.s[i];
}

static int f_send(void *ctx, const uint8_t *buf, size_t len)
{
	struct fake *f = ctx;
	size_t i;

	for (i = 0; i < len; i++) {
		struct step *st = f->cur < f->n ? &f->s[f->cur] : NULL;

		if (!st || st->kind != 'T' || st->b[st->pos] != buf[i]) {
			fprintf(stderr, "  %s: unexpected send byte %02X (step %d)\n",
				current_test, buf[i], f->cur);
			f->errors++;
			return RSIM_E_IO;
		}
		if (++st->pos == st->len)
			f->cur++;
	}
	return RSIM_OK;
}

static int f_recv(void *ctx, uint8_t *b, unsigned timeout_ms)
{
	struct fake *f = ctx;
	struct step *st = f->cur < f->n ? &f->s[f->cur] : NULL;

	if (!f->min_timeout || timeout_ms < f->min_timeout)
		f->min_timeout = timeout_ms;
	if (timeout_ms > f->max_timeout)
		f->max_timeout = timeout_ms;
	if (st && st->kind == 'X') {
		f->cur++;
		return RSIM_E_TIMEOUT;
	}
	if (!st || st->kind != 'R') {
		fprintf(stderr, "  %s: recv with no card byte due (step %d)\n",
			current_test, f->cur);
		f->errors++;
		return RSIM_E_TIMEOUT;
	}
	*b = st->b[st->pos];
	if (++st->pos == st->len)
		f->cur++;
	return RSIM_OK;
}

static uint8_t resp[RSIM_RESP_MAX];
static size_t resp_len;
static char detail[RSIM_DETAIL_MAX];

/* runs tpdu against the script; the script must be used up exactly */
static int run(struct fake *f, const char *tpdu_hex)
{
	uint8_t tpdu[300];
	struct t0_chan ch = { f_send, f_recv, f };
	int n = hex_decode(tpdu_hex, tpdu, sizeof(tpdu));
	int r;

	detail[0] = '\0';
	r = t0_transceive(&ch, 1234, tpdu, n < 0 ? 0 : (size_t)n, resp, &resp_len,
			  detail, sizeof(detail));
	CHECK(f->errors == 0);
	CHECK(f->cur == f->n);
	return r;
}

static int resp_is(const char *hex)
{
	uint8_t want[RSIM_RESP_MAX];
	int n = hex_decode(hex, want, sizeof(want));

	return n >= 0 && (size_t)n == resp_len && !memcmp(want, resp, resp_len);
}

static struct fake f;

static void test_case1(void)
{
	current_test = "case1";
	/* 4 bytes: sent with P3=00, the card answers SW at once */
	load(&f, "T:A004000000 R:9000");
	CHECK(run(&f, "A0040000") == RSIM_OK && resp_is("9000"));
	/* the same with an explicit P3 */
	load(&f, "T:A004000000 R:9000");
	CHECK(run(&f, "A004000000") == RSIM_OK && resp_is("9000"));
}

static void test_case2_256(void)
{
	size_t i;

	current_test = "case2_256";
	/* P3=00 in a case-2 command means 256 bytes */
	load(&f, "T:A0B0000000 R:B0 R:00");
	f.s[f.n - 1].len = 0;
	fill(&f, 0x00, 256);
	add(&f, "R:9000");
	CHECK(run(&f, "A0B0000000") == RSIM_OK);
	CHECK(resp_len == 258);
	for (i = 0; i < 256; i++)
		CHECK(resp[i] == (uint8_t)i);
	CHECK(resp[256] == 0x90 && resp[257] == 0x00);
	/* ... and every data byte */
	CHECK(f.min_timeout == 1234 && f.max_timeout == 1234);
}

static void test_case3(void)
{
	current_test = "case3";
	load(&f, "T:A0A4000002 R:A4 T:3F00 R:9F17");
	CHECK(run(&f, "A0A40000023F00") == RSIM_OK && resp_is("9F17"));
	/* hex of either case in */
	load(&f, "T:A0A4000002 R:A4 T:3F00 R:9F17");
	CHECK(run(&f, "a0a40000023f00") == RSIM_OK && resp_is("9F17"));
}

static void test_single_step(void)
{
	current_test = "single_step_out";
	/* ~INS (29 for D6): one byte at a time, then INS for the rest */
	load(&f, "T:A0D6000003 R:29 T:01 R:29 T:02 R:D6 T:03 R:9000");
	CHECK(run(&f, "A0D6000003010203") == RSIM_OK && resp_is("9000"));

	current_test = "single_step_in";
	load(&f, "T:A0B0000003 R:4F R:11 R:4F R:22 R:B0 R:33 R:9000");
	CHECK(run(&f, "A0B0000003") == RSIM_OK && resp_is("1122339000"));
}

static void test_null(void)
{
	current_test = "null";
	/* NULL before the ACK, between data and SW, several in a row */
	load(&f, "T:A0A4000002 R:6060 R:A4 T:3F00 R:606060 R:9F17");
	CHECK(run(&f, "A0A40000023F00") == RSIM_OK && resp_is("9F17"));
	/* the WWT bounds every wait: procedure bytes and SW2 */
	CHECK(f.min_timeout == 1234 && f.max_timeout == 1234);
}

static void test_passthrough(void)
{
	current_test = "61xx";
	/* 61xx is returned, no GET RESPONSE of our own */
	load(&f, "T:00A4000402 R:A4 T:3F00 R:611F");
	CHECK(run(&f, "00A40004023F00") == RSIM_OK && resp_is("611F"));

	current_test = "6Cxx";
	load(&f, "T:00B0000000 R:6C0A");
	CHECK(run(&f, "00B0000000") == RSIM_OK && resp_is("6C0A"));

	current_test = "early_sw";
	/* SW after part of the data: what arrived is kept, then SW */
	load(&f, "T:00B0000004 R:4F R:AA R:6282");
	CHECK(run(&f, "00B0000004") == RSIM_OK && resp_is("AA6282"));
}

static void test_timeouts(void)
{
	current_test = "timeout_proc";
	load(&f, "T:A0F2000016 X");
	CHECK(run(&f, "A0F2000016") == RSIM_E_TIMEOUT);

	current_test = "timeout_data";
	load(&f, "T:A0B0000004 R:B0 R:1122 X");
	CHECK(run(&f, "A0B0000004") == RSIM_E_TIMEOUT);

	current_test = "timeout_sw2";
	load(&f, "T:A0B0000004 R:90 X");
	CHECK(run(&f, "A0B0000004") == RSIM_E_TIMEOUT);
}

static void test_protocol(void)
{
	current_test = "bad_proc";
	load(&f, "T:A0A4000002 R:42");
	CHECK(run(&f, "A0A40000023F00") == RSIM_E_PROTOCOL);
	CHECK(strstr(detail, "42") != NULL);

	current_test = "ack_nothing_left";
	load(&f, "T:A0A4000002 R:A4 T:3F00 R:A4");
	CHECK(run(&f, "A0A40000023F00") == RSIM_E_PROTOCOL);

	current_test = "odd_ins";
	/* INS B1 is acknowledged with B1; B0 (the 1997 VPP variant of
	 * INS^01) is not an acknowledgement for it */
	load(&f, "T:00B1000002 R:B1 R:AABB R:9000");
	CHECK(run(&f, "00B1000002") == RSIM_OK && resp_is("AABB9000"));
	load(&f, "T:00B1000002 R:B0");
	CHECK(run(&f, "00B1000002") == RSIM_E_PROTOCOL);
}

static void test_bad_request(void)
{
	uint8_t big[262];
	struct t0_chan ch = { f_send, f_recv, &f };

	current_test = "bad_request";
	/* nothing may be sent for a request the engine refuses */
	load(&f, "");
	CHECK(run(&f, "A0A400") == RSIM_E_BAD_REQUEST);
	load(&f, "");
	CHECK(run(&f, "A0A40000033F00") == RSIM_E_BAD_REQUEST);	/* P3 3, 2 bytes */
	load(&f, "");
	CHECK(run(&f, "A0A40000013F00") == RSIM_E_BAD_REQUEST);	/* P3 1, 2 bytes */
	load(&f, "");
	CHECK(run(&f, "A0600000") == RSIM_E_BAD_REQUEST);	/* INS 6X */
	load(&f, "");
	CHECK(run(&f, "A0920000") == RSIM_E_BAD_REQUEST);	/* INS 9X */
	load(&f, "");
	/* P3 counts at most 255 data bytes, so no longer TPDU is consistent */
	memset(big, 0, sizeof(big));
	big[4] = 0xff;
	CHECK(t0_transceive(&ch, 1, big, sizeof(big), resp, &resp_len, NULL, 0) ==
	      RSIM_E_BAD_REQUEST);
	CHECK(f.errors == 0);
}

static void test_wwt(void)
{
	current_test = "wwt";
	/* 960 * 10 * 372 / 3.579 MHz = 997.8 ms, rounded up */
	CHECK(t0_wwt_ms(3579, 10) == 998);
	CHECK(t0_wwt_ms(6000, 10) == 596);
	CHECK(t0_wwt_ms(3579, 20) == 1996);
}

int main(void)
{
	test_case1();
	test_case2_256();
	test_case3();
	test_single_step();
	test_null();
	test_passthrough();
	test_timeouts();
	test_protocol();
	test_bad_request();
	test_wwt();
	return check_done("test_t0");
}
