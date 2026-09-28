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

    def __init__(self, fd, card=True, cfun=1, urc=False, cpin_err=None, csim_err=None, cfun_refuse=False,
                 cops='+COPS: 0,0,"Telekom.de",7', cops_refuse=False):
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
        self.cops = cops            # the +COPS? answer: how it selects its network
        self.cops_refuse = cops_refuse
        self.cops_set = []          # every AT+COPS=… it took

    def say(self, *lines):
        os.write(self.fd, b"".join(b"\r\n" + l.encode() + b"\r\n" for l in lines))

    def answer(self, cmd):
        self.log.append(cmd)
        if self.echo:
            os.write(self.fd, cmd.encode() + b"\r")
        if getattr(self, "locked", False):          # a phone that filters AT (Samsung)
            return self.say("PACM(AP),NOT_ALLOWED_CRO", "OK")
        if getattr(self, "samsung", False) and cmd not in ("ATE0", "AT"):
            # Samsung's own spelling of a refusal, followed by OK
            if cmd.startswith("AT+CSIM"):
                return self.say("ERROR")
            return self.say("+CME Error:PACM(AP),UNREGISTED", "OK")
        # a modem that still answers the queries but nothing that changes
        # its state (hung in a detach): the commands that time out
        if getattr(self, "mute", False) and cmd not in ("AT+CPIN?", "AT+CFUN?"):
            return
        if self.urc:
            self.say("+QIND: \"csq\",20,99")
        if cmd == "ATE0":
            self.echo = False
            return self.say("OK")
        if cmd in ("AT+CMEE=1", "AT"):
            return self.say("OK")
        # who it is: plain lines like a Quectel; the ICCID only through
        # the vendor command, with the trailing F of a 19/20-digit one
        if cmd == "AT+CGMI":
            return self.say("Quectel", "OK")
        if cmd == "AT+CGMM":
            return self.say("EG06", "OK")
        if cmd == "AT+CGMR":
            return self.say("EG06ELAR04A08M4G", "OK")
        if cmd == "AT+CGSN":
            return self.say("861234567890123", "OK")
        if cmd == "AT+QCCID":
            return self.say("+QCCID: 8949020000184496711F", "OK")
        if cmd == "AT+CFUN?":
            return self.say("+CFUN: %d" % self.cfun, "OK")
        if cmd.startswith("AT+CFUN="):
            if self.cfun_refuse and cmd == "AT+CFUN=4":
                return self.say("+CME ERROR: 3")
            if not getattr(self, "cfun_stuck", False):  # a modem that says OK and does not do it
                self.cfun = int(cmd[8:])
            return self.say("OK")
        if cmd == "AT+COPS?":
            return self.say(self.cops, "OK")
        if cmd.startswith("AT+COPS="):
            if self.cops_refuse:
                return self.say("+CME ERROR: 30")
            self.cops_set.append(cmd[8:])
            self.cops = "+COPS: 2" if cmd == "AT+COPS=2" else "+COPS: " + cmd[8:]
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


# where rsim-card keeps a lent modem's radio mode: a directory of its own
MARKDIR = "/tmp/rsim-card-%d" % os.geteuid()


def mark_of(slave):
    return os.path.join(MARKDIR, "cfun-" + os.ttyname(slave).replace("/", "_"))


