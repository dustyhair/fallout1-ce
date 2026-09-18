# Network character lobby

After the TCP handshake, both games show the connection state at the bottom of the main menu. Each player chooses **New Game** and selects or creates a character. The game then opens a multiplayer waiting window instead of loading the first map immediately.

Each peer sends one versioned `Lobby` message containing its existing `CharacterCreationSheet` wire format. The host requires player 1 on its local sheet and player 2 on the guest sheet. It validates names, SPECIAL totals, age, gender, tagged skills, and traits before sending `Ready`. The guest also validates the host sheet. Both peers require the authoritative session ID and an exact per-direction message sequence starting at 2, after handshake sequence 1.

The waiting window closes only after the host has both valid sheets and both peers enter the ready state. Pressing Escape returns that window to the main menu without closing the connection. Reopening New Game with the same character returns to the existing lobby; a player cannot replace a sheet after submitting it.

## Automated engine smoke test

`--multiplayer-smoke-test` replaces movies and menu input with fixed `Smoke Host` and `Smoke Guest` sheets. It still initializes the installed game data, computes the compatibility fingerprint, opens the real TCP connection, exchanges both sheets, waits for host approval, and shuts the engine down normally. Each process prints `MULTIPLAYER_SMOKE_TEST_PASS` and exits with status 0. The mode requires either `--multiplayer-host` or `--multiplayer-join`.

This hook makes the two-process path runnable under Xvfb:

```text
fallout-ce --multiplayer-host=45455 --multiplayer-smoke-test
fallout-ce --multiplayer-join=127.0.0.1:45455 --multiplayer-smoke-test
```

## Current boundary

Lobby readiness gates the local new-game flow and proves that both selected character builds crossed the real TCP connection. The connected transport remains alive after the lobby. The games still load separate worlds because command, result, event, and snapshot messages are the next Phase 2 work. This is not playable co-op yet.
