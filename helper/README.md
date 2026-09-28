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
`WITH_BLUETOOTH` (ON/OFF, no dependency) adds `bt:`.
`WITH_RSPRO` (ON/OFF, no dependency) adds `rspro:`.
`-DRSIM_WERROR=OFF` drops `-Werror` for a compiler newer than this was
written against. The end-to-end test needs `python3` (standard library only).

## Usage

    rsim-card [-v] [-s] [options] phoenix:/dev/ttyUSB0
    rsim-card [-v] [-s] [options] wbsm:[USB serial]
    rsim-card [-v] [-s] pcsc:<reader-name substring | index>
    rsim-card [-v] [-s] [options] at:<tty>
    rsim-card [-v] [-s] [options] bt:<phone's Bluetooth address>
    rsim-card [-v] [-s] [options] rspro:<server>[:<port>][/<bank>:<slot>]
    rsim-card --list --rspro-server <server>[:<port>] [--rspro-rest-port N]

| Option | Default | |
|---|---|---|
| `-v` | | debug logging (stderr): each command's INS and size and its status word, never the bytes of a command or an answer (PIN, authentication, the card's files) |
| `-s` | | also log to syslog |
| `--clock KHZ` | 3579 | the reader's card clock; baud = clock / 372 |
| `--reset MODE` | `auto` | `rts`, `rts_inv`, `dtr`, `dtr_inv`; `auto` tries RTS, then inverted RTS, and keeps what answered |
| `--detect LINE` | `none` | `cts`, `dsr`, `cd`: card-detect line (asserted = present), polled every 500 ms while idle |
| `--atr-timeout-ms N` | 1000 | wait for the first ATR byte (1..7000) |
| `--wbsm-mode MODE` | `phoenix` | `smartmouse`: the WB reader's wiring, set by software |
| `--bt-channel N` | SDP | the phone's SAP RFCOMM channel, 1..30 |
| `--bt-security L` | `medium` | `high`: a link key with MITM protection required |
| `--bt-apdu F` | `gsm` | `7816`: send CommandAPDU7816 instead of CommandAPDU |
| `--rspro-client ID[:SLOT]` | `0:0` | our client id and slot at the remsim-server |
| `--rspro-rest-port N` | 9997 | the remsim-server's REST API (bank slot mapping, `--list`) |

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

