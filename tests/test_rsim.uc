// wwand-rsim tests — the plugin: the UIM Remote wire format, and the session
// with the modem and the card, against a scripted modem client and a
// simulated card reader.

'use strict';

import * as uloop from 'uloop';
import * as struct from 'struct';
import * as tlv from 'wwand.codec.tlv';
import { eq, ok, done } from './lib/check.uc';

let rsim = require('wwand.plugins.rsim');

// one modem object per name, like the daemon's: the plugin tells a restarted
// modem by a different object, so a stub handing out a fresh one per call
// would look like a modem restarting on every look
let modems_by_ref = {};
let modem_obj = (ref) => (modems_by_ref[ref ?? '?'] ??= { state: 'READY' });
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

	// the SIM of a modem that is not wwand's, over its AT port
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'at:/dev/ttyUSB2' }), '/x'), [ '/x', 'at:/dev/ttyUSB2' ],
	   'cfg at: the modem\'s AT port; its radio goes off by the helper\'s default');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'at:/dev/ttyUSB2', rsim_at_radio: 'keep', rsim_at_baud: '9600',
	                                  rsim_clock: '6000' }), '/x'),
	   [ '/x', 'at:/dev/ttyUSB2', '--at-radio', 'keep', '--at-baud', '9600' ],
	   'cfg at: radio and baud reach the helper, the reader\'s clock does not');
	eq(rsim.cfg_of({ rsim_reader: 'ssh:root@simhost:at:/dev/ttyUSB2' })?.local_reader, 'at:/dev/ttyUSB2',
	   'cfg at: on a SIM host over SSH too');
	eq(rsim.reader_options({ type: 'at', device: '/dev/ttyUSB3', host: 'root@simhost', radio: 'keep' }).rsim_reader,
	   'ssh:root@simhost:at:/dev/ttyUSB3', 'reader at: a named one on another router');
	ok(index(rsim.reader_options({ type: 'at' }).error ?? '', 'AT port') >= 0, 'reader at: without its device, refused');

	// a paired phone's SIM over the Bluetooth SIM Access Profile
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'bt:00:11:22:AA:BB:CC' }), '/x'), [ '/x', 'bt:00:11:22:AA:BB:CC' ],
	   'cfg bt: the phone\'s address; its channel from SDP by the helper\'s default');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'bt:00:11:22:AA:BB:CC', rsim_bt_channel: '8', rsim_bt_security: 'high',
	                                  rsim_bt_apdu: '7816', rsim_at_radio: 'keep', rsim_clock: '6000' }), '/x'),
	   [ '/x', 'bt:00:11:22:AA:BB:CC', '--bt-channel', '8', '--bt-security', 'high', '--bt-apdu', '7816' ],
	   'cfg bt: channel, security and APDU format reach the helper, other readers\' options do not');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'bt:00:11:22:AA:BB:CC', rsim_bt_channel: '99', rsim_bt_security: 'x' }), '/x'),
	   [ '/x', 'bt:00:11:22:AA:BB:CC' ], 'cfg bt: values out of range are left out');
	eq(rsim.cfg_of({ rsim_reader: 'ssh:u@pc.lan:bt:00:11:22:AA:BB:CC' })?.local_reader, 'bt:00:11:22:AA:BB:CC',
	   'cfg bt: a phone paired with a SIM host, over SSH');
	let rbt = rsim.reader_options({ type: 'bt', device: '00:11:22:aa:bb:cc', host: 'u@pc.lan', channel: '8' });
	eq([ rbt.rsim_reader, rbt.rsim_bt_channel ], [ 'ssh:u@pc.lan:bt:00:11:22:aa:bb:cc', '8' ], 'reader bt: a named phone on a SIM host');
	ok(index(rsim.reader_options({ type: 'bt', device: 'phone' }).error ?? '', 'Bluetooth address') >= 0,
	   'reader bt: a device that is no address, refused');

	// a card in an osmo-remsim SIM bank: the helper is the remsim client
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'rspro:bank.lan' }), '/x'), [ '/x', 'rspro:bank.lan' ],
	   'cfg rspro: the server; client 0:0 and the REST port by the helper\'s default');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'rspro:[fd00::1]:9998/1:4', rsim_rspro_client: '12:1',
	                                  rsim_rspro_rest_port: '8997', rsim_clock: '6000' }), '/x'),
	   [ '/x', 'rspro:[fd00::1]:9998/1:4', '--rspro-client', '12:1', '--rspro-rest-port', '8997' ],
	   'cfg rspro: a bank slot, the client and the REST port reach the helper, other readers\' options do not');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'rspro:bank.lan', rsim_rspro_client: '1:x', rsim_rspro_rest_port: '0' }), '/x'),
	   [ '/x', 'rspro:bank.lan' ], 'cfg rspro: values out of range are left out');
	eq(rsim.cfg_of({ rsim_reader: 'rspro:bank.lan;reboot' }), null, 'cfg rspro: a server is a host name or address');
	eq(rsim.cfg_of({ rsim_reader: 'rspro:bank.lan/1' }), null, 'cfg rspro: a bank slot is <bank>:<slot>');
	eq([ rsim.cfg_of({ rsim_reader: 'rspro:bank.lan:0' }), rsim.cfg_of({ rsim_reader: 'rspro:bank.lan:99999' }),
	     rsim.cfg_of({ rsim_reader: 'rspro:bank.lan/70000:0' }) ], [ null, null, null ],
	   'cfg rspro: numbers rsim-card would refuse (a port 1..65535, bank and slot 0..65535)');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'rspro:bank.lan', rsim_rspro_client: '70000' }), '/x'), [ '/x', 'rspro:bank.lan' ],
	   'cfg rspro: a client id over 65535 is left out');
	eq(rsim.cfg_of({ rsim_reader: 'ssh:u@h:rspro:bank.lan' }), null, 'cfg rspro: not over SSH');
	let rb = rsim.reader_options({ type: 'rspro', device: 'bank.lan:9998', bank: '2:7', client: '5' });
	eq([ rb.rsim_reader, rb.rsim_rspro_client ], [ 'rspro:bank.lan:9998/2:7', '5' ], 'reader rspro: a named bank slot');
	eq(rsim.reader_options({ type: 'rspro', device: 'bank.lan' }).rsim_reader, 'rspro:bank.lan',
	   'reader rspro: without a bank slot, whatever the server maps to us');
	ok(index(rsim.reader_options({ type: 'rspro' }).error ?? '', 'remsim-server') >= 0, 'reader rspro: without a server, refused');
	ok(index(rsim.reader_options({ type: 'rspro', device: 'bank.lan', bank: 'x' }).error ?? '', 'bank') >= 0,
	   'reader rspro: a bank slot that is none, refused');
	ok(index(rsim.reader_options({ type: 'rspro', device: 'bank.lan', host: 'u@h' }).error ?? '', 'SSH') >= 0,
	   'reader rspro: not over SSH');

	// a modem's card on another wwand router, lent by its wwand-rsim
	let pc = rsim.cfg_of({ rsim_reader: 'ssh:root@simhost:wwand:wwmodem0' });

	eq([ pc?.local_reader, pc?.proxy ], [ 'wwand:wwmodem0', { mode: 'auto', slot: null, cond: null, apdu: null } ],
	   'cfg wwand: a modem on another router, its proxy decides how by default');
	eq(rsim.helper_argv(pc, '/x', { flavor: 'dropbear', exists: () => true })[8],
	   "'wwandctl' 'rsim' 'proxy' 'wwmodem0'", 'cfg wwand: over SSH, the proxy there instead of rsim-card (no --mode: it decides)');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'ssh:root@simhost:wwand:wwmodem0', rsim_donor_mode: 'sap' }),
	                   '/x', { flavor: 'dropbear', exists: () => true })[8],
	   "'wwandctl' 'rsim' 'proxy' 'wwmodem0' '--mode' 'sap'", 'cfg wwand: SIM Access set explicitly, passed on');
	eq(rsim.helper_argv(rsim.cfg_of({ rsim_reader: 'ssh:root@simhost:wwand:iccid:89490200001022832490',
	                                  rsim_donor_mode: 'apdu', rsim_donor_slot: '2', rsim_donor_apdu: 'at', rsim_ssh_helper: '/opt/x' }),
	                   '/x', { flavor: 'dropbear', exists: () => true })[8],
	   "'wwandctl' 'rsim' 'proxy' 'iccid:89490200001022832490' '--mode' 'apdu' '--slot' '2' '--apdu' 'at'",
	   'cfg wwand: by ICCID, with the sponsor options; the rsim-card path does not apply');
	eq(rsim.cfg_of({ rsim_reader: 'wwand:wwmodem0' }), null, 'cfg wwand: only over SSH (here it is modem:<name>)');
	eq(rsim.cfg_of({ rsim_reader: 'ssh:root@simhost:wwand:m0;reboot' }), null, 'cfg wwand: a modem name is a name');
	eq(rsim.reader_options({ type: 'wwand', host: 'root@simhost', device: 'iccid:89490200001022832490', donor_mode: 'apdu' }).rsim_reader,
	   'ssh:root@simhost:wwand:iccid:89490200001022832490', 'reader wwand: a named one');
	ok(index(rsim.reader_options({ type: 'wwand', device: 'm0' }).error ?? '', 'host') >= 0, 'reader wwand: needs its host');
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
let mk = (reader, client_of, t, more) => rsim.create({
	...(more ?? {}),
	log: (l, m) => null,
	sim_changed: (ref, why) => push(changes, why),
	open_helper: reader.open_helper,
	helper_path: '/usr/lib/wwand/rsim-card',
	modem_of: (ref) => ({ modem: modem_obj(ref) }),
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
	eq(changes, [ 'remote SIM in use', 'card inserted in the reader' ],
	   'card change: a card inserted in the reader may be another one — the modem reads it again');
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
	eq(changes, [ 'remote SIM in use', 'card inserted in the reader', 'remote SIM off, own card back' ],
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
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : target),
		qmi_release: (ref, c) => { c.released = true; },
		now: () => t.now,
	});
	let ext = { rsim_reader: 'modem:m1' };

	eq(rsim.cfg_of(ext).donor, { ref: 'm1', mode: 'auto', slot: null, cond: null, apdu: 'auto' },
	   'donor: SIM Access Profile by default (auto), the slot the donor runs on');

	p.tick('m0', ext);
	run_for(1200);

	let donor_ops = map(filter(donor.sent, (x) => x.name == 'SAP_CONNECTION'), (x) => x.args.conn.op);

	eq(slice(donor_ops, 0, 2), [ 2, 1 ],
	   'donor: a link left from before is looked for first, then the donor is asked to lend its card (SAP connect)');
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
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
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
	ok(index(p.status('m0', { rsim_reader: 'modem:m1' })?.text ?? '', 'APDU mode from now on') >= 0,
	   'donor: ...and the status says it lends in APDU mode from now on');

	p.ops.restart('m0', {}, {}, () => null);
	p.tick('m0', { rsim_reader: 'modem:m1' });
	run_for(50);
	eq(length(filter(donor.sent, (x) => x.name == 'SAP_CONNECTION' && x.args.conn.op == 1)), n,
	   'donor: `wwandctl rsim restart` does not try SIM Access on it again (auto: APDU now)');

	// SIM Access set explicitly: the old way — say what to use, retry on restart
	let p2 = rsim.create({
		log: (l, m) => null, sim_changed: () => null,
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : fake_client()),
		qmi_release: (ref, c) => { c.released = true; },
		now: () => t.now,
	});
	let ext2 = { rsim_reader: 'modem:m1', rsim_donor_mode: 'sap' };

	p.stop();
	run_for(50);
	p2.tick('m0', ext2);
	run_for(50);
	ok(index(p2.status('m0', ext2)?.text ?? '', 'rsim_donor_mode apdu') >= 0,
	   'donor, SIM Access set: the status says what to use instead');

	let n2 = length(filter(donor.sent, (x) => x.name == 'SAP_CONNECTION' && x.args.conn.op == 1));

	p2.ops.restart('m0', {}, {}, () => null);
	p2.tick('m0', ext2);
	run_for(50);
	ok(length(filter(donor.sent, (x) => x.name == 'SAP_CONNECTION' && x.args.conn.op == 1)) > n2,
	   'donor, SIM Access set: `wwandctl rsim restart` tries again');
	p2.stop();
	run_for(50);
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
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
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
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
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
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : target),
		qmi_release: (ref, c) => { c.released = true; },
		modem_radio: (ref, on, cb) => cb?.(null),
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
		let donor_modem = { state: 'READY', at: mk_at(at_log) };
		let donor = fake_client({ answer: (name) =>
			(name == 'SEND_APDU') ? { __err: { error: 'qmi', code: 71 } } : (name == 'GET_ATR') ? { atr: rsim.bytes(ATR) } : {} });
		let p = rsim.create({
			log: (l, m) => null, sim_changed: () => null,
			modem_of: (ref) => ({ modem: (ref == 'm1') ? donor_modem : modem_obj(ref) }),
			qmi_client: (ref, schema, cb) => (ref == 'm1')
				? ((variant == 'no_qmi') ? cb({ error: 'unsupported' }, null) : cb(null, donor))
				: cb(null, target),
			qmi_release: (ref, c) => null,
			modem_radio: (ref, on, cb) => cb?.(null),
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

// --- named SIM readers (config wwand_simreader) -----------------------------------
{
	eq(rsim.reader_options({ type: 'wbsm', mode: 'phoenix' }).rsim_reader, 'wbsm:', 'reader: Smartmouse USB, the first one');
	eq(rsim.reader_options({ type: 'wbsm', host: 'rsim@pc.lan' }).rsim_reader, 'ssh:rsim@pc.lan:wbsm:', 'reader: ...on another machine');
	eq(rsim.reader_options({ type: 'pcsc' }).rsim_reader, 'pcsc:0', 'reader: PC/SC reader 0 by default');
	eq(rsim.reader_options({ type: 'modem', donor: 'wwmodem1', donor_mode: 'apdu' }).rsim_donor_mode, 'apdu', 'reader: a lending modem');
	ok(index(rsim.reader_options({ type: 'phoenix' }).error ?? '', 'serial port') >= 0, 'reader: a Phoenix reader without its port says why');
	ok(index(rsim.reader_options({ type: 'modem' }).error ?? '', 'donor') >= 0, 'reader: a lending modem without a donor says why');
	ok(rsim.reader_options(null).error, 'reader: a missing section is an error');
	eq(rsim.reader_options({ host: 'u@h' }).rsim_reader, 'ssh:u@h:wbsm:',
	   'reader: no type is the page\'s default, the Smartmouse USB');

	let t = { now: 1000 };
	let reader = fake_reader(card_model());
	let targets = { m0: fake_client(), m2: fake_client() };
	let p = rsim.create({
		log: (l, m) => null, sim_changed: () => null,
		open_helper: reader.open_helper,
		readers: () => ({ sm: { type: 'phoenix', device: '/dev/ttyUSB9' } }),
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
		qmi_client: (ref, schema, cb) => cb(null, targets[ref]),
		qmi_release: () => null,
		now: () => t.now,
	});

	p.tick('m0', { rsim: 'sm' });
	run_for(30);
	eq(reader.argv?.[1], 'phoenix:/dev/ttyUSB9', 'named reader: the modem uses the reader the section defines');
	ok(index(p.status('m0', { rsim: 'sm' }).text, 'sm (phoenix:/dev/ttyUSB9)') == 0, 'named reader: the status names it');
	eq(p.card_source('m0', { rsim: 'sm' }), null, 'card source: not while the modem has not connected to it');
	targets.m0.fire('CONNECT_IND', { slot: 1 });
	run_for(30);
	eq(p.card_source('m0', { rsim: 'sm' }), 'sm', 'card source: the reader, once the modem uses its card');

	p.tick('m2', { rsim: 'sm' });
	run_for(30);
	eq(targets.m2.sent, [], 'one reader, one modem: the second modem does not take it');
	ok(index(p.status('m2', { rsim: 'sm' }).text, 'in use by modem m0') >= 0, '...and the status says who has it');

	eq(p.status('m0', { rsim: 'nope' }).level, 'error', 'named reader: an undefined reader is an error row');
	ok(index(p.status('m0', { rsim: 'nope' }).text, 'nope') >= 0, '...naming it');
}

// the sponsor: its radio parked while its card is used elsewhere (APDU), its
// identity re-read when it lends and gets back its card (SIM Access)
{
	let t = { now: 1000 };
	let target = fake_client();
	let radio = [], changed = [];
	let donor = fake_client({ answer: (name) =>
		(name == 'GET_ATR') ? { atr: rsim.bytes(ATR) } : (name == 'SEND_APDU') ? { response: rsim.bytes('9000') } : {} });
	let mods = { m1: { state: 'READY', lowpower_parked: false } };
	let p = rsim.create({
		log: (l, m) => null,
		sim_changed: (ref, why) => push(changed, [ ref, why ]),
		modem_radio: (ref, on, cb) => { push(radio, [ ref, on ]); cb?.(null); },
		modem_of: (ref) => ({ modem: mods[ref] ?? modem_obj(ref) }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : target),
		qmi_release: () => null,
		now: () => t.now,
	});
	let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu' };

	p.tick('m0', ext);
	run_for(50);
	eq(radio, [ [ 'm1', false ] ], 'sponsor (APDU): its radio is parked while its card is used elsewhere');
	ok(index(p.status('m1', {}).text, 'must stay off') >= 0 && p.status('m1', {}).level == 'warn',
	   'sponsor (APDU): still registered -> the status warns that its radio must stay off');

	mods.m1.lowpower_parked = true;
	eq(p.status('m1', {}).level, 'ok', 'sponsor (APDU): parked -> no warning');

	p.tick('m0', {});
	run_for(50);
	eq(radio[length(radio) - 1], [ 'm1', true ], 'sponsor (APDU): woken again when the lending ends');

	// SIM Access: the sponsor loses and regains its card
	changed = [];
	let sap = sap_donor([]);
	let p2 = rsim.create({
		log: (l, m) => null,
		sim_changed: (ref, why) => push(changed, [ ref, why ]),
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? sap : fake_client()),
		qmi_release: () => null,
		now: () => t.now,
	});

	p2.tick('m0', { rsim_reader: 'modem:m1' });
	run_for(1200);
	ok(length(filter(changed, (c) => c[0] == 'm1' && index(c[1], 'lent') >= 0)) == 1,
	   'sponsor (SIM Access): lending its card runs its card-change process');

	p2.tick('m0', {});
	run_for(50);
	ok(length(filter(changed, (c) => c[0] == 'm1' && index(c[1], 'back') >= 0)) == 1,
	   'sponsor (SIM Access): ...and so does getting it back');
}

// an APDU sponsor whose radio cannot be switched off does not lend: two
// modems must never register with one card
{
	let t = { now: 1000 };
	let target = fake_client();
	let donor = fake_client({ answer: (name) =>
		(name == 'GET_ATR') ? { atr: rsim.bytes(ATR) } : (name == 'SEND_APDU') ? { response: rsim.bytes('9000') } : {} });
	let p = rsim.create({
		log: (l, m) => null, sim_changed: () => null,
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : target),
		qmi_release: () => null,
		modem_radio: (ref, on, cb) => cb?.({ error: 'unsupported' }),
		now: () => t.now,
	});
	let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu' };

	p.tick('m0', ext);
	run_for(50);
	eq(filter(target.sent, (x) => x.name == 'EVENT' && x.args.info.event == 1), [],
	   'apdu sponsor: its radio could not be parked, so the card is not offered to anyone');
	eq(filter(donor.sent, (x) => x.name == 'SEND_APDU'), [], 'apdu sponsor: ...and not one command reaches it');
	ok(index(p.status('m0', ext).text, 'cannot switch off the radio') >= 0, 'apdu sponsor: the status says why');

	t.now += 1000;
	p.tick('m0', ext);
	run_for(50);
	eq(filter(target.sent, (x) => x.name == 'EVENT' && x.args.info.event == 1), [],
	   'apdu sponsor: and it is not retried on its own');
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

// --- which card a donor can lend: the slot it runs on ---------------------------
// The NR7101 (RG502Q) shape, HW-read on 242, 2026-09-26: two physical slots,
// both mapped to logical slot 1, the second one active.
let slots_242 = [
	{ physical: 1, active: false, logical_slot: 1, card: 'present', iccid: '89882390001186977790' },
	{ physical: 2, active: true,  logical_slot: 1, card: 'present', iccid: '89490200001022832490' },
];

let apdu_donor = () => fake_client({ answer: (name) =>
	(name == 'GET_ATR') ? { atr: rsim.bytes(ATR) } : (name == 'SEND_APDU') ? { response: rsim.bytes('9000') } : {} });

let donor_plugin = (o) => rsim.create({
	log: (l, m) => null, sim_changed: () => null,
	modem_of: (ref) => ({ modem: o.mods?.[ref] ?? modem_obj(ref) }),
	qmi_client: o.qmi_client ?? ((ref, schema, cb) => cb(null, (ref == 'm1') ? o.donor : o.target)),
	qmi_release: () => null,
	modem_radio: (ref, on, cb) => { push(o.radio ?? [], [ ref, on ]); if (o.mods?.[ref]) o.mods[ref].lowpower_parked = !on; cb?.(null); },
	sim_slots: o.sim_slots,
	modem_sections: o.modem_sections ?? (() => ({})),
	pid_alive: o.pid_alive,
	now: () => o.t.now,
});

{
	let t = { now: 1000 };
	let target = fake_client(), donor = apdu_donor();
	let p = donor_plugin({ t: t, target: target, donor: donor, sim_slots: (ref, cb) => cb(null, { slots: slots_242 }) });
	let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu', rsim_donor_slot: '2' };

	p.tick('m0', ext);
	run_for(50);
	target.fire('CONNECT_IND', { slot: 1 });
	run_for(20);
	target.fire('APDU_IND', { slot: 1, apdu_id: 1, command: rsim.bytes('00A40004023F00') });
	run_for(20);

	let sa = filter(donor.sent, (x) => x.name == 'SEND_APDU')[0];

	eq(sa?.args?.slot, 1, 'donor slot: physical slot 2, the active one, is logical slot 1 — which QMI takes');
}

{
	let t = { now: 1000 };
	let target = fake_client(), donor = apdu_donor();
	let p = donor_plugin({ t: t, target: target, donor: donor, sim_slots: (ref, cb) => cb(null, { slots: slots_242 }) });
	let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu', rsim_donor_slot: '1' };

	p.tick('m0', ext);
	run_for(50);
	eq(target.events(), [], 'donor slot: the inactive slot is not lent — it is switched off');
	ok(index(p.status('m0', ext).text, 'is not active (it runs on slot 2)') >= 0,
	   'donor slot: ...the status says so, and which slot the donor runs on');

	t.now += 1000;
	p.tick('m0', ext);
	run_for(50);
	eq(target.events(), [], 'donor slot: ...and it is not retried: the hardware will not change its mind');
}

{
	let t = { now: 1000 };
	let target = fake_client(), donor = apdu_donor();
	let p = donor_plugin({ t: t, target: target, donor: donor, sim_slots: (ref, cb) => cb({ error: 'sim_transport' }, null) });
	let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu', rsim_donor_slot: '2' };

	p.tick('m0', ext);
	run_for(50);
	ok(index(p.status('m0', ext).text, 'does not report its SIM slots') >= 0,
	   'donor slot: a slot other than 1 on a donor without a slot list cannot be told apart — refused');
}

// --- SIM Access: a stale link ended first; the sponsor parked; exit ---------------
{
	let t = { now: 1000 };
	let target = fake_client();
	let st = 2;   // a link left standing by a daemon that died
	let donor = fake_client({ answer: (name, a) => {
		if (name == 'SAP_CONNECTION') {
			if (a.conn.op == 1) st = 2;
			if (a.conn.op == 0) st = 5;
			return { state: st };
		}
		if (name == 'SAP_REQUEST')
			return (a.req.op == 0) ? { atr: rsim.bytes(ATR) } : {};
		return {};
	} });
	let radio = [];
	let mods = { m1: { state: 'READY', lowpower_parked: false } };
	let p = donor_plugin({ t: t, target: target, donor: donor, radio: radio, mods: mods });
	let ext = { rsim_reader: 'modem:m1' };

	p.tick('m0', ext);
	run_for(1200);

	let ops = map(filter(donor.sent, (x) => x.name == 'SAP_CONNECTION'), (x) => [ x.args.conn.op, x.args.mode ]);

	eq(slice(ops, 0, 3), [ [ 2, null ], [ 0, 0 ], [ 1, null ] ],
	   'SAP: a link from before is ended at once, then connected anew');
	eq(radio, [ [ 'm1', false ] ], 'SAP: the sponsor is parked too — its recovery must not reset it over the lost card');
	eq(p.radio_hold('m1', {}), 'its card is lent to m0', 'radio hold: an ifup on the sponsor must not wake it');
	eq(p.radio_hold('m0', {}), null, 'radio hold: ...the modem using the card is free');

	// woken behind the plugin's back: parked again on the next tick
	mods.m1.lowpower_parked = false;
	p.tick('m0', ext);
	run_for(20);
	eq(radio, [ [ 'm1', false ], [ 'm1', false ] ], 'SAP: a sponsor woken by something else is parked again');

	eq(p.stop(), true, 'exit: a lent card is on its way home — the daemon keeps its loop a moment');
	run_for(50);
	eq(radio[length(radio) - 1], [ 'm1', true ], 'exit: ...and the sponsor is woken');
	eq(p.stop(), false, 'exit: nothing left to wait for');
}

// --- the lending modem restarts under the link ----------------------------------
{
	let t = { now: 1000 };
	let target = fake_client(), donor = apdu_donor();
	let mods = { m1: { state: 'READY', lowpower_parked: false } };
	let p = donor_plugin({ t: t, target: target, donor: donor, mods: mods });
	let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu' };

	p.tick('m0', ext);
	run_for(50);
	eq(target.events(), [ 1 ], 'donor restart: first the card is offered');

	mods.m1 = { state: 'READY', lowpower_parked: false };   // a new modem object
	p.tick('m0', ext);
	run_for(50);

	let st;

	p.ops.status('m0', ext, {}, (e, r) => { st = r; });
	ok(index(st.last_error ?? '', 'restarted') >= 0 && st.retry_at != null,
	   'donor restart: the link ends with the reason, and is tried again');
	eq(target.events(), [ 1, 0 ], 'donor restart: the target is told the card is no longer there');
}

// an AT-only donor (no QMI client at all) that restarts: its radio is woken
// even though no client was ever given back
{
	let t = { now: 1000 };
	let target = fake_client();
	let radio = [];
	let mods = { m1: { state: 'READY', lowpower_parked: false, at: { send: (c, cb) => cb(null, { lines: [] }) } } };
	let p = donor_plugin({ t: t, target: target, radio: radio, mods: mods,
		qmi_client: (ref, schema, cb) => (ref == 'm1') ? cb({ error: 'unsupported' }, null) : cb(null, target) });
	let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu', rsim_donor_apdu: 'at' };

	p.tick('m0', ext);
	run_for(50);
	eq(radio, [ [ 'm1', false ] ], 'AT donor: parked');

	let old = mods.m1;

	mods.m1 = { state: 'READY', lowpower_parked: false, at: { send: (c, cb) => cb(null, { lines: [] }) } };
	p.tick('m0', ext);
	run_for(50);
	eq(radio, [ [ 'm1', false ], [ 'm1', true ] ], 'AT donor: the link ends on its restart and the radio is woken, client or not');
	ok(old != null, 'AT donor: (the old object was replaced)');
}

// a donor without QMI UIM (NCM) and no mode set: SIM Access is not there,
// so it lends in APDU mode over AT+CSIM; SIM Access set explicitly is refused
{
	for (let mode in [ null, 'sap' ]) {
		let t = { now: 1000 };
		let target = fake_client();
		let radio = [];
		let sent = [];
		let mods = { m1: { state: 'READY', lowpower_parked: false,
		                   at: { send: (c, cb) => { push(sent, c); cb(null, { lines: [ '+CSIM: 4,"9000"' ] }); } } } };
		let p = donor_plugin({ t: t, target: target, radio: radio, mods: mods,
			qmi_client: (ref, schema, cb) => (ref == 'm1') ? cb({ error: 'unsupported' }, null) : cb(null, target) });
		let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: mode };

		p.tick('m0', ext);
		run_for(200);

		let st;

		p.ops.status('m0', ext, {}, (e, r) => { st = r; });

		if (mode == null) {
			eq(radio, [ [ 'm1', false ] ], 'NCM donor, auto: its radio is parked (APDU mode)');
			eq(target.events(), [ 1 ], 'NCM donor, auto: the card is offered to the target modem');
			eq(st.reader_info?.mode, 'apdu', 'NCM donor, auto: the status says APDU, not the configured auto');

			target.fire('CONNECT_IND', { slot: 1 });
			run_for(50);
			target.fire('APDU_IND', { slot: 1, apdu_id: 5, command: rsim.bytes('00A40004023F00') });
			run_for(50);

			let ans = filter(target.sent, (x) => x.name == 'APDU');

			eq(sent, [ 'AT+CSIM=14,"00A40004023F00"' ], 'NCM donor, auto: the command goes over AT+CSIM');
			eq(rsim.hexs(ans[0]?.args?.response), '9000', 'NCM donor, auto: ...and its answer back to the target');
		}
		else {
			eq(sent, [], 'NCM donor, SIM Access set: nothing sent over AT');
			ok(index(st.last_error ?? '', 'no UIM client') >= 0,
			   sprintf('NCM donor, SIM Access set: refused with the reason (%s)', st.last_error));
		}
	}
}

