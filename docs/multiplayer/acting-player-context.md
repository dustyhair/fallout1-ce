# Acting-player context

Fallout's character rules often ask global functions such as `perk_level` or `stat_pc_get` for the current player's data. Those functions have no actor parameter. A multiplayer command therefore installs a scoped acting-player context after the session accepts its player and actor ownership.

`CommandProcessor` creates the scope immediately around `CommandExecutor`. The scope exposes the accepted command's `PlayerCharacterState`, actor object, and `CharacterBuild`. Its destructor restores the previous context, so nested engine operations are safe. Validation failures do not install a context, and the context is gone before the authoritative result leaves the processor.

## Routed state

While a context is active, the legacy character APIs use its build for:

- Base stats, bonus stats, derived-stat recalculation, and stat changes on the acting actor.
- Skill investments, tagged skills, skill increases, and skill decreases.
- Trait selection and trait modifiers.
- Perk ranks, perk selection, and perk effects.
- Unspent skill points, level, experience, and level-up hit points.
- Player prototype flags accessed through the `pc_flag` functions.

Reputation and karma still use the shared `curr_pc_stat` entries. Calls made without a context still use the original prototype and process-wide arrays, which keeps single-player behavior unchanged.

The actor check matters. An active build can replace prototype stats only when the requested object is the acting actor. NPC and world-object lookups continue through their normal prototypes. Functions without an actor parameter, such as `perk_level`, use the active build because the command scope supplies their missing identity.

## Limits of this slice

The context covers synchronous command execution. Work deferred until after the executor returns must carry player identity and install its own scope. Movement animation callbacks do not need character globals today, but later combat and queued skill work will need that audit.

Save and load handlers still read and write the legacy single-player arrays. Phase 1 will add a multiplayer sidecar after local presentation and character creation can select distinct builds. Guest sneak scheduling also remains separate because the old sneak queue has one process-wide result flag.

Headless tests cover empty, nested, restored, host-command, and guest-command contexts. The full executable build checks the stat, skill, trait, perk, progression, and player-flag integrations.
