#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 André Valentin <avalentin@marcant.net>
//
// The LuCI page (luci/.../view/wwand/rsim.js), run in node against stand-ins
// for LuCI's modules: the status table and its buttons, the scan of every
// kind of source and "Add", the SSH setup lines, and the form's checks.
// usage: node test_rsim_js.js

'use strict';

const fs = require('fs');
const path = require('path');

let checks = 0, failures = 0;
const ok = (c, what) => { checks++; if (!c) { failures++; console.error('FAIL: ' + what); } };
const eq = (a, b, what) => ok(JSON.stringify(a) === JSON.stringify(b), what + ' — got ' + JSON.stringify(a));

/* ---- LuCI stand-ins ------------------------------------------------------ */

String.prototype.format = function(...args) {
	let i = 0;
	return this.replace(/%(0?\d*)([sdXJ%])/g, (m, w, c) => {
		if (c == '%') return '%';
		let v = args[i++];
		if (c == 's') return String(v);
		if (c == 'J') return JSON.stringify(v);
		let n = (c == 'X') ? Number(v).toString(16).toUpperCase() : String(Math.floor(Number(v)));
		return w ? n.padStart(parseInt(w, 10), w[0] == '0' ? '0' : ' ') : n;
	});
};

function E(tag, attrs, children) {
	if (Array.isArray(tag))
		return { tag: 'fragment', attrs: {}, children: [].concat(attrs == null ? [] : attrs).flat(9) };
	if (typeof attrs == 'string' || Array.isArray(attrs) || (attrs && attrs.tag)) {
		children = attrs;
		attrs = {};
	}
	return { tag: tag, attrs: attrs || {}, children: [].concat(children == null ? [] : children).flat(9),
	         appendChild(c) { this.children.push(c); } };
}

// all text below a node
function text(n) {
	if (n == null || n === '') return '';
	if (typeof n == 'string' || typeof n == 'number') return String(n);
	if (Array.isArray(n)) return n.map(text).join(' ');
	return (n.children || []).map(text).join(' ');
}

// every node matching pred
function find(n, pred, out) {
	out = out || [];
	if (n && typeof n == 'object') {
		if (pred(n)) out.push(n);
		(n.children || []).forEach((c) => find(c, pred, out));
	}
	return out;
}

const buttons = (n) => find(n, (x) => x.tag == 'button');
const button = (n, label) => buttons(n).filter((b) => text(b).trim() == label)[0];

// uci: network sections in memory
let conf = {};
const uci = {
	load: () => Promise.resolve(),
	sections: (c, type) => Object.keys(conf).filter((k) => conf[k]['.type'] == type).map((k) => conf[k]),
	get: (c, sid, opt) => opt ? (conf[sid] || {})[opt] : conf[sid],
	add: (c, type, name) => { conf[name] = { '.name': name, '.type': type }; return name; },
	set: (c, sid, opt, v) => { conf[sid][opt] = v; },
};

// ubus: method -> handler
let ubus = {}, ubusCalls = [];
const rpc = {
	declare: (o) => (...a) => {
		let args = {};
		(o.params || []).forEach((p, i) => { args[p] = a[i]; });
		ubusCalls.push([ o.method, args ]);
		return Promise.resolve(ubus[o.method] ? ubus[o.method](args) : {});
	},
};

let execs = [], execAnswer = () => ({ code: 0, stdout: '', stderr: '' }), files = {};
const fsm = {
	exec: (cmd, args) => { execs.push([ cmd ].concat(args || []).join(' ')); return Promise.resolve(execAnswer(cmd, args)); },
	read: (p) => files[p] != null ? Promise.resolve(files[p]) : Promise.reject(new Error('ENOENT')),
};

let notes = [], modals = [];
const ui = {
	createHandlerFn: (ctx, fn) => fn,
	showModal: (title, body) => modals.push([ title, body ]),
	hideModal: () => null,
	addNotification: (t, body) => notes.push(text(body)),
};
const dom = { content: (node, c) => { node.children = [].concat(c).flat(9); return node; } };
let polls = [];
const poll = { add: (fn, s) => polls.push([ fn, s ]) };

