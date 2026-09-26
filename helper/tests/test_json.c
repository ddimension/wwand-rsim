/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <string.h>

#include "json.h"
#include "check.h"

int main(void)
{
	char v[64];
	uint8_t b[8];

	current_test = "object";
	CHECK(json_is_object("{\"op\":\"status\"}"));
	CHECK(json_is_object("  { \"op\" : \"status\" }  \r"));
	CHECK(!json_is_object("\"op\":\"status\""));
	CHECK(!json_is_object("{\"op\":\"status\""));
	CHECK(!json_is_object(""));

	current_test = "get";
	CHECK(json_get_string("{\"op\":\"tpdu\",\"data\":\"A0A4\"}", "data", v, sizeof(v)) == 1 &&
	      !strcmp(v, "A0A4"));
	CHECK(json_get_string("{ \"op\" :  \"tpdu\" }", "op", v, sizeof(v)) == 1 &&
	      !strcmp(v, "tpdu"));
	/* the key's text as a value earlier in the line is not the key */
	CHECK(json_get_string("{\"data\":\"op\",\"op\":\"status\"}", "op", v, sizeof(v)) == 1 &&
	      !strcmp(v, "status"));
	/* a key that only shares a prefix */
	CHECK(json_get_string("{\"opx\":\"a\"}", "op", v, sizeof(v)) == 0);
	CHECK(json_get_string("{\"op\":\"status\"}", "data", v, sizeof(v)) == 0);
	CHECK(json_get_string("{\"op\":5}", "op", v, sizeof(v)) == -1);
	CHECK(json_get_string("{\"op\":\"st\\\"x\"}", "op", v, sizeof(v)) == -1);
	CHECK(json_get_string("{\"op\":\"unterminated}", "op", v, sizeof(v)) == -1);
	CHECK(json_get_string("{\"op\":\"0123456789\"}", "op", v, 5) == -1);

	current_test = "hex";
	CHECK(hex_decode("a0A4ff", b, sizeof(b)) == 3 && b[0] == 0xa0 && b[2] == 0xff);
	CHECK(hex_decode("", b, sizeof(b)) == 0);
	CHECK(hex_decode("A0A", b, sizeof(b)) == -1);
	CHECK(hex_decode("A0G4", b, sizeof(b)) == -1);
	CHECK(hex_decode("A0 A4", b, sizeof(b)) == -1);
	CHECK(hex_decode("0x00", b, sizeof(b)) == -1);
	CHECK(hex_decode("000000000000000000", b, sizeof(b)) == -1);
	return check_done("test_json");
}
