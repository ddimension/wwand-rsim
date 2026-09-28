// wwand-rsim tests — `wwandctl rsim`: the status lines and the firmware
// switch, against a scripted modem AT port.

'use strict';

import { eq, ok, done } from './lib/check.uc';

let ctl = require('wwand.ctl.rsim');
// the provider side (package wwand-rsim-provider)
let prov = require('wwand.ctl.rsim_provider');

// --- reading the switch --------------------------------------------------------
{
	eq(ctl.qnvfr_value([ '+QNVFR: 00', 'OK' ]), '00', 'qnvfr: the value');
	eq(ctl.qnvfr_value([ '+QNVFR: "01"' ]), '01', 'qnvfr: quoted as some firmware does');
	eq(ctl.qnvfr_value([ 'ERROR' ]), null, 'qnvfr: no item is no value, not 00');
}

// --- status lines ----------------------------------------------------------------
{
	eq(ctl.status_lines({ enabled: false }), [ [ 'remote SIM', 'not configured on this modem (option rsim, or rsim_reader)' ] ],
	   'status: off');
	eq(ctl.status_lines({ enabled: true, reader: 'phoenix:/dev/ttyUSB0', slot: 1, state: 'powered',
	                      atr: '3B9F', apdus: 12, last_sw: '9000' }), [
		[ 'remote SIM', 'phoenix:/dev/ttyUSB0 · slot 1 · in use by the modem' ],
		[ 'card', 'ATR 3B9F' ],
		[ 'commands', '12 · last SW 9000' ],
	], 'status: in use');
	eq(ctl.status_lines({ enabled: true, reader: 'pcsc:0', slot: 1, state: 'idle',
	                      last_error: 'no card', retry_at: 130, now: 100 })[1],
	   [ 'last error', 'no card (retry in 30 s)' ], 'status: the error and when it tries again');
	eq(ctl.status_lines({ enabled: true, reader: 'pcsc:0', slot: 1, state: 'waiting', radio_held: true })[1],
	   [ 'radio', 'off until the remote SIM is in use — the modem does not use its own SIM' ],
	   'status: a modem waiting for its remote SIM is held off its own');
	eq(ctl.status_lines({ config_error: 'its remote SIM cannot be used (x is not a reader)', radio_held: true })[1][0], 'radio',
	   'status: ...also when its reader cannot work');
}

{
	eq(ctl.status_lines({ enabled: false, lent_to: { to: '10.0.0.2', remote: true, mode: 'sap', commands: 3 }, lend_hold: false }),
	   [ [ 'remote SIM', 'not configured on this modem (option rsim, or rsim_reader)' ],
	     [ 'lends card', 'to 10.0.0.2 (another router, SIM Access, radio off) · 3 commands' ] ],
	   'status: a card lent to another router');
	eq(ctl.status_lines({ enabled: false, lend_hold: true })[1][0], 'lending', 'status: lending stopped here');
	eq(ctl.reader_text({ backend: 'bt', name: 'Galaxy', alias: 'My Galaxy', kind: 'smartphone', vendor: 'Samsung',
	                     key: 'authenticated', channel: 8 }),
	   '"My Galaxy" (smartphone) · Samsung · authenticated pairing · channel 8', 'reader: a phone in one line');
	eq(ctl.reader_text({ backend: 'wwand', modem_manufacturer: 'Quectel', modem_model: 'RG502Q', modem_revision: 'R11',
	                     host: 'nr7101', operator: 'Telekom.de', rat: '5G', iccid: '8949' }),
	   'Quectel RG502Q · fw R11 · on nr7101 · Telekom.de 5G · ICCID 8949', 'reader: another router\'s modem');
	eq(ctl.reader_text({ backend: 'phoenix', usb_manufacturer: 'Silicon Labs', usb_product: 'CP2102', usb_serial: '0001',
	                     usb_path: '1-1.2', clock_khz: 3579, reset_line: 'rts', by_id: '/dev/serial/by-id/x' }),
	   'Silicon Labs CP2102 · serial 0001 · USB 1-1.2 · 3579 kHz, rts reset · /dev/serial/by-id/x', 'reader: a Phoenix on USB');
	let sl = ctl.status_lines({ enabled: true, reader: 'ssh:root@b:wwand:m0', slot: 1, state: 'powered',
	                            reader_info: { backend: 'wwand', modem_model: 'RG502Q', host: 'b' } });
	eq(sl[1], [ 'reader', 'RG502Q · on b' ], 'status: the reader in use, from its info event');
	eq(ctl.modem_meta({ manufacturer: 'Quectel', model: 'RG650E-EU', revision: 'R01', imei: '86', state: 'READY', rat: 'LTE',
	                    registration: { plmn: { mcc: 262, mnc: 1, mnc_digits: 2, description: 'Telekom.de' } } }).plmn,
	   '26201', 'modem meta: the PLMN spelled with its digits');
}

