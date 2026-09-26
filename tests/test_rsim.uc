// wwand-rsim tests — the plugin: the UIM Remote wire format, and the session
// with the modem and the card, against a scripted modem client and a
// simulated card reader.

'use strict';

import * as uloop from 'uloop';
import * as struct from 'struct';
import * as tlv from 'wwand.codec.tlv';
import { eq, ok, done } from './lib/check.uc';

let rsim = require('wwand.plugins.rsim');
let M = rsim.UIMRMT.messages;

uloop.init();

// --- the wire format, against hand-built buffers -----------------------------
//
// A wrong TLV id or width decodes garbage without an error, so every message
// the plugin sends or reads is checked byte by byte here.
{
	let tl = (t, v) => struct.pack('<BH', t, length(v)) + v;

	eq(tlv.pack(M.EVENT.req, { info: { event: 1, slot: 1 } }),
	   tl(0x01, struct.pack('<II', 1, 1)),
	   'wire: EVENT connection available = TLV 0x01 {u32 event, u32 slot}');
	eq(tlv.pack(M.EVENT.req, { info: { event: 5, slot: 1 }, atr: [ 0x3B, 0x9F, 0x96 ] }),
	   tl(0x01, struct.pack('<II', 5, 1)) + tl(0x10, chr(3, 0x3B, 0x9F, 0x96)),
	   'wire: EVENT card reset carries the ATR in 0x10 with a u8 length');
	eq(tlv.pack(M.EVENT.req, { info: { event: 4, slot: 2 }, error_cause: 2 }),
	   tl(0x01, struct.pack('<II', 4, 2)) + tl(0x12, struct.pack('<I', 2)),
	   'wire: EVENT card error carries the cause in 0x12 as u32');
	eq(tlv.pack(M.APDU.req, { status: 0, slot: 1, apdu_id: 7,
	                          info: { total: 2, offset: 0 }, response: [ 0x90, 0x00 ] }),
	   tl(0x01, struct.pack('<H', 0)) + tl(0x02, struct.pack('<I', 1)) + tl(0x03, struct.pack('<I', 7)) +
	   tl(0x10, struct.pack('<II', 2, 0)) + tl(0x11, struct.pack('<H', 2) + chr(0x90, 0x00)),
	   'wire: APDU answer = status u16, slot u32, id u32, {total, offset}, u16-length segment');

	let ind = tl(0x01, struct.pack('<I', 1)) + tl(0x02, struct.pack('<I', 42)) +
	          tl(0x03, struct.pack('<H', 7) + chr(0xA0, 0xA4, 0x00, 0x00, 0x02, 0x3F, 0x00));
	let d = tlv.unpack(M.APDU_IND.ind, ind);

	eq(d.slot, 1, 'wire: APDU indication slot');
	eq(d.apdu_id, 42, 'wire: APDU indication id');
	eq(d.command, [ 0xA0, 0xA4, 0x00, 0x00, 0x02, 0x3F, 0x00 ], 'wire: APDU indication command, u16 length');
	eq(M.APDU_IND.id, M.APDU.id, 'wire: the indication shares 0x0022 with the answer');

	eq(tlv.unpack(M.CARD_POWER_DOWN_IND.ind, tl(0x01, struct.pack('<I', 1)) + tl(0x10, struct.pack('<I', 1))).mode, 1,
	   'wire: power-down mode is 0x10 u32');
	eq(M.CONNECT_IND.id, 0x23, 'wire: connect 0x23');
	eq(M.DISCONNECT_IND.id, 0x24, 'wire: disconnect 0x24');
	eq(M.CARD_POWER_UP_IND.id, 0x25, 'wire: power-up 0x25');
	eq(M.CARD_POWER_DOWN_IND.id, 0x26, 'wire: power-down 0x26');
	eq(M.CARD_RESET_IND.id, 0x27, 'wire: reset 0x27');
	eq(rsim.UIMRMT.service, 0x32, 'wire: service 0x32');
}

