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

    def __init__(self, fd, card=True, cfun=1, urc=False, cpin_err=None, csim_err=None, cfun_refuse=False):
        super().__init__(daemon=True)
        self.fd = fd
        self.card = card
        self.cfun = cfun
        self.log = []
        self.running = True
        self.echo = True        # like a modem fresh from reset: ATE1
        self.urc = urc          # an unsolicited result before every answer
        self.cpin_err = cpin_err
        self.csim_err = csim_err
        self.cfun_refuse = cfun_refuse

    def say(self, *lines):
        os.write(self.fd, b"".join(b"\r\n" + l.encode() + b"\r\n" for l in lines))

    def answer(self, cmd):
        self.log.append(cmd)
        if self.echo:
            os.write(self.fd, cmd.encode() + b"\r")
        if self.urc:
            self.say("+QIND: \"csq\",20,99")
        if cmd == "ATE0":
            self.echo = False
            return self.say("OK")
        if cmd in ("AT+CMEE=1", "AT"):
            return self.say("OK")
        if cmd == "AT+CFUN?":
            return self.say("+CFUN: %d" % self.cfun, "OK")
        if cmd.startswith("AT+CFUN="):
            if self.cfun_refuse and cmd == "AT+CFUN=4":
                return self.say("+CME ERROR: 3")
            self.cfun = int(cmd[8:])
            return self.say("OK")
        if cmd == "AT+CPIN?":
            if self.cpin_err:
                return self.say(self.cpin_err)
            return self.say("+CPIN: READY", "OK") if self.card else self.say("+CME ERROR: 10")
        if cmd.startswith("AT+CSIM="):
            if self.csim_err:
                return self.say(self.csim_err)
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
    def __init__(self, args=(), reuse=None, **modem):
        if reuse:
            # the same modem on the same port, for a second helper run
            self.master, self.slave, self.modem = reuse.master, reuse.slave, reuse.modem
            self.modem.log.clear()
        else:
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

# --- killed hard: the next run still knows the radio was on -----------------------
rig = Rig()
rig.ask({"op": "power_up"})
rig.proc.kill()
rig.proc.wait(10)
check(rig.modem.cfun == 4, "SIGKILL: no cleanup possible, the radio stays off for now")
rig2 = Rig(reuse=rig)
up = rig2.ask({"op": "power_up"})
check(up and up.get("ok"), "after SIGKILL: the next run works")
rig2.close()
check(rig2.modem.cfun == 1, "after SIGKILL: the next run switches the radio back on at its end (kept mode 1)")
rig.modem.running = False

# --- one helper per port -------------------------------------------------------------
rig = Rig()
rig.ask({"op": "power_up"})
t0 = time.monotonic()
second = subprocess.run([BIN, "at:" + os.ttyname(rig.slave)], input=b"", capture_output=True, timeout=40)
waited = time.monotonic() - t0
check(second.returncode != 0, "lock: a second helper on the same port is refused")
check(15 < waited < 30, "lock: ...after a bounded wait (the first may be restoring the radio) (%.1f s)" % waited)
check(rig.modem.cfun == 4, "lock: ...and the first one's radio stays off")
rig.close()
check(rig.modem.cfun == 1, "lock: the first one restores it at its end")

# --- the lock passes on when its holder ends ---------------------------------------
rig = Rig()
rig.ask({"op": "power_up"})
second = subprocess.Popen([BIN, "at:" + os.ttyname(rig.slave)], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
time.sleep(1)
rig.proc.stdin.close()                       # the first ends, restores, unlocks
rig.proc.wait(20)
second.stdin.write(b'{"op":"power_up"}\n')
second.stdin.flush()
r, _, _ = select.select([second.stdout], [], [], 20)
ans = json.loads(second.stdout.readline()) if r else None
check(ans and ans.get("ok"), "lock: the waiting helper takes over once the first has ended (%r)" % ans)
check(rig.modem.cfun == 4, "lock: ...and has the radio off again")
second.stdin.close()
second.wait(20)
check(rig.modem.cfun == 1, "lock: ...and back on at its end")
rig.modem.running = False

# --- echo and unsolicited results do not confuse the answers -----------------------
rig = Rig(urc=True)
up = rig.ask({"op": "power_up"})
r = rig.ask({"op": "tpdu", "data": "00A40004023F00"})
check(up and up.get("ok") and r and r.get("data") == "6124", "URC + echo: the answers are still read right (%r)" % r)
rig.close()

# --- a modem that rebooted during the lending is switched off again -----------------
rig = Rig()
rig.ask({"op": "power_up"})
rig.modem.cfun = 1                               # RDY: back in its default mode
rig.ask({"op": "reset"})
check(rig.modem.cfun == 4, "reboot: the next reset from the target switches its radio off again")
rig.close()
check(rig.modem.cfun == 1, "reboot: ...and it is still restored at the end")

# ...and when it cannot be switched off again, the card is not lent
rig = Rig()
rig.ask({"op": "power_up"})
rig.modem.cfun = 1
rig.modem.cfun_refuse = True
r = rig.ask({"op": "reset"})
check(r and not r.get("ok") and r.get("error") == "io",
      "reboot: a radio that cannot be switched off again fails the reset, no ATR (%r)" % r)
rig.modem.cfun_refuse = False
rig.close()

# --- what the modem says, classified ---------------------------------------------------
rig = Rig(cpin_err="+CME ERROR: 14")
up = rig.ask({"op": "power_up"})
check(up and up.get("error") == "io", "CME 14 (SIM busy): a card that is there, not no_card (%r)" % up)
rig.close()
rig = Rig(csim_err="+CME ERROR: 100")
rig.ask({"op": "power_up"})
r = rig.ask({"op": "tpdu", "data": "00A40004023F00"})
check(r and r.get("error") == "io", "CME 100 (unknown) is a refused command, not CME 10 (%r)" % r)
rig.close()
rig = Rig(csim_err="+CME ERROR: 10")
rig.ask({"op": "power_up"})
r = rig.ask({"op": "tpdu", "data": "00A40004023F00"})
check(r and r.get("error") == "no_card", "CME 10 during a command: the card has left (%r)" % r)
rig.close()
bad = subprocess.run([BIN, "--at-baud", "1234", "at:/dev/null"], capture_output=True, timeout=10)
check(bad.returncode == 2, "an --at-baud the port cannot take is refused")

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

for f in os.listdir("/tmp"):                     # the state files of these runs
    if f.startswith("rsim-card-cfun-_dev_pts_"):
        os.unlink(os.path.join("/tmp", f))

print("test_e2e_at: %d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
