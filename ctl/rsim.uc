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
import * as libubus from 'ubus';

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
// what a reader is, in one line, from its info event (or a scan row)
function reader_text(i)
{
	let parts = [];
	let add = (v) => { if (v != null && v !== '') push(parts, v); };

	add(join(' ', filter([ i.modem_manufacturer, i.modem_model ], (x) => x)));
	add(i.modem_revision ? 'fw ' + i.modem_revision : null);
	add(i.host ? 'on ' + i.host : null);
	add(i.name ? sprintf('"%s"%s', i.alias ?? i.name, i.kind ? ' (' + i.kind + ')' : '') : null);
	add(i.vendor);
	add(i.key ? i.key + ' pairing' : null);
	add(i.channel ? sprintf('channel %d', i.channel) : null);
	add(join(' ', filter([ i.usb_manufacturer, i.usb_product ], (x) => x)));
	add(i.usb_serial ? 'serial ' + i.usb_serial : null);
	add(i.usb_path ? 'USB ' + i.usb_path : null);
	add(i.reader_name);
	add(i.ifd_type);
	add(i.protocol);
	add(i.clock_khz ? sprintf('%d kHz, %s reset', i.clock_khz, i.reset_line ?? '?') : null);
	add(i.operator ? sprintf('%s%s', i.operator, i.rat ? ' ' + i.rat : '') : null);
	add(i.iccid ? 'ICCID ' + i.iccid : null);
	add(i.by_id);

	return length(parts) ? join(' · ', parts) : (i.backend ?? '?');
}

