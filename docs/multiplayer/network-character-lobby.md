# Network character lobby

After the TCP handshake, both games show the connection state at the bottom of the main menu. Players can enter the dedicated **Multiplayer** screen, see the host and guest slots, select or create their characters, and start once both slots are ready. The earlier **New Game** waiting-window path remains available for command-line host and join sessions.

Each peer sends one versioned `Lobby` message containing its existing `CharacterCreationSheet` wire format. The host requires player 1 on its local sheet and player 2 on the guest sheet. It validates names, SPECIAL totals, age, gender, tagged skills, and traits before sending `Ready`. The guest also validates the host sheet. Both peers require the authoritative session ID and an exact per-direction message sequence starting at 2, after handshake sequence 1.

The dedicated lobby enables **Start Game** only after the host has both valid sheets and both peers enter the ready state. Pressing Escape returns to the main menu without closing the connection. A player cannot replace a sheet after submitting it.

## Automated engine smoke test

`--multiplayer-smoke-test` replaces movies and menu input with fixed `Smoke Host` and `Smoke Guest` sheets. It still initializes the installed game data, computes the compatibility fingerprint, opens the real TCP connection, exchanges both sheets, and waits for host approval. It then hands the established connection to the gameplay wire layer: the guest sends a move command and validates the host's accepted result and movement event. Each process prints `MULTIPLAYER_SMOKE_TEST_PASS` and exits with status 0. The mode requires either `--multiplayer-host` or `--multiplayer-join`.

This hook makes the two-process path runnable under Xvfb:

```text
fallout-ce --multiplayer-host=45455 --multiplayer-smoke-test
fallout-ce --multiplayer-join=127.0.0.1:45455 --multiplayer-smoke-test
```

## Current boundary

Lobby readiness gates the local new-game flow and proves that both selected character builds crossed the real TCP connection. During exploration, the runtime sends click-to-move events, actor facing changes, and usable-door events over the live connection. Movement events carry the exact hex route chosen by the sender, and multiplayer clients continue network and animation processing while their window is unfocused. Each client applies those events to its remote actor. Doors receive matching entity IDs from a canonical map scan before play starts.

Pickup and loot events exist in the wire format but are not connected to live input yet. The runtime also does not use the host-authoritative command result path for exploration, so this remains an early co-op implementation.
