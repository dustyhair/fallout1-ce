# Gameplay wire format

Gameplay messages use the shared protocol envelope and the session established by the handshake. Each direction continues its exact envelope sequence after the character lobby hands off the connection.

The version 2 command payload covers movement, door use, item pickup, and looting. Every command carries its own command sequence, player and actor identities, expected session phase, and phase revision. Results report acceptance or a specific rejection and identify the authoritative event range. Events cover the corresponding movement, door, pickup, and loot actions and link back to the command that caused them. Live movement events include the exact sequence of hex directions chosen by the sending client, so the peer follows the same route instead of independently pathfinding to the final tile. A separate facing event keeps actor rotation synchronized when movement or direct rotation changes it.

All integer fields are fixed-width and big-endian. Decoders reject zero identities and sequences, unknown versions or message types, invalid phases, noncanonical booleans, nonzero reserved bytes, malformed lengths, trailing bytes, and inconsistent accepted or rejected results.

The headless test suite round-trips every message type and exercises malformed input. The two-process engine smoke test carries a guest move command over TCP after the lobby, then carries the host result and movement event back to the guest.
