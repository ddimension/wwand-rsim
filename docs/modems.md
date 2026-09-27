# Modem support

wwand-rsim has two sides, and they differ a lot in what hardware they take:

- **The client** — the modem that runs on a remote card. It needs a way for
  the host to feed it a SIM. wwand-rsim implements exactly one: Qualcomm's
  **QMI UIM Remote** service (UIMRMT, QMI service 0x32), switched on with a
  Quectel EFS item. **Today that means a Quectel modem.**
- **The card source** — where the card is: a reader, a phone, another modem.
  That side is wide open (see [howto.md](howto.md)); a Huawei, MeiG or any
  modem with `AT+CSIM` can lend its card.

## Clients: supported

A client modem needs QMI UIM Remote to be *registered* by its firmware. It is
compiled into Qualcomm firmware but switched off by the EFS item
`/nv/item_files/modem/uim/remote/uim_remote_service_enable`, read at modem
boot. `wwandctl rsim MODEM enable --reset` writes it (`AT+QNVFW`, `01`),
reads it back and resets the modem; it refuses on a modem that does not
answer `AT+QNVFR` — i.e. anything but Quectel (`ctl/rsim.uc`,
`set_switch`). The plugin never writes it on its own.

| Modem | Control | Status | Evidence |
|---|---|---|---|
| Quectel RG650E-EU | QMI | **supported** — registers and carries data on a remote card | HW-tested on a MikroTik Chateau 5G, 2026-09-26/27 (README, *What works*) |
| Quectel RM520N-GL | MBIM, through wwand's QMI-over-MBIM passthrough | **supported** — card read, identity read (431 commands); the test card had no service there | HW-tested on a GL-X3000, 2026-09-26/27 |
| Quectel RG502Q | QMI | switch present (read `00`), **not tested as a client** (tested as a lender) | HW-read on a Zyxel NR7101, 2026-09-26 |
| other Quectel (Qualcomm-based) | QMI, or MBIM with the passthrough | **untested** — expected to work if the firmware carries the EFS item | — |

Not a client, by design of the current implementation: NCM modems (no QMI at
all, e.g. the MeiG SLM770A), MBIM modems without the QMI passthrough, and
Qualcomm modems of other vendors (UIM Remote may be there, but wwand-rsim has
no way to switch it on — see below).

## Clients: other vendors and approaches — tbd

**None of the rows below is implemented. All are tbd.** Each names the
mechanism, where it is documented, and what wwand-rsim would need. Sources
checked 2026-09-27.

