# Two-player co-op plan

Status: Phases 0, 1, 2, 2.25, 2.5, 3A, 3B, 4A, 4B, 5, 6, and 7 are complete on the `multiplayer-plan` branch. Transport, lobby, journal, snapshot recovery, authenticated reconnect, content manifest, first-contact fingerprint verification, bilateral trade, deterministic loot distribution, hidden host recovery saves, protocol diagnostics, deterministic transport-fault tests, the optional dependency-free single-player build, and the installed-data compatibility campaign are implemented. Same-map exploration converges through the host command processor and authoritative checkpoints; replicas do not rerun rule-bearing scripts, rolls, or timed queues. Installed-data two-process scenarios cover movement, doors, pickup, loot, inventory gifts, skills, item-on-target quest completion, scenery and container state, XP, independent elevator, ladder, and typed-stair travel, shared cross-map elevators, exits, and typed stairs, agreed rest, world-map travel into towns, terrain, and encounters, combat actions, effects, statuses, script-queued entry, and reconnect with complete authoritative checkpoints, plus host- and guest-led branching dialogue, split votes, attributed quest activity, talker skill checks, a script-requested combat transition, and durable recovery. Phase 6 headless coverage additionally verifies transactional trade revisions and conservation, roster-based cap/loot cursors, sidecar v1-v4 migration, saved-slot claims, durable shared activity, and cross-map recovery snapshot identity. Phase 7's 13-scenario persistent campaign and headless preflight pass with matching recovery digests after exposing and fixing mutable-string handling in interrupted-rest presentation.

The [2026-09-25 rebase audit](docs/multiplayer/rebase-regression-2026-09-25.md)
records regression fixes, the expanded automated matrix, and the remaining
manual/platform validation limits.

## Goal

Add two-player network co-op without changing Fallout's quests into separate per-player campaigns. Both players inhabit one authoritative world, create distinct characters, take turns in combat, vote on dialogue, and trade items.

The first useful release should support:

- A host and one guest over LAN or a direct IP connection.
- One shared map, world clock, quest state, and save.
- A separate character sheet, inventory, equipment, SPECIAL stats, skills, traits, and perks for each player.
- Real-time exploration on the same map.
- Sequential combat using Fallout's existing initiative order.
- Shared dialogue with configurable voting.
- Host-validated trading and a simple loot policy.
- Reconnection to the current session.

This is a new game mode. Existing single-player saves and behavior must continue to work.

## Recommended architecture

Use a host-authoritative simulation. The host runs maps, scripts, combat, random rolls, quests, inventory mutations, and saving. The guest sends commands and receives snapshots, authoritative state changes, and presentation cues.

Do not try deterministic lockstep. The engine has mutable global state, script side effects, timing-dependent input loops, and random calls spread throughout the code. Keeping two independent simulations identical would be harder than making the host authoritative.

```text
host input ─┐
            ├─> validated commands ─> host simulation ─> ordered world events
guest input ┘                                  │                    │
                                               └─> save             ├─> host view
                                                                    └─> guest view
```

Both processes keep enough world state to render the current map. Only the host advances gameplay state. The guest applies values chosen by the host and must not rerun scripts, rolls, damage calculations, or inventory rules. Presentation cues can start an animation or sound, but they are never the only record of a gameplay change.

Clients own presentation state such as camera position, open panels, cursor, audio, and HUD selection. The host owns gameplay state. Every guest command produces one of two outcomes:

- A rejection tied to the command sequence number, with no gameplay mutation.
- One ordered authoritative result containing the changed gameplay fields and any presentation cues.

Periodic snapshots repair missed or incorrectly applied state. They are also the basis for joining and reconnecting.

### Session limits for the first release

- Exactly two players.
- Both player characters stay on the same loaded map, but may occupy different elevations on that map.
- No public matchmaking, relay service, or account system.
- No host migration. If the host leaves, the session ends after writing a recovery save.
- No PvP.
- No simultaneous combat actions.
- Both peers must use the same executable protocol version and the same game-data manifest.

These limits remove several hard synchronization problems without weakening the core co-op experience.

### Future player-count expansion guardrails

The first release remains exactly two players. Do not expand the live socket,
lobby, or UI scope while the authoritative gameplay path is still being built.
However, unfinished systems should avoid adding new assumptions that make a
later three-or-more-player refactor harder than necessary:

- Model participants internally as a bounded, deterministically ordered roster
  keyed by `PlayerId`. A two-entry roster is still the only accepted runtime
  configuration until the later expansion milestone.
- Key ownership, command sequencing, reconnect state, readiness, votes, timeout
  policy, and snapshot progress by `PlayerId`, not by a new host/guest boolean or
  a second pair of named fields. Host authority and story-actor status remain
  explicit roles rather than implied array positions.
- Represent effects that can concern every player as bounded collections or as
  one ordered per-player effect. Sort encoded collections by `PlayerId`, declare
  a maximum count in the protocol version, and reject duplicates and unknown
  participants. Never add an unbounded wire collection.
- Treat `localPlayerActor()` as one actor plus a collection of remote actors.
  Preserve stable player and actor identity when any remote actor is recreated;
  do not generalize by introducing `peerActor2`, `peerActor3`, and similar
  globals.
- Define readiness against an immutable participant set captured at the start
  of an operation. Cross-map travel and shared time advances require every
  connected participant in that set; same-map elevators may move only the
  nearby subset. Disconnect and timeout policies must say whether a participant
  is removed, treated as not ready, or handled by host takeover.
- Keep bilateral operations, such as a direct trade, explicitly keyed by two
  participant IDs while allowing other roster members to exist. Party-wide
  policies, such as XP, cap distribution, loot priority, and dialogue voting,
  iterate the eligible roster rather than naming the guest.
- New headless controllers that aggregate player state must include a synthetic
  three-player test even though end-to-end engine and socket scenarios remain
  two-process until the expansion milestone.

Existing two-player shapes are accepted implementation debt, not patterns to
copy. The known conversion points are `LocalSession`'s host/guest actor and
transport members, the single `peerActor` world bridge, the two-slot character
lobby and native lobby UI, host/guest placement fields in elevator events, the
two-player save sidecar, and two-process runtime test orchestration. Keep this
list current when a remaining phase discovers another pair-shaped boundary.

### Session state and safe points

Make session phase explicit:

| Current phase | Allowed next phases |
| --- | --- |
| Lobby | Loading, ending |
| Loading | Exploration, ending |
| Exploration | Combat, dialogue, transition, ending |
| Combat | Exploration, ending |
| Dialogue | Exploration, combat, ending |
| Transition | Exploration, ending |

Only the session controller changes phase. A command includes the phase and phase revision it expects, so a delayed exploration command cannot execute during combat or after a map transition.

`main_game_loop` currently reads input, calls `game_handle_input`, then runs scripts and map-state checks. Combat and dialogue have separate blocking input loops. Transport polling can run from the GNW background-process mechanism, but it may only place decoded messages into a queue. Apply commands and state changes at explicit safe points in the main, combat, and dialogue loops. This avoids running scripts or changing objects reentrantly from a background callback.

## Player model

