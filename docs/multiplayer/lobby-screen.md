# Multiplayer lobby screen

The main menu has a sixth **Multiplayer** button using the original menu button art, typography, spacing, keyboard navigation, and sound. It opens a 640x480 lobby built from Fallout's dialogue hardware. The upper CRT reports both player slots and the network state, a centered analog panel exposes the session controls, and the lower CRT is a two-player chat terminal.

The lobby supports these actions:

- **Host** asks for a TCP port and starts listening.
- **Join** asks for a direct address in `host` or `host:port` form. Bracketed IPv6 addresses are also accepted.
- **Choose Character** opens the original premade/create/modify character screen and sends the validated character sheet to the other player. The analog ready lever and red/green lamps reflect whether the local sheet was accepted.
- **Start Game** becomes available after both named player slots show **Ready**.
- **Disconnect** closes the current session, and **Back** returns to the main menu while leaving an active session connected.

The physical controls retain the original numeric and mnemonic shortcuts. Arrow keys or Tab move the highlight, and Space activates the highlighted control. Enter opens the inline chat prompt whenever a peer is connected. Chat messages are printable ASCII, limited to 64 characters, and carried through the ordered lobby protocol with the authenticated host or guest player ID. The receive queue and visible history are bounded.

The connection status, link meter, readiness controls, player slots, and chat refresh while the screen is open. Command-line `--multiplayer-host` and `--multiplayer-join` sessions enter the same screen already connected.

## Integration coverage

The screen was exercised with two isolated X displays and real installed Fallout data. The host and guest connected over TCP, entered the redesigned lobby, and exchanged chat in both directions. The existing full-flow coverage selects the premade character, displays both `Max Stone` slots as ready, presses **Start Game**, and keeps both copies alive after loading Vault 13. The engine smoke test separately verifies the post-lobby command, result, and event exchange.
