# Network character lobby

After the TCP handshake, both games show the connection state at the bottom of the main menu. Players can enter the dedicated **Multiplayer** screen, see the host and guest slots, select or create their characters, and start once both slots are ready. The earlier **New Game** waiting-window path remains available for command-line host and join sessions.

Each peer sends one versioned `Lobby` message containing its existing `CharacterCreationSheet` wire format. The host requires player 1 on its local sheet and player 2 on the guest sheet. It validates names, SPECIAL totals, age, gender, tagged skills, and traits before sending `Ready`. The guest also validates the host sheet. Both peers require the authoritative session ID and an exact per-direction message sequence starting at 2, after handshake sequence 1.

Lobby protocol version 3 also carries chat on that ordered message stream. A chat body contains the sender player ID, a 16-bit text length, and up to 64 printable ASCII characters. The receiver requires the sender ID to match the authenticated peer role, rejects malformed or control-character text, and keeps a bounded receive queue. Chat is available after the TCP lobby starts and does not affect character readiness.

The dedicated lobby enables **Start Game** only after the host has both valid sheets and both peers enter the ready state. Pressing Escape returns to the main menu without closing the connection. A player cannot replace a sheet after submitting it.

## Automated engine smoke test

`--multiplayer-smoke-test` replaces movies and menu input with fixed `Smoke Host` and `Smoke Guest` sheets. It initializes the installed game data, computes the compatibility fingerprint, opens the real TLS connection, exchanges both sheets, and loads `V13Ent.map` before entering the replicated world (`quest` instead loads `SHADYW.MAP`). An execution probe invokes real host-only script work and, on the standard map, one random combat attack. The guest applies only explicit effects and must record zero script procedures, combat attacks, and random draws. The output reports the per-process counts. After world entry, every installed-data scenario also opens an authoritative dialogue modal, proves an exploration command is rejected during the modal phase, closes it, verifies the new exploration phase revision, and proves a 125-point party XP award mutates both host builds while the replica cannot award itself. The later state-digest checkpoint includes both complete builds.

The scenario then hands the established connection to the gameplay wire layer. The guest sends a deterministic movement destination; the host decodes it, runs real pathfinding and movement through the world command processor, and returns the resulting path event after the actor reaches the destination. The host follows it with a complete authoritative checkpoint. The guest applies the path event and checkpoint, recaptures its local world, and requires every section digest to match. Both processes then close that socket, establish a new pinned TLS connection, authenticate the reconnect credential, and validate an authoritative event replay. Each process prints `MULTIPLAYER_SMOKE_TEST_PASS` with `command=move checkpoint=state-digest` and exits with status 0. The mode requires either `--multiplayer-host` or `--multiplayer-join`.

Passing `--multiplayer-smoke-scenario=door` selects the scripted-door fixture instead. Both processes place the guest actor beside the same registered scenery door. The host runs the asynchronous door action and script to completion, publishes the final open, lock, and frame state, and checkpoints the result. The guest applies only that final state and must again match every snapshot section without executing the script.

Passing `--multiplayer-smoke-scenario=pickup` selects the pickup fixture. It uses a registered ordinary ground item when the map provides one, or places a Stimpak from the installed game data beside the guest actor. The host runs Fallout's pickup animation and callback, publishes both the start event and the callback-generated completion event, and checkpoints the resulting inventory ownership and quantity at event 2. The guest treats the start as presentation-only, applies the explicit completion effect without rerunning pickup rules, and verifies the same complete state digest. Reconnect replays both ordered events.

Passing `--multiplayer-smoke-scenario=loot` selects a registered map critter, places the guest actor beside it, first proves that a remote loot command is rejected without an event, then sends the adjacent command through the host command processor. It applies the authoritative loot-start event and verifies checkpoint and replay convergence.

Passing `--multiplayer-smoke-scenario=transfer` creates the same seven-cap guest stack on both processes. It first proves that an out-of-range gift and a forged attempt to take from the host are rejected without events. The guest then gives three caps to the adjacent host over the real command channel. Both peers must reach the same four/three cap split identities, converge at the checkpoint, and replay the transfer after reconnect.