The engine assumes one player through `obj_dude` and several global character systems. A repository scan finds `obj_dude` use across 36 files under `src/game`, plus process-wide character data in `pc_proto`, `perk_lev`, `pc_trait`, `tag_skill`, and `curr_pc_stat`. Replacing every assumption at once would be risky. Add an explicit multiplayer layer first:

```cpp
struct PlayerCharacterState {
    PlayerId id;
    EntityId actorId;
    std::string name;
    CharacterBuild build;
    PlayerOwnership ownership;
    ConnectionState connection;
};
```

`CharacterBuild` owns the data that currently behaves as process-wide player data, including the player prototype's base stats and skills, traits, perks, tagged skills, level, experience, skill points, and any other character-creation fields not stored on the critter object. Reputation and karma remain shared unless a script audit proves that a value is purely personal.

The first Phase 1 slices implement this model and its ownership checks as described in the [player character state notes](docs/multiplayer/player-character-state.md). The developer local session seeds separate host and guest build copies from the current legacy character. The [acting-player context](docs/multiplayer/acting-player-context.md) routes mechanical accessors during validated command execution without changing `obj_dude`, while the [local-player presentation context](docs/multiplayer/local-player-presentation.md) selects the actor shown by this process.

Keep the host character as the canonical story actor in `obj_dude` during the first implementation. Represent the guest as a player-owned critter, not as an AI party member. Three actor concepts must not be conflated:

| Actor | Meaning | Main consumers |
| --- | --- | --- |
| Story actor | The host character exposed to scripts as `dude_obj` | Quest scripts and compatibility code |
| Local actor | The character controlled by this process | Camera, HUD, inventory, character screen, and local input |
| Acting actor | The character whose accepted command is executing on the host | Skill checks, traits, perks, combat, and interactions |

Add a scoped acting-player context for operations that need the acting character:

- HUD and inventory display.
- Skill and stat checks.
- Perk and trait lookup.
- Dialogue checks made by the talker.
- Experience and level-up handling.

Do not switch the global `obj_dude` pointer during script execution. Fallout scripts treat `dude_obj` as the story protagonist, and changing it globally can corrupt quest logic. Refactor presentation code toward `localPlayerActor()` and mechanical code toward an explicit actor or the scoped acting-player context. Calls that intentionally mean the story protagonist can continue to use `obj_dude`.

The guest critter can reuse selected party-member persistence code, but it must not enter `combat_ai` or inherit party AI behavior. Ownership, control, and persistence are separate concerns.

Character creation happens in the lobby. Each peer creates a character locally, then sends a versioned character sheet to the host. The first sheet format carries a name, SPECIAL allocation, age, gender, three tagged skills, and up to two traits. It cannot carry perks, invested skill points, experience, or flags. The host validates the choices and constructs a fresh level-one build. See the [temporary character lobby notes](docs/multiplayer/character-lobby.md).

## Shared world and quests

The host is the only machine allowed to execute world-changing scripts. Global variables, map variables, queued events, reputation, quest outcomes, and the world clock remain shared.

The guest receives results, not script execution requests. For example, opening a locked door is sent as an intent. The host performs the skill roll, runs the script, changes the door, and broadcasts the result.

Recommended rules:

- Quest completion applies to the party.
- Quest XP is awarded to both player characters.
- Combat XP is shared between both characters to prevent one player from falling behind.
- Personal damage, conditions, inventory, perks, and level choices remain separate.
- Map transitions require both players to be ready. The host can force the transition after a configurable timeout.
- The world pauses for modal screens that can mutate shared state, including dialogue, barter, rest, and world-map travel.

## Exploration

Outside combat, each peer sends movement and interaction commands for its owned actor. The host routes those commands through the same action and pathfinding functions used by local input.

The guest does not send coordinates as truth. It sends destinations, target object IDs, or interaction commands. The host rejects impossible actions and broadcasts the resulting movement or animation.

For the first version:

- Keep both characters within a generous distance on the same map.
- Give each client a local camera that follows its own character.
- Let either player use a same-map elevator independently; automatically carry the other player only when both are close to that elevator. Require both players at shared-map exits and world-map transitions.
- Pause both players when either opens a modal interface that changes shared state.
- Let harmless local panels, such as the character sheet, remain client-side when practical.

Object IDs must remain stable for the session. The [object identity audit](docs/multiplayer/object-identity.md) found that the existing `Object::id` is serialized but not stable enough for network identity. Party persistence, script attachment, player loading, and item stacking can replace it.

Use a separate host-assigned `EntityId` in the multiplayer registry. The registry maps it to the current local object and supports explicit rebinding across load boundaries. Never expose pointers or rely on load-order addresses.

## Combat

Use Fallout's existing sequential turn structure in `src/game/combat.cc`. It already tracks the active critter in `combat_turn_obj`. The main change is ownership:

- When the active critter belongs to the host, read host commands.
- When it belongs to the guest, wait for guest commands and show a waiting state on the host.
- NPC and unowned party turns continue through `combat_ai` on the host.
- The host performs hit rolls, damage, critical effects, ammo use, animation scheduling, and death checks.

Send semantic commands such as move, attack, use item, reload, change stance, and end turn. Do not replicate raw mouse or keyboard events.

Add a turn timeout with three policies: wait, auto-end, or let the host take control. A disconnected player's character should defend or pass until reconnection.

Simultaneous turns should wait until sequential combat is stable. They would need conflict resolution for movement, line of fire, animation timing, action points, interrupts, and scripts. That is a separate design, not an MVP option toggle.

## Dialogue and roleplay

The player who starts a conversation is the talker. Dialogue skill and SPECIAL checks use that character unless a script explicitly checks the canonical protagonist.

Both clients receive the current reply and option list. Each client can cast or change one vote until the choice resolves.

Support these session policies:

- Majority vote, with the host breaking a tie.
- Talker decides after seeing the other player's vote.
- Host decides.

With two players, majority vote often ties. The recommended default is talker decides. It preserves the useful distinction between a talker and a fighter without letting either player silently choose before the other has read the options.

The host executes the selected option procedure exactly once, then broadcasts the resulting dialogue state and world changes. Both clients remain in the dialogue screen until it closes.

## Loot and trading

Each character owns a separate inventory and equipment set. All item movement is a host-side transaction.

The first loot policy should stay simple:

- The picker receives ordinary items.
- Caps are split evenly, with the extra cap alternating between players.
- The session tracks alternating priority for contested post-combat items.
- A direct trade window lets both players offer items and caps. The trade commits only after both confirm the same revision.
- Either player can cancel before the commit without moving anything.

Later versions can add need, greed, and pass rolls. Do not block the first playable build on a large loot-distribution interface.

## Network protocol

Hide transport behind a small interface so LAN development does not commit the project to a matchmaking provider. Start with an in-memory loopback transport for tests. Choose the socket transport in Phase 2 after checking desktop platform support, licensing, IPv6, connection recovery, and packet-size behavior.

The protocol needs these message families:

