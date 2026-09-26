/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_SCAN_H
#define RSIM_SCAN_H

/* `rsim-card --list`: the card sources on this machine, JSON lines (scan.c).
 * sysroot: prefix for /sys (tests), NULL for the real one. */
int scan_run(const char *sysroot);

#endif
