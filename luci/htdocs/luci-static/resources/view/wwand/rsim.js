'use strict';
'require view';
'require form';
'require rpc';
'require ui';
'require fs';
'require uci';
'require dom';
'require poll';

/* Remote SIM (wwand-rsim). Two lists, both in /etc/config/network:

   - SIM readers (`config wwand_simreader`): where a card can come from — a
     reader on this router, a reader on another machine over SSH, or another
     wwand modem lending its card (the "SIM sponsor"). Defined once.
   - Modems (`config wwand_modem`, `option rsim '<reader>'`): which reader a
     modem uses instead of its own SIM. A reader serves one modem at a time.

   A modem may also carry a reader spelled out directly (`option rsim_reader`,
   e.g. set with uci); the page shows it and leaves it alone. Status and
   restart go through the plugin (modem_plugin_status / modem_plugin). */

var callStatus = rpc.declare({ object: 'wwand', method: 'status', expect: { modems: {} } });
var callRsimStatus = rpc.declare({ object: 'wwand', method: 'modem_plugin_status',
	params: [ 'modem', 'plugin', 'op' ], expect: {} });
var callRsim = rpc.declare({ object: 'wwand', method: 'modem_plugin',
	params: [ 'modem', 'plugin', 'op' ], expect: {} });

var callRsimOp = function(modem, op) {
	return rpc.declare({ object: 'wwand', method: 'modem_plugin', params: [ 'modem', 'plugin', 'op' ], expect: {} })(modem, 'rsim', op);
};

var STATE_TEXT = {
	idle: _('waiting to start'), starting: _('starting'), waiting: _('offered, waiting for the modem'),
	connected: _('modem connected, card not powered'), powered: _('in use by the modem'), failed: _('stopped'),
};
var STATE_LEVEL = { powered: 'ok', failed: 'error' };

var KIND = {
	pcsc: _('PC/SC reader'), wbsm: _('Smartmouse USB'), tty: _('serial port'), bt: _('phone (Bluetooth)'),
	wwand: _('modem card'),
};

function level(l, text) {
	var c = { ok: '#2a8a2a', warn: '#b07800', error: '#c0392b' }[l] || 'inherit';

	return E('div', { 'style': 'color:%s'.format(c) }, [ text ]);
}

function button(label, title, fn, style) {
	return E('button', { 'class': 'btn cbi-button cbi-button-%s'.format(style || 'action'), 'title': title,
		'style': 'margin:1px', 'click': ui.createHandlerFn(this, fn) }, label);
}

function ago(now, t) {
	var d = Math.max(0, (now || Math.floor(Date.now() / 1000)) - t);

	return d < 120 ? _('%d s').format(d) : d < 7200 ? _('%d min').format(Math.floor(d / 60))
		: _('%d h').format(Math.floor(d / 3600));
}

/* the reader spec a wwand_simreader asks a machine for (the plugin's
   reader_options), and the machines: { 'user@host': [ spec… ] } */
function readerSpec(r) {
	var t = r.type || 'wbsm';

	switch (t) {
	case 'wwand': return 'wwand:' + (r.device || '');
	case 'pcsc': return 'pcsc:' + (r.device || '0');
	case 'wbsm': return 'wbsm:' + (r.device || '');
	case 'phoenix': case 'at': case 'bt': return t + ':' + (r.device || '');
	}

	return null;
}

/* the rsim-card path a machine was given (option helper / rsim_ssh_helper) */
function sshHelper(h) {
	var r = uci.sections('network', 'wwand_simreader').filter(function(x) { return x.host == h && x.helper; })[0];
	var m = uci.sections('network', 'wwand_modem').filter(function(x) {
		return (x.rsim_reader || '').indexOf('ssh:' + h + ':') == 0 && x.rsim_ssh_helper; })[0];

	return r ? r.helper : m ? m.rsim_ssh_helper : null;
}

function sshHosts() {
	var out = {};
	var add = function(h, spec) {
		if (!h || !spec)
			return;
		out[h] = out[h] || [];
		if (out[h].indexOf(spec) < 0)
			out[h].push(spec);
	};

	uci.sections('network', 'wwand_simreader').forEach(function(r) {
		if (r.host)
			add(r.host, readerSpec(r));
	});
	uci.sections('network', 'wwand_modem').forEach(function(m) {
		var x = /^ssh:([A-Za-z0-9._-]+@[A-Za-z0-9._-]+):(.+)$/.exec(m.rsim_reader || '');

		if (x)
			add(x[1], x[2]);
	});

	return out;
}