**Bluetooth SIM Access** (`bt:`, `src/bt.c`, `src/sap.c`, `WITH_BLUETOOTH`):
the SAP v1.1 client over kernel sockets only — an SDP
ServiceSearchAttributeRequest over L2CAP for the SIM Access class (0x112D)
gives the RFCOMM channel, then the RFCOMM link with `BT_SECURITY` medium (or
high) set before connecting. CONNECT_REQ asks for 1024-byte messages and
takes the phone's size instead when it says so (at least 276 are needed for
a 261-byte command); the first STATUS_IND says the card is ready. power_up
is POWER_SIM_ON (when off) + TRANSFER_ATR, reset RESET_SIM + TRANSFER_ATR,
power_down POWER_SIM_OFF, tpdu TRANSFER_APDU with the bytes as they are —
61xx/6Cxx come back unchanged. STATUS_IND is followed between requests:
removed/not accessible report the card absent, inserted present again; a
reset or recovery by the phone after an ATR went out is reported as removed
+ inserted, so the target reads the card anew. DISCONNECT_IND (answered with
DISCONNECT_REQ when graceful), a link that drops and an answer that does
not come within 25 s (SAP answers carry no request id: a late one would be
taken for the next request's) end the helper with status 1; stdin EOF ends it with POWER_SIM_OFF and DISCONNECT_REQ, so the
phone has its SIM back at once. Pairing and link keys are BlueZ's: the
helper does no pairing. `--list` reads BlueZ's storage
(`/var/lib/bluetooth/<adapter>/<device>/info`): paired (a BR/EDR link key)
phones (device class) or devices whose stored services include SAP, with
`"sap": true|false|null` (null: services never read); nothing is sent to a
phone.

**osmo-remsim SIM bank** (`rspro:`, `src/remsim.c`, `src/rspro.c`,
`WITH_RSPRO`): a remsim client. RSPRO is osmo-remsim's ASN.1 module
(`asn1/RSPRO.asn`, IMPLICIT TAGS, BER; `RsproPDU.version` 2), each message
in an IPA frame (ip.access's multiplex header: u16 length, 0xEE, extension
0x07 — not the eIM's IoT Profile Assistant) over TCP. IPA's own signalling
is answered as an IPA client does (libosmocore's ipa_ccm_rcvmsg_bts_base):
PING with PONG, ID_GET with ID_RESP (unit name `rsim-card`), ID_ACK not at
all — a server answers it, so two answering sides would never stop. `src/rspro.c` encodes and decodes by hand only what a client
needs (the tests check it against hand-built BER). A PDU is taken only
whole: one RsproPDU and nothing after it, one version (2 — another is
skipped with its number in the log), one tag, exactly one alternative, no
element that does not parse, and every field the module does not mark
OPTIONAL — a ConnectClientRes without its result is no acceptance, a
ConfigClientBankReq without a port no mapping. Anything else is logged and
skipped. The session:
ConnectClientReq (identity + client id:slot) to the server (default port
9998) → ConnectClientRes; the server's ConfigClientIdReq (it may reassign
the client slot) and ConfigClientBankReq (bank id:slot + bankd ip:port; the
all-zero address when the mapping is removed; ResetStateReq drops it too)
are answered `ok`; the client then connects to that bankd with its own
ConnectClientReq and waits for SetAtrReq, the card's ATR. A SetAtrReq for
another client slot, or a TpduCardToModem from another bank slot or for
another client than the mapping says, is not our card's: dropped (the ATR
refused with `illegalClientId`) and logged. That connect does
not block: it runs on while the helper serves (a bankd that drops SYNs
would otherwise stall stdin and the server's PINGs), 5 s at most, retried
every 5 s. power_up waits up to 10 s for the ATR (no mapping: `no_card`
with the reason; no bankd: `io`, the bankd named). An ATR the bankd has
just sent on its own (it brought the card up) is the answer as it is;
otherwise — after a power_down, or later — power_up signals a cold start as
a card emulator would: ClientSlotStatusInd RST active, then released, VCC
and CLK on, and answers with an ATR the bankd sends within 1.5 s, else the
one it has; reset the same; power_down RST active, VCC and CLK off. tpdu is one TpduModemToCard (header present, final part) → the
TpduCardToModem's data, response and SW as the bankd's SCardTransmit
returned them (25 s timeout; after one the bankd link is dropped and made
again — an answer names no command, a late one would answer the next). Between requests (every 500 ms) the server's
and bankd's messages are taken in: a mapping that arrives becomes
`inserted`, one removed (or a bankd that went away) `removed`; a bankd that
cannot be reached is tried again every 5 s. The server closing the
connection ends the helper with 1.
With `/<bank>:<slot>` in the spec the helper first POSTs
`{"bank":{"bankId","slotNr"},"client":{"clientId","slotNr"}}` to
`/api/backend/v1/slotmaps` on the REST port; a refusal is looked up in
`GET …/slotmaps` (mapped to our client slot: ours — the client slot is the
reader's identity, and a run that could not clean up, killed or without
power, must not hold the slot for good — but not removed when the server
then refuses the client as `identityInUse`: a live session holds it; to
another client: refused). A slot the server takes away during the session
is mapped again (at most every 30 s). At the
end it DELETEs `…/slotmaps/<bank << 16 | slot>`. REST answers up to 1 MiB
(the JSON token array sized for the answer). `--list --rspro-server` reads `GET /api/backend/v1/banks`
(`bankId`, `numberOfSlots`, `component_id.name`) and `…/slotmaps`: one line
per slot, `{"backend":"rspro","spec":"rspro:<server>/<bank>:<slot>",
"bank","slot","name","bank_state","peer"}` plus `mapped_to` / `map_state`
when it is mapped, then `{"done":true,"backends":"rspro","slots":N}` with an
`error` when the server could not be asked. HTTP/1.0 and a small JSON token
reader (`src/jtok.c`); no library.
Written from the osmo-remsim sources, **not run against a real
remsim-server or bankd yet** (2026-09-27); `tests/test_e2e_rspro.py`
simulates both from the same reading.

**`--serve [SPEC...]`** is the `command=` of an `authorized_keys` line: it
reads what the SSH client asked for from `SSH_ORIGINAL_COMMAND`, splits it
into words as a shell would for plain and quoted words (no expansion), and
execs it only when it is `rsim-card [options] <reader>` with a reader one of
the SPECs matches (fnmatch with `FNM_PATHNAME`: a `*` does not cross a `/`,
so `at:/dev/ttyUSB*`, not `at:*`; no SPECs: any), `rsim-card --list`, or
`wwandctl rsim proxy [options] <target>` with `wwand:<target>` matching (or
its `--list`). With SPECs, both lists are cut down to the rows they match (a
modem's card also by `wwand:<modem>`). A reader that names a path must be a
serial port under `/dev` (`tty*`, `rfcomm*`, `pts/*`) — rsim-card's AT and
Phoenix backends refuse anything that is not a tty themselves. An `rspro:`
reader is never served, whatever the SPECs say: its spec names a host and a
port, so the SIM host would connect wherever the caller says, map a bank
slot over that host's REST port and relay the card (a SIM bank is reached
from the router directly). Only rsim-card's own options pass, spelled out. A second reader, `--serve` again,
an option the proxy lacks, or anything else is refused with a message. `RSIM_TEST_SELF` /
`RSIM_TEST_WWANDCTL`: what to exec instead, for the tests.

On a router with wwand-rsim-provider, `--list` also passes on the lines of
`wwandctl rsim proxy --list` — the cards of its modems another router can
borrow — and names `wwand` among the backends (`RSIM_TEST_WWAND_LIST`: the
command to run instead, for the tests). On a wwand router without it
(`/usr/bin/wwandctl` there, `/usr/share/ucode/wwand/ctl/rsim_provider.uc`
not), the done line carries `"wwand_provider": false` and no card of its
modems is listed — the scanning router says which package is missing; its
AT ports are listed as usual.

## Test hook

`RSIM_TEST_MCTRL=<path>`: the Phoenix backend writes `RTS=0|1` / `DTR=0|1`
lines to that file instead of driving the modem-control lines, and tolerates a
failing `tcsetattr`. A pseudo-terminal has no modem-control lines, and the
simulated card (`tests/fakecard.py`) must see the reset pulse. Only
`tests/test_e2e.py` sets it.

`RSIM_TEST_SAP_SOCK=<path>`: the `bt:` backend connects to a unix socket
instead of the phone (no SDP, no Bluetooth), and
`RSIM_TEST_SAP_TIMEOUT_MS` shortens the answer timeout there; `tests/test_e2e_sap.py` runs a
simulated SAP server there.

## Status

Host-tested: ATR parser, T=0 engine (scripted channel, every branch), JSON
layer, the FTDI divisor and IN-packet parser, and the Phoenix backend on
the tty transport end to end against the simulated card (echo
on/off/corrupt, direct and inverse convention, RTS/inverted-RTS/DTR reset,
auto polarity, NULL bytes, ACK and ~INS both ways, 256-byte reads, 61xx/6Cxx,
timeout, late answers, bad procedure bytes, power-down on EOF). Builds for
aarch64 musl (OpenWrt toolchain).

Verified on hardware (2026-09-27): the Smartmouse USB (`wbsm:`, libusb
FTDI transport) with a real card; Bluetooth SAP with a Galaxy S20 FE and a
Galaxy A5 (2016); the AT backend's refusals (a diag port, a modem without
`AT+CSIM`, Samsung's AT lock) — see the top-level README, *What works*.

Not verified on hardware: the `rspro:` backend against osmo-remsim, the libusb FTDI transport's other adapters, real UART timing and parity, the termios2 rate
on a USB-serial bridge, the modem-control lines, card-detect polarity, the PC/SC
backend against pcscd with a card. Not implemented: T=0 character
repetition when the *card* flags a parity error on a byte we sent (it fails
the exchange as an I/O error or a timeout), PPS (the card always runs at
F=372 D=1; a card in specific mode at another rate is logged and will not
work).
