#ifndef FALLOUT_MULTIPLAYER_CONNECTION_HANDSHAKE_H_
#define FALLOUT_MULTIPLAYER_CONNECTION_HANDSHAKE_H_

#include <cstdint>
#include <variant>

#include "multiplayer/protocol.h"
#include "multiplayer/session_recovery.h"

namespace fallout {
namespace multiplayer {

constexpr std::uint16_t kConnectionHandshakeVersion = 2;

enum class HandshakeRejection : std::uint8_t {
    None = 0,
    ContentMismatch = 1,
    SessionFull = 2,
    ServerUnavailable = 3,
    InvalidReconnect = 4,
};

struct HandshakeHello {
    std::uint64_t contentDigest = 0;
};

struct HandshakeWelcome {
    SessionId sessionId;
    PlayerId assignedPlayerId;
    ReconnectToken reconnectToken;
};

struct ReconnectHello {
    SessionId sessionId;
    PlayerId playerId;
    ReconnectToken reconnectToken;
    EventSequence lastAppliedEvent;
};

struct ReconnectWelcome {
    SessionId sessionId;
    EventSequence latestEvent;
};

struct HandshakeRejected {
    HandshakeRejection reason = HandshakeRejection::ServerUnavailable;
};

using HandshakeMessage = std::variant<HandshakeHello, HandshakeWelcome, HandshakeRejected, ReconnectHello, ReconnectWelcome>;

enum class HandshakeError {
    None,
    WrongMessageKind,
    UnsupportedVersion,
    UnknownMessageType,
    InvalidLength,
    InvalidReservedField,
    InvalidContentDigest,
    InvalidSessionId,
    InvalidPlayerId,
    InvalidRejection,
    InvalidReconnectToken,
};

struct HandshakeDecodeResult {
    HandshakeError error = HandshakeError::None;
    HandshakeMessage message;

    explicit operator bool() const
    {
        return error == HandshakeError::None;
    }
};

HandshakeError encodeHandshakeMessage(const HandshakeMessage& message, ProtocolEnvelope& envelope);
HandshakeDecodeResult decodeHandshakeMessage(const ProtocolEnvelope& envelope);
HandshakeMessage makeHostHandshakeResponse(const HandshakeHello& hello,
    std::uint64_t expectedContentDigest,
    SessionId sessionId,
    const ReconnectToken& reconnectToken,
    bool guestSlotAvailable = true);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_CONNECTION_HANDSHAKE_H_ */