// an E392 hangs on a SIM Access connect: by default it lends in APDU mode,
// and never gets a connect; SIM Access set explicitly is still tried
{
	for (let mode in [ null, 'sap' ]) {
		let t = { now: 1000 };
		let target = fake_client();
		let donor = apdu_donor();
		let mods = { m1: { state: 'READY', lowpower_parked: false, info: { model: 'E392' } } };
		let p = donor_plugin({ t: t, donor: donor, target: target, radio: [], mods: mods });
		let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: mode };

		p.tick('m0', ext);
		run_for(300);

		let connects = filter(donor.sent, (x) => x.name == 'SAP_CONNECTION' && x.args?.conn?.op == 1);

		if (mode == null) {
			eq(length(connects), 0, 'E392, auto: no SIM Access connect is sent');
			eq(target.events(), [ 1 ], 'E392, auto: its card is offered in APDU mode');
		}
		else
			eq(length(connects), 1, 'E392, SIM Access set: it is asked all the same');

		p.stop();
		run_for(50);
	}
}

// a sponsor still coming up (a daemon restart): nothing is started until it
// is ready — started early, its park had no client and refused for good
{
	let t = { now: 1000 };
	let target = fake_client();
	let donor = apdu_donor();
	let radio = [];
	let mods = { m0: { state: 'READY' }, m1: { state: 'INIT_SERVICES', lowpower_parked: false } };
	let p = donor_plugin({ t: t, donor: donor, target: target, radio: radio, mods: mods });
	let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu' };

	p.tick('m0', ext);
	run_for(100);
	eq([ radio, target.events() ], [ [], [] ], 'sponsor not ready: nothing started, nothing parked');

	mods.m1.state = 'READY';
	t.now += 20;
	p.tick('m0', ext);
	run_for(100);
	eq([ radio, target.events() ], [ [ [ 'm1', false ] ], [ 1 ] ], 'sponsor ready: parked, and its card offered');
	p.stop();
	run_for(50);
}

