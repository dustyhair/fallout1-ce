#ifndef FALLOUT_MULTIPLAYER_DIRECT_TRADE_CONTROLLER_H_
#define FALLOUT_MULTIPLAYER_DIRECT_TRADE_CONTROLLER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

enum class DirectTradeResult {
    Applied,
    ReadyToCommit,
    InvalidParticipant,
    InvalidOffer,
    StaleRevision,
    Inactive,
};

bool isValidDirectTradeState(const DirectTradeState& state);

// Offers name source-owned items; no inventory changes while a trade is open.
// The caller validates current holdings again and applies the full plan as one
// engine transaction after ReadyToCommit.
class DirectTradeController {
public:
    bool begin(PlayerId first, PlayerId second);
    DirectTradeResult replaceOffer(PlayerId playerId, std::uint64_t expectedRevision,
        std::uint32_t caps, std::vector<DirectTradeLine> items);
    DirectTradeResult confirm(PlayerId playerId, std::uint64_t expectedRevision);
    bool finishCommit(std::uint64_t expectedRevision, bool committed);
    bool cancel(PlayerId playerId);
    bool restore(const DirectTradeState& state);
    void clear();

    bool active() const { return _state.revision != 0; }
    const DirectTradeState& state() const { return _state; }

private:
    DirectTradeOffer* offerFor(PlayerId playerId);
    DirectTradeState _state;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_DIRECT_TRADE_CONTROLLER_H_ */
