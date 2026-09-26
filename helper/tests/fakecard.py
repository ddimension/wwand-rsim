# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 André Valentin <avalentin@marcant.net>
"""A simulated SIM behind a simulated Phoenix reader, on a pseudo-terminal.

The card sits on the pty master; rsim-card opens the slave as its "tty".
A pty has no modem-control lines, so rsim-card is started with
RSIM_TEST_MCTRL=<fifo> and writes "RTS=0|1" / "DTR=0|1" lines there instead;
this class reads them and treats the configured line as the card's reset.

What it models, because rsim-card has to cope with each on real hardware:
  - the shared I/O line: every byte the reader sends comes back (echo=True)
  - direct or inverse convention (every wire byte mapped, TS reads as 03)
  - reset polarity (active-high or inverted), ATR only on a reset release
  - T=0 procedure bytes: NULL 60, ACK (INS), single-byte ~INS, SW1 SW2
No third-party modules: python3 standard library only.
"""

import os
import select
import threading
import time


def inverse(b):
    """ISO/IEC 7816-3:2006 §8.1 inverse convention as a direct UART sees it:
    bit order reversed, levels inverted. Its own inverse."""
    r = 0
    for i in range(8):
        if b & (1 << i):
            r |= 0x80 >> i
    return (~r) & 0xFF


# the answer to GET RESPONSE after selecting the MF (GSM 11.11 layout,
# content arbitrary but fixed so the test can compare)
FCP_MF = bytes.fromhex("0000FFFF3F000100000000000D13000A04008383008383")
EF_DATA = bytes(range(256))