// an SFI access the donor's UIM refuses as an internal error (E392): the
// card's answer 6A81, not an I/O error that makes the target reset the card
{
	let t = { now: 1000 };
	let target = fake_client();
	let donor = fake_client({ answer: (name, a) =>
		(name == 'GET_ATR') ? { atr: rsim.bytes(ATR) }
		: (name == 'SEND_APDU') ? (((a.apdu[1] == 0xB0 && (a.apdu[2] & 0x80)) || a.apdu[1] == 0xA2) ? { __err: { error: 'qmi', code: 3 } } : { response: rsim.bytes('9000') })
		: {} });
	let p = donor_plugin({ t: t, donor: donor, target: target, radio: [] });
	let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu' };

	p.tick('m0', ext);
	run_for(100);
	target.fire('CONNECT_IND', { slot: 1 });
	run_for(50);
	target.fire('APDU_IND', { slot: 1, apdu_id: 7, command: rsim.bytes('00B0830004') });
	run_for(50);
	target.fire('APDU_IND', { slot: 1, apdu_id: 8, command: rsim.bytes('00B0000004') });
	run_for(50);
	target.fire('APDU_IND', { slot: 1, apdu_id: 9, command: rsim.bytes('00A2010403FFFFFF') });
	run_for(50);

	let ans = map(filter(target.sent, (x) => x.name == 'APDU'), (x) => rsim.hexs(x.args?.response));

	eq(ans, [ '6A81', '9000', '6A81' ], 'refused by the donor\'s UIM (SFI read, SEARCH RECORD): answered 6A81; a plain read goes through');
	eq(rsim.sfi_access(rsim.bytes('00B2010C00')), true, 'sfi_access: READ RECORD with an SFI in P2');
	eq(rsim.sfi_access(rsim.bytes('00B2010400')), false, 'sfi_access: READ RECORD of the current EF');
	p.stop();
	run_for(50);
}

