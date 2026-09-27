/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#ifndef RSIM_SERVE_H
#define RSIM_SERVE_H

/* `rsim-card --serve [SPEC...]`: the forced command of an authorized_keys
 * line (command="..."). Runs what the SSH client asked for
 * (SSH_ORIGINAL_COMMAND) only when it is one of wwand-rsim's own calls —
 * rsim-card with a reader, rsim-card --list, wwandctl rsim proxy — and, when
 * specs are given, only for a reader one of them matches (fnmatch).
 * Returns only on refusal (exit status); otherwise it execs. */
int serve_run(int nallow, char **allow);

/* exposed for the tests: SSH_ORIGINAL_COMMAND split into words the way a
 * POSIX shell would for plain words, '…' and "…" and backslashes (no
 * expansion of any kind). The count, -1 on unbalanced quotes or too many. */
int serve_split(const char *cmd, char *buf, size_t cap, char **argv, int max);

#endif
