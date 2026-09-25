#include "multiplayer/loopback_transport.h"

#include <array>
#include <deque>
#include <mutex>
#include <utility>

namespace fallout {
namespace multiplayer {
namespace {

struct LoopbackState {
    struct PendingPacket {
        Packet packet;
        std::size_t remainingPolls = 0;
    };

    std::array<std::deque<PendingPacket>, 2> pending;
    std::array<std::deque<Packet>, 2> ready;
    std::array<std::size_t, 2> sendCounts {};
    std::array<bool, 2> open = { true, true };
    LoopbackFaultProfile profile;
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

        std::size_t sendCount = ++_state->sendCounts[_endpoint];
        if (_state->profile.dropEvery != 0
            && sendCount % _state->profile.dropEvery == 0) {
            return TransportSendResult::Sent;
        }

        bool reorder = _state->profile.reorderEvery != 0
            && sendCount % _state->profile.reorderEvery == 0;
        bool duplicate = _state->profile.duplicateEvery != 0
            && sendCount % _state->profile.duplicateEvery == 0;
        if (_state->profile.latencyPolls == 0) {
            auto& ready = _state->ready[peer];
            if (reorder && !ready.empty()) {
                ready.insert(ready.end() - 1, packet);
            } else {
                ready.push_back(packet);
            }
            if (duplicate) {
                ready.push_back(std::move(packet));
            }
            return TransportSendResult::Sent;
        }

        LoopbackState::PendingPacket pending { packet, _state->profile.latencyPolls };
        auto& destination = _state->pending[peer];
        if (reorder && !destination.empty()) {
            destination.insert(destination.end() - 1, pending);
        } else {
            destination.push_back(pending);
        }
        if (duplicate) {
            destination.push_back(std::move(pending));
        }
        return TransportSendResult::Sent;
    }

    void poll() override
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        if (!_state->open[_endpoint]) {
            return;
        }
        for (LoopbackState::PendingPacket& pending : _state->pending[_endpoint]) {
            if (pending.remainingPolls != 0) {
                pending.remainingPolls--;
            }
        }
        releaseReady(_endpoint);
    }

    std::optional<Packet> receive() override
    {
        std::lock_guard<std::mutex> lock(_state->mutex);

        if (!_state->open[_endpoint] || _state->ready[_endpoint].empty()) {
            return std::nullopt;
        }

        Packet packet = std::move(_state->ready[_endpoint].front());
        _state->ready[_endpoint].pop_front();
        return packet;
    }

    bool isConnected() const override
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        return _state->open[_endpoint] && _state->open[1 - _endpoint];
    }

    std::optional<TransportPeerIdentity> peerIdentity() const override
    {
        return std::nullopt;
    }

    void close() override
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->open[_endpoint] = false;
        _state->pending[_endpoint].clear();
        _state->ready[_endpoint].clear();
    }

private:
    void releaseReady(std::size_t endpoint)
    {
        auto& pending = _state->pending[endpoint];
        auto& ready = _state->ready[endpoint];
        while (!pending.empty() && pending.front().remainingPolls == 0) {
            ready.push_back(std::move(pending.front().packet));
            pending.pop_front();
        }
    }

    std::shared_ptr<LoopbackState> _state;
    std::size_t _endpoint;
};

} // namespace

LoopbackTransportPair createLoopbackTransportPair()
{
    return createLoopbackTransportPair({});
}

LoopbackTransportPair createLoopbackTransportPair(const LoopbackFaultProfile& profile)
{
    auto state = std::make_shared<LoopbackState>();
    state->profile = profile;

    LoopbackTransportPair pair;
    pair.first = std::make_unique<LoopbackTransport>(state, 0);
    pair.second = std::make_unique<LoopbackTransport>(state, 1);
    return pair;
}

} // namespace multiplayer
} // namespace fallout
