# Snapshot recovery

The Phase 0 snapshot contains the state needed to recover the local two-player experiment:

- Session phase, phase revision, and last included event sequence.
- Player actor identity, owner, tile, elevation, rotation, and hit points.
- Registered door identity, open state, lock state, and animation frame.

The wire format uses fixed-width big-endian fields. Its 28-byte header carries the format version, payload length, snapshot checksum, and last included event. The checksum covers the event sequence and payload. Decoders reject unknown versions, payloads over 64 KiB, invalid counts, duplicate entity IDs, malformed state, truncation, trailing bytes, and checksum failures.

Actors and doors are sorted by `EntityId` before encoding or hashing. State insertion order therefore cannot create a false mismatch.

## Sectioned digest

The diagnostic digest has three sections in comparison order:

1. `Session`
2. `Actors`
3. `Doors`

The comparison reports the first section that differs. This is more useful during development than one unexplained checksum failure. The checksum and digests use 64-bit FNV-1a for deterministic corruption and divergence detection. They are not authentication and must not replace the authenticated transport required for direct-IP play.

## Recovery contract

`SnapshotReplica::apply` validates the entire snapshot before replacing replica state. A failed validation leaves the existing state unchanged. A successful application stores actors and doors in canonical entity order.

The headless recovery test creates a host snapshot with two actors and a door, changes the guest position in replica state, detects the actor mismatch, applies the host snapshot, and confirms that every section digest matches again.

This first format does not include inventories, scripts, map variables, combat, or dialogue. Later phases will add those as separate bounded sections instead of changing the meaning of the Phase 0 fields.
