# Authoritative command routing

The command processor handles movement, door use, ground-item pickup, looting, loot-window inventory transfers, direct player gifts, and player item drops. The developer session uses the interaction subset in process, while the live network host validates every guest action. Its input and output contain only multiplayer value types and entity IDs.

For every command, the processor checks:

- The player, actor, and command sequence are valid.
- The command is the next sequence for that player. Repeating one of the 64 most recent commands returns its cached result without executing it again.
- The session is in the phase required by the command and the sender used the current phase revision. Ordinary actions and modal opens require exploration; modal closes require that modal's phase.
- The actor exists and belongs to the sending player.
- An interaction target exists in the entity registry.

Only then does the engine adapter run Fallout pathfinding, schedule the interaction, or apply the inventory mutation. An accepted command receives one session-wide event sequence. Rejected commands have a specific reason and emit no event. Guest inventory moves remain deferred until their authoritative event arrives, so rejection does not require an identity-sensitive rollback.

The developer session registers doors, ground items, and lootable critters when a player interacts with them. A world reset removes these unowned mappings before the engine frees map objects. Player actor mappings remain, and the guest actor keeps its `EntityId` when the next map loads.

Fallout defers pickup and loot callbacks until the acting character reaches the target. Those callbacks recover the initiating player from the actor registry before checking character rules or opening inventory. This keeps guest carry weight and item transfers separate even though the persistent presentation binding normally points at the host.

For live pickup, the initial map's ground items receive host-compatible IDs by sorting the same map objects before registry insertion. The host reserves an accepted target until the deferred callback reports success or failure, preventing two actors from scheduling the same item. An accepted event makes each peer schedule the same actor/item interaction; a failed callback releases the reservation for a later attempt.

Lootable critters and their recursive inventories are registered in the same canonical scan. Only the initiating player's process opens Fallout's modal loot window. The host requires the acting player to be adjacent to the target when loot begins and rechecks adjacency for every move, so a stale modal or forged command cannot transfer inventory at a distance. Moves are sent with source, destination, item, moved quantity, and source-stack quantity; the host also verifies that one side is the owned actor and the other belongs to the active loot target. A split gives the copied remainder a host-assigned ID, while identical-stack merges retire the destroyed representative on both peers. An unregistered player item is described by bounded prototype state and receives its first entity ID from the host.

Inventory drops use the same authority rule. Guest input stays unchanged until the host runs the drop script and publishes the dropped item ID, any split remainder ID, quantity, and final ground tile. Repeated ordinary-stack drops are serialized through successive remainder IDs; caps use one bounded bulk command so Fallout's special amount selection cannot mutate the guest ahead of the host.

The same inventory command supports a deliberately narrow player-gift path before the full trade UI exists. The source must be the acting player's direct inventory, the destination must be the other player actor, both actors must be adjacent on the same elevation, and the item must already have a shared identity and cannot be equipped or active. The receiver cannot use this path to take an item, and player actors cannot be opened as loot targets to bypass that direction rule. Ordinary stacks and caps can be split; the host assigns the remainder identity and publishes the same transfer event used by loot. The `game_give` semantic command exposes this path for co-op testing.

The [snapshot recovery format](snapshot-recovery.md) records the holder or ground position of every registered item so a recovery snapshot repairs pickup, loot, gift, and drop mutations after their journal events have expired.

## Shared modal policy

The command processor owns a single shared-modal controller. Opening dialogue or barter advances the authoritative session to `Dialogue`; opening rest, an elevator, or world-map travel advances it to `Transition`. Only the actor that opened a particular modal can close it. Each change is an ordered event carrying the resulting phase and revision. The engine main loop continues rendering, animation timing, and background network processing while suppressing local gameplay input, script-state checks, and map-state checks on both peers.

Dialogue, barter, rest, elevators, and world-map travel are currently blocked at their engine entry points during a multiplayer world. Their effects still depend on legacy local script or clock execution, so opening them before the corresponding host-authoritative command family exists would permit divergence. Local informational screens stay available, and loot remains live through its separate validated command path. Single-player behavior is unchanged.
