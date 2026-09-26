/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_PCSC_H
#define RSIM_PCSC_H

#include "backend.h"

/* spec is what follows "pcsc:": a reader-name substring or a list index */
struct rsim_backend *pcsc_open(const char *spec);

#endif
