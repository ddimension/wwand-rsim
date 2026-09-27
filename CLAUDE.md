# wwand-rsim — guide for Claude

**The rule: wwand-rsim — the plugin, `rsim-card`, `luci-app-wwand-rsim`, the
feed package — follows the same project conventions as wwand.** Read and
apply these first; they are not repeated here:

- `../CLAUDE.md` (the wwand workspace: layout, invariants, deploy, test
  routers, feed sequence)
- `../wwand/CLAUDE.md` (the wwand source tree: ucode, codec, tests)
- `../repository/CLAUDE.md` (the feed: `main` only, `scripts/bump-source.sh
  wwand-rsim <commit>`, one commit per bump, one push, then wait for CI;
  never `stable` unless asked)

In short: English everywhere; commit and push only when asked; every suite
green before a commit, every fix with a test that fails without it;
comments say why, with the device and date when the hardware showed it.

## This repo

| Path | What |
|---|---|
| `plugins/rsim.uc` | the wwand plugin (runs in the daemon; wwand's `plugins.uc` API) |
| `ctl/rsim.uc` | `wwandctl rsim …` |
| `helper/` | `rsim-card`, C, CMake — readers, AT+CSIM, Bluetooth SAP, `--list`, `--serve` |
| `luci/` | `luci-app-wwand-rsim` |
| `tests/` | ucode and LuCI-JS tests |

Documents: `README.md` (use, **What works** — clients, providers,
workarounds — keep it true when hardware results change),
`helper/README.md` (the helper, its protocol, `--serve`), `docs/plan.md`
(design).

## Tests

    cd tests && WWAND_SRC=../../wwand sh run_tests.sh      # from a worktree: the wwand tree's path
    cd helper && cmake -B build && cmake --build build && (cd build && ctest)

The plugin depends on the wwand core: a change that needs one is made and
tested there too (`cd ../wwand/tests && sh run_tests.sh`), and both are
pushed together before a feed bump.
