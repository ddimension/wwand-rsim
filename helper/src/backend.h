/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_BACKEND_H
#define RSIM_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "json.h"
#include "rsim.h"

struct rsim_backend;

/* Every op returns RSIM_OK or an enum rsim_err and may leave a reason in
 * be->detail (cleared by the caller before each op). The protocol layer
 * keeps the powered/ATR state; a backend only does what it is told. */
struct rsim_backend_ops {
	const char *name;	/* "phoenix" | "pcsc", reported by status */
	/* cold start; atr receives up to ATR_MAX bytes */
	int (*power_up)(struct rsim_backend *be, uint8_t *atr, size_t *atr_len);
	/* warm reset of a powered card */
	int (*reset)(struct rsim_backend *be, uint8_t *atr, size_t *atr_len);
	int (*power_down)(struct rsim_backend *be);
	/* one T=0 TPDU; resp gets RSIM_RESP_MAX bytes at most */
	int (*transmit)(struct rsim_backend *be, const uint8_t *tpdu, size_t len,
			uint8_t *resp, size_t *resp_len);
	/* 1 card present, 0 absent, -1 this backend cannot tell (then no
	 * insert/remove events are emitted) */
	int (*present)(struct rsim_backend *be);
	void (*close)(struct rsim_backend *be);
	/* optional: what is known about the reader and the card, as fields of
	 * the JSON object being written — the info event after the open and
	 * the status answer carry them (manufacturer, USB path, ATR source,
	 * the phone's name, the modem's identity, ...) */
	void (*info)(struct rsim_backend *be, struct jw *w);
	/* optional: called between requests every EVENT_POLL_MS (main.c),
	 * for what a backend must keep in order while it serves — the AT
	 * backend keeps the lending modem's radio off */
	void (*tick)(struct rsim_backend *be);
};

struct rsim_backend {
	const struct rsim_backend_ops *ops;
	const char *reader;	/* what status reports as "reader" */
	/* set by a backend whose reader is gone for good (a Bluetooth link
	 * that dropped): the helper ends, as if its SSH link had */
	bool ended;
	char detail[RSIM_DETAIL_MAX];
};

/* The open side of the table lives with each backend (phoenix_open,
 * pcsc_open): main.c picks it by the reader spec's prefix. NULL from a
 * constructor means it could not open the reader, and it has said why. */

#endif
