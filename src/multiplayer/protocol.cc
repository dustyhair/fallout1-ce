#include "multiplayer/protocol.h"

namespace fallout {
namespace multiplayer {
namespace {

void appendUInt16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendUInt32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendUInt64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

std::uint16_t readUInt16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8)
        | static_cast<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t readUInt32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24)
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
        | static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t readUInt64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (int index = 0; index < 8; index++) {
        value = (value << 8) | static_cast<std::uint64_t>(bytes[offset + index]);
    }
    return value;
}

bool isKnownMessageKind(MessageKind kind)
{
    switch (kind) {
    case MessageKind::Handshake:
    case MessageKind::Lobby:
    case MessageKind::Command:
    case MessageKind::CommandResult:
    case MessageKind::Event:
    case MessageKind::Dialogue:
    case MessageKind::Combat:
    case MessageKind::Inventory:
    case MessageKind::Snapshot:
        return true;
    }

    return false;
}

} // namespace

ProtocolError encodeEnvelope(const ProtocolEnvelope& envelope, std::vector<std::uint8_t>& packet)
{
    packet.clear();

    if (envelope.version != kProtocolVersion) {
        return ProtocolError::UnsupportedVersion;
    }

    if (!isKnownMessageKind(envelope.kind)) {
        return ProtocolError::UnknownMessageKind;
    }

    if (envelope.payload.size() > kMaxProtocolPayloadSize) {
        return ProtocolError::PayloadTooLarge;
    }

    packet.reserve(kProtocolHeaderSize + envelope.payload.size());
    appendUInt32(packet, kProtocolMagic);
    appendUInt16(packet, envelope.version);
    appendUInt16(packet, static_cast<std::uint16_t>(envelope.kind));
    appendUInt32(packet, static_cast<std::uint32_t>(envelope.payload.size()));
    appendUInt64(packet, envelope.sessionId.value);
    appendUInt64(packet, envelope.sequence);
    packet.insert(packet.end(), envelope.payload.begin(), envelope.payload.end());

    return ProtocolError::None;
}

ProtocolDecodeResult decodeEnvelope(const std::vector<std::uint8_t>& packet)
{
    ProtocolDecodeResult result;

    if (packet.size() < kProtocolHeaderSize) {
        result.error = ProtocolError::PacketTooShort;
        return result;
    }

    if (readUInt32(packet, 0) != kProtocolMagic) {
        result.error = ProtocolError::InvalidMagic;
        return result;
    }

    result.envelope.version = readUInt16(packet, 4);
    if (result.envelope.version != kProtocolVersion) {
        result.error = ProtocolError::UnsupportedVersion;
        return result;
    }

    result.envelope.kind = static_cast<MessageKind>(readUInt16(packet, 6));
    if (!isKnownMessageKind(result.envelope.kind)) {
        result.error = ProtocolError::UnknownMessageKind;
        return result;
    }

    std::uint32_t payloadSize = readUInt32(packet, 8);
    if (payloadSize > kMaxProtocolPayloadSize) {
        result.error = ProtocolError::PayloadTooLarge;
        return result;
    }

    std::size_t expectedPacketSize = kProtocolHeaderSize + payloadSize;
    if (packet.size() < expectedPacketSize) {
        result.error = ProtocolError::TruncatedPayload;
        return result;
    }

    if (packet.size() > expectedPacketSize) {
        result.error = ProtocolError::TrailingData;
        return result;
    }

    result.envelope.sessionId.value = readUInt64(packet, 12);
    result.envelope.sequence = readUInt64(packet, 20);
    result.envelope.payload.assign(packet.begin() + kProtocolHeaderSize, packet.end());
    return result;
}

} // namespace multiplayer
} // namespace fallout
