/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_PHOENIX_H
#define RSIM_PHOENIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend.h"

enum phoenix_reset {
	PHX_RESET_AUTO,
	PHX_RESET_RTS,
	PHX_RESET_RTS_INV,
	PHX_RESET_DTR,
	PHX_RESET_DTR_INV,
};

enum phoenix_detect {
	PHX_DETECT_NONE,
	PHX_DETECT_CTS,
	PHX_DETECT_DSR,
	PHX_DETECT_CD,
};

struct phoenix_cfg {
	const char *dev;
	unsigned clock_khz;
	enum phoenix_reset reset;
	enum phoenix_detect detect;
	unsigned atr_timeout_ms;
};

/*
 * The serial transport under the Phoenix protocol: a kernel tty (phx_tty.c)
 * or an FTDI chip driven directly over libusb (ftdi_usb.c) where the kernel
 * has no driver for it. Every op returns 0 or -errno unless said otherwise;
 * the transport logs its own specifics, phoenix.c turns -errno into the
 * request's detail.
 */
enum phx_parity {
	PHX_PARITY_NONE,
	PHX_PARITY_ODD,
	PHX_PARITY_EVEN,
};

struct phx_io;

struct phx_io_ops {
	/* 8 data bits, 2 stop bits, parity as given; with drop_errors a
	 * byte received with a parity or framing error is discarded (the
	 * tty's INPCK|IGNPAR), without it it is delivered like any other */
	int (*set_line)(struct phx_io *io, unsigned baud, enum phx_parity parity,
			bool drop_errors);
	/* each of rts/dtr: 1 raise, 0 drop, -1 leave as it is */
	int (*set_modem)(struct phx_io *io, int rts, int dtr);
	/* optional (NULL: no inputs to read); each 0/1 */
	int (*get_modem)(struct phx_io *io, int *cts, int *dsr, int *cd);
	/* all len bytes */
	int (*write)(struct phx_io *io, const uint8_t *buf, size_t len);
	/* up to len bytes that arrived: n > 0, 0 when nothing came within
	 * timeout_ms, or -errno */
	int (*read)(struct phx_io *io, uint8_t *buf, size_t len, unsigned timeout_ms);
	/* whatever was received and not read yet is discarded */
	void (*flush_input)(struct phx_io *io);
	void (*close)(struct phx_io *io);
};

struct phx_io {
	const struct phx_io_ops *ops;
};

/* the kernel tty at dev (phx_tty.c); NULL on failure (logged) */
struct phx_io *phx_tty_open(const char *dev);

/* the Phoenix protocol over io, which it then owns (closed on failure
 * too); io NULL opens the tty cfg->dev. cfg->dev also names the reader in
 * logs and in status. */
struct rsim_backend *phoenix_open(const struct phoenix_cfg *cfg, struct phx_io *io);

/* in phoenix_baud.c, the only file that sees <asm/termbits.h> (its struct
 * termios clashes with the libc one): sets an arbitrary rate with
 * termios2/BOTHER. 0 or -errno. */
int phoenix_set_baud(int fd, unsigned baud);

#endif