class Rig:
    def __init__(self, args=(), reuse=None, premark=None, **modem):
        if reuse:
            # the same modem on the same port, for a second helper run
            self.master, self.slave, self.modem = reuse.master, reuse.slave, reuse.modem
            self.modem.log.clear()
        else:
            self.master, self.slave = os.openpty()
            tty.setraw(self.slave)
            self.modem = FakeModem(self.master, **modem)
            self.modem.start()
        if premark is not None:
            # what a run before this one left: its mark file
            os.makedirs(MARKDIR, mode=0o700, exist_ok=True)
            with os.fdopen(os.open(mark_of(self.slave), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
                f.write(premark)
        self.proc = subprocess.Popen(
            [BIN, "-v"] + list(args) + ["at:" + os.ttyname(self.slave)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buf = b""

    def ask(self, req, timeout=15):
        self.proc.stdin.write((json.dumps(req) + "\n").encode())
        self.proc.stdin.flush()
        while True:
            # our own buffer: select() on a buffered stream misses a line
            # readline() already pulled in with the one before
            while b"\n" not in self.buf:
                r, _, _ = select.select([self.proc.stdout], [], [], timeout)
                if not r:
                    return None
                chunk = os.read(self.proc.stdout.fileno(), 4096)
                if not chunk:
                    return None
                self.buf += chunk
            l, self.buf = self.buf.split(b"\n", 1)
            o = json.loads(l)
            # the info event after the open is news, not an answer
            if o.get("event") == "info":
                self.info = o
                continue
            return o

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
check(st.get("modem_manufacturer") == "Quectel" and st.get("modem_model") == "EG06"
      and st.get("modem_revision") == "EG06ELAR04A08M4G" and st.get("modem_imei") == "861234567890123",
      "status: who the modem is (%r)" % st)
check(st.get("iccid") == "8949020000184496711", "status: its card's ICCID, through the vendor command, F dropped (%r)" % st.get("iccid"))
check(getattr(rig, "info", {}).get("modem_model") == "EG06", "info event after the open: the same")
log = [c for c in rig.modem.log if c.startswith(("AT+COPS", "AT+CFUN="))]
check(log[:3] == ["AT+COPS?", "AT+COPS=2", "AT+CFUN=4"],
      "park: its network selection read, deregistered (COPS=2), THEN the radio off (%r)" % log)
rig.close()
check(rig.modem.cfun == 1, "radio: back to the mode it had (CFUN=1) at the end")
log = [c for c in rig.modem.log if c.startswith(("AT+COPS", "AT+CFUN="))]
check(log[-2:] == ["AT+CFUN=1", "AT+COPS=0"] and rig.modem.cops == "+COPS: 0",
      "end: the radio on, then automatic network selection again (%r)" % log[-2:])

# --- a manual operator comes back as it was --------------------------------------
rig = Rig(cops='+COPS: 1,2,"26201",7')
rig.ask({"op": "power_up"})
check(rig.modem.cops == "+COPS: 2", "manual: deregistered")
rig.close()
check(rig.modem.cops_set[-1] == '1,2,"26201",7', "manual: the same operator, format and AcT again (%r)" % rig.modem.cops_set)

# --- deregistered already: nothing to put back -----------------------------------
rig = Rig(cops="+COPS: 2")
rig.ask({"op": "power_up"})
rig.close()
check(rig.modem.cops_set == ["2"], "already deregistered: left so at the end (%r)" % rig.modem.cops_set)

# --- deregistered, but the radio refuses to go off: back on the network ------------
rig = Rig(cfun_refuse=True)
rig.ask({"op": "power_up"}, timeout=10)
try:
    rig.proc.wait(20)
except subprocess.TimeoutExpired:
    pass
check(rig.proc.returncode == 1 and rig.modem.cops_set == ["2", "0"],
      "CFUN=4 refused: not lent, and its network selection put back (%r)" % rig.modem.cops_set)
rig.modem.running = False

# --- a run before deregistered and was killed before CFUN=4: its mark wins ------
rig = Rig(premark="1\nAT+COPS=0\n", cops="+COPS: 2")
rig.ask({"op": "power_up"})
rig.close()
check(rig.modem.cops_set[-1:] == ["0"] and rig.modem.cops == "+COPS: 0",
      "an earlier run's mark: its selection comes back, not the COPS=2 it left (%r)" % rig.modem.cops_set)

# --- the mark is believed only when it is ours --------------------------------------
# Its second line is sent to the modem. A mark that is a link, or one others
# may write, is ignored: with the modem off (CFUN=4) nothing is then taken as
# "how it was", and nothing is sent at the end.
bait = os.path.join(MARKDIR, "bait")
with open(bait, "w") as f:
    f.write("1\nAT+COPS=1,2,\"99999\"\n")
for how in ("link", "open"):
    rig = Rig(cfun=4, cops="+COPS: 2")
    m = mark_of(rig.slave)
    if how == "link":
        os.symlink(bait, m)
    else:
        with open(m, "w") as f:
            f.write("1\nAT+COPS=1,2,\"99999\"\n")
        os.chmod(m, 0o666)
    rig.ask({"op": "power_up"})
    rig.close()
    check(rig.modem.cfun == 4 and not any("99999" in c for c in rig.modem.cops_set),
          "mark as a %s: not believed, nothing of it sent (cfun %d, %r)" % (how, rig.modem.cfun, rig.modem.cops_set))
    if os.path.lexists(m):
        os.unlink(m)
os.unlink(bait)

# ...nor kept in a directory others can write to: then the radio is not
# switched off at all (its mode could not be kept safely)
os.chmod(MARKDIR, 0o777)
rig = Rig()
rig.ask({"op": "power_up"}, timeout=10)
try:
    rig.proc.wait(10)
except subprocess.TimeoutExpired:
    rig.proc.stdin.close()                       # it lent the card: ends it
    rig.proc.wait(20)
err = rig.proc.stderr.read().decode(errors="replace")
os.chmod(MARKDIR, 0o700)
check(rig.proc.returncode == 1 and rig.modem.cfun == 1 and "cannot keep the radio's mode" in err,
      "a mark directory open to others: not trusted, the modem not parked (%r)" % err[-200:])
rig.modem.running = False

# --- a modem that refuses COPS=2 is still parked ---------------------------------
rig = Rig(cops_refuse=True)
up = rig.ask({"op": "power_up"})
check(up and up.get("ok") and rig.modem.cfun == 4, "COPS=2 refused: parked with CFUN=4 alone (%r)" % up)
rig.close()
check(rig.modem.cfun == 1, "COPS=2 refused: radio back at the end")

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
check(rig2.modem.cops_set[-1:] == ["0"] and rig2.modem.cops == "+COPS: 0",
      "after SIGKILL: ...and its network selection from the kept file, not the COPS=2 it finds (%r)" % rig2.modem.cops_set)
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
ans, buf = None, b""
deadline = time.time() + 20
while time.time() < deadline:
    if b"\n" not in buf:
        r, _, _ = select.select([second.stdout], [], [], max(0.1, deadline - time.time()))
        if not r:
            break
        chunk = os.read(second.stdout.fileno(), 4096)
        if not chunk:
            break
        buf += chunk
        continue
    l, buf = buf.split(b"\n", 1)
    ans = json.loads(l)
    if ans.get("event") != "info":        # the info event after its open first
        break
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
rig.modem.cops = '+COPS: 0,0,"Telekom.de",7'   # ...and registered again
rig.modem.log.clear()
rig.ask({"op": "reset"})
check(rig.modem.cfun == 4, "reboot: the next reset from the target switches its radio off again")
log = [c for c in rig.modem.log if c.startswith(("AT+COPS=", "AT+CFUN="))]
check(log == ["AT+COPS=2", "AT+CFUN=4"], "reboot: deregistered again before the radio goes off (%r)" % log)
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

# ...and a re-park that hangs is answered within the plugin's 15 s for a
# power-up or reset: past it the plugin restarts the helper, which restores
# the radio it was switching off
rig = Rig()
rig.ask({"op": "power_up"})
rig.modem.cfun = 1
rig.modem.mute = True
t0 = time.monotonic()
r = rig.ask({"op": "reset"}, timeout=30)
took = time.monotonic() - t0
check(r is not None and took < 14, "reboot: a hanging re-park is answered within the plugin's 15 s (%.1f s, %r)" % (took, r))
check(r and not r.get("ok") and r.get("error") == "io", "reboot: ...as a failure, no ATR (%r)" % r)
rig.modem.mute = False
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
check(not any(c.startswith("AT+COPS") for c in rig.modem.log), "--at-radio keep: no deregistration either")
rig.close()

# --- no card in that modem -----------------------------------------------------------
rig = Rig(card=False)
up = rig.ask({"op": "power_up"})
check(up and not up.get("ok") and up.get("error") == "no_card", "no card: power_up says no_card (%r)" % up)
rig.close()

for f in os.listdir(MARKDIR):                    # the state files of these runs
    if f.startswith("cfun-_dev_pts_"):
        os.unlink(os.path.join(MARKDIR, f))

# --- the park is confirmed, like a QMI park ------------------------------------------
rig = Rig()
rig.modem.cfun_stuck = True
rig.ask({"op": "power_up"}, timeout=10)
try:
    rig.proc.wait(10)
except subprocess.TimeoutExpired:
    pass
err = rig.proc.stderr.read().decode(errors="replace")
check(rig.proc.returncode == 1 and "reads back as 1" in err, "a park that does not hold: no lending (%r)" % err[-200:])

# --- ...and kept while the card is lent: parked again when it comes back on -----------
rig = Rig()
rig.ask({"op": "power_up"})
check(rig.modem.cfun == 4, "lent: parked")
rig.modem.cfun = 1                                # a modem that restarted, a hand elsewhere
time.sleep(12)                                    # the 10 s look
check(rig.modem.cfun == 4, "lent: switched back on, parked again within 10 s")
rig.close()
check(rig.modem.cfun == 1, "the end: back to what it was")

# --- AT+CSIM refused as a command: refused at the open --------------------------------
rig = Rig(csim_err="ERROR")
rig.ask({"op": "power_up"}, timeout=10)
try:
    rig.proc.wait(10)
except subprocess.TimeoutExpired:
    pass
err = rig.proc.stderr.read().decode(errors="replace")
check(rig.proc.returncode == 1 and "refuses AT+CSIM" in err, "CSIM refused: the open fails and says why (%r)" % err[-160:])

# --- Samsung: "+CME Error:" (mixed case) is a refusal, not a value -----------------
rig = Rig()
rig.modem.samsung = True
rig.ask({"op": "power_up"}, timeout=10)
try:
    rig.proc.wait(10)
except subprocess.TimeoutExpired:
    pass
err = rig.proc.stderr.read().decode(errors="replace")
check(rig.proc.returncode == 1 and "refuses AT+CSIM" in err and "PACM" not in err.split("refuses")[0][-40:],
      "Samsung: its refusals are refusals, and the CSIM probe stops the open (%r)" % err[-200:])

# --- a phone whose AT access to its modem is locked (Samsung) ---------------------
rig = Rig()
rig.modem.locked = True
up = rig.ask({"op": "power_up"}, timeout=10)
try:
    rig.proc.wait(10)
except subprocess.TimeoutExpired:
    pass
err = rig.proc.stderr.read().decode(errors="replace")
check(rig.proc.returncode == 1 and "AT access is locked" in err,
      "locked AT: refused at the open, said so, no card claimed (%r)" % err[-200:])

print("test_e2e_at: %d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