// --- enable / disable, through a scripted AT port ----------------------------------
function fake_ctx(start)
{
	let efs = { value: start };
	let c = { at: [], resets: 0, out: '' };

	c.status = () => ({ modems: { m0: {} } });
	c.resolve_modem = (st, a) => ({ modem: 'm0', consumed: a == 'm0' });
	c.call = (method, args) => {
		push(c.at, args.command);

		let m = match(args.command, /^AT\+QNVFR="([^"]+)"$/);

		if (m)
			return (m[1] == ctl.EFS_ENABLE && efs.value != null)
				? { ok: true, response: [ sprintf('+QNVFR: %s', efs.value) ] }
				: { ok: false, error: 'at_error', response: [ 'ERROR' ] };

		m = match(args.command, /^AT\+QNVFW="([^"]+)",([0-9A-F]+)$/);

		if (m && efs.value != null) {
			efs.value = m[2];
			return { ok: true, response: [] };
		}

		return { ok: false, error: 'at_error' };
	};
	c.call_ok = (method, args) => {
		if (method == 'modem_reset')
			c.resets++;
		return {};
	};
	c.efs = efs;

	return c;
}

{
	let c = fake_ctx('00');

	ctl.run(c, [ 'enable' ]);
	eq(c.efs.value, '01', 'enable: the switch is written');
	eq(c.at[1], sprintf('AT+QNVFW="%s",01', ctl.EFS_ENABLE), 'enable: with the item path and 01');
	eq(c.at[2], sprintf('AT+QNVFR="%s"', ctl.EFS_ENABLE), 'enable: and read back');
	eq(c.resets, 0, 'enable: no modem reset unless asked for');

	let c2 = fake_ctx('01');

	ctl.run(c2, [ 'enable' ]);
	eq(length(c2.at), 1, 'enable: already on, nothing written');

	let c3 = fake_ctx('01');

	ctl.run(c3, [ 'm0', 'disable', '--reset' ]);
	eq(c3.efs.value, '00', 'disable: switched off');
	eq(c3.resets, 1, 'disable --reset: the modem is reset so it applies');

	let c4 = fake_ctx(null);
	let err = null;

	try { ctl.run(c4, [ 'enable' ]); } catch (e) { err = e.message; }
	ok(index(err ?? '', 'cannot read the UIM Remote switch') >= 0,
	   'enable: a modem without the item (or not a Quectel) is refused before anything is written');
	eq(length(filter(c4.at, (a) => index(a, 'QNVFW') >= 0)), 0, 'enable: ...and nothing was written');
}

// a write the firmware answers OK and ignores: the read-back catches it
{
	let c = fake_ctx('00');
	let call = c.call;
	let err = null;

	c.call = (m, a) => (index(a.command, 'QNVFW') >= 0) ? { ok: true, response: [] } : call(m, a);

	try { ctl.run(c, [ 'enable' ]); } catch (e) { err = e.message; }
	ok(index(err ?? '', 'not changed') >= 0, 'enable: an ignored write is reported, not claimed as done');
}

// sap-enable writes the SIM Access item and reads it back
{
	let efs = {};
	let c = { at: [], resets: 0 };

	c.status = () => ({ modems: { m0: {} } });
	c.resolve_modem = (st, a) => ({ modem: 'm0', consumed: a == 'm0' });
	c.call = (method, args) => {
		push(c.at, args.command);

		let m = match(args.command, /^AT\+QNVFR="([^"]+)"$/);

		if (m)
			return (efs[m[1]] != null) ? { ok: true, response: [ sprintf('+QNVFR: %s', efs[m[1]]) ] }
			                           : { ok: false, error: 'at_error' };

		m = match(args.command, /^AT\+QNVFW="([^"]+)",([0-9A-F]+)$/);

		if (m) {
			efs[m[1]] = m[2];
			return { ok: true, response: [] };
		}

		return { ok: false };
	};
	c.call_ok = (method) => { if (method == 'modem_reset') c.resets++; return {}; };

	ctl.run(c, [ 'm0', 'sap-enable' ]);
	eq(efs[ctl.EFS_SAP], '00', 'sap-enable: the item is written 00');
	eq(c.at[length(c.at) - 1], sprintf('AT+QNVFR="%s"', ctl.EFS_SAP), 'sap-enable: and read back');

	let n = length(c.at);

	ctl.run(c, [ 'm0', 'sap-enable', '--reset' ]);
	eq(length(c.at), n + 1, 'sap-enable: already allowed, nothing written');
	eq(c.resets, 1, 'sap-enable --reset: the modem is reset');
}