class FakeCard(threading.Thread):
    def __init__(self, master_fd, mctrl_path, atr="3B9F96801FC78031A073BE21136743200718000001A5",
                 echo=True, conv_inverse=False, reset_line="RTS", reset_active_high=True,
                 answers_reset=True, echo_corrupt=False):
        super().__init__(daemon=True)
        self.fd = master_fd
        # O_RDWR on the FIFO: the read end then never sees EOF when the
        # writer (rsim-card) is not there yet or has gone
        self.mctrl = os.open(mctrl_path, os.O_RDWR | os.O_NONBLOCK)
        self.atr = bytes.fromhex(atr)
        self.echo = echo
        self.inv = conv_inverse
        self.reset_line = reset_line
        self.active_high = reset_active_high
        self.answers_reset = answers_reset
        # a line fault: the last byte of each chunk echoes with bit 0 flipped
        self.echo_corrupt = echo_corrupt
        self.lines = {"RTS": 0, "DTR": 0}
        self.mbuf = b""
        self.rxq = bytearray()
        self.actions = []
        self.in_reset = not reset_active_high  # line low at start
        self.resets = 0          # ATRs sent
        self.tpdus = []          # headers seen
        self.received = []       # data bytes the card took in
        self.ef = bytearray(EF_DATA)
        self.stop = threading.Event()
        self.lock = threading.Lock()

    # -- wire -------------------------------------------------------------
    def _send(self, data):
        wire = bytes(inverse(b) for b in data) if self.inv else bytes(data)
        os.write(self.fd, wire)

    def _reset_asserted(self):
        lvl = self.lines[self.reset_line]
        return lvl == 1 if self.active_high else lvl == 0

    def _mctrl_line(self, line):
        name, _, val = line.partition("=")
        if name not in self.lines:
            return
        was = self._reset_asserted()
        self.lines[name] = int(val)
        now = self._reset_asserted()
        with self.lock:
            self.in_reset = now
        if now and not was:
            self.rxq.clear()
            self.actions = []
        elif was and not now and self.answers_reset:
            # §6.2.2/§6.3.1: the card answers within 400..40 000 clock
            # cycles of the reset release
            self.actions = []
            self.rxq.clear()
            with self.lock:
                self.resets += 1
            self._send(self.atr)

    # -- card behaviour ---------------------------------------------------
    def _plan(self, hdr):
        """The actions for one command header (CLA INS P1 P2 P3)."""
        cla, ins, p1, p2, p3 = hdr
        ack, nack = bytes([ins]), bytes([ins ^ 0xFF])
        if ins == 0xA4:                        # SELECT: two NULLs, ACK, data, SW
            sw = b"\x61\x1F" if cla == 0x00 else b"\x9F\x17"
            return [("send", b"\x60\x60"), ("send", ack), ("recv", p3),
                    ("send", b"\x60"), ("send", sw)]
        if ins == 0xC0:                        # GET RESPONSE
            if p3 != len(FCP_MF):
                return [("send", bytes([0x6C, len(FCP_MF)]))]
            return [("send", ack), ("send", FCP_MF), ("send", b"\x90\x00")]
        if ins == 0xB0:                        # READ BINARY
            n = p3 or 256
            if cla == 0x00 and p3 == 0:
                return [("send", b"\x6C\x0A")]
            data = bytes(self.ef[:n])
            if n < 3:
                return [("send", ack), ("send", data), ("send", b"\x90\x00")]
            # two single bytes via ~INS, then the rest via INS
            return [("send", nack), ("send", data[0:1]), ("send", nack), ("send", data[1:2]),
                    ("send", ack), ("send", data[2:]), ("send", b"\x90\x00")]
        if ins == 0xD6:                        # UPDATE BINARY: ~INS once, then INS
            acts = [("send", nack), ("recv", 1)]
            if p3 > 1:
                acts += [("send", ack), ("recv", p3 - 1)]
            return acts + [("store", p3), ("send", b"\x90\x00")]
        if ins == 0x04:                        # INVALIDATE, case 1
            return [("send", b"\x90\x00")]
        if ins == 0xFA:                        # a card that never answers
            return []
        if ins == 0xFB:                        # answers, but after the WWT
            return [("sleep", 1.4), ("send", b"\x90\x00")]
        if ins == 0xEE:                        # a card out of step
            return [("send", b"\x42")]
        return [("send", b"\x6D\x00")]

    def _run_actions(self):
        while True:
            if not self.actions:
                if len(self.rxq) < 5:
                    return
                hdr = bytes(self.rxq[:5])
                del self.rxq[:5]
                with self.lock:
                    self.tpdus.append(hdr)
                self.actions = self._plan(hdr)
                self.got = bytearray()
                continue
            kind, arg = self.actions[0]
            if kind == "send":
                self._send(arg)
            elif kind == "recv":
                if len(self.rxq) < arg:
                    return
                self.got += self.rxq[:arg]
                del self.rxq[:arg]
                with self.lock:
                    self.received.append(bytes(self.got[-arg:]))
            elif kind == "sleep":
                time.sleep(arg)
            elif kind == "store":
                self.ef[:arg] = self.got[:arg]
            self.actions.pop(0)

    def run(self):
        while not self.stop.is_set():
            r, _, _ = select.select([self.fd, self.mctrl], [], [], 0.02)
            if self.mctrl in r:
                try:
                    self.mbuf += os.read(self.mctrl, 256)
                except BlockingIOError:
                    pass
                while b"\n" in self.mbuf:
                    line, self.mbuf = self.mbuf.split(b"\n", 1)
                    self._mctrl_line(line.decode())
            if self.fd in r:
                try:
                    raw = os.read(self.fd, 512)
                except OSError:
                    return
                if self.echo:
                    if self.echo_corrupt and not self.in_reset:
                        raw_echo = raw[:-1] + bytes([raw[-1] ^ 1])
                    else:
                        raw_echo = raw
                    os.write(self.fd, raw_echo)
                if self.in_reset:
                    continue            # a card held in reset hears nothing
                self.rxq += bytes(inverse(b) for b in raw) if self.inv else raw
                self._run_actions()

    def close(self):
        self.stop.set()
        self.join(2)
        os.close(self.mctrl)
