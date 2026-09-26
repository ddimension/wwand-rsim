#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 André Valentin <avalentin@marcant.net>
"""End-to-end: rsim-card's bt: backend against a simulated phone — a SIM
Access Profile server on a unix socket (RSIM_TEST_SAP_SOCK stands in for the
RFCOMM link; SDP is not involved).

usage: test_e2e_sap.py <path to rsim-card>
"""

import json
import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "./rsim-card"
ADDR = "00:11:22:33:44:55"
ATR = bytes.fromhex("3B9F96801FC78031E073FE211B63F100")
checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAILED: %s" % what, file=sys.stderr)


def msg(mid, *params):
    out = struct.pack(">BBH", mid, len(params), 0)
    for pid, val in params:
        out += struct.pack(">BBH", pid, 0, len(val)) + val + b"\0" * (-len(val) % 4)
    return out


def u8(v):
    return bytes([v])


class Phone(threading.Thread):
    """A SAP server: the phone side. `script` tweaks it per test."""

    def __init__(self, path, **script):
        super().__init__(daemon=True)
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(1)
        self.s = script
        self.got = []           # (msg id, {param id: value})
        self.sim_on = True
        self.conn = None
        self.closed = threading.Event()

    def send(self, data):
        self.conn.sendall(data)

    def read_exact(self, n):
        b = b""
        while len(b) < n:
            c = self.conn.recv(n - len(b))
            if not c:
                raise EOFError
            b += c
        return b

    def read_msg(self):
        mid, n, _ = struct.unpack(">BBH", self.read_exact(4))
        params = {}
        for _ in range(n):
            pid, _, l = struct.unpack(">BBH", self.read_exact(4))
            params[pid] = self.read_exact(l + (-l % 4))[:l]
        return mid, params

    def run(self):
        self.conn, _ = self.srv.accept()
        try:
            self.serve()
        except (EOFError, OSError):
            pass
        self.closed.set()

    def serve(self):
        s = self.s
        while True:
            mid, p = self.read_msg()
            self.got.append((mid, p))
            if mid == 0x00:                                   # CONNECT_REQ
                size = struct.unpack(">H", p[0x00])[0]
                if s.get("refuse"):
                    self.send(msg(0x01, (0x01, u8(0x01))))
                    continue
                if s.get("max") and size != s["max"]:
                    self.send(msg(0x01, (0x01, u8(0x02)), (0x00, struct.pack(">H", s["max"]))))
                    continue
                self.send(msg(0x01, (0x01, u8(0x00))))
                if not s.get("no_status"):
                    self.send(msg(0x11, (0x08, u8(0x01))))   # STATUS_IND card reset
            elif mid == 0x07:                                 # TRANSFER_ATR_REQ
                if not self.sim_on:
                    self.send(msg(0x08, (0x02, u8(0x03))))   # already off
                else:
                    self.send(msg(0x08, (0x02, u8(0x00)), (0x06, ATR)))
            elif mid in (0x05,):                              # TRANSFER_APDU_REQ
                apdu = p.get(0x04, p.get(0x10, b""))
                if s.get("silent_apdu"):
                    continue                                  # answers nothing
                if s.get("removed_on_apdu"):
                    self.send(msg(0x06, (0x02, u8(0x04))))
                    continue
                if apdu[:2] == bytes.fromhex("A0A4"):
                    resp = bytes.fromhex("9F17")
                elif apdu[:2] == bytes.fromhex("00B0"):
                    resp = bytes(range(apdu[4])) + bytes.fromhex("9000")
                else:
                    resp = bytes.fromhex("6D00")
                if s.get("status_before_answer"):
                    self.send(msg(0x11, (0x08, u8(0x05))))   # recovered, unasked
                self.send(msg(0x06, (0x02, u8(0x00)), (0x05, resp)))
            elif mid == 0x09:                                 # POWER_SIM_OFF_REQ
                self.send(msg(0x0A, (0x02, u8(0x00 if self.sim_on else 0x03))))
                self.sim_on = False
            elif mid == 0x0B:                                 # POWER_SIM_ON_REQ
                self.send(msg(0x0C, (0x02, u8(0x00 if not self.sim_on else 0x05))))
                self.sim_on = True
            elif mid == 0x0D:                                 # RESET_SIM_REQ
                self.send(msg(0x0E, (0x02, u8(0x00))))
            elif mid == 0x02:                                 # DISCONNECT_REQ
                self.send(msg(0x03))
                self.conn.close()
                return
            else:
                self.send(msg(0x12))                          # ERROR_RESP

    def ids(self):
        return [m for m, _ in self.got]


