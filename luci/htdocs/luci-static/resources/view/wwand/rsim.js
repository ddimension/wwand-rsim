'use strict';
'require view';
'require form';
'require rpc';
'require ui';
'require fs';
'require uci';

/* Remote SIM (wwand-rsim): per modem, where its SIM comes from — a reader on
   the router, a reader on another machine over SSH, or another wwand modem
   lending its card — kept in /etc/config/network on the wwand_modem section
   like every other rsim_* option. The source is ONE option, rsim_reader
   ("phoenix:/dev/ttyUSB0", "wbsm:", "pcsc:0", "ssh:user@host:wbsm:",
   "modem:wwmodem1"); this page splits it into fields and puts it back
   together, so what `uci show` says and what the page shows are the same
   thing. Status and restart go through the plugin (modem_plugin_status /
   modem_plugin). */

var callStatus = rpc.declare({ object: 'wwand', method: 'status', expect: { modems: {} } });
var callRsimStatus = rpc.declare({ object: 'wwand', method: 'modem_plugin_status',
	params: [ 'modem', 'plugin', 'op' ], expect: {} });
var callRsim = rpc.declare({ object: 'wwand', method: 'modem_plugin',
	params: [ 'modem', 'plugin', 'op' ], expect: {} });

/* rsim_reader -> { src, value, remote } ; the inverse of compose() */
function parse(r) {
	var m;

	if (!r)
		return { src: '' };
	if ((m = r.match(/^modem:(.+)$/)))
		return { src: 'modem', donor: m[1] };
	if ((m = r.match(/^ssh:([^@:]+@[^:]+):(.+)$/)))
		return Object.assign(parse(m[2]), { ssh: m[1] });
	if ((m = r.match(/^(phoenix|pcsc):(.+)$/)))
		return { src: m[1], dev: m[2] };
	if ((m = r.match(/^wbsm:(.*)$/)))
		return { src: 'wbsm', dev: m[1] };

	return { src: '' };
}

