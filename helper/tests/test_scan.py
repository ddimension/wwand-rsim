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
    for d, k, v in ((usbdev, "idVendor", vid), (usbdev, "idProduct", pid), (iface, "bInterfaceNumber", ifnum),
                    (usbdev, "manufacturer", "Maker " + vid), (usbdev, "product", "Product " + pid),
                    (usbdev, "serial", "SER" + name), (usbdev, "speed", "480"), (usbdev, "bcdDevice", "0318"),
                    (iface, "interface", "Iface " + ifnum)):
        open(os.path.join(d, k), "w").write(v + "\n")
    drv = os.path.join(root, "bus/usb-serial/drivers", driver)
    os.makedirs(drv, exist_ok=True)
    os.symlink(drv, os.path.join(dev, "driver"))
    cls = os.path.join(root, "sys/class/tty", name)
    os.makedirs(cls, exist_ok=True)
    os.symlink(dev, os.path.join(cls, "device"))


with tempfile.TemporaryDirectory() as root:
    # the option driver's name in sysfs is "option1" (option.c:2575, 6.18.41)
    port(root, "ttyUSB2", "option1", "2c7c", "0125", "02", True)      # a modem's AT port
    port(root, "ttyUSB0", "cp210x", "10c4", "ea60", "00", True)       # a USB-serial adapter
    port(root, "ttyACM0", "cdc_acm", "1199", "9071", "03", False)     # a CDC-ACM modem port
    port(root, "ttyUSB7", "option1", "2c7c", "0122", "05", True)     # the option driver's USB-serial name
    port(root, "ttyUSB8", "option1", "12d1", "1506", "02", True)     # a Huawei DIAG port (ff/01/03)
    for k, v in (("bInterfaceClass", "ff"), ("bInterfaceProtocol", "03")):
        open(os.path.join(root, "devices/usb1/1-1-ttyUSB8/1-1:1.02", k), "w").write(v + "\n")
    port(root, "ttyUSB9", "ftdi_sio", "104f", "0002", "00", True)     # a Smartmouse via ftdi_sio
    os.makedirs(os.path.join(root, "sys/class/tty/ttyS0"))           # a UART: not listed
    byid = os.path.join(root, "dev/serial/by-id")
    os.makedirs(byid)
    os.symlink("../../ttyUSB0", os.path.join(byid, "usb-Silicon_Labs_CP2102-if00-port0"))

    # BlueZ's storage: a phone offering SAP, a phone that does not, one whose
    # services were never read, a headset, a phone that is only known (no
    # link key), and something that is no device directory at all
    SAP = "0000112d-0000-1000-8000-00805f9b34fb"
    HFP = "0000111f-0000-1000-8000-00805f9b34fb"
    for dev, cls, services, key in (("AA:BB:CC:DD:EE:01", "0x5a020c", HFP + ";" + SAP + ";", True),
                                    ("AA:BB:CC:DD:EE:02", "0x5a020c", HFP + ";", True),
                                    ("AA:BB:CC:DD:EE:03", "0x5a020c", None, True),
                                    ("AA:BB:CC:DD:EE:04", "0x240404", HFP + ";", True),
                                    ("AA:BB:CC:DD:EE:05", "0x5a020c", SAP + ";", False)):
        d = os.path.join(root, "var/lib/bluetooth/00:1A:7D:DA:71:13", dev)
        os.makedirs(d)
        with open(os.path.join(d, "info"), "w") as f:
            f.write("[General]\nName=Phone %s\nClass=%s\n" % (dev[-2:], cls))
            if services is not None:
                f.write("Services=%s\n" % services)
            if dev.endswith("01"):
                f.write("Alias=My Galaxy\nTrusted=true\nBlocked=false\n")
            if key:
                f.write("\n[LinkKey]\nKey=00112233445566778899AABBCCDDEEFF\nType=%d\nPINLength=0\n"
                        % (8 if dev.endswith("01") else 4))
            if dev.endswith("01"):
                f.write("\n[DeviceID]\nSource=1\nVendor=117\nProduct=4352\nVersion=1024\n")
    # the SDP record BlueZ cached from phone 03 (no Services= in its info):
    # SIM Access, L2CAP + RFCOMM channel 8
    cache = os.path.join(root, "var/lib/bluetooth/00:1A:7D:DA:71:13/cache")
    os.makedirs(cache, exist_ok=True)
    rec = ("3521"                       # DES of the record, 33 bytes
           "0900013503" "19112D"         # 0x0001 ServiceClassIDList: SAP
           "090004350C" "3503190100" "3505190003" "0808"   # 0x0004: L2CAP, RFCOMM 8
           "090100" "2503534150")        # 0x0100 name "SAP"
    with open(os.path.join(cache, "AA:BB:CC:DD:EE:03"), "w") as f:
        f.write("[General]\nName=Phone 03\n\n[ServiceRecords]\n0x00010005=%s\n" % rec)

    out = subprocess.run([BIN, "--list"], capture_output=True, text=True, timeout=30,
                         env=dict(os.environ, RSIM_TEST_SYSROOT=root))
    lines = [json.loads(l) for l in out.stdout.splitlines() if l.strip()]
    tty = {l["device"]: l for l in lines if l.get("backend") == "tty"}
    done = lines[-1] if lines else {}

    check(out.returncode == 0, "list: exit 0")
    check(tty.get("/dev/ttyUSB2", {}).get("spec") == "at:/dev/ttyUSB2", "a modem driver's port: at: (%r)" % tty.get("/dev/ttyUSB2"))
    check(tty.get("/dev/ttyUSB2", {}).get("usb") == "2c7c:0125" and tty["/dev/ttyUSB2"].get("interface") == "02",
          "...with its USB ids and interface")
    u0 = tty.get("/dev/ttyUSB0", {})
    check(u0.get("usb_manufacturer") == "Maker 10c4" and u0.get("usb_product") == "Product ea60"
          and u0.get("usb_serial") == "SERttyUSB0" and u0.get("usb_speed_mbps") == "480" and u0.get("usb_version") == "0318"
          and u0.get("usb_path") == "1-1-ttyUSB0" and u0.get("interface_name") == "Iface 00",
          "a port's USB details from sysfs (%r)" % u0)
    check(u0.get("by_id") == "/dev/serial/by-id/usb-Silicon_Labs_CP2102-if00-port0", "...and its stable by-id name")
    check(tty.get("/dev/ttyUSB0", {}).get("spec") == "phoenix:/dev/ttyUSB0", "a USB-serial adapter: phoenix:")
    check(tty.get("/dev/ttyACM0", {}).get("hint") == "at" and tty["/dev/ttyACM0"].get("usb") == "1199:9071",
          "a CDC-ACM port directly on its interface: at:, ids found")
    check("/dev/ttyS0" not in tty, "a built-in UART is not a candidate")
    check(tty.get("/dev/ttyUSB7", {}).get("hint") == "at", "the option driver as option1 (its usb-serial name): at:")
    d8 = tty.get("/dev/ttyUSB8", {})
    check(d8.get("hint") == "diag" and d8.get("spec") == "" and d8.get("role") == "diag",
          "a Huawei DIAG port (ff/xx/03): listed as diag, never offered as at: (%r)" % d8)
    check(done.get("done") is True and "at" in done.get("backends", "").split(","), "the closing line: this build's backends")
    if "bt" in done.get("backends", "").split(","):
        bt = {l["device"]: l for l in lines if l.get("backend") == "bt"}
        check(bt.get("AA:BB:CC:DD:EE:01", {}).get("sap") is True and bt["AA:BB:CC:DD:EE:01"].get("spec") == "bt:AA:BB:CC:DD:EE:01"
              and bt["AA:BB:CC:DD:EE:01"].get("name") == "Phone 01" and bt["AA:BB:CC:DD:EE:01"].get("phone") is True,
              "a paired phone offering SIM Access (%r)" % bt.get("AA:BB:CC:DD:EE:01"))
        check(bt.get("AA:BB:CC:DD:EE:02", {}).get("sap") is False, "a paired phone without SIM Access: listed, sap false")
        check(bt.get("AA:BB:CC:DD:EE:03", {}).get("sap") is True and bt["AA:BB:CC:DD:EE:03"].get("sap_channel") == 8,
              "no service list, but a cached SAP record: offered, on channel 8 (%r)" % bt.get("AA:BB:CC:DD:EE:03"))
        b1 = bt.get("AA:BB:CC:DD:EE:01", {})
        check(b1.get("alias") == "My Galaxy" and b1.get("trusted") is True and b1.get("key") == "authenticated"
              and b1.get("kind") == "smartphone" and b1.get("vendor") == "Samsung" and b1.get("vendor_id") == "0x0075"
              and "SAP" in b1.get("services", "").split(",") and "HFP-AG" in b1.get("services", ""),
              "a phone's details: alias, trusted, key, kind, vendor, services (%r)" % b1)
        check(bt.get("AA:BB:CC:DD:EE:02", {}).get("key") == "unauthenticated", "a Just Works pairing: unauthenticated")
        check("AA:BB:CC:DD:EE:04" not in bt, "a headset is not listed")
        check("AA:BB:CC:DD:EE:05" not in bt, "a phone that is not paired is not listed")
        check(len(bt) == 3, "three phones (%r)" % sorted(bt))
    if "wbsm" in done.get("backends", ""):
        check("/dev/ttyUSB9" not in tty, "a Smartmouse behind ftdi_sio is listed as wbsm:, not as its tty")