function status_lines(st)
{
	if (type(st) != 'object')
		return [];

	// its own card lent out: to a modem here, or to another router
	let lend = [];
	let lt = st.lent_to;

	if (lt)
		push(lend, [ 'lends card', sprintf('to %s (%s%s, radio off)%s', lt.to, lt.remote ? 'another router, ' : '',
			(lt.mode == 'apdu') ? 'APDU' : 'SIM Access', lt.commands ? sprintf(' · %d commands', lt.commands) : '') ]);
	if (st.lend_hold)
		push(lend, [ 'lending', 'stopped here (`wwandctl rsim MODEM lend-allow` allows it again)' ]);

	if (st.config_error)
		return [ [ 'remote SIM', st.config_error ], ...lend ];

	if (!st.enabled)
		return [ [ 'remote SIM', 'not configured on this modem (option rsim, or rsim_reader)' ], ...lend ];

	let out = [ ...lend ];
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

	let ri = st.reader_info;

	if (ri)
		push(out, [ 'reader', reader_text(ri) ]);

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

// the configuration as it is now (ucode resolves names where a function is
// compiled: these must come before their users)
function readers_now()
{
	let out = {};
	let c = libuci.cursor();

	c.load('network');
	c.foreach('network', 'wwand_simreader', (sec) => { out[sec['.name']] = sec; });
	return out;
}

function modems_now()
{
	let out = {};
	let c = libuci.cursor();

	c.load('network');
	c.foreach('network', 'wwand_modem', (sec) => { out[sec['.name']] = sec; });
	return out;
}

// The readers each machine is asked for over SSH, from the configuration:
// { 'user@host': [ spec, ... ] } — what its authorized_keys line has to
// allow. readers: the wwand_simreader sections, modems: the wwand_modem ones.
function ssh_hosts(readers, modems)
{
	let rsim = require('wwand.plugins.rsim');
	let out = {};
	let add = (spec) => {
		let r = rsim.ssh_split(spec ?? '');

		if (!r)
			return;
		out[r.dest] ??= [];
		if (index(out[r.dest], r.reader) < 0)
			push(out[r.dest], r.reader);
	};

	for (let name, sec in (readers ?? {}))
		add(rsim.reader_options(sec)?.rsim_reader);
	for (let name, sec in (modems ?? {}))
		add(sec.rsim_reader);

	return out;
}

// One authorized_keys line: the key, allowed to run wwand-rsim's own calls
// for these readers and nothing else (rsim-card --serve; no shell, no
// terminal, no forwarding). Without specs it serves every reader there.
function authorized_line(pub, specs, helper)
{
	// a spec is taken as an fnmatch pattern there: a reader name's own
	// * ? [ ] (PC/SC names carry "[CCID Interface]") must stay literal
	let lit = (x) => join('', map(split(replace(x, /'/g, ''), ''),
		(ch) => (index('*?[]\\', ch) >= 0) ? '\\' + ch : ch));
	// rsim-card outside that user's PATH: the reader's `helper` path
	let bin = length(helper ?? '') ? "'" + replace(helper, /'/g, '') + "'" : 'rsim-card';
	let cmd = join(' ', [ bin, '--serve', ...map(specs ?? [], (x) => "'" + lit(x) + "'") ]);

	return sprintf('command="%s",no-pty,no-port-forwarding,no-agent-forwarding,no-X11-forwarding %s',
		replace(cmd, /"/g, ''), trim(pub));
}

function ssh_key(args, run)
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

	// dropbear keeps no .pub: written next to it, for the LuCI page
	if (dropbear && !fs.access(key + '.pub'))
		fs.writefile(key + '.pub', trim(pub.out) + ' wwand-rsim\n');

	let rd = readers_now(), md = modems_now();
	let hosts = ssh_hosts(rd, md);
	// the rsim-card path a machine was given (option helper / rsim_ssh_helper)
	let helper_of = (h) => {
		for (let n, r in rd)
			if (r.host == h && length(r.helper ?? ''))
				return r.helper;
		for (let n, m in md)
			if (index(m.rsim_reader ?? '', 'ssh:' + h + ':') == 0 && length(m.rsim_ssh_helper ?? ''))
				return m.rsim_ssh_helper;
		return null;
	};
	let want = filter(args ?? [], (a) => substr(a, 0, 1) != '-')[0];

	if (want != null && !hosts[want])
		hosts = { [want]: [] };
	else if (want != null)
		hosts = { [want]: hosts[want] };

	printf('%s\n\n', trim(pub.out));

	if (!length(keys(hosts))) {
		printf('No reader on another machine is configured yet. For one, put this line into the\n' +
		       'authorized_keys of the user there (restricted to wwand-rsim; add the readers it\n' +
		       'may serve after --serve, e.g. \'bt:AA:BB:CC:DD:EE:FF\' or \'wwand:*\'):\n\n%s\n',
		       authorized_line(pub.out, []));
		return;
	}

	for (let h, specs in hosts) {
		let user = split(h, '@')[0];

		printf('%s — %s:\n', h, length(specs) ? join(', ', specs) : 'any reader');
		printf('  into %s:\n\n%s\n\n', (user == 'root') ? '/etc/dropbear/authorized_keys (OpenWrt) or /root/.ssh/authorized_keys'
		                                             : sprintf('~%s/.ssh/authorized_keys', user),
		       authorized_line(pub.out, specs, helper_of(h)));
	}

	printf('The line lets this router run wwand-rsim\'s own calls there and nothing else: rsim-card for\n' +
	       'those readers, its scan, and on a wwand router `wwandctl rsim proxy` (wwand-rsim needed there).\n' +
	       'rsim-card must be in that user\'s PATH. Check with: wwandctl rsim scan <user@host>\n');
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

// `rsim scan [user@host] [--json]`: what a machine offers as a card source —
// PC/SC readers (with or without a card), Smartmouse USB readers, serial
// ports that look like a Phoenix adapter or a modem's AT port, paired phones
// (Bluetooth SIM Access; whether a phone offers it is what BlueZ last read
// from it — the phone is not called up, that would take its SIM) — from
// `rsim-card --list`, run here or on a SIM host over SSH exactly as a
// session would run it. Ports of wwand's own modems here are marked: an at:
// reader on one of them would take a card from under wwand (a sponsor is
// the `modem` kind). Each row carries the spec to put into a reader.
// The lines of `rsim-card --list` -> { done, rows }; with st (a status
// answer, for a scan HERE) the ports of wwand's own modems are marked.
function scan_parse(out, st)
{
	let rows = [], done = null, noise = [];

	for (let l in split(out ?? '', '\n')) {
		let o = null;

		try { o = json(l); } catch (e) { o = null; }
		if (type(o) != 'object') {
			// what ssh or the far side said instead (for the diagnosis)
			if (length(trim(l)))
				push(noise, trim(l));
			continue;
		}
		if (o.done)
			done = o;
		else
			push(rows, o);
	}

	for (let name, m in (st?.modems ?? {}))
		for (let k in [ 'at_tty', 'gps_port', 'diag_port' ])
			for (let r in rows)
				if (r.device && r.device == m[k])
					r.in_use = sprintf('%s of wwand modem %s', (k == 'at_tty') ? 'AT port' : (k == 'gps_port') ? 'GPS port' : 'diag port', name);

	// on a machine with wwand-rsim its own list says which ports its modems
	// use — the same marks for a scan from another router
	for (let w in rows)
		for (let k, dev in (w.modem_ports ?? {}))
			for (let r in rows)
				if (dev && r.backend == 'tty' && r.device == dev && !r.in_use)
					r.in_use = sprintf('%s of wwand modem %s there', (k == 'at') ? 'AT port' : (k == 'gps') ? 'GPS port' : 'diag port', w.modem);

	return { done: done, rows: rows, noise: noise };
}

// What went wrong on the way to a SIM host, from what ssh and the far side
// said, as advice. null when nothing fits.
function ssh_diagnose(lines, host, key_exists)
{
	let all = join('\n', lines ?? []);

	if (key_exists === false)
		return sprintf('this router has no SSH key yet — `wwandctl rsim ssh-key %s` creates it and prints the line for %s', host, host);
	if (match(all, /wwand-rsim only/))
		return sprintf('the key is restricted on %s and does not allow this call — `wwandctl rsim ssh-key %s` prints the line to use', host, host);
	if (match(all, /Permission denied|publickey|No auth methods|[Aa]uthentication fail/))
		return sprintf('%s does not accept this router\'s key — `wwandctl rsim ssh-key %s` prints the line for its authorized_keys', host, host);
	if (match(all, /Host key verification failed|IDENTIFICATION HAS CHANGED|[Hh]ost key mismatch|fingerprint/))
		return sprintf('the host key of %s changed (reinstalled?) — remove its old entry from /root/.ssh/known_hosts', host);
	if (match(all, /rsim-card: (command )?not found|rsim-card: No such file|exec: rsim-card/))
		return sprintf('rsim-card is not installed on %s, or not in the PATH of that user — `apk add rsim-card` (OpenWrt), or option helper with its path', host);
	if (match(all, /Connection refused|No route to host|[Tt]imed out|Could not resolve|Name or service not known|Network is unreachable|Error connecting|Connection closed/))
		return sprintf('%s cannot be reached over SSH (%s)', host, (lines ?? [])[length(lines ?? []) - 1]);

	return null;
}

// sys: { run(cmd) -> output, status() } — injectable for the tests.
function scan(ctx, args, sys)
{
	let rsim = require('wwand.plugins.rsim');
	let json_out = index(args, '--json') >= 0;
	let host = filter(args, (a) => a != '--json')[0];
	let run = sys?.run ?? ((cmd) => {
		let p = fs.popen(cmd, 'r');
		let out = p ? p.read('all') : null;

		p?.close();
		return out;
	});
	let argv;

	if (host != null) {
		if (!match(host, /^[A-Za-z0-9._-]+@[A-Za-z0-9._-]+$/))
			die('usage: wwandctl rsim scan [user@host] [--json]');

		// the plugin's own SSH command line (key, keepalives, flavour),
		// with --list where a reader would go
		argv = rsim.helper_argv({ reader: '--list', local_reader: '--list',
		                          ssh: { dest: host, helper: 'rsim-card' } }, null, sys?.ssh_sys);
	}
	else
		argv = [ sys?.helper ?? rsim.helper_found(), '--list' ];

	// from a SIM host its ssh's complaints too: they say what to fix
	let out = run(join(' ', map(argv, rsim.shq)) + ((host != null) ? ' 2>&1' : ' 2>/dev/null'));
	// here, not on a SIM host: whose port is it
	let status_of = sys?.status ?? ctx.status;
	let parsed = scan_parse(out, (host == null && status_of) ? status_of() : null);
	let rows = parsed.rows, done = parsed.done;

	// this router's own modems: here a card of one is lent to another modem
	// as a SIM sponsor (modem:<name>), not through the proxy
	if (host == null)
		for (let r in rows)
			if (r.backend == 'wwand')
				r.spec = 'modem:' + r.modem;

	if (done == null) {
		let r = { ok: false, error: sprintf('no answer from rsim-card --list%s', host ? ' on ' + host : ' (package rsim-card)') };

		if (host != null) {
			let key_exists = sys?.key_exists ?? (fs.access(KEY_DIR + '/id_dropbear') || fs.access(KEY_DIR + '/id_ed25519'));

			r.hint = ssh_diagnose(parsed.noise, host, key_exists);
			r.detail = slice(parsed.noise, -4);
		}

		if (json_out)
			printf('%J\n', r);
		else {
			printf('%s\n', r.error);
			for (let l in (r.detail ?? []))
				printf('  %s\n', l);
			if (r.hint)
				printf('hint: %s\n', r.hint);
		}
		return 1;
	}

	if (json_out) {
		printf('%J\n', { ok: true, host: host, backends: split(done.backends ?? '', ','), readers: rows,
		                 note: done.note ?? null, bt_adapters: done.bt_adapters ?? null });
		return 0;
	}

	printf('backends in this rsim-card%s: %s\n', host ? ' on ' + host : '', done.backends ?? '?');

	if (done.note)
		printf('note: %s\n', done.note);

	if (done.bt_adapters)
		printf('bluetooth adapters: %s\n', done.bt_adapters);

	if (!length(rows))
		printf('no reader or port found\n');

	for (let r in rows) {
		if (r.error) {
			printf('%-34s %s\n', r.backend, r.error);
			continue;
		}

		let what = (r.backend == 'pcsc') ? sprintf('PC/SC reader, %s', r.card ? 'card inserted' : 'no card')
			: (r.backend == 'wbsm') ? 'Smartmouse USB'
			: (r.backend == 'wwand') ? sprintf('SIM %s in modem %s%s%s', r.iccid ?? '?', r.modem ?? '?',
				r.config ? sprintf(' — wwand_sim %s%s%s', r.config.name ?? '?',
					r.config.apn ? sprintf(', apn %s', r.config.apn) : '',
					r.config.pin ? ', PIN set' : '') : '',
				r.lendable ? '' : sprintf(' — not now: %s', r.why ?? '?'))
			: (r.backend == 'bt') ? sprintf('phone "%s" over Bluetooth, %s', r.name ?? '?',
				(r.sap === true) ? 'offers SIM Access'
				: (r.sap === false) ? 'SIM Access NOT offered (not supported, or off on the phone)'
				: 'SIM Access unknown (services not read yet)')
			: sprintf('serial %s%s%s', r.driver ?? '?', length(r.usb ?? '') ? ' ' + r.usb : '',
			          (r.hint == 'at') ? ', a modem port' : (r.hint == 'phoenix') ? ', a USB-serial adapter'
			          : (r.hint == 'diag') ? ', a diagnostic port (no AT, not a source)' : '');

		printf('%-34s %s%s\n', length(r.spec ?? '') ? (host ? sprintf('ssh:%s:', host) : '') + r.spec : r.device, what,
		       r.in_use ? sprintf(' — IN USE: %s', r.in_use) : '');

		let meta = reader_text({ ...r, name: (r.backend == 'bt') ? null : r.name, reader_name: null });

		if (meta != (r.backend ?? '?'))
			printf('%-34s   %s\n', '', meta);
	}

	return 0;
}

// `rsim test <spec | /dev/tty…> [user@host] [--json]`: open ONE source the
// way a session would — which is when rsim-card's own checks run (an AT
// port that refuses AT+CSIM, a phone that refuses SIM access) — power the
// card up, read its status and the reader's info, and hand the card back at
// once. The scan never does this on its own: a free AT port is only spoken
// to when it is named here. A bare /dev path is matched against the scan to
// know what it is. Ports of wwand's own modems are refused (an at: reader
// there would take the card from under wwand). sys: { run(cmd, input), status(), helper, ssh_sys } for tests.
function test_source(ctx, args, sys)
{
	let rsim = require('wwand.plugins.rsim');
	let json_out = index(args, '--json') >= 0;
	let rest = filter(args, (a) => a != '--json');
	let target = rest[0];
	let host = rest[1];
	let out = (r) => {
		if (json_out)
			printf('%J\n', r);
		else {
			printf('%s: %s\n', r.spec ?? target ?? '?', r.ok ? sprintf('works — ATR %s', r.atr ?? '?')
				: sprintf('does not work: %s', r.error ?? '?'));
			if (r.info && length(reader_text(r.info)))
				printf('  %s\n', reader_text(r.info));
			for (let l in (r.log ?? []))
				printf('  %s\n', l);
		}
		return r.ok ? 0 : 1;
	};
	let run = sys?.run ?? ((cmd, input) => {
		// the requests as a file: rsim-card reads them, then its end of input
		let path = sprintf('/tmp/wwandctl-rsim-test.%d', +(fs.readlink('/proc/self') ?? 0));

		fs.writefile(path, input);

		let p = fs.popen(sprintf("%s <'%s' 2>&1", cmd, path), 'r');
		let o = p ? p.read('all') : null;

		p?.close();
		fs.unlink(path);
		return o;
	});

	if (!length(target ?? '') || (host != null && !match(host, /^[A-Za-z0-9._-]+@[A-Za-z0-9._-]+$/))) {
		warn('usage: wwandctl rsim test <reader spec | /dev/tty…> [user@host] [--json]\n');
		return 2;
	}

	let ssh = host ? { dest: host, helper: 'rsim-card' } : null;
	// A test leaves a modem's radio as it is (at_radio keep): switching it
	// off and back is for a session, and not every modem takes the way back
	// over AT (an E392 refused CFUN=1 and needed a reset, HW-seen on 245)
	let argv_of = (spec) => rsim.helper_argv({ reader: spec, local_reader: spec, ssh: ssh,
	                                           at_radio: (substr(spec, 0, 3) == 'at:') ? 'keep' : null },
	                                         sys?.helper ?? (host ? null : rsim.helper_found()), sys?.ssh_sys);
	let spec = target;

	// a bare device: what the (passive) scan says it is
	if (substr(target, 0, 5) == '/dev/') {
		let listing = scan_parse(run(join(' ', map(argv_of('--list'), rsim.shq)), ''), null);
		let row = filter(listing.rows, (r) => r.device == target || r.by_id == target || r.by_path == target)[0];

		if (!row)
			return out({ ok: false, spec: null, error: sprintf('%s is not a port the scan knows (a modem port or a USB-serial adapter)', target) });
		if (!length(row.spec ?? ''))
			return out({ ok: false, spec: null, error: sprintf('%s is a %s port, not a card source — not opened', target, row.role ?? row.hint ?? 'special') });
		spec = row.spec;
	}

	// wwand's own ports here: not to be touched
	if (!host) {
		let dev = replace(spec, /^[a-z]+:/, '');
		let st = sys?.status ? sys.status() : ctx.status?.();

		for (let name, m in (st?.modems ?? {}))
			for (let k in [ 'at_tty', 'gps_port', 'diag_port' ])
				if (length(dev) && m[k] == dev)
					return out({ ok: false, spec: spec, error: sprintf('%s is a port of wwand modem %s — not tested (use modem:%s)', dev, name, name) });
	}

	if (!match(spec, /^((phoenix|pcsc|at|bt):.|wbsm:)/))
		return out({ ok: false, spec: spec, error: 'not a reader spec (phoenix:, wbsm:, pcsc:, at:, bt:)' });

	// power_up, status, then end of input: rsim-card hands the card back
	let text = run(join(' ', map(argv_of(spec), rsim.shq)), '{"op":"power_up"}\n{"op":"status"}\n');
	let r = { ok: false, spec: host ? sprintf('ssh:%s:%s', host, spec) : spec, atr: null, info: null, error: null, log: [] };

	for (let l in split(text ?? '', '\n')) {
		let o = null;

		try { o = json(l); } catch (e) { o = null; }

		if (type(o) != 'object') {
			if (length(trim(l)))
				push(r.log, trim(l));
			continue;
		}
		if (o.event == 'info') {
			r.info = { ...o };
			delete r.info.event;
		}
		else if (o.atr != null && o.present == null)
			r.atr = o.atr;
		else if (o.present != null) {
			r.info = { ...(r.info ?? {}), ...o };
			delete r.info.ok;
		}
		if (o.ok === false && r.error == null)
			r.error = o.detail ? sprintf('%s: %s', o.error, o.detail) : o.error;
	}

	r.ok = (r.atr != null && r.error == null);
	if (!r.ok && r.error == null)
		// the helper ended before answering: its own message says why
		r.error = length(r.log) ? r.log[length(r.log) - 1] : 'no answer';
	r.log = slice(r.log, -6);

	return out(r);
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

// `rsim <modem> use <reader|off> [--wait N] [--json]`: put a named SIM reader
// (config wwand_simreader) in the modem's `option rsim`, or take it out, and
// reload — a plugin option, so the modem is not restarted. With --wait, wait
// until the modem RUNS on it: the remote card powered and its identity read
// again (the card-change process) — registered or not — or, for off, its own
// card back and read, registered or searching.
// That is what a caller scripting a test needs — "configured" says nothing
// about whether the card is in use yet. The result is one JSON line with
// --json; exit 0 only when the state was reached.
// sys: { cursor(), sleep(s), now() } — injectable for the tests.
function use_reader(ctx, modem, args, sys)
{
	sys = sys ?? {};

	let cursor = sys.cursor ?? (() => libuci.cursor());
	let sleep = sys.sleep ?? ((s) => system(sprintf('sleep %d', s)));
	let now = sys.now ?? (() => time());
	let target = args[0];
	let json_out = index(args, '--json') >= 0;
	let wi = index(args, '--wait');
	let wv = (wi >= 0) ? args[wi + 1] : null;
	// `--wait --json`: no number after it is the default, not "do not wait"
	let wait = (wi < 0) ? 0 : (wv != null && match(wv, /^[0-9]+$/)) ? +wv : 120;
	let res = { ok: false, modem: modem, reader: (target == 'off') ? null : target };

	let finish = (ok, extra) => {
		res = { ...res, ...(extra ?? {}), ok: ok };

		if (json_out)
			printf('%J\n', res);
		else
			printf('%s\n', ok ? sprintf('%s: %s', modem, res.reader ? sprintf('runs on the card in %s', res.reader) : 'runs on its own card')
			                   : sprintf('%s: %s', modem, res.error ?? 'failed'));

		return ok ? 0 : 1;
	};

	if (!length(target ?? ''))
		die('usage: wwandctl rsim [modem] use <reader|off> [--wait SECONDS] [--json]');

	let c = cursor();

	c.load('network');

	if (c.get('network', modem) != 'wwand_modem')
		return finish(false, { error: sprintf('no wwand_modem section %s', modem) });

	// already on that reader, or already on its own card: nothing changes,
	// so no NEW identity will come — the card in use is the answer
	let was = c.get('network', modem, 'rsim') ?? c.get('network', modem, 'rsim_reader');
	let same = (target == 'off') ? (was == null) : (c.get('network', modem, 'rsim') == target);

	if (target == 'off') {
		// both ways of naming a remote card: a named reader, or one spelled out
		c.delete('network', modem, 'rsim');
		c.delete('network', modem, 'rsim_reader');
	}
	else {
		if (c.get('network', target) != 'wwand_simreader')
			return finish(false, { error: sprintf('no SIM reader %s (config wwand_simreader)', target) });

		c.set('network', modem, 'rsim', target);
	}

	// the identity before the change: "read again" means a different one
	let before = ctx.status()?.modems?.[modem]?.iccid ?? null;

	if (!c.save('network') || !c.commit('network'))
		return finish(false, { error: 'cannot write /etc/config/network' });

	let start = now();

	ctx.call_ok('reload', {});

	if (!wait)
		return finish(true, { state: 'configured' });

	let until = start + wait;
	let st = null, iccid = null;
	// a session of THIS attempt, and its card read: the identity is
	// cleared by the card change and read again — or it is the same card
	// (another reader definition, the card moved), which stays the same
	let cleared = false, fresh_since = null;

	// the plugin ticks every 10 s: the first look is a few seconds away at best
	while (now() < until) {
		sleep(2);

		st = ctx.call('modem_plugin', { modem: modem, plugin: 'rsim', op: 'status' });
		st = st?.ok === false ? null : st;

		let m = ctx.status()?.modems?.[modem];

		iccid = m?.iccid ?? null;

		if (target != 'off') {
			// A failure that is not retried on its own ends the wait at once
			// — one of THIS attempt: the note of the reader before (a held
			// failure) is there until the plugin's next tick.
			let fresh = (st?.error_at != null && st.error_at >= start);

			if (st?.config_error || (fresh && st?.last_error && st?.retry_at == null && st?.state != 'powered'))
				return finish(false, { state: st?.state, error: st?.config_error ?? st?.last_error });

			// another modem holds that reader: waiting will not change it.
			// The plugin notes it on its tick, every 10 s; an older note is
			// not looked at before one tick has passed.
			if (st?.conflict && now() - start > 12)
				return finish(false, { state: st?.state, error: st.conflict });

			// Registration is not part of it: it depends on the network (a
			// test card often has no service at all — HW-observed on 245,
			// 2026-09-26: powered, new identity read, modem REGISTERING),
			// and what a caller of `use` needs is the card in use. The
			// modem's state goes into the result for whoever cares.
			let session_new = (st?.state == 'powered' && st?.since != null && st.since >= start);

			if (session_new && iccid == null)
				cleared = true;
			if (session_new && fresh_since == null)
				fresh_since = now();

			if (st?.state == 'powered' && iccid != null &&
			    (same || iccid != before || (session_new && (cleared || now() - fresh_since >= 20))))
				return finish(true, { state: st.state, iccid: iccid, modem_state: m?.state, atr: st?.atr });
		}
		// its own card READ AGAIN: right after the switch-off the status still
		// shows the remote card's identity, and READY, until the modem has
		// re-read — unless it never was on a remote card
		// registered or still searching, as for a remote card: whether its
		// own card has service is the network's business (HW-observed on 93,
		// 2026-09-27: own card back and read, modem REGISTERING on a test APN)
		else if ((st?.state ?? 'off') == 'off' && iccid != null && index([ 'READY', 'REGISTERING' ], m?.state) >= 0 &&
		         (same || iccid != before))
			return finish(true, { state: 'off', iccid: iccid, modem_state: m.state });
	}

	return finish(false, { state: st?.state, iccid: iccid,
		error: sprintf('not reached within %d s (remote SIM %s%s)', wait, st?.state ?? '?',
		               st?.last_error ? sprintf(': %s', st.last_error) : '') });
}

// ---- this router's SIMs for another router -----------------------------------
//
// Another router's wwand-rsim (`rsim_reader 'ssh:<user>@<here>:wwand:<modem>'`
// or `wwand:iccid:<ICCID>`) runs `wwandctl rsim proxy` here over SSH. The
// proxy speaks rsim-card's line protocol on stdin/stdout and hands every
// request to the rsim plugin in THIS daemon (modem_plugin lend_*), which
// lends the card the way a local SIM sponsor does: over the SIM Access
// Profile or APDU by APDU, with the radio parked meanwhile. Only the daemon
// can do that — it holds the modem — so the proxy is a relay and nothing more.

// an ICCID in any spelling (trailing F of a 19-digit one, lower case,
// blanks) -> its digits; the SIM inventory's rule (siminventory.uc)
function norm_iccid(v)
{
	if (type(v) != 'string')
		return null;

	let d = replace(uc(replace(v, /[ \t-]/g, '')), /F+$/, '');

	return match(d, /^[0-9]{18,20}$/) ? d : null;
}

// The wwand_sim settings for a card, by ICCID: what the other router needs to
// use it (APN, PDP type, login). The PIN itself is not passed on — only
// whether one is set: the card's owner types it into the other router.
function sim_config(sims, iccid)
{
	let id = norm_iccid(iccid);

	for (let sec in (sims ?? [])) {
		if (!id || norm_iccid(sec.iccid) != id)
			continue;

		return {
			name: sec['.name'], apn: sec.apn ?? null, pdp_type: sec.pdp_type ?? null,
			auth: sec.auth ?? null, username: sec.username ?? null,
			password: (sec.password != null) ? true : null,
			pin: (sec.pincode != null && sec.pincode != ''),
		};
	}

	return null;
}

function wwand_sims()
{
	let out = [];
	let c = libuci.cursor();

	c.load('network');
	c.foreach('network', 'wwand_sim', (sec) => { push(out, sec); });

	return out;
}

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
	let id = norm_iccid(iccid);

	for (let sec in (sims ?? []))
		if (id && norm_iccid(sec.iccid) == id)
			return pick(sec, 'sim');

	for (let sec in (ifaces ?? []))
		if (sec.modem == modem && pick(sec, 'interface'))
			return pick(sec, 'interface');

	return null;
}

// The cards of this router's modems, one row each, as `rsim-card --list`
// writes them (rsim-card appends these to its own list when it finds
// wwand-rsim here): the ones another router can borrow, and why not the
// others. `call(method, args)` -> answer or null.
// what wwand knows about one of its modems, as the fields of a list row or
// an info event: who it is, which network it is on
function modem_meta(m)
{
	let reg = m?.registration;

	return {
		modem_manufacturer: m?.manufacturer ?? null, modem_model: m?.model ?? null,
		modem_revision: m?.revision ?? null, modem_imei: m?.imei ?? null,
		modem_state: m?.state ?? null, rat: m?.rat ?? null,
		operator: reg?.plmn?.description ?? null,
		plmn: (reg?.plmn?.mcc != null)
			? sprintf('%03d', reg.plmn.mcc) + sprintf((reg.plmn.mnc_digits == 3) ? '%03d' : '%02d', reg.plmn.mnc) : null,
	};
}

function lend_rows(call, sims)
{
	let inv = call('sim_inventory', {});
	let mods = call('status', {})?.modems ?? {};
	let rows = [];

	for (let c in (inv?.cards ?? [])) {
		// a card here but in a reader is not a modem's to lend
		if (!c.present || c.reader || !c.modem)
			continue;

		let why = null, sap = null;

		if (!c.active)
			why = sprintf('not the card %s runs on — only that one can be lent', c.modem);
		else {
			let r = call('modem_plugin_status', { modem: c.modem, plugin: 'rsim', op: 'lend_check' });

			why = (r == null) ? 'wwand does not answer'
				: (r.ok === false) ? sprintf('cannot ask its modem (%s)', r.error ?? '?')
				: r.why;
			sap = r?.sap ?? null;
		}

		push(rows, {
			backend: 'wwand', spec: 'wwand:iccid:' + c.iccid, device: c.modem, modem: c.modem,
			slot: c.slot ?? null, iccid: c.iccid, imsi: c.imsi ?? null, eid: c.eid ?? null,
			profile: c.profile?.name ?? null, lendable: !why, why: why,
			// the ways it can be lent: SIM Access when its UIM has it
			modes: (sap === true) ? [ 'sap', 'apdu' ] : (sap === false) ? [ 'apdu' ] : null,
			config: sim_config(sims, c.iccid),
			...modem_meta(mods[c.modem]),
			// its modem's own ports there: a scan from another router marks
			// them (an at: reader on one would take the card from under wwand)
			modem_ports: { at: mods[c.modem]?.at_tty ?? null, gps: mods[c.modem]?.gps_port ?? null,
			               diag: mods[c.modem]?.diag_port ?? null },
		});
	}

	return rows;
}

// the modem a target names: a modem, or iccid:<ICCID> — the modem that RUNS
// on that card now
function lend_target(call, target)
{
	if (substr(target ?? '', 0, 6) == 'iccid:') {
		let id = norm_iccid(substr(target, 6));

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
		for (let r in lend_rows(call, sys?.sims ?? wwand_sims()))
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
		                      mode: o.mode, iccid: m?.iccid ?? null, imsi: m?.imsi ?? null, ...modem_meta(m),
		                      sim: lend_settings(sys?.sims ?? wwand_sims(), sys?.ifaces ?? wwand_ifaces(),
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
	qnvfr_value: qnvfr_value,
	proxy: proxy,
	test_source: test_source,
	proxy_session: proxy_session,
	modem_meta: modem_meta,
	lend_rows: lend_rows,
	sim_config: sim_config,
	lend_settings: lend_settings,
	use_reader: use_reader,
	scan: scan,
	scan_parse: scan_parse,
	ssh_key: ssh_key,
	ssh_hosts: ssh_hosts,
	authorized_line: authorized_line,
	ssh_diagnose: ssh_diagnose,
	status_lines: status_lines,
	reader_text: reader_text,
	EFS_ENABLE: EFS_ENABLE,
	EFS_SAP: EFS_SAP,

	help: [
		'rsim [modem] [status]                 remote SIM: reader, card, state',
		'rsim [modem] switch                   the modem firmware switch for UIM Remote',
		'rsim [modem] enable|disable [--reset] set it (a modem reset applies it)',
		'rsim [modem] restart                  give the modem its own SIM back, then offer the remote one again',
		'rsim ssh-key [user@host]              the router\'s key, and the restricted authorized_keys line for each machine',
		'rsim readers                          the SIM readers defined (config wwand_simreader) and who uses them',
		'rsim scan [user@host] [--json]        the readers and ports a machine offers (here, or a SIM host over SSH)',
		'rsim test <spec|/dev/tty…> [user@host] [--json]  open ONE source, power its card up, report, hand it back',
		'rsim [modem] use <reader|off> [--wait S] [--json]  run the modem on that reader\'s card (or its own again); --wait until it does',
		'rsim [modem] probe                    what this modem\'s UIM offers for lending its card (read-only)',
		'rsim [modem] donor-test [sap|apdu] [qmi|at]  lend its card once: ATR + SELECT MF, then hand it back',
		'rsim [modem] sap-switch               the firmware switch for lending the card over SIM Access',
		'rsim [modem] sap-enable [--reset]     allow it (Quectel; writes the EFS item, a modem reset applies it)',
		'rsim proxy <modem|iccid:ICCID> [--mode sap|apdu|auto]  lend a card here to another router (run by its wwand-rsim over SSH)',
		'rsim proxy --list                     the cards here another router can borrow, with their wwand_sim settings',
		'rsim [modem] take-back                end lending its card to another router, and lend it no more',
		'rsim [modem] lend-allow               ...until this',
	],

	run: function(ctx, args) {
		// not about a modem: must not require one
		if (args[0] == 'ssh-key')
			return ssh_key(slice(args, 1));

		if (args[0] == 'readers')
			return list_readers(ctx);

		if (args[0] == 'scan')
			exit(scan(ctx, slice(args, 1)));

		if (args[0] == 'proxy')
			exit(proxy(ctx, slice(args, 1)));

		if (args[0] == 'test')
			exit(test_source(ctx, slice(args, 1)));

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

		case 'use':
			exit(use_reader(ctx, r.modem, slice(rest, 1)));

		case 'enable':
		case 'disable':
			set_switch(ctx, r.modem, op == 'enable', rest[1] == '--reset');
			break;

		case 'probe': {
			let r2 = ctx.call_ok('modem_plugin', { modem: r.modem, plugin: 'rsim', op: 'probe' });

			printf('%-13s%s\n', 'UIM messages', r2?.msgs ? join(' ', map(r2.msgs, (m) => sprintf('0x%02X', m)))
			                                              : sprintf('not listed by this firmware (%J)', r2?.msgs_error));
			printf('%-13s%s\n', 'UIM Remote', (r2?.uim_remote == 'available') ? 'offered by the modem (a remote card can be used)'
				: (r2?.uim_remote == 'service_unavailable') ? 'not offered — switch it on: wwandctl rsim MODEM enable --reset'
				: sprintf('not usable here (%s)', r2?.uim_remote ?? '?'));
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

		case 'take-back': {
			let res = ctx.call_ok('modem_plugin', { modem: r.modem, plugin: 'rsim', op: 'lend_end' });

			printf('modem %s: %s; not lent to another router again until `wwandctl rsim %s lend-allow`\n', r.modem,
				res?.ended ? 'card taken back' : 'its card was not lent', r.modem);
			break;
		}

		case 'lend-allow':
			ctx.call_ok('modem_plugin', { modem: r.modem, plugin: 'rsim', op: 'lend_allow' });
			printf('modem %s: its card may be lent to another router again\n', r.modem);
			break;

		case 'restart':
			ctx.call_ok('modem_plugin', { modem: r.modem, plugin: 'rsim', op: 'restart' });
			printf('modem %s: remote SIM restarted — `wwandctl rsim` shows how it goes\n', r.modem);
			break;

		default:
			die('usage: wwandctl rsim [modem] [status|switch|enable [--reset]|disable [--reset]|restart|probe|donor-test|sap-switch|sap-enable [--reset]|take-back|lend-allow]');
		}
	},
};
