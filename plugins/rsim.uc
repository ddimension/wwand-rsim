// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 André Valentin <avalentin@marcant.net>
// wwand-rsim — a SIM card in a reader on the router, used by the modem as if
// it sat in the modem's own slot. A wwand plugin (plugins.uc: installed at
// /usr/share/ucode/wwand/plugins/rsim.uc).
//
// The modem side is the QMI UIM Remote service (UIMRMT, 0x32): the modem hands
// every card command to its client and takes the answer back. The card side is
// rsim-card, a small helper that owns the reader (Phoenix/Smartmouse serial or
// PC/SC) and speaks one JSON object per line on stdin/stdout. The helper is a
// separate process because a card exchange blocks for up to seconds and the
// daemon is single-threaded. docs/plan.md has the whole design.
//
// Exportless plain script: require() returns the plugin object wwand expects
// (name, options, create), plus the pure pieces the tests reach.

'use strict';

import * as uloop from 'uloop';
import * as fs from 'fs';

const HELPER = '/usr/lib/wwand/rsim-card';

// A reader on another machine: `ssh:<user>@<host>:<reader>`. The helper runs
// there and its lines travel over SSH, so nothing but the command line
// changes (docs/plan.md §3.3). The router's key lives here; `wwandctl rsim
// ssh-key` creates it.
const SSH_KEY_DIR = '/etc/wwand/rsim';
const LOCAL_READER = /^((phoenix|pcsc):.|wbsm:)/;

// 'ssh:user@host:reader' -> { dest, reader }, or null
function ssh_split(r)
{
	let m = match(r, /^ssh:([A-Za-z0-9._-]+@[A-Za-z0-9._-]+):((phoenix|pcsc|wbsm):.*)$/);

	return (m && match(m[2], LOCAL_READER)) ? { dest: m[1], reader: m[2] } : null;
}

