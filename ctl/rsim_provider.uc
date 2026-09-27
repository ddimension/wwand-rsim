// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 André Valentin <avalentin@marcant.net>
// `wwandctl rsim proxy`: this router's modem cards lent to ANOTHER router —
// the provider side, package wwand-rsim-provider (which depends on
// wwand-rsim: the lending itself is the rsim plugin's, in the daemon).
//
// Another router's wwand-rsim (`rsim_reader 'ssh:<user>@<here>:wwand:<modem>'`
// or `wwand:iccid:<ICCID>`) runs `wwandctl rsim proxy` here over SSH. The
// proxy speaks rsim-card's line protocol on stdin/stdout and hands every
// request to the rsim plugin in THIS daemon (modem_plugin lend_*), which
// lends the card the way a local SIM sponsor does: over the SIM Access
// Profile or APDU by APDU, with the radio parked meanwhile. Only the daemon
// can do that — it holds the modem — so the proxy is a relay and nothing more.
//
// Not a wwandctl command of its own (no run): `wwandctl rsim proxy` loads it,
// and says which package is missing when it is not installed. Without it a
// router still lends a card to its own modems (a SIM sponsor) and lists its
// cards for its own scan; another router can only reach its modems' cards
// over their AT ports (rsim-card's at:).

'use strict';

import * as fs from 'fs';
import * as libuci from 'uci';
import * as libubus from 'ubus';

let base = require('wwand.ctl.rsim');

// the proto-wwand interfaces, for the settings a modem dials with
function wwand_ifaces()
{
	let out = [];
	let c = libuci.cursor();

	c.load('network');
	c.foreach('network', 'interface', (sec) => {
		if (sec.proto == 'wwand')
			push(out, sec);
	});

	return out;
}

// What a lent card is dialled with here, for the router that borrows it —
// which keeps it as its own wwand_sim for that card (the plugin's
// sim_upsert): this router's wwand_sim of the card, or else the connection
// of the modem's interface. The password goes too: that router is given the
// card itself, over the same SSH link. null when there is no APN to pass on.
function lend_settings(sims, ifaces, iccid, modem)
{
	let pick = (sec, source) => (sec?.apn != null && sec.apn != '') ? {
		apn: sec.apn, pdp_type: sec.pdp_type ?? null, auth: sec.auth ?? null,
		username: sec.username ?? null, password: sec.password ?? null, source: source,
	} : null;
	let id = base.norm_iccid(iccid);

	for (let sec in (sims ?? []))
		if (id && base.norm_iccid(sec.iccid) == id)
			return pick(sec, 'sim');

	for (let sec in (ifaces ?? []))
		if (sec.modem == modem && pick(sec, 'interface'))
			return pick(sec, 'interface');

	return null;
}

// the modem a target names: a modem, or iccid:<ICCID> — the modem that RUNS
// on that card now
function lend_target(call, target)
{
	if (substr(target ?? '', 0, 6) == 'iccid:') {
		let id = base.norm_iccid(substr(target, 6));

		if (!id)
			return { error: sprintf('%s is not an ICCID', substr(target, 6)) };

		for (let c in (call('sim_inventory', {})?.cards ?? []))
			if (c.iccid == id && c.present && c.active && c.modem && !c.reader)
				return { modem: c.modem };

		return { error: sprintf('no modem here runs on the card %s', id) };
	}

	if (!match(target ?? '', /^[A-Za-z0-9_]+$/))
		return { error: 'usage: wwandctl rsim proxy <modem | iccid:ICCID>' };

	if (!call('status', {})?.modems?.[target])
		return { error: sprintf('no modem %s here', target) };

	return { modem: target };
}

// One lend, request by request. `call` as above; o: { target, mode, slot,
// apdu, cond, client, pid }.
function proxy_session(call, o)
{
	let modem = null, id = null, ended = null;
	let plugin = (op, args) => call('modem_plugin', { modem: modem, plugin: 'rsim', op: op, args: args });
	let io_err = (why) => {
		ended ??= why;
		return { ok: false, error: 'io', detail: why };
	};

	return {
		// null, or why the card cannot be had
		open: () => {
			let t = lend_target(call, o.target);

			if (t.error)
				return t.error;

			modem = t.modem;

			let r = plugin('lend_open', { mode: o.mode, slot: o.slot, apdu: o.apdu, cond: o.cond,
			                              client: o.client, pid: o.pid });

			if (r == null)
				return 'wwand does not answer (is it running?)';
			if (r.ok === false)
				return sprintf('%s cannot lend its card: %s', modem,
					(r.error == 'invalid_op' || r.error == 'no_such_plugin')
						? 'wwand-rsim here is too old or not loaded' : (r.detail ?? r.error ?? '?'));

			id = r.id;
			return null;
		},

		// one request line -> the answer object
		line: (line) => {
			let req = null;

			try { req = json(line); } catch (e) { req = null; }

			if (type(req) != 'object' || type(req.op) != 'string')
				return { ok: false, error: 'bad_request', detail: 'not a request object with an op' };

			let r = plugin('lend_call', { id: id, req: req });

			if (r == null)
				return io_err('wwand does not answer');
			if (r.ok === false)
				return io_err(sprintf('the lend is gone (%s)', r.error ?? '?'));
			if (r.ended)
				return io_err(sprintf('the card went home: %s', r.why ?? '?'));

			return r.answer ?? io_err('no answer');
		},

		ended: () => ended,
		modem: () => modem,

		close: () => {
			if (id != null)
				plugin('lend_close', { id: id });
			id = null;
		},
	};
}