- Handshake: protocol version, build ID, content hashes, player name, and reconnect token.
- Lobby: character sheet, ready state, session rules, and selected save.
- Commands: sequence number, player ID, owned actor ID, command kind, and typed arguments.
- Events: authoritative sequence number, object changes, animation, text, sound cue, roll result, and command rejection.
- Dialogue: speaker, reply, options, votes, deadline, and selected option.
- Combat: initiative list, active actor, action points, accepted action, and end-turn reason.
- Inventory: container revisions, proposed trade, confirmations, and committed transfers.
- Snapshot: complete join or recovery state followed by the event sequence at which it was captured.

Use fixed-width integer types, explicit byte order, length limits, protocol versioning, and bounds checks. Never transmit native structs or pointers. Put a small envelope around every message with the protocol version, message kind, payload length, session ID, and sequence number.

Protocol invariants:

- Command sequence numbers are per peer. Duplicate commands return the prior result and do not execute twice.
- Authoritative event sequence numbers are session-wide and strictly increasing.
- Each result names the command that caused it when there is one.
- The guest applies events only in order. A gap starts recovery instead of guessing.
- Snapshots carry the last included event sequence, a byte length, and a checksum.
- The host retains a bounded event journal. A short gap replays from the journal; an older gap requires a fresh snapshot.
- Commands and events declare maximum encoded sizes. Decoders reject unknown required fields, invalid enum values, oversized collections, and references to missing entities.
- A reconnect token authenticates one player slot for one session. Starting a new session invalidates it.

Keep protocol data independent of the transport. Reliability, congestion handling, and encryption belong to the selected transport or its session wrapper, not to gameplay command classes.

Direct-IP sessions must use authenticated encryption. Do not design custom cryptography. If the chosen transport cannot protect commands and reconnect tokens, limit that build to trusted LAN play instead of sending them in plaintext.

## Save, load, and reconnect

The host owns the save. Keep the original `SAVE.DAT` readable and store multiplayer data in a versioned `MULTI.DAT` sidecar inside the save slot. The sidecar should contain:

- Session format version and rules.
- Both player IDs and character builds.
- Network object ownership.
- Guest inventory and equipment if the base save cannot represent them safely.
- Loot priority state.
- Reconnect tokens without reusable account credentials.
- The expected `SAVE.DAT` digest and a shared save generation number.

`src/game/loadsave.cc` already runs an ordered table of 27 save and load handlers. Add the multiplayer sidecar around that flow instead of inserting fields into the original binary stream. Write both files through temporary files and publish the sidecar only after `SAVE.DAT` succeeds. A digest or generation mismatch marks the co-op metadata unusable instead of mixing two saves. A single-player load ignores the sidecar. A multiplayer load restores the host world first, then applies player ownership and waits in the lobby for the guest.

For joining or reconnecting, serialize a dedicated network snapshot on the host and send it in bounded chunks with a checksum. Do not send `SAVE.DAT` as the network snapshot. Its load path has UI, filesystem, party, and map-transition side effects that are unsafe during a live session. The snapshot serializer can reuse low-level object serialization only after tests show that it has no such side effects.

Queue newer events while the snapshot is in flight, then apply them in sequence after the snapshot loads. Set a limit on queued bytes and restart snapshot transfer if the guest falls farther behind than that limit.

## Confirmed engine seams

These are the first integration points found in the current tree:

| Area | Current behavior | Planned seam |
| --- | --- | --- |
| Main loop | `main_game_loop` calls `get_input`, `game_handle_input`, script checks, and map checks in order | Drain commands before gameplay input and publish results after state checks |
| Combat | `combat_turn` sends `obj_dude` to `combat_input` and every other critter to `combat_ai` | Select local, remote, or AI control from actor ownership |
| Dialogue | `gDialogProcess` owns a blocking input loop and calls the chosen option directly | Poll votes in the loop, let the host resolve once, then execute one option procedure |
| Character data | Player prototype, perks, traits, tagged skills, and PC stats live in process-wide arrays | Move mutable values behind `PlayerCharacterState` accessors |
| Object identity | `Object::id` is serialized, but allocation and party persistence can change IDs | Audit lifecycle and introduce the session identity registry |
| Save/load | `master_save_list` and `master_load_list` run 27 ordered handlers | Wrap the existing save with a checked multiplayer sidecar |

The first code change should preserve the original path when no multiplayer session exists. Avoid scattering `if (multiplayer)` checks through rules code. Prefer accessors and controllers whose single-player implementation returns the current globals and `obj_dude`.

## Source areas likely to change

- `src/game/main.cc`: pump network traffic and authoritative commands in the main game loop.
- `src/game/game.cc`: separate local input handling from gameplay commands and protect shared state changes.
- `src/game/object.cc`, `object_types.h`, and `scripts.cc`: audit existing object IDs and add session identity only if required.
- `src/game/proto.cc`, `stat.cc`, `skill.cc`, `perk.cc`, and `trait.cc`: per-player character context.
- `src/game/combat.cc` and `combatai.cc`: player ownership of turns, remote commands, timeouts, and disconnect handling.
- `src/game/gdialog.cc` and `src/int/support/intextra.cc`: replicated dialogue state, voting, talker context, and host-only script execution.
- `src/game/inventry.cc`, `item.cc`, and `party.cc`: separate inventories, validated transfers, and loot rules.
- `src/game/map.cc`, `worldmap.cc`, and `scripts.cc`: shared transitions, time, events, and quest state.
- `src/game/loadsave.cc`: multiplayer sidecar lifecycle and recovery saves.
- `src/plib/gnw/input.cc`: poll transport into a queue, keep device input local, and translate actions into gameplay commands above this layer.
- `CMakeLists.txt`: optional networking dependency and a build flag such as `FALLOUT_ENABLE_MULTIPLAYER`.

New code should live in a focused `src/multiplayer` directory rather than spreading socket calls through game code. Suggested modules are session, transport, protocol, command validation, replica application, snapshot, ownership, and lobby. Game code should depend on session-facing interfaces, never on sockets.

## Delivery phases

### Phase 0: prove the shape

- Add a small CTest target for multiplayer core code. The repository has no C++ test target today, so keep this executable independent of windows, audio, and game data.
- Define `PlayerId`, `EntityId`, session phase, actor ownership, `GameCommand`, `CommandResult`, and the protocol envelope.
- Add an in-memory loopback transport and protocol encoder with no sockets.
- Audit existing object IDs, then implement the ownership and identity registry without changing the original save format.
- Spawn a second controllable critter in a developer-only local session.
- Convert one movement action and one door interaction into semantic commands.
- Reject a command for an actor the sender does not own.
- Capture and restore a small snapshot containing both actors and the door.
- Compare a sectioned state digest before and after snapshot restore.
- Run the existing single-player path with multiplayer disabled and confirm that input, save, and load still behave as before.

Exit condition: two local command producers control separate actors in one authoritative process, an unauthorized command has no effect, and snapshot restore reproduces the same state digest.

### Phase 1: separate player state

- Add `PlayerCharacterState` backed by the Phase 0 ownership registry.
- Move the player prototype, traits, perks, tagged skills, PC stats, and level-up state behind a player context.
- Make the HUD, character sheet, inventory, and equipment target the local character.
- Add two-character creation and validation in a temporary lobby.
- Preserve the host as the story actor while tests execute mechanical checks for either acting actor.

