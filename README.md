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
- `ctl/rsim.uc` — `wwandctl rsim`: status, and the modem firmware switch.

## Use

```sh
wwandctl rsim switch                 # is UIM Remote on in the modem firmware?
wwandctl rsim enable --reset         # switch it on (Quectel), reset the modem
uci set network.wwmodem.rsim_reader='phoenix:/dev/ttyUSB0'
uci commit network; ubus call wwand reload
wwandctl rsim                        # reader, card, state
```

Options on the `wwand_modem` section: `rsim_reader` (`phoenix:<tty>` or
`pcsc:<reader name or index>`), `rsim_slot` (1), and for Phoenix readers
`rsim_clock` (kHz, 3579), `rsim_reset` (`auto|rts|rts_inv|dtr|dtr_inv`),
`rsim_detect` (`none|cts|dsr|cd`).

Removing `rsim_reader` gives the modem its own SIM back.

## Tests

```sh
tests/run_tests.sh                   # plugin + ctl, against ../wwand
(cd helper && cmake -S . -B build && cmake --build build && cd build && ctest)
```

## License

GPL-2.0-only.