// DEREGISTER FIRST: over SIM Access the card leaves the sponsor at once, so
// its radio is parked (a detach) BEFORE the connect — not after, when it had
// already dropped off the network without one
{
	let t = { now: 1000 };
	let donor = sap_donor([]);
	let order = [];
	let req = donor.request;

	donor.request = (name, a, cb, o) => {
		if (name == 'SAP_CONNECTION' && a?.conn?.op == 1)
			push(order, 'connect');
		return req(name, a, cb, o);
	};

	let p = rsim.create({
		log: (l, m) => null, sim_changed: () => null,
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : fake_client()),
		qmi_release: () => null,
		modem_radio: (ref, on, cb) => { push(order, sprintf('%s radio %s', ref, on ? 'on' : 'off')); cb?.(null); },
		now: () => t.now,
	});

	p.tick('m0', { rsim_reader: 'modem:m1', rsim_donor_mode: 'sap' });
	run_for(300);
	eq(slice(order, 0, 2), [ 'm1 radio off', 'connect' ], 'SIM Access: the sponsor is parked (deregistered) before the connect');
	eq(length(filter(order, (x) => x == 'm1 radio off')), 1, 'SIM Access: ...once — the link coming up does not park it again');
	p.stop();
	run_for(50);
}

// the sponsor parked (deregistered) before a connect it then refuses (busy,
// QMI 52): the lending ends, and its radio comes back on
{
	let t = { now: 1000 };
	let radio = [];
	let donor = fake_client({ answer: (name, a) =>
		(name == 'SAP_CONNECTION' && a.conn.op == 1) ? { __err: { error: 'qmi', code: 52 } }
		: (name == 'SAP_CONNECTION') ? { state: 0 } : {} });
	let p = rsim.create({
		log: (l, m) => null, sim_changed: () => null,
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : fake_client()),
		qmi_release: () => null,
		modem_radio: (ref, on, cb) => { push(radio, [ ref, on ]); cb?.(null); },
		now: () => t.now,
	});

	p.tick('m0', { rsim_reader: 'modem:m1', rsim_donor_mode: 'sap' });
	run_for(100);
	eq(radio, [ [ 'm1', false ] ], 'busy after the deregister: parked, and asked again after a moment');
	run_for(3300);
	eq(length(filter(donor.sent, (x) => x.name == 'SAP_CONNECTION' && x.args.conn.op == 1)), 2,
	   '...once (its data session may still be tearing down)');
	eq(radio, [ [ 'm1', false ], [ 'm1', true ] ], 'still refused: the lending ends and the sponsor\'s radio is woken again');
	p.stop();
	run_for(50);
}