// one word for the remote shell, whatever it contains (a PC/SC reader name
// has spaces)
function shq(s)
{
	return "'" + replace(s, /'/g, "'\\''") + "'";
}

// which ssh this box has: OpenWrt's /usr/bin/ssh is usually dropbear's
// dbclient, whose options differ from OpenSSH's (no -o; -y instead of
// StrictHostKeyChecking, -K for keepalives)
function ssh_flavor()
{
	let l = fs.readlink('/usr/bin/ssh');

	if ((l && index(l, 'dbclient') >= 0) || (l && index(l, 'dropbear') >= 0))
		return 'dropbear';

	return fs.access('/usr/bin/ssh') ? 'openssh' : (fs.access('/usr/bin/dbclient') ? 'dropbear' : null);
}

// QMI UIM Remote, IDL 1.x. Message and TLV ids are interface facts, checked
// against the one public client implementation that runs on real modems
// (SIMComHub softsim, src/qmi_remotesim.c) and against the service's IDL
// tables; they must be re-verified on the wire on the first hardware run
// (docs/plan.md §8). Enumerations are 4 bytes on the wire, except the APDU
// status, which is QMI's 2-byte result type.
const UIMRMT = {
	service: 0x32,
	messages: {
		RESET: { id: 0x0020, req: {}, resp: {} },
		EVENT: { id: 0x0021, req: {
			info:        { t: 0x01, f: { event: 'u32', slot: 'u32' } },
			atr:         { t: 0x10, f: { n: 'u8', of: 'u8' } },
			wakeup:      { t: 0x11, f: 'u8' },
			error_cause: { t: 0x12, f: 'u32' },
		}, resp: {} },
		APDU: { id: 0x0022, req: {
			status:   { t: 0x01, f: 'u16' },
			slot:     { t: 0x02, f: 'u32' },
			apdu_id:  { t: 0x03, f: 'u32' },
			info:     { t: 0x10, f: { total: 'u32', offset: 'u32' } },
			response: { t: 0x11, f: { n: 'u16', of: 'u8' } },
		}, resp: {} },
		// the indication shares 0x0022 with the request; client.uc keys
		// indications by message, so it needs its own entry
		APDU_IND: { id: 0x0022, ind: {
			slot:    { t: 0x01, f: 'u32' },
			apdu_id: { t: 0x02, f: 'u32' },
			command: { t: 0x03, f: { n: 'u16', of: 'u8' } },
		} },
		CONNECT_IND:         { id: 0x0023, ind: { slot: { t: 0x01, f: 'u32' } } },
		DISCONNECT_IND:      { id: 0x0024, ind: { slot: { t: 0x01, f: 'u32' } } },
		CARD_POWER_UP_IND:   { id: 0x0025, ind: { slot: { t: 0x01, f: 'u32' },
		                                          timeout: { t: 0x10, f: 'u32' },
		                                          voltage: { t: 0x11, f: 'u32' } } },
		CARD_POWER_DOWN_IND: { id: 0x0026, ind: { slot: { t: 0x01, f: 'u32' },
		                                          mode: { t: 0x10, f: 'u32' } } },
		CARD_RESET_IND:      { id: 0x0027, ind: { slot: { t: 0x01, f: 'u32' } } },
	},
};

// The UIM service describing itself, for `wwandctl rsim probe`: which of its
// messages this firmware has, and which TLVs each takes. GET_SUPPORTED_MSGS
// answers a bitmask, bit n = message id n (libqmi 1.38.0
// qmi-service-uim.json "Get Supported Messages", TLV 0x10, u16 length).
// GET_SUPPORTED_FIELDS is not in libqmi; its answer is kept raw (bytes per
// TLV) and read on the hardware.
const UIM_PROBE = {
	service: 0x0B,
	messages: {
		GET_SUPPORTED_MSGS:   { id: 0x001E, req: {}, resp: { list: { t: 0x10, f: { n: 'u16', of: 'u8' } } } },
		GET_SUPPORTED_FIELDS: { id: 0x001F, req: { msg: { t: 0x01, f: 'u16' } },
		                        resp: { req_f: { t: 0x10, f: 'bytes' }, resp_f: { t: 0x11, f: 'bytes' },
		                                ind_f: { t: 0x12, f: 'bytes' } } },
	},
};

// A modem lending its own card: the SIM Access Profile server side of the
// UIM service. Layout from Qualcomm's Gobi API (BSD-3, shipped in the libqmi
// 1.38.0 tarball, gobi-api/GobiAPI_2013-07-31-1347:
// GobiConnectionMgmtAPIStructs.h ~24450, Enums.h, GobiConnectionMgmtAPI.h
// :13173/:13202) — libqmi itself implements none of it. Every enum is ONE
// byte there (`: UINT8`), unlike UIM Remote's 4-byte IDL enums.
const UIM_SAP = {
	service: 0x0B,
	messages: {
		SAP_CONNECTION: { id: 0x003C, req: {
			conn: { t: 0x01, f: { op: 'u8', slot: 'u8' } },   // 0 disconnect, 1 connect, 2 status
			mode: { t: 0x10, f: 'u8' },                       // 0 immediate, 1 graceful
			igr:  { t: 0x11, f: 'u8' },                       // intermediate GET RESPONSE
			cond: { t: 0x12, f: 'u8' },                       // 3 = allow always
		}, resp: { state: { t: 0x10, f: 'u8' } } },
		SAP_REQUEST: { id: 0x003D, req: {
			req:  { t: 0x01, f: { op: 'u8', slot: 'u8' } },   // 0 ATR, 1 APDU, 2 off, 3 on, 4 reset, 5 reader status
			apdu: { t: 0x10, f: { n: 'u16', of: 'u8' } },
		}, resp: {
			atr:    { t: 0x10, f: { n: 'u8', of: 'u8' } },
			rapdu:  { t: 0x11, f: { n: 'u16', of: 'u8' } },
			reader: { t: 0x12, f: { n: 'u8', of: 'u8' } },
		} },
		SAP_CONNECTION_IND: { id: 0x003E, ind: { st: { t: 0x10, f: { state: 'u8', slot: 'u8' } } } },
		REGISTER_EVENTS: { id: 0x002E, req: { mask: { t: 0x01, f: 'u32' } }, resp: {} },
	},
};
// For the APDU donor mode: the card's ATR and plain APDUs through the donor's
// UIM service. GET_ATR 0x0041 from the Gobi API (Structs.h:17082-17095:
// request TLV 0x01 slot, response TLV 0x10 u8-length ATR); SEND_APDU 0x003B as
// wwand's own codec/schema/uim.uc has it (libqmi 1.38: u16-prefixed APDUs).
const UIM_APDU = {
	service: 0x0B,
	messages: {
		GET_ATR:   { id: 0x0041, req: { slot: { t: 0x01, f: 'u8' } }, resp: { atr: { t: 0x10, f: { n: 'u8', of: 'u8' } } } },
		SEND_APDU: { id: 0x003B, req: { slot: { t: 0x01, f: 'u8' }, apdu: { t: 0x02, f: { n: 'u16', of: 'u8' } } },
		             resp: { response: { t: 0x10, f: { n: 'u16', of: 'u8' } },
		                     long_response: { t: 0x11, f: { total_length: 'u16', token: 'u32' } } } },
	},
};

const SAP_STATES = [ 'not enabled', 'connecting', 'connected', 'connection error', 'disconnecting', 'disconnected' ];

// AT+CSIM (3GPP TS 27.007 §8.17): `AT+CSIM=<length>,"<command>"` with the
// length in hex CHARACTERS, answered `+CSIM: <length>,"<response>"`. The
// generic path to a donor's card when it has no QMI UIM (NCM, MBIM without
// passthrough) or refuses SEND_APDU.
function csim_cmd(apdu_hex)
{
	return sprintf('AT+CSIM=%d,"%s"', length(apdu_hex), apdu_hex);
}

function csim_answer(lines)
{
	for (let l in (lines ?? [])) {
		let m = match(l, /^\+CSIM:\s*(\d+)\s*,\s*"?([0-9A-Fa-f]*)"?\s*$/);

		if (m && length(m[2]) == +m[1])
			return uc(m[2]);
	}

	return null;
}

// Over plain AT there is no standard way to read the ATR. The target modem
// needs one to know the card speaks T=0; TS 3B (direct convention) with T0 00
// (no interface bytes, no historical bytes) says exactly that and nothing
// else (ISO/IEC 7816-3:2006 §8.2).
const ATR_T0_MINIMAL = '3B00';

// bit n set in a byte array -> [ n, ... ]
function bits_of(bytes)
{
	let out = [];

	for (let i = 0; i < length(bytes ?? []); i++)
		for (let b = 0; b < 8; b++)
			if (bytes[i] & (1 << b))
				push(out, i * 8 + b);

	return out;
}

const EV_CONN_UNAVAILABLE = 0, EV_CONN_AVAILABLE = 1, EV_CARD_INSERTED = 2,
      EV_CARD_REMOVED = 3, EV_CARD_ERROR = 4, EV_CARD_RESET = 5;
const ERR_UNKNOWN = 0, ERR_NO_LINK = 1, ERR_TIMEOUT = 2;

// the largest response segment the IDL accepts; a T=0 response (256 + SW) is
// always one, the segmenting is there for correctness, not for practice
const SEG_MAX = 1024;

// after a failed bring-up; doubles up to the cap
const BACKOFF_MIN = 10, BACKOFF_MAX = 300;

// a helper answer that does not come: power-up includes the reset retries of
// a Phoenix reader, a TPDU the card's work waiting time
const HELPER_TIMEOUT_MS = 15000;

function hexs(a)
{
	let s = '';

	for (let b in (a ?? []))
		s += sprintf('%02X', b);

	return s;
}

function bytes(h)
{
	let out = [];

	if (type(h) != 'string' || length(h) % 2 || match(h, /[^0-9A-Fa-f]/))
		return null;

	for (let i = 0; i < length(h); i += 2)
		push(out, hex(substr(h, i, 2)));

	return out;
}

// The typed plugin options. `reader` is `phoenix:/dev/ttyUSB0`, `wbsm:`
// (WB Electronics Smartmouse USB, optionally `wbsm:<USB serial>`: the helper
// sets its clock and mode and finds its tty) or `pcsc:<name or index>`; null
// means the modem has no remote card.
function cfg_of(ext)
{
	let r = ext?.rsim_reader;

	if (type(r) != 'string')
		return null;

	let remote = null;
	let donor = null;

	if (substr(r, 0, 6) == 'modem:') {
		donor = substr(r, 6);

		if (!match(donor, /^[A-Za-z0-9_]+$/))
			return null;
	}
	else if (substr(r, 0, 4) == 'ssh:') {
		remote = ssh_split(r);

		if (!remote)
			return null;
	}
	else if (!match(r, LOCAL_READER))
		return null;

	let slot = +(ext.rsim_slot ?? 1);

	return {
		reader: r,
		local_reader: remote?.reader ?? r,
		ssh: remote ? {
			dest: remote.dest,
			port: ext.rsim_ssh_port ?? null,
			key: ext.rsim_ssh_key ?? null,
			helper: ext.rsim_ssh_helper ?? 'rsim-card',
		} : null,
		// 1..3: the service decodes anything else to "not applicable", and
		// its event handler serves only real slots
		slot: (slot >= 1 && slot <= 3) ? slot : 1,
		clock: ext.rsim_clock ?? null,
		reset: ext.rsim_reset ?? null,
		detect: ext.rsim_detect ?? null,
		mode: ext.rsim_mode ?? null,
		// another wwand modem lending its card (docs/plan.md §3.5)
		donor: donor ? {
			ref: donor,
			mode: (ext.rsim_donor_mode == 'apdu') ? 'apdu' : 'sap',
			slot: (+(ext.rsim_donor_slot ?? 1) >= 1 && +(ext.rsim_donor_slot ?? 1) <= 5) ? +(ext.rsim_donor_slot ?? 1) : 1,
			cond: ext.rsim_donor_cond ?? null,
			apdu: (index([ 'qmi', 'at' ], ext.rsim_donor_apdu) >= 0) ? ext.rsim_donor_apdu : 'auto',
		} : null,
	};
}

// the helper's argv for a configuration; `sys` (tests) overrides the ssh
// flavour and which key files exist
function helper_argv(cfg, path, sys)
{
	let reader = cfg.local_reader ?? cfg.reader;
	let argv = [ cfg.ssh ? (cfg.ssh.helper ?? 'rsim-card') : (path ?? HELPER), reader ];

	let wbsm = (substr(reader, 0, 5) == 'wbsm:');

	if (wbsm || substr(reader, 0, 8) == 'phoenix:') {
		if (cfg.clock != null)
			push(argv, '--clock', sprintf('%d', +cfg.clock));
		if (cfg.reset != null)
			push(argv, '--reset', cfg.reset);
		if (cfg.detect != null)
			push(argv, '--detect', cfg.detect);
	}

	if (wbsm && cfg.mode != null)
		push(argv, '--wbsm-mode', cfg.mode);

	if (!cfg.ssh)
		return argv;

	let flavor = sys?.flavor ?? ssh_flavor();
	let exists = sys?.exists ?? ((p) => fs.access(p));
	let key = cfg.ssh.key ?? sprintf('%s/%s', SSH_KEY_DIR, (flavor == 'dropbear') ? 'id_dropbear' : 'id_ed25519');
	let cmd = join(' ', map(argv, shq));

	// -T: no terminal, the lines must pass unchanged. Host keys are
	// accepted on first use and checked after that. Keepalives, so a dead
	// link ends the session (a helper exit to the plugin) instead of
	// leaving the modem waiting on a card that is gone.
	let ssh = (flavor == 'dropbear')
		? [ '/usr/bin/ssh', '-T', '-y', '-K', '15' ]
		: [ '/usr/bin/ssh', '-T', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new',
		    '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=3' ];

	if (cfg.ssh.port != null)
		push(ssh, '-p', sprintf('%d', +cfg.ssh.port));

	if (cfg.ssh.key != null || exists(key))
		push(ssh, '-i', key);

	push(ssh, cfg.ssh.dest, cmd);

	return ssh;
}

// Split a card response into APDU requests' segment fields.
function segments(resp)
{
	let out = [];
	let total = length(resp);

	for (let off = 0; off < total || (!total && !length(out)); off += SEG_MAX) {
		push(out, { info: { total: total, offset: off },
		            response: slice(resp, off, off + SEG_MAX) });

		if (!total)
			break;
	}

	return out;
}

// The production helper channel: rsim-card spawned with pipes (wwand_io
// spawn, the same primitive esim_bridge uses for lpac), stdout read line by
// line from uloop. open(argv, on_line, on_exit) -> { write(line), close() }.
function spawn_helper(argv, on_line, on_exit)
{
	let io = require('wwand_io');
	let h = io.spawn(argv);

	if (!h)
		return null;

	let buf = '', uh = null, wq = '', wtimer = null, gone = false;

	let finish = () => {
		if (gone)
			return;

		gone = true;
		wtimer?.cancel();
		uh?.delete();
		h.close();
		on_exit();
	};

	let pump;
	pump = () => {
		wtimer = null;

		while (length(wq)) {
			let n = h.write(wq);

			if (n === false)
				return finish();

			if (n === null || n === 0)
				return (wtimer = uloop.timer(20, pump));

			wq = substr(wq, n);
		}
	};

	uh = uloop.handle(h.fileno(), () => {
		for (;;) {
			let chunk = h.read();

			if (chunk === false)
				return finish();

			if (chunk == null || chunk === '')
				break;

			buf += chunk;
		}

		let i;

		while ((i = index(buf, '\n')) >= 0) {
			let line = substr(buf, 0, i);

			buf = substr(buf, i + 1);

			if (length(line))
				on_line(line);
		}
	}, uloop.ULOOP_READ);

	return {
		write: (line) => {
			if (gone)
				return;

			wq += line;

			if (!wtimer)
				pump();
		},
		// EOF on its stdin makes rsim-card power the card down and exit; the
		// kill is for one that does not
		close: () => {
			if (gone)
				return;

			let hh = h;

			uh?.delete();
			uh = null;
			gone = true;
			wtimer?.cancel();
			uloop.timer(2000, () => { hh.kill(); hh.close(); });
		},
	};
}

// One JSON request at a time to the helper, answers matched in order; lines
// with "event" are the helper's own news (card removed/inserted).
// The card of ANOTHER wwand modem on this router, as the same card channel
// the helper gives: call({op}, cb(err, msg)) with power_up / reset /
// power_down / tpdu, and close(). Two ways to borrow it (docs/plan.md §3.5):
//
// - 'sap': the donor's UIM service lends its card through the SIM Access
//   Profile server (SAP_CONNECTION connect, then SAP_REQUEST for ATR, APDU,
//   power and reset). The donor stops using the card itself for as long as
//   the link stands — its own connection goes down, which is the point:
//   one card must not be registered by two modems.
// - 'apdu': the card stays with the donor, which must have its radio off;
//   APDUs go through its UIM SEND_APDU on the basic channel, the ATR is the
//   one it reports, and power/reset cannot really be done — they answer
//   with that ATR. A fallback for firmware without SAP.
//
// Requests are served one at a time, in order, like the helper does.
function donor_card(deps, donor, dcfg, on_event, on_exit, log)
{
	let sap = (dcfg.mode != 'apdu');
	let slot = dcfg.slot;
	// the APDU path: QMI UIM SEND_APDU, or AT+CSIM; 'auto' starts with QMI
	// and moves to AT when the donor has no UIM client or refuses the command
	let via = (dcfg.apdu == 'at') ? 'at' : 'qmi';
	let at_only = (dcfg.apdu == 'at');
	let c = null, dead = false, ready = false, busy = false, atr = null;
	let queue = [];
	let poll = null;

	let reply = (r, err, msg) => r.cb(err, msg);

	let fail_all = (why) => {
		let q = queue;

		queue = [];
		for (let r in q)
			reply(r, { error: why }, null);
	};

	// set once a SAP connect has been SENT: from then on the donor may have
	// handed its card over whether or not it said so
	let connect_sent = false;

	// Give the client back — and, if a SAP connect went out, end the link
	// first, on EVERY path. A connect that timed out may still have been
	// carried out: a Huawei E392 took its card away (it lost registration the
	// moment the connect arrived) and never answered; the client was then
	// released with the link still standing, the card stayed lent to nobody,
	// and every later retry piled another connect on top until the modem's
	// UIM and all its AT ports hung (HW-observed on 245, 2026-09-26; only a
	// modem reset cleared it). Releasing the CID does not end the link.
	// `graceful` for an orderly hand-back; immediate for a failure, where the
	// donor must get its card back now.
	let release = (graceful) => {
		poll?.cancel();
		poll = null;

		if (!c)
			return;

		let cl = c;

		c = null;

		if (!sap || !connect_sent)
			return deps.qmi_release(donor, cl);

		cl.request('SAP_CONNECTION', { conn: { op: 0, slot: slot }, mode: graceful ? 1 : 0 }, () =>
			deps.qmi_release(donor, cl), { no_recovery: true, timeout: 5000 });
	};

	// the session hears the reason first (and whether to hold off retrying),
	// before the waiting requests fail with it — otherwise a pending
	// power-up's generic "no card" would win the race to the status
	let finish = (why, hold) => {
		if (dead)
			return;

		dead = true;
		release(false);
		on_exit(why, hold);
		fail_all(why);
	};

	let hexb = (a) => hexs(a ?? []);
	let q_opts = { no_recovery: true, timeout: 10000 };

	// Every SAP continuation checks `c`: an answer can arrive after finish()
	// has released the client, and a member call on null throws inside a
	// uloop callback — which ends the daemon, not just this session.
	let read_atr = (cb) => {
		if (sap && !c)
			return cb({ error: 'closed' }, null);

		if (sap)
			return c.request('SAP_REQUEST', { req: { op: 0, slot: slot } }, (e, d) =>
				cb(e ? { error: 'atr', detail: e } : null, e ? null : hexb(d.atr)), q_opts);

		if (!c)
			return cb(null, ATR_T0_MINIMAL);

		c.request('GET_ATR', { slot: slot }, (e, d) =>
			cb(null, (e || !length(d?.atr ?? [])) ? ATR_T0_MINIMAL : hexb(d.atr)), q_opts);
	};

	// Through the core's modem_at: the donor's AT channel whichever it is, a
	// tty or AT carried inside MBIM. A core without that dep: the modem's own
	// `at`, which is only there once wwand has opened a tty for it.
	let at_apdu = (hex, done) => {
		let answer = (e, res) => {
			let r = e ? null : csim_answer(res?.lines);

			(r == null || length(r) < 4)
				? done({ error: 'io', detail: e ?? 'no +CSIM answer' }, null)
				: done(null, { ok: true, data: r });
		};

		if (deps.modem_at)
			return deps.modem_at(donor, csim_cmd(hex), answer, 10000);

		let at = deps.modem_of?.(donor)?.modem?.at;

		if (!at)
			return done({ error: 'io', detail: 'the donor has no AT channel' }, null);

		at.send(csim_cmd(hex), answer, { timeout: 10000 });
	};

	let serve = (r, done) => {
		let op = r.req.op;

		if (op == 'status')
			return done(null, { ok: true, present: ready, powered: ready, backend: 'donor-' + (sap ? 'sap' : 'apdu'),
			                    reader: donor, atr: atr });

		if (op == 'power_up' || op == 'reset') {
			let after = (e) => read_atr((ae, a) => {
				if (ae || !length(a ?? ''))
					return done(ae ?? { error: 'no_card' }, null);
				atr = a;
				done(null, { ok: true, atr: a });
			});

			if (!sap)
				return after(null);

			if (!c)
				return done({ error: 'closed' }, null);

			// power on is refused for a card that is already on: harmless
			return c.request('SAP_REQUEST', { req: { op: (op == 'reset') ? 4 : 3, slot: slot } },
				() => after(null), q_opts);
		}

		if (op == 'power_down') {
			if (!sap)
				return done(null, { ok: true });

			if (!c)
				return done({ error: 'closed' }, null);

			return c.request('SAP_REQUEST', { req: { op: 2, slot: slot } }, () => done(null, { ok: true }), q_opts);
		}

		if (op == 'tpdu') {
			let apdu = bytes(r.req.data);

			if (apdu == null || length(apdu) < 4)
				return done({ error: 'bad_request' }, null);

			if (sap && !c)
				return done({ error: 'closed' }, null);

			if (sap)
				return c.request('SAP_REQUEST', { req: { op: 1, slot: slot }, apdu: apdu }, (e, d) =>
					(e || length(d.rapdu ?? []) < 2)
						? done({ error: 'io', detail: e ?? 'no response APDU' }, null)
						: done(null, { ok: true, data: hexb(d.rapdu) }), q_opts);

			if (via == 'at' || !c)
				return at_apdu(uc(r.req.data), done);

			return c.request('SEND_APDU', { slot: slot, apdu: apdu }, (e, d) => {
				// a donor that does not take APDUs this way: the AT channel,
				// from now on (auto only; an explicit 'qmi' reports the error).
				// 71 INVALID_QMI_COMMAND, 82 ACCESS_DENIED, 94 NOT_SUPPORTED
				// (libqmi 1.38.0 qmi-errors.h:298/309/317)
				if (e?.error == 'qmi' && dcfg.apdu != 'qmi' && (e.code == 71 || e.code == 82 || e.code == 94)) {
					log('notice', sprintf('rsim: %s refuses SEND_APDU (QMI error %d) — using AT+CSIM', donor, e.code));
					via = 'at';
					return at_apdu(uc(r.req.data), done);
				}

				(e || length(d.response ?? []) < 2)
					? done({ error: 'io', detail: e ?? (d?.long_response ? 'response too long' : 'no response') }, null)
					: done(null, { ok: true, data: hexb(d.response) });
			}, q_opts);
		}

		done({ error: 'bad_request' }, null);
	};

	let next;
	next = () => {
		if (dead || !ready || busy || !length(queue))
			return;

		let r = shift(queue);

		busy = true;
		serve(r, (err, msg) => {
			busy = false;
			reply(r, err, msg);
			next();
		});
	};

	let up = () => {
		ready = true;
		log('notice', sprintf('rsim: %s lends its card (%s, slot %d)', donor,
			sap ? 'SIM Access Profile' : sprintf('APDU over %s, radio off', (via == 'at') ? 'AT+CSIM' : 'QMI UIM'), slot));
		next();
	};

	let schema = { service: 0x0B, messages: sap ? UIM_SAP.messages : UIM_APDU.messages };

	// AT only: no QMI client at all
	if (!sap && at_only) {
		uloop.timer(0, () => dead ? null : up());

		return {
			call: (req, cb) => { if (dead) return cb({ error: 'helper_exit' }, null); push(queue, { req: req, cb: cb }); next(); },
			close: () => { if (!dead) { dead = true; fail_all('closed'); } },
		};
	}

	deps.qmi_client(donor, schema, (err, cl) => {
		if (dead) {
			if (cl)
				deps.qmi_release(donor, cl);
			return;
		}

		// APDU mode without QMI UIM on the donor (NCM, MBIM without the
		// passthrough): the AT channel carries everything
		if (err && !sap && dcfg.apdu != 'qmi') {
			log('notice', sprintf('rsim: %s has no QMI UIM client (%s) — using AT+CSIM', donor, err.error ?? '?'));
			via = 'at';
			return up();
		}

		if (err)
			return finish(sprintf('no UIM client on the donor %s (%s)', donor, err.error ?? '?'));

		c = cl;

		if (!sap)
			return up();

		// the link's own news: lost while in use means the card is gone
		c.on('SAP_CONNECTION_IND', (d) => {
			if (+(d?.st?.slot ?? slot) != slot)
				return;

			let st = +(d?.st?.state ?? -1);

			if (!ready && st == 2) {
				poll?.cancel();
				poll = null;
				return up();
			}

			if (ready && (st == 3 || st == 5))
				finish(sprintf('the donor %s ended the SIM Access link (%s)', donor, SAP_STATES[st]));
		});

		c.request('REGISTER_EVENTS', { mask: 0x2 }, () => {
			// torn down while the registration was on its way
			if (dead || !c)
				return;

			// cond 3: lend it even while the donor has a call or data
			// session — taking it over is what was configured
			let conn = { conn: { op: 1, slot: slot } };

			// TLV 0x12 first appears in the 2013 Gobi drop; older firmware may
			// not know it, so it can be left out (rsim_donor_cond 'none')
			if (dcfg.cond != 'none')
				conn.cond = +(dcfg.cond ?? 3);

			connect_sent = true;
			c.request('SAP_CONNECTION', conn, (e) => {
				// No answer at all is the dangerous one: the donor may have
				// handed its card over anyway (release() ends the link). Not
				// retried automatically — each retry costs the donor its
				// registration — until the configuration changes or
				// `wwandctl rsim restart`.
				if (e?.error == 'timeout')
					return finish(sprintf('the donor %s does not answer the SIM Access connect — use rsim_donor_mode apdu', donor), true);

				if (e)
					return finish(sprintf('the donor %s refused the SIM Access link (%J)', donor, e));

				// the indication may not come (not registered on every
				// firmware): poll the state as well, bounded
				let tries = 0;
				let check;

				check = () => {
					poll = null;
					if (dead || ready || !c)
						return;

					c.request('SAP_CONNECTION', { conn: { op: 2, slot: slot } }, (se, sd) => {
						if (dead || ready)
							return;
						if (!se && sd.state == 2)
							return up();
						if (++tries >= 20)
							return finish(sprintf('the donor %s did not connect the SIM Access link (state %s)',
								donor, se ? 'unknown' : (SAP_STATES[sd.state] ?? sd.state)));
						poll = uloop.timer(500, check);
					}, q_opts);
				};
				poll = uloop.timer(300, check);
			}, q_opts);
		}, q_opts);
	});

	return {
		call: (req, cb) => {
			if (dead)
				return cb({ error: 'helper_exit' }, null);

			push(queue, { req: req, cb: cb });
			next();
		},
		// hand the card back: graceful disconnect, then the client
		close: () => {
			if (dead)
				return;

			dead = true;
			fail_all('closed');

			release(ready);
		},
	};
}

function helper_rpc(open, argv, on_event, on_exit, log)
{
	let queue = [], busy = null, timer = null, dead = false;
	let ch;

	let fail_all = (err) => {
		let q = busy ? [ busy, ...queue ] : queue;

		busy = null;
		queue = [];
		timer?.cancel();
		timer = null;

		for (let r in q)
			r.cb({ error: err }, null);
	};

	let next;
	next = () => {
		if (busy || !length(queue) || dead)
			return;

		busy = shift(queue);
		timer = uloop.timer(busy.timeout ?? HELPER_TIMEOUT_MS, () => {
			timer = null;
			log('warn', sprintf('rsim: card reader did not answer %s — restarting it', busy?.req?.op));
			dead = true;
			fail_all('timeout');
			ch?.close();
			on_exit('timeout');
		});
		ch.write(sprintf('%J\n', busy.req));
	};

	ch = open(argv, (line) => {
		let msg = json(line);

		if (type(msg) != 'object')
			return log('debug', sprintf('rsim: helper said: %s', line));

		if (msg.event != null)
			return on_event(msg);

		if (!busy)
			return log('debug', sprintf('rsim: unexpected helper answer: %s', line));

		let r = busy;

		busy = null;
		timer?.cancel();
		timer = null;
		r.cb(msg.ok ? null : { error: msg.error ?? 'helper', detail: msg.detail }, msg);
		next();
	}, () => {
		if (dead)
			return;

		dead = true;
		fail_all('helper_exit');
		on_exit('exit');
	});

	if (!ch)
		return null;

	return {
		call: (req, cb, timeout) => {
			if (dead)
				return cb({ error: 'helper_exit' }, null);

			push(queue, { req: req, cb: cb, timeout: timeout });
			next();
		},
		close: () => {
			if (dead)
				return;

			dead = true;
			fail_all('closed');
			ch.close();
		},
	};
}

// create(deps): deps from the daemon (plugins.uc) — qmi_client, qmi_release,
// log — plus, for tests, open_helper (the helper channel) and now.
function create(deps)
{
	let log = deps.log ?? ((l, m) => null);
	let open = deps.open_helper ?? spawn_helper;
	let now = deps.now ?? (() => time());
	let helper_path = deps.helper_path ?? HELPER;

	// per modem: the session with the modem and the card
	let sessions = {};
	// per modem: the session whose teardown is still in flight. Its
	// card-removed / connection-unavailable reach the modem asynchronously,
	// so a new session started meanwhile would be withdrawn by the old one's
	// connection-unavailable arriving AFTER the new offer.
	let stopping = {};
	// per modem: what the status shows after a session is gone
	let notes = {};

	let stop_session;

	let new_session = (ref, cfg) => ({
		ref: ref, cfg: cfg, key: sprintf('%J', cfg),
		state: 'starting',     // starting | waiting | connected | powered | failed
		rpc: null, client: null, atr: null, card_gen: 0,
		apdus: 0, last_sw: null, last_error: null, since: now(),
		queue: [], busy: false,
	});

	let note = (ref, patch) => {
		notes[ref] = { ...(notes[ref] ?? {}), ...patch };
	};

	let fail = (s, why, hold) => {
		s.last_error = why;
		note(s.ref, { last_error: why, failures: +(notes[s.ref]?.failures ?? 0) + 1 });

		// a failure that retrying makes worse: wait for the operator
		if (hold) {
			note(s.ref, { retry_at: null, hold: true });
			log('warn', sprintf('rsim %s: %s — not retrying until the configuration changes or `wwandctl rsim restart`', s.ref, why));
			return stop_session(s, false);
		}

		let f = notes[s.ref].failures;
		let wait = BACKOFF_MIN;

		for (let i = 1; i < f && wait < BACKOFF_MAX; i++)
			wait *= 2;

		note(s.ref, { retry_at: now() + ((wait > BACKOFF_MAX) ? BACKOFF_MAX : wait) });
		log('warn', sprintf('rsim %s: %s — trying again in %d s', s.ref, why,
			notes[s.ref].retry_at - now()));
		stop_session(s, false);
	};

	let event = (s, ev, extra, cb) => {
		if (!s.client || s.client.destroyed)
			return cb ? cb({ error: 'no_client' }) : null;

		s.client.request('EVENT', { info: { event: ev, slot: s.cfg.slot }, ...(extra ?? {}) },
			(err) => {
				if (err)
					log('warn', sprintf('rsim %s: event %d refused by the modem: %J', s.ref, ev, err));
				if (cb)
					cb(err);
			}, { no_recovery: true });
	};

	// the card's ATR to the modem, after it (re)powered or reset the card
	// Every card operation the modem asks for supersedes the ones before it:
	// a power-up still on its way when a power-down (or a disconnect) comes
	// in must not answer afterwards with an ATR the modem no longer wants.
	// The helper runs them in order; `card_gen` decides whose answer counts.
	let card_up = (s, op) => {
		let gen = ++s.card_gen;

		s.rpc.call({ op: op }, (err, res) => {
			if (s.state == 'failed' || s.card_gen != gen)
				return;

			if (err || !bytes(res?.atr)) {
				log('warn', sprintf('rsim %s: card %s failed: %s', s.ref, op, err?.error ?? 'no ATR'));
				return event(s, EV_CARD_ERROR, { error_cause: (err?.error == 'timeout') ? ERR_TIMEOUT : ERR_NO_LINK });
			}

			s.atr = res.atr;
			s.state = 'powered';
			log('notice', sprintf('rsim %s: card %s, ATR %s', s.ref,
				(op == 'reset') ? 'reset' : 'powered up', s.atr));
			event(s, EV_CARD_RESET, { atr: bytes(s.atr) });
		});
	};

	// APDUs go to the card one at a time, in the order the modem sent them
	let pump_apdu;
	pump_apdu = (s) => {
		if (s.busy || !length(s.queue) || s.state == 'failed' || !s.rpc)
			return;

		let a = shift(s.queue);
		let t0 = clock(true);
		let elapsed_ms = () => {
			let t1 = clock(true);

			return int((t1[0] - t0[0]) * 1000 + (t1[1] - t0[1]) / 1000000);
		};

		// an answer that comes back after the modem powered the card down or
		// disconnected belongs to a card session that no longer exists
		let gen = s.card_gen;

		s.busy = true;
		s.rpc.call({ op: 'tpdu', data: hexs(a.command) }, (err, res) => {
			s.busy = false;

			if (s.state == 'failed')
				return;

			if (s.card_gen != gen)
				return pump_apdu(s);

			let resp = err ? null : bytes(res?.data);
			let ms = elapsed_ms();

			if (resp == null || length(resp) < 2) {
				log('warn', sprintf('rsim %s: apdu %d (INS %02X) failed on the card: %s', s.ref,
					a.apdu_id, a.command[1] ?? 0, err?.error ?? 'short response'));
				s.client?.request('APDU', { status: 1, slot: s.cfg.slot, apdu_id: a.apdu_id },
					() => null, { no_recovery: true });
				event(s, EV_CARD_ERROR, { error_cause: (err?.error == 'timeout') ? ERR_TIMEOUT : ERR_UNKNOWN });
				return pump_apdu(s);
			}

			s.apdus++;
			s.last_sw = sprintf('%02X%02X', resp[length(resp) - 2], resp[length(resp) - 1]);
			log('debug', sprintf('rsim %s: apdu %d INS %02X -> SW %s, %d bytes, %d ms', s.ref,
				a.apdu_id, a.command[1] ?? 0, s.last_sw, length(resp) - 2, ms));

			for (let seg in segments(resp))
				s.client?.request('APDU', { status: 0, slot: s.cfg.slot, apdu_id: a.apdu_id, ...seg },
					(e) => e ? log('warn', sprintf('rsim %s: modem refused apdu %d answer: %J', s.ref, a.apdu_id, e)) : null,
					{ no_recovery: true });

			pump_apdu(s);
		});
	};

	let wire = (s) => {
		let c = s.client;
		// A session that has ended ignores its client: the modem still
		// indicates on it until the release completes (a power-down answers
		// our connection-unavailable), and the helper is already gone. An
		// exception in an indication handler ends the whole daemon
		// (HW-observed on 245, 2026-09-26: `rsim restart` killed wwand).
		let mine = (d) => (s.state != 'failed' && s.rpc != null && +(d?.slot ?? -1) == s.cfg.slot);

		c.on('CONNECT_IND', (d) => {
			if (!mine(d))
				return;

			s.state = 'connected';
			log('notice', sprintf('rsim %s: the modem connected to the remote card (slot %d)', s.ref, s.cfg.slot));
			card_up(s, 'power_up');

			// the modem now works on another card: wwand's card-change
			// process (the one a slot switch runs) forgets the local card —
			// identity, per-SIM override, eSIM caches — and re-reads this one
			if (!s.swapped) {
				s.swapped = true;
				deps.sim_changed?.(s.ref, 'remote SIM in use');
			}
		});
		c.on('DISCONNECT_IND', (d) => {
			if (!mine(d))
				return;

			// the card as well, not only our state: a Phoenix card would stay
			// released from reset and a PC/SC card held exclusively
			s.card_gen++;
			s.queue = [];
			s.rpc.call({ op: 'power_down' }, (err) => err
				? log('warn', sprintf('rsim %s: powering the card down failed: %s', s.ref, err.error ?? '?'))
				: null);
			s.state = 'waiting';
			log('notice', sprintf('rsim %s: the modem disconnected from the remote card', s.ref));
		});
		c.on('CARD_POWER_UP_IND', (d) => mine(d) ? card_up(s, 'power_up') : null);
		c.on('CARD_RESET_IND', (d) => mine(d) ? card_up(s, (s.state == 'powered') ? 'reset' : 'power_up') : null);
		c.on('CARD_POWER_DOWN_IND', (d) => {
			if (!mine(d))
				return;

			s.card_gen++;
			s.rpc.call({ op: 'power_down' }, () => null);
			s.state = 'connected';
			log('notice', sprintf('rsim %s: the modem powered the card down', s.ref));
		});
		c.on('APDU_IND', (d) => {
			if (!mine(d))
				return;

			push(s.queue, { apdu_id: d.apdu_id, command: d.command ?? [] });
			pump_apdu(s);
		});
	};

	let start_session = (ref, cfg) => {
		let s = new_session(ref, cfg);

		sessions[ref] = s;

		if (cfg.donor?.ref == ref)
			return fail(s, 'a modem cannot lend its card to itself');

		// defined before the card channel exists: a donor card can report its
		// end synchronously (no such modem), during its own construction
		let on_card_event, on_card_exit;

		on_card_event = (ev) => {
			if (s.state == 'failed')
				return;

			if (ev.event == 'removed') {
				log('notice', sprintf('rsim %s: card removed from the reader', ref));
				s.atr = null;
				event(s, EV_CARD_REMOVED);
			}
			else if (ev.event == 'inserted') {
				// the service takes card-inserted only WITH the ATR: without
				// it the request falls through every branch of its event
				// decoder and is refused as malformed (QMI error 1,
				// HW-observed on the RG650E, 2026-09-26)
				log('notice', sprintf('rsim %s: card inserted in the reader', ref));
				let gen = ++s.card_gen;

				s.rpc.call({ op: 'power_up' }, (err, res) => {
					if (s.state == 'failed' || s.card_gen != gen || err || !bytes(res?.atr))
						return;

					s.atr = res.atr;
					s.state = 'powered';
					event(s, EV_CARD_INSERTED, { atr: bytes(s.atr) });
				});
			}
		};

		on_card_exit = (why, hold) => {
			if (s.state == 'failed')
				return;

			fail(s, (why == 'timeout') ? 'the card reader stopped answering'
			      : (why == 'exit') ? 'the card reader helper exited'
			      : why, hold);
		};

		s.rpc = cfg.donor ? donor_card(deps, cfg.donor.ref, cfg.donor, on_card_event, on_card_exit, log)
		                  : helper_rpc(open, helper_argv(cfg, helper_path, deps.ssh_sys), on_card_event, on_card_exit, log);

		if (!s.rpc)
			return fail(s, sprintf('cannot start %s', helper_path));

		// The card first: a reader without a card must not take the modem's
		// own SIM away, which connection-available does.
		s.rpc.call({ op: 'power_up' }, (err, res) => {
			if (s.state == 'failed')
				return;

			// A helper that exits before answering never reached a card: the
			// reader is missing, busy or not permitted (its own message is
			// in the log). Only an answer without an ATR means "no card".
			if (err?.error == 'helper_exit' || err?.error == 'timeout')
				return fail(s, sprintf('cannot use the reader %s (%s; the helper\'s reason is in the log)',
					cfg.reader, err.error));

			if (err || !bytes(res?.atr))
				return fail(s, sprintf('no card in %s (%s)', cfg.reader, err?.error ?? 'no ATR'));

			s.atr = res.atr;

			deps.qmi_client(ref, UIMRMT, (qerr, c) => {
				if (s.state == 'failed' || sessions[ref] != s) {
					if (c)
						deps.qmi_release(ref, c);
					return;
				}

				if (qerr)
					return fail(s, (qerr.error == 'service_unavailable')
						? 'the modem does not offer UIM Remote — switch it on with `wwandctl rsim enable` and reset the modem'
						: sprintf('no UIM Remote client (%s)', qerr.error ?? '?'));

				s.client = c;
				wire(s);

				// NO RESET first. The service's RESET handler runs its own
				// disconnect callback on the calling client, which takes that
				// client out of its registry — the next EVENT then finds no
				// client and fails with QMI_ERR_INTERNAL (HW-observed on the
				// RG650E, 2026-09-26: error 3 on connection-available right
				// after RESET). What a previous client of ours left behind is
				// cleared when its CID is released, which runs the same
				// disconnect.
				event(s, EV_CONN_AVAILABLE, null, (e) => {
					if (e)
						return fail(s, sprintf('the modem refused the remote card: %J', e));

					s.state = 'waiting';
					note(ref, { failures: 0, retry_at: null, last_error: null });
					log('notice', sprintf('rsim %s: remote card offered to the modem (slot %d, reader %s, ATR %s)',
						ref, cfg.slot, cfg.reader, s.atr));

					// Nothing more until the modem connects (CONNECT_IND):
					// then the card is powered and its ATR goes as card-reset.
					// That is the sequence proven on the RG650E (2026-09-26);
					// a card-inserted here, without the ATR, is refused.
				});
			});
		});
	};

	// polite: tell the modem the card is gone and the connection with it, so
	// it goes back to its own SIM; `polite` false after a failure where the
	// client may be what failed
	stop_session = (s, polite) => {
		if (sessions[s.ref] == s)
			delete sessions[s.ref];

		let was = s.state;

		s.state = 'failed';
		s.queue = [];

		let c = s.client;

		s.client = null;

		// the modem goes back to its own card: the same card-change process
		// as on the way in, once the modem has let go of ours
		let back = () => {
			if (s.swapped) {
				s.swapped = false;
				deps.sim_changed?.(s.ref, 'remote SIM off, own card back');
			}
		};

		if (c && !c.destroyed) {
			stopping[s.ref] = s;

			let rel = () => {
				deps.qmi_release(s.ref, c);
				back();

				if (stopping[s.ref] == s)
					delete stopping[s.ref];
			};

			if (polite && was != 'starting') {
				c.request('EVENT', { info: { event: EV_CARD_REMOVED, slot: s.cfg.slot } }, () =>
					c.request('EVENT', { info: { event: EV_CONN_UNAVAILABLE, slot: s.cfg.slot } }, () => rel(),
						{ no_recovery: true, timeout: 3000 }),
					{ no_recovery: true, timeout: 3000 });
			}
			else {
				c.request('EVENT', { info: { event: EV_CONN_UNAVAILABLE, slot: s.cfg.slot } }, () => rel(),
					{ no_recovery: true, timeout: 3000 });
			}
		}

		s.rpc?.close();
		s.rpc = null;
	};

	return {
		// every 10 s per modem (plugins.uc): bring the session in line with
		// the configuration and the modem's life
		tick: (ref, ext) => {
			let cfg = cfg_of(ext);
			let s = sessions[ref];

			if (s && (!cfg || s.key != sprintf('%J', cfg))) {
				log('notice', sprintf('rsim %s: %s', ref, cfg ? 'reader configuration changed' : 'remote card switched off'));
				stop_session(s, true);
				s = null;
				delete notes[ref];
			}

			// the modem restarted under us: its teardown destroyed the client
			if (s && s.client && s.client.destroyed) {
				log('notice', sprintf('rsim %s: the modem restarted — offering the remote card again', ref));
				stop_session(s, false);
				s = null;
			}

			if (!cfg || s || stopping[ref])
				return;

			if (notes[ref]?.hold || (notes[ref]?.retry_at && now() < notes[ref].retry_at))
				return;

			if (!deps.modem_of?.(ref)?.modem)
				return;

			start_session(ref, cfg);
		},

		// One row for the modem's status page (plugins.uc plugins_status):
		// polled every second, so built from what is already known — no I/O.
		status: (ref, ext) => {
			let cfg = cfg_of(ext);

			if (!cfg)
				return null;

			let s = sessions[ref];
			let n = notes[ref] ?? {};
			let what = {
				starting: 'starting', waiting: 'offered, waiting for the modem',
				connected: 'modem connected, card not powered', powered: 'in use by the modem',
			};

			if (s && s.state != 'failed')
				return { label: 'remote SIM',
				         text: sprintf('%s · %s%s', cfg.reader, what[s.state] ?? s.state,
				                       s.apdus ? sprintf(' · %d commands', s.apdus) : ''),
				         level: (s.state == 'powered') ? 'ok' : 'warn' };

			if (n.last_error)
				return { label: 'remote SIM',
				         text: sprintf('%s · %s%s', cfg.reader, n.last_error,
				                       (n.retry_at && n.retry_at > now()) ? sprintf(' (retry in %d s)', n.retry_at - now()) : ''),
				         level: 'error' };

			return { label: 'remote SIM', text: sprintf('%s · waiting to start', cfg.reader), level: 'warn' };
		},

		ops: {
			// Can this modem lend its card? Takes it over through the SIM
			// Access link (its own connection drops meanwhile), reads the ATR,
			// sends SELECT MF, and hands it back. args: { slot, cond, mode }.
			donor_test: (ref, ext, args, cb) => {
				let steps = [];
				let answered = false;
				let card;
				let out = (err) => {
					if (answered)
						return;
					answered = true;
					card?.close();
					uloop.timer(1500, () => cb(null, { ok: !err, error: err, steps: steps }));
				};

				card = donor_card(deps, ref, { mode: args?.mode ?? 'sap', slot: +(args?.slot ?? 1), cond: args?.cond, apdu: args?.apdu ?? 'auto' },
					() => null, (why) => { push(steps, sprintf('link ended: %s', why)); out(why); },
					(l, m) => push(steps, m));

				card.call({ op: 'power_up' }, (e, r) => {
					push(steps, e ? sprintf('ATR failed: %J', e) : sprintf('ATR %s', r.atr));
					if (e)
						return out('atr');

					// UICC class 00: a USIM refuses the GSM class A0 (6E00)
					card.call({ op: 'tpdu', data: '00A40004023F00' }, (e2, r2) => {
						push(steps, e2 ? sprintf('SELECT MF failed: %J', e2) : sprintf('SELECT MF -> %s', r2.data));
						out(e2 ? 'apdu' : null);
					});
				});
			},

			// Read-only: what this modem's UIM service offers (message ids,
			// and the raw field lists of the ones asked for). No state on the
			// modem changes; the client is given back at the end.
			probe: (ref, ext, args, cb) => {
				// one client, both schemas' messages: the probe and SAP status
				let schema = { service: 0x0B, messages: { ...UIM_PROBE.messages, ...UIM_SAP.messages } };

				deps.qmi_client(ref, schema, (err, c) => {
					if (err)
						return cb({ error: 'no_uim_client', detail: err });

					let done = (e, r) => { deps.qmi_release(ref, c); cb(e, r); };
					let slot = +(args?.slot ?? 1);

					// SAP "check status" changes nothing; firmware without SAP
					// answers INVALID_QMI_COMMAND (71)
					let sap = (then) => c.request('SAP_CONNECTION', { conn: { op: 2, slot: slot } }, (se, sd) => {
						then(se ? { supported: !(se.error == 'qmi' && se.code == 71), error: se }
						        : { supported: true, state: sd.state, state_name: SAP_STATES[sd.state ?? 99] ?? null });
					}, { no_recovery: true, timeout: 5000 });

					c.request('GET_SUPPORTED_MSGS', {}, (e, d) => {
						if (e)
							return sap((s) => done(null, { service: 0x0B, msgs: null, msgs_error: e, sap: s }));

						let ids = bits_of(d.list);
						let want = (type(args?.msgs) == 'array') ? args.msgs : [];
						let fields = {};
						let next;

						next = (i) => {
							if (i >= length(want))
								return sap((s) => done(null, { service: 0x0B, msgs: ids, fields: fields, sap: s }));

							c.request('GET_SUPPORTED_FIELDS', { msg: +want[i] }, (fe, fd) => {
								fields[sprintf('0x%04X', +want[i])] = fe ? { error: fe } : {
									req: hexs(map(split(fd.req_f ?? '', ''), (ch) => ord(ch))),
									resp: hexs(map(split(fd.resp_f ?? '', ''), (ch) => ord(ch))),
									ind: hexs(map(split(fd.ind_f ?? '', ''), (ch) => ord(ch))),
								};
								next(i + 1);
							}, { no_recovery: true, timeout: 5000 });
						};
						next(0);
					}, { no_recovery: true, timeout: 5000 });
				});
			},

			status: (ref, ext, args, cb) => {
				let s = sessions[ref];
				let cfg = cfg_of(ext);
				let n = notes[ref] ?? {};

				cb(null, {
					enabled: !!cfg,
					reader: cfg?.reader ?? null,
					slot: cfg?.slot ?? null,
					state: s?.state ?? (cfg ? 'idle' : 'off'),
					atr: s?.atr ?? null,
					apdus: s?.apdus ?? 0,
					last_sw: s?.last_sw ?? null,
					since: s?.since ?? null,
					last_error: s?.last_error ?? n.last_error ?? null,
					retry_at: n.retry_at ?? null,
					now: now(),
				});
			},
			// give the modem its own card back now, and offer ours again
			restart: (ref, ext, args, cb) => {
				if (sessions[ref])
					stop_session(sessions[ref], true);
				delete notes[ref];
				cb(null, { restarted: true });
			},
		},
		read_ops: [ 'status' ],

		// the plugin going away: every modem gets its own card back
		stop: () => {
			for (let ref, s in sessions)
				stop_session(s, true);
		},
	};
}

return {
	UIMRMT: UIMRMT,
	cfg_of: cfg_of,
	helper_argv: helper_argv,
	bits_of: bits_of,
	UIM_SAP: UIM_SAP,
	UIM_APDU: UIM_APDU,
	csim_cmd: csim_cmd,
	csim_answer: csim_answer,
	ssh_split: ssh_split,
	SSH_KEY_DIR: SSH_KEY_DIR,
	segments: segments,
	hexs: hexs,
	bytes: bytes,

	name: 'rsim',
	options: [ 'rsim_reader', 'rsim_slot', 'rsim_clock', 'rsim_reset', 'rsim_detect', 'rsim_mode',
	           'rsim_ssh_port', 'rsim_ssh_key', 'rsim_ssh_helper', 'rsim_donor_mode', 'rsim_donor_slot', 'rsim_donor_cond', 'rsim_donor_apdu' ],
	create: create,
};
