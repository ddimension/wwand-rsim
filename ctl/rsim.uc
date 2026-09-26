// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 André Valentin <avalentin@marcant.net>
// `wwandctl rsim`: the remote SIM from the command line. A wwandctl command
// from an optional package (installed at /usr/share/ucode/wwand/ctl/rsim.uc);
// wwandctl hands it { call, call_ok, status, resolve_modem }.
//
// Besides the plugin's state this is where the modem's firmware switch is
// read and set. UIM Remote is compiled into Qualcomm modem firmware but
// registered only when one EFS item says so, and it is read at modem boot —
// both the QMI service and the card driver's remote mode hang off it; no NV
// item is involved (the NV-453/sap_security_restrictions recipes that
// circulate are for Bluetooth SAP, another service). It reads 00 on every
// Quectel modem we have (RG650E, RG502Q, RM520N-GL; HW-observed 2026-09-26).
// Setting it is a change to the modem's own configuration, so it happens only
// when an operator runs `enable`, never from the plugin.
//
// Plain script: require() returns { run, help } plus the pure pieces the
// tests reach.

'use strict';

import * as fs from 'fs';
import * as libuci from 'uci';

const EFS_ENABLE = '/nv/item_files/modem/uim/remote/uim_remote_service_enable';
// how long the modem waits for a remote card's answer; shown, not set —
// absent on most firmware, where the modem uses its built-in default
const EFS_RESP_TIMER = '/nv/item_files/modem/uim/uimdrv/nv_remote_command_resp_timer';
// A modem LENDING its card through the SIM Access Profile server: the UIM
// service answers SAP_CONNECTION with ACCESS_DENIED (82) while this item is
// absent — HW-observed on the RG650E, 2026-09-26, where it does not exist and
// its neighbour apdu_security_restrictions reads 00 (and our APDUs pass).
// `sap-enable` writes 00 by that analogy; what the firmware makes of other
// values is not known, so nothing else is offered. The item did not exist
// before: writing it cannot be undone from here, only overwritten.
const EFS_SAP = '/nv/item_files/modem/qmi/uim/sap_security_restrictions';

// Quectel reads an EFS item as hex: `+QNVFR: 01`. null when the answer is
// anything else (no such item: ERROR; not a Quectel: unknown command).
function qnvfr_value(lines)
{
	for (let l in (lines ?? [])) {
		let m = match(l, /^\+QNVFR:\s*"?([0-9A-Fa-f]*)"?\s*$/);

		if (m)
			return uc(m[1]);
	}

	return null;
}

function efs_read(ctx, modem, path)
{
	let r = ctx.call('modem_at', { modem: modem, command: sprintf('AT+QNVFR="%s"', path), timeout: 5000 });

	if (!r?.ok)
		return { error: r?.error ?? 'no answer', lines: r?.response };

	let v = qnvfr_value(r.response);

	return (v == null) ? { error: 'unexpected answer', lines: r.response } : { value: v };
}

function efs_write(ctx, modem, path, hexval)
{
	let r = ctx.call('modem_at', { modem: modem, command: sprintf('AT+QNVFW="%s",%s', path, hexval), timeout: 5000 });

	return r?.ok ? {} : { error: r?.error ?? 'no answer', lines: r?.response };
}

function state_word(v)
{
	return (v == null) ? 'unknown' : (hex(v) ? 'on' : 'off');
}