// --- use <reader|off> --wait: done when the modem RUNS on the card -----------------
{
	let uci = { network: { sm: { '.type': 'wwand_simreader' }, m0: { '.type': 'wwand_modem' } } };
	let cur = () => ({
		load: () => true,
		get: (cf, sec, opt) => opt ? uci[cf][sec]?.[opt] : uci[cf][sec]?.['.type'],
		set: (cf, sec, opt, v) => { uci[cf][sec][opt] = v; },
		delete: (cf, sec, opt) => { delete uci[cf][sec][opt]; },
		save: () => true, commit: () => true,
	});
	let t = 0, reloads = 0;
	// the plugin's view over time: offered, then powered; the modem re-reads
	let timeline = [
		{ st: { state: 'waiting' },  m: { state: 'READY', iccid: '8949' } },
		{ st: { state: 'powered' },  m: { state: 'READY', iccid: '8949' } },   // old identity still shown
		// a test card without service: the modem keeps searching — still done
		{ st: { state: 'powered' },  m: { state: 'REGISTERING', iccid: '8988' } },
	];
	let step = 0;
	let ctx = {
		status: () => ({ modems: { m0: timeline[min(step, length(timeline) - 1)].m } }),
		call: (method, a) => timeline[min(step, length(timeline) - 1)].st,
		call_ok: (method) => { if (method == 'reload') reloads++; return {}; },
	};
	let sys = { cursor: cur, sleep: (s) => { t += s; step++; }, now: () => t };
	let rc = ctl.use_reader(ctx, 'm0', [ 'sm', '--wait', '60', '--json' ], sys);

	eq([ rc, uci.network.m0.rsim, reloads ], [ 0, 'sm', 1 ], 'use: configured, reloaded, and done');
	eq(step, 2, 'use: ...only once the card is powered AND the modem has read the NEW identity');

	step = 0; t = 0;
	timeline = [ { st: { state: 'failed', last_error: 'no card in wbsm:', retry_at: null, error_at: 1 },
	               m: { state: 'READY', iccid: '8949' } } ];
	eq(ctl.use_reader(ctx, 'm0', [ 'sm', '--wait', '60', '--json' ], sys), 1, 'use: a held failure ends the wait at once, exit 1');
	eq(step, 1, 'use: ...without sitting out the timeout');

	// the held failure of the reader BEFORE is still in the note until the
	// plugin's next tick: not this attempt's, so not an answer
	step = 0; t = 100;
	timeline = [ { st: { state: 'off', last_error: 'no card in pcsc:0', retry_at: null, error_at: 50 },
	               m: { state: 'READY', iccid: '8949' } } ];
	eq([ ctl.use_reader(ctx, 'm0', [ 'sm', '--wait', '10', '--json' ], sys), step ], [ 1, 5 ],
	   'use: an older failure does not end the wait — only the timeout does');

	// `--wait --json`: waits with the default, not "no wait"
	step = 0; t = 0;
	timeline = [ { st: { state: 'waiting' }, m: { state: 'READY', iccid: '8949' } } ];
	eq([ ctl.use_reader(ctx, 'm0', [ 'sm', '--wait', '--json' ], sys), step ], [ 1, 60 ],
	   'use: --wait without a number waits the default 120 s');

	eq(ctl.use_reader(ctx, 'nomodem', [ 'sm', '--json' ], sys), 1, 'use: a modem without a wwand_modem section is refused');

	// the same card through another reader definition: its identity does not
	// change — a session of this attempt, the identity cleared and read again
	step = 0; t = 0;
	uci.network.sm2 = { '.type': 'wwand_simreader' };
	timeline = [ { st: { state: 'waiting', since: 1 },  m: { state: 'READY', iccid: '8988' } },
	             { st: { state: 'powered', since: 1 },  m: { state: 'READY', iccid: null } },
	             { st: { state: 'powered', since: 1 },  m: { state: 'REGISTERING', iccid: '8988' } } ];
	eq([ ctl.use_reader(ctx, 'm0', [ 'sm2', '--wait', '30', '--json' ], sys), step ], [ 0, 2 ],
	   'use: the same card from another reader is done once its identity was read again');

	// ...and without seeing the identity cleared: after 20 s of the session
	step = 0; t = 0;
	uci.network.m0.rsim = 'sm';
	timeline = [ { st: { state: 'powered', since: 1 }, m: { state: 'READY', iccid: '8988' } } ];
	let rc2 = ctl.use_reader(ctx, 'm0', [ 'sm2', '--wait', '60', '--json' ], sys);
	eq([ rc2, step >= 10 && step <= 12 ], [ 0, true ],
	   'use: the same card, identity never seen cleared: done after 20 s of a fresh session');

	eq(ctl.use_reader(ctx, 'm0', [ 'nope', '--json' ], sys), 1, 'use: an undefined reader is refused');

	// already on that reader: done at once, although the identity does not change
	step = 0; t = 0;
	uci.network.m0.rsim = 'sm';
	timeline = [ { st: { state: 'powered' }, m: { state: 'READY', iccid: '8988' } } ];
	eq([ ctl.use_reader(ctx, 'm0', [ 'sm', '--wait', '20', '--json' ], sys), step ], [ 0, 1 ],
	   'use: the reader already in use is done, not a wait for a new identity');

	// off: right after the switch-off the status still shows the remote
	// card's identity, READY — done only once its own one is read again
	step = 0; t = 0;
	timeline = [ { st: { state: 'powered' }, m: { state: 'READY', iccid: '8988' } },
	             { st: { state: 'off' },     m: { state: 'READY', iccid: '8988' } },
	             { st: { state: 'off' },     m: { state: 'READY', iccid: '8949' } } ];
	eq([ ctl.use_reader(ctx, 'm0', [ 'off', '--wait', '30' ], sys), uci.network.m0.rsim, step ], [ 0, null, 2 ],
	   'use off: the option goes, done once the modem has read its own card again');

	// its own card without service: back all the same
	step = 0; t = 0;
	uci.network.m0.rsim = 'sm';
	timeline = [ { st: { state: 'powered' }, m: { state: 'REGISTERING', iccid: '8988' } },
	             { st: { state: 'off' },     m: { state: 'REGISTERING', iccid: '8949' } } ];
	eq([ ctl.use_reader(ctx, 'm0', [ 'off', '--wait', '30' ], sys), step ], [ 0, 1 ],
	   'use off: its own card read again counts, registered or not');

	step = 0; t = 0;
	timeline = [ { st: { state: 'off' }, m: { state: 'READY', iccid: '8949' } } ];
	eq([ ctl.use_reader(ctx, 'm0', [ 'off', '--wait', '30' ], sys), step ], [ 0, 1 ],
	   'use off: a modem already on its own card is done at once');
}