// --- configuration -------------------------------------------------------------
{
	eq(rsim.cfg_of({}), null, 'cfg: no reader, no remote card');
	eq(rsim.cfg_of({ rsim_reader: '/dev/ttyUSB0' }), null, 'cfg: a reader needs its backend named');
	eq(rsim.cfg_of({ rsim_reader: 'phoenix:/dev/ttyUSB0' })?.slot, 1, 'cfg: slot 1 by default');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'phoenix:/dev/ttyUSB0', rsim_clock: '6000', rsim_reset: 'rts_inv' }), '/x'),
	   [ '/x', 'phoenix:/dev/ttyUSB0', '--clock', '6000', '--reset', 'rts_inv' ],
	   'cfg: Phoenix options reach the helper');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'pcsc:0', rsim_clock: '6000' }), '/x'),
	   [ '/x', 'pcsc:0' ], 'cfg: ...and are not handed to a PC/SC reader');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'wbsm:', rsim_clock: '3680', rsim_mode: 'smartmouse' }), '/x'),
	   [ '/x', 'wbsm:', '--clock', '3680', '--wbsm-mode', 'smartmouse' ],
	   'cfg: a Smartmouse USB gets its clock and mode, which the helper sets on the reader');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'phoenix:/dev/ttyUSB0', rsim_mode: 'smartmouse' }), '/x'),
	   [ '/x', 'phoenix:/dev/ttyUSB0' ], 'cfg: ...a plain Phoenix reader has no mode to set');

	// a reader on another machine
	eq(rsim.cfg_of({ rsim_reader: 'ssh:rsim@pc.lan:wbsm:' })?.local_reader, 'wbsm:', 'ssh: the remote reader');
	eq(rsim.cfg_of({ rsim_reader: 'ssh:rsim@pc.lan:/dev/ttyUSB0' }), null, 'ssh: the remote reader needs its backend too');
	eq(rsim.cfg_of({ rsim_reader: 'ssh:pc.lan:wbsm:' }), null, 'ssh: a user is required');
	eq(rsim.cfg_of({ rsim_reader: 'ssh:u@h;rm -rf /:wbsm:' }), null, 'ssh: a host is a host name, nothing else');

	let db = { flavor: 'dropbear', exists: (p) => true };
	let os = { flavor: 'openssh', exists: (p) => false };

	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'ssh:rsim@pc.lan:wbsm:', rsim_mode: 'phoenix' }), '/x', db),
	   [ '/usr/bin/ssh', '-T', '-y', '-K', '15', '-i', '/etc/wwand/rsim/id_dropbear',
	     'rsim@pc.lan', "'rsim-card' 'wbsm:' '--wbsm-mode' 'phoenix'" ],
	   'ssh: dropbear runs the helper over there, with the router\'s key and keepalives');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: "ssh:rsim@pc.lan:pcsc:ACS ACR38U 00 'x'",
	                                  rsim_ssh_port: '2222', rsim_ssh_helper: '/opt/rsim-card' }), '/x', os),
	   [ '/usr/bin/ssh', '-T', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new',
	     '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=3', '-p', '2222',
	     'rsim@pc.lan', "'/opt/rsim-card' 'pcsc:ACS ACR38U 00 '\\''x'\\'''" ],
	   'ssh: OpenSSH options, a port, a helper path, and a reader name with spaces and quotes kept one word');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'ssh:rsim@pc.lan:wbsm:', rsim_ssh_key: '/root/k' }), '/x', os)[10],
	   '-i', 'ssh: a configured key is used even when the default does not exist');

	let seg = rsim.segments([ 1, 2, 3 ]);

	eq(seg, [ { info: { total: 3, offset: 0 }, response: [ 1, 2, 3 ] } ], 'segments: a card answer is one segment');

	let big = [];

	for (let i = 0; i < 1500; i++)
		push(big, i & 0xff);

	seg = rsim.segments(big);
	eq(length(seg), 2, 'segments: over 1024 bytes it is split');
	eq(seg[1].info, { total: 1500, offset: 1024 }, 'segments: ...with the offset of each part');
}

// --- the session ------------------------------------------------------------------

// A scripted modem client: records what the plugin sends, lets the test fire
// indications. `refuse` makes EVENT fail.
function fake_client(opt)
{
	let c = { service: 0x32, cid: 9, destroyed: false, handlers: {}, sent: [], refuse: opt?.refuse };

	c.on = (name, cb) => { c.handlers[name] = c.handlers[name] ?? []; push(c.handlers[name], cb); };
	c.request = (name, args, cb, o) => {
		push(c.sent, { name: name, args: args });
		if (opt?.answer) {
			let a = opt.answer(name, args);

			return uloop.timer(opt?.delay ?? 0, () => cb(a?.__err ?? null, a ?? {}));
		}
		if (opt?.wire)
			push(opt.wire, sprintf('%s:%d', c.tag ?? '?', args?.info?.event ?? -1));
		uloop.timer(opt?.delay ?? 0, () => cb((c.refuse && name == 'EVENT') ? { error: 'qmi', code: 3 } : null, {}));
	};
	c.fire = (name, data) => { for (let h in (c.handlers[name] ?? [])) h(data); };
	c.events = () => map(filter(c.sent, (s) => s.name == 'EVENT'), (s) => s.args.info.event);

	return c;
}