// the plugin status as [ label, text ] lines
function status_lines(st)
{
	if (type(st) != 'object')
		return [];

	if (st.config_error)
		return [ [ 'remote SIM', st.config_error ] ];

	if (!st.enabled)
		return [ [ 'remote SIM', 'not configured on this modem (option rsim, or rsim_reader)' ] ];

	let out = [];
	let what = {
		idle: 'waiting to start', starting: 'starting',
		waiting: 'offered to the modem, waiting for it to connect',
		connected: 'modem connected, card not powered',
		powered: 'in use by the modem',
		failed: 'stopped',
	};

	push(out, [ 'remote SIM', sprintf('%s · slot %d · %s',
		st.reader_name ? sprintf('%s (%s)', st.reader_name, st.reader) : st.reader,
		st.slot ?? 1, st.conflict ? sprintf('not used: %s', st.conflict) : (what[st.state] ?? st.state)) ]);

	if (st.atr)
		push(out, [ 'card', sprintf('ATR %s', st.atr) ]);

	if (st.apdus)
		push(out, [ 'commands', sprintf('%d · last SW %s', st.apdus, st.last_sw ?? '?') ]);

	if (st.last_error)
		push(out, [ 'last error', (st.retry_at != null && st.now != null && st.retry_at > st.now)
			? sprintf('%s (retry in %d s)', st.last_error, st.retry_at - st.now)
			: st.last_error ]);

	return out;
}

// The router's own SSH key for a reader on another machine (rsim_reader
// 'ssh:user@host:reader'): created once, the public half printed for the
// other machine's authorized_keys. Dropbear and OpenSSH keep keys in
// different formats, so it is made with the tool of the ssh the plugin will
// run. `run` is injectable for the tests.
const KEY_DIR = '/etc/wwand/rsim';

function ssh_key(run)
{
	run = run ?? ((cmd) => {
		let p = fs.popen(cmd, 'r');
		let out = p ? p.read('all') : null;
		let rc = p ? p.close() : -1;

		return { rc: rc, out: out ?? '' };
	});

	let l = fs.readlink('/usr/bin/ssh');
	let dropbear = (l && (index(l, 'dbclient') >= 0 || index(l, 'dropbear') >= 0)) ||
	               (!fs.access('/usr/bin/ssh') && fs.access('/usr/bin/dbclient'));
	let key = sprintf('%s/%s', KEY_DIR, dropbear ? 'id_dropbear' : 'id_ed25519');

	// parents first: fs.mkdir makes one level, and /etc/wwand does not
	// exist on a box that never needed it
	fs.mkdir('/etc/wwand', 0o755);
	fs.mkdir(KEY_DIR, 0o700);

	if (!fs.access(key)) {
		let r = run(dropbear
			? sprintf("dropbearkey -t ed25519 -f '%s' >/dev/null 2>&1", key)
			: sprintf("ssh-keygen -q -t ed25519 -N '' -C wwand-rsim -f '%s' >/dev/null 2>&1", key));

		if (r.rc != 0 || !fs.access(key))
			die(sprintf('creating %s failed (%s)', key, dropbear ? 'dropbearkey' : 'ssh-keygen'));

		printf('created %s\n', key);
	}

	let pub = run(dropbear ? sprintf("dropbearkey -y -f '%s' 2>/dev/null | grep '^ssh-'", key)
	                       : sprintf("cat '%s.pub'", key));

	if (pub.rc != 0 || !length(trim(pub.out)))
		die(sprintf('cannot read the public key of %s', key));

	printf('%s\n', trim(pub.out));
	printf('add this line to ~/.ssh/authorized_keys of the user on the machine with the reader\n');
}

// The named readers and which modem names each (option rsim). A reader is
// used by one modem at a time; a second one naming it waits.
function list_readers(ctx)
{
	let c = libuci.cursor();
	let rows = [], users = {};

	c.load('network');
	c.foreach('network', 'wwand_modem', (m) => {
		if (m.rsim)
			push(users[m.rsim] ??= [], m['.name']);
	});
	c.foreach('network', 'wwand_simreader', (r) => {
		let where = (r.type == 'modem')
			? sprintf('modem %s lends its card (%s)', r.donor ?? '?', r.donor_mode ?? 'sap')
			: sprintf('%s%s%s', r.type ?? '?', length(r.device ?? '') ? ' ' + r.device : '',
			          length(r.host ?? '') ? ' on ' + r.host : '');

		push(rows, sprintf('%-14s%s · %s', r['.name'], where,
			users[r['.name']] ? 'used by ' + join(', ', users[r['.name']]) : 'not assigned'));
	});

	if (!length(rows))
		printf('no SIM readers defined — add one: uci add network wwand_simreader (or LuCI: Network → Remote SIM)\n');

	for (let l in rows)
		printf('%s\n', l);
}

