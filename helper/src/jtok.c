/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <stdlib.h>
#include <string.h>

#include "jtok.h"

#define DEPTH_MAX 16

/* a new token inside the open container: an array counts it; an object
 * counts its keys, and takes key and value in turn. -1: a key that is not
 * a string. */
static int attach(struct jtok *t, const int *stack, int *want_key, int depth, int type)
{
	struct jtok *p;

	if (!depth)
		return 0;
	p = &t[stack[depth - 1]];
	if (p->type == JT_ARR) {
		p->size++;
		return 0;
	}
	if (want_key[depth - 1]) {
		if (type != JT_STR)
			return -1;
		p->size++;
	}
	want_key[depth - 1] = !want_key[depth - 1];
	return 0;
}

int jtok_parse(const char *js, size_t len, struct jtok *t, int max)
{
	int stack[DEPTH_MAX], depth = 0, n = 0;
	/* an object's next string is a key, not a member of its own */
	int want_key[DEPTH_MAX];
	size_t i = 0;

	while (i < len) {
		char c = js[i];

		if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ':' || c == ',') {
			i++;
			continue;
		}
		if (c == '{' || c == '[') {
			if (n == max || depth == DEPTH_MAX)
				return -1;
			t[n].type = c == '{' ? JT_OBJ : JT_ARR;
			if (attach(t, stack, want_key, depth, t[n].type))
				return -1;
			t[n].start = (int)i;
			t[n].end = -1;
			t[n].size = 0;
			want_key[depth] = 1;
			stack[depth++] = n++;
			i++;
			continue;
		}
		if (c == '}' || c == ']') {
			if (!depth || t[stack[depth - 1]].type != (c == '}' ? JT_OBJ : JT_ARR))
				return -1;
			t[stack[--depth]].end = (int)i + 1;
			i++;
			continue;
		}
		if (n == max)
			return -1;
		if (c == '"') {
			size_t s = ++i;

			while (i < len && js[i] != '"')
				i += (js[i] == '\\') ? 2 : 1;
			if (i >= len)
				return -1;
			t[n].type = JT_STR;
			t[n].start = (int)s;
			t[n].end = (int)i;
			i++;
		} else {
			size_t s = i;

			while (i < len && !strchr(" \t\r\n,:]}", js[i]))
				i++;
			t[n].type = JT_PRIM;
			t[n].start = (int)s;
			t[n].end = (int)i;
		}
		t[n].size = 0;
		if (attach(t, stack, want_key, depth, t[n].type))
			return -1;
		n++;
	}
	return depth ? -1 : n;
}

int jtok_skip(const struct jtok *t, int n, int i)
{
	int end;

	if (i < 0 || i >= n)
		return n;
	if (t[i].type != JT_OBJ && t[i].type != JT_ARR)
		return i + 1;
	end = t[i].end;
	for (i++; i < n && t[i].start < end; i++)
		;
	return i;
}

int jtok_get(const char *js, const struct jtok *t, int n, int obj, const char *key)
{
	size_t kl = strlen(key);
	int i;

	if (obj < 0 || obj >= n || t[obj].type != JT_OBJ)
		return -1;
	for (i = obj + 1; i < n && t[i].start < t[obj].end; ) {
		int v = i + 1;

		if (v >= n)
			return -1;
		if (t[i].type == JT_STR && (size_t)(t[i].end - t[i].start) == kl &&
		    !memcmp(js + t[i].start, key, kl))
			return v;
		i = jtok_skip(t, n, v);
	}
	return -1;
}

int jtok_elem(const struct jtok *t, int n, int arr, int idx)
{
	int i, k;

	if (arr < 0 || arr >= n || t[arr].type != JT_ARR || idx < 0 || idx >= t[arr].size)
		return -1;
	for (i = arr + 1, k = 0; i < n && k < idx; k++)
		i = jtok_skip(t, n, i);
	return i < n ? i : -1;
}

long jtok_long(const char *js, const struct jtok *t, int i, long def)
{
	char buf[24], *end;
	int l;
	long v;

	if (i < 0 || t[i].type != JT_PRIM)
		return def;
	l = t[i].end - t[i].start;
	if (l <= 0 || l >= (int)sizeof(buf))
		return def;
	memcpy(buf, js + t[i].start, (size_t)l);
	buf[l] = '\0';
	v = strtol(buf, &end, 10);
	return *end ? def : v;
}

void jtok_str(const char *js, const struct jtok *t, int i, char *out, size_t cap)
{
	size_t l;

	out[0] = '\0';
	if (i < 0 || t[i].type != JT_STR || !cap)
		return;
	l = (size_t)(t[i].end - t[i].start);
	if (l >= cap)
		l = cap - 1;
	memcpy(out, js + t[i].start, l);
	out[l] = '\0';
}
