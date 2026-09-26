#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 André Valentin <avalentin@marcant.net>
"""End-to-end: rsim-card's JSON line protocol, its Phoenix backend and the
T=0 engine against a simulated card on a pseudo-terminal (fakecard.py).

usage: test_e2e.py <path to rsim-card>
"""

import json
import os
import select
import subprocess
import sys
import tempfile
import time
import tty

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fakecard import FakeCard, FCP_MF, EF_DATA  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else "./rsim-card"
checks = 0
failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAILED: %s" % what, file=sys.stderr)


class Rig:
    """rsim-card on the slave side of a pty, FakeCard on the master side."""

    def __init__(self, args=(), **card):
        self.tmp = tempfile.TemporaryDirectory()
        fifo = os.path.join(self.tmp.name, "mctrl")
        os.mkfifo(fifo)
        self.master, self.slave = os.openpty()
        # raw before rsim-card configures it, so nothing the card sends
        # early is cooked or echoed by the line discipline
        tty.setraw(self.slave)
        self.card = FakeCard(self.master, fifo, **card)
        self.card.start()
        self.errlog = open(os.path.join(self.tmp.name, "stderr"), "w+")
        env = dict(os.environ, RSIM_TEST_MCTRL=fifo)
        self.proc = subprocess.Popen(
            [BIN, "-v"] + list(args) + ["phoenix:" + os.ttyname(self.slave)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.errlog, env=env)
        self.buf = b""

    def line(self, timeout=5.0):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buf:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.proc.stdout], [], [], left)
            if not r:
                return None
            chunk = os.read(self.proc.stdout.fileno(), 4096)
            if not chunk:
                return None
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode()

    def raw(self, text, timeout=5.0):
        self.proc.stdin.write(text.encode() + b"\n")
        self.proc.stdin.flush()
        ans = self.line(timeout)
        return json.loads(ans) if ans is not None else None

    def req(self, timeout=5.0, **kw):
        return self.raw(json.dumps(kw), timeout)

    def tpdu(self, hexstr, timeout=5.0):
        return self.req(timeout, op="tpdu", data=hexstr)

    def finish(self):
        """close stdin; rsim-card must power down and exit 0"""
        self.proc.stdin.close()
        try:
            rc = self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            rc = None
        time.sleep(0.1)
        return rc

    def stderr(self):
        self.errlog.flush()
        self.errlog.seek(0)
        return self.errlog.read()

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.card.close()
        os.close(self.master)
        os.close(self.slave)
        self.errlog.close()
        self.tmp.cleanup()


ATR = "3B9F96801FC78031A073BE21136743200718000001A5"


def ok_data(ans, hexstr):
    return ans is not None and ans.get("ok") is True and ans.get("data") == hexstr