// A simulated reader: answers the helper protocol from a tiny card model.
function fake_reader(card)
{
	let r = { lines: [], open: 0, closed: 0, on_line: null, on_exit: null, card: card };

	r.open_helper = (argv, on_line, on_exit) => {
		r.argv = argv;
		r.open++;
		r.on_line = on_line;
		r.on_exit = on_exit;

		return {
			write: (line) => {
				let req = json(line);

				push(r.lines, req);

				let ans;

				if (r.card.dead)
					return uloop.timer(0, () => on_exit());
				if (!r.card.present)
					ans = { ok: false, error: 'no_card' };
				else if (req.op == 'power_up' || req.op == 'reset')
					ans = { ok: true, atr: r.card.atr };
				else if (req.op == 'power_down')
					ans = { ok: true };
				else if (req.op == 'tpdu')
					ans = r.card.hang ? null : { ok: true, data: r.card.answer(req.data) };

				if (ans)
					uloop.timer((req.op == 'power_up') ? (r.card.up_delay ?? 0) : 0,
						() => on_line(sprintf('%J', ans)));
			},
			close: () => { r.closed++; },
		};
	};

	return r;
}

function run_for(ms)
{
	uloop.timer(ms, () => uloop.end());
	uloop.run();
}

const ATR = '3B9F96801FC78031A073BE21136743200718000001A5';

let card_model = () => ({
	present: true, atr: ATR, hang: false,
	answer: (d) => (substr(d, 0, 10) == 'A0A4000002') ? '9F17' : '9000',
});

let changes = [];
let mk = (reader, client_of, t) => rsim.create({
	log: (l, m) => null,
	sim_changed: (ref, why) => push(changes, why),
	open_helper: reader.open_helper,
	helper_path: '/usr/lib/wwand/rsim-card',
	modem_of: (ref) => ({ modem: {} }),
	qmi_client: client_of,
	qmi_release: (ref, c) => { c.released = true; },
	now: () => t.now,
});

const EXT = { rsim_reader: 'phoenix:/dev/ttyUSB0' };

// the whole sequence
{
	let t = { now: 1000 };
	let reader = fake_reader(card_model());
	let client = fake_client();
	let p = mk(reader, (ref, schema, cb) => { client.schema = schema; cb(null, client); }, t);

	p.tick('m0', EXT);
	run_for(20);

	eq(reader.lines[0]?.op, 'power_up', 'session: the card is powered before the modem is asked for anything');
	eq(client.schema?.service, 0x32, 'session: the UIM Remote client is asked for');
	eq(length(filter(client.sent, (x) => x.name == 'RESET')), 0,
	   'session: no RESET — the modem would drop the client it came from, and the offer fails');
	eq(client.sent[0]?.name, 'EVENT', 'session: the offer is the first thing the modem hears');
	eq(client.events(), [ 1 ],
	   'session: connection available, and nothing more until the modem connects');

	eq(changes, [], 'card change: nothing before the modem has taken the remote card');

	client.fire('CONNECT_IND', { slot: 1 });
	run_for(20);

	eq(changes, [ 'remote SIM in use' ],
	   'card change: once the modem connects, wwand re-reads the SIM (the slot-switch process)');

	let last = client.sent[length(client.sent) - 1];

	eq(last.args?.info?.event, 5, 'session: on connect the card is reset...');

	// a card put into the reader later goes to the modem WITH its ATR
	reader.on_line('{"event":"inserted"}');
	run_for(20);

	let ins = client.sent[length(client.sent) - 1];

	eq(ins.args?.info?.event, 2, 'session: a card inserted in the reader is reported...');
	eq(rsim.hexs(ins.args?.atr), ATR, '...with its ATR, which the modem requires for that event');
	eq(rsim.hexs(last.args?.atr), ATR, '...and its ATR goes to the modem');

	client.fire('APDU_IND', { slot: 1, apdu_id: 11, command: rsim.bytes('A0A40000023F00') });
	client.fire('APDU_IND', { slot: 1, apdu_id: 12, command: rsim.bytes('A0C0000017') });
	run_for(20);

	let answers = filter(client.sent, (s) => s.name == 'APDU');

	eq(length(answers), 2, 'session: every command is answered');
	eq(answers[0].args.apdu_id, 11, 'session: in the order the modem sent them');
	eq(rsim.hexs(answers[0].args.response), '9F17', 'session: with what the card said, 9Fxx included (the modem does GET RESPONSE)');
	eq(answers[0].args.info, { total: 2, offset: 0 }, 'session: in one segment');
	eq(answers[0].args.status, 0, 'session: status success');
	eq(map(filter(reader.lines, (l) => l.op == 'tpdu'), (l) => l.data), [ 'A0A40000023F00', 'A0C0000017' ],
	   'session: the TPDUs reach the card unchanged');

	client.fire('APDU_IND', { slot: 2, apdu_id: 13, command: rsim.bytes('A0F2000016') });
	run_for(20);
	eq(length(filter(client.sent, (s) => s.name == 'APDU')), 2, 'session: another slot\'s command is not ours');

	client.fire('CARD_POWER_DOWN_IND', { slot: 1, mode: 1 });
	run_for(20);
	eq(reader.lines[length(reader.lines) - 1].op, 'power_down', 'session: power-down reaches the card');

	client.fire('CARD_POWER_UP_IND', { slot: 1 });
	run_for(20);
	eq(client.sent[length(client.sent) - 1].args?.info?.event, 5, 'session: after power-up the ATR goes again');

	let st;

	eq(p.status('m0', EXT), { label: 'remote SIM', text: 'phoenix:/dev/ttyUSB0 · in use by the modem · 2 commands', level: 'ok' },
	   'status row: in use, with the commands served');
	eq(p.status('m0', {}), null, 'status row: none for a modem without a reader');

	p.ops.status('m0', EXT, {}, (e, r) => { st = r; });
	eq(st.state, 'powered', 'status: in use');
	eq(st.apdus, 2, 'status: two commands served');
	eq(st.last_sw, '9000', 'status: the last status word');

	// the reader removed from the configuration: the modem gets its own card back
	let before = length(client.sent);

	p.tick('m0', {});
	run_for(20);

	let tail = map(slice(client.sent, before), (s) => s.args?.info?.event);

	eq(tail, [ 3, 0 ], 'leave: card removed, then connection unavailable — the modem goes back to its own SIM');

	// ...and the modem answers that on the old client before it is released
	let died = null;

	try {
		client.fire('CARD_POWER_DOWN_IND', { slot: 1, mode: 1 });
		client.fire('CARD_POWER_UP_IND', { slot: 1 });
		client.fire('APDU_IND', { slot: 1, apdu_id: 99, command: rsim.bytes('A0A40000023F00') });
		run_for(20);
	} catch (e) { died = e.message; }
	eq(died, null, 'leave: indications for an ended session are ignored, not a crash of the daemon');
	ok(client.released, 'leave: the client is given back to the modem');
	eq(changes, [ 'remote SIM in use', 'remote SIM off, own card back' ],
	   'card change: and again once the modem has its own card back');
	changes = [];
	eq(reader.closed, 1, 'leave: the helper is closed');
}

