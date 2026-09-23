# Network session recovery

The network recovery foundation keeps three kinds of state separate:

- `EventJournal` retains a bounded suffix of authoritative gameplay events. Both event count and encoded byte size are bounded. Events must be contiguous, and replay is available only when the peer's next sequence is still retained. A peer older than that boundary must receive a snapshot. A peer claiming a future sequence is rejected.
- `SnapshotRecoveryQueue` holds events created after an in-flight snapshot. It requires the first event after `lastIncludedEvent` and every later event to be contiguous. If the byte limit is exceeded, the transfer is marked for restart; it never returns an incomplete snapshot-plus-events batch.
- `ReconnectTokenRegistry` binds a 256-bit opaque token to one player slot and one session. Resetting the session revokes every token, and individual slots can be revoked. Token comparisons inspect every byte.

`generateReconnectToken` seeds a CTR-DRBG from Mbed TLS platform entropy and fills each credential from the operating-system cryptographic random source. Tokens cross only the TLS transport. A guest can verify the host's displayed certificate fingerprint on first contact; omitting it explicitly uses TOFU. Reconnects always require the observed certificate pin before the token can leave the client.

The headless test covers contiguous append, count and byte bounds, replay at the retention boundary, future and stale peers, journal restoration after a snapshot, cryptographic token generation, token binding and revocation, reconnect wire round trips, pinned TLS rejection, lobby reattachment, queued post-snapshot events, sequence gaps, and overflow restart.

## Current boundary

Movement, facing, doors, pickup, loot initiation, loot-window inventory transfers, and inventory drops route both host and guest intents through the host command processor. Only host-issued events enter the session-wide sequence and bounded journal. Deferred pickup completion enters that same journal after the host script finishes, carrying the concrete inventory result instead of relying on snapshot timing. The live connection detects event gaps and requests recovery from the last contiguous sequence. The host replays retained events when possible; otherwise it captures and sends the current player-actor, NPC-critter, door, registered-item, world-time, game-global, map-global, map-local, and typed timed-queue state. The same complete snapshot shape is used for periodic authoritative correction, so presentation-only cues never require the guest to rerun gameplay logic. The guest does not run the interpreter or timed-event queue after entering the network world. Recovery completion verifies that the guest reached the host's latest event sequence.

Snapshot capture waits until player and door animations are idle. This keeps `lastIncludedEvent` aligned with the concrete actor, door, item-holder, and ground-item state represented by the snapshot instead of truncating an in-progress authoritative action.

The host keeps the TLS listener alive after the first join. A disconnected guest retries with the pinned host identity and sends its session ID, guest slot, reconnect credential, and last confirmed applied event. The host validates all four fields, rejects future recovery claims, resets per-connection envelope sequences, and resumes through the same journal-or-snapshot flow. Host events may continue entering the journal while the guest is absent; disconnected guest input is blocked.
