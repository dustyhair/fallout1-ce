# Hosting, compatibility, and limitations

## Hosting and firewall

The host listens on TCP port `42424` by default. `--multiplayer-host=PORT`
selects another port; the guest joins with
`--multiplayer-join=ADDRESS:PORT`. Allow one inbound TCP rule for that selected
port on the host machine. No UDP port, broadcast discovery, relay, or UPnP/NAT
mapping is used. Internet hosts behind a router must forward the same TCP port
to the host computer. Guests need only ordinary outbound TCP access.

All application traffic uses TLS 1.2 or newer. For authenticated first contact,
share the host's displayed 64-hex certificate fingerprint over another trusted
channel and pass it with `--multiplayer-host-fingerprint`. Without it, the first
connection is TOFU and is appropriate only for a trusted LAN. Reconnect always
pins the identity observed on the first connection.

## Compatibility rules

Peers must use the same protocol/handshake/lobby/gameplay/snapshot versions,
language, rule-bearing difficulty settings, object-hashing setting, archives,
and gameplay patch data. The manifest hashes both DAT archives and immutable
files below `SCRIPTS`, `MAPS`, `PROTO`, and `TEXT`, with normalized sorted paths.
Mutable map saves and caches (`.sav`, `.bak`, `.edg`, `.tmp`, and `automap.db`)
are intentionally excluded. A missing or unreadable input, duplicate
case-insensitive path, or digest mismatch fails before character exchange.

## Build-time switch

`-DFALLOUT_ENABLE_MULTIPLAYER=OFF` builds the same `fallout-ce` executable and
the original five-entry single-player main menu without Mbed TLS, socket/TLS
transport code, content-manifest hashing, multiplayer command-line modes, or
developer multiplayer mode. Single-player save files and launch paths remain
unchanged. Supplying a `--multiplayer...` option to that build fails explicitly
instead of silently starting single-player.

## Known v1 limitations

- Exactly one host and one guest are supported. The host is the canonical
  story actor and only the host executes world-changing scripts and rules.
- There is no discovery, join code, relay, account identity, spectator mode,
  host migration, or automatic router configuration.
- The host certificate is ephemeral. Fingerprints change with each new hosted
  session, while reconnect within that session stays pinned.
- Mods are supported only when both peers' hashed gameplay content and settings
  match. Presentation-only art and sound overrides are not compatibility inputs.
- Unclassified scripts and companion recruitment routes remain compatibility
  risks. Report the first divergent checksum section and preserve both logs.
- Recovery is host-owned and uses the hidden `SAVEGAME/RECOVERY` generation;
  ordinary single-player slots remain native Fallout saves.