Exit condition: both characters retain distinct builds through save and load.

### Phase 2: host and guest session

- Add LAN and direct-IP host and join flows.
- Implement handshake, content checks, command sequencing, event journaling, snapshots, and reconnect tokens.
- Add guest camera and local presentation state.
- Reject stale, duplicate, unauthorized, and malformed commands.

Exit condition: a guest can join, move, interact, disconnect, and reconnect on one map.

Direct-IP TLS displays an ephemeral host-certificate fingerprint and accepts it as an explicit guest launch option. When players compare that value over a trusted channel, the first connection is authenticated before application data is released. Omitting it deliberately falls back to trusted-LAN/TOFU behavior. A join code or account layer remains desirable for usability.

### Phase 2.25: authority convergence

Complete this milestone before expanding same-map gameplay or enabling combat:

- Route host and guest actions through the same `GameCommand` validation and execution path.
- Keep semantic presentation cues separate from authoritative effects. A guest may animate a host-selected path, but it must not rerun scripts, random rolls, damage calculation, or rule-bearing inventory actions.
- Include the concrete resulting state in ordered effects, or follow the cue with a sectioned authoritative state boundary that covers every mutation it can cause.
- Replicate session phase and phase revision authoritatively. Reject commands for the wrong phase before reaching engine code.
- Make recovery and periodic correction cover actors, critters, doors, items, player metadata, globals, map variables, queued events, and phase-specific state as each section becomes mutable.
- Add an engine scenario proving a scripted door and a random combat action execute exactly once on the host.
- Fail closed when an authoritative effect or snapshot cannot be applied; never continue from a presentation-only approximation.

Current implementation status:

1. [x] Route live host and guest exploration input through the authoritative command processor.
2. [x] Apply door state on the guest without invoking the door action or script again.
3. [x] Stop guest pickup and attack events from invoking gameplay mutation functions.
4. [x] Expand periodic authoritative correction to actors, critters, doors, and items.
5. [x] Carry authoritative phase revisions in snapshots and apply them on the guest.
6. [x] Replace snapshot-dependent pickup completion with an explicit authoritative completion effect.
7. [x] Add globals, indexed map/script variables, world time, and typed timed queues to sectioned snapshots/digests; suppress guest interpreter and queue execution after world entry.
8. [x] Add an installed-data two-process engine scenario proving a scripted door procedure and random combat attack execute once on the host and zero times on the guest effect paths.

Exit condition: host and guest input share one validation path, the guest executes no rule-bearing mutation or RNG from a replicated event, and sectioned checkpoints show no unexplained divergence.

### Phase 2.5: agent-driven multiplayer testing

The existing coordinate/key command file is a UI smoke-test adapter, not the semantic multiplayer agent interface. Keep it for otherwise-unreported modal screens, but do not count raw input injection toward this phase's exit condition.

- Extend the agent journal with a developer-only local command interface that accepts semantic gameplay commands rather than raw mouse or keyboard input.
- Route agent commands through the same `GameCommand`, ownership, phase, range, AP, and action validation used by a human multiplayer peer. The agent must not receive privileged mutation paths.
- Cover movement, facing, interaction, pickup, inventory actions, and dialogue/combat commands as those command families become available.
- Add deterministic scripted scenarios that can drive the second player without requiring two human testers.
- Keep screenshots and raw UI input as optional presentation checks; machine-readable state and authoritative events are the primary interface.

Optional Laya or LLM controllers sit above this semantic interface. They receive a compact decision state and return constrained semantic actions; they never mutate Fallout state directly. Deterministic scenarios and human multiplayer must work with the entire model stack disabled. Record routing tier, escalation reason, approximate token use, latency, selected action, validation result, and configured call/token budgets when a model controller is enabled.

Current implementation status:

1. [x] Add semantic movement, facing, door, pickup, and loot command-file verbs.
2. [x] Expose visible critters, doors, non-door scenery, containers, ground items, player actor IDs, and registered local inventory items with stable entity IDs in the machine-readable world state.
3. [x] Route semantic verbs through the same multiplayer runtime handlers and command processor used by human input.
4. [x] Add semantic direct-inventory gift, item-on-target, elevator, ordinary-exit, and scenery-transition verbs using the same authoritative paths as human input; dialogue and combat verbs remain pending their authoritative controllers.
5. [x] Add deterministic two-process exploration scenarios and structured completion assertions. Installed-data fixtures cover guest movement, scripted-door use, deferred pickup completion, loot initiation, a split cap gift, targeted skills, item-on-target quest completion, non-door scenery state, container state, independent elevator and ladder travel, cross-map elevators, and ordinary exits, with full section-digest convergence and authenticated event replay.
6. [ ] Add an optional Laya or LLM controller above the deterministic semantic
   interface. Its decision state names the controlled `PlayerId` and exposes a
   bounded player roster instead of host/guest-only observation fields.

Exit condition: an automated local client can observe the journal, control one player through validated semantic commands, and complete the supported exploration command set without bypassing multiplayer authority.

### Phase 3A: same-map exploration

- Replicate movement, doors, containers, item pickup, skill use, traps, and map-script results.
- [x] Require the acting player to remain adjacent to the active loot target when starting loot and when applying every inventory transfer; the deterministic loot scenario also proves a remote command is rejected without publishing an event.
- [x] Define and enforce modal-screen behavior. Dialogue and barter use the `Dialogue` phase; rest, elevators, and world-map travel use `Transition`. The peer main loop keeps rendering and pumping the network while shared-world input, scripts, and map processing are paused. Unsupported entry points fail closed until their effects have host-authoritative command families; fixed-duration rest now has one. Informational Pip-Boy, inventory, character, and options screens remain local; loot keeps its validated live path.
- [x] Add a minimal host-authoritative player-to-player item/caps gift command for practical co-op testing. It permits only direct owned inventory to move to an adjacent peer, rejects remote transfers and attempts to take, and exposes registered inventory IDs through the agent journal; the full transactional trade UI remains Phase 6.
- [x] Audit script-facing player assumptions used during exploration and classify each as story actor, acting player, local presentation, or shared party state. The [player-role audit](docs/multiplayer/player-role-audit.md) records the retained story compatibility seams, fixes acting-player item/script checks, and isolates Sneak's remaining process-wide runtime state.
- [x] Award quest, major world-event, and combat XP to both player builds on the host. Each character applies its own Swift Learner, level, Lifegiver, HP, and level-up-flag progression; guest replicas ignore local awards and receive both complete builds through versioned authoritative snapshots.
- [x] Add host-authoritative targeted First Aid, Doctor, Lockpick, Steal, Traps, Science, and Repair commands for registered actors, critters, ground items, doors, and non-door scenery. Human and `game_skill` agent input share ownership/phase/target validation and the acting-player build context; replicas apply only a presentation boundary, then receive script, roll, XP, object, and variable results through authoritative state. The installed-data Traps and scenery scenarios prove command/result/event transport, full checkpoint convergence, and replay without guest-side rule execution.
- [x] Canonically register all initial non-door scenery and replicate its position, art/frame, shared flags, light, and concrete union state. Registered items, including containers, also replicate their art/frame, shared flags, and light in addition to ownership, quantity, and item data. Installed-data scenery and container scenarios force a host-only shared-state mutation and prove snapshot repair on the guest.
- [x] Add authoritative item-on-target commands for directly owned registered items. The Jarvis fixture on `SHADYW.MAP` gives the guest an antidote and completes the real cure script on the host, including local/global variables, timed-event removal, item destruction, reputation, and 400 party XP. The guest executes no script or random rule work, removes the consumed item from the authoritative snapshot, matches the full state digest, and replays the ordered event after authenticated reconnect.

