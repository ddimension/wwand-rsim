/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 *
 * `rsim-card --serve`: an SSH key that can do what wwand-rsim needs and
 * nothing else. On the machine with the reader, the router's key goes into
 * authorized_keys as
 *
 *   command="rsim-card --serve 'bt:AA:BB:CC:DD:EE:FF'",no-pty,no-port-forwarding,
 *   no-agent-forwarding,no-X11-forwarding ssh-ed25519 AAAA… wwand-rsim
 *
 * (one line). sshd and dropbear then run this instead of what the client
 * asked for, and put the request in SSH_ORIGINAL_COMMAND. It is carried out
 * only when it is one of:
 *
 *   rsim-card [options] <reader>      a reader (the plugin's session)
 *   rsim-card --list                  the scan
 *   wwandctl rsim proxy [options] <modem | iccid:…>   a modem's card on a
 *                                     wwand router (wwand-rsim there)
 *   wwandctl rsim proxy --list        ...and its scan
 *
 * With specs after --serve only a reader one of them matches (fnmatch; a
 * proxy target as wwand:<target>) is served; the lists always are. Nothing
 * goes through a shell: the request is split into words here and exec'd.
 */
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "serve.h"

#define WORDS_MAX 32

int serve_split(const char *cmd, char *buf, size_t cap, char **argv, int max)
{
	size_t o = 0;
	int n = 0;
	const char *p = cmd;

	for (;;) {
		int in_word = 0;

		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		if (n == max)
			return -1;
		argv[n] = buf + o;
		while (*p && *p != ' ' && *p != '\t') {
			in_word = 1;
			if (*p == '\'') {
				for (p++; *p && *p != '\''; p++) {
					if (o + 1 >= cap)
						return -1;
					buf[o++] = *p;
				}
				if (!*p)
					return -1;
				p++;
			} else if (*p == '"') {
				for (p++; *p && *p != '"'; p++) {
					if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$' || p[1] == '`'))
						p++;
					if (o + 1 >= cap)
						return -1;
					buf[o++] = *p;
				}
				if (!*p)
					return -1;
				p++;
			} else {
				if (*p == '\\' && p[1])
					p++;
				if (o + 1 >= cap)
					return -1;
				buf[o++] = *p++;
			}
		}
		(void)in_word;
		if (o + 1 >= cap)
			return -1;
		buf[o++] = '\0';
		n++;
	}
	argv[n] = NULL;
	return n;
}

/* The test hooks (RSIM_TEST_*) name programs to run and files to write.
 * Inside a real SSH session they would be the client's to set wherever the
 * server passes its environment on (AcceptEnv): a way around everything
 * --serve is for. So there they count for nothing and are removed before the
 * exec; only a run outside SSH (the tests) may use them. */
static const char *test_hook(const char *name)
{
	return (getenv("SSH_CONNECTION") || getenv("SSH_CLIENT")) ? NULL : getenv(name);
}

static void drop_test_env(void)
{
	extern char **environ;
	char name[64];
	int again = 1;

	if (!getenv("SSH_CONNECTION") && !getenv("SSH_CLIENT"))
		return;
	while (again) {
		char **e;

		again = 0;
		for (e = environ; *e; e++)
			if (!strncmp(*e, "RSIM_TEST_", 10)) {
				size_t l = strcspn(*e, "=");

				snprintf(name, sizeof(name), "%.*s", (int)(l < sizeof(name) ? l : sizeof(name) - 1), *e);
				unsetenv(name);
				again = 1;
				break;
			}
	}
}

static const char *base(const char *s)
{
	const char *b = strrchr(s, '/');

	return b ? b + 1 : s;
}

/* rsim-card's options that take a value: the reader is the one word left */
static int takes_value(const char *a)
{
	static const char *const opts[] = {
		"--clock", "--reset", "--detect", "--atr-timeout-ms", "--wbsm-mode", "--at-baud", "--at-radio",
		"--bt-channel", "--bt-security", "--bt-apdu", NULL,
	};
	int i;

	for (i = 0; opts[i]; i++)
		if (!strcmp(a, opts[i]))
			return 1;
	return 0;
}

static int refuse(const char *why, const char *what)
{
	fprintf(stderr, "rsim-card --serve: %s%s%s — this key is for wwand-rsim only\n", why, what ? ": " : "", what ? what : "");
	return 1;
}

static int allowed(const char *spec, int nallow, char **allow)
{
	int i;

	if (!nallow)
		return 1;
	/* FNM_PATHNAME: a `*` does not cross a `/` — `at:/dev/ttyUSB*` must
	 * not match `at:/dev/../etc/shadow` */
	for (i = 0; i < nallow; i++)
		if (!fnmatch(allow[i], spec, FNM_PATHNAME))
			return 1;
	return 0;
}

/* A reader that names a path (`at:`, `phoenix:`, a bare `/dev/…`) must be a
 * character device under /dev, however it is spelled: the backends open it
 * read-write and write AT commands to it — named a file, through a key that
 * allows `at:*` or any reader, they would read it out over SSH and write
 * over it. The backends check again (fstat) on their own. */
static int device_ok(const char *spec)
{
	static const char *const pfx[] = { "at:", "phoenix:", "", NULL };
	char real[PATH_MAX];
	struct stat st;
	int i;

	for (i = 0; pfx[i]; i++) {
		size_t l = strlen(pfx[i]);
		const char *path;

		if (strncmp(spec, pfx[i], l) || (!l && spec[0] != '/'))
			continue;
		path = spec + l;
		if (!*path && l)
			return 1;	/* `phoenix:` alone is not a path */
		/* ...and a serial port by its name: /dev/mtdN, /dev/mem, /dev/kmsg
		 * are character devices too (found by audit, 2026-09-27) */
		return !strncmp(path, "/dev/", 5) && !strstr(path, "/..") && realpath(path, real) &&
		       (!strncmp(real, "/dev/tty", 8) || !strncmp(real, "/dev/rfcomm", 11) ||
			!strncmp(real, "/dev/pts/", 9)) && strcmp(real, "/dev/tty") &&
		       !stat(real, &st) && S_ISCHR(st.st_mode);
	}
	return 1;
}

/* The options rsim-card has, as they may be spelled here: the ones without a
 * value, and the ones with (`--clock 3579` or `--clock=3579`). Anything else
 * — `--`, an abbreviation getopt_long would expand — is refused rather than
 * guessed at. 1: a flag, 2: takes the next word, 0: not an option of ours */
static int option_kind(const char *a)
{
	static const char *const flags[] = { "-v", "-s", "-vv", "-vs", "-sv", "--verbose", "--syslog", NULL };
	int i;

	for (i = 0; flags[i]; i++)
		if (!strcmp(a, flags[i]))
			return 1;
	if (takes_value(a))
		return 2;
	if (!strncmp(a, "--", 2) && strchr(a, '=')) {
		char name[64];
		size_t l = (size_t)(strchr(a, '=') - a);

		if (l < sizeof(name)) {
			memcpy(name, a, l);
			name[l] = '\0';
			if (takes_value(name))
				return 1;
		}
	}
	return 0;
}

/* a JSON line's string field (the writers here escape `"` and `\\` only);
 * 1 when it is there and not empty */
static int field(const char *line, const char *name, char *out, size_t cap)
{
	char key[64];
	const char *p;
	size_t o = 0;

	snprintf(key, sizeof(key), "\"%s\":\"", name);
	if (!(p = strstr(line, key)))
		return 0;
	for (p += strlen(key); *p && *p != '"' && o < cap - 1; p++) {
		if (*p == '\\' && p[1])
			p++;
		out[o++] = *p;
	}
	out[o] = '\0';
	return o > 0;
}

/* `--list` for a key restricted to some readers: only those rows — the rest
 * of what that machine has (other readers, other cards with their ICCID,
 * IMSI and APN) is none of this key's business */
static int list_filtered(char **argv, int nallow, char **allow, const char *self)
{
	int pfd[2];
	pid_t pid;
	FILE *in;
	char line[8192];
	int st = 0;

	if (pipe(pfd))
		return refuse("pipe", strerror(errno));
	if ((pid = fork()) < 0)
		return refuse("fork", strerror(errno));
	if (!pid) {
		close(pfd[0]);
		dup2(pfd[1], 1);
		close(pfd[1]);
		execv(self, argv);
		_exit(127);
	}
	close(pfd[1]);
	if (!(in = fdopen(pfd[0], "r")))
		return refuse("fdopen", strerror(errno));
	int cont = 0;		/* the rest of a line longer than the buffer */

	while (fgets(line, sizeof(line), in)) {
		char spec[512], modem[300];
		size_t l = strlen(line);
		int whole = l && line[l - 1] == '\n';

		if (cont) {		/* never judged on its own */
			cont = !whole;
			continue;
		}
		cont = !whole;
		if (!field(line, "spec", spec, sizeof(spec))) {	/* the done line, notes */
			if (whole)
				fputs(line, stdout);
			continue;
		}
		/* a modem's card is named both ways in a key: by ICCID, and by
		 * the modem it sits in */
		if (allowed(spec, nallow, allow) ||
		    (field(line, "modem", modem, sizeof(modem) - 6) &&
		     (memmove(modem + 6, modem, strlen(modem) + 1), memcpy(modem, "wwand:", 6), allowed(modem, nallow, allow))))
			fputs(line, stdout);
	}
	fclose(in);
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

int serve_run(int nallow, char **allow)
{
	static char buf[4096];
	char *argv[WORDS_MAX + 1];
	const char *cmd = getenv("SSH_ORIGINAL_COMMAND");
	int n, i;

	if (!cmd || !*cmd) {
		fprintf(stderr, "rsim-card --serve: no SSH_ORIGINAL_COMMAND — this is the command= of an authorized_keys line, "
			"not something to run by hand\n");
		return 2;
	}
	if ((n = serve_split(cmd, buf, sizeof(buf), argv, WORDS_MAX)) < 1)
		return refuse("not a command this can read", cmd);

	if (!strcmp(base(argv[0]), "rsim-card")) {
		const char *spec = NULL;
		int list = 0;

		for (i = 1; i < n; i++) {
			if (!strcmp(argv[i], "--list"))
				list = 1;
			else if (!strcmp(argv[i], "--serve"))
				return refuse("not again", "--serve");
			else if (argv[i][0] == '-') {
				int k = option_kind(argv[i]);

				if (!k)
					return refuse("an option this does not pass on", argv[i]);
				if (k == 2)
					i++;
			}
			else if (!spec)
				spec = argv[i];
			else
				return refuse("more than one reader", argv[i]);
		}
		if (!list && !spec)
			return refuse("no reader", cmd);
		if (!list && !allowed(spec, nallow, allow))
			return refuse("this reader is not served to this key", spec);
		if (!list && !device_ok(spec))
			return refuse("not a serial port under /dev", spec);
		/* ourselves, whatever path the client named: not a program of its
		 * choice */
		argv[0] = "rsim-card";
		{
			const char *self = test_hook("RSIM_TEST_SELF");

			drop_test_env();
			if (list && nallow) {
				fflush(stdout);
				return list_filtered(argv, nallow, allow, self ? self : "/proc/self/exe");
			}
			execv(self ? self : "/proc/self/exe", argv);
		}
		fprintf(stderr, "rsim-card --serve: exec: %s\n", strerror(errno));
		return 1;
	}

	if (!strcmp(base(argv[0]), "wwandctl") && n >= 3 && !strcmp(argv[1], "rsim") && !strcmp(argv[2], "proxy")) {
		const char *target = NULL;
		const char *wwandctl = test_hook("RSIM_TEST_WWANDCTL");
		char spec[300];
		int list = 0;

		for (i = 3; i < n; i++) {
			if (!strcmp(argv[i], "--list"))
				list = 1;
			else if (!strcmp(argv[i], "--mode") || !strcmp(argv[i], "--slot") || !strcmp(argv[i], "--apdu") ||
				 !strcmp(argv[i], "--cond"))
				i++;
			else if (argv[i][0] == '-')
				return refuse("an option the proxy does not have", argv[i]);
			else if (!target)
				target = argv[i];
			else
				return refuse("more than one modem", argv[i]);
		}
		if (!list) {
			if (!target)
				return refuse("no modem", cmd);
			snprintf(spec, sizeof(spec), "wwand:%s", target);
			if (!allowed(spec, nallow, allow))
				return refuse("this modem's card is not served to this key", spec);
		}
		argv[0] = "wwandctl";
		drop_test_env();
		if (list && nallow) {
			fflush(stdout);
			return list_filtered(argv, nallow, allow, wwandctl ? wwandctl : "/usr/bin/wwandctl");
		}
		execv(wwandctl ? wwandctl : "/usr/bin/wwandctl", argv);
		fprintf(stderr, "rsim-card --serve: wwandctl: %s — is wwand-rsim installed here?\n", strerror(errno));
		return 1;
	}

	return refuse("not a wwand-rsim command", cmd);
}