| Vendor / method | Mechanism | Source | What wwand-rsim would need |
|---|---|---|---|
| **Qualcomm modems of other vendors** (Sierra Wireless, Telit, Fibocom, Huawei … with Qualcomm chipsets) | The same QMI UIM Remote service. The plugin's client side is vendor-neutral; only the firmware switch is written with Quectel's `AT+QNVFW`. | wwand-rsim `docs/plan.md` §1, §5 (the EFS item and its Quectel command, HW-observed); the service is a general Qualcomm feature — Android on Qualcomm phones ships a client of it, `com.qualcomm.uimremoteclient` ([APKMirror](https://www.apkmirror.com/apk/xiaomi-inc/com-qualcomm-uimremoteclient-2/), checked 2026-09-27) | A way to write that EFS item on each vendor's firmware (a vendor AT command, or an EFS write over QMI/DIAG), and a hardware test per vendor. tbd |
| **Telit** LE910Cx (and LE9x0) | SAP **client** in the module: `AT#RSEN=1,1,0,1,0` enables Remote SIM, binary SAP messages over a serial port, `#RSEN: <conn>` URC. Not supported on LE910C1-SA/SV/ST, LE910C1-EUX/SVX/SAX, LE910Cx-WWX. | [LE910Cx AT Commands Reference Guide r12, 2021-08-11, §AT#RSEN, p. 621](https://sixfab.com/wp-content/uploads/2022/02/Telit_LE910Cx_AT_Commands_Reference_Guide_r12.pdf) (checked 2026-09-27) | A SAP **server** on the router (rsim-card speaks SAP only as a client, towards phones) that serves any card source over the modem's SAP port, plus the `#RSEN` handling. Not HW-available. tbd |
| **u-blox** TOBY-L2, MPCI-L2, SARA-U2, LISA-U2 | SAP **client** in the module: `AT+USAPMODE=1,0,<beacon>`; SAP messages in binary on a dedicated USB (or MUX) channel; not while a call or PDP context is active; "u-blox cellular modules do not act as SAP server". | [u-blox cellular modules AT commands manual UBX-13002752 R67, §37 SAP](https://cdn.lantronix.com/wp-content/uploads/pdf/u-blox-CEL_ATCommands_UBX-13002752.pdf) (checked 2026-09-27) | The same SAP server as for Telit, bound to the module's SAP channel. These are LTE Cat 4 / 3G modules, rarely in current routers. tbd |
| **Sierra Wireless Legato** (on-module apps, e.g. WP series) | `le_rsim` Remote SIM service: SAP V11r00 messages between the modem service and an application that implements the SAP server; one remote card; switching local ↔ remote needs a platform reset; "has to be supported by the modem". | [Legato docs, Remote SIM service](https://docs.legato.io/latest/c_rsim.html) (checked 2026-09-27) | An application on the module itself (not the router) bridging `le_rsim` to rsim-card — a different architecture from a USB modem in a router. tbd |
| **Qualcomm TelAF** (automotive) | Remote SIM (RSIM) service, the same SAP V11r00 model as Legato. | [TelAF API reference, Remote SIM](https://docs.qualcomm.com/doc/80-41102-2/topic/page_c_tafsimRsim.html) (checked 2026-09-27) | As for Legato: an app on the platform. tbd |
| **Osmocom SIMtrace2 `cardem` / sysmoQMOD** + `osmo-remsim-client-st2` | Physical **card emulation**: a board in the modem's SIM slot implements the ISO 7816-3 electrical interface and passes the TPDUs to `osmo-remsim-client-st2` over USB; the card comes from `osmo-remsim-bankd` (PC/SC readers, e.g. sysmoOCTSIM) over RSPRO, coordinated by `osmo-remsim-server`. Works with any phone or modem electrically — no firmware support needed. | [osmo-remsim User Manual, DRAFT 1.2.0-4-g9d90, 2026-08-20, ch. 4](https://ftp.osmocom.org/docs/osmo-remsim/master/osmo-remsim-usermanual.pdf); [osmo-remsim source](https://gitea.osmocom.org/sim-card/osmo-remsim) (commit 9d90203, 2026-08-26) (checked 2026-09-27) | A client that drives the cardem hardware instead of UIM Remote — either run osmo-remsim's own stack, or a new plugin backend that feeds cardem from rsim-card sources. The only route to non-Qualcomm and non-Quectel modems without vendor support. Needs the hardware. tbd |

Other client variants of osmo-remsim, for completeness: `osmo-remsim-client-shell`
exchanges APDUs over stdio (for testing, no modem), and `libifd_remsim_client`
is a pcsc-lite IFD handler that makes a bankd card appear as a PC/SC reader.
osmo-remsim has **no** QMI UIM Remote client (no QMI code in its source at
commit 9d90203); its user manual lists only the three variants above.

Searched, **nothing found** (so not listed as a mechanism): a remote-SIM or
SAP-client command for Sierra Wireless AirPrime EM75xx (not in the
[EM75xx AT Command Reference r2](https://www.bipom.com/documents/sierra/41111748%20AirPrime%20EM75XX%20AT%20Command%20Reference%20r2.pdf),
which has only `+CRSM`/`+CSIM`), and for SIMCom, Fibocom and MeiG (web search
only; no manual with such a command found). That is absence of evidence,
not evidence of absence.

## Card sources: tbd

| Source | Mechanism | Source | What wwand-rsim would need |
|---|---|---|---|
| osmo-remsim SIM bank (`osmo-remsim-bankd`) | `libifd_remsim_client` puts a remote bankd card behind pcscd as a reader; `rsim-card-pcsc` (`pcsc:`) talks to pcscd. RSPRO is plain text with no authentication — "only … over trusted, controlled IP networks, such as inside a VPN". | [osmo-remsim User Manual, ch. 6 and §1.10](https://ftp.osmocom.org/docs/osmo-remsim/master/osmo-remsim-usermanual.pdf) (checked 2026-09-27) | Possibly nothing new: an IFD handler config on the SIM host and `pcsc:`. **Untested**, and `pcsc:` itself is not HW-tested yet. tbd |