// a lent card: the target's client first — without one the sponsor is not
// touched (no park, no connect), where it used to be on every retry
{
	let t = { now: 1000 };
	let donor = sap_donor([]);
	let radio = [];
	let p = donor_plugin({ t: t, donor: donor, radio: radio,
		qmi_client: (ref, schema, cb) => (ref == 'm1') ? cb(null, donor) : cb({ error: 'unsupported' }, null) });

	p.tick('m0', { rsim_reader: 'modem:m1' });
	run_for(200);
	eq([ radio, length(donor.sent) ], [ [], 0 ], 'no client on the target: the sponsor is left alone');
	p.stop();
	run_for(50);
}

// --- a modem that is not ready yet --------------------------------------------------
{
	let t = { now: 1000 };
	let reader = fake_reader(card_model());
	let mods = { m0: { state: 'INIT_SERVICES' } };
	let p = rsim.create({
		log: (l, m) => null, sim_changed: () => null,
		open_helper: reader.open_helper, helper_path: '/x',
		modem_of: (ref) => ({ modem: mods[ref] }),
		qmi_client: (ref, schema, cb) => cb({ error: 'not_ready' }, null),
		qmi_release: () => null,
		now: () => t.now,
	});

	p.tick('m0', EXT);
	run_for(20);
	eq(reader.open, 0, 'not ready: no reader is started for a modem still bringing up its services');

	mods.m0.state = 'READY';
	p.tick('m0', EXT);
	run_for(20);

	let st;

	p.ops.status('m0', EXT, {}, (e, r) => { st = r; });
	eq([ st.last_error, st.retry_at ], [ null, null ],
	   'not ready: a client refused as not_ready is tried again on the next tick, not counted as a failure');
}

