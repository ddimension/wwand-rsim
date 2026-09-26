/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_JSON_H
#define RSIM_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* The request side is deliberately minimal: a request is a flat object of
 * string values written by the plugin, so a key lookup is all that is
 * needed. What it must not do is accept something malformed as something
 * else, hence the object check and the strict hex decoder. */

/* true when the line, blanks trimmed, is "{ ... }" */
bool json_is_object(const char *line);

/* the string value of `key` copied into out: 1 found, 0 absent, -1 present
 * but not a plain string (escape, no closing quote, too long for out) */
int json_get_string(const char *line, const char *key, char *out, size_t outlen);

/* hex digits of either case into bytes; -1 on odd length, a non-hex
 * character or more than max bytes, else the byte count */
int hex_decode(const char *hex, uint8_t *out, size_t max);

/* line writer: jw_begin, fields, jw_end (which writes the newline and
 * flushes -- the plugin reads line by line and waits for it) */
struct jw {
	FILE *f;
	bool first;
};

void jw_begin(struct jw *w, FILE *f);
void jw_bool(struct jw *w, const char *key, bool v);
void jw_str(struct jw *w, const char *key, const char *s);
void jw_hex(struct jw *w, const char *key, const uint8_t *b, size_t n);
void jw_null(struct jw *w, const char *key);
void jw_end(struct jw *w);

#endif
