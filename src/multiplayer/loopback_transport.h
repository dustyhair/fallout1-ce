#ifndef FALLOUT_MULTIPLAYER_LOOPBACK_TRANSPORT_H_
#define FALLOUT_MULTIPLAYER_LOOPBACK_TRANSPORT_H_

#include <cstddef>
#include <memory>

#include "multiplayer/transport.h"

namespace fallout {
namespace multiplayer {

struct LoopbackTransportPair {
    std::unique_ptr<Transport> first;
    std::unique_ptr<Transport> second;
};

// Deterministic message-level network impairment used by headless tests. A
// value of zero disables that impairment. Counters are independent in each
// direction and start at one.
struct LoopbackFaultProfile {
    std::size_t latencyPolls = 0;
    std::size_t dropEvery = 0;
    std::size_t duplicateEvery = 0;
    std::size_t reorderEvery = 0;
};

LoopbackTransportPair createLoopbackTransportPair();
LoopbackTransportPair createLoopbackTransportPair(const LoopbackFaultProfile& profile);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_LOOPBACK_TRANSPORT_H_ */