// --- scan: what a machine offers, and whose port it is ------------------------------
{
	let ran = [];
	let out = join('\n', [
		'{"backend":"pcsc","spec":"pcsc:ACS ACR38U 00 00","name":"ACS ACR38U 00 00","card":true}',
		'{"backend":"wbsm","spec":"wbsm:088888-03","serial":"088888-03"}',
		'{"backend":"tty","spec":"at:/dev/ttyUSB2","device":"/dev/ttyUSB2","driver":"option1","usb":"2c7c:0122","interface":"02","hint":"at"}',
		'{"backend":"tty","spec":"at:/dev/ttyUSB6","device":"/dev/ttyUSB6","driver":"option1","usb":"12d1:1506","interface":"02","hint":"at"}',
		'{"backend":"bt","spec":"bt:AA:BB:CC:DD:EE:01","device":"AA:BB:CC:DD:EE:01","name":"Pixel","adapter":"00:1A:7D:DA:71:13","phone":true,"sap":true}',
		'{"done":true,"backends":"phoenix,at,wbsm,pcsc,bt"}',
	]) + '\n';
	let sys = {
		run: (cmd) => { push(ran, cmd); return out; },
		helper: '/usr/bin/rsim-card',
		status: () => ({ modems: { wwmodem0: { at_tty: '/dev/ttyUSB2' } } }),
		ssh_sys: { flavor: 'dropbear', exists: () => true },
	};
	let rc = ctl.scan({}, [ '--json' ], sys);

	eq(rc, 0, 'scan: done');
	ok(index(ran[0], "'/usr/bin/rsim-card' '--list'") == 0, 'scan: the local helper with --list');

	let p = ctl.scan_parse(out, sys.status());

	eq([ length(p.rows), p.done?.backends ], [ 5, 'phoenix,at,wbsm,pcsc,bt' ], 'scan: every reader, and the backends');
	eq([ p.rows[4].spec, p.rows[4].sap ], [ 'bt:AA:BB:CC:DD:EE:01', true ], 'scan: a paired phone offering SIM Access');
	eq(p.rows[2].in_use, 'AT port of wwand modem wwmodem0',
	   'scan: the AT port of wwand\'s own modem is marked — an at: reader there takes its card');
	eq(p.rows[3].in_use, null, 'scan: another modem\'s port is free to use');
	eq(ctl.scan_parse(out, null).rows[2].in_use, null, 'scan: on a SIM host nothing is marked (not our modems)');

	rc = ctl.scan({}, [ 'root@simhost', '--json' ], sys);
	ok(index(ran[1], 'root@simhost') > 0 && index(ran[1], "'--list'") > 0,
	   'scan: on a SIM host over SSH, the same --list');

	ran = [];
	out = '';
	eq(ctl.scan({}, [ '--json' ], sys), 1, 'scan: no answer (no helper) is a failure, not an empty list');

	{
	// a wwand router without wwand-rsim-provider, and what the scan prints
	let out = '{"backend":"tty","spec":"at:/dev/ttyUSB6","device":"/dev/ttyUSB6","hint":"at"}\n' +
		'{"done":true,"backends":"phoenix,at","wwand_provider":false}\n';
	let sysp = { run: () => out, helper: '/usr/bin/rsim-card', status: () => ({ modems: {} }),
	             ssh_sys: { flavor: 'dropbear', exists: () => true },
	             lend_rows: () => [ { backend: 'wwand', modem: 'm1', iccid: '8949', lendable: true } ] };
	let res = [];
	let orig = global.printf;

	global.printf = (f, ...a) => push(res, sprintf(f, ...a));
	ctl.scan({}, [ 'root@r2', '--json' ], sysp);
	ctl.scan({}, [ '--json' ], sysp);
	global.printf = orig;

	let far = json(res[0]), here = json(res[1]);

	eq([ far.wwand_provider, length(filter(far.readers, (r) => r.backend == 'wwand')), far.readers[0].spec ],
	   [ false, 0, 'at:/dev/ttyUSB6' ],
	   'scan there without the provider: no modem card offered, an AT port still is');
	ok(index(far.provider_hint ?? '', 'wwand-rsim-provider') >= 0 && index(far.provider_hint, 'root@r2') >= 0,
	   'scan there: says which package is missing, and where');
	eq(filter(here.readers, (r) => r.backend == 'wwand')[0]?.spec, 'modem:m1',
	   'scan here without the provider: the sponsors still listed (they need no provider)');
	ok(index(here.provider_hint ?? '', 'not to other routers') >= 0, 'scan here: says what is missing');
	}

	// an osmo-remsim SIM bank: its slots from the server's REST API
	ran = [];
	out = join('\n', [
		'{"backend":"rspro","spec":"rspro:bank.lan/1:0","server":"bank.lan","bank":1,"slot":0,"name":"bank-1"}',
		'{"backend":"rspro","spec":"rspro:bank.lan/1:1","server":"bank.lan","bank":1,"slot":1,"mapped_to":"7:0","map_state":"ACTIVE"}',
		'{"done":true,"backends":"rspro","server":"bank.lan","slots":2}' ]) + '\n';
	eq(ctl.scan({}, [ '--rspro', 'bank.lan', '--rest-port', '8997', '--json' ], sys), 0, 'scan --rspro: done');
	eq(ran[0], "'/usr/bin/rsim-card' '--list' '--rspro-server' 'bank.lan' '--rspro-rest-port' '8997' 2>/dev/null",
	   'scan --rspro: the helper asks that server, here');
	p = ctl.scan_parse(out, null);
	eq([ length(p.rows), p.rows[1].mapped_to ], [ 2, '7:0' ], 'scan --rspro: a row per slot, a mapped one says to whom');
	out = '{"done":true,"backends":"rspro","server":"bank.lan","slots":0,"error":"bank.lan port 9997: Connection refused"}\n';
	eq(ctl.scan({}, [ '--rspro', 'bank.lan', '--json' ], sys), 1, 'scan --rspro: a server that cannot be asked is a failure');
	let died = false;

	try { ctl.scan({}, [ '--rspro', 'bank.lan;reboot' ], sys); } catch (e) { died = true; }
	ok(died, 'scan --rspro: a server is a host name or address, nothing else');
}

