# wwand-rsim

A SIM card in a reader on the router, used by the modem as if it sat in the
modem's own slot. A plugin for [wwand](https://github.com/ddimension/wwand):
the modem side is Qualcomm's QMI UIM Remote service, the card side a local
reader — a Phoenix/Smartmouse USB-serial reader driven directly, or any PC/SC
(CCID) reader.

Status: implemented and host-tested; not yet run on hardware. See
[docs/plan.md](docs/plan.md).

## Parts

- `helper/` — `rsim-card`, the C helper that owns the reader and speaks one
  JSON object per line on stdin/stdout.
- `plugins/rsim.uc` — the wwand plugin: runs the helper, offers the card to the
  modem, serves its commands.
- `ctl/rsim.uc` — `wwandctl rsim`: status, the modem firmware switches
  (UIM Remote, SIM Access), probe and donor test.
- `luci/` — `luci-app-wwand-rsim`: the configuration page.

## Use

```sh
wwandctl rsim switch                 # is UIM Remote on in the modem firmware?
wwandctl rsim enable --reset         # switch it on (Quectel), reset the modem
uci set network.wwmodem.rsim_reader='phoenix:/dev/ttyUSB0'
uci commit network; ubus call wwand reload
wwandctl rsim                        # reader, card, state
```

With a named reader (`config wwand_simreader`, e.g. from LuCI):

```sh
wwandctl rsim wwmodem0 use smartmouse --wait 120 --json   # run on its card
wwandctl rsim wwmodem0 use off --wait 60 --json           # own SIM again
```

`use` sets `option rsim` and reloads (the modem is not restarted). With
`--wait` it returns once the modem RUNS on the card — the remote card
powered and a new identity read (whether it registers depends on the
network; `modem_state` in the result says), or its own card back and the
modem READY — and exits 0; a
failure that is not retried on its own (no card, reader missing) ends the
wait at once with exit 1. `--json` prints the result as one JSON line
(`ok`, `state`, `iccid`, `error`), for scripts such as a lab test driver.

Options on the `wwand_modem` section: `rsim_reader` (`phoenix:<tty>` or
`pcsc:<reader name or index>`), `rsim_slot` (1), and for Phoenix readers
`rsim_clock` (kHz, 3579), `rsim_reset` (`auto|rts|rts_inv|dtr|dtr_inv`),
`rsim_detect` (`none|cts|dsr|cd`).

Readers: `phoenix:<tty>` (a Phoenix/Smartmouse serial reader with switches),
`wbsm:[USB serial]` (WB Electronics Smartmouse USB: clock and mode set by
software, `rsim_clock` 3580/3680/6000, `rsim_mode` phoenix/smartmouse),
`pcsc:<name or index>`.

**A reader on another machine:** `rsim_reader 'ssh:<user>@<host>:<reader>'`
runs the helper there over SSH, e.g. `ssh:rsim@pc.lan:wbsm:`. On the router,
`wwandctl rsim ssh-key` creates its key and prints the public half for the
other machine's `~/.ssh/authorized_keys`; that machine needs `rsim-card` in
its PATH (or `rsim_ssh_helper`) and access to the reader. Optional:
`rsim_ssh_port`, `rsim_ssh_key`. A dropped connection is handled like a
helper that exited: the modem gets its own SIM back and the plugin retries.

**Another modem's card:** `rsim_reader 'modem:<donor>'` lets another wwand
modem on the router lend its SIM — over the SIM Access Profile
(`rsim_donor_mode sap`, the donor hands its card over; Quectel needs
`wwandctl rsim DONOR sap-enable`), or APDU by APDU while the card stays with
the donor (`rsim_donor_mode apdu`, over QMI UIM or `AT+CSIM`,
`rsim_donor_apdu auto|qmi|at`). A modem configured as another's donor keeps
its radio off, link or not — wwand refuses its interfaces (`radio_held`) and
parks any registration of it — and gets it back when that configuration is
removed, as its own `option lowpower` allows. `rsim_donor_slot` names the donor's physical slot (default: the
one it runs on) — only that one can be lent: on a single-standby modem the
other slot is switched off and cannot be reached, so one modem cannot use one
card and lend the other (HW-checked on a Quectel RG502Q and RG650E,
2026-09-26). `wwandctl rsim MODEM probe` and `donor-test` tell whether a modem
can lend its card.

**LuCI:** `luci-app-wwand-rsim` — Network → Remote SIM sets all of this per
modem.

Removing `rsim_reader` gives the modem its own SIM back.

## Building the helper

`rsim-card` is the only part a **SIM host** needs: with
`option rsim_reader ssh:<user>@<host>:<reader>` the plugin runs it on that
machine **by name**, so it has to be in its `PATH` (`/usr/local/bin/rsim-card`
is enough). Nothing in it is architecture-specific — the toolchain decides —
and the backends are optional, so it builds anywhere CMake does:

```sh
cmake -S helper -B build && cmake --build build          # dynamic, backends AUTO

# static, for a host with nothing installed
cmake -S helper -B build-static -DRSIM_STATIC=ON -DWITH_LIBUSB=OFF -DWITH_PCSC=OFF
cmake --build build-static

# cross build: the usual toolchain file, same switches
cmake -S helper -B build-arm -DRSIM_STATIC=ON \
      -DCMAKE_TOOLCHAIN_FILE=/path/to/arm.cmake
cmake --build build-arm
```

`RSIM_STATIC=ON` links with `-static` and takes the **static** link lines from
pkg-config, so an enabled backend brings its own dependencies along. That is
also its limit: `libusb-1.0` on a glibc distribution pulls `libudev`, which is
rarely installed as an archive — the link then stops with *"have you installed
the static version of the udev library?"*. Either install it, or leave the
Smartmouse USB out (`-DWITH_LIBUSB=OFF`, Phoenix readers still work). On
OpenWrt/musl libusb needs no udev, so a fully static build with the USB
backend works there.

## Tests

```sh
tests/run_tests.sh                   # plugin + ctl, against ../wwand
(cd helper && cmake -S . -B build && cmake --build build && cd build && ctest)
```

## License

GPL-2.0-only.
