/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_RSPRO_H
#define RSIM_RSPRO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * RSPRO, osmo-remsim's Remote SIM Protocol: an ASN.1 module (asn1/RSPRO.asn
 * in osmo-remsim, IMPLICIT TAGS, BER on the wire) carried in IPA frames over
 * TCP. Only what a remsim *client* sends and receives is here — encoded and
 * decoded by hand, so the helper has no asn1c runtime. Pure functions over
 * buffers: the sockets are remsim.c's.
 *
 * Written from the module and the osmo-remsim sources; no remsim-server or
 * remsim-bankd has been talked to yet (2026-09-27). The tests check the
 * encoder against the decoder and against hand-built BER, not against the
 * real thing.
 */

/* RsproPDU.version as osmo-remsim sends it (RSPRO_VERSION, rspro_util.h) */
#define RSPRO_VERSION 2

/* IPA here is ip.access's multiplex header (Abis over IP, which Osmocom
 * uses for all its TCP protocols) — not SGP.32's IoT Profile Assistant of
 * wwand-ipa; the two only share the letters.
 * IPA: u16 length (big endian, what follows the proto byte), u8 proto; the
 * OSMO proto carries one more byte, the extension, RSPRO's is 0x07
 * (libosmocore gsm/protocol/ipaccess.h). CCM is the IPA link's own
 * signalling: PING must be answered with PONG, or the peer's keepalive
 * drops the link. */
#define IPA_PROTO_CCM		0xFE
#define IPA_PROTO_OSMO		0xEE
#define IPA_EXT_RSPRO		0x07
#define IPA_CCM_PING		0x00
#define IPA_CCM_PONG		0x01
#define IPA_CCM_ID_GET		0x04
#define IPA_CCM_ID_RESP		0x05
#define IPA_CCM_ID_ACK		0x06
#define IPA_IDTAG_UNITNAME	0x01

/* the RsproPDUchoice alternatives, by their context tag */
enum rspro_msg {
	RSPRO_CONNECT_BANK_REQ = 0,
	RSPRO_CONNECT_BANK_RES = 1,
	RSPRO_CONNECT_CLIENT_REQ = 2,
	RSPRO_CONNECT_CLIENT_RES = 3,
	RSPRO_CREATE_MAPPING_REQ = 4,
	RSPRO_CREATE_MAPPING_RES = 5,
	RSPRO_REMOVE_MAPPING_REQ = 6,
	RSPRO_REMOVE_MAPPING_RES = 7,
	RSPRO_CONFIG_CLIENT_ID_REQ = 8,
	RSPRO_CONFIG_CLIENT_ID_RES = 9,
	RSPRO_CONFIG_CLIENT_BANK_REQ = 10,
	RSPRO_CONFIG_CLIENT_BANK_RES = 11,
	RSPRO_ERROR_IND = 12,
	RSPRO_SET_ATR_REQ = 13,
	RSPRO_SET_ATR_RES = 14,
	RSPRO_TPDU_MODEM_TO_CARD = 15,
	RSPRO_TPDU_CARD_TO_MODEM = 16,
	RSPRO_CLIENT_SLOT_STATUS_IND = 17,
	RSPRO_BANK_SLOT_STATUS_IND = 18,
	RSPRO_RESET_STATE_REQ = 19,
	RSPRO_RESET_STATE_RES = 20,
};

/* ResultCode, the values a client acts on */
#define RSPRO_RES_OK			0
#define RSPRO_RES_ILLEGAL_CLIENT_ID	1
#define RSPRO_RES_ILLEGAL_BANK_ID	2
#define RSPRO_RES_ILLEGAL_SLOT_ID	3
#define RSPRO_RES_UNSUPPORTED_VERSION	4
#define RSPRO_RES_UNKNOWN_SLOTMAP	5
#define RSPRO_RES_IDENTITY_IN_USE	6
#define RSPRO_RES_CARD_NOT_PRESENT	100
#define RSPRO_RES_CARD_UNRESPONSIVE	101
#define RSPRO_RES_CARD_TRANSMISSION_ERR	102

