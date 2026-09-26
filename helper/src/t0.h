/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_T0_H
#define RSIM_T0_H

#include <stddef.h>
#include <stdint.h>

/* The byte channel the engine drives. send() writes the bytes to the card
 * (a backend with an echoing I/O line consumes the echo inside send);
 * recv() returns one byte within timeout_ms. Both return RSIM_OK or an
 * enum rsim_err. Keeping the engine behind this makes every T=0 branch
 * testable with a scripted card. */
struct t0_chan {
	int (*send)(void *ctx, const uint8_t *buf, size_t len);
	int (*recv)(void *ctx, uint8_t *byte, unsigned timeout_ms);
	void *ctx;
};

/* One T=0 exchange (ISO/IEC 7816-3:2006 §10.3). tpdu is CLA INS P1 P2
 * [P3 [data]]; a 4-byte TPDU is sent as case 1 with P3=00. resp receives
 * the data the card sent followed by SW1 SW2 (RSIM_RESP_MAX bytes).
 * 61xx and 6Cxx come back as they are: the caller (the modem) issues GET
 * RESPONSE or the corrected command itself. detail, when not NULL, must
 * hold a string (normally empty; the channel may write its own reason
 * there) and gets a human-readable reason on failure. */
int t0_transceive(const struct t0_chan *ch, unsigned wwt_ms,
		  const uint8_t *tpdu, size_t len,
		  uint8_t *resp, size_t *resp_len,
		  char *detail, size_t detail_len);

/* Work waiting time 960 * WI * Fi / f (§10.2) in ms, rounded up, for Fi=372
 * (no PPS is done, so the card runs at the default rate) */
unsigned t0_wwt_ms(unsigned clock_khz, unsigned wi);

#endif
