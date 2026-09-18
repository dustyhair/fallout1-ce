# Two-player co-op plan

Status: Phase 0 foundation in progress on the `multiplayer-plan` branch. A developer-only local session can create two player-owned actors; command routing is next.

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
- Both player characters stay on the same map and elevation.
- No public matchmaking, relay service, or account system.
- No host migration. If the host leaves, the session ends after writing a recovery save.
- No PvP.
- No simultaneous combat actions.
- Both peers must use the same executable protocol version and the same game-data manifest.

These limits remove several hard synchronization problems without weakening the core co-op experience.

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
    CharacterBuild build;
    PlayerOwnership ownership;
    ConnectionState connection;
};
```

`CharacterBuild` owns the data that currently behaves as process-wide player data, including the player prototype's base stats and skills, traits, perks, tagged skills, level, experience, skill points, and any other character-creation fields not stored on the critter object. Reputation and karma remain shared unless a script audit proves that a value is purely personal.

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

Character creation should happen in the lobby. Each peer creates a character locally, then sends a versioned character sheet to the host. The host validates point totals, traits, skills, and perks before spawning the actor.

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
- Require both players at exits, elevators, and world-map transitions.
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

### Phase 3: exploration and transitions

- Replicate movement, doors, containers, item pickup, skill use, traps, and map-script results.
- Add range rules and synchronized exits, elevators, rest, and world-map travel.
- Define pause behavior for modal screens.

Exit condition: two players can complete a small non-combat quest together and change maps.

### Phase 4: sequential combat

- Route each player-owned turn to the correct peer.
- Replicate actions and results from the host.
- Add timeouts, disconnect behavior, combat entry and exit, and shared XP.
- Cover death, knockout, fleeing, elevation changes, and combat triggered by scripts.

Exit condition: a complete encounter survives save, load, and guest reconnection.

### Phase 5: dialogue and quests

- Replicate reply text and option lists.
- Add voting policies and visible votes.
- Execute one authoritative option procedure.
- Apply talker stats to dialogue checks while preserving canonical story state.

Exit condition: the players can complete branching dialogue with a tie, a skill check, a quest update, and a combat transition.

### Phase 6: loot, trade, and recovery

- Add transactional direct trading.
- Add cap splitting and alternating loot priority.
- Finish the multiplayer save sidecar and recovery saves.
- Handle missing guests, replaced characters, and incompatible save versions.

Exit condition: a two-player session can be stopped, loaded later, and resumed without duplicating or losing items.

### Phase 7: hardening

- Test packet loss, latency, reordered messages, duplicate commands, and malicious lengths.
- Add diagnostics for protocol events and state checksums.
- Run long play sessions across map changes, combat, barter, dialogue, saving, and loading.
- Document hosting, ports, firewall behavior, compatibility rules, and known limitations.

## First implementation series

Keep the first reviews small enough to validate the architecture before socket code arrives:

1. [x] Add the headless test target, multiplayer value types, protocol envelope, and loopback transport.
2. [x] Add the ownership registry and document the `Object::id` audit with tests for create, clone, inventory, map load, and save/load behavior.
3. [x] Add the local session controller and the second developer actor.
4. [ ] Route movement and one door interaction through commands and authoritative results.
5. [ ] Add the minimal snapshot, sectioned state digest, and recovery test.

Do not add a networking dependency, lobby UI, or broad player-state refactor in this series. Its purpose is to prove command ownership, safe-point execution, identity, and recovery.

## Testing strategy

Build multiplayer code around a loopback transport first. Split tests into two layers:

- Headless core tests run in CTest without opening windows, sockets, or proprietary game data. They cover protocol, sequencing, ownership, transactions, journals, and snapshot primitives.
- Engine integration scenarios run through a developer mode against an installed Fallout data set. They cover actors, maps, scripts, UI loops, combat, and saves. These can run locally even if public CI cannot ship the data.

Both layers should drive semantic commands rather than synthesizing mouse input.

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

Add a debug state hash for maps, global variables, objects, inventories, combat state, and player metadata. Compare the host hash with the guest's last applied snapshot during development. A mismatch should report the first divergent section rather than one opaque checksum.

## Main risks

The largest risk is the single-player global state, especially `obj_dude`, the player prototype, perks, traits, tagged skills, PC stats, UI selection, and script assumptions. Isolating player-specific data must happen before broad networking work.

Mutation coverage is another large risk. Scripts and engine functions change objects through many paths. If the guest replica depends on a hand-written event for every mutation, omissions will be common. Authoritative results should carry changed state, and periodic sectioned snapshots must detect and repair omissions while coverage grows.

Modal loops are also risky. Combat and dialogue contain their own input loops. They need to pump network traffic without allowing reentrant scripts or shared-state changes at the wrong time. The safe-point rule is a hard architecture constraint, not an optimization.

Save compatibility remains a risk. A sidecar avoids breaking original saves, but the guest actor and every guest-owned item need stable identities after maps unload and reload. The existing party code demonstrates persistence mechanisms, but it also rewrites some IDs and cannot be reused without an audit.

Content mismatch will look like network corruption if it is not rejected early. Hash scripts, message files, maps, prototypes, and relevant configuration during the handshake.

## Rough effort

This is a substantial engine change, not a networking patch. A vertical slice with two characters, one map, one dialogue, and one sequential fight is roughly three to four months for one developer familiar with the code. A usable MVP is likely six to twelve months of full-time work. A robust release with save recovery, broad quest testing, and platform support can take longer.

The fastest path is to keep the first release at two players, one map at a time, host authority, and sequential combat.

## Decision record

Use these as provisional defaults. They keep Phase 0 unblocked and give later phases a concrete target.

| Decision | Provisional answer | Must be final before |
| --- | --- | --- |
| First host and guest platforms | Linux and Windows desktop | Phase 2 transport selection |
| Internet connection model | LAN and direct IP only, no relay in the MVP | Phase 2 lobby work |
| Tied dialogue vote | The talker decides after both votes are visible | Phase 5 |
| Shared XP | Grant the original full award to both characters | Phase 1 progression tests |
| Disconnected guest | The actor passes in combat and becomes unavailable for new exploration actions until reconnection | Phase 4 |
| Compatibility manifest | Hash scripts, maps, prototypes, message files, and gameplay configuration. Exclude music, speech, and other presentation-only files | Phase 2 handshake |
| Existing save conversion | Co-op games start through the co-op lobby in the MVP. Conversion is later work | Phase 1 lobby |

Two product choices still need an explicit answer before their listed phase. Linux and Windows are the proposed first platforms, and shared XP is proposed as a full award to each character. Neither choice blocks the Phase 0 local experiment.