Phase 3A's quest exit condition is complete. General script-created object identity remains a later hardening task: destruction of registered items is now observed at the engine erase boundary, while creation outside the existing authoritative inventory and snapshot paths still fails closed.

Exit condition: two players can complete a small non-combat quest together on one map with synchronized objects, inventory, scripts, and player-specific skill checks.

### Phase 3B: transitions and world travel

- [x] Add the first synchronized elevation boundary: a semantic elevator command identifies Fallout's installed elevator table and desired level; the host requires the acting player within four hexes of that elevator's source and automatically carries the other player on a same-map ride only when that player is also within four hexes on the source elevation. It advances through `Transition` and publishes each player's exact independent tile, elevation, rotation, and the phase revision. The Vault 13 two-process scenario leaves the host downstairs while the guest travels upstairs and proves local-floor presentation, full snapshot convergence, and authenticated replay.
- [x] Add synchronized cross-map elevator transitions. Both players must be within four hexes of the source. The host enters `Transition`, loads the destination, preserves the local actor, recreates the remote actor, rebinds the same player/entity identity, restores its critter and inventory state, canonically registers the destination, and publishes both exact placements. The `BROHD12.MAP` to `BROHD34.MAP` scenario first rejects missing readiness, then proves actor rebinding, seven-cap guest inventory persistence, complete queue/world-state digest convergence, and authenticated replay.
- [x] Add synchronized ordinary cross-map exit grids on top of the shared-map transition bridge. The command names a canonically registered installed-map exit, requires the acting player on its exact source hex and every connected player within four hexes on the same elevation, and suppresses Fallout's local `map_leave_map` path. The host loads the installed destination, preserves and rebinds player identities and inventories, and publishes a bounded player-keyed placement roster. Human movement auto-submits the semantic command, while agents use `game_exit`. The `VAULT13.MAP` scenario first rejects missing party readiness, then travels to `VAULTENT.MAP`, preserves an eleven-cap guest stack, repairs item identities that differ because of authority-only map-load scripts, and proves full checkpoint convergence and authenticated replay.
- [x] Add synchronized script-backed ladder travel and the shared scenery-transition command/event for typed stairs. Use scripts execute only on the host under the acting-player context; legacy `move_to(dude_obj, ...)` and rotation animation operations are redirected to that actor, while non-acting players retain their prior same-map placements. The event uses the bounded player-keyed transition roster. The `WATRSHD.MAP` scenario leaves the host on elevation 0 while the guest uses a ladder to reach elevation 1, proves four host script procedures versus zero on the guest, and converges the full checkpoint and authenticated replay. Typed-stair scripts can also request cross-map travel through the shared load bridge, subject to all-player proximity.
- [x] Validate typed-stair behavior with installed `CHILDRN2.MAP` fixtures. One audited stair moves the guest independently to elevation 1 while the host stays on elevation 0. Another rejects cross-map travel to `CHILDRN1.MAP` before its script runs when the host is too far away, then carries both players when ready. Unclassified scripted stairs with no declared destination conservatively require both players nearby until their route is audited. The tests preserve player actor IDs and an eleven-cap guest inventory, compare the full checkpoint, replay the transition after authenticated reconnect, and prove zero guest-side script procedures. Replica authority now persists across peer-actor rebinding; the guest grows host-described map-local variable storage without running destination map-enter scripts. Static exits, doors, scenery, and critters receive IDs before variable inventories so host-only map scripts cannot renumber them.
- [x] Add agreed fixed-duration rest (10 or 30 minutes, or 1-6 hours). One player proposes a duration; the other explicitly selects the same choice. A different choice replaces the proposal, the proposer can withdraw it, and an unmatched proposal expires after 90 seconds or disconnect. The Pip-Boy marks the proposed choice and names its player; agents can use `game_rest` and inspect `pending_rest`. On unanimous consent, the host enters `Transition`, advances the shared clock through due queue events and three-hour healing boundaries, returns to `Exploration`, and emits the final time and phase revision. The peer runs no queue or script rules. The `SHADYW.MAP` two-process ten-minute scenario proves the first proposal does not advance time, matching approval does, the full checkpoint converges, and both events replay after authenticated reconnect; a three-hour variant additionally verifies both players heal.
- [x] Extend agreed rest to the Pip-Boy's until-morning/noon/evening/midnight and until-healed choices. Consent compares the selected option, and the host resolves time-of-day duration only when everyone agrees. Until-healed checks both player actors after each three-hour healing boundary, rejects a wounded actor with no healing rate, and has a 30-day ceiling. Queue interruptions and safety stops publish their actual final time and an explicit interruption flag; both players see an interruption message. Wire version 17, command/agent tests, and the `SHADYW.MAP` two-process scenarios cover all five choices, full checkpoint convergence and authenticated replay. A host-only withdrawal queue fixture additionally proves early interruption after one minute, unchanged health, and zero guest-side rule execution. The one-second installed queue cadence required raising the bounded rest queue limit above a full day's events.
- [x] Add live shared world-map travel while preserving one authoritative clock and encounter state. Snapshot version 12 carries the persistent grid, town entrances, visited-city and encounter flags, position, planning stage, controller, route, and active travel counters. The 90-second proposal requires explicit approval; source exit grids and script requests use that same consent path. A human Y/N prompt and the shared map UI show the controller's destination marker. Only the proposer controls the route initially; if that guest disconnects after approval, the host takes over the existing trip, while host disconnect pauses travel. The host-only stepper follows walkmask and terrain speed, advances bounded queues and the clock, heals both actors, rolls encounters, and resolves countdown world events. Gameplay wire version 20 carries validated route and arrival events, including destination entrance index, world clock, event kind, phase revision, and a player-keyed placement roster. Map loading preserves actor identity and inventories; authoritative snapshots reconcile host-spawned encounter critters and items without guest scripts or rolls. Cancellation before departure leaves the source map intact; after departure it lands on a playable terrain map at the authoritative world position.
  1. [x] Connect consent to human entry/approval and world-map opening; approval never transfers route control.
  2. [x] Connect controller-only route input, route changes, peer presentation, disconnect takeover, and host-only travel rules.
  3. [x] Publish town, terrain, encounter, interruption, and terminal-world-event arrivals with player placements and entrance selection; preserve recovery and cancellation semantics.
  4. [x] Verify host- and guest-proposed travel, Shady Sands arrival, route changes, after-departure cancellation, guest-controller takeover, deterministic encounter entry, and a timed-queue interruption with installed two-process scenarios, full digest convergence, zero guest rule execution, and authenticated replay. Isolated installed-data fixtures also cover before-departure cancellation, mid-route progress restore, daily healing, countdown detection, and consent rejection.
