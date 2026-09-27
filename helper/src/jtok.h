/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_JTOK_H
#define RSIM_JTOK_H

#include <stddef.h>

/* A JSON document read into a flat token array (jsmn style), for the few
 * answers the helper reads that are nested — the remsim-server's REST API.
 * The request lines of the plugin stay with json.h's flat lookup. */
enum { JT_OBJ = 1, JT_ARR, JT_STR, JT_PRIM };

struct jtok {
	int type;
	int start, end;		/* the text; a string's without its quotes */
	int size;		/* members of an object, elements of an array */
};

/* the token count, -1 malformed or more than max tokens */
int jtok_parse(const char *js, size_t len, struct jtok *t, int max);
/* the index of the token after token i and everything inside it */
int jtok_skip(const struct jtok *t, int n, int i);
/* in object token obj: the value token of key, or -1 */
int jtok_get(const char *js, const struct jtok *t, int n, int obj, const char *key);
/* the i-th element of array token arr, or -1 */
int jtok_elem(const struct jtok *t, int n, int arr, int i);
/* a number token as long (def when it is not one) */
long jtok_long(const char *js, const struct jtok *t, int i, long def);
/* a string token copied into out, escapes as they are ("" when not one) */
void jtok_str(const char *js, const struct jtok *t, int i, char *out, size_t cap);

#endif
