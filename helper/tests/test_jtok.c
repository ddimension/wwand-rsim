/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <string.h>

#include "jtok.h"
#include "check.h"

int main(void)
{
	/* the shape of remsim-server's REST answers */
	static const char js[] =
		"{\"banks\": [ {\"peer\":\"b1\", \"component_id\": {\"name\":\"bank-a\", \"x\":[1,2]},"
		" \"bankId\": 1, \"numberOfSlots\": 8}, {\"bankId\":2,\"numberOfSlots\":0} ], \"n\": -3}";
	struct jtok t[64];
	char s[32];
	int n, arr, e, c;

	current_test = "parse";
	n = jtok_parse(js, strlen(js), t, 64);
	CHECK(n > 0 && t[0].type == JT_OBJ && t[0].size == 2);
	arr = jtok_get(js, t, n, 0, "banks");
	CHECK(arr > 0 && t[arr].type == JT_ARR && t[arr].size == 2);
	e = jtok_elem(t, n, arr, 0);
	CHECK(e > 0 && t[e].size == 4);
	c = jtok_get(js, t, n, e, "component_id");
	jtok_str(js, t, jtok_get(js, t, n, c, "name"), s, sizeof(s));
	CHECK(!strcmp(s, "bank-a"));
	CHECK(jtok_long(js, t, jtok_get(js, t, n, e, "numberOfSlots"), -1) == 8);
	/* past a nested array inside the element */
	e = jtok_elem(t, n, arr, 1);
	CHECK(jtok_long(js, t, jtok_get(js, t, n, e, "bankId"), -1) == 2);
	CHECK(jtok_long(js, t, jtok_get(js, t, n, 0, "n"), 0) == -3);
	CHECK(jtok_get(js, t, n, 0, "missing") < 0);
	CHECK(jtok_elem(t, n, arr, 2) < 0);
	/* a key's text as a value is not the key */
	n = jtok_parse("{\"a\":\"bankId\",\"bankId\":4}", 25, t, 64);
	CHECK(jtok_long("{\"a\":\"bankId\",\"bankId\":4}", t, jtok_get("{\"a\":\"bankId\",\"bankId\":4}", t, n, 0, "bankId"), -1) == 4);

	current_test = "malformed";
	CHECK(jtok_parse("{\"a\":[1,2}", 10, t, 64) < 0);
	CHECK(jtok_parse("{\"a\":\"x", 7, t, 64) < 0);
	CHECK(jtok_parse("{1:2}", 5, t, 64) < 0);
	CHECK(jtok_parse(js, strlen(js), t, 4) < 0);
	/* the grammar, not a guess at it: each of these was read as a
	 * document by a tokenizer that skipped colons and commas */
	current_test = "grammar";
	{
		static const char *const bad[] = {
			"{\"a\" 1}",			/* no colon */
			"{\"a\":1 \"b\":2}",		/* no comma */
			"[1 2]",
			"{\"a\":}",			/* a key without its value */
			"{\"a\"}",
			"{\"a\":1,}",			/* a comma after the last */
			"[1,]",
			"[,1]",
			"{,}",
			"{\"a\"::1}",
			"{\"a\":1}{}",			/* two roots */
			"{} x",
			"",
			"   ",
			"\"a\x01b\"",			/* a control character */
			"\"a\nb\"",
			"\"\\q\"",			/* an escape JSON has not */
			"\"\\u12G4\"",
			"\"\\u12\"",
			"tru",				/* primitives spelled wrongly */
			"nul",
			"True",
			"01",
			"-",
			"1.",
			".5",
			"1e",
			"+1",
			"0x10",
			"{\"a\":bankId}",
			"[1:2]",
			"{\"a\",1}",
			NULL,
		};
		static const char *const good[] = {
			"{}", "[]", " [ ] ", "0", "-0.5e+3", "\"\\u00e9\\n\\\"\"", "[true,false,null]",
			"{\"a\":{\"b\":[1,{\"c\":\"d\"}]},\"e\":-12}", NULL,
		};
		int i;

		for (i = 0; bad[i]; i++) {
			int r = jtok_parse(bad[i], strlen(bad[i]), t, 64);

			CHECK(r < 0);
			if (r >= 0)
				fprintf(stderr, "  taken: %s\n", bad[i]);
		}
		for (i = 0; good[i]; i++) {
			int r = jtok_parse(good[i], strlen(good[i]), t, 64);

			CHECK(r > 0);
			if (r <= 0)
				fprintf(stderr, "  refused: %s\n", good[i]);
		}
		/* sizes as before: members of an object, elements of an array */
		n = jtok_parse("{\"a\":[1,[2,3],{}],\"b\":{}}", strlen("{\"a\":[1,[2,3],{}],\"b\":{}}"), t, 64);
		CHECK(n == 10 && t[0].size == 2 && t[2].size == 3 && t[4].size == 2 && t[9].size == 0);
		/* nesting has its limit */
		{
			char deep[64];

			memset(deep, '[', 40);
			memset(deep + 40, ']', 20);
			CHECK(jtok_parse(deep, 40, t, 64) < 0);
		}
	}

	/* a zero-sized buffer is not written to */
	current_test = "str_cap0";
	{
		char guard[2] = { 'x', 'y' };

		n = jtok_parse("\"abc\"", 5, t, 64);
		jtok_str("\"abc\"", t, 0, guard + 1, 0);
		CHECK(guard[1] == 'y');
		jtok_str("\"abc\"", t, 0, guard, 1);
		CHECK(guard[0] == '\0' && guard[1] == 'y');
	}
	return check_done("test_jtok");
}
