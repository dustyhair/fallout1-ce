# Network character lobby

After the TCP handshake, both games show the connection state at the bottom of the main menu. Players can enter the dedicated **Multiplayer** screen, see the host and guest slots, select or create their characters, and start once both slots are ready. The earlier **New Game** waiting-window path remains available for command-line host and join sessions.

Each peer sends one versioned `Lobby` message containing its existing `CharacterCreationSheet` wire format. The host requires player 1 on its local sheet and player 2 on the guest sheet. It validates names, SPECIAL totals, age, gender, tagged skills, and traits before sending `Ready`. The guest also validates the host sheet. Both peers require the authoritative session ID and an exact per-direction message sequence starting at 2, after handshake sequence 1.

Lobby protocol version 3 also carries chat on that ordered message stream. A chat body contains the sender player ID, a 16-bit text length, and up to 64 printable ASCII characters. The receiver requires the sender ID to match the authenticated peer role, rejects malformed or control-character text, and keeps a bounded receive queue. Chat is available after the TCP lobby starts and does not affect character readiness.

The dedicated lobby enables **Start Game** only after the host has both valid sheets and both peers enter the ready state. Pressing Escape returns to the main menu without closing the connection. A player cannot replace a sheet after submitting it.

## Automated engine smoke test

`--multiplayer-smoke-test` replaces movies and menu input with fixed `Smoke Host` and `Smoke Guest` sheets. It initializes the installed game data, computes the compatibility fingerprint, opens the real TLS connection, exchanges both sheets, and loads `V13Ent.map` before entering the replicated world. An execution probe invokes one real scripted door procedure and one random combat attack on the host. The guest applies the corresponding door and attack effect paths and must record zero script procedures, combat attacks, and random draws. The output reports the per-process counts.

The scenario then hands the established connection to the gameplay wire layer. The guest sends a deterministic facing command; the host decodes and executes it through the real world command processor and returns its actual result and event. The host follows it with a complete authoritative checkpoint. The guest applies the event and checkpoint, recaptures its local world, and requires every section digest to match. Both processes then close that socket, establish a new pinned TLS connection, authenticate the reconnect credential, and validate an authoritative event replay. Each process prints `MULTIPLAYER_SMOKE_TEST_PASS` with `command=face checkpoint=state-digest` and exits with status 0. The mode requires either `--multiplayer-host` or `--multiplayer-join`.

This hook makes the two-process path runnable under Xvfb:

```text
fallout-ce --multiplayer-host=45455 --multiplayer-smoke-test
fallout-ce --multiplayer-join=127.0.0.1:45455 --multiplayer-smoke-test
```

## Current boundary

Lobby readiness gates the local new-game flow and proves that both selected character builds crossed the real TCP connection. During exploration, host input publishes authoritative movement, facing, and usable-door events. Guest input sends commands to the host; accepted commands return a result and then enter the same session-wide event stream. Movement events carry the exact hex route chosen by the host, and multiplayer clients continue network and animation processing while their window is unfocused. Doors receive matching entity IDs from a canonical map scan before play starts.

The host retains a bounded journal of published exploration events for recovery. A guest detecting an event gap requests recovery from its last contiguous sequence. After a socket closes, guest input is blocked while it reconnects with its session, slot, credential, host certificate pin, and last confirmed applied event. The host keeps its listener and journal alive, then replays a retained suffix or sends a checked actor, critter, door, registered-item, world-time, and indexed game/map/script-variable snapshot when the gap predates the journal.

Ground-item pickup uses the same host-authoritative live path as movement and doors. Both peers register the initial map's ground items in a deterministic order, the guest sends only the shared item ID, and the host reserves an accepted item until Fallout's deferred pickup callback finishes. A second command for that item is rejected while the first animation is pending.

Loot initiation and inventory transfers are also live. Canonically registered critters and recursive item contents give both peers the same IDs. The initiating player alone sees the modal loot UI; guest moves are deferred until the host accepts and journals them, while rejected moves leave local inventory unchanged. Partial transfers preserve the moved object's ID and use a new host-assigned ID for Fallout's copied remainder. Previously unregistered player items receive an ID from the host when first transferred, with prototype and mutable item state reproduced on the other peer.

Player inventory drops are live as well. The host executes drop scripts, assigns IDs to dropped objects and split remainders, and publishes the final ground position. The guest inventory window refreshes as accepted events arrive. Ordinary stack quantities are drained in order through the returned remainder identities, while caps are handled as one authoritative bulk drop.
