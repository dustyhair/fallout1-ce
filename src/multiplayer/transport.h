#ifndef FALLOUT_MULTIPLAYER_TRANSPORT_H_
#define FALLOUT_MULTIPLAYER_TRANSPORT_H_

#include <cstdint>
#include <optional>
#include <vector>

namespace fallout {
namespace multiplayer {

using Packet = std::vector<std::uint8_t>;

enum class TransportSendResult {
    Sent,
    Disconnected,
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual TransportSendResult send(Packet packet) = 0;
    virtual std::optional<Packet> receive() = 0;
    virtual bool isConnected() const = 0;
    virtual void close() = 0;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_TRANSPORT_H_ */
