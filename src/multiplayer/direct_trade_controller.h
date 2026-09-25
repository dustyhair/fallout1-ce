#ifndef FALLOUT_MULTIPLAYER_DIRECT_TRADE_CONTROLLER_H_
#define FALLOUT_MULTIPLAYER_DIRECT_TRADE_CONTROLLER_H_

#include <cstdint>
#include <optional>

#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

enum class DirectTradeResult : std::uint8_t {
    Accepted = 0,
    ReadyToCommit = 1,
    InvalidState = 2,
    InvalidParticipant = 3,
    InvalidOffer = 4,
    StaleRevision = 5,
};

// The controller owns negotiation state, not engine objects. Once both named
// participants confirm the same revision, the host validates and applies the
// returned commit plan as one inventory transaction, then calls commit. A
// failed validation invalidates that revision before either player can retry.
class DirectTradeController {
public:
    DirectTradeResult begin(std::uint64_t tradeId,
        PlayerId firstPlayerId, EntityId firstActorId,
        PlayerId secondPlayerId, EntityId secondActorId);
    DirectTradeResult restore(const DirectTradeState& state);
    DirectTradeResult setOffer(PlayerId playerId, std::uint64_t revision,
        DirectTradeOffer offer);
    DirectTradeResult confirm(PlayerId playerId, std::uint64_t revision);
    DirectTradeResult cancel(PlayerId playerId, std::uint64_t revision);
    DirectTradeResult disconnect(PlayerId playerId);
    DirectTradeResult invalidateCommit(std::uint64_t revision);
    DirectTradeResult commit(std::uint64_t revision);
    void clear();

    bool active() const;
    const DirectTradeState& state() const { return _state; }
    std::optional<DirectTradeCommitPlan> commitPlan() const;

private:
    DirectTradeParticipant* participant(PlayerId playerId);
    const DirectTradeParticipant* participant(PlayerId playerId) const;
    void clearConfirmations();

    DirectTradeState _state;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_DIRECT_TRADE_CONTROLLER_H_ */
