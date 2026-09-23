#ifndef FALLOUT_MULTIPLAYER_TRANSPORT_H_
#define FALLOUT_MULTIPLAYER_TRANSPORT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace fallout {
namespace multiplayer {

using Packet = std::vector<std::uint8_t>;
using TransportPeerIdentity = std::array<std::uint8_t, 32>;

constexpr std::size_t kMaxTransportPacketSize = 1024 * 1024 + 64;

enum class TransportSendResult {
    Sent,
    Disconnected,
    PacketTooLarge,
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual TransportSendResult send(Packet packet) = 0;
    virtual void poll() = 0;
    virtual std::optional<Packet> receive() = 0;
    virtual bool isConnected() const = 0;
    virtual std::optional<TransportPeerIdentity> peerIdentity() const = 0;
    virtual bool peerIdentityMismatch() const { return false; }
    virtual void close() = 0;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_TRANSPORT_H_ */
