#include "multiplayer/direct_trade_controller.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace fallout {
namespace multiplayer {

bool operator==(const DirectTradeItemOffer& lhs, const DirectTradeItemOffer& rhs)
{
    return lhs.itemId == rhs.itemId && lhs.quantity == rhs.quantity;
}

bool operator==(const DirectTradeOffer& lhs, const DirectTradeOffer& rhs)
{
    return lhs.caps == rhs.caps && lhs.items == rhs.items;
}

namespace {

bool canonicalizeOffer(DirectTradeOffer& offer)
{
    if (offer.items.size() > kMaximumDirectTradeItemsPerPlayer) return false;
    std::sort(offer.items.begin(), offer.items.end(),
        [](const DirectTradeItemOffer& lhs, const DirectTradeItemOffer& rhs) {
            return lhs.itemId.value < rhs.itemId.value;
        });
    EntityId previous;
    for (const DirectTradeItemOffer& item : offer.items) {
        if (!isValid(item.itemId) || item.quantity == 0
            || item.quantity > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            || item.itemId == previous) {
            return false;
        }
        previous = item.itemId;
    }
    return offer.caps <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
}

} // namespace

DirectTradeResult DirectTradeController::begin(std::uint64_t tradeId,
    PlayerId firstPlayerId, EntityId firstActorId,
    PlayerId secondPlayerId, EntityId secondActorId)
{
    if (active()) return DirectTradeResult::InvalidState;
    if (tradeId == 0 || !isValid(firstPlayerId) || !isValid(secondPlayerId)
        || firstPlayerId == secondPlayerId || !isValid(firstActorId)
        || !isValid(secondActorId) || firstActorId == secondActorId) {
        return DirectTradeResult::InvalidParticipant;
    }

    clear();
    _state.tradeId = tradeId;
    _state.revision = 1;
    _state.status = DirectTradeStatus::Negotiating;
    _state.participants = {
        DirectTradeParticipant { firstPlayerId, firstActorId, {}, std::nullopt },
        DirectTradeParticipant { secondPlayerId, secondActorId, {}, std::nullopt },
    };
    if (_state.participants[1].playerId.value < _state.participants[0].playerId.value) {
        std::swap(_state.participants[0], _state.participants[1]);
    }
    return DirectTradeResult::Accepted;
}

DirectTradeResult DirectTradeController::restore(const DirectTradeState& state)
{
    if (state.tradeId == 0 || state.revision == 0
        || (state.status != DirectTradeStatus::Negotiating
            && state.status != DirectTradeStatus::ReadyToCommit)
        || state.participants[0].playerId.value
            >= state.participants[1].playerId.value
        || state.participants[0].actorId == state.participants[1].actorId) {
        return DirectTradeResult::InvalidState;
    }
    bool ready = true;
    for (const DirectTradeParticipant& party : state.participants) {
        DirectTradeOffer offer = party.offer;
        if (!isValid(party.playerId) || !isValid(party.actorId)
            || !canonicalizeOffer(offer) || !(offer == party.offer)
            || (party.confirmedRevision.has_value()
                && *party.confirmedRevision != state.revision)) {
            return DirectTradeResult::InvalidState;
        }
        ready = ready && party.confirmedRevision == state.revision;
    }
    if ((state.status == DirectTradeStatus::ReadyToCommit) != ready) {
        return DirectTradeResult::InvalidState;
    }
    _state = state;
    return DirectTradeResult::Accepted;
}

DirectTradeResult DirectTradeController::setOffer(PlayerId playerId,
    std::uint64_t revision, DirectTradeOffer offer)
{
    if (_state.status != DirectTradeStatus::Negotiating) {
        return DirectTradeResult::InvalidState;
    }
    DirectTradeParticipant* party = participant(playerId);
    if (party == nullptr) return DirectTradeResult::InvalidParticipant;
    if (revision != _state.revision) return DirectTradeResult::StaleRevision;
    if (!canonicalizeOffer(offer)) return DirectTradeResult::InvalidOffer;
    if (party->offer == offer) return DirectTradeResult::Accepted;
    if (_state.revision == std::numeric_limits<std::uint64_t>::max()) {
        return DirectTradeResult::InvalidState;
    }
    party->offer = std::move(offer);
    _state.revision++;
    clearConfirmations();
    return DirectTradeResult::Accepted;
}

DirectTradeResult DirectTradeController::confirm(PlayerId playerId,
    std::uint64_t revision)
{
    if (_state.status != DirectTradeStatus::Negotiating
        && _state.status != DirectTradeStatus::ReadyToCommit) {
        return DirectTradeResult::InvalidState;
    }
    DirectTradeParticipant* party = participant(playerId);
    if (party == nullptr) return DirectTradeResult::InvalidParticipant;
    if (revision != _state.revision) return DirectTradeResult::StaleRevision;
    party->confirmedRevision = revision;
    bool ready = std::all_of(_state.participants.begin(), _state.participants.end(),
        [revision](const DirectTradeParticipant& candidate) {
            return candidate.confirmedRevision == revision;
        });
    if (ready) {
        _state.status = DirectTradeStatus::ReadyToCommit;
        return DirectTradeResult::ReadyToCommit;
    }
    return DirectTradeResult::Accepted;
}

DirectTradeResult DirectTradeController::cancel(PlayerId playerId,
    std::uint64_t revision)
{
    if (!active()) return DirectTradeResult::InvalidState;
    if (participant(playerId) == nullptr) return DirectTradeResult::InvalidParticipant;
    if (revision != _state.revision) return DirectTradeResult::StaleRevision;
    _state.status = DirectTradeStatus::Cancelled;
    clearConfirmations();
    return DirectTradeResult::Accepted;
}

DirectTradeResult DirectTradeController::disconnect(PlayerId playerId)
{
    if (!active()) return DirectTradeResult::InvalidState;
    if (participant(playerId) == nullptr) return DirectTradeResult::InvalidParticipant;
    _state.status = DirectTradeStatus::Cancelled;
    clearConfirmations();
    return DirectTradeResult::Accepted;
}

DirectTradeResult DirectTradeController::invalidateCommit(std::uint64_t revision)
{
    if (_state.status != DirectTradeStatus::ReadyToCommit) {
        return DirectTradeResult::InvalidState;
    }
    if (revision != _state.revision) return DirectTradeResult::StaleRevision;
    if (_state.revision == std::numeric_limits<std::uint64_t>::max()) {
        return DirectTradeResult::InvalidState;
    }
    _state.revision++;
    _state.status = DirectTradeStatus::Negotiating;
    clearConfirmations();
    return DirectTradeResult::Accepted;
}

DirectTradeResult DirectTradeController::commit(std::uint64_t revision)
{
    if (_state.status != DirectTradeStatus::ReadyToCommit) {
        return DirectTradeResult::InvalidState;
    }
    if (revision != _state.revision) return DirectTradeResult::StaleRevision;
    _state.status = DirectTradeStatus::Committed;
    return DirectTradeResult::Accepted;
}

void DirectTradeController::clear()
{
    _state = {};
}

bool DirectTradeController::active() const
{
    return _state.status == DirectTradeStatus::Negotiating
        || _state.status == DirectTradeStatus::ReadyToCommit;
}

std::optional<DirectTradeCommitPlan> DirectTradeController::commitPlan() const
{
    if (_state.status != DirectTradeStatus::ReadyToCommit) return std::nullopt;
    DirectTradeCommitPlan plan;
    plan.tradeId = _state.tradeId;
    plan.revision = _state.revision;
    for (std::size_t index = 0; index < _state.participants.size(); index++) {
        const DirectTradeParticipant& source = _state.participants[index];
        const DirectTradeParticipant& destination = _state.participants[1 - index];
        plan.legs[index] = DirectTradeLeg {
            source.playerId,
            source.actorId,
            destination.playerId,
            destination.actorId,
            source.offer,
        };
    }
    return plan;
}

DirectTradeParticipant* DirectTradeController::participant(PlayerId playerId)
{
    auto found = std::find_if(_state.participants.begin(), _state.participants.end(),
        [playerId](const DirectTradeParticipant& candidate) {
            return candidate.playerId == playerId;
        });
    return found != _state.participants.end() ? &*found : nullptr;
}

const DirectTradeParticipant* DirectTradeController::participant(PlayerId playerId) const
{
    auto found = std::find_if(_state.participants.begin(), _state.participants.end(),
        [playerId](const DirectTradeParticipant& candidate) {
            return candidate.playerId == playerId;
        });
    return found != _state.participants.end() ? &*found : nullptr;
}

void DirectTradeController::clearConfirmations()
{
    for (DirectTradeParticipant& party : _state.participants) {
        party.confirmedRevision.reset();
    }
}

} // namespace multiplayer
} // namespace fallout
