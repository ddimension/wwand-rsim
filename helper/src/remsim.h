/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_REMSIM_H
#define RSIM_REMSIM_H

#include <stdint.h>

#include "backend.h"

/* the ports osmo-remsim listens on by default: remsim-server's RSPRO and
 * REST ports (remsim_server.c, rest_api.c) */
#define REMSIM_SERVER_PORT	9998
#define REMSIM_REST_PORT	9997

struct rspro_cfg {
	/* after "rspro:": <server>[:<port>][/<bank>:<slot>] */
	const char *spec;
	uint16_t client_id, client_slot;
	uint16_t rest_port;
};

/* A card in an osmo-remsim SIM bank: the helper is a remsim client. NULL
 * when the server cannot be reached or refuses this client (said why). */
struct rsim_backend *rspro_open(const struct rspro_cfg *cfg);

/* `--list --rspro-server <server>[:<port>]`: the bank slots the server's
 * REST API (rest_port, 0: REMSIM_REST_PORT) knows, one JSON line each, then
 * the done line. <port> is the RSPRO port, as in a reader spec: it is only
 * carried into the specs of the rows. */
int rspro_list(const char *server, uint16_t rest_port);

#endif