// --- review findings (Codex, 2026-09-26) ------------------------------------------

// A reader change while the old session's goodbye is still on its way: the old
// connection-unavailable must reach the modem BEFORE the new offer, or it
// withdraws the new session's card.
{
	let t = { now: 1000 };
	let reader = fake_reader(card_model());
	let wire = [];
	let n = 0;
	let p = mk(reader, (ref, schema, cb) => {
		let c = fake_client({ delay: 30, wire: wire });

		c.tag = sprintf('c%d', ++n);
		cb(null, c);
	}, t);

	p.tick('m0', EXT);
	run_for(80);
	p.tick('m0', { rsim_reader: 'phoenix:/dev/ttyUSB1' });
	p.tick('m0', { rsim_reader: 'phoenix:/dev/ttyUSB1' });
	run_for(150);
	p.tick('m0', { rsim_reader: 'phoenix:/dev/ttyUSB1' });
	run_for(150);

	let old_off = index(wire, 'c1:0'), new_on = index(wire, 'c2:1');

	ok(old_off >= 0 && new_on > old_off,
	   'teardown: the old connection-unavailable goes out before the new offer, not after it');
}

// a power-up still on its way when the modem powers the card down: its ATR
// must not go to the modem afterwards
{
	let t = { now: 1000 };
	let cm = card_model();
	let reader = fake_reader(cm);
	let client = fake_client();
	let p = mk(reader, (ref, schema, cb) => cb(null, client), t);

	p.tick('m0', EXT);
	run_for(20);
	client.fire('CONNECT_IND', { slot: 1 });
	run_for(20);

	cm.up_delay = 50;
	let before = length(client.sent);

	client.fire('CARD_POWER_UP_IND', { slot: 1 });
	client.fire('CARD_POWER_DOWN_IND', { slot: 1, mode: 1 });
	run_for(120);

	eq(filter(slice(client.sent, before), (x) => x.name == 'EVENT' && x.args.info.event == 5), [],
	   'stale power-up: no card-reset/ATR after the modem has powered the card down');

	let st;

	p.ops.status('m0', EXT, {}, (e, r) => { st = r; });
	eq(st.state, 'connected', 'stale power-up: ...and the state stays what the last request left');

	// a disconnect powers the card down too, not only our state
	cm.up_delay = 0;
	client.fire('CARD_POWER_UP_IND', { slot: 1 });
	run_for(20);
	client.fire('DISCONNECT_IND', { slot: 1 });
	run_for(20);
	eq(reader.lines[length(reader.lines) - 1].op, 'power_down', 'disconnect: the card is powered down');
}

