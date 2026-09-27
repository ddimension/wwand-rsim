#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 André Valentin <avalentin@marcant.net>
"""End-to-end: rsim-card's rspro: backend against a simulated osmo-remsim —
a remsim-server (RSPRO over IPA, and its REST API) and a remsim-bankd with
one card. The BER here is written separately from src/rspro.c, from the same
reading of asn1/RSPRO.asn; no real osmo-remsim was at hand (2026-09-27), so
this proves the helper's flow, not interoperability.

usage: test_e2e_rspro.py <path to rsim-card>
"""

import json
import os
import select
import socket
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BIN = sys.argv[1] if len(sys.argv) > 1 else "./rsim-card"
ATR = bytes.fromhex("3B9F96801FC78031E073FE211B63F100")
ATR2 = bytes.fromhex("3B9F96801FC78031E073FE211B63F1FF")
checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAILED: %s" % what, file=sys.stderr)


# ---- BER --------------------------------------------------------------------

def tlv(tag, content):
    n = len(content)
    if n < 0x80:
        l = bytes([n])
    elif n < 0x100:
        l = bytes([0x81, n])
    else:
        l = bytes([0x82, n >> 8, n & 0xFF])
    return bytes([tag]) + l + content


def integer(tag, v):
    b = v.to_bytes(max(1, (v.bit_length() + 8) // 8), "big")
    return tlv(tag, b)


def boolean(tag, v):
    return tlv(tag, b"\xff" if v else b"\x00")


def parse(b):
    out = []
    i = 0
    while i < len(b):
        tag, l = b[i], b[i + 1]
        i += 2
        if l & 0x80:
            k = l & 0x7F
            l = int.from_bytes(b[i:i + k], "big")
            i += k
        out.append((tag, b[i:i + l]))
        i += l
    return out


def slot(a, b):
    return tlv(0x30, integer(0x02, a) + integer(0x02, b))


def slot_of(c):
    f = parse(c)
    return (int.from_bytes(f[0][1], "big"), int.from_bytes(f[1][1], "big"))


def pdu(msg, content, tag=1):
    return tlv(0x30, integer(0x80, 2) + integer(0x81, tag) + tlv(0xA2, tlv(0xA0 | msg, content)))


def unpdu(b):
    f = dict(parse(parse(b)[0][1]))
    alt = parse(f[0xA2])[0]
    return alt[0] & 0x1F, parse(alt[1]), int.from_bytes(f[0x81], "big"), int.from_bytes(f[0x80], "big")


def ipa(payload):
    return struct.pack(">HBB", len(payload) + 1, 0xEE, 0x07) + payload


def identity(kind, name):
    return tlv(0x30, integer(0x0A, kind) + tlv(0x16, name) + tlv(0x80, b"sim-remsim") + tlv(0x81, b"0.3"))


class Peer(threading.Thread):
    """One RSPRO endpoint over IPA: accepts one connection at a time."""

    def __init__(self, name):
        super().__init__(daemon=True)
        self.name = name
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(4)
        self.port = self.srv.getsockname()[1]
        self.conn = None
        self.got = []           # (msg, fields)
        self.pongs = 0
        self.ccm = []           # every CCM message it got
        self.connected = threading.Event()
        self.lock = threading.Lock()

    def send(self, msg, content, tag=1):
        with self.lock:
            if self.conn:
                self.conn.sendall(ipa(pdu(msg, content, tag)))

    def run(self):
        while True:
            c, _ = self.srv.accept()
            self.conn = c
            self.connected.set()
            buf = b""
            try:
                while True:
                    d = c.recv(4096)
                    if not d:
                        break
                    buf += d
                    while len(buf) >= 3:
                        l = struct.unpack(">H", buf[:2])[0]
                        if len(buf) < 3 + l:
                            break
                        proto, body = buf[2], buf[3:3 + l]
                        buf = buf[3 + l:]
                        if proto == 0xFE:
                            if body == b"\x01":
                                self.pongs += 1
                            self.ccm.append(body)
                            continue
                        if proto != 0xEE or body[0] != 0x07:
                            continue
                        msg, fields, tag, version = unpdu(body[1:])
                        self.version = version
                        self.got.append((msg, fields))
                        self.on(msg, fields, tag)
            except OSError:
                pass
            self.conn = None
            self.connected.clear()

    def msgs(self):
        return [m for m, _ in self.got]


class Bankd(Peer):
    def __init__(self, **script):
        super().__init__("bankd")
        self.s = script
        self.statuses = []      # (rst, vcc)

    def on(self, msg, f, tag):
        if msg == 2:            # connectClientReq
            self.client = slot_of(f[1][1])
            self.send(3, identity(2, b"bankd") + integer(0x0A, 0), tag)
            if self.s.get("atr", True):
                self.send(13, slot(*self.client) + tlv(0x04, ATR))
        elif msg == 15:         # tpduModemToCard
            data = f[3][1]
            if self.s.get("silent"):
                return
            flags = tlv(0x30, boolean(0x01, True) + boolean(0x01, True) + boolean(0x01, False) + boolean(0x01, False))
            resp = {"A0A40000023F00": "9F17", "00B0000010": bytes(range(16)).hex() + "9000"}.get(data.hex().upper(), "6D00")
            self.send(16, slot(1, 1) + slot(*self.client) + flags + tlv(0x04, bytes.fromhex(resp)), tag)
        elif msg == 17:         # clientSlotStatusInd
            st = dict(parse(f[2][1]))
            rst, vcc = st[0x80] != b"\x00", st[0x81] != b"\x00"
            self.statuses.append((rst, vcc))
            if self.s.get("atr_on_reset") and len(self.statuses) >= 2 and self.statuses[-2][0] and not rst and vcc:
                self.send(13, slot(*self.client) + tlv(0x04, ATR2))


class Server(Peer):
    def __init__(self, bankd, banks=((1, 2),), maps=None, result=0):
        super().__init__("server")
        self.bankd = bankd
        self.banks = banks
        self.maps = dict(maps or {})    # (bank, slot) -> (client, slot)
        self.result = result
        self.client = None
        self.deleted = []
        self.posted = []
        self.rest = ThreadingHTTPServer(("127.0.0.1", 0), self.handler())
        self.rest_port = self.rest.server_address[1]
        threading.Thread(target=self.rest.serve_forever, daemon=True).start()

    def config_bank(self, bank=None):
        if bank:
            ip, port = socket.inet_aton("127.0.0.1"), self.bankd.port
        else:
            bank, ip, port = (0, 0), b"\0\0\0\0", 0
        self.send(10, slot(*bank) + tlv(0x30, tlv(0x80, ip) + integer(0x02, port)))

    def map(self, bank, client):
        self.maps[bank] = client
        if client == self.client:
            self.config_bank(bank)

    def unmap(self, bank):
        client = self.maps.pop(bank)
        if client == self.client:
            self.config_bank(None)

    def on(self, msg, f, tag):
        if msg == 2:
            self.client = slot_of(f[1][1])
            self.conn.sendall(struct.pack(">HBB", 1, 0xFE, 0x00))  # a PING
            self.conn.sendall(struct.pack(">HBB", 1, 0xFE, 0x06))  # an ID_ACK: not to be answered
            self.conn.sendall(struct.pack(">HBBB", 2, 0xFE, 0x04, 0x01))  # ID_GET, unit name
            self.send(3, identity(1, b"remsim-server") + integer(0x0A, self.result), tag)
            for b, c in self.maps.items():
                if c == self.client:
                    self.config_bank(b)

    def handler(self):
        srv = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def reply(self, code, obj=None):
                body = json.dumps(obj).encode() if obj is not None else b""
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path == "/api/backend/v1/slotmaps":
                    self.reply(200, {"slotmaps": [
                        {"bank": {"bankId": b[0], "slotNr": b[1]}, "client": {"clientId": c[0], "slotNr": c[1]},
                         "state": "ACTIVE"} for b, c in srv.maps.items()]})
                elif self.path == "/api/backend/v1/banks":
                    self.reply(200, {"banks": [
                        {"peer": "bank%d" % b, "state": "CONNECTED_BANKD",
                         "component_id": {"type_": "remsimBankd", "name": "bank-%d" % b, "software": "remsim-bankd"},
                         "bankId": b, "numberOfSlots": n} for b, n in srv.banks]})
                else:
                    self.reply(404)

            def do_POST(self):
                o = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                b = (o["bank"]["bankId"], o["bank"]["slotNr"])
                c = (o["client"]["clientId"], o["client"]["slotNr"])
                srv.posted.append((b, c))
                if b in srv.maps:
                    return self.reply(409)
                srv.map(b, c)
                self.reply(201)

            def do_DELETE(self):
                i = int(self.path.rsplit("/", 1)[1])
                b = (i >> 16, i & 0xFFFF)
                srv.deleted.append(b)
                if b not in srv.maps:
                    return self.reply(404)
                srv.unmap(b)
                self.reply(200)

        return H


class Helper:
    def __init__(self, spec, *args):
        self.p = subprocess.Popen([BIN, "-v", *args, spec], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.events = []
        self.buf = b""
        self.info = {}

    def line(self, timeout=15):
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            left = deadline - time.time()
            if left <= 0:
                return None
            r, _, _ = select.select([self.p.stdout], [], [], left)
            if not r:
                return None
            chunk = os.read(self.p.stdout.fileno(), 4096)
            if not chunk:
                return None
            self.buf += chunk
        l, self.buf = self.buf.split(b"\n", 1)
        return json.loads(l) if l.strip() else None

    def ask(self, req, timeout=15):
        self.p.stdin.write(json.dumps(req) + "\n")
        self.p.stdin.flush()
        while True:
            o = self.line(timeout)
            if o is None or "event" not in o:
                return o
            if o["event"] == "info":
                self.info = o
                continue
            self.events.append(o["event"])

    def event(self, timeout=5):
        if self.events:
            return self.events.pop(0)
        o = self.line(timeout)
        while o and o.get("event") == "info":
            self.info = o
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


def world(**kw):
    bankd = Bankd(**kw.pop("bankd", {}))
    bankd.start()
    server = Server(bankd, **kw)
    server.start()
    return server, bankd


def wait_for(cond, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if cond():
            return True
        time.sleep(0.05)
    return False


# --- enumeration over the REST API ----------------------------------------------
server, bankd = world(banks=((1, 2), (4, 1)), maps={(1, 1): (7, 0)})
srv = "127.0.0.1:%d" % server.port
p = subprocess.run([BIN, "--list", "--rspro-server", srv, "--rspro-rest-port", str(server.rest_port)],
                   capture_output=True, text=True, timeout=20)
rows = [json.loads(l) for l in p.stdout.splitlines() if l.strip()]
check(p.returncode == 0 and len(rows) == 4, "--list: a row per bank slot, then done (%r)" % rows)
check(rows[0] == {"backend": "rspro", "spec": "rspro:%s/1:0" % srv, "server": srv, "bank": 1, "slot": 0,
                  "name": "bank-1", "bank_state": "CONNECTED_BANKD", "peer": "bank1"},
      "--list: a free slot and how to use it (%r)" % rows[0])
check(rows[1].get("mapped_to") == "7:0" and rows[1].get("map_state") == "ACTIVE", "--list: a mapped slot says to whom")
check(rows[2].get("spec") == "rspro:%s/4:0" % srv, "--list: the second bank")
check(rows[3].get("done") and rows[3].get("backends") == "rspro" and rows[3].get("slots") == 3
      and "error" not in rows[3], "--list: done (%r)" % rows[3])
p = subprocess.run([BIN, "--list", "--rspro-server", "127.0.0.1:1", "--rspro-rest-port", "1"],
                   capture_output=True, text=True, timeout=20)
last = json.loads(p.stdout.splitlines()[-1])
check(p.returncode == 1 and last.get("done") and "refused" in last.get("error", ""),
      "--list, no server: done with the error (%r)" % last)

# --- a bank slot named in the spec: mapped for the session, then unmapped -------
server, bankd = world()
spec = "rspro:127.0.0.1:%d/1:1" % server.port
h = Helper(spec, "--rspro-client", "5:2", "--rspro-rest-port", str(server.rest_port))
r = h.ask({"op": "power_up"})
check(r and r.get("ok") and r.get("atr") == ATR.hex().upper(), "power_up: the bank's ATR (%r)" % r)
check(server.posted == [((1, 1), (5, 2))], "the mapping made over REST (%r)" % server.posted)
check(server.client == (5, 2) and server.got[0][0] == 2, "ConnectClientReq to the server as client 5:2")
check(server.pongs == 1, "the server's IPA PING answered")
check(any(c[:1] == b"\x05" and b"rsim-card\0" in c for c in server.ccm), "ID_GET answered with ID_RESP, our unit name (%r)" % server.ccm)
check(not any(c[:1] == b"\x06" for c in server.ccm), "ID_ACK not answered (a client does not; two servers would loop)")
check(getattr(server, "version", None) == 2, "RsproPDU version 2")
check(bankd.got[0][0] == 2 and bankd.client == (5, 2), "ConnectClientReq to the bankd")
check(14 in bankd.msgs(), "SetAtrReq answered")
check(bankd.statuses == [], "first power_up: the ATR the bankd just sent, no reset pulse on top (%r)" % bankd.statuses)
inf = h.info
check(inf.get("backend") == "rspro" and inf.get("reader") == spec and inf.get("client") == "5:2"
      and inf.get("server_name") == "remsim-server" and inf.get("mapping") == "helper",
      "info event (%r)" % inf)
r = h.ask({"op": "tpdu", "data": "A0A40000023F00"})
check(r and r.get("data") == "9F17", "tpdu (%r)" % r)
m, f = bankd.got[-1]
check(m == 15 and slot_of(f[0][1]) == (5, 2) and slot_of(f[1][1]) == (1, 1)
      and f[3][1] == bytes.fromhex("A0A40000023F00"), "as TpduModemToCard, client and bank slot")
r = h.ask({"op": "tpdu", "data": "00B0000010"})
check(r and r.get("data") == bytes(range(16)).hex().upper() + "9000", "tpdu: data + SW (%r)" % r)
r = h.ask({"op": "status"})
check(r and r.get("present") and r.get("powered") and r.get("bank") == "1:1"
      and r.get("bankd") == "127.0.0.1:%d" % bankd.port, "status (%r)" % r)
r = h.ask({"op": "reset"})
check(r and r.get("atr") == ATR.hex().upper()
      and wait_for(lambda: bankd.statuses == [(True, True), (False, True)]),
      "reset: RST pulse, the ATR from before (%r)" % r)
r = h.ask({"op": "power_down"})
# a status indication has no answer: wait for it to arrive
check(r and r.get("ok") and wait_for(lambda: bankd.statuses[-1] == (True, False)), "power_down: RST, no VCC")
r = h.ask({"op": "power_up"})
check(r and r.get("ok") and wait_for(lambda: bankd.statuses[-2:] == [(True, True), (False, True)]),
      "power_up after power_down: a cold start signalled, RST pulse with VCC (%r)" % bankd.statuses)
rc = h.end()
check(rc == 0 and server.deleted == [(1, 1)] and not server.maps, "the end: unmapped (%r %r)" % (rc, server.deleted))

# --- the slot the spec names is taken away: mapped again ------------------------
server, bankd = world()
h = Helper("rspro:127.0.0.1:%d/1:1" % server.port, "--rspro-rest-port", str(server.rest_port))
h.ask({"op": "power_up"})
h.event(2)                                       # the 0 -> 1 of the first power-up
server.unmap((1, 1))
ev = h.event(8)
check(ev == "removed", "slot taken away: removed (%r)" % ev)
check(h.event(8) == "inserted" and len(server.posted) == 2 and server.maps.get((1, 1)) == (0, 0),
      "...and mapped again by the helper (%r)" % server.posted)
h.end()

# --- power_down, then power_up: a cold start is signalled, even after a fresh ATR --
server, bankd = world(maps={(1, 0): (0, 0)}, bankd={"atr_on_reset": True})
h = Helper("rspro:127.0.0.1:%d" % server.port)
h.ask({"op": "power_up"})
h.ask({"op": "reset"})                           # the bankd answers it with a new ATR
h.ask({"op": "power_down"})
wait_for(lambda: bankd.statuses and bankd.statuses[-1] == (True, False))
n = len(bankd.statuses)
h.ask({"op": "power_up"})
check(wait_for(lambda: bankd.statuses[n:n + 2] == [(True, True), (False, True)]),
      "power_up after power_down: the cold start signalled although an ATR came in the reset (%r)" % bankd.statuses[n:])
h.end()

# --- the mapping is the operator's: comes and goes ----------------------------
server, bankd = world()
h = Helper("rspro:127.0.0.1:%d" % server.port, "--rspro-client", "3")
t0 = time.time()
r = h.ask({"op": "power_up"})
check(r and r.get("error") == "no_card" and "no bank slot mapped to client 3 slot 0" in r.get("detail", ""),
      "no mapping: no_card, and why (%r)" % r)
check(time.time() - t0 < 14, "...within the plugin's answer timeout")
server.map((1, 0), (3, 0))
check(h.event(8) == "inserted", "a mapping made: inserted")
r = h.ask({"op": "power_up"})
check(r and r.get("atr") == ATR.hex().upper(), "then power_up works (%r)" % r)
server.unmap((1, 0))
check(h.event(8) == "removed", "the mapping removed: removed")
check(bankd.connected.wait(0) is False or wait_for(lambda: not bankd.connected.is_set()),
      "...and the bankd connection closed")
check(h.end() == 0 and server.deleted == [], "a mapping not ours stays alone")

# --- a bankd that sends a new ATR on reset ---------------------------------------
server, bankd = world(maps={(1, 0): (0, 0)}, bankd={"atr_on_reset": True})
h = Helper("rspro:127.0.0.1:%d" % server.port)
h.ask({"op": "power_up"})
r = h.ask({"op": "reset"})
check(r and r.get("atr") == ATR2.hex().upper(), "reset: the ATR the bankd sent after it (%r)" % r)
h.end()

# --- refusals -------------------------------------------------------------------
server, bankd = world(result=6)
h = Helper("rspro:127.0.0.1:%d" % server.port)
rc = h.end()
check(rc == 1 and "identityInUse" in h.err, "the server refuses the client: exit 1, and why (%r)" % h.err[-200:])

server, bankd = world(maps={(1, 1): (9, 0)})
h = Helper("rspro:127.0.0.1:%d/1:1" % server.port, "--rspro-rest-port", str(server.rest_port))
rc = h.end()
check(rc == 1 and "in use by client 9 slot 0" in h.err and server.maps == {(1, 1): (9, 0)}
      and not server.deleted, "a slot mapped to another client: refused, left alone (%r)" % h.err[-200:])

# a slot already mapped to us (a run before that was killed): ours, removed at the end
server, bankd = world(maps={(1, 1): (0, 0)})
h = Helper("rspro:127.0.0.1:%d/1:1" % server.port, "--rspro-rest-port", str(server.rest_port))
r = h.ask({"op": "power_up"})
check(r and r.get("ok") and h.info.get("mapping") == "helper", "already mapped to us: taken as ours (%r)" % r)
check(h.end() == 0 and server.deleted == [(1, 1)], "...and removed at the end, not left holding the slot")

# a mapping found to our client while a live session holds that identity: its, not deleted
server, bankd = world(maps={(1, 1): (0, 0)}, result=6)
h = Helper("rspro:127.0.0.1:%d/1:1" % server.port, "--rspro-rest-port", str(server.rest_port))
rc = h.end()
check(rc == 1 and not server.deleted and server.maps == {(1, 1): (0, 0)},
      "identity in use: the live session's mapping is left alone (%r)" % server.deleted)

# a bankd that cannot be reached: tried again later, the helper answers meanwhile
server, bankd = world(maps={(1, 0): (0, 0)})
free = socket.socket()                           # a port nobody listens on
free.bind(("127.0.0.1", 0))
bankd.port = free.getsockname()[1]
free.close()
h = Helper("rspro:127.0.0.1:%d" % server.port)
t0 = time.time()
r = h.ask({"op": "status"})
check(r and r.get("ok") and not r.get("present") and time.time() - t0 < 2, "unreachable bankd: status answered at once (%r)" % r)
r = h.ask({"op": "power_up"})
check(r and r.get("error") == "io" and "bankd" in r.get("detail", ""), "...power_up: io, the bankd named (%r)" % r)
h.end()

check(subprocess.run([BIN, "rspro:host/1"], capture_output=True).returncode == 1, "a bad spec: exit 1")
check(subprocess.run([BIN, "--rspro-client", "1:", "rspro:host"], capture_output=True).returncode == 2,
      "a bad --rspro-client: exit 2")

# --- the server goes away: the helper ends --------------------------------------
server, bankd = world(maps={(1, 0): (0, 0)})
h = Helper("rspro:127.0.0.1:%d" % server.port)
h.ask({"op": "power_up"})
server.conn.shutdown(socket.SHUT_RDWR)
try:
    rc = h.p.wait(5)
except subprocess.TimeoutExpired:
    rc = None
check(rc == 1, "the server gone: exit 1 (%r)" % rc)
h.end()

print("test_e2e_rspro: %d checks, %d failed" % (checks, failures))
sys.exit(1 if failures else 0)
