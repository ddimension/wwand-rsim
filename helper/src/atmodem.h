/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_ATMODEM_H
#define RSIM_ATMODEM_H

#include <stdbool.h>

#include "backend.h"

struct at_cfg {
	const char *dev;	/* the modem's AT tty, e.g. /dev/ttyUSB2 */
	unsigned baud;		/* 115200 unless told; USB ttys ignore it */
	bool radio_keep;	/* leave the modem's radio as it is (default: off) */
};

/* The SIM of a modem that is NOT the one using the card — any modem with an
 * AT port, on a SIM host or on the same router but not managed by wwand — as
 * the card: APDUs over AT+CSIM (3GPP TS 27.007 §8.17). NULL when the port
 * does not open or does not answer AT (logged). */
struct rsim_backend *atmodem_open(const struct at_cfg *cfg);

/* exposed for the tests: the response of a +CSIM line into resp; its
 * length, or -1 when the line is not one */
int atmodem_csim_answer(const char *line, unsigned char *resp, int cap);

#endif