def scenario_main():
    """echo on, direct convention, RTS reset, polarity found by auto"""
    rig = Rig()
    try:
        st = rig.req(op="status")
        check(st == {"ok": True, "present": False, "powered": False, "backend": "phoenix",
                     "reader": os.ttyname(rig.slave), "atr": None}, "status before power_up: %r" % st)
        a = rig.tpdu("A0A40000023F00")
        check(a == {"ok": False, "error": "not_powered"}, "tpdu unpowered: %r" % a)

        a = rig.req(op="power_up")
        check(a == {"ok": True, "atr": ATR}, "power_up: %r" % a)

        # case 3 with two NULLs before the ACK and one before SW
        a = rig.tpdu("A0A40000023F00")
        check(ok_data(a, "9F17"), "SELECT MF: %r" % a)
        check(rig.card.received[-1] == b"\x3f\x00", "card got the file id: %r" % rig.card.received)
        # case 2 via ACK
        a = rig.tpdu("A0C0000017")
        check(ok_data(a, FCP_MF.hex().upper() + "9000"), "GET RESPONSE: %r" % a)
        # case 2 with ~INS single steps, then ACK for the rest
        a = rig.tpdu("A0B000000A")
        check(ok_data(a, EF_DATA[:10].hex().upper() + "9000"), "READ BINARY 10: %r" % a)
        # P3=00: 256 bytes
        a = rig.tpdu("A0B0000000")
        check(ok_data(a, EF_DATA.hex().upper() + "9000"), "READ BINARY 256: %s" % (a and a.get("data", a)[:40]))
        # case 3 with ~INS then ACK; lowercase hex in, uppercase out
        a = rig.tpdu("a0d6000003aabbcc")
        check(ok_data(a, "9000"), "UPDATE BINARY: %r" % a)
        a = rig.tpdu("A0B0000003")
        check(ok_data(a, "AABBCC9000"), "read back after update: %r" % a)
        # case 1 as 4 bytes
        a = rig.tpdu("A0040000")
        check(ok_data(a, "9000"), "case 1: %r" % a)
        check(rig.card.tpdus[-1] == bytes.fromhex("A004000000"), "case 1 went out with P3=00")
        # 61xx / 6Cxx pass through
        a = rig.tpdu("00A40004023F00")
        check(ok_data(a, "611F"), "61xx: %r" % a)
        a = rig.tpdu("00B0000000")
        check(ok_data(a, "6C0A"), "6Cxx: %r" % a)
        n_hdr = len(rig.card.tpdus)

        # malformed requests: none reaches the card
        for bad in ('{"op":"tpdu","data":"A0A4G0000"}', '{"op":"tpdu","data":"A0A400000"}',
                    '{"op":"tpdu","data":"A0A4"}', '{"op":"tpdu"}', '{"op":"tpdu","data":5}',
                    'op=status', '{"op":"launch"}', '{"data":"A0A4000000"}',
                    '{"op":"tpdu","data":"A0A40000033F00"}'):
            a = rig.raw(bad)
            check(a is not None and a.get("ok") is False and a.get("error") == "bad_request",
                  "bad request %s: %r" % (bad, a))
        check(len(rig.card.tpdus) == n_hdr, "bad requests reached the card")
        a = rig.raw("   {  \"op\" :  \"tpdu\" ,  \"data\" : \"A0A40000023F00\" }  ")
        check(ok_data(a, "9F17"), "spaces in request: %r" % a)
        a = rig.raw('{"op":"tpdu","data":"' + "00" * 3000 + '"}')
        check(a is not None and a.get("error") == "bad_request", "overlong line: %r" % a)
        a = rig.req(op="status")
        check(a is not None and a.get("powered") is True, "still serving after overlong line: %r" % a)

        # a card that stays silent: timeout after the work waiting time
        t0 = time.monotonic()
        a = rig.tpdu("A0FA000000")
        dt = time.monotonic() - t0
        check(a is not None and a.get("error") == "timeout", "silent card: %r" % a)
        check(0.9 < dt < 3.0, "timeout took %.2f s, expected ~1 s WWT" % dt)
        # and the next exchange is not confused by it
        a = rig.tpdu("A0A40000023F00")
        check(ok_data(a, "9F17"), "after timeout: %r" % a)
        # a card that answers after the WWT: the late SW must not be taken
        # for the next command's echo or procedure byte
        a = rig.tpdu("A0FB000000")
        check(a is not None and a.get("error") == "timeout", "late card: %r" % a)
        time.sleep(0.6)
        a = rig.tpdu("A0A40000023F00")
        check(ok_data(a, "9F17"), "after a late answer: %r" % a)
        # an invalid procedure byte
        a = rig.tpdu("A0EE000000")
        check(a is not None and a.get("error") == "protocol", "bad procedure byte: %r" % a)

        # warm reset: a fresh ATR from a second reset pulse
        a = rig.req(op="reset")
        check(a == {"ok": True, "atr": ATR}, "reset: %r" % a)
        check(rig.card.resets == 2, "card saw %d resets" % rig.card.resets)
        a = rig.tpdu("A0A40000023F00")
        check(ok_data(a, "9F17"), "after reset: %r" % a)

        st = rig.req(op="status")
        check(st == {"ok": True, "present": True, "powered": True, "backend": "phoenix",
                     "reader": os.ttyname(rig.slave), "atr": ATR}, "status powered: %r" % st)
        a = rig.req(op="power_down")
        check(a == {"ok": True}, "power_down: %r" % a)
        time.sleep(0.1)
        check(rig.card.in_reset, "card held in reset after power_down")
        a = rig.tpdu("A0A40000023F00")
        check(a == {"ok": False, "error": "not_powered"}, "tpdu after power_down: %r" % a)
        st = rig.req(op="status")
        check(st.get("powered") is False and st.get("atr") is None and st.get("present") is True,
              "status after power_down: %r" % st)

        a = rig.req(op="power_up")
        check(a == {"ok": True, "atr": ATR}, "power_up again: %r" % a)
        check(not rig.card.in_reset, "card out of reset")
        rc = rig.finish()
        check(rc == 0, "exit on EOF: rc=%r" % rc)
        check(rig.card.in_reset, "card powered down (held in reset) on EOF")
        check("I/O line echoes" in rig.stderr(), "echo detected")
    finally:
        rig.close()


