#include "multiplayer/connection_handshake.h"

#include <algorithm>

namespace fallout {
namespace multiplayer {
namespace {

enum class HandshakeMessageType : std::uint8_t {
    Hello = 1,
    Welcome = 2,
    Rejected = 3,
    Reconnect = 4,
    ReconnectWelcome = 5,
};

constexpr std::size_t kHandshakeHeaderSize = 4;
constexpr std::size_t kHelloSize = kHandshakeHeaderSize + 8;
constexpr std::size_t kWelcomeSize = kHandshakeHeaderSize + 4 + 8 + kReconnectTokenSize;
constexpr std::size_t kRejectedSize = kHandshakeHeaderSize + 4;
constexpr std::size_t kReconnectSize = kHandshakeHeaderSize + 8 + 4 + kReconnectTokenSize + 8;
constexpr std::size_t kReconnectWelcomeSize = kHandshakeHeaderSize + 8 + 8;

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
        value = (value << 8) | bytes[offset + index];
    }
    return value;
}

bool isKnownRejection(HandshakeRejection rejection)
{
    switch (rejection) {
    case HandshakeRejection::ContentMismatch:
    case HandshakeRejection::SessionFull:
    case HandshakeRejection::ServerUnavailable:
    case HandshakeRejection::InvalidReconnect:
        return true;
    case HandshakeRejection::None:
        return false;
    }
    return false;
}

void appendHeader(std::vector<std::uint8_t>& bytes, HandshakeMessageType type)
{
    appendUInt16(bytes, kConnectionHandshakeVersion);
    bytes.push_back(static_cast<std::uint8_t>(type));
    bytes.push_back(0);
}

} // namespace

HandshakeError encodeHandshakeMessage(const HandshakeMessage& message, ProtocolEnvelope& envelope)
{
    envelope.kind = MessageKind::Handshake;
    envelope.sessionId = {};
    envelope.payload.clear();

    if (const auto* hello = std::get_if<HandshakeHello>(&message)) {
        if (hello->contentDigest == 0) {
            return HandshakeError::InvalidContentDigest;
        }
        appendHeader(envelope.payload, HandshakeMessageType::Hello);
        appendUInt64(envelope.payload, hello->contentDigest);
        return HandshakeError::None;
    }

    if (const auto* welcome = std::get_if<HandshakeWelcome>(&message)) {
        if (welcome->sessionId.value == 0) {
            return HandshakeError::InvalidSessionId;
        }
        if (welcome->assignedPlayerId != kHostPlayerId && welcome->assignedPlayerId != kGuestPlayerId) {
            return HandshakeError::InvalidPlayerId;
        }
        appendHeader(envelope.payload, HandshakeMessageType::Welcome);
        appendUInt32(envelope.payload, welcome->assignedPlayerId.value);
        appendUInt64(envelope.payload, welcome->sessionId.value);
        if (!isValid(welcome->reconnectToken)) {
            return HandshakeError::InvalidReconnectToken;
        }
        envelope.payload.insert(envelope.payload.end(), welcome->reconnectToken.bytes.begin(), welcome->reconnectToken.bytes.end());
        envelope.sessionId = welcome->sessionId;
        return HandshakeError::None;
    }

    if (const auto* reconnect = std::get_if<ReconnectHello>(&message)) {
        if (reconnect->sessionId.value == 0) {
            return HandshakeError::InvalidSessionId;
        }
        if (reconnect->playerId != kGuestPlayerId) {
            return HandshakeError::InvalidPlayerId;
        }
        if (!isValid(reconnect->reconnectToken)) {
            return HandshakeError::InvalidReconnectToken;
        }
        appendHeader(envelope.payload, HandshakeMessageType::Reconnect);
        appendUInt64(envelope.payload, reconnect->sessionId.value);
        appendUInt32(envelope.payload, reconnect->playerId.value);
        envelope.payload.insert(envelope.payload.end(), reconnect->reconnectToken.bytes.begin(), reconnect->reconnectToken.bytes.end());
        appendUInt64(envelope.payload, reconnect->lastAppliedEvent.value);
        envelope.sessionId = reconnect->sessionId;
        return HandshakeError::None;
    }

    if (const auto* welcome = std::get_if<ReconnectWelcome>(&message)) {
        if (welcome->sessionId.value == 0) {
            return HandshakeError::InvalidSessionId;
        }
        appendHeader(envelope.payload, HandshakeMessageType::ReconnectWelcome);
        appendUInt64(envelope.payload, welcome->sessionId.value);
        appendUInt64(envelope.payload, welcome->latestEvent.value);
        envelope.sessionId = welcome->sessionId;
        return HandshakeError::None;
    }

    const auto* rejected = std::get_if<HandshakeRejected>(&message);
    if (rejected == nullptr || !isKnownRejection(rejected->reason)) {
        return HandshakeError::InvalidRejection;
    }
    appendHeader(envelope.payload, HandshakeMessageType::Rejected);
    envelope.payload.push_back(static_cast<std::uint8_t>(rejected->reason));
    envelope.payload.insert(envelope.payload.end(), 3, 0);
    return HandshakeError::None;
}

