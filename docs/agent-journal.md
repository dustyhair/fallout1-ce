# Agent journal

The agent journal gives a local game-control agent a machine-readable view of a running game. It is opt-in and writes one JSON object per line, flushing each record immediately.

Start Fallout with a journal path:

```sh
./fallout-ce --agent-journal=/tmp/fallout-agent.jsonl
```

The `FALLOUT_AGENT_JOURNAL` environment variable sets the same path. A command-line path takes precedence.

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

The agent should treat the journal as its first source of context. It still needs a screenshot when it reaches an unreported modal window, needs exact mouse targeting, or must verify a visual result. This keeps visual capture deliberate instead of continuous.

The file belongs to one game process. Give host and guest different paths when both run on the same machine. The game truncates the file at startup, so agents should use `tail -F` rather than holding an old file descriptor across launches.
