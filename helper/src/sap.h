/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_SAP_H
#define RSIM_SAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * The SIM Access Profile's messages (Bluetooth SAP v1.1 §5) and the few SDP
 * PDUs needed to find its RFCOMM channel (Core spec Vol 3 Part B). Pure
 * encoding and decoding, no I/O: bt.c owns the sockets, the tests feed
 * buffers.
 */

enum sap_msg_id {
	SAP_CONNECT_REQ = 0x00,
	SAP_CONNECT_RESP = 0x01,
	SAP_DISCONNECT_REQ = 0x02,
	SAP_DISCONNECT_RESP = 0x03,
	SAP_DISCONNECT_IND = 0x04,
	SAP_TRANSFER_APDU_REQ = 0x05,
	SAP_TRANSFER_APDU_RESP = 0x06,
	SAP_TRANSFER_ATR_REQ = 0x07,
	SAP_TRANSFER_ATR_RESP = 0x08,
	SAP_POWER_SIM_OFF_REQ = 0x09,
	SAP_POWER_SIM_OFF_RESP = 0x0A,
	SAP_POWER_SIM_ON_REQ = 0x0B,
	SAP_POWER_SIM_ON_RESP = 0x0C,
	SAP_RESET_SIM_REQ = 0x0D,
	SAP_RESET_SIM_RESP = 0x0E,
	SAP_STATUS_IND = 0x11,
	SAP_ERROR_RESP = 0x12,
};

enum sap_param_id {
	SAP_P_MAX_MSG_SIZE = 0x00,	/* 2 bytes, big endian */
	SAP_P_CONNECTION_STATUS = 0x01,
	SAP_P_RESULT_CODE = 0x02,
	SAP_P_DISCONNECTION_TYPE = 0x03,
	SAP_P_COMMAND_APDU = 0x04,	/* GSM 11.11 format */
	SAP_P_RESPONSE_APDU = 0x05,
	SAP_P_ATR = 0x06,
	SAP_P_STATUS_CHANGE = 0x08,
	SAP_P_COMMAND_APDU7816 = 0x10,	/* ISO/IEC 7816-4 format */
};

/* ConnectionStatus */
enum { SAP_CONN_OK = 0, SAP_CONN_FAIL = 1, SAP_CONN_MSGSIZE = 2, SAP_CONN_TOOSMALL = 3, SAP_CONN_OK_CALL = 4 };
/* ResultCode */
enum {
	SAP_RC_OK = 0, SAP_RC_NO_REASON = 1, SAP_RC_NOT_ACCESSIBLE = 2, SAP_RC_ALREADY_OFF = 3,
	SAP_RC_REMOVED = 4, SAP_RC_ALREADY_ON = 5, SAP_RC_NO_DATA = 6, SAP_RC_NOT_SUPPORTED = 7,
};
/* StatusChange */
enum {
	SAP_ST_UNKNOWN = 0, SAP_ST_RESET = 1, SAP_ST_NOT_ACCESSIBLE = 2, SAP_ST_REMOVED = 3,
	SAP_ST_INSERTED = 4, SAP_ST_RECOVERED = 5,
};
/* DisconnectionType */
enum { SAP_DISC_GRACEFUL = 0, SAP_DISC_IMMEDIATE = 1 };

/* An APDU of 261 bytes as a request: 4 + 4 + 264 = 272; a 258-byte answer
 * with its result code: 4 + 8 + 4 + 260 = 276. A server that cannot take
 * that much cannot carry every command the modem sends. */
#define SAP_MSG_MIN	276
#define SAP_MSG_WANT	1024
#define SAP_PARAMS_MAX	4

struct sap_param {
	uint8_t id;
	uint16_t len;
	const uint8_t *val;
};

struct sap_msg {
	uint8_t id;
	int n;
	struct sap_param p[SAP_PARAMS_MAX];
};

/* one message into out: its length, -1 when it does not fit */
int sap_encode(uint8_t *out, size_t cap, uint8_t id, const struct sap_param *p, int n);

/* the length of the complete message at the start of buf, 0 when more bytes
 * are needed, -1 when it cannot be one (more parameters than any message
 * has) */
int sap_frame(const uint8_t *buf, size_t len);

/* a complete message (sap_frame > 0) into m, pointing into buf; -1 when
 * malformed */
int sap_decode(const uint8_t *buf, size_t len, struct sap_msg *m);

/* the parameter id of m, NULL when absent */
const struct sap_param *sap_get(const struct sap_msg *m, uint8_t id);

/* a one-byte parameter, -1 when absent or not one byte */
int sap_get_u8(const struct sap_msg *m, uint8_t id);

const char *sap_msg_name(uint8_t id);
const char *sap_result_name(int rc);

/* ---- SDP ------------------------------------------------------------- */

/* The SIM Access service class */
#define SAP_UUID16 0x112D

/* a ServiceSearchAttributeRequest for the service class uuid16, asking for
 * its ProtocolDescriptorList; cont/cont_len is the continuation state of the
 * previous answer (NULL/0 at first). Its length, -1 when it does not fit. */
int sdp_search_req(uint8_t *out, size_t cap, uint16_t tid, uint16_t uuid16,
		   const uint8_t *cont, size_t cont_len);

/* a ServiceSearchAttributeResponse: the attribute bytes of this part and the
 * continuation state (length 0: this was the last part). 0, or -1 when it
 * is not the answer to tid or is malformed. */
int sdp_search_rsp(const uint8_t *pdu, size_t len, uint16_t tid,
		   const uint8_t **attrs, size_t *attrs_len,
		   const uint8_t **cont, size_t *cont_len);

/* the first RFCOMM channel in a list of attribute data elements (a UUID
 * 0x0003 followed by its channel number, in any sequence), -1 when none */
int sdp_rfcomm_channel(const uint8_t *attrs, size_t len);

#endif
