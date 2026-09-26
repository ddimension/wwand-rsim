/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <string.h>

#include "sap.h"
#include "check.h"

int main(void)
{
	uint8_t b[64];
	struct sap_msg m;
	int n;

	current_test = "encode";
	{
		static const uint8_t want[] = { 0x00, 0x01, 0, 0, 0x00, 0, 0x00, 0x02, 0x04, 0x00, 0, 0 };
		uint8_t sz[2] = { 0x04, 0x00 };
		struct sap_param p = { SAP_P_MAX_MSG_SIZE, 2, sz };

		n = sap_encode(b, sizeof(b), SAP_CONNECT_REQ, &p, 1);
		CHECK(n == (int)sizeof(want) && !memcmp(b, want, sizeof(want)));
		/* a request without parameters is its header */
		CHECK(sap_encode(b, sizeof(b), SAP_TRANSFER_ATR_REQ, NULL, 0) == 4 && b[0] == 0x07 && b[1] == 0);
		/* does not fit */
		CHECK(sap_encode(b, 10, SAP_CONNECT_REQ, &p, 1) == -1);
	}
	{
		/* 5 value bytes: padded to 8 with zeros */
		static const uint8_t apdu[] = { 0xA0, 0xA4, 0x00, 0x00, 0x02 };
		struct sap_param p = { SAP_P_COMMAND_APDU, 5, apdu };

		memset(b, 0xee, sizeof(b));
		n = sap_encode(b, sizeof(b), SAP_TRANSFER_APDU_REQ, &p, 1);
		CHECK(n == 16 && b[7] == 5 && !memcmp(b + 8, apdu, 5) && !b[13] && !b[14] && !b[15]);
	}

	current_test = "frame";
	{
		/* TRANSFER_APDU_RESP: ResultCode 0, ResponseAPDU 90 00 */
		static const uint8_t rsp[] = { 0x06, 0x02, 0, 0, 0x02, 0, 0x00, 0x01, 0x00, 0, 0, 0,
					       0x05, 0, 0x00, 0x02, 0x90, 0x00, 0, 0 };
		size_t i;
		int partial_ok = 1;

		for (i = 0; i < sizeof(rsp); i++)
			if (sap_frame(rsp, i) != 0)
				partial_ok = 0;
		CHECK(partial_ok);
		CHECK(sap_frame(rsp, sizeof(rsp)) == (int)sizeof(rsp));
		CHECK(sap_decode(rsp, sizeof(rsp), &m) == 0 && m.id == SAP_TRANSFER_APDU_RESP && m.n == 2);
		CHECK(sap_get_u8(&m, SAP_P_RESULT_CODE) == 0);
		CHECK(sap_get(&m, SAP_P_RESPONSE_APDU) && sap_get(&m, SAP_P_RESPONSE_APDU)->len == 2 &&
		      sap_get(&m, SAP_P_RESPONSE_APDU)->val[0] == 0x90);
		CHECK(sap_get(&m, SAP_P_ATR) == NULL && sap_get_u8(&m, SAP_P_ATR) == -1);
		/* two messages back to back: the first one's length */
		memcpy(b, rsp, sizeof(rsp));
		memcpy(b + sizeof(rsp), rsp, 4);
		CHECK(sap_frame(b, sizeof(rsp) + 4) == (int)sizeof(rsp));
	}
	{
		static const uint8_t bad[] = { 0x11, 0x09, 0, 0 };

		CHECK(sap_frame(bad, sizeof(bad)) == -1);
		CHECK(sap_decode(bad, sizeof(bad), &m) == -1);
	}
	CHECK(!strcmp(sap_msg_name(SAP_STATUS_IND), "STATUS_IND") && !strcmp(sap_msg_name(0x40), "?"));
	CHECK(!strcmp(sap_result_name(SAP_RC_REMOVED), "card removed"));

	current_test = "sdp request";
	{
		static const uint8_t want[] = { 0x06, 0x00, 0x01, 0x00, 0x0d, 0x35, 0x03, 0x19, 0x11, 0x2d,
						0x04, 0x00, 0x35, 0x03, 0x09, 0x00, 0x04, 0x00 };
		static const uint8_t cont[] = { 0xAA, 0xBB };

		n = sdp_search_req(b, sizeof(b), 1, SAP_UUID16, NULL, 0);
		CHECK(n == (int)sizeof(want) && !memcmp(b, want, sizeof(want)));
		n = sdp_search_req(b, sizeof(b), 2, SAP_UUID16, cont, 2);
		CHECK(n == 20 && b[4] == 0x0f && b[17] == 2 && b[18] == 0xAA && b[19] == 0xBB);
	}

	current_test = "sdp answer";
	{
		/* DES { DES { 0x0004: DES { DES { L2CAP }, DES { RFCOMM, 8 } } } } */
		static const uint8_t rsp[] = {
			0x07, 0x00, 0x01, 0x00, 0x18, 0x00, 0x15,
			0x35, 0x13, 0x35, 0x11, 0x09, 0x00, 0x04, 0x35, 0x0c,
			0x35, 0x03, 0x19, 0x01, 0x00,
			0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x08,
			0x00,
		};
		const uint8_t *a, *c;
		size_t al, cl;

		CHECK(sdp_search_rsp(rsp, sizeof(rsp), 1, &a, &al, &c, &cl) == 0 && al == 0x15 && cl == 0);
		CHECK(sdp_rfcomm_channel(a, al) == 8);
		/* another transaction's answer */
		CHECK(sdp_search_rsp(rsp, sizeof(rsp), 2, &a, &al, &c, &cl) == -1);
		/* cut short */
		CHECK(sdp_search_rsp(rsp, sizeof(rsp) - 3, 1, &a, &al, &c, &cl) == -1);
		/* a truncated element list is no channel */
		CHECK(sdp_rfcomm_channel(a, al - 1) == -1);
	}
	{
		/* the RFCOMM UUID in its 128-bit form, a DES with a 2-byte length */
		static const uint8_t attrs[] = {
			0x36, 0x00, 0x1b, 0x09, 0x00, 0x04, 0x35, 0x16,
			0x1c, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x10, 0x00,
			0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb, 0x08, 0x0c,
			0x09, 0x00, 0x01,
		};

		CHECK(sdp_rfcomm_channel(attrs, sizeof(attrs)) == 12);
	}
	{
		/* L2CAP only, and a channel number after something else than
		 * RFCOMM: no channel */
		static const uint8_t attrs[] = { 0x35, 0x07, 0x19, 0x01, 0x00, 0x08, 0x05, 0x08, 0x06 };

		CHECK(sdp_rfcomm_channel(attrs, sizeof(attrs)) == -1);
		CHECK(sdp_rfcomm_channel(attrs, 0) == -1);
	}

	return check_done("test_sap");
}
