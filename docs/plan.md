# wwand-rsim — plan

A SIM card in a reader on the router, used by the modem as if it sat in the
modem's own slot. The modem side is Qualcomm's QMI **UIM Remote** service; the
card side is a local reader: PC/SC (any CCID reader) or a Phoenix/Smartmouse
USB-serial reader driven directly. Other card backends (a remote reader over
IP, a software USIM) can follow behind the same helper interface.

Status: implemented and host-tested (2026-09-26): plugin, `wwandctl rsim`, the
rsim-card helper with Phoenix and PC/SC backends, the core `qmi_client` dep
(wwand 06e859d). Nothing is hardware-verified yet (§7 step 5).

## 1. What the modem offers

QMI service **UIM Remote** (`UIMRMT`, service 0x32; libqmi knows the service
id only, MR !453). The messages, TLV ids and layouts below are interface facts
needed for interoperability; they were checked against two independent
implementations of the client side and must be re-verified on the wire (§8).

| Id | Name | Direction | TLVs |
|---|---|---|---|
| 0x20 | RESET | req/resp | — |
| 0x21 | EVENT | req/resp | 0x01 `{u32 event, u32 slot}`; 0x10 ATR `u8 len + bytes` (≤ 32); 0x11 wakeup `u8`; 0x12 error cause `u32`; later IDL versions add transport, usage, APDU timeout, polling |
| 0x22 | APDU | req/resp | 0x01 status `u16` (0 ok, 1 failure); 0x02 slot `u32`; 0x03 APDU id `u32`; 0x10 `{u32 total, u32 offset}`; 0x11 response segment `u16 len + bytes` (≤ 1024) |
| 0x22 | APDU_IND | ind | 0x01 slot `u32`; 0x02 APDU id `u32`; 0x03 command `u16 len + bytes` (≤ 261) |
| 0x23 | CONNECT_IND | ind | 0x01 slot |
| 0x24 | DISCONNECT_IND | ind | 0x01 slot |
| 0x25 | CARD_POWER_UP_IND | ind | 0x01 slot; 0x10 response timeout `u32` ms; 0x11 voltage class `u32` |
| 0x26 | CARD_POWER_DOWN_IND | ind | 0x01 slot; 0x10 mode `u32` (0 telecom interface, 1 card) |
| 0x27 | CARD_RESET_IND | ind | 0x01 slot |

Events: 0 connection unavailable, 1 connection available, 2 card inserted,
3 card removed, 4 card error (+cause: 0 unknown, 1 no link, 2 timeout,
3 power down, 4 power down telecom), 5 card reset (+ATR), 6 card wakeup.
Slots: 1, 2 (3 on newer IDL).

**Sequence.**

1. Client: `EVENT(connection available, slot)`. The modem stops using the local
   card of that slot.
2. Modem: `CONNECT_IND` → client powers the card, answers
   `EVENT(card reset, slot, ATR)`. The ATR must arrive within 7 s.
3. Modem: `APDU_IND(id, TPDU)` → client sends the TPDU to the card and answers
   `APDU(status, slot, id, response)`; a response over 1024 bytes goes in
   segments with `{total, offset}` (a T=0 response is at most 258 bytes, so in
   practice one segment).
4. Modem: `CARD_POWER_DOWN_IND` / `CARD_POWER_UP_IND` / `CARD_RESET_IND` →
   the same on the card; after power-up and reset, `EVENT(card reset, ATR)`.
5. Client leaving: `EVENT(card removed)`, `EVENT(connection unavailable)`.
   The modem goes back to its local card — which wwand-rsim lets it USE
   only with `rsim_fallback local`: otherwise its radio is parked before
   the events (the configuration still assigning a remote SIM) and stays
   off until the remote card is connected again.

The commands are **T=0 TPDUs** (CLA INS P1 P2 P3 [data]); the modem does GET
RESPONSE itself, the client passes `61xx`/`6Cxx` back unchanged.

**The service is disabled on every modem we have.** The switch is the EFS item
`/nv/item_files/modem/uim/remote/uim_remote_service_enable` (read with
`AT+QNVFR`): `00` on the RG650E (245), RG502Q (242) and RM520N-GL (3.93).
Enabling it is `AT+QNVFW=…,01` plus a modem reset. That is a modem
configuration change and is done only by an explicit command (§5), never
automatically.

## 2. Architecture

