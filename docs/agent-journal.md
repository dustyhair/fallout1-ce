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

An agent can read the existing context and then wait for new records without polling screenshots:

```sh
tail -n 100 -F /tmp/fallout-agent.jsonl
```

Each record has a monotonic `seq`, a Unix `time_ms`, and an `event`. The journal currently reports:

- `multiplayer_status` for connection and lobby changes.
- `chat` with incoming or outgoing direction, player ID, character name, and message.
- `world_state` when player or visible-critter state changes. Player entries include tile, facing, health, action points, and approximate in-game screen coordinates. `visible_critters` reports each on-screen NPC's name, entity ID, tile, screen coordinates, distance, health, and `hostile`, `friendly`, `neutral`, or `dead` disposition.
- `world_exit` when the multiplayer world closes.
- `display` for text sent to Fallout's lower message monitor.
- `dialogue_reply`, `dialogue_option`, and `dialogue_choice` for NPC conversations.
- `floating_text` for overhead NPC and player speech.
- `agent_command` for command-file readiness, execution, and errors.

For combat targeting, read a hostile entry from `visible_critters`, enter combat or activate the weapon as normal, and append a `click` using that entry's `screen_x` and `screen_y`. The click passes through Fallout's normal cursor and combat hit-testing; it does not bypass combat rules, range, line of sight, or action-point costs.

The agent should treat the journal as its first source of context. It still needs a screenshot when it reaches an unreported modal window or must verify a visual result. This keeps visual capture deliberate instead of continuous.

The journal and command file each belong to one game process. Give host and guest different paths when both run on the same machine. Both files are truncated at startup, so agents should use `tail -F` for the journal and open the command file in append mode for every write.
