# Direct-IP transport

Phase 2 starts with a TCP transport that implements the same packet interface as the local loopback transport. TCP is a byte stream, so each packet has a four-byte network-order length followed by the encoded protocol envelope.

The transport:

- Listens on an IPv4 port, including port zero for an operating-system-assigned test port.
- Resolves a direct-IP address or host name when joining.
- Limits each framed packet to `kMaxTransportPacketSize`.
- Uses nonblocking accepted and connected sockets.
- Queues partial writes and reconstructs packets split across TCP reads.
- Preserves separate packet boundaries when TCP combines multiple writes.
- Bounds each connection attempt with a caller-controlled timeout.

Windows uses Winsock and links `ws2_32`. Unix-like builds use the platform socket API. No new runtime library is required.

## Connection handshake

The first packet from a guest is a protocol `Handshake` envelope containing handshake format version 1 and a 64-bit compatibility digest. The host compares that digest before assigning the guest player slot.

The host replies with one of two messages:

- `Welcome`, containing the authoritative session ID and assigned `PlayerId`.
- `Rejected`, containing `ContentMismatch`, `SessionFull`, or `ServerUnavailable`.

The decoder rejects unknown message types, unsupported versions, zero digests, invalid player or session IDs, nonzero reserved fields, and trailing bytes. The outer protocol decoder independently checks its magic, protocol version, message kind, payload size, and exact packet length.

The localhost test opens an operating-system-assigned port, connects a guest, exchanges a complete hello and welcome, and sends two sequential gameplay packets. It verifies packet boundaries and covers content mismatch, a full session, oversized packets, malformed handshakes, and explicit shutdown.

## Launch modes

The game accepts these explicit network modes:

```text
fallout-ce --multiplayer-host
fallout-ce --multiplayer-host=42424
fallout-ce --multiplayer-join=192.168.1.20
fallout-ce --multiplayer-join=192.168.1.20:42424
```

`42424` is the default port. The value can also be passed as the next argument, such as `--multiplayer-host 45123` or `--multiplayer-join 192.168.1.20:45123`. Host and join modes are mutually exclusive and cannot be combined with the in-process `--multiplayer-dev` mode.

The host starts a nonblocking listener and the guest performs a bounded connection attempt. The handshake is then pumped by the engine's background input processing, including while either game is at the main menu or in a modal input loop. Network shutdown removes that process before the engine input system is destroyed.

For this launch slice, the compatibility digest covers the protocol and handshake versions, language, archive sizes, and the first and last 64 KiB of `master.dat` and `critter.dat`. It is deterministic across supported platforms and catches common installation mismatches without hashing hundreds of megabytes at startup. The planned content manifest must expand this to scripts, maps, prototypes, message files, and gameplay configuration before compatibility checking is considered complete.

## Current boundary

The developer mode still uses its in-process loopback endpoints. Network host and guest modes establish a connection, validate compatibility, assign the host and guest player IDs, and retain the connected transport for the session layer. They do not yet exchange character sheets or create network-controlled world actors. LAN discovery, encryption, reconnect tokens, and gameplay message encoding are not part of this slice.
