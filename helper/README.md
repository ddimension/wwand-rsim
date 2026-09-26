# rsim-card

A small C99 helper that owns one smart-card reader and speaks one JSON object
per line on stdin/stdout. The wwand `rsim` plugin runs it and relays the
modem's QMI UIM Remote requests to it (`docs/plan.md` §2, §3).

## Build

    cmake -S . -B build            # WITH_PCSC=AUTO: PC/SC when pkg-config finds libpcsclite
    cmake --build build
    cd build && ctest --output-on-failure

`-DWITH_PCSC=OFF` builds the Phoenix backend only (no dependency beyond libc);
`-DWITH_PCSC=ON` fails the configure when libpcsclite is missing.
`WITH_LIBUSB` (AUTO/ON/OFF, libusb-1.0 via pkg-config) adds `wbsm:`.
`-DRSIM_WERROR=OFF` drops `-Werror` for a compiler newer than this was
written against. The end-to-end test needs `python3` (standard library only).

## Usage

    rsim-card [-v] [-s] [options] phoenix:/dev/ttyUSB0
    rsim-card [-v] [-s] [options] wbsm:[USB serial]
    rsim-card [-v] [-s] pcsc:<reader-name substring | index>

| Option | Default | |
|---|---|---|
| `-v` | | debug logging (stderr) |
| `-s` | | also log to syslog |
| `--clock KHZ` | 3579 | the reader's card clock; baud = clock / 372 |
| `--reset MODE` | `auto` | `rts`, `rts_inv`, `dtr`, `dtr_inv`; `auto` tries RTS, then inverted RTS, and keeps what answered |
| `--detect LINE` | `none` | `cts`, `dsr`, `cd`: card-detect line (asserted = present), polled every 500 ms while idle |
| `--atr-timeout-ms N` | 1000 | wait for the first ATR byte (1..7000) |
| `--wbsm-mode MODE` | `phoenix` | `smartmouse`: the WB reader's wiring, set by software |

stdin EOF ends the helper: the card is powered down first, exit status 0.

## Protocol

One request line, one answer line. Hex is accepted in either case and written
in upper case.

| Request | Answer |
|---|---|
| `{"op":"power_up"}` | `{"ok":true,"atr":"3B…"}` |
| `{"op":"reset"}` | `{"ok":true,"atr":"…"}` — warm reset; on an unpowered card it is a power-up |
| `{"op":"power_down"}` | `{"ok":true}` |
| `{"op":"tpdu","data":"A0A40000023F00"}` | `{"ok":true,"data":"9F17"}` — response data + SW1 SW2 |
| `{"op":"status"}` | `{"ok":true,"present":true,"powered":true,"backend":"phoenix","reader":"/dev/ttyUSB0","atr":"…"}` (`atr` is `null` when not powered) |

Errors: `{"ok":false,"error":"no_card"|"timeout"|"io"|"bad_request"|"not_powered"|"protocol","detail":"…"}`
(`detail` optional, for humans only).

Unsolicited, only between requests and only when the backend can see the
card (PC/SC, or Phoenix with `--detect`): `{"event":"removed"}`,
`{"event":"inserted"}`. A removal also ends the powered state. Without a
detect line `present` reports whether the last power-up got an ATR.

The TPDU is passed as the modem sent it: `61xx` and `6Cxx` come back
unchanged, GET RESPONSE is the modem's business. A 4-byte TPDU goes out as
case 1 with P3 = 00.

## Backends

**Phoenix / Smartmouse** (`src/phoenix.c`): 8E2 at clock/372 set exactly with
termios2/BOTHER (`src/phoenix_baud.c`, isolated because `<asm/termbits.h>`
clashes with `<termios.h>`; falls back to the nearest standard rate with a
warning). Reset pulse 50 ms, ATR parsed to its exact length (`src/atr.c`),
inverse convention mapped for the rest of the session (with odd parity, which
is how an inverse-convention character looks to a direct-framed UART). The
echo of the shared I/O line is detected on the first header and then
compared on every write. The T=0 engine (`src/t0.c`) runs over a byte
channel; its work waiting time is 960·WI·372/f from TC2, plus 50 ms for USB
latency. There is no VCC control: power-down holds the card in reset.

The serial side is a transport (`struct phx_io` in `src/phoenix.h`): the
kernel tty (`src/phx_tty.c`) for `phoenix:`, or an FTDI chip over libusb
(`src/ftdi_usb.c`) for `wbsm:`.

**WB Electronics Smartmouse USB** (`wbsm:`, `src/wbsm.c`, `WITH_LIBUSB`,
USB 104f:0002, an FT232BM): clock and wiring are latched in bitbang mode on
every open, then the same libusb handle drives the UART directly — no
ftdi_sio, no tty, so it works on kernels without `kmod-usb-serial-ftdi`. A
kernel driver bound to it is detached while rsim-card owns it and reattached
on close. The FTDI side follows ftdi_sio.h (linux 6.18.41): BM divisor
(`src/ftdi_proto.c`, the achieved rate and its deviation are logged), 8E2
via SET_DATA, latency timer 2 ms, flow control off, RTS/DTR via
SET_MODEM_CTRL, card detect via POLL_MODEM_STATUS. Every bulk-IN packet
starts with two status bytes, which are stripped; the error bits are per
packet, so a packet flagged with a parity or framing error is dropped whole
once parity checking is armed — the same as ftdi_sio + `INPCK|IGNPAR`.

**PC/SC** (`src/pcsc.c`, `WITH_PCSC`): exclusive connection, T=0 preferred
(T=1 when that is all the card offers), `SCardTransmit` with the TPDU as is,
`SCardReconnect` for reset/cold start, `SCardDisconnect(UNPOWER)` for
power-down, `SCardGetStatusChange(0)` for presence.

## Test hook

`RSIM_TEST_MCTRL=<path>`: the Phoenix backend writes `RTS=0|1` / `DTR=0|1`
lines to that file instead of driving the modem-control lines, and tolerates a
failing `tcsetattr`. A pseudo-terminal has no modem-control lines, and the
simulated card (`tests/fakecard.py`) must see the reset pulse. Only
`tests/test_e2e.py` sets it.

## Status

Host-tested: ATR parser, T=0 engine (scripted channel, every branch), JSON
layer, the FTDI divisor and IN-packet parser, and the Phoenix backend on
the tty transport end to end against the simulated card (echo
on/off/corrupt, direct and inverse convention, RTS/inverted-RTS/DTR reset,
auto polarity, NULL bytes, ACK and ~INS both ways, 256-byte reads, 61xx/6Cxx,
timeout, late answers, bad procedure bytes, power-down on EOF). Builds for
aarch64 musl (OpenWrt toolchain).

Not verified on hardware: the libusb FTDI transport as a whole (only its
pure parts are unit-tested), real UART timing and parity, the termios2 rate
on a USB-serial bridge, the modem-control lines, card-detect polarity, the PC/SC
backend against pcscd with a card. Not implemented: T=0 character
repetition when the *card* flags a parity error on a byte we sent (it fails
the exchange as an I/O error or a timeout), PPS (the card always runs at
F=372 D=1; a card in specific mode at another rate is logged and will not
work).
