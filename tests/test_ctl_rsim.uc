// wwand-rsim tests — `wwandctl rsim`: the status lines and the firmware
// switch, against a scripted modem AT port.

'use strict';

import { eq, ok, done } from './lib/check.uc';

let ctl = require('wwand.ctl.rsim');

// --- reading the switch --------------------------------------------------------
{
	eq(ctl.qnvfr_value([ '+QNVFR: 00', 'OK' ]), '00', 'qnvfr: the value');
	eq(ctl.qnvfr_value([ '+QNVFR: "01"' ]), '01', 'qnvfr: quoted as some firmware does');
	eq(ctl.qnvfr_value([ 'ERROR' ]), null, 'qnvfr: no item is no value, not 00');
}

// --- status lines ----------------------------------------------------------------
{
	eq(ctl.status_lines({ enabled: false }), [ [ 'remote SIM', 'not configured on this modem (option rsim_reader)' ] ],
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

done('test_ctl_rsim');