// --- proxy: this router's cards for another router ---------------------------------
{
	const ICC = '89490200001022832490';
	let calls = [];
	let lend = { ended: false };
	let inv = { cards: [
		{ iccid: ICC, present: true, active: true, modem: 'm1', slot: 1, imsi: '262011234567890' },
		{ iccid: '89882390001186977790', present: true, active: false, modem: 'm1', slot: 2 },
		{ iccid: '89000000000000000001', present: true, active: true, reader: 'sm', modem: null },
		{ iccid: '89000000000000000002', present: false, active: false, modem: 'm2' },
	] };
	let call = (m, a) => {
		push(calls, [ m, a?.op ?? null, a?.args ?? null ]);
		if (m == 'sim_inventory')
			return inv;
		if (m == 'status')
			return { modems: { m1: {}, m2: {} } };
		if (m == 'modem_plugin_status')
			return { ok: true, lendable: true, why: null };
		if (m != 'modem_plugin')
			return null;
		if (a.op == 'lend_open')
			return (a.modem == 'm2') ? { ok: false, error: 'busy', detail: 'its card is lent to x' } : { ok: true, id: 'L1' };
		if (a.op == 'lend_call')
			return lend.ended ? { ok: true, ended: true, why: 'the donor m1 ended the SIM Access link' }
				: { ok: true, answer: (a.args.req.op == 'tpdu') ? { ok: true, data: '9000' } : { ok: true, atr: '3B00' } };
		if (a.op == 'lend_close')
			return { ok: true, closed: true };
		return null;
	};
	let sims = [ { '.name': 'work', iccid: ICC + 'F', apn: 'internet.work', pdp_type: 'ipv4v6', pincode: '1234', password: 's' } ];

	// the list: lendable active cards, the others with why; settings by ICCID
	let rows = ctl.lend_rows(call, sims);

	eq(length(rows), 2, 'proxy list: the cards in this router\'s modems (not a reader\'s, not a gone one)');
	eq([ rows[0].spec, rows[0].modem, rows[0].lendable, rows[0].imsi ], [ 'wwand:iccid:' + ICC, 'm1', true, '262011234567890' ],
	   'proxy list: the active card, lendable, as wwand:iccid:');
	eq(rows[0].config, { name: 'work', apn: 'internet.work', pdp_type: 'ipv4v6', auth: null, username: null, password: true, pin: true },
	   'proxy list: its wwand_sim settings (matched with the trailing F), the PIN and password only as "set"');
	eq([ rows[1].lendable, index(rows[1].why, 'only that one') >= 0 ], [ false, true ], 'proxy list: an inactive slot\'s card, and why not');

	// a session: open by ICCID, relay, close
	let out = [];
	let lines = [ '{"op":"power_up"}', '', '{"op":"tpdu","data":"A0A40000023F00"}', 'garbage' ];
	let sys = { call: call, pid: 4711, client: '10.0.0.2', sims: sims,
	            read_line: () => length(lines) ? shift(lines) + '\n' : null, write: (l) => push(out, json(l)) };

	eq(ctl.proxy({}, [ 'iccid:' + ICC, '--mode', 'apdu', '--slot', '1' ], sys), 0, 'proxy: a clean end is 0');
	eq([ out[0].event, out[0].backend, out[0].reader ], [ 'info', 'wwand', 'm1' ], 'proxy: first an info event, like rsim-card');
	eq(out[0].sim, { apn: 'internet.work', pdp_type: 'ipv4v6', auth: null, username: null, password: 's', source: 'sim' },
	   'proxy: ...with the card\'s wwand_sim settings, the password included (the other router keeps them)');
	eq(prov.lend_settings([], [ { proto: 'wwand', modem: 'm2', apn: 'x' }, { proto: 'wwand', modem: 'm1', apn: 'internet.m1', auth: 'pap' } ], ICC, 'm1'),
	   { apn: 'internet.m1', pdp_type: null, auth: 'pap', username: null, password: null, source: 'interface' },
	   'proxy: no wwand_sim for the card: the modem\'s interface');
	eq(prov.lend_settings([], [ { proto: 'wwand', modem: 'm1' } ], ICC, 'm1'), null, 'proxy: no APN anywhere: nothing');
	eq(slice(out, 1), [ { ok: true, atr: '3B00' }, { ok: true, data: '9000' }, { ok: false, error: 'bad_request', detail: 'not a request object with an op' } ],
	   'proxy: every request answered, one line each');
	let open = filter(calls, (c) => c[1] == 'lend_open')[0];

	eq(open[2], { mode: 'apdu', slot: 1, apdu: null, cond: null, client: '10.0.0.2', pid: 4711 },
	   'proxy: the ICCID\'s modem is asked to lend, with mode, slot, who and its pid');
	eq(calls[length(calls) - 1][1], 'lend_close', 'proxy: at the end of stdin the card is handed back');

	// the lend ends on the other side: the proxy says so and ends with 1
	calls = [];
	out = [];
	lend.ended = true;
	lines = [ '{"op":"tpdu","data":"A0A40000023F00"}', '{"op":"status"}' ];
	eq(ctl.proxy({}, [ 'm1' ], sys), 1, 'proxy: a lend that ended is exit 1 (the helper exit the other router knows)');
	eq([ length(out), out[1].error ], [ 2, 'io' ], 'proxy: ...after answering the request in flight with an error');

	// refusals, before any line is read
	lines = [ '{"op":"power_up"}' ];
	eq(ctl.proxy({}, [ 'm2' ], sys), 1, 'proxy: a modem that cannot lend now');
	eq(ctl.proxy({}, [ 'm9' ], sys), 1, 'proxy: no such modem');
	eq(ctl.proxy({}, [ 'iccid:89000000000000000009' ], sys), 1, 'proxy: no modem runs on that card');
	eq(ctl.proxy({}, [ 'm1', '--mode', 'x' ], sys), 2, 'proxy: a bad option');
	eq(ctl.proxy({}, [ 'm1' ], { ...sys, pid: 0 }), 1, 'proxy: without its pid it does not open a lend');
	eq(length(lines), 1, 'proxy: ...none of them read a request');

	out = [];
	eq(ctl.proxy({}, [ '--list' ], sys), 0, 'proxy --list');
	eq(length(out), 2, 'proxy --list: one JSON line per card');

	// without wwand-rsim-provider: neither a lend nor the list, and it says
	// which package is missing
	out = [];
	calls = [];
	let noprov = { ...sys, provider: false };

	eq(ctl.proxy({}, [ 'm1' ], noprov), 1, 'proxy without the provider package: 1');
	eq(ctl.proxy({}, [ '--list' ], noprov), 1, 'proxy --list without it: 1, no cards for another router');
	eq([ length(out), length(calls) ], [ 0, 0 ], '...nothing written, wwand not asked');
	eq(ctl.proxy({}, [ '--list' ], { ...sys, provider: { proxy: () => 7 } }), 7, 'proxy: handed to the provider module');
	ok(index(ctl.ssh_diagnose([ 'wwandctl rsim proxy: wwand-rsim-provider is not installed here — this router\'s modem cards are not lent to other routers' ],
	                          'root@r2', true) ?? '', 'apk add wwand-rsim-provider') >= 0,
	   'ssh: a router without the provider — which package to install there');

	// the scan shows them, and here they are sponsors (modem:)
	let sl = join('\n', [ sprintf('%J', rows[0]), '{"done":true,"backends":"phoenix,at,wwand"}' ]) + '\n';
	let printed = ctl.scan_parse(sl, null);

	eq(printed.rows[0].backend, 'wwand', 'scan: the cards of wwand\'s modems come through rsim-card --list');
	let far = ctl.scan_parse(join('\n', [
		'{"backend":"tty","spec":"at:/dev/ttyUSB2","device":"/dev/ttyUSB2","hint":"at"}',
		'{"backend":"tty","spec":"at:/dev/ttyUSB1","device":"/dev/ttyUSB1","hint":"at"}',
		'{"backend":"wwand","spec":"wwand:iccid:8949","modem":"wwmodem0","modem_ports":{"at":"/dev/ttyUSB2","gps":null,"diag":null}}',
		'{"done":true}' ]) + '\n', null);
	eq([ far.rows[0].in_use, far.rows[1].in_use ], [ 'AT port of wwand modem wwmodem0 there', null ],
	   'scan of another router: the ports its wwand uses are marked, from its own list');
}

