# Temporary character lobby

The Phase 1 lobby accepts one versioned character sheet for each registered player before the session can enter Loading. `LocalSession::transitionTo` returns `LobbyNotReady` until both sheets pass validation.

The first sheet format carries only character-creation choices:

- A printable name of at most 31 bytes.
- Seven SPECIAL base values. Each value must be between 1 and 10, and the total must be 40.
- Age from 16 through 35 and a valid gender value.
- Three distinct tagged skills.
- Up to two distinct traits in canonical slot order.

Perks, invested skill points, bonus stats, level, experience, and player flags are absent from the packet. The host creates a fresh level-one `CharacterBuild` from accepted choices, so a client cannot inject progression through character creation. The binary sheet decoder uses fixed-width network byte order, checks lengths before copying, and rejects trailing data.

## Developer flow

The existing New Game flow creates the host character. On the first map, `--multiplayer-dev` starts a session in Lobby and submits that host character. It then binds the guest as the local presentation player and reuses Fallout's character editor for the second character.

After the guest accepts the editor, the host validates and reconstructs the guest build, restores the host as the local player, and advances through Loading to Exploration. Cancelling the editor returns to single-player mode. Existing saves are rejected by this temporary flow because converting progressed characters belongs with multiplayer save persistence.

Headless tests cover every validation rule, wire round trips, malformed lengths, host and guest readiness, the lobby phase gate, canonical build construction, and teardown.
