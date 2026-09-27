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
	return check_done("test_jtok");
}