function set_switch(ctx, modem, on, reset)
{
	let want = on ? '01' : '00';
	let cur = efs_read(ctx, modem, EFS_ENABLE);

	if (cur.error)
		die(sprintf('modem %s: cannot read the UIM Remote switch (%s)%s — this needs a Quectel modem with AT+QNVFR',
			modem, cur.error, cur.lines ? sprintf(': %s', join(' | ', cur.lines)) : ''));

	if (cur.value == want) {
		printf('modem %s: UIM Remote is already %s in the firmware\n', modem, state_word(want));
	}
	else {
		let w = efs_write(ctx, modem, EFS_ENABLE, want);

		if (w.error)
			die(sprintf('modem %s: writing the switch failed (%s)', modem, w.error));

		let back = efs_read(ctx, modem, EFS_ENABLE);

		if (back.value != want)
			die(sprintf('modem %s: the switch reads %s after writing %s — not changed', modem, back.value ?? '?', want));

		printf('modem %s: UIM Remote switched %s in the firmware (%s: %s -> %s)\n',
			modem, state_word(want), EFS_ENABLE, cur.value, want);
	}

	// read at modem boot only: the running firmware does not change until
	// the modem restarts
	if (reset) {
		ctx.call_ok('modem_reset', { modem: modem });
		printf('modem %s: reset — it comes back with the new setting\n', modem);
	}
	else if (cur.value != want) {
		printf('takes effect after a modem reset: wwandctl reset %s\n', modem);
	}
}

