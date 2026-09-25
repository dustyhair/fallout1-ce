# Multiplayer rebase regression — 2026-09-25

Audited `multiplayer-plan` at `d843688` against `MULTIPLAYER_PLAN.md`, with
particular attention to Phase 6 and the Phase 7 compatibility gate. Changes and
results below describe the working-tree fixes on top of that commit.

## Fixes found by regression

- Bilateral trades of identical partial item stacks could merge away the item
  ID promised by the second offer. Reserve inventory capacity and detach both
  offers before delivering either. The new live `trade` fixture swaps Stimpaks
  in both directions plus caps, and checks quantities and complete peer digests.
- Malformed or overflowing optional item IDs in semantic trade commands could
  be accepted as caps-only offers. Reject them and retain valid caps-only input.
- The campaign accepted two local pass markers without checking printed peer
  digests. Compare them, require recovery digests, and test mismatch/missing
  digest rejection without proprietary data. Add a hard timeout kill deadline
  and wait for the host listener instead of guessing a startup delay.
- Applying a complete live checkpoint did not advance the guest acknowledgement
  past replay events superseded by map arrival. Confirm the applied snapshot
  boundary; add a headless cursor-gap test and verify guest-led travel convergence.
- World-map fixtures measured different event boundaries during peer shutdown.
  Keep the host connection alive while the guest drains the final checkpoint.
- Recovery fixture startup could race the newly restarted host listener. Retry
  guest bootstrap within a fixed deadline; do not weaken handshake checks.
- Engine UBSan detected signed overflow in scroll bounds on elevations without
  blockers. Use finite projected map bounds when the blocker set is empty.

## Plan audit

| Requirement | Evidence |
| --- | --- |
| Revisioned bilateral trade, cancellation, stale confirmations, conservation | `testDirectTradeController`, command parser tests, new two-process `trade` fixture |
| Roster cap splitting and alternating contested loot | `testLootDistributionController`, live `loot` fixture |
| Versioned roster sidecar, migration, slot claims/replacement, activity persistence | `testMultiplayerSaveSidecar`, snapshot/recovery core tests |
| Stop, save, reload, resume without losing items | Live `recovery`, hidden native save plus sidecar, matching peer digests |
| Network faults, malformed input, ownership and replica authority | Headless multiplayer suite and installed-data scenarios |
| Optional multiplayer and single-player preservation | Both build configurations, disabled launch-flag rejection, native UI save/load |
| Earlier exploration, travel, combat and dialogue phases | Expanded 42-scenario regression matrix |

The full runner is enabled with `FALLOUT_CAMPAIGN_FULL=1`. The default persistent
campaign now includes 13 scenarios, including live direct trading. The full run
adds all existing individual combat/travel scenarios and rest/world-map variants.

## Validation

- Linux multiplayer-enabled Debug build: 3/3 CTest targets pass.
- Linux multiplayer-disabled build: 2/2 CTest targets pass; multiplayer launch
  flags fail explicitly.
- ASan + UBSan headless build: 3/3 CTest targets pass.
- ASan + UBSan live trade: both peers pass, six Stimpaks and seven caps conserved,
  matching digest `168284585708350595`, no sanitizer diagnostics after the bounds
  fix. Leak detection was disabled for the engine run; UBSan halted on errors.
- Python tooling and campaign-gate tests: 17/17 pass.
- Disabled-build UI: created a new character, saved through the native picker,
  loaded the resulting save, and observed `Game Saved.` and `Game Loaded.` in
  the agent journal. Only an isolated copied data tree was changed. The local
  Speech Dispatcher service blocked initialization, so this UI check used an
  unavailable Speech Dispatcher address and SDL dummy audio.

All 42 distinct installed-data scenarios passed across isolated batches. Both
processes exited successfully in each selected result. Affected travel,
reconnect, trade, and recovery cases were rerun after fixes; this was not one
uninterrupted 42-scenario process run. The full-mode runner test also checks
that every scenario has a distinct name, preserving both dialogue talker logs.

Local artifacts retained for review:

- `/tmp/fallout-regression-evidence/summary.tsv`: consolidated 42-case results
  and source-run directories; each scenario subdirectory contains both logs.
- `/tmp/fallout-cursor-regression`: final acknowledgement-repair scenarios.
- `/tmp/fallout-final-recovery-regression`: encounter, interrupted rest,
  scripted combat, guest dialogue, and recovery after the acknowledgement fix.
- `/tmp/fallout-trade-sanitize-clean`: engine ASan/UBSan trade logs.
- `/tmp/fallout-singleplayer.jsonl`: native save/load UI evidence.

The failed intermediate runs remain in `/tmp` for diagnosis. Startup races,
obsolete acknowledgement cursors, and the initial identical-stack trade failure
were resolved and the affected scenarios rerun successfully.

## Limits

This is Linux testing against the installed Fallout data edition. Windows,
manual companion recruitment, an entire ending route, and an extended human
playthrough were not run. The existing compatibility documentation already
identifies companion recruitment as a manual edition-specific check. More than
two live players remains the explicit post-MVP milestone; roster-shaped core
controllers retain their synthetic multi-player tests. No wire or sidecar
version change is needed for these fixes.
