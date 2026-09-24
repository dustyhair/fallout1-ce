# Snapshot recovery

Snapshot version 9 contains the state needed to recover the live two-player experiment:

- Session phase, phase revision, authoritative world time, and last included event sequence.
- Player actor identity, owner, tile, elevation, rotation, hit points, and complete progressing character build.
- Registered non-player critter identity, prototype, position, rotation, hit points, action points, combat results, and team.
- Registered door identity, open state, lock state, and animation frame.
- Registered non-door scenery identity, prototype, position, rotation, art/frame, shared object flags, light, and concrete two-word scenery data.
- Registered item identity, direct inventory holder or ground tile, elevation, stack quantity, bounded prototype/state descriptor, art/frame, shared object flags, and light. This includes concrete container presentation state.
- The indexed game-global, map-global, and map-local arrays visible to scripts.
- The ordered timed-event queue, including absolute trigger time, stable owner identity, and bounded type-specific payload.

The wire format uses fixed-width big-endian fields. Its 28-byte header carries the format version, payload length, snapshot checksum, and last included event. The checksum covers the event sequence and payload. Decoders reject unknown versions, payloads over 512 KiB, invalid counts, duplicate entity IDs, malformed state, truncation, trailing bytes, and checksum failures. Shared object flags deliberately omit process-local discovery/selection and object-lifetime bits.

Actors, including their character builds, critters, doors, non-door scenery, and items are sorted by `EntityId` before encoding or hashing. Variables retain their engine-defined index order. State insertion order therefore cannot create a false mismatch.

## Sectioned digest

The diagnostic digest has nine sections in comparison order:

1. `Session`
2. `Actors`
3. `Critters`
4. `Doors`
5. `Scenery`
6. `Items`
7. `Globals`
8. `MapVariables`
9. `TimedEvents`

The comparison reports the first section that differs. This is more useful during development than one unexplained checksum failure. The checksum and digests use 64-bit FNV-1a for deterministic corruption and divergence detection. They are not authentication and must not replace the authenticated transport required for direct-IP play.

## Recovery contract

`SnapshotReplica::apply` validates the entire snapshot before replacing replica state. A failed validation leaves the existing state unchanged. A successful application stores actors, critters, doors, scenery, and items in canonical entity order. The engine adapter requires exact variable-array sizes for the loaded map, restores the authoritative indexed values, removes registered local items absent from the host, restores registered scenery and item presentation/data state, and can recreate a missing described item and move it between the authoritative holder and map position, including repairing a pickup, split, ground drop, or scripted consumption whose journal event has expired.

The headless recovery test creates a host snapshot with actors and distinct progressing builds, a critter, a door, non-door scenery, an inventory stack, a ground item, script-visible variables, and timed events. It checks independent divergence in every section before applying the host state and confirming every section digest matches again. The installed-data smoke hook additionally awards party XP only on the host, converges both builds through the checkpoint, and captures, replaces, and recaptures the live engine queue before starting its two-process network checks.

The item section tracks objects registered for live pickup and looting, including player or script-created items introduced through an authoritative transfer. Map-local variables cover the indexed local storage used by map scripts. Timed events encode the six integer values used by drug events, three used by withdrawal, two used by script and radiation events, and no payload for the remaining event types. Script events normalize their unused legacy owner pointer; every other owner-dependent event requires a registered `EntityId`. Unsupported payloads or missing owners fail closed. Interpreter stacks and program counters are not serialized because a network guest never resumes them. Full combat state and dialogue remain outside the recovery snapshot. Once a network guest enters the world, its interpreter background loop, direct script dispatcher, queued-event processor, and pending script requests are suppressed; only the host executes them, and snapshots correct the guest's world time and covered results.

At a shared cross-map elevator boundary, Fallout's loader preserves only the
process-local `obj_dude`. The multiplayer bridge detaches the remote actor's
inventory before the load, recreates that actor afterward, rebinds its existing
player and entity identity, restores its critter and inventory state, and then
registers the destination map canonically. This keeps inventory objects and
timed-event owners addressable by the following event replay and snapshot
checkpoint instead of treating the recreated actor as a new player.
