#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 André Valentin <avalentin@marcant.net>
"""A simulated Phoenix reader with a SIM, for the plugin's end-to-end test.

Prints "<tty> <mctrl fifo>" on one line, then serves until stdin closes.
rsim-card runs with RSIM_TEST_MCTRL=<fifo> (helper/tests/fakecard.py has why).
"""

import os
import sys
import tempfile
import tty

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "helper", "tests"))
from fakecard import FakeCard  # noqa: E402

tmp = tempfile.TemporaryDirectory()
fifo = os.path.join(tmp.name, "mctrl")
os.mkfifo(fifo)
master, slave = os.openpty()
tty.setraw(slave)
card = FakeCard(master, fifo)
card.start()
print(os.ttyname(slave), fifo, flush=True)
sys.stdin.read()
