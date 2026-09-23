# Gameplay wire format

Gameplay messages use the shared protocol envelope and the session established by the handshake. Each direction continues its exact envelope sequence after the character lobby hands off the connection.

The version 10 command payload covers movement, facing, door use, item pickup, looting, inventory transfers, inventory-to-ground drops, shared modal state, and reserved combat attack messages. Every command carries its own command sequence, player and actor identities, expected session phase, and phase revision. Results report acceptance or a specific rejection and identify the authoritative event range. Inventory mutations name the source, item, moved quantity, and pre-move stack quantity. A bounded prototype/state descriptor lets the host assign identity when a player-held item has not appeared in the shared registry yet.

When a transfer splits a stack, the host assigns a second entity ID to Fallout's newly copied remainder. The authoritative event carries the original item ID, remainder ID, counts, and host-verified descriptor. The receiving process performs the same split while binding the copy to the supplied ID. Whole-stack events use an invalid remainder ID.

A drop event additionally carries the host-selected ground tile and elevation. Ordinary multi-item requests are serialized as one-item commands because Fallout creates a separate ground object for each unit. Caps use a single quantity-bearing command and preserve their amount in the item descriptor. In both cases the guest waits for each accepted event before mutating inventory.

During live exploration, both host and guest input pass through the same command processor. The host validates ownership, phase revision, targets, and action feasibility, executes accepted commands, and publishes the authoritative event. Guest commands additionally receive an acceptance or rejection result. The host assigns one session-wide event sequence and appends every published event to the bounded recovery journal.

Door events include the host-selected frame, open state, and lock state. The guest applies those values directly and never invokes the door action or script. Pickup start events are presentation cues; after the deferred host animation and pickup script finish, a second ordered event carries success, final quantity, and mutable item state. The guest applies that concrete ownership change without invoking pickup logic. Attack events remain presentation boundaries backed by authoritative state snapshots, and live attack submission is blocked until combat phase and active-turn ownership are implemented.

Movement events include the exact sequence of hex directions chosen by the host, so the guest follows the authoritative route instead of independently pathfinding to the final tile. Facing is also a command because rotation affects later interaction presentation and must stay in the same event order.

Shared modal commands identify the modal kind and whether it is opening or closing. The host changes the session phase and publishes that phase plus its new revision in the ordered event. Dialogue and barter use `Dialogue`; rest, elevators, and world-map travel use `Transition`. While either phase is active, ordinary exploration commands fail phase validation. A recovery snapshot carries the authoritative phase and revision, so a peer that missed an open event still pauses and a snapshot in `Exploration` clears stale modal state.

All integer fields are fixed-width and big-endian. Decoders reject zero identities, sequences, and transfer quantities; unknown versions or message types; invalid phases; noncanonical booleans; nonzero reserved bytes; malformed lengths; trailing bytes; and inconsistent accepted or rejected results.

The headless test suite round-trips every message type and exercises malformed input. The two-process engine smoke test carries a guest move command over TCP after the lobby, then carries the host result and movement event back to the guest.