- [x] Require readiness from every connected participant for exits, rest, and world-map transitions—both players in the MVP—with an explicit host timeout policy where appropriate. Ordinary exits require proximity, fixed-duration rest requires matching consent with a 90-second expiry, and world-map entry requires explicit acceptance of the proposer's request with a 90-second expiry. Same-map elevators remain independently usable, with proximity-based shared rides.
- [x] Verify entity rebinding, remote-player inventories, queued events, and sectioned state hashes across transition boundaries. Installed-data host/guest scenarios cover same-map and cross-map elevators, ordinary exits, independent and cross-map scripted stairs, agreed rest, town and encounter arrival, queue interruption, and guest-controller travel takeover; each checks convergence and authenticated replay.
- [x] Put transition readiness and destination placement behind roster-shaped helpers. Roster-shaped readiness checks gate rest consent, cross-map elevators, ordinary exits, and script-backed cross-map stairs. Shared roster helpers now capture, validate, place, and apply player-keyed positions for elevator rides, ordinary exits, scripted transitions, and world-map arrivals; independent scripted travel restores every non-acting player's prior position. The MVP elevator wire event retains its version 13 host/guest fields, populated from and applied through the roster helpers. Replacing that wire shape remains part of the post-MVP player-count expansion.

Exit condition: two players can complete a small non-combat quest together, change maps and elevations, travel on the world map, and remain synchronized.

### Phase 4A: combat controller and turn ownership

Complete: the host promotes all same-elevation player actors into Fallout's
initiative roster and classifies each active actor by `PlayerId` or host AI.
Gameplay wire version 21 carries a revision-keyed End Turn command and
ordered combat-state events. Snapshot version 13 carries the same initiative,
active actor, turn revision, round, and deadline for reconnect and correction.
The guest's Space key ends only its own turn; the host waits for that command,
the 60-second deadline, or a disconnect pass. Host and AI turns advance from
the engine loop, while guest combat remains passive. A three-player-plus-AI
headless test covers identical initiative on both views, stale/out-of-turn
rejection, timeout, and disconnect. The installed-data two-process
`combat-turn` scenario exercises the real host combat loop and guest command,
checks matching post-combat digests, and verifies zero guest scripts, attacks,
and RNG use. Host takeover of a disconnected combatant is not enabled; the
selected 4A policy is pass. Player movement, attack, item, reload, and stance
actions in combat remain gated for 4B.

- [x] Transition into and out of the authoritative Combat phase and replicate the phase revision.
- [x] Classify every combatant by an owning `PlayerId` or as host AI.
- [x] Give end-turn input only to the peer that owns the active actor; never pass a player-owned actor to `combat_ai`. Complete combat actions belong to 4B.
- [x] Add semantic end-turn, timeout, and disconnect/pass policies. Host takeover remains an optional later policy.
- [x] Keep the guest combat simulation passive: it renders host-selected state but does not advance AI, scripts, rolls, damage, ammo, or death checks.
- [x] Key player-controlled turns, deadlines, and disconnect handling by the
  active actor's owning `PlayerId`, without a host-turn/guest-turn enum.
  Include a three-player headless initiative test. Host takeover remains an
  optional later policy; 4A passes disconnected turns.

Exit condition: initiative advances through host, guest, and AI actors in the same order on both views, and an out-of-turn command cannot mutate state.

### Phase 4B: sequential combat actions and recovery

- [x] Replicate semantic combat movement, attack, self-use and targeted item use,
  reload, facing, and end-turn intents through the host command processor. The
  host finishes animation callbacks before publishing an action result, then
  sends an immediate full-state checkpoint containing HP, AP, ammo, inventory,
  death/animation presentation, combat identity, and bonus-move points. The
  guest does not rerun scripts, damage, rolls, ammo rules, or XP rules. "Stance"
  means facing/equipped-hand presentation and selected attack mode in this
  engine; there is no independent crouch/stance mechanic to synchronize.
- [x] Add ordered, player-keyed party XP/progression events with a complete
  build checkpoint; the host applies XP once and the guest applies no level-up
  rules. Track applied-event acknowledgement separately for each participant.
- [x] Verify installed-data live guest attack and combat movement through the
  real host combat loop, matching full-state digests and zero guest scripts,
  attacks, and RNG. Headless tests cover action round-trips, ownership, stale
  revisions, duplicate suppression, three-player XP event shape, and
  independent reconnect acknowledgement boundaries.
- [x] Cover death, knockout, fleeing, elevation changes, and the script-queued
  combat entry path with installed-data host/guest scenarios and full-state
  digests. The knockout/fleeing fixture injects those status flags at the first
  authoritative combat checkpoint; a naturally produced status or content-
  script encounter remains part of the Phase 6 compatibility campaign.
- [x] Verify self-use and targeted item use, reload, and facing through
  installed-data two-process scenarios, including action-checkpoint AP costs,
  consumed Stimpak, conserved total ammo, and final digest convergence.
- [x] Run a complete multi-round encounter with guest disconnect/reconnect
  during an active turn, then continue it without duplicate effects or turn
  rewind. Include the pending action checkpoint in recovery; the test fixture
  ends the encounter deterministically on a later player turn after both
  players and AI have advanced.

Live attacks are permitted only through the revision-keyed 4B host executor and
its complete authoritative checkpoint. A command wire type by itself is never
sufficient to enable a rule-bearing action.

Exit condition: a complete encounter, including a guest reconnect during
combat, converges without repeated guest rules or lost/duplicated effects.
Durable stop, save, load, and later resumption are Phase 6's sidecar/recovery
exit condition; requiring them here conflicted with that phase's ownership.

### Phase 5: dialogue and quests

- Add a shared, player-attributed Pip-Boy activity feed so party-relevant actions
  that normally produce Pip-Boy information on one player's screen also appear
  on the others' screens. Audit and classify the existing notification paths:
  share quest updates, discoveries, and other authoritative world outcomes;
  leave personal notes, private character information, and local UI messages
  private. Publish each qualifying message once from the host as an ordered
  event with its source `PlayerId`, display name, and stable event identity;
  system-originated updates need an explicit neutral source. In the Pip-Boy,
  mark shared entries and their originating player distinctly from the local
  player's private messages, without duplicating the actor's own notification.
  Persist/replay shared entries after reconnect and cover ordering, deduplication,
  and attribution with host/guest and multi-player tests.
- Replicate reply text and option lists.
- Add voting policies and visible votes.
- Execute one authoritative option procedure.
- Apply talker stats to dialogue checks while preserving canonical story state.
- Store votes as a bounded `PlayerId`-keyed collection captured from the
  dialogue's eligible participant set. Define majority, ties, abstentions,
  disconnects, host authority, and talker authority for any roster size, then
  cover a three-player majority in headless tests while retaining two-process
  engine scenarios.

Implemented voting default: a strict majority wins. If all connected voters
have voted or the round times out with no majority, compare the strongest
supporter of each tied option by Charisma, then Intelligence, using each
character's scoped in-game stats frozen when the options appear. Only an exact
stat tie uses one host-side random draw among the tied options. Disconnected
players abstain without changing the frozen eligible roster. Talker-decides,
host-decides, and host-tie policies remain available to the controller; the
shared dialogue UI uses the stat-based majority default.

