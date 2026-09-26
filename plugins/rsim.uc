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

	if (substr(r, 0, 4) == 'ssh:') {
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
		slot: (slot >= 0 && slot <= 3) ? slot : 1,
		clock: ext.rsim_clock ?? null,
		reset: ext.rsim_reset ?? null,
		detect: ext.rsim_detect ?? null,
		mode: ext.rsim_mode ?? null,
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
	// per modem: what the status shows after a session is gone
	let notes = {};

	let stop_session;

	let new_session = (ref, cfg) => ({
		ref: ref, cfg: cfg, key: sprintf('%J', cfg),
		state: 'starting',     // starting | waiting | connected | powered | failed
		rpc: null, client: null, atr: null,
		apdus: 0, last_sw: null, last_error: null, since: now(),
		queue: [], busy: false,
	});

	let note = (ref, patch) => {
		notes[ref] = { ...(notes[ref] ?? {}), ...patch };
	};

	let fail = (s, why) => {
		s.last_error = why;
		note(s.ref, { last_error: why, failures: +(notes[s.ref]?.failures ?? 0) + 1 });

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
	let card_up = (s, op) => {
		s.rpc.call({ op: op }, (err, res) => {
			if (s.state == 'failed')
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

		s.busy = true;
		s.rpc.call({ op: 'tpdu', data: hexs(a.command) }, (err, res) => {
			s.busy = false;

			if (s.state == 'failed')
				return;

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

			s.state = 'waiting';
			log('notice', sprintf('rsim %s: the modem disconnected from the remote card', s.ref));
		});
		c.on('CARD_POWER_UP_IND', (d) => mine(d) ? card_up(s, 'power_up') : null);
		c.on('CARD_RESET_IND', (d) => mine(d) ? card_up(s, (s.state == 'powered') ? 'reset' : 'power_up') : null);
		c.on('CARD_POWER_DOWN_IND', (d) => {
			if (!mine(d))
				return;

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

		s.rpc = helper_rpc(open, helper_argv(cfg, helper_path, deps.ssh_sys), (ev) => {
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
				s.rpc.call({ op: 'power_up' }, (err, res) => {
					if (s.state == 'failed' || err || !bytes(res?.atr))
						return;

					s.atr = res.atr;
					s.state = 'powered';
					event(s, EV_CARD_INSERTED, { atr: bytes(s.atr) });
				});
			}
		}, (why) => {
			if (s.state != 'failed')
				fail(s, (why == 'timeout') ? 'the card reader stopped answering' : 'the card reader helper exited');
		}, log);

		if (!s.rpc)
			return fail(s, sprintf('cannot start %s', helper_path));

		// The card first: a reader without a card must not take the modem's
		// own SIM away, which connection-available does.
		s.rpc.call({ op: 'power_up' }, (err, res) => {
			if (s.state == 'failed')
				return;

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
			let rel = () => { deps.qmi_release(s.ref, c); back(); };

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

			if (!cfg || s)
				return;

			if (notes[ref]?.retry_at && now() < notes[ref].retry_at)
				return;

			if (!deps.modem_of?.(ref)?.modem)
				return;

			start_session(ref, cfg);
		},

		ops: {
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
	ssh_split: ssh_split,
	SSH_KEY_DIR: SSH_KEY_DIR,
	segments: segments,
	hexs: hexs,
	bytes: bytes,

	name: 'rsim',
	options: [ 'rsim_reader', 'rsim_slot', 'rsim_clock', 'rsim_reset', 'rsim_detect', 'rsim_mode',
	           'rsim_ssh_port', 'rsim_ssh_key', 'rsim_ssh_helper' ],
	create: create,
};