```
reader ── rsim-card (C) ── stdio lines ── plugin rsim.uc ── QMI UIMRMT client ── modem
          phoenix | pcsc                  (wwand daemon)     (core dep qmi_client)
```

- **`rsim-card`** (C, small, no deps beyond libc; PC/SC optional at build
  time): owns the reader. One JSON object per line on stdin/stdout, the same
  style as ipad/lpac:
  - `{"op":"power_up"}` → `{"ok":true,"atr":"3B…"}`
  - `{"op":"reset"}` (warm) → `{"ok":true,"atr":"…"}`
  - `{"op":"power_down"}` → `{"ok":true}`
  - `{"op":"tpdu","data":"A0A4000002"}` → `{"ok":true,"data":"9F17"}`
  - `{"op":"status"}` → `{"ok":true,"present":true,"reader":"…"}`
  - errors: `{"ok":false,"error":"no_card"|"timeout"|"io"|…}`
  - unsolicited: `{"event":"removed"}` / `{"event":"inserted"}` when the
    backend can see card presence.
- **Plugin `rsim.uc`** (wwand plugin interface): per modem with
  `option rsim_reader`, it runs the helper, allocates the UIMRMT client, drives
  the sequence above, and reports state through `ops.status` /
  `wwandctl rsim`.
- **Core (wwand, neutral):** one new plugin dep, `qmi_client(ref, schema, cb)`:
  a QMI client of a schema the plugin brings (wwand's own schema format), on
  the modem's QMI channel. `cb(err, client)`; errors `no_modem`,
  `service_unavailable` (not in the modem's GET_VERSION_INFO list),
  `unsupported` (a modem with no QMI at all: NCM, or MBIM without the
  QMI-over-MBIM passthrough), or the ALLOCATE_CID failure. An MBIM modem
  with the passthrough gets its client over it — HW-verified as a client on
  the Quectel RM520N-GL (GL-X3000, 2026-09-26/27; README *What works*). The modem owns the client: it is
  released (RELEASE_CID) on teardown like its own, and `client.destroyed`
  tells the plugin it has to allocate again. It knows nothing about UIM
  Remote.

Why a separate process for the card: the daemon is single-threaded and a card
exchange blocks for up to seconds (work waiting time, NULL procedure bytes);
the reader library (pcsclite) is C; and a helper can be replaced by a
different card source without touching the plugin.

## 3. Card backends

### 3.1 Phoenix / Smartmouse (USB-serial)

The reader the maintainer has at hand is a Smartmouse USB: a USB-serial bridge
(FTDI/PL2303) with the card I/O line on RX/TX, the card reset on a modem
control line, and a fixed card clock (switch: 3.579 / 6.000 MHz).

- **Line:** 8 data bits, even parity, 2 stop bits; baud = clock / 372
  (F=372, D=1, no PPS): 9622 baud at 3.579545 MHz, 16129 at 6 MHz, set exactly
  with `termios2`/`BOTHER`.
- **Reset:** RTS by default, polarity configurable (`phoenix` vs `smartmouse`
  wiring differ exactly there); `auto` tries one polarity, then the other when
  no ATR arrives. DTR is held high (some readers take power from it).
- **Echo:** the I/O line is shared, so every sent byte comes back; it is read
  and compared, and a mismatch is an I/O error.
- **ATR:** read with the ISO 7816-3 timing (first byte within 40 000 clock
  cycles, then initial waiting time between characters), parsed for TS
  (direct convention `3B`; inverse `3F` handled by the bit-reversal table),
  T0/TA-TD/historical bytes/TCK, to know where it ends and which protocols the
  card offers. T=0 required.
- **T=0 TPDU engine:** header, then procedure bytes: `60` NULL (keep waiting),
  INS (all remaining data), ~INS (one byte), `6x`/`9x` SW1 then SW2. Case 2/4
  reads P3 bytes (0 = 256). Work waiting time 960·WI·F/f from TC2 (default
  WI=10 → ~1 s at 9600).
- **Presence:** optional (`detect cts|dsr|cd|none`), default none: a failed
  ATR means no card.

### 3.2 PC/SC

For CCID readers (the common USB token readers): `pcscd` + `ccid` from the
OpenWrt packages feed (pcsc-lite 2.5.0). `SCardConnect` T=0 (fallback T=1),
`SCardStatus` for the ATR, `SCardTransmit` passes the TPDU through as is,
`SCardReconnect` (reset / unpower) for reset and power cycles,
`SCardGetStatusChange` for insert/remove events. Built only when libpcsclite
is present (`WITH_PCSC`), so the Phoenix-only package has no dependency.

### 3.3 Next: the helper on another machine, over SSH (decided 2026-09-26)

The plugin talks to rsim-card only through its stdin/stdout, so a reader on
another machine needs no new protocol: the plugin starts
`ssh <user>@<host> rsim-card <reader>` instead of the local helper, and the
lines travel over SSH (encrypted, key-authenticated). The card sits in the
reader on the PC, the modem on the router uses it.

- Config: `rsim_reader 'ssh:<user>@<host>:<reader>'`, e.g.
  `ssh:rsim@pc.lan:wbsm:`; the plugin builds
  `ssh -T -o BatchMode=yes <user>@<host> rsim-card <reader> [options]`
  (dropbear's `dbclient` on OpenWrt; key in `/etc/wwand/rsim/id_*`,
  `rsim_ssh_key` to override).
- The remote side needs rsim-card and access to the reader (for `wbsm:`
  the udev rule for 104f:0002 plus ftdi_sio `new_id`, or root).
- Nothing else changes: a dropped connection is a helper exit, which the
  plugin already turns into "card removed, connection unavailable" and a
  retry with backoff.
- Latency: a LAN adds milliseconds per APDU against a modem timeout of
  seconds.

### 3.5 Another modem's card (donor) — implemented 2026-09-26

`rsim_reader 'modem:<donor>'`: another wwand modem on the router lends its
SIM. Both ways run inside the daemon through the core's `qmi_client` /
`modem_at` deps (no helper process) and look like the helper's card channel to
the session logic.

- **`rsim_donor_mode sap`** — the donor's UIM service as SIM Access Profile
  server: SAP_CONNECTION (0x003C) connect, SAP_REQUEST (0x003D) for ATR /
  APDU / power off / power on / reset, indication 0x003E. Layout from
  Qualcomm's Gobi API, BSD-3, shipped in the libqmi 1.38.0 tarball
  (`gobi-api/`); every enum there is one byte. The donor stops using the card
  while the link stands.
  - RG650E (245): the service is there (status answers "not enabled"), the
    connect is refused with ACCESS_DENIED while
    `/nv/item_files/modem/qmi/uim/sap_security_restrictions` is absent;
    `wwandctl rsim MODEM sap-enable` writes it 00 (by analogy with
    `apdu_security_restrictions` = 00). TLV 0x12 (condition) is refused as
    malformed there — `rsim_donor_cond none` leaves it out.
  - Huawei E392 (245): takes its card away on connect but never answers, and
    does not recover without a modem reset. The plugin therefore ends a sent
    connect on every exit path and does not retry an unanswered one on its
    own (HW-found, 2026-09-26).
- **`rsim_donor_mode apdu`** — the card stays with the donor, whose radio
  must be off (one card, one registration). APDUs over QMI UIM SEND_APDU or
  AT+CSIM (`rsim_donor_apdu auto|qmi|at`; AT goes through the core's
  modem_at, so AT over MBIM too); the ATR from UIM GET_ATR, or the minimal
  T=0 ATR `3B00` over plain AT; power and reset are answered with that ATR.
  HW-verified on the RG650E over QMI and AT (SELECT MF -> 6134).
- **Both modes park the donor's radio** (`modem_radio`) while it lends and
  hand it back afterwards. Over SIM Access the card is gone from the donor
  anyway; parked, the lost registration is intended instead of a fault the
  recovery ladder answers with a modem reset (rung 16), which would end the
  link. The core owns the park: it records it, wakes the radio on the
  hand-back only as its own policy says (`option lowpower`), releases a park
  nothing holds any more, does not dial or cycle a parked radio, and reports
  the registration after a wake, so interfaces given up while the card was
  lent come back. The plugin's `radio_hold` answers for every modem
  CONFIGURED as a sponsor, link or not: the core refuses its ifups and parks
  any registration of it at once. Each tick ends the link when the donor
  restarted.
  - Left open: after an UNCLEAN daemon exit (a crash; `stop` hands the card
    back first) the modem init of the new daemon switches the sponsor's
    radio online before its first registration is parked again — a short
    window in which it may register with the card. An init that stays in
    low power would stall its registration step instead, which the
    recovery ladder answers with resets.
- **Which slot:** `rsim_donor_slot` is the donor's PHYSICAL slot (default:
  the one it runs on), mapped to the logical slot QMI UIM addresses through
  the core's `sim_slots`. Only the active slot can be lent. "Use one slot,
  lend the other" was asked for and does not work on single-standby modems:
  on the RG502Q (NR7101, 242) and the RG650E (245) both physical slots map
  to logical slot 1, the inactive one is switched off, SEND_APDU on slot 2
  is refused NOT_SUPPORTED (94), a logical channel opened "on slot 2" lands
  on the active card (both read the same EF_ICCID), and the firmware offers
  AT+QUIMSLOT (switch) but no dual-standby option in AT+QCFG (HW-read,
  2026-09-26). A dual-standby modem (two logical slots) could in principle
  lend its second card, but its second subscription would register as well;
  without such a modem that is not built.
- Diagnostics: `wwandctl rsim MODEM probe` (read-only: SIM Access present?),
  `wwandctl rsim MODEM donor-test [sap|apdu] [qmi|at]` (lend once, ATR +
  SELECT MF, hand back).

### 3.5b A modem wwand does not manage (`at:<tty>`) — implemented 2026-09-26

The helper's `at:` backend (helper/src/atmodem.c): the card of any modem
with an AT port — typically one on a SIM host, reached over SSH like a
reader — as APDUs over AT+CSIM, the same path as the plugin's AT donor
inside the router. The modem keeps the card; power and reset answer the
minimal ATR 3B00. It deregisters (AT+COPS=2, its selection restored at the
end) and is put into CFUN=4 while the card is lent (the SIM
stays reachable, the radio off: one card, one registration) and back to its
previous mode at the end; SIGTERM/SIGHUP/SIGINT end the helper through the
same cleanup, so a dropped SSH link does not leave it off. Tested against a
simulated AT port (tests/test_e2e_at.py), not yet on hardware.

### 3.6 LuCI — implemented 2026-09-26

`luci-app-wwand-rsim`, Network → Remote SIM: per wwand_modem the SIM source
(own SIM, Smartmouse USB, Phoenix, PC/SC, over SSH on another machine,
another modem lending its card) with only the fields it needs, stored as the
one option `rsim_reader` plus the rsim_* options; status per modem with a
restart; the router's SSH key to copy. The modem status page shows the remote
SIM through the core's plugin status rows.

### 3.7 An osmo-remsim SIM bank (`rspro:`) — implemented 2026-09-27

rsim-card as a remsim client (helper/README.md, *osmo-remsim SIM bank*):
RSPRO to the remsim-server and the bankd it names, its own BER codec, no
dependency, `WITH_RSPRO` at build time. The bank slot is either the
operator's mapping for our client id, or named in the spec and mapped by
the helper over the server's REST API for as long as it runs. Enumeration
is the REST API too — RSPRO has no message that lists banks or slots.
Plugin: `rsim_reader 'rspro:…'`, `rsim_rspro_client`,
`rsim_rspro_rest_port`; a named reader `type rspro`; never over SSH (the
router reaches the server itself). `wwandctl rsim scan --rspro`, LuCI:
the kind *SIM bank*, and *Find SIM sources → SIM bank*.
Not tested against a real osmo-remsim: open points are the reset
signalling (whether a bankd resets on the RST edge, and whether it sends a
new ATR — both are handled either way), the REST id of a slotmap in the
DELETE, and the RSPRO version the server accepts.

### 3.4 Later

A software USIM (Milenage from Ki/OPc) — another card behind the same
helper lines.

## 4. Plugin behaviour

- **Config** (on the `wwand_modem` section, plugin options):
  `rsim_reader` (`phoenix:/dev/ttyUSB0`, `pcsc:<reader name or index>`),
  `rsim_slot` (1), `rsim_clock` (kHz, Phoenix, default 3579), `rsim_reset`
  (`auto|rts|rts_inv|dtr|dtr_inv`), `rsim_detect` (`none|cts|dsr|cd`). A
  helper answer that does not come within 15 s (a card sending NULL bytes
  forever, a dead reader) restarts the helper.
- **Bring-up:** when the modem is up and the reader is configured: start the
  helper, `power_up` to prove there is a card, allocate the UIMRMT client
  (`service_unavailable` → status says the EFS switch is off, with the command
  that enables it), `EVENT(connection available)`, then serve indications.
- **Serving:** APDU indications are queued and sent to the helper one at a
  time in order (the modem sends one at a time anyway; the queue only guards
  against a second one arriving while the first is on the card). A helper
  error answers the APDU with status failure and sends `EVENT(card error)`.
- **Card gone / helper died:** `EVENT(card removed)`; restart the helper with
  backoff; card back → `EVENT(card inserted, ATR)`.
- **Leaving:** `rsim_reader` removed, plugin stop, modem stop:
  `EVENT(card removed)` + `EVENT(connection unavailable)` so the modem returns
  to its own card; client released. Where the configuration still assigns a
  remote SIM (a failure, a restart, another reader, the daemon's exit) and
  the modem runs on the remote card, its radio is parked FIRST, so it
  deregisters with the card that leaves; it stays off unless
  `rsim_fallback local` (README).
- **Interplay with wwand:** the modem sees an ordinary SIM in that slot, so
  PIN (per-ICCID `wwand_sim`), APN resolution and registration work unchanged.
  The daemon's SIM hot-reset and slot switch act on the modem; the plugin
  follows through the power/reset indications.
- **Logging:** state changes at notice (`rsim: connected, ATR …`,
  `card removed`, `service disabled on this modem`), each APDU at debug with
  INS/SW and duration.
- **Status:** `wwandctl rsim [modem]`: reader, card present, ATR, state
  (disconnected/connected/powered), APDU count, last SW, last error.

## 5. Enabling the service on a modem (`wwandctl rsim`)

- `wwandctl rsim [modem] switch` — reads the EFS item (and the optional
  response timer `/nv/item_files/modem/uim/uimdrv/nv_remote_command_resp_timer`).
- `wwandctl rsim [modem] enable|disable [--reset]` — writes `01`/`00` with
  `AT+QNVFW`, reads it back (an OK that did not change the item is reported,
  not claimed), and with `--reset` resets the modem, since the item is read
  at modem boot only.

Only this one item gates the service: both the QMI UIM Remote registration
and the card driver's remote mode read it at boot. No legacy NV item is
involved; the NV 453 + `sap_security_restrictions` recipes that circulate
are for Bluetooth SAP, a different service. Quectel only (`AT+QNVFR`/
`AT+QNVFW`); anything else is refused before anything is written.

## 6. Repositories and packages

- **`ddimension/wwand-rsim`** (new, like wwand-ipa/wwand-qlog, not for
  upstream): `helper/` (C, CMake), `plugins/rsim.uc`, `ctl/rsim.uc`,
  `schema/uimrmt.uc`, `tests/`.
- **wwand core:** the `qmi_client` plugin dep (+ tests, reference.md).
- **Feed:** `wwand-rsim` (plugin + helper with the Phoenix backend,
  `+wwand-qmi`), and the PC/SC backend as a build variant
  `wwand-rsim-pcsc` (`+libpcsclite`, suggests `pcscd ccid`).

## 7. Order of work

1. Core `qmi_client` dep in wwand, tests.
2. `rsim-card` helper: ATR parser, T=0 engine, Phoenix backend, PC/SC backend;
   tests against a simulated card on a pty (echo, NULL bytes, ACK/~ACK,
   61xx/6Cxx, parity-free timing).
3. Plugin + schema + ctl; tests with a scripted UIMRMT modem (mock client):
   full sequence, APDU segmentation, card removal, helper crash, leaving.
4. Host end-to-end: plugin ↔ helper ↔ simulated card.
5. Hardware (needs permission): Smartmouse on a test router, the EFS switch
   on one modem, a real SIM: attach and register over the remote card.

## 8. Verification

- Wire layouts: wire-buffer tests for every message; re-checked on hardware
  by logging the raw QMUX of the first session.
- Helper: every T=0 branch with a simulated card; counterproofs for echo,
  NULL, ~INS and 6Cxx handling.
- Plugin: sequence tests, and that leaving always sends card removed +
  connection unavailable (the modem must never be left without a card).
- Hardware: registration and data on the remote SIM; unplug the reader and
  check the modem is parked (or, with `rsim_fallback local`, returns to its
  local card).

## 9. Open points

- Whether the modems accept `EVENT(connection available)` without the
  transport/usage TLVs (older IDL has none); first HW test answers it.
- 7 s ATR limit vs. a Phoenix reset with retries: the helper budgets its
  retries to stay under it.
- Which slot index the RG650E/RG502Q expect for their single slot (1 or
  "not applicable" 0).