// --- commands for a card session that has ended ---------------------------------------
{
	let t = { now: 1000 };
	let reader = fake_reader(card_model());
	let client = fake_client();
	let p = mk(reader, (ref, schema, cb) => cb(null, client), t);

	p.tick('m0', EXT);
	run_for(20);
	client.fire('CONNECT_IND', { slot: 1 });
	run_for(20);

	let before = length(filter(reader.lines, (l) => l.op == 'tpdu'));

	// two commands, then a reset before any of them is answered
	client.fire('APDU_IND', { slot: 1, apdu_id: 21, command: rsim.bytes('A0A40000023F00') });
	client.fire('APDU_IND', { slot: 1, apdu_id: 22, command: rsim.bytes('A0C0000017') });
	client.fire('CARD_RESET_IND', { slot: 1 });
	run_for(20);

	eq(length(filter(reader.lines, (l) => l.op == 'tpdu')) - before, 1,
	   'stale commands: the one waiting behind a reset never reaches the card');
	eq(filter(client.sent, (x) => x.name == 'APDU' && (x.args.apdu_id == 21 || x.args.apdu_id == 22)), [],
	   'stale commands: ...and nothing from before the reset is answered to the modem');

	// a card put into the reader starts a new card session without the
	// modem asking: what was queued for the old card is dropped as well
	before = length(filter(reader.lines, (l) => l.op == 'tpdu'));
	client.fire('APDU_IND', { slot: 1, apdu_id: 31, command: rsim.bytes('A0A40000023F00') });
	client.fire('APDU_IND', { slot: 1, apdu_id: 32, command: rsim.bytes('A0C0000017') });
	reader.on_line('{"event":"inserted"}');
	run_for(20);
	eq(length(filter(reader.lines, (l) => l.op == 'tpdu')) - before, 1,
	   'stale commands: a card inserted meanwhile — the command queued for the old one is dropped');
}

// --- review round: races and leftovers ----------------------------------------------

// the link ends while the sponsor's radio is still being parked: the late
// park answer must be undone, not leave the radio off for good
{
	let t = { now: 1000 };
	let target = fake_client(), donor = apdu_donor();
	let radio = [], pending = null;
	let p = rsim.create({
		log: (l, m) => null, sim_changed: () => null,
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : target),
		qmi_release: () => null,
		modem_radio: (ref, on, cb) => { push(radio, [ ref, on ]); if (on) cb?.(null); else pending = cb; },
		sim_slots: (ref, cb) => cb(null, { slots: [] }),
		now: () => t.now,
	});
	let ext = { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu' };

	p.tick('m0', ext);
	run_for(30);
	ok(pending != null, 'park in flight: the park request is out');
	p.tick('m0', {});          // the remote card switched off meanwhile
	run_for(20);
	pending(null);             // ...and then the park succeeds
	eq(radio[length(radio) - 1], [ 'm1', true ], 'park in flight: the late park is undone — the radio is woken');
}

// a helper line that is not JSON (an SSH banner) is logged, not fatal
{
	let t = { now: 1000 };
	let reader = fake_reader(card_model());
	let client = fake_client();
	let p = mk(reader, (ref, schema, cb) => cb(null, client), t);
	let died = null;

	p.tick('m0', EXT);
	run_for(20);
	try { reader.on_line('Welcome to pc.lan!'); } catch (e) { died = e.message; }
	eq(died, null, 'helper: a line that is not JSON does not throw (it would end the daemon)');
}

