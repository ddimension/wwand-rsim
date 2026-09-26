// wwand-rsim tests — end to end on the host: the plugin, the real rsim-card
// helper spawned the way the daemon spawns it, and a simulated Phoenix reader
// with a SIM on a pseudo-terminal (tests/e2e/card_rig.py). Only the modem is
// scripted. run_tests.sh sets RSIM_E2E_TTY / RSIM_E2E_HELPER when the helper
// is built; without them there is nothing to run against.

'use strict';

import * as uloop from 'uloop';
import { eq, ok, done } from './lib/check.uc';

let rsim = require('wwand.plugins.rsim');
let tty = getenv('RSIM_E2E_TTY');
let helper = getenv('RSIM_E2E_HELPER');

uloop.init();

function run_until(pred, ms)
{
	let t;
	let poll;

	poll = uloop.interval(20, () => { if (pred()) uloop.end(); });
	t = uloop.timer(ms, () => uloop.end());
	uloop.run();
	poll.cancel();
	t.cancel();
}

if (tty && helper) {
	let c = { destroyed: false, handlers: {}, sent: [] };

	c.on = (n, cb) => { c.handlers[n] = c.handlers[n] ?? []; push(c.handlers[n], cb); };
	c.request = (n, a, cb) => { push(c.sent, { name: n, args: a }); uloop.timer(0, () => cb(null, {})); };
	c.fire = (n, d) => { for (let h in (c.handlers[n] ?? [])) h(d); };

	let events = () => map(filter(c.sent, (s) => s.name == 'EVENT'), (s) => s.args.info.event);
	let apdus = () => filter(c.sent, (s) => s.name == 'APDU');
	let logs = [];
	let p = rsim.create({
		log: (l, m) => push(logs, m),
		helper_path: helper,
		modem_of: () => ({ modem: {} }),
		qmi_client: (ref, schema, cb) => cb(null, c),
		qmi_release: (ref, cl) => { cl.released = true; },
	});
	let ext = { rsim_reader: 'phoenix:' + tty };

	p.tick('m0', ext);
	run_until(() => length(events()) >= 2, 8000);
	eq(events(), [ 1, 2 ], 'e2e: the real reader answered the power-up, the card is offered');

	c.fire('CONNECT_IND', { slot: 1 });
	run_until(() => length(events()) >= 3, 8000);

	let ev = filter(c.sent, (s) => s.name == 'EVENT')[2];

	eq(ev?.args?.info?.event, 5, 'e2e: on connect the card is reset');
	eq(rsim.hexs(ev?.args?.atr), '3B9F96801FC78031A073BE21136743200718000001A5',
	   'e2e: ...and its ATR, read from the reader, goes to the modem');

	// SELECT MF (GSM class): two NULL bytes, ACK, data, 9F17 — through the
	// T=0 engine and the echoing single-wire line
	c.fire('APDU_IND', { slot: 1, apdu_id: 1, command: rsim.bytes('A0A40000023F00') });
	// READ BINARY: ~INS twice, then INS for the rest
	c.fire('APDU_IND', { slot: 1, apdu_id: 2, command: rsim.bytes('A0B0000004') });
	run_until(() => length(apdus()) >= 2, 8000);

	eq(rsim.hexs(apdus()[0]?.args?.response), '9F17', 'e2e: SELECT answered by the card');
	eq(apdus()[1]?.args?.apdu_id, 2, 'e2e: the second command after the first');
	eq(length(apdus()[1]?.args?.response ?? []), 6, 'e2e: READ BINARY: 4 data bytes + SW');
	eq(rsim.hexs(slice(apdus()[1]?.args?.response ?? [], 4)), '9000', 'e2e: ...ending 9000');

	let n = length(c.sent);

	p.tick('m0', {});
	run_until(() => c.released, 4000);
	eq(map(slice(c.sent, n), (s) => s.args?.info?.event), [ 3, 0 ], 'e2e: leaving gives the modem its SIM back');
	ok(c.released, 'e2e: and the client back');

	// the helper powers the card down on EOF and exits; give it the time
	run_until(() => false, 300);
}
else {
	ok(true, 'e2e: skipped — the helper is not built (helper/build/rsim-card)');
}

done('test_e2e_plugin');
