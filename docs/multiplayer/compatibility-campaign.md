# Compatibility campaign

Phase 7 freezes the two-player v1 protocol family only after one repeatable
campaign passes against installed Fallout data. Run it from a multiplayer-on
build:

```text
tools/run_multiplayer_compatibility_campaign.sh build/fallout-ce /path/to/Fallout campaign-output
```

The runner creates isolated host and guest patch trees, keeps them across the
whole campaign, opens a fresh TLS session per fixture, and records each process
log plus `summary.tsv`. A `BUILD_TESTING=ON` headless executable next to the
game binary is required; it gates direct-trade transactions, loot rotation,
fault injection, protocol bounds, and other core invariants before proprietary
data is loaded. Every engine fixture requires a zero exit status and both pass
markers. Printed final digests must match across peers; recovery must print a
digest on both peers. Processes are forcibly stopped if they ignore the timeout. The engine fixture itself compares every sectioned authoritative
checksum, verifies authenticated reconnect/replay, and rejects guest-side
scripts, combat rules, random draws, or timed queues where applicable.

The maintained matrix is:

| Fixture | Compatibility coverage |
| --- | --- |
| direct-trade/fault core | Barter revisions and conservation, loot rotation, transport faults, wire bounds |
| movement | Base scripts, pathfinding, modal phase gate, party XP, replay |
| direct trade | Bilateral partial-stack exchange with identical items, cap/item conservation, matching final digests |
| loot, transfer, and container | Inventory identity, quantity/cap conservation, dense item state |
| inventory actions | Native combat door/pickup/loot AP, movement, turn ownership, stale turns, replay, scoped loot access, ordinary inventory cost |
| quest | Major item-on-critter script, timer removal, globals, reputation, XP |
| elevator and map transition | Independent elevation, shared map loading, actor/inventory rebinding |
| world-map encounter | Encounter selection/spawn, clock, persistent world-map state |
| interrupted rest | Timed queue execution, healing boundary, interruption |
| party recovery before each fixture | Host-only native knockout wake-up for every roster actor, host modal exit / guest authority wait, rest/travel defeat boundaries including a zero-return lethal callback |
| scripted combat | Script-requested combat, AI/companion-shaped critter snapshots, status state |
| dialogue | Guest talker stats, voting, branching script, quest activity, combat transition |
| recovery | Ending acknowledgement, hidden save, sidecar, fresh session, map-aware load |

The standard scripted-combat fixture covers companion-shaped critters. Separate
installed-data native drivers now recruit actual Ian through ordinary voted
dialogue, dismiss and recruit him again, retain equipment across shared maps,
and recover his native party membership or corpse and resources from disk and
reconnect. These checks target the matched installed data. Native Ian combat and retaliation also pass. The other recruitable companions
and quest-specific loyalty remain content acceptance work.

Current separate native drivers are:

| Driver | Verified behavior and remaining scope |
| --- | --- |
| `test_native_multiplayer_terminal_dialogue.py` | Actual dialogue, barter and quantity windows close on either player's defeat; scripted damage action, simultaneous deaths and terminal reconnect. |
| `test_native_multiplayer_story.py` | Host-selected destruction movies, settlement slides and departure/credits on both clients; independent skipping, full guest watching, reconnect, missing assets and inactive-window playback. Quest completion itself remains the content campaign gate. |
| `test_native_multiplayer_companion.py` | Actual Ian recruitment/dismissal/recruitment, native gear fixture, map travel, floors, death/corpse resources and fresh disk/reconnect. Native AI pistol combat and retaliation, corpse resources, disk load and reconnect pass. |
| `test_native_multiplayer_character_editor.py` | Ordinary native editor cancellation, skill/perk spending, per-player entitlement and recovery. |
| `test_native_multiplayer_explosive_timer.py` | Both native timer prompts, cancel, arm, stack splitting, save/reconnect and terminal prompt cancellation. |
| `test_native_multiplayer_automap.py` | Local markers/floors/scanner charges, actual native windows, Pip-Boy and screensaver terminal exit, timed poison/radiation. |
| `test_native_multiplayer_inventory.py` | Actual drags, context actions, nested containers, combat navigation and terminal held-input cancellation. |
| `test_native_multiplayer_container_loot.py` | Ordinary approach, host/guest Take All, combat AP, invalidated target, matching resources and clean shutdown. |

Per-player active hand is covered by the core round-trip/migration checks and
native recovery fixture, including host left and guest right. Current formats
are gameplay wire 47, snapshot 26, sidecar 8 and recovery wire 4. Four-actor core
checks prove bounded actor identity and independent hand/completion encoding;
the live lobby and transport still support two players.

## Pass criteria

The campaign fails on any process error, missing pass marker, state-section
divergence, replay failure, save/sidecar mismatch, or fixture conservation
failure. A pass means no detected authoritative divergence, item or cap
duplication/loss, recovery corruption, or guest authority leak across the
matrix. It is a deterministic compatibility gate, not a claim that every
third-party mod or every Fallout script has been exercised.

