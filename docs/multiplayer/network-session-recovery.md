# Network session recovery

The network recovery foundation keeps three kinds of state separate:

- `EventJournal` retains a bounded suffix of authoritative gameplay events. Both event count and encoded byte size are bounded. Events must be contiguous, and replay is available only when the peer's next sequence is still retained. A peer older than that boundary must receive a snapshot. A peer claiming a future sequence is rejected.
- `SnapshotRecoveryQueue` holds events created after an in-flight snapshot. It requires the first event after `lastIncludedEvent` and every later event to be contiguous. If the byte limit is exceeded, the transfer is marked for restart; it never returns an incomplete snapshot-plus-events batch.
- `ReconnectTokenRegistry` binds a 256-bit opaque token to one player slot and one session. Resetting the session revokes every token, and individual slots can be revoked. Token comparisons inspect every byte.

Token generation is deliberately outside the registry. The runtime must fill tokens from an operating-system cryptographic random source. Reconnect tokens are credentials: they must only cross an authenticated encrypted transport, or a build explicitly limited to a trusted LAN. The current direct-IP TCP transport is not encrypted, so live reconnect exchange remains disabled.

The headless test covers contiguous append, count and byte bounds, replay at the retention boundary, future and stale peers, journal restoration after a snapshot, token binding and revocation, queued post-snapshot events, sequence gaps, and overflow restart.

## Current boundary

These primitives do not yet alter the live host/guest connection. The current exploration path still sends movement, facing, and door events peer-to-peer. The next integration step is to route guest intents through the host, append only host-issued events to the journal, carry a peer's last applied event during reconnect, and choose journal replay or a fresh world snapshot before re-enabling input.
