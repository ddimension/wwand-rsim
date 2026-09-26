#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 André Valentin <avalentin@marcant.net>
"""End-to-end: rsim-card's at: backend against a simulated modem AT port on
a pseudo-terminal — the card of another modem, APDUs over AT+CSIM, its radio
off while the card is used elsewhere and back afterwards.

usage: test_e2e_at.py <path to rsim-card>
"""

import json
import os
import select
import signal
import subprocess
import sys
import threading
import time
import tty

BIN = sys.argv[1] if len(sys.argv) > 1 else "./rsim-card"
checks = 0
failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAILED: %s" % what, file=sys.stderr)


class FakeModem(threading.Thread):
    """Answers AT on the master side: CFUN, CPIN, CSIM, like a Quectel."""

    def __init__(self, fd, card=True, cfun=1):
        super().__init__(daemon=True)
        self.fd = fd
        self.card = card
        self.cfun = cfun
        self.log = []
        self.running = True

    def say(self, *lines):
        os.write(self.fd, b"".join(b"\r\n" + l.encode() + b"\r\n" for l in lines))

    def answer(self, cmd):
        self.log.append(cmd)
        if cmd in ("ATE0", "AT+CMEE=1", "AT"):
            return self.say("OK")
        if cmd == "AT+CFUN?":
            return self.say("+CFUN: %d" % self.cfun, "OK")
        if cmd.startswith("AT+CFUN="):
            self.cfun = int(cmd[8:])
            return self.say("OK")
        if cmd == "AT+CPIN?":
            return self.say("+CPIN: READY", "OK") if self.card else self.say("+CME ERROR: 10")
        if cmd.startswith("AT+CSIM="):
            n, apdu = cmd[8:].split(",", 1)
            apdu = apdu.strip('"')
            if int(n) != len(apdu):
                return self.say("+CME ERROR: 50")
            if apdu.startswith("00A40004023F00"):
                resp = "6124"                   # SELECT MF: response waiting
            elif apdu.startswith("00C0000024"):
                resp = "62" + "00" * 35 + "9000"
            else:
                resp = "6D00"
            return self.say('+CSIM: %d,"%s"' % (len(resp), resp), "OK")
        self.say("ERROR")

    def run(self):
        buf = b""
        while self.running:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if not r:
                continue
            try:
                buf += os.read(self.fd, 4096)
            except OSError:
                return
            while b"\r" in buf:
                line, buf = buf.split(b"\r", 1)
                line = line.strip().decode(errors="replace")
                if line:
                    self.answer(line)


class Rig:
    def __init__(self, args=(), **modem):
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        self.modem = FakeModem(self.master, **modem)
        self.modem.start()
        self.proc = subprocess.Popen(
            [BIN, "-v"] + list(args) + ["at:" + os.ttyname(self.slave)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def ask(self, req, timeout=15):
        self.proc.stdin.write((json.dumps(req) + "\n").encode())
        self.proc.stdin.flush()
        r, _, _ = select.select([self.proc.stdout], [], [], timeout)
        return json.loads(self.proc.stdout.readline()) if r else None

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(10)
        self.modem.running = False


# --- the card of another modem -----------------------------------------------
rig = Rig()
up = rig.ask({"op": "power_up"})
check(up and up.get("ok") and up.get("atr") == "3B00", "power_up: the minimal T=0 ATR (%r)" % up)
check(rig.modem.cfun == 4, "radio: the modem is in CFUN=4 while its card is used elsewhere")
r = rig.ask({"op": "tpdu", "data": "00A40004023F00"})
check(r and r.get("data") == "6124", "tpdu: SELECT MF through AT+CSIM (%r)" % r)
check(any(c == 'AT+CSIM=14,"00A40004023F00"' for c in rig.modem.log),
      "tpdu: the length is in hex characters (TS 27.007 §8.17)")
r = rig.ask({"op": "tpdu", "data": "00C0000024"})
check(r and r.get("data", "").endswith("9000") and len(r["data"]) == 2 * 38, "tpdu: GET RESPONSE, 36 bytes + SW")
st = rig.ask({"op": "status"})
check(st and st.get("backend") == "at", "status: backend at")
rig.close()
check(rig.modem.cfun == 1, "radio: back to the mode it had (CFUN=1) at the end")

# --- a stop signal restores the radio too (an SSH link that dropped) -----------
rig = Rig()
rig.ask({"op": "power_up"})
rig.proc.send_signal(signal.SIGHUP)
try:
    rig.proc.wait(10)
    exited = True
except subprocess.TimeoutExpired:
    rig.proc.kill()
    exited = False
check(exited, "SIGHUP: the helper ends")
check(rig.modem.cfun == 1, "SIGHUP: the radio is switched back on before the helper exits")
rig.modem.running = False

# --- keep: the radio is left alone ------------------------------------------------
rig = Rig(args=["--at-radio", "keep"])
rig.ask({"op": "power_up"})
check(not any(c.startswith("AT+CFUN") for c in rig.modem.log), "--at-radio keep: no CFUN at all")
rig.close()

# --- no card in that modem -----------------------------------------------------------
rig = Rig(card=False)
up = rig.ask({"op": "power_up"})
check(up and not up.get("ok") and up.get("error") == "no_card", "no card: power_up says no_card (%r)" % up)
rig.close()

print("test_e2e_at: %d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