Verification: `dialogue` and `dialogue-guest` installed-data two-process smoke
scenarios cover a three-reply branch, visible conflicting ballots, an exact
stat tie and a guest Intelligence win, host-only option/script execution, a
dialogue-context Speech check for a guest talker, one attributed quest entry
on each peer, and script-queued combat entry and exit. Headless tests cover a
three-player majority, disconnect/timeout rules, bounded wire validation, and
ordered, deduplicated multi-player activity replay through snapshots. Durable
save/load of the shared feed is implemented in the Phase 6 sidecar.

Exit condition: the players can complete branching dialogue with a tie, a skill check, a quest update, and a combat transition.

### Phase 6: loot, trade, and recovery

- Add transactional direct trading.
- Add cap splitting and alternating loot priority.
- Finish the multiplayer save sidecar and recovery saves.
- Handle missing guests, replaced characters, and incompatible save versions.
- Keep direct trades bilateral and revisioned by the two named participants,
  while cap distribution, loot priority, save records, reconnect slots, and
  character replacement operate over a deterministically ordered roster.
- Version the sidecar so the existing two-player representation can migrate to
  a bounded repeated-player representation without changing `SAVE.DAT` or
  silently reassigning player IDs.

Implemented: the host-owned direct-trade controller keeps exactly
two named player/actor participants in deterministic order, bounds and
canonicalizes item/cap offers, clears confirmations whenever either offer
changes, and exposes a two-leg commit plan only after both participants confirm
the same revision. Cancellation and disconnect move no assets; failed host
inventory validation advances the revision before retry, and a committed
revision cannot commit twice. Headless tests cover stale confirmations, offer
edits, malformed duplicate entries, cancellation, disconnect, validation
failure, cap conservation, and double-commit rejection. Versioned wire
commands/events and snapshots carry the complete negotiation; the host validates
and applies both legs once before publishing an immediate checkpoint. The
in-game prompt and semantic-agent commands expose begin, offer, confirm, and
cancel actions.

Caps taken from a shared loot target split over the canonical player roster,
with a persisted remainder cursor. Contested dead-critter items use a second
persisted rotating priority cursor. Sidecar version 4 stores the bounded roster,
remote actor object data, reconnect/replacement policy, ownership, both loot
cursors, and shared Pip-Boy activity; versions 1 through 3 migrate explicitly.

When the host leaves, Fallout's native save transaction writes the hidden
`SAVEGAME/RECOVERY` generation plus its digest-bound sidecar, publishes an
ending checkpoint, and closes the session. The multiplayer lobby's `RESUME`
action loads that world for the host, reclaims or replaces the saved guest slot,
verifies restored ownership, and sends a map-aware authoritative snapshot so
the guest loads the recovered map before applying state. The installed-data
`recovery` two-process smoke test now completes that entire stop/restart/load
path and requires identical host and guest state digests afterward.

Exit condition: a two-player session can be stopped, loaded later, and resumed without duplicating or losing items.

### Phase 7: compatibility campaign and hardening

- Test packet loss, latency, reordered messages, duplicate commands, and malicious lengths.
- Add diagnostics for protocol events and state checksums.
- Run long play sessions across map changes, combat, barter, dialogue, saving, and loading.
- Maintain a representative Fallout compatibility campaign covering major script patterns, companions, scripted combat, inventory-heavy interactions, elevators, encounters, timed events, and ending-critical state.
- Make multiplayer and its networking dependencies optional at build time without changing the single-player binary path when disabled.
- Document hosting, ports, firewall behavior, compatibility rules, and known limitations.
- Audit the completed phases against the player-count expansion guardrails and
  update the known pair-shaped debt list before freezing the first-release
  protocol and sidecar formats.

Implemented: the loopback transport accepts deterministic latency, loss,
duplication, and reordering profiles; headless tests combine those with strict
command sequencing and malicious-length rejection. Each network lobby retains
a bounded protocol-event/checksum diagnostic history and reports aggregate
counters at shutdown. The campaign first gates transactional direct trade,
loot rotation, and network faults in the headless core, then executes thirteen
successive TLS host/guest scenarios over persistent isolated data trees,
covering movement, loot, inventory conservation, containers, quest scripts,
elevators, cross-map loading, encounters, interrupted timed rest, scripted
combat, dialogue, and hidden-save recovery. The maintained gate compares printed
peer digests as well as pass markers; recovery must report a matching complete
state digest. `FALLOUT_CAMPAIGN_FULL=1` expands the gate to 42 scenarios.

`FALLOUT_ENABLE_MULTIPLAYER=OFF` now produces the same `fallout-ce` target with
the original five-entry single-player menu and no Mbed TLS targets or live
socket/content-manifest implementation. Multiplayer flags fail explicitly.
Hosting, firewall/NAT, fingerprint, compatibility, campaign, and limitation
documentation is maintained under `docs/multiplayer`; the roster audit records
the exact pair-shaped v1 debt that must move to a new wire/sidecar generation.

Exit condition: the compatibility campaign completes without unresolved authoritative-state divergence, save corruption, item duplication/loss, or single-player regression.

### Post-MVP milestone: more than two players

This milestone begins only after the two-player compatibility campaign is
stable. Supporting a third or fourth guest should be the same architecture
change with a different configured maximum; the eventual product limit remains
a separate decision:

- Replace the remaining `LocalSession`, lobby, world, and sidecar host/guest
  members with the bounded player roster and a local-player ID.
- Let the authoritative host accept multiple authenticated client connections;
  maintain command deduplication, send queues, acknowledgement cursors,
  reconnect credentials, snapshot transfer, and failure state independently for
  each player, while keeping one session-wide ordered event journal.
- Broadcast authoritative results to every connected replica and retain journal
  entries until every required recipient has acknowledged them or fallen back
  to snapshot recovery. One slow client must not block command processing for
  the others indefinitely.
- Replace pair-shaped transition and presentation payloads, including version
  13 elevator placements, with bounded `PlayerId`-keyed collections in a new
  protocol version. Add explicit migration/rejection behavior for older peers.
- Generalize remote-actor creation, map-load rebinding, local presentation,
  party XP, loot priority, dialogue votes, combat ownership, and recovery saves
  without changing the canonical story actor exposed as `dude_obj`.
- Add three-player, four-player, and configured-maximum headless, loopback, real
  socket, installed-data, disconnect/reconnect, transition, combat, dialogue,
  trade, and save/load scenarios. Verify per-client authority suppression and
  complete section-digest convergence after every recovery boundary.

Exit condition: the same authoritative session passes the compatibility
campaign with three players, four players, and the configured maximum—including
independent client failure and reconnect—without introducing a separate
gameplay rules path.

## First implementation series

Keep the first reviews small enough to validate the architecture before socket code arrives:

1. [x] Add the headless test target, multiplayer value types, protocol envelope, and loopback transport.
2. [x] Add the ownership registry and document the `Object::id` audit with tests for create, clone, inventory, map load, and save/load behavior.
3. [x] Add the local session controller and the second developer actor.
4. [x] Route movement and one door interaction through commands and authoritative results.
5. [x] Add the minimal snapshot, sectioned state digest, and recovery test.