// ...nor an APDU answer that comes back after a disconnect, nor a card
// inserted in the reader whose power-up the modem has overtaken
{
	let t = { now: 1000 };
	let cm = card_model();
	let reader = fake_reader(cm);
	let client = fake_client();
	let p = mk(reader, (ref, schema, cb) => cb(null, client), t);

	p.tick('m0', EXT);
	run_for(20);
	client.fire('CONNECT_IND', { slot: 1 });
	run_for(20);

	let slow = reader.open_helper;
	let before = length(client.sent);

	cm.hang = true;   // the TPDU is on the card and does not come back yet
	client.fire('APDU_IND', { slot: 1, apdu_id: 77, command: rsim.bytes('A0B0000002') });
	run_for(10);
	client.fire('DISCONNECT_IND', { slot: 1 });
	cm.hang = false;
	reader.on_line('{"ok":true,"data":"12349000"}');   // the late answer
	run_for(20);
	eq(filter(slice(client.sent, before), (x) => x.name == 'APDU'), [],
	   'stale apdu: an answer after the disconnect is not sent to the modem');

	cm.up_delay = 50;
	before = length(client.sent);
	reader.on_line('{"event":"inserted"}');
	run_for(10);
	client.fire('CARD_POWER_DOWN_IND', { slot: 1, mode: 1 });
	run_for(120);
	eq(filter(slice(client.sent, before), (x) => x.name == 'EVENT' && x.args.info.event == 2), [],
	   'stale insert: no card-inserted after the modem has powered the card down');
}

// --- another modem's card (donor) -------------------------------------------------

// A scripted donor UIM with the SIM Access Profile server: connect, then the
// state becomes 2 (connected); ATR, APDU, power and reset through SAP_REQUEST.
function sap_donor(log)
{
	let st = 0;

	return fake_client({ wire: log, answer: (name, a) => {
		if (name == 'SAP_CONNECTION') {
			if (a.conn.op == 1) st = 2;
			if (a.conn.op == 0) st = 5;
			return { state: st };
		}
		if (name == 'SAP_REQUEST') {
			if (st != 2) return { __err: { error: 'qmi', code: 1 } };
			if (a.req.op == 0) return { atr: rsim.bytes(ATR) };
			if (a.req.op == 1) return { rapdu: rsim.bytes((a.apdu[1] == 0xA4) ? '9F17' : '9000') };
			return {};
		}
		return {};
	} });
}

{
	let t = { now: 1000 };
	let target = fake_client();
	let donor_log = [];
	let donor = sap_donor(donor_log);
	let p = rsim.create({
		log: (l, m) => null, sim_changed: () => null,
		modem_of: () => ({ modem: {} }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : target),
		qmi_release: (ref, c) => { c.released = true; },
		now: () => t.now,
	});
	let ext = { rsim_reader: 'modem:m1' };

	eq(rsim.cfg_of(ext).donor, { ref: 'm1', mode: 'sap', slot: 1, cond: null, apdu: 'auto' }, 'donor: SIM Access Profile by default, slot 1');

	p.tick('m0', ext);
	run_for(1200);

	let donor_ops = map(filter(donor.sent, (x) => x.name == 'SAP_CONNECTION'), (x) => x.args.conn.op);

	eq(donor_ops[0], 1, 'donor: the donor is asked to lend its card (SAP connect)');
	ok(length(filter(donor.sent, (x) => x.name == 'SAP_REQUEST' && x.args.req.op == 0)) >= 1,
	   'donor: its ATR is read through the link');
	eq(target.events(), [ 1 ], 'donor: the card is offered to the target modem');

	target.fire('CONNECT_IND', { slot: 1 });
	run_for(50);
	target.fire('APDU_IND', { slot: 1, apdu_id: 5, command: rsim.bytes('A0A40000023F00') });
	run_for(50);

	let ans = filter(target.sent, (x) => x.name == 'APDU');

	eq(rsim.hexs(ans[0]?.args?.response), '9F17', 'donor: the target\'s command is answered by the donor\'s card');
	ok(length(filter(donor.sent, (x) => x.name == 'SAP_REQUEST' && x.args.req.op == 1 &&
	                                    rsim.hexs(x.args.apdu) == 'A0A40000023F00')) == 1,
	   'donor: ...sent unchanged through SAP_REQUEST');

	p.tick('m0', {});
	run_for(50);

	let last = donor.sent[length(donor.sent) - 1];

	eq([ last?.name, last?.args?.conn?.op, last?.args?.mode ], [ 'SAP_CONNECTION', 0, 1 ],
	   'donor: leaving hands the card back gracefully');
	ok(donor.released, 'donor: ...and releases the donor client');

	// a modem lending to itself is refused
	p.tick('m1', { rsim_reader: 'modem:m1' });
	run_for(20);
	ok(index(p.status('m1', { rsim_reader: 'modem:m1' })?.text ?? '', 'itself') >= 0,
	   'donor: a modem cannot lend its card to itself');
}

