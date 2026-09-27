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

    # a terminal to name as a reader: nothing answers on it, which is fine —
    # these checks are about what --serve lets through, not about the reader
    import pty as _pty
    _master, _slave = _pty.openpty()
    TTY = os.ttyname(_slave)

    p = serve(None)
    check(p.returncode == 2 and "authorized_keys" in p.stderr, "no SSH_ORIGINAL_COMMAND: says what it is for")

    p = serve("'rsim-card' '--list'", "bt:*")
    check(p.returncode == 0 and '"done":true' in p.stdout, "the scan is served (%r)" % p.stderr)

    p = serve("'/opt/rsim/rsim-card' '-v' 'phoenix:%s'" % TTY, "phoenix:/dev/pts/*")
    check(TTY in p.stderr and "--serve" not in p.stderr,
          "an allowed reader: run (as ourselves, whatever path was named) (%r)" % p.stderr)

    p = serve("'rsim-card' 'at:/dev/ttyUSB2'", "bt:*", "wwand:m1")
    check(p.returncode == 1 and "not served to this key: at:/dev/ttyUSB2" in p.stderr, "a reader not given: refused")

    p = serve("'rsim-card' '--at-radio' 'keep' 'phoenix:%s'" % TTY, "phoenix:/dev/pts/*")
    check("not served" not in p.stderr and "--serve" not in p.stderr and TTY in p.stderr,
          "an option's value is not taken for the reader (%r)" % p.stderr)

    # A FILE IS NOT A MODEM PORT. An open key (or one allowing `at:*`) could
    # read a file out over SSH and write AT commands over it (found by audit,
    # 2026-09-27): only a character device under /dev is opened, and `*`
    # does not cross a `/`.
    victim = os.path.join(root, "victim")
    with open(victim, "w") as f:
        f.write("secret\n")
    p = serve("'rsim-card' '-v' 'at:%s'" % victim)
    check(p.returncode == 1 and "not a serial port under /dev" in p.stderr, "a file named as an AT port: refused (%r)" % p.stderr)
    p = serve("'rsim-card' 'at:/dev/../%s'" % victim.lstrip("/"))
    check(p.returncode == 1 and "not a serial port under /dev" in p.stderr, "...also through /dev/.. (%r)" % p.stderr)
    p = serve("'rsim-card' 'phoenix:%s'" % victim, "phoenix:*")
    check(p.returncode == 1 and "not served" in p.stderr, "`*` does not cross a `/` (%r)" % p.stderr)
    with open(victim) as f:
        check(f.read() == "secret\n", "...and the file is left as it was")
    p = subprocess.run([BIN, "at:%s" % victim], input="", capture_output=True, text=True, timeout=30, env=base_env)
    check(p.returncode == 1 and "not a serial port" in p.stderr,
          "rsim-card itself refuses a file as an AT port, --serve or not (%r)" % p.stderr)

    # only rsim-card's own options, spelled out
    p = serve("'rsim-card' '--' 'at:%s'" % TTY, "at:/dev/pts/*")
    check(p.returncode == 1 and "an option this does not pass on: --" in p.stderr, "`--` refused (%r)" % p.stderr)
    p = serve("'rsim-card' '--at-r' 'keep' 'at:%s'" % TTY, "at:/dev/pts/*")
    check(p.returncode == 1 and "an option this does not pass on" in p.stderr, "an abbreviated option refused")
    p = serve("'rsim-card' '--at-radio=keep' 'phoenix:%s'" % TTY, "phoenix:/dev/pts/*")
    check("--serve" not in p.stderr, "--opt=value passed on (%r)" % p.stderr)

    # a character device that is not a serial port (/dev/null here; /dev/mtdN,
    # /dev/mem on a router): refused by --serve, and by rsim-card itself
    p = serve("'rsim-card' 'at:/dev/null'")
    check(p.returncode == 1 and "not a serial port under /dev" in p.stderr, "/dev/null as an AT port: refused (%r)" % p.stderr)
    p = subprocess.run([BIN, "at:/dev/null"], input="", capture_output=True, text=True, timeout=30, env=base_env)
    check(p.returncode == 1 and "not a serial port" in p.stderr, "...by rsim-card itself too (%r)" % p.stderr)

    # a restricted key sees only its readers in the scan
    selfbin = os.path.join(root, "self")
    with open(selfbin, "w") as f:
        f.write('#!/bin/sh\necho \'{"backend":"bt","spec":"bt:AA:BB:CC:DD:EE:01"}\'\n'
                'echo \'{"backend":"wwand","spec":"wwand:iccid:8949","iccid":"8949","config":{"apn":"x"}}\'\n'
                'echo \'{"done":true}\'\n')
    os.chmod(selfbin, 0o755)
    env = dict(base_env, RSIM_TEST_SELF=selfbin, SSH_ORIGINAL_COMMAND="'rsim-card' '--list'")
    p = subprocess.run([BIN, "--serve", "bt:*"], input="", capture_output=True, text=True, timeout=30, env=env)
    check(p.returncode == 0 and "bt:AA:BB" in p.stdout and "8949" not in p.stdout and '"done":true' in p.stdout,
          "--list for a restricted key: its readers only (%r)" % p.stdout)

    # ...and so does the proxy's list of a wwand router's cards, by ICCID or
    # by the modem they sit in
    fake2 = os.path.join(root, "wwandctl2")
    with open(fake2, "w") as f:
        f.write('#!/bin/sh\necho \'{"backend":"wwand","spec":"wwand:iccid:8949","modem":"m0","imsi":"26201"}\'\n'
                'echo \'{"backend":"wwand","spec":"wwand:iccid:8988","modem":"m1","imsi":"90128"}\'\n')
    os.chmod(fake2, 0o755)
    env = dict(base_env, RSIM_TEST_WWANDCTL=fake2, SSH_ORIGINAL_COMMAND="'wwandctl' 'rsim' 'proxy' '--list'")
    p = subprocess.run([BIN, "--serve", "wwand:m1"], input="", capture_output=True, text=True, timeout=30, env=env)
    check(p.returncode == 0 and "8988" in p.stdout and "8949" not in p.stdout,
          "proxy --list for a key to one modem: that modem's card only (%r)" % p.stdout)

    p = serve("'rsim-card' 'at:%s' 'bt:00:11:22:33:44:55'" % TTY, "at:/dev/pts/*")
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