def scenario_inverted_rts():
    """a smartmouse-wired reader: reset on inverted RTS, found by auto"""
    rig = Rig(["--atr-timeout-ms", "300"], reset_active_high=False)
    try:
        a = rig.req(op="power_up")
        check(a == {"ok": True, "atr": ATR}, "auto polarity power_up: %r" % a)
        check("inverted RTS" in rig.stderr(), "auto fell back to inverted RTS")
        a = rig.tpdu("A0A40000023F00")
        check(ok_data(a, "9F17"), "SELECT with inverted RTS: %r" % a)
        # the polarity is kept: a reset does not try the wrong one again
        t0 = time.monotonic()
        a = rig.req(op="reset")
        check(a == {"ok": True, "atr": ATR} and time.monotonic() - t0 < 0.3,
              "reset with the learnt polarity: %r" % a)
        check(rig.finish() == 0, "exit")
    finally:
        rig.close()

    # a fixed, wrong polarity does not fall back
    rig = Rig(["--reset", "rts", "--atr-timeout-ms", "300"], reset_active_high=False)
    try:
        a = rig.req(op="power_up")
        check(a is not None and a.get("error") == "no_card", "fixed wrong polarity: %r" % a)
        st = rig.req(op="status")
        check(st.get("present") is False and st.get("powered") is False, "status no card: %r" % st)
    finally:
        rig.close()

    # DTR wiring
    rig = Rig(["--reset", "dtr"], reset_line="DTR")
    try:
        a = rig.req(op="power_up")
        check(a == {"ok": True, "atr": ATR}, "DTR reset: %r" % a)
    finally:
        rig.close()


def scenario_no_echo():
    rig = Rig(echo=False)
    try:
        a = rig.req(op="power_up")
        check(a == {"ok": True, "atr": ATR}, "power_up no echo: %r" % a)
        a = rig.tpdu("A0A40000023F00")
        check(ok_data(a, "9F17"), "SELECT without echo: %r" % a)
        a = rig.tpdu("A0B000000A")
        check(ok_data(a, EF_DATA[:10].hex().upper() + "9000"), "READ BINARY without echo: %r" % a)
        a = rig.tpdu("A0D6000002A1B2")
        check(ok_data(a, "9000"), "UPDATE without echo: %r" % a)
        check("no echo" in rig.stderr(), "no echo detected")
    finally:
        rig.close()


def scenario_bad_echo():
    """the echo is how a broken line shows: a mismatch is an I/O error"""
    rig = Rig(echo_corrupt=True)
    try:
        a = rig.req(op="power_up")
        check(a == {"ok": True, "atr": ATR}, "power_up: %r" % a)
        a = rig.tpdu("A0A40000023F00")
        check(a is not None and a.get("error") == "io" and "echo mismatch" in a.get("detail", ""),
              "corrupted echo: %r" % a)
    finally:
        rig.close()


def scenario_inverse():
    atr = "3F6525002C09699000"
    rig = Rig(atr=atr, conv_inverse=True)
    try:
        a = rig.req(op="power_up")
        check(a == {"ok": True, "atr": atr}, "inverse ATR: %r" % a)
        a = rig.tpdu("A0A40000023F00")
        check(ok_data(a, "9F17"), "SELECT inverse: %r" % a)
        a = rig.tpdu("A0B000000A")
        check(ok_data(a, EF_DATA[:10].hex().upper() + "9000"), "READ BINARY inverse: %r" % a)
        check("inverse convention" in rig.stderr(), "inverse logged")
    finally:
        rig.close()


def scenario_no_card():
    rig = Rig(["--atr-timeout-ms", "200"], answers_reset=False)
    try:
        t0 = time.monotonic()
        a = rig.req(op="power_up")
        check(a is not None and a.get("error") == "no_card", "no card: %r" % a)
        check(time.monotonic() - t0 < 1.5, "auto tried both polarities within budget")
        a = rig.req(op="reset")
        check(a is not None and a.get("error") == "no_card", "reset with no card: %r" % a)
        check(rig.finish() == 0, "exit without a card")
    finally:
        rig.close()


def main():
    for s in (scenario_main, scenario_inverted_rts, scenario_no_echo,
              scenario_bad_echo, scenario_inverse, scenario_no_card):
        before = failures
        s()
        print("%s: %s" % (s.__name__, "ok" if failures == before else "FAILED"))
    print("test_e2e: %d checks, %d failed" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
