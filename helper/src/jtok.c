/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "jtok.h"

#define DEPTH_MAX 16

/* JSON as RFC 8259 has it, read by recursive descent: the grammar is
 * checked, not guessed at. A lenient reader took `{"a" 1}`, `[1 2]` or a
 * key without its value as well-formed and paired keys with the wrong
 * values — and what is read here decides which bank slot is taken as
 * mapped to us, and deleted at the end. */
struct jp {
	const char *js;
	size_t len, i;
	struct jtok *t;
	int n, max;
};

static void ws(struct jp *p)
{
	while (p->i < p->len && (p->js[p->i] == ' ' || p->js[p->i] == '\t' ||
				 p->js[p->i] == '\r' || p->js[p->i] == '\n'))
		p->i++;
}

static int tok(struct jp *p, int type, size_t start)
{
	if (p->n == p->max)
		return -1;
	p->t[p->n].type = type;
	p->t[p->n].start = (int)start;
	p->t[p->n].end = -1;
	p->t[p->n].size = 0;
	return p->n++;
}

static int hexdig(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* p->i at the opening quote; the token's text without the quotes */
static int string(struct jp *p)
{
	int k = tok(p, JT_STR, p->i + 1);

	if (k < 0)
		return -1;
	for (p->i++; p->i < p->len; p->i++) {
		unsigned char c = (unsigned char)p->js[p->i];

		if (c < 0x20)
			return -1;
		if (c == '"') {
			p->t[k].end = (int)p->i++;
			return 0;
		}
		if (c != '\\')
			continue;
		if (++p->i >= p->len)
			return -1;
		c = (unsigned char)p->js[p->i];
		if (c == 'u') {
			int d;

			for (d = 0; d < 4; d++)
				if (++p->i >= p->len || !hexdig(p->js[p->i]))
					return -1;
		} else if (!strchr("\"\\/bfnrt", c) || !c) {
			return -1;
		}
	}
	return -1;
}

static int digits(struct jp *p)
{
	size_t s = p->i;

	while (p->i < p->len && p->js[p->i] >= '0' && p->js[p->i] <= '9')
		p->i++;
	return p->i > s ? 0 : -1;
}

/* a number, true, false or null */
static int primitive(struct jp *p)
{
	static const char *const lit[] = { "true", "false", "null", NULL };
	size_t s = p->i;
	int k, j;

	for (j = 0; lit[j]; j++) {
		size_t l = strlen(lit[j]);

		if (p->len - p->i >= l && !memcmp(p->js + p->i, lit[j], l)) {
			p->i += l;
			goto done;
		}
	}
	if (p->i < p->len && p->js[p->i] == '-')
		p->i++;
	if (p->i < p->len && p->js[p->i] == '0')
		p->i++;
	else if (digits(p))
		return -1;
	if (p->i < p->len && p->js[p->i] == '.') {
		p->i++;
		if (digits(p))
			return -1;
	}
	if (p->i < p->len && (p->js[p->i] == 'e' || p->js[p->i] == 'E')) {
		p->i++;
		if (p->i < p->len && (p->js[p->i] == '+' || p->js[p->i] == '-'))
			p->i++;
		if (digits(p))
			return -1;
	}
done:
	if ((k = tok(p, JT_PRIM, s)) < 0)
		return -1;
	p->t[k].end = (int)p->i;
	return 0;
}

static int value(struct jp *p, int depth);

/* an object or an array: members separated by commas, none after the last */
static int container(struct jp *p, int depth)
{
	bool obj = p->js[p->i] == '{';
	char close = obj ? '}' : ']';
	int k = tok(p, obj ? JT_OBJ : JT_ARR, p->i);

	if (k < 0 || depth >= DEPTH_MAX)
		return -1;
	p->i++;
	ws(p);
	if (p->i < p->len && p->js[p->i] == close) {
		p->t[k].end = (int)++p->i;
		return 0;
	}
	for (;;) {
		if (obj) {
			if (p->i >= p->len || p->js[p->i] != '"' || string(p))
				return -1;
			ws(p);
			if (p->i >= p->len || p->js[p->i] != ':')
				return -1;
			p->i++;
			ws(p);
		}
		if (value(p, depth + 1))
			return -1;
		p->t[k].size++;
		ws(p);
		if (p->i >= p->len)
			return -1;
		if (p->js[p->i] == close) {
			p->t[k].end = (int)++p->i;
			return 0;
		}
		if (p->js[p->i] != ',')
			return -1;
		p->i++;
		ws(p);
	}
}

static int value(struct jp *p, int depth)
{
	if (p->i >= p->len)
		return -1;
	if (p->js[p->i] == '{' || p->js[p->i] == '[')
		return container(p, depth);
	if (p->js[p->i] == '"')
		return string(p);
	return primitive(p);
}

int jtok_parse(const char *js, size_t len, struct jtok *t, int max)
{
	struct jp p = { js, len, 0, t, 0, max };

	ws(&p);
	if (value(&p, 0))
		return -1;
	ws(&p);
	/* one document: nothing after the root value */
	return p.i == len ? p.n : -1;
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

	/* nothing written without room for the terminator */
	if (!out || !cap)
		return;
	out[0] = '\0';
	if (i < 0 || t[i].type != JT_STR)
		return;
	l = (size_t)(t[i].end - t[i].start);
	if (l >= cap)
		l = cap - 1;
	memcpy(out, js + t[i].start, l);
	out[l] = '\0';
}
