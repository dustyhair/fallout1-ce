# Script-facing player-role audit

This audit covers exploration-time uses of Fallout's legacy player global and
the script operations most likely to run while a player interacts with a map.
It was made against the `multiplayer-plan` branch after the authoritative
movement, door, pickup, loot, modal, and direct-gift slices. The scan found
roughly 500 `obj_dude` references across 33 files under `src/game` and
`src/int`; a global
replacement would be incorrect because those references describe several
different roles.

## Role rules

| Role | Meaning | Required access pattern |
| --- | --- | --- |
| Story actor | The canonical protagonist exposed to Fallout scripts | Keep `obj_dude` and the `dude_obj` opcode stable |
| Acting player | The player whose accepted command is executing on the host | Pass the actor explicitly, or use `actingPlayerActorOr(obj_dude)` and the scoped character build |
| Local actor | The player presented and controlled by this process | Use `localPlayerActorOrStoryActor()` or `isPresentedPlayerActor()` for camera, HUD, messages, and hand selection |
| Shared party state | Quest/world state owned by the authoritative session | Mutate only on the host and distribute the result through events and snapshots |

`obj_dude` must never be temporarily swapped while a script runs. `dude_obj`
continues to mean the story protagonist even when a guest interaction caused
the script to execute. Scripts can still inspect their explicit `source_obj`
or another object argument when they need the acting player.

## Audited exploration paths

| Area | Classification | Result |
| --- | --- | --- |
| `scripts.cc`, `op_dude_obj`, story script attachment | Story actor | Retained. Existing content expects one stable protagonist. |
| Save/load, party slot zero, automap, endgame, map start placement | Story actor | Retained. Multiplayer metadata and the guest object are persisted separately. |
| Camera, HUD, inventory/character screens, messages, selected hand | Local actor | Presentation bridges are used by the main interfaces. This audit also routes exploration item/use feedback and script-driven HUD refreshes through `isPresentedPlayerActor()`. |
| Stats, skills, perks, traits, character build, XP/level calculations | Acting player | Existing accessors use `ScopedActingPlayerContext`. `actingPlayerActorOr(fallback)` makes the single-player/story fallback explicit. Complete builds, including prototype flags, now travel in authoritative snapshots. |
| Books | Acting player plus shared time | Skill gain and reading time use the reader's skill and Intelligence. World time and map-update scripts remain authoritative/shared; palette and text are local presentation. |
| Radios and scripted item use | Acting player | The actual user is now supplied as the script source instead of always using `obj_dude`. |
| Explosives and traps | Acting player | The Traps roll uses the player setting the explosive. The resulting timer queue remains shared host state. |
| Scenery, containers, item-on-object, skill-on-object | Acting player plus local presentation | Explicit source actors already reach scripts and skill checks. Player-only behavior now recognizes a scoped guest actor; feedback is limited to the presented actor. |
| Initial NPC reaction and dialogue IQ options | Acting player plus shared NPC state | Charisma, Presence, personality, Intelligence, and Smooth Talker resolve from the scoped talker. The resulting NPC/script local variables remain authoritative shared state. |
| `set_critter_stat` and `critter_mod_skill` opcodes | Acting player | An explicitly passed scoped guest actor can now mutate its own character build. An arbitrary NPC still cannot use player-only character storage. |
| Script-driven wield, inventory removal/move, healing, and injury UI | Acting player plus local presentation | Player-specific armor/stat work recognizes the scoped guest; HUD refresh and hand selection use the local actor. |
| Quest, major world-event, and combat XP | Shared award applied per acting player | The host awards the same base XP to both builds inside separate acting-player scopes, so personal perks and level-up effects are calculated independently. Guest replicas suppress local awards and receive both builds from snapshots. Skill-use and stealing XP remain personal. |
| Global variables, map variables, script local variables, world clock, random rolls, timed queues | Shared party state | Guest replicas do not execute scripts or timed queue work. Snapshots/checkpoints carry these authoritative sections. |
| Movement, doors, pickup, loot, and direct gifts | Acting player plus shared world | Commands carry an owned actor, execute in its scoped context on the host, and publish ordered results. Existing installed-data scenarios cover authority and convergence. |

## Findings that remain after this audit

These are explicit follow-up work, not reasons to overload `obj_dude`:

- Persistent PC flags are stored per build and now replicate with it, but
  Sneak's live success result and periodic queue behavior still use legacy
  process-wide state. Independent multiplayer Sneak activation needs an
  actor-owned runtime state and semantic command.
- Shared dialogue now executes its option procedure only on the host in the
  scoped talker's context. Guests render replicated reply/options and ballots;
  the default majority tie-break uses each eligible player's scoped Charisma
  and Intelligence, with host RNG only for an exact stat tie. The remaining
  durable save/load boundary belongs to Phase 6.
- Shared cross-map elevator and ordinary-exit boundaries preserve
  `obj_dude` as the local story actor and recreate and rebind the remote actor.
  Typed-stair cross-map travel uses that bridge but still needs a dedicated
  fixture. Same-map ladder scripts redirect legacy protagonist movement and
  rotation to the scoped acting player while preserving every other player's
  placement. World-map travel remains Phase 3B work and must use the
  coordinated seam.
- Combat-only animation, action-point, and equipment assumptions remain part
  of Phase 4 unless they are also reached by an exploration interaction.

## Review checklist for new code

Before adding another `obj_dude` comparison, decide which sentence is true:

1. "The original game story must always see the canonical protagonist." Use
   `obj_dude`.
2. "This rule belongs to the accepted command's player." Pass the actor or use
   the acting-player context.
3. "Only this process should show or control it." Use the local presentation
   bridge.
4. "Both players share this quest or world result." Mutate it on the host and
   replicate the authoritative result.

If none applies, the state likely needs a new explicit owner rather than
another player-global exception.
