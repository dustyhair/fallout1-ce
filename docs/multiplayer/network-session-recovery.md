# Network session recovery

The network recovery foundation keeps three kinds of state separate:

- `EventJournal` retains a bounded suffix of authoritative gameplay events. Both event count and encoded byte size are bounded. Events must be contiguous, and replay is available only when the peer's next sequence is still retained. A peer older than that boundary must receive a snapshot. A peer claiming a future sequence is rejected.
- `SnapshotRecoveryQueue` holds events created after an in-flight snapshot. It requires the first event after `lastIncludedEvent` and every later event to be contiguous. If the byte limit is exceeded, the transfer is marked for restart; it never returns an incomplete snapshot-plus-events batch.
- `ReconnectTokenRegistry` binds a 256-bit opaque token to one player slot and one session. Resetting the session revokes every token, and individual slots can be revoked. Token comparisons inspect every byte.

Token generation is deliberately outside the registry. The runtime must fill tokens from an operating-system cryptographic random source. Reconnect tokens are credentials: they must only cross an authenticated encrypted transport, or a build explicitly limited to a trusted LAN. The current direct-IP TCP transport is not encrypted, so live reconnect exchange remains disabled.

The headless test covers contiguous append, count and byte bounds, replay at the retention boundary, future and stale peers, journal restoration after a snapshot, token binding and revocation, queued post-snapshot events, sequence gaps, and overflow restart.

## Current boundary

Movement, facing, and door use now route guest intents through the host. Only host-issued events enter the session-wide sequence and bounded journal. The live connection detects event gaps and requests recovery from the last contiguous sequence. The host replays retained events when possible; otherwise it captures and sends the current actor-and-door snapshot. Recovery completion verifies that the guest reached the host's latest event sequence.

Snapshot capture waits until player and door animations are idle. This keeps `lastIncludedEvent` aligned with the concrete tile, facing, hit-point, door-frame, and lock state represented by the snapshot instead of truncating an in-progress authoritative action.

The remaining integration step is reconnecting after the TCP connection closes and authenticating the returning guest before using the same replay-or-snapshot flow. Reconnect token exchange remains disabled until the transport is authenticated or explicitly restricted to trusted LAN use.
