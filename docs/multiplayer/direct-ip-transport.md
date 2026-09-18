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

The first packet from a guest is a protocol `Handshake` envelope containing handshake format version 1 and a 64-bit game-content digest. The host compares that digest with its local manifest before assigning the guest player slot.

The host replies with one of two messages:

- `Welcome`, containing the authoritative session ID and assigned `PlayerId`.
- `Rejected`, containing `ContentMismatch`, `SessionFull`, or `ServerUnavailable`.

The decoder rejects unknown message types, unsupported versions, zero digests, invalid player or session IDs, nonzero reserved fields, and trailing bytes. The outer protocol decoder independently checks its magic, protocol version, message kind, payload size, and exact packet length.

The localhost test opens an operating-system-assigned port, connects a guest, exchanges a complete hello and welcome, and sends two sequential gameplay packets. It verifies packet boundaries and covers content mismatch, a full session, oversized packets, malformed handshakes, and explicit shutdown.

## Current boundary

The developer game still uses its in-process loopback endpoints. The TCP and handshake code are ready for the next slice, which will add host and join launch modes and feed accepted packets into a session queue at game-loop safe points. LAN discovery, encryption, reconnect tokens, and gameplay message encoding are not part of this slice.
