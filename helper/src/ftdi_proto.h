/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_FTDI_PROTO_H
#define RSIM_FTDI_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The parts of the FTDI FT232BM protocol that need no USB: kept apart from
 * ftdi_usb.c so the tests run without libusb or a device. */

/* The FT232BM baud divisor for `baud`: 0 with *div set, -1 when the rate is
 * outside what the divisor can express (about 183 baud .. 3 Mbaud). The
 * low 16 bits go in wValue of SET_BAUDRATE, the rest in wIndex. */
int ftdi_bm_divisor(unsigned baud, uint32_t *div);

/* the rate a divisor actually produces, in baud (rounded) */
unsigned ftdi_bm_rate(uint32_t div);

/* line-status bits of the second byte of every IN packet (ftdi_sio.h
 * FTDI_RS_*, linux 6.18.41) */
#define FTDI_LS_OE	0x02	/* overrun */
#define FTDI_LS_PE	0x04	/* parity error */
#define FTDI_LS_FE	0x08	/* framing error */
#define FTDI_LS_BI	0x10	/* break */

/* modem-status bits of the first byte (ftdi_sio.h FTDI_RS0_*, and the
 * GET_MODEM_STATUS answer, linux 6.18.41) */
#define FTDI_MS_CTS	0x10
#define FTDI_MS_DSR	0x20
#define FTDI_MS_RI	0x40
#define FTDI_MS_CD	0x80

struct ftdi_rx_stat {
	uint8_t modem;		/* modem status of the last packet */
	unsigned parity, framing, overrun;	/* packets flagged so */
	unsigned dropped;	/* data bytes discarded for an error */
};

/* Splits one bulk-IN transfer of len bytes into its packets of mps bytes
 * (the last one may be short), strips the two status bytes every packet
 * starts with, and appends the data to out (room for len bytes). With
 * drop_errors, the data of a packet flagged with a parity or framing error
 * is discarded as a whole. Returns the number of data bytes written. */
size_t ftdi_rx_parse(const uint8_t *in, size_t len, size_t mps, bool drop_errors,
		     uint8_t *out, struct ftdi_rx_stat *st);

#endif
