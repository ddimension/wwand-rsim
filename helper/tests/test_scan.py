#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 André Valentin <avalentin@marcant.net>
"""`rsim-card --list` against a fake /sys: serial ports described from sysfs
alone, with a hint what they are; the closing line names this build's
backends.

usage: test_scan.py <path to rsim-card>
"""

import json
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


def port(root, name, driver, vid, pid, ifnum, below_interface):
    """a tty the way the kernel lays it out: ttyUSB below a port device below
    the USB interface, ttyACM directly on the interface"""
    usbdev = os.path.join(root, "devices/usb1/1-1-" + name)
    iface = os.path.join(usbdev, "1-1:1." + ifnum)
    dev = os.path.join(iface, name) if below_interface else iface
    os.makedirs(dev, exist_ok=True)
    for d, k, v in ((usbdev, "idVendor", vid), (usbdev, "idProduct", pid), (iface, "bInterfaceNumber", ifnum)):
        open(os.path.join(d, k), "w").write(v + "\n")
    drv = os.path.join(root, "bus/usb-serial/drivers", driver)
    os.makedirs(drv, exist_ok=True)
    os.symlink(drv, os.path.join(dev, "driver"))
    cls = os.path.join(root, "sys/class/tty", name)
    os.makedirs(cls, exist_ok=True)
    os.symlink(dev, os.path.join(cls, "device"))


with tempfile.TemporaryDirectory() as root:
    port(root, "ttyUSB2", "option", "2c7c", "0125", "02", True)       # a modem's AT port
    port(root, "ttyUSB0", "cp210x", "10c4", "ea60", "00", True)       # a USB-serial adapter
    port(root, "ttyACM0", "cdc_acm", "1199", "9071", "03", False)     # a CDC-ACM modem port
    port(root, "ttyUSB9", "ftdi_sio", "104f", "0002", "00", True)     # a Smartmouse via ftdi_sio
    os.makedirs(os.path.join(root, "sys/class/tty/ttyS0"))           # a UART: not listed

    out = subprocess.run([BIN, "--list"], capture_output=True, text=True, timeout=30,
                         env=dict(os.environ, RSIM_TEST_SYSROOT=root))
    lines = [json.loads(l) for l in out.stdout.splitlines() if l.strip()]
    tty = {l["device"]: l for l in lines if l.get("backend") == "tty"}
    done = lines[-1] if lines else {}

    check(out.returncode == 0, "list: exit 0")
    check(tty.get("/dev/ttyUSB2", {}).get("spec") == "at:/dev/ttyUSB2", "a modem driver's port: at: (%r)" % tty.get("/dev/ttyUSB2"))
    check(tty.get("/dev/ttyUSB2", {}).get("usb") == "2c7c:0125" and tty["/dev/ttyUSB2"].get("interface") == "02",
          "...with its USB ids and interface")
    check(tty.get("/dev/ttyUSB0", {}).get("spec") == "phoenix:/dev/ttyUSB0", "a USB-serial adapter: phoenix:")
    check(tty.get("/dev/ttyACM0", {}).get("hint") == "at" and tty["/dev/ttyACM0"].get("usb") == "1199:9071",
          "a CDC-ACM port directly on its interface: at:, ids found")
    check("/dev/ttyS0" not in tty, "a built-in UART is not a candidate")
    check(done.get("done") is True and "at" in done.get("backends", "").split(","), "the closing line: this build's backends")
    if "wbsm" in done.get("backends", ""):
        check("/dev/ttyUSB9" not in tty, "a Smartmouse behind ftdi_sio is listed as wbsm:, not as its tty")

print("test_scan: %d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
