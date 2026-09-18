#ifndef FALLOUT_MULTIPLAYER_LOOPBACK_TRANSPORT_H_
#define FALLOUT_MULTIPLAYER_LOOPBACK_TRANSPORT_H_

#include <memory>

#include "multiplayer/transport.h"

namespace fallout {
namespace multiplayer {

struct LoopbackTransportPair {
    std::unique_ptr<Transport> first;
    std::unique_ptr<Transport> second;
};

LoopbackTransportPair createLoopbackTransportPair();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_LOOPBACK_TRANSPORT_H_ */