// `rsim proxy <modem | iccid:ICCID> [--mode sap|apdu|auto] [--slot N]
// [--apdu qmi|at] [--cond N]` — or `--list`. sys (tests): { call, read_line,
// write, pid, client, sims }.
function proxy(ctx, args, sys)
{
	let o = { target: null, mode: 'auto', slot: null, apdu: null, cond: null };
	let list = false;

	for (let i = 0; i < length(args); i++) {
		let a = args[i];

		if (a == '--list')
			list = true;
		else if (a == '--mode' && index([ 'sap', 'apdu', 'auto' ], args[i + 1]) >= 0)
			o.mode = args[++i];
		else if (a == '--slot' && match(args[i + 1] ?? '', /^[1-5]$/))
			o.slot = +args[++i];
		else if (a == '--apdu' && index([ 'qmi', 'at' ], args[i + 1]) >= 0)
			o.apdu = args[++i];
		else if (a == '--cond' && match(args[i + 1] ?? '', /^([0-9]+|none)$/))
			o.cond = args[++i];
		else if (o.target == null && substr(a, 0, 1) != '-')
			o.target = a;
		else {
			warn('usage: wwandctl rsim proxy <modem | iccid:ICCID> [--mode sap|apdu|auto] [--slot N] [--apdu qmi|at] [--cond N] | --list\n');
			return 2;
		}
	}

	// its own connection: a lend's first answer may take longer than
	// wwandctl's default wait, and a failed call must end the lend cleanly
	// instead of the process
	let call = sys?.call;

	if (!call) {
		let conn = libubus.connect(null, 90);

		if (!conn) {
			warn('wwandctl rsim proxy: no ubus\n');
			return 1;
		}
		call = (m, a) => conn.call('wwand', m, a ?? {});
	}

	let write = sys?.write ?? ((l) => { print(l, '\n'); fs.stdout.flush(); });

	if (list) {
		for (let r in base.lend_rows(call, sys?.sims ?? base.wwand_sims()))
			write(sprintf('%J', r));
		return 0;
	}

	// the far end's address, for the status here ("lends its card to ...")
	let peer = split(getenv('SSH_CLIENT') ?? getenv('SSH_CONNECTION') ?? '', ' ')[0];

	o.client = sys?.client ?? (length(peer) ? peer : 'another router');
	o.pid = sys?.pid ?? +(fs.readlink('/proc/self') ?? 0);

	if (!(o.pid > 0)) {
		warn('wwandctl rsim proxy: cannot tell its own pid (/proc/self), which the lend is watched by\n');
		return 1;
	}

	// A dropped SSH link closes stdin: that ends the loop below and hands the
	// card back, instead of the hang-up killing us with the card still lent.
	// Before the lend is opened: a hang-up during the open must not leave one.
	if (!sys) {
		signal('SIGHUP', 'ignore');
		signal('SIGPIPE', 'ignore');
	}

	let s = proxy_session(call, o);
	let why = s.open();

	if (why) {
		warn(sprintf('wwandctl rsim proxy: %s\n', why));
		return 1;
	}

	warn(sprintf('wwandctl rsim proxy: %s lends its card (%s)\n', s.modem(), o.mode));

	// like rsim-card after its open: what the other router is using — this
	// router, its modem, the card
	{
		let m = call('status', {})?.modems?.[s.modem()];
		let host = trim(fs.readfile('/proc/sys/kernel/hostname') ?? '');

		write(sprintf('%J', { event: 'info', backend: 'wwand', reader: s.modem(), host: length(host) ? host : null,
		                      mode: o.mode, iccid: m?.iccid ?? null, imsi: m?.imsi ?? null, ...base.modem_meta(m),
		                      sim: lend_settings(sys?.sims ?? base.wwand_sims(), sys?.ifaces ?? wwand_ifaces(),
		                                         m?.iccid ?? ((substr(o.target, 0, 6) == 'iccid:') ? substr(o.target, 6) : null),
		                                         s.modem()) }));
	}

	let read_line = sys?.read_line ?? (() => fs.stdin.read('line'));
	let line;

	while (length(line = read_line() ?? '')) {
		if (!length(trim(line)))
			continue;

		write(sprintf('%J', s.line(line)));

		if (s.ended())
			break;
	}

	s.close();

	if (s.ended())
		warn(sprintf('wwandctl rsim proxy: %s\n', s.ended()));

	return s.ended() ? 1 : 0;
}

return {
	proxy: proxy,
	proxy_session: proxy_session,
	lend_settings: lend_settings,
};
