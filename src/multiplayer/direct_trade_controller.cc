#include "multiplayer/direct_trade_controller.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace fallout {
namespace multiplayer {

bool isValidDirectTradeState(const DirectTradeState& state)
{
    if (state.revision == 0) return state.offers.empty();
    if (state.offers.size() != 2
        || !isValid(state.offers[0].playerId)
        || state.offers[0].playerId.value >= state.offers[1].playerId.value) return false;
    for (const DirectTradeOffer& offer : state.offers) {
        if (offer.caps > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            || offer.items.size() > kMaximumDirectTradeLines) return false;
        std::uint32_t previousId = 0;
        for (const DirectTradeLine& line : offer.items) {
            if (!isValid(line.itemId) || line.itemId.value <= previousId
                || line.quantity == 0
                || line.quantity > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) return false;
            previousId = line.itemId.value;
        }
    }
    return true;
}

bool DirectTradeController::begin(PlayerId first, PlayerId second)
{
    if (active() || !isValid(first) || !isValid(second) || first == second) {
        return false;
    }
    if (second.value < first.value) std::swap(first, second);
    _state.revision = 1;
    _state.offers = { DirectTradeOffer { first }, DirectTradeOffer { second } };
    return true;
}

DirectTradeOffer* DirectTradeController::offerFor(PlayerId playerId)
{
    for (DirectTradeOffer& offer : _state.offers) {
        if (offer.playerId == playerId) return &offer;
    }
    return nullptr;
}

DirectTradeResult DirectTradeController::replaceOffer(PlayerId playerId,
    std::uint64_t expectedRevision, std::uint32_t caps,
    std::vector<DirectTradeLine> items)
{
    if (!active()) return DirectTradeResult::Inactive;
    if (expectedRevision != _state.revision) return DirectTradeResult::StaleRevision;
    DirectTradeOffer* offer = offerFor(playerId);
    if (offer == nullptr) return DirectTradeResult::InvalidParticipant;
    if (caps > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
        || items.size() > kMaximumDirectTradeLines) {
        return DirectTradeResult::InvalidOffer;
    }
    std::sort(items.begin(), items.end(), [](const DirectTradeLine& a,
                                         const DirectTradeLine& b) {
        return a.itemId.value < b.itemId.value;
    });
    for (std::size_t index = 0; index < items.size(); index++) {
        if (!isValid(items[index].itemId) || items[index].quantity == 0
            || items[index].quantity > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            || (index > 0 && items[index - 1].itemId == items[index].itemId)) {
            return DirectTradeResult::InvalidOffer;
        }
    }
    if (offer->caps == caps && offer->items == items) {
        return DirectTradeResult::Applied;
    }
    if (_state.revision == std::numeric_limits<std::uint64_t>::max()) {
        return DirectTradeResult::InvalidOffer;
    }
    offer->caps = caps;
    offer->items = std::move(items);
    _state.revision++;
    for (DirectTradeOffer& participant : _state.offers) participant.confirmed = false;
    return DirectTradeResult::Applied;
}

DirectTradeResult DirectTradeController::confirm(PlayerId playerId,
    std::uint64_t expectedRevision)
{
    if (!active()) return DirectTradeResult::Inactive;
    if (expectedRevision != _state.revision) return DirectTradeResult::StaleRevision;
    DirectTradeOffer* offer = offerFor(playerId);
    if (offer == nullptr) return DirectTradeResult::InvalidParticipant;
    offer->confirmed = true;
    return std::all_of(_state.offers.begin(), _state.offers.end(),
               [](const DirectTradeOffer& participant) { return participant.confirmed; })
        ? DirectTradeResult::ReadyToCommit
        : DirectTradeResult::Applied;
}

bool DirectTradeController::finishCommit(std::uint64_t expectedRevision,
    bool committed)
{
    if (!active() || expectedRevision != _state.revision
        || !std::all_of(_state.offers.begin(), _state.offers.end(),
            [](const DirectTradeOffer& offer) { return offer.confirmed; })) {
        return false;
    }
    if (committed) {
        clear();
    } else {
        if (_state.revision == std::numeric_limits<std::uint64_t>::max()) {
            clear();
        } else {
            _state.revision++;
            for (DirectTradeOffer& offer : _state.offers) offer.confirmed = false;
        }
    }
    return true;
}

bool DirectTradeController::cancel(PlayerId playerId)
{
    if (!active() || offerFor(playerId) == nullptr) return false;
    clear();
    return true;
}

bool DirectTradeController::restore(const DirectTradeState& state)
{
    if (!isValidDirectTradeState(state)) return false;
    _state = state;
    return true;
}

void DirectTradeController::clear()
{
    _state = {};
}

} // namespace multiplayer
} // namespace fallout