// A donor that carries out the connect but never answers it (the E392 on
// 245): the link must be ended before the client goes back, or the card stays
// lent to nobody and the next retry stacks another connect on top.
{
	let t = { now: 1000 };
	let donor = fake_client({ answer: (name, a) =>
		(name == 'SAP_CONNECTION' && a.conn.op == 1) ? { __err: { error: 'timeout' } } : {} });
	let p = rsim.create({
		log: (l, m) => null, sim_changed: () => null,
		modem_of: () => ({ modem: {} }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : fake_client()),
		qmi_release: (ref, c) => { c.released = true; push(c.sent, { name: 'RELEASED' }); },
		now: () => t.now,
	});

	p.tick('m0', { rsim_reader: 'modem:m1' });
	run_for(50);

	let seq = map(donor.sent, (x) => (x.name == 'SAP_CONNECTION') ? sprintf('SAP:%d', x.args.conn.op) : x.name);
	let i_conn = index(seq, 'SAP:1'), i_disc = index(seq, 'SAP:0'), i_rel = index(seq, 'RELEASED');

	ok(i_conn >= 0 && i_disc > i_conn && i_rel > i_disc,
	   'donor: a connect that went unanswered is ended (disconnect) before the client is given back');
	eq(donor.sent[i_disc]?.args?.mode, 0, 'donor: ...immediately, so the donor gets its card back now');

	let n = length(filter(donor.sent, (x) => x.name == 'SAP_CONNECTION' && x.args.conn.op == 1));

	t.now += 1000;
	p.tick('m0', { rsim_reader: 'modem:m1' });
	run_for(50);
	eq(length(filter(donor.sent, (x) => x.name == 'SAP_CONNECTION' && x.args.conn.op == 1)), n,
	   'donor: an unanswered connect is not retried on its own — each retry costs the donor its registration');
	ok(index(p.status('m0', { rsim_reader: 'modem:m1' })?.text ?? '', 'rsim_donor_mode apdu') >= 0,
	   'donor: ...and the status says what to use instead');

	p.ops.restart('m0', {}, {}, () => null);
	p.tick('m0', { rsim_reader: 'modem:m1' });
	run_for(50);
	ok(length(filter(donor.sent, (x) => x.name == 'SAP_CONNECTION' && x.args.conn.op == 1)) > n,
	   'donor: `wwandctl rsim restart` tries again');
}

// The donor ends the link while an answer is still on its way: the late
// answer must find no client and do nothing — a member call on null inside a
// uloop callback would end the whole daemon.
{
	let t = { now: 1000 };
	let donor_log = [];
	let donor = sap_donor(donor_log);
	let target = fake_client();
	let slow = false;
	let req = donor.request;

	// power-up answers late; the link breaks before it does
	donor.request = (name, a, cb, o) => (slow && name == 'SAP_REQUEST')
		? uloop.timer(60, () => cb(null, {}))
		: req(name, a, cb, o);

	let p = rsim.create({
		log: (l, m) => null, sim_changed: () => null,
		modem_of: () => ({ modem: {} }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : target),
		qmi_release: (ref, c) => null,
		now: () => t.now,
	});

	p.tick('m0', { rsim_reader: 'modem:m1' });
	run_for(1200);
	slow = true;
	target.fire('CARD_POWER_UP_IND', { slot: 1 });
	run_for(10);
	donor.fire('SAP_CONNECTION_IND', { st: { state: 5, slot: 1 } });

	let died = null;

	try { run_for(150); } catch (e) { died = e.message; }
	eq(died, null, 'donor: an answer that arrives after the link ended does nothing, and no crash');
	ok(index(p.status('m0', { rsim_reader: 'modem:m1' })?.text ?? '', 'ended the SIM Access link') >= 0,
	   'donor: the status says the donor ended the link');
}

// torn down while the event registration is still on its way: the late
// answer must not send a connect on a released client (nor crash)
{
	let t = { now: 1000 };
	let held = null;
	let donor = sap_donor([]);
	let req = donor.request;

	donor.request = (name, a, cb, o) => (name == 'REGISTER_EVENTS') ? (held = cb) : req(name, a, cb, o);

	let p = rsim.create({
		log: (l, m) => null, sim_changed: () => null,
		modem_of: () => ({ modem: {} }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : fake_client()),
		qmi_release: (ref, c) => null,
		now: () => t.now,
	});

	p.tick('m0', { rsim_reader: 'modem:m1' });
	run_for(20);
	p.tick('m0', {});
	run_for(20);

	let died = null;

	try { held?.(null, {}); run_for(20); } catch (e) { died = e.message; }
	eq(died, null, 'donor: a registration answer after teardown does nothing, and no crash');
	eq(filter(donor.sent, (x) => x.name == 'SAP_CONNECTION' && x.args.conn.op == 1), [],
	   'donor: ...and no connect goes out on the released client');
}

