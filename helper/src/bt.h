/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_BT_H
#define RSIM_BT_H

#include <stdbool.h>
#include <stddef.h>

#include "backend.h"

struct bt_cfg {
	const char *addr;	/* the phone, AA:BB:CC:DD:EE:FF */
	int channel;		/* its RFCOMM channel; 0: looked up over SDP */
	bool secure_high;	/* BT_SECURITY_HIGH (MITM-protected key) instead of MEDIUM */
	bool apdu7816;		/* CommandAPDU7816 instead of CommandAPDU */
};

/* The SIM of a paired phone over the Bluetooth SIM Access Profile, as its
 * client: kernel RFCOMM and L2CAP sockets only, no BlueZ library. NULL when
 * the phone cannot be reached or refuses (logged). */
struct rsim_backend *bt_open(const struct bt_cfg *cfg);

/* `--list`: the paired phones BlueZ knows (its storage below
 * <sysroot>/var/lib/bluetooth), one JSON line each, with what BlueZ and the
 * kernel know about them. note gets a reason when that storage exists but
 * cannot be read; adapters the kernel's adapters ("" when it cannot say). */
int bt_list(const char *sysroot, char *note, size_t note_cap, char *adapters, size_t acap);

/* AA:BB:CC:DD:EE:FF -> the six bytes in the order the kernel takes them
 * (least significant first); -1 when it is not an address */
int bt_parse_addr(const char *s, unsigned char out[6]);

#endif