return {
	qnvfr_value: qnvfr_value,
	ssh_key: ssh_key,
	status_lines: status_lines,
	EFS_ENABLE: EFS_ENABLE,
	EFS_SAP: EFS_SAP,

	help: [
		'rsim [modem] [status]                 remote SIM: reader, card, state',
		'rsim [modem] switch                   the modem firmware switch for UIM Remote',
		'rsim [modem] enable|disable [--reset] set it (a modem reset applies it)',
		'rsim [modem] restart                  give the modem its own SIM back, then offer the remote one again',
		'rsim ssh-key                          the router\'s key for a reader on another machine (ssh:user@host:reader)',
		'rsim readers                          the SIM readers defined (config wwand_simreader) and who uses them',
		'rsim [modem] probe                    what this modem\'s UIM offers for lending its card (read-only)',
		'rsim [modem] donor-test [sap|apdu] [qmi|at]  lend its card once: ATR + SELECT MF, then hand it back',
		'rsim [modem] sap-switch               the firmware switch for lending the card over SIM Access',
		'rsim [modem] sap-enable [--reset]     allow it (Quectel; writes the EFS item, a modem reset applies it)',
	],

	run: function(ctx, args) {
		// not about a modem: must not require one
		if (args[0] == 'ssh-key')
			return ssh_key();

		if (args[0] == 'readers')
			return list_readers(ctx);

		let r = ctx.resolve_modem(ctx.status(), args[0]);
		let rest = r.consumed ? slice(args, 1) : args;
		let op = rest[0] ?? 'status';

		switch (op) {
		case 'status': {
			let st = ctx.call_ok('modem_plugin', { modem: r.modem, plugin: 'rsim', op: 'status' });

			for (let l in status_lines(st))
				printf('%-13s%s\n', l[0], l[1]);

			break;
		}

		case 'switch': {
			let cur = efs_read(ctx, r.modem, EFS_ENABLE);

			if (cur.error)
				die(sprintf('modem %s: cannot read the UIM Remote switch (%s)', r.modem, cur.error));

			printf('%-13s%s (%s = %s)\n', 'UIM Remote', state_word(cur.value), EFS_ENABLE, cur.value);

			let t = efs_read(ctx, r.modem, EFS_RESP_TIMER);

			printf('%-13s%s\n', 'resp timer', t.error ? 'not set (firmware default)' : t.value);
			break;
		}

		case 'enable':
		case 'disable':
			set_switch(ctx, r.modem, op == 'enable', rest[1] == '--reset');
			break;

		case 'probe': {
			let r2 = ctx.call_ok('modem_plugin', { modem: r.modem, plugin: 'rsim', op: 'probe' });

			printf('%-13s%s\n', 'UIM messages', r2?.msgs ? join(' ', map(r2.msgs, (m) => sprintf('0x%02X', m)))
			                                              : sprintf('not listed by this firmware (%J)', r2?.msgs_error));
			printf('%-13s%s\n', 'SIM Access', !r2?.sap ? '?'
				: !r2.sap.supported ? 'not in this firmware'
				: r2.sap.error ? sprintf('present, status query failed (%J)', r2.sap.error)
				: sprintf('present, link %s', r2.sap.state_name ?? r2.sap.state));
			break;
		}

		case 'donor-test': {
			let mode = (rest[1] == 'apdu') ? 'apdu' : 'sap';
			let apdu = (index([ 'qmi', 'at' ], rest[2]) >= 0) ? rest[2] : 'auto';

			printf('lending the card of %s once (%s%s) — its own connection drops meanwhile\n',
				r.modem, mode, (mode == 'apdu') ? sprintf(' over %s', apdu) : '');

			let res = ctx.call_ok('modem_plugin', { modem: r.modem, plugin: 'rsim', op: 'donor_test',
			                                        args: { mode: mode, apdu: apdu } });

			for (let st in (res?.steps ?? []))
				printf('  %s\n', st);
			printf('%s\n', res?.ok ? 'ok: this modem can lend its card this way'
			                        : sprintf('failed: %s', res?.error ?? '?'));
			break;
		}

		case 'sap-switch': {
			let cur = efs_read(ctx, r.modem, EFS_SAP);

			printf('%-13s%s\n', 'SIM Access', cur.error ? sprintf('not set (%s) — lending over SIM Access is denied', cur.error)
			                                          : sprintf('%s = %s', EFS_SAP, cur.value));
			break;
		}

		case 'sap-enable': {
			let cur = efs_read(ctx, r.modem, EFS_SAP);

			if (!cur.error && cur.value == '00') {
				printf('modem %s: lending over SIM Access is already allowed (%s = 00)\n', r.modem, EFS_SAP);
			}
			else {
				let w = efs_write(ctx, r.modem, EFS_SAP, '00');

				if (w.error)
					die(sprintf('modem %s: writing %s failed (%s)', r.modem, EFS_SAP, w.error));

				let back = efs_read(ctx, r.modem, EFS_SAP);

				if (back.value != '00')
					die(sprintf('modem %s: %s reads %s after writing 00 — not changed', r.modem, EFS_SAP, back.value ?? back.error ?? '?'));

				printf('modem %s: %s written (00) — lending over SIM Access allowed after a modem reset\n', r.modem, EFS_SAP);
			}

			if (rest[1] == '--reset') {
				ctx.call_ok('modem_reset', { modem: r.modem });
				printf('modem %s: reset\n', r.modem);
			}
			break;
		}

		case 'restart':
			ctx.call_ok('modem_plugin', { modem: r.modem, plugin: 'rsim', op: 'restart' });
			printf('modem %s: remote SIM restarted — `wwandctl rsim` shows how it goes\n', r.modem);
			break;

		default:
			die('usage: wwandctl rsim [modem] [status|switch|enable [--reset]|disable [--reset]|restart|probe|donor-test|sap-switch|sap-enable [--reset]]');
		}
	},
};
