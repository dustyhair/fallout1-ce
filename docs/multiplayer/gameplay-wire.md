# Gameplay wire format

Gameplay messages use the shared protocol envelope and the session established by the handshake. Each direction continues its exact envelope sequence after the character lobby hands off the connection.

The version 6 command payload covers movement, facing, door use, item pickup, looting, inventory transfers, and inventory-to-ground drops. Every command carries its own command sequence, player and actor identities, expected session phase, and phase revision. Results report acceptance or a specific rejection and identify the authoritative event range. Inventory mutations name the source, item, moved quantity, and pre-move stack quantity. A bounded prototype/state descriptor lets the host assign identity when a player-held item has not appeared in the shared registry yet.

When a transfer splits a stack, the host assigns a second entity ID to Fallout's newly copied remainder. The authoritative event carries the original item ID, remainder ID, counts, and host-verified descriptor. The receiving process performs the same split while binding the copy to the supplied ID. Whole-stack events use an invalid remainder ID.

A drop event additionally carries the host-selected ground tile and elevation. Ordinary multi-item requests are serialized as one-item commands because Fallout creates a separate ground object for each unit. Caps use a single quantity-bearing command and preserve their amount in the item descriptor. In both cases the guest waits for each accepted event before mutating inventory.

During live exploration, the guest sends commands rather than events. The host validates ownership, phase revision, targets, and action feasibility, executes accepted commands, and returns a result followed by the authoritative event. Host input uses the same ordered event stream. The host assigns one session-wide event sequence and appends every published event to the bounded recovery journal.

Movement events include the exact sequence of hex directions chosen by the host, so the guest follows the authoritative route instead of independently pathfinding to the final tile. Facing is also a command because rotation affects later interaction presentation and must stay in the same event order.

All integer fields are fixed-width and big-endian. Decoders reject zero identities, sequences, and transfer quantities; unknown versions or message types; invalid phases; noncanonical booleans; nonzero reserved bytes; malformed lengths; trailing bytes; and inconsistent accepted or rejected results.

The headless test suite round-trips every message type and exercises malformed input. The two-process engine smoke test carries a guest move command over TCP after the lobby, then carries the host result and movement event back to the guest.