// the APDU fallback: ATR and APDUs through the donor's UIM, no SAP link
{
	let t = { now: 1000 };
	let target = fake_client();
	let donor = fake_client({ answer: (name, a) =>
		(name == 'GET_ATR') ? { atr: rsim.bytes(ATR) }
		: (name == 'SEND_APDU') ? { response: rsim.bytes('9000') } : {} });
	let p = rsim.create({
		log: (l, m) => null, sim_changed: () => null,
		modem_of: () => ({ modem: {} }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : target),
		qmi_release: (ref, c) => { c.released = true; },
		now: () => t.now,
	});
	let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu' };

	p.tick('m0', ext);
	run_for(50);
	target.fire('CONNECT_IND', { slot: 1 });
	run_for(50);
	target.fire('APDU_IND', { slot: 1, apdu_id: 6, command: rsim.bytes('A0B0000002') });
	run_for(50);

	eq(filter(donor.sent, (x) => x.name == 'SAP_CONNECTION'), [], 'apdu donor: no SIM Access link');
	ok(length(filter(donor.sent, (x) => x.name == 'GET_ATR')) >= 1, 'apdu donor: the ATR the donor reports');
	eq(rsim.hexs(filter(donor.sent, (x) => x.name == 'SEND_APDU')[0]?.args?.apdu), 'A0B0000002',
	   'apdu donor: the command goes through SEND_APDU unchanged');
	eq(rsim.hexs(filter(target.sent, (x) => x.name == 'APDU')[0]?.args?.response), '9000',
	   'apdu donor: and its answer back to the target');
}

