# Authoritative command routing

The command processor handles movement, door use, ground-item pickup, and looting. It runs in process for the developer session, but its input and output contain only multiplayer value types and entity IDs.

For every command, the processor checks:

- The player, actor, and command sequence are valid.
- The command is the next sequence for that player. Repeating one of the 64 most recent commands returns its cached result without executing it again.
- The session is in exploration and the sender used the current phase revision.
- The actor exists and belongs to the sending player.
- An interaction target exists in the entity registry.

Only then does the engine adapter run Fallout pathfinding or schedule the interaction. An accepted command receives one session-wide event sequence. The event records the action the engine scheduled, not a claim that an animation or inventory transfer has already finished. Rejected commands have a specific reason and emit no event.

The developer session registers doors, ground items, and lootable critters when a player interacts with them. A world reset removes these unowned mappings before the engine frees map objects. Player actor mappings remain, and the guest actor keeps its `EntityId` when the next map loads.

Fallout defers pickup and loot callbacks until the acting character reaches the target. Those callbacks recover the initiating player from the actor registry before checking character rules or opening inventory. This keeps guest carry weight and item transfers separate even though the persistent presentation binding normally points at the host.

This change does not encode command payloads for a network transport or replicate completed animation state. The [snapshot recovery format](snapshot-recovery.md) now detects and repairs the minimal actor and door state used by this experiment.
