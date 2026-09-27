#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 André Valentin <avalentin@marcant.net>
"""`rsim-card --serve`: the forced command of an authorized_keys line runs
wwand-rsim's own calls only, for the readers it is given, and nothing else.

usage: test_serve.py <path to rsim-card>
"""

import os
import subprocess
import sys
import tempfile

BIN = sys.argv[1] if len(sys.argv) > 1 else "./rsim-card"
checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAILED: %s" % what, file=sys.stderr)


with tempfile.TemporaryDirectory() as root:
    fake = os.path.join(root, "wwandctl")
    with open(fake, "w") as f:
        f.write("#!/bin/sh\necho \"wwandctl called: $*\"\n")
    os.chmod(fake, 0o755)
    base_env = dict(os.environ, RSIM_TEST_SYSROOT=root, RSIM_TEST_WWANDCTL=fake)
    base_env.pop("SSH_ORIGINAL_COMMAND", None)

    def serve(cmd, *allow):
        env = dict(base_env)
        if cmd is not None:
            env["SSH_ORIGINAL_COMMAND"] = cmd
        return subprocess.run([BIN, "--serve", *allow], input="", capture_output=True, text=True, timeout=30, env=env)

    p = serve(None)
    check(p.returncode == 2 and "authorized_keys" in p.stderr, "no SSH_ORIGINAL_COMMAND: says what it is for")

    p = serve("'rsim-card' '--list'", "bt:*")
    check(p.returncode == 0 and '"done":true' in p.stdout, "the scan is served (%r)" % p.stderr)

    p = serve("'/opt/rsim/rsim-card' '-v' 'at:/nonexistent/tty'", "at:*")
    check(p.returncode == 1 and "/nonexistent/tty" in p.stderr and "--serve" not in p.stderr,
          "an allowed reader: run (as ourselves, whatever path was named) (%r)" % p.stderr)

    p = serve("'rsim-card' 'at:/dev/ttyUSB2'", "bt:*", "wwand:m1")
    check(p.returncode == 1 and "not served to this key: at:/dev/ttyUSB2" in p.stderr, "a reader not given: refused")

    p = serve("'rsim-card' '--at-radio' 'keep' 'at:/nonexistent/x'", "at:/nonexistent/*")
    check("not served" not in p.stderr and "/nonexistent/x" in p.stderr, "an option's value is not taken for the reader")

    p = serve("'rsim-card' 'at:/nonexistent/x' 'bt:00:11:22:33:44:55'", "at:*")
    check(p.returncode == 1 and "more than one reader" in p.stderr, "two readers: refused")

    p = serve("'rsim-card' '--serve'", "at:*")
    check(p.returncode == 1 and "not again" in p.stderr, "--serve inside: refused")

    # the plugin's quoting of a PC/SC name with a quote in it
    p = serve("'rsim-card' 'pcsc:ACS ACR38U '\\''x'\\'''", "bt:*")
    check("not served to this key: pcsc:ACS ACR38U 'x'" in p.stderr, "shell quoting is read back exactly (%r)" % p.stderr)

    p = serve("'wwandctl' 'rsim' 'proxy' 'iccid:89490200001022832490' '--mode' 'apdu'", "wwand:iccid:*")
    check(p.returncode == 0 and "wwandctl called: rsim proxy iccid:89490200001022832490 --mode apdu" in p.stdout,
          "the proxy of a wwand router: run (%r)" % (p.stdout + p.stderr))
    p = serve("'wwandctl' 'rsim' 'proxy' 'wwmodem1'", "wwand:wwmodem0")
    check(p.returncode == 1 and "wwand:wwmodem1" in p.stderr, "another modem there: refused")
    p = serve("'wwandctl' 'rsim' 'proxy' '--list'", "wwand:wwmodem0")
    check(p.returncode == 0 and "proxy --list" in p.stdout, "the proxy's list: served")
    p = serve("'wwandctl' 'rsim' 'proxy' 'm0' '--evil'")
    check(p.returncode == 1 and "--evil" in p.stderr, "an option the proxy does not have: refused")
    p = serve("'wwandctl' 'status'")
    check(p.returncode == 1 and "not a wwand-rsim command" in p.stderr, "any other wwandctl command: refused")

    for bad in ("rm -rf /", "rsim-card; reboot", "sh -c 'rsim-card --list'", "'rsim-card' 'unterminated", ""):
        p = serve(bad, "*")
        check(p.returncode != 0 and "wwandctl called" not in p.stdout and '"done"' not in p.stdout,
              "refused: %r (%r)" % (bad, p.stderr))

    # literal names: escaped brackets match the reader's own brackets only
    p = serve("'rsim-card' 'pcsc:SCM SCR 3310 [CCID Interface] 00 00'", "pcsc:SCM SCR 3310 \\[CCID Interface\\] 00 00")
    check("not served" not in p.stderr, "an escaped [..] matches the name with brackets (%r)" % p.stderr)
    p = serve("'rsim-card' 'pcsc:SCM SCR 3310 C 00 00'", "pcsc:SCM SCR 3310 \\[CCID Interface\\] 00 00")
    check("not served" in p.stderr, "...and not a name the class would have matched")

    # inside an SSH session the test hooks are the client's: ignored, removed
    env = dict(base_env, SSH_CONNECTION="10.0.0.2 5000 10.0.0.1 22",
               SSH_ORIGINAL_COMMAND="'wwandctl' 'rsim' 'proxy' 'm1'", RSIM_TEST_WWANDCTL=fake)
    p = subprocess.run([BIN, "--serve"], input="", capture_output=True, text=True, timeout=30, env=env)
    check("wwandctl called" not in p.stdout, "ssh session: RSIM_TEST_WWANDCTL is not honoured (%r)" % p.stdout)
    env["SSH_ORIGINAL_COMMAND"] = "'rsim-card' '--list'"
    env["RSIM_TEST_WWAND_LIST"] = "echo INJECTED"
    p = subprocess.run([BIN, "--serve"], input="", capture_output=True, text=True, timeout=30, env=env)
    check("INJECTED" not in p.stdout and '"done":true' in p.stdout, "ssh session: no test hook reaches the served call (%r)" % p.stdout[-200:])

    p = serve("rsim-card --list")
    check(p.returncode == 0 and '"done":true' in p.stdout, "no specs: every reader, plain words too")

print("test_serve: %d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
