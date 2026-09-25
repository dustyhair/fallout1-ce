#include "multiplayer/loot_distribution_controller.h"

#include <algorithm>
#include <unordered_set>

namespace fallout {
namespace multiplayer {

LootDistributionError validateLootDistributionState(
    const LootDistributionState& state)
{
    if (state.roster.empty()
        || state.roster.size() > kMaximumTransitionPlayers) {
        return LootDistributionError::InvalidRoster;
    }
    PlayerId previous;
    for (PlayerId playerId : state.roster) {
        if (!isValid(playerId)
            || (isValid(previous) && playerId.value <= previous.value)) {
            return LootDistributionError::InvalidRoster;
        }
        previous = playerId;
    }
    if (state.nextCapExtraIndex >= state.roster.size()
        || state.nextLootPriorityIndex >= state.roster.size()) {
        return LootDistributionError::InvalidState;
    }
    return LootDistributionError::None;
}

LootDistributionError LootDistributionController::begin(
    const std::vector<PlayerId>& roster)
{
    LootDistributionState state;
    state.roster = roster;
    std::sort(state.roster.begin(), state.roster.end(),
        [](PlayerId lhs, PlayerId rhs) { return lhs.value < rhs.value; });
    LootDistributionError error = validateLootDistributionState(state);
    if (error == LootDistributionError::None) _state = std::move(state);
    return error;
}

LootDistributionError LootDistributionController::restore(
    const LootDistributionState& state)
{
    LootDistributionError error = validateLootDistributionState(state);
    if (error == LootDistributionError::None) _state = state;
    return error;
}

std::optional<std::vector<PlayerCapShare>>
LootDistributionController::splitCaps(std::uint32_t caps)
{
    if (!active()) return std::nullopt;
    std::vector<PlayerCapShare> shares;
    shares.reserve(_state.roster.size());
    std::uint32_t playerCount = static_cast<std::uint32_t>(_state.roster.size());
    std::uint32_t base = caps / playerCount;
    std::uint32_t extras = caps % playerCount;
    for (PlayerId playerId : _state.roster) {
        shares.push_back(PlayerCapShare { playerId, {}, base });
    }
    for (std::uint32_t offset = 0; offset < extras; offset++) {
        std::size_t index = (_state.nextCapExtraIndex + offset) % shares.size();
        shares[index].caps++;
    }
    _state.nextCapExtraIndex = (_state.nextCapExtraIndex + extras) % shares.size();
    return shares;
}

std::optional<PlayerId> LootDistributionController::takeLootPriority(
    const std::vector<PlayerId>& eligible)
{
    if (!active() || eligible.empty()
        || eligible.size() > _state.roster.size()) return std::nullopt;
    std::unordered_set<PlayerId, PlayerIdHash> eligibleSet;
    for (PlayerId playerId : eligible) {
        if (!isValid(playerId)
            || std::find(_state.roster.begin(), _state.roster.end(), playerId)
                == _state.roster.end()
            || !eligibleSet.insert(playerId).second) return std::nullopt;
    }
    for (std::size_t offset = 0; offset < _state.roster.size(); offset++) {
        std::size_t index = (_state.nextLootPriorityIndex + offset)
            % _state.roster.size();
        PlayerId candidate = _state.roster[index];
        if (eligibleSet.find(candidate) == eligibleSet.end()) continue;
        _state.nextLootPriorityIndex = (index + 1) % _state.roster.size();
        return candidate;
    }
    return std::nullopt;
}

void LootDistributionController::clear()
{
    _state = {};
}

} // namespace multiplayer
} // namespace fallout
