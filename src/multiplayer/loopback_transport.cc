#include "multiplayer/loopback_transport.h"

#include <array>
#include <deque>
#include <mutex>
#include <utility>

namespace fallout {
namespace multiplayer {
namespace {

struct LoopbackState {
    std::array<std::deque<Packet>, 2> queues;
    std::array<bool, 2> open = { true, true };
    mutable std::mutex mutex;
};

class LoopbackTransport : public Transport {
public:
    LoopbackTransport(std::shared_ptr<LoopbackState> state, std::size_t endpoint)
        : _state(std::move(state))
        , _endpoint(endpoint)
    {
    }

    ~LoopbackTransport() override
    {
        close();
    }

    TransportSendResult send(Packet packet) override
    {
        std::lock_guard<std::mutex> lock(_state->mutex);

        std::size_t peer = 1 - _endpoint;
        if (!_state->open[_endpoint] || !_state->open[peer]) {
            return TransportSendResult::Disconnected;
        }
        if (packet.size() > kMaxTransportPacketSize) {
            return TransportSendResult::PacketTooLarge;
        }

        _state->queues[peer].push_back(std::move(packet));
        return TransportSendResult::Sent;
    }

    void poll() override
    {
    }

    std::optional<Packet> receive() override
    {
        std::lock_guard<std::mutex> lock(_state->mutex);

        if (!_state->open[_endpoint] || _state->queues[_endpoint].empty()) {
            return std::nullopt;
        }

        Packet packet = std::move(_state->queues[_endpoint].front());
        _state->queues[_endpoint].pop_front();
        return packet;
    }

    bool isConnected() const override
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        return _state->open[_endpoint] && _state->open[1 - _endpoint];
    }

    void close() override
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->open[_endpoint] = false;
        _state->queues[_endpoint].clear();
    }

private:
    std::shared_ptr<LoopbackState> _state;
    std::size_t _endpoint;
};

} // namespace

LoopbackTransportPair createLoopbackTransportPair()
{
    auto state = std::make_shared<LoopbackState>();

    LoopbackTransportPair pair;
    pair.first = std::make_unique<LoopbackTransport>(state, 0);
    pair.second = std::make_unique<LoopbackTransport>(state, 1);
    return pair;
}

} // namespace multiplayer
} // namespace fallout
