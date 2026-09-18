# Player character state

Phase 1 starts by giving each multiplayer slot an independent character build. `PlayerCharacterStateStore` owns those builds and binds each one to the stable `EntityId` already managed by the multiplayer ownership registry.

## State boundary

`CharacterBuild` currently owns the player-specific values held by Fallout's legacy character globals and player prototype:

- Base and bonus stats.
- Invested skill points.
- Perk ranks, tagged skills, and traits.
- Player prototype flags.
- Unspent skill points, level, and experience.

Reputation and karma stay in shared world state. Inventory and equipment already belong to critter objects and will be exposed through the local-player presentation context in a later Phase 1 change.

The store rejects a player unless its actor exists in `EntityRegistry` and is owned by the same `PlayerId`. It also prevents two player slots from sharing an actor. Actor objects can be replaced during a map load without replacing the player state: the registry rebinds the same `EntityId` to the new pointer, and the character build remains intact.

## Developer session bridge

The developer local session captures the current legacy player data after creating both actors. It copies that data into two separate `CharacterBuild` instances, one for the host and one for the guest. The copies begin identical so the existing character remains playable, but subsequent mutations to either stored build are independent.

This bridge only captures legacy state. Existing stat, skill, perk, trait, and level-up functions still read their process-wide globals. The next Phase 1 slice will add a scoped acting-player context and redirect those mechanical accessors while preserving the normal single-player path.

## Invariants

- A player state has one valid player ID and one valid actor entity ID.
- Its actor is registered and owned by that player.
- A player ID and actor entity ID each appear at most once.
- Object-pointer replacement does not alter the player build.
- Ending the session clears both the ownership registry and player state store.

Headless tests cover invalid bindings, duplicate registration, independent builds, connection changes, actor lookup, object rebinding, and local-session teardown.
