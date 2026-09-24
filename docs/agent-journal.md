# Agent journal

The agent journal gives a local game-control agent a machine-readable view of a running game. It is opt-in and writes one JSON object per line, flushing each record immediately.

Start Fallout with a journal path:

```sh
./fallout-ce --agent-journal=/tmp/fallout-agent.jsonl
```

The `FALLOUT_AGENT_JOURNAL` environment variable sets the same path. A command-line path takes precedence.

To let a local agent operate the game, also give the process a command file:

```sh
./fallout-ce \
  --agent-journal=/tmp/fallout-agent.jsonl \
  --agent-command-file=/tmp/fallout-agent.commands
```

`FALLOUT_AGENT_COMMAND_FILE` is the equivalent environment variable. Control is disabled unless a command path is explicitly configured. The game creates and truncates the file at startup. Append one command per line, using a unique positive command ID:

```text
1 move 410 220
2 click 410 220
3 right_click 410 220
4 key a
5 key enter
6 text Follow me.
```

Coordinates are logical game-screen coordinates, not desktop coordinates. Supported named keys are `escape`, `enter`, `tab`, `space`, `backspace`, the four arrow directions, `home`, `end`, `page_up`, `page_down`, and `delete`; a single printable character is also accepted. `text` injects printable ASCII in order. Each `agent_command` journal record acknowledges an accepted, executed, or rejected command and includes its ID.

When a multiplayer world is active, prefer the semantic commands below. They enter through the same runtime handlers and authoritative command processor as human host or guest input; they do not mutate game objects directly.

```text
10 game_move 12345 0 walk
11 game_move 12350 0 run
12 game_face 4
13 game_door 77
14 game_pickup 88
15 game_skill traps 42
16 game_loot 91
17 game_use_item 88 42
18 game_give 1 88 3
19 game_elevator 8 2
20 game_exit 97
21 game_stairs 103
22 game_rest 30
23 game_rest cancel
```

`game_move` takes a map tile, elevation from 0 through 2, and optional `walk` or `run`. Entity commands take an entity ID reported by the journal. `game_skill` takes one of `first_aid`, `doctor`, `lockpick`, `steal`, `traps`, `science`, or `repair`, followed by a registered target ID. `game_use_item` takes a registered item ID from `local_inventory` followed by a registered target ID; the item must be directly owned by the acting player. `game_give` takes the destination player's actor entity ID, a registered item ID from `local_inventory`, and a positive quantity. It can give ordinary items or caps, but only from the sender's direct inventory to an adjacent player. `game_elevator` takes Fallout's elevator type from 0 through 11 and a one-based level from 1 through 4; the host accepts it only when that type exists on the acting player's map/elevation and that player is at its source. For a same-map destination, a nearby player rides along automatically and a farther player remains on the original floor. A cross-map destination requires both players at the source and carries both through the synchronized map-load boundary. `game_exit` takes an `exit` entity ID from `visible_interactables`; the acting player must stand on that exit and every connected player must be within four hexes on the same elevation. Stepping onto a ready exit also submits this command automatically for human play. `game_stairs` takes a `stairs` or `ladder` entity ID and requires the acting player within four hexes. Same-map use moves only that player; cross-map typed stairs require every connected player nearby. `game_rest` takes 10 or 30 minutes, an integer number of hours from 60 through 360 minutes, `until_morning`, `until_noon`, `until_evening`, `until_midnight`, `until_healed`, or `cancel`. The first player proposes; the other must submit the same choice within 90 seconds. `pending_rest` in `world_state` names the proposer and includes the numeric choice and readable name. World-map exits are not yet supported. A syntactically accepted command can still be rejected by authority checks for phase, ownership, range, target state, or another gameplay rule. Movement, facing, doors, pickup, loot initiation, targeted exploration skills, item use, direct player gifts, elevators, ordinary exit grids, scenery transitions, and agreed rest are currently supported; dialogue and combat verbs remain planned. Live multiplayer combat input remains blocked until authoritative turn ownership is implemented.

An agent can read the existing context and then wait for new records without polling screenshots:

```sh
tail -n 100 -F /tmp/fallout-agent.jsonl
```

Each record has a monotonic `seq`, a Unix `time_ms`, and an `event`. The journal currently reports:

- `multiplayer_status` for connection and lobby changes.
- `chat` with incoming or outgoing direction, player ID, character name, and message.
- `world_state` when player or visible-world state changes. Player entries include their player and actor entity IDs, tile, facing, health, action points, and approximate in-game screen coordinates. `pending_rest` is `null` or an object with numeric choice in `minutes`, readable `choice`, proposer player ID, and proposer name. `local_inventory` reports network-registered direct inventory items with entity ID, prototype ID, name, quantity, and equipped state. `visible_critters` reports each on-screen NPC's name, entity ID, tile, screen coordinates, distance, health, and `hostile`, `friendly`, `neutral`, or `dead` disposition. `visible_interactables` reports registered doors, non-door scenery, typed `stairs` and `ladder` objects, containers, ground items, and invisible installed exit grids with their entity IDs, kinds, locations, and screen coordinates; doors and containers also report open and locked state.
- `world_exit` when the multiplayer world closes.
- `display` for text sent to Fallout's lower message monitor.
- `dialogue_reply`, `dialogue_option`, and `dialogue_choice` for NPC conversations.
- `floating_text` for overhead NPC and player speech.
- `agent_command` for command-file readiness, execution, and errors.

For single-player UI smoke tests, combat targeting can still use a hostile entry's `screen_x` and `screen_y` with `click`. The click passes through Fallout's normal cursor and combat hit-testing; it does not bypass combat rules, range, line of sight, or action-point costs. Raw clicks are not an authoritative multiplayer agent interface.

The agent should treat the journal as its first source of context. It still needs a screenshot when it reaches an unreported modal window or must verify a visual result. This keeps visual capture deliberate instead of continuous.

The journal and command file each belong to one game process. Give host and guest different paths when both run on the same machine. Both files are truncated at startup, so agents should use `tail -F` for the journal and open the command file in append mode for every write.
