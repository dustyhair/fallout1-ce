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
markers. The engine fixture itself compares every sectioned authoritative
checksum, verifies authenticated reconnect/replay, and rejects guest-side
scripts, combat rules, random draws, or timed queues where applicable.

The maintained matrix is:

| Fixture | Compatibility coverage |
| --- | --- |
| direct-trade/fault core | Barter revisions and conservation, loot rotation, transport faults, wire bounds |
| movement | Base scripts, pathfinding, modal phase gate, party XP, replay |
| loot, transfer, and container | Inventory identity, quantity/cap conservation, dense item state |
| quest | Major item-on-critter script, timer removal, globals, reputation, XP |
| elevator and map transition | Independent elevation, shared map loading, actor/inventory rebinding |
| world-map encounter | Encounter selection/spawn, clock, persistent world-map state |
| interrupted rest | Timed queue execution, healing boundary, interruption |
| scripted combat | Script-requested combat, AI/companion-shaped critter snapshots, status state |
| dialogue | Guest talker stats, voting, branching script, quest activity, combat transition |
| recovery | Ending acknowledgement, hidden save, sidecar, fresh session, map-aware load |

Fallout 1 has no generic companion recruitment fixture that is stable across
all data editions. Companion-shaped state is therefore guarded at the engine
boundary: non-player critters, their inventories, AI-owned combat turns, map
rebinding, and save/load state are included in sectioned snapshots and the
scripted-combat/map-transition/recovery fixtures. Recruitment dialogue content
remains a manual campaign check for a particular data edition.

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
