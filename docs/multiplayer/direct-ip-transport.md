# Direct-IP transport

Phase 2 uses TLS over a TCP transport that implements the same packet interface as the local loopback transport. TLS carries a byte stream, so each application packet has a four-byte network-order length followed by the encoded protocol envelope.

The transport:

- Listens on an IPv4 port, including port zero for an operating-system-assigned test port.
- Resolves a direct-IP address or host name when joining.
- Limits each framed packet to `kMaxTransportPacketSize`.
- Uses nonblocking accepted and connected sockets.
- Queues partial writes and reconstructs packets split across TCP reads.
- Preserves separate packet boundaries when TCP combines multiple writes.
- Bounds each connection attempt with a caller-controlled timeout.
- Requires TLS 1.2 or newer and encrypts every handshake, lobby, gameplay, snapshot, and reconnect packet.
- Generates an ephemeral P-256 host key and self-signed certificate for each hosted session.
- Displays the host certificate's SHA-256 fingerprint before accepting a guest, optionally verifies that fingerprint on the guest's first connection, and requires the observed identity on reconnect before releasing queued application bytes.

Windows uses Winsock and links `ws2_32`. Unix-like builds use the platform socket API. Mbed TLS 3.6 LTS is fetched from its checksum-pinned official release archive and linked statically, so no new runtime library is required.

The host status displays a 64-digit SHA-256 certificate fingerprint. For an authenticated first connection, the host player must share that value over a trusted channel and the guest must supply it with `--multiplayer-host-fingerprint`. The TLS handshake checks the fingerprint before releasing the queued connection hello. Omitting the option explicitly uses trust on first use (TOFU), which is suitable only when the network or first-contact path is already trusted. Reconnects always authenticate the exact host identity observed on the initial connection. The certificate is ephemeral, so a newly hosted session has a new fingerprint.

## Connection handshake

The first TLS application packet from a guest is a protocol `Handshake` envelope containing handshake format version 2 and a 64-bit compatibility digest. The host compares that digest before assigning the guest player slot.

The host replies with one of two messages:

- `Welcome`, containing the authoritative session ID, assigned `PlayerId`, and a cryptographically random 256-bit reconnect credential.
- `Rejected`, containing `ContentMismatch`, `SessionFull`, or `ServerUnavailable`.

The decoder rejects unknown message types, unsupported versions, zero digests, invalid player or session IDs, nonzero reserved fields, and trailing bytes. The outer protocol decoder independently checks its magic, protocol version, message kind, payload size, and exact packet length.

The localhost test opens an operating-system-assigned port, completes TLS with an explicit first-contact fingerprint, exchanges a hello and welcome, and sends sequential gameplay packets. It verifies packet boundaries, reconnects with the pinned identity, proves a mismatched identity cannot receive queued credentials, and covers content mismatch, a full session, oversized packets, malformed handshakes, and explicit shutdown.

## Launch modes

The game accepts these explicit network modes:

```text
fallout-ce --multiplayer-host
fallout-ce --multiplayer-host=42424
fallout-ce --multiplayer-join=192.168.1.20
fallout-ce --multiplayer-join=192.168.1.20:42424
fallout-ce --multiplayer-join=192.168.1.20:42424 --multiplayer-host-fingerprint=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
```

`42424` is the default port. The value can also be passed as the next argument, such as `--multiplayer-host 45123` or `--multiplayer-join 192.168.1.20:45123`. The fingerprint can likewise follow its option as a separate argument. It accepts 64 hexadecimal digits; colons, hyphens, and whitespace are ignored. Host and join modes are mutually exclusive and cannot be combined with the in-process `--multiplayer-dev` mode.

The host starts a nonblocking listener and the guest performs a bounded connection attempt. The handshake is then pumped by the engine's background input processing, including while either game is at the main menu or in a modal input loop. Network shutdown removes that process before the engine input system is destroyed.

The compatibility digest covers the protocol, handshake, lobby, gameplay-wire, and snapshot versions; configured language and rule-bearing difficulty/object settings; every byte of `master.dat` and `critter.dat`; and immutable files below the `SCRIPTS`, `MAPS`, `PROTO`, and `TEXT` patch directories. Patch paths are normalized case-insensitively and sorted before hashing, while presentation-only sound/art overrides and Fallout's mutable map save/cache files (`.sav`, `.bak`, `.edg`, `.tmp`, and `automap.db`) are excluded. SHA-256 builds the content manifest and its leading 64 bits enter the existing handshake compatibility value. A missing archive, unreadable content file, or duplicate case-insensitive patch path fails closed before networking starts.

## Current boundary

The developer mode still uses its in-process loopback endpoints. Network host and guest modes establish TLS, optionally authenticate first contact with the displayed fingerprint, validate compatibility, assign the host and guest player IDs, and pass the connected transport to the [network character lobby](network-character-lobby.md). The host listener remains open for an authenticated guest reconnect. LAN discovery and a friendlier join-code or account identity layer are not yet implemented.
