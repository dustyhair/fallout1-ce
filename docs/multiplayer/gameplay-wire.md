# Gameplay wire format

Gameplay messages use the shared protocol envelope and the session established by the handshake. Each direction continues its exact envelope sequence after the character lobby hands off the connection.

The version 4 command payload covers movement, facing, door use, item pickup, looting, and inventory transfers. Every command carries its own command sequence, player and actor identities, expected session phase, and phase revision. Results report acceptance or a specific rejection and identify the authoritative event range. Inventory transfers name the source, destination, item, and whole-stack quantity; their events describe the accepted mutation exactly.

During live exploration, the guest sends commands rather than events. The host validates ownership, phase revision, targets, and action feasibility, executes accepted commands, and returns a result followed by the authoritative event. Host input uses the same ordered event stream. The host assigns one session-wide event sequence and appends every published event to the bounded recovery journal.

Movement events include the exact sequence of hex directions chosen by the host, so the guest follows the authoritative route instead of independently pathfinding to the final tile. Facing is also a command because rotation affects later interaction presentation and must stay in the same event order.

All integer fields are fixed-width and big-endian. Decoders reject zero identities, sequences, and transfer quantities; unknown versions or message types; invalid phases; noncanonical booleans; nonzero reserved bytes; malformed lengths; trailing bytes; and inconsistent accepted or rejected results.

The headless test suite round-trips every message type and exercises malformed input. The two-process engine smoke test carries a guest move command over TCP after the lobby, then carries the host result and movement event back to the guest.
