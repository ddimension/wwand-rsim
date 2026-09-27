/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <string.h>

#include "json.h"

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	return p;
}

bool json_is_object(const char *line)
{
	const char *p = skip_ws(line);
	size_t n;

	if (*p != '{')
		return false;
	n = strlen(p);
	while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' ||
		     p[n - 1] == '\r' || p[n - 1] == '\n'))
		n--;
	return n >= 2 && p[n - 1] == '}';
}

int json_get_string(const char *line, const char *key, char *out, size_t outlen)
{
	size_t klen = strlen(key);
	const char *p = line;

	/* A match is only a key when a ':' follows it; the same text as a
	 * value ({"data":"op","op":…}) is followed by ',' or '}' and the
	 * search goes on past it. */
	while ((p = strchr(p, '"'))) {
		const char *q;
		size_t n = 0;

		if (strncmp(p + 1, key, klen) || p[1 + klen] != '"') {
			p++;
			continue;
		}
		q = skip_ws(p + klen + 2);
		if (*q != ':') {
			p += klen + 2;
			continue;
		}
		q = skip_ws(q + 1);
		if (*q != '"')
			return -1;
		for (q++; *q != '"'; q++) {
			/* no request carries an escaped character, and
			 * decoding them is more parser than this needs */
			if (!*q || *q == '\\' || n + 1 >= outlen)
				return -1;
			out[n++] = *q;
		}
		out[n] = '\0';
		return 1;
	}
	return 0;
}

static int nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int hex_decode(const char *hex, uint8_t *out, size_t max)
{
	size_t len = strlen(hex), i;

	if (len % 2 || len / 2 > max)
		return -1;
	for (i = 0; i < len / 2; i++) {
		int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);

		if (hi < 0 || lo < 0)
			return -1;
		out[i] = (uint8_t)(hi << 4 | lo);
	}
	return (int)(len / 2);
}

void jw_begin(struct jw *w, FILE *f)
{
	w->f = f;
	w->first = true;
	fputc('{', f);
}

static void jw_key(struct jw *w, const char *key)
{
	if (!w->first)
		fputc(',', w->f);
	w->first = false;
	fprintf(w->f, "\"%s\":", key);
}

void jw_bool(struct jw *w, const char *key, bool v)
{
	jw_key(w, key);
	fputs(v ? "true" : "false", w->f);
}

void jw_str(struct jw *w, const char *key, const char *s)
{
	jw_key(w, key);
	fputc('"', w->f);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\')
			fprintf(w->f, "\\%c", c);
		else if (c < 0x20)
			fprintf(w->f, "\\u%04x", c);
		else
			fputc(c, w->f);
	}
	fputc('"', w->f);
}

void jw_hex(struct jw *w, const char *key, const uint8_t *b, size_t n)
{
	size_t i;

	jw_key(w, key);
	fputc('"', w->f);
	for (i = 0; i < n; i++)
		fprintf(w->f, "%02X", b[i]);
	fputc('"', w->f);
}

void jw_int(struct jw *w, const char *key, long v)
{
	jw_key(w, key);
	fprintf(w->f, "%ld", v);
}

void jw_opt(struct jw *w, const char *key, const char *s)
{
	if (s && *s)
		jw_str(w, key, s);
}

void jw_null(struct jw *w, const char *key)
{
	jw_key(w, key);
	fputs("null", w->f);
}

void jw_end(struct jw *w)
{
	fputs("}\n", w->f);
	fflush(w->f);
}