The live lobby keeps a bounded 128-record diagnostic ring with direction,
message kind, ordered sequence, payload size, decode rejection, and sectioned
state checksums. Shutdown prints aggregate sent/received/rejected and checksum
counters as `MULTIPLAYER_PROTOCOL_DIAGNOSTICS`; smoke logs also print the full
section checksums at convergence boundaries.

## Full regression

The default campaign contains 18 installed-data scenarios. Set
`FALLOUT_CAMPAIGN_FULL=1` to run all 50 scenarios, including the individual
combat actions, both dialogue talkers, independent and shared stairs,
world-map controller/takeover variants, rest healing, and world-map persistence:

```sh
FALLOUT_CAMPAIGN_FULL=1 tools/run_multiplayer_compatibility_campaign.sh \
    build-review/fallout-ce /path/to/Fallout /tmp/fallout-full-regression
```

The runner's digest rejection checks run without game data:

```sh
PYTHONPATH=tools python3 -m unittest discover -s tools/tests -v
```

For current completion blockers, missing native UI coverage, and the full
story acceptance plan, see the
[October 1 completion review](completion-review-and-plan-2026-10-01.md).
Passing this fixture matrix does not establish full campaign completion.

## Native loot windows

The separate native UI driver opens the actual inventory window, takes both
resource stacks, closes it, and checks following combat, matching peer digests,
and clean shutdown. It uses muted isolated Xvfb sessions. Run exploration and
guest combat coverage with installed data:

```sh
python3 tools/test_native_multiplayer_container_loot.py \
    build/fallout-ce /path/to/Fallout --port 49700
python3 tools/test_native_multiplayer_container_loot.py \
    build/fallout-ce /path/to/Fallout --port 49701 --combat-loot
```

Add `--host-looter` to the exploration command for the host window. Combat
coverage checks a 3 AP loot interaction, authoritative resource transfer, and
a subsequent attack. Core and engine fixtures separately check foreign/stale
turn rejection, movement AP, native door/pickup costs, and closing access.

## Native terminal windows and knockout recovery

Automap death tests require scanner input in both actual windows, then apply
native lethal damage on the host. Both windows must unwind and the guest must
acknowledge Ending:

```sh
python3 tools/test_native_multiplayer_automap.py \
    build/fallout-ce /path/to/Fallout --port 50932 --defeat guest
python3 tools/test_native_multiplayer_automap.py \
    build/fallout-ce /path/to/Fallout --port 50933 --defeat host
python3 tools/test_native_multiplayer_inventory.py \
    build/fallout-ce /path/to/Fallout --port 50935 --defeat-drag
python3 tools/test_native_multiplayer_inventory.py \
    build/fallout-ce /path/to/Fallout --port 50936 --defeat-context-menu
```

The inventory drivers leave the mouse held throughout shutdown; releasing it
would hide the drag/menu hang. The context-menu case also checks that the
cancelled action leaves the host inventory unchanged. All sessions are isolated
and muted. These flags invoke test fixtures, not gameplay death-mode selectors.

The recovery smoke scenario now persists a knocked-out guest and its exact
wake deadline across a fresh disk-loaded session and authenticated reconnect.
Both the recovery save and `--multiplayer-smoke-saved-slot` path verify that
knockout remains active, the timer is unique and unchanged, and peer digests
match. Native fixtures separately check each roster actor’s queue wake-up and
that incapacitated voters do not delay an awake teammate’s dialogue choice.

Native poison and radiation tests use the real queue callbacks and payloads.
Only the queued hazard’s wait is shortened; the other player’s effects must
remain unchanged. Run either player as the victim:

```sh
TMPDIR=/var/tmp python3 tools/test_native_multiplayer_automap.py \
    build/fallout-ce /path/to/Fallout --defeat guest --hazard poison
TMPDIR=/var/tmp python3 tools/test_native_multiplayer_automap.py \
    build/fallout-ce /path/to/Fallout --defeat host --hazard radiation
TMPDIR=/var/tmp python3 tools/test_native_multiplayer_explosive_timer.py \
    build/fallout-ce /path/to/Fallout --defeat guest
TMPDIR=/var/tmp python3 tools/test_native_multiplayer_automap.py \
    build/fallout-ce /path/to/Fallout --defeat guest --screen pipboy
TMPDIR=/var/tmp python3 tools/test_native_multiplayer_automap.py \
    build/fallout-ce /path/to/Fallout --defeat host --screen screensaver
```

Timer death cases require both actual prompts to open and verify neither
explosive stack is split or armed. Pip-Boy cases require both actual screens
to open; screensaver cases shorten the fixture idle timeout to enter the real
native animation without waiting two minutes. Normal gameplay keeps the native
idle timeout. `TMPDIR=/var/tmp` avoids filling a small memory-backed `/tmp`
with the isolated game-data copies. These tests preserve their logs and never
use the visible desktop session.