// --- readers: what each named reader is ------------------------------------------------
{
	eq(ctl.reader_where({ type: 'modem', donor: 'm1' }), 'modem m1 lends its card (auto)',
	   'readers: a sponsor without donor_mode is automatic, as the plugin runs it (not sap)');
	eq(ctl.reader_where({ type: 'modem', donor: 'm1', donor_mode: 'apdu' }), 'modem m1 lends its card (apdu)',
	   'readers: a configured donor_mode is shown as it is');
	eq(ctl.reader_where({ type: 'modem', donor: 'm1', donor_mode: 'bogus' }), 'modem m1 lends its card (auto)',
	   'readers: an unknown donor_mode is automatic, as the plugin treats it');
	eq(ctl.reader_where({ type: 'bt', device: 'AA:BB', host: 'root@pc' }), 'bt AA:BB on root@pc',
	   'readers: another kind, its device and host');
}

// --- SSH: the restricted authorized_keys line, and what went wrong -----------------
{
	let hosts = ctl.ssh_hosts({
		phone: { type: 'bt', device: 'AA:BB:CC:DD:EE:FF', host: 'root@pc.lan' },
		sm: { type: 'wbsm', host: 'root@pc.lan' },
		b: { type: 'wwand', device: 'iccid:89490200001022832490', host: 'root@simrouter' },
		local: { type: 'pcsc' },
		sponsor: { type: 'modem', donor: 'm1' },
	}, { m0: { rsim_reader: 'ssh:rsim@pc.lan:at:/dev/ttyUSB2' }, m1: { rsim_reader: 'phoenix:/dev/ttyUSB0' } });

	eq(hosts, { 'root@pc.lan': [ 'bt:AA:BB:CC:DD:EE:FF', 'wbsm:' ], 'root@simrouter': [ 'wwand:iccid:89490200001022832490' ],
	            'rsim@pc.lan': [ 'at:/dev/ttyUSB2' ] },
	   'ssh: per machine the readers it is asked for, from named readers and spelled-out ones');
	eq(ctl.authorized_line('ssh-ed25519 AAAAC3x wwand-rsim\n', hosts['root@pc.lan']),
	   'command="rsim-card --serve \'bt:AA:BB:CC:DD:EE:FF\' \'wbsm:\'",no-pty,no-port-forwarding,no-agent-forwarding,no-X11-forwarding ssh-ed25519 AAAAC3x wwand-rsim',
	   'ssh: the key restricted to rsim-card --serve for those readers');
	eq(ctl.authorized_line('k', [ 'pcsc:SCM SCR 3310 [CCID Interface] 00 00', 'at:/dev/tty*' ]),
	   'command="rsim-card --serve \'pcsc:SCM SCR 3310 \\[CCID Interface\\] 00 00\' \'at:/dev/tty\\*\'",no-pty,no-port-forwarding,no-agent-forwarding,no-X11-forwarding k',
	   'ssh: a reader name\'s [ ] * stay literal (fnmatch there)');
	ok(index(ctl.authorized_line('k', [ 'wbsm:' ], '/home/u/.local/bin/rsim-card'), 'command="\'/home/u/.local/bin/rsim-card\' --serve \'wbsm:\'"') == 0,
	   'ssh: rsim-card outside the PATH: the reader\'s helper path in command=');
	ok(index(ctl.authorized_line('k', [ "pcsc:O'Reilly \"R\" 00" ]), "'pcsc:O?Reilly ?R? 00'") > 0,
	   'ssh: a quote in a reader name becomes ? (still matches it), not dropped (a line that never matched)');
	ok(index(ctl.ssh_diagnose([ 'root@pc.lan: Permission denied (publickey).' ], 'root@pc.lan', true), 'authorized_keys') >= 0,
	   'ssh: a key not accepted — where it goes');
	ok(index(ctl.ssh_diagnose([], 'root@pc.lan', false), 'ssh-key root@pc.lan') >= 0, 'ssh: no key yet — how to make one');
	ok(index(ctl.ssh_diagnose([ 'sh: rsim-card: not found' ], 'root@pc.lan', true), 'apk add rsim-card') >= 0,
	   'ssh: rsim-card missing there');
	ok(index(ctl.ssh_diagnose([ 'rsim-card --serve: this reader is not served to this key: pcsc:0 — this key is for wwand-rsim only' ], 'root@pc.lan', true), 'restricted') >= 0,
	   'ssh: a restricted key that does not allow it');
	ok(index(ctl.ssh_diagnose([ 'Host key verification failed.' ], 'root@pc.lan', true), 'known_hosts') >= 0, 'ssh: a changed host key');
	ok(index(ctl.ssh_diagnose([ 'dbclient: Connection to root@pc.lan:22 exited: Error connecting: Connection refused' ], 'root@pc.lan', true), 'cannot be reached') >= 0,
	   'ssh: not reachable');
	eq(ctl.ssh_diagnose([ 'something else' ], 'h', true), null, 'ssh: nothing fits, no guess');
	// ucode resolves a name where the function using it is compiled: every
	// helper ssh_key calls must be declared above it (HW-found on 245)
	let src = require('fs').readfile(sourcepath(0, true) + '/../ctl/rsim.uc') ?? '';

	for (let f in [ 'readers_now', 'modems_now', 'ssh_hosts', 'authorized_line' ])
		ok(index(src, 'function ' + f + '(') >= 0 && index(src, 'function ' + f + '(') < index(src, 'function ssh_key('),
		   sprintf('ssh-key: %s is declared before ssh_key', f));
	eq(ctl.scan_parse('Warning: Permanently added\n{"done":true}\n', null).noise, [ 'Warning: Permanently added' ],
	   'scan: what the far side said besides JSON is kept for the diagnosis');
}

