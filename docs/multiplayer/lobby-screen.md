# Multiplayer lobby screen

The main menu has a sixth **Multiplayer** button using the original menu button art, typography, spacing, keyboard navigation, and sound. It opens a 640x480 lobby built from Fallout's dialogue background and red dialogue buttons.

The lobby supports these actions:

- **Host** asks for a TCP port and starts listening.
- **Join** asks for a direct address in `host` or `host:port` form. Bracketed IPv6 addresses are also accepted.
- **Choose Character** opens the original premade/create/modify character screen and sends the validated character sheet to the other player.
- **Start Game** becomes available after both named player slots show **Ready**.
- **Disconnect** closes the current session, and **Back** returns to the main menu while leaving an active session connected.

The connection status and both player slots refresh while the screen is open. Command-line `--multiplayer-host` and `--multiplayer-join` sessions enter the same screen already connected.

## Integration coverage

The screen was exercised with two isolated X displays and real installed Fallout data. The host and guest connected over TCP, entered the lobby, selected the premade character, displayed both `Max Stone` slots as ready, pressed **Start Game**, and remained alive after both copies loaded Vault 13. The engine smoke test separately verifies the post-lobby command, result, and event exchange.
