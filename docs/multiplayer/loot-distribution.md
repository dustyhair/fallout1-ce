# Multiplayer loot distribution

Ordinary exploration pickups still belong to the picker. Loot transferred out
of a dead critter uses a deterministic roster cursor for contested items: each
successful transfer advances priority to the next eligible player. The host
selects the recipient and publishes the effective destination, so clients do
not independently decide a winner.

Caps removed from a live loot target are never awarded as an ordinary item
transfer. The host splits the requested amount evenly over the canonical
player roster. Remainder caps start at `nextCapExtraIndex`, and that cursor
advances by the number of remainders, alternating the benefit over time. One
bounded `CapsDistributedEvent` records the conserved total and the
player/actor-keyed shares; an immediate authoritative checkpoint applies all
inventory mutations together on replicas.

Both cursors are stored in the version 4 multiplayer sidecar. Roster order is
strictly increasing `PlayerId`, independent of connection or registration
order. Headless tests cover two- and three-player splits, eligible-player
filtering, cursor restoration, malformed rosters, and currency-conservation
checks on the wire.
