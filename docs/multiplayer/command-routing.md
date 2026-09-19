# Authoritative command routing

The command processor handles movement, door use, ground-item pickup, looting, and loot-window inventory transfers. The developer session uses the interaction subset in process, while the live network host validates every guest action. Its input and output contain only multiplayer value types and entity IDs.

For every command, the processor checks:

- The player, actor, and command sequence are valid.
- The command is the next sequence for that player. Repeating one of the 64 most recent commands returns its cached result without executing it again.
- The session is in exploration and the sender used the current phase revision.
- The actor exists and belongs to the sending player.
- An interaction target exists in the entity registry.

Only then does the engine adapter run Fallout pathfinding, schedule the interaction, or apply the inventory mutation. An accepted command receives one session-wide event sequence. Rejected commands have a specific reason and emit no event. Guest inventory moves remain deferred until their authoritative event arrives, so rejection does not require an identity-sensitive rollback.

The developer session registers doors, ground items, and lootable critters when a player interacts with them. A world reset removes these unowned mappings before the engine frees map objects. Player actor mappings remain, and the guest actor keeps its `EntityId` when the next map loads.

Fallout defers pickup and loot callbacks until the acting character reaches the target. Those callbacks recover the initiating player from the actor registry before checking character rules or opening inventory. This keeps guest carry weight and item transfers separate even though the persistent presentation binding normally points at the host.

For live pickup, the initial map's ground items receive host-compatible IDs by sorting the same map objects before registry insertion. The host reserves an accepted target until the deferred callback reports success or failure, preventing two actors from scheduling the same item. An accepted event makes each peer schedule the same actor/item interaction; a failed callback releases the reservation for a later attempt.

Lootable critters and their recursive inventories are registered in the same canonical scan. Only the initiating player's process opens Fallout's modal loot window. Moves are sent with source, destination, item, moved quantity, and source-stack quantity; the host verifies that one side is the owned actor and the other belongs to the active loot target. A split gives the copied remainder a host-assigned ID, while identical-stack merges retire the destroyed representative on both peers. An unregistered player item is described by bounded prototype state and receives its first entity ID from the host.

The [snapshot recovery format](snapshot-recovery.md) records the holder or ground position of every registered item so a recovery snapshot repairs pickup and loot mutations after their journal events have expired.
