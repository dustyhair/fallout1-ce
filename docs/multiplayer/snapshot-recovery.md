# Snapshot recovery

Snapshot version 3 contains the state needed to recover the live two-player experiment:

- Session phase, phase revision, and last included event sequence.
- Player actor identity, owner, tile, elevation, rotation, and hit points.
- Registered door identity, open state, lock state, and animation frame.
- Registered item identity, direct inventory holder or ground tile, elevation, stack quantity, and bounded prototype/state descriptor.

The wire format uses fixed-width big-endian fields. Its 28-byte header carries the format version, payload length, snapshot checksum, and last included event. The checksum covers the event sequence and payload. Decoders reject unknown versions, payloads over 64 KiB, invalid counts, duplicate entity IDs, malformed state, truncation, trailing bytes, and checksum failures.

Actors, doors, and items are sorted by `EntityId` before encoding or hashing. State insertion order therefore cannot create a false mismatch.

## Sectioned digest

The diagnostic digest has four sections in comparison order:

1. `Session`
2. `Actors`
3. `Doors`
4. `Items`

The comparison reports the first section that differs. This is more useful during development than one unexplained checksum failure. The checksum and digests use 64-bit FNV-1a for deterministic corruption and divergence detection. They are not authentication and must not replace the authenticated transport required for direct-IP play.

## Recovery contract

`SnapshotReplica::apply` validates the entire snapshot before replacing replica state. A failed validation leaves the existing state unchanged. A successful application stores actors, doors, and items in canonical entity order. The engine adapter can recreate a missing described item, restore its mutable state, and move it between the authoritative holder and map position, including repairing a pickup or split whose journal event has expired.

The headless recovery test creates a host snapshot with actors, a door, an inventory stack, and a ground item. It checks independent actor, door, and item divergence before applying the host state and confirming every section digest matches again.

The item section tracks objects registered for live pickup and looting, including player or script-created items introduced through an authoritative transfer. Item script VM state is not serialized. Scripts, map variables, combat, and dialogue remain outside the recovery snapshot.
