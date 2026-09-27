#!/bin/sh
# wwand-rsim tests. They run against a wwand source checkout, the one the plugin
# is loaded into: WWAND_SRC (default ../wwand beside this repository).
#
# An overlay directory makes the plugin reachable the way the daemon reaches
# it, as wwand.plugins.rsim and wwand.ctl.rsim, next to wwand's own modules.

set -e

TESTDIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$TESTDIR")"
WWAND_SRC="${WWAND_SRC:-$REPO/../wwand}"
WWAND_SRC="$(cd "$WWAND_SRC" && pwd)"

if [ -x "$HOME/.local/bin/ucode" ]; then
	UCODE="$HOME/.local/bin/ucode"
	export LD_LIBRARY_PATH="$HOME/.local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
	MODPATH="$HOME/.local/lib/ucode/*.so"
else
	UCODE=ucode
	MODPATH="/usr/lib/ucode/*.so"
fi

OVL="$(mktemp -d)"
trap 'rm -rf "$OVL"' EXIT
mkdir "$OVL/wwand"
for f in "$WWAND_SRC"/src-ucode/*; do ln -s "$f" "$OVL/wwand/"; done
ln -s "$REPO/plugins" "$OVL/wwand/plugins"
ln -s "$REPO/ctl" "$OVL/wwand/ctl"

# the end-to-end test needs the helper and a simulated reader on a pty
RIG_PID=""
HELPER_BIN="${RSIM_HELPER:-$REPO/helper/build/rsim-card}"
if [ -x "$HELPER_BIN" ] && command -v python3 >/dev/null 2>&1; then
	mkfifo "$OVL/hold"
	python3 "$TESTDIR/e2e/card_rig.py" <"$OVL/hold" >"$OVL/rig" &
	RIG_PID=$!
	exec 4>"$OVL/hold"
	for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$OVL/rig" ] && break; sleep 0.2; done
	read RSIM_E2E_TTY RSIM_TEST_MCTRL <"$OVL/rig"
	export RSIM_E2E_TTY RSIM_TEST_MCTRL
	export RSIM_E2E_HELPER="$HELPER_BIN"
fi
trap 'exec 4>&-; [ -n "$RIG_PID" ] && kill $RIG_PID 2>/dev/null; rm -rf "$OVL"' EXIT

rc=0
for t in "$TESTDIR"/test_*.uc; do
	out=$(cd "$TESTDIR" && "$UCODE" -L "$MODPATH" -L "$WWAND_SRC/io/build-host/*.so" \
		-L "$OVL/*.uc" "$t" 2>&1) || true
	printf '%s\n' "$out"
	printf '%s\n' "$out" | grep -qE '^test_[a-z_]+: [0-9]+ checks, 0 failures$' || rc=1
done

# the LuCI page, run in node against stand-ins for LuCI's modules
if command -v node >/dev/null 2>&1; then
	node "$TESTDIR/luci/test_rsim_js.js" || rc=1
else
	echo "test_rsim_js: skipped (no node)"
fi

exit $rc
