/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_PCSC_H
#define RSIM_PCSC_H

#include "backend.h"

/* spec is what follows "pcsc:": a reader-name substring or a list index */
struct rsim_backend *pcsc_open(const char *spec);

/* every reader pcscd knows, one JSON line each ({"backend":"pcsc","spec",
 * "name","card"}); 0, or -1 when pcscd cannot be reached (logged) */
int pcsc_list(void);

#endif