# On a router with wwand-rsim: the cards of its modems, from `wwandctl rsim
# proxy --list`, passed on; a line that is not JSON is not
with tempfile.TemporaryDirectory() as root:
    fake = os.path.join(root, "wwandctl")
    with open(fake, "w") as f:
        f.write("#!/bin/sh\necho 'wwandctl: noise'\n"
                "echo '{\"backend\":\"wwand\",\"spec\":\"wwand:iccid:89490200001022832490\",\"modem\":\"m1\",\"lendable\":true}'\n")
    os.chmod(fake, 0o755)
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True, timeout=30,
                         env=dict(os.environ, RSIM_TEST_SYSROOT=root, RSIM_TEST_WWAND_LIST=fake))
    lines = [json.loads(l) for l in out.stdout.splitlines() if l.strip()]
    ww = [l for l in lines if l.get("backend") == "wwand"]
    check(len(ww) == 1 and ww[0]["spec"] == "wwand:iccid:89490200001022832490", "wwand: the modems' cards passed on (%r)" % out.stdout)
    check("wwand" in lines[-1].get("backends", "").split(","), "wwand: named among the backends")
    check(all(isinstance(l, dict) for l in lines), "wwand: only JSON lines")

# BlueZ storage not readable (not root): said so, nothing guessed
with tempfile.TemporaryDirectory() as root:
    os.makedirs(os.path.join(root, "var/lib/bluetooth"))
    os.chmod(os.path.join(root, "var/lib/bluetooth"), 0)
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True, timeout=30,
                         env=dict(os.environ, RSIM_TEST_SYSROOT=root))
    os.chmod(os.path.join(root, "var/lib/bluetooth"), 0o755)
    lines = [json.loads(l) for l in out.stdout.splitlines() if l.strip()]
    if "bt" in lines[-1].get("backends", "").split(",") and os.geteuid() != 0:
        check("run as root" in lines[-1].get("note", ""), "not root: the note says how to see the phones (%r)" % lines[-1])
        check(not any(l.get("backend") == "bt" for l in lines), "...and lists none")

# without wwand-rsim here: nothing of it
with tempfile.TemporaryDirectory() as root:
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True, timeout=30,
                         env={k: v for k, v in dict(os.environ, RSIM_TEST_SYSROOT=root).items() if k != "RSIM_TEST_WWAND_LIST"})
    lines = [json.loads(l) for l in out.stdout.splitlines() if l.strip()]
    if not os.path.exists("/usr/share/ucode/wwand/ctl/rsim.uc"):
        check("wwand" not in lines[-1].get("backends", "").split(","), "no wwand-rsim: no wwand backend")

print("test_scan: %d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