Passing `--multiplayer-smoke-scenario=skill` places the guest beside the same registered door and submits Traps through the real skill command. The host runs the asynchronous skill action under the guest character context; the guest applies only the ordered presentation boundary. Both processes then require the same complete authoritative state digest and replay the skill event after reconnect.

Passing `--multiplayer-smoke-scenario=scenery` selects a registered non-door scenery object, preferring one with a script, and submits Science through the same skill path. After the host action completes, the fixture changes one shared scenery flag only on the host. The version 9 checkpoint must repair that flag and converge the complete scenery section on the guest without guest-side rule execution.

Passing `--multiplayer-smoke-scenario=container` places the guest beside a registered ground container and submits Lockpick. The fixture changes one shared container flag only on the host, and the checkpoint must converge the complete item section—including container art/frame, flags, and light—before authenticated replay.

Passing `--multiplayer-smoke-scenario=quest` loads `SHADYW.MAP`, places the guest beside Jarvis, and gives that actor a registered antidote. The real item-use command cures Jarvis on the host: the script consumes the antidote, removes its timer, changes local and global state, adds reputation, and awards 400 party XP. The guest executes no script or random rule work, removes the consumed item through the checkpoint, matches the complete digest, and replays the event after authenticated reconnect.

This hook makes the two-process path runnable under Xvfb:

```text
fallout-ce --multiplayer-host=45455 --multiplayer-smoke-test
fallout-ce --multiplayer-join=127.0.0.1:45455 --multiplayer-smoke-test
```

## Current boundary

Lobby readiness gates the local new-game flow and proves that both selected character builds crossed the real TCP connection. During exploration, host input publishes authoritative movement, facing, and usable-door events. Guest input sends commands to the host; accepted commands return a result and then enter the same session-wide event stream. Movement events carry the exact hex route chosen by the host, and multiplayer clients continue network and animation processing while their window is unfocused. Doors receive matching entity IDs from a canonical map scan before play starts.

The host retains a bounded journal of published exploration events for recovery. A guest detecting an event gap requests recovery from its last contiguous sequence. After a socket closes, guest input is blocked while it reconnects with its session, slot, credential, host certificate pin, and last confirmed applied event. The host keeps its listener and journal alive, then replays a retained suffix or sends a checked actor, critter, door, non-door-scenery, registered-item, world-time, and indexed game/map/script-variable snapshot when the gap predates the journal.

Ground-item pickup uses the same host-authoritative live path as movement and doors. Both peers register the initial map's ground items in a deterministic order, the guest sends only the shared item ID, and the host reserves an accepted item until Fallout's deferred pickup callback finishes. A second command for that item is rejected while the first animation is pending.

Loot initiation and inventory transfers are also live. Canonically registered critters and recursive item contents give both peers the same IDs. The initiating player alone sees the modal loot UI; guest moves are deferred until the host accepts and journals them, while rejected moves leave local inventory unchanged. Partial transfers preserve the moved object's ID and use a new host-assigned ID for Fallout's copied remainder. Previously unregistered player items receive an ID from the host when first transferred, with prototype and mutable item state reproduced on the other peer.

Player inventory drops are live as well. The host executes drop scripts, assigns IDs to dropped objects and split remainders, and publishes the final ground position. The guest inventory window refreshes as accepted events arrive. Ordinary stack quantities are drained in order through the returned remainder identities, while caps are handled as one authoritative bulk drop.

Minimal direct player gifts are live through the semantic `game_give` command. The host permits only a directly owned, unequipped item moving from the sender to the adjacent peer; reverse “take” requests and remote transfers are rejected. Stack and cap splits reuse the authoritative inventory event and recovery snapshot path. The revisioned two-sided offer/confirmation UI remains a later trade milestone.

Direct item-on-target use is live through human inventory input and the semantic `game_use_item` command. The host requires a registered item owned by the acting player and a registered target, runs the normal action and script, and publishes a presentation-only event before the authoritative checkpoint carries the results.