HandshakeDecodeResult decodeHandshakeMessage(const ProtocolEnvelope& envelope)
{
    HandshakeDecodeResult result;
    if (envelope.kind != MessageKind::Handshake) {
        result.error = HandshakeError::WrongMessageKind;
        return result;
    }
    if (envelope.payload.size() < kHandshakeHeaderSize) {
        result.error = HandshakeError::InvalidLength;
        return result;
    }
    if (readUInt16(envelope.payload, 0) != kConnectionHandshakeVersion) {
        result.error = HandshakeError::UnsupportedVersion;
        return result;
    }
    if (envelope.payload[3] != 0) {
        result.error = HandshakeError::InvalidReservedField;
        return result;
    }

    HandshakeMessageType type = static_cast<HandshakeMessageType>(envelope.payload[2]);
    switch (type) {
    case HandshakeMessageType::Hello: {
        if (envelope.payload.size() != kHelloSize) {
            result.error = HandshakeError::InvalidLength;
            return result;
        }
        HandshakeHello hello { readUInt64(envelope.payload, kHandshakeHeaderSize) };
        if (hello.contentDigest == 0) {
            result.error = HandshakeError::InvalidContentDigest;
            return result;
        }
        if (envelope.sessionId.value != 0) {
            result.error = HandshakeError::InvalidSessionId;
            return result;
        }
        result.message = hello;
        return result;
    }
    case HandshakeMessageType::Welcome: {
        if (envelope.payload.size() != kWelcomeSize) {
            result.error = HandshakeError::InvalidLength;
            return result;
        }
        HandshakeWelcome welcome;
        welcome.assignedPlayerId.value = readUInt32(envelope.payload, kHandshakeHeaderSize);
        welcome.sessionId.value = readUInt64(envelope.payload, kHandshakeHeaderSize + 4);
        std::copy_n(envelope.payload.begin() + kHandshakeHeaderSize + 12,
            kReconnectTokenSize,
            welcome.reconnectToken.bytes.begin());
        if (welcome.assignedPlayerId != kHostPlayerId && welcome.assignedPlayerId != kGuestPlayerId) {
            result.error = HandshakeError::InvalidPlayerId;
            return result;
        }
        if (welcome.sessionId.value == 0 || envelope.sessionId != welcome.sessionId) {
            result.error = HandshakeError::InvalidSessionId;
            return result;
        }
        if (!isValid(welcome.reconnectToken)) {
            result.error = HandshakeError::InvalidReconnectToken;
            return result;
        }
        result.message = welcome;
        return result;
    }
    case HandshakeMessageType::Rejected: {
        if (envelope.payload.size() != kRejectedSize) {
            result.error = HandshakeError::InvalidLength;
            return result;
        }
        if (envelope.payload[5] != 0 || envelope.payload[6] != 0 || envelope.payload[7] != 0) {
            result.error = HandshakeError::InvalidReservedField;
            return result;
        }
        if (envelope.sessionId.value != 0) {
            result.error = HandshakeError::InvalidSessionId;
            return result;
        }
        HandshakeRejected rejected { static_cast<HandshakeRejection>(envelope.payload[4]) };
        if (!isKnownRejection(rejected.reason)) {
            result.error = HandshakeError::InvalidRejection;
            return result;
        }
        result.message = rejected;
        return result;
    }
    case HandshakeMessageType::Reconnect: {
        if (envelope.payload.size() != kReconnectSize) {
            result.error = HandshakeError::InvalidLength;
            return result;
        }
        ReconnectHello reconnect;
        reconnect.sessionId.value = readUInt64(envelope.payload, kHandshakeHeaderSize);
        reconnect.playerId.value = readUInt32(envelope.payload, kHandshakeHeaderSize + 8);
        std::copy_n(envelope.payload.begin() + kHandshakeHeaderSize + 12,
            kReconnectTokenSize,
            reconnect.reconnectToken.bytes.begin());
        reconnect.lastAppliedEvent.value = readUInt64(envelope.payload, kHandshakeHeaderSize + 12 + kReconnectTokenSize);
        if (reconnect.sessionId.value == 0 || envelope.sessionId != reconnect.sessionId) {
            result.error = HandshakeError::InvalidSessionId;
            return result;
        }
        if (reconnect.playerId != kGuestPlayerId) {
            result.error = HandshakeError::InvalidPlayerId;
            return result;
        }
        if (!isValid(reconnect.reconnectToken)) {
            result.error = HandshakeError::InvalidReconnectToken;
            return result;
        }
        result.message = reconnect;
        return result;
    }
    case HandshakeMessageType::ReconnectWelcome: {
        if (envelope.payload.size() != kReconnectWelcomeSize) {
            result.error = HandshakeError::InvalidLength;
            return result;
        }
        ReconnectWelcome welcome;
        welcome.sessionId.value = readUInt64(envelope.payload, kHandshakeHeaderSize);
        welcome.latestEvent.value = readUInt64(envelope.payload, kHandshakeHeaderSize + 8);
        if (welcome.sessionId.value == 0 || envelope.sessionId != welcome.sessionId) {
            result.error = HandshakeError::InvalidSessionId;
            return result;
        }
        result.message = welcome;
        return result;
    }
    }

    result.error = HandshakeError::UnknownMessageType;
    return result;
}

HandshakeMessage makeHostHandshakeResponse(const HandshakeHello& hello,
    std::uint64_t expectedContentDigest,
    SessionId sessionId,
    const ReconnectToken& reconnectToken,
    bool guestSlotAvailable)
{
    if (hello.contentDigest == 0 || hello.contentDigest != expectedContentDigest) {
        return HandshakeRejected { HandshakeRejection::ContentMismatch };
    }
    if (!guestSlotAvailable) {
        return HandshakeRejected { HandshakeRejection::SessionFull };
    }
    if (sessionId.value == 0 || !isValid(reconnectToken)) {
        return HandshakeRejected { HandshakeRejection::ServerUnavailable };
    }
    return HandshakeWelcome { sessionId, kGuestPlayerId, reconnectToken };
}

} // namespace multiplayer
} // namespace fallout