/* ComponentType */
#define RSPRO_COMP_CLIENT	0
#define RSPRO_COMP_SERVER	1
#define RSPRO_COMP_BANKD	2

/* the largest RSPRO message accepted from the wire; the biggest a client
 * sees is a TpduCardToModem with 258 bytes, far below */
#define RSPRO_MSG_MAX		4096
/* data carried in one message: an ATR (1..55) or a TPDU either way */
#define RSPRO_DATA_MAX		1024
#define RSPRO_NAME_MAX		33	/* ComponentName, 1..32 */

struct rspro_slot {
	uint16_t id;	/* clientId or bankId */
	uint16_t nr;	/* slotNr */
};

/* a decoded PDU: the fields of whichever message it is, flat */
struct rspro_pdu {
	unsigned version;
	uint32_t tag;
	int msg;			/* enum rspro_msg */
	int result;			/* *Res; -1 absent */
	/* ConnectClientRes: who answered */
	int comp_type;
	char comp_name[RSPRO_NAME_MAX];
	char comp_software[RSPRO_NAME_MAX];
	char comp_version[RSPRO_NAME_MAX];
	/* ConfigClientIdReq, SetAtrReq, TpduCardToModem, BankSlotStatusInd */
	struct rspro_slot client;
	bool has_client;
	/* ConfigClientBankReq, TpduCardToModem, BankSlotStatusInd */
	struct rspro_slot bank;
	bool has_bank;
	/* ConfigClientBankReq: the bankd; ip_len 4 or 16 */
	uint8_t ip[16];
	size_t ip_len;
	uint16_t port;
	/* SetAtrReq: the ATR; TpduCardToModem: the card's answer */
	uint8_t data[RSPRO_DATA_MAX];
	size_t data_len;
	/* TpduFlags */
	bool final_part;
	/* ErrorInd */
	int err_severity, err_code;
	char err_string[256];
};

/* rspro_decode: a PDU of another RSPRO version (decoded as far as it went) */
#define RSPRO_E_VERSION		-2

/* 0 decoded (an unknown message decodes to its msg number and nothing
 * else), -1 malformed — not BER, more or less than one PDU, a field a
 * message must have missing — RSPRO_E_VERSION another version */
int rspro_decode(const uint8_t *b, size_t n, struct rspro_pdu *p);

/* The encoders write one RsproPDU into out and return its length, 0 when
 * it does not fit. */
size_t rspro_enc_connect_client_req(uint8_t *out, size_t cap, uint32_t tag,
				    const struct rspro_slot *client,
				    const char *name, const char *software,
				    const char *version);
/* a response that is only a ResultCode: ConfigClientIdRes,
 * ConfigClientBankRes, SetAtrRes, ResetStateRes */
size_t rspro_enc_result_res(uint8_t *out, size_t cap, int msg, uint32_t tag,
			    int result);
size_t rspro_enc_tpdu_modem_to_card(uint8_t *out, size_t cap, uint32_t tag,
				    const struct rspro_slot *client,
				    const struct rspro_slot *bank,
				    const uint8_t *data, size_t len);
size_t rspro_enc_client_slot_status_ind(uint8_t *out, size_t cap, uint32_t tag,
					const struct rspro_slot *client,
					const struct rspro_slot *bank,
					bool reset_active, bool vcc, bool clk,
					bool card_present);

const char *rspro_msg_name(int msg);
const char *rspro_result_name(int result);

/* IPA framing. rspro_ipa_wrap puts the 4-byte OSMO/RSPRO header before a
 * message already written at out + 4; returns the frame length. */
size_t rspro_ipa_wrap(uint8_t *out, size_t msg_len);

/* One frame from the front of a receive buffer: 1 with proto/ext/payload
 * set and *used the frame's size, 0 when more bytes are needed, -1 when
 * the length is beyond what this side accepts. ext is -1 for a frame
 * without an extension byte (CCM). */
int rspro_ipa_parse(const uint8_t *b, size_t n, size_t *used, int *proto,
		    int *ext, const uint8_t **payload, size_t *payload_len);

#endif