// form: records the options (validate functions included), renders nothing
let options = {};
class Section {
	constructor(type) { this.type = type; }
	option(cls, name) {
		let o = { name: name, values: [], deps: [], value(k, v) { this.values.push(k); }, depends(d, v) { this.deps.push(v === undefined ? d : { [d]: v }); } };
		o.section = { formvalue: (sid, opt) => (conf[sid] || {})[opt] };
		options[this.type + '.' + name] = o;
		return o;
	}
}
const form = {
	Map: class { constructor() { this.parsed = 0; this.resets = 0; }
		section(cls, type) { return new Section(type); }
		render() { return Promise.resolve(E('div', { id: 'map' })); }
		parse() { this.parsed++; return Promise.resolve(); }
		reset() { this.resets++; return Promise.resolve(); }
		load() { this.loads = (this.loads || 0) + 1; return Promise.resolve(); } },
	TypedSection: 'TypedSection', ListValue: { prototype: { load: () => null } }, Value: 'Value', DummyValue: 'DummyValue',
};

const L = {
	resolveDefault: (p, d) => Promise.resolve(p).catch(() => d),
	bind: (fn, ctx) => fn.bind(ctx),
};
const viewm = { extend: (o) => o };
const _ = (s) => s;

const src = fs.readFileSync(path.join(__dirname, '../../luci/htdocs/luci-static/resources/view/wwand/rsim.js'), 'utf8');
const page = new Function('view', 'form', 'rpc', 'ui', 'fs', 'uci', 'dom', 'poll', 'L', 'E', '_', src)(
	viewm, form, rpc, ui, fsm, uci, dom, poll, L, E, _);

/* ---- the page ------------------------------------------------------------- */

