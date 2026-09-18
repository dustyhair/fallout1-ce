#ifndef FALLOUT_MULTIPLAYER_PROTOCOL_H_
#define FALLOUT_MULTIPLAYER_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

constexpr std::uint32_t kProtocolMagic = 0x46434D50;
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kProtocolHeaderSize = 28;
constexpr std::size_t kMaxProtocolPayloadSize = 1024 * 1024;

enum class MessageKind : std::uint16_t {
    Handshake = 1,
    Lobby = 2,
    Command = 3,
    CommandResult = 4,
    Event = 5,
    Dialogue = 6,
    Combat = 7,
    Inventory = 8,
    Snapshot = 9,
};

struct ProtocolEnvelope {
    std::uint16_t version = kProtocolVersion;
    MessageKind kind = MessageKind::Handshake;
    SessionId sessionId;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> payload;
};

enum class ProtocolError {
    None,
    PacketTooShort,
    InvalidMagic,
    UnsupportedVersion,
    UnknownMessageKind,
    PayloadTooLarge,
    TruncatedPayload,
    TrailingData,
};

struct ProtocolDecodeResult {
    ProtocolError error = ProtocolError::None;
    ProtocolEnvelope envelope;

    explicit operator bool() const
    {
        return error == ProtocolError::None;
    }
};

ProtocolError encodeEnvelope(const ProtocolEnvelope& envelope, std::vector<std::uint8_t>& packet);
ProtocolDecodeResult decodeEnvelope(const std::vector<std::uint8_t>& packet);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_PROTOCOL_H_ */
