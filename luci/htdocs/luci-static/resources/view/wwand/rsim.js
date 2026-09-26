'use strict';
'require view';
'require form';
'require rpc';
'require ui';
'require fs';
'require uci';

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
				return { modems: names, st: sts, key: r[1] || r[2] };
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
		o.value('modem', _('another modem lends its card (SIM sponsor)'));
		o.default = 'wbsm';
		/* written even when left at the default: the section must say what
		   it is (the plugin assumes the same default, but uci should not
		   depend on that) */
		o.rmempty = false;
		o.description = _('Smartmouse USB: clock and mode are set by software, no driver needed. Phoenix: a serial reader whose clock is set with switches. PC/SC: any CCID reader through pcscd. AT modem: the card in a modem that wwand does not manage — on another machine, or here — reached over its AT port (AT+CSIM). SIM sponsor: another wwand modem on this router lends the card in it.');

		o = s.option(form.Value, 'device', _('Reader'),
			_('Phoenix: its serial port, e.g. <code>/dev/ttyUSB0</code>. PC/SC: the reader\'s name or index (empty: the first). Smartmouse USB: its USB serial number (empty: the first one). AT modem: its AT port, e.g. <code>/dev/ttyUSB2</code>.'));
		o.depends('type', 'wbsm');
		o.depends('type', 'phoenix');
		o.depends('type', 'pcsc');
		o.depends('type', 'at');
		o.optional = true;
		o.validate = function(sid, v) {
			var t = this.section.formvalue(sid, 'type');

			if (t == 'phoenix' && !v)
				return _('A Phoenix reader needs its serial port');
			if (t == 'at' && !v)
				return _('An AT modem needs its AT port');
			return true;
		};

		o = s.option(form.ListValue, 'radio', _('That modem\'s radio'),
			_('<strong>Off</strong> while its card is used elsewhere (<code>AT+CFUN=4</code>; the SIM stays reachable), back to what it was afterwards — two modems must never register with the same card. <strong>Keep</strong> only when that modem is off the network anyway.'));
		o.depends('type', 'at');
		o.value('', _('off while lent (default)'));
		o.value('keep', _('keep as it is'));
		o.optional = true;

		o = s.option(form.Value, 'host', _('On another machine'),
			_('<code>user@host</code> when the reader is attached to another machine: the router runs <code>rsim-card</code> there over SSH. That machine needs rsim-card and access to the reader, and the router\'s SSH key (shown below) in the user\'s <code>~/.ssh/authorized_keys</code>. Leave empty for a reader on this router.'));
		o.depends('type', 'wbsm');
		o.depends('type', 'phoenix');
		o.depends('type', 'pcsc');
		o.depends('type', 'at');
		o.optional = true;
		o.validate = function(sid, v) {
			return (!v || /^[A-Za-z0-9._-]+@[A-Za-z0-9._-]+$/.test(v)) ? true : _('Expecting user@host');
		};

		o = s.option(form.Value, 'helper', _('rsim-card on that machine'),
			_('Its path when it is not in the remote user\'s PATH.'));
		/* regex values: form.js isEqual() takes a RegExp */
		o.depends({ 'type': 'wbsm', 'host': /./ });
		o.depends({ 'type': 'phoenix', 'host': /./ });
		o.depends({ 'type': 'pcsc', 'host': /./ });
		o.depends({ 'type': 'at', 'host': /./ });
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
			_('<strong>SIM Access</strong>: the sponsor hands its card over and stops using it — its own connection goes down, and it gets the card back when the lending ends. Quectel modules need it switched on once: <code>wwandctl rsim SPONSOR sap-enable --reset</code>.<br /><strong>APDU</strong>: the card stays in the sponsor and every command is passed through it. For modems that cannot do SIM Access.<br />Either way the sponsor\'s <strong>radio stays off</strong> for as long as it is configured as a sponsor: two modems must never register with the same card. Its interfaces do not come up meanwhile — an attempt is refused with the reason (<em>Radio off: the SIM card is lent to another modem</em>). Remove the sponsor here and its radio comes back.'));
		o.depends('type', 'modem');
		o.value('', _('SIM Access (default)'));
		o.value('apdu', _('APDU'));
		o.optional = true;

		o = s.option(form.ListValue, 'donor_slot', _('Sponsor\'s slot'),
			_('Which of the sponsor\'s SIM slots holds the card to lend. Only the slot the sponsor <strong>runs on</strong> can be lent: most modems use one slot at a time and switch the other one off, so a modem cannot use one card and lend the other (checked on a Quectel RG502Q and RG650E). Switch the sponsor to the slot first if needed.'));
		o.depends('type', 'modem');
		o.value('', _('the one it runs on (default)'));
		o.value('1', _('slot 1'));
		o.value('2', _('slot 2'));
		o.optional = true;

		o = s.option(form.ListValue, 'donor_apdu', _('APDU channel'),
			_('Automatic tries QMI first and falls back to <code>AT+CSIM</code> (which also works over AT inside MBIM).'));
		o.depends({ 'type': 'modem', 'donor_mode': 'apdu' });
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
					? _('card of %s').format(r.donor || '?') : (r.host ? _('%s on %s').format(t, r.host) : t)));
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

		return m.render().then(function(node) {
			var status = E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Status')),
				E('div', { 'class': 'cbi-section-descr' }, _('What each modem uses right now. Changes above take effect when saved and applied.')),
			]);

			data.modems.forEach(function(n, i) {
				var st = data.st[i] || {};
				var name = st.reader_name ? '%s (%s)'.format(st.reader_name, st.reader || '?') : st.reader;
				var text = st.config_error ? st.config_error
					: !st.enabled ? _('its own SIM')
					: st.conflict ? _('%s — not used: %s').format(name, st.conflict)
					: '%s · %s%s%s'.format(name, st.state, st.apdus ? ' · %d commands'.format(st.apdus) : '',
					                       st.last_error ? ' · ' + st.last_error : '');

				status.appendChild(E('div', { 'class': 'cbi-value' }, [
					E('label', { 'class': 'cbi-value-title' }, [ n ]),
					E('div', { 'class': 'cbi-value-field' }, [
						/* array: reader names and errors come from the daemon */
						E('span', {}, [ text ]), ' ',
						st.enabled ? E('button', { 'class': 'btn cbi-button',
							'title': _('Give the modem its own SIM back for a moment, then offer the remote one again'),
							'click': ui.createHandlerFn(this, function() {
								return callRsim(n, 'rsim', 'restart');
							}) }, _('Restart')) : '',
					]),
				]));
			});

			status.appendChild(E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title' }, _('Router SSH key')),
				data.key
					? E('div', { 'class': 'cbi-value-field' }, [
						E('code', { 'style': 'word-break:break-all' }, [ data.key.trim() ]),
						E('div', { 'class': 'cbi-value-description' },
							_('For a reader on another machine: add this line to <code>~/.ssh/authorized_keys</code> of the user named in <em>On another machine</em>. Created with <code>wwandctl rsim ssh-key</code>.')),
					])
					: E('div', { 'class': 'cbi-value-field' },
						_('None yet. Only needed for a reader on another machine: create it with <code>wwandctl rsim ssh-key</code>.')),
			]));

			return E([], [ node, status ]);
		});
	}
});