Do not add a networking dependency, lobby UI, or broad player-state refactor in this series. Its purpose is to prove command ownership, safe-point execution, identity, and recovery.

## Phase 1 implementation series

1. [x] Add registry-backed player character state and seed both developer actors from the legacy build.
2. [x] Add a scoped acting-player context and route stat, skill, perk, trait, and progression access through it.
3. [x] Point the HUD, character sheet, inventory, and equipment views at the local actor.
4. [x] Add temporary two-character creation and host-side validation.
5. [x] Persist both builds and guest inventory in a versioned multiplayer sidecar and verify save/load separation.

## Phase 2 implementation series

1. [x] Add a [bounded, nonblocking TCP packet transport](docs/multiplayer/direct-ip-transport.md) and a versioned content-check handshake.
2. [x] Add explicit host and direct-IP join launch modes around the TCP connection bootstrap.
3. [x] Send validated lobby character sheets through the network transport.
4. [x] Add a sixth main-menu entry and a [native two-player lobby](docs/multiplayer/lobby-screen.md) for hosting, joining, and character readiness.
5. [x] Encode commands, results, and events for the existing authoritative processor.
6. [x] Add event journaling, snapshot recovery, reconnect tokens, pinned-identity reconnect, and reconnect-after-TCP-disconnect.
7. [x] Replace the sampled archive digest with a complete full-archive, scripts/maps/prototypes/messages, and gameplay-configuration manifest.
8. [x] Add an explicit first-contact host fingerprint for authenticated direct-IP play, while clearly labeling omission as trusted LAN/TOFU.

## Testing strategy

Build multiplayer code around a loopback transport first. Split tests into two layers:

- Headless core tests run in CTest without opening windows, sockets, or proprietary game data. They cover protocol, sequencing, ownership, transactions, journals, and snapshot primitives.
- Engine integration scenarios run through a developer mode against an installed Fallout data set. They cover actors, maps, scripts, UI loops, combat, and saves. These can run locally even if public CI cannot ship the data.

Both layers should drive semantic commands rather than synthesizing mouse input. Raw coordinate/key injection is retained only as a UI smoke-test adapter and must not be used to satisfy authoritative gameplay scenarios.

State synchronization is an exit criterion for every phase. Development builds should compare sectioned authoritative state after scenario checkpoints and report the first divergent section instead of only an opaque whole-state checksum.

Add deterministic scenario fixtures with explicit starting actors, entities, inventories, and expected results. Scenarios should include disconnect/reconnect and snapshot recovery where relevant and assert conservation properties such as item and cap totals.

High-value automated cases include:

- Command encode and decode round trips across protocol versions.
- Ownership and action validation.
- Duplicate and out-of-order command rejection.
- Snapshot plus queued-event recovery.
- Distinct stats, perks, traits, inventory, and equipment after save and load.
- Combat ownership, turn timeout, death, and reconnect.
- Dialogue ties under every decision policy.
- Trade cancellation, double confirmation, disconnect during commit, and item conservation.
- Quest scripts executing once on the host.
- Content-manifest mismatch at connection time.
- Session-phase mismatch during combat, dialogue, and map transition.
- A single-player save/load smoke test with multiplayer disabled.
- Host and guest inputs producing the same rejection/result through the shared processor.
- A scripted interaction executing exactly once on the host and zero times on the guest.
- A combat result consuming RNG once on the host and applying explicit values on the guest.
- Agent runs with no LLM configured, proving deterministic scenarios do not depend on model availability.
- Synthetic three-player readiness, initiative, voting, party reward, and loot
  rotation tests for every new roster-shaped controller, even while the live
  runtime is restricted to two players.

Add a debug state hash for maps, global variables, objects, inventories, combat state, and player metadata. Compare the host hash with the guest's last applied snapshot during development. A mismatch should report the first divergent section rather than one opaque checksum.

## Main risks

The largest risk is the single-player global state, especially `obj_dude`, the player prototype, perks, traits, tagged skills, PC stats, UI selection, and script assumptions. Isolating player-specific data must happen before broad networking work.

Mutation coverage is another large risk. Scripts and engine functions change objects through many paths. If the guest replica depends on a hand-written event for every mutation, omissions will be common. Authoritative results should carry changed state, and periodic sectioned snapshots must detect and repair omissions while coverage grows.

Modal loops are also risky. Combat and dialogue contain their own input loops. They need to pump network traffic without allowing reentrant scripts or shared-state changes at the wrong time. The safe-point rule is a hard architecture constraint, not an optimization.

Save compatibility remains a risk. A sidecar avoids breaking original saves, but the guest actor and every guest-owned item need stable identities after maps unload and reload. The existing party code demonstrates persistence mechanisms, but it also rewrites some IDs and cannot be reused without an audit.

Content mismatch will look like network corruption if it is not rejected early. Hash scripts, message files, maps, prototypes, and relevant configuration during the handshake.

Script compatibility is a continuing risk because legacy content often assumes `dude_obj` is the only meaningful player. Keep an explicit audit of script and engine call sites that must mean story actor, acting player, or shared party state instead of changing these assumptions opportunistically.

Recovery must fail closed. If event replay or snapshot recovery cannot establish a verified synchronized state after bounded retries, stop accepting guest gameplay commands, preserve the host's authoritative state, expose a synchronization error, and require reconnect or session recovery rather than continuing from guessed state.

## Rough effort

This is a substantial engine change, not a networking patch. A vertical slice with two characters, one map, one dialogue, and one sequential fight is roughly three to four months for one developer familiar with the code. A usable MVP is likely six to twelve months of full-time work. A robust release with save recovery, broad quest testing, and platform support can take longer.

The fastest path is to keep the first release at two players, one map at a time, host authority, and sequential combat.

## Decision record

Use these as provisional defaults. They keep Phase 0 unblocked and give later phases a concrete target.

| Decision | Provisional answer | Must be final before |
| --- | --- | --- |
| First host and guest platforms | Linux and Windows desktop | Phase 2 transport selection |
| Internet connection model | LAN and direct IP only, no relay in the MVP | Phase 2 lobby work |
| Direct-IP first contact | Display an ephemeral SHA-256 host fingerprint and verify it when supplied out of band; omission is explicitly trusted LAN/TOFU | Phase 2 completion |
| Tied dialogue vote | Highest Charisma among tied options, then Intelligence; an exact stat tie uses one host random draw | Phase 5 |
| Shared XP | Grant the original full award to both characters | Phase 1 progression tests |
| Disconnected guest | The actor passes in combat and becomes unavailable for new exploration actions until reconnection | Phase 4 |
| Compatibility manifest | Hash scripts, maps, prototypes, message files, and gameplay configuration. Exclude music, speech, and other presentation-only files | Phase 2 handshake |
| Existing save conversion | Co-op games start through the co-op lobby in the MVP. Conversion is later work | Phase 1 lobby |

Two product choices still need an explicit answer before their listed phase. Linux and Windows are the proposed first platforms, and shared XP is proposed as a full award to each character. Neither choice blocks the Phase 0 local experiment.