// a hold is for the configuration that failed: a changed one is tried
{
	let t = { now: 1000 };
	let target = fake_client(), donor = apdu_donor();
	let p = donor_plugin({ t: t, target: target, donor: donor, sim_slots: (ref, cb) => cb(null, { slots: slots_242 }) });

	p.tick('m0', { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu', rsim_donor_slot: '1' });
	run_for(50);
	eq(target.events(), [], 'hold: the inactive slot is held');

	p.tick('m0', { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu', rsim_donor_slot: '2' });
	run_for(50);
	eq(target.events(), [ 1 ], 'hold: switched to the active slot, the card is offered without a restart');
}

// SIM Access: the state indication and the status poll both report the link
// up — it is taken up once
{
	let t = { now: 1000 };
	let target = fake_client();
	let changed = [], radio = [];
	let st = 0;
	// every answer takes 250 ms: the state poll's request is in flight when
	// the indication arrives — the window a second up() would come through
	let donor = fake_client({ delay: 250, answer: (name, a) => {
		if (name == 'SAP_CONNECTION') {
			if (a.conn.op == 1) st = 2;
			return { state: st };
		}
		return (name == 'SAP_REQUEST' && a.req.op == 0) ? { atr: rsim.bytes(ATR) } : {};
	} });
	let p = rsim.create({
		log: (l, m) => null, sim_changed: (ref, why) => push(changed, ref),
		modem_of: (ref) => ({ modem: modem_obj(ref) }),
		qmi_client: (ref, schema, cb) => cb(null, (ref == 'm1') ? donor : target),
		qmi_release: () => null,
		// answered later, like the real one: the window the second report
		// lands in
		modem_radio: (ref, on, cb) => { push(radio, [ ref, on ]); uloop.timer(400, () => cb?.(null)); },
		now: () => t.now,
	});

	p.tick('m0', { rsim_reader: 'modem:m1' });
	run_for(1100);
	donor.fire('SAP_CONNECTION_IND', { st: { slot: 1, state: 2 } });
	run_for(1500);
	eq([ length(filter(changed, (r) => r == 'm1')), length(radio) ], [ 1, 1 ],
	   'SAP: the link is taken up once — one card change, one park — however it is reported');
}

// donor_test on a card lent right now is refused
{
	let t = { now: 1000 };
	let target = fake_client(), donor = apdu_donor();
	let p = donor_plugin({ t: t, target: target, donor: donor });
	let res = null;

	p.tick('m0', { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu' });
	run_for(50);
	p.ops.donor_test('m1', {}, {}, (e, r) => { res = e; });
	eq(res?.error, 'busy', 'donor test: refused while the card is lent — it would end the live link');
}

// the offer's answer arriving after the session has failed does not revive it
{
	let t = { now: 1000 };
	let reader = fake_reader(card_model());
	let client = fake_client({ delay: 50 });
	let p = mk(reader, (ref, schema, cb) => cb(null, client), t);

	p.tick('m0', EXT);
	run_for(10);               // the offer is on its way
	reader.on_exit();          // ...when the helper dies
	run_for(100);

	let st;

	p.ops.status('m0', EXT, {}, (e, r) => { st = r; });
	ok(st.retry_at != null && st.state != 'waiting',
	   'late offer answer: the failed session stays failed, its backoff stands');
}

// a card inserted while the modem is not on the remote card changes nothing
{
	let t = { now: 1000 };
	let reader = fake_reader(card_model());
	let client = fake_client();

	changes = [];
	let p = mk(reader, (ref, schema, cb) => cb(null, client), t);

	p.tick('m0', EXT);
	run_for(20);               // offered, not connected
	reader.on_line('{"event":"inserted"}');
	run_for(20);
	eq([ changes, client.events() ], [ [], [ 1 ] ],
	   'inserted while only offered: the modem runs on its own card — no card-inserted, no identity wipe');
}

// soft retries are not free for ever
{
	let t = { now: 1000 };
	let reader = fake_reader(card_model());
	let p = mk(reader, (ref, schema, cb) => cb({ error: 'not_ready' }, null), t);

	for (let i = 0; i < 4; i++) {
		p.tick('m0', EXT);
		run_for(20);
	}

	let st;

	p.ops.status('m0', EXT, {}, (e, r) => { st = r; });
	ok(st.retry_at != null, 'soft: the fourth not-ready in a row counts, and backs off');
}

// the hand-back of a SIM Access link takes its time: until it is done the
// sponsor's radio stays held and the daemon's exit waits for it
{
	let t = { now: 1000 };
	let target = fake_client();
	let st = 0;
	let donor = fake_client({ delay: 200, answer: (name, a) => {
		if (name == 'SAP_CONNECTION') {
			if (a.conn.op == 1) st = 2;
			if (a.conn.op == 0) st = 5;
			return { state: st };
		}
		return (name == 'SAP_REQUEST' && a.req.op == 0) ? { atr: rsim.bytes(ATR) } : {};
	} });
	let p = donor_plugin({ t: t, target: target, donor: donor });

	p.tick('m0', { rsim_reader: 'modem:m1' });
	run_for(2500);
	ok(p.radio_hold('m1', {}) != null, 'hand-back: lending — held');

	eq(p.stop(), true, 'hand-back: the stop has work in flight');
	ok(p.busy(), 'hand-back: ...the SAP disconnect is on its way, the exit waits');
	ok(index(p.radio_hold('m1', {}) ?? '', 'handed back') >= 0, 'hand-back: ...and the radio stays held meanwhile');
	run_for(600);
	eq([ p.busy(), p.radio_hold('m1', {}) ], [ false, null ], 'hand-back: once it is back, neither');
}

// a sponsor is held from its configuration, before any link stands: after a
// daemon restart there is no session yet, and it must not register meanwhile
{
	let t = { now: 1000 };
	let secs = { m0: { rsim_reader: 'modem:m1', rsim_donor_mode: 'apdu' }, m1: {} };
	let p = donor_plugin({ t: t, target: fake_client(), donor: apdu_donor(), modem_sections: () => secs });

	eq(p.radio_hold('m1', {}), 'it is the SIM sponsor of m0', 'config hold: the sponsor is held with no session yet');
	eq(p.radio_hold('m0', {}), null, 'config hold: the modem using the card is not');

	secs = { m0: {}, m1: {} };
	t.now++;
	eq(p.radio_hold('m1', {}), null, 'config hold: configured away, the hold goes');
}

// --- the AT init step that allows a Qualcomm modem to lend its card over SAP --------
{
	let t = { now: 100 };
	let p = donor_plugin({ t: t, target: fake_client(), donor: fake_client() });
	let q = { protocol: 'qmi', manufacturer: 'Quectel', model: 'RG502Q-EA' };
	let st = p.at_init('m1', {}, q);

	eq(length(st), 1, 'sap init: a Qualcomm (QMI) modem from Quectel gets the step');
	eq([ st[0].check, st[0].set, st[0].reset ],
	   [ 'AT+QNVFR="/nv/item_files/modem/qmi/uim/sap_security_restrictions"',
	     'AT+QNVFW="/nv/item_files/modem/qmi/uim/sap_security_restrictions",00', true ],
	   'sap init: read the EFS item, write 00 only when it differs, then one reset');
	ok(match('+QNVFR: 00', regexp(st[0].want)) && match('+QNVFR: "00"', regexp(st[0].want)) && !match('+QNVFR: 01', regexp(st[0].want)),
	   'sap init: "already set" is 00, quoted or not, and nothing else');
	eq(length(p.at_init('m1', {}, { ...q, protocol: 'mbim' })), 1, 'sap init: MBIM too (Qualcomm with the QMI passthrough)');
	eq(p.at_init('m1', {}, { ...q, protocol: 'ncm' }), [], 'sap init: not an NCM modem (not Qualcomm UIM)');
	eq(p.at_init('m1', {}, { protocol: 'qmi', manufacturer: 'Huawei Technologies Co., Ltd.' }), [],
	   'sap init: a Qualcomm modem of a vendor whose EFS command is unknown: nothing (no failing command every start)');
	eq(p.at_init('m1', { rsim_sap_auto: '0' }, q), [], 'sap init: off with rsim_sap_auto 0');

	// a UIM without the SIM Access service (error 71): left out once known
	let mods = { m1: { state: 'READY' } };
	let p2 = donor_plugin({ t: t, target: fake_client(), mods: mods,
		qmi_client: (ref, schema, cb) => cb(null, fake_client({ answer: () => ({ __err: { error: 'qmi', code: 71 } }) })) });

	p2.tick('m1', {});
	run_for(50);
	eq(p2.at_init('m1', {}, q), [], 'sap init: a UIM that has no SAP is left out');
}

// --- what the reader is: the helper's info event, kept for the status ------------
{
	let t = { now: 3000 };
	let reader = fake_reader(card_model());
	let p = mk(reader, (ref, schema, cb) => cb(null, fake_client()), t);

	p.tick('m0', EXT);
	run_for(50);
	reader.on_line('{"event":"info","backend":"phoenix","reader":"/dev/ttyUSB0","usb_product":"CP2102","clock_khz":3579}');

	let st = null;

	p.ops.status('m0', EXT, {}, (e, r) => { st = r; });
	eq(st?.reader_info, { backend: 'phoenix', reader: '/dev/ttyUSB0', usb_product: 'CP2102', clock_khz: 3579 },
	   'info: the reader\'s info event is kept for the status');
	ok(st?.state != 'failed', 'info: ...and is not taken for an answer');
	p.stop();
	run_for(50);
}

// the lending router's settings for its card: kept here as the card's
// wwand_sim (sim_upsert, origin rsim), and not in the status
{
	let t = { now: 3000 };
	let reader = fake_reader(card_model());
	let ups = [];
	let p = mk(reader, (ref, schema, cb) => cb(null, fake_client()), t,
		{ sim_upsert: (iccid, f, origin) => { push(ups, [ iccid, f, origin ]); return { written: true, section: 'wwsim_' + iccid }; } });

	p.tick('m0', EXT);
	run_for(50);
	reader.on_line('{"event":"info","backend":"wwand","reader":"m1","iccid":"89882390001760008921","sim":{"apn":"apn.global-m2m.net","pdp_type":null,"auth":"both","username":"gdsp","password":"gdsp","source":"sim"}}');

	let st = null;

	p.ops.status('m0', EXT, {}, (e, r) => { st = r; });
	eq(ups, [ [ '89882390001760008921', { apn: 'apn.global-m2m.net', pdp_type: null, auth: 'both', username: 'gdsp', password: 'gdsp' }, 'rsim' ] ],
	   'lender settings: kept as the card\'s wwand_sim, origin rsim');
	eq(st?.reader_info?.sim, null, 'lender settings: ...not in the status (the password)');
	eq(st?.reader_info?.iccid, '89882390001760008921', 'lender settings: (the rest of the info is kept)');

	// no APN: nothing to keep
	reader.on_line('{"event":"info","backend":"wwand","reader":"m1","iccid":"89882390001760008921","sim":null}');
	eq(length(ups), 1, 'lender settings: none sent, nothing written');
	p.stop();
	run_for(50);
}

// --- a card lent to another router (wwandctl rsim proxy) -------------------------
{
	let t = { now: 1000 };
	let donor_log = [];
	let donor = sap_donor(donor_log);
	let radio = [];
	let alive = true;
	let secs = { m0: {}, m1: {} };
	let p = donor_plugin({ t: t, target: fake_client(), donor: donor, radio: radio,
	                       modem_sections: () => secs, pid_alive: (pid) => alive });
	let op = (name, ref, args) => {
		let res = null;

		p.ops[name](ref, {}, args, (e, r) => { res = e ? { err: e } : r; });
		run_for(700);
		return res;
	};

	eq(op('lend_check', 'm1'), { lendable: true, why: null, sap: null }, 'lend: a modem\'s card can be lent (SIM Access not asked yet)');

	let o = op('lend_open', 'm1', { mode: 'sap', client: '10.0.0.2', pid: 4711 });

	ok(o?.id, 'lend: opened, an id');
	eq(p.radio_hold('m1', {}), 'its card is lent to 10.0.0.2', 'lend: the radio is held while lent');
	ok(length(filter(radio, (x) => x[0] == 'm1' && !x[1])), 'lend: ...and parked');
	ok(index(p.status('m1', {})?.text ?? '', 'lends its card to 10.0.0.2 (SIM Access') == 0, 'lend: the status says to whom');

	let a = op('lend_call', 'm1', { id: o.id, req: { op: 'power_up' } });

	eq(a?.answer?.atr, ATR, 'lend: power_up through the lent card, its ATR');
	a = op('lend_call', 'm1', { id: o.id, req: { op: 'tpdu', data: 'A0A40000023F00' } });
	eq(a?.answer, { ok: true, data: '9F17' }, 'lend: a command, the card\'s answer');
	eq(op('lend_call', 'm1', { id: o.id, req: 'x' })?.answer?.error, 'bad_request', 'lend: not a request');
	let st = op('status', 'm1');

	eq([ st?.enabled, st?.lent_to?.to, st?.lent_to?.remote, st?.lent_to?.mode, st?.lent_to?.commands ],
	   [ false, '10.0.0.2', true, 'sap', 1 ], 'lend: the status op says whom the card is lent to (LuCI shows it)');
	eq(op('lend_call', 'm0', { id: o.id, req: { op: 'status' } })?.err?.error, 'no_such_lend',
	   'lend: an id is only good for its modem');

	eq(op('lend_open', 'm1', { client: 'x' })?.err?.detail, 'its card is lent to 10.0.0.2', 'lend: one at a time');
	eq(op('lend_check', 'm1')?.lendable, false, 'lend: ...and the check says so');

	// a modem here that wants the same card as a sponsor waits
	secs = { m0: { rsim_reader: 'modem:m1' }, m1: {} };
	t.now++;
	p.tick('m0', { rsim_reader: 'modem:m1' });
	run_for(20);
	ok(index(p.status('m0', { rsim_reader: 'modem:m1' })?.text ?? '', 'lent to 10.0.0.2') >= 0,
	   'lend: a modem here that wants the card waits, and says why');
	secs = { m0: {}, m1: {} };
	t.now++;

	// the proxy dies hard: the next tick sends the card home
	alive = false;
	t.now += 10;
	p.tick('m1', {});
	run_for(700);
	eq(op('lend_call', 'm1', { id: o.id, req: { op: 'status' } }), { ended: true, why: 'the proxy process is gone' },
	   'lend: a proxy that is gone ends the lend, and a late call hears why');
	let last = filter(donor.sent, (x) => x.name == 'SAP_CONNECTION');

	eq(last[length(last) - 1]?.args?.conn?.op, 0, 'lend: ...the SIM Access link is ended');
	eq(p.radio_hold('m1', {}), null, 'lend: ...and the radio is free again');
	ok(length(filter(radio, (x) => x[0] == 'm1' && x[1])), 'lend: ...and woken');

	// taken back here: ended, and not lent again until allowed
	let o4 = op('lend_open', 'm1', { mode: 'apdu', client: '10.0.0.4', pid: 77 });

	eq(op('lend_end', 'm1'), { ended: true, held: true }, 'take back: the lend ends');
	eq(op('lend_call', 'm1', { id: o4.id, req: { op: 'status' } }), { ended: true, why: 'taken back here' },
	   'take back: the proxy hears why');
	ok(index(op('lend_open', 'm1', { client: 'x', pid: 78 })?.err?.detail ?? '', 'stopped here') >= 0,
	   'take back: the other router\'s retry is refused');
	eq([ op('status', 'm1')?.lend_hold, op('status', 'm1')?.lendable ], [ true, false ], 'take back: the status says so');
	eq(type(op('status', 'm1')?.lend_provider), 'bool', 'status: whether wwand-rsim-provider is installed (another router can be served)');
	op('lend_allow', 'm1');
	eq(op('status', 'm1')?.lendable, true, 'take back: allowed again');

	// again, closed properly this time
	alive = true;
	let o2 = op('lend_open', 'm1', { mode: 'apdu', client: '10.0.0.3', pid: 4712 });

	ok(o2?.id && o2.id != o.id, 'lend: lent again, a new id');
	eq(op('lend_close', 'm1', { id: o2.id }), { closed: true }, 'lend: closed by the proxy');
	eq(op('lend_call', 'm1', { id: o2.id, req: { op: 'status' } })?.err?.error, 'no_such_lend', 'lend: ...and gone');
	eq(p.radio_hold('m1', {}), null, 'lend: ...the radio free');

	// a sponsor configured for a modem here keeps its card for that one
	secs = { m0: { rsim_reader: 'modem:m1' }, m1: {} };
	t.now++;
	eq(op('lend_open', 'm1', { client: 'x' })?.err?.detail, 'it is the SIM sponsor of m0',
	   'lend: a sponsor configured here is not lent elsewhere');
	secs = { m0: {}, m1: {} };
	t.now++;

	// a lend without the proxy's pid could not be watched: refused
	eq(op('lend_open', 'm1', { mode: 'apdu', client: 'y' })?.err?.error, 'bad_request', 'lend: no pid, no lend');
	eq(p.radio_hold('m1', {}), null, 'lend: ...and nothing held');

	// an idle proxy that is alive keeps the card, however long
	let o3 = op('lend_open', 'm1', { mode: 'apdu', client: 'y', pid: 99 });

	t.now += 600;
	p.tick('m1', {});
	run_for(20);
	ok(index(p.radio_hold('m1', {}) ?? '', 'lent to y') >= 0, 'lend: an idle proxy that is alive keeps the card');
	op('lend_close', 'm1', { id: o3.id });

	eq(p.stop(), false, 'lend: nothing left to hand back at the exit');
}

// the proxy (wwandctl rsim proxy) against the plugin, through the daemon's
// modem_plugin envelope ({ ok: false, ...err } / { ok: true, ...res })
{
	let ctl = require('wwand.ctl.rsim');
	let t = { now: 5000 };
	let donor = sap_donor([]);
	let radio = [];
	let p = donor_plugin({ t: t, target: fake_client(), donor: donor, radio: radio });
	let call = (m, a) => {
		if (m == 'status')
			return { modems: { m1: {} } };
		if (m == 'sim_inventory')
			return { cards: [ { iccid: '89490200001022832490', present: true, active: true, modem: 'm1' } ] };

		let res = null;

		p.ops[a.op](a.modem, {}, a.args ?? {}, (e, r) => { res = e ? { ok: false, ...e } : { ok: true, ...(r ?? {}) }; });
		for (let i = 0; res == null && i < 40; i++)
			run_for(50);
		return res;
	};
	let lines = [ '{"op":"power_up"}', '{"op":"tpdu","data":"A0A40000023F00"}', '{"op":"power_down"}' ];
	let out = [];
	let rc = ctl.proxy({}, [ 'iccid:89490200001022832490' ], {
		call: call, pid: 1, client: '10.0.0.2', sims: [],
		read_line: () => {
			if (length(lines) == 1)
				push(radio, [ 'held', p.radio_hold('m1', {}) ]);
			return length(lines) ? shift(lines) : null;
		},
		write: (l) => push(out, json(l)),
	});

	run_for(700);
	eq(rc, 0, 'proxy+plugin: a whole lend, clean end');
	eq([ out[0].event, out[0].backend, out[0].reader, out[0].iccid ], [ 'info', 'wwand', 'm1', null ],
	   'proxy+plugin: the info event comes first, like rsim-card\'s');
	eq(map(slice(out, 1), (x) => x.atr ?? x.data ?? x.ok), [ ATR, '9F17', true ], 'proxy+plugin: ATR, the answer, power-down');
	eq(filter(radio, (x) => x[0] == 'held')[0][1], 'its card is lent to 10.0.0.2', 'proxy+plugin: the radio held meanwhile');
	eq(p.radio_hold('m1', {}), null, 'proxy+plugin: ...and free afterwards');
	ok(length(filter(radio, (x) => x[0] == 'm1' && x[1])), 'proxy+plugin: the radio woken at the end');
}

done('test_rsim');