/* wwandctl rsim ssh-key's line: the key, restricted to rsim-card --serve */
function authorizedLine(key, specs, helper) {
	/* literal there: --serve reads each as an fnmatch pattern, and PC/SC
	   names carry "[CCID Interface]" */
	/* rsim-card outside that user's PATH: the reader's helper path */
	var bin = helper ? "'" + helper.replace(/'/g, '') + "'" : 'rsim-card';
	var cmd = [ bin, '--serve' ].concat(specs.map(function(x) {
		return "'" + x.replace(/([*?\[\]\\])/g, '\\$1').replace(/['"]/g, '?') + "'";
	})).join(' ');

	return 'command="%s",no-pty,no-port-forwarding,no-agent-forwarding,no-X11-forwarding %s'.format(cmd.replace(/"/g, ''), key.trim());
}

/* what a reader is, in one line (wwandctl's reader_text): from the info
   event of the one in use, or a scan row */
function readerText(i) {
	var parts = [];
	var add = function(v) { if (v != null && v !== '') parts.push(v); };
	var both = function(a, b) { return [ a, b ].filter(function(x) { return x; }).join(' '); };

	add(both(i.modem_manufacturer, i.modem_model));
	add(i.modem_revision ? 'fw ' + i.modem_revision : null);
	add(i.host ? _('on %s').format(i.host) : null);
	add(i.name ? '"%s"%s'.format(i.alias || i.name, i.kind ? ' (%s)'.format(i.kind) : '') : null);
	add(i.vendor);
	add(i.key ? _('%s pairing').format(i.key) : null);
	add(i.channel ? _('channel %d').format(i.channel) : null);
	add(both(i.usb_manufacturer, i.usb_product));
	add(i.usb_serial ? _('serial %s').format(i.usb_serial) : null);
	add(i.usb_path ? 'USB ' + i.usb_path : null);
	add(i.reader_name);
	add(i.ifd_type);
	add(i.protocol);
	add(i.clock_khz ? _('%d kHz, %s reset').format(i.clock_khz, i.reset_line || '?') : null);
	add(i.operator ? i.operator + (i.rat ? ' ' + i.rat : '') : null);
	add(i.iccid ? 'ICCID ' + i.iccid : null);

	return parts.join(' · ');
}

/* small grey lines of key: value, for the details a row has */
function metaLines(pairs) {
	return pairs.filter(function(p) { return p[1] != null && p[1] !== ''; }).map(function(p) {
		return E('div', { 'style': 'opacity:.75;font-size:92%;word-break:break-all' }, [ '%s: %s'.format(p[0], p[1]) ]);
	});
}

function usbLines(x) {
	return metaLines([
		[ _('USB device'), [ x.usb_manufacturer, x.usb_product ].filter(function(v) { return v; }).join(' ') ],
		[ _('USB serial'), x.usb_serial ], [ _('USB path'), x.usb_path ],
		[ _('Speed'), x.usb_speed_mbps ? x.usb_speed_mbps + ' Mbit/s' : null ],
		[ _('Interface'), x.interface_name ? '%s (%s)'.format(x.interface_name, x.interface) : null ],
		[ _('Stable name'), x.by_id ], [ _('By path'), x.by_path ],
	]);
}

/* what a scan row is, for people */
function describe(x) {
	var out = [];

	switch (x.backend) {
	case 'pcsc':
		out.push(E('div', {}, [ x.name || '' ]), E('div', {}, x.card ? _('card inserted') : _('no card')));
		if (x.in_use)
			out.push(level('warn', _('held by another program (pcscd)')));
		if (x.mute)
			out.push(level('warn', _('the card does not answer')));
		out = out.concat(metaLines([ [ _('Index'), x.index != null ? String(x.index) : null ], [ 'ATR', x.atr ] ]));
		break;
	case 'wbsm':
		out.push(E('div', {}, [ _('serial %s').format(x.serial || '?') ]));
		if (x.access === false)
			out.push(level('warn', _('no permission to open it (USB device access)')));
		out = out.concat(usbLines(x), metaLines([ [ _('Kernel driver'), x.kernel_driver ? _('%s (detached while in use)').format(x.kernel_driver) : null ] ]));
		break;
	case 'tty':
		out.push(E('div', {}, [ '%s %s%s'.format(x.driver || '?', x.usb || '', x.interface ? ' if%s'.format(x.interface) : '') ]),
			E('div', {}, (x.hint == 'at') ? _('a modem\'s port (AT)') : (x.hint == 'phoenix') ? _('a USB-serial adapter (Phoenix)')
				: (x.hint == 'diag') ? _('a diagnostic port — no AT, not a source') : _('unknown')));
		out = out.concat(usbLines(x));
		break;
	case 'bt':
		out.push(E('div', {}, [ E('strong', {}, [ x.alias || x.name || '?' ]), x.alias && x.name && x.alias != x.name ? ' (%s)'.format(x.name) : '' ]),
			(x.sap === true) ? level('ok', x.sap_channel ? _('offers SIM Access (channel %d)').format(x.sap_channel) : _('offers SIM Access'))
			: (x.sap === false) ? level('warn', _('SIM Access not offered (not supported, or off on the phone)'))
			: E('div', {}, _('SIM Access unknown (services not read yet)')));
		if (x.connected != null)
			out.push(E('div', {}, x.connected ? _('connected now') : _('not connected')));
		if (x.blocked)
			out.push(level('error', _('blocked in BlueZ')));
		if (x.key == 'unauthenticated')
			out.push(level('warn', _('paired without confirmation — security "authenticated" will refuse it')));
		out = out.concat(metaLines([
			[ _('Kind'), x.kind ], [ _('Vendor'), x.vendor ? '%s (%s)'.format(x.vendor, x.vendor_id) : x.vendor_id ],
			[ _('Pairing'), x.key ], [ _('Trusted'), x.trusted != null ? (x.trusted ? _('yes') : _('no')) : null ],
			[ _('Profiles'), x.services ], [ _('Adapter'), x.adapter ? x.adapter + (x.adapter_name ? ' (%s)'.format(x.adapter_name) : '') +
				(x.adapter_powered === false ? ' — ' + _('off') : '') : null ],
		]));
		break;
	case 'wwand':
		out.push(E('div', {}, [ E('strong', {}, [ x.iccid || '?' ]), ' · ', _('modem %s').format(x.modem || '?'),
			x.slot ? ' · ' + _('slot %d').format(x.slot) : '' ]));
		if (x.modem_model || x.modem_manufacturer)
			out.push(E('div', {}, [ [ x.modem_manufacturer, x.modem_model ].filter(function(v) { return v; }).join(' '),
				x.modem_revision ? ' · fw ' + x.modem_revision : '' ]));
		if (x.operator)
			out.push(E('div', {}, [ x.operator, x.rat ? ' · ' + x.rat : '', x.modem_state ? ' · ' + x.modem_state : '' ]));
		if (x.modes)
			out.push(E('div', {}, (x.modes.indexOf('sap') >= 0) ? _('lends over SIM Access or APDU') : _('lends over APDU only (no SIM Access in its UIM)')));
		if (x.imsi)
			out.push(E('div', {}, [ 'IMSI ' + x.imsi ]));
		if (x.eid)
			out.push(E('div', { 'style': 'opacity:.75;font-size:92%' }, [ 'eUICC ' + x.eid ]));
		if (x.profile)
			out.push(E('div', {}, [ _('eSIM profile %s').format(x.profile) ]));
		if (x.config)
			out.push(E('div', { 'style': 'opacity:.75;font-size:92%' }, [
				_('settings %s').format(x.config.name || '?'),
				x.config.apn ? ' · APN ' + x.config.apn : '',
				x.config.pdp_type ? ' · ' + x.config.pdp_type : '',
				x.config.username ? ' · ' + _('user %s').format(x.config.username) : '',
				x.config.pin ? ' · ' + _('PIN set there (enter it here too)') : '',
			]));
		else
			out.push(E('div', { 'style': 'opacity:.75;font-size:92%' }, _('no wwand_sim settings there')));
		break;
	}

	return out;
}

return view.extend({
	load: function() {
		return Promise.all([
			L.resolveDefault(callStatus(), {}),
			L.resolveDefault(fs.read('/etc/wwand/rsim/id_dropbear.pub'), null),
			L.resolveDefault(fs.read('/etc/wwand/rsim/id_ed25519.pub'), null),
			uci.load('network'),
		]).then(function(r) {
			var names = Object.keys(r[0] || {}).sort();

			return Promise.all(names.map(function(n) {
				return L.resolveDefault(callRsimStatus(n, 'rsim', 'status'), {});
			})).then(function(sts) {
				return { modems: names, st: sts, key: r[1] || r[2], info: r[0] || {} };
			});
		});
	},

	render: function(data) {
		var m, s, o;
		var modems = uci.sections('network', 'wwand_modem').map(function(x) { return x['.name']; });

		m = new form.Map('network', _('Remote SIM'),
			_('Use a SIM card that is not in the modem itself. Define where cards can come from under <em>SIM readers</em>, then pick one per modem under <em>Modems</em>. The modem firmware has to allow a remote SIM — on Quectel modules once per modem with <code>wwandctl rsim MODEM enable --reset</code>. Choosing <em>its own SIM</em> again gives the modem its own card back at once.'));

		/* ---- SIM readers -------------------------------------------------- */
		s = m.section(form.TypedSection, 'wwand_simreader', _('SIM readers'),
			_('Each entry is one place a SIM card can come from. A reader serves one modem at a time; a second modem that names it waits until it is free.'));
		s.anonymous = false;
		s.addremove = true;
		s.addbtntitle = _('Add SIM reader');

		o = s.option(form.ListValue, 'type', _('Kind'));
		o.value('wbsm', _('WB Electronics Smartmouse USB'));
		o.value('phoenix', _('Phoenix/Smartmouse serial reader'));
		o.value('pcsc', _('PC/SC (CCID) reader'));
		o.value('at', _('the SIM of a modem with an AT port (not wwand\'s)'));
		o.value('bt', _('a paired phone over Bluetooth (SIM Access Profile)'));
		o.value('modem', _('another modem lends its card (SIM sponsor)'));
		o.value('wwand', _('a modem of another wwand router lends its card'));
		o.default = 'wbsm';
		/* written even when left at the default: the section must say what
		   it is (the plugin assumes the same default, but uci should not
		   depend on that) */
		o.rmempty = false;
		o.description = _('Smartmouse USB: clock and mode are set by software, no driver needed. Phoenix: a serial reader whose clock is set with switches. PC/SC: any CCID reader through pcscd. AT modem: the card in a modem that wwand does not manage — on another machine, or here — reached over its AT port (AT+CSIM). Phone: the SIM of a phone paired over Bluetooth that offers the SIM Access Profile (rSAP) — while it is lent the phone has no network of its own. SIM sponsor: another wwand modem on this router lends the card in it. Another wwand router: one of its modems lends its card the same way, over SSH — that router needs wwand-rsim.');

		o = s.option(form.Value, 'device', _('Reader'),
			_('Phoenix: its serial port, e.g. <code>/dev/ttyUSB0</code>. PC/SC: the reader\'s name or index (empty: the first). Smartmouse USB: its USB serial number (empty: the first one). AT modem: its AT port, e.g. <code>/dev/ttyUSB2</code>. Phone: its Bluetooth address, e.g. <code>AA:BB:CC:DD:EE:FF</code> (<code>wwandctl rsim scan</code> lists the paired phones). Another wwand router: its modem\'s name there, or <code>iccid:</code> and the card\'s ICCID — <code>wwandctl rsim scan user@host</code> lists them.'));
		o.depends('type', 'wbsm');
		o.depends('type', 'phoenix');
		o.depends('type', 'pcsc');
		o.depends('type', 'at');
		o.depends('type', 'bt');
		o.depends('type', 'wwand');
		o.optional = true;
		o.validate = function(sid, v) {
			var t = this.section.formvalue(sid, 'type');

			if (t == 'phoenix' && !v)
				return _('A Phoenix reader needs its serial port');
			if (t == 'at' && !v)
				return _('An AT modem needs its AT port');
			if (t == 'wwand' && !/^([A-Za-z0-9_]+|iccid:[0-9A-Fa-f]{18,20})$/.test(v || ''))
				return _('The modem on that router, or iccid:<ICCID>');
			if (t == 'bt' && !/^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$/.test(v || ''))
				return _('A phone needs its Bluetooth address (AA:BB:CC:DD:EE:FF)');
			return true;
		};

		o = s.option(form.ListValue, 'radio', _('That modem\'s radio'),
			_('<strong>Off</strong> while its card is used elsewhere (<code>AT+CFUN=4</code>; the SIM stays reachable), back to what it was afterwards — two modems must never register with the same card. <strong>Keep</strong> only when that modem is off the network anyway.'));
		o.depends('type', 'at');
		o.value('', _('off while lent (default)'));
		o.value('keep', _('keep as it is'));
		o.optional = true;

		o = s.option(form.ListValue, 'security', _('Link security'),
			_('<strong>Encrypted</strong> works with any pairing. <strong>Authenticated</strong> also requires a pairing that was confirmed on both sides (a PIN or a compared number), as the SIM Access Profile recommends.'));
		o.depends('type', 'bt');
		o.value('', _('encrypted (default)'));
		o.value('high', _('authenticated'));
		o.optional = true;

		o = s.option(form.Value, 'channel', _('RFCOMM channel'),
			_('Empty: asked from the phone (SDP). Only for a phone whose service record is wrong.'));
		o.depends('type', 'bt');
		o.datatype = 'range(1,30)';
		o.optional = true;

		o = s.option(form.Value, 'host', _('On another machine'),
			_('<code>user@host</code> when the reader is attached to another machine: the router runs <code>rsim-card</code> there over SSH. That machine needs rsim-card and access to the reader, and the router\'s SSH key (shown below) in the user\'s <code>~/.ssh/authorized_keys</code>. Leave empty for a reader on this router.<br />Another wwand router: required — the router runs <code>wwandctl rsim proxy</code> there, which needs <strong>wwand-rsim</strong> installed on it, and the key in its <code>/etc/dropbear/authorized_keys</code> (root).'));
		o.depends('type', 'wbsm');
		o.depends('type', 'phoenix');
		o.depends('type', 'pcsc');
		o.depends('type', 'at');
		o.depends('type', 'bt');
		o.depends('type', 'wwand');
		o.optional = true;
		o.validate = function(sid, v) {
			if (!v && this.section.formvalue(sid, 'type') == 'wwand')
				return _('Another wwand router needs user@host');
			return (!v || /^[A-Za-z0-9._-]+@[A-Za-z0-9._-]+$/.test(v)) ? true : _('Expecting user@host');
		};

		o = s.option(form.Value, 'helper', _('rsim-card on that machine'),
			_('Its path when it is not in the remote user\'s PATH.'));
		/* regex values: form.js isEqual() takes a RegExp */
		o.depends({ 'type': 'wbsm', 'host': /./ });
		o.depends({ 'type': 'phoenix', 'host': /./ });
		o.depends({ 'type': 'pcsc', 'host': /./ });
		o.depends({ 'type': 'at', 'host': /./ });
		o.depends({ 'type': 'bt', 'host': /./ });
		o.optional = true;
		o.placeholder = 'rsim-card';

		o = s.option(form.ListValue, 'clock', _('Card clock'),
			_('Phoenix: must match the reader\'s clock switch. Smartmouse USB: the reader is set to it. 3.58 MHz is right for SIM cards.'));
		o.depends('type', 'wbsm');
		o.depends('type', 'phoenix');
		o.value('', _('3.58 MHz (default)'));
		o.value('3680', '3.68 MHz');
		o.value('6000', '6.00 MHz');
		o.optional = true;

		o = s.option(form.ListValue, 'mode', _('Reader mode'),
			_('How the reader is wired to the card. Phoenix is right for the Smartmouse USB unless you know otherwise.'));
		o.depends('type', 'wbsm');
		o.value('', 'Phoenix');
		o.value('smartmouse', 'Smartmouse');
		o.optional = true;

		o = s.option(form.ListValue, 'donor', _('SIM sponsor'),
			_('The modem whose card is lent. It cannot use that card itself meanwhile, and it must not be the modem that uses it.'));
		o.depends('type', 'modem');
		modems.forEach(function(n) { o.value(n, n); });

		o = s.option(form.ListValue, 'donor_mode', _('How it lends'),
			_('<strong>SIM Access</strong>: the sponsor hands its card over and stops using it — its own connection goes down, and it gets the card back when the lending ends. On Quectel modules wwand-rsim switches it on by itself in the modem\'s init (one modem reset; <code>option rsim_sap_auto \'0\'</code> on the modem leaves it off).<br /><strong>APDU</strong>: the card stays in the sponsor and every command is passed through it. For modems that cannot do SIM Access.<br /><strong>Automatic</strong> (default): SIM Access where the sponsor has it; APDU where it has not — over <code>AT+CSIM</code> on a sponsor without QMI UIM (an NCM modem), and for one whose SIM Access is known to hang (Huawei E392) or did not answer once.<br />Either way the sponsor\'s <strong>radio stays off</strong> for as long as it is configured as a sponsor: two modems must never register with the same card. Its interfaces do not come up meanwhile — an attempt is refused with the reason (<em>Radio off: the SIM card is lent to another modem</em>). Remove the sponsor here and its radio comes back.<br />A modem of <strong>another wwand router</strong> is held only while its card is actually lent: when this router lets go (or loses the SSH link), that router gets its card and its radio back.'));
		o.depends('type', 'modem');
		o.depends('type', 'wwand');
		o.value('', _('automatic (default)'));
		o.value('sap', _('SIM Access'));
		o.value('apdu', _('APDU'));
		o.optional = true;

		o = s.option(form.ListValue, 'donor_slot', _('Sponsor\'s slot'),
			_('Which of the sponsor\'s SIM slots holds the card to lend. Only the slot the sponsor <strong>runs on</strong> can be lent: most modems use one slot at a time and switch the other one off, so a modem cannot use one card and lend the other (checked on a Quectel RG502Q and RG650E). Switch the sponsor to the slot first if needed.'));
		o.depends('type', 'modem');
		o.depends('type', 'wwand');
		o.value('', _('the one it runs on (default)'));
		o.value('1', _('slot 1'));
		o.value('2', _('slot 2'));
		o.optional = true;

		o = s.option(form.ListValue, 'donor_apdu', _('APDU channel'),
			_('Automatic tries QMI first and falls back to <code>AT+CSIM</code> (which also works over AT inside MBIM).'));
		o.depends({ 'type': 'modem', 'donor_mode': 'apdu' });
		o.depends({ 'type': 'wwand', 'donor_mode': 'apdu' });
		o.value('', _('automatic'));
		o.value('qmi', 'QMI UIM');
		o.value('at', 'AT+CSIM');
		o.optional = true;

		/* ---- Modems ------------------------------------------------------- */
		s = m.section(form.TypedSection, 'wwand_modem', _('Modems'),
			_('Which SIM each modem uses. When the SIM changes, the modem re-reads it: its identity, PIN and the per-SIM settings (APN) of the new card apply, and the same happens when it gets its own card back.'));
		s.anonymous = false;
		s.addremove = false;

		/* The readers are read again on every render, not once: a reader
		   added above has to be selectable here right away (the map
		   re-renders after "Add", the view's render() does not run again). */
		o = s.option(form.ListValue, 'rsim', _('SIM'));
		o.load = function(sid) {
			this.keylist = [];
			this.vallist = [];
			this.value('', _('its own SIM'));
			uci.sections('network', 'wwand_simreader').forEach(L.bind(function(r) {
				var t = r.type || 'wbsm';

				this.value(r['.name'], '%s (%s)'.format(r['.name'], t == 'modem'
					? _('card of %s').format(r.donor || '?')
					: t == 'wwand' ? _('card of %s on %s').format(r.device || '?', r.host || '?')
					: (r.host ? _('%s on %s').format(t, r.host) : t)));
			}, this));
			return form.ListValue.prototype.load.apply(this, [ sid ]);
		};
		o.optional = true;
		o.validate = function(sid, v) {
			var r = v ? uci.get('network', v) : null;

			return (r && r['.type'] == 'wwand_simreader' && r.type == 'modem' && r.donor == sid)
				? _('A modem cannot use the card it is lending out') : true;
		};

		/* a reader spelled out with uci: shown, left alone — and only where
		   one exists, so it does not clutter every modem */
		if (uci.sections('network', 'wwand_modem').some(function(x) { return x.rsim_reader; })) {
			o = s.option(form.DummyValue, 'rsim_reader', _('Set directly'),
				_('A reader spelled out with <code>option rsim_reader</code> (e.g. by uci). It applies while no SIM reader is selected above; remove it with <code>uci delete network.MODEM.rsim_reader</code>.'));
			o.cfgvalue = function(sid) { return uci.get('network', sid, 'rsim_reader') || _('—'); };
		}

		/* only once a reader is chosen: with its own SIM there is nothing to
		   place. Rebuilt per render like the list above; with no readers the
		   dependency can never hold (an empty one would always show). */
		o = s.option(form.ListValue, 'rsim_slot', _('Modem slot'),
			_('The slot of this modem the remote card appears in. 1 is right for single-slot modems.'));
		o.value('', '1');
		o.value('2', '2');
		o.value('3', '3');
		o.optional = true;
		o.load = function(sid) {
			var names = uci.sections('network', 'wwand_simreader').map(function(r) { return r['.name']; });

			this.deps = names.length ? names.map(function(n) { return { 'rsim': n }; }) : [ { 'rsim': '\u0000' } ];
			return form.ListValue.prototype.load.apply(this, [ sid ]);
		};

		var view = this;

		return m.render().then(function(node) {
			view.map = m;
			view.statusBox = E('div', {}, E('em', {}, _('Loading…')));
			view.scanBox = E('div');
			view.keyBox = E('div');
			view.key = data.key;

			view.renderStatus(data);
			view.renderKey();
			poll.add(L.bind(view.refresh, view), 5);

			var hostInput = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'placeholder': _('user@host — empty: this router'),
				'style': 'width:18em' });

			return E([], [
				E('div', { 'class': 'cbi-section' }, [
					E('h3', {}, _('Status')),
					E('div', { 'class': 'cbi-section-descr' }, _('What each modem uses right now, and whom it lends its card to. Updated every 5 seconds; changes above take effect when saved and applied.')),
					view.statusBox,
				]),
				E('div', { 'class': 'cbi-section' }, [
					E('h3', {}, _('Find SIM sources')),
					E('div', { 'class': 'cbi-section-descr' }, _('What this router — or another machine over SSH — offers as a card source: PC/SC and Smartmouse readers, serial ports, paired phones with the SIM Access Profile, and on a router with wwand-rsim the cards of its modems, with the settings it has for them. Nothing is sent to a card or a port; a phone is not called up. <em>Add</em> creates a SIM reader above (save and apply to use it).')),
					E('div', { 'class': 'cbi-value' }, [
						E('label', { 'class': 'cbi-value-title' }, _('Where')),
						E('div', { 'class': 'cbi-value-field' }, [
							hostInput, ' ',
							E('button', { 'class': 'btn cbi-button cbi-button-action', 'click': ui.createHandlerFn(view, function() {
								return view.scan(hostInput.value.trim());
							}) }, _('Scan')),
						]),
					]),
					view.scanBox,
				]),
				E('div', { 'class': 'cbi-section' }, [
					E('h3', {}, _('SSH setup')),
					view.keyBox,
				]),
				node,
			]);
		});
	},

	/* ---- status -------------------------------------------------------------- */

	refresh: function() {
		return L.resolveDefault(callStatus(), {}).then(L.bind(function(mods) {
			var names = Object.keys(mods || {}).sort();

			return Promise.all(names.map(function(n) {
				return L.resolveDefault(callRsimStatus(n, 'rsim', 'status'), {});
			})).then(L.bind(function(sts) {
				this.renderStatus({ modems: names, st: sts, info: mods });
			}, this));
		}, this));
	},

	renderStatus: function(data) {
		var view = this;
		var info = data.info || {};
		var rows = [];

		if (!data.modems.length) {
			dom.content(this.statusBox, E('em', {}, _('No modem found — is wwand running?')));
			return;
		}

		data.modems.forEach(function(n, i) {
			var st = data.st[i] || {};
			var mi = info[n] || {};
			var actions = [];

			var card = [ E('div', {}, [ '%s'.format(mi.model || mi.manufacturer || '') ]),
				E('div', {}, [ mi.state || '?' ]) ];

			if (mi.iccid)
				card.push(E('div', { 'style': 'font-family:monospace' }, [ mi.iccid ]));

			/* the remote SIM this modem uses */
			var remote;

			if (st.config_error)
				remote = [ level('error', st.config_error) ];
			else if (!st.enabled)
				remote = [ E('span', {}, _('its own SIM')) ];
			else {
				var name = st.reader_name ? '%s (%s)'.format(st.reader_name, st.reader || '?') : (st.reader || '?');

				remote = [ E('div', {}, [ E('strong', {}, [ name ]), ' · ', _('slot %d').format(st.slot || 1) ]) ];

				/* what that reader is: its info event after the open */
				if (st.reader_info && readerText(st.reader_info))
					remote.push(E('div', { 'style': 'opacity:.75;font-size:92%' }, [ readerText(st.reader_info) ]));

				if (st.conflict)
					remote.push(level('error', _('not used: %s').format(st.conflict)));
				else
					remote.push(level(STATE_LEVEL[st.state] || 'warn', STATE_TEXT[st.state] || st.state));

				if (st.state == 'powered' && st.since)
					remote.push(E('div', { 'style': 'opacity:.75;font-size:92%' }, _('since %s').format(ago(st.now, st.since))));
				if (st.atr)
					remote.push(E('div', { 'style': 'font-family:monospace;word-break:break-all' }, [ 'ATR ' + st.atr ]));
				if (st.apdus)
					remote.push(E('div', {}, [ _('%d commands · last SW %s').format(st.apdus, st.last_sw || '?') ]));
				if (st.last_error)
					remote.push(level('error', (st.retry_at && st.now && st.retry_at > st.now)
						? _('%s (retry in %d s)').format(st.last_error, st.retry_at - st.now) : st.last_error));

				actions.push(button(_('Restart'), _('Give the modem its own SIM back for a moment, then offer the remote one again'),
					function() { return callRsim(n, 'rsim', 'restart').then(L.bind(view.refresh, view)); }));
			}

			/* its own card, lent out */
			var lending = [];
			var lt = st.lent_to;

			if (lt) {
				lending.push(level('ok', lt.remote
					? _('lent to %s (another router)').format(lt.to) : _('lent to %s').format(lt.to)));
				lending.push(E('div', {}, [ (lt.mode == 'apdu') ? _('APDU') : _('SIM Access'), ' · ', _('radio off') ]));
				if (lt.remote && lt.since)
					lending.push(E('div', { 'style': 'opacity:.75;font-size:92%' }, _('since %s').format(ago(st.now, lt.since))));
				if (lt.commands)
					lending.push(E('div', {}, _('%d commands').format(lt.commands)));
				if (lt.remote)
					actions.push(button(_('Take back'), _('End the lending to the other router, and lend this card no more until allowed again'),
						function() { return callRsimOp(n, 'lend_end').then(L.bind(view.refresh, view)); }, 'negative'));
			}
			else if (st.lend_hold) {
				lending.push(level('warn', _('lending stopped here')));
			}
			else if (st.lendable === false && st.lend_why) {
				lending.push(E('span', { 'style': 'opacity:.75;font-size:92%' }, [ _('not lendable: %s').format(st.lend_why) ]));
			}
			else if (st.lendable) {
				lending.push(E('span', { 'style': 'opacity:.75;font-size:92%' }, _('can be lent to another router')));
			}

			if (st.lend_hold)
				actions.push(button(_('Allow lending'), _('Let another router borrow this card again'),
					function() { return callRsimOp(n, 'lend_allow').then(L.bind(view.refresh, view)); }, 'positive'));

			actions.push(button(_('Probe'), _('What this modem\'s UIM service offers for lending its card (read-only)'),
				function() { return view.probe(n); }));

			rows.push(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, [ E('strong', {}, [ n ]) ]),
				E('td', { 'class': 'td' }, card),
				E('td', { 'class': 'td' }, remote),
				E('td', { 'class': 'td' }, lending),
				E('td', { 'class': 'td' }, actions),
			]));
		});

		dom.content(this.statusBox, E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('Modem')),
				E('th', { 'class': 'th' }, _('Card in use')),
				E('th', { 'class': 'th' }, _('Remote SIM')),
				E('th', { 'class': 'th' }, _('Lends its card')),
				E('th', { 'class': 'th' }, _('Actions')),
			]),
		].concat(rows)));
	},

	probe: function(n) {
		ui.showModal(_('Probe %s').format(n), [ E('p', { 'class': 'spinning' }, _('Asking the modem…')) ]);

		return L.resolveDefault(callRsimOp(n, 'probe'), {}).then(function(r) {
			var sap = r && r.sap;
			var lines = [
				[ _('UIM messages'), (r && r.msgs) ? r.msgs.map(function(x) { return '0x%02X'.format(x); }).join(' ')
					: _('not listed by this firmware') ],
				[ _('SIM Access'), !sap ? '?' : !sap.supported ? _('not in this firmware')
					: sap.error ? _('present, status query failed') : _('present, link %s').format(sap.state_name || sap.state) ],
			];

			if (r && r.ok === false)
				lines = [ [ _('Error'), r.error + (r.detail ? ' (%s)'.format(JSON.stringify(r.detail)) : '') ] ];

			ui.showModal(_('Probe %s').format(n), [
				E('table', { 'class': 'table' }, lines.map(function(l) {
					return E('tr', { 'class': 'tr' }, [ E('td', { 'class': 'td' }, [ l[0] ]),
						E('td', { 'class': 'td', 'style': 'word-break:break-all' }, [ l[1] ]) ]);
				})),
				E('div', { 'style': 'opacity:.75;font-size:92%' }, _('Lending needs SIM Access in the firmware (Quectel: switched on by wwand-rsim in the modem\'s init), or the APDU mode. <code>wwandctl rsim MODEM donor-test</code> tries it once.')),
				E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Close'))),
			]);
		});
	},

	/* ---- scan ---------------------------------------------------------------- */

	scan: function(host) {
		var view = this;

		if (host && !/^[A-Za-z0-9._-]+@[A-Za-z0-9._-]+$/.test(host)) {
			dom.content(this.scanBox, level('error', _('Expecting user@host')));
			return Promise.resolve();
		}

		dom.content(this.scanBox, E('p', { 'class': 'spinning' }, [ host ? _('Asking %s…').format(host) : _('Looking…') ]));

		return fs.exec('/usr/bin/wwandctl', host ? [ 'rsim', 'scan', host, '--json' ] : [ 'rsim', 'scan', '--json' ])
			.then(function(res) {
				var r = null;

				try { r = JSON.parse(res.stdout || ''); } catch (e) { r = null; }

				if (r && r.ok)
					return view.renderScan(r, host);

				dom.content(view.scanBox, view.failure(host,
					(r && r.error) || (res.stderr || '').trim() || _('no answer'), r && r.detail, r && r.hint));
			}).catch(function(e) {
				dom.content(view.scanBox, level('error', _('Scan failed: %s').format(e.message)));
			});
	},

	renderScan: function(r, host) {
		var view = this;
		var rows = (r.readers || []).map(function(x) {
			var what = describe(x);
			var blocked = !x.spec ? _('not a card source')
				: x.in_use ? _('in use: %s').format(x.in_use)
				: (x.backend == 'wwand' && x.lendable === false) ? _('not now: %s').format(x.why || '?')
				: null;

			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, [ KIND[x.backend] || x.backend ]),
				E('td', { 'class': 'td', 'style': 'font-family:monospace;word-break:break-all' }, [ x.spec || x.device || '' ]),
				E('td', { 'class': 'td' }, what.concat(blocked ? [ level('warn', blocked) ] : [])),
				E('td', { 'class': 'td' }, [
					/* explicit: only this source is opened — the scan never
					   speaks to a port on its own; a wwand router's card is
					   checked by that router (its lend_check) */
					(x.backend != 'wwand') ? E('button', { 'class': 'btn cbi-button cbi-button-action', 'style': 'margin:1px',
						'disabled': (x.in_use || !x.spec) ? true : null,
						'title': _('Open this source once, power its card up and hand it back — for a phone, it lends its SIM for a moment'),
						'click': ui.createHandlerFn(view, function() { return view.testSource(x, host); }) }, _('Test')) : '',
					E('button', { 'class': 'btn cbi-button cbi-button-add', 'disabled': blocked ? true : null,
						'title': _('Create a SIM reader for this (save and apply to use it)'),
						'click': ui.createHandlerFn(view, function() { return view.addReader(x, host); }) }, _('Add')),
				]),
			]);
		});

		dom.content(this.scanBox, [
			E('div', { 'style': 'opacity:.75;font-size:92%' }, [
				_('rsim-card backends %s: %s').format(host ? _('on %s').format(host) : _('here'), (r.backends || []).join(', ')),
				r.note ? E('div', {}, [ r.note ]) : '',
				r.bt_adapters ? E('div', {}, [ _('Bluetooth adapters: %s').format(r.bt_adapters) ]) : '',
			]),
			rows.length ? E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('Kind')), E('th', { 'class': 'th' }, _('Source')),
					E('th', { 'class': 'th' }, _('Details')), E('th', { 'class': 'th' }, ''),
				]),
			].concat(rows)) : E('em', {}, _('Nothing found.')),
		]);
	},

	/* what went wrong on the way to a machine, and what to do about it */
	failure: function(host, error, detail, hint) {
		var out = [ level('error', error) ];

		if (detail && detail.length)
			out.push(E('pre', { 'style': 'white-space:pre-wrap;font-size:90%' }, [ detail.join('\n') ]));
		if (hint)
			out.push(E('div', { 'class': 'alert-message warning' }, [ E('strong', {}, _('Hint: ')), hint ]));
		if (host && this.key && hint && /authorized_keys|restricted|key/.test(hint))
			out.push(E('div', {}, [
				E('p', {}, [ _('The line for %s (from the readers configured for it):').format(host) ]),
				E('code', { 'style': 'word-break:break-all;display:block' }, [ authorizedLine(this.key, sshHosts()[host] || [], sshHelper(host)) ]),
			]));

		return out;
	},

	/* wwandctl rsim test: one source, opened like a session */
	testSource: function(x, host) {
		/* the title is set as HTML by showModal: a text node for a spec from
		   a remote scan */
		var title = E('span', {}, [ _('Test %s').format(x.spec) ]);

		ui.showModal(title, [ E('p', { 'class': 'spinning' }, _('Opening it…')) ]);

		return fs.exec('/usr/bin/wwandctl', [ 'rsim', 'test', x.spec ].concat(host ? [ host ] : []).concat([ '--json' ]))
			.then(function(res) {
				var r = null;

				try { r = JSON.parse(res.stdout || ''); } catch (e) { r = null; }

				var body = r ? [
					r.ok ? level('ok', _('Works — the card answered (ATR %s)').format(r.atr || '?'))
					     : level('error', _('Does not work: %s').format(r.error || '?')),
					r.info && readerText(r.info) ? E('p', {}, [ readerText(r.info) ]) : '',
					(r.log && r.log.length) ? E('pre', { 'style': 'white-space:pre-wrap;font-size:90%' }, [ r.log.join('\n') ]) : '',
				] : [ level('error', (res.stderr || res.stdout || _('no answer')).trim()) ];

				ui.showModal(E('span', {}, [ _('Test %s').format(x.spec) ]), body.concat([
					E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Close'))),
				]));
			});
	},

	/* a scan row -> a new wwand_simreader, staged in the form */
	addReader: function(x, host) {
		var m = this.map;
		var view = this;
		var i = x.spec.indexOf(':');
		var kind = x.spec.substr(0, i), rest = x.spec.substr(i + 1);
		var o = { 'modem': { type: 'modem', donor: rest } }[kind] || { type: kind, device: rest };

		if (host && kind != 'modem')
			o.host = host;
		if (kind == 'pcsc' && !rest)
			delete o.device;

		var base = (kind == 'bt' ? 'phone' : kind == 'wwand' ? 'remote' : kind) + '_' +
			((x.backend == 'wwand' ? (x.iccid || '').slice(-6) : (x.name || rest || '')).replace(/[^A-Za-z0-9]/g, '').slice(-8) || 'x');
		var name = base, k = 2;

		while (uci.get('network', name))
			name = base + '_' + (k++);

		return m.parse().then(function() {
			uci.add('network', 'wwand_simreader', name);
			Object.keys(o).forEach(function(key) { uci.set('network', name, key, o[key]); });

			/* reset() only renders again: the new section's values have
			   to be loaded first, or its fields show their defaults
			   (HW-found in the browser on 245) */
			return m.load().then(function() { return m.reset(); });
		}).then(L.bind(function() {
			/* a machine new to the list gets its authorized_keys line */
			this.renderKey();
			ui.addNotification(null, E('p', _('SIM reader %s added — pick it for a modem below, then save and apply.').format(name)), 'info');
		}, view));
	},

	/* ---- SSH key ------------------------------------------------------------- */

	renderKey: function() {
		var view = this;
		var hosts = sshHosts();
		var names = Object.keys(hosts).sort();
		var out = [];

		if (!this.key) {
			dom.content(this.keyBox, [
				E('p', {}, _('This router has no SSH key yet. It is needed for every source on another machine: a reader or phone on a PC, or a modem\'s card on another wwand router.')),
				E('button', { 'class': 'btn cbi-button cbi-button-action', 'click': ui.createHandlerFn(view, function() {
					return fs.exec('/usr/bin/wwandctl', [ 'rsim', 'ssh-key' ]).then(function() {
						return Promise.all([
							L.resolveDefault(fs.read('/etc/wwand/rsim/id_dropbear.pub'), null),
							L.resolveDefault(fs.read('/etc/wwand/rsim/id_ed25519.pub'), null),
						]);
					}).then(function(k) {
						view.key = k[0] || k[1];
						view.renderKey();
					});
				}) }, _('Create key')),
			]);
			return;
		}

		out.push(E('div', { 'class': 'cbi-value' }, [
			E('label', { 'class': 'cbi-value-title' }, _('Public key')),
			E('div', { 'class': 'cbi-value-field' }, [ E('code', { 'style': 'word-break:break-all' }, [ this.key.trim() ]) ]),
		]));

		out.push(E('div', { 'class': 'cbi-section-descr' }, _('On each machine below put its line into the <code>authorized_keys</code> of the user named (as root on OpenWrt: <code>/etc/dropbear/authorized_keys</code>; elsewhere <code>~/.ssh/authorized_keys</code>). It lets this router run only what wwand-rsim needs there — <code>rsim-card</code> for the readers listed, its scan, and on a wwand router <code>wwandctl rsim proxy</code> — through <code>rsim-card --serve</code>: no shell, no terminal, no forwarding. <code>rsim-card</code> has to be in that user\'s PATH. After changing the readers of a machine, update its line.')));

		if (!names.length)
			out.push(E('p', {}, [ E('em', {}, _('No source on another machine is configured yet. For a first scan of one, this line allows every reader there:')),
				E('code', { 'style': 'word-break:break-all;display:block' }, [ authorizedLine(this.key, []) ]) ]));

		names.forEach(function(h) {
			var res = E('div');

			out.push(E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title' }, [ h ]),
				E('div', { 'class': 'cbi-value-field' }, [
					E('div', {}, [ _('serves: %s').format(hosts[h].join(', ')) ]),
					E('code', { 'style': 'word-break:break-all;display:block;margin:.3em 0' }, [ authorizedLine(view.key, hosts[h], sshHelper(h)) ]),
					E('button', { 'class': 'btn cbi-button cbi-button-action', 'click': ui.createHandlerFn(view, function() {
						dom.content(res, E('p', { 'class': 'spinning' }, _('Connecting…')));

						return fs.exec('/usr/bin/wwandctl', [ 'rsim', 'scan', h, '--json' ]).then(function(r) {
							var o = null;

							try { o = JSON.parse(r.stdout || ''); } catch (e) { o = null; }

							dom.content(res, (o && o.ok)
								? level('ok', _('Works: rsim-card there answers (backends: %s).').format((o.backends || []).join(', ')))
								: view.failure(h, (o && o.error) || _('no answer'), o && o.detail, o && o.hint));
						});
					}) }, _('Test')),
					res,
				]),
			]));
		});

		dom.content(this.keyBox, out);
	},
});