class Helper:
    def __init__(self, sock, *args):
        self.p = subprocess.Popen([BIN, "-v", *args, "bt:" + ADDR], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                  env=dict(os.environ, RSIM_TEST_SAP_SOCK=sock,
                                           RSIM_TEST_SAP_TIMEOUT_MS="1500"))
        self.events = []

    def line(self, timeout=10):
        r, _, _ = select.select([self.p.stdout], [], [], timeout)
        if not r:
            return None
        l = self.p.stdout.readline()
        return json.loads(l) if l.strip() else None

    def ask(self, req, timeout=10):
        self.p.stdin.write(json.dumps(req) + "\n")
        self.p.stdin.flush()
        while True:
            o = self.line(timeout)
            if o is None or "event" not in o:
                return o
            self.events.append(o["event"])

    def event(self, timeout=5):
        if self.events:
            return self.events.pop(0)
        o = self.line(timeout)
        return o.get("event") if o else None

    def end(self, timeout=10):
        try:
            self.p.stdin.close()
        except BrokenPipeError:
            pass
        try:
            rc = self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            rc = None
        self.err = self.p.stderr.read()
        return rc


def run(tmp, name, **script):
    path = os.path.join(tmp, name)
    ph = Phone(path, **script)
    ph.start()
    return ph, path