// --- test: ONE named source, opened like a session; the scan never does it --------
{
	let ran = [];
	let answers = {};
	let sys = {
		helper: '/usr/bin/rsim-card',
		status: () => ({ modems: { m0: { at_tty: '/dev/ttyUSB2' } } }),
		ssh_sys: { flavor: 'dropbear', exists: () => true },
		run: (cmd, input) => { push(ran, [ cmd, input ]); for (let k, v in answers) if (index(cmd, k) >= 0) return v; return ''; },
	};

	answers["'--list'"] = join('\n', [
		'{"backend":"tty","spec":"at:/dev/ttyACM0","device":"/dev/ttyACM0","by_id":"/dev/serial/by-id/usb-SAMSUNG-if01","hint":"at"}',
		'{"backend":"tty","spec":"phoenix:/dev/ttyUSB0","device":"/dev/ttyUSB0","hint":"phoenix"}',
		'{"backend":"tty","spec":"","device":"/dev/ttyUSB8","hint":"diag","role":"diag"}',
		'{"done":true}' ]) + '\n';
	answers["'at:/dev/ttyACM0'"] = 'rsim-card: /dev/ttyACM0: the modem refuses AT+CSIM (ERROR) — its card cannot be used this way\n';
	answers["'wbsm:'"] = join('\n', [
		'{"event":"info","backend":"phoenix","reader":"wbsm:","usb_product":"Smartmouse USB"}',
		'{"ok":true,"atr":"3B9E96"}',
		'{"ok":true,"present":true,"powered":true,"backend":"phoenix","reader":"wbsm:","atr":"3B9E96","clock_khz":3580}' ]) + '\n';

	eq(ctl.test_source({}, [ 'wbsm:', '--json' ], sys), 0, 'test: a reader that works is 0');
	eq(ran[0][1], '{"op":"power_up"}\n{"op":"status"}\n', 'test: power-up and status, then its end of input hands the card back');
	eq(ctl.test_source({}, [ '/dev/serial/by-id/usb-SAMSUNG-if01', '--json' ], sys), 1, 'test: a port named by its stable name');
	ok(index(ran[length(ran) - 1][0], "'at:/dev/ttyACM0'") >= 0, 'test: ...is what the scan says it is (a modem port: at:)');
	ok(index(ran[length(ran) - 1][0], "'--at-radio' 'keep'") >= 0, 'test: ...and its radio is left as it is');
	eq(ctl.test_source({}, [ 'at:/dev/ttyUSB2' ], sys), 1, 'test: a port of wwand\'s own modem is refused');
	eq(length(filter(ran, (r) => index(r[0], 'ttyUSB2') >= 0)), 0, 'test: ...without being opened');
	eq(ctl.test_source({}, [ '/dev/ttyS9' ], sys), 1, 'test: a device the scan does not know is not guessed at');
	let before = length(ran);

	eq(ctl.test_source({}, [ '/dev/ttyUSB8' ], sys), 1, 'test: a diagnostic port is refused');
	eq(length(ran), before + 1, 'test: ...after the (passive) scan, without opening it');
	eq(ctl.test_source({}, [ 'at:/dev/ttyACM0', 'root@h;x' ], sys), 2, 'test: a host is user@host');
	ran = [];
	ctl.test_source({}, [ 'bt:34:82:C5:58:C9:21', 'root@simhost' ], sys);
	ok(index(ran[0][0], 'root@simhost') >= 0 && index(ran[0][0], "'bt:34:82:C5:58:C9:21'") >= 0, 'test: on another machine, over SSH');
	ran = [];
	answers["'rspro:bank.lan/1:0'"] = join('\n', [
		'{"event":"info","backend":"rspro","reader":"rspro:bank.lan/1:0","server":"bank.lan:9998","client":"0:0","bank":"1:0","mapping":"helper"}',
		'{"ok":true,"atr":"3B9F96"}',
		'{"ok":true,"present":true,"powered":true,"backend":"rspro","reader":"rspro:bank.lan/1:0","atr":"3B9F96"}' ]) + '\n';
	eq(ctl.test_source({}, [ 'rspro:bank.lan/1:0', '--json' ], sys), 0, 'test: a SIM bank slot');
	eq(ran[0][0], "'/usr/bin/rsim-card' 'rspro:bank.lan/1:0'", 'test: ...the helper as a remsim client, here');
	ran = [];
	eq(ctl.test_source({}, [ 'rspro:bank.lan/1:0', 'root@simhost' ], sys), 1, 'test: a SIM bank is not asked over SSH');
	eq(length(ran), 0, 'test: ...and nothing is run');
	ran = [];
	answers["'rspro:bank.lan/1:0' '--rspro-client'"] = answers["'rspro:bank.lan/1:0'"];
	eq(ctl.test_source({}, [ 'rspro:bank.lan/1:0', '--rspro-client', '10:1', '--rspro-rest-port', '8997', '--json' ], sys), 0,
	   'test: a SIM bank slot as a given client, on a given REST port');
	eq(ran[0][0], "'/usr/bin/rsim-card' 'rspro:bank.lan/1:0' '--rspro-client' '10:1' '--rspro-rest-port' '8997'",
	   'test: ...both passed to the helper');
	eq(ctl.test_source({}, [ 'rspro:bank.lan/1:0', '--rspro-client', '70000' ], sys), 2, 'test: a client id over 65535 is refused');
	eq(ctl.test_source({}, [ 'rspro:bank.lan:0/1:0' ], sys), 1, 'test: port 0 is no SIM bank spec');
}

done('test_ctl_rsim');
