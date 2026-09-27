/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * The RSPRO codec against BER built by hand from asn1/RSPRO.asn (IMPLICIT
 * TAGS; the RsproPDUchoice wrapped explicitly in [2]). Not captured from
 * a real osmo-remsim — there was none to capture from (2026-09-27).
 */
#include <string.h>

#include "rspro.h"
#include "check.h"

int main(void)
{
	uint8_t b[1024], big[300];
	struct rspro_pdu p;
	struct rspro_slot cs = { 5, 0 }, bs = { 1, 3 };
	size_t n, used, pln;
	const uint8_t *pl;
	int proto, ext;

	current_test = "connect_client_req";
	{
		static const uint8_t want[] = {
			0x30, 0x20, 0x80, 0x01, 0x02, 0x81, 0x01, 0x01,
			0xA2, 0x18, 0xA2, 0x16,
			0x30, 0x0C, 0x0A, 0x01, 0x00, 0x16, 0x01, 'a',
			0x80, 0x01, 'b', 0x81, 0x01, 'c',
			0x30, 0x06, 0x02, 0x01, 0x05, 0x02, 0x01, 0x00,
		};

		n = rspro_enc_connect_client_req(b, sizeof(b), 1, &cs, "a", "b", "c");
		CHECK(n == sizeof(want) && !memcmp(b, want, n));
		/* too small a buffer: nothing, not a cut message */
		CHECK(rspro_enc_connect_client_req(b, 20, 1, &cs, "a", "b", "c") == 0);
	}

	current_test = "config_client_bank_req";
	{
		static const uint8_t in[] = {
			0x30, 0x1E, 0x80, 0x01, 0x02, 0x81, 0x01, 0x07,
			0xA2, 0x16, 0xAA, 0x14,
			0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x03,
			0x30, 0x0A, 0x80, 0x04, 10, 0, 0, 2, 0x02, 0x02, 0x27, 0x0F,
		};

		CHECK(rspro_decode(in, sizeof(in), &p) == 0);
		CHECK(p.version == 2 && p.tag == 7 && p.msg == RSPRO_CONFIG_CLIENT_BANK_REQ);
		CHECK(p.has_bank && p.bank.id == 1 && p.bank.nr == 3);
		CHECK(p.ip_len == 4 && p.ip[0] == 10 && p.ip[3] == 2 && p.port == 9999);
		/* cut short anywhere: malformed, never a partial message */
		CHECK(rspro_decode(in, sizeof(in) - 1, &p) < 0);
		CHECK(rspro_decode(in, 10, &p) < 0);
	}

	current_test = "connect_client_res";
	{
		static const uint8_t in[] = {
			0x30, 0x1F, 0x80, 0x01, 0x02, 0x81, 0x01, 0x02,
			0xA2, 0x17, 0xA3, 0x15,
			0x30, 0x10, 0x0A, 0x01, 0x01, 0x16, 0x03, 's', 'r', 'v',
			0x80, 0x03, 'r', 'e', 'm', 0x81, 0x01, '1',
			0x0A, 0x01, 0x06,
		};

		CHECK(rspro_decode(in, sizeof(in), &p) == 0);
		CHECK(p.msg == RSPRO_CONNECT_CLIENT_RES && p.result == RSPRO_RES_IDENTITY_IN_USE);
		CHECK(p.comp_type == RSPRO_COMP_SERVER && !strcmp(p.comp_name, "srv") &&
		      !strcmp(p.comp_software, "rem") && !strcmp(p.comp_version, "1"));
	}

	current_test = "tpdu_card_to_modem";
	{
		/* tag 128 needs its leading zero; an extension field the
		 * module may grow ([5] here) is skipped */
		static const uint8_t in[] = {
			0x30, 0x2D, 0x80, 0x01, 0x02, 0x81, 0x02, 0x00, 0x80,
			0xA2, 0x24, 0xB0, 0x22,
			0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x03,
			0x30, 0x06, 0x02, 0x01, 0x05, 0x02, 0x01, 0x00,
			0x30, 0x0C, 0x01, 0x01, 0xFF, 0x01, 0x01, 0xFF, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00,
			0x04, 0x02, 0x90, 0x00,
		};
		uint8_t ext[sizeof(in) + 3];

		CHECK(rspro_decode(in, sizeof(in), &p) == 0);
		CHECK(p.tag == 128 && p.msg == RSPRO_TPDU_CARD_TO_MODEM);
		CHECK(p.bank.id == 1 && p.bank.nr == 3 && p.client.id == 5 && p.client.nr == 0);
		CHECK(p.final_part && p.data_len == 2 && p.data[0] == 0x90 && p.data[1] == 0x00);

		memcpy(ext, in, sizeof(in));
		ext[1] += 3;
		ext[10] += 3;
		ext[12] += 3;
		ext[sizeof(in)] = 0x85;
		ext[sizeof(in) + 1] = 0x01;
		ext[sizeof(in) + 2] = 0x00;
		CHECK(rspro_decode(ext, sizeof(ext), &p) == 0 && p.data_len == 2);
	}

	current_test = "tpdu_modem_to_card";
	{
		size_t i;

		/* 261 bytes: the long length form, and a round trip */
		for (i = 0; i < sizeof(big); i++)
			big[i] = (uint8_t)i;
		n = rspro_enc_tpdu_modem_to_card(b, sizeof(b), 0x12345678, &cs, &bs, big, 261);
		CHECK(n > 261);
		CHECK(b[0] == 0x30 && b[1] == 0x82);
		CHECK(rspro_decode(b, n, &p) == 0);
		CHECK(p.tag == 0x12345678 && p.msg == RSPRO_TPDU_MODEM_TO_CARD);
		CHECK(p.client.id == 5 && p.bank.id == 1 && p.bank.nr == 3);
		CHECK(p.final_part && p.data_len == 261 && !memcmp(p.data, big, 261));
		/* a tag with the top bit set stays positive */
		n = rspro_enc_tpdu_modem_to_card(b, sizeof(b), 0xFFFFFFFFu, &cs, &bs, big, 5);
		CHECK(rspro_decode(b, n, &p) == 0 && p.tag == 0xFFFFFFFFu);
	}

	current_test = "set_atr_req";
	{
		static const uint8_t in[] = {
			0x30, 0x16, 0x80, 0x01, 0x02, 0x81, 0x01, 0x09,
			0xA2, 0x0E, 0xAD, 0x0C,
			0x30, 0x06, 0x02, 0x01, 0x05, 0x02, 0x01, 0x00,
			0x04, 0x02, 0x3B, 0x00,
		};

		CHECK(rspro_decode(in, sizeof(in), &p) == 0);
		CHECK(p.msg == RSPRO_SET_ATR_REQ && p.data_len == 2 && p.data[0] == 0x3B);
		CHECK(p.has_client && p.client.id == 5);
	}

	current_test = "result_res";
	{
		static const uint8_t want[] = {
			0x30, 0x0D, 0x80, 0x01, 0x02, 0x81, 0x01, 0x09,
			0xA2, 0x05, 0xAE, 0x03, 0x0A, 0x01, 0x00,
		};

		n = rspro_enc_result_res(b, sizeof(b), RSPRO_SET_ATR_RES, 9, RSPRO_RES_OK);
		CHECK(n == sizeof(want) && !memcmp(b, want, n));
		CHECK(rspro_decode(b, n, &p) == 0 && p.msg == RSPRO_SET_ATR_RES && p.result == 0);
	}

	current_test = "slot_status";
	n = rspro_enc_client_slot_status_ind(b, sizeof(b), 3, &cs, &bs, true, false, false, true);
	CHECK(rspro_decode(b, n, &p) == 0 && p.msg == RSPRO_CLIENT_SLOT_STATUS_IND);
	{
		static const uint8_t phys[] = { 0x30, 0x0C, 0x80, 0x01, 0xFF, 0x81, 0x01, 0x00,
						0x82, 0x01, 0x00, 0x83, 0x01, 0xFF };

		CHECK(n > sizeof(phys) && !memcmp(b + n - sizeof(phys), phys, sizeof(phys)));
	}

	current_test = "malformed";
	{
		static const uint8_t indef[] = { 0x30, 0x80, 0x00, 0x00 };
		static const uint8_t neg[] = { 0x30, 0x06, 0x80, 0x01, 0x02, 0x81, 0x01, 0xFF };
		static const uint8_t nomsg[] = { 0x30, 0x06, 0x80, 0x01, 0x02, 0x81, 0x01, 0x01 };

		CHECK(rspro_decode(indef, sizeof(indef), &p) < 0);
		CHECK(rspro_decode(neg, sizeof(neg), &p) < 0);
		CHECK(rspro_decode(nomsg, sizeof(nomsg), &p) < 0);
	}

	current_test = "ipa";
	{
		static const uint8_t ping[] = { 0x00, 0x01, 0xFE, 0x00 };

		n = rspro_enc_result_res(b + 4, sizeof(b) - 4, RSPRO_SET_ATR_RES, 9, 0);
		CHECK(rspro_ipa_wrap(b, n) == n + 4);
		CHECK(b[0] == 0 && b[1] == n + 1 && b[2] == 0xEE && b[3] == 0x07);
		CHECK(rspro_ipa_parse(b, n + 3, &used, &proto, &ext, &pl, &pln) == 0);
		CHECK(rspro_ipa_parse(b, n + 4, &used, &proto, &ext, &pl, &pln) == 1);
		CHECK(used == n + 4 && proto == 0xEE && ext == 0x07 && pl == b + 4 && pln == n);
		CHECK(rspro_ipa_parse(ping, sizeof(ping), &used, &proto, &ext, &pl, &pln) == 1);
		CHECK(proto == 0xFE && ext == -1 && pln == 1 && pl[0] == 0);
		b[0] = 0xFF;
		CHECK(rspro_ipa_parse(b, n + 4, &used, &proto, &ext, &pl, &pln) < 0);
	}
	return check_done("test_rspro");
}