with tempfile.TemporaryDirectory() as tmp:
    # --- a whole session, message size agreed on the second try --------------
    ph, path = run(tmp, "a", max=512)
    h = Helper(path)
    r = h.ask({"op": "power_up"})
    check(r and r.get("ok") and r.get("atr") == ATR.hex().upper(), "power_up: the phone's ATR (%r)" % r)
    check(ph.ids()[:2] == [0x00, 0x00] and struct.unpack(">H", ph.got[1][1][0x00])[0] == 512,
          "CONNECT_REQ again with the phone's size")
    check(0x0B not in ph.ids(), "a SIM the phone reports reset is on: no POWER_SIM_ON")
    r = h.ask({"op": "tpdu", "data": "A0A40000023F00"})
    check(r and r.get("data") == "9F17", "tpdu: the answer as it is (%r)" % r)
    check(ph.got[-1][0] == 0x05 and ph.got[-1][1].get(0x04) == bytes.fromhex("A0A40000023F00"),
          "as CommandAPDU")
    r = h.ask({"op": "tpdu", "data": "00B0000010"})
    check(r and r.get("data") == bytes(range(16)).hex().upper() + "9000", "tpdu: 16 bytes read (%r)" % r)
    r = h.ask({"op": "status"})
    check(r and r.get("present") and r.get("powered") and r.get("backend") == "bt" and r.get("reader") == ADDR,
          "status (%r)" % r)
    r = h.ask({"op": "power_down"})
    check(r and r.get("ok") and ph.got[-1][0] == 0x09, "power_down: POWER_SIM_OFF_REQ")
    r = h.ask({"op": "power_up"})
    check(r and r.get("atr") and ph.ids()[-2:] == [0x0B, 0x07], "power_up again: POWER_SIM_ON, then the ATR")
    r = h.ask({"op": "reset"})
    check(r and r.get("atr") and ph.ids()[-2:] == [0x0D, 0x07], "reset: RESET_SIM, then the ATR")
    rc = h.end()
    ph.closed.wait(5)
    check(rc == 0 and ph.ids()[-2:] == [0x09, 0x02], "end of stdin: SIM off, DISCONNECT_REQ, exit 0 (%r %r)" % (rc, ph.ids()))

    # --- CommandAPDU7816 ------------------------------------------------------
    ph, path = run(tmp, "b")
    h = Helper(path, "--bt-apdu", "7816")
    h.ask({"op": "power_up"})
    h.ask({"op": "tpdu", "data": "00A40004023F00"})
    check(ph.got[-1][1].get(0x10) == bytes.fromhex("00A40004023F00"), "--bt-apdu 7816: CommandAPDU7816")
    h.end()

    # --- the phone resets its SIM behind our back: removed + inserted ---------
    ph, path = run(tmp, "c")
    h = Helper(path)
    h.ask({"op": "power_up"})
    ph.send(msg(0x11, (0x08, u8(0x01))))
    check(h.event() == "removed" and h.event() == "inserted", "a reset by the phone: removed, then inserted")
    r = h.ask({"op": "power_up"})
    check(r and r.get("atr"), "power_up after that: the ATR again")
    # a status that arrives in the middle of an exchange is taken in
    ph.s["status_before_answer"] = True
    r = h.ask({"op": "tpdu", "data": "A0A40000023F00"})
    check(r and r.get("data") == "9F17", "an unsolicited STATUS_IND before the answer is passed over")
    check(h.event() == "removed" and h.event() == "inserted", "...and reported as a new card")
    ph.s["status_before_answer"] = False

    # the SIM taken out of the phone
    h.ask({"op": "power_up"})
    ph.send(msg(0x11, (0x08, u8(0x03))))
    check(h.event() == "removed", "card removed at the phone: removed")
    r = h.ask({"op": "power_up"})
    check(r and r.get("error") == "no_card", "power_up without a card: no_card (%r)" % r)
    ph.send(msg(0x11, (0x08, u8(0x04))))
    check(h.event() == "inserted", "card inserted at the phone: inserted")
    r = h.ask({"op": "power_up"})
    check(r and r.get("atr") and 0x0B in ph.ids()[-3:], "power_up after insertion: POWER_SIM_ON, the ATR")
    ph.s["removed_on_apdu"] = True
    r = h.ask({"op": "tpdu", "data": "A0A40000023F00"})
    check(r and r.get("error") == "no_card", "APDU result 'card removed': no_card (%r)" % r)
    h.end()

    # --- the phone ends SIM access -------------------------------------------
    ph, path = run(tmp, "d")
    h = Helper(path)
    h.ask({"op": "power_up"})
    ph.send(msg(0x04, (0x03, u8(0x00))))                    # DISCONNECT_IND graceful
    try:
        rc = h.p.wait(5)
    except subprocess.TimeoutExpired:
        rc = None
    ph.closed.wait(5)
    check(rc == 1 and ph.ids()[-1] == 0x02, "DISCONNECT_IND: DISCONNECT_REQ, the helper ends with 1 (%r)" % rc)
    h.end()

    # --- the link drops -------------------------------------------------------
    ph, path = run(tmp, "e")
    h = Helper(path)
    h.ask({"op": "power_up"})
    ph.conn.shutdown(socket.SHUT_RDWR)
    try:
        rc = h.p.wait(5)
    except subprocess.TimeoutExpired:
        rc = None
    check(rc == 1, "a dropped link ends the helper with 1 (%r)" % rc)
    h.end()

    # --- an answer that does not come: the session ends ------------------------
    # (a late one would otherwise be taken for the next request's answer)
    ph, path = run(tmp, "h")
    h = Helper(path)
    h.ask({"op": "power_up"})
    ph.s["silent_apdu"] = True
    r = h.ask({"op": "tpdu", "data": "A0A40000023F00"})
    check(r and r.get("error") == "timeout", "no APDU answer: timeout (%r)" % r)
    try:
        rc = h.p.wait(5)
    except subprocess.TimeoutExpired:
        rc = None
    check(rc == 1, "...and the helper ends with 1 instead of going on out of step (%r)" % rc)
    h.end()

    # --- the phone refuses ----------------------------------------------------
    ph, path = run(tmp, "f", refuse=True)
    h = Helper(path)
    rc = h.end()
    check(rc == 1 and "refuses SIM access" in h.err, "refused: exit 1 before any answer (%r)" % h.err[-200:])

    # --- no STATUS_IND (a call going on): no card until it comes --------------
    ph, path = run(tmp, "g", no_status=True)
    t0 = time.time()
    h = Helper(path)
    r = h.ask({"op": "power_up"}, timeout=15)
    check(r and r.get("error") == "no_card", "no STATUS_IND yet: no_card (%r)" % r)
    ph.send(msg(0x11, (0x08, u8(0x01))))
    check(h.event() == "inserted", "the STATUS_IND later: inserted")
    h.end()

    # --- a spec that is not an address ----------------------------------------
    p = subprocess.run([BIN, "bt:nonsense"], input="", capture_output=True, text=True, timeout=10)
    check(p.returncode == 1 and "not a Bluetooth address" in p.stderr, "bt:nonsense: refused")

print("test_e2e_sap: %d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