// the AT channel: AT+CSIM (TS 27.007 §8.17), length in hex characters
{
	eq(rsim.csim_cmd('A0A40000023F00'), 'AT+CSIM=14,"A0A40000023F00"', 'csim: the length counts hex characters');
	eq(rsim.csim_answer([ '+CSIM: 4,"9F17"', 'OK' ]), '9F17', 'csim: the answer');
	eq(rsim.csim_answer([ '+CSIM: 6,"9F17"' ]), null, 'csim: a length that does not match is not an answer');

	// a donor whose UIM refuses SEND_APDU (auto): the AT channel from then on;
	// and one with no QMI UIM at all: AT from the start, with the minimal ATR
	let mk_at = (at_log) => ({ send: (cmd, cb) => {
		push(at_log, cmd);
		uloop.timer(0, () => cb(null, { lines: [ '+CSIM: 4,"9000"', 'OK' ] }));
	} });

	for (let variant in [ 'refuses', 'no_qmi' ]) {
		let t = { now: 1000 };
		let target = fake_client();
		let at_log = [];
		let donor = fake_client({ answer: (name) =>
			(name == 'SEND_APDU') ? { __err: { error: 'qmi', code: 71 } } : (name == 'GET_ATR') ? { atr: rsim.bytes(ATR) } : {} });
		let p = rsim.create({
			log: (l, m) => null, sim_changed: () => null,
			modem_of: (ref) => ({ modem: (ref == 'm1') ? { at: mk_at(at_log) } : {} }),
			qmi_client: (ref, schema, cb) => (ref == 'm1')
				? ((variant == 'no_qmi') ? cb({ error: 'unsupported' }, null) : cb(null, donor))
				: cb(null, target),
			qmi_release: (ref, c) => null,
			now: () => t.now,
		});

		p.tick('m0', { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu' });
		run_for(50);
		target.fire('CONNECT_IND', { slot: 1 });
		run_for(50);

		let ev = filter(target.sent, (x) => x.name == 'EVENT' && x.args.info.event == 5)[0];

		eq(rsim.hexs(ev?.args?.atr), (variant == 'no_qmi') ? '3B00' : ATR,
		   sprintf('at donor (%s): the ATR the donor reports, or the minimal T=0 one', variant));

		target.fire('APDU_IND', { slot: 1, apdu_id: 9, command: rsim.bytes('A0B0000002') });
		run_for(50);

		eq(at_log, [ 'AT+CSIM=10,"A0B0000002"' ], sprintf('at donor (%s): the command goes over AT+CSIM', variant));
		eq(rsim.hexs(filter(target.sent, (x) => x.name == 'APDU')[0]?.args?.response), '9000',
		   sprintf('at donor (%s): and its answer back to the target', variant));
	}
}

// slots are 1..3
{
	eq(rsim.cfg_of({ rsim_reader: 'pcsc:0', rsim_slot: '0' }).slot, 1, 'slot 0 is not a slot the service serves');
	eq(rsim.cfg_of({ rsim_reader: 'pcsc:0', rsim_slot: '2' }).slot, 2, 'slot 2 is');
}

// every quote, not only the first (OpenWrt ucode replace() with /g is global;
// this pins it, since a string pattern would not be)
{
	let a = rsim.helper_argv(rsim.cfg_of({ rsim_reader: "ssh:u@h:pcsc:a'b'c;x" }), '/x', { flavor: 'openssh', exists: () => false });

	eq(a[length(a) - 1], "'rsim-card' 'pcsc:a'\\''b'\\''c;x'", 'quoting: two quotes and a metacharacter stay one word');
}

// no card in the reader: the modem's own SIM is never taken away
{
	let t = { now: 1000 };
	let cm = card_model();

	cm.present = false;

	let reader = fake_reader(cm);
	let asked = 0;
	let p = mk(reader, (ref, schema, cb) => { asked++; cb(null, fake_client()); }, t);

	p.tick('m0', EXT);
	run_for(20);

	eq(asked, 0, 'no card: the modem is not asked for the service at all');

	let st;

	p.ops.status('m0', EXT, {}, (e, r) => { st = r; });
	ok(index(st.last_error ?? '', 'no card') >= 0, 'no card: the status says so');
	eq(p.status('m0', EXT)?.level, 'error', 'status row: a failure shows as an error...');
	ok(index(p.status('m0', EXT)?.text ?? '', 'retry in') >= 0, '...with when it tries again');

	cm.present = true;
	p.tick('m0', EXT);
	run_for(20);
	eq(asked, 0, 'no card: nothing again before the backoff has passed');

	t.now += 11;
	p.tick('m0', EXT);
	run_for(20);
	eq(asked, 1, 'no card: tried again after the backoff, and the card is found');
}

// a reader the helper cannot open is not "no card"
{
	let t = { now: 1000 };
	let cm = card_model();

	cm.dead = true;

	let p = mk(fake_reader(cm), (ref, schema, cb) => cb(null, fake_client()), t);

	p.tick('m0', EXT);
	run_for(20);
	ok(index(p.status('m0', EXT)?.text ?? '', 'cannot use the reader') >= 0,
	   'dead helper: says the reader could not be used, not that there is no card');
}

// a modem without the service
{
	let t = { now: 1000 };
	let reader = fake_reader(card_model());
	let p = mk(reader, (ref, schema, cb) => cb({ error: 'service_unavailable' }, null), t);
	let st;

	p.tick('m0', EXT);
	run_for(20);
	p.ops.status('m0', EXT, {}, (e, r) => { st = r; });
	ok(index(st.last_error ?? '', 'wwandctl rsim enable') >= 0,
	   'service missing: the status names the command that switches it on');
	eq(reader.closed, 1, 'service missing: the helper is not left running');
}

// the modem restarts: its teardown destroyed our client
{
	let t = { now: 1000 };
	let reader = fake_reader(card_model());
	let clients = [];
	let p = mk(reader, (ref, schema, cb) => { let c = fake_client(); push(clients, c); cb(null, c); }, t);

	p.tick('m0', EXT);
	run_for(20);
	clients[0].destroyed = true;
	p.tick('m0', EXT);
	run_for(20);
	p.tick('m0', EXT);
	run_for(20);

	eq(length(clients), 2, 'modem restart: a new client, the card offered again');
	eq(clients[1].events(), [ 1 ], 'modem restart: ...offered again');
}

// a card that stops answering: the command fails and the modem hears about it
{
	let t = { now: 1000 };
	let cm = card_model();
	let reader = fake_reader(cm);
	let client = fake_client();
	let p = mk(reader, (ref, schema, cb) => cb(null, client), t);

	p.tick('m0', EXT);
	run_for(20);
	client.fire('CONNECT_IND', { slot: 1 });
	run_for(20);

	cm.answer = (d) => '9';   // a response too short to carry a status word
	client.fire('APDU_IND', { slot: 1, apdu_id: 21, command: rsim.bytes('A0B0000010') });
	run_for(20);

	let a = filter(client.sent, (s) => s.name == 'APDU');

	eq(a[0]?.args?.status, 1, 'bad answer: the command is answered with status failure');
	eq(a[0]?.args?.response, null, 'bad answer: ...and no response bytes');
	eq(client.sent[length(client.sent) - 1].args?.info?.event, 4, 'bad answer: and a card error event follows');
}

// the helper dies: the session goes, the modem gets its SIM back, a retry follows
{
	let t = { now: 1000 };
	let reader = fake_reader(card_model());
	let client = fake_client();
	let p = mk(reader, (ref, schema, cb) => cb(null, client), t);

	p.tick('m0', EXT);
	run_for(20);
	reader.on_exit();
	run_for(20);

	eq(client.events()[length(client.events()) - 1], 0,
	   'helper exit: connection unavailable, so the modem does not wait on a card that is gone');

	t.now += 11;
	p.tick('m0', EXT);
	run_for(20);
	eq(reader.open, 2, 'helper exit: started again after the backoff');
}

done('test_rsim');