(async () => {
	const KEY = 'ssh-ed25519 AAAAC3NzaTEST wwand-rsim\n';

	conf = {
		m0: { '.name': 'm0', '.type': 'wwand_modem', rsim: 'phone' },
		m1: { '.name': 'm1', '.type': 'wwand_modem' },
		m2: { '.name': 'm2', '.type': 'wwand_modem', rsim_reader: 'ssh:rsim@pc.lan:at:/dev/ttyUSB2' },
		phone: { '.name': 'phone', '.type': 'wwand_simreader', type: 'bt', device: 'AA:BB:CC:DD:EE:FF', host: 'root@pc.lan' },
		b: { '.name': 'b', '.type': 'wwand_simreader', type: 'wwand', device: 'iccid:89490200001022832490', host: 'root@simrouter' },
	};

	const status = { m0: { model: 'RG650E', state: 'READY', iccid: '8988' }, m1: { model: 'EG06', state: 'READY' }, m2: { state: 'SIM_BLOCKED' } };
	const rs = {
		m0: { enabled: true, reader_name: 'phone', reader: 'ssh:root@pc.lan:bt:AA:BB:CC:DD:EE:FF', slot: 1, state: 'powered',
		      reader_info: { backend: 'bt', name: 'Galaxy S9', kind: 'smartphone', key: 'authenticated', channel: 8 },
		      atr: '3B9F', apdus: 12, last_sw: '9000', since: 900, now: 1000, lendable: false, lend_why: 'this modem uses a remote card itself' },
		m1: { enabled: false, now: 1000, lent_to: { to: '10.0.0.2', remote: true, mode: 'sap', commands: 5, since: 940 }, lendable: false, lend_why: 'its card is lent to 10.0.0.2' },
		m2: { enabled: true, reader: 'ssh:rsim@pc.lan:at:/dev/ttyUSB2', slot: 1, state: 'failed', last_error: 'no card', retry_at: 1030, now: 1000,
		      lend_hold: true, lendable: false },
	};

	ubus.status = () => status;
	ubus.modem_plugin_status = (a) => rs[a.modem];
	ubus.modem_plugin = (a) => ({ ok: true });

	const data = { modems: [ 'm0', 'm1', 'm2' ], st: [ rs.m0, rs.m1, rs.m2 ], key: KEY };
	const root = await page.render(data);
	const view = page;

	ok(root && text(root).indexOf('Status') >= 0, 'render: the page');
	eq(polls.length, 1, 'status: polled');
	eq(polls[0][1], 5, 'status: ...every 5 s');

	/* status table */
	const st = text(view.statusBox);

	ok(st.indexOf('in use by the modem') >= 0 && st.indexOf('ATR 3B9F') >= 0 && st.indexOf('12 commands · last SW 9000') >= 0,
	   'status: a modem on a phone\'s SIM, its card and commands');
	ok(st.indexOf('since 100 s') >= 0, 'status: since when');
	ok(st.indexOf('"Galaxy S9" (smartphone) · authenticated pairing · channel 8') >= 0, 'status: what the reader in use is (its info event)');
	ok(st.indexOf('lent to 10.0.0.2 (another router)') >= 0 && st.indexOf('5 commands') >= 0, 'status: a card lent to another router');
	ok(st.indexOf('no card (retry in 30 s)') >= 0, 'status: an error and when it is tried again');
	ok(st.indexOf('lending stopped here') >= 0, 'status: lending stopped');
	ok(st.indexOf('not lendable: this modem uses a remote card itself') >= 0, 'status: why a card cannot be lent');

	const rows = find(view.statusBox, (x) => x.tag == 'tr').slice(1);

	eq(rows.length, 3, 'status: one row per modem');
	ok(button(rows[1], 'Take back'), 'status: "Take back" for a card lent to another router');
	ok(!button(rows[0], 'Take back'), 'status: ...not where nothing is lent');
	ok(button(rows[2], 'Allow lending'), 'status: "Allow lending" where it was stopped');
	ok(button(rows[0], 'Restart') && !button(rows[1], 'Restart'), 'status: "Restart" only where a remote SIM is set');

	ubusCalls = [];
	await button(rows[1], 'Take back').attrs.click();
	ok(ubusCalls.some((c) => c[0] == 'modem_plugin' && c[1].modem == 'm1' && c[1].op == 'lend_end'), 'status: Take back = lend_end of that modem');
	ok(ubusCalls.some((c) => c[0] == 'status'), 'status: ...and the table is refreshed');

	ubusCalls = [];
	await button(find(view.statusBox, (x) => x.tag == 'tr')[3], 'Allow lending').attrs.click();
	ok(ubusCalls.some((c) => c[1].op == 'lend_allow' && c[1].modem == 'm2'), 'status: Allow lending = lend_allow');

	ubus.modem_plugin = (a) => a.op == 'probe' ? { ok: true, msgs: [ 0x20, 0x3b ], sap: { supported: true, state: 0, state_name: 'not enabled' } } : { ok: true };
	await button(find(view.statusBox, (x) => x.tag == 'tr')[1], 'Probe').attrs.click();
	const pm = text(modals[modals.length - 1][1]);
	ok(pm.indexOf('0x20 0x3B') >= 0 && pm.indexOf('present, link not enabled') >= 0, 'probe: UIM messages and SIM Access in a dialog');

	/* scan: every kind of source */
	const rows_all = [
		{ backend: 'pcsc', spec: 'pcsc:ACS ACR38U 00 00', name: 'ACS ACR38U 00 00', card: true },
		{ backend: 'wbsm', spec: 'wbsm:088888-03', serial: '088888-03' },
		{ backend: 'tty', spec: 'phoenix:/dev/ttyUSB0', device: '/dev/ttyUSB0', driver: 'cp210x', usb: '10c4:ea60', hint: 'phoenix',
		  usb_manufacturer: 'Silicon Labs', usb_product: 'CP2102', usb_serial: '0001', usb_path: '1-1.2', usb_speed_mbps: '12',
		  by_id: '/dev/serial/by-id/usb-Silicon_Labs_CP2102-if00-port0' },
		{ backend: 'tty', spec: 'at:/dev/ttyUSB2', device: '/dev/ttyUSB2', driver: 'option', usb: '2c7c:0125', hint: 'at', in_use: 'AT port of wwand modem m0' },
		{ backend: 'bt', spec: 'bt:11:22:33:44:55:66', device: '11:22:33:44:55:66', name: 'Galaxy', alias: 'My Galaxy', phone: true, sap: true,
		  sap_channel: 8, connected: false, kind: 'smartphone', vendor: 'Samsung', vendor_id: '0x0075', key: 'unauthenticated',
		  trusted: true, services: 'SAP,HFP-AG', adapter: '00:1A:7D:DA:71:13', adapter_name: 'router', adapter_powered: true },
		{ backend: 'bt', spec: 'bt:11:22:33:44:55:77', name: 'Pixel', phone: true, sap: false },
		{ backend: 'wwand', spec: 'modem:m1', modem: 'm1', iccid: '89490200001022832490', imsi: '262011', slot: 1, lendable: true,
		  modem_manufacturer: 'Quectel', modem_model: 'RG502Q-EA', modem_revision: 'RG502QEAAAR11A06M4G', operator: 'Telekom.de', rat: '5G', modem_state: 'READY', modes: [ 'sap', 'apdu' ],
		  config: { name: 'work', apn: 'internet', pdp_type: 'ipv4v6', pin: true } },
		{ backend: 'wwand', spec: 'modem:m2', modem: 'm2', iccid: '89490200001022830000', lendable: false, why: 'its card is lent to x', config: null },
	];

	execAnswer = (cmd, args) => ({ code: 0, stdout: JSON.stringify({ ok: true, backends: [ 'phoenix', 'at', 'wbsm', 'pcsc', 'bt', 'wwand' ], readers: rows_all, note: null }) });
	await view.scan('');
	eq(execs[execs.length - 1], '/usr/bin/wwandctl rsim scan --json', 'scan: here, through wwandctl (the ACL grants exactly this)');

	const sc = text(view.scanBox);

	[ 'PC/SC reader', 'card inserted', 'Smartmouse USB', 'serial 088888-03', 'a USB-serial adapter (Phoenix)', 'a modem\'s port (AT)',
	  'in use: AT port of wwand modem m0', 'Galaxy', 'offers SIM Access', 'SIM Access not offered', '89490200001022832490',
	  'modem m1', 'IMSI 262011', 'APN internet', 'PIN set there', 'not now: its card is lent to x', 'no wwand_sim settings there',
	  'phoenix, at, wbsm, pcsc, bt, wwand',
	  'My Galaxy', 'offers SIM Access (channel 8)', 'not connected', 'Vendor: Samsung (0x0075)', 'Profiles: SAP,HFP-AG',
	  'paired without confirmation', 'Adapter: 00:1A:7D:DA:71:13 (router)',
	  'USB device: Silicon Labs CP2102', 'USB path: 1-1.2', 'Stable name: /dev/serial/by-id/usb-Silicon_Labs_CP2102-if00-port0',
	  'Quectel RG502Q-EA', 'fw RG502QEAAAR11A06M4G', 'Telekom.de', '5G', 'lends over SIM Access or APDU' ].forEach((w) => ok(sc.indexOf(w) >= 0, 'scan shows: ' + w));

	const srows = find(view.scanBox, (x) => x.tag == 'tr').slice(1);

	eq(srows.length, rows_all.length, 'scan: one row per source');
	ok(button(srows[3], 'Add').attrs.disabled === true, 'scan: a port in use cannot be added');
	ok(button(srows[7], 'Add').attrs.disabled === true, 'scan: a card that cannot be lent now cannot be added');
	ok(button(srows[4], 'Add').attrs.disabled == null, 'scan: a phone can');

	await button(srows[4], 'Add').attrs.click();
	let added = uci.sections('network', 'wwand_simreader').filter((r) => r.type == 'bt' && r.device == '11:22:33:44:55:66')[0];
	ok(added && !added.host, 'add: the phone here as a bt reader, no host');
	ok(notes.some((n) => n.indexOf(added['.name'] + ' added') >= 0), 'add: said so');
	ok(view.map.parsed >= 1 && view.map.resets >= 1, 'add: the form\'s edits kept (parsed first), then shown again');
	ok(view.map.loads >= 1, 'add: the values loaded before the form is shown again (else the new section shows defaults)');

	await button(srows[6], 'Add').attrs.click();
	added = uci.sections('network', 'wwand_simreader').filter((r) => r.type == 'modem' && r.donor == 'm1')[0];
	ok(added, 'add: a card of a modem here = a SIM sponsor (type modem)');

	ok(button(srows[4], 'Test') && !button(srows[6], 'Test'), 'scan: a phone has Test; a wwand router\'s card does not (that router checks it)');
	ok(button(srows[3], 'Test').attrs.disabled === true, 'scan: ...and a port wwand uses cannot be tested');
	execAnswer = (cmd, args) => ({ code: 1, stdout: JSON.stringify({ ok: false, spec: 'at:/dev/ttyACM0', error: 'the modem refuses AT+CSIM (ERROR)', log: [ 'rsim-card: ...' ] }) });
	await button(srows[4], 'Test').attrs.click();
	eq(execs[execs.length - 1], '/usr/bin/wwandctl rsim test bt:11:22:33:44:55:66 --json', 'test: exactly that source, through wwandctl (the ACL grants this)');
	ok(text(modals[modals.length - 1][1]).indexOf('Does not work: the modem refuses AT+CSIM') >= 0, 'test: the result in a dialog');
	execAnswer = (cmd, args) => ({ code: 0, stdout: JSON.stringify({ ok: true, backends: [ 'phoenix', 'at', 'wbsm', 'pcsc', 'bt', 'wwand' ], readers: rows_all, note: null }) });

	await button(srows[0], 'Add').attrs.click();
	ok(uci.sections('network', 'wwand_simreader').some((r) => r.type == 'pcsc' && r.device == 'ACS ACR38U 00 00'), 'add: a PC/SC reader by name');

	/* the same on another machine: host set, a wwand router's card as wwand:iccid: */
	execAnswer = () => ({ code: 0, stdout: JSON.stringify({ ok: true, backends: [ 'wwand' ], readers: [
		{ backend: 'wwand', spec: 'wwand:iccid:89490200001022839999', modem: 'wwmodem0', iccid: '89490200001022839999', lendable: true, config: null },
		{ backend: 'bt', spec: 'bt:11:22:33:44:55:88', name: 'Galaxy2', sap: true },
	] }) });
	await view.scan('root@simrouter2');
	eq(execs[execs.length - 1], '/usr/bin/wwandctl rsim scan root@simrouter2 --json', 'scan: another machine');
	let r2 = find(view.scanBox, (x) => x.tag == 'tr').slice(1);
	await button(r2[0], 'Add').attrs.click();
	await button(r2[1], 'Add').attrs.click();
	ok(uci.sections('network', 'wwand_simreader').some((r) => r.type == 'wwand' && r.device == 'iccid:89490200001022839999' && r.host == 'root@simrouter2'),
	   'add: a card of the other router\'s modem, by ICCID, with its host');
	ok(uci.sections('network', 'wwand_simreader').some((r) => r.type == 'bt' && r.host == 'root@simrouter2'), 'add: a phone paired with another machine');

	/* SSH setup: per machine, restricted to what it is asked for */
	const kb = text(view.keyBox);

	ok(kb.indexOf('command="rsim-card --serve \'bt:AA:BB:CC:DD:EE:FF\'",no-pty,no-port-forwarding,no-agent-forwarding,no-X11-forwarding ssh-ed25519 AAAAC3NzaTEST wwand-rsim') >= 0,
	   'ssh: the line for pc.lan, restricted to its phone');
	ok(kb.indexOf('rsim-card --serve \'wwand:iccid:89490200001022832490\'') >= 0, 'ssh: ...for the other wwand router, its card');
	ok(kb.indexOf('rsim@pc.lan') >= 0 && kb.indexOf('\'at:/dev/ttyUSB2\'') >= 0, 'ssh: a reader spelled out on a modem counts too');
	ok(kb.indexOf('root@simrouter2') >= 0 && kb.indexOf('\'wwand:iccid:89490200001022839999\' \'bt:11:22:33:44:55:88\'') >= 0,
	   'ssh: a machine just added gets its line at once');

	conf.scm = { '.name': 'scm', '.type': 'wwand_simreader', type: 'pcsc', device: 'SCM SCR 3310 [CCID Interface] 00 00', host: 'root@pcsc.lan' };
	view.renderKey();
	ok(text(view.keyBox).indexOf("rsim-card --serve 'pcsc:SCM SCR 3310 \\[CCID Interface\\] 00 00'") >= 0,
	   'ssh: a PC/SC name\'s brackets stay literal (as wwandctl writes it)');

	conf.sm2 = { '.name': 'sm2', '.type': 'wwand_simreader', type: 'wbsm', host: 'u@dev', helper: '/home/u/.local/bin/rsim-card' };
	view.renderKey();
	ok(text(view.keyBox).indexOf("command=\"'/home/u/.local/bin/rsim-card' --serve 'wbsm:'\"") >= 0, 'ssh: the helper path in command=');

	const hostRow = find(view.keyBox, (x) => x.tag == 'div' && x.attrs['class'] == 'cbi-value' && text(x).indexOf('root@pc.lan') >= 0)[0];
	execAnswer = () => ({ code: 1, stdout: JSON.stringify({ ok: false, error: 'no answer from rsim-card --list on root@pc.lan',
		detail: [ 'root@pc.lan: Permission denied (publickey).' ], hint: 'root@pc.lan does not accept this router\'s key — `wwandctl rsim ssh-key root@pc.lan` prints the line for its authorized_keys' }) });
	await button(hostRow, 'Test').attrs.click();
	const tr = text(hostRow);
	ok(tr.indexOf('Permission denied') >= 0 && tr.indexOf('Hint:') >= 0 && tr.indexOf('authorized_keys') >= 0, 'ssh test: the failure, what ssh said, and the hint');
	ok(tr.indexOf('command="rsim-card --serve') >= 0, 'ssh test: ...with the line to put there');

	execAnswer = () => ({ code: 0, stdout: JSON.stringify({ ok: true, backends: [ 'phoenix', 'bt' ], readers: [] }) });
	await button(hostRow, 'Test').attrs.click();
	ok(text(hostRow).indexOf('Works: rsim-card there answers (backends: phoenix, bt)') >= 0, 'ssh test: works');

	await view.scan('not a host');
	ok(text(view.scanBox).indexOf('Expecting user@host') >= 0, 'scan: a host that is not user@host is not run');

	/* no key yet */
	view.key = null;
	view.renderKey();
	files['/etc/wwand/rsim/id_dropbear.pub'] = KEY;
	await button(view.keyBox, 'Create key').attrs.click();
	ok(execs[execs.length - 1] == '/usr/bin/wwandctl rsim ssh-key' && text(view.keyBox).indexOf('AAAAC3NzaTEST') >= 0,
	   'ssh: "Create key" makes it and shows it');

	/* the form's checks */
	const dev = options['wwand_simreader.device'];
	conf.t1 = { '.name': 't1', '.type': 'wwand_simreader', type: 'wwand' };
	ok(dev.validate('t1', 'wwmodem0') === true && dev.validate('t1', 'iccid:89490200001022832490') === true, 'form: a wwand router\'s modem or iccid:');
	ok(dev.validate('t1', 'x;reboot') !== true && dev.validate('t1', '') !== true, 'form: ...nothing else');
	conf.t1.type = 'bt';
	ok(dev.validate('t1', 'AA:BB:CC:DD:EE:FF') === true && dev.validate('t1', 'phone') !== true, 'form: a phone\'s address');
	const host = options['wwand_simreader.host'];
	conf.t1.type = 'wwand';
	ok(host.validate('t1', '') !== true && host.validate('t1', 'root@b') === true, 'form: another wwand router needs its host');
	conf.t1.type = 'pcsc';
	ok(host.validate('t1', '') === true, 'form: a reader here needs none');
	ok(options['wwand_simreader.type'].values.join(',') == 'wbsm,phoenix,pcsc,at,bt,modem,wwand', 'form: every kind of source');
	ok(options['wwand_simreader.donor_mode'].deps.some((d) => d.type == 'wwand' || d == 'type') , 'form: how it lends, also for another router');

	console.log('test_rsim_js: %d checks, %d failures', checks, failures);
	process.exit(failures ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