function compose(p) {
	var local;

	switch (p.src) {
	case 'modem':   return p.donor ? 'modem:' + p.donor : null;
	case 'phoenix': local = p.dev ? 'phoenix:' + p.dev : null; break;
	case 'pcsc':    local = 'pcsc:' + (p.dev || '0'); break;
	case 'wbsm':    local = 'wbsm:' + (p.dev || ''); break;
	default:        return null;
	}

	return (local && p.ssh) ? 'ssh:' + p.ssh + ':' + local : local;
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
				return { modems: names, st: sts, key: r[1] || r[2] };
			});
		});
	},

	render: function(data) {
		var m, s, o;
		var modems = uci.sections('network', 'wwand_modem').map(function(x) { return x['.name']; });

		m = new form.Map('network', _('Remote SIM'),
			_('Use a SIM card that is not in the modem: in a reader on this router, in a reader on another machine reached over SSH, or in another wwand modem that lends its card. The modem firmware must allow it (Quectel: <code>wwandctl rsim MODEM enable --reset</code>). Removing the source gives the modem its own SIM back.'));

		s = m.section(form.TypedSection, 'wwand_modem', _('Modems'));
		s.anonymous = false;
		s.addremove = false;

		/* the split view of rsim_reader: every field below is virtual and
		   written back as the one option by the source field */
		var cur = {};
		var get = function(sid) {
			if (!cur[sid])
				cur[sid] = parse(uci.get('network', sid, 'rsim_reader'));
			return cur[sid];
		};

		/* The fields are written one after the other, in no order this page
		   controls, so each of them puts the whole option back together
		   from the current split view — the last one writes the final
		   value. */
		var store = function(sid) {
			var r = compose(get(sid));

			if (r) uci.set('network', sid, 'rsim_reader', r);
			else uci.unset('network', sid, 'rsim_reader');
		};

		o = s.option(form.ListValue, '_src', _('SIM source'));
		o.value('', _('the modem\'s own SIM'));
		o.value('wbsm', _('WB Electronics Smartmouse USB'));
		o.value('phoenix', _('Phoenix/Smartmouse serial reader'));
		o.value('pcsc', _('PC/SC reader'));
		o.value('modem', _('another modem lends its card'));
		o.cfgvalue = function(sid) { return get(sid).src; };
		o.write = function(sid, v) { get(sid).src = v; store(sid); };
		o.remove = function(sid) { get(sid).src = ''; store(sid); };

		var field = function(key, title, descr, dep) {
			var f = s.option(form.Value, '_' + key, title, descr);

			f.cfgvalue = function(sid) { return get(sid)[key]; };
			f.write = function(sid, v) { get(sid)[key] = v; store(sid); };
			f.remove = function(sid) { get(sid)[key] = null; store(sid); };
			(dep || []).forEach(function(d) { f.depends('_src', d); });
			return f;
		};

		/* empty is valid for two of the three: the Smartmouse USB means "the
		   first one", PC/SC means reader 0; only a Phoenix reader needs a port */
		o = field('dev', _('Reader'), _('Phoenix: the serial port (/dev/ttyUSB0). PC/SC: reader name or index. Smartmouse USB: its USB serial number, empty for the first one.'),
			[ 'phoenix', 'pcsc', 'wbsm' ]);
		o.optional = true;
		o.validate = function(sid, v) {
			return (this.section.formvalue(sid, '_src') == 'phoenix' && !v)
				? _('A Phoenix reader needs its serial port') : true;
		};
		o = field('ssh', _('On another machine'), _('<code>user@host</code> to run the reader there over SSH; empty for a reader on this router.'),
			[ 'phoenix', 'pcsc', 'wbsm' ]);
		o.optional = true;
		o.validate = function(sid, v) {
			return (!v || /^[A-Za-z0-9._-]+@[A-Za-z0-9._-]+$/.test(v)) ? true : _('Expecting user@host');
		};

		o = s.option(form.ListValue, '_donor', _('Lending modem'));
		o.depends('_src', 'modem');
		modems.forEach(function(n) { o.value(n, n); });
		o.cfgvalue = function(sid) { return get(sid).donor; };
		o.write = function(sid, v) { get(sid).donor = v; store(sid); };
		o.validate = function(sid, v) {
			return (v && v == sid) ? _('A modem cannot lend its card to itself') : true;
		};

		o = s.option(form.ListValue, 'rsim_donor_mode', _('How it lends'),
			_('SIM Access: the lending modem hands its card over and stops using it (needs <code>wwandctl rsim DONOR sap-enable</code> on Quectel). APDU: the card stays with it, which must have its radio off.'));
		o.depends('_src', 'modem');
		o.value('sap', _('SIM Access Profile'));
		o.value('apdu', _('APDU'));
		o.optional = true;

		o = s.option(form.ListValue, 'rsim_donor_apdu', _('APDU channel'));
		o.depends({ '_src': 'modem', 'rsim_donor_mode': 'apdu' });
		o.value('auto', _('automatic (QMI, else AT)'));
		o.value('qmi', 'QMI UIM');
		o.value('at', 'AT+CSIM');
		o.optional = true;

		o = s.option(form.ListValue, 'rsim_clock', _('Card clock'), _('Must match the reader. The Smartmouse USB is set to it.'));
		o.depends('_src', 'wbsm');
		o.depends('_src', 'phoenix');
		[ '3580', '3680', '6000' ].forEach(function(v) { o.value(v, v + ' kHz'); });
		o.optional = true;

		o = s.option(form.ListValue, 'rsim_mode', _('Reader mode'));
		o.depends('_src', 'wbsm');
		o.value('phoenix', 'Phoenix');
		o.value('smartmouse', 'Smartmouse');
		o.optional = true;

		o = s.option(form.ListValue, 'rsim_slot', _('Modem slot'), _('The slot of THIS modem the card appears in.'));
		[ 'wbsm', 'phoenix', 'pcsc', 'modem' ].forEach(function(d) { o.depends('_src', d); });
		[ '1', '2', '3' ].forEach(function(v) { o.value(v, v); });
		o.optional = true;

		return m.render().then(function(node) {
			var status = E('div', { 'class': 'cbi-section' }, [ E('h3', {}, _('Status')) ]);

			data.modems.forEach(function(n, i) {
				var st = data.st[i] || {};
				var text = !st.enabled ? _('own SIM')
					: '%s · %s%s'.format(st.reader, st.state, st.apdus ? ' · %d commands'.format(st.apdus) : '')
					  + (st.last_error ? ' · ' + st.last_error : '');

				status.appendChild(E('div', { 'class': 'cbi-value' }, [
					E('label', { 'class': 'cbi-value-title' }, [ n ]),
					E('div', { 'class': 'cbi-value-field' }, [
						/* array: reader names and errors come from the daemon */
						E('span', {}, [ text ]), ' ',
						st.enabled ? E('button', { 'class': 'btn cbi-button',
							'click': ui.createHandlerFn(this, function() {
								return callRsim(n, 'rsim', 'restart');
							}) }, _('Restart')) : '',
					]),
				]));
			});

			if (data.key)
				status.appendChild(E('div', { 'class': 'cbi-value' }, [
					E('label', { 'class': 'cbi-value-title' }, _('Router SSH key')),
					E('div', { 'class': 'cbi-value-field' }, [
						E('code', { 'style': 'word-break:break-all' }, [ data.key.trim() ]),
						E('div', { 'class': 'cbi-value-description' },
							_('Add to <code>~/.ssh/authorized_keys</code> on the machine with the reader. Created with <code>wwandctl rsim ssh-key</code>.')),
					]),
				]));

			return E([], [ node, status ]);
		});
	}
});
